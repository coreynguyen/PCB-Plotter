// derive_machine_tables.cpp — DEV-TIME tool (not part of the app). R24b.
//
// Fits the generic size-band tables used by src/core/machine_assign.cpp from a LOCAL
// component database (Jet .mdb, read with pcbp::jet), and measures hold-out accuracy.
// Only aggregate band edges / majority values are emitted: no part numbers, names,
// descriptions or records leave the database.
//
//   build:  g++ -std=c++20 -O2 -Isrc tools/derive_machine_tables.cpp src/core/machine_assign.cpp 
//               src/io/jet_mdb.cpp src/core/fileio.cpp src/core/project.cpp -o /tmp/derive
//   derive: /tmp/derive derive <db.mdb> src/core/machine_tables.inc
//   eval:   /tmp/derive eval <db.mdb>        (5-fold hold-out, folds = CompoID mod 5)
//
// Expected tables: Compo(CompoID, Description, PkgClassID, Width, Length, Height),
// LaserFx3(CompoID, NozzleNo), Laser(CompoID, GX4NozzleNo), Feeder(CompoID, FdrTypeID, TapeTypeID, Pitch).
#include "core/machine_assign.h"
#include "io/jet_mdb.h"

#include <algorithm>
#include <array>
#include <functional>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace pcbp;
using namespace pcbp::mach;

struct Row {
    int id = 0;
    std::string desc;
    int cls = 0;
    double w = 0, l = 0, h = 0;        // mm
    int noz = 0;                        // remapped
    int fdr = 0;                        // 1 tape 2 stick 4 tray 8 bulk
    int tw = 0;                         // mm
    std::string carrier;
    int pitch = 0;                      // mm
    PkgId pid;
};

static bool load(const char* path, std::vector<Row>& rows) {
    jet::Database db; std::string err;
    if (!db.open(path, err)) { std::fprintf(stderr, "open: %s\n", err.c_str()); return false; }
    jet::Table co, x3, la, fe;
    if (!db.read_table("Compo", co, err) || !db.read_table("Feeder", fe, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return false; }
    db.read_table("LaserFx3", x3, err); db.read_table("Laser", la, err);
    auto idx = [](const jet::Table& t, const char* col) {
        std::map<int64_t, int64_t> m; int ci = t.col("CompoID"), cv = t.col(col);
        if (ci < 0 || cv < 0) return m;
        for (auto& r : t.rows) if (!r[cv].null) m[r[ci].as_int()] = r[cv].as_int();
        return m;
    };
    auto nz3 = idx(x3, "NozzleNo"), nzg = idx(la, "GX4NozzleNo"), fty = idx(fe, "FdrTypeID"), ftt = idx(fe, "TapeTypeID"), fp = idx(fe, "Pitch");
    static const int tw_of[] = {0, 8, 8, 12, 16, 24, 32, 32, 44, 56};
    for (size_t i = 0; i < co.rows.size(); ++i) {
        Row r;
        r.id = (int)co.get(i, "CompoID")->as_int();
        r.desc = co.get(i, "Description")->as_string();
        r.cls = (int)co.get(i, "PkgClassID")->as_int();
        r.w = co.get(i, "Width")->as_double() / 1e4; r.l = co.get(i, "Length")->as_double() / 1e4; r.h = co.get(i, "Height")->as_double() / 1e4;
        int n = nz3.count(r.id) ? (int)nz3[r.id] : 0;
        if (!n && nzg.count(r.id)) n = (int)nzg[r.id];
        r.noz = remap_nozzle(n);
        r.fdr = fty.count(r.id) ? (int)fty[r.id] : 0;
        if (r.fdr == 1 && ftt.count(r.id)) {
            int t = (int)ftt[r.id];
            if (t >= 1 && t <= 9) { r.tw = tw_of[t]; r.carrier = (t == 1 || t == 6) ? "Paper" : "Embossed"; }
            int p = fp.count(r.id) ? (int)fp[r.id] : 0;
            if (p >= 200) p = 2 * (p - 198);
            r.pitch = p;
        }
        r.pid = identify("", r.desc);
        rows.push_back(r);
    }
    return true;
}

// Ordered step fit: maximise correct labels with a non-decreasing step function of x.
static Step fit_step(std::vector<std::pair<double, int>> xs, int feature) {
    Step st; st.feature = feature; st.n = (int)xs.size();
    if (xs.empty()) return st;
    std::sort(xs.begin(), xs.end());
    std::vector<int> labs;
    for (auto& p : xs) labs.push_back(p.second);
    std::sort(labs.begin(), labs.end()); labs.erase(std::unique(labs.begin(), labs.end()), labs.end());
    // keep labels with support >= 1.5 % (avoid one-off bands)
    std::map<int, int> cnt; for (auto& p : xs) cnt[p.second]++;
    std::vector<int> K; for (int v : labs) if (cnt[v] * 1000 >= 15 * (int)xs.size() || labs.size() == 1) K.push_back(v);
    if (K.empty()) K = labs;
    const int N = (int)xs.size(), k = (int)K.size();
    // group by distinct x so a band edge never splits equal x
    std::vector<int> gstart; for (int i = 0; i < N; ++i) if (i == 0 || xs[i].first != xs[i - 1].first) gstart.push_back(i);
    const int G = (int)gstart.size();
    std::vector<std::vector<int>> gc(G, std::vector<int>(k, 0));
    for (int g = 0; g < G; ++g) {
        int e = g + 1 < G ? gstart[g + 1] : N;
        for (int i = gstart[g]; i < e; ++i) for (int j = 0; j < k; ++j) if (xs[i].second == K[j]) gc[g][j]++;
    }
    // dp[g][j] = best correct for groups 0..g with group g labelled K[j], labels non-decreasing
    std::vector<std::vector<int>> dp(G, std::vector<int>(k)), from(G, std::vector<int>(k));
    for (int g = 0; g < G; ++g) {
        int best = -1, bj = 0;
        for (int j = 0; j < k; ++j) {
            if (g > 0 && dp[g - 1][j] > best) { best = dp[g - 1][j]; bj = j; }
            dp[g][j] = (g > 0 ? best : 0) + gc[g][j];
            from[g][j] = bj;
        }
    }
    int j = 0; for (int q = 1; q < k; ++q) if (dp[G - 1][q] > dp[G - 1][j]) j = q;
    std::vector<int> lab(G);
    for (int g = G - 1; g >= 0; --g) { lab[g] = j; if (g > 0) j = from[g][j]; }
    for (int g = 0; g < G; ++g) {
        if (st.vals.empty() || st.vals.back() != K[lab[g]]) {
            if (!st.vals.empty()) st.bounds.back() = 0.5 * (xs[gstart[g] - 1].first + xs[gstart[g]].first);
            st.vals.push_back(K[lab[g]]); st.bounds.push_back(1e9); st.purity.push_back(0);
        }
    }
    // purity per band
    std::vector<int> tot(st.vals.size(), 0), ok(st.vals.size(), 0);
    for (auto& p : xs) {
        size_t b = 0; while (b + 1 < st.vals.size() && p.first > st.bounds[b]) ++b;
        tot[b]++; if (p.second == st.vals[b]) ok[b]++;
    }
    for (size_t b = 0; b < st.vals.size(); ++b) st.purity[b] = tot[b] ? (double)ok[b] / tot[b] : 0;
    return st;
}
static int step_correct(const Step& s, const std::vector<std::pair<double, int>>& xs) {
    int c = 0; for (auto& p : xs) if (s.eval(p.first, p.first) == p.second) ++c; return c;
}
static Step best_step(const std::vector<std::array<double, 3>>& v) {   // (short, long, label)
    std::vector<std::pair<double, int>> a, b;
    for (auto& r : v) { a.push_back({r[0], (int)r[2]}); b.push_back({r[1], (int)r[2]}); }
    Step s0 = fit_step(a, 0), s1 = fit_step(b, 1);
    return step_correct(s1, b) > step_correct(s0, a) ? s1 : s0;
}

static bool train_row_ok(const Row& r) { return r.w > 0 && r.l > 0; }

static Tables derive(const std::vector<Row>& rows, const std::function<bool(const Row&)>& use) {
    Tables t;
    std::map<std::string, std::vector<std::array<double, 3>>> noz, tw, pit;
    std::map<std::string, std::map<int, int>> cls;
    int n = 0;
    for (auto& r : rows) {
        if (!use(r)) continue;
        const std::string f = fam_name(r.pid.fam);
        if (r.cls) cls[f][r.cls]++;
        if (!train_row_ok(r)) continue;
        ++n;
        const double s = std::min(r.w, r.l), l = std::max(r.w, r.l);
        if (r.noz) { noz[f].push_back({s, l, (double)r.noz}); noz["ALL"].push_back({s, l, (double)r.noz}); }
        if (r.tw) {
            tw[f].push_back({s, l, (double)r.tw}); tw["ALL"].push_back({s, l, (double)r.tw});
            if (r.pitch > 0) pit[std::to_string(r.tw)].push_back({s, l, (double)r.pitch});
        }
    }
    for (auto& kv : noz) if (kv.first != "Unknown" || true) t.steps["noz." + kv.first] = best_step(kv.second);
    for (auto& kv : tw) t.steps["tw." + kv.first] = best_step(kv.second);
    for (auto& kv : pit) t.steps["pitch." + kv.first] = best_step(kv.second);
    for (auto& kv : cls) {
        if (kv.first == "Unknown") continue;
        int bv = 0, bc = 0; for (auto& c : kv.second) if (c.second > bc) { bc = c.second; bv = c.first; }
        t.consts["type." + kv.first] = bv;
    }
    char b[160];
    std::snprintf(b, sizeof b, "derive_machine_tables: %d rows with body dims; ordered step fit (DP), labels >=1.5%% support; R24b", n);
    t.source = b;
    return t;
}

struct Acc { int n = 0, cov = 0, ok = 0; void add(bool has_truth, int got, int want) { if (!has_truth) return; ++n; if (got) { ++cov; if (got == want) ++ok; } } };
static void pr(const char* name, const Acc& a) {
    std::printf("  %-10s truth %5d  covered %5d (%5.1f%%)  exact %5.1f%% of covered  (%5.1f%% of all)\n", name, a.n, a.cov,
                a.n ? 100.0 * a.cov / a.n : 0, a.cov ? 100.0 * a.ok / a.cov : 0, a.n ? 100.0 * a.ok / a.n : 0);
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: derive <db> <out.inc> | eval <db>\n"); return 2; }
    std::vector<Row> rows;
    if (!load(argv[2], rows)) return 1;
    const std::string mode = argv[1];
    if (mode == "derive") {
        Tables t = derive(rows, [](const Row&) { return true; });
        std::string txt = format_tables(t);
        FILE* f = std::fopen(argv[3], "wb");
        if (!f) return 1;
        std::fprintf(f, "// GENERATED by tools/derive_machine_tables.cpp — generic size bands only (no records). Do not edit.\n");
        std::fprintf(f, "R\"MT(%s)MT\"\n", txt.c_str());
        std::fclose(f);
        std::printf("%s", txt.c_str());
        return 0;
    }
    // ---- 5-fold hold-out
    std::map<int, int> famcount; for (auto& r : rows) famcount[(int)r.pid.fam]++;
    std::printf("rows %zu; families from text:", rows.size());
    for (auto& kv : famcount) std::printf(" %s=%d", fam_name((Fam)kv.first), kv.second);
    std::printf("\n");
    for (int mode2 = 0; mode2 < 2; ++mode2) {
        Acc noz, tw, pit, car, typ, pkg, pkg_rule, all;
        std::map<std::string, Acc> noz_by;
        int idc = 0, n = 0;
        for (int fold = 0; fold < 5; ++fold) {
            Tables t = derive(rows, [&](const Row& r) { return r.id % 5 != fold; });
            for (auto& r : rows) {
                if (r.id % 5 != fold) continue;
                Input in; in.desc = r.desc;
                if (mode2 == 1) { in.body_x = r.w; in.body_y = r.l; in.body_h = r.h; }
                Suggestion s = suggest(in, t);
                ++n; if (s.fam != Fam::Unknown) ++idc;
                noz.add(r.noz != 0, s.f.nozzle, r.noz);
                noz_by[fam_name(s.fam)].add(r.noz != 0, s.f.nozzle, r.noz);
                tw.add(r.tw != 0, s.f.tape_w, r.tw);
                pit.add(r.pitch != 0, s.f.pitch, r.pitch);
                car.add(!r.carrier.empty(), s.f.carrier.empty() ? 0 : (s.f.carrier == "Paper" ? 1 : 2), r.carrier == "Paper" ? 1 : 2);
                typ.add(r.cls != 0, s.f.type_code, r.cls);
                const int gk = s.f.packaging == "Tape" ? 1 : s.f.packaging == "Stick" ? 2 : s.f.packaging == "Tray" ? 4 : 8;
                pkg.add(r.fdr != 0, s.fam == Fam::Unknown ? 0 : gk, r.fdr);
                if (r.noz && r.tw) all.add(true, (s.f.nozzle && s.f.tape_w) ? 1 : 0,
                                           (s.f.nozzle == r.noz && s.f.tape_w == r.tw && s.f.pitch == r.pitch) ? 1 : 2);
            }
        }
        std::printf("\n== hold-out, input = %s ==  rows %d, package identified %d (%.1f%%)\n",
                    mode2 ? "description text + DB body dims (as CAD)" : "description text only", n, idc, 100.0 * idc / n);
        pr("nozzle", noz); pr("tape_w", tw); pr("pitch", pit); pr("carrier", car); pr("type", typ); pr("packaging", pkg);
        pr("noz+tw+pit", all);
        std::printf("  nozzle by family:\n");
        for (auto& kv : noz_by) if (kv.second.n >= 20) { std::printf("  "); pr(kv.first.c_str(), kv.second); }
    }
    return 0;
}
