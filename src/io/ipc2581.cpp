// ============================================================================
// ipc2581.cpp — IPC-2581 import (see ipc2581.h).
// ============================================================================
#include "ipc2581.h"
#include "io_util.h"
#include "xml_lite.h"

#include <cmath>
#include <cstdio>
#include <map>

namespace pcbp::io {
namespace {

struct Prim { double w = 0, h = 0; PadShape shape = PadShape::Rect; };

void read_dictionary(const XmlNode& root, double k_default, std::map<std::string, Prim>& dict) {
    std::vector<const XmlNode*> dicts;
    root.find_all("DictionaryStandard", dicts);
    for (auto* d : dicts) {
        double k = unit_scale_word(d->attr("units"));
        if (k <= 0) k = k_default;
        for (auto* e : d->kids("EntryStandard")) {
            if (e->children.empty()) continue;
            const XmlNode& s = e->children[0];
            Prim p;
            const std::string_view n = s.local();
            if (ieq(n, "Circle")) { p.w = p.h = s.attr_num("diameter") * k; p.shape = PadShape::Round; }
            else if (ieq(n, "Donut")) { p.w = p.h = s.attr_num("outerDiameter") * k; p.shape = PadShape::Round; }
            else if (ieq(n, "Oval")) { p.w = s.attr_num("width") * k; p.h = s.attr_num("height") * k; p.shape = PadShape::Oblong; }
            else if (ieq(n, "Octagon")) { p.w = s.attr_num("width") * k; p.h = s.attr_num("height") * k; p.shape = PadShape::Octagon; }
            else if (ieq(n, "RectCenter") || ieq(n, "RectRound") || ieq(n, "RectCham") || ieq(n, "RectCorner")) {
                p.w = s.attr_num("width") * k; p.h = s.attr_num("height") * k;
                if (ieq(n, "RectCorner")) {
                    p.w = std::fabs(s.attr_num("upperRightX") - s.attr_num("lowerLeftX")) * k;
                    p.h = std::fabs(s.attr_num("upperRightY") - s.attr_num("lowerLeftY")) * k;
                }
                p.shape = std::fabs(p.w - p.h) < 1e-9 ? PadShape::Square : PadShape::Rect;
            } else if (ieq(n, "Contour")) {
                std::vector<const XmlNode*> pts;
                s.find_all("PolyBegin", pts);
                s.find_all("PolyStepSegment", pts);
                BBox bb;
                for (auto* q : pts) bb.add({q->attr_num("x") * k, q->attr_num("y") * k});
                p.w = bb.w(); p.h = bb.h();
            } else continue;
            dict[e->attr("id")] = p;
        }
    }
}

double xf_rot(const XmlNode& n, bool* mirror = nullptr) {
    const XmlNode* x = n.child("Xform");
    if (mirror) *mirror = x && ieq(x->attr("mirror"), "true");
    return x ? x->attr_num("rotation") : 0;
}

Vec2 loc(const XmlNode& n, double k) {
    const XmlNode* l = n.child("Location");
    if (!l) return {n.attr_num("x") * k, n.attr_num("y") * k};
    return {l->attr_num("x") * k, l->attr_num("y") * k};
}

void read_polygon(const XmlNode& poly, double k, OutlineBuilder& ob) {
    Vec2 cur, start;
    bool begun = false;
    for (auto& c : poly.children) {
        const std::string_view n = c.local();
        Vec2 p{c.attr_num("x") * k, c.attr_num("y") * k};
        if (ieq(n, "PolyBegin")) { cur = start = p; begun = true; }
        else if (!begun) continue;
        else if (ieq(n, "PolyStepSegment")) { ob.add_line(cur, p); cur = p; }
        else if (ieq(n, "PolyStepCurve")) {
            Vec2 ctr{c.attr_num("centerX") * k, c.attr_num("centerY") * k};
            ob.add_arc_ends(cur, p, ctr, !ieq(c.attr("clockwise"), "true"));
            cur = p;
        }
    }
    if (begun && dist(cur, start) > 1e-9) ob.add_line(cur, start);
}

} // namespace

int sniff_ipc2581(const std::string& path, const std::string& head) {
    (void)path;
    if (head.find("<IPC-2581") != std::string::npos) return 95;
    if (head.find("webstds.ipc.org/2581") != std::string::npos) return 90;
    return 0;
}

bool import_ipc2581_text(const std::string& text, Project& prj, const CadImportOptions& opt, CadImportResult& res,
                         std::string& err) {
    res = CadImportResult{};
    res.format = "IPC-2581";
    XmlNode root;
    if (!xml_parse(text, root, err)) return false;
    if (!icontains(root.name, "IPC-2581")) { err = "IPC-2581: root element is <" + root.name + ">"; return false; }
    const XmlNode* ecad = root.child("Ecad");
    if (!ecad) { err = "IPC-2581: no <Ecad> section"; return false; }
    double k = 1.0;
    std::string units = "MILLIMETER";
    if (const XmlNode* ch = ecad->child("CadHeader")) {
        units = ch->attr("units", units);
        k = unit_scale_word(units);
        if (k <= 0) { k = 1.0; res.warnings.push_back("unknown units '" + units + "', assuming mm"); }
    }
    res.units = to_lower(units);
    res.units_evidence = "CadHeader units=\"" + units + "\"";
    if (units_override_mm(opt.units) > 0) { k = units_override_mm(opt.units); res.units = opt.units + " (override)"; }
    res.unit_mm = k;
    std::map<std::string, Prim> dict;
    read_dictionary(root, k, dict);

    CadBuilder b(opt, res);
    const XmlNode* cad = ecad->child("CadData");
    if (!cad) { err = "IPC-2581: no <CadData>"; return false; }
    std::map<std::string, bool> layer_bottom;
    std::map<std::string, bool> layer_drill;
    for (auto* L : cad->kids("Layer")) {
        layer_bottom[L->attr("name")] = ieq(L->attr("side"), "BOTTOM");
        const std::string f = to_upper(L->attr("layerFunction"));
        layer_drill[L->attr("name")] = f == "DRILL" || f == "ROUT";
    }
    auto steps = cad->kids("Step");
    if (steps.empty()) { err = "IPC-2581: no <Step>"; return false; }
    const XmlNode* step = steps[0];
    if (!opt.step.empty()) {
        step = nullptr;
        for (auto* s : steps) if (ieq(s->attr("name"), opt.step)) step = s;
        if (!step) { err = "IPC-2581: step '" + opt.step + "' not found"; return false; }
    }
    res.source_name = step->attr("name");
    if (!step->kids("StepRepeat").empty()) b.warn("StepRepeat ignored (single image imported)");

    // BOM
    std::map<std::string, std::pair<std::string, std::string>> bom;   // ref -> (part, value)
    for (auto* B : root.kids("Bom")) {
        for (auto* item : B->kids("BomItem")) {
            std::string pn = item->attr("OEMDesignNumberRef"), val;
            if (const XmlNode* ch = item->child("Characteristics")) {
                for (auto* t : ch->kids("Textual")) {
                    const std::string nm = to_upper(t->attr("textualCharacteristicName", t->attr("definitionSource")));
                    if (nm == "VALUE" || nm == "VAL") val = t->attr("textualCharacteristicValue");
                }
                for (auto* m : ch->kids("Measured")) {
                    if (val.empty() && ieq(m->attr("measuredCharacteristicName"), "VALUE"))
                        val = m->attr("measuredCharacteristicValue") + m->attr("engineeringUnitOfMeasure");
                }
            }
            for (auto* r : item->kids("RefDes")) bom[r->attr("name")] = {pn, val};
        }
    }

    // Profile
    if (opt.import_board) {
        if (const XmlNode* prof = step->child("Profile")) {
            for (auto& c : prof->children) {
                if (ieq(c.local(), "Polygon") || ieq(c.local(), "Cutout")) read_polygon(c, k, b.outline);
            }
        }
    }

    // Padstack holes
    std::map<std::string, double> ps_hole;
    std::map<std::string, Prim> ps_pad;
    for (auto* ps : step->kids("PadStackDef")) {
        if (const XmlNode* h = ps->child("PadstackHoleDef")) ps_hole[ps->attr("name")] = h->attr_num("diameter") * k;
        for (auto* pd : ps->kids("PadstackPadDef")) {
            if (const XmlNode* r = pd->child("StandardPrimitiveRef")) {
                auto it = dict.find(r->attr("id"));
                if (it != dict.end() && !ps_pad.count(ps->attr("name"))) ps_pad[ps->attr("name")] = it->second;
            }
        }
    }

    // Packages
    std::map<std::string, CadPackage> pk;
    for (auto* P : step->kids("Package")) {
        CadPackage cp;
        cp.name = P->attr("name");
        cp.height = P->attr_num("height") * k;
        // R24a (user v23 #2): Package/Outline = the component body outline (package-local); never the land pattern.
        if (const XmlNode* ol = P->child("Outline")) {
            OutlineBuilder ob;
            for (auto& c : ol->children)
                if (ieq(c.local(), "Polygon")) read_polygon(c, k, ob);
            for (auto& poly : ob.finish()) {
                if (poly.size() < 2) continue;
                GPrim g; g.kind = poly.size() > 2 && dist(poly.front(), poly.back()) < 1e-6 ? GKind::Polygon : GKind::Polyline;
                g.pts = poly; g.width = 0.1;
                cp.body.push_back(std::move(g));
            }
            if (!cp.body.empty()) cp.body_source = "cad";
        }
        std::map<std::string, size_t> idx;
        for (auto* pin : P->kids("Pin")) {
            CadPin p;
            p.name = pin->attr("number");
            p.pos = loc(*pin, k);
            p.rot = xf_rot(*pin);
            p.th_hint = ieq(pin->attr("type"), "THRU");
            p.smd_hint = ieq(pin->attr("type"), "SURFACE");
            if (const XmlNode* r = pin->child("StandardPrimitiveRef")) {
                auto it = dict.find(r->attr("id"));
                if (it != dict.end()) { p.w = it->second.w; p.h = it->second.h; p.shape = it->second.shape; }
            }
            if (auto it = ps_hole.find(pin->attr("padstackDefRef")); it != ps_hole.end()) p.drill = it->second;
            idx[p.name] = cp.pins.size();
            cp.pins.push_back(p);
        }
        if (const XmlNode* lp = P->child("LandPattern")) {
            for (auto* pad : lp->kids("Pad")) {
                const XmlNode* pr = pad->child("PinRef");
                std::string pname = pr ? pr->attr("pin") : std::string();
                Prim prim;
                bool have = false;
                if (const XmlNode* r = pad->child("StandardPrimitiveRef")) {
                    auto it = dict.find(r->attr("id"));
                    if (it != dict.end()) { prim = it->second; have = true; }
                }
                const std::string psr = pad->attr("padstackDefRef");
                if (!have) { auto it = ps_pad.find(psr); if (it != ps_pad.end()) { prim = it->second; have = true; } }
                auto pi = idx.find(pname);
                if (pi == idx.end()) {
                    if (pname.empty()) continue;
                    CadPin p; p.name = pname; p.pos = loc(*pad, k);
                    idx[pname] = cp.pins.size();
                    cp.pins.push_back(p);
                    pi = idx.find(pname);
                }
                CadPin& p = cp.pins[pi->second];
                if (have && p.w <= 0) { p.w = prim.w; p.h = prim.h; p.shape = prim.shape; p.rot = xf_rot(*pad); }
                if (auto h = ps_hole.find(psr); h != ps_hole.end() && p.drill <= 0) p.drill = h->second;
            }
        }
        pk[cp.name] = cp;
    }

    // Components
    std::vector<CadComp> comps;
    for (auto* C : step->kids("Component")) {
        CadComp c;
        c.ref = C->attr("refDes");
        c.package = C->attr("packageRef");
        c.part = C->attr("part");
        bool mir = false;
        c.rot = xf_rot(*C, &mir);
        const std::string lr = C->attr("layerRef");
        c.bottom = mir || layer_bottom[lr];
        c.pos = loc(*C, k);
        if (auto it = bom.find(c.ref); it != bom.end()) {
            if (!it->second.first.empty()) c.part = it->second.first;
            c.value = it->second.second;
        }
        comps.push_back(c);
    }

    // Drill holes and fiducials from LayerFeature
    std::vector<std::pair<Vec2, double>> holes;
    int fidn = 0;
    for (auto* lf : step->kids("LayerFeature")) {
        const std::string lr = lf->attr("layerRef");
        const bool drill = layer_drill[lr];
        for (auto* set : lf->kids("Set")) {
            for (auto* h : set->kids("Hole")) holes.push_back({{h->attr_num("x") * k, h->attr_num("y") * k}, h->attr_num("diameter") * k});
            for (auto& f : set->children) {
                const std::string_view n = f.local();
                if (drill || !(ieq(n, "Fiducial") || ieq(n, "GlobalFiducial") || ieq(n, "LocalFiducial"))) continue;
                std::vector<const XmlNode*> list;
                if (ieq(n, "Fiducial")) { for (auto& g : f.children) list.push_back(&g); }
                else list.push_back(&f);
                for (auto* g : list) {
                    CadComp c;
                    c.ref = "FID_" + std::to_string(++fidn);
                    c.pos = loc(*g, k);
                    c.bottom = layer_bottom[lr];
                    c.fiducial = true;
                    Prim pr{1.0, 1.0, PadShape::Round};
                    if (const XmlNode* r = g->child("StandardPrimitiveRef")) {
                        auto it = dict.find(r->attr("id"));
                        if (it != dict.end()) pr = it->second;
                    }
                    char nm[64];
                    std::snprintf(nm, sizeof nm, "FIDUCIAL_%.3g", pr.w);
                    if (!pk.count(nm)) {
                        CadPackage fp; fp.name = nm;
                        CadPin p; p.name = "1"; p.w = pr.w; p.h = pr.h; p.shape = pr.shape;
                        fp.pins.push_back(p);
                        pk[nm] = fp;
                    }
                    c.package = nm;
                    comps.push_back(c);
                }
            }
        }
    }
    if (!holes.empty()) {
        // R18c: a hole counts for a pin only when found at that pin in at least half of the package's placements
        // and smaller than the pad (a via beside / in one placement's pad is not a component hole).
        std::map<std::string, int> inst;
        std::map<std::string, std::vector<std::pair<int, double>>> hit;
        for (auto& c : comps) {
            auto it = pk.find(c.package);
            if (it == pk.end()) continue;
            ++inst[c.package];
            auto& hv = hit[c.package];
            hv.resize(it->second.pins.size(), {0, 0.0});
            for (size_t q = 0; q < it->second.pins.size(); ++q) {
                Vec2 a = local_to_abs(it->second.pins[q].pos, c.pos, c.rot, c.bottom);
                for (auto& h : holes) if (dist(h.first, a) < 0.05) { ++hv[q].first; hv[q].second = std::max(hv[q].second, h.second); break; }
            }
        }
        for (auto& [name, hv] : hit) {
            auto& pins = pk[name].pins;
            std::vector<size_t> drilled;
            for (size_t q = 0; q < pins.size() && q < hv.size(); ++q) {
                auto& pin = pins[q];
                if (pin.drill > 0 || pin.smd_hint || hv[q].first * 2 < inst[name]) continue;
                const double m = std::min(pin.w > 0 ? pin.w : 1e9, pin.h > 0 ? pin.h : 1e9);
                if (hv[q].second < m) drilled.push_back(q);
            }
            // a through-hole part has a hole at (nearly) every lead: holes at fewer than half of the pins are vias
            if (drilled.size() * 2 >= pins.size())
                for (size_t q : drilled) pins[q].drill = hv[q].second;
        }
    }
    int guessed = 0;
    std::map<std::string, std::string> fin;
    for (auto& kv : pk) {
        for (auto& pin : kv.second.pins)
            if (holes.empty() && pin.th_hint && pin.drill <= 0) { pin.drill = 0.6 * std::min(pin.w > 0 ? pin.w : 1.0, pin.h > 0 ? pin.h : 1.0); ++guessed; }
        fin[kv.first] = b.add_package(kv.second);
    }
    if (guessed) b.warn(std::to_string(guessed) + " THRU pins without hole data: drill estimated");
    for (auto& c : comps) {
        if (auto it = fin.find(c.package); it != fin.end()) c.package = it->second;
        b.add_component(c);
    }
    return b.commit(prj, err);
}

bool import_ipc2581_file(const std::string& path, Project& prj, const CadImportOptions& opt, CadImportResult& res,
                         std::string& err) {
    std::string text;
    if (!read_file(path, text, err)) return false;
    return import_ipc2581_text(text, prj, opt, res, err);
}

} // namespace pcbp::io
