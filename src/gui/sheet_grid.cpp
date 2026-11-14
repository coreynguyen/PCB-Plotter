// ============================================================================
// sheet_grid.cpp — spreadsheet grid: styled cell painter (fonts, colours,
// fills, borders, alignment, wrap, merges, number formats, conditional
// formatting incl. data bars), frozen panes, headers with resize / autofit,
// selection (multi-range), fill handle, drag-move, point mode (click cells
// while editing a formula), autofilter + validation dropdowns, smooth
// scrolling + Ctrl+wheel zoom, context menus and the keyboard map.
// Only visible cells are looked up / formatted (looks cached per book gen).
// ============================================================================
#include "sheet_editor.h"
#include "../app/commands.h"
#include "../ui/win.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>

namespace pcbp::gui {
using namespace sheet;

static float lum(uint32_t argb) {
    float r = ((argb >> 16) & 255) / 255.f, g = ((argb >> 8) & 255) / 255.f, b = (argb & 255) / 255.f;
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}
// resolve a document text colour against the painted background (auto / dark-mode contrast)
static ImU32 text_col(uint32_t c, uint32_t fill) {
    const GTheme& t = th();
    if (fill) {
        if (!c) return lum(fill) > 0.5f ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);
        return doc_col(c);
    }
    if (!c) return t.text;
    if (t.dark && lum(c) < 0.22f) return t.text;           // black-on-dark: keep readable
    if (!t.dark && lum(c) > 0.92f) return t.text;           // white-on-white
    return doc_col(c);
}
static ImU32 border_col(uint32_t c) {
    if (!c) return th().dark ? IM_COL32(200, 200, 200, 255) : IM_COL32(0, 0, 0, 255);
    if (th().dark && lum(c) < 0.2f) return IM_COL32(200, 200, 200, 255);
    return doc_col(c);
}
static ImFont* cell_font(const Font& f) {
    const std::string& n = f.name;
    if (g_font_mono && (n.find("Consol") != std::string::npos || n.find("Courier") != std::string::npos || n.find("Mono") != std::string::npos)) return g_font_mono;
    if (f.bold && g_font_bold) return g_font_bold;
    return g_font ? g_font : ImGui::GetFont();
}

static void dashed(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float th_, float on, float off, float phase = 0) {
    float len = std::hypot(b.x - a.x, b.y - a.y);
    if (len <= 0) return;
    ImVec2 d((b.x - a.x) / len, (b.y - a.y) / len);
    float p = -std::fmod(phase, on + off);
    for (; p < len; p += on + off) {
        float s = std::max(0.f, p), e = std::min(len, p + on);
        if (e > s) dl->AddLine(ImVec2(a.x + d.x * s, a.y + d.y * s), ImVec2(a.x + d.x * e, a.y + d.y * e), col, th_);
    }
}
static void border_line(ImDrawList* dl, ImVec2 a, ImVec2 b, const BorderSide& s, bool horiz) {
    if (s.style == BorderStyle::None) return;
    ImU32 c = border_col(s.color);
    float u = std::max(1.f, S(1));
    switch (s.style) {
    case BorderStyle::Thin: dl->AddLine(a, b, c, u); break;
    case BorderStyle::Hair: dl->AddLine(a, b, col_alpha(c, 0.6f), u); break;
    case BorderStyle::Medium: dl->AddLine(a, b, c, 2 * u); break;
    case BorderStyle::Thick: dl->AddLine(a, b, c, 3 * u); break;
    case BorderStyle::Double: {
        ImVec2 o = horiz ? ImVec2(0, u * 1.5f) : ImVec2(u * 1.5f, 0);
        dl->AddLine(a - o, b - o, c, u); dl->AddLine(a + o, b + o, c, u); break;
    }
    case BorderStyle::Dotted: dashed(dl, a, b, c, u, u, u * 1.5f); break;
    case BorderStyle::Dashed: dashed(dl, a, b, c, u, 4 * u, 2 * u); break;
    case BorderStyle::MediumDashed: dashed(dl, a, b, c, 2 * u, 6 * u, 3 * u); break;
    case BorderStyle::DashDot: case BorderStyle::DashDotDot: dashed(dl, a, b, c, u, 6 * u, 2 * u); break;
    case BorderStyle::MediumDashDot: case BorderStyle::MediumDashDotDot: case BorderStyle::SlantDashDot: dashed(dl, a, b, c, 2 * u, 6 * u, 2 * u); break;
    default: dl->AddLine(a, b, c, u);
    }
}

// text with style: font, italic shear, underline / strike. Returns drawn width.
static void styled_text(ImDrawList* dl, const Font& f, float fs, ImVec2 pos, ImU32 col, const char* b, const char* e, const ImVec4& clip, float wrap = 0) {
    ImFont* font = font_for_px(cell_font(f), fs);   // bake near the destination size: no magnified 1x glyphs (§3)
    int v0 = dl->VtxBuffer.Size;
    dl->AddText(font, fs, pos, col, b, e, wrap, &clip);
    bool synth_bold = f.bold && font != g_font_bold;
    if (synth_bold) dl->AddText(font, fs, ImVec2(pos.x + std::max(1.f, fs / 18.f), pos.y), col, b, e, wrap, &clip);
    if (f.italic) {
        float base = pos.y + fs;
        for (int i = v0; i < dl->VtxBuffer.Size; ++i) dl->VtxBuffer[i].pos.x += (base - dl->VtxBuffer[i].pos.y) * 0.2f;
    }
    if (f.underline != Underline::None || f.strike) {
        ImVec2 sz = font->CalcTextSizeA(fs, FLT_MAX, wrap, b, e);
        float x1 = std::min(pos.x + sz.x, clip.z);
        float x0 = std::max(pos.x, clip.x);
        if (x1 > x0) {
            dl->PushClipRect(ImVec2(clip.x, clip.y), ImVec2(clip.z, clip.w), true);
            float lw = std::max(1.f, fs / 14.f);
            if (f.underline != Underline::None) {
                float y = pos.y + fs * 0.98f;
                dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), col, lw);
                if (f.underline == Underline::Double || f.underline == Underline::DoubleAccounting) dl->AddLine(ImVec2(x0, y + lw * 2), ImVec2(x1, y + lw * 2), col, lw);
            }
            if (f.strike) dl->AddLine(ImVec2(x0, pos.y + fs * 0.55f), ImVec2(x1, pos.y + fs * 0.55f), col, lw);
            dl->PopClipRect();
        }
    }
}

// ===========================================================================
void SheetDoc::draw_grid(ImVec2 size) {
    const GTheme& t = th();
    SheetUi& U = su();
    Worksheet& W = ws();
    const float z = zoomf();
    ensure_layout(z);
    const Layout& L = lay;
    ImGuiIO& io = ImGui::GetIO();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.field);
    ImGui::BeginChild("##grid", ImVec2(size.x, std::max(size.y, S(60))), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    if (focus_grid) { ImGui::SetWindowFocus(); focus_grid = false; }
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float hz = std::clamp(z, 0.75f, 1.5f);
    const float CH = show_headers ? std::round((ImGui::GetTextLineHeight() + S(6)) * hz) : 0;
    char nb[32];
    std::snprintf(nb, sizeof nb, "%d", L.rows.empty() ? 1 : L.rows.back() + 1);
    const float hfs = ImGui::GetFontSize() * hz;
    const float RW = show_headers ? std::max(S(36) * hz, ImGui::GetFont()->CalcTextSizeA(hfs, FLT_MAX, 0, nb).x + S(14)) : 0;
    const int NR = (int)L.rows.size();
    const int NC = L.nc;
    ImGui::Dummy(ImVec2(RW + L.cx[NC] + S(40), CH + L.ry[NR] + S(40)));
    if (U.restore_scroll) { ImGui::SetScrollX(U.sx); ImGui::SetScrollY(U.sy); U.restore_scroll = false; U.lsx = U.tsx = U.sx; U.lsy = U.tsy = U.sy; }
    // ---- smooth scrolling (wheel) + Ctrl+wheel zoom
    {
        float cy = ImGui::GetScrollY(), cxs = ImGui::GetScrollX();
        if (std::fabs(cy - U.lsy) > 0.5f) U.tsy = cy;     // scrollbar / programmatic change
        if (std::fabs(cxs - U.lsx) > 0.5f) U.tsx = cxs;
        if (ImGui::IsWindowHovered() && (io.MouseWheel != 0 || io.MouseWheelH != 0)) {
            if (io.KeyCtrl) {
                int st = (io.MouseWheel > 0 ? W.zoom >= 200 : W.zoom > 200) ? 25 : 10;   // coarser steps above 200 %
                int zz = std::clamp(W.zoom + (io.MouseWheel > 0 ? st : -st), 25, 400);
                W.zoom = zz;
            } else {
                float rh = std::round(20.f * dpi_scale() * z);
                if (io.KeyShift) U.tsx -= io.MouseWheel * rh * 6;
                else U.tsy -= io.MouseWheel * rh * 3;
                U.tsx -= io.MouseWheelH * rh * 4;
            }
        }
        U.tsy = std::clamp(U.tsy, 0.f, ImGui::GetScrollMaxY());
        U.tsx = std::clamp(U.tsx, 0.f, ImGui::GetScrollMaxX());
        ImGuiID sid = ImGui::GetID("##smooth");
        float ny = anim(sid, U.tsy, 22.f), nx = anim(sid + 7, U.tsx, 22.f);
        if (std::fabs(ny - U.tsy) < 0.5f) ny = U.tsy;
        if (std::fabs(nx - U.tsx) < 0.5f) nx = U.tsx;
        if (std::fabs(ny - cy) > 0.01f) ImGui::SetScrollY(ny);
        if (std::fabs(nx - cxs) > 0.01f) ImGui::SetScrollX(nx);
        U.lsy = ny; U.lsx = nx;
    }
    const float sx = U.lsx, sy = U.lsy;
    U.sx = sx; U.sy = sy;
    const ImRect in = win->InnerRect;
    const ImVec2 o = in.Min;
    const float VW = in.GetWidth(), VH = in.GetHeight();
    const float X0 = o.x + RW, Y0 = o.y + CH;          // top-left of cell area
    const float XS = X0 + L.fw, YS = Y0 + L.fh;          // top-left of the scrolling pane

    // ---- coordinate helpers
    auto colX = [&](int c) { return X0 + L.cx[c] - (c >= L.fc ? sx : 0.f); };
    auto rowI = [&](int r) { return (int)(std::lower_bound(L.rows.begin(), L.rows.end(), r) - L.rows.begin()); };
    auto rowYi = [&](int i) { return Y0 + L.ry[i] - (i >= L.fri ? sy : 0.f); };
    auto rowY = [&](int r) { return rowYi(std::min(rowI(r), NR)); };
    auto col_at_x = [&](float x) {
        float cxv = x < XS ? x - X0 : x - X0 + sx;
        int c = (int)(std::upper_bound(L.cx.begin(), L.cx.end(), cxv) - L.cx.begin()) - 1;
        c = std::clamp(c, 0, NC - 1);
        while (c > 0 && L.cx[c + 1] - L.cx[c] <= 0) --c;   // hidden column: report the previous
        return c;
    };
    auto row_at_y = [&](float y) {
        float cyv = y < YS ? y - Y0 : y - Y0 + sy;
        int i = (int)(std::upper_bound(L.ry.begin(), L.ry.end(), cyv) - L.ry.begin()) - 1;
        i = std::clamp(i, 0, std::max(0, NR - 1));
        return NR ? L.rows[i] : 0;
    };
    // visible ranges per pane
    auto vis_cols = [&](bool frozen, int& c0, int& c1) {
        if (frozen) { c0 = 0; c1 = L.fc - 1; return; }
        if (L.fc >= NC) { c0 = 1; c1 = 0; return; }
        float a = L.fw + sx, bb = sx + VW - RW;
        c0 = (int)(std::upper_bound(L.cx.begin(), L.cx.end(), a) - L.cx.begin()) - 1;
        c1 = (int)(std::upper_bound(L.cx.begin(), L.cx.end(), bb) - L.cx.begin()) - 1;
        c0 = std::clamp(c0, L.fc, NC - 1); c1 = std::clamp(c1, c0, NC - 1);
    };
    auto vis_rows = [&](bool frozen, int& i0, int& i1) {
        if (frozen) { i0 = 0; i1 = L.fri - 1; return; }
        if (L.fri >= NR) { i0 = 1; i1 = 0; return; }
        float a = L.fh + sy, bb = sy + VH - CH;
        i0 = (int)(std::upper_bound(L.ry.begin(), L.ry.end(), a) - L.ry.begin()) - 1;
        i1 = (int)(std::upper_bound(L.ry.begin(), L.ry.end(), bb) - L.ry.begin()) - 1;
        i0 = std::clamp(i0, L.fri, std::max(L.fri, NR - 1)); i1 = std::clamp(i1, i0, std::max(0, NR - 1));
    };

    const float base_fs = 11.f * 96.f / 72.f * dpi_scale() * z;
    const ImU32 grid_c = t.dark ? col_alpha(t.border, 0.75f) : IM_COL32(218, 220, 224, 255);
    const bool show_grid = W.show_grid;
    const bool cond_any = !W.cond.empty();
    const float pad = std::max(2.f, S(3) * z);

    // editing formula -> ref boxes
    std::vector<UsedRef> eref;
    if (editing && ebuf[0] == '=' && esi < (int)b.sheets.size()) eref = formula_refs(std::string_view(ebuf.data() + 1), b, esi, er, ec);

    // ================================================================ panes
    struct Ent { int r, c; float x, y, w, h; const CellLook* lk; };
    std::vector<Ent> ents;
    std::vector<Range> merges_vis;
    auto paint_pane = [&](bool fr_rows, bool fr_cols) {
        int i0, i1, c0, c1;
        vis_rows(fr_rows, i0, i1);
        vis_cols(fr_cols, c0, c1);
        if (i1 < i0 || c1 < c0) return;
        float px0 = fr_cols ? X0 : XS, py0 = fr_rows ? Y0 : YS;
        float px1 = fr_cols ? XS : in.Max.x, py1 = fr_rows ? YS : in.Max.y;
        if (px1 <= px0 || py1 <= py0) return;
        ImVec4 pclip(px0, py0, px1, py1);
        dl->PushClipRect(ImVec2(px0, py0), ImVec2(px1, py1), true);
        ents.clear();
        // merges intersecting this pane
        merges_vis.clear();
        Range pv(L.rows[i0], c0, L.rows[i1], c1);
        for (auto& m : W.merges) if (m.intersects(pv)) merges_vis.push_back(m);
        auto in_merge = [&](int r, int c) { for (auto& m : merges_vis) if (m.contains(r, c)) return true; return false; };
        // 1. collect + fills
        for (int i = i0; i <= i1; ++i) {
            int r = L.rows[i];
            float y = rowYi(i), h = L.ry[i + 1] - L.ry[i];
            if (h <= 0) continue;
            const bool row_has = r < (int)W.rows.size() && (!W.rows[r].cells.empty() || W.rows[r].style);
            for (int c = c0; c <= c1; ++c) {
                float w = L.cx[c + 1] - L.cx[c];
                if (w <= 0) continue;
                if (!merges_vis.empty() && in_merge(r, c)) continue;
                bool has = (row_has && (c < (int)W.rows[r].cells.size() || W.rows[r].style)) || (c < (int)W.cols.size() && W.cols[c].style) || cond_any;
                if (!has && W.spills.empty()) continue;
                const CellLook& lk = look(r, c);
                float x = colX(c);
                if (lk.fill_color) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), doc_col(lk.fill_color));
                ents.push_back({r, c, x, y, w, h, &lk});
            }
        }
        // 2. gridlines (under content, skipped over fills)
        if (show_grid) {
            for (int i = i0; i <= i1; ++i) {
                float y = rowYi(i + 1 <= NR ? i : i) + (L.ry[i + 1] - L.ry[i]) - 0.5f;
                dl->AddLine(ImVec2(px0, y), ImVec2(px1, y), grid_c);
            }
            for (int c = c0; c <= c1; ++c) {
                if (L.cx[c + 1] - L.cx[c] <= 0) continue;
                float x = colX(c + 1) - 0.5f;
                dl->AddLine(ImVec2(x, py0), ImVec2(x, py1), grid_c);
            }
            for (auto& e : ents) if (e.lk->fill_color) dl->AddRectFilled(ImVec2(e.x, e.y), ImVec2(e.x + e.w - 1, e.y + e.h - 1), doc_col(e.lk->fill_color));
        }
        // 3. merged blocks
        struct MEnt { Range m; float x, y, w, h; CellLook lk; };
        std::vector<MEnt> ments;
        for (auto& m : merges_vis) {
            float x = colX(m.c0), x2 = colX(m.c1 + 1);
            int ia = rowI(m.r0), ib = rowI(m.r1 + 1);
            float y = rowYi(std::min(ia, NR)), y2 = rowYi(std::min(ib, NR));
            if (m.c0 < L.fc && m.c1 >= L.fc) x2 = X0 + L.cx[m.c1 + 1] - sx;
            if (x2 <= x || y2 <= y) continue;
            MEnt me{m, x, y, x2 - x, y2 - y, b.look(cur, m.r0, m.c0)};
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x2 - 1, y2 - 1), me.lk.fill_color ? doc_col(me.lk.fill_color) : t.field);
            ments.push_back(std::move(me));
        }
        // 4. text
        auto draw_text = [&](int r, int c, float x, float y, float w, float h, const CellLook& lk, bool merged) {
            if (lk.bar >= 0) {
                float bw = std::max(0.f, (w - 4) * std::clamp(lk.bar, 0.f, 1.f));
                ImU32 bc = doc_col(lk.bar_color ? lk.bar_color : 0xFF638EC6);
                dl->AddRectFilledMultiColor(ImVec2(x + 2, y + 2), ImVec2(x + 2 + bw, y + h - 3), bc, col_alpha(bc, 0.25f), col_alpha(bc, 0.25f), bc);
            }
            if (lk.text.empty()) return;
            const Style& st = lk.style;
            float fs = base_fs * (float)(st.font.size / 11.0);
            ImFont* font = cell_font(st.font);
            ImU32 col = text_col(lk.text_color, lk.fill_color);
            const char* tb = lk.text.c_str();
            const char* te = tb + std::min(lk.text.size(), (size_t)4000);
            float ind = st.indent * 9.f * dpi_scale() * z;
            float avail = w - 2 * pad - ind;
            bool wrap = st.wrap || lk.text.find('\n') != std::string::npos;
            ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, wrap ? std::max(avail, 1.f) : 0.f, tb, te);
            HAlign ha = lk.align;
            if (ha == HAlign::General) ha = HAlign::Left;
            std::string hashes;
            float clip_l = x, clip_r = x + w - 1;
            if (!wrap && ts.x > avail) {
                const Value& v = W.value(r, c);
                if ((v.is_num() || v.is_bool()) && !merged && !lk.style.shrink) {   // numbers never overflow: ####
                    float hw = font->CalcTextSizeA(fs, FLT_MAX, 0, "#").x;
                    hashes.assign((size_t)std::max(1.f, avail / std::max(1.f, hw)), '#');
                    tb = hashes.c_str(); te = tb + hashes.size();
                    ts = font->CalcTextSizeA(fs, FLT_MAX, 0, tb, te);
                } else if (lk.overflow_ok && !merged) {
                    // overflow into empty neighbours (within the pane)
                    auto empty_at = [&](int cc) { const Cell* ce = W.cell(r, cc); return (!ce || ce->v.is_empty()) && !W.merge_at(r, cc) && W.spill_anchor(r, cc).first < 0; };
                    if (ha == HAlign::Left || ha == HAlign::Center) {
                        float need = x + pad + ind + ts.x;
                        for (int cc = c + 1; cc < NC && clip_r < need && clip_r < px1; ++cc) { if (!empty_at(cc)) break; clip_r = colX(cc + 1) - 1; }
                    }
                    if (ha == HAlign::Right || ha == HAlign::Center) {
                        float need = x + w - pad - ts.x;
                        for (int cc = c - 1; cc >= 0 && clip_l > need && clip_l > px0; --cc) { if (!empty_at(cc)) break; clip_l = colX(cc); }
                    }
                }
            }
            if (lk.style.shrink && !wrap && ts.x > avail && avail > 4) { fs *= avail / ts.x; ts = font->CalcTextSizeA(fs, FLT_MAX, 0, tb, te); }
            float tx;
            switch (ha) {
            case HAlign::Right: tx = x + w - pad - ts.x - ind; break;
            case HAlign::Center: case HAlign::CenterContinuous: case HAlign::Distributed: case HAlign::Justify: tx = x + (w - ts.x) * 0.5f; break;
            default: tx = x + pad + ind;
            }
            if (lk.dropdown && r == ar() && c == ac()) {}   // arrow drawn later
            float ty;
            switch (st.valign) {
            case VAlign::Top: ty = y + pad * 0.5f; break;
            case VAlign::Center: case VAlign::Justify: case VAlign::Distributed: ty = y + (h - ts.y) * 0.5f; break;
            default: ty = y + h - ts.y - std::max(1.f, pad * 0.5f);
            }
            if (!wrap) ty = std::max(ty, y + (h - ts.y) * (st.valign == VAlign::Top ? 0.f : 0.5f) * (h < ts.y ? 1.f : 0.f));
            ImVec4 clip(std::max(clip_l, px0), std::max(y, py0), std::min(clip_r, px1), std::min(y + h - 1, py1));
            if (clip.z <= clip.x || clip.w <= clip.y) return;
            styled_text(dl, st.font, fs, ImVec2(tx, ty), col, tb, te, clip, wrap ? std::max(avail, 1.f) : 0.f);
        };
        for (auto& e : ents) {
            if (editing && e.r == er && e.c == ec && cur == esi) continue;
            draw_text(e.r, e.c, e.x, e.y, e.w, e.h, *e.lk, false);
        }
        for (auto& me : ments) {
            if (editing && me.m.r0 == er && me.m.c0 == ec && cur == esi) continue;
            draw_text(me.m.r0, me.m.c0, me.x, me.y, me.w, me.h, me.lk, true);
        }
        // 5. borders
        auto draw_borders = [&](float x, float y, float w, float h, const Border& bd) {
            if (bd.left.style != BorderStyle::None) border_line(dl, ImVec2(x - 0.5f, y - 0.5f), ImVec2(x - 0.5f, y + h - 0.5f), bd.left, false);
            if (bd.right.style != BorderStyle::None) border_line(dl, ImVec2(x + w - 0.5f, y - 0.5f), ImVec2(x + w - 0.5f, y + h - 0.5f), bd.right, false);
            if (bd.top.style != BorderStyle::None) border_line(dl, ImVec2(x - 0.5f, y - 0.5f), ImVec2(x + w - 0.5f, y - 0.5f), bd.top, true);
            if (bd.bottom.style != BorderStyle::None) border_line(dl, ImVec2(x - 0.5f, y + h - 0.5f), ImVec2(x + w - 0.5f, y + h - 0.5f), bd.bottom, true);
            if (bd.diag_down && bd.diagonal.style != BorderStyle::None) border_line(dl, ImVec2(x, y), ImVec2(x + w, y + h), bd.diagonal, false);
            if (bd.diag_up && bd.diagonal.style != BorderStyle::None) border_line(dl, ImVec2(x, y + h), ImVec2(x + w, y), bd.diagonal, false);
        };
        for (auto& e : ents) draw_borders(e.x, e.y, e.w, e.h, e.lk->style.border);
        for (auto& me : ments) {
            // merged: outline from the edge cells' borders
            Border bd = me.lk.style.border;
            bd.right = b.style_of(cur, me.m.r0, me.m.c1).border.right;
            bd.bottom = b.style_of(cur, me.m.r1, me.m.c0).border.bottom;
            draw_borders(me.x, me.y, me.w, me.h, bd);
        }
        // 6. formula reference boxes
        for (size_t k = 0; k < eref.size(); ++k) {
            const UsedRef& u = eref[k];
            int usi = u.si < 0 ? esi : u.si;
            if (usi != cur) continue;
            Range g = u.rg;
            if (!g.intersects(pv)) continue;
            g.r1 = std::min(g.r1, L.rows.empty() ? 0 : L.rows.back()); g.c1 = std::min(g.c1, NC - 1);
            float x = colX(g.c0), x2 = colX(g.c1 + 1), y = rowY(g.r0), y2 = rowY(g.r1 + 1);
            ImU32 rc = sg::ref_color((int)k);
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x2, y2), col_alpha(rc, 0.10f));
            dl->AddRect(ImVec2(x, y), ImVec2(x2, y2), rc, 0, 0, S(1.5f));
            dl->AddRectFilled(ImVec2(x2 - S(3), y2 - S(3)), ImVec2(x2 + S(2), y2 + S(2)), rc);
        }
        // 7. selection
        const auto& sel = U.sel;
        for (size_t k = 0; k < sel.size(); ++k) {
            Range g = sel[k].norm();
            if (!g.intersects(pv)) continue;
            int gr1 = std::min(g.r1, L.rows.back()), gc1 = std::min(g.c1, NC - 1);
            float x = colX(g.c0), x2 = colX(gc1 + 1), y = rowY(g.r0), y2 = rowY(gr1 + 1);
            if (g.c0 < L.fc && gc1 >= L.fc && !fr_cols) x = std::max(x, XS);
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x2, y2), col_alpha(t.accent, t.dark ? 0.20f : 0.13f));
        }
        if (!sel.empty()) {
            // active cell unshaded, primary range outline
            int r = ar(), c = ac();
            Range a(r, c, r, c);
            if (const Range* m = W.merge_at(r, c)) a = *m;
            if (a.intersects(pv)) {
                float x = colX(a.c0), x2 = colX(a.c1 + 1), y = rowY(a.r0), y2 = rowY(a.r1 + 1);
                if (sel.size() > 1 || !(prim().rows() == a.rows() && prim().cols() == a.cols())) {
                    const CellLook* lk = nullptr;
                    for (auto& e : ents) if (e.r == a.r0 && e.c == a.c0) lk = e.lk;
                    ImU32 bg = lk && lk->fill_color ? doc_col(lk->fill_color) : t.field;
                    (void)bg;
                    dl->AddRect(ImVec2(x + 1, y + 1), ImVec2(x2 - 1, y2 - 1), col_alpha(t.accent, 0.55f), 0, 0, 1.f);
                }
            }
            Range g = prim();
            if (g.intersects(pv) || true) {
                int gr1 = std::min(g.r1, L.rows.empty() ? 0 : L.rows.back()), gc1 = std::min(g.c1, NC - 1);
                float x = colX(g.c0), x2 = colX(gc1 + 1), y = rowY(g.r0), y2 = rowY(gr1 + 1);
                dl->AddRect(ImVec2(x - 1, y - 1), ImVec2(x2, y2), t.accent, 0, 0, S(2));
                if (!editing && sel.size() == 1) {
                    float hs = S(3.5f);
                    dl->AddRectFilled(ImVec2(x2 - hs, y2 - hs), ImVec2(x2 + hs, y2 + hs), t.field);
                    dl->AddRectFilled(ImVec2(x2 - hs + 1, y2 - hs + 1), ImVec2(x2 + hs - 1, y2 + hs - 1), t.accent);
                }
            }
        }
        // 8. fill / move preview, copy marquee
        auto dash_rect = [&](Range g, ImU32 c, float phase) {
            float x = colX(g.c0), x2 = colX(std::min(g.c1, NC - 1) + 1), y = rowY(g.r0), y2 = rowY(std::min(g.r1, L.rows.back()) + 1);
            dashed(dl, ImVec2(x, y), ImVec2(x2, y), c, S(2), S(5), S(3), phase);
            dashed(dl, ImVec2(x2, y), ImVec2(x2, y2), c, S(2), S(5), S(3), phase);
            dashed(dl, ImVec2(x2, y2), ImVec2(x, y2), c, S(2), S(5), S(3), phase);
            dashed(dl, ImVec2(x, y2), ImVec2(x, y), c, S(2), S(5), S(3), phase);
        };
        if (drag == GDrag::Fill && fill_dst.valid()) dash_rect(fill_dst, t.text_dim, 0);
        if (drag == GDrag::Move) {
            Range g = move_src;
            Range d2(g.r0 + move_dr, g.c0 + move_dc, g.r1 + move_dr, g.c1 + move_dc);
            if (d2.r0 >= 0 && d2.c0 >= 0) dash_rect(d2, t.accent, 0);
        }
        if (clip_si == cur && clip_rg.valid()) {
            dash_rect(clip_rg, t.accent, (float)ImGui::GetTime() * S(18));
            request_redraw(2);
        }
        dl->PopClipRect();
    };
    paint_pane(false, false);
    if (L.fr) paint_pane(true, false);
    if (L.fc) paint_pane(false, true);
    if (L.fr && L.fc) paint_pane(true, true);
    // frozen split lines
    if (L.fr) dl->AddLine(ImVec2(X0, YS - 0.5f), ImVec2(in.Max.x, YS - 0.5f), t.dark ? col_alpha(t.text_dim, 0.8f) : IM_COL32(150, 150, 150, 255), S(1));
    if (L.fc) dl->AddLine(ImVec2(XS - 0.5f, Y0), ImVec2(XS - 0.5f, in.Max.y), t.dark ? col_alpha(t.text_dim, 0.8f) : IM_COL32(150, 150, 150, 255), S(1));

    // ================================================================ headers
    std::vector<char> csel(NC, 0);
    auto row_sel_any = [&](int r) { for (auto& s : U.sel) { Range g = s.norm(); if (r >= g.r0 && r <= g.r1) return true; } return false; };
    auto row_full = [&](int r) { for (auto& s : U.sel) { Range g = s.norm(); if (r >= g.r0 && r <= g.r1 && g.c0 == 0 && g.c1 >= MAX_COLS - 1) return true; } return false; };
    auto col_full = [&](int c) { for (auto& s : U.sel) { Range g = s.norm(); if (c >= g.c0 && c <= g.c1 && g.r0 == 0 && g.r1 >= MAX_ROWS - 1) return true; } return false; };
    const ImU32 hdr_bg = t.panel2, hdr_sel = col_alpha(t.accent, 0.22f), hdr_full = col_alpha(t.accent, 0.55f);
    int hc0f = 0, hc1f = L.fc - 1, hc0, hc1;
    vis_cols(false, hc0, hc1);
    int hi0, hi1;
    vis_rows(false, hi0, hi1);
    float hty = (CH - hfs) * 0.5f;
    if (show_headers) {
        for (auto& s : U.sel) { Range g = s.norm(); for (int c = std::max(0, g.c0); c <= std::min(g.c1, NC - 1); ++c) csel[c] = 1; }
        auto col_hdr = [&](int c) {
            float x = colX(c), w = L.cx[c + 1] - L.cx[c];
            if (w <= 0) {   // hidden column marker
                dl->AddLine(ImVec2(x - 1, o.y + S(3)), ImVec2(x - 1, o.y + CH - S(3)), t.accent, S(1.5f));
                return;
            }
            bool full = col_full(c);
            if (csel[c]) dl->AddRectFilled(ImVec2(x, o.y), ImVec2(x + w, o.y + CH), full ? hdr_full : hdr_sel);
            std::string nm = col_name(c);
            ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(hfs, FLT_MAX, 0, nm.c_str());
            dl->AddText(font_for_px(g_font, hfs), hfs, ImVec2(x + (w - ts.x) * 0.5f, o.y + hty), csel[c] ? (full ? t.accent_text : t.text) : t.text_dim, nm.c_str());
            dl->AddLine(ImVec2(x + w - 0.5f, o.y + S(2)), ImVec2(x + w - 0.5f, o.y + CH), grid_c);
            if (csel[c]) dl->AddLine(ImVec2(x, o.y + CH - 1), ImVec2(x + w, o.y + CH - 1), t.accent, S(2));
        };
        dl->AddRectFilled(ImVec2(o.x, o.y), ImVec2(in.Max.x, o.y + CH), hdr_bg);
        dl->PushClipRect(ImVec2(XS, o.y), ImVec2(in.Max.x, o.y + CH), true);
        for (int c = hc0; c <= hc1; ++c) col_hdr(c);
        dl->PopClipRect();
        dl->PushClipRect(ImVec2(X0, o.y), ImVec2(XS, o.y + CH), true);
        for (int c = hc0f; c <= hc1f; ++c) col_hdr(c);
        dl->PopClipRect();
        dl->AddLine(ImVec2(o.x, o.y + CH - 0.5f), ImVec2(in.Max.x, o.y + CH - 0.5f), t.border);
        // row headers
        dl->AddRectFilled(ImVec2(o.x, Y0), ImVec2(X0, in.Max.y), hdr_bg);
        auto row_hdr = [&](int i) {
            int r = L.rows[i];
            float y = rowYi(i), h = L.ry[i + 1] - L.ry[i];
            bool s = row_sel_any(r), full = s && row_full(r);
            if (s) dl->AddRectFilled(ImVec2(o.x, y), ImVec2(X0, y + h), full ? hdr_full : hdr_sel);
            char bnum[16]; std::snprintf(bnum, sizeof bnum, "%d", r + 1);
            ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(hfs, FLT_MAX, 0, bnum);
            bool filtered_hidden_before = i > 0 && L.rows[i - 1] != r - 1;
            ImU32 tc = s ? (full ? t.accent_text : t.text) : (W.autofilter.active() && W.autofilter.range.contains(r, W.autofilter.range.c0) && r > W.autofilter.range.r0 && filtered_hidden_before ? t.accent : t.text_dim);
            if (h >= ts.y * 0.7f) dl->AddText(font_for_px(g_font, hfs), hfs, ImVec2(X0 - ts.x - S(6), y + (h - ts.y) * 0.5f), tc, bnum);
            dl->AddLine(ImVec2(o.x + S(2), y + h - 0.5f), ImVec2(X0, y + h - 0.5f), grid_c);
            if (filtered_hidden_before) dl->AddLine(ImVec2(o.x, y), ImVec2(X0, y), t.accent, S(1.5f));
            if (s) dl->AddLine(ImVec2(X0 - 1, y), ImVec2(X0 - 1, y + h), t.accent, S(2));
        };
        dl->PushClipRect(ImVec2(o.x, YS), ImVec2(X0, in.Max.y), true);
        for (int i = hi0; i <= hi1 && i < NR; ++i) row_hdr(i);
        dl->PopClipRect();
        dl->PushClipRect(ImVec2(o.x, Y0), ImVec2(X0, YS), true);
        for (int i = 0; i < L.fri; ++i) row_hdr(i);
        dl->PopClipRect();
        dl->AddLine(ImVec2(X0 - 0.5f, o.y), ImVec2(X0 - 0.5f, in.Max.y), t.border);
        dl->AddRectFilled(o, ImVec2(X0 - 1, o.y + CH - 1), hdr_bg);
        dl->AddTriangleFilled(ImVec2(X0 - S(5), o.y + CH - S(13)), ImVec2(X0 - S(5), o.y + CH - S(5)), ImVec2(X0 - S(13), o.y + CH - S(5)), t.text_faint);
    }

    // ================================================================ autofilter buttons / validation arrow
    struct Btn { ImVec2 a, b; int kind; int col; };   // kind 0 autofilter, 1 validation list
    std::vector<Btn> btns;
    const float bsz = std::min(S(16) * hz, std::max(S(10), (L.ry.size() > 1 ? L.ry[1] : S(18)) - S(3)));
    if (W.autofilter.active()) {
        const Range& af = W.autofilter.range;
        int hr = af.r0;
        int ii = rowI(hr);
        if (ii < NR && L.rows[ii] == hr) {
            float y = rowYi(ii), h = L.ry[ii + 1] - L.ry[ii];
            bool frozen_row = ii < L.fri;
            if (frozen_row || (y + h > YS && y < in.Max.y)) {
                for (int c = af.c0; c <= af.c1 && c < NC; ++c) {
                    float w = L.cx[c + 1] - L.cx[c];
                    if (w <= 0) continue;
                    float x = colX(c);
                    if (c >= L.fc && x + w <= XS) continue;
                    if (x > in.Max.x) break;
                    ImVec2 a(x + w - bsz - S(2), y + (h - bsz) * 0.5f), bb(a.x + bsz, a.y + bsz);
                    btns.push_back({a, bb, 0, c});
                }
            }
        }
    }
    {
        int r = ar(), c = ac();
        if (!editing && r < (int)W.rows.size() && !W.validations.empty()) {
            const CellLook& lk = look(r, c);
            if (lk.dropdown) {
                int ii = rowI(r);
                if (ii < NR && L.rows[ii] == r) {
                    float y = rowYi(ii), h = L.ry[ii + 1] - L.ry[ii], x = colX(c + 1);
                    btns.push_back({ImVec2(x + S(1), y), ImVec2(x + S(1) + std::max(bsz, h * 0.9f), y + h), 1, c});
                }
            }
        }
    }
    int btn_hover = -1;
    for (size_t k = 0; k < btns.size(); ++k) {
        auto& bt = btns[k];
        bool hv = ImRect(bt.a, bt.b).Contains(io.MousePos) && ImGui::IsWindowHovered();
        if (hv) btn_hover = (int)k;
        bool active_flt = false;
        if (bt.kind == 0) for (auto& fc : W.autofilter.cols) if (fc.col == bt.col - W.autofilter.range.c0 && fc.active()) active_flt = true;
        dl->AddRectFilled(bt.a, bt.b, hv ? t.raised : t.panel2, S(3));
        dl->AddRect(bt.a, bt.b, col_alpha(t.border, 0.9f), S(3));
        float s = bt.b.x - bt.a.x;
        if (active_flt) sg::glyph(dl, sg::G::Funnel, ImVec2(bt.a.x + s * 0.12f, bt.a.y + s * 0.12f), s * 0.76f, t.accent);
        else {
            ImVec2 cc((bt.a.x + bt.b.x) * 0.5f, (bt.a.y + bt.b.y) * 0.5f);
            float k2 = s * 0.2f;
            dl->AddTriangleFilled(ImVec2(cc.x - k2, cc.y - k2 * 0.5f), ImVec2(cc.x + k2, cc.y - k2 * 0.5f), ImVec2(cc.x, cc.y + k2 * 0.7f), t.text_dim);
        }
    }

    // ================================================================ in-cell editor
    if (editing && esi == cur) {
        Range a(er, ec, er, ec);
        if (const Range* m = W.merge_at(er, ec)) a = *m;
        int ii = rowI(a.r0);
        if (ii < NR) {
            float x = colX(a.c0), y = rowYi(ii), w = colX(a.c1 + 1) - x, h = rowY(a.r1 + 1) - y;
            draw_cell_editor(ImVec2(x, y), std::max(w, S(30)), std::max(h, S(18)));
        }
    }

    // ================================================================ mouse
    const bool hov = ImGui::IsWindowHovered() && in.Contains(io.MousePos) && !ImGui::IsAnyItemHovered();
    const float mx = io.MousePos.x, my = io.MousePos.y;
    point_clicked = false;
    // hot zones
    int col_border = -1, row_border = -1;
    if (hov && show_headers && my < Y0 && mx > X0) {
        auto chk = [&](int c) { float e = colX(c + 1); if (std::fabs(mx - e) < S(4) && (c < L.fc || e > XS)) col_border = c; };
        for (int c = hc0f; c <= hc1f; ++c) chk(c);
        for (int c = hc0; c <= hc1; ++c) chk(c);
        // hidden columns: prefer the right-most zero-width column at this edge (unhide by dragging)
    }
    if (hov && show_headers && mx < X0 && my > Y0) {
        auto chk = [&](int i) { float e = rowYi(i) + (L.ry[i + 1] - L.ry[i]); if (std::fabs(my - e) < S(3) && (i < L.fri || e > YS)) row_border = L.rows[i]; };
        for (int i = 0; i < L.fri; ++i) chk(i);
        for (int i = hi0; i <= hi1 && i < NR; ++i) chk(i);
    }
    // selection outline / fill handle
    bool on_handle = false, on_border = false;
    if (hov && !editing && U.sel.size() == 1 && mx > X0 && my > Y0) {
        Range g = prim();
        int gr1 = std::min(g.r1, L.rows.empty() ? 0 : L.rows.back()), gc1 = std::min(g.c1, NC - 1);
        float x = colX(g.c0), x2 = colX(gc1 + 1), y = rowY(g.r0), y2 = rowY(gr1 + 1);
        if (std::fabs(mx - x2) < S(5) && std::fabs(my - y2) < S(5)) on_handle = true;
        else {
            bool inx = mx > x - S(3) && mx < x2 + S(3), iny = my > y - S(3) && my < y2 + S(3);
            if ((inx && (std::fabs(my - y) < S(3) || std::fabs(my - y2) < S(3))) || (iny && (std::fabs(mx - x) < S(3) || std::fabs(mx - x2) < S(3)))) on_border = true;
        }
    }
    if (col_border >= 0 || drag == GDrag::ColResize) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    else if (row_border >= 0 || drag == GDrag::RowResize) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    else if (on_handle || drag == GDrag::Fill) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    else if (on_border || drag == GDrag::Move) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    else if (btn_hover >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    bool lclick = ImGui::IsMouseClicked(ImGuiMouseButton_Left), rclick = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    bool dbl = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    const bool formula_pt = editing && ebuf[0] == '=' && (point_mode || sg::ref_insert_point(ebuf.data(), caret));
    if (hov && (lclick || rclick)) {
        bool right = rclick;
        ImGui::SetWindowFocus();
        if (btn_hover >= 0 && lclick) {
            auto& bt = btns[btn_hover];
            if (editing) commit_edit();
            if (bt.kind == 0) { af_col = bt.col; ImGui::OpenPopup("##af"); }
            else ImGui::OpenPopup("##dvlist");
        } else if (col_border >= 0 && !right) {
            if (editing) commit_edit();
            if (dbl) {
                if (col_full(col_border)) { Range g = prim(); autofit_cols(g.c0, g.c1); } else autofit_cols(col_border, col_border);
            } else { drag = GDrag::ColResize; drag_idx = col_border; drag_x0 = mx; drag_v0 = L.cx[col_border + 1] - L.cx[col_border]; }
        } else if (row_border >= 0 && !right) {
            if (editing) commit_edit();
            if (dbl) autofit_rows(row_border, row_border);
            else { drag = GDrag::RowResize; drag_idx = row_border; drag_x0 = my; drag_v0 = (float)W.row_height(row_border); }
        } else if (show_headers && my < Y0 && mx < X0) { if (editing) commit_edit(); su().sel = {SelRng{0, 0, MAX_ROWS - 1, MAX_COLS - 1}}; }
        else if (show_headers && my < Y0) {   // column header
            int c = col_at_x(mx);
            if (editing && formula_pt && !right) { insert_ref_text(col_name(c) + ":" + col_name(c), point_mode); point_clicked = true; }
            else {
                if (editing) commit_edit();
                Range pg = prim();
                if (!(right && c >= pg.c0 && c <= pg.c1 && col_full(c))) {
                    if (io.KeyShift) { auto& s = U.sel.back(); s.c1 = c; s.r0 = 0; s.r1 = MAX_ROWS - 1; }
                    else { if (!io.KeyCtrl) U.sel.clear(); U.sel.push_back({0, c, MAX_ROWS - 1, c}); }
                    if (!right) drag = GDrag::Cols;
                }
            }
        } else if (show_headers && mx < X0) {   // row header
            int r = row_at_y(my);
            if (editing) commit_edit();
            Range pg = prim();
            if (!(right && r >= pg.r0 && r <= pg.r1 && row_full(r))) {
                if (io.KeyShift) { auto& s = U.sel.back(); s.r1 = r; s.c0 = 0; s.c1 = MAX_COLS - 1; }
                else { if (!io.KeyCtrl) U.sel.clear(); U.sel.push_back({r, 0, r, MAX_COLS - 1}); }
                if (!right) drag = GDrag::Rows;
            }
        } else {
            int r = row_at_y(my), c = col_at_x(mx);
            if (editing && formula_pt && !right) {
                std::string ref = sg::a1(r, c);
                if (const Range* m = W.merge_at(r, c)) ref = range_name(*m);
                if (cur != esi) ref = quote_sheet_name(ws().name) + "!" + ref;
                bool replace = point_mode && io.KeyCtrl == false && !io.KeyShift ? true : false;
                if (io.KeyShift && point_mode) { ref = sg::a1_range(pt_r0, pt_c0, r, c); replace = true; }
                else { pt_r0 = r; pt_c0 = c; }
                // a new click after typing an operator starts a new reference
                if (!sg::ref_insert_point(ebuf.data(), caret) && !point_mode) replace = false;
                if (sg::ref_insert_point(ebuf.data(), pt_b >= 0 && point_mode ? pt_b : caret) && point_mode && caret == pt_e) replace = true;
                if (!point_mode) replace = false;
                insert_ref_text(ref, replace);
                drag = GDrag::Point; point_clicked = true;
            } else if (!right && on_handle) {
                drag = GDrag::Fill; fill_src = prim(); fill_dst = fill_src;
            } else if (!right && on_border && !editing) {
                drag = GDrag::Move; move_src = prim(); move_r0 = r; move_c0 = c; move_dr = move_dc = 0;
            } else {
                if (editing && !commit_edit()) { /* validation stopped */ }
                else if (right) { bool in = false; for (auto& s : U.sel) if (s.norm().contains(r, c)) in = true; if (!in) select_cell(r, c); }
                else if (dbl && !io.KeyShift && !io.KeyCtrl) { select_cell(r, c); begin_edit(true); }
                else { select_cell(r, c, io.KeyShift, io.KeyCtrl); drag = GDrag::Cells; }
                want_scroll = false;
            }
        }
        if (right) {
            if (show_headers && my < Y0 && mx >= X0) ImGui::OpenPopup("##colctx");
            else if (show_headers && mx < X0 && my >= Y0) ImGui::OpenPopup("##rowctx");
            else ImGui::OpenPopup("##ctx");
        }
    }
    if (drag != GDrag::None) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // release
            if (drag == GDrag::Cells && fp_on) {
                Style st = fp_style;
                fp_on = false;
                style_sel("Format Painter", [st](Style& s) { s = st; });
            }
            if (drag == GDrag::Fill && fill_dst.valid() && !(fill_dst == fill_src)) {
                b.fill(cur, fill_src, fill_dst);
                select_range(fill_dst);
                msg = "Filled " + range_name(fill_dst);
            } else if (drag == GDrag::Move && (move_dr || move_dc)) {
                Range g = move_src;
                if (g.r0 + move_dr >= 0 && g.c0 + move_dc >= 0) {
                    Book::Batch bt(b, io.KeyCtrl ? "Copy cells" : "Move cells");
                    ClipBlock blk = b.copy(cur, g);
                    if (!io.KeyCtrl) b.clear(cur, g, Book::ClearAll);
                    b.paste(cur, g.r0 + move_dr, g.c0 + move_dc, blk, PasteAll);
                    select_range(Range(g.r0 + move_dr, g.c0 + move_dc, g.r1 + move_dr, g.c1 + move_dc));
                }
            } else if (drag == GDrag::ColResize) {
                float wpx = std::max(0.f, drag_v0 + (mx - drag_x0));
                double chars = std::max(0.0, (wpx / (dpi_scale() * z) - 5.0) / 7.0);
                Range g = prim();
                if (col_full(drag_idx)) b.set_col_width(cur, g.c0, g.c1, chars);
                else b.set_col_width(cur, drag_idx, drag_idx, chars);
                if (chars <= 0.01) b.hide_cols(cur, drag_idx, drag_idx, true);
            } else if (drag == GDrag::RowResize) {
                double pt = std::max(0.0, drag_v0 + (my - drag_x0) / (dpi_scale() * z) * 72.0 / 96.0);
                Range g = prim();
                if (row_full(drag_idx)) b.set_row_height(cur, g.r0, g.r1, pt);
                else b.set_row_height(cur, drag_idx, drag_idx, pt);
            }
            drag = GDrag::None;
        } else {
            int r = row_at_y(my), c = col_at_x(mx);
            auto& s = U.sel.back();
            switch (drag) {
            case GDrag::Cells: s.r1 = r; s.c1 = c; break;
            case GDrag::Rows: s.r1 = r; break;
            case GDrag::Cols: s.c1 = c; break;
            case GDrag::Point:
                if (editing && (r != pt_r0 || c != pt_c0 || pt_e - pt_b > 0)) {
                    std::string ref = sg::a1_range(pt_r0, pt_c0, r, c);
                    if (cur != esi) ref = quote_sheet_name(ws().name) + "!" + ref;
                    std::string curref(ebuf.data() + std::max(0, pt_b), std::max(0, pt_e - pt_b));
                    if (curref != ref) insert_ref_text(ref, true);
                }
                break;
            case GDrag::Fill: {
                Range g = fill_src;
                int dr = r > g.r1 ? r - g.r1 : (r < g.r0 ? r - g.r0 : 0);
                int dc = c > g.c1 ? c - g.c1 : (c < g.c0 ? c - g.c0 : 0);
                Range d = g;
                if (std::abs(dr) * 1.0 >= std::abs(dc) * 1.0) { if (dr > 0) d.r1 = r; else if (dr < 0) d.r0 = r; }
                else { if (dc > 0) d.c1 = c; else if (dc < 0) d.c0 = c; }
                fill_dst = d;
                break;
            }
            case GDrag::Move: move_dr = r - move_r0; move_dc = c - move_c0; break;
            case GDrag::ColResize: {
                float wpx = std::max(0.f, drag_v0 + (mx - drag_x0));
                char tip[64]; std::snprintf(tip, sizeof tip, "Width: %.2f (%d pixels)", std::max(0.0, (wpx / (dpi_scale() * z) - 5.0) / 7.0), (int)(wpx / (dpi_scale() * z)));
                ImGui::SetTooltip("%s", tip);
                dl->AddLine(ImVec2(colX(drag_idx) + wpx, Y0), ImVec2(colX(drag_idx) + wpx, in.Max.y), t.accent, S(1));
                break;
            }
            case GDrag::RowResize: {
                double pt = std::max(0.0, drag_v0 + (my - drag_x0) / (dpi_scale() * z) * 72.0 / 96.0);
                ImGui::SetTooltip("Height: %.2f (%d pixels)", pt, (int)(pt * 96 / 72));
                dl->AddLine(ImVec2(X0, my), ImVec2(in.Max.x, my), t.accent, S(1));
                break;
            }
            default: break;
            }
            // auto scroll at the edges
            if (drag != GDrag::ColResize && drag != GDrag::RowResize) {
                if (my > in.Max.y - S(10)) U.tsy += S(20) * z;
                else if (my < YS + S(4) && drag != GDrag::Cols && sy > 0) U.tsy = std::max(0.f, U.tsy - S(20) * z);
                if (mx > in.Max.x - S(10)) U.tsx += S(30) * z;
                else if (mx < XS + S(4) && drag != GDrag::Rows && sx > 0) U.tsx = std::max(0.f, U.tsx - S(30) * z);
            }
        }
    }
    // editor focus loss (clicked outside the editors, not a point-mode click)
    if (deact_pending) {
        ImGuiID act = ImGui::GetActiveID();
        if (editing && !point_clicked && act != bar_id && act != cell_id && drag != GDrag::Point) commit_edit();
        deact_pending = false;
    }

    // popups
    grid_context_menu();
    row_header_menu();
    col_header_menu();
    filter_popup();
    if (ImGui::BeginPopup("##dvlist")) {
        auto items = b.list_items(cur, ar(), ac());
        if (items.empty()) ImGui::TextDisabled("(no items)");
        for (auto& it : items) if (ImGui::Selectable(it.c_str())) b.set_input(cur, ar(), ac(), it);
        ImGui::EndPopup();
    }

    // ---- keyboard
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput);
    if (focused && !editing && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && (dlg == Dlg::None || dlg == Dlg::Find)) handle_keys();
    // ---- scroll the active end of the selection into view
    if (want_scroll) {
        want_scroll = false;
        auto& s = U.sel.back();
        int r = s.r1, c = s.c1;
        if (r >= MAX_ROWS - 1) r = s.r0;
        if (c >= MAX_COLS - 1) c = s.c0;
        ensure_layout(z);
        int ii = rowI(r);
        if (ii < (int)lay.rows.size() && ii >= lay.fri) {
            float y = lay.ry[ii] - lay.fh, h = lay.ry[ii + 1] - lay.ry[ii];
            float vh = VH - CH - lay.fh;
            if (y < U.tsy) U.tsy = y;
            else if (y + h > U.tsy + vh) U.tsy = y + h - vh;
        }
        if (c >= lay.fc && c < lay.nc) {
            float x = lay.cx[c] - lay.fw, w = lay.cx[c + 1] - lay.cx[c];
            float vw = VW - RW - lay.fw;
            if (x < U.tsx) U.tsx = x;
            else if (x + w > U.tsx + vw) U.tsx = x + w - vw;
        }
        request_redraw(3);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

// ===========================================================================
// in-cell editor
// ===========================================================================
void SheetDoc::draw_cell_editor(ImVec2 pos, float w, float h) {
    const GTheme& t = th();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float tw = ImGui::CalcTextSize(ebuf.data()).x + S(20);
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    float maxw = win->InnerRect.Max.x - pos.x - S(2);
    float ew = std::clamp(tw, w, std::max(w, maxw));
    dl->AddRectFilled(pos, ImVec2(pos.x + ew, pos.y + h), t.field);
    // characters typed while the editor is still activating (focus lands next frame): append them
    ImGuiIO& io = ImGui::GetIO();
    if (!edit_in_bar && ImGui::GetActiveID() != cell_id && !io.InputQueueCharacters.empty() && !io.KeyCtrl) {
        std::string s(ebuf.data());
        for (ImWchar w : io.InputQueueCharacters) {
            if (w < 32 || w == 127) continue;
            if (w < 0x80) s += (char)w;
            else if (w < 0x800) { s += (char)(0xC0 | w >> 6); s += (char)(0x80 | (w & 63)); }
            else { s += (char)(0xE0 | w >> 12); s += (char)(0x80 | ((w >> 6) & 63)); s += (char)(0x80 | (w & 63)); }
        }
        io.InputQueueCharacters.resize(0);
        size_t n = std::min(s.size(), ebuf.size() - 1);
        std::memcpy(ebuf.data(), s.data(), n); ebuf[n] = 0;
        want_caret = (int)n; edit_focus = true;
    }
    ImGui::SetCursorScreenPos(pos);
    formula_input("##cell", ew, h, false);
    dl->AddRect(ImVec2(pos.x - 1, pos.y - 1), ImVec2(pos.x + ew + 1, pos.y + h + 1), t.accent, 0, 0, S(2));
    draw_autocomplete(ImVec2(pos.x, pos.y + h + S(2)));
}

// ===========================================================================
// keyboard
// ===========================================================================
void SheetDoc::handle_keys() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::GetCurrentContext()->WantTextInputNextFrame = 1;   // the shell's global shortcuts stay out
    const bool ctrl = io.KeyCtrl, shift = io.KeyShift, alt = io.KeyAlt;
    auto P = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
    SheetUi& U = su();
    auto& s = U.sel.back();
    Worksheet& W = ws();
    bool consumed_chars = false;
    if (ctrl && !alt) {
        if (P(ImGuiKey_C)) copy(false);
        else if (P(ImGuiKey_X)) copy(true);
        else if (P(ImGuiKey_V)) { if (shift) open_dialog(Dlg::PasteSpecial); else paste(); }
        else if (P(ImGuiKey_Z)) { if (shift) b.redo(); else b.undo(); }
        else if (P(ImGuiKey_Y)) b.redo();
        else if (P(ImGuiKey_A)) select_all();
        else if (P(ImGuiKey_D)) fill_dir(0);
        else if (P(ImGuiKey_R)) fill_dir(1);
        else if (P(ImGuiKey_B) || P(ImGuiKey_2)) { bool v = !b.style_of(cur, ar(), ac()).font.bold; style_sel("Bold", [v](Style& st) { st.font.bold = v; }); }
        else if (P(ImGuiKey_I) || P(ImGuiKey_3)) { bool v = !b.style_of(cur, ar(), ac()).font.italic; style_sel("Italic", [v](Style& st) { st.font.italic = v; }); }
        else if (P(ImGuiKey_U) || P(ImGuiKey_4)) { bool v = b.style_of(cur, ar(), ac()).font.underline == Underline::None; style_sel("Underline", [v](Style& st) { st.font.underline = v ? Underline::Single : Underline::None; }); }
        else if (P(ImGuiKey_5)) { bool v = !b.style_of(cur, ar(), ac()).font.strike; style_sel("Strikethrough", [v](Style& st) { st.font.strike = v; }); }
        else if (P(ImGuiKey_1)) { fmt_tab = 0; open_dialog(Dlg::FormatCells); }
        else if (P(ImGuiKey_9)) { Range g = prim(); b.hide_rows(cur, g.r0, std::min(g.r1, W.nrows() + 1000), !shift); }
        else if (P(ImGuiKey_0)) { Range g = prim(); b.hide_cols(cur, g.c0, std::min(g.c1, W.ncols() + 100), !shift); }
        else if (P(ImGuiKey_L) && shift) toggle_filter();
        else if (P(ImGuiKey_F)) { show_replace = false; open_dialog(Dlg::Find); }
        else if (P(ImGuiKey_H)) { show_replace = true; open_dialog(Dlg::Find); }
        else if (P(ImGuiKey_G)) open_dialog(Dlg::GoTo);
        else if (P(ImGuiKey_S)) { bool as = shift; defer([this, as]() { save(as); }); }
        else if (P(ImGuiKey_W)) defer([]() { files_close_current_doc(); });
        else if (ImGui::IsKeyPressed(ImGuiKey_O, false)) post_command(app::ID_FILE_OPEN);   // R20: no key repeat for a dialog command
        else if (P(ImGuiKey_N)) defer([]() { files_new_sheet(); });
        else if (P(ImGuiKey_Space)) { int r = ar(), cc = ac(); s.r0 = 0; s.r1 = MAX_ROWS - 1; U.act_r = r; U.act_c = cc; U.act_for = s; consumed_chars = true; }
        else if (P(ImGuiKey_Semicolon)) { insert_date(shift); consumed_chars = true; }
        else if (P(ImGuiKey_Minus) || P(ImGuiKey_KeypadSubtract)) {
            Range g = prim();
            if (g.c0 == 0 && g.c1 >= MAX_COLS - 1) delete_rows(); else if (g.r0 == 0 && g.r1 >= MAX_ROWS - 1) delete_cols(); else delete_cells_shift(true);
        } else if ((P(ImGuiKey_Equal) && shift) || P(ImGuiKey_KeypadAdd)) {
            Range g = prim();
            if (g.c0 == 0 && g.c1 >= MAX_COLS - 1) insert_rows(false); else if (g.r0 == 0 && g.r1 >= MAX_ROWS - 1) insert_cols(false); else insert_cells_shift(true);
        } else if (P(ImGuiKey_PageDown)) { if (cur + 1 < (int)b.sheets.size()) ++cur; }
        else if (P(ImGuiKey_PageUp)) { if (cur > 0) --cur; }
        else if (P(ImGuiKey_F1)) ribbon_min = !ribbon_min;
    }
    if (alt && !ctrl && P(ImGuiKey_Equal)) { autosum(); consumed_chars = true; }
    if (P(ImGuiKey_F5)) open_dialog(Dlg::GoTo);
    if (P(ImGuiKey_F3) && shift) {}
    if (shift && !ctrl && P(ImGuiKey_Space)) { int r = ar(), cc = ac(); s.c0 = 0; s.c1 = MAX_COLS - 1; U.act_r = r; U.act_c = cc; U.act_for = s; consumed_chars = true; }
    if (shift && P(ImGuiKey_F10)) ImGui::OpenPopup("##ctx");
    if (P(ImGuiKey_Escape)) { clear_marquee(); }
    // ---- navigation
    int r = shift ? s.r1 : ar(), c = shift ? s.c1 : ac();
    if (r >= MAX_ROWS - 1) r = W.nrows();
    if (c >= MAX_COLS - 1) c = W.ncols();
    bool move = false;
    auto next_vis_row = [&](int rr, int d) {
        rr += d;
        while (rr > 0 && rr < MAX_ROWS - 1 && W.row_hidden(rr)) rr += d;
        return std::clamp(rr, 0, MAX_ROWS - 1);
    };
    auto next_vis_col = [&](int cc, int d) {
        cc += d;
        while (cc > 0 && cc < MAX_COLS - 1 && W.col_hidden(cc)) cc += d;
        return std::clamp(cc, 0, MAX_COLS - 1);
    };
    // step out of a merged area
    auto mrg = [&](int rr, int cc) { return shift ? nullptr : W.merge_at(rr, cc); };
    if (P(ImGuiKey_DownArrow)) { const Range* m = mrg(r, c); r = ctrl ? edge_jump(r, c, 1, 0) : next_vis_row(m ? m->r1 : r, 1); move = true; }
    if (P(ImGuiKey_UpArrow)) { const Range* m = mrg(r, c); r = ctrl ? edge_jump(r, c, -1, 0) : next_vis_row(m ? m->r0 : r, -1); move = true; }
    if (P(ImGuiKey_RightArrow)) { const Range* m = mrg(r, c); c = ctrl ? edge_jump(r, c, 0, 1) : next_vis_col(m ? m->c1 : c, 1); move = true; }
    if (P(ImGuiKey_LeftArrow)) { const Range* m = mrg(r, c); c = ctrl ? edge_jump(r, c, 0, -1) : next_vis_col(m ? m->c0 : c, -1); move = true; }
    int page = 20;
    if (!lay.ry.empty()) page = std::max(1, (int)(ImGui::GetWindowHeight() / std::max(1.f, lay.ry.size() > 1 ? lay.ry[1] : 20.f)) - 2);
    if (P(ImGuiKey_PageDown) && !ctrl) { for (int k = 0; k < page; ++k) r = next_vis_row(r, 1); move = true; }
    if (P(ImGuiKey_PageUp) && !ctrl) { for (int k = 0; k < page; ++k) r = next_vis_row(r, -1); move = true; }
    if (P(ImGuiKey_Home)) { c = 0; if (ctrl) r = 0; move = true; }
    if (P(ImGuiKey_End) && ctrl) { Range u = W.used_range(); if (u.valid()) { r = u.r1; c = u.c1; } move = true; }
    if (move) {
        if (shift) { s.r1 = r; s.c1 = c; want_scroll = true; }
        else select_cell(r, c);
    }
    // Enter / Tab move inside a multi-cell selection (Excel)
    auto cycle = [&](int dr, int dc) {
        Range g = prim();
        int rr = ar(), cc = ac();
        if (U.sel.size() == 1 && (g.rows() > 1 || g.cols() > 1) && g.r1 < MAX_ROWS - 1 && g.c1 < MAX_COLS - 1) {
            if (dr) { rr += dr; if (rr > g.r1) { rr = g.r0; cc = cc + 1 > g.c1 ? g.c0 : cc + 1; } else if (rr < g.r0) { rr = g.r1; cc = cc - 1 < g.c0 ? g.c1 : cc - 1; } }
            else { cc += dc; if (cc > g.c1) { cc = g.c0; rr = rr + 1 > g.r1 ? g.r0 : rr + 1; } else if (cc < g.c0) { cc = g.c1; rr = rr - 1 < g.r0 ? g.r1 : rr - 1; } }
            U.act_r = rr; U.act_c = cc; U.act_for = U.sel.back();
            want_scroll = true;
        } else select_cell(dr ? next_vis_row(rr, dr) : rr, dc ? next_vis_col(cc, dc) : cc);
    };
    if (P(ImGuiKey_Enter) || P(ImGuiKey_KeypadEnter)) cycle(shift ? -1 : 1, 0);
    if (P(ImGuiKey_Tab)) cycle(0, shift ? -1 : 1);
    if (P(ImGuiKey_F2)) begin_edit(true);
    else if (P(ImGuiKey_Delete)) clear_sel(Book::ClearContents, "Clear contents");
    else if (P(ImGuiKey_Backspace)) { b.clear(cur, Range::cell(ar(), ac()), Book::ClearContents); begin_edit(false); }
    else if (!ctrl && !alt && !consumed_chars && !io.InputQueueCharacters.empty()) {
        std::string str;
        for (ImWchar w : io.InputQueueCharacters) {
            if (w < 32 || w == 127) continue;
            if (w < 0x80) str += (char)w;
            else if (w < 0x800) { str += (char)(0xC0 | w >> 6); str += (char)(0x80 | (w & 63)); }
            else { str += (char)(0xE0 | w >> 12); str += (char)(0x80 | ((w >> 6) & 63)); str += (char)(0x80 | (w & 63)); }
        }
        io.InputQueueCharacters.resize(0);
        if (!str.empty()) { if (str == "+" ) str = "="; begin_edit(false, str.c_str()); }
    }
    if (consumed_chars) io.InputQueueCharacters.resize(0);
}

// ===========================================================================
// context menus
// ===========================================================================
void SheetDoc::grid_context_menu() {
    if (!ImGui::BeginPopup("##ctx")) return;
    if (menu_item_icon("Cut", ui::Icon::Cut, "Ctrl+X")) copy(true);
    if (menu_item_icon("Copy", ui::Icon::Copy, "Ctrl+C")) copy(false);
    if (menu_item_icon("Paste", ui::Icon::Paste, "Ctrl+V")) paste();
    if (menu_item_icon("Paste Special\xE2\x80\xA6", ui::Icon::Paste, "Ctrl+Shift+V")) open_dialog(Dlg::PasteSpecial);
    ImGui::Separator();
    if (begin_menu_icon("Insert", ui::Icon::Plus)) {
        if (menu_item_icon("Shift cells right", ui::Icon::None)) insert_cells_shift(false);
        if (menu_item_icon("Shift cells down", ui::Icon::None)) insert_cells_shift(true);
        if (sg::gitem("Entire row", sg::G::InsRow)) insert_rows(false);
        if (sg::gitem("Entire column", sg::G::InsCol)) insert_cols(false);
        ImGui::EndMenu();
    }
    if (begin_menu_icon("Delete", ui::Icon::Delete)) {
        if (menu_item_icon("Shift cells left", ui::Icon::None)) delete_cells_shift(false);
        if (menu_item_icon("Shift cells up", ui::Icon::None)) delete_cells_shift(true);
        if (sg::gitem("Entire row", sg::G::DelRow)) delete_rows();
        if (sg::gitem("Entire column", sg::G::DelCol)) delete_cols();
        ImGui::EndMenu();
    }
    if (menu_item_icon("Clear contents", ui::Icon::Eraser, "Del")) clear_sel(Book::ClearContents, "Clear contents");
    if (sg::gitem("Clear formats", sg::G::Clear)) clear_sel(Book::ClearFormats, "Clear formats");
    ImGui::Separator();
    if (begin_menu_icon("Filter", ui::Icon::Query)) {
        if (menu_item_icon("Filter by selected cell's value", ui::Icon::Query)) {
            const Worksheet& W = ws();
            if (!W.autofilter.active() || !W.autofilter.range.contains(W.autofilter.range.r0, ac())) toggle_filter();
            const Worksheet& W2 = ws();
            if (W2.autofilter.active()) {
                FilterColumn fc; fc.col = ac() - W2.autofilter.range.c0;
                std::string v = b.display_text(cur, ar(), ac());
                if (v.empty()) fc.blanks = true; else fc.values = {v};
                b.set_filter_column(cur, fc);
            }
        }
        if (menu_item_icon("Clear all filters", ui::Icon::EyeOff, nullptr, false, ws().autofilter.active())) b.clear_filters(cur);
        if (menu_item_icon("Reapply", ui::Icon::Redo, nullptr, false, ws().autofilter.active())) b.reapply_filter(cur);
        ImGui::EndMenu();
    }
    if (begin_menu_icon("Sort", ui::Icon::ArrowUp)) {
        if (menu_item_icon("Sort A \xE2\x86\x92 Z", ui::Icon::ArrowUp)) sort_quick(false);
        if (menu_item_icon("Sort Z \xE2\x86\x92 A", ui::Icon::ArrowDown)) sort_quick(true);
        if (menu_item_icon("Custom Sort\xE2\x80\xA6", ui::Icon::Layers)) open_dialog(Dlg::Sort);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (sg::gitem("Format Cells\xE2\x80\xA6", sg::G::CellStyles, "Ctrl+1")) { fmt_tab = 0; open_dialog(Dlg::FormatCells); }
    if (sg::gitem(ws().merge_at(ar(), ac()) ? "Unmerge cells" : "Merge & Center", sg::G::Merge)) merge_center();
    if (sg::gitem("Conditional Formatting\xE2\x80\xA6", sg::G::CondFmt)) open_dialog(Dlg::CondFmt);
    if (sg::gitem("Data Validation\xE2\x80\xA6", sg::G::Validation)) open_dialog(Dlg::Validation);
    if (!ws().validations.empty() && look(ar(), ac()).dropdown && menu_item_icon("Pick from drop-down list", ui::Icon::ArrowDown)) { ImGui::CloseCurrentPopup(); }
    ImGui::Separator();
    if (begin_menu_icon("BOM", ui::Icon::Bom)) {
        if (sg::gitem("Expand references  (R1-R3 \xE2\x86\x92 R1, R2, R3)", sg::G::Refdes)) bom_expand_refs(true);
        if (sg::gitem("Compress references  (R1, R2, R3 \xE2\x86\x92 R1-R3)", sg::G::Refdes)) bom_expand_refs(false);
        if (menu_item_icon("Expand references to rows", ui::Icon::Locations)) bom_expand_to_rows();
        ImGui::EndMenu();
    }
    ImGui::EndPopup();
}

void SheetDoc::row_header_menu() {
    if (!ImGui::BeginPopup("##rowctx")) return;
    Range g = prim();
    std::string rows = g.rows() > 1 ? std::to_string(g.r0 + 1) + "-" + std::to_string(std::min(g.r1, MAX_ROWS - 1) + 1) : std::to_string(g.r0 + 1);
    if (menu_item_icon("Cut", ui::Icon::Cut, "Ctrl+X")) copy(true);
    if (menu_item_icon("Copy", ui::Icon::Copy, "Ctrl+C")) copy(false);
    if (menu_item_icon("Paste", ui::Icon::Paste, "Ctrl+V")) paste();
    ImGui::Separator();
    if (sg::gitem("Insert rows above", sg::G::InsRow, "Ctrl++")) insert_rows(false);
    if (sg::gitem("Insert rows below", sg::G::InsRow)) insert_rows(true);
    if (sg::gitem(("Delete row" + std::string(g.rows() > 1 ? "s " : " ") + rows).c_str(), sg::G::DelRow, "Ctrl+-")) delete_rows();
    if (menu_item_icon("Clear contents", ui::Icon::Eraser, "Del")) clear_sel(Book::ClearContents, "Clear contents");
    ImGui::Separator();
    if (sg::gitem("Format Cells\xE2\x80\xA6", sg::G::CellStyles, "Ctrl+1")) { fmt_tab = 0; open_dialog(Dlg::FormatCells); }
    if (sg::gitem("Row Height\xE2\x80\xA6", sg::G::RowHeight)) open_dialog(Dlg::RowHeight);
    if (sg::gitem("Autofit row height", sg::G::RowHeight)) autofit_rows(g.r0, g.r1);
    if (sg::gitem("Hide", sg::G::Hide, "Ctrl+9")) b.hide_rows(cur, g.r0, std::min(g.r1, ws().nrows() + 1000), true);
    if (menu_item_icon("Unhide", ui::Icon::Eye, "Ctrl+Shift+9")) b.hide_rows(cur, g.r0, std::min(g.r1, ws().nrows() + 1000), false);
    if (sg::gitem(ws().freeze_rows == g.r1 + 1 ? "Unfreeze panes" : "Freeze rows above here", sg::G::Freeze)) b.set_freeze(cur, ws().freeze_rows == g.r1 + 1 ? 0 : g.r1 + 1, ws().freeze_cols);
    ImGui::EndPopup();
}

void SheetDoc::col_header_menu() {
    if (!ImGui::BeginPopup("##colctx")) return;
    Range g = prim();
    std::string cols = g.cols() > 1 ? col_name(g.c0) + "-" + col_name(std::min(g.c1, MAX_COLS - 1)) : col_name(g.c0);
    if (menu_item_icon("Cut", ui::Icon::Cut, "Ctrl+X")) copy(true);
    if (menu_item_icon("Copy", ui::Icon::Copy, "Ctrl+C")) copy(false);
    if (menu_item_icon("Paste", ui::Icon::Paste, "Ctrl+V")) paste();
    ImGui::Separator();
    if (sg::gitem("Insert columns left", sg::G::InsCol, "Ctrl++")) insert_cols(false);
    if (sg::gitem("Insert columns right", sg::G::InsCol)) insert_cols(true);
    if (sg::gitem(("Delete column" + std::string(g.cols() > 1 ? "s " : " ") + cols).c_str(), sg::G::DelCol, "Ctrl+-")) delete_cols();
    if (menu_item_icon("Clear contents", ui::Icon::Eraser, "Del")) clear_sel(Book::ClearContents, "Clear contents");
    ImGui::Separator();
    if (menu_item_icon("Sort A \xE2\x86\x92 Z", ui::Icon::ArrowUp)) sort_quick(false);
    if (menu_item_icon("Sort Z \xE2\x86\x92 A", ui::Icon::ArrowDown)) sort_quick(true);
    if (sg::gitem(ws().autofilter.active() ? "Remove filter" : "Filter", sg::G::Funnel, "Ctrl+Shift+L")) toggle_filter();
    ImGui::Separator();
    if (sg::gitem("Format Cells\xE2\x80\xA6", sg::G::CellStyles, "Ctrl+1")) { fmt_tab = 0; open_dialog(Dlg::FormatCells); }
    if (sg::gitem("Column Width\xE2\x80\xA6", sg::G::ColWidth)) open_dialog(Dlg::ColWidth);
    if (sg::gitem("Autofit column width", sg::G::ColWidth)) autofit_cols(g.c0, g.c1);
    if (sg::gitem("Hide", sg::G::Hide, "Ctrl+0")) b.hide_cols(cur, g.c0, std::min(g.c1, ws().ncols() + 100), true);
    if (menu_item_icon("Unhide", ui::Icon::Eye)) b.hide_cols(cur, g.c0, std::min(g.c1, ws().ncols() + 100), false);
    if (sg::gitem(ws().freeze_cols == g.c1 + 1 ? "Unfreeze panes" : "Freeze columns left of here", sg::G::Freeze)) b.set_freeze(cur, ws().freeze_rows, ws().freeze_cols == g.c1 + 1 ? 0 : g.c1 + 1);
    ImGui::Separator();
    if (sg::gitem("Text to Columns\xE2\x80\xA6", sg::G::TextToCols)) open_dialog(Dlg::TextToCols);
    if (sg::gitem("Count references (REFCOUNT column)", sg::G::RefCount)) bom_count_refs();
    ImGui::EndPopup();
}

// ===========================================================================
// autofilter dropdown
// ===========================================================================
void SheetDoc::filter_popup() {
    static std::vector<std::string> vals;
    static std::vector<char> on;
    static bool blanks_on = true, has_blanks = false;
    static char search[128];
    static int popup_col = -1;
    static int custom_op = 0;
    static char custom_txt[128];
    ImGui::SetNextWindowSizeConstraints(ImVec2(S(250), 0), ImVec2(S(380), S(560)));
    if (!ImGui::BeginPopup("##af")) { popup_col = -1; return; }
    const Worksheet& W = ws();
    if (!W.autofilter.active()) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    const Range af = W.autofilter.range;
    const int off = af_col - af.c0;
    const FilterColumn* curfc = nullptr;
    for (auto& fc : W.autofilter.cols) if (fc.col == off) curfc = &fc;
    if (popup_col != af_col) {
        popup_col = af_col;
        vals = b.filter_values(cur, off);
        has_blanks = false;
        for (size_t i = 0; i < vals.size(); ++i) if (vals[i].empty()) { has_blanks = true; vals.erase(vals.begin() + i); break; }
        on.assign(vals.size(), 1);
        blanks_on = true;
        search[0] = 0; custom_txt[0] = 0; custom_op = 0;
        if (curfc && !curfc->values.empty()) {
            for (size_t i = 0; i < vals.size(); ++i) on[i] = std::find(curfc->values.begin(), curfc->values.end(), vals[i]) != curfc->values.end();
            blanks_on = curfc->blanks;
        }
        if (curfc && curfc->use_custom) {
            std::string v = curfc->v1;
            if (v.size() >= 2 && v.front() == '*' && v.back() == '*') { custom_op = 0; v = v.substr(1, v.size() - 2); }
            else if (!v.empty() && v.back() == '*') { custom_op = 2; v.pop_back(); }
            else if (!v.empty() && v.front() == '*') { custom_op = 3; v.erase(0, 1); }
            else custom_op = curfc->op1 == CmpOp::NotEqual ? 4 : curfc->op1 == CmpOp::Greater ? 5 : curfc->op1 == CmpOp::Less ? 6 : 1;
            std::snprintf(custom_txt, sizeof custom_txt, "%s", v.c_str());
        }
    }
    auto sort_by = [&](bool desc) {
        b.sort(cur, Range(af.r0, af.c0, std::max(af.r1, ws().nrows() - 1), af.c1), {SortKey{af_col, desc, true, false}}, true);
        ImGui::CloseCurrentPopup();
    };
    if (menu_item_icon("Sort A \xE2\x86\x92 Z", ui::Icon::ArrowUp)) sort_by(false);
    if (menu_item_icon("Sort Z \xE2\x86\x92 A", ui::Icon::ArrowDown)) sort_by(true);
    ImGui::Separator();
    std::string clr = "Clear filter from \"" + b.display_text(cur, af.r0, af_col) + "\"";
    if (menu_item_icon(clr.c_str(), ui::Icon::EyeOff, nullptr, false, curfc && curfc->active())) {
        FilterColumn fc; fc.col = off;
        b.set_filter_column(cur, fc);
        popup_col = -1;
        ImGui::CloseCurrentPopup();
    }
    // custom text filter
    section_header("Text filter");
    static const char* ops[] = {"contains", "equals", "begins with", "ends with", "does not equal", "greater than", "less than"};
    ImGui::SetNextItemWidth(S(110));
    ImGui::Combo("##cop", &custom_op, ops, IM_ARRAYSIZE(ops));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    bool apply_custom = ImGui::InputTextWithHint("##ctx", "value", custom_txt, sizeof custom_txt, ImGuiInputTextFlags_EnterReturnsTrue);
    section_header("Values");
    search_box("##afsearch", search, sizeof search, "Search", -1);
    ImGui::BeginChild("##afvals", ImVec2(-1, S(220)), ImGuiChildFlags_Borders);
    {
        std::string q = search;
        for (auto& ch : q) ch = (char)tolower((unsigned char)ch);
        auto match = [&](const std::string& v) {
            if (q.empty()) return true;
            std::string l = v; for (auto& ch : l) ch = (char)tolower((unsigned char)ch);
            return l.find(q) != std::string::npos;
        };
        bool all = blanks_on || !has_blanks;
        for (size_t i = 0; i < vals.size(); ++i) if (match(vals[i]) && !on[i]) all = false;
        bool a2 = all;
        if (ImGui::Checkbox("(Select All)", &a2)) {
            for (size_t i = 0; i < vals.size(); ++i) if (match(vals[i])) on[i] = a2;
            if (q.empty()) blanks_on = a2;
        }
        ImGuiListClipper clip;
        clip.Begin((int)vals.size());
        std::vector<int> idx;
        if (!q.empty()) { for (size_t i = 0; i < vals.size(); ++i) if (match(vals[i])) idx.push_back((int)i); clip.End(); clip.Begin((int)idx.size()); }
        while (clip.Step()) for (int k = clip.DisplayStart; k < clip.DisplayEnd; ++k) {
            int i = q.empty() ? k : idx[k];
            bool v = on[i];
            ImGui::PushID(i);
            if (ImGui::Checkbox(vals[i].c_str(), &v)) on[i] = v;
            ImGui::PopID();
        }
        if (has_blanks && q.empty()) ImGui::Checkbox("(Blanks)", &blanks_on);
    }
    ImGui::EndChild();
    bool ok = button("OK", ui::Icon::Check, true, ImVec2(S(90), 0));
    ImGui::SameLine();
    if (button("Cancel", ui::Icon::None, false, ImVec2(S(90), 0))) { popup_col = -1; ImGui::CloseCurrentPopup(); }
    if (ok || apply_custom) {
        FilterColumn fc; fc.col = off;
        if (custom_txt[0] && (apply_custom || ok)) {
            std::string v = custom_txt;
            fc.use_custom = true;
            switch (custom_op) {
            case 0: fc.op1 = CmpOp::Equal; fc.v1 = "*" + v + "*"; break;
            case 1: fc.op1 = CmpOp::Equal; fc.v1 = v; break;
            case 2: fc.op1 = CmpOp::Equal; fc.v1 = v + "*"; break;
            case 3: fc.op1 = CmpOp::Equal; fc.v1 = "*" + v; break;
            case 4: fc.op1 = CmpOp::NotEqual; fc.v1 = v; break;
            case 5: fc.op1 = CmpOp::Greater; fc.v1 = v; break;
            default: fc.op1 = CmpOp::Less; fc.v1 = v; break;
            }
        } else {
            bool all = blanks_on || !has_blanks;
            for (char x : on) if (!x) all = false;
            if (!all) {
                for (size_t i = 0; i < vals.size(); ++i) if (on[i]) fc.values.push_back(vals[i]);
                fc.blanks = has_blanks && blanks_on;
                if (fc.values.empty() && !fc.blanks) { fc.values.push_back("\x01"); }   // nothing selected: hide all
            }
        }
        b.set_filter_column(cur, fc);
        int shown = 0;
        for (int r = af.r0 + 1; r < ws().nrows(); ++r) shown += !ws().row_hidden(r);
        msg = std::to_string(shown) + " rows shown";
        popup_col = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace pcbp::gui
