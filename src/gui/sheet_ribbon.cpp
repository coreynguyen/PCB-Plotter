// ============================================================================
// sheet_ribbon.cpp — spreadsheet ribbon (Home / Data / BOM / View tabs with
// icon groups), "Send to Project" split button, name box + formula bar
// (reference colouring, point mode, function autocomplete + argument hints),
// sheet tabs and the status strip (Sum / Average / Count, zoom).
// ============================================================================
#include "sheet_editor.h"
#include "../app/commands.h"
#include "../ui/form.h"
#include "../ui/win.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace pcbp::gui {
using namespace sheet;
using sg::G;

static const char* k_fonts[] = {"Calibri", "Aptos", "Arial", "Segoe UI", "Tahoma", "Verdana", "Cambria", "Times New Roman", "Consolas", "Courier New"};
static const int k_sizes[] = {8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 28, 36, 48, 72};

struct NumCat { const char* label; const char* code; };
static const NumCat k_numfmts[] = {
    {"General", "General"}, {"Number", "0.00"}, {"Number (1,000)", "#,##0.00"}, {"Currency", "$#,##0.00"},
    {"Accounting", "_($* #,##0.00_);_($* (#,##0.00);_($* \"-\"??_);_(@_)"}, {"Short Date", "m/d/yyyy"},
    {"Long Date", "dddd, mmmm d, yyyy"}, {"ISO Date", "yyyy-mm-dd"}, {"Time", "h:mm:ss AM/PM"}, {"Percentage", "0.00%"},
    {"Fraction", "# ?/?"}, {"Scientific", "0.00E+00"}, {"Text", "@"},
};

// small helper: vertical stack of two button rows inside a ribbon group
static void row_gap() { ImGui::SameLine(0, S(2)); }

// ===========================================================================
// ribbon
// ===========================================================================
void SheetDoc::draw_ribbon() {
    const GTheme& t = th();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float W = ImGui::GetContentRegionAvail().x;
    const float TH = S(28), BH = ribbon_min ? 0 : S(80);
    dl->AddRectFilled(p, ImVec2(p.x + W, p.y + TH), t.panel2);
    if (!ribbon_min) dl->AddRectFilled(ImVec2(p.x, p.y + TH), ImVec2(p.x + W, p.y + TH + BH), t.panel);
    dl->AddLine(ImVec2(p.x, p.y + TH + BH - 0.5f), ImVec2(p.x + W, p.y + TH + BH - 0.5f), t.border);
    // ---- tab strip
    static const char* tabs[] = {"Home", "Data", "BOM", "View"};
    ImGui::SetCursorScreenPos(ImVec2(p.x + S(8), p.y + S(2)));
    for (int i = 0; i < 4; ++i) {
        ImGui::PushID(i);
        ImVec2 ts = ImGui::CalcTextSize(tabs[i]);
        ImVec2 a = ImGui::GetCursorScreenPos();
        float w = ts.x + S(24);
        if (ImGui::InvisibleButton("##rt", ImVec2(w, TH - S(2)))) { if (ribbon_min) ribbon_min = false; ribbon_tab = i; }   // collapsed: any tab click shows its ribbon
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) ribbon_min = !ribbon_min;
        bool hv = ImGui::IsItemHovered(), sel = ribbon_tab == i;
        float an = anim(ImGui::GetItemID(), sel ? 1.f : 0.f, 16.f);
        if (sel && !ribbon_min) dl->AddRectFilled(ImVec2(a.x, a.y + S(2)), ImVec2(a.x + w, a.y + TH), t.panel, S(6), ImDrawFlags_RoundCornersTop);
        else if (hv) dl->AddRectFilled(ImVec2(a.x, a.y + S(3)), ImVec2(a.x + w, a.y + TH - S(4)), col_alpha(t.raised, 0.8f), S(5));
        dl->AddText(ImVec2(a.x + (w - ts.x) * 0.5f, a.y + (TH - ts.y) * 0.5f), sel ? t.text : t.text_dim, tabs[i]);
        if (an > 0.01f) dl->AddLine(ImVec2(a.x + w * 0.5f - w * 0.3f * an, a.y + TH - S(3)), ImVec2(a.x + w * 0.5f + w * 0.3f * an, a.y + TH - S(3)), t.accent, S(2.5f));
        ImGui::SameLine(0, S(2));
        ImGui::PopID();
    }
    // quick access: undo / redo / save on the strip
    ImGui::SameLine(0, S(16));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(1));
    if (sg::ibtn("q_save", ui::Icon::Save, "Save  (Ctrl+S)", false, true, S(24))) defer([this]() { save(false); });
    row_gap();
    std::string ul = "Undo " + b.undo_label() + "  (Ctrl+Z)", rl = "Redo " + b.redo_label() + "  (Ctrl+Y)";
    if (sg::ibtn("q_undo", ui::Icon::Undo, ul.c_str(), false, b.can_undo() || editing, S(24))) { if (editing) cancel_edit(); else b.undo(); }
    row_gap();
    if (sg::ibtn("q_redo", ui::Icon::Redo, rl.c_str(), false, b.can_redo(), S(24))) b.redo();
    // link banner (project BOM)
    if (auto d = link_doc.lock()) {
        ImGui::SameLine(0, S(16));
        std::string lb = "Linked to " + ui::U(d->display_name());
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.accent), "\xE2\x97\x8F");
        ImGui::SameLine(0, S(4));
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_dim), "%s", lb.c_str());
    }
    // right: Send to Project (always visible) + collapse
    ImGui::SetCursorScreenPos(ImVec2(p.x + W - S(30) - S(176), p.y + S(2)));
    send_button();
    ImGui::SetCursorScreenPos(ImVec2(p.x + W - S(30), p.y + S(2)));
    if (sg::gbtn("rb_min", ribbon_min ? G::VBot : G::VTop, ribbon_min ? "Show the ribbon  (Ctrl+F1)" : "Collapse the ribbon  (Ctrl+F1)", false, true, 0, S(24))) ribbon_min = !ribbon_min;
    // ---- body
    if (!ribbon_min) {
        ImGui::SetCursorScreenPos(ImVec2(p.x + S(8), p.y + TH + S(4)));
        ImGui::BeginGroup();
        switch (ribbon_tab) {
        case 0: ribbon_home(); break;
        case 1: ribbon_data(); break;
        case 2: ribbon_bom(); break;
        default: ribbon_view(); break;
        }
        ImGui::EndGroup();
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + TH + BH));
    ImGui::Dummy(ImVec2(W, 0));
}

// "Send to Project" — primary split button
void SheetDoc::send_button() {
    const GTheme& t = th();
    bool compact = true;
    ImGui::PushID("send");
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = compact ? S(160) : S(140), h = compact ? S(24) : S(54), aw = S(22);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool c1 = ImGui::InvisibleButton("##main", ImVec2(w - aw, h));
    bool h1 = ImGui::IsItemHovered(), a1 = ImGui::IsItemActive();
    ImGui::SameLine(0, 0);
    bool c2 = ImGui::InvisibleButton("##arrow", ImVec2(aw, h));
    bool h2 = ImGui::IsItemHovered();
    float hv = anim(ImGui::GetID("##hv"), (h1 || h2) ? 1.f : 0.f, 18.f);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), a1 ? t.accent : col_lerp(t.accent, t.accent_hover, hv), S(7));
    if (h2) dl->AddRectFilled(ImVec2(p.x + w - aw, p.y), ImVec2(p.x + w, p.y + h), col_alpha(IM_COL32(0, 0, 0, 255), 0.12f), S(7), ImDrawFlags_RoundCornersRight);
    dl->AddLine(ImVec2(p.x + w - aw, p.y + S(6)), ImVec2(p.x + w - aw, p.y + h - S(6)), col_alpha(t.accent_text, 0.4f));
    const char* lbl = "Send to Project";
    ImVec2 ts = ImGui::CalcTextSize(lbl);
    if (compact) {
        sg::glyph(dl, G::Send, ImVec2(p.x + S(8), p.y + (h - S(16)) * 0.5f), S(16), t.accent_text);
        dl->AddText(ImVec2(p.x + S(30), p.y + (h - ts.y) * 0.5f), t.accent_text, lbl);
    } else {
        sg::glyph(dl, G::Send, ImVec2(p.x + (w - aw - S(22)) * 0.5f, p.y + S(5)), S(22), t.accent_text);
        dl->AddText(ImVec2(p.x + (w - aw - ts.x) * 0.5f, p.y + h - ts.y - S(5)), t.accent_text, lbl);
    }
    ImVec2 ac(p.x + w - aw * 0.5f, p.y + h * 0.5f);
    dl->AddTriangleFilled(ImVec2(ac.x - S(4), ac.y - S(2)), ImVec2(ac.x + S(4), ac.y - S(2)), ImVec2(ac.x, ac.y + S(3)), t.accent_text);
    if (h1) tooltip("Import the visible (filtered) rows into the BOM import wizard.\nHeader row and column roles are preselected.");
    if (c1) send_to_project(0);
    if (c2) ImGui::OpenPopup("##sendmenu");
    if (ImGui::BeginPopup("##sendmenu")) {
        section_header("Send visible rows to the project");
        if (menu_item_icon("Import as BOM\xE2\x80\xA6", ui::Icon::ImportBom)) send_to_project(0);
        if (menu_item_icon("Import as Centroid / Pick & Place\xE2\x80\xA6", ui::Icon::ImportCentroid)) send_to_project(1);
        if (menu_item_icon("Update BOM (merge into the current project)\xE2\x80\xA6", ui::Icon::Fusion, nullptr, false, app::active_doc() != nullptr)) send_to_project(2);
        if (!link_doc.expired()) {
            ImGui::Separator();
            if (menu_item_icon("Apply back to linked project\xE2\x80\xA6", ui::Icon::Check)) bom_apply_preview();
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void SheetDoc::borders_menu() {
    struct BI { const char* l; G g; };
    static const BI items[] = {{"Bottom Border", G::BBottom}, {"Top Border", G::BTop}, {"Left Border", G::BLeft}, {"Right Border", G::BRight},
                               {"No Border", G::BNone}, {"All Borders", G::BAll}, {"Outside Borders", G::BOutside}, {"Thick Outside Borders", G::BThickOutside}, {"Inside Borders", G::BInside}};
    for (int i = 0; i < 9; ++i) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (menu_item_icon(items[i].l, ui::Icon::None)) { last_border = i; apply_border(items[i].g); }
        float s = ImGui::GetTextLineHeight();
        sg::glyph(ImGui::GetWindowDrawList(), items[i].g, ImVec2(p.x + S(2), p.y + (ImGui::GetItemRectSize().y - s) * 0.5f), s, th().text);
        if (i == 3 || i == 4) ImGui::Separator();
    }
    ImGui::Separator();
    if (menu_item_icon("More Borders\xE2\x80\xA6", ui::Icon::Grid)) { fmt_tab = 3; open_dialog(Dlg::FormatCells); }
}

void SheetDoc::apply_border(G kind) {
    commit_edit();
    Book::Batch bt(b, "Borders");
    each_sel([&](const Range& g) {
        BorderSide thin{BorderStyle::Thin, 0}, thick{BorderStyle::Medium, 0}, none{};
        auto edge = [&](const Range& rg, int which, BorderSide s) {
            b.apply_style(cur, rg, [which, s](Style& st) {
                switch (which) { case 0: st.border.top = s; break; case 1: st.border.bottom = s; break; case 2: st.border.left = s; break; default: st.border.right = s; }
            }, "Border");
        };
        Range top(g.r0, g.c0, g.r0, g.c1), bot(g.r1, g.c0, g.r1, g.c1), lft(g.r0, g.c0, g.r1, g.c0), rgt(g.r0, g.c1, g.r1, g.c1);
        switch (kind) {
        case G::BBottom: edge(bot, 1, thin); break;
        case G::BTop: edge(top, 0, thin); break;
        case G::BLeft: edge(lft, 2, thin); break;
        case G::BRight: edge(rgt, 3, thin); break;
        case G::BNone:
            b.apply_style(cur, g, [](Style& st) { st.border.left = st.border.right = st.border.top = st.border.bottom = BorderSide{}; }, "No border");
            // neighbours' facing sides too
            if (g.r0 > 0) edge(Range(g.r0 - 1, g.c0, g.r0 - 1, g.c1), 1, none);
            if (g.c0 > 0) edge(Range(g.r0, g.c0 - 1, g.r1, g.c0 - 1), 3, none);
            break;
        case G::BAll: b.apply_style(cur, g, [thin](Style& st) { st.border.left = st.border.right = st.border.top = st.border.bottom = thin; }, "All borders"); break;
        case G::BOutside: edge(top, 0, thin); edge(bot, 1, thin); edge(lft, 2, thin); edge(rgt, 3, thin); break;
        case G::BThickOutside: edge(top, 0, thick); edge(bot, 1, thick); edge(lft, 2, thick); edge(rgt, 3, thick); break;
        case G::BInside:
            if (g.cols() > 1) edge(Range(g.r0, g.c0, g.r1, g.c1 - 1), 3, thin);
            if (g.rows() > 1) edge(Range(g.r0, g.c0, g.r1 - 1, g.c1), 1, thin);
            break;
        default: break;
        }
    });
}

// ---------------------------------------------------------------- Home
void SheetDoc::ribbon_home() {
    const Style& st = b.style_of(cur, ar(), ac());
    // Clipboard
    sg::begin_group("Clipboard");
    if (sg::big_btn("paste", ui::Icon::Paste, G::None, "Paste", "Paste  (Ctrl+V)\nArrow: Paste Special", false, true, true)) {
        ImVec2 mn = ImGui::GetItemRectMin(), mxx = ImGui::GetItemRectMax();
        if (ImGui::GetIO().MousePos.y > mxx.y - S(20) || ImGui::GetIO().MousePos.x > mxx.x - S(12)) ImGui::OpenPopup("##pastemenu");
        else paste();
        (void)mn;
    }
    if (ImGui::BeginPopup("##pastemenu")) {
        if (menu_item_icon("Paste", ui::Icon::Paste, "Ctrl+V")) paste();
        if (menu_item_icon("Values", ui::Icon::Text)) paste(PasteValues);
        if (menu_item_icon("Formulas", ui::Icon::Text)) paste(PasteFormulas);
        if (menu_item_icon("Formatting", ui::Icon::Palette)) paste(PasteFormats);
        if (menu_item_icon("Transpose", ui::Icon::Flip)) paste(PasteAll | PasteTranspose);
        ImGui::Separator();
        if (menu_item_icon("Paste Special\xE2\x80\xA6", ui::Icon::Paste, "Ctrl+Shift+V")) open_dialog(Dlg::PasteSpecial);
        ImGui::EndPopup();
    }
    ImGui::SameLine(0, S(4));
    ImGui::BeginGroup();
    if (sg::ibtn("cut", ui::Icon::Cut, "Cut  (Ctrl+X)")) copy(true);
    row_gap();
    if (sg::ibtn("copy", ui::Icon::Copy, "Copy  (Ctrl+C)\nExcel-compatible: values + formatting")) copy(false);
    if (sg::ibtn("fp", ui::Icon::Palette, "Format Painter\nCopy the active cell's formatting, then drag over the target cells", fp_on)) {
        fp_on = !fp_on;
        if (fp_on) fp_style = b.style_of(cur, ar(), ac());
    }
    ImGui::EndGroup();
    sg::end_group();
    ImGui::SameLine(0, S(14));
    // Font
    sg::begin_group("Font");
    ImGui::BeginGroup();
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(6), S(4)));
        ImGui::SetNextItemWidth(S(130));
        if (ImGui::BeginCombo("##font", st.font.name.c_str())) {
            for (const char* f : k_fonts) if (ImGui::Selectable(f, st.font.name == f)) { std::string n = f; style_sel("Font", [n](Style& s) { s.font.name = n; }); }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) tooltip("Font (Consolas / Courier render monospaced)");
        ImGui::SameLine(0, S(4));
        ImGui::SetNextItemWidth(S(58));
        char sz[16]; std::snprintf(sz, sizeof sz, "%g", st.font.size);
        if (ImGui::BeginCombo("##fsize", sz)) {
            for (int s : k_sizes) { char l[8]; std::snprintf(l, sizeof l, "%d", s); if (ImGui::Selectable(l, (int)st.font.size == s)) style_sel("Font size", [s](Style& x) { x.font.size = s; }); }
            ImGui::EndCombo();
        }
        ImGui::PopStyleVar();
        row_gap();
        if (sg::gbtn("fgrow", G::DecInc, "Increase font size")) { double s0 = st.font.size; style_sel("Font size", [s0](Style& x) { x.font.size = std::min(409.0, (x.font.size >= 12 ? x.font.size + 2 : x.font.size + 1)); (void)s0; }); }
        if (ImGui::IsItemHovered()) {}
    }
    if (sg::gbtn("b", G::Bold, "Bold  (Ctrl+B)", st.font.bold)) { bool v = !st.font.bold; style_sel("Bold", [v](Style& s) { s.font.bold = v; }); }
    row_gap();
    if (sg::gbtn("i", G::Italic, "Italic  (Ctrl+I)", st.font.italic)) { bool v = !st.font.italic; style_sel("Italic", [v](Style& s) { s.font.italic = v; }); }
    row_gap();
    if (sg::gbtn("u", G::Underline, "Underline  (Ctrl+U)", st.font.underline != Underline::None)) { bool v = st.font.underline == Underline::None; style_sel("Underline", [v](Style& s) { s.font.underline = v ? Underline::Single : Underline::None; }); }
    row_gap();
    if (sg::gbtn("s", G::Strike, "Strikethrough  (Ctrl+5)", st.font.strike)) { bool v = !st.font.strike; style_sel("Strikethrough", [v](Style& s) { s.font.strike = v; }); }
    ImGui::SameLine(0, S(6));
    static const G bg[] = {G::BBottom, G::BTop, G::BLeft, G::BRight, G::BNone, G::BAll, G::BOutside, G::BThickOutside, G::BInside};
    int r = sg::gsplit("brd", bg[last_border], "Borders");
    if (r == 1) apply_border(bg[last_border]);
    if (r == 2) ImGui::OpenPopup("##borders");
    if (ImGui::BeginPopup("##borders")) { borders_menu(); ImGui::EndPopup(); }
    row_gap();
    r = sg::gsplit("fill", G::FillColor, "Fill Color", doc_col(last_fill_color));
    if (r == 1) { uint32_t c = last_fill_color; style_sel("Fill color", [c](Style& s) { s.fill = c ? Fill{FillPattern::Solid, c, 0} : Fill{}; }); }
    if (r == 2) ImGui::OpenPopup("##fillpal");
    if (ImGui::BeginPopup("##fillpal")) {
        uint32_t c = last_fill_color;
        if (sg::color_palette(c, "No Fill")) {
            if (c) last_fill_color = c;
            style_sel("Fill color", [c](Style& s) { s.fill = c ? Fill{FillPattern::Solid, c, 0} : Fill{}; });
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    row_gap();
    r = sg::gsplit("fcol", G::FontColor, "Font Color", doc_col(last_font_color));
    if (r == 1) { uint32_t c = last_font_color; style_sel("Font color", [c](Style& s) { s.font.color = c; }); }
    if (r == 2) ImGui::OpenPopup("##fontpal");
    if (ImGui::BeginPopup("##fontpal")) {
        uint32_t c = last_font_color;
        if (sg::color_palette(c, "Automatic")) {
            if (c) last_font_color = c;
            style_sel("Font color", [c](Style& s) { s.font.color = c; });
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::EndGroup();
    sg::end_group();
    ImGui::SameLine(0, S(14));
    // Alignment
    sg::begin_group("Alignment");
    ImGui::BeginGroup();
    if (sg::gbtn("vt", G::VTop, "Top Align", st.valign == VAlign::Top)) style_sel("Align", [](Style& s) { s.valign = VAlign::Top; });
    row_gap();
    if (sg::gbtn("vm", G::VMid, "Middle Align", st.valign == VAlign::Center)) style_sel("Align", [](Style& s) { s.valign = VAlign::Center; });
    row_gap();
    if (sg::gbtn("vb", G::VBot, "Bottom Align", st.valign == VAlign::Bottom)) style_sel("Align", [](Style& s) { s.valign = VAlign::Bottom; });
    ImGui::SameLine(0, S(6));
    if (sg::gbtn("wrap", G::Wrap, "Wrap Text", st.wrap)) { bool v = !st.wrap; style_sel("Wrap text", [v](Style& s) { s.wrap = v; }); }
    if (sg::gbtn("al", G::AlignL, "Align Left", st.halign == HAlign::Left)) { bool off = st.halign == HAlign::Left; style_sel("Align", [off](Style& s) { s.halign = off ? HAlign::General : HAlign::Left; }); }
    row_gap();
    if (sg::gbtn("ac", G::AlignC, "Center", st.halign == HAlign::Center)) { bool off = st.halign == HAlign::Center; style_sel("Align", [off](Style& s) { s.halign = off ? HAlign::General : HAlign::Center; }); }
    row_gap();
    if (sg::gbtn("ar", G::AlignR, "Align Right", st.halign == HAlign::Right)) { bool off = st.halign == HAlign::Right; style_sel("Align", [off](Style& s) { s.halign = off ? HAlign::General : HAlign::Right; }); }
    ImGui::SameLine(0, S(6));
    if (sg::gbtn("outd", G::Outdent, "Decrease Indent")) style_sel("Indent", [](Style& s) { if (s.indent) --s.indent; });
    row_gap();
    if (sg::gbtn("ind", G::Indent, "Increase Indent")) style_sel("Indent", [](Style& s) { if (s.indent < 15) ++s.indent; if (s.halign == HAlign::General || s.halign == HAlign::Center) s.halign = HAlign::Left; });
    ImGui::EndGroup();
    ImGui::SameLine(0, S(4));
    bool merged = ws().merge_at(ar(), ac()) != nullptr;
    if (sg::big_btn("merge", ui::Icon::None, G::Merge, merged ? "Unmerge" : "Merge", "Merge & Center\nCombine the selected cells into one centred cell (click again to unmerge)", false, true)) merge_center();
    sg::end_group();
    ImGui::SameLine(0, S(14));
    // Number
    sg::begin_group("Number");
    ImGui::BeginGroup();
    {
        const char* curl = "Custom";
        for (auto& n : k_numfmts) if (st.numfmt == n.code) curl = n.label;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(6), S(4)));
        ImGui::SetNextItemWidth(S(150));
        if (ImGui::BeginCombo("##numfmt", curl, ImGuiComboFlags_HeightLarge)) {
            Value v = ws().value(ar(), ac());
            for (auto& n : k_numfmts) {
                Formatted f = v.is_empty() ? Formatted{} : format_value(v, n.code, b.date1904);
                if (ImGui::Selectable(n.label, st.numfmt == n.code)) set_numfmt(n.code);
                if (!f.text.empty()) { ImGui::SameLine(S(110)); ImGui::TextDisabled("%s", f.text.c_str()); }
            }
            ImGui::Separator();
            if (ImGui::Selectable("More Number Formats\xE2\x80\xA6")) { fmt_tab = 0; open_dialog(Dlg::FormatCells); }
            ImGui::EndCombo();
        }
        ImGui::PopStyleVar();
    }
    if (sg::gbtn("cur", G::Currency, "Currency format")) set_numfmt("$#,##0.00");
    row_gap();
    if (sg::gbtn("pct", G::Percent, "Percent Style  (Ctrl+Shift+%)")) set_numfmt("0%");
    row_gap();
    if (sg::gbtn("thou", G::Thousands, "Comma Style (thousands separator)")) set_numfmt("#,##0.00");
    row_gap();
    if (sg::gbtn("dinc", G::DecInc, "Increase Decimal")) bump_decimals(1);
    row_gap();
    if (sg::gbtn("ddec", G::DecDec, "Decrease Decimal")) bump_decimals(-1);
    ImGui::EndGroup();
    sg::end_group();
    ImGui::SameLine(0, S(14));
    // Styles
    sg::begin_group("Styles");
    if (sg::big_btn("cf", ui::Icon::None, G::CondFmt, "Conditional", "Conditional Formatting\nHighlight cells by rules, data bars, colour scales", false, true, true)) ImGui::OpenPopup("##cfmenu");
    if (ImGui::BeginPopup("##cfmenu")) {
        auto quick = [&](CondRule::Type ty, CmpOp op, const char* f1, int preset_fill) {
            CondFormat cf;
            each_sel([&](const Range& rg) { cf.ranges.push_back(rg); });
            CondRule ru; ru.type = ty; ru.op = op; ru.f1 = f1 ? f1 : "";
            Dxf dx;
            if (preset_fill == 0) { dx.fill_color = 0xFFFFC7CE; dx.font_color = 0xFF9C0006; }
            else if (preset_fill == 1) { dx.fill_color = 0xFFC6EFCE; dx.font_color = 0xFF006100; }
            else { dx.fill_color = 0xFFFFEB9C; dx.font_color = 0xFF9C5700; }
            b.dxfs.push_back(dx);
            ru.dxf = (int)b.dxfs.size() - 1;
            cf.rules.push_back(ru);
            b.add_cond_format(cur, cf);
        };
        section_header("Highlight cells rules");
        if (menu_item_icon("Duplicate values (light red)", ui::Icon::CloseAll)) quick(CondRule::Duplicate, CmpOp::Equal, nullptr, 0);
        if (menu_item_icon("Unique values (green)", ui::Icon::Check)) quick(CondRule::Unique, CmpOp::Equal, nullptr, 1);
        if (menu_item_icon("Blank cells (yellow)", ui::Icon::Eraser)) quick(CondRule::ContainsBlanks, CmpOp::Equal, nullptr, 2);
        if (menu_item_icon("Errors (light red)", ui::Icon::Warning)) quick(CondRule::ContainsErrors, CmpOp::Equal, nullptr, 0);
        if (menu_item_icon("Greater than / less than / between\xE2\x80\xA6", ui::Icon::Query)) open_dialog(Dlg::CondFmt);
        section_header("Visual");
        if (sg::gitem("Data bars (blue)", G::DecInc)) {
            CondFormat cf; each_sel([&](const Range& rg) { cf.ranges.push_back(rg); });
            CondRule ru; ru.type = CondRule::DataBar; ru.cfvo = {CfValueObject{CfValueObject::Min, "", 0}, CfValueObject{CfValueObject::Max, "", 0}};
            cf.rules.push_back(ru); b.add_cond_format(cur, cf);
        }
        if (sg::gitem("Colour scale (green - yellow - red)", G::CondFmt)) {
            CondFormat cf; each_sel([&](const Range& rg) { cf.ranges.push_back(rg); });
            CondRule ru; ru.type = CondRule::ColorScale;
            ru.cfvo = {CfValueObject{CfValueObject::Min, "", 0xFF63BE7B}, CfValueObject{CfValueObject::Percentile, "50", 0xFFFFEB84}, CfValueObject{CfValueObject::Max, "", 0xFFF8696B}};
            cf.rules.push_back(ru); b.add_cond_format(cur, cf);
        }
        ImGui::Separator();
        if (menu_item_icon("Clear rules from selected cells", ui::Icon::Eraser, nullptr, false, !ws().cond.empty())) {
            auto list = ws().cond;
            Range g = prim();
            for (auto& cf : list) cf.ranges.erase(std::remove_if(cf.ranges.begin(), cf.ranges.end(), [&](const Range& r2) { return g.contains(r2.r0, r2.c0) && g.contains(r2.r1, r2.c1); }), cf.ranges.end());
            list.erase(std::remove_if(list.begin(), list.end(), [](const CondFormat& c) { return c.ranges.empty(); }), list.end());
            b.set_cond_formats(cur, list);
        }
        if (menu_item_icon("Clear rules from entire sheet", ui::Icon::Delete, nullptr, false, !ws().cond.empty())) b.set_cond_formats(cur, {});
        if (menu_item_icon("Manage Rules\xE2\x80\xA6", ui::Icon::Settings)) open_dialog(Dlg::CondFmt);
        ImGui::EndPopup();
    }
    ImGui::SameLine(0, S(2));
    if (sg::big_btn("cs", ui::Icon::None, G::CellStyles, "Cell Styles", "Cell Styles\nHeader, Good, Bad, Neutral, ...", false, true, true)) ImGui::OpenPopup("##cstyles");
    if (ImGui::BeginPopup("##cstyles")) {
        struct P { const char* n; uint32_t fill, text; bool bold; int id; };
        static const P ps[] = {{"Normal", 0, 0, false, 0}, {"Header", 0xFF305496, 0xFFFFFFFF, true, 1}, {"Good", 0xFFC6EFCE, 0xFF006100, false, 2}, {"Bad", 0xFFFFC7CE, 0xFF9C0006, false, 3},
                               {"Neutral", 0xFFFFEB9C, 0xFF9C5700, false, 4}, {"Input", 0xFFFFCC99, 0xFF3F3F76, false, 7}, {"Note", 0xFFFFFFCC, 0xFF000000, false, 6}, {"Check Cell", 0xFFA5A5A5, 0xFFFFFFFF, true, 11},
                               {"Title", 0, 0xFF44546A, true, 5}, {"Heading 1", 0, 0xFF44546A, true, 10}, {"Total", 0, 0, true, 8}, {"Warning Text", 0, 0xFFFF0000, false, 9}};
        section_header("Good, Bad and Neutral / Data and Model / Titles");
        for (int i = 0; i < 12; ++i) {
            if (i % 4) ImGui::SameLine(0, S(4));
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImVec2 sz(S(96), S(30));
            ImGui::PushID(i);
            bool clk = ImGui::InvisibleButton("##ps", sz);
            bool hv = ImGui::IsItemHovered();
            ImGui::PopID();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImU32 fill = ps[i].fill ? doc_col(ps[i].fill) : th().field;
            dl->AddRectFilled(p, p + sz, fill, S(3));
            dl->AddRect(p, p + sz, hv ? th().accent : th().border, S(3), 0, hv ? S(2) : 1.f);
            ImU32 tc = ps[i].text ? doc_col(ps[i].text) : th().text;
            if (!ps[i].fill && th().dark && ps[i].text && ps[i].text != 0xFFFF0000) tc = th().text;
            ImFont* f = ps[i].bold && g_font_bold ? g_font_bold : ImGui::GetFont();
            ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, ps[i].n);
            dl->AddText(f, f->FontSize, ImVec2(p.x + S(6), p.y + (sz.y - ts.y) * 0.5f), tc, ps[i].n);
            if (ps[i].id == 8) { dl->AddLine(ImVec2(p.x + 3, p.y + 3), ImVec2(p.x + sz.x - 3, p.y + 3), th().accent); dl->AddLine(ImVec2(p.x + 3, p.y + sz.y - 4), ImVec2(p.x + sz.x - 3, p.y + sz.y - 4), th().accent); }
            if (clk) { apply_cell_style(ps[i].id); ImGui::CloseCurrentPopup(); }
        }
        ImGui::EndPopup();
    }
    sg::end_group();
    ImGui::SameLine(0, S(14));
    // Cells
    sg::begin_group("Cells");
    if (sg::big_btn("ins", ui::Icon::None, G::InsRow, "Insert", "Insert cells, rows or columns", false, true, true)) ImGui::OpenPopup("##insm");
    if (ImGui::BeginPopup("##insm")) {
        if (sg::gitem("Insert Cells (shift down)", G::InsRow)) insert_cells_shift(true);
        if (sg::gitem("Insert Cells (shift right)", G::InsCol)) insert_cells_shift(false);
        if (sg::gitem("Insert Sheet Rows", G::InsRow)) insert_rows(false);
        if (sg::gitem("Insert Sheet Columns", G::InsCol)) insert_cols(false);
        if (menu_item_icon("Insert Sheet", ui::Icon::New)) { int si = b.add_sheet("Sheet" + std::to_string(b.sheets.size() + 1), cur + 1); sync_ui(); cur = si; }
        ImGui::EndPopup();
    }
    ImGui::SameLine(0, S(2));
    if (sg::big_btn("del", ui::Icon::None, G::DelRow, "Delete", "Delete cells, rows or columns", false, true, true)) ImGui::OpenPopup("##delm");
    if (ImGui::BeginPopup("##delm")) {
        if (sg::gitem("Delete Cells (shift up)", G::DelRow)) delete_cells_shift(true);
        if (sg::gitem("Delete Cells (shift left)", G::DelCol)) delete_cells_shift(false);
        if (sg::gitem("Delete Sheet Rows", G::DelRow)) delete_rows();
        if (sg::gitem("Delete Sheet Columns", G::DelCol)) delete_cols();
        if (menu_item_icon("Delete Sheet", ui::Icon::Delete, nullptr, false, b.sheets.size() > 1)) { b.remove_sheet(cur); sync_ui(); }
        ImGui::EndPopup();
    }
    ImGui::SameLine(0, S(2));
    if (sg::big_btn("fmt", ui::Icon::None, G::RowHeight, "Format", "Row height, column width, hide & unhide, sheet", false, true, true)) ImGui::OpenPopup("##fmtm");
    if (ImGui::BeginPopup("##fmtm")) {
        Range g = prim();
        section_header("Cell size");
        if (sg::gitem("Row Height\xE2\x80\xA6", G::RowHeight)) open_dialog(Dlg::RowHeight);
        if (sg::gitem("AutoFit Row Height", G::RowHeight)) autofit_rows(g.r0, g.r1);
        if (sg::gitem("Column Width\xE2\x80\xA6", G::ColWidth)) open_dialog(Dlg::ColWidth);
        if (sg::gitem("AutoFit Column Width", G::ColWidth)) autofit_cols(g.c0, g.c1);
        section_header("Visibility");
        if (sg::gitem("Hide Rows", G::Hide)) b.hide_rows(cur, g.r0, std::min(g.r1, ws().nrows() + 1000), true);
        if (sg::gitem("Hide Columns", G::Hide)) b.hide_cols(cur, g.c0, std::min(g.c1, ws().ncols() + 100), true);
        if (menu_item_icon("Unhide Rows", ui::Icon::Eye)) b.hide_rows(cur, g.r0, std::min(g.r1, ws().nrows() + 1000), false);
        if (menu_item_icon("Unhide Columns", ui::Icon::Eye)) b.hide_cols(cur, g.c0, std::min(g.c1, ws().ncols() + 100), false);
        section_header("Organize sheets");
        if (menu_item_icon("Rename Sheet", ui::Icon::Text)) { rename_sheet = cur; std::snprintf(rename_buf, sizeof rename_buf, "%s", ws().name.c_str()); }
        if (begin_menu_icon("Tab Color", ui::Icon::Palette)) {
            uint32_t c = ws().tab_color;
            if (sg::color_palette(c, "No Color")) { b.set_tab_color(cur, c); ImGui::CloseCurrentPopup(); }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (sg::gitem("Format Cells\xE2\x80\xA6", G::CellStyles, "Ctrl+1")) { fmt_tab = 0; open_dialog(Dlg::FormatCells); }
        ImGui::EndPopup();
    }
    sg::end_group();
    ImGui::SameLine(0, S(14));
    // Editing
    sg::begin_group("Editing");
    ImGui::BeginGroup();
    int ar_ = sg::gsplit("sum", G::AutoSum, "AutoSum  (Alt+=)");
    if (ar_ == 1) autosum();
    if (ar_ == 2) ImGui::OpenPopup("##summ");
    if (ImGui::BeginPopup("##summ")) {
        static const char* fns[] = {"SUM", "AVERAGE", "COUNT", "MAX", "MIN"};
        for (const char* f : fns) if (ImGui::MenuItem(f)) {
            autosum();
            if (editing) { std::string tx = ebuf.data(); size_t k = tx.find("SUM("); if (k != std::string::npos) { tx.replace(k, 3, f); std::snprintf(ebuf.data(), ebuf.size(), "%s", tx.c_str()); want_caret = (int)tx.size(); } }
        }
        ImGui::EndPopup();
    }
    row_gap();
    if (sg::gbtn("filld", G::FillDown, "Fill  (Ctrl+D down, Ctrl+R right)")) ImGui::OpenPopup("##fillm");
    if (ImGui::BeginPopup("##fillm")) {
        if (menu_item_icon("Down", ui::Icon::ArrowDown, "Ctrl+D")) fill_dir(0);
        if (menu_item_icon("Right", ui::Icon::Redo, "Ctrl+R")) fill_dir(1);
        if (menu_item_icon("Up", ui::Icon::ArrowUp)) fill_dir(2);
        if (menu_item_icon("Left", ui::Icon::Undo)) fill_dir(3);
        ImGui::Separator();
        if (menu_item_icon("Series (extend numbers / R1, R2 \xE2\x80\xA6 down the selection)", ui::Icon::Wand)) {
            Range g = prim();
            if (g.rows() > 1) b.fill(cur, Range(g.r0, g.c0, g.r0, g.c1), g);
            else if (g.cols() > 1) b.fill(cur, Range(g.r0, g.c0, g.r1, g.c0), g);
        }
        ImGui::EndPopup();
    }
    row_gap();
    if (sg::gbtn("clr", G::Clear, "Clear")) ImGui::OpenPopup("##clrm");
    if (ImGui::BeginPopup("##clrm")) {
        if (menu_item_icon("Clear All", ui::Icon::Delete)) clear_sel(Book::ClearAll, "Clear all");
        if (sg::gitem("Clear Formats", G::Clear)) clear_sel(Book::ClearFormats, "Clear formats");
        if (menu_item_icon("Clear Contents", ui::Icon::Eraser, "Del")) clear_sel(Book::ClearContents, "Clear contents");
        ImGui::EndPopup();
    }
    if (sg::gbtn("srt", G::SortFilter, "Sort & Filter")) ImGui::OpenPopup("##sfm");
    if (ImGui::BeginPopup("##sfm")) {
        if (menu_item_icon("Sort A \xE2\x86\x92 Z", ui::Icon::ArrowUp)) sort_quick(false);
        if (menu_item_icon("Sort Z \xE2\x86\x92 A", ui::Icon::ArrowDown)) sort_quick(true);
        if (menu_item_icon("Custom Sort\xE2\x80\xA6", ui::Icon::Layers)) open_dialog(Dlg::Sort);
        ImGui::Separator();
        if (sg::gitem("Filter", G::Funnel, "Ctrl+Shift+L", true, ws().autofilter.active())) toggle_filter();
        if (menu_item_icon("Clear", ui::Icon::EyeOff, nullptr, false, ws().autofilter.active())) b.clear_filters(cur);
        if (menu_item_icon("Reapply", ui::Icon::Redo, nullptr, false, ws().autofilter.active())) b.reapply_filter(cur);
        ImGui::EndPopup();
    }
    row_gap();
    if (sg::gbtn("fnd", G::FindSel, "Find & Select")) ImGui::OpenPopup("##fsm");
    if (ImGui::BeginPopup("##fsm")) {
        if (menu_item_icon("Find\xE2\x80\xA6", ui::Icon::FindRef, "Ctrl+F")) { show_replace = false; open_dialog(Dlg::Find); }
        if (menu_item_icon("Replace\xE2\x80\xA6", ui::Icon::Wand, "Ctrl+H")) { show_replace = true; open_dialog(Dlg::Find); }
        if (menu_item_icon("Go To\xE2\x80\xA6", ui::Icon::ArrowDown, "Ctrl+G")) open_dialog(Dlg::GoTo);
        ImGui::Separator();
        auto select_where = [&](auto pred, const char* what) {
            std::vector<SelRng> out;
            const Worksheet& w = ws();
            for (int r = 0; r < (int)w.rows.size() && out.size() < 5000; ++r) {
                if (w.row_hidden(r)) continue;
                for (int c = 0; c < (int)w.rows[r].cells.size(); ++c) if (pred(w.rows[r].cells[c], r, c)) out.push_back({r, c, r, c});
            }
            if (out.empty()) { toast(std::string("No ") + what + " found", ToastKind::Info, 2.5f); return; }
            su().sel = out;
            msg = std::to_string(out.size()) + " " + what;
        };
        if (menu_item_icon("Formulas", ui::Icon::Text)) select_where([](const Cell& c, int, int) { return c.has_formula(); }, "formulas");
        if (menu_item_icon("Constants", ui::Icon::Text)) select_where([](const Cell& c, int, int) { return !c.has_formula() && !c.v.is_empty(); }, "constants");
        if (menu_item_icon("Errors", ui::Icon::Warning)) select_where([](const Cell& c, int, int) { return c.v.is_err(); }, "errors");
        if (sg::gitem("Data Validation cells", G::Validation)) select_where([this](const Cell&, int r, int c) { for (auto& dv : ws().validations) for (auto& rg : dv.ranges) if (rg.contains(r, c)) return true; return false; }, "validated cells");
        ImGui::EndPopup();
    }
    ImGui::EndGroup();
    sg::end_group();
}

// ---------------------------------------------------------------- Data
void SheetDoc::ribbon_data() {
    sg::begin_group("Sort & Filter");
    if (sg::big_btn("az", ui::Icon::ArrowUp, G::None, "A \xE2\x86\x92 Z", "Sort ascending by the active column (natural: R2 < R10)")) sort_quick(false);
    ImGui::SameLine(0, S(2));
    if (sg::big_btn("za", ui::Icon::ArrowDown, G::None, "Z \xE2\x86\x92 A", "Sort descending by the active column")) sort_quick(true);
    ImGui::SameLine(0, S(2));
    if (sg::big_btn("sortd", ui::Icon::Layers, G::None, "Sort\xE2\x80\xA6", "Custom sort: several keys, header row")) open_dialog(Dlg::Sort);
    ImGui::SameLine(0, S(2));
    if (sg::big_btn("flt", ui::Icon::None, G::Funnel, ws().autofilter.active() ? "Filter (on)" : "Filter", "AutoFilter dropdowns on the header row  (Ctrl+Shift+L)")) toggle_filter();
    ImGui::SameLine(0, S(4));
    ImGui::BeginGroup();
    if (button("Clear", ui::Icon::EyeOff, false, ImVec2(S(84), S(26)), ws().autofilter.active())) b.clear_filters(cur);
    if (button("Reapply", ui::Icon::Redo, false, ImVec2(S(84), S(26)), ws().autofilter.active())) b.reapply_filter(cur);
    ImGui::EndGroup();
    sg::end_group();
    ImGui::SameLine(0, S(14));
    sg::begin_group("Data Tools");
    if (sg::big_btn("t2c", ui::Icon::None, G::TextToCols, "Text to Columns", "Split the selected column by a delimiter or fixed widths")) open_dialog(Dlg::TextToCols);
    ImGui::SameLine(0, S(2));
    if (sg::big_btn("rdup", ui::Icon::None, G::Dedupe, "Remove Duplicates", "Delete rows whose chosen columns repeat an earlier row")) open_dialog(Dlg::RemoveDup);
    ImGui::SameLine(0, S(2));
    if (sg::big_btn("dv", ui::Icon::None, G::Validation, "Data Validation", "Restrict what can be entered (lists with dropdowns, numbers, lengths)")) open_dialog(Dlg::Validation);
    sg::end_group();
    ImGui::SameLine(0, S(14));
    sg::begin_group("Calculation");
    if (sg::big_btn("calc", ui::Icon::Generate, G::None, "Calculate Now", "Recalculate every formula in the workbook")) {
        auto st = b.recalc_all();
        char m[96]; std::snprintf(m, sizeof m, "Recalculated %d cells in %.1f ms", st.evaluated, st.ms);
        toast(m, ToastKind::Success, 2.5f);
    }
    ImGui::SameLine(0, S(2));
    if (sg::big_btn("autoc", ui::Icon::Check, G::None, b.auto_calc ? "Automatic" : "Manual", "Toggle automatic calculation")) b.auto_calc = !b.auto_calc;
    sg::end_group();
}

// ---------------------------------------------------------------- BOM
void SheetDoc::ribbon_bom() {
    sg::begin_group("References");
    if (sg::big_btn("rexp", ui::Icon::None, G::Refdes, "Expand", "Expand reference ranges: R1-R4 \xE2\x86\x92 R1, R2, R3, R4\n(selection, or the detected reference column)")) bom_expand_refs(true);
    ImGui::SameLine(0, S(2));
    if (sg::big_btn("rcmp", ui::Icon::Compact, G::None, "Compress", "Compress reference lists: R1, R2, R3 \xE2\x86\x92 R1-R3")) bom_expand_refs(false);
    ImGui::SameLine(0, S(2));
    if (sg::big_btn("rrow", ui::Icon::Locations, G::None, "To Rows", "One reference per row (duplicates the other columns)")) bom_expand_to_rows();
    sg::end_group();
    ImGui::SameLine(0, S(14));
    sg::begin_group("Checks");
    if (sg::big_btn("rcnt", ui::Icon::None, G::RefCount, "Count Refs", "Add a REFCOUNT column (formula) next to the references")) bom_count_refs();
    ImGui::SameLine(0, S(2));
    if (sg::big_btn("qchk", ui::Icon::None, G::QtyCheck, "Check Qty", "Highlight rows whose Qty differs from the reference count\n(conditional formatting rule)")) bom_check_qty();
    sg::end_group();
    ImGui::SameLine(0, S(14));
    sg::begin_group("Clean up");
    if (sg::big_btn("sdnp", ui::Icon::None, G::SplitDnp, "Split DNP", "Move DNP / not-fitted rows to a new \"DNP\" sheet")) bom_split_dnp();
    ImGui::SameLine(0, S(2));
    if (sg::big_btn("ddpn", ui::Icon::None, G::Dedupe, "Merge Part #", "Combine rows with the same part number: references joined, Qty summed")) bom_dedupe_pn();
    ImGui::SameLine(0, S(2));
    if (sg::big_btn("trim", ui::Icon::Eraser, G::None, "Trim", "Trim spaces in the selection (or the whole sheet)")) {
        Book::Batch bt(b, "Trim spaces");
        Range g = prim();
        Range rg = (g.rows() == 1 && g.cols() == 1) ? ws().used_range() : g;
        if (rg.valid()) b.map_cells(cur, rg, [](const std::string& v) {
            size_t a = v.find_first_not_of(" \t\r\n"), e = v.find_last_not_of(" \t\r\n");
            if (a == std::string::npos) return std::string();
            std::string o; bool sp = false;
            for (size_t i = a; i <= e; ++i) { char ch = v[i]; if (ch == ' ' || ch == '\t') { if (!sp) o += ' '; sp = true; } else { o += ch; sp = false; } }
            return o;
        }, "Trim spaces");
    }
    sg::end_group();
    ImGui::SameLine(0, S(14));
    sg::begin_group("Project");
    if (sg::big_btn("bomimp", ui::Icon::ImportBom, G::None, "Import BOM", "Send the visible rows to the BOM import wizard")) send_to_project(0);
    ImGui::SameLine(0, S(2));
    if (sg::big_btn("bomupd", ui::Icon::Fusion, G::None, "Update BOM", "Merge the visible rows into the current project's BOM", false, app::active_doc() != nullptr)) send_to_project(2);
    ImGui::SameLine(0, S(2));
    if (sg::big_btn("cenimp", ui::Icon::ImportCentroid, G::None, "Centroid", "Send the visible rows to the pick & place import wizard")) send_to_project(1);
    if (!link_doc.expired()) {
        ImGui::SameLine(0, S(2));
        if (sg::big_btn("apply", ui::Icon::Check, G::None, "Apply to Project", "Write part numbers, descriptions, packages and DNP back to the linked project (diff preview first)", true)) bom_apply_preview();
    }
    sg::end_group();
}

// ---------------------------------------------------------------- View
void SheetDoc::ribbon_view() {
    Worksheet& W = ws();
    sg::begin_group("Window");
    bool frozen = W.freeze_rows || W.freeze_cols;
    if (sg::big_btn("frz", ui::Icon::None, G::Freeze, frozen ? "Unfreeze" : "Freeze Panes", "Freeze rows above and columns left of the active cell", false, true, true)) ImGui::OpenPopup("##frzm");
    if (ImGui::BeginPopup("##frzm")) {
        if (sg::gitem(frozen ? "Unfreeze Panes" : "Freeze Panes", G::Freeze)) freeze_at_cursor();
        if (menu_item_icon("Freeze Top Row", ui::Icon::ArrowUp)) b.set_freeze(cur, 1, 0);
        if (menu_item_icon("Freeze First Column", ui::Icon::Undo)) b.set_freeze(cur, 0, 1);
        ImGui::EndPopup();
    }
    sg::end_group();
    ImGui::SameLine(0, S(14));
    sg::begin_group("Show");
    ImGui::BeginGroup();
    bool g = W.show_grid;
    if (ImGui::Checkbox("Gridlines", &g)) W.show_grid = g;
    ImGui::Checkbox("Headings", &show_headers);
    ImGui::EndGroup();
    ImGui::SameLine(0, S(10));
    ImGui::BeginGroup();
    ImGui::Checkbox("Formula Bar", &show_formula_bar);
    bool m = ribbon_min;
    if (ImGui::Checkbox("Compact ribbon", &m)) ribbon_min = m;
    ImGui::EndGroup();
    sg::end_group();
    ImGui::SameLine(0, S(14));
    sg::begin_group("Zoom");
    if (sg::big_btn("z100", ui::Icon::Zoom100, G::None, "100%", "Zoom to 100%")) W.zoom = 100;
    ImGui::SameLine(0, S(2));
    if (sg::big_btn("zsel", ui::Icon::ZoomRect, G::None, "Selection", "Zoom so the selection fills the window")) {
        Range s = prim();
        if (s.r1 < MAX_ROWS - 1 && s.c1 < MAX_COLS - 1) {
            float wpx = 0, hpx = 0;
            for (int c = s.c0; c <= s.c1; ++c) wpx += col_px(c) / zoomf();
            for (int r = s.r0; r <= s.r1 && r - s.r0 < 500; ++r) hpx += row_px(r) / zoomf();
            float avw = ImGui::GetContentRegionAvail().x + S(600), avh = S(600);
            int zz = (int)(std::min(avw / std::max(wpx, 1.f), avh / std::max(hpx, 1.f)) * 100);
            W.zoom = std::clamp(zz / 10 * 10, 25, 400);
            want_scroll = true;
        }
    }
    ImGui::SameLine(0, S(6));
    ImGui::BeginGroup();
    ImGui::SetNextItemWidth(S(140));
    int zz = W.zoom;
    if (ImGui::SliderInt("##zoomsl", &zz, 25, 400, "%d%%")) W.zoom = zz;
    static const int zs[] = {25, 50, 75, 100, 125, 150, 200, 300, 400};
    for (int i = 0; i < (int)(sizeof zs / sizeof zs[0]); ++i) {
        char l[8]; std::snprintf(l, sizeof l, "%d", zs[i]);
        if (i) ImGui::SameLine(0, S(2));
        ImGui::PushID(i);
        if (ImGui::SmallButton(l)) W.zoom = zs[i];
        ImGui::PopID();
    }
    ImGui::EndGroup();
    sg::end_group();
}

// ===========================================================================
// formula bar + shared formula editor
// ===========================================================================
static int fi_cb(ImGuiInputTextCallbackData* d) {
    SheetDoc* s = (SheetDoc*)d->UserData;
    switch (d->EventFlag) {
    case ImGuiInputTextFlags_CallbackAlways:
        if (s->want_caret >= 0) { d->CursorPos = d->SelectionStart = d->SelectionEnd = std::min(s->want_caret, d->BufTextLen); s->want_caret = -1; }
        s->caret = d->CursorPos;
        break;
    case ImGuiInputTextFlags_CallbackHistory:
        if (s->ac_visible) s->ac_move += d->EventKey == ImGuiKey_UpArrow ? -1 : 1;
        else if (!s->edit_in_bar) s->ac_move += d->EventKey == ImGuiKey_UpArrow ? -100 : 100;   // commit + move
        break;
    case ImGuiInputTextFlags_CallbackCompletion:
        if (s->ac_visible && s->ac_start >= 0 && s->ac_sel < (int)s->ac_items.size()) {
            std::string name = s->ac_items[s->ac_sel]->name;
            int end = d->CursorPos;
            while (end < d->BufTextLen && (isalnum((unsigned char)d->Buf[end]) || d->Buf[end] == '.' || d->Buf[end] == '_')) ++end;
            d->DeleteChars(s->ac_start, end - s->ac_start);
            std::string ins = name + "(";
            if (end < d->BufTextLen + (end - s->ac_start) && d->Buf[s->ac_start] == '(') ins = name;
            d->InsertChars(s->ac_start, ins.c_str());
            s->ac_visible = false; s->ac_start = -1;
            s->edit_dirty = true;
        } else s->tab_commit = true;
        break;
    }
    return 0;
}

bool SheetDoc::formula_input(const char* id, float w, float h, bool in_bar) {
    const GTheme& t = th();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool formula = ebuf[0] == '=';
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float fs = ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(4), std::max(0.f, (h - fs) * 0.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
    if (formula) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
    ImGuiID iid = ImGui::GetID(id);
    bool want_focus = edit_focus && (in_bar == edit_in_bar);
    if (want_focus) { ImGui::SetKeyboardFocusHere(); edit_focus = false; if (want_caret < 0) want_caret = caret; }
    // keep Escape for closing the autocomplete list
    if (ac_visible && ImGui::GetActiveID() == iid && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::SetKeyOwner(ImGuiKey_Escape, iid, ImGuiInputFlags_LockThisFrame);
        ac_visible = false; ac_start = -2;
    }
    bool esc = ImGui::GetActiveID() == iid && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && ac_start != -2;
    bool shift = ImGui::GetIO().KeyShift;
    ImGui::SetNextItemWidth(w);
    tab_commit = false; ac_move = 0;
    bool enter = ImGui::InputText(id, ebuf.data(), ebuf.size(),
                                  ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackCompletion, fi_cb, this);
    if (ac_start == -2) ac_start = -1;
    if (formula) ImGui::PopStyleColor();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
    if (in_bar) bar_id = iid; else cell_id = iid;
    bool active = ImGui::IsItemActive();
    if (ImGui::IsItemActivated() && !editing) { begin_edit(true, nullptr, in_bar); want_caret = -1; edit_focus = false; }
    if (ImGui::IsItemActivated() && editing) edit_in_bar = in_bar;
    if (ImGui::IsItemEdited()) { edit_dirty = true; point_mode = false; }
    ImRect fr(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    // ---- coloured overlay (formulas)
    if (formula) {
        ImGuiInputTextState* st = active ? ImGui::GetInputTextState(iid) : nullptr;
        float scroll = st ? st->Scroll.x : 0.f;
        ImVec2 tp(fr.Min.x + S(4) - scroll, fr.Min.y + std::max(0.f, (h - fs) * 0.5f));
        const char* txt = ebuf.data();
        auto toks = tokenize_formula(std::string_view(txt));
        std::vector<UsedRef> refs = (editing && esi < (int)b.sheets.size()) ? formula_refs(std::string_view(txt + 1), b, esi, er, ec) : std::vector<UsedRef>{};
        dl->PushClipRect(fr.Min, fr.Max, true);
        ImFont* f = ImGui::GetFont();
        float x = tp.x;
        int pos = 0;
        auto draw_span = [&](int a, int e2, ImU32 c) {
            if (e2 <= a) return;
            dl->AddText(f, fs, ImVec2(x, tp.y), c, txt + a, txt + e2);
            x += f->CalcTextSizeA(fs, FLT_MAX, 0, txt + a, txt + e2).x;
        };
        static const ImU32 paren_cols[4] = {IM_COL32(120, 120, 120, 255), IM_COL32(200, 120, 40, 255), IM_COL32(60, 150, 90, 255), IM_COL32(150, 80, 200, 255)};
        for (auto& tk : toks) {
            int a = tk.pos, e2 = tk.pos + tk.len;
            if (a > pos) draw_span(pos, a, t.text);
            ImU32 c = t.text;
            switch (tk.kind) {
            case LexToken::Ref: {
                c = t.accent;
                for (size_t k = 0; k < refs.size(); ++k) if (refs[k].text_pos + 1 <= a && a < refs[k].text_pos + 1 + std::max(1, refs[k].text_len)) c = sg::ref_color((int)k);
                break;
            }
            case LexToken::String: c = t.dark ? IM_COL32(150, 200, 120, 255) : IM_COL32(40, 120, 40, 255); break;
            case LexToken::Number: c = t.dark ? IM_COL32(180, 170, 255, 255) : IM_COL32(60, 60, 200, 255); break;
            case LexToken::Function: c = t.text; break;
            case LexToken::Error: c = t.error; break;
            case LexToken::OpenParen: case LexToken::CloseParen: c = paren_cols[tk.depth & 3]; break;
            default: c = t.text;
            }
            draw_span(a, e2, c);
            pos = e2;
        }
        int n = (int)strlen(txt);
        if (pos < n) draw_span(pos, n, t.text);
        // caret
        if (active) {
            int cpos = std::clamp(caret, 0, n);
            float cx = tp.x + f->CalcTextSizeA(fs, FLT_MAX, 0, txt, txt + cpos).x;
            if (std::fmod(ImGui::GetTime(), 1.2) < 0.8) dl->AddLine(ImVec2(cx, tp.y - 1), ImVec2(cx, tp.y + fs + 1), t.text, S(1.2f));
            request_redraw(2);
        }
        dl->PopClipRect();
    }
    (void)p;
    // ---- keys
    if (editing && (in_bar == edit_in_bar)) {
        if (esc) { cancel_edit(); return false; }
        if (enter) { commit_edit(shift ? -1 : 1, 0); return true; }
        if (tab_commit) { commit_edit(0, shift ? -1 : 1); return true; }
        if (ac_move <= -100 || ac_move >= 100) { commit_edit(ac_move > 0 ? 1 : -1, 0); return true; }
        if (ac_move) ac_sel += ac_move;
        if (ImGui::IsItemDeactivated()) deact_pending = true;
    }
    // ---- autocomplete state
    if (editing && active && formula) {
        std::string pre;
        int st0 = sg::ident_at(ebuf.data(), caret, pre);
        if (st0 >= 0 && !pre.empty() && ac_start != -2) {
            if (st0 != ac_start || pre != ac_prefix) { ac_sel = 0; ac_prefix = pre; }
            ac_start = st0;
            ac_items.clear();
            for (auto& fi : function_catalog()) {
                if (strncmp(fi.name, pre.c_str(), pre.size()) == 0) { ac_items.push_back(&fi); if (ac_items.size() >= 12) break; }
            }
            ac_visible = !ac_items.empty() && !(ac_items.size() == 1 && pre == ac_items[0]->name && ebuf[caret] == '(');
            if (ac_visible) ac_sel = (ac_sel % (int)ac_items.size() + (int)ac_items.size()) % (int)ac_items.size();
        } else { ac_visible = false; ac_items.clear(); if (ac_start != -2) ac_start = -1; }
    } else if (!editing) { ac_visible = false; ac_items.clear(); }
    return false;
}

void SheetDoc::draw_autocomplete(ImVec2 at) {
    if (!editing || ebuf[0] != '=') return;
    const GTheme& t = th();
    ImGuiWindowFlags fl = ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ac_visible && !ac_items.empty()) {
        ImGui::SetNextWindowPos(at);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(4), S(4)));
        ImGui::Begin("##sheet_ac", nullptr, fl);
        for (int i = 0; i < (int)ac_items.size(); ++i) {
            const FunctionInfo* fi = ac_items[i];
            ImVec2 p = ImGui::GetCursorScreenPos();
            float w = S(320), hh = ImGui::GetTextLineHeightWithSpacing();
            if (i == ac_sel) ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + w, p.y + hh), col_alpha(t.accent, 0.25f), S(3));
            sg::glyph(ImGui::GetWindowDrawList(), G::Fx, ImVec2(p.x + S(2), p.y + S(1)), hh - S(2), t.text_dim);
            ImGui::SetCursorScreenPos(ImVec2(p.x + hh + S(4), p.y));
            ImGui::TextUnformatted(fi->name);
            ImGui::SameLine(S(120));
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_faint), "%s", fi->category);
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + hh));
            ImGui::Dummy(ImVec2(w, 0));
        }
        if (ac_sel < (int)ac_items.size()) {
            ImGui::Separator();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + S(320));
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_dim), "%s", ac_items[ac_sel]->description);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_faint), "Tab inserts \xC2\xB7 \xE2\x86\x91\xE2\x86\x93 choose \xC2\xB7 Esc closes");
            ImGui::PopTextWrapPos();
        }
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }
    // argument hint
    std::string fn; int arg = 0;
    if (!sg::call_at(ebuf.data(), caret, fn, arg)) return;
    const FunctionInfo* fi = find_function(fn);
    if (!fi) return;
    ImGui::SetNextWindowPos(at);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(8), S(5)));
    ImGui::Begin("##sheet_arg", nullptr, fl);
    // syntax "NAME(a, b, [c])": bold the current argument
    std::string syn = fi->syntax;
    size_t lp = syn.find('(');
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float x = p.x;
    ImFont* f = ImGui::GetFont();
    ImFont* fb = g_font_bold ? g_font_bold : f;
    float fs = ImGui::GetFontSize();
    auto put = [&](const std::string& s, bool bold, ImU32 c) { ImFont* ff = bold ? fb : f; dl->AddText(ff, fs, ImVec2(x, p.y), c, s.c_str()); x += ff->CalcTextSizeA(fs, FLT_MAX, 0, s.c_str()).x; };
    if (lp == std::string::npos) put(syn, false, t.text);
    else {
        put(syn.substr(0, lp + 1), false, t.text);
        std::string args = syn.substr(lp + 1);
        if (!args.empty() && args.back() == ')') args.pop_back();
        int k = 0;
        size_t s0 = 0;
        int nargs = 1;
        for (char ch : args) nargs += ch == ',';
        bool variadic = args.find("...") != std::string::npos;
        int hl = (variadic && arg >= nargs - 1) ? nargs - 2 : arg;
        while (true) {
            size_t c = args.find(',', s0);
            std::string a = args.substr(s0, c == std::string::npos ? std::string::npos : c - s0);
            put(a, k == hl, k == hl ? t.accent : t.text_dim);
            if (c == std::string::npos) break;
            put(",", false, t.text_dim);
            s0 = c + 1; ++k;
        }
        put(")", false, t.text);
    }
    ImGui::Dummy(ImVec2(x - p.x, fs));
    ImGui::End();
    ImGui::PopStyleVar();
}

void SheetDoc::draw_formula_bar() {
    const GTheme& t = th();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float W = ImGui::GetContentRegionAvail().x, H = S(30);
    dl->AddRectFilled(p, ImVec2(p.x + W, p.y + H), t.panel);
    dl->AddLine(ImVec2(p.x, p.y + H - 0.5f), ImVec2(p.x + W, p.y + H - 0.5f), t.border);
    // ---- name box
    ImGui::SetCursorScreenPos(ImVec2(p.x + S(6), p.y + S(3)));
    ImGuiID nid = ImGui::GetID("##namebox");
    if (ImGui::GetActiveID() != nid) {
        Range g = prim();
        std::string nm;
        if (drag == GDrag::Cells && (g.rows() > 1 || g.cols() > 1)) nm = std::to_string(g.rows()) + "R x " + std::to_string(g.cols()) + "C";
        else nm = sg::a1(ar(), ac());
        // a defined name that matches the selection exactly
        for (auto& dn : b.names) {
            Range rg; std::string f = dn.formula; size_t k = f.rfind('!');
            std::string sh = k == std::string::npos ? "" : f.substr(0, k);
            if (!sh.empty() && sh.front() == '\'') sh = sh.substr(1, sh.size() - 2);
            if ((sh.empty() || b.find_sheet(sh) == cur) && parse_range_name(k == std::string::npos ? f : f.substr(k + 1), rg) && rg.norm() == g) nm = dn.name;
        }
        std::snprintf(namebox, sizeof namebox, "%s", nm.c_str());
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(6), S(4)));
    ImGui::SetNextItemWidth(S(120));
    if (ImGui::InputText("##namebox", namebox, sizeof namebox, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
        std::string nb = namebox;
        if (!goto_ref(nb)) {
            // a new name for the selection (Excel: type a name in the Name Box)
            bool ident = !nb.empty() && (isalpha((unsigned char)nb[0]) || nb[0] == '_');
            for (char ch : nb) if (!(isalnum((unsigned char)ch) || ch == '_' || ch == '.')) ident = false;
            if (ident) {
                Range g = prim();
                b.define_name({nb, -1, quote_sheet_name(ws().name) + "!" + cell_name(g.r0, g.c0, true, true) + (g.rows() > 1 || g.cols() > 1 ? ":" + cell_name(g.r1, g.c1, true, true) : "")});
                toast("Defined name " + nb, ToastKind::Success, 2.5f);
            } else toast("\"" + nb + "\" is not a valid reference or name", ToastKind::Warning, 3.f);
        }
        focus_grid = true;
    }
    if (ImGui::IsItemHovered()) tooltip("Name Box: type a reference (B12, A1:D20, Sheet2!C3) or a name and press Enter.\nWith cells selected, a new name defines it.");
    ImGui::PopStyleVar();
    ImGui::SameLine(0, S(2));
    // names dropdown
    if (sg::gbtn("names", G::VBot, "Defined names", false, !b.names.empty(), 0, S(24))) ImGui::OpenPopup("##names");
    if (ImGui::BeginPopup("##names")) {
        for (auto& dn : b.names) if (!dn.hidden && ImGui::Selectable((dn.name + "   " + dn.formula).c_str())) goto_ref(dn.formula);
        ImGui::EndPopup();
    }
    ImGui::SameLine(0, S(8));
    // ---- cancel / enter / fx
    if (sg::ibtn("fb_x", ui::Icon::ErrorX, "Cancel  (Esc)", false, editing, S(24))) cancel_edit();
    ImGui::SameLine(0, S(1));
    if (sg::ibtn("fb_ok", ui::Icon::Check, "Enter", false, editing, S(24))) commit_edit();
    ImGui::SameLine(0, S(1));
    if (sg::gbtn("fb_fx", G::Fx, "Insert Function", false, true, 0, S(24))) ImGui::OpenPopup("##fxpick");
    if (ImGui::BeginPopup("##fxpick")) {
        static char q[64];
        static int cat = 0;
        static const char* cats[] = {"All", "Math", "Statistical", "Logical", "Text", "Lookup", "Date", "Info", "BOM"};
        search_box("##fxq", q, sizeof q, "Search functions", S(260));
        ImGui::SetNextItemWidth(S(260));
        ImGui::Combo("##fxcat", &cat, cats, IM_ARRAYSIZE(cats));
        ImGui::BeginChild("##fxlist", ImVec2(S(420), S(260)), ImGuiChildFlags_Borders);
        std::string qq = q;
        for (auto& c : qq) c = (char)toupper((unsigned char)c);
        for (auto& fi : function_catalog()) {
            if (cat && strcmp(fi.category, cats[cat]) != 0) continue;
            if (!qq.empty() && !strstr(fi.name, qq.c_str())) continue;
            if (ImGui::Selectable(fi.name)) {
                std::string ins = std::string(fi.name) + "(";
                if (!editing) begin_edit(false, ("=" + ins).c_str());
                else { std::string tx = ebuf.data(); int at = std::clamp(caret, 0, (int)tx.size()); tx.insert(at, ins); std::snprintf(ebuf.data(), ebuf.size(), "%s", tx.c_str()); want_caret = at + (int)ins.size(); edit_focus = true; }
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered()) tooltip((std::string(fi.syntax) + "\n" + fi.description).c_str());
            ImGui::SameLine(S(130));
            ImGui::TextDisabled("%s", fi.syntax);
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    ImGui::SameLine(0, S(6));
    // ---- formula input
    float fx0 = ImGui::GetCursorScreenPos().x;
    float fw = p.x + W - fx0 - S(8);
    dl->AddRectFilled(ImVec2(fx0, p.y + S(3)), ImVec2(fx0 + fw, p.y + H - S(4)), t.field, S(4));
    dl->AddRect(ImVec2(fx0, p.y + S(3)), ImVec2(fx0 + fw, p.y + H - S(4)), editing && edit_in_bar ? t.accent : col_alpha(t.border, 0.8f), S(4));
    ImGui::SetCursorScreenPos(ImVec2(fx0, p.y + S(3)));
    if (!editing) {
        // show the active cell's input text (read-only view; clicking starts editing)
        std::string v = b.input_text(cur, ar(), ac());
        size_t n = std::min(v.size(), ebuf.size() - 1);
        std::memcpy(ebuf.data(), v.data(), n); ebuf[n] = 0;
    }
    formula_input("##fbar", fw, H - S(7), true);
    if (editing && edit_in_bar) draw_autocomplete(ImVec2(fx0, p.y + H));
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + H));
    ImGui::Dummy(ImVec2(W, 0));
}

// ===========================================================================
// sheet tabs + status strip
// ===========================================================================
void SheetDoc::draw_sheet_tabs(float h) {
    const GTheme& t = th();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float W = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + W, p.y + h), t.panel2);
    dl->AddLine(p, ImVec2(p.x + W, p.y), t.border);
    ImGui::SetCursorScreenPos(ImVec2(p.x + S(6), p.y + S(2)));
    if (sg::ibtn("addsh", ui::Icon::Plus, "New sheet  (Shift+F11)", false, true, S(24))) { commit_edit(); int si = b.add_sheet("Sheet" + std::to_string(b.sheets.size() + 1), -1); sync_ui(); cur = si; }
    ImGui::SameLine(0, S(6));
    float status_w = S(420);
    float tabs_right = p.x + W - status_w;
    int move_from = -1, move_to = -1;
    for (int i = 0; i < (int)b.sheets.size(); ++i) {
        const Worksheet& w = b.sheets[i];
        if (w.hidden) continue;
        ImGui::PushID(i);
        bool sel = i == cur;
        if (rename_sheet == i) {
            ImGui::SetNextItemWidth(S(120));
            ImGui::SetKeyboardFocusHere();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(6), S(3)));
            if (ImGui::InputText("##rn", rename_buf, sizeof rename_buf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                if (rename_buf[0]) b.rename_sheet(i, rename_buf);
                rename_sheet = -1;
            }
            ImGui::PopStyleVar();
            if (ImGui::IsKeyPressed(ImGuiKey_Escape) || (ImGui::IsItemDeactivated() && rename_sheet == i)) rename_sheet = -1;
        } else {
            ImVec2 ts = ImGui::CalcTextSize(w.name.c_str());
            ImVec2 a = ImGui::GetCursorScreenPos();
            float tw = ts.x + S(26);
            if (a.x + tw > tabs_right) { ImGui::PopID(); break; }
            bool clk = ImGui::InvisibleButton("##tab", ImVec2(tw, h - S(4)));
            bool hv = ImGui::IsItemHovered();
            if (clk && !sel) { commit_edit(); if (!editing) cur = i; }
            if (hv && ImGui::IsMouseDoubleClicked(0)) { rename_sheet = i; std::snprintf(rename_buf, sizeof rename_buf, "%s", w.name.c_str()); }
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) tab_drag = i;
            if (tab_drag >= 0 && tab_drag != i && hv && ImGui::IsMouseReleased(0)) { move_from = tab_drag; move_to = i; }
            ImU32 bg = sel ? t.field : (hv ? t.raised : t.panel2);
            dl->AddRectFilled(a, ImVec2(a.x + tw, a.y + h - S(4)), bg, S(5), ImDrawFlags_RoundCornersBottom);
            if (sel) dl->AddRect(a, ImVec2(a.x + tw, a.y + h - S(4)), t.border, S(5), ImDrawFlags_RoundCornersBottom);
            if (w.tab_color) dl->AddRectFilled(ImVec2(a.x + S(4), a.y + h - S(8)), ImVec2(a.x + tw - S(4), a.y + h - S(5)), doc_col(w.tab_color), S(1));
            if (sel) dl->AddLine(ImVec2(a.x + S(1), a.y), ImVec2(a.x + tw - S(1), a.y), t.accent, S(2));
            ImFont* f = sel && g_font_bold ? g_font_bold : ImGui::GetFont();
            dl->AddText(f, ImGui::GetFontSize(), ImVec2(a.x + (tw - ts.x) * 0.5f, a.y + (h - S(4) - ts.y) * 0.5f), sel ? t.accent : t.text_dim, w.name.c_str());
            if (ImGui::BeginPopupContextItem("##sctx")) {
                if (menu_item_icon("Insert Sheet", ui::Icon::New)) { int si = b.add_sheet("Sheet" + std::to_string(b.sheets.size() + 1), i); sync_ui(); cur = si; }
                if (menu_item_icon("Rename", ui::Icon::Text)) { rename_sheet = i; std::snprintf(rename_buf, sizeof rename_buf, "%s", w.name.c_str()); }
                if (menu_item_icon("Duplicate", ui::Icon::Copy)) {
                    Book::Batch bt(b, "Duplicate sheet");
                    int si = b.add_sheet(w.name + " (2)", i + 1);
                    Range u = b.sheets[i].used_range();
                    if (u.valid()) { ClipBlock blk = b.copy(i, u); b.paste(si, u.r0, u.c0, blk, PasteAll); }
                    for (int c = 0; c < (int)b.sheets[i].cols.size(); ++c) if (b.sheets[i].cols[c].width >= 0) b.set_col_width(si, c, c, b.sheets[i].cols[c].width);
                    b.set_freeze(si, b.sheets[i].freeze_rows, b.sheets[i].freeze_cols);
                    sync_ui(); cur = si;
                }
                if (menu_item_icon("Move Left", ui::Icon::Undo, nullptr, false, i > 0)) { move_from = i; move_to = i - 1; }
                if (menu_item_icon("Move Right", ui::Icon::Redo, nullptr, false, i + 1 < (int)b.sheets.size())) { move_from = i; move_to = i + 1; }
                if (begin_menu_icon("Tab Color", ui::Icon::Palette)) {
                    uint32_t c = w.tab_color;
                    if (sg::color_palette(c, "No Color")) { b.set_tab_color(i, c); ImGui::CloseCurrentPopup(); }
                    ImGui::EndMenu();
                }
                int visible = 0;
                for (auto& x : b.sheets) visible += !x.hidden;
                if (menu_item_icon("Hide", ui::Icon::EyeOff, nullptr, false, visible > 1)) { b.sheets[i].hidden = true; ++b.gen; if (cur == i) for (int k = 0; k < (int)b.sheets.size(); ++k) if (!b.sheets[k].hidden) { cur = k; break; } }
                bool any_hidden = false;
                for (auto& x : b.sheets) any_hidden |= x.hidden;
                if (any_hidden && begin_menu_icon("Unhide", ui::Icon::Eye)) {
                    for (int k = 0; k < (int)b.sheets.size(); ++k) if (b.sheets[k].hidden && ImGui::MenuItem(b.sheets[k].name.c_str())) { b.sheets[k].hidden = false; ++b.gen; cur = k; }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (menu_item_icon("Delete", ui::Icon::Delete, nullptr, false, b.sheets.size() > 1)) {
                    int del = i;
                    defer([this, del]() {
                        if (del < (int)b.sheets.size() && confirm("Delete Sheet", "Delete sheet \"" + b.sheets[del].name + "\"? This can be undone with Ctrl+Z.", "Delete", true)) { b.remove_sheet(del); sync_ui(); }
                    });
                }
                ImGui::EndPopup();
            }
        }
        ImGui::PopID();
        ImGui::SameLine(0, S(2));
    }
    if (!ImGui::IsMouseDown(0)) tab_drag = -1;
    if (move_from >= 0 && move_to >= 0) { b.move_sheet(move_from, move_to); if (cur == move_from) cur = move_to; else if (cur == move_to) cur = move_from; sync_ui(); }
    draw_statusbar(h);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
}

void SheetDoc::draw_statusbar(float h) {
    const GTheme& t = th();
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    float right = win->WorkRect.Max.x;
    ImVec2 p(right - S(420), ImGui::GetCursorScreenPos().y);
    float y0 = win->DC.CursorPos.y;
    (void)y0;
    // stats (cached by gen + selection)
    uint64_t key = b.gen * 1000003ull + (uint64_t)cur;
    for (auto& s : su().sel) key = key * 31 + (uint64_t)s.r0 * 7 + s.r1 * 13 + s.c0 * 17 + s.c1 * 19;
    if (key != stat_key) {
        stat_key = key;
        double sum = 0, mn = 1e300, mx = -1e300; long cnt = 0, num = 0, cells = 0;
        const Worksheet& w = ws();
        for (auto& s : su().sel) {
            Range g = s.norm();
            g.r1 = std::min(g.r1, w.nrows() - 1); g.c1 = std::min(g.c1, w.ncols() - 1);
            for (int r = g.r0; r <= g.r1 && cells < 3000000; ++r) {
                if (w.row_hidden(r)) continue;
                for (int c = g.c0; c <= g.c1; ++c, ++cells) {
                    const Value& v = w.value(r, c);
                    if (v.is_empty()) continue;
                    ++cnt;
                    if (v.is_num()) { sum += v.num; ++num; mn = std::min(mn, v.num); mx = std::max(mx, v.num); }
                }
            }
        }
        char buf[200] = "";
        Range g = prim();
        if (cnt > 1 || su().sel.size() > 1 || (g.rows() > 1 || g.cols() > 1)) {
            if (num) std::snprintf(buf, sizeof buf, "Average: %.10g    Count: %ld    Sum: %.10g", sum / num, cnt, sum);
            else std::snprintf(buf, sizeof buf, "Count: %ld", cnt);
        }
        stat_text = buf;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float yy = ImGui::GetCursorScreenPos().y;
    ImVec2 ts = ImGui::CalcTextSize(stat_text.c_str());
    // - [slider] + [400%]: size the group from the real label width so the percentage is never clipped
    float zx = right - (S(152) + ImGui::CalcTextSize("400%").x + 2 * ImGui::GetStyle().FramePadding.x + S(18));
    float sy = yy + (h - S(4) - ts.y) * 0.5f;
    if (!stat_text.empty()) dl->AddText(ImVec2(zx - ts.x - S(14), sy), t.text_dim, stat_text.c_str());
    if (ImGui::IsMouseHoveringRect(ImVec2(zx - ts.x - S(14), sy), ImVec2(zx, sy + ts.y)) && !stat_text.empty()) tooltip("Selection statistics (visible cells)");
    // zoom: - [slider] + 100%
    Worksheet& W = ws();
    ImGui::SetCursorScreenPos(ImVec2(zx, yy));
    if (sg::gbtn("zminus", G::None, "Zoom out", false, true, 0, S(22))) W.zoom = std::max(25, (W.zoom - (W.zoom > 200 ? 25 : 10)) / 5 * 5);
    dl->AddLine(ImVec2(zx + S(6), yy + S(11)), ImVec2(zx + S(16), yy + S(11)), t.text, S(1.5f));
    ImGui::SameLine(0, S(2));
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, yy + S(1)));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(2), S(2)));
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, S(8));
    ImGui::SetNextItemWidth(S(100));
    int z = W.zoom;
    if (ImGui::SliderInt("##zs", &z, 25, 400, "")) W.zoom = z;
    ImGui::PopStyleVar(2);
    ImGui::SameLine(0, S(2));
    ImVec2 pp = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(pp.x, yy));
    if (sg::gbtn("zplus", G::None, "Zoom in", false, true, 0, S(22))) W.zoom = std::min(400, (W.zoom + (W.zoom >= 200 ? 25 : 10)) / 5 * 5);
    dl->AddLine(ImVec2(pp.x + S(6), yy + S(11)), ImVec2(pp.x + S(16), yy + S(11)), t.text, S(1.5f));
    dl->AddLine(ImVec2(pp.x + S(11), yy + S(6)), ImVec2(pp.x + S(11), yy + S(16)), t.text, S(1.5f));
    ImGui::SameLine(0, S(4));
    char zl[16]; std::snprintf(zl, sizeof zl, "%d%%", W.zoom);
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, yy + S(3)));
    if (ImGui::SmallButton(zl)) W.zoom = 100;
    if (ImGui::IsItemHovered()) tooltip("Zoom (Ctrl+wheel). Click: 100%");
    (void)p;
}

} // namespace pcbp::gui
