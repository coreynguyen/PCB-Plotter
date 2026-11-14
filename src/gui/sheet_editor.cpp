// ============================================================================
// sheet_editor.cpp — "Editors" workspace (R21: was "Files"; the project Files tab is job_files_view.cpp) + spreadsheet document core.
//
//   * Editors workspace: document-independent page (shell "Editors" tabs) hosting
//     spreadsheet / text editor tabs (FileDoc, file_editors.h).
//   * SheetDoc on the sheet::Book engine (formulas, styles, number formats,
//     conditional formatting, validation, merges, freeze panes, autofilter,
//     undo). Load/save .xlsx / .xls (read) / .ods / .csv / .tsv / .txt / .prn.
//   * Clipboard: Excel-compatible TSV + "HTML Format" (styles), internal
//     block for formula-shifting paste.
//   * Grid: sheet_grid.cpp; ribbon / formula bar: sheet_ribbon.cpp; dialogs:
//     sheet_dialogs.cpp; BOM tools + project round trip: sheet_bom.cpp.
// ============================================================================
#include "sheet_editor.h"
#include "../app/app.h"
#include "../app/commands.h"
#include "../core/fileio.h"
#include "../core/project.h"
#include "../io/ods.h"
#include "../io/text_import.h"
#include "../io/xlsx.h"
#include "../ui/form.h"
#include "../ui/win.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

namespace pcbp::gui {
using namespace sheet;

// ===========================================================================
// shared helpers
// ===========================================================================
std::string wide_to_u8(const std::wstring& w) { return ui::U(w); }
std::wstring file_name_of(const std::wstring& p) {
    size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? p : p.substr(k + 1);
}
std::string ext_lower(const std::wstring& p) {
    std::string s = ui::U(file_name_of(p));
    size_t k = s.find_last_of('.');
    if (k == std::string::npos) return {};
    std::string e = s.substr(k + 1);
    for (auto& c : e) c = (char)tolower((unsigned char)c);
    return e;
}
bool fe_tool(const char* id, ui::Icon ic, const char* tip, bool enabled, bool checked) {
    ImGui::SameLine(0, S(2));
    return icon_button(id, ic, tip, checked, enabled, S(28));
}
float sheet_mdw() { return S(7.f); }
ImU32 doc_col(uint32_t argb) { if (argb && !(argb >> 24)) argb |= 0xFF000000u; return sg::argb_to_u32(argb); }   // files often carry alpha 00

// import wizard source override -------------------------------------------------
static std::string g_src_bytes;
static int g_src_start = -1;
static bool g_src_set = false;
static int g_mode_hint = -1;
void set_import_source(std::string bytes, int start_line) { g_src_bytes = std::move(bytes); g_src_start = start_line; g_src_set = true; }
bool take_import_source(std::string& bytes, int& start_line) {
    if (!g_src_set) return false;
    bytes = std::move(g_src_bytes); start_line = g_src_start;
    g_src_bytes.clear(); g_src_set = false;
    return true;
}
void set_import_mode_hint(int mode) { g_mode_hint = mode; }
int take_import_mode_hint() { int m = g_mode_hint; g_mode_hint = -1; return m; }

void import_from_bytes(std::string bytes, int start_line, bool bom, const std::wstring& display_path) {
    app::DocPtr d = app::active_doc();
    bool fresh = false;
    if (!d) { d = app::new_document(); fresh = true; }
    files_set_active(true);   // new_document() switched pages; the wizard is modal anyway
    set_import_source(std::move(bytes), start_line);
    bool ok = bom ? app::dlg_import_bom(app::frame_hwnd(), *d, display_path) : app::dlg_import_centroid(app::frame_hwnd(), *d, display_path, false);
    std::string dummy; int di;
    take_import_source(dummy, di);   // not consumed (dialog failed early)
    take_import_mode_hint();
    if (ok) {
        d->changed(ui::CH_ALL);
        files_set_active(false);
        activate_view(d, bom ? ViewKind::Bom : ViewKind::Placements);
    } else if (fresh && d->prj.comps.empty()) {
        app::close_document(d);
        files_set_active(true);
    } else files_set_active(true);
}

// ===========================================================================
// SheetDoc core
// ===========================================================================
void SheetDoc::sync_ui() {
    if (ui_.size() != b.sheets.size()) ui_.resize(b.sheets.size());
    if (cur >= (int)b.sheets.size()) cur = (int)b.sheets.size() - 1;
    if (cur < 0) cur = 0;
}

float SheetDoc::col_px(int c) const {
    const Worksheet& w = b.sheets[cur];
    if (w.col_hidden(c)) return 0;
    double ch = w.col_width(c);
    return std::floor((float)(ch * 7.0 + 5.0)) * dpi_scale() * zoomf();
}
float SheetDoc::row_px(int r) const {
    const Worksheet& w = b.sheets[cur];
    if (w.row_hidden(r)) return 0;
    return std::round((float)w.row_height(r) * 96.f / 72.f * dpi_scale() * zoomf());
}

void SheetDoc::ensure_layout(float z) {
    const Worksheet& w = ws();
    Range mx = prim();
    int nr = std::max({w.nrows() + 300, 1000, std::min(mx.r1, MAX_ROWS - 1) + 60});
    nr = std::min(nr, MAX_ROWS);
    int nc = std::max({w.ncols() + 8, 26, std::min(mx.c1, MAX_COLS - 1) + 4, w.freeze_cols + 4});
    nc = std::min(nc, MAX_COLS);
    if (lay.gen == b.gen && lay.si == cur && lay.z == z && lay.nr >= nr && lay.nc >= nc) return;
    lay.gen = b.gen; lay.si = cur; lay.z = z; lay.nr = nr; lay.nc = nc;
    lay.rows.clear(); lay.ry.clear();
    lay.rows.reserve(nr); lay.ry.reserve(nr + 1);
    float y = 0;
    const float defh = std::round((float)w.default_row_height * 96.f / 72.f * dpi_scale() * z);
    const int nstore = (int)w.rows.size();
    for (int r = 0; r < nr; ++r) {
        float h;
        if (r < nstore) {
            const RowStore& rs = w.rows[r];
            if (rs.is_hidden()) continue;
            h = rs.height >= 0 ? std::round(rs.height * 96.f / 72.f * dpi_scale() * z) : defh;
        } else h = defh;
        lay.rows.push_back(r); lay.ry.push_back(y); y += h;
    }
    lay.ry.push_back(y);
    lay.cx.assign(nc + 1, 0.f);
    for (int c = 0; c < nc; ++c) lay.cx[c + 1] = lay.cx[c] + col_px(c);
    lay.fr = std::min(w.freeze_rows, nr); lay.fc = std::min(w.freeze_cols, nc);
    lay.fri = (int)(std::lower_bound(lay.rows.begin(), lay.rows.end(), lay.fr) - lay.rows.begin());
    lay.fh = lay.ry[lay.fri];
    lay.fw = lay.cx[lay.fc];
}

const CellLook& SheetDoc::look(int r, int c) {
    if (look_gen != b.gen || look_si != cur || looks.size() > 20000) { looks.clear(); look_gen = b.gen; look_si = cur; }
    uint64_t k = (uint64_t)r * 16384u + (uint64_t)c;
    auto it = looks.find(k);
    if (it != looks.end()) return it->second;
    return looks.emplace(k, b.look(cur, r, c)).first->second;
}

void SheetDoc::select_cell(int r, int c, bool extend, bool add) {
    auto& s = su();
    r = std::clamp(r, 0, MAX_ROWS - 1); c = std::clamp(c, 0, MAX_COLS - 1);
    if (extend) { s.sel.back().r1 = r; s.sel.back().c1 = c; }
    else {
        // a click inside a merged area selects the whole merge with the anchor at the top-left
        SelRng n{r, c, r, c};
        if (const Range* m = ws().merge_at(r, c)) n = {m->r0, m->c0, m->r1, m->c1};
        if (add) s.sel.push_back(n); else s.sel = {n};
    }
    want_scroll = true;
}
void SheetDoc::select_range(const Range& rg) { su().sel = {SelRng{rg.r0, rg.c0, rg.r1, rg.c1}}; want_scroll = true; }
void SheetDoc::scroll_to(int r, int c) { select_cell(r, c); }
void SheetDoc::select_all() {
    Range u = ws().used_range();
    Range reg = data_region(ar(), ac());
    Range p = prim();
    // Excel: first Ctrl+A selects the current region, a second one the whole sheet
    if (u.valid() && !(p == reg) && (reg.rows() > 1 || reg.cols() > 1)) select_range(reg);
    else su().sel = {SelRng{0, 0, MAX_ROWS - 1, MAX_COLS - 1}};
    want_scroll = false;
}

// ---------------------------------------------------------------- editing
void SheetDoc::begin_edit(bool keep, const char* initial, bool in_bar) {
    if (editing) { if (in_bar != edit_in_bar) { edit_in_bar = in_bar; edit_focus = true; } return; }
    int r = ar(), c = ac();
    if (const Range* m = ws().merge_at(r, c)) { r = m->r0; c = m->c0; }
    std::string v = keep ? b.input_text(cur, r, c) : std::string(initial ? initial : "");
    size_t n = std::min(v.size(), ebuf.size() - 1);
    std::memcpy(ebuf.data(), v.data(), n);
    ebuf[n] = 0;
    editing = true; edit_in_bar = in_bar; er = r; ec = c; esi = cur;
    edit_focus = true; want_caret = (int)n; caret = (int)n;
    point_mode = false; pt_b = pt_e = -1; ac_start = -1; ac_sel = 0;
    edit_dirty = !keep;
    clear_marquee();
}

bool SheetDoc::commit_edit(int dr, int dc) {
    if (!editing) return true;
    std::string t(ebuf.data());
    if (esi >= (int)b.sheets.size()) { editing = false; return true; }
    if (t != b.input_text(esi, er, ec)) {
        if (t.empty() || t[0] != '=') {
            std::string msgv;
            DataValidation::ErrStyle how = DataValidation::Stop;
            if (!b.validate(esi, er, ec, parse_input(t, nullptr, b.date1904), &msgv, &how)) {
                if (how == DataValidation::Stop) {
                    toast(msgv.empty() ? std::string("The value doesn't match the data validation restrictions for this cell.") : msgv, ToastKind::Error, 5.f);
                    edit_focus = true;
                    return false;
                }
                toast(msgv.empty() ? std::string("Value outside the validation rule") : msgv, ToastKind::Warning, 4.f);
            }
        }
        b.set_input(esi, er, ec, t);
    }
    editing = false; point_mode = false; ac_start = -1; focus_grid = true;
    if (cur != esi) cur = esi;
    if (dr || dc) {
        int r = er + dr, c = ec + dc;
        if (const Range* m = ws().merge_at(er, ec)) { if (dr > 0) r = m->r1 + 1; if (dc > 0) c = m->c1 + 1; }
        // skip hidden rows / cols
        while (r > 0 && r < MAX_ROWS - 1 && ws().row_hidden(r)) r += dr > 0 ? 1 : -1;
        while (c > 0 && c < MAX_COLS - 1 && ws().col_hidden(c)) c += dc > 0 ? 1 : -1;
        select_cell(std::max(0, r), std::max(0, c));
    }
    return true;
}
void SheetDoc::cancel_edit() { editing = false; point_mode = false; ac_start = -1; focus_grid = true; cur = std::min(esi, (int)b.sheets.size() - 1); }

void SheetDoc::insert_ref_text(const std::string& ref, bool replace_point) {
    std::string t(ebuf.data());
    int at = std::clamp(caret, 0, (int)t.size());
    if (replace_point && pt_b >= 0 && pt_e <= (int)t.size() && pt_b <= pt_e) { t.erase(pt_b, pt_e - pt_b); at = pt_b; }
    t.insert(at, ref);
    pt_b = at; pt_e = at + (int)ref.size();
    size_t n = std::min(t.size(), ebuf.size() - 1);
    std::memcpy(ebuf.data(), t.data(), n); ebuf[n] = 0;
    want_caret = pt_e; caret = pt_e;
    edit_focus = true; point_mode = true; edit_dirty = true;
}

// ---------------------------------------------------------------- selection ops
static Range clamp_used(const Worksheet& w, Range g) {
    // whole rows / columns: limit to the used area (styles on 1M cells would be wasteful)
    if (g.r1 >= MAX_ROWS - 1) g.r1 = std::max(g.r0, w.nrows() - 1);
    if (g.c1 >= MAX_COLS - 1) g.c1 = std::max(g.c0, w.ncols() - 1);
    return g;
}
void SheetDoc::each_sel(const std::function<void(const Range&)>& fn) {
    for (auto& s : su().sel) fn(clamp_used(ws(), s.norm()));
}
void SheetDoc::style_sel(const char* label, const std::function<void(Style&)>& fn) {
    commit_edit();
    Book::Batch g(b, label);
    each_sel([&](const Range& r) { b.apply_style(cur, r, fn, label); });
}
void SheetDoc::clear_sel(unsigned what, const char* label) {
    commit_edit();
    Book::Batch g(b, label);
    each_sel([&](const Range& r) { b.clear(cur, r, what); });
    msg = label;
}

void SheetDoc::insert_rows(bool below) {
    commit_edit();
    Range g = prim();
    int n = g.rows();
    if (n > 100000) n = 1;
    b.insert_rows(cur, below ? g.r1 + 1 : g.r0, n);
    msg = "Inserted " + std::to_string(n) + " row" + (n > 1 ? "s" : "");
}
void SheetDoc::delete_rows() {
    commit_edit();
    Range g = clamp_used(ws(), prim());
    b.delete_rows(cur, g.r0, g.rows());
    select_cell(std::min(g.r0, std::max(0, ws().nrows() - 1)), ac());
}
void SheetDoc::insert_cols(bool right) {
    commit_edit();
    Range g = prim();
    int n = g.cols();
    if (n > 1000) n = 1;
    b.insert_cols(cur, right ? g.c1 + 1 : g.c0, n);
}
void SheetDoc::delete_cols() {
    commit_edit();
    Range g = clamp_used(ws(), prim());
    b.delete_cols(cur, g.c0, g.cols());
}
void SheetDoc::insert_cells_shift(bool down) {
    // shift cells down / right inside the selection's columns / rows (copy + paste)
    commit_edit();
    Range g = prim();
    Range u = ws().used_range();
    if (!u.valid()) return;
    Book::Batch bt(b, "Insert cells");
    if (down) {
        Range src(g.r0, g.c0, std::max(u.r1, g.r0), g.c1);
        ClipBlock blk = b.copy(cur, src);
        b.clear(cur, src, Book::ClearAll);
        b.paste(cur, g.r0 + g.rows(), g.c0, blk, PasteAll);
    } else {
        Range src(g.r0, g.c0, g.r1, std::max(u.c1, g.c0));
        ClipBlock blk = b.copy(cur, src);
        b.clear(cur, src, Book::ClearAll);
        b.paste(cur, g.r0, g.c0 + g.cols(), blk, PasteAll);
    }
}
void SheetDoc::delete_cells_shift(bool up) {
    commit_edit();
    Range g = prim();
    Range u = ws().used_range();
    if (!u.valid()) return;
    Book::Batch bt(b, "Delete cells");
    if (up) {
        if (u.r1 > g.r1) {
            Range src(g.r1 + 1, g.c0, u.r1, g.c1);
            ClipBlock blk = b.copy(cur, src);
            b.clear(cur, Range(g.r0, g.c0, u.r1, g.c1), Book::ClearAll);
            b.paste(cur, g.r0, g.c0, blk, PasteAll);
        } else b.clear(cur, g, Book::ClearAll);
    } else {
        if (u.c1 > g.c1) {
            Range src(g.r0, g.c1 + 1, g.r1, u.c1);
            ClipBlock blk = b.copy(cur, src);
            b.clear(cur, Range(g.r0, g.c0, g.r1, u.c1), Book::ClearAll);
            b.paste(cur, g.r0, g.c0, blk, PasteAll);
        } else b.clear(cur, g, Book::ClearAll);
    }
}

static float text_px(const std::string& s, float size_scale, bool bold) {
    ImFont* f = bold && g_font_bold ? g_font_bold : (g_font ? g_font : ImGui::GetFont());
    float fs = f->FontSize * size_scale;
    const char* b0 = s.c_str();
    const char* e = b0 + std::min(s.size(), (size_t)300);
    float w = 0;
    for (const char* p = b0; p < e;) {   // widest line
        const char* nl = (const char*)memchr(p, '\n', e - p);
        const char* le = nl ? nl : e;
        w = std::max(w, f->CalcTextSizeA(fs, FLT_MAX, 0, p, le).x);
        p = nl ? nl + 1 : e;
    }
    return w;
}

void SheetDoc::autofit_cols(int c0, int c1) {
    commit_edit();
    const Worksheet& w = ws();
    int n = w.nrows();
    if (c1 >= MAX_COLS - 1) c1 = std::max(c0, w.ncols() - 1);
    int step = n > 20000 ? n / 20000 : 1;
    Book::Batch g(b, "Autofit column width");
    for (int c = c0; c <= c1; ++c) {
        float best = 0;
        for (int r = 0; r < n; r += step) {
            if (w.row_hidden(r)) continue;
            const Cell* ce = w.cell(r, c);
            if (!ce || ce->v.is_empty()) continue;
            if (w.merge_at(r, c)) continue;
            const Style& st = b.styles.get(ce->style);
            std::string t = b.display_text(cur, r, c);
            float px = text_px(t, (float)(st.font.size / 11.0) * 0.98f, st.font.bold) / dpi_scale();
            px += st.indent * 9.f;
            best = std::max(best, px);
        }
        double chars = best > 0 ? std::min(255.0, (best + 10.0) / 7.0) : w.default_col_width;
        b.set_col_width(cur, c, c, chars);
    }
}

void SheetDoc::autofit_rows(int r0, int r1) {
    commit_edit();
    const Worksheet& w = ws();
    if (r1 >= MAX_ROWS - 1) r1 = std::max(r0, w.nrows() - 1);
    Book::Batch g(b, "Autofit row height");
    for (int r = r0; r <= r1 && r - r0 < 100000; ++r) {
        if (r >= (int)w.rows.size()) break;
        double best = w.default_row_height;
        const RowStore& rs = w.rows[r];
        for (int c = 0; c < (int)rs.cells.size(); ++c) {
            const Cell& ce = rs.cells[c];
            const Style& st = b.styles.get(ce.style);
            double line = std::max(15.0, st.font.size * 1.36);
            int lines = 1;
            if (!ce.v.is_empty() && st.wrap) {
                std::string t = b.display_text(cur, r, c);
                float cw = std::max(10.f, (float)(w.col_width(c) * 7.0 + 5.0) - 6.f);
                float tw = text_px(t, (float)(st.font.size / 11.0), st.font.bold) / dpi_scale();
                lines = std::max(1, (int)std::ceil(tw / cw));
                lines += (int)std::count(t.begin(), t.end(), '\n');
            }
            best = std::max(best, line * lines);
        }
        if (std::fabs(best - w.row_height(r)) > 0.1) b.set_row_height(cur, r, r, best);
    }
}

Range SheetDoc::data_region(int r, int c) {
    const Worksheet& w = ws();
    auto filled = [&](int rr, int cc) { return rr >= 0 && cc >= 0 && !w.value(rr, cc).is_empty(); };
    Range g(r, c, r, c);
    for (int it = 0; it < 2000000; ++it) {
        bool grow = false;
        auto any_row = [&](int rr) { if (rr < 0 || rr >= w.nrows()) return false; for (int cc = std::max(0, g.c0 - 1); cc <= g.c1 + 1; ++cc) if (filled(rr, cc)) return true; return false; };
        auto any_col = [&](int cc) { if (cc < 0 || cc >= w.ncols()) return false; for (int rr = std::max(0, g.r0 - 1); rr <= g.r1 + 1; ++rr) if (filled(rr, cc)) return true; return false; };
        if (any_row(g.r1 + 1)) { ++g.r1; grow = true; }
        if (any_row(g.r0 - 1)) { --g.r0; grow = true; }
        if (any_col(g.c1 + 1)) { ++g.c1; grow = true; }
        if (any_col(g.c0 - 1)) { --g.c0; grow = true; }
        if (!grow) break;
    }
    return g;
}

int SheetDoc::edge_jump(int r, int c, int dr, int dc) {
    const Worksheet& w = ws();
    auto filled = [&](int rr, int cc) { return !w.value(rr, cc).is_empty(); };
    int lim_r = std::max(w.nrows(), 1) - 1, lim_c = std::max(w.ncols(), 1) - 1;
    int pos = dr ? r : c;
    int lim = dr ? (dr > 0 ? std::max(lim_r, r) : 0) : (dc > 0 ? std::max(lim_c, c) : 0);
    int d = dr ? dr : dc;
    auto at = [&](int p) { return dr ? filled(p, c) : filled(r, p); };
    int nxt = pos + d;
    if ((d > 0 && nxt > lim) || (d < 0 && nxt < 0)) return d > 0 ? (dr ? std::min(MAX_ROWS - 1, pos) : pos) : 0;
    if (at(pos) && at(nxt)) {   // run: go to its end
        while ((d > 0 ? nxt + d <= lim : nxt + d >= 0) && at(nxt + d)) nxt += d;
        return nxt;
    }
    while ((d > 0 ? nxt <= lim : nxt >= 0) && !at(nxt)) nxt += d;
    if (d > 0 && nxt > lim) return dr ? std::max(lim_r + 1, r) : std::max(lim_c + 1, c) ;
    if (d < 0 && nxt < 0) return 0;
    return nxt;
}

void SheetDoc::autosum() {
    commit_edit();
    Range g = prim();
    const Worksheet& w = ws();
    auto num = [&](int r, int c) { return w.value(r, c).is_num(); };
    if (g.r0 == g.r1 && g.c0 == g.c1) {
        int r = g.r0, c = g.c0;
        int top = r - 1;
        while (top >= 0 && (num(top, c) || (top == r - 1 && w.value(top, c).is_empty()))) --top;
        std::string f;
        if (r > 0 && top < r - 1 && num(r - 1, c)) f = "=SUM(" + range_name(Range(top + 1, c, r - 1, c)) + ")";
        else {
            int left = c - 1;
            while (left >= 0 && num(r, left)) --left;
            if (c > 0 && left < c - 1) f = "=SUM(" + range_name(Range(r, left + 1, r, c - 1)) + ")";
            else f = "=SUM()";
        }
        begin_edit(false, f.c_str());
        if (f == "=SUM()") { want_caret = 5; caret = 5; }
        return;
    }
    // range: totals under each column (first empty row below the selection)
    Book::Batch bt(b, "AutoSum");
    int rr = g.r1 + 1;
    bool last_row_empty = true;
    for (int c = g.c0; c <= g.c1; ++c) if (!w.value(g.r1, c).is_empty()) last_row_empty = false;
    if (last_row_empty && g.r1 > g.r0) rr = g.r1;
    for (int c = g.c0; c <= g.c1; ++c) {
        bool any = false;
        for (int r = g.r0; r < rr && !any; ++r) any = num(r, c);
        if (any) b.set_input(cur, rr, c, "=SUM(" + range_name(Range(g.r0, c, rr - 1, c)) + ")");
    }
}

void SheetDoc::fill_dir(int dir) {
    commit_edit();
    Range g = clamp_used(ws(), prim());
    Range src, dst = g;
    switch (dir) {
    case 0: if (g.r0 == g.r1) { if (g.r0 == 0) return; src = Range(g.r0 - 1, g.c0, g.r0 - 1, g.c1); dst.r0 = g.r0 - 1; } else src = Range(g.r0, g.c0, g.r0, g.c1); break;
    case 1: if (g.c0 == g.c1) { if (g.c0 == 0) return; src = Range(g.r0, g.c0 - 1, g.r1, g.c0 - 1); dst.c0 = g.c0 - 1; } else src = Range(g.r0, g.c0, g.r1, g.c0); break;
    case 2: src = Range(g.r1, g.c0, g.r1, g.c1); break;
    default: src = Range(g.r0, g.c1, g.r1, g.c1); break;
    }
    // Ctrl+D / Ctrl+R copy (Excel does not build a series here): paste the source block over the target
    Book::Batch bt(b, dir == 0 ? "Fill down" : dir == 1 ? "Fill right" : "Fill");
    ClipBlock blk = b.copy(cur, src);
    if (dir == 0 || dir == 2) { for (int r = dst.r0; r <= dst.r1; ++r) if (r < src.r0 || r > src.r1) b.paste(cur, r, src.c0, blk, PasteAll); }
    else for (int c = dst.c0; c <= dst.c1; ++c) if (c < src.c0 || c > src.c1) b.paste(cur, src.r0, c, blk, PasteAll);
}

void SheetDoc::sort_quick(bool desc) {
    commit_edit();
    Range g = prim();
    Range rg;
    bool header = false;
    const Worksheet& w = ws();
    if (w.autofilter.active() && w.autofilter.range.contains(g.r0, g.c0)) { rg = w.autofilter.range; header = true; }
    else if (g.rows() > 1) rg = clamp_used(w, g);
    else {
        rg = data_region(g.r0, g.c0);
        // header: first row text while the column below is not all text, or frozen header
        header = w.freeze_rows > 0 && rg.r0 < w.freeze_rows;
        if (header) rg.r0 = w.freeze_rows - 1;
        else if (rg.rows() > 1) {
            int txt = 0, n = 0;
            for (int c = rg.c0; c <= rg.c1; ++c) { const Value& v = w.value(rg.r0, c); if (!v.is_empty()) { ++n; txt += v.is_str(); } }
            int txt2 = 0, n2 = 0;
            for (int c = rg.c0; c <= rg.c1; ++c) { const Value& v = w.value(rg.r0 + 1, c); if (!v.is_empty()) { ++n2; txt2 += v.is_str(); } }
            header = n > 0 && txt == n && (txt2 < n2 || b.style_of(cur, rg.r0, rg.c0).font.bold);
        }
    }
    if (rg.rows() < 2) return;
    b.sort(cur, rg, {SortKey{ac(), desc, true, false}}, header);
    msg = std::string("Sorted ") + (desc ? "Z \xE2\x86\x92 A" : "A \xE2\x86\x92 Z") + " by column " + col_name(ac());
}

void SheetDoc::toggle_filter() {
    commit_edit();
    const Worksheet& w = ws();
    if (w.autofilter.active()) { b.set_autofilter(cur, Range()); msg = "Filter removed"; return; }
    Range g = prim();
    Range rg = (g.rows() > 1 || g.cols() > 1) ? clamp_used(w, g) : data_region(g.r0, g.c0);
    if (rg.rows() < 1) return;
    if (w.freeze_rows > 0 && rg.r0 < w.freeze_rows && rg.r0 + 1 < w.freeze_rows) rg.r0 = w.freeze_rows - 1;
    b.set_autofilter(cur, rg);
    msg = "Filter on " + range_name(rg);
}

void SheetDoc::freeze_at_cursor() {
    commit_edit();
    const Worksheet& w = ws();
    if (w.freeze_rows || w.freeze_cols) { b.set_freeze(cur, 0, 0); return; }
    b.set_freeze(cur, ar(), ac());
}

void SheetDoc::insert_date(bool time) {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    char buf[32];
    if (time) std::snprintf(buf, sizeof buf, "%d:%02d", tmv.tm_hour, tmv.tm_min);
    else std::snprintf(buf, sizeof buf, "%d/%d/%d", tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_year + 1900);
    if (editing) { insert_ref_text(buf, false); point_mode = false; return; }
    b.set_input(cur, ar(), ac(), buf);
}

void SheetDoc::apply_cell_style(int preset) {
    static const char* names[] = {"Normal", "Header", "Good", "Bad", "Neutral", "Title", "Note", "Input", "Total", "Warning Text", "Heading 1", "Check Cell"};
    style_sel(names[preset], [preset](Style& s) {
        Style d;
        switch (preset) {
        case 0: s = Style{}; s.numfmt = s.numfmt; break;
        case 1: s.font.bold = true; s.font.color = 0xFFFFFFFF; s.fill = Fill{FillPattern::Solid, 0xFF305496, 0}; s.border.bottom = {BorderStyle::Medium, 0xFF1F3864}; s.valign = VAlign::Center; break;
        case 2: s.fill = Fill{FillPattern::Solid, 0xFFC6EFCE, 0}; s.font.color = 0xFF006100; break;
        case 3: s.fill = Fill{FillPattern::Solid, 0xFFFFC7CE, 0}; s.font.color = 0xFF9C0006; break;
        case 4: s.fill = Fill{FillPattern::Solid, 0xFFFFEB9C, 0}; s.font.color = 0xFF9C5700; break;
        case 5: s.font.bold = true; s.font.size = 18; s.font.color = 0xFF44546A; break;
        case 6: s.fill = Fill{FillPattern::Solid, 0xFFFFFFCC, 0}; for (auto* b2 : {&s.border.left, &s.border.right, &s.border.top, &s.border.bottom}) *b2 = {BorderStyle::Thin, 0xFFB2B2B2}; break;
        case 7: s.fill = Fill{FillPattern::Solid, 0xFFFFCC99, 0}; s.font.color = 0xFF3F3F76; for (auto* b2 : {&s.border.left, &s.border.right, &s.border.top, &s.border.bottom}) *b2 = {BorderStyle::Thin, 0xFF7F7F7F}; break;
        case 8: s.font.bold = true; s.border.top = {BorderStyle::Thin, 0xFF4472C4}; s.border.bottom = {BorderStyle::Double, 0xFF4472C4}; break;
        case 9: s.font.color = 0xFFFF0000; break;
        case 10: s.font.bold = true; s.font.size = 15; s.font.color = 0xFF44546A; s.border.bottom = {BorderStyle::Thick, 0xFF4472C4}; break;
        case 11: s.font.bold = true; s.font.color = 0xFFFFFFFF; s.fill = Fill{FillPattern::Solid, 0xFFA5A5A5, 0}; for (auto* b2 : {&s.border.left, &s.border.right, &s.border.top, &s.border.bottom}) *b2 = {BorderStyle::Double, 0xFF3F3F3F}; break;
        }
        (void)d;
    });
}

void SheetDoc::set_numfmt(const std::string& code) {
    commit_edit();
    Book::Batch g(b, "Number format");
    each_sel([&](const Range& r) { b.set_number_format(cur, r, code); });
}

void SheetDoc::bump_decimals(int d) {
    std::string code = b.style_of(cur, ar(), ac()).numfmt;
    if (code == "General" || code.empty()) {
        std::string t = b.display_text(cur, ar(), ac());
        size_t k = t.find('.');
        int dec = k == std::string::npos ? 0 : (int)(t.size() - k - 1);
        dec = std::max(0, dec + d);
        code = dec ? "0." + std::string(dec, '0') : "0";
    } else {
        // adjust every section: digits after the decimal point of the first number placeholder group
        std::string out;
        size_t start = 0;
        while (start <= code.size()) {
            size_t semi = code.find(';', start);
            std::string sec = code.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
            size_t dot = std::string::npos;
            bool q = false;
            for (size_t i = 0; i < sec.size(); ++i) { if (sec[i] == '"') q = !q; else if (!q && sec[i] == '.') { dot = i; break; } }
            if (d > 0) {
                if (dot == std::string::npos) {
                    size_t last = sec.find_last_of("0#");
                    if (last != std::string::npos) sec.insert(last + 1, ".0");
                } else sec.insert(dot + 1, "0");
            } else if (dot != std::string::npos) {
                size_t i = dot + 1;
                if (i < sec.size() && (sec[i] == '0' || sec[i] == '#')) {
                    sec.erase(i, 1);
                    if (i >= sec.size() || (sec[i] != '0' && sec[i] != '#')) sec.erase(dot, 1);
                }
            }
            out += sec;
            if (semi == std::string::npos) break;
            out += ';';
            start = semi + 1;
        }
        code = out;
    }
    set_numfmt(code);
}

void SheetDoc::merge_center() {
    commit_edit();
    Range g = prim();
    if (const Range* m = ws().merge_at(ar(), ac())) { b.unmerge(cur, *m); return; }
    if (g.rows() == 1 && g.cols() == 1) return;
    int filled = 0;
    for (int r = g.r0; r <= g.r1 && r - g.r0 < 1000; ++r) for (int c = g.c0; c <= g.c1; ++c) filled += !ws().value(r, c).is_empty();
    if (filled > 1) toast("Merging keeps only the upper-left value", ToastKind::Warning, 3.5f);
    Book::Batch bt(b, "Merge & Center");
    b.merge(cur, g);
    b.apply_style(cur, g, [](Style& s) { s.halign = HAlign::Center; }, "Center");
}

// ---------------------------------------------------------------- find / replace
static bool icontains(const std::string& hay, const std::string& needle, bool mc) {
    if (needle.empty()) return true;
    if (mc) return hay.find(needle) != std::string::npos;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return tolower((unsigned char)a) == tolower((unsigned char)b); });
    return it != hay.end();
}
static bool cell_match(const std::string& v, const std::string& f, bool mc, bool whole) {
    if (f.empty()) return false;
    if (whole) {
        if (v.size() != f.size()) return false;
        return mc ? v == f : icontains(v, f, false);
    }
    return icontains(v, f, mc);
}

bool SheetDoc::find_next(bool back) {
    std::string f = find_buf;
    if (f.empty()) return false;
    commit_edit();
    int nsh = find_all_sheets ? (int)b.sheets.size() : 1;
    for (int pass = 0; pass < nsh + 1; ++pass) {
        int si = find_all_sheets ? (cur + (back ? -pass : pass) + nsh * 4) % nsh : cur;
        const Worksheet& w = b.sheets[si];
        int nr = w.nrows(), nc = w.ncols();
        if (!nr || !nc) continue;
        long total = (long)nr * nc;
        long start = (si == cur && pass == 0) ? (long)ar() * nc + ac() : (back ? total : -1);
        long lim = (si == cur && pass == 0) ? total : total;
        for (long k = 1; k <= lim; ++k) {
            long idx = back ? start - k : start + k;
            if (idx < 0 || idx >= total) { if (find_all_sheets && nsh > 1) break; idx = ((idx % total) + total) % total; }
            int r = (int)(idx / nc), c = (int)(idx % nc);
            if (w.row_hidden(r)) continue;
            const Cell* ce = w.cell(r, c);
            if (!ce) continue;
            std::string v = find_formulas ? b.input_text(si, r, c) : b.display_text(si, r, c);
            if (cell_match(v, f, match_case, whole_cell)) {
                if (si != cur) { cur = si; }
                select_cell(r, c);
                return true;
            }
        }
        if (!find_all_sheets) break;
    }
    msg = "\"" + f + "\" not found";
    gui::status(msg, ToastKind::Warning);
    return false;
}

static std::string replaced_text(const std::string& v, const std::string& f, const std::string& rp, bool mc, bool whole) {
    if (whole) return cell_match(v, f, mc, true) ? rp : v;
    std::string o;
    size_t p = 0;
    while (p < v.size()) {
        size_t k;
        if (mc) k = v.find(f, p);
        else {
            auto it = std::search(v.begin() + p, v.end(), f.begin(), f.end(), [](char a, char b) { return tolower((unsigned char)a) == tolower((unsigned char)b); });
            k = it == v.end() ? std::string::npos : (size_t)(it - v.begin());
        }
        if (k == std::string::npos) { o += v.substr(p); break; }
        o += v.substr(p, k - p); o += rp; p = k + f.size();
    }
    return o;
}

void SheetDoc::replace_one() {
    std::string f = find_buf;
    std::string v = b.input_text(cur, ar(), ac());
    if (cell_match(v, f, match_case, whole_cell)) b.set_input(cur, ar(), ac(), replaced_text(v, f, repl_buf, match_case, whole_cell));
    find_next(false);
}

int SheetDoc::replace_all() {
    std::string f = find_buf;
    if (f.empty()) return 0;
    commit_edit();
    int n = 0;
    Book::Batch g(b, "Replace all");
    for (int si = 0; si < (int)b.sheets.size(); ++si) {
        if (!find_all_sheets && si != cur) continue;
        const Worksheet& w = b.sheets[si];
        int nr = w.nrows();
        for (int r = 0; r < nr; ++r) {
            if (w.row_hidden(r) || r >= (int)w.rows.size()) continue;
            int nc = (int)w.rows[r].cells.size();
            for (int c = 0; c < nc; ++c) {
                if (w.rows[r].cells[c].blank()) continue;
                std::string v = b.input_text(si, r, c);
                if (!cell_match(v, f, match_case, whole_cell)) continue;
                std::string nv = replaced_text(v, f, repl_buf, match_case, whole_cell);
                if (nv != v) { b.set_input(si, r, c, nv); ++n; }
            }
        }
    }
    toast("Replaced " + std::to_string(n) + " cell" + (n == 1 ? "" : "s"), n ? ToastKind::Success : ToastKind::Info, 2.5f);
    return n;
}

void SheetDoc::find_all() {
    find_hits.clear();
    std::string f = find_buf;
    if (f.empty()) return;
    for (int si = 0; si < (int)b.sheets.size(); ++si) {
        if (!find_all_sheets && si != cur) continue;
        const Worksheet& w = b.sheets[si];
        for (int r = 0; r < (int)w.rows.size(); ++r) {
            if (w.row_hidden(r)) continue;
            for (int c = 0; c < (int)w.rows[r].cells.size(); ++c) {
                if (w.rows[r].cells[c].blank()) continue;
                std::string v = find_formulas ? b.input_text(si, r, c) : b.display_text(si, r, c);
                if (cell_match(v, f, match_case, whole_cell)) { find_hits.push_back({si, {r, c}}); if (find_hits.size() >= 5000) return; }
            }
        }
    }
}

bool SheetDoc::goto_ref(const std::string& txt0) {
    std::string txt = txt0;
    while (!txt.empty() && txt.back() == ' ') txt.pop_back();
    while (!txt.empty() && txt[0] == ' ') txt.erase(0, 1);
    if (txt.empty()) return false;
    int si = cur;
    std::string ref = txt;
    size_t bang = txt.rfind('!');
    if (bang != std::string::npos) {
        std::string sn = txt.substr(0, bang);
        if (sn.size() >= 2 && sn.front() == '\'' && sn.back() == '\'') sn = sn.substr(1, sn.size() - 2);
        si = b.find_sheet(sn);
        if (si < 0) return false;
        ref = txt.substr(bang + 1);
    }
    Range rg;
    if (!parse_range_name(ref, rg)) {
        const DefinedName* dn = b.find_name(txt, cur);
        if (!dn) return false;
        return goto_ref(dn->formula);
    }
    rg = rg.norm();
    if (rg.r1 < 0 || rg.r1 >= MAX_ROWS) rg.r1 = MAX_ROWS - 1;
    if (rg.c1 < 0 || rg.c1 >= MAX_COLS) rg.c1 = MAX_COLS - 1;
    commit_edit();
    cur = si;
    sync_ui();
    su().sel = {SelRng{rg.r0, rg.c0, rg.r1, rg.c1}};
    want_scroll = true;
    return true;
}

// ---------------------------------------------------------------- clipboard (Win32)
static bool clip_set(const std::string& tsv, const std::string& html) {
    HWND h = app::frame_hwnd();
    if (!OpenClipboard(h)) return false;
    EmptyClipboard();
    std::wstring w = ui::W(tsv);
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t))) {
        memcpy(GlobalLock(g), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    if (!html.empty()) {
        static UINT fmt = RegisterClipboardFormatW(L"HTML Format");
        if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, html.size() + 1)) {
            memcpy(GlobalLock(g), html.c_str(), html.size() + 1);
            GlobalUnlock(g);
            if (!SetClipboardData(fmt, g)) GlobalFree(g);
        }
    }
    CloseClipboard();
    return true;
}
static bool clip_get(std::string& tsv, std::string& html) {
    tsv.clear(); html.clear();
    if (!OpenClipboard(app::frame_hwnd())) return false;
    static UINT fmt = RegisterClipboardFormatW(L"HTML Format");
    if (HANDLE g = GetClipboardData(fmt)) {
        if (const char* p = (const char*)GlobalLock(g)) { html.assign(p, strnlen(p, GlobalSize(g))); GlobalUnlock(g); }
    }
    if (HANDLE g = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = (const wchar_t*)GlobalLock(g)) { tsv = ui::U(std::wstring(p, wcsnlen(p, GlobalSize(g) / 2))); GlobalUnlock(g); }
    }
    CloseClipboard();
    return true;
}

void SheetDoc::copy(bool cut) {
    commit_edit();
    if (su().sel.size() > 1) { toast("This action won't work on multiple selections", ToastKind::Warning, 3.f); return; }
    Range g = clamp_used(ws(), prim());
    clip = std::make_unique<ClipBlock>(b.copy(cur, g));
    std::string tsv = b.copy_tsv(cur, g);
    std::string html = g.rows() * (long)g.cols() <= 200000 ? cf_html_wrap(b.copy_html(cur, g)) : std::string();
    clip_set(tsv, html);
    clip_seq = GetClipboardSequenceNumber();
    clip_rg = g; clip_si = cur; clip_cut = cut;
    msg = std::string(cut ? "Cut " : "Copied ") + std::to_string(g.rows()) + " x " + std::to_string(g.cols());
}

void SheetDoc::paste(unsigned what, bool from_dialog) {
    (void)from_dialog;
    commit_edit();
    ClipBlock ext;
    const ClipBlock* blk = nullptr;
    bool internal = clip && GetClipboardSequenceNumber() == clip_seq;
    if (internal) blk = clip.get();
    else {
        std::string tsv, html;
        clip_get(tsv, html);
        if (!html.empty() && parse_html_table(cf_html_unwrap(html), ext) && ext.rows > 0) blk = &ext;
        else if (!tsv.empty()) { ext = clip_from_tsv(tsv); if (ext.rows > 0) blk = &ext; }
        if (blk && !(what & (PasteFormats))) {}   // values only fine
    }
    if (!blk || blk->rows <= 0 || blk->cols <= 0) return;
    Range g = prim();
    int R = blk->rows, C = blk->cols;
    if (what & PasteTranspose) std::swap(R, C);
    Book::Batch bt(b, internal && clip_cut ? "Cut & paste" : "Paste");
    // tile the block over a larger selection that is a whole multiple of it (Excel)
    int tr = 1, tc = 1;
    if (g.rows() > R && g.rows() % R == 0 && g.rows() <= 100000) tr = g.rows() / R;
    if (g.cols() > C && g.cols() % C == 0 && g.cols() <= 1000) tc = g.cols() / C;
    if (internal && clip_cut) { tr = tc = 1; b.clear(clip_si, clip_rg, Book::ClearAll); }
    for (int i = 0; i < tr; ++i) for (int k = 0; k < tc; ++k) b.paste(cur, g.r0 + i * R, g.c0 + k * C, *blk, what);
    if (internal && clip_cut) { clip.reset(); clip_si = -1; }
    su().sel = {SelRng{g.r0, g.c0, g.r0 + R * tr - 1, g.c0 + C * tc - 1}};
    msg = "Pasted " + std::to_string(R) + " x " + std::to_string(C);
}

// ---------------------------------------------------------------- import / save
void SheetDoc::import_as(bool bom) { send_to_project(bom ? 0 : 1); }

static std::string fixed_width(const Book& b, int si) {
    const Worksheet& w = b.sheets[si];
    int nr = w.nrows(), nc = w.ncols();
    std::vector<size_t> wd(nc, 0);
    std::vector<std::vector<std::string>> t(nr, std::vector<std::string>(nc));
    for (int r = 0; r < nr; ++r) for (int c = 0; c < nc; ++c) { t[r][c] = b.display_text(si, r, c); wd[c] = std::max(wd[c], t[r][c].size()); }
    std::string o;
    for (auto& row : t) {
        std::string line;
        for (int c = 0; c < nc; ++c) { line += row[c]; if (c + 1 < nc) line.append(wd[c] - row[c].size() + 2, ' '); }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        o += line + "\r\n";
    }
    return o;
}

bool SheetDoc::write_to(const std::wstring& p, std::string& err) {
    std::string e = ext_lower(p);
    std::vector<uint8_t> out;
    if (e == "xlsx" || e == "xlsm") out = io::xlsx_write(b);
    else if (e == "ods") out = io::ods_write(b);
    else if (e == "prn") { std::string t = fixed_width(b, cur); out.assign(t.begin(), t.end()); }
    else {
        io::CsvOptions o;
        o.sep = e == "csv" ? ',' : '\t';
        o.encoding = e == "csv" ? io::TextEncoding::Utf8Bom : io::TextEncoding::Utf8;
        out = io::csv_write(b, cur, o);
    }
    return write_file(ui::U(p), out.data(), out.size(), err);
}

bool SheetDoc::save(bool save_as) {
    commit_edit();
    std::wstring p = path;
    if (save_as || p.empty() || readonly_src) {
        std::wstring def = p.empty() ? ui::W(name) : p;
        if (readonly_src || ext_lower(def) == "txt" || ext_lower(def) == "bom") { size_t k = def.find_last_of(L'.'); if (k != std::wstring::npos) def = def.substr(0, k); def += L".xlsx"; }
        if (!ui::save_file_dialog(app::frame_hwnd(), L"Save Spreadsheet As",
                                  {{L"Excel workbook (*.xlsx)", L"*.xlsx"}, {L"OpenDocument spreadsheet (*.ods)", L"*.ods"}, {L"CSV UTF-8 (*.csv)", L"*.csv"},
                                   {L"Tab separated (*.tsv;*.txt)", L"*.tsv;*.txt"}, {L"Fixed width text (*.prn)", L"*.prn"}}, L"xlsx", def)) return false;
        p = def;
    }
    std::string e = ext_lower(p);
    bool book_fmt = e == "xlsx" || e == "xlsm" || e == "ods";
    if (!book_fmt && b.sheets.size() > 1) toast("Only the current sheet was saved (" + e + " holds one sheet, values only)", ToastKind::Warning, 5.f);
    std::string err;
    bool ok = false;
    run_with_progress_wait("Saving", [&](Progress& pr) { pr.text(ui::U(file_name_of(p)).c_str()); ok = write_to(p, err); }, ProgressOpts{true, false, 0.25f});
    if (!ok) { toast("Could not save: " + err, ToastKind::Error, 6.f); return false; }
    path = p; name = ui::U(file_name_of(p)); dirty_ = false; readonly_src = false; saved_gen = b.gen;
    toast("Saved " + name, ToastKind::Success, 2.5f);
    return true;
}

bool SheetDoc::on_command(int id) {
    using namespace app;
    switch (id) {
    case ID_EDIT_UNDO: if (editing) cancel_edit(); else b.undo(); return true;
    case ID_EDIT_REDO: if (!editing) b.redo(); return true;
    case ID_EDIT_COPY: copy(false); return true;
    case ID_EDIT_CUT: copy(true); return true;
    case ID_EDIT_PASTE: paste(); return true;
    case ID_EDIT_DELETE: clear_sel(Book::ClearContents, "Clear contents"); return true;
    case ID_EDIT_SELECT_ALL: select_all(); return true;
    case ID_TOOL_SEARCH: open_dialog(Dlg::Find); return true;
    }
    return false;
}

std::string SheetDoc::status() const {
    auto* self = const_cast<SheetDoc*>(this);
    std::string s = self->ws().name + "  \xC2\xB7  " + range_name(self->prim());
    if (!msg.empty()) s += "  \xC2\xB7  " + msg;
    return s;
}

// ---------------------------------------------------------------- top-level draw
void SheetDoc::draw(ImVec2 size) {
    sync_ui();
    b.active_sheet = cur;
    ImVec2 p0 = ImGui::GetCursorPos();
    draw_ribbon();
    if (show_formula_bar) draw_formula_bar();
    float tabs_h = S(28);
    float y0 = ImGui::GetCursorPosY();
    float used = y0 - p0.y;
    float gh = std::max(S(60), size.y - used - tabs_h);
    draw_grid(ImVec2(size.x, gh));
    ImGui::SetCursorPosY(y0 + gh);
    draw_sheet_tabs(tabs_h);
    draw_dialogs();
}

// ===========================================================================
// factories
// ===========================================================================
static void prepare_new(SheetDoc& d, bool guess_header) {
    d.b.clear_undo();
    d.sync_ui();
    for (size_t i = 0; i < d.b.sheets.size(); ++i) {
        Worksheet& w = d.b.sheets[i];
        d.ui_[i].sel = {SelRng{w.active_r, w.active_c, w.active_r, w.active_c}};
    }
    d.cur = std::clamp(d.b.active_sheet, 0, (int)d.b.sheets.size() - 1);
    for (size_t i = 0; i < d.b.sheets.size(); ++i) {
        Worksheet& w = d.b.sheets[i];
        if (const Range* m = w.merge_at(w.active_r, w.active_c)) d.ui_[i].sel = {SelRng{m->r0, m->c0, m->r1, m->c1}};
    }
    // formulas without a cached result (files written by tools that don't calculate): evaluate now
    bool need_calc = false;
    for (auto& w : d.b.sheets) {
        for (auto& rs : w.rows) { for (auto& ce : rs.cells) if (ce.f && (ce.v.is_empty() || (ce.v.is_str() && ce.v.str.empty()))) { need_calc = true; break; } if (need_calc) break; }
        if (need_calc) break;
    }
    if (need_calc) d.b.recalc_all();
    // automation: PCBP_SHEET_TEST = "fmt" | "cf" | "sort" | "dv" | "find" | "light-rows" ...
    if (const char* tst = getenv("PCBP_SHEET_TEST")) {
        std::string t = tst;
        if (t.find("fmt") != std::string::npos) { d.fmt_tab = t.find("fmt2") != std::string::npos ? 2 : t.find("fmt3") != std::string::npos ? 3 : 0; d.open_dialog(Dlg::FormatCells); }
        if (t.find("cf") != std::string::npos) d.open_dialog(Dlg::CondFmt);
        if (t.find("sort") != std::string::npos) d.open_dialog(Dlg::Sort);
        if (t.find("dv") != std::string::npos) d.open_dialog(Dlg::Validation);
        if (t.find("find") != std::string::npos) { std::snprintf(d.find_buf, sizeof d.find_buf, "0805"); d.show_replace = true; d.open_dialog(Dlg::Find); }
        if (t.find("sel") != std::string::npos) { d.ui_[d.cur].sel = {SelRng{3, 1, 9, 3}}; }
        if (t.find("edit") != std::string::npos) { d.ui_[d.cur].sel = {SelRng{16, 7, 16, 7}}; d.begin_edit(false, "=SUM(H3:H15)*VLOOKUP(B4,B3:G15,6,"); }
        if (t.find("bomtab") != std::string::npos) d.ribbon_tab = 2;
        if (t.find("zoom") != std::string::npos) d.b.sheets[d.cur].zoom = 150;
    }
    if (guess_header) {
        // plain text sources: freeze + autofit, header row when row 1 looks like one
        for (size_t si = 0; si < d.b.sheets.size(); ++si) {
            Worksheet& w = d.b.sheets[si];
            int txt = 0, n = 0;
            for (int c = 0; c < w.ncols(); ++c) { const Value& v = w.value(0, c); if (!v.is_empty()) { ++n; txt += v.is_str(); } }
            if (n >= 2 && txt * 10 >= n * 8 && w.nrows() > 2) w.freeze_rows = 1;
            int nr = w.nrows(), step = nr > 4000 ? nr / 4000 : 1;
            for (int c = 0; c < w.ncols() && c < 200; ++c) {
                float best = 0;
                for (int r = 0; r < nr; r += step) {
                    const Cell* ce = w.cell(r, c);
                    if (!ce || ce->v.is_empty()) continue;
                    if (w.ncols() > 1 && r < (int)w.rows.size() && w.rows[r].cells.size() == 1 && c == 0) continue;   // comment lines
                    best = std::max(best, text_px(d.b.display_text((int)si, r, c), 1.f, false) / dpi_scale());
                }
                if (best > 0) w.col(c).width = std::min(60.0, std::max(6.0, (best + 12.0) / 7.0));
            }
        }
    }
    d.saved_gen = d.b.gen;
}

std::unique_ptr<FileDoc> make_sheet_doc_book(Book bk, const std::string& name, const std::wstring& src_path) {
    auto d = std::make_unique<SheetDoc>();
    if (bk.sheets.empty()) bk.add_sheet("Sheet1");
    d->b = std::move(bk);
    d->name = name;
    d->path = src_path;
    prepare_new(*d, false);
    return d;
}

std::unique_ptr<FileDoc> make_sheet_doc_from(Workbook wb, const std::string& name, const std::wstring& src_path) {
    auto d = std::make_unique<SheetDoc>();
    if (wb.sheets.empty()) wb.sheets.push_back(Sheet{});
    d->b = from_legacy(wb);
    d->name = name;
    d->path = src_path;
    prepare_new(*d, true);
    return d;
}

// R11 P2-11: spreadsheet readability. The grid already follows the global UI scale (cell px =
// points x 96/72 x monitor DPI x UI scale x sheet zoom), but customer BOMs are typically set in
// 8-10 pt fonts with 10 pt rows, i.e. ~70% of the UI text size, which is why ~200% zoom was needed.
// A sheet stored at 100% opens at the zoom that brings its dominant font up to the UI text size
// (11 pt), capped at 160%; a zoom chosen by the user is remembered per document for the session.
static std::map<std::wstring, std::vector<int>> g_sheet_zoom;   // lower-cased path -> zoom per sheet
static std::wstring zoom_key(const std::wstring& p) { std::wstring k = p; for (auto& ch : k) ch = (wchar_t)towlower(ch); return k; }
static void remember_sheet_zoom(const FileDoc* f) {
    auto* sd = dynamic_cast<const SheetDoc*>(f);
    if (!sd || sd->path.empty()) return;
    std::vector<int> z;
    for (auto& w : sd->b.sheets) z.push_back(w.zoom);
    g_sheet_zoom[zoom_key(sd->path)] = z;
}
static void apply_readable_zoom(Book& bk, const std::wstring& path) {
    auto it = g_sheet_zoom.find(zoom_key(path));
    for (size_t si = 0; si < bk.sheets.size(); ++si) {
        auto& w = bk.sheets[si];
        if (it != g_sheet_zoom.end() && si < it->second.size()) { w.zoom = it->second[si]; continue; }
        if (w.zoom != 100) continue;   // the file has its own zoom: keep it
        std::map<int, int> hist;       // font size (0.5 pt) -> non-empty cells
        const int nr = std::min(400, (int)w.rows.size());
        for (int r = 0; r < nr; ++r)
            for (int c = 0; c < 40; ++c) {
                const Cell* ce = w.cell(r, c);
                if (!ce || w.value(r, c).is_empty()) continue;
                ++hist[(int)std::lround(bk.style_of((int)si, r, c).font.size * 2)];
            }
        int best = 0, dom = 22;
        for (auto& [k, n] : hist) if (n > best) { best = n; dom = k; }
        const double pt = std::max(4.0, dom / 2.0);
        if (pt < 10.5) w.zoom = std::clamp((int)std::lround(11.0 / pt * 100 / 5) * 5, 100, 160);
    }
}

std::unique_ptr<FileDoc> make_sheet_doc(const std::wstring& path, std::string& err) {
    std::vector<uint8_t> data;
    if (!read_file(ui::U(path), data, err)) return nullptr;
    Book bk;
    std::string how;
    bool ro = false, text = false;
    std::string e = ext_lower(path);
    if (io::is_ods_bytes(data)) { if (!io::ods_read(data, bk, err)) return nullptr; how = "OpenDocument spreadsheet (.ods)"; }
    else if (io::is_xlsx_bytes(data)) { if (!io::xlsx_read(data, bk, err)) return nullptr; how = "Excel workbook (.xlsx)"; }
    else if (io::is_ole2_bytes(data)) { if (!io::xls_read(data, bk, err)) return nullptr; how = "Excel 97-2003 (.xls, read-only: saves as .xlsx)"; ro = true; }
    else if (e == "csv" || e == "tsv") {
        io::CsvOptions o; o.sep = 0;
        if (!io::csv_read(data, bk, 0, o, err)) return nullptr;
        std::string fn = ui::U(file_name_of(path));
        size_t k = fn.find_last_of('.');
        bk.sheets[0].name = (k == std::string::npos ? fn : fn.substr(0, k)).substr(0, 31);
        how = e == "csv" ? "CSV" : "Tab separated";
        text = true;
    } else {
        Workbook wb;
        Sheet s = sheet_from_text(std::string(data.begin(), data.end()), &how);
        std::string fn = ui::U(file_name_of(path));
        size_t k = fn.find_last_of('.');
        s.name = (k == std::string::npos ? fn : fn.substr(0, k)).substr(0, 31);
        wb.sheets.push_back(std::move(s));
        bk = from_legacy(wb);
        how = "Text: " + how;
        text = true;
    }
    auto d = std::make_unique<SheetDoc>();
    d->b = std::move(bk);
    if (d->b.sheets.empty()) d->b.add_sheet("Sheet1");
    d->name = ui::U(file_name_of(path));
    d->path = path;
    d->how = how;
    d->readonly_src = ro;
    prepare_new(*d, text);
    apply_readable_zoom(d->b, path);
    return d;
}

// ===========================================================================
// Editors workspace (internal names files_*)
// ===========================================================================
static std::vector<std::unique_ptr<FileDoc>> g_files;
static int g_cur = -1;
static bool g_active = false;
static int g_select = -1;   // tab to force-select next frame

bool files_any() { return !g_files.empty(); }
bool files_active() { return g_active && !g_files.empty(); }
void files_set_active(bool on) { g_active = on && !g_files.empty(); request_redraw(2); }

void files_add(std::unique_ptr<FileDoc> d) {
    if (!d) return;
    g_files.push_back(std::move(d));
    g_cur = (int)g_files.size() - 1;
    g_select = g_cur;
    g_active = true;
    request_redraw(3);
}

static bool close_file(int i) {
    if (i < 0 || i >= (int)g_files.size()) return true;
    FileDoc* f = g_files[i].get();
    if (f->dirty()) {
        int ch = ui::prompt_choice(app::frame_hwnd(), L"Close", ui::W(f->name) + L" has unsaved changes.",
                                   {L"Save changes", L"Discard changes"}, 0);
        if (ch < 0) return false;
        if (ch == 0 && !f->save(false)) return false;
    }
    remember_sheet_zoom(f);   // per-document zoom survives close / reopen (session)
    g_files.erase(g_files.begin() + i);
    if (g_cur >= (int)g_files.size()) g_cur = (int)g_files.size() - 1;
    g_select = g_cur;
    if (g_files.empty()) g_active = false;
    request_redraw(2);
    return true;
}
void files_close_current_doc() { close_file(g_cur); }

static int find_open_file(const std::wstring& path) {
    for (size_t i = 0; i < g_files.size(); ++i)
        if (!g_files[i]->path.empty() && !lstrcmpiW(g_files[i]->path.c_str(), path.c_str())) return (int)i;
    return -1;
}

// mode: 0 auto (extension, then content), 1 spreadsheet, 2 text, 3 rich document, 4 email, 5 hex inspector
static int kind_by_ext(const std::string& e) {
    if (e == "txt" || e == "prn" || e == "log" || e == "ini" || e == "xml" || e == "json" || e == "rep" || e == "mnt" || e == "pos" || e == "cfg" || e == "md") return 2;
    if (e == "xlsx" || e == "xlsm" || e == "xls" || e == "ods" || e == "csv" || e == "tsv" || e == "bom") return 1;
    if (e == "rtf" || e == "docx" || e == "dotx" || e == "htm" || e == "html" || e == "doc") return 3;
    if (e == "eml" || e == "msg") return 4;
    return 0;
}

bool files_open(const std::wstring& path, int mode) {
    int k = find_open_file(path);
    if (k >= 0 && (mode == 0 || (mode == 1) == g_files[k]->is_sheet())) { g_cur = g_select = k; g_active = true; request_redraw(2); return true; }
    std::string e = ext_lower(path), err;
    if (mode == 0) mode = kind_by_ext(e);
    if (mode == 0) { mode = sniff_document_kind(path); if (mode == 0) mode = 2; }   // unknown extension: by content
    // R11 P2-11: never show a package / binary file as text. A forced text or spreadsheet open of a
    // Word package (PK zip with word/) goes to the document viewer; other binaries to the inspector.
    if (mode == 1 || mode == 2) {
        const int sn = sniff_document_kind(path);
        if (sn == 3) mode = 3;
        else if (mode == 2 && (sn == 5 || sn == 4)) mode = sn;
    }
    std::unique_ptr<FileDoc> d;
    std::wstring fname = file_name_of(path);
    bool ok = run_with_progress_wait("Opening file", [&](Progress& pr) {
        pr.text(ui::U(fname).c_str());
        switch (mode) {
        case 1: d = make_sheet_doc(path, err); break;
        case 3: d = make_rich_doc(path, err); break;
        case 4: d = make_mail_doc(path, err); break;
        case 5: d = make_hex_doc(path, err); break;
        default: d = make_text_doc(path, err); break;
        }
    }, ProgressOpts{true, false, 0.2f});
    if (ok && !d && (mode == 3 || mode == 4)) {   // not what the extension claimed: inspect safely instead
        std::string e2;
        d = make_hex_doc(path, e2);
        if (d) toast(ui::U(fname) + ": " + err + " \xE2\x80\x94 shown in the binary inspector", ToastKind::Warning, 6.f);
    }
    if (!ok || !d) { toast("Could not open " + ui::U(fname) + (err.empty() ? "" : ": " + err), ToastKind::Error, 6.f); return false; }
    FileDoc* raw = d.get();
    files_add(std::move(d));
    if (const wchar_t* ai = _wgetenv(L"PCBP_EDITOR_IMPORT")) raw->import_as(ai[0] == L'b');   // automation: b = BOM, c = centroid
    return true;
}

static bool tabular_ext(const std::string& e) {
    return e == "xlsx" || e == "xlsm" || e == "xls" || e == "ods" || e == "csv" || e == "tsv" || e == "txt" || e == "prn" || e == "bom";
}

bool files_route_open(const std::wstring& path) {
    std::string e = ext_lower(path);
    if (kind_by_ext(e) >= 3 || e == "log") { files_open(path, 0); return true; }   // documents / email: built-in viewers
    if (!tabular_ext(e)) return false;
    if (e == "xlsx" || e == "xlsm" || e == "xls" || e == "ods") {   // no importer reads workbooks: editor, then "Send to Project"
        files_open(path, 1);
        return true;
    }
    int ch = ui::prompt_choice(app::frame_hwnd(), L"Open " + file_name_of(path),
                               L"Open this file in an editor to view / clean it up, or import it?",
                               {L"Open in Spreadsheet editor", L"Open in Text editor", L"Import (BOM / centroid / CAD importer)"}, 0);
    if (ch < 0) return true;
    if (ch == 2) return false;
    files_open(path, ch == 0 ? 1 : 2);
    return true;
}

void files_open_dialog(int mode) {
    std::wstring p = app::settings().dir_bom;
    if (!ui::open_file_dialog(app::frame_hwnd(), mode == 2 ? L"Open in Text Editor" : L"Open Document",
                              {{L"Documents, spreadsheets and email", L"*.xlsx;*.xls;*.ods;*.csv;*.tsv;*.txt;*.prn;*.bom;*.pos;*.xy;*.mnt;*.rep;*.log;*.rtf;*.docx;*.eml;*.msg;*.htm;*.html"},
                               {L"BOM / centroid files", L"*.xlsx;*.xls;*.ods;*.csv;*.tsv;*.txt;*.prn;*.bom;*.pos;*.xy;*.mnt;*.rep"},
                               {L"Spreadsheets", L"*.xlsx;*.xls;*.ods"}, {L"Email (*.eml, *.msg)", L"*.eml;*.msg"}, {L"Documents (*.rtf, *.docx, *.txt)", L"*.rtf;*.docx;*.txt;*.log"},
                               {L"All files", L"*.*"}}, p)) return;
    files_open(p, mode);
}

void files_new_sheet() {
    Book bk;
    static int n = 1;
    files_add(make_sheet_doc_book(std::move(bk), "Book" + std::to_string(n++), L""));
}

// ---------------------------------------------------------------------------
// Project-linked documents (semantics: specs/DOCS_EVAL.md §4). A link is a Project::SourceRef with
// importer "document"; settings["mode"] = "reference" (points at the original file) or "copy"
// (file copied next to the project, the link points at the copy). Links are saved with the project;
// document content is never embedded in the .pcbp.
static bool same_path(const std::string& a, const std::wstring& b) { return !b.empty() && !lstrcmpiW(ui::W(a).c_str(), b.c_str()); }
static const Project::SourceRef* doc_link(const app::DocPtr& d, const std::wstring& path) {
    if (!d || path.empty()) return nullptr;
    for (auto& s : d->prj.sources) if (s.importer == "document" && (same_path(s.path, path) || same_path(s.settings.count("original") ? s.settings.at("original") : std::string(), path))) return &s;
    return nullptr;
}
static app::DocPtr link_target() { return app::active_doc(); }

static void attach_document(FileDoc* f, bool copy) {
    app::DocPtr d = link_target();
    if (!d) { toast("Open or create a project first, then attach the document to it.", ToastKind::Warning, 5.f); return; }
    if (f->path.empty() || f->dirty()) {
        if (!f->save(f->path.empty())) return;   // a link needs a file on disk
    }
    if (doc_link(d, f->path)) { toast("Already linked to " + ui::U(d->display_name()), ToastKind::Info, 3.f); return; }
    std::wstring target = f->path;
    if (copy) {
        if (d->prj.path.empty()) { toast("Save the project first: copies are stored in a folder next to the project file.", ToastKind::Warning, 6.f); return; }
        std::wstring pp = ui::W(d->prj.path);
        size_t k = pp.find_last_of(L"\\/");
        std::wstring dir = (k == std::wstring::npos ? std::wstring() : pp.substr(0, k + 1)), stem = pp.substr(k + 1);
        size_t dot = stem.find_last_of(L'.');
        if (dot != std::wstring::npos) stem.resize(dot);
        dir += stem + L" documents";
        CreateDirectoryW(dir.c_str(), nullptr);
        target = dir + L"\\" + file_name_of(f->path);
        if (lstrcmpiW(target.c_str(), f->path.c_str()) && !CopyFileW(f->path.c_str(), target.c_str(), FALSE)) { toast("Could not copy the document next to the project.", ToastKind::Error, 5.f); return; }
    }
    Project::SourceRef sr;
    sr.path = ui::U(target);
    sr.importer = "document";
    char ts[32]; std::time_t now = std::time(nullptr); std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
    sr.imported_at = ts;
    sr.settings["mode"] = copy ? "copy" : "reference";
    sr.settings["kind"] = f->kind();
    if (copy) sr.settings["original"] = ui::U(f->path);
    sr.summary = std::string("Linked document: ") + f->name;
    d->prj.sources.push_back(sr);
    d->prj.dirty = true;
    toast(f->name + " linked to project " + ui::U(d->display_name()) + (copy ? " (copy stored next to the project)" : " (reference)"), ToastKind::Success, 4.f);
    request_redraw(2);
}
static void detach_document(FileDoc* f) {
    app::DocPtr d = link_target();
    if (!d) return;
    auto& v = d->prj.sources;
    size_t n = v.size();
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Project::SourceRef& s) { return s.importer == "document" && (same_path(s.path, f->path) || (s.settings.count("original") && same_path(s.settings.at("original"), f->path))); }), v.end());
    if (v.size() != n) { d->prj.dirty = true; toast(f->name + " is no longer linked to the project (files are not deleted)", ToastKind::Info, 4.f); }
}

void files_project_menu_items() {
    app::DocPtr d = link_target();
    FileDoc* f = (g_cur >= 0 && g_cur < (int)g_files.size()) ? g_files[g_cur].get() : nullptr;
    bool can = d && f;
    if (menu_item_icon("Attach Open Document to Project (reference)", ui::Icon::Link, nullptr, false, can && !doc_link(d, f->path))) { FileDoc* ff = f; defer([ff]() { attach_document(ff, false); }); }
    if (menu_item_icon("Attach Open Document to Project (store a copy)", ui::Icon::Folder, nullptr, false, can && !doc_link(d, f->path))) { FileDoc* ff = f; defer([ff]() { attach_document(ff, true); }); }
    std::vector<std::string> links;
    if (d) for (auto& s : d->prj.sources) if (s.importer == "document") links.push_back(s.path);
    if (begin_menu_icon("Project Documents", ui::Icon::Notes, !links.empty())) {
        for (auto& l : links) {
            std::wstring w = ui::W(l);
            bool exists = GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES;
            if (menu_item_icon((ui::U(file_name_of(w)) + (exists ? "" : "  (missing)")).c_str(), ui::Icon::Open, nullptr, false, exists)) defer([w]() { files_open(w, 0); });
            if (ImGui::IsItemHovered()) tooltip(l.c_str());
        }
        ImGui::EndMenu();
    }
}

// Top-level document tabs (REVIEW_V9 §10): emitted by the shell inside its document tab bar, one
// per open file document, labelled with a type icon + type name. Active one is highlighted.
void files_doc_tabs() {
    // P1-10: same tab style as project tabs (gui::tab_item: icon, close x, middle-click, context menu); placed by the shell's tab strip
    const float vw = tab_strip_width();   // 0 = horizontal strip
    if (!g_files.empty()) {   // R21: the open-editor tabs are the "Editors" area (the project's Files tab is a different thing)
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Editors");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Editors: spreadsheets, documents, e-mail and program editors opened outside a project.\n"
                                                      "Project files (the job archive) are on the project's Files tab.");
        if (vw <= 0) ImGui::SameLine(0, S(6));
    }
    for (int i = 0; i < (int)g_files.size(); ++i) {
        FileDoc* f = g_files[i].get();
        bool cur = g_active && i == g_cur;
        app::DocPtr d = link_target();
        bool linked = doc_link(d, f->path) != nullptr;
        std::string lbl = f->name + (linked ? "  [linked]" : "");
        std::string tip = "Editors: " + std::string(f->kind()) + " \xE2\x80\x94 " + f->name + (f->path.empty() ? "\n(not saved)" : "\n" + ui::U(f->path)) +
                          (linked ? "\nProject-linked: " + ui::U(d->display_name()) : "\nOpen document (not part of a project)") + "\nRight-click for more";
        TabHit h = tab_item(("##ftab" + std::to_string((uintptr_t)f)).c_str(), lbl, f->icon(), cur, f->dirty(), tip.c_str(), vw);
        if (h.clicked) { g_cur = g_select = i; g_active = true; request_redraw(2); }
        if (h.close) { int k = i; defer([k]() { close_file(k); }); }
        if (ImGui::BeginPopupContextItem(nullptr)) {
            if (menu_item_icon("Close", ui::Icon::CloseDoc)) { int k = i; defer([k]() { close_file(k); }); }
            if (menu_item_icon("Close Others", ui::Icon::CloseDoc, nullptr, false, g_files.size() > 1)) { FileDoc* keep = f; defer([keep]() { for (int k = (int)g_files.size() - 1; k >= 0; --k) if (g_files[k].get() != keep) close_file(k); }); }
            ImGui::Separator();
            if (menu_item_icon("Save", ui::Icon::Save)) { int k = i; defer([k]() { if (k < (int)g_files.size()) g_files[k]->save(false); }); }
            if (menu_item_icon("Save As\xE2\x80\xA6", ui::Icon::SaveAs)) { int k = i; defer([k]() { if (k < (int)g_files.size()) g_files[k]->save(true); }); }
            ImGui::Separator();
            if (!linked && menu_item_icon("Attach to Project (reference)", ui::Icon::Link, nullptr, false, d != nullptr)) defer([f]() { attach_document(f, false); });
            if (!linked && menu_item_icon("Attach to Project (store a copy)", ui::Icon::Folder, nullptr, false, d != nullptr)) defer([f]() { attach_document(f, true); });
            if (linked && menu_item_icon("Detach from Project", ui::Icon::Unassign)) defer([f]() { detach_document(f); });
            if (!f->path.empty() && menu_item_icon("Copy Path", ui::Icon::Copy)) ImGui::SetClipboardText(ui::U(f->path).c_str());
            ImGui::EndPopup();
        }
        if (vw <= 0) ImGui::SameLine(0, S(4));
    }
}

bool files_on_command(int id) {
    if (!files_active() || g_cur < 0) return false;
    using namespace app;
    FileDoc* f = g_files[g_cur].get();
    switch (id) {
    case ID_FILE_SAVE: f->save(false); return true;
    case ID_FILE_SAVEAS: f->save(true); return true;
    case ID_FILE_CLOSE: close_file(g_cur); return true;
    }
    return f->on_command(id);
}

void files_draw(ImVec2 size) {
    const GTheme& t = th();
    if (g_files.empty()) { g_active = false; return; }
    if (g_cur < 0 || g_cur >= (int)g_files.size()) g_cur = 0;
    g_select = -1;
    FileDoc* f = g_files[g_cur].get();
    // document header: type icon + label, name, path, project-link badge, actions
    const float HB = S(34);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + HB), t.panel);
    dl->AddLine(ImVec2(p0.x, p0.y + HB - 1), ImVec2(p0.x + size.x, p0.y + HB - 1), t.border);
    icon_draw(dl, f->icon(), ImVec2(p0.x + S(10), p0.y + (HB - S(20)) * 0.5f), S(20));
    float x = p0.x + S(38), ty = p0.y + (HB - ImGui::GetFontSize()) * 0.5f;
    std::string kind = f->kind();
    ImVec2 ks = ImGui::CalcTextSize(kind.c_str());
    dl->AddRectFilled(ImVec2(x - S(4), ty - S(2)), ImVec2(x + ks.x + S(4), ty + ks.y + S(2)), col_alpha(t.accent, 0.18f), S(4));
    dl->AddText(ImVec2(x, ty), t.text, kind.c_str());
    x += ks.x + S(14);
    std::string nm = f->name + (f->dirty() ? " *" : "");
    dl->AddText(g_font_bold, g_font_bold->FontSize, ImVec2(x, ty), t.text, nm.c_str());
    x += g_font_bold->CalcTextSizeA(g_font_bold->FontSize, FLT_MAX, 0, nm.c_str()).x + S(12);
    app::DocPtr d = link_target();
    const Project::SourceRef* link = doc_link(d, f->path);
    float right = p0.x + size.x - S(8);
    // actions (right aligned)
    ImGui::SetCursorScreenPos(ImVec2(right - S(30), p0.y + S(3)));
    if (icon_button("##fclose", ui::Icon::CloseDoc, "Close document (Ctrl+W)", false, true, S(28))) { int k = g_cur; defer([k]() { close_file(k); }); }
    ImGui::SetCursorScreenPos(ImVec2(right - S(62), p0.y + S(3)));
    if (ImGui::Button("+##fnew", ImVec2(S(28), S(28)))) ImGui::OpenPopup("##newfile");
    if (ImGui::IsItemHovered()) tooltip("New spreadsheet / open another document");
    if (ImGui::BeginPopup("##newfile")) {
        if (menu_item_icon("New blank spreadsheet", ui::Icon::New)) defer([]() { files_new_sheet(); });
        if (menu_item_icon("Open document\xE2\x80\xA6", ui::Icon::Open)) defer([]() { files_open_dialog(0); });
        ImGui::EndPopup();
    }
    float bx = right - S(70);
    if (link) {
        std::string badge = std::string("Project-linked \xC2\xB7 ") + ui::U(d->display_name()) + (link->settings.count("mode") && link->settings.at("mode") == "copy" ? " (copy)" : "");
        ImVec2 bs = ImGui::CalcTextSize(badge.c_str());
        float bx0 = bx - bs.x - S(16);
        dl->AddRectFilled(ImVec2(bx0, ty - S(3)), ImVec2(bx, ty + bs.y + S(3)), col_alpha(t.ok, 0.22f), S(8));
        dl->AddText(ImVec2(bx0 + S(8), ty), t.text, badge.c_str());
        ImGui::SetCursorScreenPos(ImVec2(bx0, ty - S(3)));
        ImGui::InvisibleButton("##linkbadge", ImVec2(bx - bx0, bs.y + S(6)));
        if (ImGui::IsItemHovered()) tooltip(("Linked to the project as " + link->settings.at("mode") + ":\n" + link->path + "\nSaved with the project (Project Documents menu). Right-click to detach.").c_str());
        if (ImGui::BeginPopupContextItem("##linkctx")) { if (menu_item_icon("Detach from Project", ui::Icon::Unassign)) defer([f]() { detach_document(f); }); ImGui::EndPopup(); }
        bx = bx0 - S(8);
    } else if (d) {
        const char* lbl = "Attach to Project\xE2\x80\xA6";
        float bw = ImGui::CalcTextSize(lbl).x + S(20);
        ImGui::SetCursorScreenPos(ImVec2(bx - bw, p0.y + S(4)));
        if (ImGui::Button(lbl, ImVec2(bw, S(26)))) ImGui::OpenPopup("##attach");
        if (ImGui::IsItemHovered()) tooltip(("Link this document to project " + ui::U(d->display_name()) + ".\nReference: remembers where the file is.\nStore a copy: copies it into a folder next to the project file.").c_str());
        if (ImGui::BeginPopup("##attach")) {
            if (menu_item_icon("Reference the file", ui::Icon::Link)) defer([f]() { attach_document(f, false); });
            if (menu_item_icon("Store a copy next to the project", ui::Icon::Folder)) defer([f]() { attach_document(f, true); });
            ImGui::EndPopup();
        }
        bx -= bw + S(8);
    }
    if (!f->path.empty() && x < bx - S(40)) {
        std::string ps = ui::U(f->path);
        dl->PushClipRect(ImVec2(x, p0.y), ImVec2(bx - S(6), p0.y + HB), true);
        dl->AddText(ImVec2(x, ty), t.text_dim, ps.c_str());
        dl->PopClipRect();
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + HB));
    ImGui::PushID(f);
    ImVec2 avail(size.x, size.y - HB);
    ImGui::BeginChild("##filedoc", avail, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    f->draw(ImGui::GetContentRegionAvail());
    ImGui::EndChild();
    ImGui::PopID();
    static std::string last;
    std::string s = f->status();
    if (s != last) { last = s; status(s); }
}

} // namespace pcbp::gui
