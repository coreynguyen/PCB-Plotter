#pragma once
// ============================================================================
// sheet.h — spreadsheet engine (pcbp::sheet). Portable, no Windows headers.
//
// Two layers:
//   1. Book / Worksheet / Cell (NEW engine): typed values, formulas with a
//      dependency graph + incremental recalc, shared style table, number
//      formats, conditional formatting, data validation, merges, freeze panes,
//      autofilter, multi-key sort, defined names, undo/redo command stack,
//      rich clipboard (TSV + HTML). File I/O lives in io/xlsx.h + io/ods.h.
//   2. Legacy string grid (Sheet / Workbook / Row) + BOM clean-up helpers,
//      kept unchanged for existing callers; convert with from_legacy/to_legacy.
//
// Clean-room implementation from public specs and documented behaviour —
// see specs/SHEET_ENGINE.md (behaviour sources) and specs/SHEET_API.md (usage).
// ============================================================================
#include "formula.h"
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pcbp::sheet {

// =============================================================== styles ====
// Colours are 0xAARRGGBB. 0 = automatic / not set.
using Argb = uint32_t;

enum class Underline : uint8_t { None, Single, Double, SingleAccounting, DoubleAccounting };
struct Font {
    std::string name = "Calibri";
    double size = 11;
    bool bold = false, italic = false, strike = false;
    Underline underline = Underline::None;
    Argb color = 0;
    bool operator==(const Font& o) const;
};

// ECMA-376 ST_PatternType order
enum class FillPattern : uint8_t { None, Solid, MediumGray, DarkGray, LightGray, DarkHorizontal, DarkVertical,
    DarkDown, DarkUp, DarkGrid, DarkTrellis, LightHorizontal, LightVertical, LightDown, LightUp, LightGrid,
    LightTrellis, Gray125, Gray0625 };
struct Fill {
    FillPattern pattern = FillPattern::None;
    Argb fg = 0;                 // pattern / solid colour
    Argb bg = 0;
    bool operator==(const Fill& o) const { return pattern == o.pattern && fg == o.fg && bg == o.bg; }
};

// ECMA-376 ST_BorderStyle order
enum class BorderStyle : uint8_t { None, Thin, Medium, Dashed, Dotted, Thick, Double, Hair, MediumDashed,
    DashDot, MediumDashDot, DashDotDot, MediumDashDotDot, SlantDashDot };
struct BorderSide {
    BorderStyle style = BorderStyle::None;
    Argb color = 0;
    bool operator==(const BorderSide& o) const { return style == o.style && color == o.color; }
};
struct Border {
    BorderSide left, right, top, bottom, diagonal;
    bool diag_up = false, diag_down = false;
    bool operator==(const Border& o) const;
};

enum class HAlign : uint8_t { General, Left, Center, Right, Fill, Justify, CenterContinuous, Distributed };
enum class VAlign : uint8_t { Bottom, Center, Top, Justify, Distributed };

struct Style {
    Font font;
    Fill fill;
    Border border;
    std::string numfmt = "General";   // format code (see format_value)
    HAlign halign = HAlign::General;
    VAlign valign = VAlign::Bottom;
    bool wrap = false, shrink = false;
    uint8_t indent = 0;
    int16_t rotation = 0;              // degrees -90..90, 255 = vertical stacked (ECMA textRotation)
    bool locked = true, hidden = false;
    bool operator==(const Style& o) const;
};

// Interned style table. Index 0 is always the default Style.
class StyleTable {
public:
    StyleTable();
    uint32_t intern(const Style& s);                  // existing index or new one
    const Style& get(uint32_t id) const { return id < v_.size() ? v_[id] : v_[0]; }
    size_t size() const { return v_.size(); }
    const std::vector<Style>& all() const { return v_; }
    void clear();
private:
    std::vector<Style> v_;
    std::unordered_multimap<uint64_t, uint32_t> idx_;
};

// Differential style for conditional formats (only the "set" parts apply).
struct Dxf {
    std::optional<Argb> font_color, fill_color;
    std::optional<bool> bold, italic, strike;
    std::optional<Underline> underline;
    std::optional<Border> border;
    std::optional<std::string> numfmt;
};

// ======================================================= number formats ====
// Excel-compatible format codes: General, 0, 0.00, #,##0.00, 0%, 0.00E+00,
// # ?/?, @, dates/times (yyyy mm dd hh mm ss AM/PM [h] .000), literal text "",
// \x escapes, _x spacing, *x fill (ignored), up to 4 ;-sections, [Red]/[Color n],
// [<100] conditions, [$-409]/[$€-x] locale+currency tags.
struct Formatted {
    std::string text;
    Argb color = 0;               // section colour, 0 = none
    bool is_number = false;       // right-align under General
};
Formatted format_value(const Value& v, const std::string& code, bool date1904 = false);
bool is_date_format(const std::string& code);   // any of y m d h s outside quotes/brackets

// Built-in numFmtId <-> code (ECMA-376 §18.8.30), en-US. "" if unknown.
std::string builtin_numfmt(int id);
int builtin_numfmt_id(const std::string& code);   // -1 if not built-in

// Excel serial dates. 1900 system includes the phantom 1900-02-29 (serial 60).
double date_serial(int y, int m, int d, bool date1904 = false);
void serial_to_ymd(double serial, int& y, int& m, int& d, bool date1904 = false);
double time_serial(int h, int mi, double s);

// Interpret typed user input the way Excel does: "12" -> 12, "1,234.5", "12%" -> 0.12 [0%],
// "$5" -> 5 [$#,##0], "1e3", "TRUE", "#N/A", "2024-03-15" / "3/15/2024" -> date [m/d/yyyy],
// "13:45" -> time [h:mm], "'0805" -> text "0805". Leading-zero numbers stay text ("0805").
// Returns the value and (optionally) a format to apply when the cell is General.
Value parse_input(const std::string& text, std::string* fmt_hint = nullptr, bool date1904 = false);

// ================================================== conditional formats ====
enum class CmpOp : uint8_t { Between, NotBetween, Equal, NotEqual, Greater, Less, GreaterEqual, LessEqual };

struct CfValueObject {                        // colour scale / data bar threshold
    enum Type : uint8_t { Min, Max, Num, Percent, Percentile, Formula } type = Min;
    std::string val;                          // number or formula text
    Argb color = 0;
};

struct CondRule {
    enum Type : uint8_t { CellIs, Expression, ContainsText, NotContainsText, BeginsWith, EndsWith,
                          Duplicate, Unique, Top10, AboveAverage, ContainsBlanks, NotContainsBlanks,
                          ContainsErrors, NotContainsErrors, ColorScale, DataBar };
    Type type = CellIs;
    CmpOp op = CmpOp::Equal;
    std::string f1, f2;                       // formulas (no '=') relative to the range's top-left
    std::string text;                         // text rules
    int dxf = -1;                             // index into Book::dxfs
    int priority = 1;
    bool stop = false;
    int rank = 10; bool bottom = false, percent = false;  // Top10
    bool below = false, equal_avg = false;    // AboveAverage
    std::vector<CfValueObject> cfvo;          // ColorScale (2-3) / DataBar (2)
    Argb bar_color = 0xFF638EC6;
};
struct CondFormat {
    std::vector<Range> ranges;
    std::vector<CondRule> rules;
};

// ===================================================== data validation ====
struct DataValidation {
    enum Type : uint8_t { Any, Whole, Decimal, List, Date, Time, TextLength, Custom };
    enum ErrStyle : uint8_t { Stop, Warning, Information };
    std::vector<Range> ranges;
    Type type = Any;
    CmpOp op = CmpOp::Between;
    std::string f1, f2;          // List: "\"a,b,c\"" literal list or a range formula "$A$1:$A$9"
    bool allow_blank = true;
    bool dropdown = true;        // show in-cell dropdown (note: ECMA attr is inverted showDropDown)
    ErrStyle err_style = Stop;
    bool show_input = true, show_error = true;
    std::string prompt_title, prompt, error_title, error;
};

// ============================================================ autofilter ====
struct FilterColumn {
    int col = 0;                              // offset within the autofilter range
    std::vector<std::string> values;          // allowed displayed values (empty + !blanks = no value filter)
    bool blanks = false;                      // include blanks
    bool use_custom = false;                  // custom criteria (op1 v1 [and|or op2 v2]); wildcards * ?
    CmpOp op1 = CmpOp::Equal, op2 = CmpOp::Equal;
    std::string v1, v2;
    bool and_ = true;
    bool active() const { return !values.empty() || blanks || use_custom; }
};
struct AutoFilter {
    Range range;                              // incl. header row; invalid = none
    std::vector<FilterColumn> cols;
    bool active() const { return range.valid(); }
};

struct SortKey {
    int col = 0;                              // absolute sheet column
    bool desc = false;
    bool natural = true;                      // "R2" < "R10" (default on: refdes friendly)
    bool case_sensitive = false;
};

// ============================================================ cell model ====
struct Cell {
    Value v;                       // literal value, or cached result of the formula
    uint32_t style = 0;            // index into Book::styles
    FormulaPtr f;                  // non-null => formula cell (shared between copies)
    mutable uint8_t calc = 0;      // engine-internal recalc state (ignore)
    bool has_formula() const { return (bool)f; }
    bool blank() const { return v.type == Value::Empty && !f; }   // no content (may have style)
};

struct RowStore {
    std::vector<Cell> cells;       // dense by column, trailing empties trimmed lazily
    float height = -1;             // points; -1 = default
    bool hidden = false;           // manual hide
    bool filtered = false;         // hidden by autofilter
    uint32_t style = 0;
    uint8_t outline = 0;
    bool is_hidden() const { return hidden || filtered; }
};
struct ColInfo {
    double width = -1;             // Excel character units; -1 = default
    bool hidden = false;
    uint32_t style = 0;
    uint8_t outline = 0;
};

struct DefinedName {
    std::string name;
    int scope = -1;                // -1 workbook, else sheet index
    std::string formula;           // without '=' e.g. "BOM!$A$1:$A$20"
    bool hidden = false;
};

class Worksheet {
public:
    std::string name = "Sheet1";
    uint32_t id = 0;               // stable id used by formula references (Book assigns)
    Argb tab_color = 0;
    bool hidden = false;
    bool show_grid = true;
    int zoom = 100;
    int freeze_rows = 0, freeze_cols = 0;   // panes frozen above/left
    int active_r = 0, active_c = 0;         // saved cursor
    double default_col_width = 8.43;        // chars
    double default_row_height = 15;         // points
    std::vector<RowStore> rows;             // index = row; may be shorter than the used row
    std::vector<ColInfo> cols;              // index = col; may be short
    std::vector<Range> merges;
    AutoFilter autofilter;
    std::vector<CondFormat> cond;
    std::vector<DataValidation> validations;
    // Dynamic-array spill results (anchor -> array). Cells covered read through value().
    std::map<std::pair<int, int>, std::shared_ptr<const Array>> spills;

    // ---- read access ----
    const Cell* cell(int r, int c) const;              // nullptr if never set
    const Value& value(int r, int c) const;            // includes spilled values
    uint32_t style_at(int r, int c) const;             // cell style, else row style, else col style
    int nrows() const;                                 // 1 + last row with any cell / row info
    int ncols() const;                                 // 1 + last col with any cell
    Range used_range() const;                          // invalid when empty
    double col_width(int c) const;                     // chars
    double row_height(int r) const;                    // points
    bool row_hidden(int r) const { return r < (int)rows.size() && rows[r].is_hidden(); }
    bool col_hidden(int c) const { return c < (int)cols.size() && cols[c].hidden; }
    const Range* merge_at(int r, int c) const;         // merged area containing (r,c)
    std::pair<int, int> spill_anchor(int r, int c) const;   // {-1,-1} if not in a spill

    // ---- raw mutation (NO undo, NO recalc; for I/O and engine internals) ----
    Cell& at(int r, int c);                            // creates
    void set_value_raw(int r, int c, Value v);         // keeps style, drops formula
    void clear_raw(int r, int c);                      // removes content+style
    RowStore& row(int r);                              // creates
    ColInfo& col(int c);                               // creates
};

// What a cell looks like on screen: style after conditional formatting,
// formatted text and resolved alignment. Built for the grid painter.
struct CellLook {
    Style style;                   // effective (dxf merged)
    std::string text;              // formatted display text
    Argb text_color = 0;           // number-format colour or font colour (0 = auto/black)
    Argb fill_color = 0;           // solid fill to paint (0 = none); colour scales land here
    HAlign align = HAlign::Left;   // resolved (General -> Right for numbers, Center for bool/err)
    float bar = -1;                // data bar fraction 0..1, -1 none
    Argb bar_color = 0;
    bool dropdown = false;         // list validation applies
    bool is_formula = false;
    bool overflow_ok = false;      // text may overflow into empty neighbours (left/general, no wrap)
};

// ========================================================= undo/redo =====
struct Command {
    std::string label;
    virtual ~Command() = default;
    virtual void undo(Book& b) = 0;
    virtual void redo(Book& b) = 0;
};

// ========================================================== clipboard =====
// Internal copy buffer: formulas keep relative refs (shift on paste),
// styles are carried by value so pasting into another Book works.
struct ClipBlock {
    int rows = 0, cols = 0;
    std::vector<Cell> cells;                 // row-major; style field indexes `styles`
    std::vector<Style> styles;
    std::vector<std::string> formula_text;   // per cell, A1 text at source (for cross-book paste)
    int src_sheet = -1, src_r = 0, src_c = 0;
    const void* src_book = nullptr;
};
enum PasteWhat : unsigned { PasteValues = 1, PasteFormulas = 2, PasteFormats = 4, PasteAll = 7, PasteTranspose = 8, PasteSkipBlanks = 16 };

// Text clipboard: Excel-style TSV of displayed values (CRLF, quoted when needed).
// HTML: <table> with inline CSS (fonts, fills, borders, alignment, merges).
// CF_HTML: Windows "HTML Format" envelope (Version:0.9 + byte offsets).
std::string cf_html_wrap(const std::string& html_fragment);
std::string cf_html_unwrap(const std::string& cf_html);          // returns the fragment/html
// Parse the first <table> of an HTML document into values + styles.
bool parse_html_table(const std::string& html, ClipBlock& out);
ClipBlock clip_from_tsv(const std::string& tsv);                 // typed by parse_input

// =========================================================== workbook =====
struct RecalcStats { int evaluated = 0; int circular = 0; double ms = 0; };

class Book {
public:
    Book();                                   // one empty sheet "Sheet1"
    ~Book();
    Book(Book&&) noexcept;
    Book& operator=(Book&&) noexcept;
    Book(const Book&) = delete;
    Book& operator=(const Book&) = delete;
    Book clone() const;                       // deep copy (no undo history)

    // ---- workbook data (direct access OK for reading; edit via the API below) ----
    std::vector<Worksheet> sheets;
    StyleTable styles;
    std::vector<Dxf> dxfs;
    std::vector<DefinedName> names;
    bool date1904 = false;
    bool auto_calc = true;
    int active_sheet = 0;
    uint64_t gen = 0;                         // bumps on every change (UI cache key)

    int sheet_count() const { return (int)sheets.size(); }
    Worksheet& sheet(int si) { return sheets[si]; }
    const Worksheet& sheet(int si) const { return sheets[si]; }
    int find_sheet(const std::string& name) const;     // case-insensitive, -1
    int sheet_index_of_id(uint32_t id) const;          // -1
    const DefinedName* find_name(const std::string& name, int scope_si) const;

    // ---- display ----
    std::string display_text(int si, int r, int c) const;   // formatted value
    std::string input_text(int si, int r, int c) const;     // for the editor: "=SUM(A1:A3)", "0805", dates as m/d/yyyy
    CellLook look(int si, int r, int c) const;
    const Style& style_of(int si, int r, int c) const { return styles.get(sheets[si].style_at(r, c)); }
    std::vector<std::string> list_items(int si, int r, int c) const;   // dropdown items for list validation
    // Check a proposed value against data validation; false -> msg (error title/text)
    bool validate(int si, int r, int c, const Value& v, std::string* msg = nullptr, DataValidation::ErrStyle* how = nullptr) const;

    // ---- editing (undoable; each call is one undo step unless inside a batch) ----
    void set_input(int si, int r, int c, const std::string& text);   // parses like Excel typing ("=..." -> formula)
    void set_value(int si, int r, int c, const Value& v);
    bool set_formula(int si, int r, int c, const std::string& text_without_eq, std::string* err = nullptr);
    void set_inputs(int si, int r, int c, const std::vector<std::vector<std::string>>& block);  // bulk, one step
    enum ClearWhat : unsigned { ClearContents = 1, ClearFormats = 2, ClearAll = 3 };
    void clear(int si, const Range& rg, unsigned what = ClearContents);
    void apply_style(int si, const Range& rg, const std::function<void(Style&)>& fn, const char* label = "Format");
    void set_number_format(int si, const Range& rg, const std::string& code);
    void set_col_width(int si, int c0, int c1, double chars);
    void set_row_height(int si, int r0, int r1, double points);
    void hide_rows(int si, int r0, int r1, bool hide);
    void hide_cols(int si, int c0, int c1, bool hide);
    void insert_rows(int si, int at, int n);      // shifts cells, styles, merges, CF, DV, refs in all formulas
    void delete_rows(int si, int at, int n);      // refs into deleted cells become #REF!
    void insert_cols(int si, int at, int n);
    void delete_cols(int si, int at, int n);
    void merge(int si, const Range& rg);          // keeps top-left content
    void unmerge(int si, const Range& rg);
    void set_freeze(int si, int rows, int cols);
    void sort(int si, const Range& rg, const std::vector<SortKey>& keys, bool has_header);
    void set_autofilter(int si, const Range& rg);  // invalid range removes
    void set_filter_column(int si, const FilterColumn& fc);   // and re-applies the filter
    void clear_filters(int si);
    void reapply_filter(int si);
    std::vector<std::string> filter_values(int si, int col_offset) const;   // distinct displayed values (sorted)
    void fill(int si, const Range& src, const Range& dst);    // fill down/right/up/left; series for numbers & "R1"
    void add_cond_format(int si, const CondFormat& cf);
    void set_cond_formats(int si, std::vector<CondFormat> list);
    void add_validation(int si, const DataValidation& dv);
    void set_validations(int si, std::vector<DataValidation> list);
    void define_name(const DefinedName& n);       // replaces same name+scope
    void remove_name(const std::string& name, int scope);
    int  add_sheet(const std::string& name, int at = -1);   // returns index (unique name enforced)
    void remove_sheet(int si);                    // refs to it become #REF!
    void rename_sheet(int si, const std::string& name);
    void move_sheet(int from, int to);
    void set_tab_color(int si, Argb c);
    // Refdes-friendly helpers on a column range
    void map_cells(int si, const Range& rg, const std::function<std::string(const std::string&)>& fn, const char* label);

    // clipboard
    ClipBlock copy(int si, const Range& rg) const;
    void paste(int si, int r, int c, const ClipBlock& blk, unsigned what = PasteAll);
    std::string copy_tsv(int si, const Range& rg) const;
    std::string copy_html(int si, const Range& rg) const;          // fragment (wrap with cf_html_wrap)

    // ---- undo/redo ----
    void begin_batch(const std::string& label);   // nestable; outermost end commits one step
    void end_batch();
    struct Batch { Book& b; Batch(Book& b_, const std::string& l) : b(b_) { b.begin_batch(l); } ~Batch() { b.end_batch(); } };
    bool can_undo() const;
    bool can_redo() const;
    std::string undo_label() const;
    std::string redo_label() const;
    bool undo();
    bool redo();
    void clear_undo();
    void push_command(std::unique_ptr<Command> cmd, bool already_applied = true);   // custom ops
    size_t undo_limit = 200;

    // ---- calculation ----
    RecalcStats recalc_all();                  // full rebuild of the dependency graph + evaluate
    RecalcStats recalc_dirty();                // dirty cells + dependents + volatiles
    void mark_dirty(int si, int r, int c);     // after raw edits (I/O); recalc_dirty() picks it up
    void invalidate_graph();                   // after raw structural edits
    const RecalcStats& last_recalc() const;
    Value evaluate(const std::string& formula_text, int si = 0, int r = 0, int c = 0) const;   // ad-hoc (no side effects)
    std::vector<std::pair<int, int>> precedents(int si, int r, int c) const;   // direct, flattened (capped)
    std::vector<std::pair<int, int>> dependents(int si, int r, int c) const;   // direct, same sheet

    // raw formula install without undo/recalc (I/O): compile + attach, keep cached value
    bool set_formula_raw(int si, int r, int c, const std::string& text_without_eq, const Value* cached = nullptr);

    // engine internals (formula_eval.cpp)
    struct Calc;
    Calc& calc() const { return *calc_; }

    // engine internal: record an applied command (undo stack / batch) and recalc
    void commit_(std::unique_ptr<Command> cmd, bool structural);

private:
    std::unique_ptr<Calc> calc_;
    struct UndoState;
    std::unique_ptr<UndoState> undo_;
    friend struct CellsCommand;
    friend struct BookSnapshotCommand;
};

// Legacy bridge ------------------------------------------------------------
struct Workbook;
Book from_legacy(const Workbook& wb);           // strings -> typed via parse_input (keeps "0805" text)
Workbook to_legacy(const Book& b);              // displayed text of every cell

// BOM helpers (also exposed as REFCOUNT/REFEXPAND/REFCOMPRESS functions)
int refdes_count(const std::string& cell);      // "R1-R5,R7" -> 6

// ===========================================================================
// LEGACY string-grid API (unchanged; used by sheet_editor.cpp / text_editor)
// ===========================================================================
using Row = std::vector<std::string>;

struct Sheet {
    std::string name = "Sheet1";
    std::vector<Row> rows;
    int ncols() const;                                     // widest row
    int nrows() const { return (int)rows.size(); }
    const std::string& at(int r, int c) const;             // "" when out of range
    void set(int r, int c, const std::string& v);          // grows as needed
    void trim_trailing();                                  // drop trailing empty cells / rows
};

struct Workbook {
    std::vector<Sheet> sheets;
};

// Natural compare ("R2" < "R10", case-insensitive). <0, 0, >0
int natural_cmp(const std::string& a, const std::string& b);

// RFC-4180 delimited parse of a whole document (quoted fields may span lines, "" escapes).
std::vector<Row> parse_delimited(const std::string& text, char sep, char quote = '"');
// Serialise rows; cells are quoted when they contain sep, quote, CR/LF or lead/trail blanks.
std::string to_delimited(const std::vector<Row>& rows, char sep, bool crlf = true, char quote = '"');
// Excel clipboard: TSV, CRLF, quotes only when needed. Parsing tolerates LF and a trailing newline.
std::string to_clipboard(const std::vector<Row>& rows);
std::vector<Row> from_clipboard(const std::string& text);

// Text file -> sheet using io::analyze_text structure detection (delimiter / fixed cuts).
Sheet sheet_from_text(const std::string& bytes, std::string* how = nullptr);

// Is the cell a plain number that may be written as numeric (no leading zeros like "0805").
bool is_plain_number(const std::string& s);

// Reference designator helpers
std::string expand_refs_cell(const std::string& cell, const std::string& sep = ", ");
std::string compress_refs(const std::vector<std::string>& refs, const std::string& sep = ", ");
std::string compress_refs_cell(const std::string& cell, const std::string& sep = ", ");

// Column operations (in place; return number of changed cells / rows)
int trim_cells(Sheet& s, int c0, int c1, int r0 = 0, int r1 = -1);
int remove_empty_rows(Sheet& s);
int dedupe_rows(Sheet& s, int first_row = 0);
int split_column(Sheet& s, int col, const std::string& delim, int first_row = 0);
int merge_columns(Sheet& s, int c0, int c1, const std::string& sep, int first_row = 0);
void sort_rows(Sheet& s, int col, bool desc, int first_row = 0, bool natural = true);
void insert_rows(Sheet& s, int at, int n);
void delete_rows(Sheet& s, int at, int n);
void insert_cols(Sheet& s, int at, int n);
void delete_cols(Sheet& s, int at, int n);

} // namespace pcbp::sheet
