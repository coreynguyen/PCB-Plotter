#pragma once
// ============================================================================
// formula.h — spreadsheet values, formula compiler and function catalogue
// (pcbp::sheet). Portable, no Windows headers.
//
// Clean-room: behaviour follows ECMA-376 Part 1 §18.17 (formula grammar),
// the OpenFormula spec (ODF 1.2 part 2) and the public Excel / LibreOffice
// Calc function reference documentation. See specs/SHEET_ENGINE.md.
//
//   * Value      — typed cell value (empty / number / string / bool / error)
//   * Array      — 2-D value matrix (array constants, range results, spills)
//   * Formula    — compiled, position-independent RPN code. Relative
//                  references are stored as offsets from the host cell, so the
//                  same Formula can be shared by copied / filled cells.
//   * compile_formula / formula_text — text <-> code (A1 syntax, en-US)
//   * tokenize_formula — lexical tokens with positions for syntax colouring
//   * function_catalog — name / syntax / description of every built-in
// ============================================================================
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pcbp::sheet {

constexpr int MAX_ROWS = 1048576;   // Excel 2007+ grid
constexpr int MAX_COLS = 16384;     // A..XFD

// ---------------------------------------------------------------- errors ----
enum class Err : uint8_t {
    None = 0,
    Null,    // #NULL!   empty intersection
    Div0,    // #DIV/0!
    Value,   // #VALUE!
    Ref,     // #REF!
    Name,    // #NAME?
    Num,     // #NUM!
    NA,      // #N/A
    Spill,   // #SPILL!  dynamic array blocked
    Calc,    // #CALC!   e.g. empty FILTER result
    Circ,    // circular reference (shown as "#CIRC!", written to files as 0)
};
const char* err_text(Err e);                 // "#DIV/0!" ...
Err err_from_text(std::string_view s);       // Err::None when not an error literal

// ----------------------------------------------------------------- value ----
struct Value {
    enum Type : uint8_t { Empty, Number, String, Bool, Error };
    Type type = Empty;
    Err err = Err::None;      // Error
    double num = 0;           // Number; Bool as 0/1
    std::string str;          // String

    static Value number(double d) { Value v; v.type = Number; v.num = d; return v; }
    static Value text(std::string s) { Value v; v.type = String; v.str = std::move(s); return v; }
    static Value boolean(bool b) { Value v; v.type = Bool; v.num = b ? 1 : 0; return v; }
    static Value error(Err e) { Value v; v.type = Error; v.err = e; return v; }

    bool is_empty() const { return type == Empty; }
    bool is_num() const { return type == Number; }
    bool is_str() const { return type == String; }
    bool is_bool() const { return type == Bool; }
    bool is_err() const { return type == Error; }
    bool b() const { return num != 0; }
    bool operator==(const Value& o) const;
    bool operator!=(const Value& o) const { return !(*this == o); }
};

// Raw (General-format) text of a value: numbers up to 15 significant digits
// like Excel's General ("0.1", "1E+20", "TRUE", "#N/A").
std::string value_to_text(const Value& v);

// ----------------------------------------------------------------- array ----
struct Array {
    int rows = 0, cols = 0;
    std::vector<Value> v;     // row-major
    Array() = default;
    Array(int r, int c) : rows(r), cols(c), v((size_t)r * c) {}
    Value& at(int r, int c) { return v[(size_t)r * cols + c]; }
    const Value& at(int r, int c) const { return v[(size_t)r * cols + c]; }
};

// ------------------------------------------------------------ addressing ----
struct Range {                         // inclusive, 0-based
    int r0 = 0, c0 = 0, r1 = -1, c1 = -1;
    Range() = default;
    Range(int r0_, int c0_, int r1_, int c1_) : r0(r0_), c0(c0_), r1(r1_), c1(c1_) {}
    static Range cell(int r, int c) { return Range(r, c, r, c); }
    bool valid() const { return r0 >= 0 && c0 >= 0 && r1 >= r0 && c1 >= c0; }
    bool contains(int r, int c) const { return r >= r0 && r <= r1 && c >= c0 && c <= c1; }
    bool intersects(const Range& o) const { return r0 <= o.r1 && o.r0 <= r1 && c0 <= o.c1 && o.c0 <= c1; }
    int rows() const { return r1 - r0 + 1; }
    int cols() const { return c1 - c0 + 1; }
    Range norm() const;                 // swap so r0<=r1, c0<=c1
    bool operator==(const Range& o) const { return r0 == o.r0 && c0 == o.c0 && r1 == o.r1 && c1 == o.c1; }
};

std::string col_name(int c);                                   // 0 -> "A", 27 -> "AB"
int col_index(const std::string& letters);                     // inverse, -1 on error
std::string cell_name(int r, int c, bool abs_r = false, bool abs_c = false);   // "B3", "$B$3"
bool parse_cell_name(std::string_view s, int& r, int& c);      // "B3" / "$B$3"
std::string range_name(const Range& rg);                       // "A1:B5" ("A1" for single, "A:A" for full cols)
bool parse_range_name(std::string_view s, Range& rg);          // "A1:B5", "B3", "A:C", "2:5"
std::string quote_sheet_name(const std::string& name);         // 'Sheet 2' when needed

// ------------------------------------------------------- compiled formula ----
// One reference endpoint inside formula code.
struct RefPart {
    int32_t row = 0, col = 0;     // absolute index when abs_*, else offset from host cell
    bool abs_r = false, abs_c = false;
};

struct Tok {
    enum Kind : uint8_t {
        Num, Str, Bool, ErrLit,   // literals
        Ref,                       // single cell  (a)
        Area,                      // a:b  (whole_rows / whole_cols flags)
        ArrayConst,                // {1,2;3,4} -> arr
        Name,                      // defined name (str = name, upper-cased lookup)
        Func,                      // fn = function id, argc
        Op,                        // op, unary/binary
        Missing,                   // omitted argument  =IF(A1,,2)
        Paren,                     // display only: wraps previous expression in ()
        BadRef,                    // #REF! reference (deleted)
    };
    enum OpCode : uint8_t {
        Add, Sub, Mul, Div, Pow, Concat, Eq, Ne, Lt, Le, Gt, Ge,
        Neg, Plus, Percent,       // unary
        RangeOp, Union, Isect,    // reference operators ':' ',' ' '
        SpillRef,                 // postfix '#' (A1#)
    };
    Kind kind = Num;
    OpCode op = Add;
    uint8_t argc = 0;
    bool whole_rows = false, whole_cols = false;
    uint16_t fn = 0;
    int32_t sheet_id = -1;        // Ref/Area: -1 = host sheet, else Worksheet::id
    int32_t sheet_id2 = -1;       // 3-D refs Sheet1:Sheet3!A1 (else -1)
    RefPart a, b;
    double num = 0;
    std::string str;              // Str literal / Name / unknown function name
    std::shared_ptr<const Array> arr;
};

struct Formula {
    std::vector<Tok> code;        // RPN
    std::string bad_text;         // set when the text could not be parsed (value #NAME?/#VALUE!)
    bool is_volatile = false;     // NOW, TODAY, RAND, OFFSET, INDIRECT, CELL...
    bool dynamic_refs = false;    // INDIRECT / OFFSET: dependencies known only at run time
    bool is_array = false;        // legacy CSE array formula {=...}
    Range array_range;            // CSE target range (absolute) when is_array
};
using FormulaPtr = std::shared_ptr<const Formula>;

class Book;

// Compile `text` (without the leading '=') for host cell (sheet index si, r, c).
// Unknown function names compile (value #NAME?) so they round-trip.
// Returns false + err on a syntax error; `out.bad_text` keeps the text.
bool compile_formula(const std::string& text, const Book& book, int si, int r, int c,
                     Formula& out, std::string& err);
// Render code back to A1 text (without '='), relative refs resolved for the host cell.
std::string formula_text(const Formula& f, const Book& book, int si, int r, int c);

// ------------------------------------------------------- lexical tokens ----
// For formula-bar syntax colouring and reference highlighting.
struct LexToken {
    enum Kind : uint8_t { Space, Number, String, Bool, Error, Ref, Name, Function, Operator,
                          OpenParen, CloseParen, Comma, Semicolon, OpenBrace, CloseBrace, Unknown };
    Kind kind;
    int pos, len;                 // byte offsets in the input text
    int depth = 0;                // paren depth (for rainbow brackets)
};
std::vector<LexToken> tokenize_formula(std::string_view text);   // text with or without '='

// References used by a formula (absolute, resolved for the host cell) — for coloured
// highlighting of precedents while editing. sheet index -1 = host sheet.
struct UsedRef { int si; Range rg; int text_pos, text_len; };
std::vector<UsedRef> formula_refs(std::string_view text, const Book& book, int si, int r, int c);

// ------------------------------------------------------ function catalog ----
struct FunctionInfo {
    const char* name;             // "VLOOKUP"
    const char* category;         // "Lookup", "Math", "Statistical", "Text", "Logical", "Date", "Info", "BOM"
    const char* syntax;           // "VLOOKUP(lookup_value, table_array, col_index_num, [range_lookup])"
    const char* description;
    int min_args, max_args;       // max -1 = unlimited
};
const std::vector<FunctionInfo>& function_catalog();
const FunctionInfo* find_function(std::string_view name);   // case-insensitive, accepts "_xlfn." prefix

} // namespace pcbp::sheet
