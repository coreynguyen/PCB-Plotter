#pragma once
// ============================================================================
// gcanvas.h — zoom/pan board canvas for the ImGui UI. World units mm, +Y up.
// Draws with Direct2D into a GPU texture only when invalidated; input comes
// from ImGui. Subclasses override the same hooks the old Win32 canvas had.
//
// Mouse model: wheel = zoom at cursor, middle-drag or Space+left-drag = pan,
// Shift+wheel = horizontal pan, left click/drag = tool, right click = context.
// Keyboard (when hovered): Home/F = zoom all, +/- zoom, arrows pan.
// ============================================================================
#include "../core/geom.h"
#include "gfx.h"
#include "imgui.h"

namespace pcbp::gui {

class GCanvas {
public:
    virtual ~GCanvas() = default;
    // Draw the canvas filling `size` at the current cursor. Returns true if hovered.
    bool draw(const char* id, ImVec2 size);
    void invalidate() { dirty_ = true; }

    // view control
    void zoom_all();
    void zoom_to(const BBox& b, double margin = 0.06);
    void zoom_by(double f);                     // about view centre
    void zoom_by_at(double f, ImVec2 screen);   // about a screen point
    void zoom_100();
    void begin_zoom_rect() { zoom_rect_mode_ = true; }
    void end_zoom_rect() { zoom_rect_mode_ = false; }
    // R24a (user v23 #3): navigation modes off -> the active tool gets the clicks again (Select button / Esc)
    void leave_nav_modes() { zoom_rect_mode_ = false; pan_mode_ = false; }
    bool zoom_rect_mode() const { return zoom_rect_mode_; }
    // R17c (REVIEW_V16 §9): Pan tool of the floating viewport toolbar: left-drag pans (tools get no clicks)
    void set_pan_mode(bool on) { pan_mode_ = on; }
    bool pan_mode() const { return pan_mode_; }
    void fly_to(const BBox& b);                 // animated zoom_to
    void set_mirror(bool m) { mirror_ = m; dirty_ = true; }
    bool mirror() const { return mirror_; }
    void set_grid(bool vis, double mm) { if (vis != grid_ || mm != grid_mm_) { grid_ = vis; grid_mm_ = mm; dirty_ = true; } }
    void set_snap(bool s) { snap_ = s; }
    Vec2 snap_pt(Vec2 p) const;
    double px_per_mm() const { return scale_; }     // DIPs per mm
    D2D1_MATRIX_3X2_F world_matrix() const;         // world -> DIPs inside the canvas
    ImVec2 w2s(Vec2 w) const;                       // world -> screen (ImGui coords)
    Vec2 s2w(ImVec2 s) const;                       // screen -> world
    Vec2 mouse_world() const { return mouse_w_; }
    bool hovered() const { return hovered_; }
    ImVec2 origin() const { return pos_; }          // screen pos of canvas top-left
    ImVec2 size() const { return size_; }

protected:
    // hooks
    virtual BBox world_extent() const { return BBox(0, 0, 100, 80); }
    virtual uint32_t background() const;            // ARGB
    virtual void paint_world(ID2D1DeviceContext* rt) { (void)rt; }   // world transform set
    virtual void paint_overlay_d2d(ID2D1DeviceContext* rt) { (void)rt; } // DIP transform
    virtual void paint_overlay(ImDrawList* dl) { (void)dl; }          // live ImGui overlay (cheap, every frame)
    virtual void on_click(Vec2 w, int mods) { (void)w; (void)mods; }
    virtual void on_dblclick(Vec2 w, int mods) { (void)w; (void)mods; }
    virtual void on_rect(const BBox& w, int mods) { (void)w; (void)mods; }
    virtual bool wants_rubber_band() const { return true; }
    virtual bool hit_drag_target(Vec2 w, int mods) { (void)w; (void)mods; return false; }
    virtual void on_drag(Vec2 from, Vec2 to, int mods, bool done) { (void)from; (void)to; (void)mods; (void)done; }
    virtual void on_context(Vec2 w) { (void)w; }   // open an ImGui popup here (BeginPopupContext style)
    virtual void on_mouse_move(Vec2 w) { (void)w; }
    virtual bool on_key(ImGuiKey k, int mods) { (void)k; (void)mods; return false; }
    virtual ImGuiMouseCursor cursor() const { return ImGuiMouseCursor_Arrow; }

    double scale_ = 4.0, cx_ = 50, cy_ = 40;
    bool mirror_ = false, grid_ = true, snap_ = false;
    double grid_mm_ = 2.54;
    bool dirty_ = true;
    // P2-12: pan / zoom do not re-run paint_world every frame. They mark the view as moved; the last render is
    // blitted with the new view transform and re-rendered once input settles (or zoom drifts > 2x / mirror changes).
    void view_changed();
    enum { KM_SHIFT = 1, KM_CTRL = 2, KM_ALT = 4 };

private:
    void render();
    bool cache_covers_view() const;
    static constexpr float kMargin = 0.25f;                 // cached texture = viewport + 25% each side (1.5x)
    static constexpr double kZoomOut = 0.8, kZoomIn = 1.3;  // transformed-cache scale window before re-render
    float r_w_ = 0, r_h_ = 0;                     // cached texture size (px)
public:
    long long renders_ = 0;                       // test / profiling counter
private:
    void draw_grid(ID2D1DeviceContext* rt);
    RenderTexture tex_;
    double r_scale_ = 0, r_cx_ = 0, r_cy_ = 0;   // view the texture was rendered with
    bool r_mirror_ = false, view_moved_ = false;
    double view_t_ = 0;                           // time of the last view change
    ImVec2 pos_{}, size_{};
    bool hovered_ = false, first_ = true, user_zoomed_ = false;
    Vec2 mouse_w_{};
    // interaction
    enum class Drag { None, Pending, Pan, Band, Move, ZoomRect } drag_ = Drag::None;
    Vec2 down_w_{};
    ImVec2 down_s_{};
    int down_mods_ = 0;
    bool zoom_rect_mode_ = false;
    bool pan_mode_ = false;
    // animation
    bool anim_ = false;
    double a0s_, a0x_, a0y_, a1s_, a1x_, a1y_, at_ = 0;
};

} // namespace pcbp::gui
