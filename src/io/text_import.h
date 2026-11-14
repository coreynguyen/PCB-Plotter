#pragma once
// ============================================================================
// text_import.h — robust tabular text import engine (pcbp::io).
//
// Shared engine behind the BOM import (CircuitCAM #166 "BOM Import") and the
// ASCII centroid / pick-and-place import (#508 "ASCII Import"). In both
// dialogs the user sees the raw file, picks Fixed Columns vs Delimited (+
// delimiter char), tags each column with a field (Reference, X, Y, Rotation,
// Side, Part Number, Package, Description, ...) and can save the layout as a
// "Format Template". This module provides everything below the UI:
//
//   1. decode_text()      bytes -> UTF-8 (UTF-8 BOM, UTF-16LE/BE BOM or
//                         heuristic, Windows-1252 fallback), LF line endings.
//   2. analyze_text()     structure detection: delimiter (, ; TAB | runs of
//                         spaces) or fixed-width cuts, RFC-4180 quoting incl.
//                         embedded newlines and doubled quotes, Excel "sep=;"
//                         and ="0805" cells, decimal comma, comment lines,
//                         preamble / title rows, (multi-row / commented)
//                         header detection, trailing summary rows.
//   3. Column model       header text, samples, inferred value type and
//                         confidence, Role assignment from a multi-CAD /
//                         multi-language header synonym table and, when no
//                         header exists, from value patterns.
//   4. FormatTemplate     JSON-serialisable layout + roles + PNP unit/side/
//                         rotation conventions; template file load/save and
//                         matching a file against stored templates.
//   5. Value helpers      length with units, angles, side, booleans, refdes
//                         list expansion with Qty cross-check.
//
// Every step is overridable through ImportLayout so a UI can re-run
// analyze_text() live while the user changes delimiter / cuts / roles.
// ============================================================================
#include "../core/json.h"
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pcbp::io {

// ---------------------------------------------------------------------------
// Basic enums
// ---------------------------------------------------------------------------
enum class LenUnit { Auto, Mil, Inch, MM, CM };
const char* len_unit_name(LenUnit u);
double len_unit_mm(LenUnit u);         // mm per unit (Auto -> 0)
LenUnit len_unit_from_word(const std::string& w);   // "mm","mils","in","\"",... ; Auto if unknown

enum class Encoding { Auto, Utf8, Utf8Bom, Utf16LE, Utf16BE, Cp1252 };
const char* encoding_name(Encoding e);

enum class Sep { Auto, Comma, Semicolon, Tab, Pipe, Custom, Spaces /*runs of 2+ blanks*/, Whitespace /*any blank run*/, Fixed };
const char* sep_name(Sep s);

enum class Purpose { Generic, Bom, Pnp };

// Explicit structure mode (Excel "Original data type"). Auto keeps the legacy
// ImportLayout::sep behaviour (detection unless sep is forced).
enum class SplitMode : uint8_t { Auto, Delimited, Fixed };
const char* split_mode_name(SplitMode m);

// Per-column data format (Excel step 3).
enum class ColFormat : uint8_t { General, Text, Number, Angle, Skip };
const char* col_format_name(ColFormat f);

// Column roles (union of BOM and PNP fields; CircuitCAM names in comments).
enum class Role : uint8_t {
    Unused = 0,
    Ref,            // "Reference" (single ref or ref list)
    X, Y,           // PNP
    Rotation,       // PNP "Rotation"
    Side,           // PNP "Side"
    PartNumber,     // "Part Number"
    Package,        // "Package" / "Type" / footprint
    Value,
    Description,
    Qty,
    Manufacturer,
    MPN,            // manufacturer part number
    Vendor,
    VendorPN,       // "Vendor Part Number" / supplier / distributor PN
    CustPN,
    Dnp,            // DNP / fitted flag (Column::invert for "Fitted"-like headers)
    Process,        // "Process" (SMT/AUTO/SLIDE/HAND...)
    Polarity,
    Height,
    Cost,
    SoftRev,
    PartRev,
    StockCode,
    Bin,
    LineItem,
    PopType,
    PartMarking,
    Socket,
    Unit,           // per-row length unit column (PNP)
    Count_
};
const char* role_name(Role r);                      // display name ("Part Number")
const char* role_key(Role r);                       // stable JSON key ("part_number")
Role role_from_key(const std::string& key);         // inverse of role_key; Unused if unknown
std::vector<Role> roles_for(Purpose p);             // roles a UI should offer for a purpose

// Inferred value types.
enum class VType : uint8_t {
    Empty, Text, Integer, Number, Length /*number+unit*/, Angle, SideTok, Ref, RefList,
    Bool, PartNo, PackageName, CompValue /*10k, 100nF*/, Words /*description*/
};
const char* vtype_name(VType t);

enum class RoleSource : uint8_t { None, Header, Pattern, Template, User };

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------
// Decodes bytes to UTF-8 with '\n' line endings. `forced` != Auto skips detection.
std::string decode_text(const std::string& bytes, Encoding forced = Encoding::Auto,
                        Encoding* detected = nullptr);

// ---------------------------------------------------------------------------
// Layout (user/template overridable analysis parameters)
// ---------------------------------------------------------------------------
struct ImportLayout {
    Encoding encoding = Encoding::Auto;
    Sep sep = Sep::Auto;
    char custom_sep = 0;                // Sep::Custom
    std::vector<int> fixed_cuts;        // Sep::Fixed: column start positions (0-based chars, ascending)
    char quote = '"';
    int tab_size = 8;                   // tab expansion for fixed-width analysis / preview
    int replace_tabs = 0;               // CircuitCAM "Replace Tabs with N spaces" (0 = off; fixed only)
    int skip_lines = 0;                 // raw lines ignored before analysis
    int header_line = -1;               // -1 auto, -2 none, >=0 raw line index (0-based) of header
    int header_rows = 0;                // 0 auto, else number of header lines (multi-row)
    int decimal_comma = -1;             // -1 auto, 0 no, 1 yes
    std::vector<std::string> comment_prefixes{";", "#", "//", "*"};
    bool skip_summary = true;           // drop trailing "Total ..." / ragged rows
    bool trim = true;                   // CircuitCAM "Trim Whitespace"
    std::map<int, Role> role_override;  // column index -> role chosen by the user
    std::map<int, bool> invert_override;// column index -> invert flag (Dnp/Side/bool columns)

    // ---- explicit wizard controls (all optional; defaults = auto) ----
    SplitMode mode = SplitMode::Auto;   // Delimited: use `delims`/`other_delim` (empty = detect char); Fixed: fixed_cuts (empty = detect)
    std::string delims;                 // Delimited: set of single-char delimiters, e.g. "\t;" (any of them splits)
    std::string other_delim;            // Delimited: extra delimiter string (may be multi-char, e.g. "::" or " - ")
    bool consecutive_as_one = false;    // Delimited: runs of delimiters count as one (and leading/trailing ones are ignored)
    int stop_line = -1;                 // raw line index (0-based) where the table ends (exclusive); -1 = EOF
    char thousands_sep = 0;             // 0 none; ',' '.' ' ' '\'' removed from numeric cells
    bool trailing_minus = false;        // "12.5-" -> "-12.5"
    std::map<int, ColFormat> col_format;// column index -> format (Skip = never mapped)
    std::map<int, LenUnit> unit_override;   // column index -> length unit for this column
    std::map<int, bool> negate;         // column index -> numeric values negated
};

// ---------------------------------------------------------------------------
// Analysis result
// ---------------------------------------------------------------------------
struct Column {
    std::string header;                 // detected header text ("" if none)
    std::vector<std::string> samples;   // first non-empty values (<= 8)
    VType type = VType::Empty;
    double type_conf = 0;               // fraction of non-empty cells matching type
    int non_empty = 0;
    Role role = Role::Unused;
    double role_conf = 0;
    RoleSource role_src = RoleSource::None;
    bool invert = false;                // Dnp role: column means "fitted"; Side: "mirror"-style
    LenUnit unit = LenUnit::Auto;       // unit from header ("(mm)") or uniform cell suffix
    int fixed_start = -1, fixed_end = -1;   // Fixed: char span [start,end) ; end -1 = EOL
    ColFormat format = ColFormat::General;
    bool negate = false;
};

enum class RowKind : uint8_t { Data, Header, Preamble, Comment, Blank, Summary, RepeatedHeader, Ragged };
const char* row_kind_name(RowKind k);

struct Row {
    int line = 0;                       // 0-based raw line where the record starts
    int line_count = 1;                 // >1 when a quoted cell spans lines
    RowKind kind = RowKind::Data;
    std::vector<std::string> cells;     // always ncols() cells for Data rows
    std::string note;                   // reason for non-data kinds
};

struct ParsedText {
    Encoding encoding = Encoding::Utf8;
    std::vector<std::string> lines;     // decoded raw lines (for the preview pane)
    Sep sep = Sep::Auto;
    char sep_char = 0;                  // delimiter char for delimited modes
    std::vector<int> cuts;              // Fixed: column starts
    SplitMode mode = SplitMode::Delimited;   // effective mode (Fixed for fixed cuts, else Delimited)
    std::string delims;                 // effective single-char delimiters (" " for whitespace modes)
    std::string other_delim;            // effective multi-char delimiter ("" none)
    bool consecutive_as_one = false;    // effective (true for whitespace modes)
    char quote = '"';
    int stop_line = -1;                 // effective table end (exclusive), -1 EOF
    bool decimal_comma = false;
    bool excel_sep_hint = false;        // first line was "sep=X"
    int header_line = -1;               // raw line index of (first) header line, -1 none
    int header_rows = 0;
    bool header_commented = false;
    std::vector<Column> cols;
    std::vector<Row> rows;              // every record (data and skipped) in file order
    std::vector<std::string> notes;     // detection diagnostics
    std::vector<std::string> meta;      // comment / preamble text (for unit hints)
    LenUnit unit_hint = LenUnit::Auto;  // unit stated in comments/preamble ("Unit = mm")

    size_t ncols() const { return cols.size(); }
    int data_rows() const;
    int col(Role r) const;              // first column with role, -1 if none
    std::vector<int> cols_with(Role r) const;
    std::string cell(const Row& row, Role r) const;   // "" if role unmapped
    std::string describe() const;
};

// Run the whole analysis. Purpose influences role assignment only.
bool analyze_text(const std::string& bytes, const ImportLayout& layout, Purpose purpose,
                  ParsedText& out, std::string& err);
bool analyze_file(const std::string& path, const ImportLayout& layout, Purpose purpose,
                  ParsedText& out, std::string& err);

// Re-run only role assignment (cheap; for live role edits in a UI).
void assign_roles(ParsedText& t, const ImportLayout& layout, Purpose purpose);

// Header text -> role (synonym table). conf 0 when unknown; unit gets "(mm)" etc.
Role role_from_header(const std::string& header, Purpose purpose, double* conf = nullptr,
                      LenUnit* unit = nullptr, bool* invert = nullptr);

// Value-type inference for a single cell / a column of cells.
VType classify_cell(const std::string& cell, bool decimal_comma);
VType classify_column(const std::vector<std::string>& cells, bool decimal_comma, double* conf);

// Quote-aware delimited split of a single line.
std::vector<std::string> split_delim(const std::string& line, char sep, char quote = '"');
// Quote-aware split on any char of `delims` and/or the string `other` (multi-char);
// `consecutive` merges runs of delimiters and drops leading/trailing ones.
std::vector<std::string> split_multi(const std::string& line, const std::string& delims, const std::string& other,
                                     char quote = '"', bool consecutive = false);
// Fixed-width column starts suggested for the given (tab-expanded) lines.
std::vector<int> suggest_fixed_cuts(const std::vector<std::string>& lines, int tab_size = 8);
// Numeric cell clean-up used for Number/Angle (and numeric-looking General) cells:
// thousands separator removal, trailing minus, negation. Returns the input unchanged when not numeric.
std::string normalize_numeric_cell(const std::string& s, char thousands, bool decimal_comma, bool trailing_minus, bool negate);

// ---------------------------------------------------------------------------
// Value helpers
// ---------------------------------------------------------------------------
// Number with optional decimal comma (only when allow_comma), thousands-free.
std::optional<double> parse_number(const std::string& s, bool decimal_comma);
// "12.5mm", "500 mil", "0.5in", "0.5\"", "-3,2 mm" ; unit Auto when no suffix.
std::optional<double> parse_length(const std::string& s, bool decimal_comma, LenUnit* unit);
// "90", "-90.0", "90°", "90 deg", "R90" -> normalized? no: raw value (deg).
std::optional<double> parse_angle(const std::string& s, bool decimal_comma);
double normalize_angle(double deg);                 // -> [0,360)
// TRUE/FALSE, Y/N, YES/NO, 1/0, X, DNP/DNI/NC, FITTED/NOT FITTED ... ; nullopt if unknown.
std::optional<bool> parse_flag(const std::string& s);
// DNP-style cell -> true when the part is NOT to be placed (handles "fitted" semantics).
bool cell_means_dnp(const std::string& s, bool invert_column);

// Ref expansion: "R1-R5", "R1..R5", "R1:R5", "R1~R5", "C1,C3-5", "U1 U2", "r1 - r5",
// "U1A-U1D", "R1A-R5A", "R01-R10". Rules and limits: specs/REFEXPAND.md.
// Output is bounded (max_span per range, max_total per cell) for ANY input.
enum class RefIssue : uint8_t {
    Reversed, SpanTooLarge, PrefixMismatch, SuffixMismatch, PadMismatch, Malformed, TotalLimit,
    Duplicate,      // info: dropped
    BareNumber      // info: "C1,3" -> C3 (only with bare_numbers)
};
const char* ref_issue_name(RefIssue k);
struct RefExpandOptions {
    bool upper = true;
    int max_span = 2000;                // refs per single range ("R1-R2000" ok, "R1-R2001" error)
    size_t max_total = 20000;           // refs per cell
    size_t max_input = 65536;           // characters examined per cell
    bool bare_numbers = false;          // "C1, 3, 5" -> C1 C3 C5 (reported as info)
};
struct RefIssueItem { RefIssue kind; std::string token, msg; bool error = true; };
struct RefExpansion {
    std::vector<std::string> refs;      // unique (case-insensitive), file order
    std::vector<RefIssueItem> issues;
    std::vector<std::string> duplicates;
    int errors() const { int n = 0; for (auto& i : issues) n += i.error; return n; }
};
RefExpansion expand_refs_ex(const std::string& s, const RefExpandOptions& opt = {});
// Legacy wrapper: refs only; error messages appended to `warnings`. Bad ranges are
// kept as literal tokens (they never match a board reference).
std::vector<std::string> expand_refs(const std::string& s, bool upper = true,
                                     std::vector<std::string>* warnings = nullptr);
bool is_refdes(const std::string& s);
bool is_ref_list(const std::string& s);             // >= 1 ref, separators / ranges allowed

// ---------------------------------------------------------------------------
// Format templates
// ---------------------------------------------------------------------------
enum class BottomX { AsIs, Negate, MirrorWidth };          // how bottom X was exported
enum class BottomRot { AsIs, Negate, Mirror180, Plus180 };  // rot -> rot / -rot / 180-rot / rot+180

struct FormatTemplate {
    std::string name;
    Purpose purpose = Purpose::Generic;
    ImportLayout layout;                // sep, cuts, skip, header line/rows, decimal comma, encoding
    std::vector<std::pair<std::string, Role>> header_roles;   // normalized header -> role
    std::vector<Role> index_roles;      // headerless files: role per column index
    std::vector<std::string> signature; // normalized header texts used for matching
    int ncols = 0;
    // PNP conventions
    LenUnit units = LenUnit::Auto;
    double factor = 1.0;
    double rot_offset = 0;
    bool mirror_x = false, mirror_y = false;
    BottomX bottom_x = BottomX::AsIs;
    BottomRot bottom_rot = BottomRot::AsIs;
    std::vector<std::string> top_tokens, bottom_tokens;   // empty = defaults

    json::Value to_json() const;
    static bool from_json(const json::Value& v, FormatTemplate& out);
};

// Build a template from the current analysis (captures roles by header and index).
FormatTemplate make_template(const std::string& name, Purpose p, const ParsedText& t,
                             const ImportLayout& layout);
// Layout to feed analyze_text() for a template (roles resolved against headers later
// with apply_template_roles()).
ImportLayout template_layout(const FormatTemplate& t);
// Apply template roles onto an analysed file (by header text, then by index).
void apply_template_roles(const FormatTemplate& tpl, ParsedText& t, ImportLayout& layout);

bool save_templates(const std::string& path, const std::vector<FormatTemplate>& v, std::string& err);
bool load_templates(const std::string& path, std::vector<FormatTemplate>& v, std::string& err);
std::string templates_to_json(const std::vector<FormatTemplate>& v);
bool templates_from_json(const std::string& text, std::vector<FormatTemplate>& v, std::string& err);

// Score 0..1 of how well a template matches an analysed file (header signature
// Jaccard; headerless: column count + separator). Returns best index or -1 (< min_score).
double template_score(const FormatTemplate& tpl, const ParsedText& t);
int match_template(const std::vector<FormatTemplate>& v, const ParsedText& t, Purpose p,
                   double min_score = 0.75, double* score = nullptr);

std::string norm_header(const std::string& s);      // lower, alnum + non-ASCII kept, units stripped

} // namespace pcbp::io
