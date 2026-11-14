// ============================================================================
// fabmaster.cpp — FABmaster FATF import (see fabmaster.h).
// ============================================================================
#include "fabmaster.h"
#include "io_util.h"

#include <map>

namespace pcbp::io {
namespace {

double parse_units(const std::string& spec, std::string& name) {
    // "= 1/1000 INCH" | "MM" | "1/100 MM"
    std::string s = trim(spec);
    if (!s.empty() && s[0] == '=') s = trim(s.substr(1));
    double div = 1;
    auto t = split_ws_quoted(s);
    if (t.empty()) return 0;
    size_t ui = 0;
    if (t.size() >= 2 && t[0].find('/') != std::string::npos) {
        auto f = split_char(t[0], '/');
        double a = num_or(f[0], 1), b = num_or(f.size() > 1 ? f[1] : "1", 1);
        div = b != 0 ? b / a : 1;
        ui = 1;
    }
    double k = unit_scale_word(t[ui]);
    if (k <= 0) return 0;
    name = (div != 1 ? "1/" + fmt_fixed(div, 0) + " " : "") + to_lower(t[ui]);
    return k / div;
}

struct PadDef { double w = 0, h = 0, drill = 0; PadShape shape = PadShape::Round; };

} // namespace

int sniff_fabmaster(const std::string& path, const std::string& head) {
    (void)path;
    if (icontains(head.substr(0, 400), ":FABMASTER")) return 95;
    if (head.find(":PARTS") != std::string::npos && head.find(":PINS") != std::string::npos) return 70;
    return 0;
}

bool import_fabmaster_text(const std::string& text, Project& prj, const CadImportOptions& opt, CadImportResult& res,
                           std::string& err) {
    res = CadImportResult{};
    res.format = "FABmaster FATF";
    double k = kMmPerMil;
    res.units = "1/1000 inch";
    bool have_units = false;
    // tokenise into (section, record fields)
    std::vector<std::pair<std::string, std::vector<std::string>>> recs;
    std::string sec, acc;
    for (auto& l0 : split_lines(text)) {
        std::string l = trim(l0);
        if (l.empty()) continue;
        if (l[0] == ':') {
            std::string body = l.substr(1);
            if (!body.empty() && body.back() == ';') body.pop_back();
            auto eq = body.find('=');
            std::string head = to_upper(trim(eq == std::string::npos ? body : body.substr(0, eq)));
            if (head == "UNITS" || head.rfind("UNITS ", 0) == 0) {
                double s = parse_units(eq == std::string::npos ? body.substr(5) : body.substr(eq + 1), res.units);
                if (s > 0) { k = s; have_units = true; }
                continue;
            }
            auto sp = head.find(' ');
            sec = sp == std::string::npos ? head : head.substr(0, sp);
            if (sec == "FABMASTER") res.source_name = trim(body);
            acc.clear();
            continue;
        }
        for (char c : l) {
            if (c == '{' || c == '}') continue;
            if (c == ';') {
                auto f = split_char(acc, ',');
                for (auto& x : f) x = trim(x);
                if (!(f.size() == 1 && f[0].empty())) recs.push_back({sec, f});
                acc.clear();
            } else acc += c;
        }
        acc += ' ';
    }
    if (!have_units) res.warnings.push_back("no :UNITS; assuming 1/1000 inch");
    if (units_override_mm(opt.units) > 0) { k = units_override_mm(opt.units); res.units = opt.units + " (override)"; }
    res.unit_mm = k;

    CadBuilder b(opt, res);
    std::map<std::string, PadDef> pads;
    struct PartRec { CadComp c; std::vector<CadPin> pins; std::vector<Vec2> abs; };
    std::map<std::string, PartRec> parts;
    std::vector<std::string> order;
    for (auto& [s, f] : recs) {
        if (s == "PADS" && f.size() >= 5) {
            PadDef p;
            const std::string sh = to_upper(f[2]);
            p.shape = (sh == "RECT" || sh == "RECTANGLE") ? PadShape::Rect : sh == "SQUARE" ? PadShape::Square
                    : (sh == "OBLONG" || sh == "OVAL") ? PadShape::Oblong : sh == "OCTAGON" ? PadShape::Octagon : PadShape::Round;
            p.w = num_or(f[3]) * k; p.h = num_or(f[4]) * k;
            if (f.size() >= 6) p.drill = num_or(f[5]) * k;
            pads[f[0]] = p;
        } else if (s == "PARTS" && f.size() >= 8) {
            PartRec pr;
            pr.c.ref = f[1]; pr.c.part = f[2]; pr.c.package = f[3];
            pr.c.pos = {num_or(f[4]) * k, num_or(f[5]) * k};
            pr.c.rot = num_or(f[6]);
            pr.c.bottom = ieq(f[7], "B") || ieq(f[7], "BOTTOM");
            if (f.size() >= 9) pr.c.value = f[8];
            if (!parts.count(f[0])) order.push_back(f[0]);
            parts[f[0]] = pr;
        }
    }
    int orphan = 0;
    for (auto& [s, f] : recs) {
        if (s != "PINS" || f.size() < 5) continue;
        auto it = parts.find(f[0]);
        if (it == parts.end()) { ++orphan; continue; }
        PartRec& pr = it->second;
        CadPin p;
        p.name = f[1];
        p.pos = abs_to_local({num_or(f[2]) * k, num_or(f[3]) * k}, pr.c.pos, pr.c.rot, pr.c.bottom);
        if (auto pd = pads.find(f[4]); pd != pads.end()) { p.w = pd->second.w; p.h = pd->second.h; p.shape = pd->second.shape; p.drill = pd->second.drill; }
        else p.w = p.h = 0.5;
        pr.pins.push_back(p);
    }
    if (orphan) b.warn(std::to_string(orphan) + " pins reference unknown parts");
    if (parts.empty()) { err = "FABmaster: no :PARTS records"; return false; }
    for (auto& id : order) {
        PartRec& pr = parts[id];
        CadPackage P;
        P.name = pr.c.package.empty() ? "PKG_" + pr.c.ref : pr.c.package;
        P.pins = pr.pins;
        sort_pins_natural(P.pins);
        pr.c.package = (P.pins.empty() && b.has_package(P.name)) ? P.name : b.add_package(P);
        b.add_component(pr.c);
    }
    if (opt.import_board) {
        for (auto& [s, f] : recs) {
            if (s != "BOARD_OUTLINE") continue;
            auto N = [&](size_t i) { return i < f.size() ? num_or(f[i]) * k : 0.0; };
            if (ieq(f[0], "LINE") && f.size() >= 5) b.outline.add_line({N(1), N(2)}, {N(3), N(4)});
            else if (ieq(f[0], "ARC") && f.size() >= 7)
                b.outline.add_arc_ends({N(1), N(2)}, {N(3), N(4)}, {N(5), N(6)}, !(f.size() >= 8 && ieq(f[7], "CW")));
        }
    }
    return b.commit(prj, err);
}

bool import_fabmaster_file(const std::string& path, Project& prj, const CadImportOptions& opt, CadImportResult& res,
                           std::string& err) {
    std::string text;
    if (!read_file(path, text, err)) return false;
    return import_fabmaster_text(text, prj, opt, res, err);
}

} // namespace pcbp::io
