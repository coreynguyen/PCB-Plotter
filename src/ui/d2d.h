#pragma once
// ============================================================================
// d2d.h — Direct2D / DirectWrite helpers.
// ============================================================================
#include "win.h"
#include <map>

namespace pcbp::ui {

ID2D1Factory* d2d_factory();
IDWriteFactory* dwrite_factory();
const wchar_t* resolve_face(const wchar_t* face);

// Per-window HWND render target with lazily created resources.
class D2DSurface {
public:
    bool begin(HWND h);                 // creates target if needed, BeginDraw
    void end();                         // EndDraw, handles device loss
    void resize(UINT w, UINT h);
    void discard();
    ID2D1RenderTarget* rt() const { return rt_.get(); }
    ID2D1SolidColorBrush* brush(uint32_t argb, float alpha = 1.0f);
    IDWriteTextFormat* text(float size_dip, bool bold = false, bool center = false, const wchar_t* face = L"Segoe UI");
    float dpi_scale() const { return dpi_ / 96.f; }
private:
    Com<ID2D1HwndRenderTarget> rt_;
    Com<ID2D1SolidColorBrush> brush_;
    std::map<unsigned long long, Com<IDWriteTextFormat>> fmts_;
    HWND hwnd_ = nullptr;
    float dpi_ = 96;
};

IDWriteTextFormat* shared_text_format(float size_dip, bool bold = false, int align = 0 /*0 left,1 centre,2 right*/,
                                      const wchar_t* face = L"Segoe UI");

void draw_text(ID2D1RenderTarget* rt, const std::wstring& s, const D2D1_RECT_F& r, IDWriteTextFormat* f, ID2D1Brush* b);
D2D1_SIZE_F measure_text(const std::wstring& s, IDWriteTextFormat* f, float max_w = 10000.f);

// rounded rectangle convenience
void fill_round(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, float rad, ID2D1Brush* b);
void stroke_round(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, float rad, ID2D1Brush* b, float w = 1.f);

// Render arbitrary D2D drawing into a 32-bpp premultiplied HBITMAP (menu icons, cursors, .ico)
template <class F>
HBITMAP render_to_hbitmap(int w, int h, F&& draw);

HBITMAP render_to_hbitmap_impl(int w, int h, void (*cb)(ID2D1RenderTarget*, void*), void* ctx);
template <class F>
HBITMAP render_to_hbitmap(int w, int h, F&& draw) {
    return render_to_hbitmap_impl(w, h, [](ID2D1RenderTarget* rt, void* c) { (*(F*)c)(rt); }, (void*)&draw);
}

} // namespace pcbp::ui
