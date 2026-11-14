// ============================================================================
// centroid_import.cpp — ASCII centroid / pick&place import and Reference
// Fusion. See centroid_import.h.
// ============================================================================
#include "centroid_import.h"
#include "io_util.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <sstream>
#include <unordered_map>
#include <cstdlib>

namespace pcbp::io {

static LenUnit unit_from_suffix(const std::string& s) {
    if (s.empty()) return LenUnit::Auto;
    if (s == "mm") return LenUnit::MM;
    if (s == "cm") return LenUnit::CM;
    if (s == "mil" || s == "mils" || s == "thou" || s == "th") return LenUnit::Mil;
    if (s == "in" || s == "inch" || s == "inches" || s == "\"") return LenUnit::Inch;
    return LenUnit::Auto;
}

// Find a unit word inside free text such as "Center-X(mm)" or "## Unit = mm, Angle = deg."
static LenUnit unit_in_text(const std::string& text) {
    std::string t = to_lower(text);
    // tokenise on non-alnum
    std::vector<std::string> w;
    std::string cur;
    for (char c : t) {
        if (std::isalnum((unsigned char)c)) cur += c;
        else { if (!cur.empty()) w.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) w.push_back(cur);
    for (auto& x : w) {
        if (x == "mm" || x == "millimeter" || x == "millimeters" || x == "millimetres" || x == "metric") return LenUnit::MM;
        if (x == "mil" || x == "mils" || x == "thou") return LenUnit::Mil;
        if (x == "inch" || x == "inches" || x == "in" || x == "imperial") return LenUnit::Inch;
        if (x == "cm") return LenUnit::CM;
        // glued forms: "xmm", "centerxmm", "midxmil"
    }
    for (auto& x : w) {
        auto ends = [&](const char* s) { size_t n = std::char_traits<char>::length(s); return x.size() > n && x.compare(x.size() - n, n, s) == 0; };
        if ((x[0] == 'x' || x[0] == 'y' || x.find("center") == 0 || x.find("mid") == 0 || x.find("pos") == 0)) {
            if (ends("mm")) return LenUnit::MM;
            if (ends("mil") || ends("mils")) return LenUnit::Mil;
            if (ends("inch")) return LenUnit::Inch;
        }
    }
    return LenUnit::Auto;
}

bool parse_side(const std::string& cell, const CentroidOptions& o, Side& out) {
    std::string k = to_upper(trim(cell));
    if (k.empty()) return false;
    for (auto& f : o.top_filters) if (ieq(k, f)) { out = Side::Top; return true; }
    for (auto& f : o.bottom_filters) if (ieq(k, f)) { out = Side::Bottom; return true; }
    // prefix forms: "TOP SIDE", "Bottom Layer", "BOT"
    if (k.rfind("TOP", 0) == 0) { out = Side::Top; return true; }
    if (k.rfind("BOT", 0) == 0) { out = Side::Bottom; return true; }
    return false;
}

// ---------------------------------------------------------------------------
// Auto mapping
// ---------------------------------------------------------------------------
namespace {

std::string strip_unit_suffix(const std::string& k) {
    static const char* sufs[] = {"mils", "mil", "mm", "inches", "inch", "in", "thou", "cm", "deg", "degrees"};
    for (const char* s : sufs) {
        size_t n = std::char_traits<char>::length(s);
        if (k.size() > n && k.compare(k.size() - n, n, s) == 0) return k.substr(0, k.size() - n);
    }
    return k;
}

// Ordered synonym lists (normalized). Earlier = preferred.
const std::vector<std::string> kRef = {"designator", "refdes", "ref", "reference", "referencedesignator",
    "refdesignator", "designators", "component", "componentname", "compname", "partname", "name", "des", "location", "part"};
const std::vector<std::string> kX = {"midx", "centerx", "centrex", "centroidx", "bodycenterx", "posx", "x", "xloc",
    "locationx", "locx", "xcoord", "xcoordinate", "xposition", "positionx", "placex", "pickx", "refx", "padx", "originx"};
const std::vector<std::string> kY = {"midy", "centery", "centrey", "centroidy", "bodycentery", "posy", "y", "yloc",
    "locationy", "locy", "ycoord", "ycoordinate", "yposition", "positiony", "placey", "picky", "refy", "pady", "originy"};
const std::vector<std::string> kRot = {"rotation", "rot", "angle", "orientation", "theta", "rotate", "placementangle", "ang", "r"};
const std::vector<std::string> kSide = {"layer", "side", "pcbside", "placementside", "mountside", "boardside", "tb", "mirror", "surface"};
const std::vector<std::string> kPart = {"partnumber", "partno", "pn", "partnum", "internalpn", "mpn", "manufacturerpartnumber", "device", "partid", "stockcode", "part"};
const std::vector<std::string> kPkg = {"footprint", "package", "pattern", "pcbfootprint", "shape", "case", "pkg", "decal",
    "geometry", "cell", "packagename", "footprintname", "patternname"};
const std::vector<std::string> kVal = {"comment", "value", "val", "description", "desc", "partdescription", "partvalue"};

int syn_rank(const std::vector<std::string>& syn, const std::string& key) {
    for (size_t i = 0; i < syn.size(); ++i) if (syn[i] == key) return (int)i;
    return -1;
}

} // namespace

CentroidMapping auto_map_centroid(const TextTable& t, LenUnit* units_hint, std::string* reason) {
    CentroidMapping m;
    const int nc = (int)t.ncols();
    std::vector<char> taken(nc, 0);
    LenUnit uh = LenUnit::Auto;
    std::string why;

    struct Field { int* slot; const std::vector<std::string>* syn; };
    Field fields[] = {{&m.ref, &kRef}, {&m.x, &kX}, {&m.y, &kY}, {&m.rot, &kRot}, {&m.side, &kSide},
                      {&m.part, &kPart}, {&m.package, &kPkg}, {&m.value, &kVal}};
    if (t.has_header) {
        // pass 1: exact (after unit-suffix strip) synonym, best rank wins
        for (auto& f : fields) {
            int best = -1, br = 1 << 30;
            for (int c = 0; c < nc; ++c) {
                if (taken[c]) continue;
                std::string k = norm_key(t.columns[c]);
                if (k.empty()) continue;
                int r = syn_rank(*f.syn, k);
                if (r < 0) r = syn_rank(*f.syn, strip_unit_suffix(k));
                if (r >= 0 && r < br) { br = r; best = c; }
            }
            if (best >= 0) { *f.slot = best; taken[best] = 1; }
        }
        // pass 2: prefix match ("Center-X(mm)" -> "centerxmm" starts with "centerx")
        for (auto& f : fields) {
            if (*f.slot >= 0) continue;
            int best = -1, br = 1 << 30;
            for (int c = 0; c < nc; ++c) {
                if (taken[c]) continue;
                std::string k = norm_key(t.columns[c]);
                for (size_t i = 0; i < f.syn->size(); ++i) {
                    const std::string& s = (*f.syn)[i];
                    if (s.size() >= 3 && k.rfind(s, 0) == 0 && (int)i < br) { br = (int)i; best = c; }
                }
            }
            if (best >= 0) { *f.slot = best; taken[best] = 1; }
        }
        for (int c : {m.x, m.y}) {
            if (c >= 0 && uh == LenUnit::Auto) {
                uh = unit_in_text(t.columns[c]);
                if (uh != LenUnit::Auto) why = "header \"" + t.columns[c] + "\"";
            }
        }
    }

    // Content-based fallback for anything still unmapped.
    auto frac = [&](int c, auto pred) {
        int n = 0, ok = 0;
        for (size_t r = 0; r < t.rows.size() && r < 200; ++r) {
            const std::string& v = t.rows[r][c];
            if (v.empty()) continue;
            ++n;
            if (pred(v)) ++ok;
        }
        return n ? (double)ok / n : 0.0;
    };
    if (m.ref < 0) {
        for (int c = 0; c < nc; ++c)
            if (!taken[c] && frac(c, [](const std::string& v) { return looks_like_refdes(v); }) >= 0.8) { m.ref = c; taken[c] = 1; break; }
    }
    std::vector<int> numeric;
    for (int c = 0; c < nc; ++c)
        if (!taken[c] && frac(c, [](const std::string& v) { return looks_numeric(v); }) >= 0.9) numeric.push_back(c);
    size_t ni = 0;
    if (m.x < 0 && ni < numeric.size()) { m.x = numeric[ni++]; taken[m.x] = 1; }
    else if (m.x >= 0) { auto it = std::find(numeric.begin(), numeric.end(), m.x); if (it != numeric.end()) numeric.erase(it); }
    if (m.y < 0 && ni < numeric.size()) { m.y = numeric[ni++]; taken[m.y] = 1; }
    if (m.rot < 0 && ni < numeric.size()) {
        int c = numeric[ni];
        if (frac(c, [](const std::string& v) { auto d = parse_num_suffix(v, nullptr); return d && std::fabs(*d) <= 360.0; }) >= 0.99) {
            m.rot = c; taken[c] = 1; ++ni;
        }
    }
    if (m.side < 0) {
        CentroidOptions o;
        for (int c = 0; c < nc; ++c) {
            if (taken[c]) continue;
            if (frac(c, [&](const std::string& v) { Side s; return !looks_numeric(v) && parse_side(v, o, s); }) >= 0.9) { m.side = c; taken[c] = 1; break; }
        }
    }
    // remaining text columns (no header): value then package
    if (!t.has_header) {
        for (int c = 0; c < nc; ++c) {
            if (taken[c]) continue;
            if (frac(c, [](const std::string& v) { return !v.empty(); }) < 0.5) continue;
            if (m.value < 0) { m.value = c; taken[c] = 1; }
            else if (m.package < 0) { m.package = c; taken[c] = 1; }
        }
    }
    if (units_hint) *units_hint = uh;
    if (reason) *reason = why;
    return m;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
bool parse_centroid_text(const std::string& text, const CentroidOptions& opt,
                         CentroidReport& rep, std::string& err) {
    rep = CentroidReport{};
    if (!read_text_table(text, opt.table, rep.table, err)) return false;
    const TextTable& t = rep.table;

    CentroidMapping m = opt.map;
    LenUnit hint = LenUnit::Auto;
    std::string why;
    if (opt.auto_map) {
        CentroidMapping a = auto_map_centroid(t, &hint, &why);
        if (m.ref < 0) m.ref = a.ref;
        if (m.x < 0) m.x = a.x;
        if (m.y < 0) m.y = a.y;
        if (m.rot < 0) m.rot = a.rot;
        if (m.side < 0) m.side = a.side;
        if (m.part < 0) m.part = a.part;
        if (m.package < 0) m.package = a.package;
        if (m.value < 0) m.value = a.value;
    }
    rep.map = m;
    if (!m.valid()) {
        err = "column mapping incomplete (need Reference, X, Y): " + t.describe();
        return false;
    }
    const int nc = (int)t.ncols();
    auto cell = [&](const std::vector<std::string>& row, int c) -> std::string {
        return (c >= 0 && c < nc) ? row[c] : std::string();
    };

    // Units: explicit > header > comment/preamble > cell suffix > magnitude
    LenUnit u = opt.units;
    if (u == LenUnit::Auto && hint != LenUnit::Auto) { u = hint; rep.units_reason = why; }
    if (u == LenUnit::Auto) {
        auto scan = [&](const std::vector<std::string>& lines) {
            for (auto& l : lines) {
                if (!icontains(l, "unit")) continue;
                LenUnit x = unit_in_text(l);
                if (x != LenUnit::Auto) { u = x; rep.units_reason = "text \"" + trim(l) + "\""; return; }
            }
        };
        scan(t.comments);
        if (u == LenUnit::Auto) scan(t.preamble);
    }
    double maxabs = 0;
    int nvals = 0, nint = 0, decsum = 0;
    if (u == LenUnit::Auto) {
        for (auto& row : t.rows) {
            for (int c : {m.x, m.y}) {
                std::string suf;
                std::string v = cell(row, c);
                auto d = parse_num_suffix(v, &suf);
                if (!d) continue;
                if (!suf.empty() && u == LenUnit::Auto) {
                    LenUnit x = unit_from_suffix(suf);
                    if (x != LenUnit::Auto) { u = x; rep.units_reason = "value suffix \"" + suf + "\""; }
                }
                maxabs = std::max(maxabs, std::fabs(*d));
                ++nvals;
                size_t dot = v.find_first_of(".,");
                if (dot == std::string::npos) ++nint;
                else {
                    int dd = 0;
                    for (size_t k = dot + 1; k < v.size() && std::isdigit((unsigned char)v[k]); ++k) ++dd;
                    decsum += dd;
                }
            }
        }
    }
    if (u == LenUnit::Auto) {
        const int ndec = nvals - nint;
        const double avgdec = ndec ? (double)decsum / ndec : 0;
        std::ostringstream s;
        if (nvals && nint == nvals && maxabs >= 50) { u = LenUnit::Mil; s << "all integers, max " << maxabs; }
        else if (maxabs < 12 || (maxabs < 30 && avgdec >= 3.5)) { u = LenUnit::Inch; s << "max " << maxabs << ", " << avgdec << " avg decimals"; }
        else { u = LenUnit::MM; s << "max " << maxabs; }
        rep.units_reason = "magnitude (" + s.str() + ")";
    }
    rep.units = u;
    const double k = len_unit_mm(u) * (opt.factor == 0 ? 1.0 : opt.factor);

    for (size_t r = 0; r < t.rows.size(); ++r) {
        const auto& row = t.rows[r];
        CentroidRow cr;
        cr.line = t.row_line[r];
        cr.ref = cell(row, m.ref);
        ++rep.rows;
        std::string sx, sy;
        auto x = parse_num_suffix(cell(row, m.x), &sx);
        auto y = parse_num_suffix(cell(row, m.y), &sy);
        if (cr.ref.empty() || !x || !y) {
            ++rep.skipped;
            if (rep.messages.size() < 50)
                rep.messages.push_back("line " + std::to_string(cr.line) + ": skipped (missing ref or coordinate)");
            continue;
        }
        // per-cell unit suffix overrides file units
        double kx = k, ky = k;
        if (LenUnit ux = unit_from_suffix(sx); ux != LenUnit::Auto) kx = len_unit_mm(ux) * (opt.factor == 0 ? 1.0 : opt.factor);
        if (LenUnit uy = unit_from_suffix(sy); uy != LenUnit::Auto) ky = len_unit_mm(uy) * (opt.factor == 0 ? 1.0 : opt.factor);
        cr.pos = {*x * kx, *y * ky};
        if (m.rot >= 0) {
            if (auto a = parse_num_suffix(cell(row, m.rot), nullptr)) { cr.rot = *a; cr.has_rot = true; }
        }
        cr.side = opt.default_side;
        if (m.side >= 0) {
            Side s;
            if (parse_side(cell(row, m.side), opt, s)) { cr.side = s; cr.has_side = true; }
        }
        cr.part = cell(row, m.part);
        cr.package = cell(row, m.package);
        cr.value = cell(row, m.value);

        if (cr.side == Side::Bottom) {
            if (opt.bottom_mirror_x) cr.pos.x = -cr.pos.x;
            if (opt.bottom_negate_rot) cr.rot = -cr.rot;
        }
        if (opt.mirror_x) { cr.pos.x = -cr.pos.x; cr.rot = 180.0 - cr.rot; }
        if (opt.mirror_y) { cr.pos.y = -cr.pos.y; cr.rot = -cr.rot; }
        cr.rot = norm_deg(cr.rot + opt.rot_offset);

        if ((opt.side_select == SideSelect::Top && cr.side != Side::Top) ||
            (opt.side_select == SideSelect::Bottom && cr.side != Side::Bottom)) {
            ++rep.filtered;
            continue;
        }
        rep.rows_parsed.push_back(cr);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Fusion
// ---------------------------------------------------------------------------
namespace {

struct Grid {
    double cell;
    std::unordered_map<long long, std::vector<int>> m;   // -> point index
    std::vector<Vec2> pts;
    std::vector<int> owner;                              // point -> comp index
    explicit Grid(double c) : cell(c > 1e-6 ? c : 1e-6) {}
    long long key(long long ix, long long iy) const { return (ix << 32) ^ (iy & 0xffffffffLL); }
    void add(Vec2 p, int own) {
        long long ix = (long long)std::floor(p.x / cell), iy = (long long)std::floor(p.y / cell);
        m[key(ix, iy)].push_back((int)pts.size());
        pts.push_back(p); owner.push_back(own);
    }
    template <class F> void near(Vec2 p, double r, F&& f) const {
        long long ix0 = (long long)std::floor((p.x - r) / cell), ix1 = (long long)std::floor((p.x + r) / cell);
        long long iy0 = (long long)std::floor((p.y - r) / cell), iy1 = (long long)std::floor((p.y + r) / cell);
        for (long long ix = ix0; ix <= ix1; ++ix)
            for (long long iy = iy0; iy <= iy1; ++iy) {
                auto it = m.find(key(ix, iy));
                if (it == m.end()) continue;
                for (int i : it->second) {
                    double d = dist(pts[i], p);
                    if (d <= r) f(owner[i], d);
                }
            }
    }
};

} // namespace

static void do_fusion(Project& prj, const CentroidOptions& opt, CentroidReport& rep) {
    auto& rows = rep.rows_parsed;
    const double tol = opt.fusion_tol_mm > 0 ? opt.fusion_tol_mm : 0.5;

    // Points per component (centroid, plus pad positions for Pin method).
    Grid g(tol);
    for (size_t j = 0; j < prj.comps.size(); ++j) {
        const Component& c = prj.comps[j];
        g.add(c.pos, (int)j);
        if (opt.fusion_method == FusionMethod::Pin) {
            if (const Package* p = prj.package_of(c)) {
                Xform x = prj.comp_xform(c, 0);
                for (auto& pad : p->pads) g.add(x.apply(pad.pos), (int)j);
            }
        }
    }
    Grid cg(tol);                                          // centroids only, for alignment
    for (size_t j = 0; j < prj.comps.size(); ++j) cg.add(prj.comps[j].pos, (int)j);

    // ---- origin delta ----
    Vec2 delta{};
    bool have = false;
    if (!opt.key_ref.empty()) {
        const Component* c = prj.find_ref(opt.key_ref);
        for (auto& r : rows)
            if (c && ieq(r.ref, opt.key_ref)) { delta = c->pos - r.pos; have = true; rep.fusion_delta_votes = 1; break; }
        if (!have) rep.messages.push_back("key component " + opt.key_ref + " not found in both project and file");
    }
    if (!have) {
        std::vector<Vec2> ds;
        for (auto& r : rows)
            if (const Component* c = prj.find_ref(r.ref)) ds.push_back(c->pos - r.pos);
        int best = 0;
        for (auto& d0 : ds) {
            int n = 0; Vec2 sum{};
            for (auto& d1 : ds) if (dist(d0, d1) <= tol) { ++n; sum += d1; }
            if (n > best) { best = n; delta = sum * (1.0 / n); }
        }
        if (best > 0 && best * 2 >= (int)std::min<size_t>(ds.size(), 3)) {
            have = true; rep.fusion_delta_votes = best;
            rep.messages.push_back("delta from " + std::to_string(best) + " common references");
        }
    }
    if (!have && opt.auto_align && !rows.empty() && !prj.comps.empty()) {
        // Try row_i -> comp_j for a sample; score by number of rows landing on a centroid.
        const size_t ns = std::min<size_t>(rows.size(), 24);
        const size_t nt = std::min<size_t>(rows.size(), 400);
        const size_t step_s = std::max<size_t>(1, rows.size() / ns);
        const size_t step_t = std::max<size_t>(1, rows.size() / nt);
        int best = 0;
        for (size_t i = 0; i < rows.size(); i += step_s) {
            for (auto& c : prj.comps) {
                Vec2 d = c.pos - rows[i].pos;
                int n = 0;
                for (size_t k = 0; k < rows.size(); k += step_t) {
                    bool hit = false;
                    cg.near(rows[k].pos + d, tol, [&](int, double) { hit = true; });
                    if (hit) ++n;
                }
                if (n > best) { best = n; delta = d; }
            }
        }
        if (best > 0) {
            // refine: mean residual of all hits
            Vec2 sum{}; int n = 0;
            for (auto& r : rows) {
                double bd = 1e300; Vec2 bp{};
                cg.near(r.pos + delta, tol, [&](int j, double dd) { if (dd < bd) { bd = dd; bp = prj.comps[j].pos; } });
                if (bd < 1e300) { sum += bp - (r.pos + delta); ++n; }
            }
            if (n) delta += sum * (1.0 / n);
            have = true; rep.fusion_delta_votes = best;
            rep.messages.push_back("delta from auto-alignment (" + std::to_string(best) + " sampled hits)");
        }
    }
    if (!have) rep.messages.push_back("no origin delta found; assuming file origin == project origin");
    rep.fusion_delta = delta;

    if (opt.clear_refs) for (auto& c : prj.comps) c.ref.clear();

    // ---- matching: global greedy by distance ----
    struct Cand { int r, c; double d; };
    std::vector<Cand> cands;
    for (size_t i = 0; i < rows.size(); ++i) {
        std::map<int, double> bestd;
        g.near(rows[i].pos + delta, tol, [&](int j, double d) {
            auto it = bestd.find(j);
            if (it == bestd.end() || d < it->second) bestd[j] = d;
        });
        for (auto& [j, d] : bestd) cands.push_back({(int)i, j, d});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
    std::vector<char> rused(rows.size(), 0), cused(prj.comps.size(), 0);
    for (auto& k : cands) {
        if (rused[k.r] || cused[k.c]) continue;
        rused[k.r] = cused[k.c] = 1;
        const CentroidRow& r = rows[k.r];
        // the ref may currently sit on another component: release it there
        for (size_t j = 0; j < prj.comps.size(); ++j)
            if ((int)j != k.c && ieq(prj.comps[j].ref, r.ref)) {
                prj.comps[j].ref.clear();
                rep.messages.push_back("ref " + r.ref + " moved from another component");
            }
        Component& c = prj.comps[k.c];
        c.ref = r.ref;
        if (opt.set_part && !r.part.empty()) c.number = r.part;
        if (opt.set_package && !r.package.empty()) c.package = r.package;
        if (opt.fusion_update_rot && r.has_rot) c.rot = r.rot;
        ++rep.fused;
        rep.fusion_max_dev = std::max(rep.fusion_max_dev, k.d);
    }
    for (size_t i = 0; i < rows.size(); ++i) if (!rused[i]) rep.unmatched_refs.push_back(rows[i].ref);
    for (size_t j = 0; j < prj.comps.size(); ++j) if (!cused[j]) ++rep.unmatched_comps;
    rep.imported = rep.fused;
    rep.updated = rep.fused;
    prj.dirty = true;
}

// ---------------------------------------------------------------------------
bool import_centroid_text(const std::string& text, Project& prj, const CentroidOptions& opt,
                          CentroidReport& rep, std::string& err) {
    if (!parse_centroid_text(text, opt, rep, err)) return false;
    return apply_centroid_rows(prj, opt, rep);
}

bool apply_centroid_rows(Project& prj, const CentroidOptions& opt, CentroidReport& rep) {
    if (opt.mode == CentroidMode::Fusion) {
        do_fusion(prj, opt, rep);
        return true;
    }
    if (opt.mode == CentroidMode::Replace) prj.comps.clear();
    // R17a (REVIEW_V16 §14): panelised files repeat references (U1 of circuit 1, U1 of circuit 2 ...).
    // Merge pairs the n-th row of a reference with the n-th existing placement of that reference
    // (file order), so repeated designators never collapse onto one placement.
    auto key = [](const std::string& s) { std::string u = s; for (auto& ch : u) ch = (char)std::toupper((unsigned char)ch); return u; };
    std::map<std::string, std::vector<size_t>> existing;
    if (opt.mode == CentroidMode::Merge)
        for (size_t i = 0; i < prj.comps.size(); ++i) if (!prj.comps[i].ref.empty()) existing[key(prj.comps[i].ref)].push_back(i);
    std::map<std::string, size_t> seen;
    for (auto& r : rep.rows_parsed) {
        Component* c = nullptr;
        if (opt.mode == CentroidMode::Merge) {
            const size_t k = seen[key(r.ref)]++;
            auto it = existing.find(key(r.ref));
            if (it != existing.end() && k < it->second.size()) c = &prj.comps[it->second[k]];
        }
        if (c) {
            c->pos = r.pos;
            if (r.has_rot) c->rot = r.rot;
            if (r.has_side) c->side = r.side;
            c->cad_pos = c->pos; c->rot_imported = c->rot;
            if (opt.set_part && !r.part.empty()) c->number = r.part;
            if (opt.set_package && !r.package.empty()) c->package = r.package;
            ++rep.updated;
        } else {
            Component n;
            n.ref = r.ref;
            n.pos = r.pos;
            n.rot = r.rot;
            n.side = r.side;
            n.number = r.part;
            n.package = r.package;
            n.dnp = r.dnp;
            n.cad_pos = n.pos; n.rot_imported = n.rot;
            if (istarts_with(r.ref, "FID")) {
                n.feature = Feature::Fiducial;
                std::string d;
                for (char ch : r.ref) if (std::isdigit((unsigned char)ch)) d += ch;
                if (!d.empty()) n.fid_number = std::atoi(d.c_str());
            }
            prj.add_component(n);
            ++rep.added;
        }
        ++rep.imported;
    }
    prj.dirty = true;
    return true;
}

bool import_centroid_file(const std::string& path, Project& prj, const CentroidOptions& opt,
                          CentroidReport& rep, std::string& err) {
    std::string text;
    if (!read_file(path, text, err)) return false;
    return import_centroid_text(text, prj, opt, rep, err);
}

std::string CentroidReport::describe() const {
    std::ostringstream s;
    s << table.describe() << "\n";
    auto nm = [&](int c) { return c < 0 ? std::string("-") : table.col_name(c); };
    s << "mapping: ref=" << nm(map.ref) << " x=" << nm(map.x) << " y=" << nm(map.y)
      << " rot=" << nm(map.rot) << " side=" << nm(map.side) << " part=" << nm(map.part)
      << " package=" << nm(map.package) << " value=" << nm(map.value) << "\n";
    s << "units: " << len_unit_name(units) << " (" << units_reason << ")\n";
    s << "rows " << rows << ", imported " << imported << " (added " << added << ", updated " << updated
      << "), skipped " << skipped << ", side-filtered " << filtered;
    if (fused || !unmatched_refs.empty())
        s << "\nfusion: matched " << fused << ", unmatched file refs " << unmatched_refs.size()
          << ", unmatched comps " << unmatched_comps << ", delta (" << fusion_delta.x << ", " << fusion_delta.y
          << ") mm, max dev " << fusion_max_dev << " mm";
    return s.str();
}

} // namespace pcbp::io
