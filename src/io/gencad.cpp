// ============================================================================
// gencad.cpp — GenCAD 1.4 import / export. See gencad.h.
// ============================================================================
#include "gencad.h"
#include "io_util.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <cstdlib>
#include <utility>

namespace pcbp::io {

namespace {

// ---------------------------------------------------------------------------
// Parsing helpers
// ---------------------------------------------------------------------------
struct GLine {
    std::string key;                    // upper-cased keyword
    std::vector<std::string> tok;       // tokens after keyword (quotes removed)
    std::string rest;                   // raw text after keyword (trimmed, unquoted)
    int lineno = 0;
};

GLine parse_line(const std::string& raw, int lineno) {
    GLine g;
    g.lineno = lineno;
    std::string s = trim(raw);
    size_t sp = 0;
    while (sp < s.size() && !std::isspace((unsigned char)s[sp])) ++sp;
    g.key = to_upper(s.substr(0, sp));
    g.rest = trim(s.substr(sp));
    if (g.rest.size() >= 2 && g.rest.front() == '"' && g.rest.back() == '"' &&
        g.rest.find('"', 1) == g.rest.size() - 1)
        g.rest = g.rest.substr(1, g.rest.size() - 2);
    g.tok = split_ws_quoted(s.substr(sp));
    return g;
}

double num(const GLine& g, size_t i, double def = 0) {
    if (i >= g.tok.size()) return def;
    auto v = parse_num(g.tok[i]);
    return v ? *v : def;
}

bool geom_prim(const GLine& g, double k, GPrim& out) {
    if (g.key == "LINE" && g.tok.size() >= 4) {
        out = GPrim{};
        out.kind = GKind::Line;
        out.pts = {{num(g, 0) * k, num(g, 1) * k}, {num(g, 2) * k, num(g, 3) * k}};
        return true;
    }
    if (g.key == "ARC" && g.tok.size() >= 6) {
        Vec2 s{num(g, 0) * k, num(g, 1) * k}, e{num(g, 2) * k, num(g, 3) * k}, c{num(g, 4) * k, num(g, 5) * k};
        out = GPrim{};
        out.kind = GKind::Arc;
        out.pts = {c};
        out.radius = dist(s, c);
        double a0 = norm_deg(rad2deg(std::atan2(s.y - c.y, s.x - c.x)));
        double ae = norm_deg(rad2deg(std::atan2(e.y - c.y, e.x - c.x)));
        double sw = norm_deg(ae - a0);
        if (sw < 1e-6) sw = 360.0;               // start == end: full circle
        out.a0 = a0;
        out.a1 = sw;
        return true;
    }
    if (g.key == "CIRCLE" && g.tok.size() >= 3) {
        out = GPrim{};
        out.kind = GKind::Circle;
        out.pts = {{num(g, 0) * k, num(g, 1) * k}};
        out.radius = num(g, 2) * k;
        return true;
    }
    if (g.key == "RECTANGLE" && g.tok.size() >= 4) {
        out = GPrim{};
        out.kind = GKind::Rect;
        double x = num(g, 0) * k, y = num(g, 1) * k, w = num(g, 2) * k, h = num(g, 3) * k;
        out.pts = {{std::min(x, x + w), std::min(y, y + h)}, {std::max(x, x + w), std::max(y, y + h)}};
        return true;
    }
    return false;
}

// Arc/circle -> polyline points (CCW from a0 over sweep).
std::vector<Vec2> arc_points(Vec2 c, double r, double a0, double sweep, double step_deg) {
    int n = std::max(2, (int)std::ceil(std::fabs(sweep) / std::max(0.5, step_deg)));
    std::vector<Vec2> v;
    for (int i = 0; i <= n; ++i) {
        double a = deg2rad(a0 + sweep * i / n);
        v.push_back({c.x + r * std::cos(a), c.y + r * std::sin(a)});
    }
    return v;
}

std::vector<Vec2> prim_points(const GPrim& p, double step) {
    switch (p.kind) {
    case GKind::Line: case GKind::Polyline: return p.pts;
    case GKind::Polygon: { auto v = p.pts; if (!v.empty()) v.push_back(v[0]); return v; }
    case GKind::Arc: return arc_points(p.pts[0], p.radius, p.a0, p.a1, step);
    case GKind::Circle: return arc_points(p.pts[0], p.radius, 0, 360, step);
    case GKind::Rect: {
        Vec2 a = p.pts[0], b = p.pts[1];
        return {{a.x, a.y}, {b.x, a.y}, {b.x, b.y}, {a.x, b.y}, {a.x, a.y}};
    }
    default: return {};
    }
}

// Chain open pieces into polylines by matching endpoints.
std::vector<std::vector<Vec2>> chain(std::vector<std::vector<Vec2>> pieces, double eps) {
    std::vector<std::vector<Vec2>> out;
    std::vector<char> used(pieces.size(), 0);
    auto same = [&](Vec2 a, Vec2 b) { return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps; };
    for (size_t i = 0; i < pieces.size(); ++i) {
        if (used[i] || pieces[i].size() < 2) continue;
        used[i] = 1;
        std::vector<Vec2> poly = pieces[i];
        bool grown = true;
        while (grown && !same(poly.front(), poly.back())) {
            grown = false;
            for (size_t j = 0; j < pieces.size(); ++j) {
                if (used[j] || pieces[j].size() < 2) continue;
                auto& q = pieces[j];
                if (same(q.front(), poly.back())) { poly.insert(poly.end(), q.begin() + 1, q.end()); }
                else if (same(q.back(), poly.back())) { poly.insert(poly.end(), q.rbegin() + 1, q.rend()); }
                else if (same(q.back(), poly.front())) { poly.insert(poly.begin(), q.begin(), q.end() - 1); }
                else if (same(q.front(), poly.front())) { poly.insert(poly.begin(), q.rbegin(), q.rend() - 1); }
                else continue;
                used[j] = 1; grown = true;
                if (same(poly.front(), poly.back())) break;
            }
        }
        if (poly.size() > 2 && same(poly.front(), poly.back())) poly.back() = poly.front();
        out.push_back(std::move(poly));
    }
    return out;
}

struct PadDef {
    PadShape shape = PadShape::Rect;
    double w = 0.5, h = 0.5, drill = 0;
    Vec2 off;                           // geometry centre relative to pad origin
    bool has_geom = false;
};

struct ShapeDef {
    Package pkg;
    bool fiducial = false;
    bool insert_set = false;
    std::vector<std::pair<std::string, Pad>> pins;   // name -> pad
};

struct CompDef {
    std::string ref, device, shape;
    Vec2 pos;
    double rot = 0;
    bool bottom = false, flip = false;
    std::string mirror;
};

struct DevDef { std::string name, part, type, value, desc, package; int pincount = 0; };

PadDef finish_pad(const std::string& shape_kw, double drill, const std::vector<GPrim>& geo) {
    PadDef d;
    d.drill = drill > 0 ? drill : 0;
    BBox b = gprims_bbox(geo);
    if (!b.empty()) { d.w = b.w(); d.h = b.h(); d.off = b.center(); d.has_geom = true; }
    const std::string s = to_upper(shape_kw);
    if (s == "ROUND" || s == "CIRCLE") d.shape = std::fabs(d.w - d.h) < 1e-6 ? PadShape::Round : PadShape::Oblong;
    else if (s == "FINGER" || s == "BULLET" || s == "OBLONG") d.shape = PadShape::Oblong;
    else if (s == "POLYGON") {
        int lines = 0;
        for (auto& g : geo) if (g.kind == GKind::Line) ++lines;
        d.shape = lines == 8 ? PadShape::Octagon : PadShape::Rect;
    } else if (s == "SQUARE") d.shape = PadShape::Square;
    else d.shape = PadShape::Rect;
    return d;
}

int pin_number(const std::string& name, int ordinal) {
    if (!name.empty() && std::all_of(name.begin(), name.end(), [](char c) { return std::isdigit((unsigned char)c); }))
        return std::atoi(name.c_str());
    return ordinal;
}

} // namespace

// ---------------------------------------------------------------------------
// Import
// ---------------------------------------------------------------------------
bool import_gencad_text(const std::string& text, Project& prj, const GencadOptions& opt,
                        GencadResult& res, std::string& err) {
    res = GencadResult{};
    auto lines = split_lines(text);
    double k = 25.4;                    // mm per file unit
    std::string section;

    std::vector<GPrim> board;
    std::map<std::string, PadDef> pads;
    std::map<std::string, PadDef> stacks;
    std::vector<ShapeDef> shapes;
    std::map<std::string, size_t> shape_idx;
    std::vector<CompDef> comps;
    std::map<std::string, DevDef> devs;
    std::vector<GencadNet> nets;

    // current-record state
    std::string pad_name, pad_kw; double pad_drill = 0; std::vector<GPrim> pad_geo;
    std::string stack_name; double stack_drill = 0; bool stack_has_pad = false;
    ShapeDef* cur_shape = nullptr;
    CompDef* cur_comp = nullptr;
    DevDef* cur_dev = nullptr;
    bool saw_header = false;

    auto flush_pad = [&]() {
        if (!pad_name.empty()) pads[pad_name] = finish_pad(pad_kw, pad_drill, pad_geo);
        pad_name.clear(); pad_geo.clear();
    };

    for (size_t li = 0; li < lines.size(); ++li) {
        std::string t = trim(lines[li]);
        if (t.empty()) continue;
        if (t[0] == '$') {
            std::string sec = to_upper(t);
            if (sec.rfind("$END", 0) == 0) {
                if (section == "PADS") flush_pad();
                section.clear();
                cur_shape = nullptr; cur_comp = nullptr; cur_dev = nullptr;
            } else {
                section = sec.substr(1);
                static const std::set<std::string> known = {"HEADER", "BOARD", "PADS", "PADSTACKS", "SHAPES",
                                                            "COMPONENTS", "DEVICES", "SIGNALS"};
                if (!known.count(section)) res.skipped_sections.push_back(section);
                if (section == "HEADER") saw_header = true;
            }
            continue;
        }
        GLine g = parse_line(t, (int)li + 1);

        if (section == "HEADER") {
            if (g.key == "GENCAD") res.version = g.rest;
            else if (g.key == "USER") res.user = g.rest;
            else if (g.key == "DRAWING") res.drawing = g.rest;
            else if (g.key == "REVISION") res.revision = g.rest;
            else if (g.key == "UNITS") {
                std::string u = g.tok.empty() ? "" : to_upper(g.tok[0]);
                res.units = g.rest;
                if (u == "INCH") k = 25.4;
                else if (u == "MM" || u == "METRIC") k = 1.0;
                else if (u == "THOU" || u == "MIL" || u == "MILS") k = 0.0254;
                else if (u == "USER" && g.tok.size() > 1) k = 25.4 / std::max(1e-12, num(g, 1, 1));
                else if (u == "USERM" && g.tok.size() > 1) k = 10.0 / std::max(1e-12, num(g, 1, 1));
                else if (u == "USERMM" && g.tok.size() > 1) k = 1.0 / std::max(1e-12, num(g, 1, 1));
                else res.warnings.push_back("unknown UNITS \"" + g.rest + "\", assuming INCH");
            }
        } else if (section == "BOARD") {
            GPrim p;
            if (geom_prim(g, k, p)) { board.push_back(p); ++res.board_items; }
            // CUTOUT / MASK / ARTWORK sub-blocks: their geometry is also outline-ish; keep all.
        } else if (section == "PADS") {
            if (g.key == "PAD") {
                flush_pad();
                pad_name = g.tok.size() > 0 ? g.tok[0] : "";
                pad_kw = g.tok.size() > 1 ? g.tok[1] : "ROUND";
                pad_drill = num(g, 2, -1);
                pad_drill = pad_drill > 0 ? pad_drill * k : 0;
                ++res.pads;
            } else {
                GPrim p;
                if (geom_prim(g, k, p)) pad_geo.push_back(p);
            }
        } else if (section == "PADSTACKS") {
            if (g.key == "PADSTACK") {
                stack_name = g.tok.size() > 0 ? g.tok[0] : "";
                stack_drill = num(g, 1, -1);
                stack_drill = stack_drill > 0 ? stack_drill * k : 0;
                stack_has_pad = false;
                ++res.padstacks;
                PadDef d; d.drill = stack_drill;
                stacks[stack_name] = d;
            } else if (g.key == "PAD" && !stack_name.empty()) {
                std::string pn = g.tok.size() > 0 ? g.tok[0] : "";
                std::string layer = g.tok.size() > 1 ? to_upper(g.tok[1]) : "";
                auto it = pads.find(pn);
                if (it != pads.end() && (!stack_has_pad || layer == "TOP")) {
                    PadDef d = it->second;
                    d.drill = std::max(d.drill, stack_drill);
                    stacks[stack_name] = d;
                    stack_has_pad = true;
                }
            }
        } else if (section == "SHAPES") {
            if (g.key == "SHAPE") {
                ShapeDef s;
                s.pkg.name = g.rest.empty() ? "SHAPE" + std::to_string(shapes.size() + 1) : g.tok[0];
                shape_idx[s.pkg.name] = shapes.size();
                shapes.push_back(std::move(s));
                cur_shape = &shapes.back();
                ++res.shapes;
            } else if (cur_shape) {
                GPrim p;
                if (g.key == "PIN" && g.tok.size() >= 4) {
                    Pad pad;
                    std::string pname = g.tok[0], padname = g.tok[1];
                    Vec2 at{num(g, 2) * k, num(g, 3) * k};
                    double rot = num(g, 5, 0);
                    PadDef d;
                    auto it = pads.find(padname);
                    if (it != pads.end()) d = it->second;
                    else if (auto is = stacks.find(padname); is != stacks.end()) d = is->second;
                    else res.warnings.push_back("line " + std::to_string(g.lineno) + ": unknown pad " + padname);
                    pad.pos = at + rotate(d.off, rot);
                    pad.w = d.w; pad.h = d.h; pad.shape = d.shape; pad.drill = d.drill;
                    pad.rot = norm_deg(rot);
                    pad.pin = pin_number(pname, (int)cur_shape->pins.size() + 1);
                    cur_shape->pins.push_back({pname, pad});
                } else if (g.key == "INSERT") {
                    std::string v = g.tok.empty() ? "" : to_upper(g.tok[0]);
                    cur_shape->insert_set = true;
                    cur_shape->pkg.smt = (v == "SMD" || v == "SMT");
                } else if (g.key == "HEIGHT") {
                    cur_shape->pkg.height = num(g, 0) * k;
                } else if (g.key == "FIDUCIAL") {
                    cur_shape->fiducial = true;
                    // FIDUCIAL [name] pad x y ...: take the first two consecutive numbers as position
                    for (size_t i = 0; i + 1 < g.tok.size(); ++i) {
                        auto a = parse_num(g.tok[i]), b = parse_num(g.tok[i + 1]);
                        if (a && b) {
                            Pad pad; pad.pos = {*a * k, *b * k}; pad.shape = PadShape::Round;
                            std::string padname = i > 0 ? g.tok[i - 1] : "";
                            if (auto it = pads.find(padname); it != pads.end()) { pad.w = it->second.w; pad.h = it->second.h; pad.shape = it->second.shape; }
                            cur_shape->pins.push_back({"FID", pad});
                            break;
                        }
                    }
                } else if (geom_prim(g, k, p)) {
                    p.width = 0;
                    cur_shape->pkg.body.push_back(p);
                }
            }
        } else if (section == "COMPONENTS") {
            if (g.key == "COMPONENT") {
                comps.push_back(CompDef{});
                cur_comp = &comps.back();
                cur_comp->ref = g.rest;
                ++res.components;
            } else if (cur_comp) {
                if (g.key == "DEVICE") cur_comp->device = g.rest;
                else if (g.key == "PLACE") cur_comp->pos = {num(g, 0) * k, num(g, 1) * k};
                else if (g.key == "LAYER") cur_comp->bottom = !g.tok.empty() && to_upper(g.tok[0]) == "BOTTOM";
                else if (g.key == "ROTATION") cur_comp->rot = num(g, 0);
                else if (g.key == "SHAPE") {
                    cur_comp->shape = g.tok.empty() ? "" : g.tok[0];
                    for (size_t i = 1; i < g.tok.size(); ++i) {
                        std::string f = to_upper(g.tok[i]);
                        if (f == "FLIP") cur_comp->flip = true;
                        else if (f == "MIRRORX" || f == "MIRRORY") cur_comp->mirror = f;
                    }
                }
            }
        } else if (section == "DEVICES") {
            if (g.key == "DEVICE") {
                DevDef d; d.name = g.rest;
                devs[d.name] = d;
                cur_dev = &devs[d.name];
                ++res.devices;
            } else if (cur_dev) {
                if (g.key == "PART") cur_dev->part = g.rest;
                else if (g.key == "TYPE") cur_dev->type = g.rest;
                else if (g.key == "VALUE") cur_dev->value = g.rest;
                else if (g.key == "DESC") cur_dev->desc = g.rest;
                else if (g.key == "PACKAGE") cur_dev->package = g.rest;
                else if (g.key == "PINCOUNT") cur_dev->pincount = (int)num(g, 0);
            }
        } else if (section == "SIGNALS") {
            if (g.key == "SIGNAL") nets.push_back({g.rest, {}});
            else if (g.key == "NODE" && !nets.empty() && g.tok.size() >= 2)
                nets.back().nodes.push_back({g.tok[0], g.tok[1]});
        }
    }
    if (!saw_header && comps.empty() && shapes.empty()) { err = "not a GenCAD file (no $HEADER / $SHAPES / $COMPONENTS)"; return false; }
    res.unit_mm = k;
    res.nets = std::move(nets);

    // ---------------- build project ----------------
    if (opt.replace) {
        prj.comps.clear();
        if (opt.import_packages) prj.packages.clear();
        prj.parts.clear();
        if (opt.import_board) prj.board_frame.clear();
    }
    std::set<std::string> fid_shapes;
    if (opt.import_packages) {
        for (auto& s : shapes) {
            Package p = s.pkg;
            for (auto& [n, pad] : s.pins) p.pads.push_back(pad);
            p.leads = (int)p.pads.size();
            {   // R18c: drills decide SMT / TH; INSERT (declared type) is ignored for this
                bool th = false;
                for (auto& pd : p.pads) if (pd.drill > 0) th = true;
                p.smt = !th;
            }
            if (p.body.empty()) p.auto_body();
            if (s.fiducial) fid_shapes.insert(p.name);
            if (!opt.overwrite_packages && prj.packages.count(p.name)) continue;
            prj.packages[p.name] = std::move(p);
        }
    } else {
        for (auto& s : shapes) if (s.fiducial) fid_shapes.insert(s.pkg.name);
    }
    for (auto& [name, d] : devs) {
        std::string number = !d.part.empty() ? d.part : d.name;
        if (number.empty()) continue;           // CircuitCAM writes an unnamed device for comps w/o PN
        PartNumber& pn = prj.parts[number];
        pn.number = number;
        if (!d.desc.empty()) pn.description = d.desc;
        else if (!d.value.empty()) pn.description = d.value;
        else if (!d.type.empty() && pn.description.empty()) pn.description = d.type;
        if (!d.package.empty()) pn.package = d.package;
    }
    for (auto& c : comps) {
        Component n;
        n.ref = c.ref;
        n.pos = c.pos;
        n.rot = norm_deg(c.rot);
        n.side = (c.bottom || c.flip) ? Side::Bottom : Side::Top;
        n.package = c.shape;
        auto dv = devs.find(c.device);
        if (dv != devs.end()) n.number = !dv->second.part.empty() ? dv->second.part : dv->second.name;
        else n.number = c.device;
        if (!n.number.empty()) {
            PartNumber& pn = prj.parts[n.number];
            pn.number = n.number;
            if (pn.package.empty()) pn.package = c.shape;
        }
        n.cad_pos = n.pos; n.rot_imported = n.rot;
        if (fid_shapes.count(c.shape) || (opt.fiducial_by_name && istarts_with(c.ref, "FID"))) {
            n.feature = Feature::Fiducial;
            std::string d;
            for (char ch : c.ref) if (std::isdigit((unsigned char)ch)) d += ch;
            n.fid_number = d.empty() ? 0 : std::atoi(d.c_str());
            ++res.fiducials;
        }
        if (!c.shape.empty() && !shape_idx.count(c.shape) && !prj.packages.count(c.shape))
            res.warnings.push_back(c.ref + ": unknown shape " + c.shape);
        prj.add_component(n);
    }
    if (opt.import_board && !board.empty()) {
        std::vector<std::vector<Vec2>> pieces, closed;
        for (auto& p : board) {
            auto pts = prim_points(p, opt.arc_seg_deg);
            if (p.kind == GKind::Circle || p.kind == GKind::Rect) closed.push_back(pts);
            else pieces.push_back(pts);
        }
        auto polys = chain(pieces, 0.002);
        for (auto& c : closed) polys.push_back(c);
        res.board_polylines = (int)polys.size();
        for (auto& p : polys) prj.board_frame.push_back(std::move(p));
        if (!polys.empty()) prj.board_outline_src = OutlineSource::Imported;   // R16 provenance
    }
    if (prj.info.name.empty()) prj.info.name = res.drawing;
    if (prj.info.assembly_rev.empty()) prj.info.assembly_rev = res.revision;
    prj.dirty = true;
    return true;
}

bool import_gencad_file(const std::string& path, Project& prj, const GencadOptions& opt,
                        GencadResult& res, std::string& err) {
    std::string text;
    if (!read_file(path, text, err)) return false;
    if (!import_gencad_text(text, prj, opt, res, err)) return false;
    if (prj.info.original_filename.empty()) prj.info.original_filename = path;
    return true;
}

std::string GencadResult::describe() const {
    std::ostringstream s;
    s << "GenCAD " << version << " \"" << user << "\" drawing \"" << drawing << "\" rev " << revision
      << ", units " << units << " (" << unit_mm << " mm/unit)\n"
      << "pads " << pads << ", padstacks " << padstacks << ", shapes " << shapes << ", components " << components
      << " (fiducials " << fiducials << "), devices " << devices << ", nets " << nets.size()
      << ", board items " << board_items << " -> " << board_polylines << " polylines";
    if (!skipped_sections.empty()) {
        s << "\nskipped sections:";
        for (auto& x : skipped_sections) s << " " << x;
    }
    for (auto& w : warnings) s << "\nwarning: " << w;
    return s.str();
}

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------
namespace {

struct Writer {
    std::string out;
    const char* eol;
    int dec;
    void line(const std::string& s) { out += s; out += eol; }
    std::string n(double mm) const { return fmt_fixed(mm / kMmPerInch, dec); }
};

std::string pad_key(const Pad& p) {
    std::ostringstream s;
    s << (int)p.shape << ":" << fmt_fixed(p.w / kMmPerInch, 4) << ":" << fmt_fixed(p.h / kMmPerInch, 4)
      << ":" << fmt_fixed(p.drill / kMmPerInch, 4);
    return s.str();
}

void write_prim(Writer& w, const GPrim& g) {
    switch (g.kind) {
    case GKind::Line:
    case GKind::Polyline:
    case GKind::Polygon: {
        auto pts = g.pts;
        if (g.kind == GKind::Polygon && !pts.empty()) pts.push_back(pts[0]);
        for (size_t i = 1; i < pts.size(); ++i)
            w.line("LINE " + w.n(pts[i - 1].x) + " " + w.n(pts[i - 1].y) + " " + w.n(pts[i].x) + " " + w.n(pts[i].y));
        break;
    }
    case GKind::Rect:
        if (g.pts.size() >= 2) {
            if (std::fabs(g.rot) < 1e-9) {
                Vec2 a = g.pts[0], b = g.pts[1];
                w.line("RECTANGLE " + w.n(std::min(a.x, b.x)) + " " + w.n(std::min(a.y, b.y)) + " " +
                       w.n(std::fabs(b.x - a.x)) + " " + w.n(std::fabs(b.y - a.y)));
            } else {
                GPrim p; p.kind = GKind::Polygon;
                Vec2 c = (g.pts[0] + g.pts[1]) * 0.5;
                Vec2 q[4] = {{g.pts[0].x, g.pts[0].y}, {g.pts[1].x, g.pts[0].y}, {g.pts[1].x, g.pts[1].y}, {g.pts[0].x, g.pts[1].y}};
                for (auto& v : q) p.pts.push_back(rotate_about(v, c, g.rot));
                write_prim(w, p);
            }
        }
        break;
    case GKind::Circle:
        if (!g.pts.empty()) w.line("CIRCLE " + w.n(g.pts[0].x) + " " + w.n(g.pts[0].y) + " " + w.n(g.radius));
        break;
    case GKind::Arc:
        if (!g.pts.empty()) {
            double a0 = g.a0, sw = g.a1;
            if (sw < 0) { a0 += sw; sw = -sw; }
            Vec2 c = g.pts[0];
            if (sw >= 360.0 - 1e-9) {
                w.line("CIRCLE " + w.n(c.x) + " " + w.n(c.y) + " " + w.n(g.radius));
                break;
            }
            Vec2 s{c.x + g.radius * std::cos(deg2rad(a0)), c.y + g.radius * std::sin(deg2rad(a0))};
            Vec2 e{c.x + g.radius * std::cos(deg2rad(a0 + sw)), c.y + g.radius * std::sin(deg2rad(a0 + sw))};
            w.line("ARC " + w.n(s.x) + " " + w.n(s.y) + " " + w.n(e.x) + " " + w.n(e.y) + " " + w.n(c.x) + " " + w.n(c.y));
        }
        break;
    case GKind::Group:
        for (auto& ch : g.children) write_prim(w, ch);
        break;
    default:
        break;
    }
}

std::string gname(const std::string& s) {
    // GenCAD names cannot contain blanks unless quoted
    if (s.find_first_of(" \t") != std::string::npos) return "\"" + s + "\"";
    return s;
}

} // namespace

bool export_gencad(const Project& prj, const GencadExportOptions& opt, std::string& out, std::string& err) {
    Writer w{std::string(), opt.crlf ? "\r\n" : "\n", std::clamp(opt.decimals, 2, 8)};

    // ---- component selection ----
    std::vector<const Component*> sel;
    for (auto& c : prj.comps) {
        if (opt.side == SideFilter::Top && c.side != Side::Top) continue;
        if (opt.side == SideFilter::Bottom && c.side != Side::Bottom) continue;
        if (!opt.include_fiducials && c.feature == Feature::Fiducial) continue;
        if (c.feature == Feature::BadMark || c.feature == Feature::Hole) continue;
        if (!opt.model_points.empty() &&
            std::find(opt.model_points.begin(), opt.model_points.end(), c.assigned_point) == opt.model_points.end())
            continue;
        sel.push_back(&c);
    }
    if (sel.empty() && prj.comps.size()) { err = "no components match the export filter"; return false; }

    // ---- shapes used (first-use order) ----
    std::vector<std::string> shape_order;
    std::map<std::string, const Package*> shape_pkg;
    auto shape_of = [&](const Component& c) -> std::string {
        const Package* p = prj.package_of(c);
        std::string nm = p ? p->name : (c.package.empty() ? std::string("NOSHAPE") : c.package);
        if (nm.empty()) nm = "NOSHAPE";
        if (!shape_pkg.count(nm)) { shape_pkg[nm] = p; shape_order.push_back(nm); }
        return nm;
    };
    std::vector<std::string> comp_shape;
    for (auto* c : sel) comp_shape.push_back(shape_of(*c));

    // ---- pads (dedupe by geometry) ----
    std::map<std::string, std::string> pad_names;
    std::vector<std::pair<std::string, Pad>> pad_list;
    for (auto& nm : shape_order) {
        const Package* p = shape_pkg[nm];
        if (!p) continue;
        for (auto& pd : p->pads) {
            std::string key = pad_key(pd);
            if (pad_names.count(key)) continue;
            std::string pn = "Pad" + std::to_string(pad_list.size() + 1);
            pad_names[key] = pn;
            pad_list.push_back({pn, pd});
        }
    }

    // ---- header ----
    w.line("$HEADER");
    w.line("GENCAD 1.4");
    w.line("USER \"" + opt.user + "\"");
    w.line("DRAWING " + (prj.info.name.empty() ? std::string("PCB Plotter") : prj.info.name));
    w.line("REVISION " + (prj.info.assembly_rev.empty() ? std::string("A") : prj.info.assembly_rev));
    w.line("UNITS INCH");
    w.line("ORIGIN 0 0");
    w.line("INTERTRACK 0");
    w.line("$ENDHEADER");
    w.line("");

    // ---- board ----
    w.line("$BOARD");
    if (opt.include_board)
        for (auto& poly : prj.board_frame)
            for (size_t i = 1; i < poly.size(); ++i)
                w.line("LINE " + w.n(poly[i - 1].x) + " " + w.n(poly[i - 1].y) + " " + w.n(poly[i].x) + " " + w.n(poly[i].y));
    w.line("$ENDBOARD");
    w.line("");

    // ---- pads ----
    w.line("$PADS");
    for (auto& [pn, pd] : pad_list) {
        const std::string drill = pd.drill > 0 ? w.n(pd.drill) : std::string("-1");
        const double hw = pd.w * 0.5, hh = pd.h * 0.5;
        switch (pd.shape) {
        case PadShape::Round:
            w.line("PAD " + pn + " ROUND " + drill);
            w.line("CIRCLE 0.0000 0.0000 " + w.n(std::max(hw, hh)));
            break;
        case PadShape::Oblong:
            w.line("PAD " + pn + " FINGER " + drill);
            w.line("RECTANGLE " + w.n(-hw) + " " + w.n(-hh) + " " + w.n(pd.w) + " " + w.n(pd.h));
            break;
        case PadShape::Octagon: {
            w.line("PAD " + pn + " POLYGON " + drill);
            const double cx = std::min(hw, hh) * (1.0 - 1.0 / (1.0 + std::sqrt(2.0)));
            Vec2 o[8] = {{-hw + cx, -hh}, {hw - cx, -hh}, {hw, -hh + cx}, {hw, hh - cx},
                         {hw - cx, hh}, {-hw + cx, hh}, {-hw, hh - cx}, {-hw, -hh + cx}};
            for (int i = 0; i < 8; ++i) {
                Vec2 a = o[i], b = o[(i + 1) % 8];
                w.line("LINE " + w.n(a.x) + " " + w.n(a.y) + " " + w.n(b.x) + " " + w.n(b.y));
            }
            break;
        }
        default:
            w.line("PAD " + pn + " RECTANGULAR " + drill);
            w.line("RECTANGLE " + w.n(-hw) + " " + w.n(-hh) + " " + w.n(pd.w) + " " + w.n(pd.h));
            break;
        }
    }
    w.line("$ENDPADS");
    w.line("");

    // ---- shapes ----
    w.line("$SHAPES");
    for (auto& nm : shape_order) {
        const Package* p = shape_pkg[nm];
        w.line("SHAPE " + gname(nm));
        if (!p) { w.line("RECTANGLE -0.0200 -0.0200 0.0400 0.0400"); continue; }
        for (auto& g : p->body) write_prim(w, g);
        int ord = 0;
        for (auto& pd : p->pads) {
            ++ord;
            std::string pin = pd.pin > 0 ? std::to_string(pd.pin) : std::to_string(ord);
            w.line("PIN " + pin + " " + pad_names[pad_key(pd)] + " " + w.n(pd.pos.x) + " " + w.n(pd.pos.y) +
                   " TOP " + fmt_fixed(pd.rot, 1) + " 0");
        }
    }
    w.line("$ENDSHAPES");
    w.line("");

    // ---- components ----
    w.line("$COMPONENTS");
    for (size_t i = 0; i < sel.size(); ++i) {
        const Component& c = *sel[i];
        w.line("COMPONENT " + gname(c.ref));
        w.line("DEVICE " + c.number);
        w.line("PLACE " + w.n(c.pos.x) + " " + w.n(c.pos.y));
        w.line(std::string("LAYER ") + (c.side == Side::Bottom ? "BOTTOM" : "TOP"));
        w.line("ROTATION " + fmt_fixed(norm_deg(c.rot), 1));
        w.line("SHAPE " + gname(comp_shape[i]) + (c.side == Side::Bottom ? " MIRRORY FLIP" : " 0 0"));
    }
    w.line("$ENDCOMPONENTS");
    w.line("");

    // ---- devices ----
    w.line("$DEVICES");
    std::set<std::string> done;
    for (size_t i = 0; i < sel.size(); ++i) {
        const Component& c = *sel[i];
        if (c.number.empty() || !done.insert(c.number).second) continue;
        const PartNumber* pn = prj.part_of(c);
        const Package* p = shape_pkg[comp_shape[i]];
        std::string pkg = pn && !pn->package.empty() ? pn->package : comp_shape[i];
        w.line("DEVICE " + c.number);
        w.line("PART " + c.number);
        w.line("PACKAGE " + pkg);
        w.line("PINCOUNT " + std::to_string(p ? (int)p->pads.size() : 0));
        if (pn && !pn->description.empty()) w.line("DESC " + pn->description);
    }
    w.line("$ENDDEVICES");
    out = std::move(w.out);
    return true;
}

} // namespace pcbp::io
