// ============================================================================
// pads_ascii.cpp — PADS ASCII import (see pads_ascii.h).
// ============================================================================
#include "pads_ascii.h"
#include "io_util.h"

#include <cctype>
#include <cmath>
#include <map>

namespace pcbp::io {
namespace {

bool is_section(const std::string& l) {
    return l.size() >= 3 && l[0] == '*' && l.find('*', 1) != std::string::npos && std::isalpha((unsigned char)l[1]);
}
std::string section_name(const std::string& l) {
    auto e = l.find('*', 1);
    return to_upper(l.substr(1, e - 1));
}

bool is_piece_kw(const std::string& t) {
    static const char* kw[] = {"OPEN", "CLOSED", "CIRCLE", "COPOPN", "COPCLS", "COPCUT", "COPCCO", "COPCIR",
                               "KPTCLS", "KPTCIR", "KPTOPN", "TAG", "BRDCLS", "BRDCIR", "BRDOPN", "LINE"};
    for (auto* k : kw) if (t == k) return true;
    return false;
}

bool all_num(const std::vector<std::string>& t, size_t from, size_t to) {
    for (size_t i = from; i < to && i < t.size(); ++i) if (!parse_num(t[i])) return false;
    return true;
}

struct Stack { double w = 0, h = 0, rot = 0, drill = 0; PadShape shape = PadShape::Round; bool set = false; };

struct Decal {
    std::string name;
    std::vector<CadPin> pins;
    std::map<int, Stack> stacks;   // 0 = default
};

// parse "PAD" block starting at lines[i]; returns lines consumed
size_t parse_pad(const std::vector<std::vector<std::string>>& L, size_t i, double k, Decal& d) {
    const auto& h = L[i];
    const int pin = h.size() > 1 ? (int)num_or(h[1]) : 0;
    const int n = h.size() > 2 ? (int)num_or(h[2]) : 0;
    Stack st;
    if (h.size() >= 5 && (h[3] == "P" || h[3] == "N")) st.drill = num_or(h[4]) * k;
    Stack best;
    int best_rank = 99;
    for (int j = 1; j <= n && i + j < L.size(); ++j) {
        const auto& t = L[i + j];
        if (t.size() < 3) continue;
        const int layer = (int)num_or(t[0]);
        const double size = num_or(t[1]) * k;
        if (size <= 0) continue;
        const std::string sh = to_upper(t[2]);
        Stack s;
        s.w = s.h = size;
        if (sh == "R" || sh == "A") s.shape = PadShape::Round;
        else if (sh == "S") s.shape = PadShape::Square;
        else if (sh == "OF" || sh == "RF") {
            s.shape = sh == "OF" ? PadShape::Oblong : PadShape::Rect;
            // newer files carry a corner-radius field first: "RF <corner> <ori> <len> <offset>"
            const size_t o = (sh == "RF" && t.size() >= 7) ? 4 : 3;
            s.rot = t.size() > o ? num_or(t[o]) : 0;
            s.w = t.size() > o + 1 ? num_or(t[o + 1]) * k : size;
            s.h = size;
        } else continue;   // thermals / anti-pads
        const int rank = layer == -2 ? 0 : layer == 1 ? 1 : 2;
        if (rank < best_rank) { best = s; best_rank = rank; }
    }
    if (best_rank < 99) { best.drill = st.drill; best.set = true; d.stacks[pin] = best; }
    else if (st.drill > 0) { st.w = st.h = st.drill * 1.6; st.set = true; d.stacks[pin] = st; }
    return size_t(n) + 1;
}

double header_units(const std::string& first, std::string& name) {
    std::string u = to_upper(first);
    if (u.find("-MILS") != std::string::npos) { name = "mil"; return kMmPerMil; }
    if (u.find("-METRIC") != std::string::npos || u.find("-MM") != std::string::npos) { name = "mm"; return 1.0; }
    if (u.find("-INCHES") != std::string::npos || u.find("-INCH") != std::string::npos) { name = "inch"; return kMmPerInch; }
    if (u.find("-BASIC") != std::string::npos) { name = "basic"; return kMmPerInch / 38100.0; }
    name = "mil";
    return kMmPerMil;
}

} // namespace

int sniff_pads_ascii(const std::string& path, const std::string& head) {
    (void)path;
    if (head.rfind("!PADS-", 0) == 0 || head.find("!PADS-POWERPCB") != std::string::npos) return 95;
    if (head.find("*PARTDECAL*") != std::string::npos || (head.find("*PART*") != std::string::npos && head.find("*PCB*") != std::string::npos))
        return 75;
    return 0;
}

bool import_pads_ascii_text(const std::string& text, Project& prj, const CadImportOptions& opt, CadImportResult& res,
                            std::string& err) {
    res = CadImportResult{};
    res.format = "PADS ASCII";
    auto raw = split_lines(text);
    if (raw.empty()) { err = "PADS: empty file"; return false; }
    double k = header_units(raw[0], res.units);
    if (raw[0].rfind("!PADS", 0) != 0) res.warnings.push_back("no !PADS- header; assuming mils");
    if (units_override_mm(opt.units) > 0) { k = units_override_mm(opt.units); res.units = opt.units + " (override)"; }
    res.unit_mm = k;
    res.source_name = trim(raw[0]);

    // split into sections
    std::map<std::string, std::vector<std::vector<std::string>>> sec;
    std::string cur;
    for (size_t i = 1; i < raw.size(); ++i) {
        std::string l = trim(raw[i]);
        if (l.empty()) continue;
        if (is_section(l)) {
            std::string s = section_name(l);
            if (s == "REMARK") continue;
            cur = s;
            continue;
        }
        if (!cur.empty()) sec[cur].push_back(split_ws_quoted(l));
    }
    CadBuilder b(opt, res);

    // ---- decals ----
    std::map<std::string, Decal> decals;
    {
        auto& L = sec["PARTDECAL"];
        Decal* d = nullptr;
        for (size_t i = 0; i < L.size();) {
            const auto& t = L[i];
            if (t.empty()) { ++i; continue; }
            if (is_piece_kw(t[0]) && t.size() >= 2) { i += 1 + size_t(std::max(0.0, num_or(t[1]))); continue; }
            if (t.size() >= 9 && t[1].size() <= 3 && std::isalpha((unsigned char)t[1][0]) && all_num(t, 2, 9)) {
                Decal nd;
                nd.name = t[0];
                d = &(decals[t[0]] = nd);
                ++i;
                continue;
            }
            if (t[0] == "PAD" && d) { i += parse_pad(L, i, k, *d); continue; }
            if (t[0].size() > 1 && t[0][0] == 'T' && parse_num(t[0].substr(1)) && t.size() >= 2 && d) {
                CadPin p;
                p.pos = {num_or(t[0].substr(1)) * k, num_or(t[1]) * k};
                p.name = t.size() >= 5 ? t[4] : std::to_string(d->pins.size() + 1);
                d->pins.push_back(p);
                ++i;
                continue;
            }
            ++i;
        }
    }
    // ---- part types ----
    std::map<std::string, std::string> ptype;
    for (auto& t : sec["PARTTYPE"]) {
        if (t.size() < 2) continue;
        std::string first = split_char(t[1], ':')[0];
        if (decals.count(first) && !ptype.count(t[0])) ptype[t[0]] = first;
    }
    // ---- packages ----
    std::map<std::string, std::string> fin;
    auto package_for = [&](const std::string& dn) -> std::string {
        if (auto f = fin.find(dn); f != fin.end()) return f->second;
        auto it = decals.find(dn);
        if (it == decals.end()) return dn;
        CadPackage P;
        P.name = dn;
        int idx = 0;
        for (auto p : it->second.pins) {
            ++idx;
            Stack s;
            if (auto si = it->second.stacks.find(idx); si != it->second.stacks.end()) s = si->second;
            else if (auto s0 = it->second.stacks.find(0); s0 != it->second.stacks.end()) s = s0->second;
            if (s.set) { p.w = s.w; p.h = s.h; p.rot = s.rot; p.shape = s.shape; p.drill = s.drill; }
            else { p.w = p.h = 0.5; }
            P.pins.push_back(p);
        }
        return fin[dn] = b.add_package(P);
    };
    // ---- parts ----
    {
        auto& L = sec["PART"];
        for (size_t i = 0; i < L.size();) {
            const auto& t = L[i];
            if (t.size() < 7 || !all_num(t, 2, 5)) { ++i; continue; }
            CadComp c;
            c.ref = t[0];
            std::string type = t[1], decal;
            if (auto at = type.find('@'); at != std::string::npos) { decal = type.substr(at + 1); type = type.substr(0, at); }
            else if (auto pt = ptype.find(type); pt != ptype.end()) decal = pt->second;
            else decal = type;
            c.part = type;
            c.pos = {num_or(t[2]) * k, num_or(t[3]) * k};
            c.rot = num_or(t[4]);
            c.bottom = t[6] == "M";
            if (!decals.count(decal)) b.warn(c.ref + ": decal " + decal + " not found");
            c.package = package_for(decal);
            b.add_component(c);
            const int labels = (int)num_or(t.back(), 0);
            i += 1 + size_t(std::max(0, std::min(labels, 64))) * 2;
        }
    }
    // ---- board outline ----
    if (opt.import_board) {
        auto& L = sec["LINES"];
        for (size_t i = 0; i < L.size();) {
            const auto& t = L[i];
            if (t.size() >= 5 && !parse_num(t[0]) && !is_piece_kw(t[0]) && !parse_num(t[1]) && all_num(t, 2, 5)) {
                const bool board = ieq(t[1], "BOARD");
                const Vec2 org{num_or(t[2]) * k, num_or(t[3]) * k};
                const int pieces = (int)num_or(t[4]);
                ++i;
                for (int p = 0; p < pieces && i < L.size(); ++p) {
                    const auto& ph = L[i];
                    const int n = ph.size() >= 2 ? (int)num_or(ph[1]) : 0;
                    const std::string kind = ph.empty() ? "" : to_upper(ph[0]);
                    std::vector<std::vector<std::string>> pts(L.begin() + i + 1, L.begin() + std::min(L.size(), i + 1 + size_t(std::max(0, n))));
                    i += 1 + size_t(std::max(0, n));
                    if (!board) continue;
                    auto P = [&](const std::vector<std::string>& v) { return Vec2{num_or(v[0]) * k, num_or(v[1]) * k} + org; };
                    if (kind == "CIRCLE" && pts.size() >= 2) {
                        Vec2 a = P(pts[0]), c2 = P(pts[1]);
                        b.outline.add_circle((a + c2) * 0.5, dist(a, c2) * 0.5);
                        continue;
                    }
                    for (size_t j = 0; j + 1 < pts.size(); ++j) {
                        if (pts[j].size() < 2) continue;
                        Vec2 a = P(pts[j]), e = P(pts[j + 1]);
                        if (pts[j].size() >= 8) {
                            Vec2 c1{num_or(pts[j][4]) * k, num_or(pts[j][5]) * k}, c2{num_or(pts[j][6]) * k, num_or(pts[j][7]) * k};
                            b.outline.add_arc_ends(a, e, (c1 + c2) * 0.5 + org, num_or(pts[j][3]) > 0);
                        } else b.outline.add_line(a, e);
                    }
                    if ((kind == "CLOSED" || kind == "BRDCLS") && pts.size() > 2) b.outline.add_line(P(pts.back()), P(pts.front()));
                }
                continue;
            }
            ++i;
        }
    }
    return b.commit(prj, err);
}

bool import_pads_ascii_file(const std::string& path, Project& prj, const CadImportOptions& opt, CadImportResult& res,
                            std::string& err) {
    std::string text;
    if (!read_file(path, text, err)) return false;
    return import_pads_ascii_text(text, prj, opt, res, err);
}

} // namespace pcbp::io
