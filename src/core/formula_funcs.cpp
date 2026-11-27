// ============================================================================
// formula_funcs.cpp — built-in worksheet functions. Each implementation is
// written from the public function reference (Microsoft Excel function docs /
// OpenFormula ODF 1.2 part 2 §6); see specs/SHEET_ENGINE.md §Functions for
// the documented edge cases we follow.
// ============================================================================
#include "formula_internal.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <numeric>
#include <random>
#include <set>
#include <cmath>

namespace pcbp::sheet {

namespace {

constexpr double M_PI_ = 3.14159265358979323846;
using Args = std::vector<Opnd>;

// ---------------------------------------------------------------- helpers --
inline bool missing(const Args& a, size_t i) { return i >= a.size() || a[i].missing; }

// scalar number from an operand (direct argument rules: text coerced)
bool num_of(Evaluator& ev, const Opnd& o, double& d, Opnd& err) {
    Value v = ev.scalar(o);
    Err e = Err::Value;
    if (!to_number(v, d, e)) { err = Opnd::err(e); return false; }
    return true;
}
#define NUM(var, idx) double var; { Opnd _e; if (!num_of(ev, a[idx], var, _e)) return _e; }
#define NUM_OR(var, idx, def) double var = def; if (!missing(a, idx)) { Opnd _e; if (!num_of(ev, a[idx], var, _e)) return _e; }
#define STR(var, idx) std::string var; { Value _v = ev.scalar(a[idx]); if (_v.is_err()) return Opnd::val(_v); var = to_text(_v); }
#define STR_OR(var, idx, def) std::string var = def; if (!missing(a, idx)) { Value _v = ev.scalar(a[idx]); if (_v.is_err()) return Opnd::val(_v); var = to_text(_v); }
#define BOOL_OR(var, idx, def) bool var = def; if (!missing(a, idx)) { Value _v = ev.scalar(a[idx]); Err _e = Err::Value; if (!to_bool(_v, var, _e)) { double _d; if (_v.is_str() && text_to_number(_v.str, _d)) var = _d != 0; else return Opnd::err(_e); } }

// 2-D view of a ref or array operand
struct Grid {
    Evaluator* ev = nullptr;
    bool is_ref = false;
    int si = 0;
    Range rg;                  // full logical area (refs)
    int real_rows = 0;         // rows that may contain data
    std::shared_ptr<const Array> arr;
    int rows = 0, cols = 0;
    Value scalar_v;
    bool is_scalar = false;
    bool whole = false;          // whole-column reference
    const Value& get(int i, int j) const {
        static const Value empty;
        if (is_scalar) return scalar_v;
        if (is_ref) {
            if (i >= real_rows) return empty;
            return ev->cell_value(si, rg.r0 + i, rg.c0 + j);
        }
        int ri = arr->rows == 1 ? 0 : i, cj = arr->cols == 1 ? 0 : j;
        if (ri >= arr->rows || cj >= arr->cols) return empty;
        return arr->at(ri, cj);
    }
    bool hidden_row(int i) const { return is_ref && ev->book.sheets[si].row_hidden(rg.r0 + i); }
};
Grid grid_of(Evaluator& ev, const Opnd& o) {
    Grid g; g.ev = &ev;
    if (o.k == Opnd::R) {
        g.is_ref = true; g.si = o.ref.si; g.rg = o.ref.rg;
        Range u = o.ref.rg;
        bool whole_r = u.r1 >= MAX_ROWS - 1, whole_c = u.c1 >= MAX_COLS - 1;
        if (whole_r || whole_c) {
            Range cl = ev.clip_used(o.ref.si, u);
            if (whole_c) g.rg.c1 = std::max(cl.c1, g.rg.c0);
            if (whole_r) g.rg.r1 = std::max(cl.r1, g.rg.r0);
        }
        g.rows = g.rg.rows(); g.cols = g.rg.cols();
        Range cl = ev.clip_used(o.ref.si, g.rg);
        g.real_rows = std::max(0, std::min(g.rows, cl.r1 - g.rg.r0 + 1));
        g.whole = whole_r;
        if (g.real_rows > 0) ev.prefetch(g.si, Range(g.rg.r0, g.rg.c0, g.rg.r0 + g.real_rows - 1, g.rg.c1));
        return g;
    }
    if (o.k == Opnd::V) { g.is_scalar = true; g.scalar_v = o.v; g.rows = g.cols = 1; return g; }
    g.arr = ev.to_array(o);
    g.rows = g.arr->rows; g.cols = g.arr->cols;
    return g;
}

// numbers from SUM-like args. mode_a: text->0, bool->num in refs (the *A functions)
bool collect(Evaluator& ev, const Args& a, size_t from, std::vector<double>& out, Opnd& err, bool mode_a = false, bool skip_hidden = false, bool skip_subtotal = false) {
    for (size_t i = from; i < a.size(); ++i) {
        const Opnd& o = a[i];
        if (o.missing) continue;
        if (o.k == Opnd::V) {
            const Value& v = o.v;
            if (v.is_err()) { err = Opnd::val(v); return false; }
            if (v.is_num() || v.is_bool()) { out.push_back(v.num); continue; }
            if (v.is_str()) { double d; if (text_to_number(v.str, d)) { out.push_back(d); continue; } if (mode_a) { out.push_back(0); continue; } err = Opnd::err(Err::Value); return false; }
            continue;
        }
        bool bad = false; Value bad_v;
        auto visit_area = [&](const Area& ar) {
            Range rg = ev.clip_used(ar.si, ar.rg);
            ev.prefetch(ar.si, rg);
            const Worksheet& ws = ev.book.sheets[ar.si];
            for (int r = rg.r0; r <= rg.r1 && !bad; ++r) {
                if (skip_hidden && ws.row_hidden(r)) continue;
                if (!skip_hidden && skip_subtotal && ws.rows.size() > (size_t)r && ws.rows[r].filtered) continue;
                for (int c = rg.c0; c <= rg.c1; ++c) {
                    if (skip_subtotal) {
                        const Cell* cl = ws.cell(r, c);
                        if (cl && cl->f) {
                            bool st = false;
                            for (auto& t : cl->f->code) if (t.kind == Tok::Func && t.fn < fn_table().size() && (!strcmp(fn_table()[t.fn].info.name, "SUBTOTAL") || !strcmp(fn_table()[t.fn].info.name, "AGGREGATE"))) { st = true; break; }
                            if (st) continue;
                        }
                    }
                    const Value& v = ev.cell_value(ar.si, r, c);
                    if (v.is_num()) out.push_back(v.num);
                    else if (v.is_err()) { bad = true; bad_v = v; break; }
                    else if (mode_a && v.is_bool()) out.push_back(v.num);
                    else if (mode_a && v.is_str()) out.push_back(0);
                }
            }
        };
        if (o.k == Opnd::R) visit_area(o.ref);
        else if (o.k == Opnd::RL) for (auto& ar : o.refs) visit_area(ar);
        else for (auto& v : o.arr->v) {
            if (v.is_num()) out.push_back(v.num);
            else if (v.is_err()) { bad = true; bad_v = v; break; }
            else if (mode_a && v.is_bool()) out.push_back(v.num);
            else if (mode_a && v.is_str()) out.push_back(0);
        }
        if (bad) { err = Opnd::val(bad_v); return false; }
    }
    return true;
}
#define COLLECT(vec, from, ...) std::vector<double> vec; { Opnd _e; if (!collect(ev, a, from, vec, _e, ##__VA_ARGS__)) return _e; }

Opnd num_result(double d) { if (!std::isfinite(d)) return Opnd::err(Err::Num); return Opnd::num(d); }

// ---------------------------------------------------------------- criteria --
struct Crit {
    int op = 0;        // 0 '=', 1 '<>', 2 '<', 3 '<=', 4 '>', 5 '>='
    Value v;
    bool wild = false;
    bool empty_eq = false;     // "" or "=" -> match blanks
    bool nonempty = false;     // "<>"
};
Crit make_crit(const Value& c0) {
    Crit c;
    if (!c0.is_str()) { c.v = c0.is_empty() ? Value::number(0) : c0; return c; }
    std::string s = c0.str;
    size_t k = 0;
    if (s.compare(0, 2, "<=") == 0) { c.op = 3; k = 2; } else if (s.compare(0, 2, ">=") == 0) { c.op = 5; k = 2; }
    else if (s.compare(0, 2, "<>") == 0) { c.op = 1; k = 2; } else if (!s.empty() && s[0] == '<') { c.op = 2; k = 1; }
    else if (!s.empty() && s[0] == '>') { c.op = 4; k = 1; } else if (!s.empty() && s[0] == '=') { c.op = 0; k = 1; }
    std::string rest = s.substr(k);
    if (rest.empty()) {
        if (c.op == 0) c.empty_eq = true;
        else if (c.op == 1) c.nonempty = true;
        c.v = Value::text("");
        return c;
    }
    double d;
    std::string u = upper_ascii(rest);
    if (text_to_number(rest, d)) c.v = Value::number(d);
    else if (u == "TRUE" || u == "FALSE") c.v = Value::boolean(u == "TRUE");
    else if (err_from_text(rest) != Err::None) c.v = Value::error(err_from_text(rest));
    else { c.v = Value::text(rest); c.wild = (c.op <= 1) && rest.find_first_of("*?~") != std::string::npos; }
    return c;
}
bool crit_match(const Crit& c, const Value& x) {
    if (c.empty_eq) return x.is_empty() || (x.is_str() && x.str.empty());
    if (c.nonempty) return !(x.is_empty());
    if (c.v.is_num()) {
        double xv;
        if (x.is_num()) xv = x.num;
        else if (x.is_str() && c.op == 0 && parse_number_strict(x.str, xv)) {}
        else return c.op == 1;
        switch (c.op) {
        case 0: return xv == c.v.num; case 1: return xv != c.v.num; case 2: return xv < c.v.num;
        case 3: return xv <= c.v.num; case 4: return xv > c.v.num; case 5: return xv >= c.v.num;
        }
        return false;
    }
    if (c.v.is_bool()) {
        if (!x.is_bool()) return c.op == 1;
        int cmp = (x.num > c.v.num) - (x.num < c.v.num);
        switch (c.op) { case 0: return cmp == 0; case 1: return cmp != 0; case 2: return cmp < 0; case 3: return cmp <= 0; case 4: return cmp > 0; case 5: return cmp >= 0; }
        return false;
    }
    if (c.v.is_err()) { bool eq = x.is_err() && x.err == c.v.err; return c.op == 1 ? !eq : (c.op == 0 && eq); }
    // text criterion
    if (c.op == 0 || c.op == 1) {
        bool eq;
        if (!x.is_str()) eq = false;
        else eq = c.wild ? wildcard_match(c.v.str, x.str) : ci_equal(c.v.str, x.str);
        return c.op == 0 ? eq : !eq;
    }
    if (!x.is_str()) return false;
    int cmp = ci_compare(x.str, c.v.str);
    switch (c.op) { case 2: return cmp < 0; case 3: return cmp <= 0; case 4: return cmp > 0; case 5: return cmp >= 0; }
    return false;
}

// multi-criteria evaluation: pairs (range, criteria) starting at index `from`; returns mask over the first grid
bool ifs_mask(Evaluator& ev, const Args& a, size_t from, std::vector<char>& mask, int& rows, int& cols, Opnd& err) {
    if ((a.size() - from) % 2 != 0 || a.size() <= from) { err = Opnd::err(Err::Value); return false; }
    std::vector<Grid> gs;
    std::vector<Crit> cs;
    for (size_t i = from; i < a.size(); i += 2) {
        if (!a[i].is_ref() && a[i].k != Opnd::A) { err = Opnd::err(Err::Value); return false; }
        gs.push_back(grid_of(ev, a[i]));
        Value cv = ev.scalar(a[i + 1]);
        if (a[i + 1].missing) cv = Value::number(0);
        cs.push_back(make_crit(cv));
    }
    rows = gs[0].rows; cols = gs[0].cols;
    for (auto& g : gs) {
        if (g.cols != cols) { err = Opnd::err(Err::Value); return false; }
        if (g.rows != rows) {
            bool whole = g.whole && gs[0].whole;
            if (!whole) { err = Opnd::err(Err::Value); return false; }
            rows = std::max(rows, g.rows);
        }
    }
    for (auto& g : gs) { g.rows = rows; }
    int real = 0;
    for (auto& g : gs) real = std::max(real, g.is_ref ? g.real_rows : g.rows);
    rows = std::min(rows, std::max(real, 0));
    mask.assign((size_t)rows * cols, 1);
    for (size_t k = 0; k < gs.size(); ++k)
        for (int i = 0; i < rows; ++i)
            for (int j = 0; j < cols; ++j) {
                char& m = mask[(size_t)i * cols + j];
                if (m && !crit_match(cs[k], gs[k].get(i, j))) m = 0;
            }
    return true;
}

// ------------------------------------------------------------ math & stat --
Opnd f_sum(Evaluator& ev, Args& a) { COLLECT(v, 0); double s = 0; for (double x : v) s += x; return num_result(s); }
Opnd f_product(Evaluator& ev, Args& a) { COLLECT(v, 0); if (v.empty()) return Opnd::num(0); double s = 1; for (double x : v) s *= x; return num_result(s); }
Opnd f_sumsq(Evaluator& ev, Args& a) { COLLECT(v, 0); double s = 0; for (double x : v) s += x * x; return num_result(s); }
Opnd f_average(Evaluator& ev, Args& a) { COLLECT(v, 0); if (v.empty()) return Opnd::err(Err::Div0); double s = 0; for (double x : v) s += x; return num_result(s / v.size()); }
Opnd f_averagea(Evaluator& ev, Args& a) { COLLECT(v, 0, true); if (v.empty()) return Opnd::err(Err::Div0); double s = 0; for (double x : v) s += x; return num_result(s / v.size()); }
Opnd f_min(Evaluator& ev, Args& a) { COLLECT(v, 0); if (v.empty()) return Opnd::num(0); return Opnd::num(*std::min_element(v.begin(), v.end())); }
Opnd f_max(Evaluator& ev, Args& a) { COLLECT(v, 0); if (v.empty()) return Opnd::num(0); return Opnd::num(*std::max_element(v.begin(), v.end())); }
Opnd f_mina(Evaluator& ev, Args& a) { COLLECT(v, 0, true); if (v.empty()) return Opnd::num(0); return Opnd::num(*std::min_element(v.begin(), v.end())); }
Opnd f_maxa(Evaluator& ev, Args& a) { COLLECT(v, 0, true); if (v.empty()) return Opnd::num(0); return Opnd::num(*std::max_element(v.begin(), v.end())); }

Opnd f_count(Evaluator& ev, Args& a) {
    double n = 0;
    for (auto& o : a) {
        if (o.missing) continue;
        if (o.k == Opnd::V) { if (o.v.is_num() || o.v.is_bool()) ++n; else if (o.v.is_str()) { double d; if (text_to_number(o.v.str, d)) ++n; } continue; }
        ev.each(o, [&](const Value& v, bool) { if (v.is_num()) ++n; });
    }
    return Opnd::num(n);
}
Opnd f_counta(Evaluator& ev, Args& a) {
    double n = 0;
    for (auto& o : a) {
        if (o.missing) { ++n; continue; }
        if (o.k == Opnd::V) { ++n; continue; }
        ev.each(o, [&](const Value& v, bool) { if (!v.is_empty()) ++n; });
    }
    return Opnd::num(n);
}
Opnd f_countblank(Evaluator& ev, Args& a) {
    if (!a[0].is_ref() && a[0].k != Opnd::A) return Opnd::err(Err::Value);
    Grid g = grid_of(ev, a[0]);
    double n = 0;
    if (g.is_ref) {
        for (int i = 0; i < g.rows; ++i) for (int j = 0; j < g.cols; ++j) { const Value& v = g.get(i, j); if (v.is_empty() || (v.is_str() && v.str.empty())) ++n; }
        // rows beyond real_rows are blank
    } else for (auto& v : g.arr->v) if (v.is_empty() || (v.is_str() && v.str.empty())) ++n;
    if (g.is_ref && a[0].ref.rg.r1 >= MAX_ROWS - 1) n += (double)(MAX_ROWS - g.rows) * g.cols;
    return Opnd::num(n);
}
Opnd f_countif(Evaluator& ev, Args& a) {
    std::vector<char> m; int R, C; Opnd e;
    if (!ifs_mask(ev, a, 0, m, R, C, e)) return e;
    double n = 0; for (char x : m) n += x;
    return Opnd::num(n);
}
Opnd f_countifs(Evaluator& ev, Args& a) { return f_countif(ev, a); }

Opnd agg_if(Evaluator& ev, Args& a, int kind, bool single) {
    // kind: 0 sum, 1 average, 2 min, 3 max
    std::vector<char> m; int R, C; Opnd e;
    Grid target;
    if (single) {
        // (range, criteria, [sum_range])
        Args crit{a[0], a[1]};
        if (!ifs_mask(ev, crit, 0, m, R, C, e)) return e;
        const Opnd& tr = missing(a, 2) ? a[0] : a[2];
        if (!tr.is_ref() && tr.k != Opnd::A) return Opnd::err(Err::Value);
        target = grid_of(ev, tr);
        if (tr.k == Opnd::R && !missing(a, 2)) {   // sum_range is resized to the criteria range shape
            target.rg.r1 = target.rg.r0 + std::max(R, 1) - 1; target.rg.c1 = target.rg.c0 + C - 1;
            target.rows = R; target.cols = C;
            target.real_rows = std::min(target.real_rows, R);
            if (target.real_rows < R) { Range cl = ev.clip_used(target.si, target.rg); target.real_rows = std::max(0, cl.r1 - target.rg.r0 + 1); }
        }
    } else {
        if (!ifs_mask(ev, a, 1, m, R, C, e)) return e;
        if (!a[0].is_ref() && a[0].k != Opnd::A) return Opnd::err(Err::Value);
        target = grid_of(ev, a[0]);
        if (target.cols != C || (target.rows != R && !(target.is_ref && target.rg.r0 == 0))) {
            if (!(target.is_ref && target.real_rows <= R && target.cols == C)) return Opnd::err(Err::Value);
        }
    }
    double s = 0, n = 0, mn = HUGE_VAL, mx = -HUGE_VAL;
    for (int i = 0; i < R; ++i)
        for (int j = 0; j < C; ++j) {
            if (!m[(size_t)i * C + j]) continue;
            const Value& v = target.get(i, j);
            if (v.is_err()) return Opnd::val(v);
            if (!v.is_num()) continue;
            s += v.num; ++n; mn = std::min(mn, v.num); mx = std::max(mx, v.num);
        }
    switch (kind) {
    case 0: return num_result(s);
    case 1: return n ? num_result(s / n) : Opnd::err(Err::Div0);
    case 2: return Opnd::num(n ? mn : 0);
    default: return Opnd::num(n ? mx : 0);
    }
}
Opnd f_sumif(Evaluator& ev, Args& a) { return agg_if(ev, a, 0, true); }
Opnd f_sumifs(Evaluator& ev, Args& a) { return agg_if(ev, a, 0, false); }
Opnd f_averageif(Evaluator& ev, Args& a) { return agg_if(ev, a, 1, true); }
Opnd f_averageifs(Evaluator& ev, Args& a) { return agg_if(ev, a, 1, false); }
Opnd f_minifs(Evaluator& ev, Args& a) { return agg_if(ev, a, 2, false); }
Opnd f_maxifs(Evaluator& ev, Args& a) { return agg_if(ev, a, 3, false); }

Opnd f_sumproduct(Evaluator& ev, Args& a) {
    std::vector<Grid> gs;
    for (auto& o : a) { if (o.missing) return Opnd::err(Err::Value); gs.push_back(grid_of(ev, o)); }
    int R = gs[0].rows, C = gs[0].cols;
    for (auto& g : gs) if (g.rows != R || g.cols != C) return Opnd::err(Err::Value);
    int real = R;
    bool all_ref = true;
    for (auto& g : gs) if (!g.is_ref) all_ref = false;
    if (all_ref) { real = 0; for (auto& g : gs) real = std::max(real, g.real_rows); }
    double s = 0;
    for (int i = 0; i < real; ++i)
        for (int j = 0; j < C; ++j) {
            double p = 1;
            for (auto& g : gs) {
                const Value& v = g.get(i, j);
                if (v.is_err()) return Opnd::val(v);
                p *= v.is_num() ? v.num : 0;
            }
            s += p;
        }
    return num_result(s);
}

Opnd f_round(Evaluator& ev, Args& a) {
    NUM(x, 0) NUM(d, 1)
    int n = (int)std::trunc(d);
    if (n > 15) return Opnd::num(x);
    char buf[64];
    if (n >= 0) {
        // decimal-string rounding avoids 2.675 -> 2.67 binary artefacts
        snprintf(buf, sizeof buf, "%.15g", x);
        double xr = atof(buf);
        double m = std::pow(10.0, n);
        double v = std::round(std::fabs(xr) * m * (1 + 1e-15)) / m;
        return Opnd::num(xr < 0 ? -v : v);
    }
    double m = std::pow(10.0, -n);
    double v = std::round(std::fabs(x) / m) * m;
    return Opnd::num(x < 0 ? -v : v);
}
Opnd f_roundup(Evaluator& ev, Args& a) {
    NUM(x, 0) NUM(d, 1)
    int n = (int)std::trunc(d);
    double m = std::pow(10.0, n);
    char buf[64]; snprintf(buf, sizeof buf, "%.15g", std::fabs(x) * m);
    double y = std::ceil(atof(buf));
    return Opnd::num((x < 0 ? -y : y) / m);
}
Opnd f_rounddown(Evaluator& ev, Args& a) {
    NUM(x, 0) NUM(d, 1)
    int n = (int)std::trunc(d);
    double m = std::pow(10.0, n);
    char buf[64]; snprintf(buf, sizeof buf, "%.15g", std::fabs(x) * m);
    double y = std::floor(atof(buf));
    return Opnd::num((x < 0 ? -y : y) / m);
}
Opnd f_trunc(Evaluator& ev, Args& a) { if (a.size() < 2) { a.push_back(Opnd::num(0)); } return f_rounddown(ev, a); }
Opnd f_int(Evaluator& ev, Args& a) { NUM(x, 0) return Opnd::num(std::floor(x)); }
Opnd f_mod(Evaluator& ev, Args& a) {
    NUM(x, 0) NUM(y, 1)
    if (y == 0) return Opnd::err(Err::Div0);
    double r = x - y * std::floor(x / y);
    if (std::fabs(r) < 1e-15 * std::fabs(y)) r = 0;
    return Opnd::num(r);
}
Opnd f_abs(Evaluator& ev, Args& a) { NUM(x, 0) return Opnd::num(std::fabs(x)); }
Opnd f_sign(Evaluator& ev, Args& a) { NUM(x, 0) return Opnd::num((x > 0) - (x < 0)); }
Opnd f_sqrt(Evaluator& ev, Args& a) { NUM(x, 0) if (x < 0) return Opnd::err(Err::Num); return Opnd::num(std::sqrt(x)); }
Opnd f_power(Evaluator& ev, Args& a) { return apply_binary(ev, Tok::Pow, a[0], a[1]); }
Opnd f_exp(Evaluator& ev, Args& a) { NUM(x, 0) return num_result(std::exp(x)); }
Opnd f_ln(Evaluator& ev, Args& a) { NUM(x, 0) if (x <= 0) return Opnd::err(Err::Num); return Opnd::num(std::log(x)); }
Opnd f_log10(Evaluator& ev, Args& a) { NUM(x, 0) if (x <= 0) return Opnd::err(Err::Num); return Opnd::num(std::log10(x)); }
Opnd f_log(Evaluator& ev, Args& a) {
    NUM(x, 0) NUM_OR(b, 1, 10)
    if (x <= 0 || b <= 0) return Opnd::err(Err::Num);
    if (b == 1) return Opnd::err(Err::Div0);
    return Opnd::num(std::log(x) / std::log(b));
}
Opnd f_pi(Evaluator&, Args&) { return Opnd::num(M_PI_); }
std::mt19937_64& rng() { static std::mt19937_64 g((uint64_t)std::chrono::steady_clock::now().time_since_epoch().count()); return g; }
Opnd f_rand(Evaluator&, Args&) { return Opnd::num(std::uniform_real_distribution<double>(0, 1)(rng())); }
Opnd f_randbetween(Evaluator& ev, Args& a) {
    NUM(lo, 0) NUM(hi, 1)
    double l = std::ceil(lo), h = std::floor(hi);
    if (h < l) return Opnd::err(Err::Num);
    return Opnd::num(l + std::floor(std::uniform_real_distribution<double>(0, 1)(rng()) * (h - l + 1)));
}
Opnd f_fact(Evaluator& ev, Args& a) {
    NUM(x, 0)
    if (x < 0) return Opnd::err(Err::Num);
    double r = 1; for (int i = 2; i <= (int)std::floor(x); ++i) r *= i;
    return num_result(r);
}
Opnd f_combin(Evaluator& ev, Args& a) {
    NUM(n0, 0) NUM(k0, 1)
    double n = std::floor(n0), k = std::floor(k0);
    if (n < 0 || k < 0 || k > n) return Opnd::err(Err::Num);
    double r = 1; for (int i = 1; i <= (int)k; ++i) r = r * (n - k + i) / i;
    return num_result(std::round(r));
}
Opnd f_permut(Evaluator& ev, Args& a) {
    NUM(n0, 0) NUM(k0, 1)
    double n = std::floor(n0), k = std::floor(k0);
    if (n < 0 || k < 0 || k > n) return Opnd::err(Err::Num);
    double r = 1; for (int i = 0; i < (int)k; ++i) r *= (n - i);
    return num_result(r);
}
Opnd f_gcd(Evaluator& ev, Args& a) {
    COLLECT(v, 0);
    unsigned long long g = 0;
    for (double x : v) { if (x < 0) return Opnd::err(Err::Num); g = std::gcd(g, (unsigned long long)std::floor(x)); }
    return Opnd::num((double)g);
}
Opnd f_lcm(Evaluator& ev, Args& a) {
    COLLECT(v, 0);
    unsigned long long l = 1;
    for (double x : v) { if (x < 0) return Opnd::err(Err::Num); unsigned long long y = (unsigned long long)std::floor(x); if (y == 0) return Opnd::num(0); l = l / std::gcd(l, y) * y; }
    return Opnd::num((double)l);
}
Opnd f_even(Evaluator& ev, Args& a) { NUM(x, 0) double y = std::ceil(std::fabs(x) / 2) * 2; return Opnd::num(x < 0 ? -y : y); }
Opnd f_odd(Evaluator& ev, Args& a) { NUM(x, 0) double y = std::ceil(std::fabs(x)); if (std::fmod(y, 2) == 0) y += 1; return Opnd::num(x < 0 ? -y : y); }
Opnd f_quotient(Evaluator& ev, Args& a) { NUM(x, 0) NUM(y, 1) if (y == 0) return Opnd::err(Err::Div0); return Opnd::num(std::trunc(x / y)); }
Opnd f_mround(Evaluator& ev, Args& a) {
    NUM(x, 0) NUM(m, 1)
    if (m == 0) return Opnd::num(0);
    if ((x > 0 && m < 0) || (x < 0 && m > 0)) return Opnd::err(Err::Num);
    return Opnd::num(std::round(x / m + 1e-12 * (x / m >= 0 ? 1 : -1)) * m);
}
Opnd f_ceiling(Evaluator& ev, Args& a) {
    NUM(x, 0) NUM_OR(s, 1, x >= 0 ? 1 : -1)
    if (s == 0) return Opnd::num(0);
    if (x > 0 && s < 0) return Opnd::err(Err::Num);
    double q = x / s;
    return Opnd::num(std::ceil(q - 1e-12) * s);
}
Opnd f_floor(Evaluator& ev, Args& a) {
    NUM(x, 0) NUM_OR(s, 1, x >= 0 ? 1 : -1)
    if (s == 0) return x == 0 ? Opnd::num(0) : Opnd::err(Err::Div0);
    if (x > 0 && s < 0) return Opnd::err(Err::Num);
    double q = x / s;
    return Opnd::num(std::floor(q + 1e-12) * s);
}
Opnd f_ceiling_math(Evaluator& ev, Args& a) {
    NUM(x, 0) NUM_OR(s, 1, 1) NUM_OR(mode, 2, 0)
    s = std::fabs(s);
    if (s == 0) return Opnd::num(0);
    double q = x / s;
    if (x < 0 && mode != 0) return Opnd::num(std::floor(q + 1e-12) * s);
    return Opnd::num(std::ceil(q - 1e-12) * s);
}
Opnd f_floor_math(Evaluator& ev, Args& a) {
    NUM(x, 0) NUM_OR(s, 1, 1) NUM_OR(mode, 2, 0)
    s = std::fabs(s);
    if (s == 0) return Opnd::num(0);
    double q = x / s;
    if (x < 0 && mode != 0) return Opnd::num(std::ceil(q - 1e-12) * s);
    return Opnd::num(std::floor(q + 1e-12) * s);
}
#define TRIG(name, expr) Opnd name(Evaluator& ev, Args& a) { NUM(x, 0) double r = expr; return num_result(r); }
TRIG(f_sin, std::sin(x)) TRIG(f_cos, std::cos(x)) TRIG(f_tan, std::tan(x)) TRIG(f_atan, std::atan(x))
TRIG(f_sinh, std::sinh(x)) TRIG(f_cosh, std::cosh(x)) TRIG(f_tanh, std::tanh(x))
TRIG(f_degrees, x * 180 / M_PI_) TRIG(f_radians, x * M_PI_ / 180)
Opnd f_asin(Evaluator& ev, Args& a) { NUM(x, 0) if (x < -1 || x > 1) return Opnd::err(Err::Num); return Opnd::num(std::asin(x)); }
Opnd f_acos(Evaluator& ev, Args& a) { NUM(x, 0) if (x < -1 || x > 1) return Opnd::err(Err::Num); return Opnd::num(std::acos(x)); }
Opnd f_atan2(Evaluator& ev, Args& a) { NUM(x, 0) NUM(y, 1) if (x == 0 && y == 0) return Opnd::err(Err::Div0); return Opnd::num(std::atan2(y, x)); }

Opnd f_median(Evaluator& ev, Args& a) {
    COLLECT(v, 0);
    if (v.empty()) return Opnd::err(Err::Num);
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return Opnd::num(n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2);
}
Opnd f_mode(Evaluator& ev, Args& a) {
    COLLECT(v, 0);
    std::map<double, int> cnt; std::vector<double> order;
    for (double x : v) { if (!cnt.count(x)) order.push_back(x); ++cnt[x]; }
    int best = 1; double bv = 0; bool found = false;
    for (double x : order) if (cnt[x] > best) { best = cnt[x]; bv = x; found = true; }
    if (!found) return Opnd::err(Err::NA);
    return Opnd::num(bv);
}
Opnd large_small(Evaluator& ev, Args& a, bool large) {
    std::vector<double> v; Opnd e;
    Args one{a[0]};
    if (!collect(ev, one, 0, v, e)) return e;
    NUM(k0, 1)
    long k = (long)std::ceil(k0 - 1e-12);
    if (v.empty() || k < 1 || k > (long)v.size()) return Opnd::err(Err::Num);
    std::sort(v.begin(), v.end());
    return Opnd::num(large ? v[v.size() - k] : v[k - 1]);
}
Opnd f_large(Evaluator& ev, Args& a) { return large_small(ev, a, true); }
Opnd f_small(Evaluator& ev, Args& a) { return large_small(ev, a, false); }
Opnd f_rank(Evaluator& ev, Args& a) {
    NUM(x, 0)
    std::vector<double> v; Opnd e;
    Args one{a[1]};
    if (!a[1].is_ref() && a[1].k != Opnd::A) return Opnd::err(Err::Value);
    if (!collect(ev, one, 0, v, e)) return e;
    NUM_OR(ord, 2, 0)
    bool found = false;
    double r = 1;
    for (double y : v) { if (y == x) found = true; if (ord == 0 ? y > x : y < x) ++r; }
    if (!found) return Opnd::err(Err::NA);
    return Opnd::num(r);
}
Opnd var_impl(Evaluator& ev, Args& a, bool sample, bool sd, bool mode_a = false) {
    COLLECT(v, 0, mode_a);
    size_t n = v.size();
    if (n < (sample ? 2u : 1u)) return Opnd::err(Err::Div0);
    double m = 0; for (double x : v) m += x; m /= n;
    double s = 0; for (double x : v) s += (x - m) * (x - m);
    s /= sample ? n - 1 : n;
    return Opnd::num(sd ? std::sqrt(s) : s);
}
Opnd f_stdev(Evaluator& ev, Args& a) { return var_impl(ev, a, true, true); }
Opnd f_stdevp(Evaluator& ev, Args& a) { return var_impl(ev, a, false, true); }
Opnd f_var(Evaluator& ev, Args& a) { return var_impl(ev, a, true, false); }
Opnd f_varp(Evaluator& ev, Args& a) { return var_impl(ev, a, false, false); }
Opnd percentile(std::vector<double> v, double p, bool exc) {
    if (v.empty()) return Opnd::err(Err::Num);
    std::sort(v.begin(), v.end());
    double n = (double)v.size();
    double h;
    if (exc) { if (p <= 0 || p >= 1) return Opnd::err(Err::Num); h = p * (n + 1) - 1; if (h < 0 || h > n - 1) return Opnd::err(Err::Num); }
    else { if (p < 0 || p > 1) return Opnd::err(Err::Num); h = p * (n - 1); }
    size_t lo = (size_t)std::floor(h);
    double fr = h - lo;
    if (lo + 1 >= v.size()) return Opnd::num(v.back());
    return Opnd::num(v[lo] + fr * (v[lo + 1] - v[lo]));
}
Opnd f_percentile(Evaluator& ev, Args& a) { std::vector<double> v; Opnd e; Args one{a[0]}; if (!collect(ev, one, 0, v, e)) return e; NUM(p, 1) return percentile(v, p, false); }
Opnd f_percentile_exc(Evaluator& ev, Args& a) { std::vector<double> v; Opnd e; Args one{a[0]}; if (!collect(ev, one, 0, v, e)) return e; NUM(p, 1) return percentile(v, p, true); }
Opnd f_quartile(Evaluator& ev, Args& a) {
    std::vector<double> v; Opnd e; Args one{a[0]};
    if (!collect(ev, one, 0, v, e)) return e;
    NUM(q, 1)
    int qi = (int)std::floor(q);
    if (qi < 0 || qi > 4) return Opnd::err(Err::Num);
    return percentile(v, qi / 4.0, false);
}
Opnd f_subtotal(Evaluator& ev, Args& a) {
    NUM(fn0, 0)
    int fn = (int)fn0;
    bool skip_hidden = fn > 100;
    int base = fn % 100;
    if (base < 1 || base > 11 || (fn > 11 && fn < 101) || fn > 111) return Opnd::err(Err::Value);
    for (size_t i = 1; i < a.size(); ++i) if (!a[i].is_ref()) return Opnd::err(Err::Value);
    std::vector<double> v; Opnd e;
    if (base == 3) {   // COUNTA
        double n = 0;
        for (size_t i = 1; i < a.size(); ++i) {
            const Area& ar = a[i].ref;
            Range rg = ev.clip_used(ar.si, ar.rg);
            const Worksheet& ws = ev.book.sheets[ar.si];
            for (int r = rg.r0; r <= rg.r1; ++r) {
                if (skip_hidden ? ws.row_hidden(r) : (r < (int)ws.rows.size() && ws.rows[r].filtered)) continue;
                for (int c = rg.c0; c <= rg.c1; ++c) if (!ev.cell_value(ar.si, r, c).is_empty()) ++n;
            }
        }
        return Opnd::num(n);
    }
    if (!collect(ev, a, 1, v, e, false, skip_hidden, true)) return e;
    double s = 0; for (double x : v) s += x;
    size_t n = v.size();
    switch (base) {
    case 1: return n ? Opnd::num(s / n) : Opnd::err(Err::Div0);
    case 2: return Opnd::num((double)n);
    case 4: return Opnd::num(n ? *std::max_element(v.begin(), v.end()) : 0);
    case 5: return Opnd::num(n ? *std::min_element(v.begin(), v.end()) : 0);
    case 6: { double p = 1; for (double x : v) p *= x; return Opnd::num(n ? p : 0); }
    case 9: return num_result(s);
    default: {
        bool sample = base == 7 || base == 10;
        if (n < (sample ? 2u : 1u)) return Opnd::err(Err::Div0);
        double m = s / n, q = 0; for (double x : v) q += (x - m) * (x - m);
        q /= sample ? n - 1 : n;
        return Opnd::num(base == 7 || base == 8 ? std::sqrt(q) : q);
    }
    }
}
Opnd f_sequence(Evaluator& ev, Args& a) {
    NUM(r0, 0) NUM_OR(c0, 1, 1) NUM_OR(st, 2, 1) NUM_OR(step, 3, 1)
    long R = (long)r0, C = (long)c0;
    if (R < 1 || C < 1) return Opnd::err(Err::Calc);
    if ((double)R * C > 1048576.0) return Opnd::err(Err::Num);
    auto arr = std::make_shared<Array>((int)R, (int)C);
    double v = st;
    for (int i = 0; i < R; ++i) for (int j = 0; j < C; ++j) { arr->at(i, j) = Value::number(v); v += step; }
    return Opnd::array(arr);
}

// ---------------------------------------------------------------- logical --
Opnd f_if(Evaluator& ev, Args& a) {
    Value c = ev.scalar(a[0]);
    if (c.is_err()) return Opnd::val(c);
    bool b; Err e = Err::Value;
    if (!to_bool(c, b, e)) { double d; if (c.is_str() && text_to_number(c.str, d)) b = d != 0; else return Opnd::err(e); }
    if (b) { if (a.size() < 2) return Opnd::boolean(true); if (a[1].missing) return Opnd::num(0); return a[1]; }
    if (a.size() < 3) return Opnd::boolean(false);
    if (a[2].missing) return Opnd::num(0);
    return a[2];
}
Opnd f_ifs(Evaluator& ev, Args& a) {
    if (a.size() % 2) return Opnd::err(Err::Value);
    for (size_t i = 0; i < a.size(); i += 2) {
        Value c = ev.scalar(a[i]);
        if (c.is_err()) return Opnd::val(c);
        bool b; Err e = Err::Value;
        if (!to_bool(c, b, e)) return Opnd::err(e);
        if (b) return a[i + 1].missing ? Opnd::num(0) : a[i + 1];
    }
    return Opnd::err(Err::NA);
}
Opnd f_iferror(Evaluator& ev, Args& a) {
    Value v = ev.scalar(a[0]);
    if (v.is_err()) return a[1].missing ? Opnd::num(0) : a[1];
    return a[0];
}
Opnd f_ifna(Evaluator& ev, Args& a) {
    Value v = ev.scalar(a[0]);
    if (v.is_err() && v.err == Err::NA) return a[1].missing ? Opnd::num(0) : a[1];
    return a[0];
}
Opnd logic(Evaluator& ev, Args& a, int kind) {   // 0 and, 1 or, 2 xor
    int n = 0, t = 0;
    for (auto& o : a) {
        if (o.missing) continue;
        if (o.k == Opnd::V) {
            const Value& v = o.v;
            if (v.is_err()) return Opnd::val(v);
            if (v.is_str()) { std::string u = upper_ascii(v.str); if (u == "TRUE") { ++n; ++t; continue; } if (u == "FALSE") { ++n; continue; } return Opnd::err(Err::Value); }
            if (v.is_empty()) continue;
            ++n; if (v.num != 0) ++t;
            continue;
        }
        Value bad; bool isbad = false;
        ev.each(o, [&](const Value& v, bool) {
            if (isbad) return;
            if (v.is_err()) { bad = v; isbad = true; return; }
            if (v.is_num() || v.is_bool()) { ++n; if (v.num != 0) ++t; }
        });
        if (isbad) return Opnd::val(bad);
    }
    if (!n) return Opnd::err(Err::Value);
    if (kind == 0) return Opnd::boolean(t == n);
    if (kind == 1) return Opnd::boolean(t > 0);
    return Opnd::boolean(t % 2 == 1);
}
Opnd f_and(Evaluator& ev, Args& a) { return logic(ev, a, 0); }
Opnd f_or(Evaluator& ev, Args& a) { return logic(ev, a, 1); }
Opnd f_xor(Evaluator& ev, Args& a) { return logic(ev, a, 2); }
Opnd f_not(Evaluator& ev, Args& a) {
    Value v = ev.scalar(a[0]);
    bool b; Err e = Err::Value;
    if (!to_bool(v, b, e)) { double d; if (v.is_str() && text_to_number(v.str, d)) b = d != 0; else return Opnd::err(e); }
    return Opnd::boolean(!b);
}
Opnd f_true(Evaluator&, Args&) { return Opnd::boolean(true); }
Opnd f_false(Evaluator&, Args&) { return Opnd::boolean(false); }
Opnd f_switch(Evaluator& ev, Args& a) {
    Value x = ev.scalar(a[0]);
    if (x.is_err()) return Opnd::val(x);
    size_t i = 1;
    for (; i + 1 < a.size(); i += 2) {
        Value c = ev.scalar(a[i]);
        if (c.is_err()) return Opnd::val(c);
        if (compare_values(x, c) == 0 && x.type == c.type) return a[i + 1];
    }
    if (i < a.size()) return a[i];
    return Opnd::err(Err::NA);
}

// ----------------------------------------------------------------- lookup --
// exact match position (0-based) in a vector, honouring wildcards for text
bool values_equal_lookup(const Value& key, const Value& x, bool wild) {
    if (key.is_str() && x.is_str()) return wild ? wildcard_match(key.str, x.str) : ci_equal(key.str, x.str);
    if (key.type != x.type) {
        if (key.is_num() && x.is_empty()) return false;
        return false;
    }
    return compare_values(key, x) == 0;
}
// approximate (sorted ascending): last index with x <= key of the same type class
long approx_asc(const std::vector<const Value*>& v, const Value& key) {
    long lo = 0, hi = (long)v.size() - 1, ans = -1;
    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        // skip values of different type by scanning down
        long m = mid;
        while (m >= lo && (v[m]->is_empty() || (v[m]->is_num() != key.is_num()) || (v[m]->is_str() != key.is_str()))) --m;
        if (m < lo) { lo = mid + 1; continue; }
        int c = compare_values(*v[m], key);
        if (c <= 0) { ans = m; lo = mid + 1; }
        else hi = m - 1;
    }
    return ans;
}
long approx_desc(const std::vector<const Value*>& v, const Value& key) {
    // sorted descending: first... Excel MATCH -1: smallest value >= key
    long ans = -1;
    for (long i = 0; i < (long)v.size(); ++i) {
        if (v[i]->type != key.type) continue;
        if (compare_values(*v[i], key) >= 0) ans = i; else break;
    }
    return ans;
}
struct Vec {   // 1-D slice of a grid (row or column)
    std::vector<Value> store;
    std::vector<const Value*> p;
};
Vec slice(const Grid& g, bool col, int idx) {
    Vec v;
    int n = col ? g.rows : g.cols;
    if (g.is_ref) n = col ? std::min(g.rows, std::max(g.real_rows, 0)) : g.cols;
    v.store.reserve(n);
    for (int i = 0; i < n; ++i) v.store.push_back(col ? g.get(i, idx) : g.get(idx, i));
    for (auto& x : v.store) v.p.push_back(&x);
    return v;
}
Opnd lookup_vh(Evaluator& ev, Args& a, bool vertical) {
    Value key = ev.scalar(a[0]);
    if (key.is_err()) return Opnd::val(key);
    if (a[1].k == Opnd::V || a[1].k == Opnd::RL) return Opnd::err(Err::NA);
    Grid g = grid_of(ev, a[1]);
    NUM(idx0, 2)
    long idx = (long)std::floor(idx0);
    BOOL_OR(approx, 3, true)
    if (idx < 1) return Opnd::err(Err::Value);
    if (idx > (vertical ? g.cols : g.rows)) return Opnd::err(Err::Ref);
    Vec keys = slice(g, vertical, 0);
    long pos = -1;
    if (!approx) {
        bool wild = key.is_str() && key.str.find_first_of("*?~") != std::string::npos;
        for (long i = 0; i < (long)keys.p.size(); ++i) if (values_equal_lookup(key, *keys.p[i], wild)) { pos = i; break; }
    } else pos = approx_asc(keys.p, key);
    if (pos < 0) return Opnd::err(Err::NA);
    if (g.is_ref) {
        int r = vertical ? g.rg.r0 + (int)pos : g.rg.r0 + (int)idx - 1;
        int c = vertical ? g.rg.c0 + (int)idx - 1 : g.rg.c0 + (int)pos;
        return Opnd::area(g.si, Range(r, c, r, c));
    }
    return Opnd::val(vertical ? g.get((int)pos, (int)idx - 1) : g.get((int)idx - 1, (int)pos));
}
Opnd f_vlookup(Evaluator& ev, Args& a) { return lookup_vh(ev, a, true); }
Opnd f_hlookup(Evaluator& ev, Args& a) { return lookup_vh(ev, a, false); }

Opnd f_match(Evaluator& ev, Args& a) {
    Value key = ev.scalar(a[0]);
    if (key.is_err()) return Opnd::val(key);
    if (a[1].k == Opnd::RL) return Opnd::err(Err::NA);
    Grid g = grid_of(ev, a[1]);
    if (g.rows > 1 && g.cols > 1) return Opnd::err(Err::NA);
    NUM_OR(mt0, 2, 1)
    int mt = mt0 > 0 ? 1 : mt0 < 0 ? -1 : 0;
    Vec v = slice(g, g.cols == 1, 0);
    if (g.cols != 1) v = slice(g, false, 0);
    long pos = -1;
    if (mt == 0) {
        bool wild = key.is_str() && key.str.find_first_of("*?~") != std::string::npos;
        for (long i = 0; i < (long)v.p.size(); ++i) if (values_equal_lookup(key, *v.p[i], wild)) { pos = i; break; }
    } else if (mt == 1) pos = approx_asc(v.p, key);
    else pos = approx_desc(v.p, key);
    if (pos < 0) return Opnd::err(Err::NA);
    return Opnd::num((double)pos + 1);
}

// XMATCH/XLOOKUP core: returns index or -1
long xfind(const std::vector<const Value*>& v, const Value& key, int match_mode, int search_mode) {
    long n = (long)v.size();
    bool wild = match_mode == 2;
    if (search_mode == 2 || search_mode == -2) {   // binary search
        bool asc = search_mode == 2;
        long lo = 0, hi = n - 1, exact = -1, below = -1, above = -1;
        while (lo <= hi) {
            long mid = (lo + hi) / 2;
            int c = compare_values(*v[mid], key);
            if (!asc) c = -c;
            if (c == 0) { exact = mid; break; }
            if (c < 0) { (asc ? below : above) = mid; lo = mid + 1; }
            else { (asc ? above : below) = mid; hi = mid - 1; }
        }
        if (exact >= 0) return exact;
        if (match_mode == -1) return below;
        if (match_mode == 1) return above;
        return -1;
    }
    long best = -1;
    for (long k = 0; k < n; ++k) {
        long i = search_mode == -1 ? n - 1 - k : k;
        const Value& x = *v[i];
        if (match_mode == 0 || match_mode == 2) {
            if (values_equal_lookup(key, x, wild && key.is_str())) return i;
            continue;
        }
        if (x.type != key.type) continue;
        int c = compare_values(x, key);
        if (c == 0) return i;
        if (match_mode == -1 && c < 0 && (best < 0 || compare_values(x, *v[best]) > 0)) best = i;
        if (match_mode == 1 && c > 0 && (best < 0 || compare_values(x, *v[best]) < 0)) best = i;
    }
    return best;
}
Opnd f_xmatch(Evaluator& ev, Args& a) {
    Value key = ev.scalar(a[0]);
    if (key.is_err()) return Opnd::val(key);
    Grid g = grid_of(ev, a[1]);
    if (g.rows > 1 && g.cols > 1) return Opnd::err(Err::Value);
    NUM_OR(mm, 2, 0) NUM_OR(sm, 3, 1)
    Vec v = slice(g, g.cols == 1 && g.rows >= 1, 0);
    if (g.rows == 1) v = slice(g, false, 0);
    long pos = xfind(v.p, key, (int)mm, (int)sm);
    if (pos < 0) return Opnd::err(Err::NA);
    return Opnd::num((double)pos + 1);
}
Opnd f_xlookup(Evaluator& ev, Args& a) {
    Value key = ev.scalar(a[0]);
    if (key.is_err()) return Opnd::val(key);
    Grid lg = grid_of(ev, a[1]);
    if (lg.rows > 1 && lg.cols > 1) return Opnd::err(Err::Value);
    NUM_OR(mm, 4, 0) NUM_OR(sm, 5, 1)
    bool vertical = lg.cols == 1 && !(lg.rows == 1 && lg.cols == 1 && false);
    if (lg.rows == 1 && lg.cols > 1) vertical = false;
    Vec v = slice(lg, vertical, 0);
    long pos = xfind(v.p, key, (int)mm, (int)sm);
    if (pos < 0) {
        if (!missing(a, 3)) return a[3];
        return Opnd::err(Err::NA);
    }
    const Opnd& ret = a[2];
    if (ret.k == Opnd::R) {
        Range g = ret.ref.rg;
        if (vertical) { if (g.r1 - g.r0 + 1 < (long)v.p.size() && g.r1 < MAX_ROWS - 1 && lg.rows != g.rows()) return Opnd::err(Err::Value); return Opnd::area(ret.ref.si, Range(g.r0 + (int)pos, g.c0, g.r0 + (int)pos, g.c1)); }
        return Opnd::area(ret.ref.si, Range(g.r0, g.c0 + (int)pos, g.r1, g.c0 + (int)pos));
    }
    Grid rg = grid_of(ev, ret);
    if (vertical) {
        if (pos >= rg.rows) return Opnd::err(Err::Value);
        auto arr = std::make_shared<Array>(1, rg.cols);
        for (int j = 0; j < rg.cols; ++j) arr->at(0, j) = rg.get((int)pos, j);
        return Opnd::array(arr);
    }
    if (pos >= rg.cols) return Opnd::err(Err::Value);
    auto arr = std::make_shared<Array>(rg.rows, 1);
    for (int i = 0; i < rg.rows; ++i) arr->at(i, 0) = rg.get(i, (int)pos);
    return Opnd::array(arr);
}
Opnd f_lookup(Evaluator& ev, Args& a) {
    Value key = ev.scalar(a[0]);
    if (key.is_err()) return Opnd::val(key);
    Grid g = grid_of(ev, a[1]);
    bool vertical = g.rows >= g.cols;
    Vec v = slice(g, vertical, 0);
    long pos = approx_asc(v.p, key);
    if (pos < 0) return Opnd::err(Err::NA);
    if (!missing(a, 2)) {
        Grid r = grid_of(ev, a[2]);
        bool rv = r.rows >= r.cols;
        if (r.is_ref) { int rr = r.rg.r0 + (rv ? (int)pos : 0), cc = r.rg.c0 + (rv ? 0 : (int)pos); return Opnd::area(r.si, Range(rr, cc, rr, cc)); }
        return Opnd::val(rv ? r.get((int)pos, 0) : r.get(0, (int)pos));
    }
    return Opnd::val(vertical ? g.get((int)pos, g.cols - 1) : g.get(g.rows - 1, (int)pos));
}
Opnd f_index(Evaluator& ev, Args& a) {
    NUM_OR(r0, 1, 0) NUM_OR(c0, 2, 0) NUM_OR(ar, 3, 1)
    long r = (long)r0, c = (long)c0;
    const Opnd* src = &a[0];
    Opnd pick;
    if (a[0].k == Opnd::RL) {
        long k = (long)ar;
        if (k < 1 || k > (long)a[0].refs.size()) return Opnd::err(Err::Ref);
        pick = Opnd::area(a[0].refs[k - 1].si, a[0].refs[k - 1].rg);
        src = &pick;
    }
    if (r < 0 || c < 0) return Opnd::err(Err::Value);
    if (src->k == Opnd::R) {
        Range g = src->ref.rg;
        int R = g.rows(), C = g.cols();
        if (missing(a, 2) && R == 1 && a.size() >= 2) { c = r; r = 1; }   // INDEX(row_vector, n)
        if (r > R || c > C) return Opnd::err(Err::Ref);
        int rr0 = r ? g.r0 + (int)r - 1 : g.r0, rr1 = r ? rr0 : g.r1;
        int cc0 = c ? g.c0 + (int)c - 1 : g.c0, cc1 = c ? cc0 : g.c1;
        if (!c && C == 1) { cc0 = cc1 = g.c0; }
        return Opnd::area(src->ref.si, Range(rr0, cc0, rr1, cc1));
    }
    auto A = ev.to_array(*src);
    if (missing(a, 2) && A->rows == 1 && a.size() >= 2) { c = r; r = 1; }
    if (r > A->rows || c > A->cols) return Opnd::err(Err::Ref);
    if (r && c) return Opnd::val(A->at((int)r - 1, (int)c - 1));
    if (r && A->cols == 1) return Opnd::val(A->at((int)r - 1, 0));
    if (r) { auto o = std::make_shared<Array>(1, A->cols); for (int j = 0; j < A->cols; ++j) o->at(0, j) = A->at((int)r - 1, j); return Opnd::array(o); }
    if (c) { auto o = std::make_shared<Array>(A->rows, 1); for (int i = 0; i < A->rows; ++i) o->at(i, 0) = A->at(i, (int)c - 1); return Opnd::array(o); }
    return Opnd::array(A);
}
Opnd f_choose(Evaluator& ev, Args& a) {
    NUM(k0, 0)
    long k = (long)k0;
    if (k < 1 || k >= (long)a.size()) return Opnd::err(Err::Value);
    if (a[k].missing) return Opnd::num(0);
    return a[k];
}
Opnd f_offset(Evaluator& ev, Args& a) {
    if (a[0].k != Opnd::R) return Opnd::err(Err::Value);
    NUM(dr, 1) NUM(dc, 2)
    Range g = a[0].ref.rg;
    NUM_OR(h, 3, g.rows()) NUM_OR(w, 4, g.cols())
    long H = (long)h, W = (long)w;
    if (H == 0 || W == 0) return Opnd::err(Err::Ref);
    long r0 = g.r0 + (long)dr, c0 = g.c0 + (long)dc;
    long r1 = r0 + (H > 0 ? H - 1 : H + 1), c1 = c0 + (W > 0 ? W - 1 : W + 1);
    if (std::min(r0, r1) < 0 || std::min(c0, c1) < 0 || std::max(r0, r1) >= MAX_ROWS || std::max(c0, c1) >= MAX_COLS) return Opnd::err(Err::Ref);
    return Opnd::area(a[0].ref.si, Range((int)r0, (int)c0, (int)r1, (int)c1).norm());
}
Opnd f_indirect(Evaluator& ev, Args& a) {
    STR(t, 0) BOOL_OR(a1, 1, true)
    Opnd out;
    if (!ev.parse_ref_text(t, a1, out)) return Opnd::err(Err::Ref);
    return out;
}
Opnd f_row(Evaluator& ev, Args& a) {
    if (a.empty() || a[0].missing) return Opnd::num(ev.r + 1);
    if (a[0].k != Opnd::R) return Opnd::err(Err::Value);
    const Range& g = a[0].ref.rg;
    if (g.rows() == 1) return Opnd::num(g.r0 + 1);
    Range u = g.r1 >= MAX_ROWS - 1 ? ev.clip_used(a[0].ref.si, g) : g;
    auto arr = std::make_shared<Array>(std::max(1, u.rows()), 1);
    for (int i = 0; i < arr->rows; ++i) arr->at(i, 0) = Value::number(g.r0 + i + 1);
    return Opnd::array(arr);
}
Opnd f_column(Evaluator& ev, Args& a) {
    if (a.empty() || a[0].missing) return Opnd::num(ev.c + 1);
    if (a[0].k != Opnd::R) return Opnd::err(Err::Value);
    const Range& g = a[0].ref.rg;
    if (g.cols() == 1) return Opnd::num(g.c0 + 1);
    auto arr = std::make_shared<Array>(1, std::min(g.cols(), 16384));
    for (int j = 0; j < arr->cols; ++j) arr->at(0, j) = Value::number(g.c0 + j + 1);
    return Opnd::array(arr);
}
Opnd f_rows(Evaluator& ev, Args& a) {
    if (a[0].k == Opnd::R) return Opnd::num(a[0].ref.rg.rows());
    if (a[0].k == Opnd::A) return Opnd::num(a[0].arr->rows);
    if (a[0].k == Opnd::V) return a[0].v.is_err() ? a[0] : Opnd::num(1);
    return Opnd::err(Err::Value);
}
Opnd f_columns(Evaluator& ev, Args& a) {
    if (a[0].k == Opnd::R) return Opnd::num(a[0].ref.rg.cols());
    if (a[0].k == Opnd::A) return Opnd::num(a[0].arr->cols);
    if (a[0].k == Opnd::V) return a[0].v.is_err() ? a[0] : Opnd::num(1);
    return Opnd::err(Err::Value);
}
Opnd f_address(Evaluator& ev, Args& a) {
    NUM(r, 0) NUM(c, 1) NUM_OR(absn, 2, 1) BOOL_OR(a1, 3, true) STR_OR(sheet, 4, "")
    int R = (int)r, C = (int)c, ab = (int)absn;
    if (R < 1 || C < 1 || R > MAX_ROWS || C > MAX_COLS || ab < 1 || ab > 4) return Opnd::err(Err::Value);
    std::string o;
    if (a1) o = cell_name(R - 1, C - 1, ab == 1 || ab == 2, ab == 1 || ab == 3);
    else {
        o = "R" + (ab == 1 || ab == 2 ? std::to_string(R) : "[" + std::to_string(R) + "]");
        o += "C" + (ab == 1 || ab == 3 ? std::to_string(C) : "[" + std::to_string(C) + "]");
    }
    if (!sheet.empty()) o = quote_sheet_name(sheet) + "!" + o;
    return Opnd::str(o);
}
Opnd f_transpose(Evaluator& ev, Args& a) {
    auto A = ev.to_array(a[0]);
    auto o = std::make_shared<Array>(A->cols, A->rows);
    for (int i = 0; i < A->rows; ++i) for (int j = 0; j < A->cols; ++j) o->at(j, i) = A->at(i, j);
    return Opnd::array(o);
}
std::string row_key(const Array& A, int i, bool by_col) {
    std::string k;
    int n = by_col ? A.rows : A.cols;
    for (int j = 0; j < n; ++j) {
        const Value& v = by_col ? A.at(j, i) : A.at(i, j);
        k += (char)('0' + v.type);
        k += v.is_str() ? lower_utf8(v.str) : value_to_text(v);
        k += '\x1f';
    }
    return k;
}
Opnd f_unique(Evaluator& ev, Args& a) {
    auto A = ev.to_array(a[0]);
    BOOL_OR(by_col, 1, false) BOOL_OR(once, 2, false)
    int n = by_col ? A->cols : A->rows;
    std::unordered_map<std::string, int> cnt;
    std::vector<std::string> keys(n);
    for (int i = 0; i < n; ++i) { keys[i] = row_key(*A, i, by_col); ++cnt[keys[i]]; }
    std::vector<int> pick;
    std::unordered_set<std::string> seen;
    for (int i = 0; i < n; ++i) {
        if (once) { if (cnt[keys[i]] == 1) pick.push_back(i); }
        else if (seen.insert(keys[i]).second) pick.push_back(i);
    }
    if (pick.empty()) return Opnd::err(Err::Calc);
    std::shared_ptr<Array> o;
    if (by_col) { o = std::make_shared<Array>(A->rows, (int)pick.size()); for (size_t k = 0; k < pick.size(); ++k) for (int i = 0; i < A->rows; ++i) o->at(i, (int)k) = A->at(i, pick[k]); }
    else { o = std::make_shared<Array>((int)pick.size(), A->cols); for (size_t k = 0; k < pick.size(); ++k) for (int j = 0; j < A->cols; ++j) o->at((int)k, j) = A->at(pick[k], j); }
    return Opnd::array(o);
}
int sort_cmp(const Value& x, const Value& y) {
    // blanks last, numbers < text < bool < errors; natural compare for text (refdes friendly)
    if (x.is_empty() != y.is_empty()) return x.is_empty() ? 1 : -1;
    if (x.is_str() && y.is_str()) return natural_cmp(x.str, y.str);
    return compare_values(x, y);
}
Opnd f_sort(Evaluator& ev, Args& a) {
    auto A = ev.to_array(a[0]);
    NUM_OR(idx0, 1, 1) NUM_OR(ord0, 2, 1) BOOL_OR(by_col, 3, false)
    int idx = (int)idx0 - 1;
    bool desc = ord0 < 0;
    int n = by_col ? A->cols : A->rows, m = by_col ? A->rows : A->cols;
    if (idx < 0 || idx >= m) return Opnd::err(Err::Value);
    std::vector<int> p(n);
    std::iota(p.begin(), p.end(), 0);
    std::stable_sort(p.begin(), p.end(), [&](int x, int y) {
        const Value& vx = by_col ? A->at(idx, x) : A->at(x, idx);
        const Value& vy = by_col ? A->at(idx, y) : A->at(y, idx);
        int c = sort_cmp(vx, vy);
        return desc ? c > 0 : c < 0;
    });
    auto o = std::make_shared<Array>(A->rows, A->cols);
    for (int k = 0; k < n; ++k)
        for (int j = 0; j < m; ++j) { if (by_col) o->at(j, k) = A->at(j, p[k]); else o->at(k, j) = A->at(p[k], j); }
    return Opnd::array(o);
}
Opnd f_sortby(Evaluator& ev, Args& a) {
    auto A = ev.to_array(a[0]);
    struct Key { std::shared_ptr<const Array> k; bool desc; };
    std::vector<Key> keys;
    for (size_t i = 1; i < a.size(); i += 2) {
        auto K = ev.to_array(a[i]);
        double ord = 1;
        if (i + 1 < a.size() && !a[i + 1].missing) { Value v = ev.scalar(a[i + 1]); Err e; if (!to_number(v, ord, e)) return Opnd::err(e); }
        if (K->rows != A->rows || K->cols != 1) { if (!(K->cols == A->cols && K->rows == 1)) return Opnd::err(Err::Value); }
        keys.push_back({K, ord < 0});
    }
    bool by_col = !keys.empty() && keys[0].k->rows == 1 && A->rows > 1 ? false : (!keys.empty() && keys[0].k->rows == 1 && keys[0].k->cols == A->cols && A->cols > 1);
    int n = by_col ? A->cols : A->rows;
    std::vector<int> p(n);
    std::iota(p.begin(), p.end(), 0);
    std::stable_sort(p.begin(), p.end(), [&](int x, int y) {
        for (auto& k : keys) {
            const Value& vx = by_col ? k.k->at(0, x) : k.k->at(x, 0);
            const Value& vy = by_col ? k.k->at(0, y) : k.k->at(y, 0);
            int c = sort_cmp(vx, vy);
            if (c) return k.desc ? c > 0 : c < 0;
        }
        return false;
    });
    auto o = std::make_shared<Array>(A->rows, A->cols);
    for (int k = 0; k < n; ++k) {
        if (by_col) for (int i = 0; i < A->rows; ++i) o->at(i, k) = A->at(i, p[k]);
        else for (int j = 0; j < A->cols; ++j) o->at(k, j) = A->at(p[k], j);
    }
    return Opnd::array(o);
}
Opnd f_filter(Evaluator& ev, Args& a) {
    auto A = ev.to_array(a[0]);
    auto M = ev.to_array(a[1]);
    bool rows_mode = M->cols == 1 && M->rows == A->rows;
    bool cols_mode = !rows_mode && M->rows == 1 && M->cols == A->cols;
    if (!rows_mode && !cols_mode) return Opnd::err(Err::Value);
    std::vector<int> keep;
    int n = rows_mode ? A->rows : A->cols;
    for (int i = 0; i < n; ++i) {
        const Value& v = rows_mode ? M->at(i, 0) : M->at(0, i);
        if (v.is_err()) return Opnd::val(v);
        bool b = false; Err e;
        if (v.is_str()) { double d; if (!text_to_number(v.str, d)) return Opnd::err(Err::Value); b = d != 0; }
        else if (!to_bool(v, b, e)) return Opnd::err(e);
        if (b) keep.push_back(i);
    }
    if (keep.empty()) { if (!missing(a, 2)) return a[2]; return Opnd::err(Err::Calc); }
    std::shared_ptr<Array> o;
    if (rows_mode) { o = std::make_shared<Array>((int)keep.size(), A->cols); for (size_t k = 0; k < keep.size(); ++k) for (int j = 0; j < A->cols; ++j) o->at((int)k, j) = A->at(keep[k], j); }
    else { o = std::make_shared<Array>(A->rows, (int)keep.size()); for (size_t k = 0; k < keep.size(); ++k) for (int i = 0; i < A->rows; ++i) o->at(i, (int)k) = A->at(i, keep[k]); }
    return Opnd::array(o);
}
Opnd f_hyperlink(Evaluator& ev, Args& a) {
    if (!missing(a, 1)) return a[1];
    return Opnd::val(ev.scalar(a[0]));
}

// ------------------------------------------------------------------- text --
Opnd f_left(Evaluator& ev, Args& a) {
    STR(s, 0) NUM_OR(n, 1, 1)
    if (n < 0) return Opnd::err(Err::Value);
    auto cp = utf8_decode(s);
    return Opnd::str(utf8_encode(cp, 0, (size_t)std::min<double>(n, (double)cp.size())));
}
Opnd f_right(Evaluator& ev, Args& a) {
    STR(s, 0) NUM_OR(n, 1, 1)
    if (n < 0) return Opnd::err(Err::Value);
    auto cp = utf8_decode(s);
    size_t k = (size_t)std::min<double>(n, (double)cp.size());
    return Opnd::str(utf8_encode(cp, cp.size() - k, k));
}
Opnd f_mid(Evaluator& ev, Args& a) {
    STR(s, 0) NUM(st, 1) NUM(n, 2)
    if (st < 1 || n < 0) return Opnd::err(Err::Value);
    auto cp = utf8_decode(s);
    size_t from = (size_t)st - 1;
    if (from >= cp.size()) return Opnd::str("");
    return Opnd::str(utf8_encode(cp, from, (size_t)std::min<double>(n, 1e9)));
}
Opnd f_len(Evaluator& ev, Args& a) { STR(s, 0) return Opnd::num((double)utf8_len(s)); }
Opnd find_impl(Evaluator& ev, Args& a, bool ci) {
    STR(needle, 0) STR(hay, 1) NUM_OR(st, 2, 1)
    auto H = utf8_decode(hay);
    if (st < 1 || st > H.size() + 1) return Opnd::err(Err::Value);
    size_t start = (size_t)st - 1;
    if (!ci) {
        auto N = utf8_decode(needle);
        if (N.empty()) return Opnd::num(st);
        for (size_t i = start; i + N.size() <= H.size(); ++i) if (std::equal(N.begin(), N.end(), H.begin() + i)) return Opnd::num((double)i + 1);
        return Opnd::err(Err::Value);
    }
    // SEARCH: case-insensitive with wildcards; find the earliest start position whose prefix matches
    if (needle.empty()) return Opnd::num(st);
    std::string pat = needle + "*";
    bool wild = needle.find_first_of("*?~") != std::string::npos;
    if (!wild) {
        auto N = utf8_decode(lower_utf8(needle));
        auto L = utf8_decode(lower_utf8(hay));
        for (size_t i = start; i + N.size() <= L.size(); ++i) if (std::equal(N.begin(), N.end(), L.begin() + i)) return Opnd::num((double)i + 1);
        return Opnd::err(Err::Value);
    }
    for (size_t i = start; i < H.size(); ++i) if (wildcard_match(pat, utf8_encode(H, i))) return Opnd::num((double)i + 1);
    return Opnd::err(Err::Value);
}
Opnd f_find(Evaluator& ev, Args& a) { return find_impl(ev, a, false); }
Opnd f_search(Evaluator& ev, Args& a) { return find_impl(ev, a, true); }
Opnd f_substitute(Evaluator& ev, Args& a) {
    STR(s, 0) STR(old, 1) STR(nw, 2) NUM_OR(inst, 3, 0)
    if (!missing(a, 3) && inst < 1) return Opnd::err(Err::Value);
    if (old.empty()) return Opnd::str(s);
    std::string o;
    size_t p = 0; int k = 0;
    for (;;) {
        size_t q = s.find(old, p);
        if (q == std::string::npos) { o += s.substr(p); break; }
        ++k;
        o += s.substr(p, q - p);
        if (inst == 0 || k == (int)inst) o += nw; else o += old;
        p = q + old.size();
    }
    return Opnd::str(o);
}
Opnd f_replace(Evaluator& ev, Args& a) {
    STR(s, 0) NUM(st, 1) NUM(n, 2) STR(nw, 3)
    if (st < 1 || n < 0) return Opnd::err(Err::Value);
    auto cp = utf8_decode(s);
    size_t from = std::min((size_t)st - 1, cp.size());
    size_t cnt = std::min((size_t)n, cp.size() - from);
    return Opnd::str(utf8_encode(cp, 0, from) + nw + utf8_encode(cp, from + cnt));
}
Opnd f_trim(Evaluator& ev, Args& a) {
    STR(s, 0)
    std::string o; bool sp = false;
    for (char ch : s) { if (ch == ' ') { sp = true; continue; } if (sp && !o.empty()) o += ' '; sp = false; o += ch; }
    return Opnd::str(o);
}
Opnd f_clean(Evaluator& ev, Args& a) { STR(s, 0) std::string o; for (char ch : s) if ((unsigned char)ch >= 32) o += ch; return Opnd::str(o); }
Opnd f_upper(Evaluator& ev, Args& a) { STR(s, 0) return Opnd::str(upper_utf8(s)); }
Opnd f_lower(Evaluator& ev, Args& a) { STR(s, 0) return Opnd::str(lower_utf8(s)); }
Opnd f_proper(Evaluator& ev, Args& a) {
    STR(s, 0)
    std::string o; bool prev_letter = false;
    for (uint32_t c : utf8_decode(s)) {
        std::string one; utf8_append(one, c);
        bool letter = (c < 128 && isalpha((int)c)) || c >= 0xC0;
        o += letter ? (prev_letter ? lower_utf8(one) : upper_utf8(one)) : one;
        prev_letter = letter;
    }
    return Opnd::str(o);
}
Opnd f_text(Evaluator& ev, Args& a) {
    Value v = ev.scalar(a[0]);
    if (v.is_err()) return Opnd::val(v);
    STR(fmt, 1)
    if (v.is_str()) { double d; if (text_to_number(v.str, d)) v = Value::number(d); }
    if (v.is_empty()) v = Value::number(0);
    return Opnd::str(format_value(v, fmt, ev.book.date1904).text);
}
Opnd f_value(Evaluator& ev, Args& a) {
    Value v = ev.scalar(a[0]);
    if (v.is_err()) return Opnd::val(v);
    if (v.is_num()) return Opnd::val(v);
    if (v.is_empty()) return Opnd::num(0);
    if (v.is_bool()) return Opnd::err(Err::Value);
    double d;
    std::string s = v.str;
    if (text_to_number(s, d)) return Opnd::num(d);
    return Opnd::err(Err::Value);
}
Opnd f_numbervalue(Evaluator& ev, Args& a) {
    STR(s, 0) STR_OR(dec, 1, ".") STR_OR(grp, 2, ",")
    std::string t;
    char dc = dec.empty() ? '.' : dec[0];
    for (char ch : s) { if (ch == ' ') continue; if (grp.find(ch) != std::string::npos && ch != dc) continue; t += ch == dc ? '.' : ch; }
    double pct = 1;
    while (!t.empty() && t.back() == '%') { pct /= 100; t.pop_back(); }
    if (t.empty()) return Opnd::num(0);
    double d;
    if (!parse_number_strict(t, d)) return Opnd::err(Err::Value);
    return Opnd::num(d * pct);
}
Opnd f_concat(Evaluator& ev, Args& a) {
    std::string o;
    for (auto& x : a) {
        if (x.missing) continue;
        if (x.k == Opnd::V) { if (x.v.is_err()) return x; o += to_text(x.v); continue; }
        Value bad; bool isbad = false;
        ev.each(x, [&](const Value& v, bool) { if (isbad) return; if (v.is_err()) { bad = v; isbad = true; return; } o += to_text(v); });
        if (isbad) return Opnd::val(bad);
        if (o.size() > 32767) return Opnd::err(Err::Value);
    }
    return Opnd::str(o);
}
Opnd f_textjoin(Evaluator& ev, Args& a) {
    STR(delim, 0) BOOL_OR(skip, 1, true)
    std::string o; bool first = true;
    for (size_t i = 2; i < a.size(); ++i) {
        const Opnd& x = a[i];
        if (x.missing) { if (!skip) { if (!first) o += delim; first = false; } continue; }
        Value bad; bool isbad = false;
        auto add = [&](const Value& v, bool) {
            if (isbad) return;
            if (v.is_err()) { bad = v; isbad = true; return; }
            std::string t = to_text(v);
            if (skip && t.empty()) return;
            if (!first) o += delim;
            o += t; first = false;
        };
        if (x.k == Opnd::V) add(x.v, false); else ev.each(x, add);
        if (isbad) return Opnd::val(bad);
    }
    if (o.size() > 32767) return Opnd::err(Err::Value);
    return Opnd::str(o);
}
Opnd f_textsplit(Evaluator& ev, Args& a) {
    STR(s, 0)
    auto delims = [&](size_t i) {
        std::vector<std::string> d;
        if (missing(a, i)) return d;
        ev.each(a[i], [&](const Value& v, bool) { std::string t = to_text(v); if (!t.empty()) d.push_back(t); });
        return d;
    };
    std::vector<std::string> cd = delims(1), rd = delims(2);
    BOOL_OR(ignore_empty, 3, false) NUM_OR(mm, 4, 0)
    Value pad = Value::error(Err::NA);
    if (!missing(a, 5)) pad = ev.scalar(a[5]);
    auto split = [&](const std::string& t, const std::vector<std::string>& ds) {
        std::vector<std::string> out;
        if (ds.empty()) { out.push_back(t); return out; }
        std::string hay = mm ? lower_utf8(t) : t;
        size_t p = 0;
        for (;;) {
            size_t best = std::string::npos, bl = 0;
            for (auto& d : ds) { std::string dd = mm ? lower_utf8(d) : d; size_t q = hay.find(dd, p); if (q < best) { best = q; bl = dd.size(); } }
            if (best == std::string::npos) { out.push_back(t.substr(p)); break; }
            out.push_back(t.substr(p, best - p));
            p = best + bl;
        }
        if (ignore_empty) out.erase(std::remove(out.begin(), out.end(), std::string()), out.end());
        return out;
    };
    std::vector<std::vector<std::string>> rows;
    for (auto& r : split(s, rd)) rows.push_back(split(r, cd));
    size_t nc = 0; for (auto& r : rows) nc = std::max(nc, r.size());
    if (rows.empty() || nc == 0) return Opnd::err(Err::Calc);
    auto o = std::make_shared<Array>((int)rows.size(), (int)nc);
    for (size_t i = 0; i < rows.size(); ++i) for (size_t j = 0; j < nc; ++j) o->at((int)i, (int)j) = j < rows[i].size() ? Value::text(rows[i][j]) : pad;
    return Opnd::array(o);
}
Opnd text_before_after(Evaluator& ev, Args& a, bool before) {
    STR(s, 0) STR(d, 1) NUM_OR(inst, 2, 1)
    if (inst == 0) return Opnd::err(Err::Value);
    if (d.empty()) return Opnd::str(before ? (inst > 0 ? "" : s) : (inst > 0 ? s : ""));
    std::vector<size_t> pos;
    for (size_t p = s.find(d); p != std::string::npos; p = s.find(d, p + d.size())) pos.push_back(p);
    long k = (long)inst;
    if (std::labs(k) > (long)pos.size()) return Opnd::err(Err::NA);
    size_t p = k > 0 ? pos[k - 1] : pos[pos.size() + k];
    return Opnd::str(before ? s.substr(0, p) : s.substr(p + d.size()));
}
Opnd f_textbefore(Evaluator& ev, Args& a) { return text_before_after(ev, a, true); }
Opnd f_textafter(Evaluator& ev, Args& a) { return text_before_after(ev, a, false); }
Opnd f_exact(Evaluator& ev, Args& a) { STR(x, 0) STR(y, 1) return Opnd::boolean(x == y); }
Opnd f_rept(Evaluator& ev, Args& a) {
    STR(s, 0) NUM(n, 1)
    if (n < 0) return Opnd::err(Err::Value);
    if (s.size() * (size_t)n > 32767) return Opnd::err(Err::Value);
    std::string o; for (int i = 0; i < (int)n; ++i) o += s;
    return Opnd::str(o);
}
static const uint16_t k_cp1252[32] = {0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};
Opnd f_char(Evaluator& ev, Args& a) {
    NUM(n, 0)
    int k = (int)n;
    if (k < 1 || k > 255) return Opnd::err(Err::Value);
    uint32_t cp = (k >= 128 && k < 160) ? k_cp1252[k - 128] : (uint32_t)k;
    if (!cp) cp = (uint32_t)k;
    std::string o; utf8_append(o, cp);
    return Opnd::str(o);
}
Opnd f_code(Evaluator& ev, Args& a) {
    STR(s, 0)
    if (s.empty()) return Opnd::err(Err::Value);
    uint32_t cp = utf8_decode(s)[0];
    if (cp >= 256 || (cp >= 128 && cp < 160)) for (int i = 0; i < 32; ++i) if (k_cp1252[i] == cp) return Opnd::num(128 + i);
    return Opnd::num(cp < 256 ? cp : 63);
}
Opnd f_unichar(Evaluator& ev, Args& a) { NUM(n, 0) if (n < 1 || n > 0x10FFFF) return Opnd::err(Err::Value); std::string o; utf8_append(o, (uint32_t)n); return Opnd::str(o); }
Opnd f_unicode(Evaluator& ev, Args& a) { STR(s, 0) if (s.empty()) return Opnd::err(Err::Value); return Opnd::num(utf8_decode(s)[0]); }
Opnd f_fixed(Evaluator& ev, Args& a) {
    NUM(x, 0) NUM_OR(d, 1, 2) BOOL_OR(nocomma, 2, false)
    int dd = (int)d;
    if (dd > 127) return Opnd::err(Err::Value);
    double r = dd >= 0 ? x : std::round(x / std::pow(10.0, -dd)) * std::pow(10.0, -dd);
    std::string code = nocomma ? "0" : "#,##0";
    if (dd > 0) code += "." + std::string(dd, '0');
    return Opnd::str(format_value(Value::number(r), code).text);
}
Opnd f_dollar(Evaluator& ev, Args& a) {
    NUM(x, 0) NUM_OR(d, 1, 2)
    int dd = (int)d;
    std::string dec = dd > 0 ? "." + std::string(dd, '0') : "";
    double r = dd >= 0 ? x : std::round(x / std::pow(10.0, -dd)) * std::pow(10.0, -dd);
    std::string t = format_value(Value::number(r), "\"$\"#,##0" + dec + "_);(\"$\"#,##0" + dec + ")").text;
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return Opnd::str(t);
}
Opnd f_t(Evaluator& ev, Args& a) { Value v = ev.scalar(a[0]); if (v.is_err()) return Opnd::val(v); return Opnd::str(v.is_str() ? v.str : ""); }
Opnd f_n(Evaluator& ev, Args& a) { Value v = ev.scalar(a[0]); if (v.is_err()) return Opnd::val(v); return Opnd::num(v.is_num() || v.is_bool() ? v.num : 0); }

// ------------------------------------------------------------------- date --
bool date_arg(Evaluator& ev, const Opnd& o, double& d, Opnd& err) {
    Value v = ev.scalar(o);
    if (v.is_err()) { err = Opnd::val(v); return false; }
    if (v.is_str()) { if (!text_to_number(v.str, d, ev.book.date1904)) { err = Opnd::err(Err::Value); return false; } }
    else d = v.num;
    if (d < 0) { err = Opnd::err(Err::Num); return false; }
    return true;
}
#define DATE(var, idx) double var; { Opnd _e; if (!date_arg(ev, a[idx], var, _e)) return _e; }
void now_parts(int& y, int& m, int& d, double& frac) {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    y = tmv.tm_year + 1900; m = tmv.tm_mon + 1; d = tmv.tm_mday;
    frac = time_serial(tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}
Opnd f_today(Evaluator& ev, Args&) { int y, m, d; double f; now_parts(y, m, d, f); return Opnd::num(date_serial(y, m, d, ev.book.date1904)); }
Opnd f_now(Evaluator& ev, Args&) { int y, m, d; double f; now_parts(y, m, d, f); return Opnd::num(date_serial(y, m, d, ev.book.date1904) + f); }
Opnd f_date(Evaluator& ev, Args& a) {
    NUM(y0, 0) NUM(m0, 1) NUM(d0, 2)
    int y = (int)std::trunc(y0);
    if (y < 0 || y > 9999) return Opnd::err(Err::Num);
    if (y < 1900) y += 1900;
    double s = date_serial(y, (int)std::trunc(m0), (int)std::trunc(d0), ev.book.date1904);
    if (s < 0) return Opnd::err(Err::Num);
    return Opnd::num(s);
}
Opnd f_time(Evaluator& ev, Args& a) {
    NUM(h, 0) NUM(m, 1) NUM(s, 2)
    double t = (std::trunc(h) * 3600 + std::trunc(m) * 60 + std::trunc(s)) / 86400.0;
    if (t < 0) return Opnd::err(Err::Num);
    return Opnd::num(t - std::floor(t));
}
Opnd ymd_part(Evaluator& ev, Args& a, int which) {
    DATE(d, 0)
    int y, m, dd; serial_to_ymd(d, y, m, dd, ev.book.date1904);
    return Opnd::num(which == 0 ? y : which == 1 ? m : dd);
}
Opnd f_year(Evaluator& ev, Args& a) { return ymd_part(ev, a, 0); }
Opnd f_month(Evaluator& ev, Args& a) { return ymd_part(ev, a, 1); }
Opnd f_day(Evaluator& ev, Args& a) { return ymd_part(ev, a, 2); }
Opnd hms_part(Evaluator& ev, Args& a, int which) {
    DATE(d, 0)
    double secs = std::floor((d - std::floor(d)) * 86400 + 0.5);
    if (secs >= 86400) secs -= 86400;
    long s = (long)secs;
    return Opnd::num(which == 0 ? s / 3600 : which == 1 ? s / 60 % 60 : s % 60);
}
Opnd f_hour(Evaluator& ev, Args& a) { return hms_part(ev, a, 0); }
Opnd f_minute(Evaluator& ev, Args& a) { return hms_part(ev, a, 1); }
Opnd f_second(Evaluator& ev, Args& a) { return hms_part(ev, a, 2); }
Opnd f_isoweeknum(Evaluator& ev, Args& a);
int dow0(double serial, bool d1904) { long s = (long)std::floor(serial); return (int)(((s + (d1904 ? 5 : 6)) % 7 + 7) % 7); }   // 0 = Sunday
Opnd f_weekday(Evaluator& ev, Args& a) {
    DATE(d, 0) NUM_OR(t, 1, 1)
    int w = dow0(d, ev.book.date1904);   // Sun=0
    switch ((int)t) {
    case 1: case 17: return Opnd::num(w + 1);
    case 2: case 11: return Opnd::num((w + 6) % 7 + 1);
    case 3: return Opnd::num((w + 6) % 7);
    case 12: return Opnd::num((w + 5) % 7 + 1);
    case 13: return Opnd::num((w + 4) % 7 + 1);
    case 14: return Opnd::num((w + 3) % 7 + 1);
    case 15: return Opnd::num((w + 2) % 7 + 1);
    case 16: return Opnd::num((w + 1) % 7 + 1);
    default: return Opnd::err(Err::Num);
    }
}
Opnd f_weeknum(Evaluator& ev, Args& a) {
    DATE(d, 0) NUM_OR(t, 1, 1)
    int y, m, dd; serial_to_ymd(d, y, m, dd, ev.book.date1904);
    double jan1 = date_serial(y, 1, 1, ev.book.date1904);
    int start = (int)t == 2 || (int)t == 11 ? 1 : 0;   // week starts Monday for 2/11
    if ((int)t >= 12 && (int)t <= 17) start = ((int)t - 10) % 7;
    if ((int)t == 21) {   // ISO
        Args b{Opnd::num(d)};
        return f_isoweeknum(ev, b);
    }
    int w1 = (dow0(jan1, ev.book.date1904) - start + 7) % 7;
    return Opnd::num(std::floor((d - jan1 + w1) / 7) + 1);
}
Opnd f_isoweeknum(Evaluator& ev, Args& a) {
    DATE(d, 0)
    int w = (dow0(d, ev.book.date1904) + 6) % 7;   // Mon=0
    double thursday = std::floor(d) - w + 3;
    int y, m, dd; serial_to_ymd(thursday, y, m, dd, ev.book.date1904);
    double jan1 = date_serial(y, 1, 1, ev.book.date1904);
    return Opnd::num(std::floor((thursday - jan1) / 7) + 1);
}
Opnd f_datedif(Evaluator& ev, Args& a) {
    DATE(s, 0) DATE(e, 1) STR(unit, 2)
    if (e < s) return Opnd::err(Err::Num);
    std::string u = upper_ascii(unit);
    bool d1904 = ev.book.date1904;
    int y1, m1, d1, y2, m2, d2;
    serial_to_ymd(s, y1, m1, d1, d1904); serial_to_ymd(e, y2, m2, d2, d1904);
    int months = (y2 - y1) * 12 + (m2 - m1) - (d2 < d1 ? 1 : 0);
    if (u == "D") return Opnd::num(std::floor(e) - std::floor(s));
    if (u == "M") return Opnd::num(months);
    if (u == "Y") return Opnd::num(std::floor(months / 12.0));
    if (u == "YM") return Opnd::num(months % 12);
    if (u == "MD") {
        int pm = m2 - 1, py = y2; if (pm < 1) { pm = 12; --py; }
        if (d2 >= d1) return Opnd::num(d2 - d1);
        double prev = date_serial(py, pm, d1, d1904);
        return Opnd::num(std::floor(e) - prev);
    }
    if (u == "YD") {
        int yy = y2; if (m2 < m1 || (m2 == m1 && d2 < d1)) --yy;
        double st = date_serial(yy, m1, d1, d1904);
        return Opnd::num(std::floor(e) - st);
    }
    return Opnd::err(Err::Num);
}
Opnd f_edate(Evaluator& ev, Args& a) {
    DATE(s, 0) NUM(mo, 1)
    int y, m, d; serial_to_ymd(s, y, m, d, ev.book.date1904);
    int nm = m + (int)std::trunc(mo);
    double first = date_serial(y, nm, 1, ev.book.date1904);
    int yy, mm, dd0; serial_to_ymd(first, yy, mm, dd0, ev.book.date1904);
    double last = date_serial(yy, mm + 1, 1, ev.book.date1904) - 1;
    int ly, lm, ld; serial_to_ymd(last, ly, lm, ld, ev.book.date1904);
    double r = date_serial(yy, mm, std::min(d, ld), ev.book.date1904);
    if (r < 0) return Opnd::err(Err::Num);
    return Opnd::num(r);
}
Opnd f_eomonth(Evaluator& ev, Args& a) {
    DATE(s, 0) NUM(mo, 1)
    int y, m, d; serial_to_ymd(s, y, m, d, ev.book.date1904);
    double r = date_serial(y, m + (int)std::trunc(mo) + 1, 1, ev.book.date1904) - 1;
    if (r < 0) return Opnd::err(Err::Num);
    return Opnd::num(r);
}
Opnd f_datevalue(Evaluator& ev, Args& a) {
    STR(s, 0)
    std::string fmt;
    Value v = parse_input(s, &fmt, ev.book.date1904);
    if (!v.is_num() || !is_date_format(fmt.empty() ? "General" : fmt)) return Opnd::err(Err::Value);
    return Opnd::num(std::floor(v.num));
}
Opnd f_timevalue(Evaluator& ev, Args& a) {
    STR(s, 0)
    std::string fmt;
    Value v = parse_input(s, &fmt, ev.book.date1904);
    if (!v.is_num() || !is_date_format(fmt.empty() ? "General" : fmt)) return Opnd::err(Err::Value);
    return Opnd::num(v.num - std::floor(v.num));
}
Opnd f_days(Evaluator& ev, Args& a) { DATE(e, 0) DATE(s, 1) return Opnd::num(std::floor(e) - std::floor(s)); }
bool holidays(Evaluator& ev, Args& a, size_t i, std::set<long>& h, Opnd& err) {
    if (missing(a, i)) return true;
    std::vector<double> v;
    Args one{a[i]};
    if (!collect(ev, one, 0, v, err)) return false;
    for (double x : v) h.insert((long)std::floor(x));
    return true;
}
Opnd f_networkdays(Evaluator& ev, Args& a) {
    DATE(s, 0) DATE(e, 1)
    std::set<long> h; Opnd er;
    if (!holidays(ev, a, 2, h, er)) return er;
    long x = (long)std::floor(s), y = (long)std::floor(e);
    int sign = 1;
    if (y < x) { std::swap(x, y); sign = -1; }
    long n = 0;
    for (long d = x; d <= y; ++d) { int w = dow0((double)d, ev.book.date1904); if (w != 0 && w != 6 && !h.count(d)) ++n; }
    return Opnd::num((double)(n * sign));
}
Opnd f_workday(Evaluator& ev, Args& a) {
    DATE(s, 0) NUM(n0, 1)
    std::set<long> h; Opnd er;
    if (!holidays(ev, a, 2, h, er)) return er;
    long d = (long)std::floor(s), n = (long)std::trunc(n0);
    int step = n >= 0 ? 1 : -1;
    while (n != 0) { d += step; int w = dow0((double)d, ev.book.date1904); if (w != 0 && w != 6 && !h.count(d)) n -= step; }
    return Opnd::num((double)d);
}
Opnd f_yearfrac(Evaluator& ev, Args& a) {
    DATE(s, 0) DATE(e, 1) NUM_OR(b, 2, 0)
    if (e < s) std::swap(s, e);
    bool d1904 = ev.book.date1904;
    int y1, m1, d1, y2, m2, d2;
    serial_to_ymd(s, y1, m1, d1, d1904); serial_to_ymd(e, y2, m2, d2, d1904);
    switch ((int)b) {
    case 0: {
        if (d1 == 31) d1 = 30;
        if (d2 == 31 && d1 >= 30) d2 = 30;
        return Opnd::num(((y2 - y1) * 360.0 + (m2 - m1) * 30.0 + (d2 - d1)) / 360.0);
    }
    case 1: {
        double days = std::floor(e) - std::floor(s);
        auto leap = [](int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; };
        double avg;
        if (y1 == y2) avg = leap(y1) ? 366 : 365;
        else if (y2 == y1 + 1 && (m2 < m1 || (m2 == m1 && d2 <= d1))) avg = (leap(y1) && (m1 <= 2)) || (leap(y2) && (m2 > 2 || (m2 == 2 && d2 == 29))) ? 366 : 365;
        else { double tot = 0; for (int y = y1; y <= y2; ++y) tot += leap(y) ? 366 : 365; avg = tot / (y2 - y1 + 1); }
        return Opnd::num(days / avg);
    }
    case 2: return Opnd::num((std::floor(e) - std::floor(s)) / 360.0);
    case 3: return Opnd::num((std::floor(e) - std::floor(s)) / 365.0);
    case 4: { if (d1 == 31) d1 = 30; if (d2 == 31) d2 = 30; return Opnd::num(((y2 - y1) * 360.0 + (m2 - m1) * 30.0 + (d2 - d1)) / 360.0); }
    default: return Opnd::err(Err::Num);
    }
}

// ------------------------------------------------------------------- info --
Opnd f_isblank(Evaluator& ev, Args& a) { Value v = ev.scalar(a[0]); return Opnd::boolean(v.is_empty() && (a[0].k == Opnd::R || a[0].k == Opnd::V)); }
Opnd f_isnumber(Evaluator& ev, Args& a) { return Opnd::boolean(ev.scalar(a[0]).is_num()); }
Opnd f_istext(Evaluator& ev, Args& a) { return Opnd::boolean(ev.scalar(a[0]).is_str()); }
Opnd f_isnontext(Evaluator& ev, Args& a) { return Opnd::boolean(!ev.scalar(a[0]).is_str()); }
Opnd f_islogical(Evaluator& ev, Args& a) { return Opnd::boolean(ev.scalar(a[0]).is_bool()); }
Opnd f_iserror(Evaluator& ev, Args& a) { return Opnd::boolean(ev.scalar(a[0]).is_err()); }
Opnd f_iserr(Evaluator& ev, Args& a) { Value v = ev.scalar(a[0]); return Opnd::boolean(v.is_err() && v.err != Err::NA); }
Opnd f_isna(Evaluator& ev, Args& a) { Value v = ev.scalar(a[0]); return Opnd::boolean(v.is_err() && v.err == Err::NA); }
Opnd f_isref(Evaluator&, Args& a) { return Opnd::boolean(a[0].is_ref()); }
Opnd f_iseven(Evaluator& ev, Args& a) { NUM(x, 0) return Opnd::boolean(((long long)std::floor(std::fabs(x))) % 2 == 0); }
Opnd f_isodd(Evaluator& ev, Args& a) { NUM(x, 0) return Opnd::boolean(((long long)std::floor(std::fabs(x))) % 2 == 1); }
Opnd f_isformula(Evaluator& ev, Args& a) {
    if (a[0].k != Opnd::R) return Opnd::err(Err::Value);
    const Cell* c = ev.book.sheets[a[0].ref.si].cell(a[0].ref.rg.r0, a[0].ref.rg.c0);
    return Opnd::boolean(c && c->f);
}
Opnd f_formulatext(Evaluator& ev, Args& a) {
    if (a[0].k != Opnd::R) return Opnd::err(Err::NA);
    int r = a[0].ref.rg.r0, c = a[0].ref.rg.c0;
    const Cell* cl = ev.book.sheets[a[0].ref.si].cell(r, c);
    if (!cl || !cl->f) return Opnd::err(Err::NA);
    return Opnd::str("=" + formula_text(*cl->f, ev.book, a[0].ref.si, r, c));
}
Opnd f_na(Evaluator&, Args&) { return Opnd::err(Err::NA); }
Opnd f_errortype(Evaluator& ev, Args& a) {
    Value v = ev.scalar(a[0]);
    if (!v.is_err()) return Opnd::err(Err::NA);
    static const int code[] = {0, 1, 2, 3, 4, 5, 6, 7, 9, 14, 0};
    return Opnd::num(code[(int)v.err]);
}
Opnd f_type(Evaluator& ev, Args& a) {
    if (a[0].k == Opnd::A || (a[0].k == Opnd::R && (a[0].ref.rg.rows() > 1 || a[0].ref.rg.cols() > 1)) || a[0].k == Opnd::RL) return Opnd::num(64);
    Value v = ev.scalar(a[0]);
    switch (v.type) { case Value::Empty: case Value::Number: return Opnd::num(1); case Value::String: return Opnd::num(2); case Value::Bool: return Opnd::num(4); case Value::Error: return Opnd::num(16); }
    return Opnd::num(1);
}
Opnd f_sheet(Evaluator& ev, Args& a) {
    if (a.empty() || a[0].missing) return Opnd::num(ev.si + 1);
    if (a[0].is_ref()) return Opnd::num((a[0].k == Opnd::R ? a[0].ref.si : a[0].refs[0].si) + 1);
    Value v = ev.scalar(a[0]);
    if (v.is_str()) { int i = ev.book.find_sheet(v.str); if (i >= 0) return Opnd::num(i + 1); }
    return Opnd::err(Err::NA);
}
Opnd f_sheets(Evaluator& ev, Args& a) {
    if (!a.empty() && a[0].k == Opnd::RL) return Opnd::num((double)a[0].refs.size());
    if (!a.empty() && a[0].k == Opnd::R) return Opnd::num(1);
    return Opnd::num(ev.book.sheet_count());
}

// -------------------------------------------------------------------- BOM --
std::vector<std::string> refdes_expand(const std::string& s) {
    std::vector<std::string> out;
    std::string tok;
    auto flush = [&]() {
        if (tok.empty()) return;
        size_t dash = tok.find('-');
        if (dash != std::string::npos && dash > 0 && dash + 1 < tok.size()) {
            std::string l = tok.substr(0, dash), r = tok.substr(dash + 1);
            auto split = [](const std::string& x, std::string& pre, long& n) {
                size_t k = x.size();
                while (k > 0 && isdigit((unsigned char)x[k - 1])) --k;
                if (k == x.size() || x.size() - k > 9) return false;
                pre = x.substr(0, k); n = atol(x.c_str() + k);
                return true;
            };
            std::string pl, pr; long nl, nr;
            if (split(l, pl, nl) && split(r, pr, nr) && !pl.empty() && (pr.empty() || ci_equal(pr, pl)) && nr >= nl && nr - nl < 100000) {
                for (long i = nl; i <= nr; ++i) out.push_back(pl + std::to_string(i));
                tok.clear();
                return;
            }
        }
        out.push_back(tok);
        tok.clear();
    };
    for (size_t i = 0; i < s.size(); ++i) {
        char ch = s[i];
        if (ch == ',' || ch == ';' || ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') { flush(); continue; }
        // "R1 - R5" with spaces around the dash
        if (ch == '-' && tok.empty() && !out.empty()) { tok = out.back(); out.pop_back(); tok += '-'; while (i + 1 < s.size() && s[i + 1] == ' ') ++i; continue; }
        tok += ch;
    }
    flush();
    return out;
}
Opnd f_refcount(Evaluator& ev, Args& a) {
    double n = 0;
    for (auto& o : a) {
        if (o.missing) continue;
        Value bad; bool isbad = false;
        auto add = [&](const Value& v, bool) { if (isbad) return; if (v.is_err()) { bad = v; isbad = true; return; } if (v.is_str()) n += (double)refdes_expand(v.str).size(); else if (!v.is_empty()) n += 1; };
        if (o.k == Opnd::V) add(o.v, false); else ev.each(o, add);
        if (isbad) return Opnd::val(bad);
    }
    return Opnd::num(n);
}
Opnd f_refexpand(Evaluator& ev, Args& a) {
    STR(s, 0) STR_OR(sep, 1, ", ")
    std::string o;
    for (auto& r : refdes_expand(s)) { if (!o.empty()) o += sep; o += r; }
    return Opnd::str(o);
}
Opnd f_refcompress(Evaluator& ev, Args& a) {
    STR(s, 0) STR_OR(sep, 1, ", ")
    return Opnd::str(compress_refs(refdes_expand(s), sep));
}

} // namespace

int refdes_count(const std::string& cell) { return (int)refdes_expand(cell).size(); }

// ------------------------------------------------------------------ table --
#define F(name, cat, syn, desc, mn, mx, impl, args, fl) {{name, cat, syn, desc, mn, mx}, impl, args, fl}
const std::vector<FnDef>& fn_table() {
    static const std::vector<FnDef> t = {
        // Math
        F("SUM", "Math", "SUM(number1, [number2], ...)", "Adds all numbers in the arguments.", 1, -1, f_sum, "r", 0),
        F("SUMIF", "Math", "SUMIF(range, criteria, [sum_range])", "Sums cells that meet a criterion.", 2, 3, f_sumif, "rvr", 0),
        F("SUMIFS", "Math", "SUMIFS(sum_range, criteria_range1, criteria1, ...)", "Sums cells that meet several criteria.", 3, -1, f_sumifs, "r", 0),
        F("SUMPRODUCT", "Math", "SUMPRODUCT(array1, [array2], ...)", "Sum of the products of corresponding array elements.", 1, -1, f_sumproduct, "a", 0),
        F("SUMSQ", "Math", "SUMSQ(number1, ...)", "Sum of squares.", 1, -1, f_sumsq, "r", 0),
        F("PRODUCT", "Math", "PRODUCT(number1, ...)", "Multiplies all numbers.", 1, -1, f_product, "r", 0),
        F("ROUND", "Math", "ROUND(number, num_digits)", "Rounds half away from zero.", 2, 2, f_round, "v", 0),
        F("ROUNDUP", "Math", "ROUNDUP(number, num_digits)", "Rounds away from zero.", 2, 2, f_roundup, "v", 0),
        F("ROUNDDOWN", "Math", "ROUNDDOWN(number, num_digits)", "Rounds toward zero.", 2, 2, f_rounddown, "v", 0),
        F("MROUND", "Math", "MROUND(number, multiple)", "Rounds to the nearest multiple.", 2, 2, f_mround, "v", 0),
        F("CEILING", "Math", "CEILING(number, [significance])", "Rounds up to a multiple of significance.", 1, 2, f_ceiling, "v", 0),
        F("CEILING.MATH", "Math", "CEILING.MATH(number, [significance], [mode])", "Rounds up to a multiple.", 1, 3, f_ceiling_math, "v", 0),
        F("FLOOR", "Math", "FLOOR(number, [significance])", "Rounds down to a multiple of significance.", 1, 2, f_floor, "v", 0),
        F("FLOOR.MATH", "Math", "FLOOR.MATH(number, [significance], [mode])", "Rounds down to a multiple.", 1, 3, f_floor_math, "v", 0),
        F("INT", "Math", "INT(number)", "Rounds down to the nearest integer.", 1, 1, f_int, "v", 0),
        F("TRUNC", "Math", "TRUNC(number, [num_digits])", "Truncates to an integer / digits.", 1, 2, f_trunc, "v", 0),
        F("MOD", "Math", "MOD(number, divisor)", "Remainder with the sign of the divisor.", 2, 2, f_mod, "v", 0),
        F("ABS", "Math", "ABS(number)", "Absolute value.", 1, 1, f_abs, "v", 0),
        F("SIGN", "Math", "SIGN(number)", "Sign of a number (-1, 0, 1).", 1, 1, f_sign, "v", 0),
        F("SQRT", "Math", "SQRT(number)", "Square root.", 1, 1, f_sqrt, "v", 0),
        F("POWER", "Math", "POWER(number, power)", "Number raised to a power.", 2, 2, f_power, "v", 0),
        F("EXP", "Math", "EXP(number)", "e raised to a power.", 1, 1, f_exp, "v", 0),
        F("LN", "Math", "LN(number)", "Natural logarithm.", 1, 1, f_ln, "v", 0),
        F("LOG", "Math", "LOG(number, [base])", "Logarithm (default base 10).", 1, 2, f_log, "v", 0),
        F("LOG10", "Math", "LOG10(number)", "Base-10 logarithm.", 1, 1, f_log10, "v", 0),
        F("PI", "Math", "PI()", "The constant pi.", 0, 0, f_pi, "v", 0),
        F("RAND", "Math", "RAND()", "Random number 0..1 (volatile).", 0, 0, f_rand, "v", FN_VOLATILE),
        F("RANDBETWEEN", "Math", "RANDBETWEEN(bottom, top)", "Random integer between two numbers (volatile).", 2, 2, f_randbetween, "v", FN_VOLATILE),
        F("FACT", "Math", "FACT(number)", "Factorial.", 1, 1, f_fact, "v", 0),
        F("COMBIN", "Math", "COMBIN(number, number_chosen)", "Number of combinations.", 2, 2, f_combin, "v", 0),
        F("PERMUT", "Math", "PERMUT(number, number_chosen)", "Number of permutations.", 2, 2, f_permut, "v", 0),
        F("GCD", "Math", "GCD(number1, ...)", "Greatest common divisor.", 1, -1, f_gcd, "r", 0),
        F("LCM", "Math", "LCM(number1, ...)", "Least common multiple.", 1, -1, f_lcm, "r", 0),
        F("EVEN", "Math", "EVEN(number)", "Rounds away from zero to an even integer.", 1, 1, f_even, "v", 0),
        F("ODD", "Math", "ODD(number)", "Rounds away from zero to an odd integer.", 1, 1, f_odd, "v", 0),
        F("QUOTIENT", "Math", "QUOTIENT(numerator, denominator)", "Integer part of a division.", 2, 2, f_quotient, "v", 0),
        F("SIN", "Math", "SIN(number)", "Sine (radians).", 1, 1, f_sin, "v", 0),
        F("COS", "Math", "COS(number)", "Cosine (radians).", 1, 1, f_cos, "v", 0),
        F("TAN", "Math", "TAN(number)", "Tangent (radians).", 1, 1, f_tan, "v", 0),
        F("ASIN", "Math", "ASIN(number)", "Arcsine.", 1, 1, f_asin, "v", 0),
        F("ACOS", "Math", "ACOS(number)", "Arccosine.", 1, 1, f_acos, "v", 0),
        F("ATAN", "Math", "ATAN(number)", "Arctangent.", 1, 1, f_atan, "v", 0),
        F("ATAN2", "Math", "ATAN2(x_num, y_num)", "Arctangent of x,y coordinates.", 2, 2, f_atan2, "v", 0),
        F("SINH", "Math", "SINH(number)", "Hyperbolic sine.", 1, 1, f_sinh, "v", 0),
        F("COSH", "Math", "COSH(number)", "Hyperbolic cosine.", 1, 1, f_cosh, "v", 0),
        F("TANH", "Math", "TANH(number)", "Hyperbolic tangent.", 1, 1, f_tanh, "v", 0),
        F("DEGREES", "Math", "DEGREES(angle)", "Radians to degrees.", 1, 1, f_degrees, "v", 0),
        F("RADIANS", "Math", "RADIANS(angle)", "Degrees to radians.", 1, 1, f_radians, "v", 0),
        F("SUBTOTAL", "Math", "SUBTOTAL(function_num, ref1, ...)", "Subtotal ignoring filtered rows (101-111 also ignore hidden rows).", 2, -1, f_subtotal, "vr", 0),
        F("SEQUENCE", "Math", "SEQUENCE(rows, [columns], [start], [step])", "Array of sequential numbers.", 1, 4, f_sequence, "v", 0),
        // Statistical
        F("AVERAGE", "Statistical", "AVERAGE(number1, ...)", "Arithmetic mean.", 1, -1, f_average, "r", 0),
        F("AVERAGEA", "Statistical", "AVERAGEA(value1, ...)", "Mean counting text as 0 and TRUE as 1.", 1, -1, f_averagea, "r", 0),
        F("AVERAGEIF", "Statistical", "AVERAGEIF(range, criteria, [average_range])", "Mean of cells meeting a criterion.", 2, 3, f_averageif, "rvr", 0),
        F("AVERAGEIFS", "Statistical", "AVERAGEIFS(average_range, criteria_range1, criteria1, ...)", "Mean of cells meeting several criteria.", 3, -1, f_averageifs, "r", 0),
        F("COUNT", "Statistical", "COUNT(value1, ...)", "Counts numbers.", 1, -1, f_count, "r", 0),
        F("COUNTA", "Statistical", "COUNTA(value1, ...)", "Counts non-empty values.", 1, -1, f_counta, "r", 0),
        F("COUNTBLANK", "Statistical", "COUNTBLANK(range)", "Counts empty cells.", 1, 1, f_countblank, "r", 0),
        F("COUNTIF", "Statistical", "COUNTIF(range, criteria)", "Counts cells meeting a criterion.", 2, 2, f_countif, "rv", 0),
        F("COUNTIFS", "Statistical", "COUNTIFS(criteria_range1, criteria1, ...)", "Counts cells meeting several criteria.", 2, -1, f_countifs, "r", 0),
        F("MIN", "Statistical", "MIN(number1, ...)", "Smallest number.", 1, -1, f_min, "r", 0),
        F("MAX", "Statistical", "MAX(number1, ...)", "Largest number.", 1, -1, f_max, "r", 0),
        F("MINA", "Statistical", "MINA(value1, ...)", "Smallest value counting text/logicals.", 1, -1, f_mina, "r", 0),
        F("MAXA", "Statistical", "MAXA(value1, ...)", "Largest value counting text/logicals.", 1, -1, f_maxa, "r", 0),
        F("MINIFS", "Statistical", "MINIFS(min_range, criteria_range1, criteria1, ...)", "Minimum of cells meeting criteria.", 3, -1, f_minifs, "r", 0),
        F("MAXIFS", "Statistical", "MAXIFS(max_range, criteria_range1, criteria1, ...)", "Maximum of cells meeting criteria.", 3, -1, f_maxifs, "r", 0),
        F("MEDIAN", "Statistical", "MEDIAN(number1, ...)", "Median.", 1, -1, f_median, "r", 0),
        F("MODE", "Statistical", "MODE(number1, ...)", "Most frequent value.", 1, -1, f_mode, "r", 0),
        F("MODE.SNGL", "Statistical", "MODE.SNGL(number1, ...)", "Most frequent value.", 1, -1, f_mode, "r", 0),
        F("LARGE", "Statistical", "LARGE(array, k)", "k-th largest value.", 2, 2, f_large, "av", 0),
        F("SMALL", "Statistical", "SMALL(array, k)", "k-th smallest value.", 2, 2, f_small, "av", 0),
        F("RANK", "Statistical", "RANK(number, ref, [order])", "Rank of a number in a list.", 2, 3, f_rank, "vrv", 0),
        F("RANK.EQ", "Statistical", "RANK.EQ(number, ref, [order])", "Rank of a number in a list.", 2, 3, f_rank, "vrv", 0),
        F("STDEV", "Statistical", "STDEV(number1, ...)", "Sample standard deviation.", 1, -1, f_stdev, "r", 0),
        F("STDEV.S", "Statistical", "STDEV.S(number1, ...)", "Sample standard deviation.", 1, -1, f_stdev, "r", 0),
        F("STDEVP", "Statistical", "STDEVP(number1, ...)", "Population standard deviation.", 1, -1, f_stdevp, "r", 0),
        F("STDEV.P", "Statistical", "STDEV.P(number1, ...)", "Population standard deviation.", 1, -1, f_stdevp, "r", 0),
        F("VAR", "Statistical", "VAR(number1, ...)", "Sample variance.", 1, -1, f_var, "r", 0),
        F("VAR.S", "Statistical", "VAR.S(number1, ...)", "Sample variance.", 1, -1, f_var, "r", 0),
        F("VARP", "Statistical", "VARP(number1, ...)", "Population variance.", 1, -1, f_varp, "r", 0),
        F("VAR.P", "Statistical", "VAR.P(number1, ...)", "Population variance.", 1, -1, f_varp, "r", 0),
        F("PERCENTILE", "Statistical", "PERCENTILE(array, k)", "k-th percentile (inclusive).", 2, 2, f_percentile, "av", 0),
        F("PERCENTILE.INC", "Statistical", "PERCENTILE.INC(array, k)", "k-th percentile (inclusive).", 2, 2, f_percentile, "av", 0),
        F("PERCENTILE.EXC", "Statistical", "PERCENTILE.EXC(array, k)", "k-th percentile (exclusive).", 2, 2, f_percentile_exc, "av", 0),
        F("QUARTILE", "Statistical", "QUARTILE(array, quart)", "Quartile (inclusive).", 2, 2, f_quartile, "av", 0),
        F("QUARTILE.INC", "Statistical", "QUARTILE.INC(array, quart)", "Quartile (inclusive).", 2, 2, f_quartile, "av", 0),
        // Logical
        F("IF", "Logical", "IF(logical_test, [value_if_true], [value_if_false])", "Returns one value if true, another if false.", 1, 3, f_if, "vvv", 0),
        F("IFS", "Logical", "IFS(test1, value1, [test2, value2], ...)", "First value whose test is TRUE.", 2, -1, f_ifs, "v", 0),
        F("IFERROR", "Logical", "IFERROR(value, value_if_error)", "Alternative value on any error.", 2, 2, f_iferror, "vv", 0),
        F("IFNA", "Logical", "IFNA(value, value_if_na)", "Alternative value on #N/A.", 2, 2, f_ifna, "vv", 0),
        F("AND", "Logical", "AND(logical1, ...)", "TRUE if all arguments are TRUE.", 1, -1, f_and, "r", 0),
        F("OR", "Logical", "OR(logical1, ...)", "TRUE if any argument is TRUE.", 1, -1, f_or, "r", 0),
        F("XOR", "Logical", "XOR(logical1, ...)", "TRUE if an odd number of arguments are TRUE.", 1, -1, f_xor, "r", 0),
        F("NOT", "Logical", "NOT(logical)", "Reverses a logical value.", 1, 1, f_not, "v", 0),
        F("TRUE", "Logical", "TRUE()", "The logical value TRUE.", 0, 0, f_true, "v", 0),
        F("FALSE", "Logical", "FALSE()", "The logical value FALSE.", 0, 0, f_false, "v", 0),
        F("SWITCH", "Logical", "SWITCH(expression, value1, result1, ..., [default])", "Matches an expression against values.", 3, -1, f_switch, "vr", 0),
        // Lookup
        F("VLOOKUP", "Lookup", "VLOOKUP(lookup_value, table_array, col_index_num, [range_lookup])", "Looks down the first column and returns a value from another column.", 3, 4, f_vlookup, "vavv", 0),
        F("HLOOKUP", "Lookup", "HLOOKUP(lookup_value, table_array, row_index_num, [range_lookup])", "Looks across the first row and returns a value from another row.", 3, 4, f_hlookup, "vavv", 0),
        F("XLOOKUP", "Lookup", "XLOOKUP(lookup_value, lookup_array, return_array, [if_not_found], [match_mode], [search_mode])", "Searches a range and returns the matching item.", 3, 6, f_xlookup, "varrvv", 0),
        F("LOOKUP", "Lookup", "LOOKUP(lookup_value, lookup_vector, [result_vector])", "Approximate lookup in a sorted vector.", 2, 3, f_lookup, "var", 0),
        F("INDEX", "Lookup", "INDEX(array, row_num, [column_num], [area_num])", "Value or reference at a row/column position.", 1, 4, f_index, "rvvv", 0),
        F("MATCH", "Lookup", "MATCH(lookup_value, lookup_array, [match_type])", "Relative position of an item.", 2, 3, f_match, "vav", 0),
        F("XMATCH", "Lookup", "XMATCH(lookup_value, lookup_array, [match_mode], [search_mode])", "Relative position of an item.", 2, 4, f_xmatch, "vavv", 0),
        F("CHOOSE", "Lookup", "CHOOSE(index_num, value1, ...)", "Chooses a value from a list.", 2, -1, f_choose, "vr", 0),
        F("OFFSET", "Lookup", "OFFSET(reference, rows, cols, [height], [width])", "Reference offset from a starting reference (volatile).", 3, 5, f_offset, "rv", FN_DYNREF),
        F("INDIRECT", "Lookup", "INDIRECT(ref_text, [a1])", "Reference given by text (volatile).", 1, 2, f_indirect, "v", FN_DYNREF),
        F("ROW", "Lookup", "ROW([reference])", "Row number of a reference.", 0, 1, f_row, "r", 0),
        F("ROWS", "Lookup", "ROWS(array)", "Number of rows.", 1, 1, f_rows, "r", 0),
        F("COLUMN", "Lookup", "COLUMN([reference])", "Column number of a reference.", 0, 1, f_column, "r", 0),
        F("COLUMNS", "Lookup", "COLUMNS(array)", "Number of columns.", 1, 1, f_columns, "r", 0),
        F("ADDRESS", "Lookup", "ADDRESS(row_num, column_num, [abs_num], [a1], [sheet_text])", "Cell address as text.", 2, 5, f_address, "v", 0),
        F("TRANSPOSE", "Lookup", "TRANSPOSE(array)", "Swaps rows and columns.", 1, 1, f_transpose, "a", 0),
        F("UNIQUE", "Lookup", "UNIQUE(array, [by_col], [exactly_once])", "Distinct rows (or columns) of an array.", 1, 3, f_unique, "avv", 0),
        F("SORT", "Lookup", "SORT(array, [sort_index], [sort_order], [by_col])", "Sorts an array (text compares naturally: R2 < R10).", 1, 4, f_sort, "avvv", 0),
        F("SORTBY", "Lookup", "SORTBY(array, by_array1, [sort_order1], ...)", "Sorts an array by other arrays.", 2, -1, f_sortby, "a", 0),
        F("FILTER", "Lookup", "FILTER(array, include, [if_empty])", "Rows/columns where include is TRUE.", 2, 3, f_filter, "aar", 0),
        F("HYPERLINK", "Lookup", "HYPERLINK(link_location, [friendly_name])", "Link text (shown value).", 1, 2, f_hyperlink, "v", 0),
        // Text
        F("LEFT", "Text", "LEFT(text, [num_chars])", "Leftmost characters.", 1, 2, f_left, "v", 0),
        F("RIGHT", "Text", "RIGHT(text, [num_chars])", "Rightmost characters.", 1, 2, f_right, "v", 0),
        F("MID", "Text", "MID(text, start_num, num_chars)", "Characters from the middle.", 3, 3, f_mid, "v", 0),
        F("LEN", "Text", "LEN(text)", "Number of characters.", 1, 1, f_len, "v", 0),
        F("FIND", "Text", "FIND(find_text, within_text, [start_num])", "Case-sensitive position of text.", 2, 3, f_find, "v", 0),
        F("SEARCH", "Text", "SEARCH(find_text, within_text, [start_num])", "Case-insensitive position (wildcards * ?).", 2, 3, f_search, "v", 0),
        F("SUBSTITUTE", "Text", "SUBSTITUTE(text, old_text, new_text, [instance_num])", "Replaces occurrences of text.", 3, 4, f_substitute, "v", 0),
        F("REPLACE", "Text", "REPLACE(old_text, start_num, num_chars, new_text)", "Replaces characters by position.", 4, 4, f_replace, "v", 0),
        F("TRIM", "Text", "TRIM(text)", "Removes extra spaces.", 1, 1, f_trim, "v", 0),
        F("CLEAN", "Text", "CLEAN(text)", "Removes non-printable characters.", 1, 1, f_clean, "v", 0),
        F("UPPER", "Text", "UPPER(text)", "Converts to upper case.", 1, 1, f_upper, "v", 0),
        F("LOWER", "Text", "LOWER(text)", "Converts to lower case.", 1, 1, f_lower, "v", 0),
        F("PROPER", "Text", "PROPER(text)", "Capitalises each word.", 1, 1, f_proper, "v", 0),
        F("TEXT", "Text", "TEXT(value, format_text)", "Formats a number as text.", 2, 2, f_text, "v", 0),
        F("VALUE", "Text", "VALUE(text)", "Converts text to a number.", 1, 1, f_value, "v", 0),
        F("NUMBERVALUE", "Text", "NUMBERVALUE(text, [decimal_separator], [group_separator])", "Locale-independent text to number.", 1, 3, f_numbervalue, "v", 0),
        F("CONCAT", "Text", "CONCAT(text1, ...)", "Joins text (ranges allowed).", 1, -1, f_concat, "r", 0),
        F("CONCATENATE", "Text", "CONCATENATE(text1, ...)", "Joins text items.", 1, -1, f_concat, "v", 0),
        F("TEXTJOIN", "Text", "TEXTJOIN(delimiter, ignore_empty, text1, ...)", "Joins text with a delimiter.", 3, -1, f_textjoin, "vvr", 0),
        F("TEXTSPLIT", "Text", "TEXTSPLIT(text, col_delimiter, [row_delimiter], [ignore_empty], [match_mode], [pad_with])", "Splits text into an array.", 2, 6, f_textsplit, "vaavvv", 0),
        F("TEXTBEFORE", "Text", "TEXTBEFORE(text, delimiter, [instance_num])", "Text before a delimiter.", 2, 3, f_textbefore, "v", 0),
        F("TEXTAFTER", "Text", "TEXTAFTER(text, delimiter, [instance_num])", "Text after a delimiter.", 2, 3, f_textafter, "v", 0),
        F("EXACT", "Text", "EXACT(text1, text2)", "Case-sensitive equality.", 2, 2, f_exact, "v", 0),
        F("REPT", "Text", "REPT(text, number_times)", "Repeats text.", 2, 2, f_rept, "v", 0),
        F("CHAR", "Text", "CHAR(number)", "Character from a Windows-1252 code.", 1, 1, f_char, "v", 0),
        F("CODE", "Text", "CODE(text)", "Code of the first character.", 1, 1, f_code, "v", 0),
        F("UNICHAR", "Text", "UNICHAR(number)", "Unicode character.", 1, 1, f_unichar, "v", 0),
        F("UNICODE", "Text", "UNICODE(text)", "Code point of the first character.", 1, 1, f_unicode, "v", 0),
        F("FIXED", "Text", "FIXED(number, [decimals], [no_commas])", "Number as text with fixed decimals.", 1, 3, f_fixed, "v", 0),
        F("DOLLAR", "Text", "DOLLAR(number, [decimals])", "Number as currency text.", 1, 2, f_dollar, "v", 0),
        F("T", "Text", "T(value)", "Text if the value is text, else empty.", 1, 1, f_t, "v", 0),
        F("N", "Info", "N(value)", "Value converted to a number.", 1, 1, f_n, "v", 0),
        // Date
        F("TODAY", "Date", "TODAY()", "Current date (volatile).", 0, 0, f_today, "v", FN_VOLATILE),
        F("NOW", "Date", "NOW()", "Current date and time (volatile).", 0, 0, f_now, "v", FN_VOLATILE),
        F("DATE", "Date", "DATE(year, month, day)", "Serial number of a date.", 3, 3, f_date, "v", 0),
        F("TIME", "Date", "TIME(hour, minute, second)", "Serial number of a time.", 3, 3, f_time, "v", 0),
        F("YEAR", "Date", "YEAR(serial_number)", "Year of a date.", 1, 1, f_year, "v", 0),
        F("MONTH", "Date", "MONTH(serial_number)", "Month of a date.", 1, 1, f_month, "v", 0),
        F("DAY", "Date", "DAY(serial_number)", "Day of a date.", 1, 1, f_day, "v", 0),
        F("HOUR", "Date", "HOUR(serial_number)", "Hour of a time.", 1, 1, f_hour, "v", 0),
        F("MINUTE", "Date", "MINUTE(serial_number)", "Minute of a time.", 1, 1, f_minute, "v", 0),
        F("SECOND", "Date", "SECOND(serial_number)", "Second of a time.", 1, 1, f_second, "v", 0),
        F("WEEKDAY", "Date", "WEEKDAY(serial_number, [return_type])", "Day of the week.", 1, 2, f_weekday, "v", 0),
        F("WEEKNUM", "Date", "WEEKNUM(serial_number, [return_type])", "Week of the year.", 1, 2, f_weeknum, "v", 0),
        F("ISOWEEKNUM", "Date", "ISOWEEKNUM(date)", "ISO 8601 week number.", 1, 1, f_isoweeknum, "v", 0),
        F("DATEDIF", "Date", "DATEDIF(start_date, end_date, unit)", "Difference in days/months/years.", 3, 3, f_datedif, "v", 0),
        F("EDATE", "Date", "EDATE(start_date, months)", "Date months before/after.", 2, 2, f_edate, "v", 0),
        F("EOMONTH", "Date", "EOMONTH(start_date, months)", "Last day of the month months before/after.", 2, 2, f_eomonth, "v", 0),
        F("DATEVALUE", "Date", "DATEVALUE(date_text)", "Date text to serial number.", 1, 1, f_datevalue, "v", 0),
        F("TIMEVALUE", "Date", "TIMEVALUE(time_text)", "Time text to serial number.", 1, 1, f_timevalue, "v", 0),
        F("DAYS", "Date", "DAYS(end_date, start_date)", "Days between two dates.", 2, 2, f_days, "v", 0),
        F("NETWORKDAYS", "Date", "NETWORKDAYS(start_date, end_date, [holidays])", "Working days between two dates.", 2, 3, f_networkdays, "vvr", 0),
        F("WORKDAY", "Date", "WORKDAY(start_date, days, [holidays])", "Date after a number of working days.", 2, 3, f_workday, "vvr", 0),
        F("YEARFRAC", "Date", "YEARFRAC(start_date, end_date, [basis])", "Fraction of a year between dates.", 2, 3, f_yearfrac, "v", 0),
        // Info
        F("ISBLANK", "Info", "ISBLANK(value)", "TRUE if the cell is empty.", 1, 1, f_isblank, "v", 0),
        F("ISNUMBER", "Info", "ISNUMBER(value)", "TRUE for numbers.", 1, 1, f_isnumber, "v", 0),
        F("ISTEXT", "Info", "ISTEXT(value)", "TRUE for text.", 1, 1, f_istext, "v", 0),
        F("ISNONTEXT", "Info", "ISNONTEXT(value)", "TRUE for anything but text.", 1, 1, f_isnontext, "v", 0),
        F("ISLOGICAL", "Info", "ISLOGICAL(value)", "TRUE for logical values.", 1, 1, f_islogical, "v", 0),
        F("ISERROR", "Info", "ISERROR(value)", "TRUE for any error.", 1, 1, f_iserror, "v", 0),
        F("ISERR", "Info", "ISERR(value)", "TRUE for errors other than #N/A.", 1, 1, f_iserr, "v", 0),
        F("ISNA", "Info", "ISNA(value)", "TRUE for #N/A.", 1, 1, f_isna, "v", 0),
        F("ISREF", "Info", "ISREF(value)", "TRUE for references.", 1, 1, f_isref, "r", 0),
        F("ISEVEN", "Info", "ISEVEN(number)", "TRUE for even numbers.", 1, 1, f_iseven, "v", 0),
        F("ISODD", "Info", "ISODD(number)", "TRUE for odd numbers.", 1, 1, f_isodd, "v", 0),
        F("ISFORMULA", "Info", "ISFORMULA(reference)", "TRUE if the cell contains a formula.", 1, 1, f_isformula, "r", 0),
        F("FORMULATEXT", "Info", "FORMULATEXT(reference)", "Formula of a cell as text.", 1, 1, f_formulatext, "r", 0),
        F("NA", "Info", "NA()", "The #N/A error.", 0, 0, f_na, "v", 0),
        F("ERROR.TYPE", "Info", "ERROR.TYPE(error_val)", "Number of an error type.", 1, 1, f_errortype, "v", 0),
        F("TYPE", "Info", "TYPE(value)", "Type code of a value (1 num, 2 text, 4 bool, 16 error, 64 array).", 1, 1, f_type, "r", 0),
        F("SHEET", "Info", "SHEET([value])", "Sheet number.", 0, 1, f_sheet, "r", 0),
        F("SHEETS", "Info", "SHEETS([reference])", "Number of sheets.", 0, 1, f_sheets, "r", 0),
        // BOM helpers
        F("REFCOUNT", "BOM", "REFCOUNT(designators, ...)", "Counts reference designators (\"R1-R5,R7\" = 6).", 1, -1, f_refcount, "r", 0),
        F("REFEXPAND", "BOM", "REFEXPAND(designators, [separator])", "Expands ranges (\"R1-R3\" -> \"R1, R2, R3\").", 1, 2, f_refexpand, "v", 0),
        F("REFCOMPRESS", "BOM", "REFCOMPRESS(designators, [separator])", "Compresses runs (\"R1, R2, R3\" -> \"R1-R3\").", 1, 2, f_refcompress, "v", 0),
    };
    return t;
}
#undef F

int fn_lookup(std::string_view name) {
    static const std::unordered_map<std::string, int> idx = [] {
        std::unordered_map<std::string, int> m;
        const auto& t = fn_table();
        for (size_t i = 0; i < t.size(); ++i) m.emplace(t[i].info.name, (int)i);
        return m;
    }();
    std::string u = upper_ascii(std::string(name));
    auto it = idx.find(u);
    return it == idx.end() ? -1 : it->second;
}

const std::vector<FunctionInfo>& function_catalog() {
    static const std::vector<FunctionInfo> v = [] {
        std::vector<FunctionInfo> o;
        for (auto& d : fn_table()) o.push_back(d.info);
        std::sort(o.begin(), o.end(), [](const FunctionInfo& a, const FunctionInfo& b) { return strcmp(a.name, b.name) < 0; });
        return o;
    }();
    return v;
}
const FunctionInfo* find_function(std::string_view name) {
    std::string u = upper_ascii(std::string(name));
    for (const char* pre : {"_XLFN._XLWS.", "_XLFN.", "_XLWS."}) if (u.rfind(pre, 0) == 0) { u = u.substr(strlen(pre)); break; }
    int i = fn_lookup(u);
    return i < 0 ? nullptr : &fn_table()[i].info;
}

} // namespace pcbp::sheet
