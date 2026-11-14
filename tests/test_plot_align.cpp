// ============================================================================
// test_plot_align.cpp — R14: CAD/plot <-> Gerber alignment, provenance-aware moves,
// Gerber-driven panel helpers and instance export through the JUKI H8H circuit paths.
// Synthetic data only.
// ============================================================================
#include "../src/core/plot_align.h"
#include "../src/core/panelize.h"
#include "../src/core/machine_job.h"
#include "../src/core/project_io.h"
#include "../src/io/juki_writer.h"
#include <cmath>
#include <cstdio>

using namespace pcbp;
namespace pj = pcbp::juki;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool nearv(Vec2 a, Vec2 b, double t = 1e-6) { return std::fabs(a.x - b.x) <= t && std::fabs(a.y - b.y) <= t; }
static bool neara(double a, double b, double t = 1e-6) { return std::fabs(std::remainder(a - b, 360.0)) <= t; }

// synthetic QFP-ish + asymmetric pad pattern (pin-1 pad bigger offset so rotations are distinguishable)
static std::vector<Vec2> pattern(Vec2 c) {
    std::vector<Vec2> v;
    for (int i = 0; i < 6; ++i) { v.push_back(c + Vec2{-3.0 + i * 1.0, -4}); v.push_back(c + Vec2{-3.0 + i * 1.0, 4}); }
    v.push_back(c + Vec2{-5, 2.5});   // asymmetric marker pad
    return v;
}

static Project make_project() {
    Project p;
    Package pk; pk.name = "P13";
    for (auto& q : pattern({0, 0})) pk.pads.push_back(Pad{q, 0.5, 0.5});
    p.packages["P13"] = pk;
    Package r; r.name = "0603"; r.pads = {Pad{{-0.8, 0}, 0.8, 0.9}, Pad{{0.8, 0}, 0.8, 0.9}}; p.packages["0603"] = r;
    p.board_frame = {{{0, 0}, {40, 0}, {40, 30}, {0, 30}, {0, 0}}};
    p.model.points.clear();
    ModelPoint mp; mp.name = "Top SMT"; mp.has_machine = true; mp.machine.exporter_id = "juki.h8h";
    p.model.points.push_back(mp);
    auto mk = [&](const char* ref, double x, double y, double a, const char* pkg) {
        Component c; c.ref = ref; c.pos = c.cad_pos = {x, y}; c.rot = c.rot_imported = a;
        c.package = pkg; c.number = std::string("PN-") + pkg; c.assigned_point = 0; p.add_component(c);
    };
    mk("U1", 12, 15, 0, "P13");
    mk("R1", 30, 8, 90, "0603");
    mk("R2", 30, 22, 0, "0603");
    return p;
}

static void test_fit() {
    // CAD pads of U1, Gerber = CAD rotated by an arbitrary angle, mirrored or not, and offset
    const std::vector<Vec2> cad = pattern({12, 15});
    for (int m = 0; m < 2; ++m)
        for (double ang : {0.0, 90.0, 180.0, 270.0, 33.7, -12.25}) {
            const Xform truth = Xform::translate(101.234, -57.891) * Xform::rotation(ang) * (m ? Xform::scale(-1, 1) : Xform{});
            std::vector<Vec2> ger;
            for (auto& q : cad) ger.push_back(truth.apply(q));
            std::swap(ger[0], ger[7]);   // selection order is arbitrary
            AlignFit f;
            const double hint = std::round(ang / 90.0) * 90.0;
            CHECK(fit_cad_to_gerber(cad, ger, hint, m != 0, true, f));
            CHECK(f.mirror == (m != 0) && neara(f.rot, ang, 1e-6) && f.rms < 1e-6 && f.matched == (int)cad.size());
            CHECK(nearv(f.T.apply({0, 0}), truth.apply({0, 0}), 0.01) && nearv(f.T.apply({50, 50}), truth.apply({50, 50}), 0.01));
            if (std::fmod(std::fabs(ang), 90.0) == 0) {
                AlignFit d;
                CHECK(detect_cad_to_gerber(cad, ger, true, false, d) && d.mirror == (m != 0) && neara(d.rot, ang, 1e-9) && d.rms < 1e-9);
            }
        }
    // selection centre only (translation): exact when rotation is known
    std::vector<Vec2> ger;
    for (auto& q : cad) ger.push_back(q + Vec2{7, -3});
    AlignFit f;
    CHECK(fit_cad_to_gerber(cad, ger, 0, false, false, f) && nearv({f.T.tx, f.T.ty}, {7, -3}, 1e-9));
}

static void test_layer_compose() {
    GerberLayerRef L; L.offset = {5, 6}; L.rot = 30; L.mirror = true;
    const Xform T = Xform::translate(-20, 13) * Xform::rotation(90) * Xform::scale(-1, 1);
    const Xform want = T.inverse() * layer_ref_xform(L);
    premultiply_layer(L, T.inverse());
    const Xform got = layer_ref_xform(L);
    CHECK(!L.mirror);   // two mirrors cancel
    for (Vec2 q : {Vec2{0, 0}, Vec2{10, 3}, Vec2{-7, 22}}) CHECK(nearv(got.apply(q), want.apply(q), 1e-9));
}

static void test_move_plot() {
    Project p = make_project();
    Component g; g.ref = "FIDG"; g.pos = {200, 200}; g.feature = Feature::Fiducial; g.from_gerber = true; p.add_component(g);
    p.panel_frame = {{{190, 190}, {260, 190}, {260, 260}}};
    p.panel_frame_from_gerber = true;
    const Xform T = Xform::translate(100, 50) * Xform::rotation(90);
    // reference: world position of R1 pad 1 before
    const Component* r1 = p.find_ref("R1");
    const Vec2 pad0 = p.comp_xform(*r1).apply(p.packages["0603"].pads[0].pos);
    move_plot(p, T);
    r1 = p.find_ref("R1");
    CHECK(nearv(p.comp_xform(*r1).apply(p.packages["0603"].pads[0].pos), T.apply(pad0), 1e-9));
    CHECK(neara(r1->rot, 180, 1e-9));
    CHECK(nearv(p.find_ref("FIDG")->pos, {200, 200}));          // Gerber-built: stays
    CHECK(nearv(p.panel_frame[0][0], {190, 190}));
    CHECK(nearv(p.board_frame[0][1], T.apply({40, 0}), 1e-9));
    // mirrored move: pads land where the mirrored transform puts them, part changes side
    Project q = make_project();
    const Xform M = Xform::translate(80, 0) * Xform::rotation(30) * Xform::scale(-1, 1);
    const Component* u = q.find_ref("U1");
    std::vector<Vec2> before;
    for (auto& pd : q.packages["P13"].pads) before.push_back(q.comp_xform(*u).apply(pd.pos));
    move_plot(q, M);
    u = q.find_ref("U1");
    CHECK(u->side == Side::Bottom);
    for (size_t i = 0; i < before.size(); ++i) CHECK(nearv(q.comp_xform(*u).apply(q.packages["P13"].pads[i].pos), M.apply(before[i]), 1e-9));
    // panel images move rigidly with the plot
    Project r = make_project();
    r.images = step_repeat_grid(2, 1, 45, 0);
    PanelImage rot; rot.offset = {150, 10}; rot.rot = 180; add_instance(r, rot);
    const Vec2 w2 = r.image_xform(2).apply(r.find_ref("R2")->pos);
    move_plot(r, T);
    CHECK(nearv(r.image_xform(2).apply(r.find_ref("R2")->pos), T.apply(w2), 1e-9));
    // move_gerber_built follows the Gerbers
    Project s = make_project();
    s.add_component(g);
    move_gerber_built(s, T.inverse());
    CHECK(nearv(s.find_ref("FIDG")->pos, T.inverse().apply({200, 200}), 1e-9) && nearv(s.find_ref("U1")->pos, {12, 15}));
}

static void test_panel_helpers() {
    PitchPick pk = pitch_from_points({12, 15}, {57.002, 15.001}, {12.0, 50}, true, true);
    CHECK(std::fabs(pk.dx - 45.002) < 1e-9 && std::fabs(pk.dy - 35) < 1e-9 && std::fabs(pk.skew.y - 0.001) < 1e-9);
    BBox circ; circ.add({0, 0}); circ.add({40, 30});
    BBox ger; ger.add({-50, -5}); ger.add({130, 105});   // reference circuit is the 2nd column, 1st row of 4 x 3
    GridSuggest gs = suggest_grid(circ, ger, 45, 35);
    CHECK(gs.nx == 4 && gs.ny == 3 && gs.root_col == 1 && gs.root_row == 0);
    auto ims = step_repeat_grid(gs.nx, gs.ny, 45, 35, gs.root_col, gs.root_row);
    CHECK(ims.size() == 12 && ims[0].root && ims[0].x_index == 1);
    bool left = false;
    for (auto& im : ims) left |= nearv(im.offset, {-45, 70});
    CHECK(left);
    // rotated instance from two point pairs (180 and arbitrary), and one pair + hint
    const Vec2 p1{12, 15}, p2{30, 22};
    for (double a : {180.0, 90.0, 270.0, 17.5}) {
        const Xform I = Xform::translate(140, 80) * Xform::rotation(a);
        PanelImage im = instance_from_points(p1, I.apply(p1), p2, I.apply(p2), true, 0, false);
        CHECK(neara(im.rot, a, 1e-9) && nearv(im.offset, {140, 80}, 1e-9));
        PanelImage one = instance_from_points(p1, I.apply(p1), {}, {}, false, a, false);
        CHECK(nearv(one.offset, {140, 80}, 1e-9));
    }
    PanelImage sn = instance_from_points(p1, Vec2{140, 80} + rotate(p1, 180), p2, Vec2{140.02, 80} + rotate(p2, 180.05), true, 0, true);
    CHECK(sn.rot == 180);
}

// instances -> machine job -> H8H circuit modes (matrix for a regular unrotated grid, non-matrix with rotation)
static void test_export() {
    for (int variant = 0; variant < 3; ++variant) {
        Project p = make_project();
        if (variant == 0) p.images = step_repeat_grid(3, 2, 45, 35);
        if (variant == 1) { p.images = step_repeat_grid(2, 1, 45, 0); PanelImage r; r.offset = {130, 30}; r.rot = 180; add_instance(p, r); }
        if (variant == 2) { p.images = step_repeat_grid(1, 1, 0, 0); PanelImage r; r.offset = {80, 0}; r.rot = 90; add_instance(p, r); PanelImage s; s.offset = {80, 80}; s.rot = 270; add_instance(p, s); }
        BBox pb;
        for (int i = 0; i < (int)p.images.size(); ++i) for (auto& q : p.circuit_frame_at(i)) for (auto& v : q) pb.add(v);
        p.set_panel_frame_rect(pb);
        MachineJob j; std::string err;
        CHECK(build_machine_job(p, 0, j, err));
        CHECK(j.circuits.size() == p.images.size());
        // every placement instance must equal R(arot)*circuit-1 + a (the H8H repeat model)
        for (auto& pl : j.placements) {
            if (pl.circuit <= 1) continue;
            const JobCircuit* ck = nullptr;
            for (auto& c : j.circuits) if (c.index == pl.circuit) ck = &c;
            const JobPlacement* b = nullptr;
            for (auto& q : j.placements) if (q.circuit == 1 && q.ref == pl.ref) b = &q;
            CHECK(ck && b);
            if (ck && b) CHECK(nearv(rotate({b->x, b->y}, ck->arot) + Vec2{ck->ax, ck->ay}, {pl.x, pl.y}, 1e-6) && neara(pl.rot, b->rot + ck->arot, 1e-6));
        }
        for (auto& c : j.circuits) {
            const double expect = c.index == 1 ? 0 : p.images[c.index - 1].rot;
            CHECK(neara(c.arot, expect, 1e-9));
        }
        pj::CircuitPlan cp = pj::plan_circuits(j);
        CHECK(cp.error.empty() && cp.mode == (variant == 0 ? pj::CircuitMode::Matrix : pj::CircuitMode::NonMatrix));
        if (variant == 0) CHECK(cp.nx == 3 && cp.ny == 2 && std::fabs(cp.px - 45) < 1e-9 && std::fabs(cp.py - 35) < 1e-9);
        j.options["juki.timestamp"] = "1700000000";
        std::vector<uint8_t> b;
        pj::HlcWriteInfo wi;
        const bool ok = pj::write_hlc_variant(j, pj::HlcVariant::H8H, b, err, &wi);
        if (!ok) std::printf("  variant %d: %s\n", variant, err.c_str());
        CHECK(ok && wi.mode == cp.mode);
        // forced non-matrix also works for the regular grid
        if (variant == 0) {
            j.options["juki.circuit_mode"] = "nonmatrix";
            CHECK(pj::write_hlc_variant(j, pj::HlcVariant::H8H, b, err, &wi) && wi.mode == pj::CircuitMode::NonMatrix);
        }
    }
}

static void test_io() {
    Project p = make_project();
    p.comps[0].from_gerber = true;
    p.board_frame_from_gerber = true;
    std::string err;
    Project q;
    CHECK(project_from_json(project_to_json(p), q, err));
    CHECK(q.comps.size() == p.comps.size() && q.comps[0].from_gerber && !q.comps[1].from_gerber && q.board_frame_from_gerber && !q.panel_frame_from_gerber);
}

static void test_outlines() {
    Project p = make_project();
    p.images = step_repeat_grid(2, 1, 45, 0);
    p.set_panel_frame_rect(BBox(-5, -5, 90, 35));
    auto has = [](const std::vector<FrameIssue>& v, const char* k) { for (auto& i : v) if (i.msg.find(k) != std::string::npos) return true; return false; };
    auto v = check_outlines(p);
    CHECK(v.empty());
    p.images = step_repeat_grid(2, 1, 35, 0);   // pitch smaller than the 40 mm circuit
    v = check_outlines(p);
    CHECK(has(v, "overlap"));
    p.images = step_repeat_grid(2, 1, 40, 0);   // touching edges are fine
    CHECK(!has(check_outlines(p), "overlap"));
    p.images = step_repeat_grid(3, 1, 45, 0);   // third circuit beyond the board
    CHECK(has(check_outlines(p), "beyond the board"));
    p.images = step_repeat_grid(2, 1, 45, 0);
    p.find_ref("R1")->pos = {41, 8};
    v = check_outlines(p);
    CHECK(has(v, "outside the circuit outline") && v[0].ref == "R1");
    CHECK(has(check_outlines(p, 80, 50), "length"));
    BBox r = rect_from_ref_offsets({12, 15}, {-12, 15}, {28, -15});
    CHECK(nearv({r.x0, r.y0}, {0, 0}) && nearv({r.x1, r.y1}, {40, 30}));
}

int main() {
    test_outlines();
    test_fit();
    test_layer_compose();
    test_move_plot();
    test_panel_helpers();
    test_export();
    test_io();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
