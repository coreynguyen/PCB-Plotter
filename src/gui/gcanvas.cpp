// ============================================================================
// gcanvas.cpp — ImGui-hosted D2D canvas.
// ============================================================================
#include "gcanvas.h"
#include "view_cache.h"
#include "gapp.h"
#include "../ui/d2d.h"
#include "../ui/theme.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pcbp::gui {

uint32_t GCanvas::background() const { return ui::pal().canvas_bg; }

D2D1_MATRIX_3X2_F GCanvas::world_matrix() const {
    const float sx = (float)(mirror_ ? -scale_ : scale_), sy = (float)-scale_;
    return D2D1::Matrix3x2F(sx, 0, 0, sy, (float)(size_.x * 0.5 - cx_ * sx), (float)(size_.y * 0.5 - cy_ * sy));
}

ImVec2 GCanvas::w2s(Vec2 w) const {
    double x = (w.x - cx_) * scale_;
    if (mirror_) x = -x;
    double y = -(w.y - cy_) * scale_;
    return ImVec2((float)(pos_.x + size_.x * 0.5 + x), (float)(pos_.y + size_.y * 0.5 + y));
}

Vec2 GCanvas::s2w(ImVec2 s) const {
    double dx = (s.x - pos_.x - size_.x * 0.5) / scale_;
    if (mirror_) dx = -dx;
    double dy = -(s.y - pos_.y - size_.y * 0.5) / scale_;
    return {cx_ + dx, cy_ + dy};
}

Vec2 GCanvas::snap_pt(Vec2 p) const {
    if (!snap_ || grid_mm_ <= 0) return p;
    return {std::round(p.x / grid_mm_) * grid_mm_, std::round(p.y / grid_mm_) * grid_mm_};
}

void GCanvas::zoom_to(const BBox& b0, double margin) {
    BBox b = b0.empty() ? BBox(0, 0, 100, 80) : b0;
    if (size_.x < 4 || size_.y < 4) { first_ = true; return; }
    double w = std::max(b.w(), 0.5), h = std::max(b.h(), 0.5);
    scale_ = std::clamp(std::min(size_.x * (1 - 2 * margin) / w, size_.y * (1 - 2 * margin) / h), 0.01, 50000.0);
    cx_ = b.center().x;
    cy_ = b.center().y;
    anim_ = false;
    dirty_ = true;
}

void GCanvas::zoom_all() { zoom_to(world_extent()); user_zoomed_ = false; }

void GCanvas::fly_to(const BBox& b0) {
    if (size_.x < 4) { zoom_to(b0); return; }
    BBox b = b0;
    double w = std::max(b.w(), 0.5), h = std::max(b.h(), 0.5);
    a0s_ = scale_; a0x_ = cx_; a0y_ = cy_;
    a1s_ = std::clamp(std::min(size_.x * 0.8 / w, size_.y * 0.8 / h), 0.01, 50000.0);
    a1x_ = b.center().x; a1y_ = b.center().y;
    at_ = 0;
    anim_ = true;
    user_zoomed_ = true;
    request_redraw(30);
}

void GCanvas::zoom_by_at(double f, ImVec2 s) {
    Vec2 before = s2w(s);
    scale_ = std::clamp(scale_ * f, 0.01, 50000.0);
    Vec2 after = s2w(s);
    cx_ += before.x - after.x;
    cy_ += before.y - after.y;
    user_zoomed_ = true;
    anim_ = false;
    view_changed();
}

void GCanvas::view_changed() { view_moved_ = true; view_t_ = ImGui::GetTime(); request_redraw(2); }

void GCanvas::zoom_by(double f) { zoom_by_at(f, ImVec2(pos_.x + size_.x * 0.5f, pos_.y + size_.y * 0.5f)); }

void GCanvas::zoom_100() {
    scale_ = 96.0 / 25.4;
    user_zoomed_ = true;
    dirty_ = true;
}

void GCanvas::draw_grid(ID2D1DeviceContext* rt) {
    if (!grid_ || grid_mm_ <= 0) return;
    double step = grid_mm_;
    while (step * scale_ < 9) step *= 5;
    Vec2 a = s2w(pos_), b = s2w(ImVec2(pos_.x + size_.x, pos_.y + size_.y));
    double x0 = std::floor(std::min(a.x, b.x) / step) * step, x1 = std::max(a.x, b.x);
    double y0 = std::floor(std::min(a.y, b.y) / step) * step, y1 = std::max(a.y, b.y);
    if ((x1 - x0) / step > 800 || (y1 - y0) / step > 800) return;
    Com<ID2D1SolidColorBrush> br;
    rt->CreateSolidColorBrush(ui::d2c(ui::pal().grid), br.put());
    const float hair = 1.f / dpi_scale();
    for (double x = x0; x <= x1; x += step) {
        bool major = std::fmod(std::fabs(x) + step * 0.01, step * 5) < step * 0.02;
        br->SetColor(ui::d2c(major ? ui::pal().grid_major : ui::pal().grid));
        float sx = w2s({x, 0}).x - pos_.x;
        rt->DrawLine(D2D1::Point2F(sx, 0), D2D1::Point2F(sx, size_.y), br.get(), hair);
    }
    for (double y = y0; y <= y1; y += step) {
        bool major = std::fmod(std::fabs(y) + step * 0.01, step * 5) < step * 0.02;
        br->SetColor(ui::d2c(major ? ui::pal().grid_major : ui::pal().grid));
        float sy = w2s({0, y}).y - pos_.y;
        rt->DrawLine(D2D1::Point2F(0, sy), D2D1::Point2F(size_.x, sy), br.get(), hair);
    }
}

void GCanvas::render() {
    // R12 §6: render with a margin (each side kMargin of the viewport) so a pan / zoom-out between re-renders never
    // exposes uncovered (blank) area. During the render pos_/size_ describe the expanded texture so world_matrix(),
    // w2s() / s2w() and any viewport culling in subclasses all agree with the texture.
    const ImVec2 vpos = pos_, vsize = size_;
    const float mx = std::floor(vsize.x * kMargin), my = std::floor(vsize.y * kMargin);
    pos_ = ImVec2(vpos.x - mx, vpos.y - my);
    size_ = ImVec2(vsize.x + 2 * mx, vsize.y + 2 * my);
    tex_.ensure((int)size_.x, (int)size_.y);
    ID2D1DeviceContext* rt = tex_.begin(1.0f);
    if (rt) {
        rt->Clear(ui::d2c(background()));
        draw_grid(rt);
        rt->SetTransform(world_matrix());
        paint_world(rt);
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        paint_overlay_d2d(rt);
        tex_.end();
        ++renders_;
    }
    pos_ = vpos; size_ = vsize;
    if (!rt) return;
    dirty_ = false;
    view_moved_ = false;
    r_scale_ = scale_; r_cx_ = cx_; r_cy_ = cy_; r_mirror_ = mirror_;
    r_w_ = (float)tex_.width(); r_h_ = (float)tex_.height();
}

bool GCanvas::cache_covers_view() const {
    if (!tex_.valid() || r_mirror_ != mirror_) return false;
    const vcache::View c{r_scale_, r_cx_, r_cy_, r_w_, r_h_}, v{scale_, cx_, cy_, size_.x, size_.y};
    return vcache::covers(c, v, kZoomOut, kZoomIn);
}

bool GCanvas::draw(const char* id, ImVec2 size) {
    ImGuiIO& io = ImGui::GetIO();
    pos_ = ImGui::GetCursorScreenPos();
    size.x = std::max(size.x, 8.f);
    size.y = std::max(size.y, 8.f);
    if (size.x != size_.x || size.y != size_.y) {
        size_ = size;
        dirty_ = true;
        if (!user_zoomed_ || first_) { first_ = false; zoom_all(); }
    }
    if (first_) { first_ = false; zoom_all(); }
    if (anim_) {
        at_ = std::min(1.0, at_ + io.DeltaTime * 3.5);
        double t = 1 - std::pow(1 - at_, 3);
        double ls = std::log(a0s_) + (std::log(a1s_) - std::log(a0s_)) * t;
        scale_ = std::exp(ls);
        cx_ = a0x_ + (a1x_ - a0x_) * t;
        cy_ = a0y_ + (a1y_ - a0y_) * t;
        view_changed();
        if (at_ >= 1) { anim_ = false; dirty_ = true; }
        request_redraw(2);
    }
    ImGui::InvisibleButton(id, size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    hovered_ = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    int mods = (io.KeyShift ? KM_SHIFT : 0) | (io.KeyCtrl ? KM_CTRL : 0) | (io.KeyAlt ? KM_ALT : 0);
    ImVec2 mp = io.MousePos;
    if (hovered_ || active) {
        Vec2 w = s2w(mp);
        if (w.x != mouse_w_.x || w.y != mouse_w_.y) { mouse_w_ = w; on_mouse_move(w); }
    }
    // wheel
    if (hovered_ && io.MouseWheel != 0) {
        if (io.KeyShift) { cx_ -= (mirror_ ? -1 : 1) * io.MouseWheel * 60 / scale_; view_changed(); user_zoomed_ = true; }
        else zoom_by_at(std::pow(1.25, io.MouseWheel), mp);
    }
    const bool space = (ImGui::IsKeyDown(ImGuiKey_Space) && hovered_ && !io.KeyCtrl) || (pan_mode_ && !zoom_rect_mode_);   // R12: Ctrl+Space = maximize, not pan; R17c: Pan tool
    // pan with middle or space+left
    if ((active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0)) || (active && space && ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
        ImVec2 d = io.MouseDelta;
        if (d.x != 0 || d.y != 0) {
            cx_ -= (mirror_ ? -1 : 1) * d.x / scale_;
            cy_ += d.y / scale_;
            view_changed();
            user_zoomed_ = true;
            anim_ = false;
        }
        drag_ = Drag::Pan;
    } else if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !space && !hint_hit(mp)) {   // a click on a tutorial hint's x is not a tool click
        down_s_ = mp;
        down_w_ = s2w(mp);
        down_mods_ = mods;
        if (zoom_rect_mode_) drag_ = Drag::ZoomRect;
        else if (hit_drag_target(down_w_, mods)) drag_ = Drag::Move;
        else drag_ = Drag::Pending;
    }
    if (drag_ == Drag::Pending && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4) && wants_rubber_band()) drag_ = Drag::Band;
    if (drag_ == Drag::Move && ImGui::IsMouseDown(ImGuiMouseButton_Left)) { on_drag(down_w_, s2w(mp), down_mods_, false); dirty_ = true; }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && drag_ != Drag::None) {
        Vec2 up = s2w(mp);
        BBox b; b.add(down_w_); b.add(up);
        switch (drag_) {
        case Drag::ZoomRect: zoom_rect_mode_ = false; if (b.w() > 0 && b.h() > 0) { zoom_to(b, 0.02); user_zoomed_ = true; } break;
        case Drag::Band: on_rect(b, down_mods_); break;
        case Drag::Move: on_drag(down_w_, up, down_mods_, true); break;
        case Drag::Pending: on_click(up, down_mods_); break;
        default: break;
        }
        drag_ = Drag::None;
        dirty_ = true;
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Middle) && drag_ == Drag::Pan) drag_ = Drag::None;
    if (drag_ == Drag::Pan && !ImGui::IsMouseDown(ImGuiMouseButton_Middle) && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) drag_ = Drag::None;
    if (hovered_ && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) on_dblclick(s2w(mp), mods);
    if (hovered_ && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).x * ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).x < 16)
        on_context(s2w(mp));
    // keys
    if (hovered_ && !io.WantTextInput && !keyboard_claimed()) {   // P1-09: a focused list owns the arrow keys
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {   // R24a: Esc first leaves Pan / Zoom-rectangle (back to selecting)
            if (pan_mode_ || zoom_rect_mode_) { pan_mode_ = zoom_rect_mode_ = false; }
            else on_key(ImGuiKey_Escape, mods);
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_Home)) zoom_all();
        else if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) zoom_by(1.3);
        else if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) zoom_by(1 / 1.3);
        else {
            const double step = 80 / scale_;
            bool used = false;
            for (ImGuiKey k : {ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_Delete, ImGuiKey_R,
                               ImGuiKey_F, ImGuiKey_Space, ImGuiKey_Enter, ImGuiKey_Tab, ImGuiKey_A, ImGuiKey_M, ImGuiKey_T, ImGuiKey_B, ImGuiKey_P}) {
                if (!ImGui::IsKeyPressed(k)) continue;
                if (on_key(k, mods)) { used = true; continue; }
                if (k == ImGuiKey_LeftArrow) { cx_ -= (mirror_ ? -1 : 1) * step; view_changed(); used = true; }
                if (k == ImGuiKey_RightArrow) { cx_ += (mirror_ ? -1 : 1) * step; view_changed(); used = true; }
                if (k == ImGuiKey_UpArrow) { cy_ += step; view_changed(); used = true; }
                if (k == ImGuiKey_DownArrow) { cy_ -= step; view_changed(); used = true; }
                if (k == ImGuiKey_F) { zoom_all(); used = true; }
            }
            (void)used;
        }
    }
    if (hovered_) ImGui::SetMouseCursor(space || drag_ == Drag::Pan ? ImGuiMouseCursor_ResizeAll : cursor());

    static const bool no_cache = _wgetenv(L"PCBP_NO_VIEW_CACHE") != nullptr;   // A/B: re-render on every pan / zoom frame
    if (!dirty_ && view_moved_ && no_cache) dirty_ = true;
    if (!dirty_ && view_moved_) {
        // R12 §6: the cached texture is shown transformed only while it still covers the whole viewport at a near
        // scale; otherwise re-render synchronously this frame (never a blank or popping region). Settle re-render
        // once input stops gives full quality.
        const bool interacting = drag_ == Drag::Pan || ImGui::GetTime() - view_t_ < 0.18;
        if (!interacting || !cache_covers_view()) dirty_ = true;
        else request_redraw(2);
    }
    // R12 §6/§21: PCBP_CANVAS_PROF=<file> logs one line per frame while the view moves: render ms (0 = cached blit)
    static FILE* prof = nullptr; static bool prof_chk = false;
    if (!prof_chk) { prof_chk = true; if (const wchar_t* lp = _wgetenv(L"PCBP_CANVAS_PROF")) prof = _wfopen(lp, L"a"); }
    const bool moving = view_moved_;
    LARGE_INTEGER q0{}, q1{}, qf{};
    if (prof) QueryPerformanceCounter(&q0);
    const bool did_render = dirty_;
    if (dirty_) render();
    if (prof && (moving || did_render)) {
        QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&qf);
        std::fprintf(prof, "%.3f %d %.0fx%.0f %.4g\n", did_render ? (q1.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart : 0.0, did_render ? 1 : 0, size_.x, size_.y, scale_);
        std::fflush(prof);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (tex_.valid()) {
        // texture pixel (tx,ty) shows the world point under the render-time (expanded) view; place its corners with
        // the current view. Exact pixel mapping when the view has not moved.
        auto old_s2w = [&](float tx, float ty) {
            double dx = (tx - r_w_ * 0.5) / r_scale_; if (r_mirror_) dx = -dx;
            return Vec2{r_cx_ + dx, r_cy_ - (ty - r_h_ * 0.5) / r_scale_};
        };
        const ImVec2 a = w2s(old_s2w(0, 0)), b = w2s(old_s2w(r_w_, r_h_));
        dl->PushClipRect(pos_, ImVec2(pos_.x + size_.x, pos_.y + size_.y), true);
        if (view_moved_) {
            const uint32_t bg = background();
            dl->AddRectFilled(pos_, ImVec2(pos_.x + size_.x, pos_.y + size_.y), IM_COL32((bg >> 16) & 255, (bg >> 8) & 255, bg & 255, 255));
        }
        dl->AddImage((ImTextureID)tex_.imgui_id(), ImVec2(std::min(a.x, b.x), a.y), ImVec2(std::max(a.x, b.x), b.y),
                     ImVec2(a.x <= b.x ? 0.f : 1.f, 0), ImVec2(a.x <= b.x ? 1.f : 0.f, 1));
        dl->PopClipRect();
    }
    dl->PushClipRect(pos_, ImVec2(pos_.x + size_.x, pos_.y + size_.y), true);
    if (drag_ == Drag::Band || drag_ == Drag::ZoomRect) {
        ImVec2 a = down_s_, b = mp;
        ImVec2 mn(std::min(a.x, b.x), std::min(a.y, b.y)), mx(std::max(a.x, b.x), std::max(a.y, b.y));
        dl->AddRectFilled(mn, mx, (th().accent & 0x00FFFFFF) | 0x28000000);
        dl->AddRect(mn, mx, th().accent, 0, 0, 1.5f);
    }
    paint_overlay(dl);
    dl->PopClipRect();
    return hovered_;
}

} // namespace pcbp::gui
