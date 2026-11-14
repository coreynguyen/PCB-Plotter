#pragma once
// ============================================================================
// sheet_ui.h — private widgets for the spreadsheet editor (sheet_*.cpp):
// vector glyphs for the ribbon (drawn with ImDrawList, theme coloured),
// ribbon buttons / split buttons / colour palettes, and formula text helpers
// (reference tokenizer for highlight colours + function catalogue).
// ============================================================================
#include "gapp.h"
#include <string>
#include <vector>

namespace pcbp::gui::sg {

enum class G : int {
    None, Bold, Italic, Underline, Strike, FontColor, FillColor, Borders,
    AlignL, AlignC, AlignR, VTop, VMid, VBot, Wrap, Merge,
    Percent, Thousands, DecInc, DecDec, Currency,
    AutoSum, FillDown, Clear, SortFilter, Funnel, FindSel,
    CondFmt, CellStyles, InsRow, DelRow, InsCol, DelCol, RowHeight, ColWidth, Hide,
    TextToCols, Dedupe, Validation, Freeze, Gridlines, ZoomG,
    Refdes, RefCount, QtyCheck, SplitDnp, Send, Sheet, Fx, Indent, Outdent,
    // border styles (menu)
    BAll, BOutside, BThickOutside, BTop, BBottom, BLeft, BRight, BNone, BInside,
    Count
};

// Draw glyph g into the square [p, p+s]. col: stroke; accent: colour bar (FontColor/FillColor) or 0.
void glyph(ImDrawList* dl, G g, ImVec2 p, float s, ImU32 col, ImU32 accent = 0);

// Ribbon square button with glyph (+ animated hover like icon_button). Returns clicked.
bool gbtn(const char* id, G g, const char* tip, bool checked = false, bool enabled = true, ImU32 accent = 0, float size = 0);
// Same, but draws a ui::Icon.
bool ibtn(const char* id, ui::Icon ic, const char* tip, bool checked = false, bool enabled = true, float size = 0);
// Split button: main part + narrow arrow. Returns 1 main clicked, 2 arrow clicked, 0 none.
int gsplit(const char* id, G g, const char* tip, ImU32 accent = 0, bool enabled = true);
// Large labelled ribbon button (icon over text). Returns clicked.
bool big_btn(const char* id, ui::Icon ic, G g, const char* label, const char* tip, bool primary = false, bool enabled = true, bool arrow = false);

// Menu item with a glyph in the icon column.
bool gitem(const char* label, G g, const char* shortcut = nullptr, bool enabled = true, bool checked = false);

// Ribbon group layout: begin_group draws nothing; end_group draws the caption under the
// group and a separator. Groups are laid out horizontally by the caller (SameLine).
void begin_group(const char* caption);
void end_group();

// Colour palette popup content (theme-independent document colours, Office-like 10x6 grid +
// standard row + "No colour" + custom picker). Returns true when a colour was chosen.
// out = 0xAARRGGBB document colour; 0 = automatic / none.
bool color_palette(uint32_t& out, const char* none_label);
// Recently used custom colours (shared by font + fill palettes)
void remember_color(uint32_t argb);

// Document ARGB (0xAARRGGBB) <-> ImU32 (ABGR)
inline ImU32 argb_to_u32(uint32_t c) {
    return (c & 0xFF00FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu);
}
inline uint32_t u32_to_argb(ImU32 c) { return argb_to_u32(c); }   // symmetric swap

// ---------------------------------------------------------------------------
// Formula text helpers
// ---------------------------------------------------------------------------
ImU32 ref_color(int i);       // Excel-like highlight cycle (blue, red, purple, green, ...)
std::string a1(int r, int c, bool abs_r = false, bool abs_c = false);
std::string a1_range(int r0, int c0, int r1, int c1);
// Does the caret position allow inserting a reference (point mode): after = ( , operator : etc.
bool ref_insert_point(const char* s, int caret);
// Identifier being typed at caret (function name prefix) — returns start or -1.
int ident_at(const char* s, int caret, std::string& out);
// Innermost open function call at caret: name + 0-based argument index. false when none.
bool call_at(const char* s, int caret, std::string& fn, int& arg);


} // namespace pcbp::gui::sg
