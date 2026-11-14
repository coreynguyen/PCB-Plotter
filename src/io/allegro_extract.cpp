// ============================================================================
// allegro_extract.cpp — Allegro extract import (see allegro_extract.h).
// ============================================================================
#include "allegro_extract.h"
#include "io_util.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace pcbp::io {
namespace {

struct Row { std::map<std::string, std::string> v; std::string get(const char* k) const { auto it = v.find(k); return it == v.end() ? std::string() : trim(it->second); } };

struct PinRec { std::string name, stack; Vec2 abs; double drill = 0; };
struct CompRec { CadComp c; std::string sym; bool have = false; std::vector<PinRec> pins; };

double units_from_tokens(const std::vector<std::string>& t, std::string& name) {
    for (auto& s : t) {
        std::string u = to_upper(trim(s));
        if (u == "MILS" || u == "MIL") { name = "mil"; return kMmPerMil; }
        if (u == "MILLIMETERS" || u == "MILLIMETER" || u == "MM") { name = "mm"; return 1.0; }
        if (u == "INCHES" || u == "INCH") { name = "inch"; return kMmPerInch; }
        if (u == "MICRONS" || u == "MICRON") { name = "um"; return 0.001; }
        if (u == "CENTIMETER" || u == "CENTIMETERS") { name = "cm"; return 10.0; }
    }
    return 0;
}

} // namespace

int sniff_allegro_extract(const std::string& path, const std::string& head) {
    (void)path;
    const bool a = head.rfind("A!", 0) == 0 || head.find("\nA!") != std::string::npos;
    if (a && head.find("REFDES") != std::string::npos) return 92;
    if (head.rfind("J!", 0) == 0 && a) return 85;
    if (head.find("REFDES|") != std::string::npos && (head.find("|SYM_X") != std::string::npos || head.find("|PIN_X") != std::string::npos))
        return 85;
    return 0;
}

bool import_allegro_extract_text(const std::string& text, Project& prj, const CadImportOptions& opt, CadImportResult& res,
                                 std::string& err) {
    res = CadImportResult{};
    res.format = "Allegro extract";
    double k = 0;
    std::vector<std::string> cols;
    char delim = '!';
    std::vector<Row> comp_rows, pin_rows, gfx_rows;
    for (auto& l0 : split_lines(text)) {
        std::string l = trim(l0);
        if (l.empty() || l[0] == '#') continue;
        std::vector<std::string> t;
        bool header = false, data = false;
        if (l.size() >= 2 && l[1] == '!' && (l[0] == 'A' || l[0] == 'S' || l[0] == 'J')) {
            t = split_char(l.substr(2), '!');
            if (l[0] == 'J') {
                if (k == 0) k = units_from_tokens(t, res.units);
                if (res.source_name.empty() && !t.empty()) res.source_name = trim(t[0]);
                continue;
            }
            header = l[0] == 'A';
            data = l[0] == 'S';
            delim = '!';
        } else if (l.find('|') != std::string::npos) {
            delim = '|';
            t = split_char(l, '|');
            header = false;
            for (auto& s : t) if (ieq(trim(s), "REFDES") || ieq(trim(s), "CLASS")) header = true;
            data = !header;
        } else {
            if (istarts_with(l, "UNITS")) { auto t2 = split_ws_quoted(l); if (k == 0) k = units_from_tokens(t2, res.units); }
            continue;
        }
        if (!t.empty() && trim(t.back()).empty()) t.pop_back();
        if (header) { cols.clear(); for (auto& s : t) cols.push_back(to_upper(trim(s))); continue; }
        if (!data || cols.empty()) continue;
        Row r;
        for (size_t i = 0; i < cols.size() && i < t.size(); ++i) r.v[cols[i]] = t[i];
        auto has = [&](const char* c) { return std::find(cols.begin(), cols.end(), c) != cols.end(); };
        if (has("REFDES") && has("PIN_X")) pin_rows.push_back(r);
        if (has("REFDES") && has("SYM_X")) comp_rows.push_back(r);
        if (has("CLASS") && has("GRAPHIC_DATA_NAME")) gfx_rows.push_back(r);
    }
    (void)delim;
    if (k == 0) { k = kMmPerMil; res.units = "mil"; res.warnings.push_back("no units in J! line; assuming mils"); }
    if (units_override_mm(opt.units) > 0) { k = units_override_mm(opt.units); res.units = opt.units + " (override)"; }
    res.unit_mm = k;
    if (comp_rows.empty() && pin_rows.empty() && gfx_rows.empty()) { err = "Allegro: no REFDES/SYM_X/PIN_X data rows"; return false; }

    CadBuilder b(opt, res);
    std::map<std::string, CompRec> comps;
    std::vector<std::string> order;
    auto comp_of = [&](const Row& r) -> CompRec& {
        const std::string ref = r.get("REFDES");
        auto it = comps.find(ref);
        if (it == comps.end()) { order.push_back(ref); it = comps.emplace(ref, CompRec{}).first; it->second.c.ref = ref; }
        CompRec& cr = it->second;
        if (!cr.have && !r.get("SYM_X").empty()) {
            cr.have = true;
            cr.c.pos = {num_or(r.get("SYM_X")) * k, num_or(r.get("SYM_Y")) * k};
            cr.c.rot = num_or(r.get("SYM_ROTATE"));
            const std::string m = to_upper(r.get("SYM_MIRROR"));
            cr.c.bottom = m == "YES" || m == "Y" || m == "TRUE" || m == "1";
            cr.sym = r.get("SYM_NAME");
            cr.c.value = r.get("COMP_VALUE");
            cr.c.part = r.get("COMP_PART_NUMBER");
            if (cr.c.part.empty()) cr.c.part = r.get("COMP_DEVICE_TYPE");
        }
        return cr;
    };
    for (auto& r : comp_rows) if (!r.get("REFDES").empty()) comp_of(r);
    for (auto& r : pin_rows) {
        if (r.get("REFDES").empty()) continue;
        CompRec& cr = comp_of(r);
        PinRec p;
        p.name = r.get("PIN_NUMBER");
        if (p.name.empty()) p.name = r.get("PIN_NAME");
        p.abs = {num_or(r.get("PIN_X")) * k, num_or(r.get("PIN_Y")) * k};
        p.stack = r.get("PAD_STACK_NAME");
        p.drill = num_or(r.get("DRILL_SIZE")) * k;
        if (p.drill <= 0) p.drill = num_or(r.get("DRILL_HOLE_SIZE")) * k;
        cr.pins.push_back(p);
    }
    int unguessed = 0;
    for (auto& ref : order) {
        CompRec& cr = comps[ref];
        if (!cr.have) { b.warn(ref + ": pins without SYM_X placement row; skipped"); continue; }
        if (cr.sym.empty()) cr.sym = "SYM_" + ref;
        CadPackage P;
        P.name = cr.sym;
        for (auto& pr : cr.pins) {
            CadPin p;
            p.name = pr.name;
            p.pos = abs_to_local(pr.abs, cr.c.pos, cr.c.rot, cr.c.bottom);
            if (!guess_padstack(pr.stack, k, p)) { p.w = p.h = 0.5; ++unguessed; }
            if (pr.drill > 0) p.drill = pr.drill;
            P.pins.push_back(p);
        }
        sort_pins_natural(P.pins);
        cr.c.package = P.pins.empty() && b.has_package(P.name) ? P.name : b.add_package(P);
        b.add_component(cr.c);
    }
    if (unguessed) b.warn(std::to_string(unguessed) + " pins: pad size not derivable from pad-stack name (0.5 mm used)");

    if (opt.import_board) {
        for (auto& r : gfx_rows) {
            if (!ieq(r.get("CLASS"), "BOARD GEOMETRY")) continue;
            const std::string sub = to_upper(r.get("SUBCLASS"));
            if (sub != "OUTLINE" && sub != "DESIGN_OUTLINE") continue;
            const std::string g = to_upper(r.get("GRAPHIC_DATA_NAME"));
            auto D = [&](int i) { return num_or(r.get(("GRAPHIC_DATA_" + std::to_string(i)).c_str())) * k; };
            if (g == "LINE") b.outline.add_line({D(1), D(2)}, {D(3), D(4)});
            else if (g == "ARC") {
                const std::string dir = to_upper(r.get("GRAPHIC_DATA_9"));
                b.outline.add_arc_ends({D(1), D(2)}, {D(3), D(4)}, {D(5), D(6)}, dir != "CLOCKWISE");
            } else if (g == "RECTANGLE") {
                Vec2 a{D(1), D(2)}, c{D(3), D(4)};
                b.outline.add_polyline({a, {c.x, a.y}, c, {a.x, c.y}}, true);
            } else if (g == "CIRCLE") {
                b.outline.add_circle({D(1), D(2)}, D(3) * 0.5);
            }
        }
    }
    return b.commit(prj, err);
}

bool import_allegro_extract_file(const std::string& path, Project& prj, const CadImportOptions& opt, CadImportResult& res,
                                 std::string& err) {
    std::string text;
    if (!read_file(path, text, err)) return false;
    return import_allegro_extract_text(text, prj, opt, res, err);
}

} // namespace pcbp::io
