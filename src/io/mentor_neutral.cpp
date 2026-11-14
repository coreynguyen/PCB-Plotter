// ============================================================================
// mentor_neutral.cpp — Mentor neutral file import (see mentor_neutral.h).
// ============================================================================
#include "mentor_neutral.h"
#include "io_util.h"

#include <map>

namespace pcbp::io {
namespace {

int side_word(const std::string& s) {      // 0 top, 1 bottom, -1 not a side word
    const std::string u = to_upper(s);
    if (u == "TOP" || u == "T" || u == "COMP" || u == "COMPONENT") return 0;
    if (u == "BOTTOM" || u == "BOT" || u == "B" || u == "SOLDER") return 1;
    return -1;
}

struct PinRec { std::string name, stack; Vec2 abs; };
struct CompRec { CadComp c; std::vector<PinRec> pins; bool have = false; };

} // namespace

int sniff_mentor_neutral(const std::string& path, const std::string& head) {
    (void)path;
    int hits = 0;
    for (auto& l : split_lines(head)) {
        if (istarts_with(l, "B_UNITS")) hits += 3;
        else if (istarts_with(l, "B_ATTR")) hits += 1;
        else if (l.size() > 2 && (l[0] == 'C' || l[0] == 'P') && l[1] == ' ') hits += 1;
    }
    if (hits >= 5) return 90;
    if (hits >= 3 && head.find("B_UNITS") != std::string::npos) return 70;
    return 0;
}

bool import_mentor_neutral_text(const std::string& text, Project& prj, const CadImportOptions& opt, CadImportResult& res,
                                std::string& err) {
    res = CadImportResult{};
    res.format = "Mentor neutral";
    double k = kMmPerInch;
    res.units = "inch";
    bool have_units = false;
    std::map<std::string, CompRec> comps;
    std::vector<std::string> order;
    int bad = 0;
    auto get = [&](const std::string& ref) -> CompRec& {
        auto it = comps.find(ref);
        if (it == comps.end()) { order.push_back(ref); it = comps.emplace(ref, CompRec{}).first; it->second.c.ref = ref; }
        return it->second;
    };
    // pass 1 collects raw records; coordinates are scaled at the end (B_UNITS may follow)
    struct RawC { std::string ref, part, geom; double x, y, rot; bool bottom; };
    struct RawP { std::string ref, pin, stack; double x, y; };
    std::vector<RawC> rc;
    std::vector<RawP> rp;
    for (auto& l0 : split_lines(text)) {
        std::string l = trim(l0);
        if (l.empty() || l[0] == '#' || l[0] == ';') continue;
        auto t = split_ws_quoted(l);
        if (ieq(t[0], "B_UNITS") && t.size() >= 2) {
            double s = unit_scale_word(t[1]);
            if (s > 0) { k = s; res.units = to_lower(t[1]); have_units = true; }
            continue;
        }
        if (ieq(t[0], "B_ATTR")) {
            if (t.size() >= 3 && ieq(t[1], "BOARD_NAME")) res.source_name = t[2];
            continue;
        }
        if (t[0] == "C" && t.size() >= 4) {
            size_t i = 2;
            while (i < t.size() && !parse_num(t[i])) ++i;
            if (i + 1 >= t.size()) { ++bad; continue; }
            RawC c;
            c.ref = t[1];
            std::vector<std::string> names(t.begin() + 2, t.begin() + i);
            if (names.size() >= 2) { c.part = names[0]; c.geom = names[1]; }
            else if (names.size() == 1) c.geom = names[0];
            c.x = num_or(t[i]); c.y = num_or(t[i + 1]);
            c.rot = 0; c.bottom = false;
            std::vector<double> nums;
            for (size_t j = i + 2; j < t.size(); ++j) {
                int sw = side_word(t[j]);
                if (sw >= 0) c.bottom = sw == 1;
                else if (auto v = parse_num(t[j])) nums.push_back(*v);
            }
            if (nums.size() >= 2) { c.bottom = nums[0] == 2; c.rot = nums[1]; }
            else if (nums.size() == 1) c.rot = nums[0];
            rc.push_back(c);
            continue;
        }
        if (t[0] == "P" && t.size() >= 5) {
            RawP p;
            p.ref = t[1]; p.pin = t[2];
            if (!parse_num(t[3]) || !parse_num(t[4])) { ++bad; continue; }
            p.x = num_or(t[3]); p.y = num_or(t[4]);
            for (size_t j = 5; j < t.size(); ++j) {
                if (side_word(t[j]) >= 0 || parse_num(t[j])) continue;
                p.stack = t[j];
                break;
            }
            rp.push_back(p);
        }
    }
    if (!have_units) res.warnings.push_back("no B_UNITS record; assuming inches");
    if (units_override_mm(opt.units) > 0) { k = units_override_mm(opt.units); res.units = opt.units + " (override)"; }
    res.unit_mm = k;
    if (rc.empty()) { err = "Mentor neutral: no C (component) records"; return false; }
    CadBuilder b(opt, res);
    if (bad) b.warn(std::to_string(bad) + " malformed C/P records skipped");
    for (auto& c : rc) {
        CompRec& cr = get(c.ref);
        cr.have = true;
        cr.c.part = c.part;
        cr.c.package = c.geom;
        cr.c.pos = {c.x * k, c.y * k};
        cr.c.rot = c.rot;
        cr.c.bottom = c.bottom;
    }
    for (auto& p : rp) {
        auto it = comps.find(p.ref);
        if (it == comps.end()) continue;
        it->second.pins.push_back({p.pin, p.stack, {p.x * k, p.y * k}});
    }
    int unguessed = 0;
    for (auto& ref : order) {
        CompRec& cr = comps[ref];
        CadPackage P;
        P.name = cr.c.package.empty() ? "GEOM_" + ref : cr.c.package;
        for (auto& pr : cr.pins) {
            CadPin p;
            p.name = pr.name;
            p.pos = abs_to_local(pr.abs, cr.c.pos, cr.c.rot, cr.c.bottom);
            if (!guess_padstack(pr.stack, k, p)) { p.w = p.h = 0.5; ++unguessed; }
            P.pins.push_back(p);
        }
        sort_pins_natural(P.pins);
        cr.c.package = (P.pins.empty() && b.has_package(P.name)) ? P.name : b.add_package(P);
        b.add_component(cr.c);
    }
    if (unguessed) b.warn(std::to_string(unguessed) + " pins: pad size not derivable from pad-stack name (0.5 mm used)");
    return b.commit(prj, err);
}

bool import_mentor_neutral_file(const std::string& path, Project& prj, const CadImportOptions& opt, CadImportResult& res,
                                std::string& err) {
    std::string text;
    if (!read_file(path, text, err)) return false;
    return import_mentor_neutral_text(text, prj, opt, res, err);
}

} // namespace pcbp::io
