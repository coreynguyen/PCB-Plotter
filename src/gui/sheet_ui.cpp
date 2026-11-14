// ============================================================================
// sheet_ui.cpp — ribbon glyphs, ribbon buttons, colour palettes and formula
// text helpers for the spreadsheet editor. See sheet_ui.h.
// ============================================================================
#include "sheet_ui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace pcbp::gui::sg {

// ===========================================================================
// glyphs (24x24 design grid scaled into s)
// ===========================================================================
void glyph(ImDrawList* dl, G g, ImVec2 p, float s, ImU32 col, ImU32 accent) {
    const float k = s / 24.f;
    auto P = [&](float x, float y) { return ImVec2(p.x + x * k, p.y + y * k); };
    const float lw = std::max(1.f, 1.6f * k);
    auto L = [&](float x0, float y0, float x1, float y1, float w = 0) { dl->AddLine(P(x0, y0), P(x1, y1), col, w > 0 ? w * k : lw); };
    auto R = [&](float x0, float y0, float x1, float y1, float w = 0) { dl->AddRect(P(x0, y0), P(x1, y1), col, 0, 0, w > 0 ? w * k : lw); };
    auto RF = [&](float x0, float y0, float x1, float y1, ImU32 c) { dl->AddRectFilled(P(x0, y0), P(x1, y1), c); };
    auto T = [&](const char* txt, float x, float y, ImFont* f, float size, ImU32 c) {
        if (!f) f = ImGui::GetFont();
        dl->AddText(f, size * k, P(x, y), c, txt);
    };
    const ImU32 dim = col_alpha(col, 0.45f);
    const ImU32 acc = accent ? accent : th().accent;
    auto bar = [&](ImU32 c) { RF(3, 19, 21, 23, c); if ((c >> 24) < 40) R(3, 19, 21, 23, 0.8f); };
    auto hl = [&](float y, float x0, float x1) { L(x0, y, x1, y); };
    auto grid9 = [&](ImU32 c) {   // dotted 3x3 cell grid
        for (int i = 0; i <= 3; ++i) for (int j = 0; j < 9; ++j) {
            float a = 4 + i * 16.f / 3, b = 4 + j * 2.f;
            dl->AddRectFilled(P(a - 0.4f, b), P(a + 0.4f, b + 0.8f), c);
            dl->AddRectFilled(P(b, a - 0.4f), P(b + 0.8f, a + 0.4f), c);
        }
    };
    switch (g) {
    case G::None: break;
    case G::Bold: T("B", 6.5f, 1.5f, g_font_bold, 20, col); break;
    case G::Italic: {
        L(9, 19, 15, 19); L(11, 4, 17, 4); L(15.5f, 4, 11.5f, 19, 2.2f); break;
    }
    case G::Underline: {
        dl->PathArcTo(P(12, 11), 5 * k, 0, IM_PI, 10); dl->PathStroke(col, 0, lw * 1.2f);
        L(7, 3, 7, 11); L(17, 3, 17, 11); L(5, 20, 19, 20); break;
    }
    case G::Strike: T("S", 7.f, 2.f, g_font_bold, 18, col); L(4, 12, 20, 12, 1.6f); break;
    case G::FontColor: {
        L(6, 16, 12, 2.5f, 2); L(12, 2.5f, 18, 16, 2); L(8.5f, 11, 15.5f, 11, 1.8f);
        bar(acc); break;
    }
    case G::FillColor: {
        // tilted bucket
        ImVec2 q[4] = {P(5, 9), P(12, 2.5f), P(18.5f, 9), P(11.5f, 15.5f)};
        dl->AddPolyline(q, 4, col, ImDrawFlags_Closed, lw);
        L(8, 6, 12, 2.5f);
        dl->AddCircleFilled(P(19.5f, 13.5f), 1.6f * k, col);
        L(19.5f, 10, 19.5f, 12.5f);
        bar(acc); break;
    }
    case G::Borders: case G::BAll: grid9(dim); R(4, 4, 20, 20); L(12, 4, 12, 20); L(4, 12, 20, 12); break;
    case G::BOutside: grid9(dim); R(4, 4, 20, 20); break;
    case G::BThickOutside: grid9(dim); R(4, 4, 20, 20, 3); break;
    case G::BTop: grid9(dim); L(4, 4, 20, 4, 2.4f); break;
    case G::BBottom: grid9(dim); L(4, 20, 20, 20, 2.4f); break;
    case G::BLeft: grid9(dim); L(4, 4, 4, 20, 2.4f); break;
    case G::BRight: grid9(dim); L(20, 4, 20, 20, 2.4f); break;
    case G::BInside: grid9(dim); L(12, 4, 12, 20); L(4, 12, 20, 12); break;
    case G::BNone: grid9(dim); break;
    case G::AlignL: hl(5, 3, 21); hl(9.5f, 3, 14); hl(14, 3, 21); hl(18.5f, 3, 14); break;
    case G::AlignC: hl(5, 3, 21); hl(9.5f, 7, 17); hl(14, 3, 21); hl(18.5f, 7, 17); break;
    case G::AlignR: hl(5, 3, 21); hl(9.5f, 10, 21); hl(14, 3, 21); hl(18.5f, 10, 21); break;
    case G::VTop: L(3, 3.5f, 21, 3.5f, 2); L(12, 7, 12, 20); L(8, 11, 12, 7); L(16, 11, 12, 7); break;
    case G::VMid: L(3, 12, 21, 12, 2); L(12, 2, 12, 9); L(9, 6, 12, 9); L(15, 6, 12, 9); L(12, 22, 12, 15); L(9, 18, 12, 15); L(15, 18, 12, 15); break;
    case G::VBot: L(3, 20.5f, 21, 20.5f, 2); L(12, 4, 12, 17); L(8, 13, 12, 17); L(16, 13, 12, 17); break;
    case G::Wrap: {
        hl(5, 3, 21); hl(19, 3, 9);
        dl->PathLineTo(P(3, 12)); dl->PathLineTo(P(17, 12));
        dl->PathArcTo(P(17, 15.5f), 3.5f * k, -IM_PI / 2, IM_PI / 2, 8); dl->PathLineTo(P(13, 19));
        dl->PathStroke(col, 0, lw);
        L(13, 19, 15.5f, 16.5f); L(13, 19, 15.5f, 21.5f); break;
    }
    case G::Merge: {
        R(3, 5, 21, 19); L(3, 9, 7, 9); L(17, 9, 21, 9); L(3, 15, 7, 15); L(17, 15, 21, 15);
        L(6, 12, 18, 12); L(6, 12, 8.5f, 9.5f); L(6, 12, 8.5f, 14.5f); L(18, 12, 15.5f, 9.5f); L(18, 12, 15.5f, 14.5f); break;
    }
    case G::Percent: {
        dl->AddCircle(P(7, 7), 3 * k, col, 12, lw); dl->AddCircle(P(17, 17), 3 * k, col, 12, lw); L(18, 4, 6, 20); break;
    }
    case G::Thousands: T("000", 1.f, 2.f, g_font_bold, 13, col); T(",", 9.f, 6.f, g_font_bold, 16, col); break;
    case G::DecInc: T(".0", 1.f, 1.f, nullptr, 12, col); T(".00", 7.f, 11.f, nullptr, 12, col); L(4, 18, 4, 14); L(2.5f, 16, 5.5f, 16); break;
    case G::DecDec: T(".00", 1.f, 1.f, nullptr, 12, col); T(".0", 10.f, 11.f, nullptr, 12, col); L(2.5f, 17, 6.5f, 17); break;
    case G::Currency: T("$", 6.f, 1.f, g_font_bold, 20, col); break;
    case G::AutoSum: {
        ImVec2 q[5] = {P(18, 5), P(6, 5), P(13, 12), P(6, 19), P(18, 19)};
        dl->AddPolyline(q, 5, col, 0, lw * 1.3f); break;
    }
    case G::FillDown: R(5, 3, 19, 8); L(12, 10, 12, 20); L(8, 16, 12, 20); L(16, 16, 12, 20); break;
    case G::Clear: {
        ImVec2 q[4] = {P(3, 15), P(12, 6), P(19, 13), P(12, 20)};
        dl->AddPolyline(q, 4, col, ImDrawFlags_Closed, lw); L(7.5f, 10.5f, 14.5f, 17.5f); hl(21, 12, 21); break;
    }
    case G::SortFilter: case G::Funnel: {
        ImVec2 q[6] = {P(3, 4), P(21, 4), P(14, 12), P(14, 20), P(10, 18), P(10, 12)};
        dl->AddPolyline(q, 6, col, ImDrawFlags_Closed, lw);
        if (g == G::SortFilter) { RF(16, 15, 22, 16.6f, acc); RF(16, 18.5f, 20, 20.1f, acc); }
        break;
    }
    case G::FindSel: dl->AddCircle(P(10, 10), 6 * k, col, 16, lw); L(14.5f, 14.5f, 21, 21, 2.4f); break;
    case G::CondFmt: {
        RF(3, 3, 21, 8.5f, col_alpha(IM_COL32(220, 80, 70, 255), 0.85f));
        RF(3, 9.5f, 21, 15, col_alpha(IM_COL32(240, 190, 60, 255), 0.85f));
        RF(3, 16, 21, 21.5f, col_alpha(IM_COL32(80, 170, 90, 255), 0.85f));
        R(3, 3, 21, 21.5f, 1); break;
    }
    case G::CellStyles: {
        RF(3, 4, 11, 11, col_alpha(IM_COL32(80, 170, 90, 255), 0.7f)); RF(13, 4, 21, 11, col_alpha(IM_COL32(220, 80, 70, 255), 0.7f));
        RF(3, 13, 11, 20, col_alpha(IM_COL32(240, 190, 60, 255), 0.7f)); RF(13, 13, 21, 20, col_alpha(acc, 0.7f));
        R(3, 4, 11, 11, 1); R(13, 4, 21, 11, 1); R(3, 13, 11, 20, 1); R(13, 13, 21, 20, 1); break;
    }
    case G::InsRow: R(3, 10, 21, 17); L(3, 13.5f, 21, 13.5f, 1); RF(3, 10, 21, 17, col_alpha(acc, 0.25f)); L(12, 2, 12, 8, 2); L(9, 5, 15, 5, 2); break;
    case G::DelRow: R(3, 10, 21, 17); RF(3, 10, 21, 17, col_alpha(th().error, 0.25f)); L(9, 2, 15, 8, 2); L(15, 2, 9, 8, 2); break;
    case G::InsCol: R(10, 3, 17, 21); RF(10, 3, 17, 21, col_alpha(acc, 0.25f)); L(2, 12, 8, 12, 2); L(5, 9, 5, 15, 2); break;
    case G::DelCol: R(10, 3, 17, 21); RF(10, 3, 17, 21, col_alpha(th().error, 0.25f)); L(2, 9, 8, 15, 2); L(8, 9, 2, 15, 2); break;
    case G::RowHeight: R(6, 7, 22, 17); L(3, 3, 3, 21); L(1, 5, 3, 3); L(5, 5, 3, 3); L(1, 19, 3, 21); L(5, 19, 3, 21); break;
    case G::ColWidth: R(7, 6, 17, 22); L(3, 3, 21, 3); L(5, 1, 3, 3); L(5, 5, 3, 3); L(19, 1, 21, 3); L(19, 5, 21, 3); break;
    case G::Hide: {
        dl->PathArcTo(P(12, 18), 10 * k, -IM_PI * 0.85f, -IM_PI * 0.15f, 12); dl->PathStroke(col, 0, lw);
        dl->PathArcTo(P(12, 6), 10 * k, IM_PI * 0.15f, IM_PI * 0.85f, 12); dl->PathStroke(col, 0, lw);
        dl->AddCircle(P(12, 12), 2.6f * k, col, 10, lw); L(4, 20, 20, 4); break;
    }
    case G::TextToCols: R(2, 6, 10, 18); R(14, 6, 22, 18); L(11, 12, 13, 12); L(4, 10, 8, 10, 1); L(4, 14, 8, 14, 1); L(16, 10, 20, 10, 1); L(16, 14, 20, 14, 1); break;
    case G::Dedupe: R(2, 3, 14, 13); R(8, 9, 20, 19); RF(8, 9, 20, 19, col_alpha(th().error, 0.2f)); L(15, 20, 22, 13, 1.4f); break;
    case G::Validation: R(3, 3, 21, 21); L(7, 12, 10.5f, 16, 2); L(10.5f, 16, 17, 7.5f, 2); break;
    case G::Freeze: grid9(dim); RF(4, 4, 20, 8, col_alpha(acc, 0.35f)); RF(4, 4, 8, 20, col_alpha(acc, 0.35f)); L(4, 8, 20, 8, 1.8f); L(8, 4, 8, 20, 1.8f); R(4, 4, 20, 20, 1); break;
    case G::Gridlines: grid9(col); R(4, 4, 20, 20, 1); break;
    case G::ZoomG: dl->AddCircle(P(10, 10), 6 * k, col, 16, lw); L(14.5f, 14.5f, 21, 21, 2.4f); L(7, 10, 13, 10); L(10, 7, 10, 13); break;
    case G::Refdes: T("R1", 1.5f, 1.f, g_font_bold, 12, col); T("..5", 9.f, 11.f, nullptr, 12, col); break;
    case G::RefCount: T("#", 3.f, 2.f, g_font_bold, 19, col); T("R", 13.f, 9.f, g_font_bold, 12, acc); break;
    case G::QtyCheck: T("Q", 1.5f, 1.f, g_font_bold, 14, col); L(11, 16, 14.5f, 20, 2); L(14.5f, 20, 22, 10, 2); break;
    case G::SplitDnp: T("DNP", 0.5f, 5.f, g_font_bold, 11, col); L(2, 18, 22, 18, 1); L(12, 18, 12, 22, 1); break;
    case G::Send: {
        ImVec2 q[4] = {P(2, 11), P(22, 3), P(15, 21), P(11, 13)};
        dl->AddPolyline(q, 4, col, ImDrawFlags_Closed, lw); L(11, 13, 22, 3); break;
    }
    case G::Sheet: R(3, 3, 21, 21); L(3, 8, 21, 8); L(9, 3, 9, 21); L(3, 13, 21, 13, 1); L(3, 17, 21, 17, 1); L(15, 3, 15, 21, 1); break;
    case G::Fx: T("f", 4.f, 1.f, g_font_bold ? g_font_bold : nullptr, 19, col); T("x", 12.f, 6.5f, nullptr, 14, col); break;
    case G::Indent: hl(4, 3, 21); hl(20, 3, 21); hl(10, 11, 21); hl(14, 11, 21); L(3, 9, 7, 12); L(3, 15, 7, 12); break;
    case G::Outdent: hl(4, 3, 21); hl(20, 3, 21); hl(10, 11, 21); hl(14, 11, 21); L(7, 9, 3, 12); L(7, 15, 3, 12); break;
    case G::Count: break;
    }
}

// ===========================================================================
// buttons
// ===========================================================================
static void hover_bg(ImDrawList* dl, ImVec2 a, ImVec2 b, ImGuiID aid, bool hov, bool act, bool checked, bool enabled) {
    const GTheme& t = th();
    float h = anim(aid, hov && enabled ? 1.f : 0.f, 20.f);
    float pr = anim(aid + 1, act && enabled ? 1.f : 0.f, 30.f);
    float ck = anim(aid + 2, checked ? 1.f : 0.f, 16.f);
    if (ck > 0.01f) {
        dl->AddRectFilled(a, b, col_alpha(t.accent, 0.22f * ck), S(5));
        dl->AddRect(a, b, col_alpha(t.accent, 0.69f * ck), S(5), 0, 1.2f);
    }
    if (h > 0.01f) dl->AddRectFilled(a, b, col_alpha(col_lerp(t.raised, t.border, pr), h * (1 - 0.5f * ck)), S(5));
}

bool gbtn(const char* id, G g, const char* tip, bool checked, bool enabled, ImU32 accent, float size) {
    if (size <= 0) size = S(26);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::BeginDisabled(!enabled);
    bool clicked = ImGui::InvisibleButton("##gb", ImVec2(size, size));
    bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    ImGui::EndDisabled();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    hover_bg(dl, p, ImVec2(p.x + size, p.y + size), ImGui::GetID("##gb"), hov, act, checked, enabled);
    float is = std::round(size * 0.70f);
    ImU32 c = enabled ? th().text : col_alpha(th().text, 0.35f);
    glyph(dl, g, ImVec2(p.x + (size - is) * 0.5f, p.y + (size - is) * 0.5f), is, c, accent);
    if (hov && tip) tooltip(tip);
    ImGui::PopID();
    return clicked && enabled;
}

bool ibtn(const char* id, ui::Icon ic, const char* tip, bool checked, bool enabled, float size) {
    if (size <= 0) size = S(26);
    return icon_button(id, ic, tip, checked, enabled, size);
}

static void arrow_down(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    dl->AddTriangleFilled(ImVec2(c.x - s, c.y - s * 0.5f), ImVec2(c.x + s, c.y - s * 0.5f), ImVec2(c.x, c.y + s * 0.6f), col);
}

int gsplit(const char* id, G g, const char* tip, ImU32 accent, bool enabled) {
    float size = S(26), aw = S(12);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::BeginDisabled(!enabled);
    int res = 0;
    bool c1 = ImGui::InvisibleButton("##m", ImVec2(size, size));
    bool h1 = ImGui::IsItemHovered(), a1 = ImGui::IsItemActive();
    ImGuiID id1 = ImGui::GetItemID();
    ImGui::SameLine(0, 0);
    bool c2 = ImGui::InvisibleButton("##a", ImVec2(aw, size));
    bool h2 = ImGui::IsItemHovered(), a2 = ImGui::IsItemActive();
    ImGuiID id2 = ImGui::GetItemID();
    ImGui::EndDisabled();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (h1 || h2) dl->AddRect(p, ImVec2(p.x + size + aw, p.y + size), col_alpha(th().border, 0.8f), S(5));
    hover_bg(dl, p, ImVec2(p.x + size, p.y + size), id1, h1, a1, false, enabled);
    hover_bg(dl, ImVec2(p.x + size, p.y), ImVec2(p.x + size + aw, p.y + size), id2, h2, a2, false, enabled);
    float is = std::round(size * 0.70f);
    ImU32 c = enabled ? th().text : col_alpha(th().text, 0.35f);
    glyph(dl, g, ImVec2(p.x + (size - is) * 0.5f, p.y + (size - is) * 0.5f), is, c, accent);
    arrow_down(dl, ImVec2(p.x + size + aw * 0.5f, p.y + size * 0.5f), S(3), th().text_dim);
    if ((h1 || h2) && tip) tooltip(tip);
    if (c1 && enabled) res = 1;
    if (c2 && enabled) res = 2;
    ImGui::PopID();
    return res;
}

bool big_btn(const char* id, ui::Icon ic, G g, const char* label, const char* tip, bool primary, bool enabled, bool arrow) {
    const GTheme& t = th();
    ImVec2 ts = ImGui::CalcTextSize(label);
    float w = std::max(S(44), ts.x + S(12) + (arrow ? S(8) : 0)), h = S(54);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::BeginDisabled(!enabled);
    bool clicked = ImGui::InvisibleButton("##bb", ImVec2(w, h));
    bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    ImGui::EndDisabled();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiID aid = ImGui::GetItemID();
    if (primary) {
        float hv = anim(aid, hov && enabled ? 1.f : 0.f, 20.f);
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), act ? t.accent : col_lerp(t.accent, t.accent_hover, hv), S(6));
    } else hover_bg(dl, p, ImVec2(p.x + w, p.y + h), aid, hov, act, false, enabled);
    float is = S(24);
    ImVec2 ip(p.x + (w - is) * 0.5f, p.y + S(5));
    ImU32 tc = primary ? t.accent_text : (enabled ? t.text : col_alpha(t.text, 0.4f));
    if (ic != ui::Icon::None) icon_draw(dl, ic, ip, is, enabled ? 1.f : 0.4f, primary);
    else glyph(dl, g, ip, is, tc);
    float tx = p.x + (w - ts.x - (arrow ? S(8) : 0)) * 0.5f;
    dl->AddText(ImVec2(tx, p.y + h - ts.y - S(5)), tc, label);
    if (arrow) arrow_down(dl, ImVec2(tx + ts.x + S(6), p.y + h - ts.y * 0.5f - S(5)), S(2.5f), primary ? t.accent_text : t.text_dim);
    if (hov && tip) tooltip(tip);
    ImGui::PopID();
    return clicked && enabled;
}

bool gitem(const char* label, G g, const char* shortcut, bool enabled, bool checked) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool r = menu_item_icon(label, ui::Icon::None, shortcut, checked, enabled);
    float s = ImGui::GetTextLineHeight();
    glyph(ImGui::GetWindowDrawList(), g, ImVec2(p.x + S(2), p.y + (ImGui::GetItemRectSize().y - s) * 0.5f), s, enabled ? th().text : th().text_faint);
    return r;
}

// ---------------------------------------------------------------------------
// ribbon groups
// ---------------------------------------------------------------------------
struct GroupFrame { const char* caption; ImVec2 start; };
static std::vector<GroupFrame> g_groups;

void begin_group(const char* caption) {
    g_groups.push_back({caption, ImGui::GetCursorScreenPos()});
    ImGui::BeginGroup();
}

void end_group() {
    ImGui::EndGroup();
    if (g_groups.empty()) return;
    GroupFrame f = g_groups.back();
    g_groups.pop_back();
    ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const GTheme& t = th();
    float cy = f.start.y + S(58);
    ImFont* fnt = ImGui::GetFont();
    float fs = ImGui::GetFontSize() * 0.82f;
    ImVec2 cs = fnt->CalcTextSizeA(fs, FLT_MAX, 0, f.caption);
    float right = std::max(mx.x, mn.x + cs.x);
    dl->AddText(fnt, fs, ImVec2(mn.x + (right - mn.x - cs.x) * 0.5f, cy), t.text_faint, f.caption);
    float sx = right + S(7);
    dl->AddLine(ImVec2(sx, f.start.y + S(2)), ImVec2(sx, cy + cs.y), col_alpha(t.border, 0.8f));
    ImGui::SetCursorScreenPos(ImVec2(right, f.start.y));
    ImGui::Dummy(ImVec2(0, 0));
}

// ---------------------------------------------------------------------------
// colour palette
// ---------------------------------------------------------------------------
static std::vector<uint32_t> g_recent;
void remember_color(uint32_t argb) {
    if (!argb) return;
    g_recent.erase(std::remove(g_recent.begin(), g_recent.end(), argb), g_recent.end());
    g_recent.insert(g_recent.begin(), argb);
    if (g_recent.size() > 10) g_recent.resize(10);
}

static uint32_t tint(uint32_t c, float f) {   // f>0 lighten toward white, f<0 darken
    int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    auto m = [&](int v) { return f >= 0 ? (int)(v + (255 - v) * f) : (int)(v * (1 + f)); };
    return 0xFF000000u | (m(r) << 16) | (m(g) << 8) | m(b);
}

static bool swatch(const char* id, uint32_t c, float s, bool sel) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clk = ImGui::InvisibleButton(id, ImVec2(s, s));
    bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + s, p.y + s), argb_to_u32(c));
    dl->AddRect(p, ImVec2(p.x + s, p.y + s), hov || sel ? th().accent : col_alpha(th().border, 0.7f), 0, 0, hov || sel ? S(2) : 1.f);
    if (hov) {
        char b[24]; std::snprintf(b, sizeof b, "#%06X", c & 0xFFFFFF);
        tooltip(b);
    }
    return clk;
}

bool color_palette(uint32_t& out, const char* none_label) {
    static const uint32_t base[10] = {0xFFFFFFFF, 0xFF000000, 0xFFE7E6E6, 0xFF44546A, 0xFF4472C4, 0xFFED7D31, 0xFFA5A5A5, 0xFFFFC000, 0xFF5B9BD5, 0xFF70AD47};
    static const float shades[5] = {0.8f, 0.6f, 0.4f, -0.25f, -0.5f};
    static const uint32_t std_[10] = {0xFFC00000, 0xFFFF0000, 0xFFFFC000, 0xFFFFFF00, 0xFF92D050, 0xFF00B050, 0xFF00B0F0, 0xFF0070C0, 0xFF002060, 0xFF7030A0};
    bool chosen = false;
    const float s = S(17), gap = S(3);
    if (button(none_label, ui::Icon::None, false, ImVec2(10 * (s + gap) - gap, 0))) { out = 0; chosen = true; }
    section_header("Theme colours");
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    for (int row = 0; row < 6; ++row) {
        for (int i = 0; i < 10; ++i) {
            uint32_t c = row == 0 ? base[i] : tint(base[i], (i == 1 && shades[row - 1] < 0) ? -shades[row - 1] : (i == 0 && shades[row - 1] > 0 ? -shades[row - 1] * 0.3f : shades[row - 1]));
            char id[16]; std::snprintf(id, sizeof id, "##t%d_%d", row, i);
            if (i) ImGui::SameLine();
            if (swatch(id, c, s, out == c)) { out = c; chosen = true; }
        }
        if (row == 0) ImGui::Dummy(ImVec2(0, S(2)));
    }
    ImGui::PopStyleVar();
    section_header("Standard colours");
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    for (int i = 0; i < 10; ++i) {
        char id[16]; std::snprintf(id, sizeof id, "##s%d", i);
        if (i) ImGui::SameLine();
        if (swatch(id, std_[i], s, out == std_[i])) { out = std_[i]; chosen = true; }
    }
    if (!g_recent.empty()) {
        ImGui::PopStyleVar();
        section_header("Recent colours");
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
        for (size_t i = 0; i < g_recent.size(); ++i) {
            char id[16]; std::snprintf(id, sizeof id, "##r%d", (int)i);
            if (i) ImGui::SameLine();
            if (swatch(id, g_recent[i], s, out == g_recent[i])) { out = g_recent[i]; chosen = true; }
        }
    }
    ImGui::PopStyleVar();
    ImGui::Spacing();
    if (ImGui::TreeNodeEx("More colours\xE2\x80\xA6", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        static float col[3] = {0.27f, 0.45f, 0.77f};
        if (out) { col[0] = ((out >> 16) & 255) / 255.f; col[1] = ((out >> 8) & 255) / 255.f; col[2] = (out & 255) / 255.f; }
        ImGui::SetNextItemWidth(10 * (s + gap));
        ImGui::ColorPicker3("##pick", col, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview | ImGuiColorEditFlags_DisplayHex);
        if (button("Use colour", ui::Icon::Check, true)) {
            out = 0xFF000000u | ((uint32_t)(col[0] * 255 + .5f) << 16) | ((uint32_t)(col[1] * 255 + .5f) << 8) | (uint32_t)(col[2] * 255 + .5f);
            remember_color(out);
            chosen = true;
        }
        ImGui::TreePop();
    }
    return chosen;
}

// ===========================================================================
// formula text helpers
// ===========================================================================
static const ImU32 k_ref_cols[8] = {IM_COL32(64, 128, 255, 255), IM_COL32(230, 70, 70, 255), IM_COL32(160, 90, 220, 255), IM_COL32(40, 170, 90, 255),
                                    IM_COL32(230, 140, 30, 255), IM_COL32(20, 170, 190, 255), IM_COL32(210, 70, 160, 255), IM_COL32(130, 130, 40, 255)};
ImU32 ref_color(int i) { return k_ref_cols[((i % 8) + 8) % 8]; }

static std::string colname(int c) {
    std::string s;
    for (++c; c > 0; c = (c - 1) / 26) s.insert(s.begin(), (char)('A' + (c - 1) % 26));
    return s;
}
std::string a1(int r, int c, bool ar, bool ac) { return (ac ? "$" : "") + colname(c) + (ar ? "$" : "") + std::to_string(r + 1); }
std::string a1_range(int r0, int c0, int r1, int c1) {
    if (r0 == r1 && c0 == c1) return a1(r0, c0);
    return a1(std::min(r0, r1), std::min(c0, c1)) + ":" + a1(std::max(r0, r1), std::max(c0, c1));
}

// parse "$A$1" at s[i]; returns chars consumed (0 = none)
static int parse_cell(const char* s, int i, int& r, int& c) {
    int k = i;
    if (s[k] == '$') ++k;
    int cs = k, col = 0;
    while (isalpha((unsigned char)s[k]) && k - cs < 3) { col = col * 26 + (toupper((unsigned char)s[k]) - 'A' + 1); ++k; }
    if (k == cs || isalpha((unsigned char)s[k])) return 0;
    if (s[k] == '$') ++k;
    int rs = k, row = 0;
    while (isdigit((unsigned char)s[k]) && k - rs < 7) { row = row * 10 + (s[k] - '0'); ++k; }
    if (k == rs || row == 0) return 0;
    if (isalnum((unsigned char)s[k]) || s[k] == '_' || s[k] == '(') return 0;
    r = row - 1; c = col - 1;
    return k - i;
}

bool ref_insert_point(const char* s, int caret) {
    if (!s || s[0] != '=' || caret < 1) return false;
    int i = caret - 1;
    while (i > 0 && s[i] == ' ') --i;
    char c = s[i];
    // inside a string literal?
    int q = 0;
    for (int k = 0; k < caret; ++k) if (s[k] == '"') ++q;
    if (q & 1) return false;
    return strchr("=(,+-*/^&<>:;", c) != nullptr;
}

int ident_at(const char* s, int caret, std::string& out) {
    out.clear();
    if (!s || s[0] != '=') return -1;
    int i = caret;
    while (i > 1 && (isalpha((unsigned char)s[i - 1]) || isdigit((unsigned char)s[i - 1]) || s[i - 1] == '.' || s[i - 1] == '_')) --i;
    if (i >= caret || !isalpha((unsigned char)s[i])) return -1;
    if (i > 0 && (s[i - 1] == '"' || s[i - 1] == '!' || s[i - 1] == '$')) return -1;
    out.assign(s + i, caret - i);
    for (auto& c : out) c = (char)toupper((unsigned char)c);
    // a pure cell ref (A1) is not a function prefix unless followed by letters
    int r, c;
    if (parse_cell(s, i, r, c) == caret - i) return -1;
    return i;
}

bool call_at(const char* s, int caret, std::string& fn, int& arg) {
    if (!s || s[0] != '=') return false;
    struct Fr { int name_b, name_e, arg; };
    std::vector<Fr> st;
    bool str = false;
    for (int i = 1; i < caret && s[i]; ++i) {
        char c = s[i];
        if (c == '"') { str = !str; continue; }
        if (str) continue;
        if (c == '(') {
            int e = i, b = i;
            while (b > 1 && (isalnum((unsigned char)s[b - 1]) || s[b - 1] == '.' || s[b - 1] == '_')) --b;
            st.push_back({b, e, 0});
        } else if (c == ')') { if (!st.empty()) st.pop_back(); }
        else if ((c == ',' || c == ';') && !st.empty()) st.back().arg++;
    }
    while (!st.empty() && st.back().name_b == st.back().name_e) st.pop_back();
    if (st.empty()) return false;
    fn.assign(s + st.back().name_b, st.back().name_e - st.back().name_b);
    for (auto& c : fn) c = (char)toupper((unsigned char)c);
    arg = st.back().arg;
    return true;
}

} // namespace pcbp::gui::sg
