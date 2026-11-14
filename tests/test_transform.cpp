// test_transform.cpp — R16c (REVIEW_V15 §9): unified Transform maths.
#include "../src/core/transform.h"
#include <cmath>
#include <cstdio>

using namespace pcbp;
static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool near(double a, double b, double e = 1e-6) { return std::fabs(a - b) < e; }
static bool nearv(Vec2 a, Vec2 b) { return near(a.x, b.x) && near(a.y, b.y); }
static bool near_ang(double a, double b) { double d = std::fmod(std::fabs(a - b), 360.0); return d < 1e-6 || d > 360 - 1e-6; }

static Project make() {
    Project p;
    Component a; a.ref = "R1"; a.pos = {10, 0}; a.rot = 0; a.side = Side::Top; p.add_component(a);
    Component b; b.ref = "C1"; b.pos = {0, 5}; b.rot = 90; b.side = Side::Bottom; p.add_component(b);
    p.board_frame = {{{0, 0}, {20, 0}, {20, 10}, {0, 10}}};
    p.panel_frame = {{{-5, -5}, {25, -5}, {25, 15}, {-5, 15}}};
    return p;
}

int main() {
    {   // nothing enabled: identity, nothing changes
        Project p = make();
        TransformSpec s;
        CHECK(!s.any());
        CHECK(apply_transform(p, s) == 0);
        CHECK(nearv(p.comps[0].pos, {10, 0}));
    }
    {   // translate only
        Project p = make();
        TransformSpec s; s.translate = true; s.dx = 1.5; s.dy = -2;
        CHECK(apply_transform(p, s) == 2);
        CHECK(nearv(p.comps[0].pos, {11.5, -2}));
        CHECK(near_ang(p.comps[0].rot, 0));
        CHECK(nearv(p.board_frame[0][2], {21.5, 8}));
        CHECK(nearv(p.virtual_origin, {1.5, -2}));
    }
    {   // rotate 90 about the origin: (10,0) -> (0,10); angles + 90
        Project p = make();
        TransformSpec s; s.rotate = true; s.angle = 90;
        apply_transform(p, s);
        CHECK(nearv(p.comps[0].pos, {0, 10}));
        CHECK(near_ang(p.comps[0].rot, 90));
        CHECK(near_ang(p.comps[1].rot, 180));
        CHECK(p.comps[1].side == Side::Bottom);
    }
    {   // rotate about a pivot keeps the pivot fixed
        TransformSpec s; s.rotate = true; s.angle = 180; s.pivot = {10, 5};
        Xform T = transform_xform(s);
        CHECK(nearv(T.apply({10, 5}), {10, 5}));
        CHECK(nearv(T.apply({0, 0}), {20, 10}));
    }
    {   // mirror X (geometric): x -> -x, rot -> 180 - rot, side unchanged
        Project p = make();
        TransformSpec s; s.mirror = true; s.mirror_axis = 0;
        apply_transform(p, s);
        CHECK(nearv(p.comps[0].pos, {-10, 0}));
        CHECK(near_ang(p.comps[0].rot, 180));
        CHECK(near_ang(p.comps[1].rot, 90));
        CHECK(p.comps[0].side == Side::Top);
    }
    {   // mirror Y: y -> -y, rot -> -rot
        Project p = make();
        p.comps[0].rot = 30;
        TransformSpec s; s.mirror = true; s.mirror_axis = 1;
        apply_transform(p, s);
        CHECK(nearv(p.comps[1].pos, {0, -5}));
        CHECK(near_ang(p.comps[0].rot, 330));
    }
    {   // board flip: side changes, angle = -rot (Align convention)
        Project p = make();
        p.comps[0].rot = 30;
        TransformSpec s; s.mirror = true; s.flip_side = true;
        apply_transform(p, s);
        CHECK(p.comps[0].side == Side::Bottom);
        CHECK(p.comps[1].side == Side::Top);
        CHECK(near_ang(p.comps[0].rot, 330));
    }
    {   // scale about a pivot: positions only
        Project p = make();
        TransformSpec s; s.scale = true; s.sx = 2; s.pivot = {10, 0};
        apply_transform(p, s);
        CHECK(nearv(p.comps[0].pos, {10, 0}));
        CHECK(nearv(p.comps[1].pos, {-10, 10}));
        CHECK(near_ang(p.comps[1].rot, 90));
    }
    {   // order: scale -> mirror -> rotate -> translate
        TransformSpec s; s.scale = true; s.sx = 2; s.mirror = true; s.rotate = true; s.angle = 90; s.translate = true; s.dx = 1;
        Xform T = transform_xform(s);
        // (1,0) -> scale (2,0) -> mirror X (-2,0) -> rot90 (0,-2) -> +1 (1,-2)
        CHECK(nearv(T.apply({1, 0}), {1, -2}));
    }
    {   // side filter + outlines off
        Project p = make();
        TransformSpec s; s.translate = true; s.dx = 3; s.sides = 2; s.board_outline = false; s.circuit_outline = false; s.move_origin = false;
        CHECK(apply_transform(p, s) == 1);
        CHECK(nearv(p.comps[0].pos, {10, 0}));
        CHECK(nearv(p.comps[1].pos, {3, 5}));
        CHECK(nearv(p.board_frame[0][0], {0, 0}));
        CHECK(nearv(p.virtual_origin, {0, 0}));
    }
    {   // panel instance follows a rotation (conjugation)
        Project p = make();
        PanelImage im; im.offset = {30, 0}; im.rot = 0; p.images.push_back(im);
        TransformSpec s; s.rotate = true; s.angle = 90;
        apply_transform(p, s);
        // instance point q = (x+30, y); after T: R q; image' must map R p -> R(p + 30,0) = R p + (0,30)
        CHECK(nearv(p.images[1].offset, {0, 30}));
        CHECK(near_ang(p.images[1].rot, 0));
    }
    std::printf("test_transform: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
