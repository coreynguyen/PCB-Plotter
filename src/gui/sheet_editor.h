#pragma once
// ============================================================================
// sheet_editor.h — private declarations of the spreadsheet editor (Files
// workspace). Split over:
//   sheet_editor.cpp   document core: load/save, clipboard, commands, Files workspace
//   sheet_grid.cpp     grid painter, mouse, keyboard, in-cell editor, autofilter popup, context menus
//   sheet_ribbon.cpp   ribbon tabs, name box + formula bar, sheet tabs, status bar
//   sheet_dialogs.cpp  Format Cells, Find & Replace, Go To, Sort, Conditional formatting,
//                      Data validation, Paste Special, Text to columns, Row height / Col width
//   sheet_bom.cpp      BOM tools, Send to Project, project BOM round trip
//   sheet_ui.cpp       glyphs, ribbon buttons, palettes, formula text helpers
// Engine: pcbp::sheet::Book (src/core/sheet.h, specs/SHEET_API.md).
// ============================================================================
#include "file_editors.h"
#include "sheet_ui.h"
#include "../core/sheet.h"
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pcbp::gui {

using sheet::Range;

// selection range with anchor (r0,c0) and moving end (r1,c1)
struct SelRng {
    int r0 = 0, c0 = 0, r1 = 0, c1 = 0;
    Range norm() const { return Range(std::min(r0, r1), std::min(c0, c1), std::max(r0, r1), std::max(c0, c1)); }
    bool operator==(const SelRng& o) const { return r0 == o.r0 && c0 == o.c0 && r1 == o.r1 && c1 == o.c1; }
};

struct SheetUi {                          // per-sheet view state
    std::vector<SelRng> sel{SelRng{}};
    int act_r = -1, act_c = -1;           // active cell moved inside the range by Enter/Tab
    SelRng act_for;                       // ...valid while the selection equals this
    float sx = 0, sy = 0;
    float tsx = 0, tsy = 0;               // smooth-scroll targets
    float lsx = -1, lsy = -1;             // last scroll we set (detect scrollbar drags)
    bool restore_scroll = false;
};

// layout cache (rebuilt when book gen / zoom / sheet changes)
struct Layout {
    uint64_t gen = ~0ull;
    int si = -1;
    float z = 0;
    int nr = 0, nc = 0;                   // displayed grid size
    std::vector<int> rows;                // visible (non hidden) rows, ascending, including frozen ones
    std::vector<float> ry;                // ry[i] = top of rows[i] (content px, from row 0), size rows+1
    std::vector<float> cx;                // cx[c] = left of column c (content px), size nc+1 (hidden cols width 0)
    int fr = 0, fc = 0;                   // frozen rows / cols (sheet indices)
    int fri = 0;                          // number of entries of rows[] that are frozen
    float fh = 0, fw = 0;                 // frozen pane height / width (px)
};

enum class GDrag { None, Cells, Rows, Cols, ColResize, RowResize, Fill, Move, Point };

enum class Dlg { None, FormatCells, Find, GoTo, Sort, CondFmt, Validation, PasteSpecial, TextToCols, RowHeight, ColWidth,
                 ApplyBom, RemoveDup, Hyper };

class SheetDoc final : public FileDoc {
public:
    sheet::Book b;
    std::vector<SheetUi> ui_;
    int cur = 0;
    bool dirty_ = false;
    uint64_t saved_gen = 0;
    bool readonly_src = false;            // .xls: Save forces Save As
    std::string how;                      // load description
    std::string msg;                      // last op result (status)
    Layout lay;
    // project link (Open project BOM in spreadsheet)
    std::weak_ptr<app::Doc> link_doc;
    bool show_link_banner = true;

    // ---- ribbon / view
    int ribbon_tab = 0;                   // 0 Home 1 Data 2 BOM 3 View
    bool ribbon_min = true;     // REVIEW_V9 §10: ribbon collapsed by default (tab click / Ctrl+F1 expands)
    bool show_headers = true, show_formula_bar = true;
    uint32_t last_font_color = 0xFFC00000, last_fill_color = 0xFFFFFF00;
    int last_border = 0;                  // sg::G border style index for split button

    // ---- editing
    bool editing = false;
    bool edit_in_bar = false;             // formula bar has the caret
    int er = 0, ec = 0, esi = 0;
    std::vector<char> ebuf = std::vector<char>(1 << 16);
    bool edit_focus = false;              // focus the active editor next frame
    bool edit_select_all = false;
    int caret = 0;                        // caret in ebuf (bytes), updated by the input callback
    int want_caret = -1;                  // move the caret next frame
    int pt_b = -1, pt_e = -1;             // point-mode inserted ref span in ebuf (replace while dragging)
    bool point_mode = false;              // last click inserted a ref; next click replaces it
    int ac_sel = 0;                       // autocomplete selection
    int ac_start = -1;                    // ident start (autocomplete active when >= 0)
    std::string ac_prefix;
    bool ac_accept = false;               // Tab/Enter requested acceptance (set in callback)
    int ac_move = 0;                      // Up/Down from callback
    bool tab_commit = false;              // Tab pressed in the editor (callback)
    bool edit_dirty = false;              // user changed the text since begin_edit
    bool deact_pending = false;           // editor lost focus this frame (resolved after grid mouse handling)
    bool point_clicked = false;           // this frame's grid click inserted a reference
    ImGuiID bar_id = 0, cell_id = 0;      // InputText ids of the formula bar / in-cell editor
    bool ac_visible = false;
    std::vector<const sheet::FunctionInfo*> ac_items;

    // ---- mouse drag
    GDrag drag = GDrag::None;
    int drag_idx = -1;                    // column/row being resized
    float drag_x0 = 0, drag_v0 = 0;
    Range fill_src, fill_dst;
    Range move_src; int move_dr = 0, move_dc = 0, move_r0 = 0, move_c0 = 0;
    int pt_r0 = 0, pt_c0 = 0;            // point-mode anchor

    // ---- clipboard
    std::unique_ptr<sheet::ClipBlock> clip;
    Range clip_rg; int clip_si = -1; bool clip_cut = false;
    unsigned clip_seq = 0;                // GetClipboardSequenceNumber after our copy

    // ---- dialogs
    Dlg dlg = Dlg::None;
    bool dlg_open_req = false;
    int fmt_tab = 0;                      // Format Cells tab
    // find
    char find_buf[256] = {}, repl_buf[256] = {};
    bool match_case = false, whole_cell = false, find_formulas = false, find_all_sheets = false;
    bool show_replace = false;
    std::vector<std::pair<int, std::pair<int, int>>> find_hits;   // si, (r,c)
    // filter popup
    int af_col = -1;                      // autofilter dropdown open for this sheet column
    // rename tab
    int rename_sheet = -1;
    char rename_buf[64] = {};
    int tab_drag = -1;
    // status cache
    uint64_t stat_key = 0; std::string stat_text;
    // look cache
    uint64_t look_gen = ~0ull; int look_si = -1;
    std::unordered_map<uint64_t, sheet::CellLook> looks;
    bool focus_grid = false;              // give the grid keyboard focus next frame
    bool fp_on = false;                   // format painter armed
    sheet::Style fp_style;
    char namebox[128] = {};
    bool namebox_active = false;

    // ---------------------------------------------------------------- FileDoc
    bool is_sheet() const override { return true; }
    bool dirty() const override { return dirty_ || b.gen != saved_gen; }
    void draw(ImVec2 size) override;
    bool save(bool save_as) override;
    bool on_command(int id) override;
    std::string status() const override;
    void import_as(bool bom) override;

    // ---------------------------------------------------------------- core (sheet_editor.cpp)
    sheet::Worksheet& ws() { return b.sheets[cur]; }
    SheetUi& su() { return ui_[cur]; }
    void sync_ui();                       // ui_ size == sheets
    Range prim() { return su().sel.back().norm(); }
    bool act_ok() { auto& u = su(); return u.act_r >= 0 && u.sel.size() == 1 && u.sel.back() == u.act_for; }
    int ar() { return act_ok() ? su().act_r : su().sel.back().r0; }     // active cell
    int ac() { return act_ok() ? su().act_c : su().sel.back().c0; }
    void select_cell(int r, int c, bool extend = false, bool add = false);
    void select_range(const Range& rg);
    void scroll_to(int r, int c);
    bool want_scroll = false;
    void ensure_layout(float z);
    float zoomf() const { return b.sheets[cur].zoom / 100.f; }
    float col_px(int c) const;            // width in px at current zoom (0 hidden)
    float row_px(int r) const;
    const sheet::CellLook& look(int r, int c);
    // edit
    void begin_edit(bool keep, const char* initial = nullptr, bool in_bar = false);
    bool commit_edit(int move_dr = 0, int move_dc = 0);   // false: validation stopped
    void cancel_edit();
    bool is_formula_edit() const { return editing && ebuf[0] == '='; }
    void insert_ref_text(const std::string& ref, bool replace_point);
    // clipboard
    void copy(bool cut);
    void paste(unsigned what = sheet::PasteAll, bool from_dialog = false);
    void clear_marquee() { clip_si = -1; }
    // ops
    void each_sel(const std::function<void(const Range&)>& fn);
    void style_sel(const char* label, const std::function<void(sheet::Style&)>& fn);
    void clear_sel(unsigned what, const char* label);
    void insert_rows(bool below); void delete_rows();
    void insert_cols(bool right); void delete_cols();
    void insert_cells_shift(bool down); void delete_cells_shift(bool up);
    void autofit_cols(int c0, int c1);
    void autofit_rows(int r0, int r1);
    void autosum();
    void fill_dir(int dir);               // 0 down 1 right 2 up 3 left
    void sort_quick(bool desc);
    void toggle_filter();
    void freeze_at_cursor();
    void select_all();
    void insert_date(bool time);
    void apply_cell_style(int preset);
    void bump_decimals(int d);
    void set_numfmt(const std::string& code);
    void merge_center();
    Range data_region(int r, int c);      // contiguous block around the cell (Ctrl+A / AutoSum)
    int edge_jump(int r, int c, int dr, int dc);   // Ctrl+arrow target (row or col index)
    bool find_next(bool back);
    int replace_all();
    void replace_one();
    void find_all();
    bool goto_ref(const std::string& txt);
    // files
    bool write_to(const std::wstring& p, std::string& err);

    // ---------------------------------------------------------------- grid (sheet_grid.cpp)
    void draw_grid(ImVec2 size);
    void handle_keys();
    void grid_context_menu();
    void row_header_menu();
    void col_header_menu();
    void filter_popup();
    void draw_cell_editor(ImVec2 pos, float w, float h);

    // ---------------------------------------------------------------- ribbon (sheet_ribbon.cpp)
    void draw_ribbon();
    void ribbon_home();
    void ribbon_data();
    void ribbon_bom();
    void ribbon_view();
    void send_button();
    void draw_formula_bar();
    void draw_sheet_tabs(float h);
    void draw_statusbar(float h);
    bool formula_input(const char* id, float w, float h, bool in_bar);
    void draw_autocomplete(ImVec2 anchor_bl);
    void borders_menu();
    void apply_border(sg::G kind);

    // ---------------------------------------------------------------- dialogs (sheet_dialogs.cpp)
    void open_dialog(Dlg d) { dlg = d; dlg_open_req = true; }
    void draw_dialogs();
    void dlg_format_cells();
    void dlg_find();
    void dlg_goto();
    void dlg_sort();
    void dlg_condfmt();
    void dlg_validation();
    void dlg_paste_special();
    void dlg_text_to_cols();
    void dlg_size(bool rows);
    void dlg_remove_dup();
    void dlg_apply_bom();

    // ---------------------------------------------------------------- BOM (sheet_bom.cpp)
    struct BomCols { int header = 0, ref = -1, qty = -1, pn = -1, desc = -1, dnp = -1, pkg = -1, value = -1, item = -1; };
    BomCols guess_bom_cols();
    std::string export_tsv(int& start_line);
    void send_to_project(int mode);       // 0 import BOM, 1 import centroid, 2 update BOM (merge)
    void bom_expand_refs(bool expand);
    void bom_expand_to_rows();
    void bom_count_refs();
    void bom_check_qty();
    void bom_split_dnp();
    void bom_dedupe_pn();
    void bom_apply_preview();             // computes the diff then opens Dlg::ApplyBom
    struct BomDiffRow { std::string kind, pn, what, before, after; bool on = true; };
    std::vector<BomDiffRow> bom_diff;
    struct BomApplyPlan;                  // sheet_bom.cpp
    std::shared_ptr<BomApplyPlan> bom_plan;
    void bom_apply_commit();
};

// shared helpers
std::unique_ptr<FileDoc> make_sheet_doc_book(sheet::Book bk, const std::string& name, const std::wstring& src_path);
float sheet_mdw();                        // max digit width (px at 100%, DPI-scaled)
ImU32 doc_col(uint32_t argb);             // document ARGB -> ImU32
std::string ext_lower(const std::wstring& p);
void files_close_current_doc();

} // namespace pcbp::gui
