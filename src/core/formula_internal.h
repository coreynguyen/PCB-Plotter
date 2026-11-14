#pragma once
// ============================================================================
// formula_internal.h — engine internals shared by formula*.cpp / sheet*.cpp.
// Not part of the public API (UI code must not include this).
// ============================================================================
#include "sheet.h"
#include <array>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace pcbp::sheet {

// ---- small string helpers ----
std::string upper_ascii(std::string s);
std::string lower_utf8(const std::string& s);       // ASCII + Latin-1/Greek/Cyrillic basic
std::string upper_utf8(const std::string& s);
int ci_compare(const std::string& a, const std::string& b);   // case-insensitive (UTF-8 aware lowering)
bool ci_equal(const std::string& a, const std::string& b);
std::vector<uint32_t> utf8_decode(const std::string& s);
std::string utf8_encode(const std::vector<uint32_t>& cps, size_t from = 0, size_t n = (size_t)-1);
void utf8_append(std::string& o, uint32_t cp);
size_t utf8_len(const std::string& s);
bool wildcard_match(const std::string& pattern, const std::string& text, bool ci = true);   // * ? ~
std::string fmt_general(double d, int max_sig = 15);   // Excel General-ish number text (no width limit)
bool parse_number_strict(const std::string& s, double& out);   // whole string is a number (allows spaces, no %)

// ---- operand (evaluator stack item) ----
struct Area { int si; Range rg; };
struct Opnd {
    enum K : uint8_t { V, A, R, RL } k = V;   // value, array, single-area reference, multi-area reference
    Value v;
    std::shared_ptr<const Array> arr;
    Area ref{0, Range()};
    std::vector<Area> refs;
    bool missing = false;     // omitted function argument

    static Opnd val(Value x) { Opnd o; o.k = V; o.v = std::move(x); return o; }
    static Opnd num(double d) { return val(Value::number(d)); }
    static Opnd err(Err e) { return val(Value::error(e)); }
    static Opnd str(std::string s) { return val(Value::text(std::move(s))); }
    static Opnd boolean(bool b) { return val(Value::boolean(b)); }
    static Opnd array(std::shared_ptr<const Array> a) { Opnd o; o.k = A; o.arr = std::move(a); return o; }
    static Opnd area(int si, Range rg) { Opnd o; o.k = R; o.ref = {si, rg}; return o; }
    bool is_ref() const { return k == R || k == RL; }
    bool is_err() const { return k == V && v.is_err(); }
};

struct NeedCell { int si, r, c; };   // thrown when a dirty precedent must be computed first
struct NeedCells { std::vector<std::array<int, 3>> cells; };   // several dirty precedents of one range

// Calc state flags in Cell::calc
enum : uint8_t { CS_DIRTY = 1, CS_ONSTACK = 2, CS_CIRC = 4 };

class Evaluator;
using FnImpl = Opnd (*)(Evaluator& ev, std::vector<Opnd>& a);

struct FnDef {
    FunctionInfo info;
    FnImpl impl;
    const char* args;       // per-arg kind, last repeats: 'v' scalar (lifted over arrays), 'r' ref/any, 'a' array/any
    uint8_t flags;          // FN_VOLATILE | FN_DYNREF
};
enum : uint8_t { FN_VOLATILE = 1, FN_DYNREF = 2 };
const std::vector<FnDef>& fn_table();
int fn_lookup(std::string_view name);   // id or -1

// ---- the evaluator ----
class Evaluator {
public:
    Evaluator(const Book& b, int si, int r, int c) : book(b), si(si), r(r), c(c) {}
    const Book& book;
    int si, r, c;               // host cell
    bool probe = false;         // ad-hoc evaluation: dirty precedents are read as cached (no throw)
    int depth = 0;

    Opnd run(const Formula& f);
    // cell access (throws NeedCell for dirty formula cells unless probe)
    const Value& cell_value(int si, int r, int c);
    // Dereference: ref -> scalar (implicit intersection with host row/col), array -> [0,0]
    Value scalar(const Opnd& o);
    // ref/array/value -> Array (whole area)
    std::shared_ptr<const Array> to_array(const Opnd& o);
    Opnd call(const Tok& k, std::vector<Opnd>& args);
    Opnd ref_opnd(const Tok& k);
    // Visit every cell of a ref/array (values only). Refs skip nothing (empties included as Empty).
    template <class F> void each(const Opnd& o, F&& fn);
    Range clip_used(int si, const Range& rg) const;    // shrink whole-col/row ranges to used area
    void prefetch(int si, const Range& rg);           // throws NeedCells when the range holds dirty formulas
    // name resolution
    bool resolve_name(const std::string& name, Opnd& out);
    // INDIRECT helper
    bool parse_ref_text(const std::string& text, bool a1, Opnd& out);
};

template <class F> void Evaluator::each(const Opnd& o, F&& fn) {
    if (o.k == Opnd::V) { fn(o.v, false); return; }
    if (o.k == Opnd::A) { for (auto& x : o.arr->v) fn(x, false); return; }
    auto one = [&](const Area& a) {
        Range rg = clip_used(a.si, a.rg);
        prefetch(a.si, rg);
        for (int rr = rg.r0; rr <= rg.r1; ++rr)
            for (int cc = rg.c0; cc <= rg.c1; ++cc) fn(cell_value(a.si, rr, cc), true);
    };
    if (o.k == Opnd::R) one(o.ref);
    else for (auto& a : o.refs) one(a);
}

// value coercions (Excel rules). Return false + err on failure.
bool to_number(const Value& v, double& d, Err& e);           // text "12" ok, "" -> 0 only when empty-type
bool to_bool(const Value& v, bool& b, Err& e);
std::string to_text(const Value& v);                         // General formatting
int compare_values(const Value& a, const Value& b, bool case_sensitive = false);  // Excel sort order: num < text < bool; empty
Opnd apply_binary(Evaluator& ev, Tok::OpCode op, const Opnd& a, const Opnd& b);

// ---- compiled-formula utilities ----
// Resolve a ref part for host (r,c): absolute index or host+offset.
inline int abs_row(const RefPart& p, int host_r) { return p.abs_r ? p.row : host_r + p.row; }
inline int abs_col(const RefPart& p, int host_c) { return p.abs_c ? p.col : host_c + p.col; }

// Structural change applied to formulas.
struct StructOp {
    uint32_t sheet_id;     // sheet where rows/cols were inserted/deleted
    bool rows;             // true = rows, false = cols
    int at, n;             // n > 0 insert, n < 0 delete (-n rows/cols starting at `at`)
    // map an index; returns -1 when deleted
    int map(int i) const { if (i < at) return i; if (n > 0) return i + n; if (i < at - n) return -1; return i + n; }
};
// Returns a new formula with refs adjusted (or nullptr when nothing changed). host_old/new are the
// formula's own coordinates before/after the change; host_sheet_id is the id of the formula's sheet.
FormulaPtr adjust_formula(const Formula& f, uint32_t host_sheet_id, int old_r, int old_c, int new_r, int new_c, const StructOp& op);
// Formula moved (copy/paste/fill) keeps relative offsets: nothing to do except #REF! for off-grid refs.
// Re-host a formula onto another position with absolute refs fixed (for moving cells): refs keep pointing
// to the same absolute cells. Used by cut/paste and sort-free moves.
FormulaPtr rehost_formula(const Formula& f, int old_r, int old_c, int new_r, int new_c);
// Static precedents of a formula at host (si,r,c): areas (si resolved).
void formula_precedents(const Book& b, const Formula& f, int si, int r, int c, std::vector<Area>& out, bool& has_names);
// Sheet removed: refs to sheet_id become BadRef
FormulaPtr drop_sheet_refs(const Formula& f, uint32_t sheet_id);

// ---- calc engine internals (formula_eval.cpp) ----
struct CellKey {
    int32_t si, r, c;
    bool operator==(const CellKey& o) const { return si == o.si && r == o.r && c == o.c; }
};
struct CellKeyHash { size_t operator()(const CellKey& k) const { return ((size_t)k.si * 0x9E3779B97F4A7C15ull) ^ ((size_t)k.r * 0x100000001B3ull) ^ (size_t)k.c * 0xC2B2AE3D27D4EB4Full; } };

struct Book::Calc {
    bool graph_valid = false;
    bool needs_full = true;      // values may be stale everywhere (structural edit, snapshot undo)
    std::unordered_map<CellKey, std::vector<CellKey>, CellKeyHash> cell_deps;     // precedent cell -> dependents
    struct RangeDep { Area a; CellKey dep; };
    std::vector<RangeDep> range_deps;
    std::vector<CellKey> volatiles;
    std::vector<CellKey> name_users;          // formulas using names (always conservatively dirty)
    std::vector<CellKey> pending;             // dirty roots since last recalc
    RecalcStats last;
    bool in_recalc = false;
    std::unordered_map<std::string, std::shared_ptr<Formula>> name_cache;   // "scope|NAME" -> compiled
    // conditional-format caches (rebuilt when Book::gen changes)
    struct CfStats { double mn = 0, mx = 0, mean = 0, sd = 0, thr = 0; std::vector<double> sorted; std::unordered_map<std::string, int> counts; bool ok = false; };
    uint64_t cf_gen = ~0ull;
    std::unordered_map<std::string, CfStats> cf_stats;
    std::unordered_map<std::string, std::shared_ptr<Formula>> cf_formulas;
};

// Evaluate one formula cell now (used by the scheduler). Returns the result operand.
Opnd eval_formula_cell(const Book& b, int si, int r, int c, const Formula& f, bool probe);

// incremental dependency-graph maintenance
void graph_add(Book& b, int si, int r, int c, const Formula& f);
void graph_remove(Book& b, int si, int r, int c, const Formula& f);

// Book helpers implemented in sheet_model.cpp / used across files
Value opnd_to_cell_value(Evaluator& ev, const Opnd& o, std::shared_ptr<const Array>* arr_out);

// Number parsing for criteria & coercion
bool text_to_number(const std::string& s, double& d, bool date1904 = false);

} // namespace pcbp::sheet
