// ============================================================================
// formula_eval.cpp — RPN evaluator, value coercion rules, dependency graph and
// the recalculation scheduler (dirty propagation + demand-driven evaluation
// with an explicit work stack, circular-reference detection, dynamic-array
// spilling). Behaviour per the Excel / OpenFormula documentation: see
// specs/SHEET_ENGINE.md §Calculation.
// ============================================================================
#include "formula_internal.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cmath>

namespace pcbp::sheet {

// ------------------------------------------------------------- coercions --
bool to_number(const Value& v, double& d, Err& e) {
    switch (v.type) {
    case Value::Empty: d = 0; return true;
    case Value::Number: case Value::Bool: d = v.num; return true;
    case Value::Error: e = v.err; return false;
    case Value::String:
        if (v.str.empty()) { e = Err::Value; return false; }
        if (text_to_number(v.str, d)) return true;
        e = Err::Value; return false;
    }
    e = Err::Value;
    return false;
}
bool to_bool(const Value& v, bool& b, Err& e) {
    switch (v.type) {
    case Value::Empty: b = false; return true;
    case Value::Number: case Value::Bool: b = v.num != 0; return true;
    case Value::Error: e = v.err; return false;
    case Value::String: {
        std::string u = upper_ascii(v.str);
        if (u == "TRUE") { b = true; return true; }
        if (u == "FALSE") { b = false; return true; }
        e = Err::Value; return false;
    }
    }
    return false;
}
std::string to_text(const Value& v) { return value_to_text(v); }

int compare_values(const Value& a0, const Value& b0, bool cs) {
    Value a = a0, b = b0;
    // empty adopts the other side's type
    if (a.is_empty() && b.is_empty()) return 0;
    if (a.is_empty()) { if (b.is_num()) a = Value::number(0); else if (b.is_str()) a = Value::text(""); else if (b.is_bool()) a = Value::boolean(false); }
    if (b.is_empty()) { if (a.is_num()) b = Value::number(0); else if (a.is_str()) b = Value::text(""); else if (a.is_bool()) b = Value::boolean(false); }
    auto rank = [](const Value& v) { return v.is_num() ? 0 : v.is_str() ? 1 : v.is_bool() ? 2 : 3; };
    int ra = rank(a), rb = rank(b);
    if (ra != rb) return ra < rb ? -1 : 1;
    if (ra == 0) {
        if (a.num == b.num) return 0;
        double m = std::max(std::fabs(a.num), std::fabs(b.num));
        if (std::fabs(a.num - b.num) <= m * 1e-15) return 0;   // Excel compares to 15 significant digits
        return a.num < b.num ? -1 : 1;
    }
    if (ra == 2) return a.num < b.num ? -1 : a.num > b.num ? 1 : 0;
    if (ra == 1) { int c = cs ? a.str.compare(b.str) : ci_compare(a.str, b.str); return c < 0 ? -1 : c > 0 ? 1 : 0; }
    return (int)a.err - (int)b.err;
}

static Value arith(Tok::OpCode op, const Value& a, const Value& b) {
    if (op == Tok::Concat) {
        if (a.is_err()) return a;
        if (b.is_err()) return b;
        return Value::text(to_text(a) + to_text(b));
    }
    if (op >= Tok::Eq && op <= Tok::Ge) {
        if (a.is_err()) return a;
        if (b.is_err()) return b;
        int c = compare_values(a, b);
        bool r = false;
        switch (op) {
        case Tok::Eq: r = c == 0; break; case Tok::Ne: r = c != 0; break; case Tok::Lt: r = c < 0; break;
        case Tok::Le: r = c <= 0; break; case Tok::Gt: r = c > 0; break; case Tok::Ge: r = c >= 0; break;
        default: break;
        }
        return Value::boolean(r);
    }
    double x, y; Err e = Err::Value;
    if (!to_number(a, x, e)) return Value::error(e);
    if (!to_number(b, y, e)) return Value::error(e);
    double r = 0;
    switch (op) {
    case Tok::Add: r = x + y; break;
    case Tok::Sub: r = x - y; break;
    case Tok::Mul: r = x * y; break;
    case Tok::Div: if (y == 0) return Value::error(Err::Div0); r = x / y; break;
    case Tok::Pow:
        if (x == 0 && y == 0) return Value::error(Err::Num);
        if (x == 0 && y < 0) return Value::error(Err::Div0);
        if (x < 0 && y != std::floor(y)) {
            // odd-root of negatives (1/3) allowed by Excel when 1/y is an odd integer
            double inv = 1 / y, ri = std::round(inv);
            if (std::fabs(inv - ri) < 1e-12 && ((long long)ri % 2 != 0)) { r = -std::pow(-x, y); break; }
            return Value::error(Err::Num);
        }
        r = std::pow(x, y); break;
    default: return Value::error(Err::Value);
    }
    if (!std::isfinite(r)) return Value::error(Err::Num);
    return Value::number(r);
}

Opnd apply_binary(Evaluator& ev, Tok::OpCode op, const Opnd& a, const Opnd& b) {
    auto single = [](const Opnd& o) {
        return o.k == Opnd::V || (o.k == Opnd::R && o.ref.rg.r0 == o.ref.rg.r1 && o.ref.rg.c0 == o.ref.rg.c1) || (o.k == Opnd::A && o.arr->rows == 1 && o.arr->cols == 1);
    };
    if (a.k == Opnd::RL || b.k == Opnd::RL) return Opnd::err(Err::Value);
    if (single(a) && single(b)) return Opnd::val(arith(op, ev.scalar(a), ev.scalar(b)));
    auto A = ev.to_array(a), B = ev.to_array(b);
    int R = std::max(A->rows, B->rows), C = std::max(A->cols, B->cols);
    auto out = std::make_shared<Array>(R, C);
    for (int i = 0; i < R; ++i)
        for (int j = 0; j < C; ++j) {
            auto pick = [&](const Array& X, int ii, int jj, bool& ok) -> const Value& {
                int ri = X.rows == 1 ? 0 : ii, cj = X.cols == 1 ? 0 : jj;
                ok = ri < X.rows && cj < X.cols;
                static const Value e;
                return ok ? X.at(ri, cj) : e;
            };
            bool oa, ob;
            const Value& va = pick(*A, i, j, oa);
            const Value& vb = pick(*B, i, j, ob);
            out->at(i, j) = (oa && ob) ? arith(op, va, vb) : Value::error(Err::NA);
        }
    return Opnd::array(out);
}

// ------------------------------------------------------------- evaluator --
const Value& Evaluator::cell_value(int s, int rr, int cc) {
    static const Value empty;
    const Worksheet& ws = book.sheets[s];
    if (rr < (int)ws.rows.size()) {
        const auto& row = ws.rows[rr].cells;
        if (cc < (int)row.size()) {
            const Cell& cl = row[cc];
            if (cl.f) {
                if ((cl.calc & CS_DIRTY) && !probe) throw NeedCell{s, rr, cc};
                return cl.v;
            }
            if (cl.v.type != Value::Empty) return cl.v;
        }
    }
    if (!ws.spills.empty()) {
        for (auto& [k, arr] : ws.spills) {
            int ar = k.first, ac = k.second;
            if (rr >= ar && cc >= ac && rr < ar + arr->rows && cc < ac + arr->cols) {
                const Cell* an = ws.cell(ar, ac);
                if (an && an->f && (an->calc & CS_DIRTY) && !probe) throw NeedCell{s, ar, ac};
                return arr->at(rr - ar, cc - ac);
            }
        }
    }
    return empty;
}

Range Evaluator::clip_used(int s, const Range& rg) const {
    const Worksheet& ws = book.sheets[s];
    Range o = rg;
    int nr = (int)ws.rows.size();
    for (auto& [k, arr] : ws.spills) nr = std::max(nr, k.first + arr->rows);
    if (o.r1 >= nr) o.r1 = nr - 1;
    if (o.c1 >= MAX_COLS - 1 || o.c1 - o.c0 > 1024) {   // only whole-row / very wide ranges need the column extent
        int nc = ws.ncols();
        for (auto& [k, arr] : ws.spills) nc = std::max(nc, k.second + arr->cols);
        if (o.c1 >= nc) o.c1 = nc - 1;
    }
    return o;
}

void Evaluator::prefetch(int s, const Range& rg0) {
    if (probe) return;
    Range rg = clip_used(s, rg0);
    if (rg.rows() * (long long)rg.cols() <= 1) return;
    const Worksheet& ws = book.sheets[s];
    NeedCells n;
    for (int r = std::max(0, rg.r0); r <= rg.r1 && r < (int)ws.rows.size(); ++r) {
        const auto& cells = ws.rows[r].cells;
        for (int c = std::max(0, rg.c0); c <= rg.c1 && c < (int)cells.size(); ++c)
            if (cells[c].f && (cells[c].calc & CS_DIRTY)) n.cells.push_back({s, r, c});
    }
    if (!n.cells.empty()) throw n;
}

Value Evaluator::scalar(const Opnd& o) {
    switch (o.k) {
    case Opnd::V: return o.v;
    case Opnd::A: return o.arr->rows && o.arr->cols ? o.arr->at(0, 0) : Value::error(Err::Value);
    case Opnd::R: {
        const Range& g = o.ref.rg;
        if (g.r0 == g.r1 && g.c0 == g.c1) return cell_value(o.ref.si, g.r0, g.c0);
        // implicit intersection
        if (o.ref.si == si) {
            if (g.c0 == g.c1 && r >= g.r0 && r <= g.r1) return cell_value(o.ref.si, r, g.c0);
            if (g.r0 == g.r1 && c >= g.c0 && c <= g.c1) return cell_value(o.ref.si, g.r0, c);
        }
        return Value::error(Err::Value);
    }
    case Opnd::RL: return Value::error(Err::Value);
    }
    return Value();
}

std::shared_ptr<const Array> Evaluator::to_array(const Opnd& o) {
    if (o.k == Opnd::A) return o.arr;
    if (o.k == Opnd::V) { auto a = std::make_shared<Array>(1, 1); a->at(0, 0) = o.v; return a; }
    if (o.k == Opnd::RL) { auto a = std::make_shared<Array>(1, 1); a->at(0, 0) = Value::error(Err::Value); return a; }
    Range g = o.ref.rg;
    bool whole = g.r1 >= MAX_ROWS - 1 || g.c1 >= MAX_COLS - 1;
    if (whole) { g = clip_used(o.ref.si, g); if (g.r1 < g.r0 || g.c1 < g.c0) return std::make_shared<Array>(1, 1); }
    if ((long long)g.rows() * g.cols() > 50000000LL) { auto a = std::make_shared<Array>(1, 1); a->at(0, 0) = Value::error(Err::Num); return a; }
    auto a = std::make_shared<Array>(g.rows(), g.cols());
    Range u = clip_used(o.ref.si, g);
    prefetch(o.ref.si, u);
    for (int i = g.r0; i <= std::min(g.r1, u.r1); ++i)
        for (int j = g.c0; j <= std::min(g.c1, u.c1); ++j) a->at(i - g.r0, j - g.c0) = cell_value(o.ref.si, i, j);
    return a;
}

Opnd Evaluator::ref_opnd(const Tok& k) {
    int s0 = k.sheet_id < 0 ? si : book.sheet_index_of_id((uint32_t)k.sheet_id);
    if (s0 < 0) return Opnd::err(Err::Ref);
    int r0 = abs_row(k.a, r), c0 = abs_col(k.a, c);
    int r1 = r0, c1 = c0;
    if (k.kind == Tok::Area) { r1 = abs_row(k.b, r); c1 = abs_col(k.b, c); }
    if (r0 < 0 || c0 < 0 || r1 < 0 || c1 < 0 || r0 >= MAX_ROWS || r1 >= MAX_ROWS || c0 >= MAX_COLS || c1 >= MAX_COLS) return Opnd::err(Err::Ref);
    Range rg = Range(r0, c0, r1, c1).norm();
    if (k.sheet_id2 >= 0) {
        int s1 = book.sheet_index_of_id((uint32_t)k.sheet_id2);
        if (s1 < 0) return Opnd::err(Err::Ref);
        if (s1 < s0) std::swap(s0, s1);
        Opnd o; o.k = Opnd::RL;
        for (int s = s0; s <= s1; ++s) o.refs.push_back({s, rg});
        return o;
    }
    return Opnd::area(s0, rg);
}

bool Evaluator::resolve_name(const std::string& name, Opnd& out) {
    const DefinedName* dn = book.find_name(name, si);
    if (!dn) return false;
    if (depth > 32) { out = Opnd::err(Err::Ref); return true; }
    auto& cache = book.calc().name_cache;
    std::string key = std::to_string(dn->scope) + "|" + upper_ascii(dn->name);
    auto it = cache.find(key);
    std::shared_ptr<Formula> f;
    if (it == cache.end()) {
        f = std::make_shared<Formula>();
        std::string err;
        int host = dn->scope >= 0 ? dn->scope : si;
        if (!compile_formula(dn->formula, book, host, 0, 0, *f, err)) f->code.clear();
        cache[key] = f;
    } else f = it->second;
    if (f->code.empty()) { out = Opnd::err(Err::Name); return true; }
    Evaluator sub(book, dn->scope >= 0 ? dn->scope : si, 0, 0);
    sub.probe = probe;
    sub.depth = depth + 1;
    out = sub.run(*f);
    return true;
}

bool Evaluator::parse_ref_text(const std::string& text, bool a1, Opnd& out) {
    std::string t = text;
    while (!t.empty() && t.front() == ' ') t.erase(0, 1);
    while (!t.empty() && t.back() == ' ') t.pop_back();
    if (t.empty()) return false;
    if (!a1) {
        // R1C1 absolute only: R2C3 or R2C3:R4C5 (optionally Sheet!)
        std::string sheetp;
        size_t bang = t.rfind('!');
        if (bang != std::string::npos) { sheetp = t.substr(0, bang + 1); t = t.substr(bang + 1); }
        auto conv = [](const std::string& s, std::string& o) {
            int rr = 0, cc = 0;
            if (s.size() < 4 || toupper((unsigned char)s[0]) != 'R') return false;
            size_t k = 1; while (k < s.size() && isdigit((unsigned char)s[k])) ++k;
            if (k == 1 || k >= s.size() || toupper((unsigned char)s[k]) != 'C') return false;
            rr = atoi(s.substr(1, k - 1).c_str());
            size_t q = k + 1; while (q < s.size() && isdigit((unsigned char)s[q])) ++q;
            if (q != s.size() || q == k + 1) return false;
            cc = atoi(s.substr(k + 1).c_str());
            if (rr < 1 || cc < 1) return false;
            o = cell_name(rr - 1, cc - 1);
            return true;
        };
        size_t colon = t.find(':');
        std::string x, y;
        if (colon == std::string::npos) { if (!conv(t, x)) return false; t = sheetp + x; }
        else { if (!conv(t.substr(0, colon), x) || !conv(t.substr(colon + 1), y)) return false; t = sheetp + x + ":" + y; }
    }
    Formula f; std::string err;
    if (!compile_formula(t, book, si, r, c, f, err)) {
        // maybe a defined name
        Opnd n;
        if (resolve_name(t, n) && n.is_ref()) { out = n; return true; }
        return false;
    }
    if (f.code.size() == 1 && (f.code[0].kind == Tok::Ref || f.code[0].kind == Tok::Area)) { out = ref_opnd(f.code[0]); return !out.is_err(); }
    if (f.code.size() == 1 && f.code[0].kind == Tok::Name) { Opnd n; if (resolve_name(f.code[0].str, n) && n.is_ref()) { out = n; return true; } }
    return false;
}

Opnd Evaluator::call(const Tok& k, std::vector<Opnd>& args) {
    if (k.fn == 0xFFFF) return Opnd::err(Err::Name);
    const FnDef& d = fn_table()[k.fn];
    size_t nspec = strlen(d.args);
    bool lift = false;
    int R = 1, C = 1;
    for (size_t i = 0; i < args.size(); ++i) {
        char kind = nspec ? d.args[std::min(i, nspec - 1)] : 'v';
        if (kind != 'v') continue;
        Opnd& a = args[i];
        if (a.missing) continue;
        if (a.k == Opnd::R) {
            const Range& g = a.ref.rg;
            if (g.r0 == g.r1 && g.c0 == g.c1) { a = Opnd::val(cell_value(a.ref.si, g.r0, g.c0)); continue; }
            lift = true;
            Range u = (g.r1 >= MAX_ROWS - 1 || g.c1 >= MAX_COLS - 1) ? clip_used(a.ref.si, g) : g;
            R = std::max(R, std::max(1, u.rows())); C = std::max(C, std::max(1, u.cols()));
        } else if (a.k == Opnd::A) {
            if (a.arr->rows == 1 && a.arr->cols == 1) { a = Opnd::val(a.arr->at(0, 0)); continue; }
            lift = true;
            R = std::max(R, a.arr->rows); C = std::max(C, a.arr->cols);
        } else if (a.k == Opnd::RL) a = Opnd::err(Err::Value);
    }
    if (!lift) return d.impl(*this, args);
    std::vector<std::shared_ptr<const Array>> arrs(args.size());
    for (size_t i = 0; i < args.size(); ++i) {
        char kind = nspec ? d.args[std::min(i, nspec - 1)] : 'v';
        if (kind == 'v' && !args[i].missing && (args[i].k == Opnd::R || args[i].k == Opnd::A)) arrs[i] = to_array(args[i]);
    }
    auto out = std::make_shared<Array>(R, C);
    std::vector<Opnd> one(args.size());
    for (int i = 0; i < R; ++i)
        for (int j = 0; j < C; ++j) {
            for (size_t q = 0; q < args.size(); ++q) {
                if (!arrs[q]) { one[q] = args[q]; continue; }
                const Array& X = *arrs[q];
                int ri = X.rows == 1 ? 0 : i, cj = X.cols == 1 ? 0 : j;
                one[q] = (ri < X.rows && cj < X.cols) ? Opnd::val(X.at(ri, cj)) : Opnd::err(Err::NA);
            }
            Opnd res = d.impl(*this, one);
            out->at(i, j) = scalar(res);
        }
    return Opnd::array(out);
}

Opnd Evaluator::run(const Formula& f) {
    if (f.code.empty()) return Opnd::err(f.bad_text.empty() ? Err::Value : Err::Name);
    std::vector<Opnd> st;
    st.reserve(8);
    std::vector<Opnd> args;
    for (const Tok& k : f.code) {
        switch (k.kind) {
        case Tok::Num: st.push_back(Opnd::num(k.num)); break;
        case Tok::Str: st.push_back(Opnd::str(k.str)); break;
        case Tok::Bool: st.push_back(Opnd::boolean(k.num != 0)); break;
        case Tok::ErrLit: st.push_back(Opnd::err((Err)(int)k.num)); break;
        case Tok::BadRef: st.push_back(Opnd::err(Err::Ref)); break;
        case Tok::Ref: case Tok::Area: st.push_back(ref_opnd(k)); break;
        case Tok::ArrayConst: st.push_back(Opnd::array(k.arr)); break;
        case Tok::Name: {
            Opnd o;
            if (!resolve_name(k.str, o)) o = Opnd::err(Err::Name);
            st.push_back(std::move(o));
            break;
        }
        case Tok::Missing: { Opnd o; o.missing = true; st.push_back(o); break; }
        case Tok::Paren: break;
        case Tok::Func: {
            size_t n = k.argc;
            if (st.size() < n) return Opnd::err(Err::Value);
            args.assign(std::make_move_iterator(st.end() - n), std::make_move_iterator(st.end()));
            st.resize(st.size() - n);
            st.push_back(call(k, args));
            break;
        }
        case Tok::Op: {
            if (k.op == Tok::Neg || k.op == Tok::Plus || k.op == Tok::Percent) {
                if (st.empty()) return Opnd::err(Err::Value);
                Opnd a = std::move(st.back()); st.pop_back();
                if (k.op == Tok::Plus) { st.push_back(std::move(a)); break; }
                Opnd z = Opnd::num(k.op == Tok::Neg ? -1 : 0.01);
                // -x == x * -1 ; x% == x * 0.01 (keeps array semantics)
                st.push_back(apply_binary(*this, Tok::Mul, a, z));
                break;
            }
            if (k.op == Tok::SpillRef) {
                if (st.empty()) return Opnd::err(Err::Value);
                Opnd a = std::move(st.back()); st.pop_back();
                if (a.k != Opnd::R) { st.push_back(Opnd::err(Err::Ref)); break; }
                const Worksheet& ws = book.sheets[a.ref.si];
                int ar = a.ref.rg.r0, ac = a.ref.rg.c0;
                const Cell* an = ws.cell(ar, ac);
                if (an && an->f && (an->calc & CS_DIRTY) && !probe) throw NeedCell{a.ref.si, ar, ac};
                auto it = ws.spills.find({ar, ac});
                if (it != ws.spills.end()) st.push_back(Opnd::area(a.ref.si, Range(ar, ac, ar + it->second->rows - 1, ac + it->second->cols - 1)));
                else if (an && an->f) st.push_back(Opnd::area(a.ref.si, Range(ar, ac, ar, ac)));
                else st.push_back(Opnd::err(Err::Ref));
                break;
            }
            if (st.size() < 2) return Opnd::err(Err::Value);
            Opnd b = std::move(st.back()); st.pop_back();
            Opnd a = std::move(st.back()); st.pop_back();
            if (k.op == Tok::RangeOp) {
                if (a.k != Opnd::R || b.k != Opnd::R || a.ref.si != b.ref.si) { st.push_back(a.is_err() ? a : b.is_err() ? b : Opnd::err(Err::Value)); break; }
                Range g(std::min(a.ref.rg.r0, b.ref.rg.r0), std::min(a.ref.rg.c0, b.ref.rg.c0), std::max(a.ref.rg.r1, b.ref.rg.r1), std::max(a.ref.rg.c1, b.ref.rg.c1));
                st.push_back(Opnd::area(a.ref.si, g));
                break;
            }
            if (k.op == Tok::Union) {
                if (!a.is_ref() || !b.is_ref()) { st.push_back(Opnd::err(Err::Value)); break; }
                Opnd o; o.k = Opnd::RL;
                if (a.k == Opnd::R) o.refs.push_back(a.ref); else o.refs = a.refs;
                if (b.k == Opnd::R) o.refs.push_back(b.ref); else o.refs.insert(o.refs.end(), b.refs.begin(), b.refs.end());
                st.push_back(std::move(o));
                break;
            }
            if (k.op == Tok::Isect) {
                if (a.k != Opnd::R || b.k != Opnd::R) { st.push_back(Opnd::err(a.is_err() ? a.v.err : b.is_err() ? b.v.err : Err::Value)); break; }
                if (a.ref.si != b.ref.si || !a.ref.rg.intersects(b.ref.rg)) { st.push_back(Opnd::err(Err::Null)); break; }
                Range g(std::max(a.ref.rg.r0, b.ref.rg.r0), std::max(a.ref.rg.c0, b.ref.rg.c0), std::min(a.ref.rg.r1, b.ref.rg.r1), std::min(a.ref.rg.c1, b.ref.rg.c1));
                st.push_back(Opnd::area(a.ref.si, g));
                break;
            }
            st.push_back(apply_binary(*this, k.op, a, b));
            break;
        }
        }
    }
    if (st.empty()) return Opnd::err(Err::Value);
    return std::move(st.back());
}

// Convert a formula result to the value stored in the cell (+ spill array).
Value opnd_to_cell_value(Evaluator& ev, const Opnd& o, std::shared_ptr<const Array>* arr_out) {
    if (arr_out) arr_out->reset();
    auto fix_empty = [](Value v) { if (v.is_empty()) return Value::number(0); return v; };
    if (o.k == Opnd::V) return fix_empty(o.v);
    if (o.k == Opnd::RL) return Value::error(Err::Value);
    if (o.k == Opnd::R) {
        const Range& g = o.ref.rg;
        if (g.r0 == g.r1 && g.c0 == g.c1) return fix_empty(ev.cell_value(o.ref.si, g.r0, g.c0));
    }
    auto a = ev.to_array(o);
    if (a->rows == 0 || a->cols == 0) return Value::error(Err::Calc);
    if (a->rows == 1 && a->cols == 1) return fix_empty(a->at(0, 0));
    if (arr_out) {
        auto c = std::make_shared<Array>(*a);
        for (auto& v : c->v) if (v.is_empty()) v = Value::number(0);
        *arr_out = c;
    }
    return fix_empty(a->at(0, 0));
}

Opnd eval_formula_cell(const Book& b, int si, int r, int c, const Formula& f, bool probe) {
    Evaluator ev(b, si, r, c);
    ev.probe = probe;
    return ev.run(f);
}

// ------------------------------------------------------------ dep graph --
static inline bool small_area(const Area& a) { return (long long)a.rg.rows() * a.rg.cols() <= 16; }

void graph_add(Book& b, int si, int r, int c, const Formula& f) {
    auto& C = b.calc();
    CellKey me{si, r, c};
    if (f.is_volatile || f.dynamic_refs) C.volatiles.push_back(me);
    std::vector<Area> pre; bool names = false;
    formula_precedents(b, f, si, r, c, pre, names);
    if (names) C.name_users.push_back(me);
    for (auto& a : pre) {
        if (small_area(a)) {
            for (int i = a.rg.r0; i <= a.rg.r1; ++i) for (int j = a.rg.c0; j <= a.rg.c1; ++j) C.cell_deps[{a.si, i, j}].push_back(me);
        } else C.range_deps.push_back({a, me});
    }
}
void graph_remove(Book& b, int si, int r, int c, const Formula& f) {
    auto& C = b.calc();
    CellKey me{si, r, c};
    auto erase_key = [&](std::vector<CellKey>& v) { v.erase(std::remove(v.begin(), v.end(), me), v.end()); };
    if (f.is_volatile || f.dynamic_refs) erase_key(C.volatiles);
    std::vector<Area> pre; bool names = false;
    formula_precedents(b, f, si, r, c, pre, names);
    if (names) erase_key(C.name_users);
    bool any_range = false;
    for (auto& a : pre) {
        if (small_area(a)) {
            for (int i = a.rg.r0; i <= a.rg.r1; ++i) for (int j = a.rg.c0; j <= a.rg.c1; ++j) {
                auto it = C.cell_deps.find({a.si, i, j});
                if (it != C.cell_deps.end()) { erase_key(it->second); if (it->second.empty()) C.cell_deps.erase(it); }
            }
        } else any_range = true;
    }
    if (any_range) C.range_deps.erase(std::remove_if(C.range_deps.begin(), C.range_deps.end(), [&](const Book::Calc::RangeDep& d) { return d.dep == me; }), C.range_deps.end());
}

static void build_graph(Book& b) {
    auto& C = b.calc();
    C.cell_deps.clear(); C.range_deps.clear(); C.volatiles.clear(); C.name_users.clear();
    for (int s = 0; s < b.sheet_count(); ++s) {
        const Worksheet& ws = b.sheets[s];
        for (int r = 0; r < (int)ws.rows.size(); ++r) {
            const auto& cells = ws.rows[r].cells;
            for (int c = 0; c < (int)cells.size(); ++c) if (cells[c].f) graph_add(b, s, r, c, *cells[c].f);
        }
    }
    C.graph_valid = true;
}

// ------------------------------------------------------------- scheduler --
namespace {

struct Sched {
    Book& b;
    int evaluated = 0, circular = 0;
    struct SpillChange { CellKey k; int rows, cols; };
    std::vector<SpillChange> spill_changed;   // anchors whose spill rect/content changed (max extent)
    explicit Sched(Book& bk) : b(bk) {}

    Cell* cellp(const CellKey& k) {
        Worksheet& ws = b.sheets[k.si];
        if (k.r >= (int)ws.rows.size() || k.c >= (int)ws.rows[k.r].cells.size()) return nullptr;
        return &ws.rows[k.r].cells[k.c];
    }

    void store(const CellKey& k, Cell& cl, Value v, std::shared_ptr<const Array> arr) {
        Worksheet& ws = b.sheets[k.si];
        auto it = ws.spills.find({k.r, k.c});
        std::shared_ptr<const Array> old = it != ws.spills.end() ? it->second : nullptr;
        if (it != ws.spills.end()) ws.spills.erase(it);
        if (arr) {
            // check the spill area is free
            bool blocked = k.r + arr->rows > MAX_ROWS || k.c + arr->cols > MAX_COLS;
            for (int i = 0; i < arr->rows && !blocked; ++i)
                for (int j = 0; j < arr->cols && !blocked; ++j) {
                    if (!i && !j) continue;
                    const Cell* o = ws.cell(k.r + i, k.c + j);
                    if (o && !o->blank()) blocked = true;
                }
            if (!blocked) {
                Range me(k.r, k.c, k.r + arr->rows - 1, k.c + arr->cols - 1);
                for (auto& [ak, aa] : ws.spills) {
                    Range other(ak.first, ak.second, ak.first + aa->rows - 1, ak.second + aa->cols - 1);
                    if (other.intersects(me)) { blocked = true; break; }
                }
                for (auto& m : ws.merges) if (m.intersects(me)) { blocked = true; break; }
            }
            if (blocked) { v = Value::error(Err::Spill); arr.reset(); }
            else ws.spills[{k.r, k.c}] = arr;
        }
        bool changed_spill = (bool)old != (bool)arr || (old && arr && (old->rows != arr->rows || old->cols != arr->cols || old->v != arr->v));
        if (changed_spill) spill_changed.push_back({k, std::max(old ? old->rows : 1, arr ? arr->rows : 1), std::max(old ? old->cols : 1, arr ? arr->cols : 1)});
        cl.v = std::move(v);
    }

    void throw_cycle(std::vector<CellKey>& stack, const CellKey& nk) {
        // cycle: everything from nk up to the top of the stack
        size_t idx = stack.size();
        for (size_t q = 0; q < stack.size(); ++q) if (stack[q] == nk) { idx = q; break; }
        if (idx == stack.size()) idx = stack.size() - 1;
        for (size_t q = idx; q < stack.size(); ++q) {
            Cell* x = cellp(stack[q]);
            if (!x) continue;
            x->calc = CS_CIRC;
            x->v = Value::error(Err::Circ);
            ++circular;
        }
        stack.resize(idx);
    }

    void compute(const CellKey& root) {
        std::vector<CellKey> stack{root};
        if (Cell* c0 = cellp(root)) c0->calc |= CS_ONSTACK;
        while (!stack.empty()) {
            CellKey k = stack.back();
            Cell* cl = cellp(k);
            if (!cl || !cl->f || !(cl->calc & CS_DIRTY)) { if (cl) cl->calc &= ~CS_ONSTACK; stack.pop_back(); continue; }
            FormulaPtr f = cl->f;
            Evaluator ev(b, k.si, k.r, k.c);
            try {
                Opnd res = ev.run(*f);
                std::shared_ptr<const Array> arr;
                Value v = opnd_to_cell_value(ev, res, &arr);   // may still read (and throw for) precedents
                cl = cellp(k);
                cl->calc &= ~(CS_DIRTY | CS_ONSTACK | CS_CIRC);
                store(k, *cl, std::move(v), std::move(arr));
                ++evaluated;
                stack.pop_back();
            } catch (const NeedCells& nn) {
                bool cyc = false;
                for (auto it = nn.cells.rbegin(); it != nn.cells.rend(); ++it) {
                    CellKey nk{(*it)[0], (*it)[1], (*it)[2]};
                    Cell* nc = cellp(nk);
                    if (!nc || !(nc->calc & CS_DIRTY)) continue;
                    if (nc->calc & CS_ONSTACK) { cyc = true; continue; }
                    nc->calc |= CS_ONSTACK;
                    stack.push_back(nk);
                }
                if (cyc && stack.back() == k) {
                    // only cyclic precedents remain: resolve like a single-cell cycle
                    for (auto& c3 : nn.cells) { CellKey nk{c3[0], c3[1], c3[2]}; Cell* nc = cellp(nk); if (nc && (nc->calc & CS_ONSTACK) && (nc->calc & CS_DIRTY)) { throw_cycle(stack, nk); break; } }
                }
            } catch (const NeedCell& n) {
                CellKey nk{n.si, n.r, n.c};
                Cell* nc = cellp(nk);
                if (!nc) { stack.pop_back(); continue; }
                if (nc->calc & CS_ONSTACK) { throw_cycle(stack, nk); continue; }
                nc->calc |= CS_ONSTACK;
                stack.push_back(nk);
            }
        }
    }
};

} // namespace

static void build_graph(Book& b);
static RecalcStats run_dirty(Book& b, std::vector<CellKey> dirty, bool full) {
    auto t0 = std::chrono::steady_clock::now();
    Sched S(b);
    auto& C = b.calc();
    C.in_recalc = true;
    for (int round = 0; round < 6; ++round) {
        for (auto& k : dirty) {
            Cell* cl = S.cellp(k);
            if (cl && cl->f && (cl->calc & CS_DIRTY)) S.compute(k);
        }
        if (S.spill_changed.empty()) break;
        // cells covered by changed spills: their dependents need recalculation
        std::vector<CellKey> roots;
        for (auto& sc : S.spill_changed)
            for (int i = 0; i < sc.rows; ++i)
                for (int j = 0; j < sc.cols; ++j) if (i || j) roots.push_back({sc.k.si, sc.k.r + i, sc.k.c + j});
        S.spill_changed.clear();
        dirty.clear();
        if (!C.graph_valid) build_graph(b);
        // propagate
        std::vector<CellKey> q = roots;
        for (size_t i = 0; i < q.size(); ++i) {
            auto it = C.cell_deps.find(q[i]);
            if (it == C.cell_deps.end()) continue;
            for (auto& d : it->second) {
                Cell* x = S.cellp(d);
                if (x && x->f && !(x->calc & CS_DIRTY)) { x->calc |= CS_DIRTY; dirty.push_back(d); q.push_back(d); }
            }
        }
        for (auto& rd : C.range_deps) {
            Cell* x = S.cellp(rd.dep);
            if (!x || !x->f || (x->calc & CS_DIRTY)) continue;
            for (auto& rk : roots) if (rk.si == rd.a.si && rd.a.rg.contains(rk.r, rk.c)) { x->calc |= CS_DIRTY; dirty.push_back(rd.dep); break; }
        }
        if (dirty.empty()) break;
    }
    C.in_recalc = false;
    RecalcStats st;
    st.evaluated = S.evaluated;
    st.circular = S.circular;
    st.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    (void)full;
    C.last = st;
    ++b.gen;
    return st;
}

RecalcStats Book::recalc_all() {
    auto& C = *calc_;
    C.pending.clear();
    C.name_cache.clear();
    C.needs_full = false;
    // the dependency graph is only needed for incremental recalcs: build it lazily
    C.graph_valid = false;
    C.cell_deps.clear(); C.range_deps.clear(); C.volatiles.clear(); C.name_users.clear();
    std::vector<CellKey> dirty;
    for (int s = 0; s < sheet_count(); ++s) {
        Worksheet& ws = sheets[s];
        ws.spills.clear();
        for (int r = 0; r < (int)ws.rows.size(); ++r) {
            auto& cells = ws.rows[r].cells;
            for (int c = 0; c < (int)cells.size(); ++c) if (cells[c].f) { cells[c].calc = CS_DIRTY; dirty.push_back({s, r, c}); }
        }
    }
    return run_dirty(*this, std::move(dirty), true);
}

void Book::mark_dirty(int si, int r, int c) { calc_->pending.push_back({si, r, c}); }
void Book::invalidate_graph() { calc_->graph_valid = false; calc_->needs_full = true; calc_->name_cache.clear(); }
const RecalcStats& Book::last_recalc() const { return calc_->last; }

RecalcStats Book::recalc_dirty() {
    auto& C = *calc_;
    if (C.needs_full) return recalc_all();
    if (!C.graph_valid) build_graph(*this);   // cached values are current; only the graph is missing
    std::vector<CellKey> roots;
    roots.swap(C.pending);
    std::vector<CellKey> dirty;
    auto cellp = [&](const CellKey& k) -> Cell* {
        if (k.si < 0 || k.si >= sheet_count()) return nullptr;
        Worksheet& ws = sheets[k.si];
        if (k.r >= (int)ws.rows.size() || k.c >= (int)ws.rows[k.r].cells.size()) return nullptr;
        return &ws.rows[k.r].cells[k.c];
    };
    auto mark = [&](const CellKey& k) {
        Cell* x = cellp(k);
        if (x && x->f && !(x->calc & CS_DIRTY)) { x->calc |= CS_DIRTY; dirty.push_back(k); return true; }
        return false;
    };
    // roots inside an existing spill make the anchor dirty (spill may become blocked / unblocked)
    std::vector<CellKey> extra;
    for (auto& k : roots) {
        const Worksheet& ws = sheets[k.si];
        for (auto& [a, arr] : ws.spills) {
            if (k.r >= a.first && k.c >= a.second && k.r < a.first + arr->rows && k.c < a.second + arr->cols) extra.push_back({k.si, a.first, a.second});
        }
    }
    // A blocked (#SPILL!) anchor might become free: re-evaluate all #SPILL! cells on the sheet
    for (auto& k : roots) {
        const Worksheet& ws = sheets[k.si];
        for (int r = std::max(0, k.r - 2000); r <= k.r && r < (int)ws.rows.size(); ++r) {
            auto& cells = ws.rows[r].cells;
            for (int c = 0; c <= k.c && c < (int)cells.size(); ++c) if (cells[c].f && cells[c].v.is_err() && cells[c].v.err == Err::Spill) extra.push_back({k.si, r, c});
        }
    }
    for (auto& e : extra) mark(e);
    for (auto& k : roots) mark(k);
    for (auto& k : C.volatiles) mark(k);
    for (auto& k : C.name_users) mark(k);
    // circular cells are re-tried whenever something changes
    // propagate through cell deps (BFS), range deps (sweeps)
    std::vector<CellKey> changed = roots;
    changed.insert(changed.end(), dirty.begin(), dirty.end());
    size_t qi = 0;
    for (int sweep = 0; sweep < 64; ++sweep) {
        for (; qi < changed.size(); ++qi) {
            auto it = C.cell_deps.find(changed[qi]);
            if (it == C.cell_deps.end()) continue;
            for (auto& d : it->second) if (mark(d)) changed.push_back(d);
        }
        // range sweep: index changed cells per sheet/col
        std::unordered_map<long long, std::vector<int>> bycol;
        for (auto& k : changed) bycol[((long long)k.si << 20) | k.c].push_back(k.r);
        for (auto& [key, v] : bycol) std::sort(v.begin(), v.end());
        size_t before = changed.size();
        for (auto& rd : C.range_deps) {
            Cell* x = cellp(rd.dep);
            if (!x || !x->f || (x->calc & CS_DIRTY)) continue;
            bool hit = false;
            const Range& g = rd.a.rg;
            if (g.cols() <= 64 || (long long)bycol.size() > g.cols()) {
                for (int c = g.c0; c <= g.c1 && c < MAX_COLS && !hit; ++c) {
                    auto it = bycol.find(((long long)rd.a.si << 20) | c);
                    if (it == bycol.end()) continue;
                    auto lb = std::lower_bound(it->second.begin(), it->second.end(), g.r0);
                    if (lb != it->second.end() && *lb <= g.r1) hit = true;
                }
            } else {
                for (auto& k : changed) if (k.si == rd.a.si && g.contains(k.r, k.c)) { hit = true; break; }
            }
            if (hit && mark(rd.dep)) changed.push_back(rd.dep);
        }
        if (changed.size() == before && qi >= changed.size()) break;
    }
    return run_dirty(*this, std::move(dirty), false);
}

Value Book::evaluate(const std::string& text, int si, int r, int c) const {
    Formula f; std::string err;
    std::string t = text;
    if (!t.empty() && t[0] == '=') t.erase(0, 1);
    if (!compile_formula(t, *this, si, r, c, f, err)) return Value::error(Err::Name);
    Evaluator ev(*this, si, r, c);
    ev.probe = true;
    Opnd o = ev.run(f);
    return opnd_to_cell_value(ev, o, nullptr);
}

std::vector<std::pair<int, int>> Book::precedents(int si, int r, int c) const {
    std::vector<std::pair<int, int>> out;
    const Cell* cl = sheets[si].cell(r, c);
    if (!cl || !cl->f) return out;
    std::vector<Area> pre; bool names;
    formula_precedents(*this, *cl->f, si, r, c, pre, names);
    for (auto& a : pre) {
        if (a.si != si) continue;
        Range g = a.rg;
        if (g.r1 >= MAX_ROWS - 1 || g.c1 >= MAX_COLS - 1) { g.r1 = std::min(g.r1, sheets[si].nrows() - 1); g.c1 = std::min(g.c1, sheets[si].ncols() - 1); }
        for (int i = g.r0; i <= g.r1; ++i) for (int j = g.c0; j <= g.c1; ++j) { out.push_back({i, j}); if (out.size() > 10000) return out; }
    }
    return out;
}

std::vector<std::pair<int, int>> Book::dependents(int si, int r, int c) const {
    std::vector<std::pair<int, int>> out;
    auto& C = *calc_;
    if (!C.graph_valid) build_graph(*const_cast<Book*>(this));
    auto it = C.cell_deps.find({si, r, c});
    if (it != C.cell_deps.end()) for (auto& d : it->second) if (d.si == si) out.push_back({d.r, d.c});
    for (auto& rd : C.range_deps) if (rd.a.si == si && rd.a.rg.contains(r, c) && rd.dep.si == si) out.push_back({rd.dep.r, rd.dep.c});
    return out;
}

} // namespace pcbp::sheet
