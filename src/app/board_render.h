#pragma once
// ============================================================================
// board_render.h — shared Direct2D renderer for the assembled board
// (Universal view, Visual Aid, Machine View, printing).
// ============================================================================
#include "doc.h"
#include "../ui/d2d.h"

namespace pcbp::app {

struct BoardRenderOpts {
    D2D1_MATRIX_3X2_F world{};      // world (mm, +Y up) -> target DIPs
    double px_per_mm = 4;           // DIPs per mm (for hairlines / text sizing)
    bool mirror = false;            // viewing from bottom
    int focus_point = -1;           // >=0: colour only this point's comps; others ghosted
    bool only_focus = false;        // hide comps not in focus_point
    bool show_labels = true, show_frame = true, show_top = true, show_bottom = true, all_images = true;
    bool highlight_unassigned = false, highlight_noref = false, verify_angles = false;
    bool show_pads = true, show_origin = true, show_selection = true;
    bool second_side = true;        // false: hide the side facing away from the viewer
    // ghost_far: draw the far side even when it is not in show_top/show_bottom
    // (Ghost / X-ray display: visible but not selectable). far_alpha = its opacity.
    bool ghost_far = false;
    float far_alpha = 0.35f;
    bool print_mode = false;        // white background friendly colours
    // polarity markers (R15, package data): 0 hidden, 1 shown (packages with a polarity marker only),
    // 2 = markers + every placement's pin-1 pad highlighted (R26)
    int orient_markers = 1;
    bool lead_labels = false;       // lead-index numbers on pads (hidden by default)
    const std::vector<bool>* angle_errors = nullptr;   // per comp index: machine angle not calculated
    float dip_per_px = 1.f;
};

void render_board(ID2D1RenderTarget* rt, const Doc& d, const BoardRenderOpts& o);
// Underlay hook (board photo / AOI images, gui/board_image.cpp): called by
// render_board after the board fill, before frames / components. nullptr = none.
using BoardUnderlayHook = void (*)(ID2D1RenderTarget* rt, const Doc& d, const BoardRenderOpts& o);
extern BoardUnderlayHook g_board_underlay;

// hit testing (world mm). Returns component index or -1. Prefers smallest bbox.
int hit_component(const Doc& d, Vec2 p, double tol_mm, bool top_vis, bool bot_vis);
// returns pad index of component under point or -1
int hit_pad(const Doc& d, const Component& c, Vec2 p, double tol_mm);
// world position of pad i of component
Vec2 pad_world(const Doc& d, const Component& c, int pad);
uint32_t point_color(const Doc& d, int point);

} // namespace pcbp::app
