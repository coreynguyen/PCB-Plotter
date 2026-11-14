// ============================================================================
// d2d.cpp — Direct2D / DirectWrite helpers.
// ============================================================================
#include "d2d.h"
#include <d2d1_1.h>
#include <cstring>
#include <algorithm>

namespace pcbp::ui {

ID2D1Factory* d2d_factory() {
    static Com<ID2D1Factory> f;
    if (!f) {
        D2D1_FACTORY_OPTIONS o{};
        // ID2D1Factory1 so the same factory can create device contexts for D3D11 interop (gui/gfx.cpp)
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &o, (void**)f.put())))
            D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &o, (void**)f.put());
    }
    return f.get();
}

IDWriteFactory* dwrite_factory() {
    static Com<IDWriteFactory> f;
    if (!f) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)f.put());
    return f.get();
}

bool D2DSurface::begin(HWND h) {
    hwnd_ = h;
    RECT rc;
    GetClientRect(h, &rc);
    if (!rt_) {
        dpi_ = (float)window_dpi(h);
        D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN), dpi_, dpi_);
        D2D1_HWND_RENDER_TARGET_PROPERTIES hp = D2D1::HwndRenderTargetProperties(
            h, D2D1::SizeU((UINT32)std::max<LONG>(1, rc.right), (UINT32)std::max<LONG>(1, rc.bottom)));
        if (FAILED(d2d_factory()->CreateHwndRenderTarget(rp, hp, rt_.put()))) return false;
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        brush_.reset();
    }
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    return true;
}

void D2DSurface::end() {
    if (!rt_) return;
    HRESULT hr = rt_->EndDraw();
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) discard();
}

void D2DSurface::resize(UINT w, UINT h) {
    if (rt_) rt_->Resize(D2D1::SizeU(std::max(1u, w), std::max(1u, h)));
}

void D2DSurface::discard() {
    brush_.reset();
    rt_.reset();
}

ID2D1SolidColorBrush* D2DSurface::brush(uint32_t argb, float alpha) {
    if (!rt_) return nullptr;
    if (!brush_) rt_->CreateSolidColorBrush(d2c(argb, alpha), brush_.put());
    else brush_->SetColor(d2c(argb, alpha));
    return brush_.get();
}

// Resolve a font family that is actually installed. Without this, a missing face (e.g. Segoe UI under Wine)
// falls back with mismatched advances and letter spacing comes out broken ("Contr d B oard").
const wchar_t* resolve_face(const wchar_t* face) {
    static std::map<std::wstring, const wchar_t*> memo;
    auto it = memo.find(face);
    if (it != memo.end()) return it->second;
    const wchar_t* pick = face;
    Com<IDWriteFontCollection> fc;
    if (dwrite_factory() && SUCCEEDED(dwrite_factory()->GetSystemFontCollection(fc.put(), FALSE)) && fc) {
        auto has = [&](const wchar_t* n) { UINT32 i = 0; BOOL ok = FALSE; return SUCCEEDED(fc->FindFamilyName(n, &i, &ok)) && ok; };
        if (!has(face)) {
            static const wchar_t* fallbacks[] = {L"Segoe UI", L"Tahoma", L"Arial", L"DejaVu Sans", L"Liberation Sans"};
            for (const wchar_t* f : fallbacks) if (has(f)) { pick = f; break; }
        }
    }
    memo[face] = pick;
    return pick;
}

IDWriteTextFormat* shared_text_format(float size, bool bold, int align, const wchar_t* face) {
    face = resolve_face(face);
    static std::map<std::wstring, Com<IDWriteTextFormat>> cache;
    std::wstring key = std::wstring(face) + L"|" + std::to_wstring((int)(size * 10)) + (bold ? L"b" : L"n") + std::to_wstring(align);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second.get();
    Com<IDWriteTextFormat> f;
    dwrite_factory()->CreateTextFormat(face, nullptr, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                       DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", f.put());
    if (f) {
        f->SetTextAlignment(align == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER : align == 2 ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING tr{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        Com<IDWriteInlineObject> ell;
        dwrite_factory()->CreateEllipsisTrimmingSign(f.get(), ell.put());
        f->SetTrimming(&tr, ell.get());
    }
    IDWriteTextFormat* raw = f.get();
    cache[key] = f;
    return raw;
}

IDWriteTextFormat* D2DSurface::text(float size, bool bold, bool center, const wchar_t* face) {
    return shared_text_format(size, bold, center ? 1 : 0, face);
}

void draw_text(ID2D1RenderTarget* rt, const std::wstring& s, const D2D1_RECT_F& r, IDWriteTextFormat* f, ID2D1Brush* b) {
    if (!f || !b || s.empty()) return;
    rt->DrawText(s.c_str(), (UINT32)s.size(), f, r, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

D2D1_SIZE_F measure_text(const std::wstring& s, IDWriteTextFormat* f, float max_w) {
    Com<IDWriteTextLayout> lay;
    if (!f || FAILED(dwrite_factory()->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, max_w, 1000.f, lay.put())))
        return {0, 0};
    DWRITE_TEXT_METRICS m{};
    lay->GetMetrics(&m);
    return {m.widthIncludingTrailingWhitespace, m.height};
}

void fill_round(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, float rad, ID2D1Brush* b) {
    rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), b);
}
void stroke_round(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, float rad, ID2D1Brush* b, float w) {
    rt->DrawRoundedRectangle(D2D1::RoundedRect(r, rad, rad), b, w);
}

HBITMAP render_to_hbitmap_impl(int w, int h, void (*cb)(ID2D1RenderTarget*, void*), void* ctx) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!bmp) return nullptr;
    std::memset(bits, 0, (size_t)w * h * 4);
    HDC mem = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(mem, bmp);
    D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    Com<ID2D1DCRenderTarget> rt;
    if (SUCCEEDED(d2d_factory()->CreateDCRenderTarget(&rp, rt.put()))) {
        RECT r{0, 0, w, h};
        rt->BindDC(mem, &r);
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        cb(rt.get(), ctx);
        rt->EndDraw();
    }
    SelectObject(mem, old);
    DeleteDC(mem);
    return bmp;
}

} // namespace pcbp::ui
