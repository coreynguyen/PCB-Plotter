// ============================================================================
// test_panelize.cpp — tests for core/panelize (CircuitCAM #202 / #321 semantics).
// ============================================================================
#include "../src/core/panelize.h"
#include <cmath>
#include <cstdio>

using namespace pcbp;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool nearv(Vec2 a, Vec2 b, double t = 1e-9) { return std::fabs(a.x - b.x) <= t && std::fabs(a.y - b.y) <= t; }

// where the root image's point p lands in image cell c
static Vec2 map(const PanelCell& c, Vec2 p) { return rotate(p, c.rot) + c.offset; }

int main() {
    std::string err;
    // ---- 0 deg 2x2 ----
    {
        PanelSetup s;
        s.reset();
        s.set(1, 0, 0); s.set(0, 1, 0); s.set(1, 1, 0);
        CHECK(panel_group_enabled(s, 0) && !panel_group_enabled(s, 1));
        PanelMarkers m = panel_markers(s);
        CHECK(m.root_x == 0 && m.root_y == 0);
        CHECK(m.red_x[0] == 1 && m.red_y[0] == 0 && m.blue_x[0] == 0 && m.blue_y[0] == 1);
        s.groups[0].red_x = 100;
        s.groups[0].blue_y = -80;
        CHECK(panelize(s, {}, err));
        CHECK(nearv(s.at(1, 0)->offset, {100, 0}) && nearv(s.at(0, 1)->offset, {0, -80}) && nearv(s.at(1, 1)->offset, {100, -80}));
        for (auto& c : s.cells) CHECK(c.assigned);
    }
    // ---- 0 deg: red at index distance 2 divides the distance ----
    {
        PanelSetup s;
        s.reset();
        s.set(2, 0, 0); s.set(3, 0, 0);
        PanelMarkers m = panel_markers(s);
        CHECK(m.red_x[0] == 2);
        s.groups[0].red_x = 120;     // root -> red spans 2 columns => pitch 60
        CHECK(panelize(s, {}, err));
        CHECK(nearv(s.at(2, 0)->offset, {120, 0}) && nearv(s.at(3, 0)->offset, {180, 0}));
    }
    // ---- root not in corner; units inch ----
    {
        PanelSetup s;
        s.reset();
        s.set(1, 0, 0);
        s.assign_root(1, 0);
        CHECK(s.at(1, 0)->root && !s.at(0, 0)->root);
        PanelMarkers m = panel_markers(s);
        CHECK(m.root_x == 1 && m.red_x[0] == 0);
        s.units = Units::Inch;
        s.groups[0].red_x = -2;       // root -> red is 2" to the left
        CHECK(panelize(s, {}, err));
        CHECK(nearv(s.at(0, 0)->offset, {-50.8, 0}, 1e-9));
    }
    // ---- rotated groups need a reference location ----
    {
        PanelSetup s;
        s.reset();
        s.set(1, 0, 180);
        CHECK(!panelize(s, {}, err));
    }
    // ---- 180 deg row: Yellow->Red / Yellow->Blue measured between ref locations ----
    {
        PanelSetup s;
        s.reset();
        s.set(1, 0, 180); s.set(2, 0, 180);
        s.ref_location = "C1";
        CHECK(!panel_group_enabled(s, 0) && panel_group_enabled(s, 2));
        PanelMarkers m = panel_markers(s);
        CHECK(m.red_x[2] == 1 && m.blue_x[2] == 2);   // blue falls back (same row)
        s.groups[2] = {60, 0, 120, 0};
        Vec2 ref{10, 5};
        CHECK(panelize(s, ref, err));
        CHECK(nearv(s.at(1, 0)->offset, {80, 10}, 1e-9));
        CHECK(nearv(map(*s.at(1, 0), ref), {70, 5}, 1e-9));   // ref + YellowToRed
        CHECK(nearv(map(*s.at(2, 0), ref), {130, 5}, 1e-9));  // ref + YellowToBlue
    }
    // ---- 90 deg 2x2 block next to a 0 deg root ----
    {
        PanelSetup s;
        s.reset();
        s.set(1, 0, 90); s.set(2, 0, 90); s.set(1, 1, 90); s.set(2, 1, 90);
        s.ref_location = "U1";
        PanelMarkers m = panel_markers(s);
        // scan X outer, Y inner: red = (1,0); blue must differ in both X and Y -> (2,1)
        CHECK(m.red_x[1] == 1 && m.red_y[1] == 0 && m.blue_x[1] == 2 && m.blue_y[1] == 1);
        s.groups[1] = {70, 3, 130, -47};       // red (70,3), blue (130,-47)
        Vec2 ref{20, 15};
        CHECK(panelize(s, ref, err));
        CHECK(nearv(map(*s.at(1, 0), ref), ref + Vec2{70, 3}, 1e-9));
        CHECK(nearv(map(*s.at(2, 1), ref), ref + Vec2{130, -47}, 1e-9));
        CHECK(nearv(map(*s.at(2, 0), ref), ref + Vec2{130, 3}, 1e-9));
        CHECK(nearv(map(*s.at(1, 1), ref), ref + Vec2{70, -47}, 1e-9));
        auto req = panel_requirements(s);
        CHECK(req.size() == 2);
    }
    // ---- step & repeat ----
    {
        PanelSetup s;
        s.reset();
        CHECK(!step_and_repeat_all(s, 1, 1, 10, 10, err) && err.find("greater than 1") != std::string::npos);
        CHECK(!step_and_repeat_all(s, 3, 1, 0, 10, err) && err == "X Offset invalid");
        CHECK(!step_and_repeat_all(s, 1, 2, 5, 0, err) && err == "Y Offset invalid");
        CHECK(step_and_repeat_all(s, 3, 2, 50, 40, err));
        CHECK(s.count() == 6 && nearv(s.at(2, 1)->offset, {100, 40}) && !s.at(2, 1)->root);
        auto imgs = panel_images(s);
        CHECK(imgs.size() == 6 && imgs[0].root && imgs[0].name == "1" && imgs[5].name == "6");
        CHECK(imgs[1].x_index == 0 && imgs[1].y_index == 1);   // scan order
        // repeat a 2-image block (root + 180 deg) twice in Y, in mil
        PanelSetup b;
        b.reset();
        b.set(1, 0, 180);
        b.ref_location = "C1";
        b.groups[2] = {60, 0, 0, 0};
        CHECK(panelize(b, {10, 5}, err));
        b.units = Units::Mil;
        CHECK(step_and_repeat_all(b, 1, 2, 0, 1000, err));
        CHECK(b.count() == 4 && b.at(1, 1) && b.at(1, 1)->rot == 180);
        CHECK(nearv(b.at(1, 1)->offset, b.at(1, 0)->offset + Vec2{0, 25.4}, 1e-9));
    }
    // ---- project round trip + frame ----
    {
        Project p;
        p.board_frame = {{{0, 0}, {40, 0}, {40, 30}, {0, 30}, {0, 0}}};
        PanelSetup s;
        s.reset();
        CHECK(step_and_repeat_all(s, 2, 2, 45, 35, err));
        apply_panel(p, s);
        CHECK(p.images.size() == 4 && p.panel_frame.empty());   // R22: never generates a board outline
        PanelSetup r = PanelSetup::from_images(p.images);
        CHECK(r.count() == 4 && r.at(1, 1) && nearv(r.at(1, 1)->offset, {45, 35}) && r.at(0, 0)->root);
        Vec2 rp;
        Component c;
        c.ref = "C1"; c.pos = {3, 4};
        p.add_component(c);
        CHECK(panel_ref_pos(p, "c1", rp) && nearv(rp, {3, 4}));
        CHECK(!s.remove(0, 0) && s.remove(1, 1) && s.count() == 3);
    }
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
