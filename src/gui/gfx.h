#pragma once
// ============================================================================
// gfx.h — D3D11 device + swap chain, Dear ImGui renderer, Direct2D interop.
//
// Canvases (board plotter, Gerber, package editor, visual aid) draw with
// Direct2D into GPU textures (RenderTexture) that ImGui shows as images, so
// the existing D2D renderers are reused and redraws only happen when dirty.
// All OS components: d3d11, dxgi, d2d1, dwrite, d3dcompiler_47 (runtime-loaded).
// ============================================================================
#include "../ui/win.h"
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>

struct ImDrawData;

namespace pcbp::gui {
using ui::Com;

bool gfx_init(HWND hwnd, std::string& err);
void gfx_shutdown();
void gfx_resize(UINT w, UINT h);
void gfx_begin_frame(float r, float g, float b);
void gfx_render_imgui(ImDrawData* dd);
void gfx_present(bool vsync);
bool gfx_font_texture_rebuild();
// Frosted glass (R11, ported from MPP's imgui_soft glass): popups, menus, tooltips and modal dialogs get a blurred
// copy of what is behind them (half-res separable gaussian, luminance capped for text contrast), a rounded clip and a
// soft drop shadow; their PopupBg is translucent (gtheme.cpp). Off = solid (Performance mode).
void gfx_set_glass(bool on, bool dark);
bool gfx_glass();            // after ImGui fonts change
ID3D11Device* gfx_device();
ID3D11DeviceContext* gfx_context();
ID2D1DeviceContext* gfx_d2d();              // shared D2D device context (one target at a time)
ID2D1Factory1* gfx_d2d_factory();

// An offscreen B8G8R8A8 texture usable as a D2D target and as an ImGui image.
class RenderTexture {
public:
    ~RenderTexture() { release(); }
    bool ensure(int w_px, int h_px);        // (re)create when size changes; returns true if recreated
    void release();
    // begin D2D drawing into this texture; dpi_scale maps DIPs -> pixels
    ID2D1DeviceContext* begin(float dpi_scale);
    void end();
    void* imgui_id() const { return (void*)srv_.get(); }
    int width() const { return w_; }
    int height() const { return h_; }
    bool valid() const { return (bool)srv_; }
private:
    Com<ID3D11Texture2D> tex_;
    Com<ID3D11ShaderResourceView> srv_;
    Com<ID2D1Bitmap1> bmp_;
    int w_ = 0, h_ = 0;
};

// Uploads a BGRA (premultiplied) bitmap to a static texture (icon atlas, pictures).
class StaticTexture {
public:
    bool create(int w, int h, const void* bgra, int pitch);
    void release() { srv_.reset(); tex_.reset(); }
    void* imgui_id() const { return (void*)srv_.get(); }
    int w = 0, h = 0;
private:
    Com<ID3D11Texture2D> tex_;
    Com<ID3D11ShaderResourceView> srv_;
};

} // namespace pcbp::gui
