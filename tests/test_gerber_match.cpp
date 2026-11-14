// ============================================================================
// test_gerber_match.cpp — R22b (REVIEW_V21 §6/§8/§9/§10): Gerber viewer selection model, diagonal
// panel pitch, repeated-circuit pattern search at 0/90/180/270 deg, outline-from-elements exactness.
// Synthetic Gerbers only (written here, parsed by the real parser).
// ============================================================================
#include "../src/io/gerber_match.h"
#include "../src/io/gerber.h"
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>

using namespace pcbp;
using namespace pcbp::gmatch;
namespace gb = pcbp::gerber;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool nearv(Vec2 a, Vec2 b, double t = 1e-6) { return std::fabs(a.x - b.x) <= t && std::fabs(a.y - b.y) <= t; }

// ---- tiny Gerber writer (RS-274X, mm 3.6) ------------------------------------------------------
struct GW {
    std::map<std::tuple<char, long, long>, int> ap;
    std::string apdefs, body;
    int next = 10, cur = -1;
    static long u(double v) { return std::lround(v * 1e6); }
    int aperture(char s, double w, double h) {
        auto k = std::make_tuple(s, std::lround(w * 1e6), std::lround(h * 1e6));
        auto it = ap.find(k);
        if (it != ap.end()) return it->second;
        char b[80];
        if (s == 'C') std::snprintf(b, sizeof b, "%%ADD%dC,%.4f*%%\n", next, w);
        else std::snprintf(b, sizeof b, "%%ADD%dR,%.4fX%.4f*%%\n", next, w, h);
        apdefs += b;
        return ap[k] = next++;
    }
    void sel(int d) { if (d != cur) { body += "D" + std::to_string(d) + "*\n"; cur = d; } }
    void xy(Vec2 p, const char* op) { char b[80]; std::snprintf(b, sizeof b, "X%ldY%ld%s*\n", u(p.x), u(p.y), op); body += b; }
    void flash(char s, double w, double h, Vec2 p) { sel(aperture(s, w, h)); xy(p, "D03"); }
    void line(double wd, Vec2 a, Vec2 b) { sel(aperture('C', wd, wd)); xy(a, "D02"); xy(b, "D01"); }
    void arc_ccw(double wd, Vec2 a, Vec2 b, Vec2 c) {
        sel(aperture('C', wd, wd)); xy(a, "D02");
        char t[120]; std::snprintf(t, sizeof t, "G75*\nG03X%ldY%ldI%ldJ%ldD01*\nG01*\n", u(b.x), u(b.y), u(c.x - a.x), u(c.y - a.y)); body += t;
    }
    std::string text() const { return "%FSLAX36Y36*%\n%MOMM*%\n" + apdefs + "G01*\n" + body + "M02*\n"; }
};

static gb::Layer parse(const std::string& s) {
    gb::Layer L; std::string err; gb::ParseOptions o;
    const bool ok = gb::parse(s, o, L, err);
    if (!ok) std::printf("parse error: %s\n", err.c_str());
    return L;
}

// one circuit (asymmetric): flashes + a trace, local coordinates; instance p' = R(q*90) p + off
static void emit_circuit(GW& g, int q, Vec2 off) {
    auto P = [&](Vec2 p) { return rot_quarter(p, q) + off; };
    auto rect = [&](double w, double h, Vec2 p) { if (q & 1) std::swap(w, h); g.flash('R', w, h, P(p)); };
    rect(1.6, 0.8, {1, 1});
    rect(1.6, 0.8, {1, 3});
    rect(0.6, 1.2, {5, 2});
    g.flash('C', 1.0, 1.0, P({3, 6}));
    g.line(0.2, P({0, 8}), P({4, 8}));
}

static std::vector<int> features_in(const std::vector<Feature>& f, BBox b) {
    std::vector<int> v;
    for (int i = 0; i < (int)f.size(); ++i) if (b.contains(f[i].c)) v.push_back(i);
    return v;
}

static const Candidate* find(const std::vector<Candidate>& c, Vec2 off, double rot) {
    for (auto& k : c) if (nearv(k.offset, off, 1e-4) && std::fabs(k.rot - rot) < 1e-9) return &k;
    return nullptr;
}

static void test_selection_model() {
    ElemSelection s;
    CHECK(s.apply({{0, 1}, {0, 2}}, false) == 2);
    CHECK(s.apply({{0, 2}, {1, 5}}, false) == 1);           // additive: an already selected element stays (no toggle)
    CHECK(s.size() == 3 && s.contains({0, 2}) && s.contains({1, 5}));
    CHECK(s.apply({{0, 2}, {3, 3}}, true) == 1);            // Ctrl: removes only selected ones
    CHECK(s.size() == 2 && !s.contains({0, 2}));
    CHECK(s.apply({{0, 1}}, false) == 0);                   // clicking a selected element again keeps it
    CHECK(s.size() == 2);
    s.clear();
    CHECK(s.empty());
}

static void test_pitch() {
    // two selections of the same features: centre difference = pitch, exact
    GW g;
    emit_circuit(g, 0, {0, 0});
    emit_circuit(g, 0, {20.32, 15.24});
    const gb::Layer L = parse(g.text());
    const auto f = layer_features(L, Xform{}, 0);
    CHECK(f.size() == 10);
    std::vector<Feature> a, b;
    for (int i : features_in(f, BBox(-2, -2, 8, 10))) a.push_back(f[i]);
    for (int i : features_in(f, BBox(18, 13, 30, 26))) b.push_back(f[i]);
    CHECK(a.size() == 5 && b.size() == 5);
    PitchResult r = pitch_from_centres(selection_centre(a), selection_centre(b), PitchMode::Diagonal);
    CHECK(r.ok && std::fabs(r.dx - 20.32) < 1e-6 && std::fabs(r.dy - 15.24) < 1e-6 && r.why.empty());
    // one row: the second circuit is one across only -> Diagonal refuses, Across gives dy = 0
    r = pitch_from_centres({1, 2}, {26, 2.0}, PitchMode::Diagonal);
    CHECK(!r.ok && !r.why.empty());
    r = pitch_from_centres({1, 2}, {26, 2.0}, PitchMode::Across);
    CHECK(r.ok && std::fabs(r.dx - 25) < 1e-12 && r.dy == 0 && r.why.empty());
    r = pitch_from_centres({1, 2}, {26, 2.2}, PitchMode::Across);   // 0.2 mm out of line: ok with a warning
    CHECK(r.ok && std::fabs(r.off_axis - 0.2) < 1e-9 && !r.why.empty() && r.dy == 0);
    r = pitch_from_centres({1, 2}, {1, -18}, PitchMode::Up);        // one column, negative pitch kept signed
    CHECK(r.ok && r.dx == 0 && std::fabs(r.dy + 20) < 1e-12);
    r = pitch_from_centres({1, 2}, {1.01, 2}, PitchMode::Up);
    CHECK(!r.ok);
}

static void test_pattern_2x2_with_180() {
    // 2 x 2 panel, pitch 20 x 15; the top-right circuit is turned 180 deg
    GW g;
    emit_circuit(g, 0, {0, 0});
    emit_circuit(g, 0, {20, 0});
    emit_circuit(g, 0, {0, 15});
    emit_circuit(g, 2, {26, 23});            // 180 deg: occupies 20..26 x 15..23
    g.flash('R', 1.6, 0.8, {40, 40});        // distractor: one lone pad of the same shape
    const gb::Layer L = parse(g.text());
    const auto f = layer_features(L, Xform{}, 0);
    CHECK(f.size() == 21);
    const auto ref = features_in(f, BBox(-2, -2, 8, 10));
    CHECK(ref.size() == 5);
    MatchOptions o;
    std::string why;
    const auto c = find_pattern(f, ref, o, &why);
    CHECK(c.size() == 3);
    const Candidate* b = find(c, {20, 0}, 0);
    const Candidate* u = find(c, {0, 15}, 0);
    const Candidate* r = find(c, {26, 23}, 180);
    CHECK(b && u && r);
    if (b && u && r) {
        CHECK(b->matched == 5 && u->matched == 5 && r->matched == 5);
        CHECK(!b->ambiguous && !u->ambiguous && !r->ambiguous);
        CHECK(b->rms < 1e-6 && r->rms < 1e-6);
    }
    // a subset of the reference (3 pads) still finds the same three circuits
    std::vector<int> sub;
    for (int i : ref) if (f[i].kind == FFlash && sub.size() < 3) sub.push_back(i);
    const auto c2 = find_pattern(f, sub, o);
    CHECK(c2.size() == 3 && find(c2, {26, 23}, 180));
    // 180 disabled: the rotated circuit is not proposed
    MatchOptions o2; o2.rot[2] = false;
    const auto c3 = find_pattern(f, ref, o2);
    CHECK(c3.size() == 2 && !find(c3, {26, 23}, 180));
    // a single element is refused with a reason
    std::string w1;
    CHECK(find_pattern(f, {ref[0]}, o, &w1).empty() && !w1.empty());
}

static void test_pattern_four_rotations() {
    GW g;
    emit_circuit(g, 0, {0, 0});
    emit_circuit(g, 1, {40, 0});
    emit_circuit(g, 2, {60, 20});
    emit_circuit(g, 3, {80, 10});
    const gb::Layer L = parse(g.text());
    const auto f = layer_features(L, Xform{}, 0);
    const auto ref = features_in(f, BBox(-2, -2, 8, 10));
    CHECK(ref.size() == 5);
    MatchOptions o;
    const auto c = find_pattern(f, ref, o);
    CHECK(c.size() == 3);
    CHECK(find(c, {40, 0}, 90) != nullptr);
    CHECK(find(c, {60, 20}, 180) != nullptr);
    CHECK(find(c, {80, 10}, 270) != nullptr);
    // tolerance: a copy displaced by 0.03 mm matches at 0.05, not at 0.01
    GW h;
    emit_circuit(h, 0, {0, 0});
    emit_circuit(h, 0, {30, 0});
    h.flash('R', 1.6, 0.8, {0, 0});   // placeholder never inside the reference box below
    const gb::Layer L2 = parse(h.text());
    auto f2 = layer_features(L2, Xform{}, 0);
    f2.pop_back();
    for (auto& q : f2) if (q.c.x > 20 && q.c.y > 2.5 && q.c.y < 7) q.c.x += 0.03;   // one feature of the copy is off
    const auto r2 = features_in(f2, BBox(-2, -2, 8, 10));
    MatchOptions t5; t5.tol = 0.05;
    MatchOptions t1; t1.tol = 0.01;
    CHECK(find_pattern(f2, r2, t5).size() == 1);
    CHECK(find_pattern(f2, r2, t1).empty());
}

static void test_outline_exact() {
    // rectangle 50 x 30 with a rounded corner (R 2), segments out of order and one reversed
    GW g;
    g.line(0.1, {50, 0}, {50, 28});
    g.line(0.1, {0, 30}, {0, 0});
    g.arc_ccw(0.1, {50, 28}, {48, 30}, {48, 28});
    g.line(0.1, {48, 30}, {0, 30});
    g.line(0.1, {50, 0}, {0, 0});   // reversed
    const gb::Layer L = parse(g.text());
    CHECK(L.objs.size() == 5);
    std::vector<std::vector<Vec2>> open, closed;
    for (auto& o : L.objs) {
        CHECK(outline_element(o));
        bool cl = false;
        auto pl = element_polyline(L, o, Xform{}, cl);
        (cl ? closed : open).push_back(pl);
    }
    const OutlineBuild b = build_outline(open, closed);
    CHECK(b.closed && b.polys.size() == 1);
    if (!b.polys.empty()) {
        const auto& p = b.polys[0];
        auto has = [&](Vec2 q) { for (auto& v : p) if (nearv(v, q, 1e-9)) return true; return false; };
        CHECK(has({0, 0}) && has({50, 0}) && has({50, 28}) && has({48, 30}) && has({0, 30}));
        bool on_arc = true; int narc = 0;
        for (auto& v : p) if (v.x > 48 + 1e-9 && v.y > 28 + 1e-9) { ++narc; on_arc = on_arc && std::fabs(dist(v, {48, 28}) - 2.0) < 1e-9; }
        CHECK(narc > 2 && on_arc);   // arc kept as vector points ON the arc (not a bitmap, not a chord box)
        CHECK(std::fabs(b.box.w() - 50) < 1e-9 && std::fabs(b.box.h() - 30) < 1e-9);
    }
    // open selection (one side missing): no closed contour, the exact chain is returned
    open.erase(open.begin() + 1);
    const OutlineBuild c = build_outline(open, {});
    CHECK(!c.closed && c.polys.size() == 1 && c.polys[0].size() > 4);
    // layer transform (aligned Gerbers) applied exactly
    bool cl = false;
    Xform t = Xform::translate(10, -5);
    auto pl = element_polyline(L, L.objs[0], t, cl);
    CHECK(pl.size() == 2 && nearv(pl[0], {60, -5}, 1e-12) && nearv(pl[1], {60, 23}, 1e-12));
}

int main() {
    test_selection_model();
    test_pitch();
    test_pattern_2x2_with_180();
    test_pattern_four_rotations();
    test_outline_exact();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
