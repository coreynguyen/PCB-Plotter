// ============================================================================
// sheet_model.cpp — Worksheet / Book model, style table, editing operations
// (all undoable through command objects), structural edits with reference
// adjustment, sort, autofilter, fill series, internal clipboard, legacy bridge.
// ============================================================================
#include "formula_internal.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <numeric>
#include <set>
#include <cmath>

namespace pcbp::sheet {

// ================================================================= styles ==
bool Font::operator==(const Font& o) const {
    return name == o.name && size == o.size && bold == o.bold && italic == o.italic && strike == o.strike && underline == o.underline && color == o.color;
}
bool Border::operator==(const Border& o) const {
    return left == o.left && right == o.right && top == o.top && bottom == o.bottom && diagonal == o.diagonal && diag_up == o.diag_up && diag_down == o.diag_down;
}
bool Style::operator==(const Style& o) const {
    return font == o.font && fill == o.fill && border == o.border && numfmt == o.numfmt && halign == o.halign && valign == o.valign &&
           wrap == o.wrap && shrink == o.shrink && indent == o.indent && rotation == o.rotation && locked == o.locked && hidden == o.hidden;
}
static uint64_t style_hash(const Style& s) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    for (char ch : s.font.name) mix((unsigned char)ch);
    mix((uint64_t)(s.font.size * 100)); mix((uint64_t)((int)s.font.bold | (int)s.font.italic << 1 | (int)s.font.strike << 2 | (int)s.font.underline << 3)); mix(s.font.color);
    mix((int)s.fill.pattern); mix(s.fill.fg); mix(s.fill.bg);
    for (const BorderSide* b : {&s.border.left, &s.border.right, &s.border.top, &s.border.bottom, &s.border.diagonal}) { mix((int)b->style); mix(b->color); }
    for (char ch : s.numfmt) mix((unsigned char)ch);
    mix((int)s.halign | (int)s.valign << 4 | s.wrap << 8 | s.shrink << 9 | s.locked << 10 | s.hidden << 11); mix(s.indent); mix((uint64_t)(int64_t)s.rotation);
    return h;
}
StyleTable::StyleTable() { clear(); }
void StyleTable::clear() { v_.assign(1, Style()); idx_.clear(); idx_.emplace(style_hash(v_[0]), 0); }
uint32_t StyleTable::intern(const Style& s) {
    uint64_t h = style_hash(s);
    auto range = idx_.equal_range(h);
    for (auto it = range.first; it != range.second; ++it) if (v_[it->second] == s) return it->second;
    v_.push_back(s);
    idx_.emplace(h, (uint32_t)(v_.size() - 1));
    return (uint32_t)(v_.size() - 1);
}

// ============================================================== worksheet ==
const Cell* Worksheet::cell(int r, int c) const {
    if (r < 0 || c < 0 || r >= (int)rows.size()) return nullptr;
    const auto& v = rows[r].cells;
    if (c >= (int)v.size()) return nullptr;
    return &v[c];
}
const Value& Worksheet::value(int r, int c) const {
    static const Value empty;
    const Cell* cl = cell(r, c);
    if (cl && (cl->f || cl->v.type != Value::Empty)) return cl->v;
    for (auto& [k, arr] : spills)
        if (r >= k.first && c >= k.second && r < k.first + arr->rows && c < k.second + arr->cols) return arr->at(r - k.first, c - k.second);
    return empty;
}
uint32_t Worksheet::style_at(int r, int c) const {
    const Cell* cl = cell(r, c);
    if (cl && cl->style) return cl->style;
    if (r >= 0 && r < (int)rows.size() && rows[r].style) return rows[r].style;
    if (c >= 0 && c < (int)cols.size()) return cols[c].style;
    return 0;
}
int Worksheet::nrows() const {
    int n = (int)rows.size();
    while (n > 0) {
        const RowStore& rs = rows[n - 1];
        bool any = rs.height >= 0 || rs.hidden || rs.style || rs.filtered;
        for (auto& c : rs.cells) if (!c.blank() || c.style) { any = true; break; }
        if (any) break;
        --n;
    }
    return n;
}
int Worksheet::ncols() const {
    int n = 0;
    for (auto& r : rows) {
        int k = (int)r.cells.size();
        while (k > n && r.cells[k - 1].blank() && !r.cells[k - 1].style) --k;
        n = std::max(n, k);
    }
    return n;
}
Range Worksheet::used_range() const {
    int r0 = -1, r1 = -1, c0 = MAX_COLS, c1 = -1;
    for (int r = 0; r < (int)rows.size(); ++r) {
        const auto& v = rows[r].cells;
        for (int c = 0; c < (int)v.size(); ++c) if (!v[c].blank()) {
            if (r0 < 0) r0 = r;
            r1 = r; c0 = std::min(c0, c); c1 = std::max(c1, c);
        }
    }
    if (r0 < 0) return Range();
    return Range(r0, c0, r1, c1);
}
double Worksheet::col_width(int c) const { return c < (int)cols.size() && cols[c].width >= 0 ? cols[c].width : default_col_width; }
double Worksheet::row_height(int r) const { return r < (int)rows.size() && rows[r].height >= 0 ? rows[r].height : default_row_height; }
const Range* Worksheet::merge_at(int r, int c) const {
    for (auto& m : merges) if (m.contains(r, c)) return &m;
    return nullptr;
}
std::pair<int, int> Worksheet::spill_anchor(int r, int c) const {
    for (auto& [k, arr] : spills)
        if (r >= k.first && c >= k.second && r < k.first + arr->rows && c < k.second + arr->cols) return k;
    return {-1, -1};
}
Cell& Worksheet::at(int r, int c) {
    if (r >= (int)rows.size()) rows.resize(r + 1);
    auto& v = rows[r].cells;
    if (c >= (int)v.size()) v.resize(c + 1);
    return v[c];
}
void Worksheet::set_value_raw(int r, int c, Value v) { Cell& cl = at(r, c); cl.v = std::move(v); cl.f.reset(); }
void Worksheet::clear_raw(int r, int c) {
    if (r >= (int)rows.size() || c >= (int)rows[r].cells.size()) return;
    rows[r].cells[c] = Cell();
}
RowStore& Worksheet::row(int r) { if (r >= (int)rows.size()) rows.resize(r + 1); return rows[r]; }
ColInfo& Worksheet::col(int c) { if (c >= (int)cols.size()) cols.resize(c + 1); return cols[c]; }

// ========================================================== undo commands ==
namespace {

struct Composite : Command {
    std::vector<std::unique_ptr<Command>> items;
    void undo(Book& b) override { for (auto it = items.rbegin(); it != items.rend(); ++it) (*it)->undo(b); }
    void redo(Book& b) override { for (auto& c : items) c->redo(b); }
};

} // namespace

struct CellsCommand : Command {
    struct E { int si, r, c; Cell cell; };
    std::vector<E> e;
    void swap_all(Book& b) {
        if (e.size() >= 5000) b.invalidate_graph();   // cheaper to rebuild + full recalc
        bool gv = b.calc().graph_valid;
        for (auto& x : e) {
            if (x.si >= b.sheet_count()) continue;
            Cell& cur = b.sheets[x.si].at(x.r, x.c);
            if (gv && cur.f) graph_remove(b, x.si, x.r, x.c, *cur.f);
            std::swap(cur, x.cell);
            cur.calc = 0;
            if (gv && cur.f) graph_add(b, x.si, x.r, x.c, *cur.f);
            b.mark_dirty(x.si, x.r, x.c);
        }
    }
    void undo(Book& b) override { swap_all(b); }
    void redo(Book& b) override { swap_all(b); }
};

struct BookSnapshotCommand : Command {
    // saved sheets by index (all sheets when `all`), names, dxfs, sheet order
    bool all = false;
    std::vector<std::pair<int, Worksheet>> saved;
    std::vector<Worksheet> all_sheets;
    std::vector<DefinedName> names;
    std::vector<Dxf> dxfs;
    int active = 0;
    void swap_all(Book& b) {
        if (all) { std::swap(b.sheets, all_sheets); }
        else for (auto& [i, ws] : saved) if (i < b.sheet_count()) std::swap(b.sheets[i], ws);
        std::swap(b.names, names);
        std::swap(b.dxfs, dxfs);
        std::swap(b.active_sheet, active);
        if (b.active_sheet >= b.sheet_count()) b.active_sheet = b.sheet_count() - 1;
        b.invalidate_graph();
    }
    void undo(Book& b) override { swap_all(b); }
    void redo(Book& b) override { swap_all(b); }
};

namespace {

std::unique_ptr<BookSnapshotCommand> snapshot(Book& b, const std::vector<int>& sheet_ix, const std::string& label, bool all = false) {
    auto c = std::make_unique<BookSnapshotCommand>();
    c->label = label;
    c->all = all;
    if (all) c->all_sheets = b.sheets;   // copy
    else for (int i : sheet_ix) if (i >= 0 && i < b.sheet_count()) c->saved.push_back({i, b.sheets[i]});
    c->names = b.names; c->dxfs = b.dxfs; c->active = b.active_sheet;
    return c;
}
// A snapshot command stores the BEFORE state; after mutation swap semantics flip correctly:
// undo swaps current (after) with saved (before). Nothing else to do.

std::vector<int> sheets_with_formulas(const Book& b, int also) {
    std::vector<int> v;
    for (int i = 0; i < b.sheet_count(); ++i) {
        if (i == also) { v.push_back(i); continue; }
        bool any = false;
        for (auto& r : b.sheets[i].rows) { for (auto& c : r.cells) if (c.f) { any = true; break; } if (any) break; }
        if (any) v.push_back(i);
    }
    return v;
}

} // namespace

struct Book::UndoState {
    std::vector<std::unique_ptr<Command>> undo, redo;
    int depth = 0;
    std::unique_ptr<Composite> batch;
    bool pending_structural = false, pending = false;
};

// ================================================================== book ==
Book::Book() : calc_(std::make_unique<Calc>()), undo_(std::make_unique<UndoState>()) {
    Worksheet ws; ws.name = "Sheet1"; ws.id = 1;
    sheets.push_back(std::move(ws));
}
Book::~Book() = default;
Book::Book(Book&&) noexcept = default;
Book& Book::operator=(Book&&) noexcept = default;

Book Book::clone() const {
    Book b;
    b.sheets = sheets;
    b.styles = styles;
    b.dxfs = dxfs;
    b.names = names;
    b.date1904 = date1904;
    b.auto_calc = auto_calc;
    b.active_sheet = active_sheet;
    for (auto& ws : b.sheets) for (auto& r : ws.rows) for (auto& c : r.cells) c.calc = 0;
    b.invalidate_graph();
    return b;
}

int Book::find_sheet(const std::string& name) const {
    for (int i = 0; i < sheet_count(); ++i) if (ci_equal(sheets[i].name, name)) return i;
    return -1;
}
int Book::sheet_index_of_id(uint32_t id) const {
    for (int i = 0; i < sheet_count(); ++i) if (sheets[i].id == id) return i;
    return -1;
}
const DefinedName* Book::find_name(const std::string& name, int scope_si) const {
    const DefinedName* wb = nullptr;
    for (auto& n : names) {
        if (!ci_equal(n.name, name)) continue;
        if (n.scope == scope_si) return &n;
        if (n.scope < 0) wb = &n;
    }
    return wb;
}

// ------------------------------------------------------------- display --
std::string Book::display_text(int si, int r, int c) const {
    const Worksheet& ws = sheets[si];
    const Value& v = ws.value(r, c);
    if (v.is_empty()) return {};
    return format_value(v, styles.get(ws.style_at(r, c)).numfmt, date1904).text;
}
std::string Book::input_text(int si, int r, int c) const {
    const Worksheet& ws = sheets[si];
    const Cell* cl = ws.cell(r, c);
    if (!cl) return {};
    if (cl->f) return "=" + formula_text(*cl->f, *this, si, r, c);
    const Value& v = cl->v;
    const std::string& fmt = styles.get(ws.style_at(r, c)).numfmt;
    if (v.is_num()) {
        if (is_date_format(fmt)) {
            double frac = v.num - std::floor(v.num);
            bool has_date = v.num >= 1, has_time = frac > 1e-9;
            std::string code = has_date && has_time ? "m/d/yyyy h:mm:ss" : has_date ? "m/d/yyyy" : "h:mm:ss";
            return format_value(v, code, date1904).text;
        }
        if (fmt.find('%') != std::string::npos && fmt.find('"') == std::string::npos) return fmt_general(v.num * 100) + "%";
        return fmt_general(v.num);
    }
    if (v.is_str()) {
        if (fmt == "@") return v.str;
        Value p = parse_input(v.str, nullptr, date1904);
        if (!p.is_str() || (!v.str.empty() && v.str[0] == '\'') ) return "'" + v.str;
        return v.str;
    }
    return value_to_text(v);
}

// ---------------------------------------------------------- undo/batch --
void Book::commit_(std::unique_ptr<Command> cmd, bool structural) {
    ++gen;
    auto& U = *undo_;
    if (structural) U.pending_structural = true;
    U.pending = true;
    if (cmd) {
        if (U.depth > 0) { if (!U.batch) { U.batch = std::make_unique<Composite>(); } U.batch->items.push_back(std::move(cmd)); }
        else {
            U.undo.push_back(std::move(cmd));
            U.redo.clear();
            if (U.undo.size() > undo_limit) U.undo.erase(U.undo.begin());
        }
    }
    if (U.depth == 0) {
        if (auto_calc) { if (U.pending_structural) recalc_all(); else recalc_dirty(); }
        U.pending = U.pending_structural = false;
    }
}
void Book::begin_batch(const std::string& label) {
    auto& U = *undo_;
    if (U.depth++ == 0) { U.batch = std::make_unique<Composite>(); U.batch->label = label; }
}
void Book::end_batch() {
    auto& U = *undo_;
    if (U.depth <= 0) return;
    if (--U.depth > 0) return;
    std::unique_ptr<Composite> b = std::move(U.batch);
    if (b && !b->items.empty()) {
        if (b->items.size() == 1 && b->label.empty()) U.undo.push_back(std::move(b->items[0]));
        else U.undo.push_back(std::move(b));
        U.redo.clear();
        if (U.undo.size() > undo_limit) U.undo.erase(U.undo.begin());
    }
    if (U.pending && auto_calc) { if (U.pending_structural) recalc_all(); else recalc_dirty(); }
    U.pending = U.pending_structural = false;
    ++gen;
}
bool Book::can_undo() const { return !undo_->undo.empty() && undo_->depth == 0; }
bool Book::can_redo() const { return !undo_->redo.empty() && undo_->depth == 0; }
std::string Book::undo_label() const { return undo_->undo.empty() ? std::string() : undo_->undo.back()->label; }
std::string Book::redo_label() const { return undo_->redo.empty() ? std::string() : undo_->redo.back()->label; }
bool Book::undo() {
    auto& U = *undo_;
    if (U.undo.empty() || U.depth) return false;
    auto c = std::move(U.undo.back()); U.undo.pop_back();
    c->undo(*this);
    U.redo.push_back(std::move(c));
    ++gen;
    if (auto_calc) recalc_dirty();
    return true;
}
bool Book::redo() {
    auto& U = *undo_;
    if (U.redo.empty() || U.depth) return false;
    auto c = std::move(U.redo.back()); U.redo.pop_back();
    c->redo(*this);
    U.undo.push_back(std::move(c));
    ++gen;
    if (auto_calc) recalc_dirty();
    return true;
}
void Book::clear_undo() { undo_->undo.clear(); undo_->redo.clear(); }
void Book::push_command(std::unique_ptr<Command> cmd, bool already_applied) {
    if (!already_applied) cmd->redo(*this);
    invalidate_graph();
    commit_(std::move(cmd), true);
}

// ----------------------------------------------------- cell edit helpers --
namespace {

// Install new cells: `e` holds the NEW cells; after swap_all it holds the old ones.
void apply_cells(Book& b, std::unique_ptr<CellsCommand> cmd) {
    if (cmd->e.empty()) return;
    cmd->swap_all(b);
    b.commit_(std::move(cmd), false);
}

Cell make_value_cell(const Cell* old, Value v) {
    Cell c;
    if (old) c.style = old->style;
    c.v = std::move(v);
    return c;
}

} // namespace

bool Book::set_formula_raw(int si, int r, int c, const std::string& text, const Value* cached) {
    auto f = std::make_shared<Formula>();
    std::string err;
    bool ok = compile_formula(text, *this, si, r, c, *f, err);
    Cell& cl = sheets[si].at(r, c);
    cl.f = f;
    cl.v = cached ? *cached : Value();
    cl.calc = cached ? 0 : CS_DIRTY;
    if (!cached) mark_dirty(si, r, c);
    calc_->graph_valid = false;
    return ok;
}

void Book::set_value(int si, int r, int c, const Value& v) {
    auto cmd = std::make_unique<CellsCommand>();
    cmd->label = "Edit cell";
    cmd->e.push_back({si, r, c, make_value_cell(sheets[si].cell(r, c), v)});
    apply_cells(*this, std::move(cmd));
}

bool Book::set_formula(int si, int r, int c, const std::string& text0, std::string* err) {
    std::string text = text0;
    if (!text.empty() && text[0] == '=') text.erase(0, 1);
    auto f = std::make_shared<Formula>();
    std::string e;
    if (!compile_formula(text, *this, si, r, c, *f, e)) { if (err) *err = e; return false; }
    const Cell* old = sheets[si].cell(r, c);
    Cell nc;
    if (old) nc.style = old->style;
    nc.f = f;
    auto cmd = std::make_unique<CellsCommand>();
    cmd->label = "Edit cell";
    cmd->e.push_back({si, r, c, std::move(nc)});
    apply_cells(*this, std::move(cmd));
    return true;
}

static Cell input_cell(Book& b, int si, int r, int c, const std::string& text) {
    const Worksheet& ws = b.sheets[si];
    const Cell* old = ws.cell(r, c);
    Cell nc;
    if (old) nc.style = old->style;
    if (text.empty()) return nc;
    const Style& st = b.styles.get(ws.style_at(r, c));
    if (text.size() > 1 && text[0] == '=' && st.numfmt != "@") {
        auto f = std::make_shared<Formula>();
        std::string e;
        if (compile_formula(text.substr(1), b, si, r, c, *f, e)) { nc.f = f; return nc; }
        nc.v = Value::text(text);
        return nc;
    }
    if (st.numfmt == "@") { nc.v = Value::text(text); return nc; }
    std::string hint;
    nc.v = parse_input(text, &hint, b.date1904);
    if (!hint.empty() && (st.numfmt.empty() || st.numfmt == "General")) {
        Style s2 = st;
        s2.numfmt = hint;
        nc.style = b.styles.intern(s2);
    }
    return nc;
}

void Book::set_input(int si, int r, int c, const std::string& text) {
    auto cmd = std::make_unique<CellsCommand>();
    cmd->label = "Edit cell";
    cmd->e.push_back({si, r, c, input_cell(*this, si, r, c, text)});
    apply_cells(*this, std::move(cmd));
}

void Book::set_inputs(int si, int r, int c, const std::vector<std::vector<std::string>>& block) {
    auto cmd = std::make_unique<CellsCommand>();
    cmd->label = "Enter data";
    for (size_t i = 0; i < block.size(); ++i)
        for (size_t j = 0; j < block[i].size(); ++j) {
            int rr = r + (int)i, cc = c + (int)j;
            if (rr >= MAX_ROWS || cc >= MAX_COLS) continue;
            if (block[i][j].empty() && !sheets[si].cell(rr, cc)) continue;
            cmd->e.push_back({si, rr, cc, input_cell(*this, si, rr, cc, block[i][j])});
        }
    apply_cells(*this, std::move(cmd));
}

static Range clip_to_used(const Worksheet& ws, Range rg) {
    rg = rg.norm();
    int nr = (int)ws.rows.size();
    if (rg.r1 >= nr) rg.r1 = nr - 1;
    if (rg.c1 >= MAX_COLS - 1) rg.c1 = std::max(rg.c0, ws.ncols() - 1);
    return rg;
}

void Book::clear(int si, const Range& rg0, unsigned what) {
    Worksheet& ws = sheets[si];
    Range rg = clip_to_used(ws, rg0);
    auto cmd = std::make_unique<CellsCommand>();
    cmd->label = what == ClearFormats ? "Clear formats" : what == ClearContents ? "Clear contents" : "Clear";
    for (int r = rg.r0; r <= rg.r1; ++r) {
        if (r >= (int)ws.rows.size()) break;
        auto& cells = ws.rows[r].cells;
        for (int c = rg.c0; c <= rg.c1 && c < (int)cells.size(); ++c) {
            const Cell& cl = cells[c];
            if (cl.blank() && (!(what & ClearFormats) || !cl.style)) continue;
            Cell nc;
            if (!(what & ClearContents)) { nc.v = cl.v; nc.f = cl.f; }
            if (!(what & ClearFormats)) nc.style = cl.style;
            cmd->e.push_back({si, r, c, std::move(nc)});
        }
    }
    bool whole = rg0.norm().r1 >= MAX_ROWS - 1 || rg0.norm().c1 >= MAX_COLS - 1;
    if ((what & ClearFormats) && whole) {
        // drop column / row default styles too (snapshot for undo)
        begin_batch(cmd->label);
        auto snap = snapshot(*this, {si}, cmd->label);
        Range g = rg0.norm();
        if (g.r1 >= MAX_ROWS - 1) for (int c = g.c0; c <= std::min(g.c1, (int)ws.cols.size() - 1); ++c) ws.cols[c].style = 0;
        if (g.c1 >= MAX_COLS - 1) for (int r = g.r0; r <= std::min(g.r1, (int)ws.rows.size() - 1); ++r) ws.rows[r].style = 0;
        commit_(std::move(snap), true);
        apply_cells(*this, std::move(cmd));
        end_batch();
        return;
    }
    apply_cells(*this, std::move(cmd));
}

void Book::apply_style(int si, const Range& rg0, const std::function<void(Style&)>& fn, const char* label) {
    Worksheet& ws = sheets[si];
    Range g = rg0.norm();
    std::unordered_map<uint32_t, uint32_t> memo;
    auto restyle = [&](uint32_t id) {
        auto it = memo.find(id);
        if (it != memo.end()) return it->second;
        Style s = styles.get(id);
        fn(s);
        uint32_t n = styles.intern(s);
        memo[id] = n;
        return n;
    };
    bool whole_cols = g.r1 >= MAX_ROWS - 1, whole_rows = g.c1 >= MAX_COLS - 1;
    if (whole_cols || whole_rows) {
        auto snap = snapshot(*this, {si}, label);
        if (whole_cols && !whole_rows) for (int c = g.c0; c <= g.c1; ++c) ws.col(c).style = restyle(ws.col(c).style);
        else if (whole_rows && !whole_cols) for (int r = g.r0; r <= g.r1; ++r) ws.row(r).style = restyle(ws.row(r).style);
        else for (int c = 0; c < (int)ws.cols.size(); ++c) ws.cols[c].style = restyle(ws.cols[c].style);
        Range u = clip_to_used(ws, g);
        for (int r = u.r0; r <= u.r1 && r < (int)ws.rows.size(); ++r) {
            auto& cells = ws.rows[r].cells;
            for (int c = u.c0; c <= u.c1 && c < (int)cells.size(); ++c) {
                uint32_t base = cells[c].style ? cells[c].style : ws.style_at(r, c);
                cells[c].style = restyle(base);
            }
        }
        commit_(std::move(snap), false);
        return;
    }
    auto cmd = std::make_unique<CellsCommand>();
    cmd->label = label;
    for (int r = g.r0; r <= g.r1; ++r)
        for (int c = g.c0; c <= g.c1; ++c) {
            const Cell* old = ws.cell(r, c);
            Cell nc = old ? *old : Cell();
            nc.calc = 0;
            nc.style = restyle(old && old->style ? old->style : ws.style_at(r, c));
            cmd->e.push_back({si, r, c, std::move(nc)});
        }
    apply_cells(*this, std::move(cmd));
}

void Book::set_number_format(int si, const Range& rg, const std::string& code) {
    apply_style(si, rg, [&](Style& s) { s.numfmt = code.empty() ? "General" : code; }, "Number format");
}

// ------------------------------------------------------ sheet-level ops --
namespace {
template <class F> void sheet_op(Book& b, int si, const char* label, F&& fn, bool structural = false) {
    auto snap = snapshot(b, {si}, label);
    fn(b.sheets[si]);
    b.commit_(std::move(snap), structural);
}
}

void Book::set_col_width(int si, int c0, int c1, double w) { sheet_op(*this, si, "Column width", [&](Worksheet& ws) { for (int c = c0; c <= c1; ++c) ws.col(c).width = w; }); }
void Book::set_row_height(int si, int r0, int r1, double h) { sheet_op(*this, si, "Row height", [&](Worksheet& ws) { for (int r = r0; r <= r1; ++r) ws.row(r).height = (float)h; }); }
void Book::hide_rows(int si, int r0, int r1, bool hide) { sheet_op(*this, si, hide ? "Hide rows" : "Unhide rows", [&](Worksheet& ws) { for (int r = r0; r <= r1; ++r) { if (!hide && r >= (int)ws.rows.size()) break; ws.row(r).hidden = hide; } }, true); }
void Book::hide_cols(int si, int c0, int c1, bool hide) { sheet_op(*this, si, hide ? "Hide columns" : "Unhide columns", [&](Worksheet& ws) { for (int c = c0; c <= c1; ++c) ws.col(c).hidden = hide; }); }
void Book::set_freeze(int si, int rows, int cols) { sheet_op(*this, si, "Freeze panes", [&](Worksheet& ws) { ws.freeze_rows = std::max(0, rows); ws.freeze_cols = std::max(0, cols); }); }
void Book::set_tab_color(int si, Argb c) { sheet_op(*this, si, "Tab colour", [&](Worksheet& ws) { ws.tab_color = c; }); }

void Book::merge(int si, const Range& rg0) {
    Range rg = rg0.norm();
    if (rg.rows() * (long long)rg.cols() < 2) return;
    sheet_op(*this, si, "Merge cells", [&](Worksheet& ws) {
        ws.merges.erase(std::remove_if(ws.merges.begin(), ws.merges.end(), [&](const Range& m) { return m.intersects(rg); }), ws.merges.end());
        // keep only the top-left value (Excel behaviour)
        for (int r = rg.r0; r <= rg.r1 && r < (int)ws.rows.size(); ++r)
            for (int c = rg.c0; c <= rg.c1 && c < (int)ws.rows[r].cells.size(); ++c)
                if (r != rg.r0 || c != rg.c0) { Cell& cl = ws.rows[r].cells[c]; cl.v = Value(); cl.f.reset(); }
        ws.merges.push_back(rg);
    }, true);
}
void Book::unmerge(int si, const Range& rg0) {
    Range rg = rg0.norm();
    sheet_op(*this, si, "Unmerge cells", [&](Worksheet& ws) {
        ws.merges.erase(std::remove_if(ws.merges.begin(), ws.merges.end(), [&](const Range& m) { return m.intersects(rg); }), ws.merges.end());
    });
}
void Book::add_cond_format(int si, const CondFormat& cf) { sheet_op(*this, si, "Conditional format", [&](Worksheet& ws) { ws.cond.push_back(cf); }); }
void Book::set_cond_formats(int si, std::vector<CondFormat> list) { sheet_op(*this, si, "Conditional formats", [&](Worksheet& ws) { ws.cond = std::move(list); }); }
void Book::add_validation(int si, const DataValidation& dv) { sheet_op(*this, si, "Data validation", [&](Worksheet& ws) { ws.validations.push_back(dv); }); }
void Book::set_validations(int si, std::vector<DataValidation> list) { sheet_op(*this, si, "Data validation", [&](Worksheet& ws) { ws.validations = std::move(list); }); }

void Book::define_name(const DefinedName& n) {
    auto snap = snapshot(*this, {}, "Define name");
    names.erase(std::remove_if(names.begin(), names.end(), [&](const DefinedName& x) { return x.scope == n.scope && ci_equal(x.name, n.name); }), names.end());
    DefinedName d = n;
    if (!d.formula.empty() && d.formula[0] == '=') d.formula.erase(0, 1);
    names.push_back(d);
    invalidate_graph();
    commit_(std::move(snap), true);
}
void Book::remove_name(const std::string& name, int scope) {
    auto snap = snapshot(*this, {}, "Remove name");
    names.erase(std::remove_if(names.begin(), names.end(), [&](const DefinedName& x) { return x.scope == scope && ci_equal(x.name, name); }), names.end());
    invalidate_graph();
    commit_(std::move(snap), true);
}

// --------------------------------------------------------------- sheets --
int Book::add_sheet(const std::string& name0, int at) {
    auto snap = snapshot(*this, {}, "Insert sheet", true);
    std::string base = name0.empty() ? "Sheet" : name0;
    std::string name = base;
    if (name0.empty() || find_sheet(name) >= 0) {
        for (int k = name0.empty() ? sheet_count() + 1 : 2;; ++k) { name = base + (name0.empty() ? "" : " (") + std::to_string(k) + (name0.empty() ? "" : ")"); if (find_sheet(name) < 0) break; }
    }
    if (name.size() > 31) name = name.substr(0, 31);
    Worksheet ws;
    ws.name = name;
    uint32_t mx = 0;
    for (auto& s : sheets) mx = std::max(mx, s.id);
    ws.id = mx + 1;
    if (at < 0 || at > sheet_count()) at = sheet_count();
    sheets.insert(sheets.begin() + at, std::move(ws));
    if (active_sheet >= at && sheet_count() > 1) ++active_sheet;
    // named scopes refer to sheet indices
    for (auto& n : names) if (n.scope >= at) ++n.scope;
    invalidate_graph();
    commit_(std::move(snap), true);
    return at;
}
void Book::remove_sheet(int si) {
    if (sheet_count() <= 1 || si < 0 || si >= sheet_count()) return;
    auto snap = snapshot(*this, {}, "Delete sheet", true);
    uint32_t id = sheets[si].id;
    sheets.erase(sheets.begin() + si);
    for (int s = 0; s < sheet_count(); ++s)
        for (auto& r : sheets[s].rows) for (auto& c : r.cells) if (c.f) { auto nf = drop_sheet_refs(*c.f, id); if (nf) c.f = nf; }
    names.erase(std::remove_if(names.begin(), names.end(), [&](const DefinedName& n) { return n.scope == si; }), names.end());
    for (auto& n : names) if (n.scope > si) --n.scope;
    if (active_sheet >= sheet_count()) active_sheet = sheet_count() - 1;
    invalidate_graph();
    commit_(std::move(snap), true);
}
void Book::rename_sheet(int si, const std::string& name) {
    if (name.empty() || name.size() > 31) return;
    int ex = find_sheet(name);
    if (ex >= 0 && ex != si) return;
    if (name.find_first_of(":\\/?*[]") != std::string::npos) return;
    // formulas reference sheets by id: their text follows automatically. Defined names are text: rewrite.
    auto snap = snapshot(*this, {si}, "Rename sheet");
    std::vector<std::pair<DefinedName*, std::shared_ptr<Formula>>> comp;
    for (auto& n : names) {
        auto f = std::make_shared<Formula>(); std::string e;
        if (compile_formula(n.formula, *this, n.scope >= 0 ? n.scope : 0, 0, 0, *f, e)) comp.push_back({&n, f});
    }
    sheets[si].name = name;
    for (auto& [n, f] : comp) n->formula = formula_text(*f, *this, n->scope >= 0 ? n->scope : 0, 0, 0);
    invalidate_graph();
    commit_(std::move(snap), true);
}
void Book::move_sheet(int from, int to) {
    if (from < 0 || from >= sheet_count() || to < 0 || to >= sheet_count() || from == to) return;
    auto snap = snapshot(*this, {}, "Move sheet", true);
    Worksheet ws = std::move(sheets[from]);
    sheets.erase(sheets.begin() + from);
    sheets.insert(sheets.begin() + to, std::move(ws));
    for (auto& n : names) {
        if (n.scope == from) n.scope = to;
        else if (from < to && n.scope > from && n.scope <= to) --n.scope;
        else if (from > to && n.scope >= to && n.scope < from) ++n.scope;
    }
    if (active_sheet == from) active_sheet = to;
    invalidate_graph();
    commit_(std::move(snap), true);
}

// ----------------------------------------------------- structural edits --
static bool adjust_range(Range& g, const StructOp& op) {
    int lo = op.rows ? g.r0 : g.c0, hi = op.rows ? g.r1 : g.c1;
    int lim = op.rows ? MAX_ROWS : MAX_COLS;
    if (hi >= lim - 1 && lo == 0) return true;   // whole row/col span unaffected
    if (op.n > 0) { if (lo >= op.at) lo += op.n; if (hi >= op.at) hi += op.n; }
    else {
        int d0 = op.at, d1 = op.at - op.n;
        int nlo = lo < d0 ? lo : (lo < d1 ? d0 : lo + op.n);
        int nhi = hi < d0 ? hi : (hi < d1 ? d0 - 1 : hi + op.n);
        if (nhi < nlo) return false;
        lo = nlo; hi = nhi;
    }
    hi = std::min(hi, lim - 1);
    if (lo > hi) return false;
    if (op.rows) { g.r0 = lo; g.r1 = hi; } else { g.c0 = lo; g.c1 = hi; }
    return true;
}
static void adjust_ranges(std::vector<Range>& v, const StructOp& op) {
    std::vector<Range> o;
    for (auto g : v) if (adjust_range(g, op)) o.push_back(g);
    v.swap(o);
}

static void structural(Book& b, int si, bool rows, int at, int n, const char* label) {
    if (n == 0 || at < 0) return;
    auto snap = snapshot(b, sheets_with_formulas(b, si), label);
    std::vector<std::pair<DefinedName*, std::shared_ptr<Formula>>> names_c;
    for (auto& nm : b.names) {
        auto f = std::make_shared<Formula>(); std::string e;
        if (compile_formula(nm.formula, b, nm.scope >= 0 ? nm.scope : 0, 0, 0, *f, e)) names_c.push_back({&nm, f});
    }
    Worksheet& ws = b.sheets[si];
    StructOp op{ws.id, rows, at, n};
    // 1. move cells
    if (rows) {
        if (n > 0) { if (at < (int)ws.rows.size()) ws.rows.insert(ws.rows.begin() + at, (size_t)n, RowStore()); }
        else if (at < (int)ws.rows.size()) ws.rows.erase(ws.rows.begin() + at, ws.rows.begin() + std::min((int)ws.rows.size(), at - n));
        if ((int)ws.rows.size() > MAX_ROWS) ws.rows.resize(MAX_ROWS);
    } else {
        for (auto& r : ws.rows) {
            auto& v = r.cells;
            if (n > 0) { if (at < (int)v.size()) v.insert(v.begin() + at, (size_t)n, Cell()); if ((int)v.size() > MAX_COLS) v.resize(MAX_COLS); }
            else if (at < (int)v.size()) v.erase(v.begin() + at, v.begin() + std::min((int)v.size(), at - n));
        }
        if (n > 0) { if (at < (int)ws.cols.size()) ws.cols.insert(ws.cols.begin() + at, (size_t)n, ColInfo()); }
        else if (at < (int)ws.cols.size()) ws.cols.erase(ws.cols.begin() + at, ws.cols.begin() + std::min((int)ws.cols.size(), at - n));
    }
    // 2. adjust formulas everywhere
    auto inv = [&](int i) { if (n > 0) return i >= at + n ? i - n : i; return i >= at ? i - n : i; };
    for (int s = 0; s < b.sheet_count(); ++s) {
        Worksheet& w = b.sheets[s];
        uint32_t hid = w.id;
        for (int r = 0; r < (int)w.rows.size(); ++r) {
            auto& cells = w.rows[r].cells;
            for (int c = 0; c < (int)cells.size(); ++c) {
                Cell& cl = cells[c];
                if (!cl.f) continue;
                int orr = r, oc = c;
                if (s == si) { if (rows) orr = inv(r); else oc = inv(c); }
                auto nf = adjust_formula(*cl.f, hid, orr, oc, r, c, op);
                if (nf) cl.f = nf;
            }
        }
        w.spills.clear();
    }
    // 3. sheet structures
    adjust_ranges(ws.merges, op);
    for (auto& cf : ws.cond) adjust_ranges(cf.ranges, op);
    ws.cond.erase(std::remove_if(ws.cond.begin(), ws.cond.end(), [](const CondFormat& c) { return c.ranges.empty(); }), ws.cond.end());
    for (auto& dv : ws.validations) adjust_ranges(dv.ranges, op);
    ws.validations.erase(std::remove_if(ws.validations.begin(), ws.validations.end(), [](const DataValidation& d) { return d.ranges.empty(); }), ws.validations.end());
    if (ws.autofilter.active()) {
        Range g = ws.autofilter.range;
        int c0 = g.c0;
        if (!adjust_range(g, op)) ws.autofilter = AutoFilter();
        else {
            ws.autofilter.range = g;
            if (!rows) {
                std::vector<FilterColumn> keep;
                for (auto fc : ws.autofilter.cols) {
                    int abs = c0 + fc.col, m = op.map(abs);
                    if (m < 0) continue;
                    fc.col = m - g.c0;
                    keep.push_back(fc);
                }
                ws.autofilter.cols = keep;
            }
        }
    }
    // 4. names
    for (auto& [nm, f] : names_c) {
        int host = nm->scope >= 0 ? nm->scope : 0;
        auto nf = adjust_formula(*f, b.sheets[host].id, 0, 0, 0, 0, op);
        if (nf) nm->formula = formula_text(*nf, b, host, 0, 0);
    }
    b.invalidate_graph();
    b.commit_(std::move(snap), true);
}

void Book::insert_rows(int si, int at, int n) { if (n > 0) structural(*this, si, true, at, n, "Insert rows"); }
void Book::delete_rows(int si, int at, int n) { if (n > 0) structural(*this, si, true, at, -n, "Delete rows"); }
void Book::insert_cols(int si, int at, int n) { if (n > 0) structural(*this, si, false, at, n, "Insert columns"); }
void Book::delete_cols(int si, int at, int n) { if (n > 0) structural(*this, si, false, at, -n, "Delete columns"); }

// ------------------------------------------------------------------ sort --
static int sort_key_cmp(const Value& x, const Value& y, const SortKey& k) {
    // blanks always last (both directions), numbers < text < bool < errors
    if (x.is_empty() != y.is_empty()) return x.is_empty() ? 2 : -2;
    if (x.is_empty()) return 0;
    if (x.is_str() && y.is_str()) {
        int c = k.natural ? natural_cmp(x.str, y.str) : (k.case_sensitive ? x.str.compare(y.str) : ci_compare(x.str, y.str));
        if (c == 0 && k.case_sensitive) c = x.str.compare(y.str);
        return c < 0 ? -1 : c > 0 ? 1 : 0;
    }
    return compare_values(x, y, k.case_sensitive);
}

void Book::sort(int si, const Range& rg0, const std::vector<SortKey>& keys, bool has_header) {
    Worksheet& ws = sheets[si];
    Range rg = rg0.norm();
    if (rg.r1 >= (int)ws.rows.size()) rg.r1 = (int)ws.rows.size() - 1;
    if (rg.c1 >= MAX_COLS - 1) rg.c1 = ws.ncols() - 1;
    int first = rg.r0 + (has_header ? 1 : 0);
    if (first >= rg.r1 || keys.empty()) return;
    auto snap = snapshot(*this, sheets_with_formulas(*this, si), "Sort");
    int n = rg.r1 - first + 1;
    std::vector<int> order(n);
    std::iota(order.begin(), order.end(), first);
    // snapshot key values
    std::vector<std::vector<Value>> kv(keys.size(), std::vector<Value>(n));
    for (size_t k = 0; k < keys.size(); ++k) for (int i = 0; i < n; ++i) kv[k][i] = ws.value(first + i, keys[k].col);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        for (size_t k = 0; k < keys.size(); ++k) {
            int c = sort_key_cmp(kv[k][a - first], kv[k][b - first], keys[k]);
            if (c == 2 || c == -2) return c < 0;
            if (c) return keys[k].desc ? c > 0 : c < 0;
        }
        return false;
    });
    // move row segments
    std::vector<std::vector<Cell>> seg(n);
    for (int i = 0; i < n; ++i) {
        int r = order[i];
        seg[i].resize(rg.cols());
        for (int c = rg.c0; c <= rg.c1; ++c) if (r < (int)ws.rows.size() && c < (int)ws.rows[r].cells.size()) seg[i][c - rg.c0] = ws.rows[r].cells[c];
    }
    for (int i = 0; i < n; ++i) {
        int r = first + i;
        for (int c = rg.c0; c <= rg.c1; ++c) {
            Cell cl = seg[i][c - rg.c0];
            // formulas keep their relative offsets: same-row references follow the moved row
            cl.calc = 0;
            if (cl.blank() && !cl.style && (r >= (int)ws.rows.size() || c >= (int)ws.rows[r].cells.size())) continue;
            ws.at(r, c) = std::move(cl);
        }
    }
    ws.spills.clear();
    invalidate_graph();
    commit_(std::move(snap), true);
}

// ------------------------------------------------------------ autofilter --
static bool filter_match(const Book& b, int si, int r, int c, const FilterColumn& fc) {
    const Value& v = b.sheets[si].value(r, c);
    std::string t = b.display_text(si, r, c);
    bool blank = v.is_empty() || (v.is_str() && v.str.empty());
    if (fc.use_custom) {
        auto test = [&](CmpOp op, const std::string& crit) {
            Value cv = parse_input(crit);
            double d;
            if (cv.is_num() && (v.is_num() || (v.is_str() && parse_number_strict(v.str, d)))) {
                double x = v.is_num() ? v.num : d;
                switch (op) {
                case CmpOp::Equal: return x == cv.num; case CmpOp::NotEqual: return x != cv.num; case CmpOp::Greater: return x > cv.num;
                case CmpOp::Less: return x < cv.num; case CmpOp::GreaterEqual: return x >= cv.num; case CmpOp::LessEqual: return x <= cv.num;
                default: return false;
                }
            }
            bool wild = crit.find_first_of("*?") != std::string::npos;
            int cmp = ci_compare(t, crit);
            switch (op) {
            case CmpOp::Equal: return wild ? wildcard_match(crit, t) : cmp == 0;
            case CmpOp::NotEqual: return wild ? !wildcard_match(crit, t) : cmp != 0;
            case CmpOp::Greater: return cmp > 0; case CmpOp::Less: return cmp < 0;
            case CmpOp::GreaterEqual: return cmp >= 0; case CmpOp::LessEqual: return cmp <= 0;
            default: return false;
            }
        };
        bool a = test(fc.op1, fc.v1);
        if (fc.v2.empty() && fc.op2 == CmpOp::Equal) return a;
        bool bb = test(fc.op2, fc.v2);
        return fc.and_ ? (a && bb) : (a || bb);
    }
    if (blank) return fc.blanks;
    if (fc.values.empty()) return !fc.blanks;   // only "(Blanks)" selected
    for (auto& x : fc.values) if (ci_equal(x, t)) return true;
    return false;
}

static void apply_filter_raw(Book& b, int si) {
    Worksheet& ws = b.sheets[si];
    for (auto& r : ws.rows) r.filtered = false;
    if (!ws.autofilter.active()) return;
    Range g = ws.autofilter.range;
    int last = std::max(g.r1, (int)ws.rows.size() - 1);
    // extend down while data continues (Excel grows the filter range with contiguous data)
    int end = g.r1;
    for (int r = g.r1 + 1; r <= last; ++r) {
        bool any = false;
        for (int c = g.c0; c <= g.c1; ++c) if (!ws.value(r, c).is_empty()) { any = true; break; }
        if (!any) break;
        end = r;
    }
    for (int r = g.r0 + 1; r <= end; ++r) {
        bool ok = true;
        for (auto& fc : ws.autofilter.cols) {
            if (!fc.active()) continue;
            if (!filter_match(b, si, r, g.c0 + fc.col, fc)) { ok = false; break; }
        }
        if (!ok) ws.row(r).filtered = true;
    }
}

void Book::set_autofilter(int si, const Range& rg) {
    sheet_op(*this, si, rg.valid() ? "AutoFilter" : "Remove AutoFilter", [&](Worksheet& ws) {
        if (!rg.valid()) { ws.autofilter = AutoFilter(); for (auto& r : ws.rows) r.filtered = false; return; }
        Range g = rg.norm();
        if (g.r1 >= MAX_ROWS - 1) g.r1 = std::max(g.r0, ws.nrows() - 1);
        ws.autofilter.range = g;
        ws.autofilter.cols.clear();
        for (auto& r : ws.rows) r.filtered = false;
    }, true);
}
void Book::set_filter_column(int si, const FilterColumn& fc) {
    auto snap = snapshot(*this, {si}, "Filter");
    Worksheet& ws = sheets[si];
    auto& v = ws.autofilter.cols;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const FilterColumn& x) { return x.col == fc.col; }), v.end());
    if (fc.active()) v.push_back(fc);
    apply_filter_raw(*this, si);
    commit_(std::move(snap), true);
}
void Book::clear_filters(int si) {
    auto snap = snapshot(*this, {si}, "Clear filter");
    sheets[si].autofilter.cols.clear();
    apply_filter_raw(*this, si);
    commit_(std::move(snap), true);
}
void Book::reapply_filter(int si) {
    auto snap = snapshot(*this, {si}, "Reapply filter");
    apply_filter_raw(*this, si);
    commit_(std::move(snap), true);
}
std::vector<std::string> Book::filter_values(int si, int col_off) const {
    const Worksheet& ws = sheets[si];
    std::vector<std::string> out;
    if (!ws.autofilter.active()) return out;
    Range g = ws.autofilter.range;
    int c = g.c0 + col_off;
    std::set<std::string> seen;
    int last = std::max(g.r1, (int)ws.rows.size() - 1);
    for (int r = g.r0 + 1; r <= last; ++r) {
        std::string t = display_text(si, r, c);
        if (seen.insert(lower_utf8(t)).second) out.push_back(t);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        if (a.empty() != b.empty()) return b.empty();
        return natural_cmp(a, b) < 0;
    });
    return out;
}

// ------------------------------------------------------------------ fill --
static bool split_trailing_number(const std::string& s, std::string& pre, long long& n, int& width) {
    size_t k = s.size();
    while (k > 0 && isdigit((unsigned char)s[k - 1])) --k;
    if (k == s.size() || s.size() - k > 15) return false;
    pre = s.substr(0, k);
    width = (int)(s.size() - k);
    n = atoll(s.c_str() + k);
    return true;
}

void Book::fill(int si, const Range& src0, const Range& dst0) {
    Worksheet& ws = sheets[si];
    Range src = src0.norm(), dst = dst0.norm();
    bool vertical = dst.c0 == src.c0 && dst.c1 == src.c1;
    auto cmd = std::make_unique<CellsCommand>();
    cmd->label = "Fill";
    int lines = vertical ? src.cols() : src.rows();
    int slen = vertical ? src.rows() : src.cols();
    bool forward = vertical ? dst.r1 > src.r1 : dst.c1 > src.c1;
    int d0 = vertical ? (forward ? src.r1 + 1 : dst.r0) : (forward ? src.c1 + 1 : dst.c0);
    int d1 = vertical ? (forward ? dst.r1 : src.r0 - 1) : (forward ? dst.c1 : src.c0 - 1);
    for (int L = 0; L < lines; ++L) {
        auto at = [&](int k) -> std::pair<int, int> { return vertical ? std::make_pair(src.r0 + k, src.c0 + L) : std::make_pair(src.r0 + L, src.c0 + k); };
        // series detection
        std::vector<const Cell*> sc(slen);
        bool all_num = true, any_formula = false;
        for (int k = 0; k < slen; ++k) {
            auto [r, c] = at(k);
            sc[k] = ws.cell(r, c);
            if (!sc[k] || sc[k]->f || !sc[k]->v.is_num()) all_num = false;
            if (sc[k] && sc[k]->f) any_formula = true;
        }
        double step = 0, base = 0;
        bool series = false;
        if (all_num && slen >= 2) {
            base = sc[0]->v.num;
            step = (sc[slen - 1]->v.num - base) / (slen - 1);
            series = true;
        } else if (all_num && slen == 1) {
            auto [r, c] = at(0);
            if (is_date_format(styles.get(ws.style_at(r, c)).numfmt)) { base = sc[0]->v.num; step = 1; series = true; }
        }
        // text with trailing number ("R1", "Item 7"): increment
        std::string pre; long long tn = 0; int tw = 0;
        bool text_series = !any_formula && slen == 1 && sc[0] && sc[0]->v.is_str() && split_trailing_number(sc[0]->v.str, pre, tn, tw);
        for (int d = d0; d <= d1; ++d) {
            int off = forward ? d - (vertical ? src.r0 : src.c0) : (vertical ? src.r0 : src.c0) - d;   // distance from first src cell
            int k = forward ? off % slen : (slen - 1) - ((off - 1) % slen);
            auto [sr, scc] = at(k);
            int r = vertical ? d : src.r0 + L, c = vertical ? src.c0 + L : d;
            const Cell* s = ws.cell(sr, scc);
            Cell nc = s ? *s : Cell();
            nc.calc = 0;
            if (series) {
                double idx = forward ? off : -(double)off;
                nc.v = Value::number(base + step * idx);
                nc.f.reset();
            } else if (text_series) {
                long long v = tn + (forward ? off : -off);
                if (v < 0) v = 0;
                std::string num = std::to_string(v);
                while ((int)num.size() < tw && tw > 1 && sc[0]->v.str[pre.size()] == '0') num = "0" + num;
                nc.v = Value::text(pre + num);
            }
            // formulas: relative offsets make the shared Formula re-target automatically
            cmd->e.push_back({si, r, c, std::move(nc)});
        }
    }
    apply_cells(*this, std::move(cmd));
}

void Book::map_cells(int si, const Range& rg0, const std::function<std::string(const std::string&)>& fn, const char* label) {
    Worksheet& ws = sheets[si];
    Range rg = clip_to_used(ws, rg0);
    auto cmd = std::make_unique<CellsCommand>();
    cmd->label = label;
    for (int r = rg.r0; r <= rg.r1 && r < (int)ws.rows.size(); ++r) {
        auto& cells = ws.rows[r].cells;
        for (int c = rg.c0; c <= rg.c1 && c < (int)cells.size(); ++c) {
            const Cell& cl = cells[c];
            if (cl.f || !cl.v.is_str()) continue;
            std::string n = fn(cl.v.str);
            if (n == cl.v.str) continue;
            Cell nc = cl; nc.calc = 0; nc.v = Value::text(n);
            cmd->e.push_back({si, r, c, std::move(nc)});
        }
    }
    apply_cells(*this, std::move(cmd));
}

// ------------------------------------------------------------- clipboard --
ClipBlock Book::copy(int si, const Range& rg0) const {
    const Worksheet& ws = sheets[si];
    Range rg = rg0.norm();
    if (rg.r1 >= MAX_ROWS - 1) rg.r1 = std::max(rg.r0, ws.nrows() - 1);
    if (rg.c1 >= MAX_COLS - 1) rg.c1 = std::max(rg.c0, ws.ncols() - 1);
    ClipBlock b;
    b.rows = rg.rows(); b.cols = rg.cols();
    b.src_sheet = si; b.src_r = rg.r0; b.src_c = rg.c0; b.src_book = this;
    b.cells.resize((size_t)b.rows * b.cols);
    b.formula_text.resize(b.cells.size());
    std::unordered_map<uint32_t, uint32_t> smap;
    for (int i = 0; i < b.rows; ++i)
        for (int j = 0; j < b.cols; ++j) {
            int r = rg.r0 + i, c = rg.c0 + j;
            Cell& d = b.cells[(size_t)i * b.cols + j];
            const Cell* s = ws.cell(r, c);
            if (s) { d = *s; d.calc = 0; }
            else d.v = ws.value(r, c);   // spilled value copies as a value
            uint32_t st = ws.style_at(r, c);
            auto it = smap.find(st);
            if (it == smap.end()) { smap[st] = (uint32_t)b.styles.size(); b.styles.push_back(styles.get(st)); d.style = (uint32_t)b.styles.size() - 1; }
            else d.style = it->second;
            if (d.f) b.formula_text[(size_t)i * b.cols + j] = formula_text(*d.f, *this, si, r, c);
        }
    return b;
}

void Book::paste(int si, int r0, int c0, const ClipBlock& blk, unsigned what) {
    Worksheet& ws = sheets[si];
    auto cmd = std::make_unique<CellsCommand>();
    cmd->label = "Paste";
    bool tr = (what & PasteTranspose) != 0;
    std::vector<uint32_t> smap(blk.styles.size(), UINT32_MAX);
    for (int i = 0; i < blk.rows; ++i)
        for (int j = 0; j < blk.cols; ++j) {
            const Cell& s = blk.cells[(size_t)i * blk.cols + j];
            if ((what & PasteSkipBlanks) && s.blank()) continue;
            int r = r0 + (tr ? j : i), c = c0 + (tr ? i : j);
            if (r >= MAX_ROWS || c >= MAX_COLS) continue;
            const Cell* old = ws.cell(r, c);
            Cell nc = old ? *old : Cell();
            nc.calc = 0;
            const std::string& ft = blk.formula_text.size() > (size_t)i * blk.cols + j ? blk.formula_text[(size_t)i * blk.cols + j] : std::string();
            bool has_f = s.f || !ft.empty();
            if ((what & PasteFormulas) && has_f) {
                if (s.f && blk.src_book == this && !tr) nc.f = s.f;   // relative offsets retarget automatically
                else {
                    auto f = std::make_shared<Formula>(); std::string e;
                    int sr = blk.src_book ? blk.src_r + i : r, sc = blk.src_book ? blk.src_c + j : c;   // external text: literal refs
                    int host_si = (blk.src_book == this && blk.src_sheet >= 0 && blk.src_sheet < sheet_count()) ? blk.src_sheet : si;
                    if (compile_formula(ft, *this, host_si, sr, sc, *f, e)) nc.f = f;   // offsets => copy semantics at the destination
                    else { nc.f.reset(); nc.v = Value::text("=" + ft); }
                }
                if (nc.f) nc.v = Value();
            } else if (what & (PasteValues | PasteFormulas)) {
                nc.f.reset();
                nc.v = s.v;
            }
            if ((what & PasteFormats) && s.style < blk.styles.size()) {
                if (smap[s.style] == UINT32_MAX) smap[s.style] = styles.intern(blk.styles[s.style]);
                nc.style = smap[s.style];
            }
            cmd->e.push_back({si, r, c, std::move(nc)});
        }
    apply_cells(*this, std::move(cmd));
}

std::string Book::copy_tsv(int si, const Range& rg0) const {
    const Worksheet& ws = sheets[si];
    Range rg = rg0.norm();
    if (rg.r1 >= MAX_ROWS - 1) rg.r1 = std::max(rg.r0, ws.nrows() - 1);
    if (rg.c1 >= MAX_COLS - 1) rg.c1 = std::max(rg.c0, ws.ncols() - 1);
    std::vector<Row> rows;
    for (int r = rg.r0; r <= rg.r1; ++r) {
        if (ws.row_hidden(r) && ws.rows[r].filtered) continue;   // Excel copies only visible rows of a filtered list
        Row row;
        for (int c = rg.c0; c <= rg.c1; ++c) row.push_back(display_text(si, r, c));
        rows.push_back(std::move(row));
    }
    return to_clipboard(rows);
}

ClipBlock clip_from_tsv(const std::string& tsv) {
    ClipBlock b;
    auto rows = from_clipboard(tsv);
    b.rows = (int)rows.size();
    for (auto& r : rows) b.cols = std::max(b.cols, (int)r.size());
    b.cells.resize((size_t)b.rows * b.cols);
    b.formula_text.resize(b.cells.size());
    b.styles.push_back(Style());
    for (int i = 0; i < b.rows; ++i)
        for (int j = 0; j < (int)rows[i].size(); ++j) {
            const std::string& t = rows[i][j];
            Cell& c = b.cells[(size_t)i * b.cols + j];
            if (t.size() > 1 && t[0] == '=') { b.formula_text[(size_t)i * b.cols + j] = t.substr(1); continue; }
            std::string hint;
            c.v = parse_input(t, &hint);
            if (!hint.empty()) { Style s; s.numfmt = hint; c.style = (uint32_t)b.styles.size(); b.styles.push_back(s); }
        }
    return b;
}

// ---------------------------------------------------------- legacy bridge --
Book from_legacy(const Workbook& wb) {
    Book b;
    b.sheets.clear();
    uint32_t id = 1;
    for (auto& s : wb.sheets) {
        Worksheet ws;
        ws.name = s.name;
        ws.id = id++;
        for (int r = 0; r < s.nrows(); ++r)
            for (int c = 0; c < (int)s.rows[r].size(); ++c) {
                const std::string& t = s.rows[r][c];
                if (t.empty()) continue;
                std::string hint;
                Value v = parse_input(t, &hint);
                if ((!hint.empty() && is_date_format(hint)) || t[0] == '\'') v = Value::text(t);   // legacy text stays text
                ws.at(r, c).v = v;
            }
        b.sheets.push_back(std::move(ws));
    }
    if (b.sheets.empty()) { Worksheet ws; ws.id = 1; b.sheets.push_back(std::move(ws)); }
    b.invalidate_graph();
    return b;
}
Workbook to_legacy(const Book& b) {
    Workbook wb;
    for (int si = 0; si < b.sheet_count(); ++si) {
        const Worksheet& ws = b.sheets[si];
        Sheet s;
        s.name = ws.name;
        int nr = ws.nrows();
        for (auto& [k, arr] : ws.spills) nr = std::max(nr, k.first + arr->rows);
        s.rows.resize(nr);
        for (int r = 0; r < nr; ++r) {
            int nc = r < (int)ws.rows.size() ? (int)ws.rows[r].cells.size() : 0;
            for (auto& [k, arr] : ws.spills) if (r >= k.first && r < k.first + arr->rows) nc = std::max(nc, k.second + arr->cols);
            for (int c = 0; c < nc; ++c) { std::string t = b.display_text(si, r, c); if (!t.empty()) s.set(r, c, t); }
        }
        s.trim_trailing();
        wb.sheets.push_back(std::move(s));
    }
    return wb;
}

} // namespace pcbp::sheet
