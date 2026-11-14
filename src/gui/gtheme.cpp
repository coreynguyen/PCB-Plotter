// ============================================================================
// gtheme.cpp — ImGui theme (rounded, light/dark), fonts, icon atlas, widgets.
// ============================================================================
#include "gapp.h"
#include "gfx.h"
#include "../ui/d2d.h"
#include "../ui/theme.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <thread>
#include <unordered_map>
#include "../app/commands.h"
#include "../ui/form.h"

namespace pcbp::gui {

ImFont* g_font = nullptr;
ImFont* g_font_bold = nullptr;
ImFont* g_font_mono = nullptr;
ImFont* g_font_big = nullptr;

namespace {
GTheme g_th;
float g_dpi = 1.f;              // monitor DPI / 96 (per-monitor v2)
float g_ui = 1.3f;              // user UI scale (Settings > Appearance; default ~30% larger)
float g_icon = 1.f;             // extra icon scale relative to g_ui
bool g_perf = false;            // performance mode: no animations / fades / glow
RenderTexture g_atlas;
int kCell = 48;                 // atlas cell px (grows with the icon scale so large icons stay crisp)
const int kCols = 16;
int g_rows = 0;
bool g_atlas_dirty = true;

constexpr ImU32 C(uint32_t rgb, uint8_t a = 255) {   // 0xRRGGBB -> ImU32 (ABGR)
    return ((ImU32)a << 24) | ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF);
}

void make_theme(bool dark) {
    GTheme& t = g_th;
    t.dark = dark;
    if (dark) {
        t.bg = C(0x1B1C1F); t.panel = C(0x232428); t.panel2 = C(0x2A2C31); t.raised = C(0x33363C);
        t.text = C(0xE8E9EC); t.text_dim = C(0xA3A7AF); t.text_faint = C(0x6E727A); t.border = C(0x3A3D44);
        t.accent = C(0x4C8DFF); t.accent_hover = C(0x6AA1FF); t.accent_text = C(0xFFFFFF);
        t.field = C(0x16171A); t.field_hover = C(0x1E2024); t.sel = C(0x2B4C80); t.sel_text = C(0xFFFFFF);
        t.warn = C(0xF2B84B); t.error = C(0xF0605A); t.ok = C(0x4CC38A);
        t.canvas = C(0x16181B); t.statusbar = C(0x2B4C80); t.statusbar_text = C(0xFFFFFF);
        t.tab_active = C(0x33363C); t.tab_idle = C(0x232428);
    } else {
        t.bg = C(0xEEF0F3); t.panel = C(0xF8F9FB); t.panel2 = C(0xFFFFFF); t.raised = C(0xE4E7EC);
        t.text = C(0x1C1E22); t.text_dim = C(0x5E636B); t.text_faint = C(0x9AA0A8); t.border = C(0xD3D7DE);
        t.accent = C(0x2570EC); t.accent_hover = C(0x3B82F6); t.accent_text = C(0xFFFFFF);
        t.field = C(0xFFFFFF); t.field_hover = C(0xF3F5F8); t.sel = C(0xD3E3FD); t.sel_text = C(0x0B1B33);
        t.warn = C(0xB7791F); t.error = C(0xD03B36); t.ok = C(0x1E8E5A);
        t.canvas = C(0xE6E9EE); t.statusbar = C(0x2570EC); t.statusbar_text = C(0xFFFFFF);
        t.tab_active = C(0xFFFFFF); t.tab_idle = C(0xE4E7EC);
    }
}

ImVec4 V(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

void style_imgui() {
    ImGuiStyle st;
    ImGui::StyleColorsDark(&st);
    const GTheme& t = g_th;
    st.WindowRounding = 8; st.ChildRounding = 6; st.FrameRounding = 6; st.PopupRounding = 8; st.ScrollbarRounding = 8;
    st.GrabRounding = 6; st.TabRounding = 6; st.WindowBorderSize = 1; st.FrameBorderSize = 1; st.PopupBorderSize = 1;
    st.WindowPadding = ImVec2(12, 10); st.FramePadding = ImVec2(9, 5); st.ItemSpacing = ImVec2(8, 7); st.ItemInnerSpacing = ImVec2(6, 5);
    st.CellPadding = ImVec2(6, 3); st.ScrollbarSize = 13; st.GrabMinSize = 10; st.IndentSpacing = 18; st.TabBarBorderSize = 0;
    st.SeparatorTextBorderSize = 1; st.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = V(t.text);
    c[ImGuiCol_TextDisabled] = V(t.text_faint);
    c[ImGuiCol_WindowBg] = V(t.panel);
    c[ImGuiCol_ChildBg] = V(t.panel);
    c[ImGuiCol_PopupBg] = V(t.panel2);
    gfx_set_glass(!g_perf && app::ui_pref("Glass", 1) != 0, t.dark);
    if (gfx_glass()) c[ImGuiCol_PopupBg].w = t.dark ? 0.80f : 0.76f;   // frosted: the blurred backdrop shows through (gfx.cpp)
    c[ImGuiCol_Border] = V(t.border);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = V(t.field);
    c[ImGuiCol_FrameBgHovered] = V(t.field_hover);
    c[ImGuiCol_FrameBgActive] = V(t.field_hover);
    c[ImGuiCol_TitleBg] = V(t.panel);
    c[ImGuiCol_TitleBgActive] = V(t.panel2);
    c[ImGuiCol_TitleBgCollapsed] = V(t.panel);
    c[ImGuiCol_MenuBarBg] = V(t.bg);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = V(t.raised);
    c[ImGuiCol_ScrollbarGrabHovered] = V(t.border);
    c[ImGuiCol_ScrollbarGrabActive] = V(t.accent);
    c[ImGuiCol_CheckMark] = V(t.accent);
    c[ImGuiCol_SliderGrab] = V(t.accent);
    c[ImGuiCol_SliderGrabActive] = V(t.accent_hover);
    c[ImGuiCol_Button] = V(t.raised);
    c[ImGuiCol_ButtonHovered] = V(t.border);
    c[ImGuiCol_ButtonActive] = V(t.accent);
    c[ImGuiCol_Header] = V(t.sel);
    c[ImGuiCol_HeaderHovered] = V(t.raised);
    c[ImGuiCol_HeaderActive] = V(t.sel);
    c[ImGuiCol_Separator] = V(t.border);
    c[ImGuiCol_SeparatorHovered] = V(t.accent);
    c[ImGuiCol_SeparatorActive] = V(t.accent);
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = V(t.accent);
    c[ImGuiCol_ResizeGripActive] = V(t.accent);
    c[ImGuiCol_Tab] = V(t.tab_idle);
    c[ImGuiCol_TabHovered] = V(t.raised);
    c[ImGuiCol_TabSelected] = V(t.tab_active);
    c[ImGuiCol_TabSelectedOverline] = V(t.accent);
    c[ImGuiCol_TabDimmed] = V(t.tab_idle);
    c[ImGuiCol_TabDimmedSelected] = V(t.tab_active);
    c[ImGuiCol_TableHeaderBg] = V(t.panel2);
    c[ImGuiCol_TableBorderStrong] = V(t.border);
    c[ImGuiCol_TableBorderLight] = V(t.border);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = t.dark ? ImVec4(1, 1, 1, 0.025f) : ImVec4(0, 0, 0, 0.025f);
    c[ImGuiCol_TextSelectedBg] = V((t.accent & 0x00FFFFFF) | 0x66000000);
    c[ImGuiCol_NavHighlight] = V(t.accent);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, t.dark ? 0.55f : 0.30f);
    c[ImGuiCol_DockingPreview] = V((t.accent & 0x00FFFFFF) | 0x66000000);
    c[ImGuiCol_DockingEmptyBg] = V(t.bg);
    c[ImGuiCol_PlotHistogram] = V(t.accent);
    st.ScaleAllSizes(g_dpi * g_ui);
    ImGui::GetStyle() = st;
}

constexpr int kFontSteps = 6;
struct FontSteps { ImFont* base = nullptr; ImFont* font[kFontSteps] = {}; float px[kFontSteps] = {}; };
FontSteps g_steps[3];

void load_fonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    wchar_t win[MAX_PATH] = L"C:\\Windows";
    GetWindowsDirectoryW(win, MAX_PATH);
    std::string fd = wide_to_utf8(win) + "\\Fonts\\";
    auto exists = [](const std::string& p) { return GetFileAttributesW(utf8_to_wide(p).c_str()) != INVALID_FILE_ATTRIBUTES; };
    auto pick = [&](std::initializer_list<const char*> names) -> std::string {
        for (const char* n : names) {
            std::string p = n;
            if (p.find(':') == std::string::npos) p = fd + n;
            if (exists(p)) return p;
        }
        return {};
    };
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0x2000, 0x22FF, 0x2190, 0x21FF, 0x25A0, 0x25FF, 0};
    const float sz = std::round(15.f * g_dpi * g_ui);
    std::string ui = pick({"segoeui.ttf", "Z:\\usr\\share\\fonts\\truetype\\dejavu\\DejaVuSans.ttf", "tahoma.ttf", "arial.ttf"});
    std::string bold = pick({"seguisb.ttf", "segoeuib.ttf", "Z:\\usr\\share\\fonts\\truetype\\dejavu\\DejaVuSans-Bold.ttf", "tahomabd.ttf", "arialbd.ttf"});
    std::string mono = pick({"CascadiaMono.ttf", "consola.ttf", "Z:\\usr\\share\\fonts\\truetype\\dejavu\\DejaVuSansMono.ttf", "cour.ttf"});
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.PixelSnapH = false;
    // v17: symbols the UI face lacks (Segoe UI has no U+22EF etc.) fall back to Segoe UI Symbol, merged into the
    // face just added; ImGui keeps the glyphs the UI face already has. UI strings should still prefer ASCII / icons.
    static const ImWchar sym_ranges[] = {0x2190, 0x23FF, 0x25A0, 0x27BF, 0};
    const std::string sym = pick({"seguisym.ttf"});
    auto merge_sym = [&](ImFont* f, float px) {
        if (!f || sym.empty()) return;
        ImFontConfig mc = cfg;
        mc.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(sym.c_str(), px, &mc, sym_ranges);
    };
    g_font = ui.empty() ? io.Fonts->AddFontDefault() : io.Fonts->AddFontFromFileTTF(ui.c_str(), sz, &cfg, ranges);
    if (!g_font) g_font = io.Fonts->AddFontDefault();
    else if (!ui.empty()) merge_sym(g_font, sz);
    g_font_bold = bold.empty() ? g_font : io.Fonts->AddFontFromFileTTF(bold.c_str(), sz, &cfg, ranges);
    if (!g_font_bold) g_font_bold = g_font;
    else if (!bold.empty()) merge_sym(g_font_bold, sz);
    ImFontConfig mcfg = cfg;   // fixed pitch: whole-pixel advances, no half-pixel glyph fringes (§3 BOM review artefacts)
    mcfg.PixelSnapH = true;
    mcfg.OversampleH = 1;
    g_font_mono = mono.empty() ? g_font : io.Fonts->AddFontFromFileTTF(mono.c_str(), std::round(14.f * g_dpi * g_ui), &mcfg, ranges);
    if (!g_font_mono) g_font_mono = g_font;
    g_font_big = bold.empty() ? g_font : io.Fonts->AddFontFromFileTTF(bold.c_str(), std::round(30.f * g_dpi * g_ui), &cfg, ranges);
    if (!g_font_big) g_font_big = g_font;
    // REVIEW_V9 §3: extra raster sizes so zoomed / small text (spreadsheet cells, document viewer) is
    // drawn from a glyph bake near its destination size instead of a bilinear-scaled 1x bake
    // (font_for_px). Reduced range (Latin + common punctuation / symbols) keeps the atlas small.
    {
        static const ImWchar rng2[] = {0x0020, 0x017F, 0x2010, 0x2027, 0x2030, 0x203A, 0x20AC, 0x20AC, 0x2122, 0x2122, 0x2190, 0x2193, 0x2212, 0x2212, 0x2260, 0x2265, 0x25A0, 0x25CF, 0};
        static const float mul[kFontSteps] = {0.8f, 1.25f, 1.6f, 2.0f, 2.6f, 3.4f};
        ImFontConfig c2;
        c2.PixelSnapH = false;
        const std::string* files[3] = {&ui, &bold, &mono};
        ImFont* bases[3] = {g_font, g_font_bold, g_font_mono};
        for (int f = 0; f < 3; ++f) {
            g_steps[f] = FontSteps{};
            g_steps[f].base = bases[f];
            if (files[f]->empty()) continue;
            for (int k = 0; k < kFontSteps; ++k) {
                float px = std::round(bases[f]->FontSize * mul[k]);
                c2.OversampleH = px < 24 ? 2 : 1;
                g_steps[f].font[k] = io.Fonts->AddFontFromFileTTF(files[f]->c_str(), px, &c2, rng2);
                g_steps[f].px[k] = g_steps[f].font[k] ? px : 0;
            }
        }
    }
    io.FontDefault = g_font;
    io.Fonts->Build();
    gfx_font_texture_rebuild();
}

void build_atlas() {
    const int n = (int)ui::Icon::Count;
    g_rows = (n + kCols - 1) / kCols;
    kCell = std::clamp((int)std::lround(S(30) * g_icon * 0.75f) + 8, 48, 160);   // toolbar icon px + margin
    g_atlas.ensure(kCols * kCell, g_rows * 2 * kCell);
    ID2D1DeviceContext* rt = g_atlas.begin(1.0f);
    if (!rt) return;
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    const GTheme& t = g_th;
    auto argb = [](ImU32 c) { return 0xFF000000u | ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); };
    for (int i = 1; i < n; ++i) {
        int cx = i % kCols, cy = i / kCols;
        D2D1_RECT_F r = D2D1::RectF((float)(cx * kCell + 2), (float)(cy * kCell + 2), (float)(cx * kCell + kCell - 2), (float)(cy * kCell + kCell - 2));
        ui::draw_icon(rt, (ui::Icon)i, r, argb(t.text), argb(t.accent));
        D2D1_RECT_F r2 = r;
        r2.top += g_rows * kCell; r2.bottom += g_rows * kCell;
        ui::draw_icon(rt, (ui::Icon)i, r2, 0xFFFFFFFF, 0xFFFFFFFF);
    }
    g_atlas.end();
    g_atlas_dirty = false;
}
} // namespace

const GTheme& th() { return g_th; }

ImFont* font_for_px(ImFont* family, float px) {
    if (!family) family = g_font;
    if (!family || px <= 0) return family;
    for (const FontSteps& st : g_steps) {
        if (st.base != family) continue;
        // smallest bake >= 0.92 * px (slight down-scaling stays sharp), else the largest bake
        ImFont* pf = family->FontSize >= px * 0.92f ? family : nullptr;
        float pick = pf ? family->FontSize : 1e9f, big_px = family->FontSize;
        ImFont* big = family;
        for (int k = 0; k < kFontSteps; ++k) {
            if (!st.font[k]) continue;
            if (st.px[k] >= px * 0.92f && st.px[k] < pick) { pick = st.px[k]; pf = st.font[k]; }
            if (st.px[k] > big_px) { big_px = st.px[k]; big = st.font[k]; }
        }
        return pf ? pf : big;
    }
    return family;
}
float dpi_scale() { return g_dpi * g_ui; }
float S(float px) { return std::round(px * g_dpi * g_ui); }
float SI(float px) { return std::round(px * g_dpi * g_ui * g_icon); }
float monitor_dpi_scale() { return g_dpi; }
float ui_scale() { return g_ui; }
float icon_scale() { return g_icon; }
bool perf_mode() { return g_perf; }
void set_perf_mode(bool on) { g_perf = on; if (ImGui::GetCurrentContext()) style_imgui(); request_redraw(2); }

void set_dpi_scale(float s) { g_dpi = s; }

// Live rescale (Settings > Appearance, WM_DPICHANGED): fonts, style sizes and the icon atlas are rebuilt.
// Call OUTSIDE a frame (font atlas rebuild).
void set_ui_scale(float ui, float icon) {
    g_ui = std::clamp(ui, 0.8f, 2.5f);
    g_icon = std::clamp(icon, 0.5f, 2.5f);
    if (!ImGui::GetCurrentContext()) return;
    load_fonts();
    style_imgui();
    g_atlas_dirty = true;
    build_atlas();
    request_redraw(3);
}

void apply_theme() {
    make_theme(ui::theme_dark());
    style_imgui();
    if (!g_font) load_fonts();
    g_atlas_dirty = true;
    build_atlas();
}

void rebuild_fonts_for_dpi(float s) {
    g_dpi = s;
    load_fonts();
    style_imgui();
    g_atlas_dirty = true;   // cell size follows the effective scale
}

void icon_draw(ImDrawList* dl, ui::Icon ic, ImVec2 p, float size, float alpha, bool on_accent) {
    if (ic == ui::Icon::None || !g_atlas.valid()) return;
    if (g_atlas_dirty) build_atlas();
    int i = (int)ic, cx = i % kCols, cy = i / kCols + (on_accent ? g_rows : 0);
    float W = (float)(kCols * kCell), H = (float)(g_rows * 2 * kCell);
    ImVec2 uv0(cx * kCell / W, cy * kCell / H), uv1((cx + 1) * kCell / W, (cy + 1) * kCell / H);
    dl->AddImage((ImTextureID)g_atlas.imgui_id(), p, ImVec2(p.x + size, p.y + size), uv0, uv1, IM_COL32(255, 255, 255, (int)(255 * alpha)));
}

void icon_image(ui::Icon ic, float size, float alpha) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    icon_draw(ImGui::GetWindowDrawList(), ic, p, size, alpha);
}

void tooltip(const char* text) {
    if (text && *text && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(8), S(6)));
        ImGui::SetTooltip("%s", text);
        ImGui::PopStyleVar();
    }
}

float tab_item_height() { return S(32); }
TabHit tab_item(const char* id, const std::string& label, ui::Icon ic, bool active, bool dirty, const char* tip, float width) {
    TabHit r;
    const GTheme& t = th();
    const float h = tab_item_height(), is = std::min(SI(18), h - S(10)), xs = S(18);
    if (active) ImGui::PushFont(g_font_bold);
    const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    if (active) ImGui::PopFont();
    const float w = width > 0 ? width : S(10) + is + S(8) + ts.x + S(8) + xs + S(6);
    const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + h);
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##tab", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 xc(b.x - S(6) - xs * 0.5f, (a.y + b.y) * 0.5f);
    const ImVec2 mp = ImGui::GetIO().MousePos;
    const bool xhov = hov && std::fabs(mp.x - xc.x) <= xs * 0.5f && std::fabs(mp.y - xc.y) <= xs * 0.5f;
    if (clicked) {
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Middle) || xhov) r.close = true;
        else r.clicked = true;
    }
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) r.close = true;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float hv = anim(ImGui::GetID("hv"), hov ? 1.f : 0.f, 18.f);
    dl->AddRectFilled(a, b, active ? t.tab_active : col_lerp(t.tab_idle, t.raised, hv), S(7), ImDrawFlags_RoundCornersAll);
    if (active) { dl->AddRect(a, b, t.border, S(7)); dl->AddRectFilled(ImVec2(a.x + S(6), b.y - S(3)), ImVec2(b.x - S(6), b.y - S(1)), t.accent, S(2)); }
    icon_draw(dl, ic, ImVec2(a.x + S(10), a.y + (h - is) * 0.5f), is, active ? 1.f : 0.8f);
    const float tx = a.x + S(10) + is + S(8), tmax = b.x - S(8) - xs - S(4);
    ImGui::PushClipRect(ImVec2(tx, a.y), ImVec2(tmax, b.y), true);
    if (active) ImGui::PushFont(g_font_bold);
    dl->AddText(ImVec2(tx, a.y + (h - ts.y) * 0.5f), active ? t.text : t.text_dim, label.c_str());
    if (active) ImGui::PopFont();
    ImGui::PopClipRect();
    if (hov || active) {   // close x
        if (xhov) dl->AddCircleFilled(xc, xs * 0.5f, t.raised, 16);
        const float k = xs * 0.22f;
        const ImU32 xcol = xhov ? t.text : t.text_dim;
        dl->AddLine(ImVec2(xc.x - k, xc.y - k), ImVec2(xc.x + k, xc.y + k), xcol, S(1.5f));
        dl->AddLine(ImVec2(xc.x - k, xc.y + k), ImVec2(xc.x + k, xc.y - k), xcol, S(1.5f));
    } else if (dirty) dl->AddCircleFilled(xc, S(3.5f), t.text_dim, 12);   // unsaved changes
    if (hov && !xhov) tooltip(tip ? tip : label.c_str());
    if (xhov) tooltip("Close (middle-click also closes)");
    ImGui::PopID();
    return r;
}

bool icon_button(const char* id, ui::Icon ic, const char* tip, bool checked, bool enabled, float size) {
    if (size <= 0) size = SI(30);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::BeginDisabled(!enabled);
    bool clicked = ImGui::InvisibleButton("##ib", ImVec2(size, size));
    bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    ImGui::EndDisabled();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const GTheme& t = g_th;
    ImGuiID aid = ImGui::GetID("##ib");
    float h = anim(aid, hov && enabled ? 1.f : 0.f, 20.f);
    float pr = anim(aid + 1, act && enabled ? 1.f : 0.f, 30.f);
    float ck = anim(aid + 2, checked ? 1.f : 0.f, 16.f);
    if (ck > 0.01f) {
        dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), col_alpha(t.accent, 0.22f * ck), S(6));
        dl->AddRect(p, ImVec2(p.x + size, p.y + size), col_alpha(t.accent, 0.69f * ck), S(6), 0, 1.2f);
    }
    if (h > 0.01f) dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), col_alpha(col_lerp(t.raised, t.border, pr), h * (1 - 0.5f * ck)), S(6));
    float is = std::round(size * 0.66f * (1.f - 0.08f * pr));
    icon_draw(dl, ic, ImVec2(p.x + (size - is) * 0.5f, p.y + (size - is) * 0.5f + pr * S(0.5f)), is, enabled ? 1.f : 0.35f);
    tooltip(tip);
    ImGui::PopID();
    return clicked && enabled;
}

bool button(const char* label, ui::Icon ic, bool primary, ImVec2 size, bool enabled) {
    const GTheme& t = g_th;
    const char* end = ImGui::FindRenderedTextEnd(label);
    ImVec2 ts = ImGui::CalcTextSize(label, end);
    float isz = ic != ui::Icon::None ? std::round(ImGui::GetFontSize() * 1.15f) : 0;
    ImVec2 pad = ImGui::GetStyle().FramePadding;
    float w = size.x > 0 ? size.x : ts.x + pad.x * 2 + (isz > 0 ? isz + S(6) : 0) + S(6);
    float h = size.y > 0 ? size.y : ImGui::GetFrameHeight() + S(2);
    if (size.x < 0) w = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    bool clicked = ImGui::InvisibleButton(label, ImVec2(w, h));
    bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    ImGui::EndDisabled();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiID aid = ImGui::GetItemID();
    float hv = anim(aid, hov && enabled ? 1.f : 0.f, 20.f);
    ImU32 bg = primary ? (act ? t.accent : col_lerp(t.accent, t.accent_hover, hv)) : (act ? t.border : col_lerp(t.panel2, t.raised, hv));
    if (!enabled) bg = (bg & 0x00FFFFFF) | 0x80000000;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, S(7));
    if (!primary) dl->AddRect(p, ImVec2(p.x + w, p.y + h), t.border, S(7), 0, 1.f);
    float cx = p.x + (w - (ts.x + (isz > 0 ? isz + S(6) : 0))) * 0.5f;
    ImU32 tc = primary ? t.accent_text : t.text;
    if (!enabled) tc = (tc & 0x00FFFFFF) | 0x70000000;
    if (isz > 0) { icon_draw(dl, ic, ImVec2(cx, p.y + (h - isz) * 0.5f), isz, enabled ? 1.f : .4f, primary); cx += isz + S(6); }
    dl->AddText(ImVec2(cx, p.y + (h - ts.y) * 0.5f), tc, label, end);
    return clicked && enabled;
}

void draw_logo(ImDrawList* dl, ImVec2 p, float L) {
    const GTheme& t = g_th;
    auto P = [&](float x, float y) { return ImVec2(p.x + x * L / 64.f, p.y + y * L / 64.f); };
    const float u = L / 64.f;
    // board: accent rounded square with a darker lower band (two-tone)
    ImU32 a = t.accent, a2 = (t.accent & 0x00FFFFFF) | 0xFF000000;
    ImU32 dark = t.dark ? IM_COL32(18, 40, 86, 255) : IM_COL32(16, 58, 140, 255);
    (void)a2;
    dl->AddRectFilled(P(4, 4), P(60, 60), a, 12 * u);
    dl->AddRectFilled(P(4, 31), P(60, 60), dark, 12 * u, ImDrawFlags_RoundCornersBottom);
    dl->AddRectFilled(P(4, 31), P(60, 34), (dark & 0x00FFFFFF) | 0x90000000);
    const ImU32 W = IM_COL32(255, 255, 255, 235), Wd = IM_COL32(255, 255, 255, 110), gold = IM_COL32(255, 205, 80, 255);
    // traces
    ImVec2 tr1[] = {P(13, 18), P(28, 18), P(36, 26), P(48, 26)};
    dl->AddPolyline(tr1, 4, W, 0, 3.2f * u);
    ImVec2 tr2[] = {P(13, 46), P(22, 46), P(30, 38)};
    dl->AddPolyline(tr2, 3, Wd, 0, 3.2f * u);
    dl->AddCircleFilled(P(13, 18), 4.2f * u, gold);
    dl->AddCircleFilled(P(48, 26), 4.2f * u, gold);
    dl->AddCircleFilled(P(13, 46), 4.2f * u, gold);
    // chip
    dl->AddRectFilled(P(30, 34), P(46, 50), IM_COL32(20, 24, 32, 230), 2.5f * u);
    for (int i = 0; i < 3; ++i) {
        float y = 37.5f + i * 4.5f;
        dl->AddLine(P(26.5f, y), P(30, y), W, 2 * u);
        dl->AddLine(P(46, y), P(49.5f, y), W, 2 * u);
    }
    dl->AddCircleFilled(P(33.5f, 37.5f), 1.6f * u, gold);
    // plotter crosshair
    dl->AddCircle(P(50, 48), 7.5f * u, W, 0, 2.4f * u);
    dl->AddLine(P(50, 37), P(50, 59), W, 1.8f * u);
    dl->AddLine(P(39, 48), P(61, 48), W, 1.8f * u);
}

void section_header(const char* text) {
    ImGui::PushFont(g_font_bold);
    ImGui::PushStyleColor(ImGuiCol_Text, g_th.text_dim);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

bool search_box(const char* id, char* buf, size_t n, const char* hint, float width) {
    ImGui::SetNextItemWidth(width);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x + S(20), ImGui::GetStyle().FramePadding.y));
    bool ch = ImGui::InputTextWithHint(id, hint, buf, n);
    ImGui::PopStyleVar();
    float is = ImGui::GetFontSize();
    icon_draw(ImGui::GetWindowDrawList(), ui::Icon::ZoomIn, ImVec2(p.x + S(6), p.y + (ImGui::GetFrameHeight() - is) * 0.5f), is, 0.6f);
    return ch;
}


// ===========================================================================
// v2.3 feedback: animation helpers, toasts, progress jobs, inline widgets
// ===========================================================================
HWND main_hwnd();
std::string strip_menu_u8(const wchar_t* t, std::string* shortcut);   // shell.cpp

namespace {
std::unordered_map<ImGuiID, float> g_anim;
std::unordered_map<ImGuiID, double> g_flash;
float dt() { float d = ImGui::GetIO().DeltaTime; return d > 0.1f ? 0.1f : d; }

struct Toast { std::string msg; ToastKind kind; float secs; std::string act; std::function<void()> fn; double t0 = -1; float life = 0; float y = -1; bool dying = false; float out = 0; int id; };
std::mutex g_tmx;
std::vector<Toast> g_toasts;      // guarded by g_tmx when touched off-thread (pending only)
std::vector<Toast> g_pending;
int g_toast_serial = 0;

struct Job {
    std::string title;
    std::shared_ptr<Progress> pr;
    std::function<void(bool)> done;
    ProgressOpts opts;
    std::atomic<int> state{0};    // 0 running, 1 ok, 2 failed
    std::string err;
    double t0 = 0;
    bool waited = false;
    std::thread th;
};
std::vector<std::shared_ptr<Job>> g_jobs;

void wake() { if (HWND h = main_hwnd()) PostMessageW(h, WM_NULL, 0, 0); }
ImU32 kind_col(ToastKind k) { const GTheme& t = g_th; return k == ToastKind::Success ? t.ok : k == ToastKind::Warning ? t.warn : k == ToastKind::Error ? t.error : t.accent; }
} // namespace

ui::Icon toast_icon(ToastKind k) { return k == ToastKind::Success ? ui::Icon::Check : k == ToastKind::Warning ? ui::Icon::Warning : k == ToastKind::Error ? ui::Icon::ErrorX : ui::Icon::About; }
ImU32 toast_color(ToastKind k) { return kind_col(k); }

ImU32 col_lerp(ImU32 a, ImU32 b, float t) {
    t = std::clamp(t, 0.f, 1.f);
    auto ch = [&](int sh) { float x = ((a >> sh) & 255), y = ((b >> sh) & 255); return (ImU32)(x + (y - x) * t + 0.5f) << sh; };
    return ch(0) | ch(8) | ch(16) | ch(24);
}
ImU32 col_alpha(ImU32 c, float a) { return (c & 0x00FFFFFF) | ((ImU32)(std::clamp(a, 0.f, 1.f) * 255.f + 0.5f) << 24); }

float anim(ImGuiID id, float target, float speed) {
    auto it = g_anim.find(id);
    if (it == g_anim.end() || g_perf) { g_anim[id] = target; return target; }
    float& v = it->second;
    float k = 1.f - std::exp(-speed * dt());
    v += (target - v) * k;
    if (std::fabs(target - v) < 0.002f) v = target; else request_redraw(2);
    return v;
}
float anim(const char* key, float target, float speed) { return anim(ImGui::GetID(key), target, speed); }
float pulse(float period) { if (g_perf) return 1.f; request_redraw(2); return 0.5f - 0.5f * std::cos((float)ImGui::GetTime() * 6.2831853f / period); }
void flash(ImGuiID key) { g_flash[key] = ImGui::GetTime(); request_redraw(2); }
float flash_amount(ImGuiID key, float secs) {
    auto it = g_flash.find(key);
    if (it == g_flash.end()) return 0;
    float e = (float)(ImGui::GetTime() - it->second) / secs;
    if (e >= 1) { g_flash.erase(it); return 0; }
    request_redraw(2);
    float a = 1 - e;
    return a * a;
}

// ---- Progress ----
void Progress::set(float f, const char* t) { frac = f; if (t) text(t); else wake(); }
void Progress::text(const char* t) { { std::lock_guard<std::mutex> l(mx); msg = t ? t : ""; } wake(); }
bool Progress::cancelled() const { return cancel.load(); }

bool busy() { return !g_jobs.empty(); }

void run_with_progress(const char* title, std::function<void(Progress&)> work, std::function<void(bool)> done, ProgressOpts opts) {
    auto j = std::make_shared<Job>();
    j->title = title ? title : "Working";
    j->pr = std::make_shared<Progress>();
    j->done = std::move(done);
    j->opts = opts;
    j->t0 = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0;
    std::weak_ptr<Job> wj = j;
    auto pr = j->pr;
    Job* raw = j.get();
    g_jobs.push_back(j);
    j->th = std::thread([raw, pr, work = std::move(work)]() mutable {
        int st = 1;
        try { work(*pr); if (pr->cancelled()) st = 2; }
        catch (const std::exception& e) { raw->err = e.what(); st = 2; }
        catch (...) { raw->err = "unknown error"; st = 2; }
        raw->state = st;
        wake();
    });
    request_redraw(2);
}

bool pump_and_render();   // shell.cpp
bool run_with_progress_wait(const char* title, std::function<void(Progress&)> work, ProgressOpts opts) {
    if (in_frame() || !ImGui::GetCurrentContext()) {   // cannot spin frames here: run inline
        Progress p;
        try { work(p); } catch (const std::exception& e) { toast(std::string(title) + " failed: " + e.what(), ToastKind::Error, 6.f); return false; }
        return !p.cancelled();
    }
    run_with_progress(title, std::move(work), {}, opts);
    std::shared_ptr<Job> j = g_jobs.back();
    j->waited = true;
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_APPSTARTING));
    while (j->state.load() == 0) {
        if (!pump_and_render()) break;
        request_redraw(2);
        if (j->state.load() == 0) MsgWaitForMultipleObjects(0, nullptr, FALSE, 15, QS_ALLINPUT);
    }
    SetCursor(old);
    if (j->th.joinable()) j->th.join();
    g_jobs.erase(std::remove(g_jobs.begin(), g_jobs.end(), j), g_jobs.end());
    request_redraw(3);
    if (!j->err.empty()) { toast(j->title + " failed: " + j->err, ToastKind::Error, 6.f); return false; }
    if (j->pr->cancelled()) { toast(j->title + " cancelled", ToastKind::Warning); return false; }
    return j->state.load() == 1;
}

// R12 (REVIEW_V11 §14): Appearance > Notifications filters. Errors always show; "Tip:" toasts only in tutorial mode
// and never again once closed by the user (Hint.toast.<hash>, cleared by Help > Reset Hints).
static std::string tip_key(const std::string& msg) { return "Hint.toast." + std::to_string((unsigned)ImHashStr(msg.c_str())); }
static bool toast_allowed(const std::string& msg, ToastKind kind, const char* action_label) {
    if (kind == ToastKind::Error) return true;
    const bool tip = msg.rfind("Tip:", 0) == 0;
    if (tip) return tutorial_mode() && app::ui_pref("NotifyTips", 1) && !app::ui_pref(tip_key(msg).c_str(), 0);
    if (action_label && *action_label) return app::ui_pref("NotifyShow", 1) != 0 || kind == ToastKind::Warning;   // actionable (Undo...)
    if (kind == ToastKind::Success) return app::ui_pref("NotifyConfirm", 1) != 0;
    return app::ui_pref("NotifyShow", 1) != 0;
}
void toast(const std::string& msg, ToastKind kind, float secs, const char* action_label, std::function<void()> action) {
    if (!toast_allowed(msg, kind, action_label)) return;
    Toast t{msg, kind, secs, action_label ? action_label : "", std::move(action)};
    { std::lock_guard<std::mutex> l(g_tmx); t.id = ++g_toast_serial; g_pending.push_back(std::move(t)); }
    wake();
    if (ImGui::GetCurrentContext() && GetCurrentThreadId() == GetWindowThreadProcessId(main_hwnd(), nullptr)) request_redraw(3);
}

// called once per frame by the shell (UI thread, inside the frame) AFTER all windows/modals
void draw_feedback() {
    const GTheme& t = g_th;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    // ---- jobs: finish ----
    for (size_t i = 0; i < g_jobs.size();) {
        auto j = g_jobs[i];
        int st = j->state.load();
        if (st == 0 || j->waited) { ++i; continue; }
        if (j->th.joinable()) j->th.join();
        g_jobs.erase(g_jobs.begin() + (long)i);
        bool ok = st == 1;
        if (!j->err.empty()) toast(j->title + " failed: " + j->err, ToastKind::Error, 6.f);
        else if (j->pr->cancelled()) toast(j->title + " cancelled", ToastKind::Warning);
        if (j->done) { auto fn = j->done; defer([fn, ok]() { fn(ok); }); }
        request_redraw(3);
    }
    // ---- job overlay (front-most job) ----
    if (!g_jobs.empty()) {
        auto& j = g_jobs.back();
        float el = (float)(ImGui::GetTime() - j->t0);
        request_redraw(2);
        if (el >= j->opts.delay) {
            float a = std::min(1.f, (el - j->opts.delay) / 0.18f);
            float e = 1 - (1 - a) * (1 - a);
            if (j->opts.modal) {
                ImGui::SetNextWindowPos(vp->Pos); ImGui::SetNextWindowSize(vp->Size);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, (t.dark ? 0.45f : 0.25f) * e));
                ImGui::SetNextWindowFocus();
                ImGui::Begin("##busy_dim", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav);
                ImGui::End();
                ImGui::PopStyleColor(); ImGui::PopStyleVar(2);
            }
            const float W = S(380);
            ImVec2 c(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.45f + (1 - e) * S(16));
            if (!j->opts.modal) c = ImVec2(vp->Pos.x + vp->Size.x - W * 0.5f - S(16), vp->Pos.y + vp->Size.y - S(90) + (1 - e) * S(16));
            ImGui::SetNextWindowPos(c, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(W, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, e);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(18), S(16)));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(12));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, t.panel2);
            if (j->opts.modal) ImGui::SetNextWindowFocus();
            ImGui::Begin("##busy_card", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
            spinner(ImGui::GetFontSize() * 0.55f);
            ImGui::SameLine(0, S(10));
            ImGui::PushFont(g_font_bold); ImGui::TextUnformatted(j->title.c_str()); ImGui::PopFont();
            std::string msg; { std::lock_guard<std::mutex> l(j->pr->mx); msg = j->pr->msg; }
            float f = j->pr->frac.load();
            ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim);
            ImGui::TextWrapped("%s", msg.empty() ? "Please wait..." : msg.c_str());
            ImGui::PopStyleColor();
            char pct[16]; std::snprintf(pct, sizeof pct, "%d%%", (int)(f * 100 + 0.5f));
            progress_bar(f, ImVec2(-1, S(8)));
            ImGui::PushStyleColor(ImGuiCol_Text, t.text_faint);
            ImGui::Text("%s  \xC2\xB7  %.1fs", f >= 0 ? pct : "working", el);
            ImGui::PopStyleColor();
            if (j->opts.cancellable) {
                ImGui::SameLine(ImGui::GetContentRegionMax().x - S(84));
                bool c2 = j->pr->cancelled();
                if (button(c2 ? "Cancelling" : "Cancel", ui::Icon::None, false, ImVec2(S(84), 0), !c2) || (!c2 && j->opts.modal && ImGui::IsKeyPressed(ImGuiKey_Escape))) j->pr->cancel = true;
            }
            ImGui::End();
            ImGui::PopStyleColor(); ImGui::PopStyleVar(3);
        }
    }
    // ---- toasts ----
    { std::lock_guard<std::mutex> l(g_tmx); for (auto& p : g_pending) g_toasts.push_back(std::move(p)); g_pending.clear(); }
    if (g_toasts.size() > 6) for (size_t i = 0; i + 6 < g_toasts.size(); ++i) g_toasts[i].dying = true;
    const float TW = S(360), gap = S(8);
    float y = vp->Pos.y + vp->Size.y - S(40);   // above the status bar
    // R12 (REVIEW_V11 §10): toasts never cover an open dialog (its footer buttons): stack above the dialog, or
    // beside it; toasts that fit nowhere wait (their life timer pauses) until the dialog closes.
    ImRect modal_r(0, 0, 0, 0);
    bool modal_on = false;
    if (ImGuiWindow* mw = ImGui::GetTopMostPopupModal()) { modal_r = mw->Rect(); modal_on = true; }
    const float TW0 = S(360);
    float toast_x = vp->Pos.x + vp->Size.x - TW0 - S(16);
    if (modal_on && toast_x < modal_r.Max.x + S(8) && toast_x + TW0 > modal_r.Min.x - S(8)) {
        if (modal_r.Max.x + S(16) + TW0 <= vp->Pos.x + vp->Size.x) toast_x = modal_r.Max.x + S(16);   // room on the right
        else if (modal_r.Min.x - S(16) - TW0 >= vp->Pos.x) toast_x = modal_r.Min.x - S(16) - TW0;     // room on the left
        else y = std::min(y, modal_r.Min.y - S(8));                                                    // above it
    }
    double now = ImGui::GetTime();
    for (int i = (int)g_toasts.size() - 1; i >= 0; --i) {
        Toast& o = g_toasts[i];
        if (o.t0 < 0) o.t0 = now;
        float age = (float)(now - o.t0);
        float in = std::min(1.f, age / 0.22f);
        in = 1 - (1 - in) * (1 - in) * (1 - in);
        if (o.dying) o.out = std::min(1.f, o.out + dt() / 0.18f);
        float vis = in * (1 - o.out);
        ImGui::PushFont(g_font);
        ImVec2 ts = ImGui::CalcTextSize(o.msg.c_str(), nullptr, false, TW - S(80));
        ImGui::PopFont();
        float h = std::max(S(48), ts.y + S(24)) + (o.act.empty() ? 0 : S(30));
        float target = y - h;
        if (o.y < 0) o.y = target;
        o.y += (target - o.y) * (1 - std::exp(-18.f * dt()));
        float x = toast_x + (1 - in) * S(40) + o.out * S(40);
        if (modal_on && o.y < vp->Pos.y) { y = o.y - gap; continue; }   // no room: not drawn, its life timer does not run
        char wid[32]; std::snprintf(wid, sizeof wid, "##toast%d", o.id);
        ImGui::SetNextWindowPos(ImVec2(x, o.y));
        ImGui::SetNextWindowSize(ImVec2(TW, h));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, vis);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
        ImGui::Begin(wid, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 a(x, o.y), b(x + TW, o.y + h);
        bool hov = ImGui::IsWindowHovered();
        ImU32 kc = kind_col(o.kind);
        dl->AddRectFilled(ImVec2(a.x + S(2), a.y + S(4)), ImVec2(b.x + S(2), b.y + S(5)), IM_COL32(0, 0, 0, (int)(60 * vis)), S(10));
        dl->AddRectFilled(a, b, hov ? t.raised : t.panel2, S(10));
        dl->AddRect(a, b, col_alpha(kc, 0.55f), S(10), 0, 1.f);
        dl->AddRectFilled(a, ImVec2(a.x + S(4), b.y), kc, S(10), ImDrawFlags_RoundCornersLeft);
        float isz = S(22);
        dl->AddCircleFilled(ImVec2(a.x + S(28), a.y + S(24)), S(15), col_alpha(kc, 0.18f), 24);
        icon_draw(dl, toast_icon(o.kind), ImVec2(a.x + S(28) - isz * 0.5f, a.y + S(24) - isz * 0.5f), isz);
        dl->AddText(g_font, ImGui::GetFontSize(), ImVec2(a.x + S(52), a.y + S(12) + std::max(0.f, (S(48) - S(24) - ts.y) * 0.5f)), t.text, o.msg.c_str(), nullptr, TW - S(80));
        // close x
        ImVec2 cx(b.x - S(16), a.y + S(16));
        ImU32 xc = hov ? t.text_dim : col_alpha(t.text_faint, 0.6f);
        dl->AddLine(ImVec2(cx.x - S(4), cx.y - S(4)), ImVec2(cx.x + S(4), cx.y + S(4)), xc, S(1.5f));
        dl->AddLine(ImVec2(cx.x + S(4), cx.y - S(4)), ImVec2(cx.x - S(4), cx.y + S(4)), xc, S(1.5f));
        // life bar
        if (o.secs > 0) {
            if (!hov && !o.dying) o.life += dt();
            float lf = std::clamp(1 - o.life / o.secs, 0.f, 1.f);
            dl->AddRectFilled(ImVec2(a.x + S(10), b.y - S(3)), ImVec2(a.x + S(10) + (TW - S(20)) * lf, b.y - S(1)), col_alpha(kc, 0.5f), S(1));
            if (o.life >= o.secs) o.dying = true;
        }
        bool acted = false;
        if (!o.act.empty()) {
            ImGui::SetCursorScreenPos(ImVec2(a.x + S(52), b.y - S(34)));
            if (button(o.act.c_str(), ui::Icon::None, true, ImVec2(0, S(26)))) { acted = true; if (o.fn) defer(o.fn); o.dying = true; }
        }
        if (!acted && hov && ImGui::IsMouseReleased(0)) { o.dying = true; if (o.msg.rfind("Tip:", 0) == 0) app::set_ui_pref(tip_key(o.msg).c_str(), 1); }   // dismissed tips never return
        ImGui::End();
        ImGui::PopStyleColor(); ImGui::PopStyleVar(3);
        y = o.y - gap;
        request_redraw(2);
    }
    g_toasts.erase(std::remove_if(g_toasts.begin(), g_toasts.end(), [](const Toast& o) { return o.dying && o.out >= 1; }), g_toasts.end());
}

void progress_bar(float frac, ImVec2 size, const char* overlay) {
    const GTheme& t = g_th;
    if (size.x <= 0) size.x = ImGui::GetContentRegionAvail().x + (size.x < 0 ? size.x + 1 : 0);
    if (size.x <= 0) size.x = S(100);
    if (size.y <= 0) size.y = overlay ? ImGui::GetFrameHeight() : S(6);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 b(p.x + size.x, p.y + size.y);
    float r = std::min(size.y * 0.5f, S(6));
    dl->AddRectFilled(p, b, t.dark ? col_alpha(t.field, 1) : t.raised, r);
    if (frac >= 0) {
        float f = anim(ImGui::GetID("##progress_bar"), std::clamp(frac, 0.f, 1.f), 10.f);
        if (f > 0.001f) {
            dl->AddRectFilled(p, ImVec2(p.x + std::max(size.x * f, r * 2), b.y), t.accent, r);
            // sheen
            float sx = std::fmod((float)ImGui::GetTime() * S(220), size.x * f + S(80)) - S(40);
            dl->PushClipRect(p, ImVec2(p.x + size.x * f, b.y), true);
            dl->AddRectFilledMultiColor(ImVec2(p.x + sx - S(30), p.y), ImVec2(p.x + sx, b.y), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 70), IM_COL32(255, 255, 255, 70), IM_COL32(255, 255, 255, 0));
            dl->AddRectFilledMultiColor(ImVec2(p.x + sx, p.y), ImVec2(p.x + sx + S(30), b.y), IM_COL32(255, 255, 255, 70), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 70));
            dl->PopClipRect();
        }
    } else {
        float w = size.x * 0.32f;
        float ph = std::fmod((float)ImGui::GetTime() * 0.9f, 1.f);
        float e = ph < 0.5f ? 2 * ph * ph : 1 - 2 * (1 - ph) * (1 - ph);
        float x0 = p.x - w + (size.x + w) * e;
        dl->PushClipRect(p, b, true);
        dl->AddRectFilled(ImVec2(x0, p.y), ImVec2(x0 + w, b.y), t.accent, r);
        dl->PopClipRect();
    }
    if (overlay) {
        ImVec2 ts = ImGui::CalcTextSize(overlay);
        dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f), t.text, overlay);
    }
    request_redraw(2);
}

void spinner(float radius, ImU32 col) {
    if (radius <= 0) radius = ImGui::GetTextLineHeight() * 0.5f;
    if (!col) col = g_th.accent;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = std::max(radius * 2, ImGui::GetTextLineHeight());
    ImGui::Dummy(ImVec2(radius * 2, h));
    ImVec2 c(p.x + radius, p.y + h * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float tm = (float)ImGui::GetTime();
    float th0 = std::max(1.5f, radius * 0.28f);
    dl->PathArcTo(c, radius - th0 * 0.5f, 0, 6.2831853f, 32);
    dl->PathStroke(col_alpha(col, 0.18f), 0, th0);
    float a0 = tm * 6.0f, len = 1.6f + 1.2f * std::sin(tm * 2.3f);
    dl->PathArcTo(c, radius - th0 * 0.5f, a0, a0 + len, 24);
    dl->PathStroke(col, 0, th0);
    request_redraw(2);
}

bool menu_item_icon(const char* label, ui::Icon ic, const char* shortcut, bool checked, bool enabled) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float fs = ImGui::GetFontSize();
    std::string l = std::string("      ") + label;
    bool r = ImGui::MenuItem(l.c_str(), shortcut, false, enabled);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float is = std::round(fs * 1.1f);
    ImVec2 ip(p.x - S(1), p.y + (ImGui::GetTextLineHeight() - is) * 0.5f);
    if (checked) {
        dl->AddRectFilled(ImVec2(ip.x - S(2), ip.y - S(2)), ImVec2(ip.x + is + S(2), ip.y + is + S(2)), col_alpha(g_th.accent, 0.22f), S(4));
        dl->AddRect(ImVec2(ip.x - S(2), ip.y - S(2)), ImVec2(ip.x + is + S(2), ip.y + is + S(2)), col_alpha(g_th.accent, 0.7f), S(4));
        if (ic == ui::Icon::None) ic = ui::Icon::Check;
    }
    if (ic != ui::Icon::None) icon_draw(dl, ic, ip, is, enabled ? 1.f : 0.35f);
    return r;
}

bool begin_menu_icon(const char* label, ui::Icon ic, bool enabled) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    std::string l = std::string("      ") + label;
    float is = std::round(ImGui::GetFontSize() * 1.1f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool r = ImGui::BeginMenu(l.c_str(), enabled);
    if (ic != ui::Icon::None) icon_draw(dl, ic, ImVec2(p.x - S(1), p.y + (ImGui::GetTextLineHeight() - is) * 0.5f), is, enabled ? 1.f : 0.35f);
    return r;
}

bool cmd_button(int id) {
    const app::CmdInfo* ci = app::cmd_info(id);
    if (!ci) return false;
    int st = command_state(id);
    std::string sc;
    std::string tip = strip_menu_u8(ci->text, &sc);
    if (!sc.empty()) tip += "  (" + sc + ")";
    if (ci->tip && *ci->tip) tip += "\n" + wide_to_utf8(ci->tip);
    ImGui::PushID(id);
    bool r = icon_button("b", ci->icon, tip.c_str(), (st & 2) != 0, (st & 1) != 0);
    ImGui::PopID();
    if (r) post_command(id);
    ImGui::SameLine(0, S(2));
    return r;
}

void toolbar_separator() {
    ImGui::SameLine(0, S(6));
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + S(6)), ImVec2(p.x, p.y + S(24)), g_th.border, 1.f);
    ImGui::Dummy(ImVec2(S(1), SI(30)));
    ImGui::SameLine(0, S(6));
}

bool empty_state(ui::Icon ic, const char* title, const char* hint, const char* blabel, ui::Icon bicon) {
    const GTheme& t = g_th;
    ImVec2 av = ImGui::GetContentRegionAvail();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float big = S(64);
    float th_ = ImGui::GetTextLineHeight();
    float hint_w = std::min(av.x - S(40), S(420));
    ImVec2 hs = hint ? ImGui::CalcTextSize(hint, nullptr, false, hint_w) : ImVec2(0, 0);
    float total = big + S(18) + th_ + (hint ? S(8) + hs.y : 0) + (blabel ? S(18) + ImGui::GetFrameHeight() : 0);
    float y = p.y + std::max(S(10), (av.y - total) * 0.45f);
    float cx = p.x + av.x * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float bob = std::sin((float)ImGui::GetTime() * 1.6f) * S(2);
    dl->AddCircleFilled(ImVec2(cx, y + big * 0.5f), big * 0.72f, col_alpha(t.accent, 0.08f), 48);
    icon_draw(dl, ic, ImVec2(cx - big * 0.5f, y + bob), big, 0.75f);
    y += big + S(18);
    ImGui::PushFont(g_font_bold);
    ImVec2 ts = ImGui::CalcTextSize(title);
    dl->AddText(ImVec2(cx - ts.x * 0.5f, y), t.text, title);
    ImGui::PopFont();
    y += th_;
    if (hint) {
        y += S(8);
        ImGui::SetCursorScreenPos(ImVec2(cx - hint_w * 0.5f, y));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + hint_w);
        ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim);
        // centre each wrapped line: simple approach for single/short hints
        if (hs.x < hint_w) ImGui::SetCursorScreenPos(ImVec2(cx - hs.x * 0.5f, y));
        ImGui::TextWrapped("%s", hint);
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        y += hs.y;
    }
    bool clicked = false;
    if (blabel) {
        y += S(18);
        ImVec2 bs = ImGui::CalcTextSize(blabel);
        float bw = bs.x + S(30) + (bicon != ui::Icon::None ? S(24) : 0);
        ImGui::SetCursorScreenPos(ImVec2(cx - bw * 0.5f, y));
        clicked = button(blabel, bicon, true, ImVec2(bw, 0));
    }
    request_redraw(2);
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(av);
    return clicked;
}

bool confirm(const char* title, const std::string& text, const char* yes, bool danger) {
    ui::Form f(utf8_to_wide(title ? title : "PCB Plotter"), 420);
    f.icon((int)(danger ? ui::Icon::Warning : ui::Icon::Question));
    f.label(utf8_to_wide(text));
    f.ok_text(utf8_to_wide(yes ? yes : "OK"));
    return f.run(main_hwnd());
}

bool confirm_info(const char* key, const char* title, const std::string& text, const char* yes) {
    const std::string k = std::string("Confirm.") + key;
    if (app::ui_pref(k.c_str(), 0)) return true;
    ui::Form f(utf8_to_wide(title ? title : "PCB Plotter"), 440);
    f.icon((int)ui::Icon::About);
    f.label(utf8_to_wide(text));
    f.check("dsa", L"Don't show this message again", false).tip(L"Settings > Reset Suppressed Warnings shows it again");
    f.ok_text(utf8_to_wide(yes ? yes : "OK"));
    const bool ok = f.run(main_hwnd());
    if (ok && f.checked("dsa")) app::set_ui_pref(k.c_str(), 1);
    return ok;
}
static int g_kbd_claim_frame = -10;
void claim_keyboard() { g_kbd_claim_frame = ImGui::GetFrameCount(); }
bool keyboard_claimed() { return ImGui::GetFrameCount() - g_kbd_claim_frame <= 1; }
int reset_suppressed_warnings() { return app::reset_ui_prefs("Confirm."); }

bool tutorial_mode() { return app::ui_pref("TutorialMode", 0) != 0; }
void set_tutorial_mode(bool on) { app::set_ui_pref("TutorialMode", on ? 1 : 0); request_redraw(2); }
bool hint_shown(const char* key) { return tutorial_mode() && !app::ui_pref((std::string("Hint.") + key).c_str(), 0); }
void hint_dismiss(const char* key) { app::set_ui_pref((std::string("Hint.") + key).c_str(), 1); request_redraw(2); }
int hints_reset() { request_redraw(2); return app::reset_ui_prefs("Hint."); }
namespace { struct HintRect { ImRect r; int frame; }; std::unordered_map<std::string, HintRect> g_hint_rects; }
bool hint_hit(ImVec2 p) {
    const int f = ImGui::GetFrameCount();
    for (auto& [k, h] : g_hint_rects) if (h.frame >= f - 1 && h.r.Contains(p)) return true;
    return false;
}
bool hint_pill(ImDrawList* dl, const char* key, const std::string& text, ImVec2 anchor, bool bottom, ui::Icon ic, float a) {
    if (!hint_shown(key) || text.empty()) return false;
    const GTheme& t = th();
    const float is = ImGui::GetFontSize(), xw = is + S(8);
    const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    const float w = ts.x + is + S(26) + xw, h = ts.y + S(12);
    ImVec2 p0(anchor.x - w * 0.5f, bottom ? anchor.y - h : anchor.y), p1(p0.x + w, p0.y + h);
    const float r = h * 0.5f;
    dl->AddRectFilled(p0, p1, col_alpha(t.panel, 0.94f * a), r);
    dl->AddRect(p0, p1, col_alpha(t.accent, 0.8f * a), r, 0, S(1.2f));
    icon_draw(dl, ic, ImVec2(p0.x + S(10), p0.y + (h - is) * 0.5f), is, a);
    dl->AddText(ImVec2(p0.x + S(16) + is, p0.y + S(6)), col_alpha(t.text, a), text.c_str());
    // dismiss (x)
    const ImVec2 xc(p1.x - xw * 0.5f - S(4), (p0.y + p1.y) * 0.5f);
    const ImVec2 mp = ImGui::GetIO().MousePos;
    const bool xh = std::fabs(mp.x - xc.x) < xw * 0.5f && std::fabs(mp.y - xc.y) < h * 0.5f;
    if (xh) dl->AddCircleFilled(xc, is * 0.5f, col_alpha(t.raised, a), 16);
    const float k = is * 0.22f;
    dl->AddLine(ImVec2(xc.x - k, xc.y - k), ImVec2(xc.x + k, xc.y + k), col_alpha(xh ? t.text : t.text_dim, a), S(1.5f));
    dl->AddLine(ImVec2(xc.x - k, xc.y + k), ImVec2(xc.x + k, xc.y - k), col_alpha(xh ? t.text : t.text_dim, a), S(1.5f));
    if (xh && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) hint_dismiss(key);
    if (xh) ImGui::SetTooltip("Hide this hint (Help > Tutorial Mode turns all hints off; Help > Reset Hints shows them again)");
    g_hint_rects[key] = HintRect{ImRect(p0, p1), ImGui::GetFrameCount()};
    return true;
}

} // namespace pcbp::gui
