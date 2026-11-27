// ============================================================================
// gfx.cpp — D3D11 + ImGui renderer + D2D interop (written for PCB Plotter).
// ============================================================================
#include "gfx.h"
#include "../ui/d2d.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>     // std::floor / std::ceil (MSVC 14.51 no longer pulls it in through <algorithm>)
#include <vector>
#include <cstring>

namespace pcbp::gui {

namespace {
Com<ID3D11Device> g_dev;
Com<ID3D11DeviceContext> g_ctx;
Com<IDXGISwapChain> g_swap;
Com<ID3D11RenderTargetView> g_rtv;
Com<ID2D1Device> g_d2d_dev;
Com<ID2D1DeviceContext> g_d2d;
// imgui pipeline
Com<ID3D11VertexShader> g_vs;
Com<ID3D11PixelShader> g_ps;
Com<ID3D11InputLayout> g_layout;
Com<ID3D11Buffer> g_cb, g_vb, g_ib;
int g_vb_size = 0, g_ib_size = 0;
Com<ID3D11SamplerState> g_sampler;
Com<ID3D11BlendState> g_blend;
Com<ID3D11RasterizerState> g_raster;
Com<ID3D11DepthStencilState> g_depth;
Com<ID3D11ShaderResourceView> g_font_srv;
UINT g_w = 0, g_h = 0;
// ---- frosted glass
bool g_glass_on = true, g_glass_dark = true;
Com<ID3D11VertexShader> g_gvs;
Com<ID3D11PixelShader> g_gps_blur, g_gps_comp, g_gps_shadow;
Com<ID3D11Buffer> g_gcb;
Com<ID3D11BlendState> g_noblend;
Com<ID3D11Texture2D> g_copy, g_half[2];
Com<ID3D11ShaderResourceView> g_copy_srv, g_half_srv[2];
Com<ID3D11RenderTargetView> g_half_rtv[2];
UINT g_gw = 0, g_gh = 0;
bool g_glass_ok = false;
struct GlassCB { float rect[4]; float src[4]; float p[4]; float dir[4]; };
const char* kGVS = R"(
cbuffer g : register(b0) { float4 rect; float4 src; float4 p; float4 dir; };
float4 main(uint id : SV_VertexID) : SV_POSITION {
    float2 t = float2(id & 1, id >> 1);
    float2 px = lerp(rect.xy, rect.zw, t);              // target pixels
    return float4(px.x * p.z * 2 - 1, 1 - px.y * p.w * 2, 0, 1);   // p.zw = 1 / target size
}
)";
// blur: 13 taps along dir (source pixels), source uv = target px * src.z (scale) / src size
const char* kGPSBlur = R"(
cbuffer g : register(b0) { float4 rect; float4 src; float4 p; float4 dir; };
sampler s0; Texture2D t0;
float4 main(float4 pos : SV_POSITION) : SV_Target {
    float2 uv = pos.xy * src.z * src.xy;                  // src.xy = 1 / source size
    static const float w[7] = {0.1597, 0.1471, 0.1149, 0.0761, 0.0428, 0.0204, 0.0083};
    float3 c = t0.Sample(s0, uv).rgb * w[0];
    [unroll] for (int i = 1; i < 7; ++i) {
        float2 o = dir.xy * src.xy * (float)i;
        c += (t0.Sample(s0, uv + o).rgb + t0.Sample(s0, uv - o).rgb) * w[i];
    }
    return float4(c / 0.9783, 1);
}
)";
const char* kGPSComp = R"(
cbuffer g : register(b0) { float4 rect; float4 src; float4 p; float4 dir; };
sampler s0; Texture2D t0;
float sdr(float2 q, float4 r, float rad) { float2 c = (r.xy + r.zw) * 0.5, h = (r.zw - r.xy) * 0.5 - rad; float2 d = abs(q - c) - h; return length(max(d, 0)) + min(max(d.x, d.y), 0) - rad; }
float4 main(float4 pos : SV_POSITION) : SV_Target {
    float3 c = t0.Sample(s0, pos.xy * 0.5 * src.xy).rgb;  // half-res blur, bilinear upsample
    float L = dot(c, float3(0.25, 0.625, 0.125));
    float cap = dir.z;
    if (dir.w > 0.5) { if (L > cap) c *= cap / max(L, 1e-4); }          // dark theme: cap backdrop luminance
    else if (L < cap) c = lerp(c, 1.0.xxx, (cap - L) / max(1 - L, 1e-4)); // light theme: floor it
    float a = saturate(0.5 - sdr(pos.xy, rect, p.x));
    return float4(c, a);
}
)";
const char* kGPSShadow = R"(
cbuffer g : register(b0) { float4 rect; float4 src; float4 p; float4 dir; };
float sdr(float2 q, float4 r, float rad) { float2 c = (r.xy + r.zw) * 0.5, h = (r.zw - r.xy) * 0.5 - rad; float2 d = abs(q - c) - h; return length(max(d, 0)) + min(max(d.x, d.y), 0) - rad; }
float4 main(float4 pos : SV_POSITION) : SV_Target {
    // src = window rect; p.x radius, p.y shadow reach, dir.x peak alpha, dir.y offset down
    float4 sr = src + float4(0, dir.y, 0, dir.y);
    float d = sdr(pos.xy, sr, p.x + 2);
    float f = saturate(1 - d / p.y);
    float inside = saturate(0.5 - sdr(pos.xy, src, p.x));
    return float4(0, 0, 0, dir.x * f * f * (1 - inside));
}
)";

const char* kVS = R"(
cbuffer vb : register(b0) { float4x4 P; };
struct VI { float2 pos : POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
struct PI { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
PI main(VI i) { PI o; o.pos = mul(P, float4(i.pos, 0, 1)); o.col = i.col; o.uv = i.uv; return o; }
)";
const char* kPS = R"(
struct PI { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
sampler s0; Texture2D t0;
float4 main(PI i) : SV_Target { return i.col * t0.Sample(s0, i.uv); }
)";

typedef HRESULT(WINAPI* fnD3DCompile)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

bool compile(const char* src, const char* target, Com<ID3DBlob>& out, std::string& err) {
    static fnD3DCompile cmp = nullptr;
    if (!cmp) {
        const wchar_t* dlls[] = {L"d3dcompiler_47.dll", L"d3dcompiler_46.dll", L"d3dcompiler_43.dll"};
        for (auto* n : dlls) {
            if (HMODULE m = LoadLibraryW(n)) { cmp = (fnD3DCompile)GetProcAddress(m, "D3DCompile"); if (cmp) break; }
        }
        if (!cmp) { err = "d3dcompiler_47.dll not found"; return false; }
    }
    Com<ID3DBlob> errs;
    HRESULT hr = cmp(src, strlen(src), nullptr, nullptr, nullptr, "main", target, 0, 0, out.put(), errs.put());
    if (FAILED(hr)) { err = errs ? std::string((const char*)errs->GetBufferPointer(), errs->GetBufferSize()) : "shader compile failed"; return false; }
    return true;
}

bool create_rtv() {
    Com<ID3D11Texture2D> back;
    if (FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)back.put()))) return false;
    return SUCCEEDED(g_dev->CreateRenderTargetView(back.get(), nullptr, g_rtv.put()));
}

bool create_pipeline(std::string& err) {
    Com<ID3DBlob> vsb, psb;
    if (!compile(kVS, "vs_4_0", vsb, err) || !compile(kPS, "ps_4_0", psb, err)) return false;
    if (FAILED(g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, g_vs.put()))) { err = "CreateVertexShader"; return false; }
    if (FAILED(g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, g_ps.put()))) { err = "CreatePixelShader"; return false; }
    D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, (UINT)offsetof(ImDrawVert, pos), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, (UINT)offsetof(ImDrawVert, uv), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, (UINT)offsetof(ImDrawVert, col), D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(g_dev->CreateInputLayout(il, 3, vsb->GetBufferPointer(), vsb->GetBufferSize(), g_layout.put()))) { err = "CreateInputLayout"; return false; }
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = 64; cb.Usage = D3D11_USAGE_DYNAMIC; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&cb, nullptr, g_cb.put());
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    g_dev->CreateSamplerState(&sd, g_sampler.put());
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_dev->CreateBlendState(&bd, g_blend.put());
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.ScissorEnable = TRUE; rd.DepthClipEnable = TRUE;
    g_dev->CreateRasterizerState(&rd, g_raster.put());
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = FALSE; dd.StencilEnable = FALSE;
    g_dev->CreateDepthStencilState(&dd, g_depth.put());
    return true;
}

bool create_glass(std::string& err) {
    Com<ID3DBlob> vb, b1, b2, b3;
    if (!compile(kGVS, "vs_4_0", vb, err) || !compile(kGPSBlur, "ps_4_0", b1, err) || !compile(kGPSComp, "ps_4_0", b2, err) || !compile(kGPSShadow, "ps_4_0", b3, err)) return false;
    if (FAILED(g_dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, g_gvs.put())) ||
        FAILED(g_dev->CreatePixelShader(b1->GetBufferPointer(), b1->GetBufferSize(), nullptr, g_gps_blur.put())) ||
        FAILED(g_dev->CreatePixelShader(b2->GetBufferPointer(), b2->GetBufferSize(), nullptr, g_gps_comp.put())) ||
        FAILED(g_dev->CreatePixelShader(b3->GetBufferPointer(), b3->GetBufferSize(), nullptr, g_gps_shadow.put()))) { err = "glass shaders"; return false; }
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(GlassCB); cb.Usage = D3D11_USAGE_DYNAMIC; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_dev->CreateBuffer(&cb, nullptr, g_gcb.put()))) return false;
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_dev->CreateBlendState(&bd, g_noblend.put());
    return true;
}

bool ensure_glass_targets() {
    if (g_copy && g_gw == g_w && g_gh == g_h) return true;
    g_copy.reset(); g_copy_srv.reset();
    for (int i = 0; i < 2; ++i) { g_half[i].reset(); g_half_srv[i].reset(); g_half_rtv[i].reset(); }
    if (!g_w || !g_h) return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = g_w; td.Height = g_h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, g_copy.put())) || FAILED(g_dev->CreateShaderResourceView(g_copy.get(), nullptr, g_copy_srv.put()))) return false;
    td.Width = (g_w + 1) / 2; td.Height = (g_h + 1) / 2; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    for (int i = 0; i < 2; ++i)
        if (FAILED(g_dev->CreateTexture2D(&td, nullptr, g_half[i].put())) || FAILED(g_dev->CreateShaderResourceView(g_half[i].get(), nullptr, g_half_srv[i].put())) ||
            FAILED(g_dev->CreateRenderTargetView(g_half[i].get(), nullptr, g_half_rtv[i].put()))) return false;
    g_gw = g_w; g_gh = g_h;
    return true;
}

void glass_cb(const GlassCB& c) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(g_gcb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &c, sizeof c); g_ctx->Unmap(g_gcb.get(), 0); }
}

// One frosted region: rect in backbuffer pixels. Everything already drawn is the backdrop.
void glass_pass(float x0, float y0, float x1, float y1, float rad, bool dialog) {
    if (!ensure_glass_targets()) return;
    const float reach = dialog ? 30.f : 22.f;          // blur reach (full-res px)
    const float W = (float)g_w, H = (float)g_h, hw = (float)((g_w + 1) / 2), hh = (float)((g_h + 1) / 2);
    auto clampx = [&](float v) { return std::clamp(v, 0.f, W); };
    auto clampy = [&](float v) { return std::clamp(v, 0.f, H); };
    // 1. copy the backdrop (region + 2 reach) out of the backbuffer
    Com<ID3D11Resource> back;
    ID3D11RenderTargetView* rtv = g_rtv.get();
    rtv->GetResource(back.put());
    D3D11_BOX box{(UINT)clampx(x0 - 2 * reach), (UINT)clampy(y0 - 2 * reach), 0, (UINT)clampx(x1 + 2 * reach), (UINT)clampy(y1 + 2 * reach), 1};
    if (box.right <= box.left || box.bottom <= box.top) return;
    g_ctx->CopySubresourceRegion(g_copy.get(), 0, box.left, box.top, 0, back.get(), 0, &box);
    ID3D11ShaderResourceView* null_srv = nullptr;
    g_ctx->PSSetShaderResources(0, 1, &null_srv);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    g_ctx->VSSetShader(g_gvs.get(), nullptr, 0);
    ID3D11Buffer* cb = g_gcb.get();
    g_ctx->VSSetConstantBuffers(0, 1, &cb);
    g_ctx->PSSetConstantBuffers(0, 1, &cb);
    ID3D11SamplerState* smp = g_sampler.get();
    g_ctx->PSSetSamplers(0, 1, &smp);
    const float bf[4] = {0, 0, 0, 0};
    // half-res region (+ reach)
    const float hx0 = std::max(0.f, std::floor((x0 - reach) * 0.5f)), hy0 = std::max(0.f, std::floor((y0 - reach) * 0.5f));
    const float hx1 = std::min(hw, std::ceil((x1 + reach) * 0.5f)), hy1 = std::min(hh, std::ceil((y1 + reach) * 0.5f));
    D3D11_VIEWPORT hvp{0, 0, hw, hh, 0, 1};
    D3D11_RECT hsc{(LONG)hx0, (LONG)hy0, (LONG)hx1, (LONG)hy1};
    g_ctx->RSSetViewports(1, &hvp);
    g_ctx->RSSetScissorRects(1, &hsc);
    g_ctx->OMSetBlendState(g_noblend.get(), bf, 0xffffffff);
    g_ctx->PSSetShader(g_gps_blur.get(), nullptr, 0);
    const float step = dialog ? 2.4f : 1.8f;           // tap spacing in source px
    // 2a. full -> half, horizontal
    GlassCB c{{hx0, hy0, hx1, hy1}, {1.f / W, 1.f / H, 2.f, 0}, {0, 0, 1.f / hw, 1.f / hh}, {step * 2, 0, 0, 0}};
    glass_cb(c);
    ID3D11RenderTargetView* t0 = g_half_rtv[0].get();
    g_ctx->OMSetRenderTargets(1, &t0, nullptr);
    ID3D11ShaderResourceView* s_copy = g_copy_srv.get();
    g_ctx->PSSetShaderResources(0, 1, &s_copy);
    g_ctx->Draw(4, 0);
    // 2b. half -> half, vertical
    g_ctx->PSSetShaderResources(0, 1, &null_srv);
    c.src[0] = 1.f / hw; c.src[1] = 1.f / hh; c.src[2] = 1.f; c.dir[0] = 0; c.dir[1] = step;
    glass_cb(c);
    ID3D11RenderTargetView* t1 = g_half_rtv[1].get();
    g_ctx->OMSetRenderTargets(1, &t1, nullptr);
    ID3D11ShaderResourceView* s_h0 = g_half_srv[0].get();
    g_ctx->PSSetShaderResources(0, 1, &s_h0);
    g_ctx->Draw(4, 0);
    // 2c. second H+V pair (box -> smooth gaussian falloff)
    g_ctx->PSSetShaderResources(0, 1, &null_srv);
    c.dir[0] = step; c.dir[1] = 0; glass_cb(c);
    g_ctx->OMSetRenderTargets(1, &t0, nullptr);
    ID3D11ShaderResourceView* s_h1 = g_half_srv[1].get();
    g_ctx->PSSetShaderResources(0, 1, &s_h1);
    g_ctx->Draw(4, 0);
    g_ctx->PSSetShaderResources(0, 1, &null_srv);
    c.dir[0] = 0; c.dir[1] = step; glass_cb(c);
    g_ctx->OMSetRenderTargets(1, &t1, nullptr);
    g_ctx->PSSetShaderResources(0, 1, &s_h0);
    g_ctx->Draw(4, 0);
    g_ctx->PSSetShaderResources(0, 1, &null_srv);
    // 3. back to the backbuffer: shadow outside the window, then the frosted backdrop inside its rounded shape
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{0, 0, W, H, 0, 1};
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->OMSetBlendState(g_blend.get(), bf, 0xffffffff);
    const float sh = dialog ? 22.f : 14.f, off = dialog ? 6.f : 4.f;
    D3D11_RECT sc{(LONG)clampx(x0 - sh), (LONG)clampy(y0 - sh), (LONG)clampx(x1 + sh), (LONG)clampy(y1 + sh + off)};
    g_ctx->RSSetScissorRects(1, &sc);
    GlassCB s2{{x0 - sh, y0 - sh, x1 + sh, y1 + sh + off}, {x0, y0, x1, y1}, {rad, sh, 1.f / W, 1.f / H}, {g_glass_dark ? 0.42f : 0.20f, off, 0, 0}};
    glass_cb(s2);
    g_ctx->PSSetShader(g_gps_shadow.get(), nullptr, 0);
    g_ctx->Draw(4, 0);
    D3D11_RECT sc2{(LONG)clampx(x0), (LONG)clampy(y0), (LONG)clampx(x1), (LONG)clampy(y1)};
    g_ctx->RSSetScissorRects(1, &sc2);
    const float cap = g_glass_dark ? (dialog ? 0.30f : 0.24f) : (dialog ? 0.80f : 0.84f);
    GlassCB s3{{x0, y0, x1, y1}, {1.f / hw, 1.f / hh, 0, 0}, {rad, 0, 1.f / W, 1.f / H}, {0, 0, cap, g_glass_dark ? 1.f : 0.f}};
    glass_cb(s3);
    g_ctx->PSSetShader(g_gps_comp.get(), nullptr, 0);
    ID3D11ShaderResourceView* s_fin = g_half_srv[1].get();
    g_ctx->PSSetShaderResources(0, 1, &s_fin);
    g_ctx->Draw(4, 0);
    g_ctx->PSSetShaderResources(0, 1, &null_srv);
}
} // namespace

void gfx_set_glass(bool on, bool dark) { g_glass_on = on; g_glass_dark = dark; }
bool gfx_glass() { return g_glass_on && g_glass_ok; }

ID3D11Device* gfx_device() { return g_dev.get(); }
ID3D11DeviceContext* gfx_context() { return g_ctx.get(); }
ID2D1DeviceContext* gfx_d2d() { return g_d2d.get(); }
ID2D1Factory1* gfx_d2d_factory() { return (ID2D1Factory1*)ui::d2d_factory(); }

bool gfx_init(HWND hwnd, std::string& err) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 3, D3D11_SDK_VERSION,
                                               &sd, g_swap.put(), g_dev.put(), &got, g_ctx.put());
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 3, D3D11_SDK_VERSION,
                                           &sd, g_swap.put(), g_dev.put(), &got, g_ctx.put());
    if (FAILED(hr)) { err = "D3D11 device creation failed"; return false; }
    if (!create_rtv()) { err = "swap chain render target"; return false; }
    { DXGI_SWAP_CHAIN_DESC d{}; g_swap->GetDesc(&d); g_w = d.BufferDesc.Width; g_h = d.BufferDesc.Height; }
    Com<IDXGIDevice> dxgi;
    g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)dxgi.put());
    ID2D1Factory1* f = gfx_d2d_factory();
    if (!f || !dxgi || FAILED(f->CreateDevice(dxgi.get(), g_d2d_dev.put())) ||
        FAILED(g_d2d_dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, g_d2d.put()))) {
        err = "Direct2D device context unavailable";
        return false;
    }
    g_d2d->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (!create_pipeline(err)) return false;
    std::string gerr;
    g_glass_ok = create_glass(gerr);   // optional: solid popups when the glass shaders are unavailable
    return true;
}

void gfx_shutdown() {
    g_font_srv.reset(); g_vb.reset(); g_ib.reset(); g_cb.reset(); g_layout.reset(); g_vs.reset(); g_ps.reset();
    g_gvs.reset(); g_gps_blur.reset(); g_gps_comp.reset(); g_gps_shadow.reset(); g_gcb.reset(); g_noblend.reset();
    g_copy_srv.reset(); g_copy.reset(); for (int i = 0; i < 2; ++i) { g_half_rtv[i].reset(); g_half_srv[i].reset(); g_half[i].reset(); }
    g_sampler.reset(); g_blend.reset(); g_raster.reset(); g_depth.reset();
    g_d2d.reset(); g_d2d_dev.reset(); g_rtv.reset(); g_swap.reset(); g_ctx.reset(); g_dev.reset();
}

void gfx_resize(UINT w, UINT h) {
    if (!g_swap || w == 0 || h == 0 || (w == g_w && h == g_h)) return;
    g_w = w; g_h = h;
    g_rtv.reset();
    g_swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
    create_rtv();
}

void gfx_begin_frame(float r, float g, float b) {
    if (!g_rtv) return;
    const float c[4] = {r, g, b, 1};
    ID3D11RenderTargetView* rtv = g_rtv.get();
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g_ctx->ClearRenderTargetView(rtv, c);
}

bool gfx_font_texture_rebuild() {
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* px; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sub{px, (UINT)w * 4, 0};
    Com<ID3D11Texture2D> tex;
    if (FAILED(g_dev->CreateTexture2D(&td, &sub, tex.put()))) return false;
    g_font_srv.reset();
    if (FAILED(g_dev->CreateShaderResourceView(tex.get(), nullptr, g_font_srv.put()))) return false;
    io.Fonts->SetTexID((ImTextureID)g_font_srv.get());
    return true;
}

void gfx_render_imgui(ImDrawData* dd) {
    if (!dd || dd->DisplaySize.x <= 0 || dd->DisplaySize.y <= 0 || !g_rtv) return;
    if (!g_vb || g_vb_size < dd->TotalVtxCount) {
        g_vb.reset();
        g_vb_size = dd->TotalVtxCount + 8192;
        D3D11_BUFFER_DESC d{};
        d.Usage = D3D11_USAGE_DYNAMIC; d.ByteWidth = (UINT)(g_vb_size * sizeof(ImDrawVert)); d.BindFlags = D3D11_BIND_VERTEX_BUFFER; d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_dev->CreateBuffer(&d, nullptr, g_vb.put()))) return;
    }
    if (!g_ib || g_ib_size < dd->TotalIdxCount) {
        g_ib.reset();
        g_ib_size = dd->TotalIdxCount + 16384;
        D3D11_BUFFER_DESC d{};
        d.Usage = D3D11_USAGE_DYNAMIC; d.ByteWidth = (UINT)(g_ib_size * sizeof(ImDrawIdx)); d.BindFlags = D3D11_BIND_INDEX_BUFFER; d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_dev->CreateBuffer(&d, nullptr, g_ib.put()))) return;
    }
    D3D11_MAPPED_SUBRESOURCE vm, im;
    if (FAILED(g_ctx->Map(g_vb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &vm))) return;
    if (FAILED(g_ctx->Map(g_ib.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &im))) { g_ctx->Unmap(g_vb.get(), 0); return; }
    ImDrawVert* vd = (ImDrawVert*)vm.pData;
    ImDrawIdx* id = (ImDrawIdx*)im.pData;
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* cl = dd->CmdLists[n];
        memcpy(vd, cl->VtxBuffer.Data, cl->VtxBuffer.Size * sizeof(ImDrawVert));
        memcpy(id, cl->IdxBuffer.Data, cl->IdxBuffer.Size * sizeof(ImDrawIdx));
        vd += cl->VtxBuffer.Size;
        id += cl->IdxBuffer.Size;
    }
    g_ctx->Unmap(g_vb.get(), 0);
    g_ctx->Unmap(g_ib.get(), 0);
    D3D11_MAPPED_SUBRESOURCE cm;
    if (SUCCEEDED(g_ctx->Map(g_cb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) {
        float L = dd->DisplayPos.x, R = dd->DisplayPos.x + dd->DisplaySize.x, T = dd->DisplayPos.y, B = dd->DisplayPos.y + dd->DisplaySize.y;
        float m[4][4] = {{2.0f / (R - L), 0, 0, 0}, {0, 2.0f / (T - B), 0, 0}, {0, 0, 0.5f, 0}, {(R + L) / (L - R), (T + B) / (B - T), 0.5f, 1.0f}};
        memcpy(cm.pData, m, sizeof m);
        g_ctx->Unmap(g_cb.get(), 0);
    }
    auto setup = [&]() {
        D3D11_VIEWPORT vp{0, 0, dd->DisplaySize.x * dd->FramebufferScale.x, dd->DisplaySize.y * dd->FramebufferScale.y, 0, 1};
        g_ctx->RSSetViewports(1, &vp);
        UINT stride = sizeof(ImDrawVert), off = 0;
        ID3D11Buffer* vb = g_vb.get();
        g_ctx->IASetInputLayout(g_layout.get());
        g_ctx->IASetVertexBuffers(0, 1, &vb, &stride, &off);
        g_ctx->IASetIndexBuffer(g_ib.get(), sizeof(ImDrawIdx) == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, 0);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->VSSetShader(g_vs.get(), nullptr, 0);
        ID3D11Buffer* cb = g_cb.get();
        g_ctx->VSSetConstantBuffers(0, 1, &cb);
        g_ctx->PSSetShader(g_ps.get(), nullptr, 0);
        ID3D11SamplerState* s = g_sampler.get();
        g_ctx->PSSetSamplers(0, 1, &s);
        g_ctx->GSSetShader(nullptr, nullptr, 0);
        const float bf[4] = {0, 0, 0, 0};
        g_ctx->OMSetBlendState(g_blend.get(), bf, 0xffffffff);
        g_ctx->OMSetDepthStencilState(g_depth.get(), 0);
        g_ctx->RSSetState(g_raster.get());
    };
    setup();
    int vo = 0, io = 0;
    ImVec2 clip_off = dd->DisplayPos, scale = dd->FramebufferScale;
    // glass windows: draw list index -> window (popups, menus, tooltips, modals; not their child windows)
    struct GW { const ImGuiWindow* w; int dim; };
    std::vector<GW> glass(dd->CmdListsCount, GW{nullptr, -1});
    if (gfx_glass() && GImGui) {
        for (ImGuiWindow* w : GImGui->Windows) {
            if (!w->Active || w->Hidden || !(w->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_ChildMenu | ImGuiWindowFlags_Modal))) continue;
            if ((w->Flags & ImGuiWindowFlags_ChildWindow) && !(w->Flags & ImGuiWindowFlags_ChildMenu)) continue;
            if (w->Size.x < 4 || w->Size.y < 4) continue;
            for (int n = 0; n < dd->CmdListsCount; ++n)
                if (dd->CmdLists[n] == w->DrawList) {
                    int dim = -1;   // modal: ImGui pushed the dim overlay to the front of the modal's own list
                    if (w->Flags & ImGuiWindowFlags_Modal)
                        for (int k = 0; k < std::min(2, w->DrawList->CmdBuffer.Size); ++k) {
                            const ImDrawCmd& c = w->DrawList->CmdBuffer[k];
                            if (c.ElemCount == 6 && !c.UserCallback && c.ClipRect.x <= clip_off.x && c.ClipRect.y <= clip_off.y) { dim = k; break; }
                        }
                    glass[n] = GW{w, dim};
                    break;
                }
        }
    }
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* cl = dd->CmdLists[n];
        const GW gw = glass[n];
        for (int c = 0; c < cl->CmdBuffer.Size; ++c) {
            const ImDrawCmd* cmd = &cl->CmdBuffer[c];
            if (gw.w && c == gw.dim + 1) {   // after the modal dim (or at the list start): frost the backdrop
                // R13 (P1-08): frost exactly the rect the window's background was emitted at this frame
                // (OuterRectClipped, fixed in Begin), not Pos/Size, which code may still change after Begin.
                const ImGuiWindow* w = gw.w;
                const ImRect r = w->OuterRectClipped;
                if (r.GetWidth() < 4 || r.GetHeight() < 4) { setup(); goto no_glass; }
                glass_pass((r.Min.x - clip_off.x) * scale.x, (r.Min.y - clip_off.y) * scale.y, (r.Max.x - clip_off.x) * scale.x,
                           (r.Max.y - clip_off.y) * scale.y, w->WindowRounding * scale.x,
                           (w->Flags & ImGuiWindowFlags_Modal) || !(w->Flags & ImGuiWindowFlags_NoTitleBar));
                setup();
            }
        no_glass:
            if (cmd->UserCallback) {
                if (cmd->UserCallback == ImDrawCallback_ResetRenderState) setup();
                else cmd->UserCallback(cl, cmd);
                continue;
            }
            ImVec2 a((cmd->ClipRect.x - clip_off.x) * scale.x, (cmd->ClipRect.y - clip_off.y) * scale.y);
            ImVec2 b((cmd->ClipRect.z - clip_off.x) * scale.x, (cmd->ClipRect.w - clip_off.y) * scale.y);
            if (b.x <= a.x || b.y <= a.y) continue;
            D3D11_RECT r{(LONG)a.x, (LONG)a.y, (LONG)b.x, (LONG)b.y};
            g_ctx->RSSetScissorRects(1, &r);
            ID3D11ShaderResourceView* srv = (ID3D11ShaderResourceView*)cmd->GetTexID();
            g_ctx->PSSetShaderResources(0, 1, &srv);
            g_ctx->DrawIndexed(cmd->ElemCount, cmd->IdxOffset + io, (INT)cmd->VtxOffset + vo);
        }
        io += cl->IdxBuffer.Size;
        vo += cl->VtxBuffer.Size;
    }
}

void gfx_present(bool vsync) { if (g_swap) g_swap->Present(vsync ? 1 : 0, 0); }

// ---------------------------------------------------------------------------
bool RenderTexture::ensure(int w, int h) {
    w = std::max(1, w); h = std::max(1, h);
    if (w == w_ && h == h_ && srv_) return false;
    release();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, tex_.put()))) return false;
    g_dev->CreateShaderResourceView(tex_.get(), nullptr, srv_.put());
    Com<IDXGISurface> surf;
    tex_->QueryInterface(__uuidof(IDXGISurface), (void**)surf.put());
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (surf) g_d2d->CreateBitmapFromDxgiSurface(surf.get(), &bp, bmp_.put());
    w_ = w; h_ = h;
    return true;
}

void RenderTexture::release() { bmp_.reset(); srv_.reset(); tex_.reset(); w_ = h_ = 0; }

ID2D1DeviceContext* RenderTexture::begin(float dpi_scale) {
    if (!bmp_) return nullptr;
    g_d2d->SetTarget(bmp_.get());
    g_d2d->SetDpi(96.f * dpi_scale, 96.f * dpi_scale);
    g_d2d->BeginDraw();
    g_d2d->SetTransform(D2D1::Matrix3x2F::Identity());
    return g_d2d.get();
}

void RenderTexture::end() {
    g_d2d->EndDraw();
    g_d2d->SetTarget(nullptr);
}

bool StaticTexture::create(int w_, int h_, const void* bgra, int pitch) {
    release();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)w_; td.Height = (UINT)h_; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sub{bgra, (UINT)pitch, 0};
    if (FAILED(g_dev->CreateTexture2D(&td, &sub, tex_.put()))) return false;
    if (FAILED(g_dev->CreateShaderResourceView(tex_.get(), nullptr, srv_.put()))) return false;
    w = w_; h = h_;
    return true;
}

} // namespace pcbp::gui
