// ============================================================================
// dxf_outline.cpp — DXF outline import (see dxf_outline.h).
// ============================================================================
#include "dxf_outline.h"
#include "io_util.h"

#include <cmath>
#include <set>

namespace pcbp::io {
namespace {

struct Pair { int code; std::string val; };

bool read_pairs(const std::string& text, std::vector<Pair>& out, std::string& err) {
    auto lines = split_lines(text);
    for (size_t i = 0; i + 1 < lines.size(); i += 2) {
        auto c = parse_num(lines[i]);
        if (!c) { err = "DXF: bad group code at line " + std::to_string(i + 1); return false; }
        out.push_back({(int)*c, trim(lines[i + 1])});
    }
    return true;
}

struct Ent {
    std::string type;
    std::vector<Pair> g;
    std::string layer() const { for (auto& p : g) if (p.code == 8) return p.val; return "0"; }
    double num(int code, double def = 0) const { for (auto& p : g) if (p.code == code) return num_or(p.val, def); return def; }
    bool has(int code) const { for (auto& p : g) if (p.code == code) return true; return false; }
};

double insunits_scale(int u, std::string& name) {
    switch (u) {
    case 1: name = "inch"; return 25.4;
    case 2: name = "feet"; return 304.8;
    case 4: name = "mm"; return 1.0;
    case 5: name = "cm"; return 10.0;
    case 6: name = "m"; return 1000.0;
    case 8: name = "microinch"; return 25.4e-6;
    case 9: name = "mil"; return 0.0254;
    case 10: name = "yard"; return 914.4;
    case 13: name = "um"; return 0.001;
    case 14: name = "dm"; return 100.0;
    default: return 0;
    }
}

void collect(const std::vector<Pair>& P, std::vector<Ent>& ents, int& insunits) {
    std::string section;
    bool in_ent = false;
    for (size_t i = 0; i < P.size(); ++i) {
        const Pair& p = P[i];
        if (p.code == 0) {
            if (p.val == "SECTION") {
                if (i + 1 < P.size() && P[i + 1].code == 2) section = P[i + 1].val;
                in_ent = false;
                continue;
            }
            if (p.val == "ENDSEC") { section.clear(); in_ent = false; continue; }
            if (section == "ENTITIES") { ents.push_back({p.val, {}}); in_ent = true; }
            continue;
        }
        if (section == "HEADER" && p.code == 9 && p.val == "$INSUNITS") {
            for (size_t j = i + 1; j < P.size() && P[j].code != 9 && P[j].code != 0; ++j)
                if (P[j].code == 70) insunits = (int)num_or(P[j].val);
        }
        if (in_ent) ents.back().g.push_back(p);
    }
}

bool layer_ok(const std::string& layer, const std::vector<std::string>& filt) {
    if (filt.empty()) return true;
    for (auto& f : filt) if (ieq(f, layer)) return true;
    return false;
}

} // namespace

int sniff_dxf(const std::string& path, const std::string& head) {
    (void)path;
    if (head.rfind("AutoCAD Binary DXF", 0) == 0) return 30;
    auto lines = split_lines(head.substr(0, 4096));
    for (size_t i = 0; i + 1 < lines.size() && i < 40; ++i) {
        if (trim(lines[i]) == "0" && trim(lines[i + 1]) == "SECTION") {
            if (head.find("ENTITIES") != std::string::npos || head.find("HEADER") != std::string::npos ||
                head.find("TABLES") != std::string::npos)
                return 90;
            return 60;
        }
    }
    return 0;
}

std::vector<std::string> dxf_layers(const std::string& text) {
    std::vector<Pair> P;
    std::string err;
    std::vector<std::string> out;
    if (!read_pairs(text, P, err)) return out;
    std::vector<Ent> ents;
    int u = 0;
    collect(P, ents, u);
    std::set<std::string> s;
    for (auto& e : ents) if (e.type != "VERTEX" && e.type != "SEQEND") s.insert(e.layer());
    out.assign(s.begin(), s.end());
    return out;
}

bool import_dxf_outline_text(const std::string& text, Project& prj, const CadImportOptions& opt, CadImportResult& res,
                             std::string& err) {
    res = CadImportResult{};
    res.format = "DXF outline";
    if (text.rfind("AutoCAD Binary DXF", 0) == 0) { err = "DXF: binary DXF not supported (save as ASCII DXF)"; return false; }
    std::vector<Pair> P;
    if (!read_pairs(text, P, err)) return false;
    std::vector<Ent> ents;
    int insunits = 0;
    collect(P, ents, insunits);
    double k = insunits_scale(insunits, res.units);
    if (k <= 0) { k = 1.0; res.units = "mm"; if (insunits == 0) res.warnings.push_back("no $INSUNITS: assuming mm"); else res.warnings.push_back("unsupported $INSUNITS " + std::to_string(insunits) + ": assuming mm"); }
    if (units_override_mm(opt.units) > 0) { k = units_override_mm(opt.units); res.units = opt.units + " (override)"; }
    res.unit_mm = k;
    std::vector<std::string> filt;
    for (auto& f : split_char(opt.layer, ',')) if (!trim(f).empty()) filt.push_back(trim(f));
    OutlineBuilder ob;
    ob.seg_deg = opt.arc_seg_deg;
    int skipped = 0;
    std::set<std::string> unsupported;
    for (size_t i = 0; i < ents.size(); ++i) {
        const Ent& e = ents[i];
        if (e.type == "VERTEX" || e.type == "SEQEND") continue;
        if (!layer_ok(e.layer(), filt)) { ++skipped; continue; }
        const double mx = e.num(230, 1) < 0 ? -1 : 1;     // mirrored OCS
        auto V = [&](double x, double y) { return Vec2{x * k * mx, y * k}; };
        if (e.type == "LINE") ob.add_line(V(e.num(10), e.num(20)), V(e.num(11), e.num(21)));
        else if (e.type == "CIRCLE") ob.add_circle(V(e.num(10), e.num(20)), e.num(40) * k);
        else if (e.type == "ARC") {
            double a0 = e.num(50), a1 = e.num(51);
            if (mx < 0) { double t = 180 - a1; a1 = 180 - a0; a0 = t; }
            double sw = a1 - a0;
            while (sw <= 0) sw += 360;
            ob.add_arc(V(e.num(10), e.num(20)), e.num(40) * k, a0, sw);
        } else if (e.type == "LWPOLYLINE") {
            std::vector<Vec2> pts;
            std::vector<double> bulge;
            const bool closed = ((int)e.num(70) & 1) != 0;
            for (auto& g : e.g) {
                if (g.code == 10) { pts.push_back({num_or(g.val), 0}); bulge.push_back(0); }
                else if (g.code == 20 && !pts.empty()) pts.back().y = num_or(g.val);
                else if (g.code == 42 && !bulge.empty()) bulge.back() = num_or(g.val);
            }
            const size_t n = pts.size();
            for (size_t j = 0; j + (closed ? 0 : 1) < n && n >= 2; ++j) {
                Vec2 a = V(pts[j].x, pts[j].y), b = V(pts[(j + 1) % n].x, pts[(j + 1) % n].y);
                ob.add_bulge(a, b, bulge[j] * mx);
            }
        } else if (e.type == "POLYLINE") {
            const bool closed = ((int)e.num(70) & 1) != 0;
            std::vector<Vec2> pts;
            std::vector<double> bulge;
            size_t j = i + 1;
            for (; j < ents.size() && ents[j].type == "VERTEX"; ++j) {
                pts.push_back({ents[j].num(10), ents[j].num(20)});
                bulge.push_back(ents[j].num(42));
            }
            const size_t n = pts.size();
            for (size_t q = 0; q + (closed ? 0 : 1) < n && n >= 2; ++q) {
                Vec2 a = V(pts[q].x, pts[q].y), b = V(pts[(q + 1) % n].x, pts[(q + 1) % n].y);
                ob.add_bulge(a, b, bulge[q] * mx);
            }
            i = j - 1;
        } else if (e.type == "SPLINE") {
            std::vector<Vec2> fit, ctl;
            for (size_t q = 0; q < e.g.size(); ++q) {
                if (e.g[q].code == 11) fit.push_back({num_or(e.g[q].val), 0});
                else if (e.g[q].code == 21 && !fit.empty()) fit.back().y = num_or(e.g[q].val);
                else if (e.g[q].code == 10) ctl.push_back({num_or(e.g[q].val), 0});
                else if (e.g[q].code == 20 && !ctl.empty()) ctl.back().y = num_or(e.g[q].val);
            }
            auto& src = fit.size() >= 2 ? fit : ctl;
            std::vector<Vec2> pts;
            for (auto& p : src) pts.push_back(V(p.x, p.y));
            ob.add_polyline(pts, ((int)e.num(70) & 1) != 0);
        } else if (e.type != "TEXT" && e.type != "MTEXT" && e.type != "POINT" && e.type != "DIMENSION" && e.type != "HATCH") {
            unsupported.insert(e.type);
        }
    }
    for (auto& u : unsupported) res.warnings.push_back("DXF entity " + u + " ignored");
    if (skipped) res.warnings.push_back(std::to_string(skipped) + " entities on other layers skipped");
    auto frame = ob.finish();
    res.outline_items = ob.items();
    if (frame.empty()) { err = "DXF: no outline geometry" + std::string(filt.empty() ? "" : " on layer " + opt.layer); return false; }
    if (opt.replace) prj.board_frame.clear();
    int open = 0;
    for (auto& f : frame) {
        if (f.size() < 2) continue;
        if (dist(f.front(), f.back()) > 1e-6) ++open;
        prj.board_frame.push_back(f);
        ++res.outline_polylines;
    }
    if (res.outline_polylines) prj.board_outline_src = OutlineSource::Imported;   // R16 provenance
    if (open) res.warnings.push_back(std::to_string(open) + " outline polyline(s) not closed");
    prj.dirty = true;
    return true;
}

bool import_dxf_outline_file(const std::string& path, Project& prj, const CadImportOptions& opt, CadImportResult& res,
                             std::string& err) {
    std::string text;
    if (!read_file(path, text, err)) return false;
    return import_dxf_outline_text(text, prj, opt, res, err);
}

} // namespace pcbp::io
