// ============================================================================
// kicad_pcb.cpp — KiCad .kicad_pcb import (see kicad_pcb.h).
// ============================================================================
#include "kicad_pcb.h"
#include "io_util.h"

#include <cmath>
#include <map>

namespace pcbp::io {
namespace {

struct SNode {
    bool list = false;
    std::string atom;
    std::vector<SNode> kids;
    const std::string& head() const { static const std::string e; return list && !kids.empty() && !kids[0].list ? kids[0].atom : e; }
    const SNode* get(std::string_view h) const {
        for (auto& k : kids) if (k.list && k.head() == h) return &k;
        return nullptr;
    }
    std::string str(size_t i) const { return i < kids.size() && !kids[i].list ? kids[i].atom : std::string(); }
    double num(size_t i, double def = 0) const { return num_or(str(i), def); }
};

struct SParser {
    std::string_view s;
    size_t i = 0;
    std::string err;
    void ws() {
        while (i < s.size()) {
            char c = s[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++i;
            else if (c == '#' && (i == 0 || s[i - 1] == '\n')) { while (i < s.size() && s[i] != '\n') ++i; }
            else break;
        }
    }
    bool parse(SNode& n, int depth) {
        ws();
        if (i >= s.size()) { err = "unexpected end"; return false; }
        if (depth > 400) { err = "nesting too deep"; return false; }
        if (s[i] == '(') {
            ++i;
            n.list = true;
            for (;;) {
                ws();
                if (i >= s.size()) { err = "unbalanced '('"; return false; }
                if (s[i] == ')') { ++i; return true; }
                n.kids.emplace_back();
                if (!parse(n.kids.back(), depth + 1)) return false;
            }
        }
        if (s[i] == ')') { err = "unexpected ')'"; return false; }
        if (s[i] == '"') {
            ++i;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    char e = s[++i];
                    n.atom += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                    ++i;
                } else n.atom += s[i++];
            }
            if (i >= s.size()) { err = "unterminated string"; return false; }
            ++i;
            return true;
        }
        size_t b = i;
        while (i < s.size() && s[i] != '(' && s[i] != ')' && s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n') ++i;
        n.atom.assign(s.substr(b, i - b));
        return true;
    }
};

Vec2 xy(const SNode* n, size_t at = 1) {        // KiCad -> Y-up mm
    if (!n) return {};
    return {n->num(at), -n->num(at + 1)};
}

bool on_edge(const SNode& n) {
    const SNode* l = n.get("layer");
    return l && l->str(1) == "Edge.Cuts";
}

void add_outline_item(const SNode& g, OutlineBuilder& ob) {
    const std::string& h = g.head();
    if (h == "gr_line") ob.add_line(xy(g.get("start")), xy(g.get("end")));
    else if (h == "gr_rect") {
        Vec2 a = xy(g.get("start")), c = xy(g.get("end"));
        ob.add_polyline({a, {c.x, a.y}, c, {a.x, c.y}}, true);
    } else if (h == "gr_circle") {
        Vec2 c = xy(g.get("center")), e = xy(g.get("end"));
        ob.add_circle(c, dist(c, e));
    } else if (h == "gr_arc") {
        if (const SNode* m = g.get("mid")) ob.add_arc_3pt(xy(g.get("start")), xy(m), xy(g.get("end")));
        else {
            // legacy: start = centre, end = arc start point, angle (screen CW) -> CCW in Y-up is negative
            Vec2 c = xy(g.get("start")), s = xy(g.get("end"));
            const SNode* a = g.get("angle");
            double sweep = a ? -a->num(1) : 0;
            ob.add_arc(c, dist(c, s), rad2deg(std::atan2(s.y - c.y, s.x - c.x)), sweep);
        }
    } else if (h == "gr_poly") {
        std::vector<Vec2> pts;
        if (const SNode* p = g.get("pts")) {
            for (auto& k : p->kids) {
                if (!k.list) continue;
                if (k.head() == "xy") pts.push_back(xy(&k));
                else if (k.head() == "arc") {
                    // approximate: add start/mid/end so the polygon follows the arc coarsely
                    pts.push_back(xy(k.get("start"))); pts.push_back(xy(k.get("mid"))); pts.push_back(xy(k.get("end")));
                }
            }
        }
        ob.add_polyline(pts, true);
    }
}

// Footprint body outline from the fabrication layer (F.Fab / B.Fab): fp_line,
// fp_rect, fp_circle, fp_arc (start/mid/end), fp_poly in footprint-local mm.
// Bottom footprints are stored mirrored by KiCad; mirror X back like the pads.
void fab_outline(const SNode& fp, bool bottom, std::vector<GPrim>& out) {
    auto L = [&](Vec2 p) { return bottom ? Vec2{-p.x, p.y} : p; };
    for (auto& g : fp.kids) {
        if (!g.list) continue;
        const std::string& h = g.head();
        const SNode* l = g.get("layer");
        if (!l || (l->str(1) != "F.Fab" && l->str(1) != "B.Fab")) continue;
        double w = 0.1;
        if (const SNode* st = g.get("stroke")) { if (const SNode* ww = st->get("width")) w = ww->num(1, 0.1); }
        else if (const SNode* ww = g.get("width")) w = ww->num(1, 0.1);
        GPrim p; p.width = w;
        if (h == "fp_line") { p.kind = GKind::Line; p.pts = {L(xy(g.get("start"))), L(xy(g.get("end")))}; }
        else if (h == "fp_rect") { p.kind = GKind::Rect; p.pts = {L(xy(g.get("start"))), L(xy(g.get("end")))}; }
        else if (h == "fp_circle") { p.kind = GKind::Circle; Vec2 c = L(xy(g.get("center"))); p.pts = {c}; p.radius = dist(c, L(xy(g.get("end")))); }
        else if (h == "fp_arc" && g.get("mid")) {
            Vec2 a = L(xy(g.get("start"))), m = L(xy(g.get("mid"))), e = L(xy(g.get("end")));
            const double d = 2 * (a.x * (m.y - e.y) + m.x * (e.y - a.y) + e.x * (a.y - m.y));
            if (std::fabs(d) < 1e-12) { p.kind = GKind::Line; p.pts = {a, e}; }
            else {
                const double a2 = a.x * a.x + a.y * a.y, m2 = m.x * m.x + m.y * m.y, e2 = e.x * e.x + e.y * e.y;
                Vec2 c{(a2 * (m.y - e.y) + m2 * (e.y - a.y) + e2 * (a.y - m.y)) / d, (a2 * (e.x - m.x) + m2 * (a.x - e.x) + e2 * (m.x - a.x)) / d};
                auto ang = [&](Vec2 q) { return rad2deg(std::atan2(q.y - c.y, q.x - c.x)); };
                double a0 = ang(a), am = ang(m), ae = ang(e);
                auto ccw = [](double x, double y) { double v = y - x; while (v < 0) v += 360; while (v >= 360) v -= 360; return v; };
                double sw = ccw(a0, ae);
                if (ccw(a0, am) > sw) sw -= 360;    // mid not on the CCW path: arc runs CW
                p.kind = GKind::Arc; p.pts = {c}; p.radius = dist(c, a); p.a0 = a0; p.a1 = sw;
            }
        } else if (h == "fp_poly") {
            p.kind = GKind::Polygon;
            if (const SNode* pts = g.get("pts")) for (auto& k : pts->kids) if (k.list && k.head() == "xy") p.pts.push_back(L(xy(&k)));
            if (p.pts.size() < 2) continue;
        } else continue;
        out.push_back(std::move(p));
    }
}

std::string prop_value(const SNode& fp, std::string_view key) {
    for (auto& k : fp.kids) {
        if (!k.list) continue;
        if (k.head() == "property" && ieq(k.str(1), key)) return k.str(2);
        if (k.head() == "fp_text" && ieq(k.str(1), key)) return k.str(2);
    }
    return {};
}

} // namespace

int sniff_kicad_pcb(const std::string& path, const std::string& head) {
    (void)path;
    std::string h = trim(head.substr(0, 200));
    if (h.rfind("(kicad_pcb", 0) == 0) return 95;
    return 0;
}

bool import_kicad_pcb_text(const std::string& text, Project& prj, const CadImportOptions& opt, CadImportResult& res,
                           std::string& err) {
    res = CadImportResult{};
    res.format = "KiCad PCB";
    res.units = "mm";
    SParser sp;
    sp.s = text;
    if (sp.s.size() >= 3 && (unsigned char)sp.s[0] == 0xEF) sp.i = 3;
    SNode root;
    if (!sp.parse(root, 0)) { err = "KiCad: " + sp.err + " at offset " + std::to_string(sp.i); return false; }
    if (root.head() != "kicad_pcb") { err = "KiCad: not a kicad_pcb file"; return false; }
    if (const SNode* v = root.get("version")) res.source_name = "version " + v->str(1);
    CadBuilder b(opt, res);
    int anon = 0, mech = 0;
    for (auto& n : root.kids) {
        if (!n.list) continue;
        const std::string& h = n.head();
        if (opt.import_board && (h == "gr_line" || h == "gr_arc" || h == "gr_rect" || h == "gr_circle" || h == "gr_poly") && on_edge(n))
            add_outline_item(n, b.outline);
        if (h != "footprint" && h != "module") continue;
        CadComp c;
        std::string name = n.str(1);
        if (auto p = name.find(':'); p != std::string::npos) name = name.substr(p + 1);
        const SNode* at = n.get("at");
        c.pos = xy(at);
        c.rot = at ? at->num(3) : 0;
        const SNode* layer = n.get("layer");
        c.bottom = layer && layer->str(1) == "B.Cu";
        c.ref = prop_value(n, "reference");
        c.value = prop_value(n, "value");
        for (const char* k : {"MPN", "Manufacturer_Part_Number", "Manufacturer Part Number", "PartNumber", "Part Number", "MFR_PN", "PN"}) {
            std::string v = prop_value(n, k);
            if (!v.empty()) { c.part = v; break; }
        }
        CadPackage P;
        P.name = name;
        int npth = 0, pads = 0;
        for (auto& pd : n.kids) {
            if (!pd.list || pd.head() != "pad") continue;
            ++pads;
            const std::string type = pd.str(2), shape = pd.str(3);
            if (type == "np_thru_hole") { ++npth; continue; }
            CadPin pin;
            pin.name = pd.str(1);
            const SNode* pat = pd.get("at");
            Vec2 lu{pat ? pat->num(1) : 0, pat ? -pat->num(2) : 0};
            double pa = pat ? pat->num(3) : 0;
            double lrot = pa - c.rot;
            if (c.bottom) { lu.x = -lu.x; lrot = -lrot; }
            pin.pos = lu;
            pin.rot = lrot;
            if (const SNode* sz = pd.get("size")) { pin.w = sz->num(1); pin.h = sz->num(2, pin.w); }
            if (const SNode* dr = pd.get("drill")) {
                size_t i = 1;
                if (dr->str(1) == "oval") i = 2;
                pin.drill = dr->num(i);
                if (i == 2) pin.drill = std::min(pin.drill, dr->num(3, pin.drill));
            }
            pin.th_hint = type == "thru_hole";
            if (shape == "circle") pin.shape = PadShape::Round;
            else if (shape == "oval") pin.shape = PadShape::Oblong;
            else if (shape == "rect" && std::fabs(pin.w - pin.h) < 1e-9) pin.shape = PadShape::Square;
            else pin.shape = PadShape::Rect;
            P.pins.push_back(pin);
        }
        if (pads > 0 && npth == pads) { ++mech; continue; }     // mounting hole / mechanical only
        sort_pins_natural(P.pins);
        fab_outline(n, c.bottom, P.body);      // CAD body outline (empty => derived from pads)
        c.package = b.add_package(P);
        if (c.ref.empty()) { c.ref = "?" + std::to_string(++anon); b.warn("footprint without reference"); }
        b.add_component(c);
    }
    if (mech) b.warn(std::to_string(mech) + " mechanical footprint(s) (NPTH only) skipped");
    return b.commit(prj, err);
}

bool import_kicad_pcb_file(const std::string& path, Project& prj, const CadImportOptions& opt, CadImportResult& res,
                           std::string& err) {
    std::string text;
    if (!read_file(path, text, err)) return false;
    return import_kicad_pcb_text(text, prj, opt, res, err);
}

} // namespace pcbp::io
