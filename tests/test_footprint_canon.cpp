// ============================================================================
// test_footprint_canon.cpp — R26: placement rotation recovered from board-space pads
// (IPC-D-356), canonical axis-aligned packages, normalise_packages on existing projects,
// generated body rebuilt on package rotation. Synthetic footprints only.
// ============================================================================
#include "../src/core/footprint_canon.h"
#include "../src/core/package_edit.h"
#include "../src/core/project.h"
#include "../src/io/ipc356.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace pcbp;
static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// library footprints (pin order = vector order)
static Package chip(double pitch, double w, double h) {
    Package p; p.pads = {Pad{{-pitch / 2, 0}, w, h}, Pad{{pitch / 2, 0}, w, h}};
    p.pads[0].pin = 1; p.pads[1].pin = 2; return p;
}
static Package soic8() {
    Package p;
    for (int i = 0; i < 4; ++i) { Pad d{{-2.7, 1.905 - 1.27 * i}, 1.55, 0.6}; d.pin = i + 1; p.pads.push_back(d); }
    for (int i = 0; i < 4; ++i) { Pad d{{2.7, -1.905 + 1.27 * i}, 1.55, 0.6}; d.pin = i + 5; p.pads.push_back(d); }
    return p;
}
static Package sot23() {
    Package p;
    Pad a{{-1.0, 0.95}, 1.0, 0.6}; a.pin = 1; Pad b{{-1.0, -0.95}, 1.0, 0.6}; b.pin = 2; Pad c{{1.0, 0}, 1.0, 0.6}; c.pin = 3;
    p.pads = {a, b, c}; return p;
}
static Package bga(int n, double pitch, double d) {
    Package p; int k = 0;
    for (int r = 0; r < n; ++r) for (int c = 0; c < n; ++c) {
        if (r == n - 1 && c == n - 1) continue;   // one missing ball: not 90-symmetric
        Pad q{{(c - (n - 1) / 2.0) * pitch, ((n - 1) / 2.0 - r) * pitch}, d, d}; q.shape = PadShape::Round; q.pin = ++k; p.pads.push_back(q);
    }
    return p;
}

// board pads of a library footprint placed at (pos, rot, side); pad rotations whole degrees (IPC-356A)
static std::vector<BoardPad> place(const Package& lib, Vec2 pos, double rot, Side side) {
    std::vector<BoardPad> out;
    for (auto& d : lib.pads) {
        Vec2 l = d.pos;
        if (side == Side::Bottom) l.x = -l.x;
        BoardPad b;
        b.pos = rotate(l, rot) + pos;
        b.pos = {std::round(b.pos.x * 1e4) / 1e4, std::round(b.pos.y * 1e4) / 1e4};   // Allegro: 4 decimals
        b.w = d.w; b.h = d.h; b.shape = d.shape; b.pin = d.pin; b.pin1 = d.pin == 1;
        b.rot = std::round(norm_deg(side == Side::Bottom ? rot - d.rot : rot + d.rot));
        out.push_back(b);
    }
    return out;
}
// world pads of a placement built from a canon result
static bool round_trip(const std::vector<BoardPad>& in, const CanonFootprint& cf, Side side, double tol = 0.003) {
    Project p;
    Package pk; pk.name = "X"; pk.pads = cf.pads; p.packages["X"] = pk;
    Component c; c.ref = "U1"; c.package = "X"; c.pos = cf.pos; c.rot = cf.rot; c.side = side;
    p.add_component(c);
    const Xform t = p.instance_xform(p.comps[0], 0);
    for (size_t i = 0; i < in.size(); ++i) {
        const Vec2 w = t.apply(cf.pads[i].pos);
        if (std::hypot(w.x - in[i].pos.x, w.y - in[i].pos.y) > tol) { std::printf("   pad %zu off by %.4f\n", i, std::hypot(w.x - in[i].pos.x, w.y - in[i].pos.y)); return false; }
    }
    return true;
}
static bool axis_aligned(const std::vector<Pad>& v) {
    for (auto& d : v) if (std::fabs(std::remainder(d.rot, 90.0)) > 1e-6) return false;
    Package p; p.pads = v;
    return !pkg_axis_info(p).off_axis;
}

int main() {
    const Package libs[] = {chip(1.6, 0.9, 0.95), soic8(), sot23(), bga(7, 0.8, 0.4)};
    const double angles[] = {0, 90, 180, 270, 44.955, 135.025, 20.0, 312.5, 70.0, 5.0};
    for (int li = 0; li < 4; ++li) {
        std::vector<Pad> first;
        for (Side side : {Side::Top, Side::Bottom}) {
            for (double a : angles) {
                const auto bp = place(libs[li], {50 + li * 10.0, 30}, a, side);
                const CanonFootprint cf = canon_footprint(bp, side);
                CHECK(round_trip(bp, cf, side));
                CHECK(axis_aligned(cf.pads));
                // the placement angle equals the library angle up to a pattern symmetry (2-pin / SOIC: 180)
                const double d = std::fabs(std::remainder(cf.rot - a, li == 3 ? 360.0 : 180.0));
                if (d > 0.02) std::printf("   lib %d side %d angle %.3f -> rot %.3f (%s)\n", li, (int)side, a, cf.rot, cf.how.c_str());
                CHECK(d <= 0.02);
                // pin 1 at the canonical place: left (2-pin) / upper-left (>= 3 pins)
                const Vec2 p1 = cf.pads[cf.pin1].pos;
                CHECK(cf.pin1 >= 0 && p1.x < 0 && (li == 0 || p1.y > 0));
                // one package for every angle and both sides
                if (first.empty()) first = cf.pads;
                else CHECK(same_pad_pattern(first, cf.pads));
            }
        }
    }
    // degenerate inputs
    {
        CanonFootprint e = canon_footprint({}, Side::Top);
        CHECK(e.pads.empty() && e.rot == 0);
        BoardPad one; one.pos = {3, 4}; one.w = 1; one.h = 0.5; one.rot = 30;
        CanonFootprint s = canon_footprint({one}, Side::Top);
        CHECK(s.pads.size() == 1 && std::fabs(s.rot - 30) < 1e-9 && std::fabs(s.pos.x - 3) < 1e-9 && s.pads[0].rot == 0);
    }
    // canon_pad folds 90 into w/h; squares mod 90
    {
        Pad a{{0.00004, -0.00004}, 0.6, 1.0}; a.rot = 270;
        canon_pad(a);
        CHECK(a.rot == 0 && a.w == 1.0 && a.h == 0.6 && a.pos.x == 0 && a.pos.y == 0);
        Pad s{{0, 0}, 0.5, 0.5}; s.shape = PadShape::Square; s.rot = 135;
        canon_pad(s);
        CHECK(std::fabs(s.rot - 45) < 1e-9);
    }

    // ---- IPC-356 import: rotation on the placement, one package across angles
    {
        auto rec = [](const char* ref, const char* pin, double x, double y, double w, double h, int rot) {
            std::string s(80, ' ');
            auto put = [&](size_t col1, const std::string& v) { for (size_t i = 0; i < v.size(); ++i) s[col1 - 1 + i] = v[i]; };
            char b[64];
            put(1, "327"); put(4, "N"); put(21, ref); put(27, "-"); put(28, pin); put(39, "A01");
            std::snprintf(b, sizeof b, "X%c%06ldY%c%06ld", x < 0 ? '-' : '+', std::labs(std::lround(x * 1000)), y < 0 ? '-' : '+', std::labs(std::lround(y * 1000))); put(42, b);
            std::snprintf(b, sizeof b, "X%04ldY%04ldR%03d", std::lround(w * 1000), std::lround(h * 1000), rot); put(58, b);
            while (!s.empty() && s.back() == ' ') s.pop_back();
            return s + "\n";
        };
        std::string f = "P  UNITS CUST 1\n";
        const char* refs[] = {"R1", "R2", "R3", "R4"};
        const double ang[] = {0, 44.955, 90, 225.5};
        for (int i = 0; i < 4; ++i)
            for (auto& b : place(chip(1.6, 0.9, 0.95), {10.0 + 10 * i, 20}, ang[i], Side::Top))
                f += rec(refs[i], b.pin == 1 ? "1" : "2", b.pos.x, b.pos.y, b.w, b.h, (int)b.rot);
        for (auto& b : place(soic8(), {60, 20}, 30, Side::Top)) f += rec("U1", std::to_string(b.pin).c_str(), b.pos.x, b.pos.y, b.w, b.h, (int)b.rot);
        f += "999\n";
        Project p; io::Ipc356Result r; std::string err;
        CHECK(io::import_ipc356_text(f, p, {}, r, err));
        CHECK(p.comps.size() == 5 && r.packages == 2);
        for (int i = 0; i < 4; ++i) {
            const Component* c = p.find_ref(refs[i]);
            CHECK(c && std::fabs(std::remainder(c->rot - ang[i], 360.0)) < 0.02 && c->package == p.find_ref("R1")->package);
        }
        const Component* u1 = p.find_ref("U1");
        CHECK(u1 && std::fabs(std::remainder(u1->rot - 30, 360.0)) < 0.02 && u1->pin1_set);
        for (auto& kv : p.packages) CHECK(!pkg_axis_info(kv.second).off_axis);
        // legacy behaviour still available
        Project q; io::Ipc356Options o; o.normalise_rotation = false;
        CHECK(io::import_ipc356_text(f, q, o, r, err) && q.find_ref("R2")->rot == 0 && r.packages == 5);

        // ---- normalise_imported_packages on the legacy import == the new import (board unchanged)
        std::vector<Vec2> before;
        for (auto& c : q.comps) { const Xform t = q.instance_xform(c, 0); for (auto& d : q.package_of(c)->pads) before.push_back(t.apply(d.pos)); }
        NormaliseReport nr = normalise_imported_packages(q);
        CHECK(nr.packages >= 4 && nr.merged == 3 && q.packages.size() == 2);
        size_t k = 0; double worst = 0;
        for (auto& c : q.comps) { const Xform t = q.instance_xform(c, 0); for (auto& d : q.package_of(c)->pads) { const Vec2 w = t.apply(d.pos); worst = std::max(worst, std::hypot(w.x - before[k].x, w.y - before[k].y)); ++k; } }
        CHECK(k == before.size() && worst < 0.003);
        CHECK(std::fabs(std::remainder(q.find_ref("R2")->rot - 44.955, 360.0)) < 0.02);
        for (auto& kv : q.packages) CHECK(!pkg_axis_info(kv.second).off_axis);
    }

    // ---- generated body is rebuilt on rotation (was rotated into a diamond); CAD artwork rotates
    {
        Package g = chip(1.6, 0.9, 0.95);
        pkg_geom_rotate(g, 45);   // pads at 45 deg (positions and angles), no body yet
        g.auto_body();            // the import-time box around diamond pads
        pkg_geom_rotate(g, -45);
        CHECK(g.body.size() == 1 && g.body[0].kind == GKind::Rect && std::fabs(g.body[0].rot) < 1e-9);
        const BBox bb = gprims_bbox(g.body);
        if (std::fabs(bb.h() - 1.05) >= 1e-6) std::printf("   body %.6f x %.6f  pad0 (%.4f,%.4f) r%.6f src=%s\n", bb.w(), bb.h(), g.pads[0].pos.x, g.pads[0].pos.y, g.pads[0].rot, g.body_source.c_str());
        CHECK(std::fabs(bb.h() - (0.95 + 0.1)) < 1e-6);   // pad height + 0.1 mm stroke: axis-aligned again
        Package cad = chip(1.6, 0.9, 0.95);
        GPrim r; r.kind = GKind::Rect; r.pts = {{-1, -0.5}, {1, 0.5}}; cad.body = {r}; cad.body_source = "cad";
        pkg_geom_rotate(cad, 30);
        CHECK(!cad.body.empty() && cad.body_source == "cad");
        const BBox cb = gprims_bbox(cad.body);
        CHECK(cb.w() > 2.0 - 1e-9);   // rotated artwork is wider than the original 2.0
    }

    std::printf("test_footprint_canon: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
