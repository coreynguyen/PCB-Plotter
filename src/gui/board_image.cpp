// ============================================================================
// board_image.cpp — board photo / AOI image underlays (LEAD_BRIEF §14).
//
//   * decoding: WIC (png / jpg / bmp / tif / gif) -> premultiplied BGRA, PPM/PGM
//     via core decode_pnm; images larger than the GPU limit are downscaled for
//     display only (registration always works in original pixel coordinates).
//   * drawing: app::g_board_underlay hook (render_board, after the board fill):
//     ID2D1DeviceContext::DrawBitmap with the image homography as perspective
//     transform; affine fallback on plain render targets (printing).
//   * Board Images dialog: image pane | board pane. Click an image point, then
//     the matching board point (snaps to pads, fiducials, frame corners) — or
//     type its coordinates. 1 pair = move, 2 = scale + rotation, 3 = affine,
//     4+ = perspective. Residual (rms) is shown per solve.
// ============================================================================
#include "import_extras.h"
#include "../app/board_render.h"
#include "../core/board_image.h"
#include "../core/fileio.h"
#include "../ui/form.h"
#include "gapp.h"
#include "gfx.h"
#include "imgui_internal.h"

#include <d2d1_1.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

namespace pcbp::app {
using namespace pcbp::ui;
using gui::S;
using gui::th;

namespace {

// ---------------------------------------------------------------------------
// pixels (CPU cache)
// ---------------------------------------------------------------------------
struct Pix {
    int w = 0, h = 0;               // stored (possibly downscaled) size
    int src_w = 0, src_h = 0;       // original size
    std::vector<uint8_t> bgra;      // premultiplied
    std::string err;
};

uint64_t content_key(const BoardImage& bi) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    for (char c : bi.path) mix((uint8_t)c);
    mix(bi.data.size());
    mix(bi.revision);
    if (!bi.data.empty()) {
        const size_t step = std::max<size_t>(1, bi.data.size() / 4096);
        for (size_t i = 0; i < bi.data.size(); i += step) mix(bi.data[i]);
    }
    return h;
}

bool wic_decode(const std::vector<uint8_t>& data, int max_dim, Pix& px) {
    Com<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(f.put())))) { px.err = "WIC unavailable"; return false; }
    Com<IWICStream> st;
    if (FAILED(f->CreateStream(st.put())) || FAILED(st->InitializeFromMemory((BYTE*)data.data(), (DWORD)data.size()))) { px.err = "WIC stream"; return false; }
    Com<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromStream(st.get(), nullptr, WICDecodeMetadataCacheOnDemand, dec.put()))) { px.err = "unsupported image format"; return false; }
    Com<IWICBitmapFrameDecode> fr;
    if (FAILED(dec->GetFrame(0, fr.put()))) { px.err = "no image frame"; return false; }
    UINT w = 0, h = 0;
    fr->GetSize(&w, &h);
    if (!w || !h) { px.err = "empty image"; return false; }
    px.src_w = (int)w; px.src_h = (int)h;
    IWICBitmapSource* src = fr.get();
    Com<IWICBitmapScaler> sc;
    UINT ow = w, oh = h;
    if ((int)std::max(w, h) > max_dim) {
        const double k = (double)max_dim / std::max(w, h);
        ow = std::max(1u, (UINT)(w * k)); oh = std::max(1u, (UINT)(h * k));
        if (SUCCEEDED(f->CreateBitmapScaler(sc.put())) && SUCCEEDED(sc->Initialize(fr.get(), ow, oh, WICBitmapInterpolationModeFant))) src = sc.get();
        else { ow = w; oh = h; }
    }
    Com<IWICFormatConverter> cv;
    if (FAILED(f->CreateFormatConverter(cv.put())) ||
        FAILED(cv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) { px.err = "pixel conversion"; return false; }
    px.w = (int)ow; px.h = (int)oh;
    px.bgra.resize((size_t)ow * oh * 4);
    if (FAILED(cv->CopyPixels(nullptr, ow * 4, (UINT)px.bgra.size(), px.bgra.data()))) { px.err = "decode failed"; return false; }
    return true;
}

bool decode_any(const std::vector<uint8_t>& data, int max_dim, Pix& px) {
    if (data.size() > 2 && data[0] == 'P' && (data[1] == '5' || data[1] == '6')) {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (!decode_pnm(data.data(), data.size(), w, h, rgba, px.err)) return false;
        px.src_w = w; px.src_h = h;
        const int step = std::max(1, (std::max(w, h) + max_dim - 1) / max_dim);
        px.w = w / step; px.h = h / step;
        px.bgra.resize((size_t)px.w * px.h * 4);
        for (int y = 0; y < px.h; ++y)
            for (int x = 0; x < px.w; ++x) {
                const uint8_t* s = &rgba[((size_t)(y * step) * w + x * step) * 4];
                uint8_t* d = &px.bgra[((size_t)y * px.w + x) * 4];
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255;
            }
        return true;
    }
    return wic_decode(data, max_dim, px);
}

std::map<uint64_t, std::shared_ptr<Pix>> g_pix;   // key ^ max_dim

std::shared_ptr<Pix> pixels(const BoardImage& bi, int max_dim) {
    const uint64_t k = content_key(bi) ^ ((uint64_t)max_dim << 48);
    if (auto it = g_pix.find(k); it != g_pix.end()) return it->second;
    if (g_pix.size() > 12) g_pix.clear();
    auto px = std::make_shared<Pix>();
    std::vector<uint8_t> file;
    const std::vector<uint8_t>* data = &bi.data;
    if (bi.data.empty()) {
        std::string e;
        if (!read_file(bi.path, file, e)) { px->err = "cannot read " + bi.path; g_pix[k] = px; return px; }
        data = &file;
    }
    decode_any(*data, max_dim, *px);
    g_pix[k] = px;
    return px;
}

// ---------------------------------------------------------------------------
// D2D underlay
// ---------------------------------------------------------------------------
struct BmpKey { uint64_t content; void* device; bool operator<(const BmpKey& o) const { return content != o.content ? content < o.content : device < o.device; } };
std::map<BmpKey, Com<ID2D1Bitmap>> g_bmp;

void draw_underlay(ID2D1RenderTarget* rt, const Doc& d, const BoardRenderOpts& o) {
    Com<ID2D1DeviceContext> dc;
    rt->QueryInterface(IID_PPV_ARGS(dc.put()));
    void* dev = rt;
    Com<ID2D1Device> device;
    if (dc) { dc->GetDevice(device.put()); if (device) dev = device.get(); }
    const int max_dim = (int)std::min<UINT32>(rt->GetMaximumBitmapSize(), 8192);
    for (const BoardImage& bi : d.prj.board_images) {
        if (!bi.visible || bi.opacity <= 0.01f || bi.px_w <= 0 || bi.px_h <= 0) continue;
        if ((bi.side == 0 && o.mirror) || (bi.side == 1 && !o.mirror)) continue;
        auto px = pixels(bi, max_dim);
        if (!px || px->bgra.empty()) continue;
        BmpKey key{content_key(bi) ^ (uint64_t)max_dim, dev};
        auto it = g_bmp.find(key);
        if (it == g_bmp.end()) {
            if (g_bmp.size() > 16) g_bmp.clear();
            Com<ID2D1Bitmap> b;
            D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (FAILED(rt->CreateBitmap(D2D1::SizeU(px->w, px->h), px->bgra.data(), px->w * 4, bp, b.put()))) continue;
            it = g_bmp.emplace(key, b).first;
        }
        const double* H = bi.H;
        const D2D1_RECT_F dest = D2D1::RectF(0, 0, (float)bi.px_w, (float)bi.px_h);
        const bool affine = std::fabs(H[6]) < 1e-12 && std::fabs(H[7]) < 1e-12;
        rt->SetTransform(o.world);
        if (dc && !affine) {
            D2D1_MATRIX_4X4_F m{};
            m._11 = (float)H[0]; m._12 = (float)H[3]; m._13 = 0; m._14 = (float)H[6];
            m._21 = (float)H[1]; m._22 = (float)H[4]; m._23 = 0; m._24 = (float)H[7];
            m._31 = 0;           m._32 = 0;           m._33 = 1; m._34 = 0;
            m._41 = (float)H[2]; m._42 = (float)H[5]; m._43 = 0; m._44 = (float)H[8];
            dc->DrawBitmap(it->second.get(), &dest, bi.opacity, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, &m);
        } else {
            const double w = std::fabs(H[8]) > 1e-15 ? H[8] : 1;
            D2D1_MATRIX_3X2_F m = D2D1::Matrix3x2F((float)(H[0] / w), (float)(H[3] / w), (float)(H[1] / w), (float)(H[4] / w), (float)(H[2] / w), (float)(H[5] / w));
            rt->SetTransform(m * o.world);
            rt->DrawBitmap(it->second.get(), dest, bi.opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
    }
    rt->SetTransform(o.world);
}

const int g_hook_installed = (g_board_underlay = &draw_underlay, 1);
}  // namespace

bool board_image_pixels(const BoardImage& bi, int max_dim, int& w, int& h, const std::vector<uint8_t>*& bgra, std::string& err) {
    auto px = pixels(bi, max_dim);
    if (!px || px->bgra.empty()) { err = px ? px->err : "no pixels"; return false; }
    static std::vector<std::shared_ptr<Pix>> keep;   // the cache may drop it; the caller copies to the GPU at once
    keep.push_back(px);
    if (keep.size() > 4) keep.erase(keep.begin());
    w = px->w; h = px->h; bgra = &px->bgra;
    return true;
}

namespace {

// ---------------------------------------------------------------------------
// dialog helpers
// ---------------------------------------------------------------------------
struct Pane {
    float scale = 1;
    ImVec2 off{0, 0};
    bool fitted = false;
};

struct Snap { Vec2 p; const char* what; std::string label; };

std::vector<Snap> board_snaps(const Doc& d) {
    std::vector<Snap> v;
    const Project& p = d.prj;
    for (auto& c : p.comps) {
        if (c.feature == Feature::Fiducial) { v.push_back({c.pos, "fiducial", c.ref}); continue; }
        const Package* pk = p.package_of(c);
        if (!pk) { v.push_back({c.pos, "centroid", c.ref}); continue; }
        for (size_t i = 0; i < pk->pads.size() && i < 400; ++i) v.push_back({pad_world(d, c, (int)i), "pad", c.ref + "." + std::to_string(pk->pads[i].pin)});
    }
    for (auto& poly : p.board_frame)
        for (size_t i = 0; i < poly.size(); ++i)
            if (i + 1 < poly.size() || dist(poly.front(), poly.back()) > 0.01) v.push_back({poly[i], "frame corner", ""});
    BBox b = p.board_bbox();
    if (!b.empty()) for (Vec2 q : {Vec2{b.x0, b.y0}, Vec2{b.x1, b.y0}, Vec2{b.x1, b.y1}, Vec2{b.x0, b.y1}}) v.push_back({q, "board extent corner", ""});
    return v;
}

std::wstring file_name_of(const std::wstring& p) {
    auto s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

} // namespace

// ===========================================================================
bool cmd_import_board_image(HWND owner, Doc& d) {
    static std::wstring last;
    std::wstring p = last;
    if (const wchar_t* tf = _wgetenv(L"PCBP_TEST_BOARD_IMAGE")) p = tf;   // automation
    else if (!open_file_dialog(owner, L"Import Board Image", {{L"Images", L"*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff;*.gif;*.ppm;*.pgm"}, {L"All files", L"*.*"}}, p)) return false;
    last = p;
    BoardImage bi;
    std::string err;
    if (!read_file(U(p), bi.data, err)) { message(owner, L"Import Board Image", L"Cannot read the file:\n" + W(err)); return false; }
    bi.path = U(p);
    bi.name = U(file_name_of(p));
    Pix px;
    if (!decode_any(bi.data, 256, px)) { message(owner, L"Import Board Image", L"The image could not be decoded (" + W(px.err) + L").\nSupported: PNG, JPEG, BMP, TIFF, GIF, PPM."); return false; }
    bi.px_w = px.src_w; bi.px_h = px.src_h;
    if (bi.data.size() > (size_t(64) << 20)) {
        bi.embed = false;
        bi.data.clear();   // very large: keep a path reference only
    }
    BBox b = d.prj.board_bbox();
    fit_to_box(bi, b);
    bi.side = d.view_from_bottom ? 1 : 0;
    d.checkpoint("Import board image");
    d.prj.board_images.push_back(std::move(bi));
    d.changed(CH_LAYERS);
    return dlg_board_images(owner, d, (int)d.prj.board_images.size() - 1) || true;
}

bool dlg_board_images(HWND owner, Doc& d, int select) {
    if (d.prj.board_images.empty()) { message(owner, L"Board Images", L"No board images yet.\nUse Import > Board Image to load a photo or AOI image."); return false; }
    const std::vector<BoardImage> saved = d.prj.board_images;
    struct St {
        int cur = 0;
        Pane ip, bp;
        bool have_img = false, have_brd = false;
        Vec2 img_pt, brd_pt;
        std::string brd_label;
        gui::StaticTexture tex;
        uint64_t tex_key = 0;
        std::string msg;
        float typed[2] = {0, 0};
        std::vector<Snap> snaps;
    } st;
    st.cur = select >= 0 && select < (int)d.prj.board_images.size() ? select : 0;
    st.snaps = board_snaps(d);
    bool checkpointed = false;
    auto touch = [&](const char* what) {
        if (!checkpointed) { d.checkpoint(what); checkpointed = true; }
        d.changed(CH_LAYERS);
    };
    auto add_pair = [&]() {
        BoardImage& bi = d.prj.board_images[st.cur];
        bi.pairs.push_back({st.img_pt, st.brd_pt});
        st.have_img = st.have_brd = false;
        std::string err;
        if (calibrate(bi, err)) {
            const char* mode = bi.calib == 1 ? "moved" : bi.calib == 2 ? "scale + rotation" : bi.calib == 3 ? "affine" : "perspective";
            char b[160];
            std::snprintf(b, sizeof b, "%d point(s): %s, residual %.3f mm", bi.calib, mode, bi.rms_mm);
            st.msg = b;
        } else { st.msg = err; bi.pairs.pop_back(); }
        touch("Calibrate board image");
    };

    Form f(L"Board Images", 1060);
    f.icon((int)Icon::Image).header(L"Board Images \x2014 underlay & registration");
    f.subtitle(L"Click a point in the image, then the same feature on the board (snaps to pads, fiducials, frame corners). 2 points: scale + rotation, 4 points: perspective.");
    f.custom("ui", [&](Form& ff, float w) {
        const gui::GTheme& t = th();
        auto& imgs = d.prj.board_images;
        if (st.cur >= (int)imgs.size()) st.cur = (int)imgs.size() - 1;
        if (st.cur < 0) { ImGui::TextUnformatted("No images."); return; }
        BoardImage& bi = imgs[st.cur];
        // ---- controls ----
        ImGui::SetNextItemWidth(S(200));
        if (ImGui::BeginCombo("##img", bi.name.c_str())) {
            for (int i = 0; i < (int)imgs.size(); ++i)
                if (ImGui::Selectable((imgs[i].name + "##" + std::to_string(i)).c_str(), i == st.cur)) { st.cur = i; st.have_img = st.have_brd = false; st.ip.fitted = false; }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Visible", &bi.visible)) touch("Board image visibility");
        ImGui::SameLine();
        if (ImGui::Checkbox("Lock", &bi.locked)) touch("Lock board image");
        gui::tooltip("Locked images keep their registration: picking and solving are disabled.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(140));
        if (ImGui::SliderFloat("Opacity", &bi.opacity, 0.f, 1.f, "%.2f")) touch("Board image opacity");
        ImGui::SameLine();
        const char* sides[] = {"Top view", "Bottom view", "Both"};
        ImGui::SetNextItemWidth(S(120));
        if (ImGui::Combo("##side", &bi.side, sides, 3)) touch("Board image side");
        gui::tooltip("Shown when the board is viewed from this side.");
        ImGui::SameLine();
        if (ImGui::Checkbox("Mirrored", &bi.mirrored)) { std::string e; if (bi.pairs.size() == 2) calibrate(bi, e); touch("Board image mirror"); }
        gui::tooltip("2-point solve with an X mirror (e.g. bottom side scanned face down and viewed from the top).");
        ImGui::SameLine();
        if (ImGui::Checkbox("Embed in project", &bi.embed)) {
            if (bi.embed && bi.data.empty()) { std::string e; if (!read_file(bi.path, bi.data, e)) { bi.embed = false; st.msg = "cannot read " + bi.path; } }
            touch("Board image embedding");
        }
        gui::tooltip("Embedded: the image travels with the .pcbp file. Off: only the file path is stored.");
        ImGui::BeginDisabled(bi.locked);
        if (gui::button("Undo point", Icon::None, false, ImVec2(0, 0), !bi.pairs.empty())) {
            bi.pairs.pop_back();
            std::string e;
            if (!bi.pairs.empty()) calibrate(bi, e);
            st.msg = std::to_string(bi.pairs.size()) + " point(s)";
            touch("Calibrate board image");
        }
        ImGui::SameLine();
        if (gui::button("Clear points", Icon::None, false, ImVec2(0, 0), !bi.pairs.empty())) { bi.pairs.clear(); st.msg = "points cleared (registration kept)"; touch("Calibrate board image"); }
        ImGui::SameLine();
        if (gui::button("Reset fit")) { bi.pairs.clear(); fit_to_box(bi, d.prj.board_bbox()); st.msg = "fitted to the board extent"; touch("Reset board image"); }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(160));
        ImGui::InputFloat2("##typed", st.typed, "%.3f");
        ImGui::SameLine();
        if (gui::button("Use typed board point (mm)", Icon::None, false, ImVec2(0, 0), st.have_img)) {
            st.brd_pt = d.prj.from_user({st.typed[0], st.typed[1]});
            st.have_brd = true;
            add_pair();
        }
        gui::tooltip("Board X / Y in mm relative to the current origin, for features that are not in the data (e.g. a known frame dimension).");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (gui::button("Remove image")) {
            touch("Remove board image");
            imgs.erase(imgs.begin() + st.cur);
            st.cur = std::max(0, st.cur - 1);
            st.ip.fitted = false;
            if (imgs.empty()) { ff.close(true); return; }
            return;
        }
        // ---- texture ----
        auto px = pixels(bi, 4096);
        const uint64_t key = content_key(bi);
        if (st.tex_key != key) {
            st.tex.release();
            if (px && !px->bgra.empty()) st.tex.create(px->w, px->h, px->bgra.data(), px->w * 4);
            st.tex_key = key;
        }
        // ---- panes ----
        const float ph = S(470), gap = S(8), pw = (w - gap) * 0.5f;
        ImGuiIO& io = ImGui::GetIO();
        double Hi[9];
        const bool inv = h_invert(bi.H, Hi);
        auto pane = [&](const char* id, Pane& pn, bool is_img, ImVec2 a) {
            ImGui::SetCursorScreenPos(a);
            ImGui::InvisibleButton(id, ImVec2(pw, ph), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
            const bool hov = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 b(a.x + pw, a.y + ph);
            dl->AddRectFilled(a, b, t.canvas, S(4));
            dl->PushClipRect(a, b, true);
            // world extents
            BBox ext;
            if (is_img) { ext.add({0, 0}); ext.add({(double)bi.px_w, (double)bi.px_h}); }
            else { ext = d.prj.board_bbox(); Vec2 c4[4]; image_corners(bi, c4); for (auto& q : c4) ext.add(q); }
            if (!pn.fitted && !ext.empty()) {
                pn.scale = (float)std::min((pw - S(20)) / std::max(ext.w(), 1e-6), (ph - S(20)) / std::max(ext.h(), 1e-6));
                const float cx = (float)(ext.x0 + ext.x1) * 0.5f, cy = (float)(ext.y0 + ext.y1) * 0.5f;
                pn.off = is_img ? ImVec2(pw * 0.5f - cx * pn.scale, ph * 0.5f - cy * pn.scale) : ImVec2(pw * 0.5f - cx * pn.scale, ph * 0.5f + cy * pn.scale);
                pn.fitted = true;
            }
            auto to_s = [&](Vec2 p) { return is_img ? ImVec2(a.x + pn.off.x + (float)p.x * pn.scale, a.y + pn.off.y + (float)p.y * pn.scale)
                                                    : ImVec2(a.x + pn.off.x + (float)p.x * pn.scale, a.y + pn.off.y - (float)p.y * pn.scale); };
            auto to_w = [&](ImVec2 s) { return is_img ? Vec2{(s.x - a.x - pn.off.x) / pn.scale, (s.y - a.y - pn.off.y) / pn.scale}
                                                      : Vec2{(s.x - a.x - pn.off.x) / pn.scale, -(s.y - a.y - pn.off.y) / pn.scale}; };
            if (hov && io.MouseWheel != 0) {
                const Vec2 before = to_w(io.MousePos);
                pn.scale *= std::pow(1.2f, io.MouseWheel);
                const ImVec2 now = to_s(before);
                pn.off.x += io.MousePos.x - now.x; pn.off.y += io.MousePos.y - now.y;
            }
            if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
                pn.off.x += io.MouseDelta.x; pn.off.y += io.MouseDelta.y;
            }
            if (is_img) {
                if (st.tex.imgui_id()) dl->AddImage((ImTextureID)st.tex.imgui_id(), to_s({0, 0}), to_s({(double)bi.px_w, (double)bi.px_h}));
                else dl->AddText(ImVec2(a.x + S(10), a.y + S(10)), t.error, px ? px->err.c_str() : "no pixels");
                // board overlay through H^-1
                if (inv) {
                    for (auto& poly : d.prj.board_frame)
                        for (size_t i = 0; i + 1 < poly.size(); ++i) dl->AddLine(to_s(h_apply(Hi, poly[i])), to_s(h_apply(Hi, poly[i + 1])), (t.accent & 0x00FFFFFF) | 0xC0000000, S(1.5f));
                    for (auto& sn : st.snaps) dl->AddCircle(to_s(h_apply(Hi, sn.p)), S(2.5f), (t.accent & 0x00FFFFFF) | 0x90000000);
                }
            } else {
                for (auto& poly : d.prj.board_frame)
                    for (size_t i = 0; i + 1 < poly.size(); ++i) dl->AddLine(to_s(poly[i]), to_s(poly[i + 1]), t.text_dim, S(1.5f));
                for (auto& sn : st.snaps) {
                    const bool fid = sn.what[0] == 'f' && sn.what[1] == 'i';
                    dl->AddCircleFilled(to_s(sn.p), fid ? S(4) : S(2), fid ? t.ok : t.text_faint);
                }
                Vec2 c4[4];
                image_corners(bi, c4);
                for (int i = 0; i < 4; ++i) dl->AddLine(to_s(c4[i]), to_s(c4[(i + 1) % 4]), t.warn, S(1));
            }
            // pairs
            for (size_t i = 0; i < bi.pairs.size(); ++i) {
                ImVec2 q = to_s(is_img ? bi.pairs[i].img : bi.pairs[i].board);
                dl->AddCircle(q, S(7), t.accent, 0, S(2));
                dl->AddLine(ImVec2(q.x - S(10), q.y), ImVec2(q.x + S(10), q.y), t.accent, S(1));
                dl->AddLine(ImVec2(q.x, q.y - S(10)), ImVec2(q.x, q.y + S(10)), t.accent, S(1));
                dl->AddText(gui::g_font_bold, ImGui::GetFontSize(), ImVec2(q.x + S(8), q.y - S(20)), t.accent, std::to_string(i + 1).c_str());
            }
            const bool pend = is_img ? st.have_img : st.have_brd;
            if (pend) { ImVec2 q = to_s(is_img ? st.img_pt : st.brd_pt); dl->AddCircle(q, S(8), t.warn, 0, S(2.5f)); }
            // hover snap
            const Snap* hs = nullptr;
            if (hov && !is_img) {
                float bd = S(14);
                for (auto& sn : st.snaps) {
                    ImVec2 q = to_s(sn.p);
                    float dd = std::hypot(q.x - io.MousePos.x, q.y - io.MousePos.y);
                    if (dd < bd) { bd = dd; hs = &sn; }
                }
                if (hs) {
                    ImVec2 q = to_s(hs->p);
                    dl->AddCircle(q, S(9), t.ok, 0, S(2));
                    std::string lb = std::string(hs->what) + (hs->label.empty() ? "" : " " + hs->label);
                    dl->AddText(ImVec2(q.x + S(10), q.y + S(4)), t.text, lb.c_str());
                }
            }
            if (hov) {
                Vec2 wpt = to_w(io.MousePos);
                char b2[96];
                if (is_img) std::snprintf(b2, sizeof b2, "px %.1f, %.1f", wpt.x, wpt.y);
                else { Vec2 u = d.prj.to_user(wpt); std::snprintf(b2, sizeof b2, "%.3f, %.3f mm", u.x, u.y); }
                dl->AddText(ImVec2(a.x + S(6), b.y - S(20)), t.text_dim, b2);
            }
            dl->AddText(ImVec2(a.x + S(6), a.y + S(4)), t.text_dim, is_img ? "IMAGE  (wheel zoom, right-drag pan)" : "BOARD  (click snaps to pads / fiducials / corners)");
            dl->PopClipRect();
            if (!bi.locked && hov && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                if (is_img) { st.img_pt = to_w(io.MousePos); st.have_img = true; }
                else { st.brd_pt = hs ? hs->p : to_w(io.MousePos); st.have_brd = true; st.brd_label = hs ? hs->label : ""; }
                if (st.have_img && st.have_brd) add_pair();
            }
        };
        const ImVec2 o0 = ImGui::GetCursorScreenPos();
        pane("##ipane", st.ip, true, o0);
        pane("##bpane", st.bp, false, ImVec2(o0.x + pw + gap, o0.y));
        ImGui::SetCursorScreenPos(ImVec2(o0.x, o0.y + ph + S(6)));
        std::string status = st.msg;
        if (status.empty()) status = bi.pairs.empty() ? (bi.calib ? "registered" : "default fit (not registered)") : std::to_string(bi.pairs.size()) + " point pair(s)";
        if (st.have_img && !st.have_brd) status += "  \xE2\x80\x94 now click the matching board point";
        if (st.have_brd && !st.have_img) status += "  \xE2\x80\x94 now click the matching image point";
        if (bi.locked) status += "  [locked]";
        char b3[64];
        std::snprintf(b3, sizeof b3, "   %d x %d px", bi.px_w, bi.px_h);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_dim), "%s%s", status.c_str(), b3);
    }, 560);
    f.ok_text(L"Done");
    if (!f.run(owner)) {
        d.prj.board_images = saved;
        d.changed(CH_LAYERS);
        return false;
    }
    return true;
}

} // namespace pcbp::app
