// ============================================================================
// sheet_dialogs.cpp — spreadsheet dialogs (ImGui modals, non-blocking):
// Format Cells (Number / Alignment / Font / Border / Fill), Find & Replace
// (modeless), Go To, Sort (multi-key), Conditional Formatting manager + rule
// editor, Data Validation, Paste Special, Text to Columns, Row height /
// Column width, Remove Duplicates, Apply BOM diff preview (sheet_bom.cpp).
// ============================================================================
#include "sheet_editor.h"
#include "../ui/win.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace pcbp::gui {
using namespace sheet;
using sg::G;

static const char* dlg_name(Dlg d) {
    switch (d) {
    case Dlg::FormatCells: return "Format Cells###sfc";
    case Dlg::GoTo: return "Go To###sgo";
    case Dlg::Sort: return "Sort###ssort";
    case Dlg::CondFmt: return "Conditional Formatting Rules###scf";
    case Dlg::Validation: return "Data Validation###sdv";
    case Dlg::PasteSpecial: return "Paste Special###sps";
    case Dlg::TextToCols: return "Text to Columns###st2c";
    case Dlg::RowHeight: return "Row Height###srh";
    case Dlg::ColWidth: return "Column Width###scw";
    case Dlg::RemoveDup: return "Remove Duplicates###srd";
    case Dlg::ApplyBom: return "Apply BOM to Project###sab";
    default: return "##none";
    }
}

static bool footer(const char* ok_label = "OK", bool ok_enabled = true) {
    ImGui::Separator();
    float w = S(96);
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - 2 * w - S(8));
    bool ok = button(ok_label, ui::Icon::Check, true, ImVec2(w, 0), ok_enabled);
    ImGui::SameLine(0, S(8));
    if (button("Cancel", ui::Icon::None, false, ImVec2(w, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    if (ok_enabled && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) && !ImGui::GetIO().WantTextInput) ok = true;
    return ok;
}

static void color_button(const char* id, uint32_t& c, const char* none_label) {
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = S(120), h = ImGui::GetFrameHeight();
    if (ImGui::InvisibleButton("##cb", ImVec2(w, h))) ImGui::OpenPopup("##pal");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), th().field, S(4));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), ImGui::IsItemHovered() ? th().accent : th().border, S(4));
    if (c) dl->AddRectFilled(ImVec2(p.x + S(4), p.y + S(4)), ImVec2(p.x + w - S(22), p.y + h - S(4)), doc_col(c), S(2));
    else dl->AddText(ImVec2(p.x + S(8), p.y + (h - ImGui::GetFontSize()) * 0.5f), th().text_dim, none_label);
    dl->AddTriangleFilled(ImVec2(p.x + w - S(15), p.y + h * 0.45f), ImVec2(p.x + w - S(7), p.y + h * 0.45f), ImVec2(p.x + w - S(11), p.y + h * 0.62f), th().text_dim);
    if (ImGui::BeginPopup("##pal")) {
        if (sg::color_palette(c, none_label)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

static void label(const char* s, float x = 0) {
    if (x > 0) ImGui::SetCursorPosX(x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(th().text_dim), "%s", s);
}

// ===========================================================================
// Format Cells
// ===========================================================================
namespace {
struct FcState {
    Style o, e;
    bool merge0 = false, merge = false;
    int cat = 0, dec = 2, neg = 0, date_i = 0, time_i = 0, sym = 0;
    bool thou = false;
    char custom[160] = "General";
    // border editor: top, bottom, left, right, inside-h, inside-v, diag-down, diag-up
    BorderSide side[8], side0[8];
    int line_style = 1;
    uint32_t line_color = 0;
    bool multi_r = false, multi_c = false;
    Value sample;
};
FcState fc;

const char* k_cats[] = {"General", "Number", "Currency", "Accounting", "Date", "Time", "Percentage", "Fraction", "Scientific", "Text", "Special", "Custom"};
const char* k_dates[] = {"m/d/yyyy", "d-mmm-yy", "d-mmm", "mmm-yy", "mmmm d, yyyy", "dddd, mmmm d, yyyy", "yyyy-mm-dd", "dd/mm/yyyy", "m/d/yy h:mm"};
const char* k_times[] = {"h:mm", "h:mm AM/PM", "h:mm:ss", "h:mm:ss AM/PM", "[h]:mm:ss", "mm:ss.0"};
const char* k_customs[] = {"General", "0", "0.00", "#,##0", "#,##0.00", "0%", "0.00%", "0.00E+00", "# ?/?", "@", "m/d/yyyy", "h:mm", "\"R\"0", "000", "0.0\" mm\"", "[Red]0;[Blue]-0"};
const char* k_syms[] = {"$", "\xE2\x82\xAC", "\xC2\xA3", "\xC2\xA5", "None"};

std::string build_code() {
    std::string d = fc.dec > 0 ? "." + std::string(fc.dec, '0') : "";
    std::string sym = fc.sym < 4 ? k_syms[fc.sym] : "";
    switch (fc.cat) {
    case 0: return "General";
    case 1: {
        std::string base = (fc.thou ? "#,##0" : "0") + d;
        switch (fc.neg) { case 1: return base + ";[Red]" + base; case 2: return base + "_);(" + base + ")"; case 3: return base + "_);[Red](" + base + ")"; default: return base; }
    }
    case 2: { std::string base = (sym.empty() ? "" : "\"" + sym + "\"") + "#,##0" + d; if (sym == "$") base = "$#,##0" + d; return fc.neg == 1 ? base + ";[Red]-" + base : fc.neg == 2 ? base + "_);(" + base + ")" : base; }
    case 3: { std::string s2 = sym.empty() ? "" : (sym == "$" ? "$" : "\"" + sym + "\""); return "_(" + s2 + "* #,##0" + d + "_);_(" + s2 + "* (#,##0" + d + ");_(" + s2 + "* \"-\"??_);_(@_)"; }
    case 4: return k_dates[fc.date_i];
    case 5: return k_times[fc.time_i];
    case 6: return "0" + d + "%";
    case 7: return fc.dec <= 1 ? "# ?/?" : fc.dec == 2 ? "# ?" "?/?" "?" : "# ?" "?" "?/?" "?" "?";
    case 8: return "0" + d + "E+00";
    case 9: return "@";
    case 10: return "00000";
    default: return fc.custom;
    }
}

int guess_cat(const std::string& code) {
    if (code == "General" || code.empty()) return 0;
    if (code == "@") return 9;
    if (code.find('%') != std::string::npos) return 6;
    if (code.find("E+") != std::string::npos) return 8;
    if (code.find('?') != std::string::npos && code.find('/') != std::string::npos) return 7;
    if (code.find("_(") == 0) return 3;
    if (is_date_format(code)) { for (auto* t : k_times) if (code == t) return 5; return 4; }
    if (code.find('$') != std::string::npos || code.find("\xE2\x82\xAC") != std::string::npos) return 2;
    if (code.find_first_not_of("#,0.;[]Red()_-") == std::string::npos) return 1;
    return 11;
}
} // namespace

static void init_format_cells(SheetDoc& d) {
    Range g = d.prim();
    fc = FcState{};
    fc.o = fc.e = d.b.style_of(d.cur, d.ar(), d.ac());
    fc.merge0 = fc.merge = d.ws().merge_at(d.ar(), d.ac()) != nullptr;
    fc.sample = d.ws().value(d.ar(), d.ac());
    if (fc.sample.is_empty()) fc.sample = Value::number(-1234.5678);
    std::snprintf(fc.custom, sizeof fc.custom, "%s", fc.e.numfmt.c_str());
    fc.cat = guess_cat(fc.e.numfmt);
    size_t dot = fc.e.numfmt.find('.');
    if (dot != std::string::npos) { int n = 0; for (size_t i = dot + 1; i < fc.e.numfmt.size() && (fc.e.numfmt[i] == '0' || fc.e.numfmt[i] == '#'); ++i) ++n; fc.dec = n; }
    else if (fc.cat != 0) fc.dec = 0;
    fc.thou = fc.e.numfmt.find("#,##") != std::string::npos;
    for (int i = 0; i < 9; ++i) if (fc.e.numfmt == k_dates[i]) fc.date_i = i;
    for (int i = 0; i < 6; ++i) if (fc.e.numfmt == k_times[i]) fc.time_i = i;
    fc.multi_r = g.rows() > 1; fc.multi_c = g.cols() > 1;
    const Border& bd = fc.e.border;
    // outer edges from the corner cells
    fc.side[0] = d.b.style_of(d.cur, g.r0, g.c0).border.top;
    fc.side[1] = d.b.style_of(d.cur, std::min(g.r1, d.ws().nrows()), g.c0).border.bottom;
    fc.side[2] = d.b.style_of(d.cur, g.r0, g.c0).border.left;
    fc.side[3] = d.b.style_of(d.cur, g.r0, std::min(g.c1, d.ws().ncols())).border.right;
    fc.side[4] = fc.multi_r ? bd.bottom : BorderSide{};
    fc.side[5] = fc.multi_c ? bd.right : BorderSide{};
    fc.side[6] = bd.diag_down ? bd.diagonal : BorderSide{};
    fc.side[7] = bd.diag_up ? bd.diagonal : BorderSide{};
    for (int i = 0; i < 8; ++i) fc.side0[i] = fc.side[i];
}

static void apply_format_cells(SheetDoc& d) {
    Style o = fc.o, e = fc.e;
    if (fc.cat != 11) e.numfmt = build_code(); else e.numfmt = fc.custom;
    Book::Batch bt(d.b, "Format Cells");
    d.commit_edit();
    // apply only what changed (mixed selections keep their other attributes)
    d.each_sel([&](const Range& g) {
        d.b.apply_style(d.cur, g, [o, e](Style& s) {
            if (!(o.font.name == e.font.name)) s.font.name = e.font.name;
            if (o.font.size != e.font.size) s.font.size = e.font.size;
            if (o.font.bold != e.font.bold) s.font.bold = e.font.bold;
            if (o.font.italic != e.font.italic) s.font.italic = e.font.italic;
            if (o.font.strike != e.font.strike) s.font.strike = e.font.strike;
            if (o.font.underline != e.font.underline) s.font.underline = e.font.underline;
            if (o.font.color != e.font.color) s.font.color = e.font.color;
            if (!(o.fill == e.fill)) s.fill = e.fill;
            if (o.numfmt != e.numfmt) s.numfmt = e.numfmt;
            if (o.halign != e.halign) s.halign = e.halign;
            if (o.valign != e.valign) s.valign = e.valign;
            if (o.wrap != e.wrap) s.wrap = e.wrap;
            if (o.shrink != e.shrink) s.shrink = e.shrink;
            if (o.indent != e.indent) s.indent = e.indent;
            if (o.rotation != e.rotation) s.rotation = e.rotation;
            if (o.locked != e.locked) s.locked = e.locked;
            if (o.hidden != e.hidden) s.hidden = e.hidden;
        }, "Format Cells");
        // borders
        bool changed = false;
        for (int i = 0; i < 8; ++i) if (!(fc.side[i] == fc.side0[i])) changed = true;
        if (changed) {
            auto edge = [&](const Range& rg, int which, BorderSide s) {
                if (!rg.valid()) return;
                d.b.apply_style(d.cur, rg, [which, s](Style& st) {
                    switch (which) { case 0: st.border.top = s; break; case 1: st.border.bottom = s; break; case 2: st.border.left = s; break; case 3: st.border.right = s; break;
                    case 6: st.border.diagonal = s; st.border.diag_down = s.style != BorderStyle::None; break;
                    case 7: st.border.diagonal = s; st.border.diag_up = s.style != BorderStyle::None; break; }
                }, "Border");
            };
            if (!(fc.side[0] == fc.side0[0])) edge(Range(g.r0, g.c0, g.r0, g.c1), 0, fc.side[0]);
            if (!(fc.side[1] == fc.side0[1])) edge(Range(g.r1, g.c0, g.r1, g.c1), 1, fc.side[1]);
            if (!(fc.side[2] == fc.side0[2])) edge(Range(g.r0, g.c0, g.r1, g.c0), 2, fc.side[2]);
            if (!(fc.side[3] == fc.side0[3])) edge(Range(g.r0, g.c1, g.r1, g.c1), 3, fc.side[3]);
            if (!(fc.side[4] == fc.side0[4]) && g.rows() > 1) edge(Range(g.r0, g.c0, g.r1 - 1, g.c1), 1, fc.side[4]);
            if (!(fc.side[5] == fc.side0[5]) && g.cols() > 1) edge(Range(g.r0, g.c0, g.r1, g.c1 - 1), 3, fc.side[5]);
            if (!(fc.side[6] == fc.side0[6])) edge(g, 6, fc.side[6]);
            if (!(fc.side[7] == fc.side0[7])) edge(g, 7, fc.side[7]);
        }
        if (fc.merge != fc.merge0) { if (fc.merge) d.b.merge(d.cur, g); else d.b.unmerge(d.cur, g); }
    });
}

static void preview_cell(ImVec2 sz, const Style& st, const std::string& text) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(sz);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    uint32_t fill = st.fill.pattern != FillPattern::None ? (st.fill.fg ? st.fill.fg : 0xFF000000) : 0;
    dl->AddRectFilled(p, p + sz, fill ? doc_col(fill) : th().field, S(4));
    dl->AddRect(p, p + sz, th().border, S(4));
    ImFont* f = st.font.bold && g_font_bold ? g_font_bold : ImGui::GetFont();
    float fs = (float)(st.font.size * 96.0 / 72.0) * S(1);
    ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, text.c_str());
    float x = st.halign == HAlign::Right ? p.x + sz.x - ts.x - S(8) : st.halign == HAlign::Center ? p.x + (sz.x - ts.x) * 0.5f : p.x + S(8);
    float y = st.valign == VAlign::Top ? p.y + S(4) : st.valign == VAlign::Center ? p.y + (sz.y - ts.y) * 0.5f : p.y + sz.y - ts.y - S(4);
    ImU32 col = st.font.color ? doc_col(st.font.color) : (fill ? (((fill >> 16 & 255) * 0.3 + (fill >> 8 & 255) * 0.59 + (fill & 255) * 0.11) > 128 ? IM_COL32(0, 0, 0, 255) : IM_COL32_WHITE) : th().text);
    int v0 = dl->VtxBuffer.Size;
    dl->PushClipRect(p, p + sz, true);
    dl->AddText(f, fs, ImVec2(x, y), col, text.c_str());
    if (st.font.italic) for (int i = v0; i < dl->VtxBuffer.Size; ++i) dl->VtxBuffer[i].pos.x += (y + fs - dl->VtxBuffer[i].pos.y) * 0.2f;
    if (st.font.underline != Underline::None) dl->AddLine(ImVec2(x, y + fs), ImVec2(x + ts.x, y + fs), col);
    if (st.font.strike) dl->AddLine(ImVec2(x, y + fs * 0.55f), ImVec2(x + ts.x, y + fs * 0.55f), col);
    dl->PopClipRect();
}

void SheetDoc::dlg_format_cells() {
    ImGui::SetNextWindowSize(ImVec2(S(640), S(520)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(dlg_name(Dlg::FormatCells), nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    static const char* tabs[] = {"Number", "Alignment", "Font", "Border", "Fill", "Protection"};
    if (ImGui::BeginTabBar("##fct")) {
        for (int i = 0; i < 6; ++i) {
            if (ImGui::BeginTabItem(tabs[i], nullptr, fmt_tab == i && ImGui::IsWindowAppearing() ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (!ImGui::IsWindowAppearing()) fmt_tab = i;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    float body_h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() - S(14);
    ImGui::BeginChild("##fcb", ImVec2(0, body_h));
    Style& e = fc.e;
    switch (fmt_tab) {
    case 0: {   // Number
        ImGui::BeginChild("##cats", ImVec2(S(150), 0), ImGuiChildFlags_Borders);
        for (int i = 0; i < 12; ++i) if (ImGui::Selectable(k_cats[i], fc.cat == i)) { fc.cat = i; if (i == 11) std::snprintf(fc.custom, sizeof fc.custom, "%s", build_code().c_str()); }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginGroup();
        std::string code = fc.cat == 11 ? std::string(fc.custom) : build_code();
        Formatted sm = format_value(fc.sample, code, b.date1904);
        section_header("Sample");
        ImGui::TextColored(sm.color ? ImGui::ColorConvertU32ToFloat4(doc_col(sm.color)) : ImGui::ColorConvertU32ToFloat4(th().text), "%s", sm.text.empty() ? " " : sm.text.c_str());
        ImGui::Spacing();
        if (fc.cat == 1 || fc.cat == 2 || fc.cat == 3 || fc.cat == 6 || fc.cat == 8) {
            label("Decimal places");
            ImGui::SameLine(S(140)); ImGui::SetNextItemWidth(S(90));
            ImGui::InputInt("##dec", &fc.dec); fc.dec = std::clamp(fc.dec, 0, 15);
        }
        if (fc.cat == 2 || fc.cat == 3) {
            label("Symbol"); ImGui::SameLine(S(140)); ImGui::SetNextItemWidth(S(90));
            ImGui::Combo("##sym", &fc.sym, k_syms, 5);
        }
        if (fc.cat == 1) ImGui::Checkbox("Use 1000 separator (,)", &fc.thou);
        if (fc.cat == 1 || fc.cat == 2) {
            section_header("Negative numbers");
            static const char* negs[] = {"-1234.10", "1234.10 (red)", "(1234.10)", "(1234.10) (red)"};
            for (int i = 0; i < (fc.cat == 2 ? 3 : 4); ++i) if (ImGui::RadioButton(negs[i], fc.neg == i)) fc.neg = i;
        }
        if (fc.cat == 4) {
            section_header("Type");
            ImGui::BeginChild("##dt", ImVec2(S(300), S(200)), ImGuiChildFlags_Borders);
            for (int i = 0; i < 9; ++i) { Formatted f = format_value(Value::number(45366.5625), k_dates[i], b.date1904); if (ImGui::Selectable((f.text + "##" + std::to_string(i)).c_str(), fc.date_i == i)) fc.date_i = i; }
            ImGui::EndChild();
        }
        if (fc.cat == 5) {
            section_header("Type");
            for (int i = 0; i < 6; ++i) { Formatted f = format_value(Value::number(0.5729), k_times[i], b.date1904); if (ImGui::RadioButton((f.text + "   (" + k_times[i] + ")").c_str(), fc.time_i == i)) fc.time_i = i; }
        }
        if (fc.cat == 9) ImGui::TextWrapped("Text format cells are treated as text even when a number is in the cell (part numbers like 0805 keep their zeros).");
        if (fc.cat == 10) ImGui::TextWrapped("Zip code (00000). Use Custom for other fixed-digit codes.");
        if (fc.cat == 11) {
            label("Type");
            ImGui::SetNextItemWidth(S(300));
            ImGui::InputText("##cust", fc.custom, sizeof fc.custom);
            ImGui::BeginChild("##cl", ImVec2(S(300), S(180)), ImGuiChildFlags_Borders);
            for (auto* c : k_customs) if (ImGui::Selectable(c, strcmp(fc.custom, c) == 0)) std::snprintf(fc.custom, sizeof fc.custom, "%s", c);
            ImGui::EndChild();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(th().text_faint), "Sections: positive;negative;zero;text  \xC2\xB7  [Red] colours  \xC2\xB7  \"text\" literals");
        }
        if (fc.cat == 0) ImGui::TextWrapped("General format cells have no specific number format.");
        ImGui::EndGroup();
        break;
    }
    case 1: {   // Alignment
        static const char* ha[] = {"General", "Left (Indent)", "Center", "Right (Indent)", "Fill", "Justify", "Center Across Selection", "Distributed"};
        static const char* va[] = {"Bottom", "Center", "Top", "Justify", "Distributed"};
        section_header("Text alignment");
        label("Horizontal"); ImGui::SameLine(S(120)); ImGui::SetNextItemWidth(S(220));
        int h = (int)e.halign; if (ImGui::Combo("##ha", &h, ha, 8)) e.halign = (HAlign)h;
        ImGui::SameLine(S(370)); label("Indent"); ImGui::SameLine(); ImGui::SetNextItemWidth(S(90));
        int ind = e.indent; if (ImGui::InputInt("##ind", &ind)) e.indent = (uint8_t)std::clamp(ind, 0, 15);
        label("Vertical"); ImGui::SameLine(S(120)); ImGui::SetNextItemWidth(S(220));
        int v = (int)e.valign; if (ImGui::Combo("##va", &v, va, 5)) e.valign = (VAlign)v;
        section_header("Text control");
        ImGui::Checkbox("Wrap text", &e.wrap);
        ImGui::Checkbox("Shrink to fit", &e.shrink);
        ImGui::Checkbox("Merge cells", &fc.merge);
        section_header("Orientation");
        int rot = e.rotation == 255 ? 0 : e.rotation;
        ImGui::SetNextItemWidth(S(240));
        if (ImGui::SliderInt("Degrees", &rot, -90, 90)) e.rotation = (int16_t)rot;
        bool stacked = e.rotation == 255;
        if (ImGui::Checkbox("Vertical text (stacked)", &stacked)) e.rotation = stacked ? 255 : 0;
        ImGui::Spacing();
        preview_cell(ImVec2(S(300), S(70)), e, "Preview text");
        break;
    }
    case 2: {   // Font
        static const char* fonts[] = {"Calibri", "Aptos", "Arial", "Segoe UI", "Tahoma", "Verdana", "Cambria", "Times New Roman", "Consolas", "Courier New"};
        ImGui::BeginGroup();
        label("Font");
        ImGui::BeginChild("##fl", ImVec2(S(220), S(170)), ImGuiChildFlags_Borders);
        for (auto* f : fonts) if (ImGui::Selectable(f, e.font.name == f)) e.font.name = f;
        ImGui::EndChild();
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::BeginGroup();
        label("Font style");
        static const char* fsty[] = {"Regular", "Italic", "Bold", "Bold Italic"};
        ImGui::BeginChild("##fsl", ImVec2(S(150), S(170)), ImGuiChildFlags_Borders);
        int cs = (e.font.bold ? 2 : 0) + (e.font.italic ? 1 : 0);
        for (int i = 0; i < 4; ++i) if (ImGui::Selectable(fsty[i], cs == i)) { e.font.bold = i >= 2; e.font.italic = i & 1; }
        ImGui::EndChild();
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::BeginGroup();
        label("Size");
        ImGui::BeginChild("##fzl", ImVec2(S(90), S(170)), ImGuiChildFlags_Borders);
        static const int sizes[] = {8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72};
        for (int s : sizes) { char l[8]; std::snprintf(l, sizeof l, "%d", s); if (ImGui::Selectable(l, (int)e.font.size == s)) e.font.size = s; }
        ImGui::EndChild();
        ImGui::EndGroup();
        static const char* ul[] = {"None", "Single", "Double", "Single Accounting", "Double Accounting"};
        label("Underline"); ImGui::SameLine(S(110)); ImGui::SetNextItemWidth(S(200));
        int u = (int)e.font.underline; if (ImGui::Combo("##ul", &u, ul, 5)) e.font.underline = (Underline)u;
        ImGui::SameLine(S(340)); label("Color"); ImGui::SameLine();
        color_button("fcolor", e.font.color, "Automatic");
        ImGui::Checkbox("Strikethrough", &e.font.strike);
        section_header("Preview");
        preview_cell(ImVec2(S(420), S(64)), e, "AaBbCcYyZz  R12  0805");
        break;
    }
    case 3: {   // Border
        ImGui::BeginGroup();
        section_header("Line");
        ImGui::BeginChild("##ls", ImVec2(S(170), S(250)), ImGuiChildFlags_Borders);
        for (int i = 0; i < 14; ++i) {
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushID(i);
            if (ImGui::Selectable("##st", fc.line_style == i, 0, ImVec2(0, S(14)))) fc.line_style = i;
            ImGui::PopID();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (i == 0) dl->AddText(ImVec2(p.x + S(4), p.y), th().text, "None");
            else {
                BorderSide s{(BorderStyle)i, 0};
                float y = p.y + S(7), w = S(140);
                ImU32 c = th().text;
                float u = S(1);
                switch (s.style) {
                case BorderStyle::Medium: case BorderStyle::MediumDashed: case BorderStyle::MediumDashDot: case BorderStyle::MediumDashDotDot: case BorderStyle::SlantDashDot: u = S(2); break;
                case BorderStyle::Thick: u = S(3); break;
                default: break;
                }
                if (s.style == BorderStyle::Double) { dl->AddLine(ImVec2(p.x + S(4), y - S(1.5f)), ImVec2(p.x + w, y - S(1.5f)), c); dl->AddLine(ImVec2(p.x + S(4), y + S(1.5f)), ImVec2(p.x + w, y + S(1.5f)), c); }
                else if (s.style == BorderStyle::Thin || s.style == BorderStyle::Medium || s.style == BorderStyle::Thick || s.style == BorderStyle::Hair) dl->AddLine(ImVec2(p.x + S(4), y), ImVec2(p.x + w, y), s.style == BorderStyle::Hair ? col_alpha(c, 0.5f) : c, u);
                else { float on = s.style == BorderStyle::Dotted ? S(1) : S(5), off = S(2); for (float x = p.x + S(4); x < p.x + w; x += on + off) dl->AddLine(ImVec2(x, y), ImVec2(std::min(x + on, p.x + w), y), c, u); }
            }
        }
        ImGui::EndChild();
        label("Color");
        color_button("bcol", fc.line_color, "Automatic");
        ImGui::EndGroup();
        ImGui::SameLine(0, S(16));
        ImGui::BeginGroup();
        section_header("Presets");
        BorderSide cur_side{(BorderStyle)fc.line_style, fc.line_color};
        if (cur_side.style == BorderStyle::None) cur_side = BorderSide{BorderStyle::Thin, fc.line_color};
        if (button("None", ui::Icon::None, false, ImVec2(S(90), 0))) for (auto& s : fc.side) s = BorderSide{};
        ImGui::SameLine();
        if (button("Outline", ui::Icon::None, false, ImVec2(S(90), 0))) for (int i = 0; i < 4; ++i) fc.side[i] = cur_side;
        ImGui::SameLine();
        if (button("Inside", ui::Icon::None, false, ImVec2(S(90), 0), fc.multi_r || fc.multi_c)) { if (fc.multi_r) fc.side[4] = cur_side; if (fc.multi_c) fc.side[5] = cur_side; }
        section_header("Border (click the edges)");
        ImVec2 p = ImGui::GetCursorScreenPos();
        float W = S(260), H = S(160);
        ImGui::InvisibleButton("##bprev", ImVec2(W, H));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + W, p.y + H), th().field, S(4));
        ImVec2 a(p.x + S(20), p.y + S(20)), bb(p.x + W - S(20), p.y + H - S(20));
        // corner ticks
        for (ImVec2 c : {a, ImVec2(bb.x, a.y), ImVec2(a.x, bb.y), bb}) { dl->AddLine(ImVec2(c.x - S(8), c.y), ImVec2(c.x - S(3), c.y), th().text_faint); dl->AddLine(ImVec2(c.x, c.y - S(8)), ImVec2(c.x, c.y - S(3)), th().text_faint); }
        ImVec2 m((a.x + bb.x) * 0.5f, (a.y + bb.y) * 0.5f);
        dl->AddText(ImVec2(a.x + S(10), m.y - S(8)), th().text_faint, "Text");
        if (fc.multi_c) dl->AddText(ImVec2(m.x + S(10), m.y - S(8)), th().text_faint, "Text");
        auto drawside = [&](ImVec2 s0, ImVec2 s1, const BorderSide& s) {
            if (s.style == BorderStyle::None) return;
            ImU32 c = s.color ? doc_col(s.color) : th().text;
            float u = s.style == BorderStyle::Thick ? S(3) : (s.style == BorderStyle::Medium ? S(2) : S(1));
            dl->AddLine(s0, s1, c, u);
        };
        drawside(a, ImVec2(bb.x, a.y), fc.side[0]);
        drawside(ImVec2(a.x, bb.y), bb, fc.side[1]);
        drawside(a, ImVec2(a.x, bb.y), fc.side[2]);
        drawside(ImVec2(bb.x, a.y), bb, fc.side[3]);
        if (fc.multi_r) drawside(ImVec2(a.x, m.y), ImVec2(bb.x, m.y), fc.side[4]);
        if (fc.multi_c) drawside(ImVec2(m.x, a.y), ImVec2(m.x, bb.y), fc.side[5]);
        drawside(a, bb, fc.side[6]);
        drawside(ImVec2(a.x, bb.y), ImVec2(bb.x, a.y), fc.side[7]);
        if (ImGui::IsItemClicked()) {
            ImVec2 q = ImGui::GetIO().MousePos;
            int hit = -1;
            float tol = S(10);
            if (std::fabs(q.y - a.y) < tol) hit = 0;
            else if (std::fabs(q.y - bb.y) < tol) hit = 1;
            else if (std::fabs(q.x - a.x) < tol) hit = 2;
            else if (std::fabs(q.x - bb.x) < tol) hit = 3;
            else if (fc.multi_r && std::fabs(q.y - m.y) < tol) hit = 4;
            else if (fc.multi_c && std::fabs(q.x - m.x) < tol) hit = 5;
            else { float t1 = (q.x - a.x) / (bb.x - a.x), t2 = (q.y - a.y) / (bb.y - a.y); hit = std::fabs(t1 - t2) < std::fabs(t1 - (1 - t2)) ? 6 : 7; }
            if (hit >= 0) fc.side[hit] = (fc.side[hit] == cur_side) ? BorderSide{} : cur_side;
        }
        if (ImGui::IsItemHovered()) tooltip("Click an edge to apply the selected line style and colour (click again to remove)");
        ImGui::EndGroup();
        break;
    }
    case 4: {   // Fill
        section_header("Background colour");
        uint32_t c = e.fill.pattern == FillPattern::Solid ? e.fill.fg : (e.fill.pattern == FillPattern::None ? 0 : e.fill.bg);
        if (sg::color_palette(c, "No Color")) {
            if (c) e.fill = Fill{FillPattern::Solid, c, 0}; else e.fill = Fill{};
        }
        ImGui::Spacing();
        static const char* pats[] = {"None", "Solid", "Medium gray", "Dark gray", "Light gray", "Dark horizontal", "Dark vertical", "Dark down", "Dark up", "Dark grid", "Dark trellis",
                                     "Light horizontal", "Light vertical", "Light down", "Light up", "Light grid", "Light trellis", "Gray 12.5%", "Gray 6.25%"};
        label("Pattern style"); ImGui::SameLine(S(120)); ImGui::SetNextItemWidth(S(200));
        int pi = (int)e.fill.pattern;
        if (ImGui::Combo("##pat", &pi, pats, 19)) { e.fill.pattern = (FillPattern)pi; if (pi > 1 && !e.fill.fg) e.fill.fg = 0xFF000000; }
        if (e.fill.pattern > FillPattern::Solid) { ImGui::SameLine(); label("Pattern colour"); ImGui::SameLine(); color_button("pcol", e.fill.fg, "Automatic"); }
        section_header("Sample");
        preview_cell(ImVec2(S(420), S(50)), e, "");
        break;
    }
    default: {
        ImGui::Checkbox("Locked", &e.locked);
        ImGui::Checkbox("Hidden", &e.hidden);
        ImGui::TextWrapped("Locking cells or hiding formulas has no effect until the worksheet is protected (kept for Excel round trips).");
    }
    }
    ImGui::EndChild();
    if (footer()) { apply_format_cells(*this); ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

// ===========================================================================
// Find & Replace (modeless window)
// ===========================================================================
void SheetDoc::dlg_find() {
    static bool first = true;
    if (dlg_open_req) { first = true; dlg_open_req = false; }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - S(560), vp->WorkPos.y + S(170)), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(S(500), 0), ImGuiCond_Appearing);
    bool open = true;
    if (!ImGui::Begin("Find and Replace###sheet_find", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::End(); if (!open) dlg = Dlg::None; return; }
    if (ImGui::BeginTabBar("##frt")) {
        if (ImGui::BeginTabItem("Find", nullptr, first && !show_replace ? ImGuiTabItemFlags_SetSelected : 0)) { show_replace = false; ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Replace", nullptr, first && show_replace ? ImGuiTabItemFlags_SetSelected : 0)) { show_replace = true; ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    label("Find what"); ImGui::SameLine(S(100)); ImGui::SetNextItemWidth(S(380));
    if (first) ImGui::SetKeyboardFocusHere();
    bool enter = ImGui::InputText("##fw", find_buf, sizeof find_buf, ImGuiInputTextFlags_EnterReturnsTrue);
    if (show_replace) { label("Replace with"); ImGui::SameLine(S(100)); ImGui::SetNextItemWidth(S(380)); ImGui::InputText("##rw", repl_buf, sizeof repl_buf); }
    ImGui::Checkbox("Match case", &match_case); ImGui::SameLine();
    ImGui::Checkbox("Match entire cell", &whole_cell); ImGui::SameLine();
    ImGui::Checkbox("Look in formulas", &find_formulas); ImGui::SameLine();
    ImGui::Checkbox("Workbook", &find_all_sheets);
    ImGui::Spacing();
    if (show_replace) {
        if (button("Replace All", ui::Icon::Wand)) replace_all();
        ImGui::SameLine();
        if (button("Replace", ui::Icon::None)) replace_one();
        ImGui::SameLine();
    }
    if (button("Find All", ui::Icon::Query)) find_all();
    ImGui::SameLine();
    if (button("Find Previous", ui::Icon::ArrowUp)) find_next(true);
    ImGui::SameLine();
    if (button("Find Next", ui::Icon::ArrowDown, true) || enter) { find_next(ImGui::GetIO().KeyShift); }
    if (!find_hits.empty()) {
        ImGui::Text("%d cell(s) found", (int)find_hits.size());
        ImGui::BeginChild("##hits", ImVec2(S(480), S(180)), ImGuiChildFlags_Borders);
        ImGuiListClipper clip;
        clip.Begin((int)find_hits.size());
        while (clip.Step()) for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
            auto& h = find_hits[i];
            if (h.first >= (int)b.sheets.size()) continue;
            std::string l = b.sheets[h.first].name + "!" + sg::a1(h.second.first, h.second.second);
            ImGui::PushID(i);
            if (ImGui::Selectable(l.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) { cur = h.first; sync_ui(); select_cell(h.second.first, h.second.second); }
            ImGui::SameLine(S(130));
            ImGui::TextDisabled("%s", b.display_text(h.first, h.second.first, h.second.second).c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape)) open = false;
    first = false;
    ImGui::End();
    if (!open) { dlg = Dlg::None; focus_grid = true; }
}

// ===========================================================================
// Go To
// ===========================================================================
void SheetDoc::dlg_goto() {
    static char ref[128];
    if (ImGui::IsPopupOpen(dlg_name(Dlg::GoTo)) && ImGui::GetCurrentContext()->OpenPopupStack.Size && false) {}
    ImGui::SetNextWindowSize(ImVec2(S(380), S(360)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(dlg_name(Dlg::GoTo), nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    if (ImGui::IsWindowAppearing()) ref[0] = 0;
    section_header("Go to");
    ImGui::BeginChild("##names", ImVec2(0, S(180)), ImGuiChildFlags_Borders);
    for (auto& dn : b.names) {
        if (dn.hidden) continue;
        if (ImGui::Selectable((dn.name + "##n").c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
            std::snprintf(ref, sizeof ref, "%s", dn.name.c_str());
            if (ImGui::IsMouseDoubleClicked(0)) { goto_ref(dn.formula); ImGui::CloseCurrentPopup(); }
        }
        ImGui::SameLine(S(140)); ImGui::TextDisabled("%s", dn.formula.c_str());
    }
    if (b.names.empty()) ImGui::TextDisabled("No defined names. Type a reference below.");
    ImGui::EndChild();
    label("Reference");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    bool enter = ImGui::InputTextWithHint("##ref", "e.g. B12, A1:D20, Sheet2!C3, a name", ref, sizeof ref, ImGuiInputTextFlags_EnterReturnsTrue);
    if (footer() || enter) {
        std::string r = ref;
        const DefinedName* dn = b.find_name(r, cur);
        if (goto_ref(dn ? dn->formula : r)) ImGui::CloseCurrentPopup();
        else toast("Reference is not valid", ToastKind::Warning, 2.5f);
    }
    ImGui::EndPopup();
}

// ===========================================================================
// Sort (multi key)
// ===========================================================================
void SheetDoc::dlg_sort() {
    static std::vector<SortKey> keys;
    static bool header = true;
    static Range rg;
    ImGui::SetNextWindowSize(ImVec2(S(600), S(360)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(dlg_name(Dlg::Sort), nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    const Worksheet& W = ws();
    if (ImGui::IsWindowAppearing()) {
        Range g = prim();
        if (W.autofilter.active() && W.autofilter.range.contains(g.r0, g.c0)) { rg = W.autofilter.range; rg.r1 = std::max(rg.r1, W.nrows() - 1); header = true; }
        else if (g.rows() > 1) { rg = g; rg.r1 = std::min(rg.r1, W.nrows() - 1); rg.c1 = std::min(rg.c1, W.ncols() - 1); header = false; }
        else { rg = data_region(g.r0, g.c0); header = W.freeze_rows > 0 || b.style_of(cur, rg.r0, rg.c0).font.bold || (W.value(rg.r0, rg.c0).is_str() && !W.value(rg.r0 + 1, rg.c0).is_str()); }
        keys = {SortKey{ac(), false, true, false}};
    }
    auto col_label = [&](int c) {
        std::string h = header ? b.display_text(cur, rg.r0, c) : "";
        return "Column " + col_name(c) + (h.empty() ? "" : "  (" + h + ")");
    };
    ImGui::Text("Range %s", range_name(rg).c_str());
    ImGui::SameLine(S(250));
    ImGui::Checkbox("My data has headers", &header);
    if (button("Add Level", ui::Icon::Plus)) keys.push_back(SortKey{rg.c0, false, true, false});
    ImGui::SameLine();
    ImGui::Spacing();
    ImGui::BeginChild("##keys", ImVec2(0, S(200)), ImGuiChildFlags_Borders);
    int del = -1, up = -1;
    for (int i = 0; i < (int)keys.size(); ++i) {
        ImGui::PushID(i);
        label(i == 0 ? "Sort by" : "Then by");
        ImGui::SameLine(S(70)); ImGui::SetNextItemWidth(S(220));
        if (ImGui::BeginCombo("##col", col_label(keys[i].col).c_str())) {
            for (int c = rg.c0; c <= rg.c1; ++c) if (ImGui::Selectable(col_label(c).c_str(), keys[i].col == c)) keys[i].col = c;
            ImGui::EndCombo();
        }
        ImGui::SameLine(); ImGui::SetNextItemWidth(S(110));
        int o = keys[i].desc;
        static const char* ord[] = {"A \xE2\x86\x92 Z", "Z \xE2\x86\x92 A"};
        if (ImGui::Combo("##ord", &o, ord, 2)) keys[i].desc = o;
        ImGui::SameLine(); ImGui::Checkbox("Natural", &keys[i].natural);
        if (ImGui::IsItemHovered()) tooltip("Natural order: R2 before R10, C9 before C10");
        ImGui::SameLine();
        if (icon_button("up", ui::Icon::ArrowUp, "Move up", false, i > 0, S(22))) up = i;
        ImGui::SameLine(0, S(2));
        if (icon_button("del", ui::Icon::Delete, "Delete level", false, keys.size() > 1, S(22))) del = i;
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (up > 0) std::swap(keys[up], keys[up - 1]);
    if (del >= 0) keys.erase(keys.begin() + del);
    if (footer("Sort")) {
        commit_edit();
        b.sort(cur, rg, keys, header);
        msg = "Sorted " + range_name(rg) + " by " + std::to_string(keys.size()) + " key(s)";
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ===========================================================================
// Conditional formatting manager + rule editor
// ===========================================================================
namespace {
struct CfEdit {
    bool open = false;
    int cf = -1, rule = -1;                // editing existing (cf index, rule index) or new (-1)
    int type = 0;                          // UI type index
    int op = 4;
    char f1[128] = "", f2[128] = "", text[128] = "", range[128] = "";
    uint32_t fill = 0xFFFFC7CE, font = 0xFF9C0006;
    bool bold = false, italic = false, stop = false;
    int rank = 10; bool bottom = false, percent = false, below = false;
    uint32_t c_min = 0xFF63BE7B, c_mid = 0xFFFFEB84, c_max = 0xFFF8696B, bar = 0xFF638EC6;
    bool three = true;
};
CfEdit ce;
const char* k_cf_types[] = {"Cell value", "Formula is", "Text contains", "Text does not contain", "Begins with", "Ends with", "Duplicate values", "Unique values",
                            "Top / Bottom N", "Above / below average", "Blanks", "No blanks", "Errors", "No errors", "Colour scale", "Data bar"};
const CondRule::Type k_cf_map[] = {CondRule::CellIs, CondRule::Expression, CondRule::ContainsText, CondRule::NotContainsText, CondRule::BeginsWith, CondRule::EndsWith,
                                   CondRule::Duplicate, CondRule::Unique, CondRule::Top10, CondRule::AboveAverage, CondRule::ContainsBlanks, CondRule::NotContainsBlanks,
                                   CondRule::ContainsErrors, CondRule::NotContainsErrors, CondRule::ColorScale, CondRule::DataBar};
const char* k_ops[] = {"between", "not between", "equal to", "not equal to", "greater than", "less than", "greater than or equal to", "less than or equal to"};

std::string rule_summary(const Book& b, const CondRule& r) {
    int ti = 0;
    for (int i = 0; i < 16; ++i) if (k_cf_map[i] == r.type) ti = i;
    std::string s = k_cf_types[ti];
    switch (r.type) {
    case CondRule::CellIs: s += std::string(" ") + k_ops[(int)r.op] + " " + r.f1 + (r.op == CmpOp::Between || r.op == CmpOp::NotBetween ? " and " + r.f2 : ""); break;
    case CondRule::Expression: s += ": =" + r.f1; break;
    case CondRule::ContainsText: case CondRule::NotContainsText: case CondRule::BeginsWith: case CondRule::EndsWith: s += " \"" + r.text + "\""; break;
    case CondRule::Top10: s = std::string(r.bottom ? "Bottom " : "Top ") + std::to_string(r.rank) + (r.percent ? "%" : ""); break;
    case CondRule::AboveAverage: s = r.below ? "Below average" : "Above average"; break;
    default: break;
    }
    (void)b;
    return s;
}
} // namespace

static void cf_load(SheetDoc& d, int cfi, int ri) {
    ce = CfEdit{};
    ce.open = true; ce.cf = cfi; ce.rule = ri;
    const Worksheet& W = d.ws();
    if (cfi < 0) {
        std::string rs;
        d.each_sel([&](const Range& g) { if (!rs.empty()) rs += " "; rs += range_name(g); });
        std::snprintf(ce.range, sizeof ce.range, "%s", rs.c_str());
        return;
    }
    const CondFormat& cf = W.cond[cfi];
    const CondRule& r = cf.rules[ri];
    std::string rs;
    for (auto& g : cf.ranges) { if (!rs.empty()) rs += " "; rs += range_name(g); }
    std::snprintf(ce.range, sizeof ce.range, "%s", rs.c_str());
    for (int i = 0; i < 16; ++i) if (k_cf_map[i] == r.type) ce.type = i;
    ce.op = (int)r.op;
    std::snprintf(ce.f1, sizeof ce.f1, "%s", r.f1.c_str());
    std::snprintf(ce.f2, sizeof ce.f2, "%s", r.f2.c_str());
    std::snprintf(ce.text, sizeof ce.text, "%s", r.text.c_str());
    ce.stop = r.stop; ce.rank = r.rank; ce.bottom = r.bottom; ce.percent = r.percent; ce.below = r.below;
    if (r.dxf >= 0 && r.dxf < (int)d.b.dxfs.size()) {
        const Dxf& x = d.b.dxfs[r.dxf];
        ce.fill = x.fill_color.value_or(0); ce.font = x.font_color.value_or(0);
        ce.bold = x.bold.value_or(false); ce.italic = x.italic.value_or(false);
    }
    if (r.type == CondRule::ColorScale) { ce.three = r.cfvo.size() >= 3; if (!r.cfvo.empty()) ce.c_min = r.cfvo.front().color; if (r.cfvo.size() >= 3) ce.c_mid = r.cfvo[1].color; if (!r.cfvo.empty()) ce.c_max = r.cfvo.back().color; }
    if (r.type == CondRule::DataBar) ce.bar = r.bar_color;
}

static bool cf_store(SheetDoc& d) {
    std::vector<Range> ranges;
    std::string rs = ce.range;
    size_t p = 0;
    while (p < rs.size()) {
        size_t q = rs.find_first_of(" ,;", p);
        std::string tok = rs.substr(p, q == std::string::npos ? std::string::npos : q - p);
        if (!tok.empty()) { Range g; if (!parse_range_name(tok, g)) { toast("Invalid range: " + tok, ToastKind::Warning, 3.f); return false; } ranges.push_back(g.norm()); }
        if (q == std::string::npos) break;
        p = q + 1;
    }
    if (ranges.empty()) { toast("Enter the range the rule applies to", ToastKind::Warning, 3.f); return false; }
    CondRule r;
    r.type = k_cf_map[ce.type];
    r.op = (CmpOp)ce.op;
    r.f1 = ce.f1; r.f2 = ce.f2; r.text = ce.text;
    if (!r.f1.empty() && r.f1[0] == '=') r.f1.erase(0, 1);
    if (!r.f2.empty() && r.f2[0] == '=') r.f2.erase(0, 1);
    r.stop = ce.stop; r.rank = ce.rank; r.bottom = ce.bottom; r.percent = ce.percent; r.below = ce.below;
    if (r.type == CondRule::ColorScale) {
        r.cfvo.push_back(CfValueObject{CfValueObject::Min, "", ce.c_min});
        if (ce.three) r.cfvo.push_back(CfValueObject{CfValueObject::Percentile, "50", ce.c_mid});
        r.cfvo.push_back(CfValueObject{CfValueObject::Max, "", ce.c_max});
    } else if (r.type == CondRule::DataBar) {
        r.cfvo = {CfValueObject{CfValueObject::Min, "", 0}, CfValueObject{CfValueObject::Max, "", 0}};
        r.bar_color = ce.bar;
    } else {
        Dxf x;
        if (ce.fill) x.fill_color = ce.fill;
        if (ce.font) x.font_color = ce.font;
        if (ce.bold) x.bold = true;
        if (ce.italic) x.italic = true;
        d.b.dxfs.push_back(x);
        r.dxf = (int)d.b.dxfs.size() - 1;
    }
    auto list = d.ws().cond;
    if (ce.cf >= 0 && ce.cf < (int)list.size()) {
        r.priority = list[ce.cf].rules[ce.rule].priority;
        list[ce.cf].rules[ce.rule] = r;
        list[ce.cf].ranges = ranges;
    } else {
        int maxp = 0;
        for (auto& cf : list) for (auto& x : cf.rules) maxp = std::max(maxp, x.priority);
        r.priority = maxp + 1;
        CondFormat cf; cf.ranges = ranges; cf.rules.push_back(r);
        list.push_back(cf);
    }
    d.b.set_cond_formats(d.cur, list);
    return true;
}

void SheetDoc::dlg_condfmt() {
    ImGui::SetNextWindowSize(ImVec2(S(720), S(480)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(dlg_name(Dlg::CondFmt), nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    if (ImGui::IsWindowAppearing()) { ce = CfEdit{}; if (ws().cond.empty()) cf_load(*this, -1, -1); }
    const Worksheet& W = ws();
    float body_h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() - S(14);
    ImGui::BeginChild("##cfb", ImVec2(0, body_h));
    if (!ce.open) {
        if (button("New Rule\xE2\x80\xA6", ui::Icon::Plus, true)) cf_load(*this, -1, -1);
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(th().text_dim), "Rules for sheet \"%s\" (lower priority number applies first)", W.name.c_str());
        // flatten
        struct It { int cf, r, prio; };
        std::vector<It> items;
        for (int i = 0; i < (int)W.cond.size(); ++i) for (int k = 0; k < (int)W.cond[i].rules.size(); ++k) items.push_back({i, k, W.cond[i].rules[k].priority});
        std::sort(items.begin(), items.end(), [](const It& a, const It& b2) { return a.prio < b2.prio; });
        int del_cf = -1, del_r = -1, swap_a = -1, swap_b = -1;
        if (ImGui::BeginTable("##cft", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, -1))) {
            ImGui::TableSetupColumn("Rule (applied in the order shown)", 0, 3.f);
            ImGui::TableSetupColumn("Format", 0, 1.2f);
            ImGui::TableSetupColumn("Applies to", 0, 1.5f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(80));
            ImGui::TableHeadersRow();
            for (int i = 0; i < (int)items.size(); ++i) {
                const CondRule& r = W.cond[items[i].cf].rules[items[i].r];
                ImGui::TableNextRow();
                ImGui::PushID(i);
                ImGui::TableNextColumn();
                if (ImGui::Selectable(rule_summary(b, r).c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(0)) cf_load(*this, items[i].cf, items[i].r);
                ImGui::TableNextColumn();
                ImVec2 p = ImGui::GetCursorScreenPos();
                ImVec2 sz(S(90), ImGui::GetTextLineHeight());
                ImDrawList* dl = ImGui::GetWindowDrawList();
                if (r.type == CondRule::ColorScale) {
                    for (size_t k = 0; k + 1 < r.cfvo.size(); ++k) {
                        float x0 = p.x + sz.x * k / (r.cfvo.size() - 1), x1 = p.x + sz.x * (k + 1) / (r.cfvo.size() - 1);
                        dl->AddRectFilledMultiColor(ImVec2(x0, p.y), ImVec2(x1, p.y + sz.y), doc_col(r.cfvo[k].color), doc_col(r.cfvo[k + 1].color), doc_col(r.cfvo[k + 1].color), doc_col(r.cfvo[k].color));
                    }
                } else if (r.type == CondRule::DataBar) {
                    dl->AddRectFilledMultiColor(p, ImVec2(p.x + sz.x * 0.7f, p.y + sz.y), doc_col(r.bar_color), col_alpha(doc_col(r.bar_color), 0.2f), col_alpha(doc_col(r.bar_color), 0.2f), doc_col(r.bar_color));
                } else if (r.dxf >= 0 && r.dxf < (int)b.dxfs.size()) {
                    const Dxf& x = b.dxfs[r.dxf];
                    dl->AddRectFilled(p, p + sz, x.fill_color ? doc_col(*x.fill_color) : th().field);
                    dl->AddText(ImVec2(p.x + S(6), p.y), x.font_color ? doc_col(*x.font_color) : th().text, "AaBbCc");
                }
                ImGui::Dummy(sz);
                ImGui::TableNextColumn();
                std::string rs;
                for (auto& g : W.cond[items[i].cf].ranges) { if (!rs.empty()) rs += " "; rs += range_name(g); }
                ImGui::TextUnformatted(rs.c_str());
                ImGui::TableNextColumn();
                if (icon_button("ed", ui::Icon::Settings, "Edit rule", false, true, S(20))) cf_load(*this, items[i].cf, items[i].r);
                ImGui::SameLine(0, S(2));
                if (icon_button("up", ui::Icon::ArrowUp, "Move up (higher priority)", false, i > 0, S(20))) { swap_a = i; swap_b = i - 1; }
                ImGui::SameLine(0, S(2));
                if (icon_button("dl", ui::Icon::Delete, "Delete rule", false, true, S(20))) { del_cf = items[i].cf; del_r = items[i].r; }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (items.empty()) ImGui::TextDisabled("No rules yet.");
        if (del_cf >= 0) {
            auto list = W.cond;
            list[del_cf].rules.erase(list[del_cf].rules.begin() + del_r);
            if (list[del_cf].rules.empty()) list.erase(list.begin() + del_cf);
            b.set_cond_formats(cur, list);
        }
        if (swap_a >= 0) {
            auto list = W.cond;
            std::swap(list[items[swap_a].cf].rules[items[swap_a].r].priority, list[items[swap_b].cf].rules[items[swap_b].r].priority);
            b.set_cond_formats(cur, list);
        }
    } else {
        // ---- rule editor
        section_header(ce.cf >= 0 ? "Edit formatting rule" : "New formatting rule");
        label("Applies to"); ImGui::SameLine(S(120)); ImGui::SetNextItemWidth(S(300));
        ImGui::InputText("##rng", ce.range, sizeof ce.range);
        label("Rule type"); ImGui::SameLine(S(120)); ImGui::SetNextItemWidth(S(300));
        ImGui::Combo("##ty", &ce.type, k_cf_types, 16);
        CondRule::Type ty = k_cf_map[ce.type];
        ImGui::Spacing();
        switch (ty) {
        case CondRule::CellIs:
            label("Cell value is"); ImGui::SameLine(S(120)); ImGui::SetNextItemWidth(S(190));
            ImGui::Combo("##op", &ce.op, k_ops, 8);
            ImGui::SameLine(); ImGui::SetNextItemWidth(S(140)); ImGui::InputTextWithHint("##f1", "value or =formula", ce.f1, sizeof ce.f1);
            if (ce.op <= 1) { ImGui::SameLine(); ImGui::TextUnformatted("and"); ImGui::SameLine(); ImGui::SetNextItemWidth(S(140)); ImGui::InputText("##f2", ce.f2, sizeof ce.f2); }
            break;
        case CondRule::Expression:
            label("Formula"); ImGui::SameLine(S(120)); ImGui::SetNextItemWidth(S(400));
            ImGui::InputTextWithHint("##f1", "e.g. =REFCOUNT($B2)<>$C2   (relative to the top-left cell)", ce.f1, sizeof ce.f1);
            break;
        case CondRule::ContainsText: case CondRule::NotContainsText: case CondRule::BeginsWith: case CondRule::EndsWith:
            label("Text"); ImGui::SameLine(S(120)); ImGui::SetNextItemWidth(S(300)); ImGui::InputText("##tx", ce.text, sizeof ce.text);
            break;
        case CondRule::Top10:
            ImGui::Checkbox("Bottom", &ce.bottom); ImGui::SameLine(); ImGui::SetNextItemWidth(S(100)); ImGui::InputInt("##rank", &ce.rank); ImGui::SameLine(); ImGui::Checkbox("% of the range", &ce.percent);
            ce.rank = std::clamp(ce.rank, 1, 1000);
            break;
        case CondRule::AboveAverage: ImGui::Checkbox("Below average", &ce.below); break;
        case CondRule::ColorScale:
            ImGui::Checkbox("3-colour scale", &ce.three);
            label("Minimum"); ImGui::SameLine(S(120)); color_button("cmin", ce.c_min, "-");
            if (ce.three) { label("Midpoint (50th percentile)"); ImGui::SameLine(S(200)); color_button("cmid", ce.c_mid, "-"); }
            label("Maximum"); ImGui::SameLine(S(120)); color_button("cmax", ce.c_max, "-");
            break;
        case CondRule::DataBar:
            label("Bar colour"); ImGui::SameLine(S(120)); color_button("bar", ce.bar, "-");
            break;
        default: break;
        }
        if (ty != CondRule::ColorScale && ty != CondRule::DataBar) {
            section_header("Format");
            label("Fill"); ImGui::SameLine(S(120)); color_button("cfill", ce.fill, "No Fill");
            label("Font colour"); ImGui::SameLine(S(120)); color_button("cfont", ce.font, "Automatic");
            ImGui::Checkbox("Bold", &ce.bold); ImGui::SameLine(); ImGui::Checkbox("Italic", &ce.italic);
            ImGui::SameLine(S(260));
            static const struct { const char* n; uint32_t f, t; } pre[] = {{"Light red", 0xFFFFC7CE, 0xFF9C0006}, {"Yellow", 0xFFFFEB9C, 0xFF9C5700}, {"Green", 0xFFC6EFCE, 0xFF006100}, {"Red text", 0, 0xFFFF0000}};
            for (int i = 0; i < 4; ++i) { ImGui::SameLine(); ImGui::PushID(i); if (ImGui::SmallButton(pre[i].n)) { ce.fill = pre[i].f; ce.font = pre[i].t; } ImGui::PopID(); }
            ImGui::Checkbox("Stop if true", &ce.stop);
        }
        ImGui::Spacing();
        if (button("Save rule", ui::Icon::Check, true)) { if (cf_store(*this)) { bool had = !ws().cond.empty(); ce.open = false; (void)had; } }
        ImGui::SameLine();
        if (button("Back to rules", ui::Icon::Undo)) ce.open = false;
    }
    ImGui::EndChild();
    ImGui::Separator();
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - S(100));
    if (button("Close", ui::Icon::None, false, ImVec2(S(96), 0)) || (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::GetIO().WantTextInput)) {
        if (ce.open && ce.cf < 0 && W.cond.empty() && ce.f1[0]) cf_store(*this);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ===========================================================================
// Data validation
// ===========================================================================
void SheetDoc::dlg_validation() {
    static DataValidation dv;
    static char f1[256], f2[128], pt[64], pm[256], et[64], em[256];
    static int tab = 0;
    ImGui::SetNextWindowSize(ImVec2(S(560), S(420)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(dlg_name(Dlg::Validation), nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    const Worksheet& W = ws();
    if (ImGui::IsWindowAppearing()) {
        dv = DataValidation{};
        for (auto& x : W.validations) for (auto& g : x.ranges) if (g.contains(ar(), ac())) dv = x;
        std::snprintf(f1, sizeof f1, "%s", dv.f1.c_str()); std::snprintf(f2, sizeof f2, "%s", dv.f2.c_str());
        if (dv.type == DataValidation::List && dv.f1.size() >= 2 && dv.f1.front() == '"') std::snprintf(f1, sizeof f1, "%s", dv.f1.substr(1, dv.f1.size() - 2).c_str());
        std::snprintf(pt, sizeof pt, "%s", dv.prompt_title.c_str()); std::snprintf(pm, sizeof pm, "%s", dv.prompt.c_str());
        std::snprintf(et, sizeof et, "%s", dv.error_title.c_str()); std::snprintf(em, sizeof em, "%s", dv.error.c_str());
        tab = 0;
    }
    if (ImGui::BeginTabBar("##dvt")) {
        if (ImGui::BeginTabItem("Settings")) { tab = 0; ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Input Message")) { tab = 1; ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Error Alert")) { tab = 2; ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    float body_h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() - S(14);
    ImGui::BeginChild("##dvb", ImVec2(0, body_h));
    static const char* allow[] = {"Any value", "Whole number", "Decimal", "List", "Date", "Time", "Text length", "Custom"};
    if (tab == 0) {
        label("Allow"); ImGui::SameLine(S(110)); ImGui::SetNextItemWidth(S(200));
        int ty = dv.type; if (ImGui::Combo("##allow", &ty, allow, 8)) dv.type = (DataValidation::Type)ty;
        ImGui::SameLine(); ImGui::Checkbox("Ignore blank", &dv.allow_blank);
        if (dv.type == DataValidation::List) {
            ImGui::Checkbox("In-cell dropdown", &dv.dropdown);
            label("Source"); ImGui::SameLine(S(110)); ImGui::SetNextItemWidth(S(380));
            ImGui::InputTextWithHint("##src", "Yes,No,DNP   or   =$H$2:$H$20", f1, sizeof f1);
        } else if (dv.type == DataValidation::Custom) {
            label("Formula"); ImGui::SameLine(S(110)); ImGui::SetNextItemWidth(S(380));
            ImGui::InputTextWithHint("##cf", "=LEN(A2)<=20", f1, sizeof f1);
        } else if (dv.type != DataValidation::Any) {
            label("Data"); ImGui::SameLine(S(110)); ImGui::SetNextItemWidth(S(200));
            int op = (int)dv.op; if (ImGui::Combo("##op", &op, k_ops, 8)) dv.op = (CmpOp)op;
            label(dv.op <= CmpOp::NotBetween ? "Minimum" : "Value"); ImGui::SameLine(S(110)); ImGui::SetNextItemWidth(S(200)); ImGui::InputText("##f1", f1, sizeof f1);
            if (dv.op <= CmpOp::NotBetween) { label("Maximum"); ImGui::SameLine(S(110)); ImGui::SetNextItemWidth(S(200)); ImGui::InputText("##f2", f2, sizeof f2); }
        }
    } else if (tab == 1) {
        ImGui::Checkbox("Show input message when cell is selected", &dv.show_input);
        label("Title"); ImGui::SetNextItemWidth(-1); ImGui::InputText("##pt", pt, sizeof pt);
        label("Input message"); ImGui::InputTextMultiline("##pm", pm, sizeof pm, ImVec2(-1, S(120)));
    } else {
        ImGui::Checkbox("Show error alert after invalid data is entered", &dv.show_error);
        static const char* es[] = {"Stop", "Warning", "Information"};
        label("Style"); ImGui::SameLine(S(110)); ImGui::SetNextItemWidth(S(160));
        int e = dv.err_style; if (ImGui::Combo("##es", &e, es, 3)) dv.err_style = (DataValidation::ErrStyle)e;
        label("Title"); ImGui::SetNextItemWidth(-1); ImGui::InputText("##et", et, sizeof et);
        label("Error message"); ImGui::InputTextMultiline("##em", em, sizeof em, ImVec2(-1, S(120)));
    }
    ImGui::EndChild();
    ImGui::Separator();
    if (button("Clear All", ui::Icon::Eraser)) {
        auto list = W.validations;
        Range g = prim();
        for (auto& x : list) x.ranges.erase(std::remove_if(x.ranges.begin(), x.ranges.end(), [&](const Range& r2) { return r2.intersects(g); }), x.ranges.end());
        list.erase(std::remove_if(list.begin(), list.end(), [](const DataValidation& x) { return x.ranges.empty(); }), list.end());
        b.set_validations(cur, list);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (footer()) {
        DataValidation n = dv;
        n.f1 = f1; n.f2 = f2;
        if (!n.f1.empty() && n.f1[0] == '=') n.f1.erase(0, 1);
        if (!n.f2.empty() && n.f2[0] == '=') n.f2.erase(0, 1);
        if (n.type == DataValidation::List && !n.f1.empty() && std::string(f1)[0] != '=') n.f1 = "\"" + std::string(f1) + "\"";
        n.prompt_title = pt; n.prompt = pm; n.error_title = et; n.error = em;
        n.ranges.clear();
        each_sel([&](const Range& g) { n.ranges.push_back(g); });
        auto list = W.validations;
        for (auto& x : list) x.ranges.erase(std::remove_if(x.ranges.begin(), x.ranges.end(), [&](const Range& r2) { for (auto& g : n.ranges) if (g.contains(r2.r0, r2.c0) && g.contains(r2.r1, r2.c1)) return true; return false; }), x.ranges.end());
        list.erase(std::remove_if(list.begin(), list.end(), [](const DataValidation& x) { return x.ranges.empty(); }), list.end());
        if (n.type != DataValidation::Any) list.push_back(n);
        b.set_validations(cur, list);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ===========================================================================
// Paste Special
// ===========================================================================
void SheetDoc::dlg_paste_special() {
    static int what = 0;
    static bool skip = false, transpose = false;
    if (!ImGui::BeginPopupModal(dlg_name(Dlg::PasteSpecial), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
    section_header("Paste");
    static const char* opts[] = {"All", "Formulas", "Values", "Formats", "Values and number formats", "All except borders"};
    for (int i = 0; i < 4; ++i) ImGui::RadioButton(opts[i], &what, i);
    ImGui::Spacing();
    ImGui::Checkbox("Skip blanks", &skip); ImGui::SameLine(S(160)); ImGui::Checkbox("Transpose", &transpose);
    ImGui::Dummy(ImVec2(S(320), 0));
    if (footer()) {
        unsigned w = what == 0 ? PasteAll : what == 1 ? PasteFormulas : what == 2 ? PasteValues : PasteFormats;
        if (skip) w |= PasteSkipBlanks;
        if (transpose) w |= PasteTranspose;
        paste(w, true);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ===========================================================================
// Text to Columns
// ===========================================================================
static std::vector<std::string> split_text(const std::string& s, const std::string& delims, bool merge, char qual) {
    std::vector<std::string> out;
    std::string curs;
    bool inq = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char ch = s[i];
        if (qual && ch == qual) { if (inq && i + 1 < s.size() && s[i + 1] == qual) { curs += qual; ++i; continue; } inq = !inq; continue; }
        if (!inq && delims.find(ch) != std::string::npos) {
            if (merge && curs.empty() && !out.empty() && delims.find(s[i - 1]) != std::string::npos) continue;
            out.push_back(curs); curs.clear(); continue;
        }
        curs += ch;
    }
    out.push_back(curs);
    return out;
}

void SheetDoc::dlg_text_to_cols() {
    static bool tab = true, semi = false, comma = true, space = false, other = false, merge = false, insert = true;
    static char oth[8] = "|";
    static int qual = 0;
    ImGui::SetNextWindowSize(ImVec2(S(620), S(440)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(dlg_name(Dlg::TextToCols), nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    int col = ac();
    const Worksheet& W = ws();
    Range g = prim();
    int r0 = g.r0, r1 = std::min(g.r1, W.nrows() - 1);
    if (g.rows() == 1) { r0 = 0; r1 = W.nrows() - 1; }
    ImGui::Text("Split column %s (rows %d-%d)", col_name(col).c_str(), r0 + 1, r1 + 1);
    section_header("Delimiters");
    ImGui::Checkbox("Tab", &tab); ImGui::SameLine(); ImGui::Checkbox("Semicolon", &semi); ImGui::SameLine(); ImGui::Checkbox("Comma", &comma); ImGui::SameLine();
    ImGui::Checkbox("Space", &space); ImGui::SameLine(); ImGui::Checkbox("Other:", &other); ImGui::SameLine(); ImGui::SetNextItemWidth(S(40)); ImGui::InputText("##oth", oth, sizeof oth);
    ImGui::Checkbox("Treat consecutive delimiters as one", &merge);
    ImGui::SameLine(S(300)); label("Text qualifier"); ImGui::SameLine(); ImGui::SetNextItemWidth(S(80));
    static const char* qs[] = {"\"", "'", "{none}"};
    ImGui::Combo("##q", &qual, qs, 3);
    ImGui::Checkbox("Insert new columns (don't overwrite the columns to the right)", &insert);
    std::string d;
    if (tab) d += '\t';
    if (semi) d += ';';
    if (comma) d += ',';
    if (space) d += ' ';
    if (other) d += oth;
    char qc = qual == 0 ? '"' : qual == 1 ? '\'' : 0;
    section_header("Preview");
    if (ImGui::BeginTable("##pv", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX, ImVec2(0, S(180)))) {
        for (int r = r0; r <= std::min(r1, r0 + 9); ++r) {
            auto parts = split_text(b.input_text(cur, r, col), d, merge, qc);
            ImGui::TableNextRow();
            for (int k = 0; k < (int)parts.size() && k < 8; ++k) { ImGui::TableSetColumnIndex(k); ImGui::TextUnformatted(parts[k].c_str()); }
        }
        ImGui::EndTable();
    }
    if (footer("Finish", !d.empty())) {
        commit_edit();
        std::vector<std::vector<std::string>> blk;
        size_t maxp = 1;
        for (int r = r0; r <= r1; ++r) { blk.push_back(split_text(b.input_text(cur, r, col), d, merge, qc)); maxp = std::max(maxp, blk.back().size()); }
        for (auto& row : blk) row.resize(maxp);
        Book::Batch bt(b, "Text to Columns");
        if (insert && maxp > 1) b.insert_cols(cur, col + 1, (int)maxp - 1);
        b.set_inputs(cur, r0, col, blk);
        toast("Split into " + std::to_string(maxp) + " columns", ToastKind::Success, 2.5f);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ===========================================================================
// Row height / column width
// ===========================================================================
void SheetDoc::dlg_size(bool rows) {
    static double v = 15;
    Dlg me = rows ? Dlg::RowHeight : Dlg::ColWidth;
    if (!ImGui::BeginPopupModal(dlg_name(me), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
    if (ImGui::IsWindowAppearing()) v = rows ? ws().row_height(ar()) : ws().col_width(ac());
    label(rows ? "Row height (points)" : "Column width (characters)");
    ImGui::SetNextItemWidth(S(160));
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputDouble("##v", &v, rows ? 1.0 : 0.5, 5.0, "%.2f");
    v = std::clamp(v, 0.0, rows ? 409.0 : 255.0);
    ImGui::Dummy(ImVec2(S(260), 0));
    if (footer()) {
        Range g = prim();
        if (rows) b.set_row_height(cur, g.r0, std::min(g.r1, std::max(ws().nrows(), g.r0 + 1) + 1000), v);
        else b.set_col_width(cur, g.c0, std::min(g.c1, ws().ncols() + 100), v);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ===========================================================================
// Remove duplicates
// ===========================================================================
void SheetDoc::dlg_remove_dup() {
    static Range rg;
    static bool header = true;
    static std::vector<char> use;
    ImGui::SetNextWindowSize(ImVec2(S(460), S(420)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(dlg_name(Dlg::RemoveDup), nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    const Worksheet& W = ws();
    if (ImGui::IsWindowAppearing()) {
        Range g = prim();
        rg = (g.rows() > 1) ? Range(g.r0, g.c0, std::min(g.r1, W.nrows() - 1), std::min(g.c1, W.ncols() - 1)) : data_region(g.r0, g.c0);
        header = W.freeze_rows > 0 || W.autofilter.active() || b.style_of(cur, rg.r0, rg.c0).font.bold;
        use.assign(rg.cols(), 1);
    }
    ImGui::TextWrapped("Rows of %s whose checked columns repeat an earlier row are deleted.", range_name(rg).c_str());
    ImGui::Checkbox("My data has headers", &header);
    if (button("Select All")) std::fill(use.begin(), use.end(), 1);
    ImGui::SameLine();
    if (button("Unselect All")) std::fill(use.begin(), use.end(), 0);
    ImGui::BeginChild("##cols", ImVec2(0, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() - S(14)), ImGuiChildFlags_Borders);
    for (int i = 0; i < rg.cols(); ++i) {
        std::string l = header ? b.display_text(cur, rg.r0, rg.c0 + i) : "";
        l = "Column " + col_name(rg.c0 + i) + (l.empty() ? "" : "  " + l);
        bool v = use[i];
        if (ImGui::Checkbox((l + "##" + std::to_string(i)).c_str(), &v)) use[i] = v;
    }
    ImGui::EndChild();
    bool any = false;
    for (char x : use) any |= x != 0;
    if (footer("Remove", any)) {
        commit_edit();
        std::set<std::string> seen;
        std::vector<int> dels;
        for (int r = rg.r0 + (header ? 1 : 0); r <= rg.r1; ++r) {
            std::string key;
            for (int i = 0; i < rg.cols(); ++i) if (use[i]) { key += b.display_text(cur, r, rg.c0 + i); key += '\x1f'; }
            if (!seen.insert(key).second) dels.push_back(r);
        }
        {
            Book::Batch bt(b, "Remove duplicates");
            // delete contiguous runs bottom-up
            for (int i = (int)dels.size() - 1; i >= 0;) {
                int e = dels[i], s = e;
                while (i > 0 && dels[i - 1] == s - 1) { --i; --s; }
                b.delete_rows(cur, s, e - s + 1);
                --i;
            }
        }
        int kept = rg.rows() - (header ? 1 : 0) - (int)dels.size();
        toast(std::to_string(dels.size()) + " duplicate rows removed; " + std::to_string(kept) + " unique rows remain.", dels.empty() ? ToastKind::Info : ToastKind::Success, 4.f);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ===========================================================================
void SheetDoc::draw_dialogs() {
    if (dlg == Dlg::None) return;
    if (dlg == Dlg::Find) { dlg_find(); return; }
    if (dlg_open_req) { commit_edit(); if (dlg == Dlg::FormatCells) init_format_cells(*this); ImGui::OpenPopup(dlg_name(dlg)); dlg_open_req = false; }
    switch (dlg) {
    case Dlg::FormatCells: dlg_format_cells(); break;
    case Dlg::GoTo: dlg_goto(); break;
    case Dlg::Sort: dlg_sort(); break;
    case Dlg::CondFmt: dlg_condfmt(); break;
    case Dlg::Validation: dlg_validation(); break;
    case Dlg::PasteSpecial: dlg_paste_special(); break;
    case Dlg::TextToCols: dlg_text_to_cols(); break;
    case Dlg::RowHeight: dlg_size(true); break;
    case Dlg::ColWidth: dlg_size(false); break;
    case Dlg::RemoveDup: dlg_remove_dup(); break;
    case Dlg::ApplyBom: dlg_apply_bom(); break;
    default: break;
    }
    if (!ImGui::IsPopupOpen(dlg_name(dlg))) { dlg = Dlg::None; focus_grid = true; }
}

} // namespace pcbp::gui
