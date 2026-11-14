// ============================================================================
// eagle_brd.cpp — EAGLE .brd import (see eagle_brd.h).
// ============================================================================
#include "eagle_brd.h"
#include "io_util.h"
#include "xml_lite.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace pcbp::io {
namespace {

void parse_rot(const std::string& r, double& deg, bool& mirror) {
    deg = 0; mirror = false;
    size_t i = 0;
    while (i < r.size() && (r[i] == 'M' || r[i] == 'S' || r[i] == 'm' || r[i] == 's')) {
        if (r[i] == 'M' || r[i] == 'm') mirror = true;
        ++i;
    }
    if (i < r.size() && (r[i] == 'R' || r[i] == 'r')) ++i;
    deg = num_or(r.substr(i), 0);
}

bool on_dim(const XmlNode& n) { return n.attr("layer") == "20"; }

void plain_outline(const XmlNode& plain, OutlineBuilder& ob) {
    for (auto& c : plain.children) {
        if (!on_dim(c)) continue;
        const std::string_view n = c.local();
        if (ieq(n, "wire")) {
            Vec2 a{c.attr_num("x1"), c.attr_num("y1")}, b{c.attr_num("x2"), c.attr_num("y2")};
            const double curve = c.attr_num("curve");
            if (std::fabs(curve) < 1e-9) ob.add_line(a, b);
            else ob.add_bulge(a, b, std::tan(deg2rad(curve) / 4));
        } else if (ieq(n, "circle")) {
            ob.add_circle({c.attr_num("x"), c.attr_num("y")}, c.attr_num("radius"));
        } else if (ieq(n, "rectangle")) {
            Vec2 a{c.attr_num("x1"), c.attr_num("y1")}, b{c.attr_num("x2"), c.attr_num("y2")};
            ob.add_polyline({a, {b.x, a.y}, b, {a.x, b.y}}, true);
        } else if (ieq(n, "polygon")) {
            auto vs = c.kids("vertex");
            for (size_t i = 0; i < vs.size(); ++i) {
                const XmlNode* v = vs[i];
                const XmlNode* w = vs[(i + 1) % vs.size()];
                Vec2 a{v->attr_num("x"), v->attr_num("y")}, b{w->attr_num("x"), w->attr_num("y")};
                const double curve = v->attr_num("curve");
                if (std::fabs(curve) < 1e-9) ob.add_line(a, b);
                else ob.add_bulge(a, b, std::tan(deg2rad(curve) / 4));
            }
        }
    }
}

CadPackage read_package(const XmlNode& p) {
    CadPackage cp;
    cp.name = p.attr("name");
    for (auto& c : p.children) {
        const std::string_view n = c.local();
        CadPin pin;
        bool mir = false;
        if (ieq(n, "smd")) {
            pin.name = c.attr("name");
            pin.pos = {c.attr_num("x"), c.attr_num("y")};
            pin.w = c.attr_num("dx"); pin.h = c.attr_num("dy");
            parse_rot(c.attr("rot"), pin.rot, mir);
            const double rnd = c.attr_num("roundness");
            pin.shape = rnd >= 100 ? (std::fabs(pin.w - pin.h) < 1e-9 ? PadShape::Round : PadShape::Oblong) : PadShape::Rect;
        } else if (ieq(n, "pad")) {
            pin.name = c.attr("name");
            pin.pos = {c.attr_num("x"), c.attr_num("y")};
            pin.drill = c.attr_num("drill");
            double d = c.attr_num("diameter");
            if (d <= 0) d = pin.drill + 2 * std::max(0.25 * pin.drill, 0.254);
            pin.w = pin.h = d;
            parse_rot(c.attr("rot"), pin.rot, mir);
            const std::string sh = to_lower(c.attr("shape", "round"));
            if (sh == "square") pin.shape = PadShape::Square;
            else if (sh == "octagon") pin.shape = PadShape::Octagon;
            else if (sh == "long" || sh == "offset") { pin.shape = PadShape::Oblong; pin.w = 2 * d; }
            else pin.shape = PadShape::Round;
            pin.th_hint = true;
        } else continue;
        cp.pins.push_back(pin);
    }
    sort_pins_natural(cp.pins);
    // body outline: tDocu (51) wires/circles/rectangles, else tPlace (21) silkscreen
    for (const char* layer : {"51", "21"}) {
        for (auto& c : p.children) {
            if (c.attr("layer") != layer) continue;
            const std::string_view n = c.local();
            GPrim g;
            g.width = std::max(0.05, c.attr_num("width"));
            if (ieq(n, "wire")) {
                Vec2 a{c.attr_num("x1"), c.attr_num("y1")}, b{c.attr_num("x2"), c.attr_num("y2")};
                const double curve = c.attr_num("curve");
                const double ch = dist(a, b);
                if (std::fabs(curve) < 1e-9 || ch < 1e-9) { g.kind = GKind::Line; g.pts = {a, b}; }
                else {
                    const double th = deg2rad(curve), r = ch / (2 * std::sin(std::fabs(th) / 2));
                    Vec2 m{(a.x + b.x) / 2, (a.y + b.y) / 2}, u{(b.x - a.x) / ch, (b.y - a.y) / ch};
                    const double hgt = std::sqrt(std::max(0.0, r * r - ch * ch / 4)) * (std::fabs(th) > kPi ? -1 : 1);
                    const double sgn = curve > 0 ? 1 : -1;      // CCW arc: centre left of a->b
                    Vec2 c0{m.x - u.y * hgt * sgn, m.y + u.x * hgt * sgn};
                    g.kind = GKind::Arc; g.pts = {c0}; g.radius = r;
                    g.a0 = rad2deg(std::atan2(a.y - c0.y, a.x - c0.x)); g.a1 = curve;
                }
            } else if (ieq(n, "circle")) { g.kind = GKind::Circle; g.pts = {{c.attr_num("x"), c.attr_num("y")}}; g.radius = c.attr_num("radius"); }
            else if (ieq(n, "rectangle")) { g.kind = GKind::Rect; g.pts = {{c.attr_num("x1"), c.attr_num("y1")}, {c.attr_num("x2"), c.attr_num("y2")}}; g.filled = true; }
            else continue;
            cp.body.push_back(std::move(g));
        }
        if (!cp.body.empty()) break;
    }
    return cp;
}

} // namespace

int sniff_eagle_brd(const std::string& path, const std::string& head) {
    (void)path;
    if (head.find("<eagle") != std::string::npos && head.find("<board") != std::string::npos) return 95;
    if (head.find("<!DOCTYPE eagle") != std::string::npos) return 85;
    if (head.find("<eagle") != std::string::npos) return 70;
    return 0;
}

bool import_eagle_brd_text(const std::string& text, Project& prj, const CadImportOptions& opt, CadImportResult& res,
                           std::string& err) {
    res = CadImportResult{};
    res.format = "EAGLE board";
    res.units = "mm";
    if (text.size() >= 4 && (unsigned char)text[0] == 0x10 && (unsigned char)text[1] == 0x80) {
        err = "EAGLE: binary (pre-6.0) board files are not supported; re-save with EAGLE 6+";
        return false;
    }
    XmlNode root;
    if (!xml_parse(text, root, err)) return false;
    if (!ieq(root.local(), "eagle")) { err = "EAGLE: root element is <" + root.name + ">"; return false; }
    res.source_name = "version " + root.attr("version");
    const XmlNode* board = root.find("board");
    if (!board) { err = "EAGLE: no <board> (schematic or library file?)"; return false; }
    CadBuilder b(opt, res);
    if (opt.import_board)
        if (const XmlNode* plain = board->child("plain")) plain_outline(*plain, b.outline);

    std::map<std::string, CadPackage> lib;   // "library\npackage"
    if (const XmlNode* libs = board->child("libraries")) {
        for (auto* L : libs->kids("library")) {
            if (const XmlNode* pk = L->child("packages"))
                for (auto* p : pk->kids("package")) lib[L->attr("name") + "\n" + p->attr("name")] = read_package(*p);
        }
    }
    std::map<std::string, std::string> fin;
    const XmlNode* els = board->child("elements");
    if (els) {
        for (auto* e : els->kids("element")) {
            CadComp c;
            c.ref = e->attr("name");
            c.value = e->attr("value");
            c.pos = {e->attr_num("x"), e->attr_num("y")};
            parse_rot(e->attr("rot"), c.rot, c.bottom);
            for (auto* a : e->kids("attribute")) {
                const std::string k = norm_key(a->attr("name"));
                if ((k == "mpn" || k == "partno" || k == "partnumber" || k == "manufacturerpartnumber" || k == "mfrpn") &&
                    !a->attr("value").empty())
                    c.part = a->attr("value");
            }
            const std::string key = e->attr("library") + "\n" + e->attr("package");
            auto it = lib.find(key);
            if (it == lib.end()) {
                b.warn(c.ref + ": package " + e->attr("package") + " not found in library " + e->attr("library"));
                c.package = e->attr("package");
            } else {
                auto f = fin.find(key);
                if (f == fin.end()) f = fin.emplace(key, b.add_package(it->second)).first;
                c.package = f->second;
            }
            b.add_component(c);
        }
    }
    return b.commit(prj, err);
}

bool import_eagle_brd_file(const std::string& path, Project& prj, const CadImportOptions& opt, CadImportResult& res,
                           std::string& err) {
    std::string text;
    if (!read_file(path, text, err)) return false;
    return import_eagle_brd_text(text, prj, opt, res, err);
}

} // namespace pcbp::io
