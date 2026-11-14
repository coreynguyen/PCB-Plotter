// ============================================================================
// sheet_look.cpp — cell appearance (style + conditional formatting per
// ECMA-376 §18.3.1.10 cfRule semantics), data validation (§18.3.1.32), and
// the rich clipboard: HTML table export with inline CSS, the Windows
// "HTML Format" (CF_HTML) envelope, and an HTML table importer.
// ============================================================================
#include "formula_internal.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace pcbp::sheet {

// ------------------------------------------------------------ CF helpers --
namespace {

Value eval_cf_formula(const Book& b, int si, int r, int c, const std::string& text, const std::string& key, int tl_r, int tl_c) {
    auto& C = b.calc();
    auto it = C.cf_formulas.find(key);
    std::shared_ptr<Formula> f;
    if (it == C.cf_formulas.end()) {
        f = std::make_shared<Formula>();
        std::string err;
        std::string t = text;
        if (!t.empty() && t[0] == '=') t.erase(0, 1);
        if (!compile_formula(t, b, si, tl_r, tl_c, *f, err)) f->code.clear();
        C.cf_formulas[key] = f;
    } else f = it->second;
    if (f->code.empty()) return Value::error(Err::Name);
    Evaluator ev(b, si, r, c);
    ev.probe = true;
    Opnd o = ev.run(*f);
    return ev.scalar(o);
}

bool truthy(const Value& v) {
    if (v.is_num() || v.is_bool()) return v.num != 0;
    if (v.is_str()) { std::string u = upper_ascii(v.str); return u == "TRUE"; }
    return false;
}

bool cmp_op(CmpOp op, const Value& x, const Value& a, const Value& b2) {
    int c1 = compare_values(x, a);
    switch (op) {
    case CmpOp::Equal: return c1 == 0;
    case CmpOp::NotEqual: return c1 != 0;
    case CmpOp::Greater: return c1 > 0;
    case CmpOp::Less: return c1 < 0;
    case CmpOp::GreaterEqual: return c1 >= 0;
    case CmpOp::LessEqual: return c1 <= 0;
    case CmpOp::Between: case CmpOp::NotBetween: {
        Value lo = a, hi = b2;
        if (compare_values(lo, hi) > 0) std::swap(lo, hi);
        bool in = compare_values(x, lo) >= 0 && compare_values(x, hi) <= 0;
        return op == CmpOp::Between ? in : !in;
    }
    }
    return false;
}

const Book::Calc::CfStats& cf_stats(const Book& b, int si, int cfi, int ri) {
    auto& C = b.calc();
    if (C.cf_gen != b.gen) { C.cf_stats.clear(); C.cf_formulas.clear(); C.cf_gen = b.gen; }
    std::string key = std::to_string(si) + "|" + std::to_string(cfi) + "|" + std::to_string(ri);
    auto& S = C.cf_stats[key];
    if (S.ok) return S;
    S.ok = true;
    const Worksheet& ws = b.sheets[si];
    const CondFormat& cf = ws.cond[cfi];
    const CondRule& rule = cf.rules[ri];
    bool want_counts = rule.type == CondRule::Duplicate || rule.type == CondRule::Unique;
    double sum = 0;
    for (auto g : cf.ranges) {
        if (g.r1 >= (int)ws.rows.size()) g.r1 = (int)ws.rows.size() - 1;
        if (g.c1 >= MAX_COLS - 1) g.c1 = ws.ncols() - 1;
        for (int r = g.r0; r <= g.r1; ++r)
            for (int c = g.c0; c <= g.c1; ++c) {
                const Value& v = ws.value(r, c);
                if (want_counts) { if (!v.is_empty()) ++S.counts[lower_utf8(value_to_text(v)) + (char)('0' + v.type)]; continue; }
                if (v.is_num()) { S.sorted.push_back(v.num); sum += v.num; }
            }
    }
    std::sort(S.sorted.begin(), S.sorted.end());
    size_t n = S.sorted.size();
    if (n) {
        S.mn = S.sorted.front(); S.mx = S.sorted.back(); S.mean = sum / n;
        double q = 0; for (double x : S.sorted) q += (x - S.mean) * (x - S.mean);
        S.sd = n > 1 ? std::sqrt(q / (n - 1)) : 0;
    }
    if (rule.type == CondRule::Top10 && n) {
        size_t k = rule.percent ? std::max<size_t>(1, (size_t)std::floor(n * rule.rank / 100.0)) : (size_t)std::max(1, rule.rank);
        k = std::min(k, n);
        S.thr = rule.bottom ? S.sorted[k - 1] : S.sorted[n - k];
    }
    return S;
}

double cfvo_value(const Book& b, int si, const CfValueObject& o, const Book::Calc::CfStats& S, bool is_min) {
    switch (o.type) {
    case CfValueObject::Min: return S.mn;
    case CfValueObject::Max: return S.mx;
    case CfValueObject::Num: { double d; if (parse_number_strict(o.val, d)) return d; Value v = b.evaluate(o.val, si); return v.is_num() ? v.num : (is_min ? S.mn : S.mx); }
    case CfValueObject::Percent: { double p = atof(o.val.c_str()); return S.mn + (S.mx - S.mn) * p / 100; }
    case CfValueObject::Percentile: {
        double p = atof(o.val.c_str()) / 100;
        if (S.sorted.empty()) return 0;
        double h = p * (S.sorted.size() - 1);
        size_t lo = (size_t)std::floor(h);
        if (lo + 1 >= S.sorted.size()) return S.sorted.back();
        return S.sorted[lo] + (h - lo) * (S.sorted[lo + 1] - S.sorted[lo]);
    }
    case CfValueObject::Formula: { Value v = b.evaluate(o.val, si); return v.is_num() ? v.num : 0; }
    }
    return 0;
}

Argb lerp_color(Argb a, Argb b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    auto ch = [&](int sh) { int x = (a >> sh) & 255, y = (b >> sh) & 255; return (Argb)std::lround(x + (y - x) * t) << sh; };
    return 0xFF000000u | ch(16) | ch(8) | ch(0);
}

void apply_dxf(CellLook& L, const Dxf& d, const Book& b, const Value& v) {
    if (d.font_color) L.text_color = *d.font_color, L.style.font.color = *d.font_color;
    if (d.fill_color) L.fill_color = *d.fill_color, L.style.fill.pattern = FillPattern::Solid, L.style.fill.fg = *d.fill_color;
    if (d.bold) L.style.font.bold = *d.bold;
    if (d.italic) L.style.font.italic = *d.italic;
    if (d.strike) L.style.font.strike = *d.strike;
    if (d.underline) L.style.font.underline = *d.underline;
    if (d.border) L.style.border = *d.border;
    if (d.numfmt) { L.style.numfmt = *d.numfmt; Formatted f = format_value(v, *d.numfmt, b.date1904); L.text = f.text; if (f.color) L.text_color = f.color; }
}

} // namespace

CellLook Book::look(int si, int r, int c) const {
    CellLook L;
    const Worksheet& ws = sheets[si];
    L.style = styles.get(ws.style_at(r, c));
    const Value& v = ws.value(r, c);
    const Cell* cl = ws.cell(r, c);
    L.is_formula = cl && cl->f;
    Formatted f = format_value(v, L.style.numfmt, date1904);
    L.text = std::move(f.text);
    L.text_color = f.color ? f.color : L.style.font.color;
    if (L.style.fill.pattern == FillPattern::Solid) L.fill_color = L.style.fill.fg ? L.style.fill.fg : L.style.fill.bg;
    else if (L.style.fill.pattern != FillPattern::None) L.fill_color = L.style.fill.bg ? L.style.fill.bg : L.style.fill.fg;
    // conditional formats (by priority)
    struct Hit { int prio; int cfi; int ri; };
    std::vector<Hit> hits;
    for (int i = 0; i < (int)ws.cond.size(); ++i) {
        const CondFormat& cf = ws.cond[i];
        bool in = false;
        for (auto& g : cf.ranges) if (g.contains(r, c)) { in = true; break; }
        if (!in) continue;
        for (int k = 0; k < (int)cf.rules.size(); ++k) hits.push_back({cf.rules[k].priority, i, k});
    }
    if (!hits.empty()) {
        std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.prio < b.prio; });
        for (auto& h : hits) {
            const CondFormat& cf = ws.cond[h.cfi];
            const CondRule& rule = cf.rules[h.ri];
            int tl_r = cf.ranges[0].r0, tl_c = cf.ranges[0].c0;
            std::string kb = std::to_string(si) + "|" + std::to_string(h.cfi) + "|" + std::to_string(h.ri) + "|";
            if (calc().cf_gen != gen) { calc().cf_stats.clear(); calc().cf_formulas.clear(); calc().cf_gen = gen; }
            bool match = false;
            switch (rule.type) {
            case CondRule::CellIs: {
                if (v.is_err() || (v.is_empty() && rule.op != CmpOp::Equal && rule.op != CmpOp::NotEqual)) { match = false; break; }
                Value a = eval_cf_formula(*this, si, r, c, rule.f1, kb + "1", tl_r, tl_c);
                Value b2 = rule.f2.empty() ? Value() : eval_cf_formula(*this, si, r, c, rule.f2, kb + "2", tl_r, tl_c);
                Value x = v.is_empty() ? Value::number(0) : v;
                match = !a.is_err() && cmp_op(rule.op, x, a, b2);
                break;
            }
            case CondRule::Expression: match = truthy(eval_cf_formula(*this, si, r, c, rule.f1, kb + "1", tl_r, tl_c)); break;
            case CondRule::ContainsText: case CondRule::NotContainsText: case CondRule::BeginsWith: case CondRule::EndsWith: {
                std::string t = lower_utf8(value_to_text(v)), q = lower_utf8(rule.text);
                if (rule.type == CondRule::ContainsText) match = t.find(q) != std::string::npos;
                else if (rule.type == CondRule::NotContainsText) match = t.find(q) == std::string::npos;
                else if (rule.type == CondRule::BeginsWith) match = t.compare(0, q.size(), q) == 0;
                else match = t.size() >= q.size() && t.compare(t.size() - q.size(), q.size(), q) == 0;
                break;
            }
            case CondRule::Duplicate: case CondRule::Unique: {
                if (v.is_empty()) break;
                auto& S = cf_stats(*this, si, h.cfi, h.ri);
                auto it = S.counts.find(lower_utf8(value_to_text(v)) + (char)('0' + v.type));
                int n = it == S.counts.end() ? 0 : it->second;
                match = rule.type == CondRule::Duplicate ? n > 1 : n == 1;
                break;
            }
            case CondRule::Top10: {
                if (!v.is_num()) break;
                auto& S = cf_stats(*this, si, h.cfi, h.ri);
                if (S.sorted.empty()) break;
                match = rule.bottom ? v.num <= S.thr : v.num >= S.thr;
                break;
            }
            case CondRule::AboveAverage: {
                if (!v.is_num()) break;
                auto& S = cf_stats(*this, si, h.cfi, h.ri);
                if (S.sorted.empty()) break;
                match = rule.below ? (rule.equal_avg ? v.num <= S.mean : v.num < S.mean) : (rule.equal_avg ? v.num >= S.mean : v.num > S.mean);
                break;
            }
            case CondRule::ContainsBlanks: match = v.is_empty() || (v.is_str() && value_to_text(v).find_first_not_of(" ") == std::string::npos); break;
            case CondRule::NotContainsBlanks: match = !(v.is_empty() || (v.is_str() && value_to_text(v).find_first_not_of(" ") == std::string::npos)); break;
            case CondRule::ContainsErrors: match = v.is_err(); break;
            case CondRule::NotContainsErrors: match = !v.is_err(); break;
            case CondRule::ColorScale: {
                if (!v.is_num() || rule.cfvo.size() < 2) break;
                auto& S = cf_stats(*this, si, h.cfi, h.ri);
                if (S.sorted.empty()) break;
                std::vector<double> th;
                for (size_t q = 0; q < rule.cfvo.size(); ++q) th.push_back(cfvo_value(*this, si, rule.cfvo[q], S, q == 0));
                Argb col;
                if (v.num <= th.front()) col = rule.cfvo.front().color;
                else if (v.num >= th.back()) col = rule.cfvo.back().color;
                else {
                    size_t q = 0;
                    while (q + 2 < th.size() && v.num > th[q + 1]) ++q;
                    double span = th[q + 1] - th[q];
                    col = lerp_color(rule.cfvo[q].color, rule.cfvo[q + 1].color, span > 0 ? (v.num - th[q]) / span : 0);
                }
                L.fill_color = col;
                continue;   // colour scales do not stop evaluation
            }
            case CondRule::DataBar: {
                if (!v.is_num() || rule.cfvo.size() < 2) break;
                auto& S = cf_stats(*this, si, h.cfi, h.ri);
                if (S.sorted.empty()) break;
                double lo = cfvo_value(*this, si, rule.cfvo[0], S, true), hi = cfvo_value(*this, si, rule.cfvo[1], S, false);
                double fr = hi > lo ? (v.num - lo) / (hi - lo) : 1;
                L.bar = (float)std::clamp(fr, 0.0, 1.0);
                L.bar_color = rule.bar_color;
                continue;
            }
            }
            if (match) {
                if (rule.dxf >= 0 && rule.dxf < (int)dxfs.size()) apply_dxf(L, dxfs[rule.dxf], *this, v);
                if (rule.stop) break;
            }
        }
    }
    // alignment
    switch (L.style.halign) {
    case HAlign::General: L.align = v.is_num() ? HAlign::Right : (v.is_bool() || v.is_err()) ? HAlign::Center : HAlign::Left; break;
    case HAlign::Fill: case HAlign::Justify: case HAlign::Distributed: L.align = HAlign::Left; break;
    case HAlign::CenterContinuous: L.align = HAlign::Center; break;
    default: L.align = L.style.halign;
    }
    L.overflow_ok = v.is_str() && !L.style.wrap && (L.style.halign == HAlign::General || L.style.halign == HAlign::Left);
    for (auto& dv : ws.validations) {
        if (dv.type != DataValidation::List || !dv.dropdown) continue;
        for (auto& g : dv.ranges) if (g.contains(r, c)) { L.dropdown = true; break; }
        if (L.dropdown) break;
    }
    return L;
}

// ------------------------------------------------------- data validation --
static const DataValidation* dv_at(const Worksheet& ws, int r, int c) {
    for (auto& dv : ws.validations) for (auto& g : dv.ranges) if (g.contains(r, c)) return &dv;
    return nullptr;
}

std::vector<std::string> Book::list_items(int si, int r, int c) const {
    std::vector<std::string> out;
    const DataValidation* dv = dv_at(sheets[si], r, c);
    if (!dv || dv->type != DataValidation::List) return out;
    std::string f = dv->f1;
    if (!f.empty() && f[0] == '=') f.erase(0, 1);
    if (f.size() >= 2 && f.front() == '"' && f.back() == '"') {
        std::string body = f.substr(1, f.size() - 2), cur;
        for (char ch : body) { if (ch == ',') { size_t a = cur.find_first_not_of(' '); out.push_back(a == std::string::npos ? "" : cur.substr(a)); cur.clear(); } else cur += ch; }
        size_t a = cur.find_first_not_of(' ');
        out.push_back(a == std::string::npos ? "" : cur.substr(a));
        return out;
    }
    Formula fm; std::string err;
    int tl_r = dv->ranges.empty() ? r : dv->ranges[0].r0, tl_c = dv->ranges.empty() ? c : dv->ranges[0].c0;
    if (!compile_formula(f, *this, si, tl_r, tl_c, fm, err)) return out;
    Evaluator ev(*this, si, r, c);
    ev.probe = true;
    Opnd o = ev.run(fm);
    if (o.k == Opnd::V) { if (!o.v.is_empty()) out.push_back(value_to_text(o.v)); return out; }
    if (o.k == Opnd::R) {
        Range g = ev.clip_used(o.ref.si, o.ref.rg);
        for (int i = g.r0; i <= g.r1; ++i) for (int j = g.c0; j <= g.c1; ++j) {
            std::string t = display_text(o.ref.si, i, j);
            if (!t.empty()) out.push_back(t);
        }
        return out;
    }
    ev.each(o, [&](const Value& v, bool) { if (!v.is_empty()) out.push_back(value_to_text(v)); });
    return out;
}

bool Book::validate(int si, int r, int c, const Value& v, std::string* msg, DataValidation::ErrStyle* how) const {
    const DataValidation* dv = dv_at(sheets[si], r, c);
    if (!dv || dv->type == DataValidation::Any) return true;
    if (how) *how = dv->err_style;
    auto fail = [&]() {
        if (msg) *msg = dv->error.empty() ? "The value doesn't match the data validation restrictions defined for this cell." : dv->error;
        return false;
    };
    bool blank = v.is_empty() || (v.is_str() && v.str.empty());
    if (blank) return dv->allow_blank ? true : fail();
    int tl_r = dv->ranges[0].r0, tl_c = dv->ranges[0].c0;
    auto evalf = [&](const std::string& f) {
        if (f.empty()) return Value();
        Formula fm; std::string err;
        std::string t = f;
        if (!t.empty() && t[0] == '=') t.erase(0, 1);
        if (!compile_formula(t, *this, si, tl_r, tl_c, fm, err)) return Value::error(Err::Name);
        Evaluator ev(*this, si, r, c);
        ev.probe = true;
        return ev.scalar(ev.run(fm));
    };
    switch (dv->type) {
    case DataValidation::List: {
        std::string t = value_to_text(v);
        for (auto& it : list_items(si, r, c)) if (ci_equal(it, t)) return true;
        return fail();
    }
    case DataValidation::Whole: case DataValidation::Decimal: case DataValidation::Date: case DataValidation::Time: {
        double x; Err e;
        if (!to_number(v, x, e) || v.is_bool()) return fail();
        if (dv->type == DataValidation::Whole && x != std::floor(x)) return fail();
        Value a = evalf(dv->f1), b2 = evalf(dv->f2);
        return cmp_op(dv->op, Value::number(x), a, b2) ? true : fail();
    }
    case DataValidation::TextLength: {
        Value a = evalf(dv->f1), b2 = evalf(dv->f2);
        return cmp_op(dv->op, Value::number((double)utf8_len(value_to_text(v))), a, b2) ? true : fail();
    }
    case DataValidation::Custom: return truthy(evalf(dv->f1)) ? true : fail();
    default: return true;
    }
}

// ================================================================= HTML ==
namespace {

std::string html_esc(const std::string& s) {
    std::string o;
    for (char ch : s) {
        switch (ch) {
        case '&': o += "&amp;"; break; case '<': o += "&lt;"; break; case '>': o += "&gt;"; break; case '"': o += "&quot;"; break;
        case '\n': o += "<br>"; break;
        default: o += ch;
        }
    }
    return o;
}
std::string css_color(Argb c) { char b[16]; snprintf(b, sizeof b, "#%06X", (unsigned)(c & 0xFFFFFF)); return b; }
std::string css_border(const BorderSide& s) {
    if (s.style == BorderStyle::None) return {};
    const char* w = "1px", *k = "solid";
    switch (s.style) {
    case BorderStyle::Medium: case BorderStyle::MediumDashed: case BorderStyle::MediumDashDot: case BorderStyle::MediumDashDotDot: w = "2px"; break;
    case BorderStyle::Thick: w = "3px"; break;
    case BorderStyle::Double: w = "3px"; k = "double"; break;
    default: break;
    }
    switch (s.style) {
    case BorderStyle::Dashed: case BorderStyle::MediumDashed: case BorderStyle::DashDot: case BorderStyle::MediumDashDot: case BorderStyle::DashDotDot: case BorderStyle::MediumDashDotDot: case BorderStyle::SlantDashDot: k = "dashed"; break;
    case BorderStyle::Dotted: case BorderStyle::Hair: k = "dotted"; break;
    default: break;
    }
    return std::string(w) + " " + k + " " + css_color(s.color ? s.color : 0xFF000000);
}

} // namespace

std::string Book::copy_html(int si, const Range& rg0) const {
    const Worksheet& ws = sheets[si];
    Range rg = rg0.norm();
    if (rg.r1 >= MAX_ROWS - 1) rg.r1 = std::max(rg.r0, ws.nrows() - 1);
    if (rg.c1 >= MAX_COLS - 1) rg.c1 = std::max(rg.c0, ws.ncols() - 1);
    std::string o = "<table border=0 cellpadding=0 cellspacing=0 style=\"border-collapse:collapse\">\n";
    for (int c = rg.c0; c <= rg.c1; ++c) { char b[64]; snprintf(b, sizeof b, "<col width=%d>", (int)std::lround(ws.col_width(c) * 7 + 5)); o += b; }
    o += "\n";
    for (int r = rg.r0; r <= rg.r1; ++r) {
        if (r < (int)ws.rows.size() && ws.rows[r].filtered) continue;
        char hb[64]; snprintf(hb, sizeof hb, "<tr style=\"height:%.1fpt\">", ws.row_height(r));
        o += hb;
        for (int c = rg.c0; c <= rg.c1; ++c) {
            const Range* m = ws.merge_at(r, c);
            if (m && (m->r0 != r || m->c0 != c)) continue;
            CellLook L = look(si, r, c);
            const Style& s = L.style;
            std::string st = "font-family:'" + s.font.name + "';font-size:" + fmt_general(s.font.size) + "pt;";
            if (s.font.bold) st += "font-weight:700;";
            if (s.font.italic) st += "font-style:italic;";
            if (s.font.underline != Underline::None || s.font.strike) st += std::string("text-decoration:") + (s.font.underline != Underline::None ? "underline" : "") + (s.font.strike ? " line-through" : "") + ";";
            if (L.text_color) st += "color:" + css_color(L.text_color) + ";";
            if (L.fill_color) st += "background:" + css_color(L.fill_color) + ";";
            static const char* ha[] = {"left", "left", "center", "right", "left", "justify", "center", "justify"};
            st += std::string("text-align:") + ha[(int)L.align] + ";";
            static const char* va[] = {"bottom", "middle", "top", "middle", "middle"};
            st += std::string("vertical-align:") + va[(int)s.valign] + ";";
            if (s.wrap) st += "white-space:normal;"; else st += "white-space:nowrap;";
            std::string b;
            if (!(b = css_border(s.border.left)).empty()) st += "border-left:" + b + ";";
            if (!(b = css_border(s.border.right)).empty()) st += "border-right:" + b + ";";
            if (!(b = css_border(s.border.top)).empty()) st += "border-top:" + b + ";";
            if (!(b = css_border(s.border.bottom)).empty()) st += "border-bottom:" + b + ";";
            if (s.indent) st += "padding-left:" + std::to_string(s.indent * 9) + "px;";
            if (s.numfmt != "General") {
                std::string nf;
                for (char ch : s.numfmt) { if (ch == '"') nf += "\\0022"; else if (ch == '\\') nf += "\\\\"; else nf += ch; }
                st += "mso-number-format:\"" + nf + "\";";
            }
            o += "<td";
            if (m) { if (m->rows() > 1) o += " rowspan=" + std::to_string(m->rows()); if (m->cols() > 1) o += " colspan=" + std::to_string(m->cols()); }
            const Value& v = ws.value(r, c);
            if (v.is_num()) o += " x:num=\"" + fmt_general(v.num, 17) + "\"";
            const Cell* cl = ws.cell(r, c);
            if (cl && cl->f) o += " x:fmla=\"" + html_esc("=" + formula_text(*cl->f, *this, si, r, c)) + "\"";
            o += " style=\"" + html_esc(st) + "\">" + html_esc(L.text) + "</td>";
        }
        o += "</tr>\n";
    }
    o += "</table>\n";
    return o;
}

std::string cf_html_wrap(const std::string& frag) {
    const std::string pre = "<html>\r\n<head><meta charset=\"utf-8\"></head>\r\n<body>\r\n<!--StartFragment-->";
    const std::string post = "<!--EndFragment-->\r\n</body>\r\n</html>";
    const char* hdr_fmt = "Version:0.9\r\nStartHTML:%010d\r\nEndHTML:%010d\r\nStartFragment:%010d\r\nEndFragment:%010d\r\n";
    char tmp[256];
    int hlen = snprintf(tmp, sizeof tmp, hdr_fmt, 0, 0, 0, 0);
    int sh = hlen, sf = sh + (int)pre.size(), ef = sf + (int)frag.size(), eh = ef + (int)post.size();
    snprintf(tmp, sizeof tmp, hdr_fmt, sh, eh, sf, ef);
    return std::string(tmp) + pre + frag + post;
}

std::string cf_html_unwrap(const std::string& s) {
    auto num = [&](const char* key) -> long {
        size_t p = s.find(key);
        if (p == std::string::npos || p > 512) return -1;
        return atol(s.c_str() + p + strlen(key));
    };
    long sf = num("StartFragment:"), ef = num("EndFragment:");
    long sh = num("StartHTML:"), eh = num("EndHTML:");
    if (sh >= 0 && eh > sh && (size_t)eh <= s.size()) {
        std::string doc = s.substr(sh, eh - sh);
        if (doc.find("<table") != std::string::npos || doc.find("<TABLE") != std::string::npos) return doc;
    }
    if (sf >= 0 && ef > sf && (size_t)ef <= s.size()) return s.substr(sf, ef - sf);
    size_t k = s.find('<');
    return k == std::string::npos ? s : s.substr(k);
}

// ----------------------------------------------------------- HTML import --
namespace {

std::string decode_entities(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { o += s[i]; continue; }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) { o += s[i]; continue; }
        std::string e = s.substr(i + 1, semi - i - 1);
        if (e == "amp") o += '&'; else if (e == "lt") o += '<'; else if (e == "gt") o += '>'; else if (e == "quot") o += '"';
        else if (e == "apos") o += '\''; else if (e == "nbsp") o += ' ';
        else if (!e.empty() && e[0] == '#') {
            uint32_t cp = (e.size() > 1 && (e[1] == 'x' || e[1] == 'X')) ? (uint32_t)strtoul(e.c_str() + 2, nullptr, 16) : (uint32_t)strtoul(e.c_str() + 1, nullptr, 10);
            if (cp == 160) cp = ' ';
            utf8_append(o, cp);
        } else { o += s.substr(i, semi - i + 1); }
        i = semi;
    }
    return o;
}

struct Tag { std::string name; std::vector<std::pair<std::string, std::string>> attrs; bool close = false; };

bool read_tag(const std::string& h, size_t& i, Tag& t) {
    // h[i] == '<'
    size_t j = i + 1;
    t = Tag();
    if (j < h.size() && h[j] == '!') {   // comment / doctype
        size_t e = h.compare(j, 3, "!--") == 0 ? h.find("-->", j) : h.find('>', j);
        i = e == std::string::npos ? h.size() : e + (h.compare(j, 3, "!--") == 0 ? 3 : 1);
        t.name = "!";
        return true;
    }
    if (j < h.size() && h[j] == '/') { t.close = true; ++j; }
    size_t k = j;
    while (k < h.size() && (isalnum((unsigned char)h[k]) || h[k] == ':')) ++k;
    t.name = lower_utf8(h.substr(j, k - j));
    // attributes
    while (k < h.size() && h[k] != '>') {
        while (k < h.size() && isspace((unsigned char)h[k])) ++k;
        if (k >= h.size() || h[k] == '>' || h[k] == '/') { if (k < h.size() && h[k] == '/') ++k; continue; }
        size_t a = k;
        while (k < h.size() && !isspace((unsigned char)h[k]) && h[k] != '=' && h[k] != '>') ++k;
        std::string an = lower_utf8(h.substr(a, k - a)), av;
        while (k < h.size() && isspace((unsigned char)h[k])) ++k;
        if (k < h.size() && h[k] == '=') {
            ++k;
            while (k < h.size() && isspace((unsigned char)h[k])) ++k;
            if (k < h.size() && (h[k] == '"' || h[k] == '\'')) { char q = h[k++]; size_t e = h.find(q, k); if (e == std::string::npos) e = h.size(); av = h.substr(k, e - k); k = e + 1; }
            else { size_t e = k; while (e < h.size() && !isspace((unsigned char)h[e]) && h[e] != '>') ++e; av = h.substr(k, e - k); k = e; }
        }
        t.attrs.push_back({an, decode_entities(av)});
    }
    i = k < h.size() ? k + 1 : h.size();
    return true;
}
std::string attr(const Tag& t, const char* n) { for (auto& a : t.attrs) if (a.first == n) return a.second; return {}; }

Argb parse_css_color(std::string v) {
    while (!v.empty() && v[0] == ' ') v.erase(0, 1);
    std::string l = lower_utf8(v);
    if (!l.empty() && l[0] == '#') {
        std::string hx = l.substr(1, 6);
        if (hx.size() == 3) hx = {hx[0], hx[0], hx[1], hx[1], hx[2], hx[2]};
        return 0xFF000000u | (Argb)strtoul(hx.c_str(), nullptr, 16);
    }
    if (l.rfind("rgb", 0) == 0) {
        int r = 0, g = 0, b = 0;
        if (sscanf(l.c_str() + l.find('(') + 1, "%d , %d , %d", &r, &g, &b) == 3) return 0xFF000000u | (Argb)(r << 16 | g << 8 | b);
    }
    static const struct { const char* n; Argb c; } named[] = {{"black", 0xFF000000}, {"white", 0xFFFFFFFF}, {"red", 0xFFFF0000}, {"green", 0xFF008000},
        {"blue", 0xFF0000FF}, {"yellow", 0xFFFFFF00}, {"gray", 0xFF808080}, {"grey", 0xFF808080}, {"silver", 0xFFC0C0C0}, {"orange", 0xFFFFA500}, {"navy", 0xFF000080}};
    for (auto& x : named) if (l.rfind(x.n, 0) == 0) return x.c;
    return 0;
}

void apply_css(Style& s, const std::string& css, bool* text_fmt) {
    size_t p = 0;
    while (p < css.size()) {
        size_t e = css.find(';', p);
        // mso-number-format values can contain ';' inside quotes
        size_t colon = css.find(':', p);
        if (colon != std::string::npos && (e == std::string::npos || colon < e)) {
            size_t q = colon + 1;
            while (q < css.size() && css[q] == ' ') ++q;
            if (q < css.size() && (css[q] == '"' || css[q] == '\'')) { size_t qe = css.find(css[q], q + 1); if (qe != std::string::npos) e = css.find(';', qe); }
        }
        std::string decl = css.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? css.size() : e + 1;
        size_t c = decl.find(':');
        if (c == std::string::npos) continue;
        std::string k = lower_utf8(decl.substr(0, c)), v = decl.substr(c + 1);
        while (!k.empty() && isspace((unsigned char)k[0])) k.erase(0, 1);
        while (!k.empty() && isspace((unsigned char)k.back())) k.pop_back();
        while (!v.empty() && isspace((unsigned char)v[0])) v.erase(0, 1);
        while (!v.empty() && isspace((unsigned char)v.back())) v.pop_back();
        std::string lv = lower_utf8(v);
        if (k == "font-weight") s.font.bold = lv == "bold" || lv == "bolder" || atoi(lv.c_str()) >= 600;
        else if (k == "font-style") s.font.italic = lv == "italic" || lv == "oblique";
        else if (k == "text-decoration" || k == "text-decoration-line") { if (lv.find("underline") != std::string::npos) s.font.underline = Underline::Single; if (lv.find("line-through") != std::string::npos) s.font.strike = true; }
        else if (k == "color") { if (Argb a = parse_css_color(v)) s.font.color = a; }
        else if (k == "background" || k == "background-color") { if (Argb a = parse_css_color(v)) { s.fill.pattern = FillPattern::Solid; s.fill.fg = a; } }
        else if (k == "font-size") { double d = atof(lv.c_str()); if (lv.find("px") != std::string::npos) d *= 0.75; if (d > 0) s.font.size = d; }
        else if (k == "font-family") { std::string f = v; size_t cm = f.find(','); if (cm != std::string::npos) f = f.substr(0, cm); f.erase(std::remove(f.begin(), f.end(), '\''), f.end()); f.erase(std::remove(f.begin(), f.end(), '"'), f.end()); if (!f.empty()) s.font.name = f; }
        else if (k == "text-align") { if (lv == "center") s.halign = HAlign::Center; else if (lv == "right") s.halign = HAlign::Right; else if (lv == "left") s.halign = HAlign::Left; else if (lv == "justify") s.halign = HAlign::Justify; }
        else if (k == "vertical-align") { if (lv == "top") s.valign = VAlign::Top; else if (lv == "middle") s.valign = VAlign::Center; else if (lv == "bottom") s.valign = VAlign::Bottom; }
        else if (k == "white-space") s.wrap = lv == "normal" || lv == "pre-wrap";
        else if (k == "mso-number-format") {
            std::string f = v;
            if (f.size() >= 2 && (f[0] == '"' || f[0] == '\'')) f = f.substr(1, f.size() - 2);
            std::string o;
            for (size_t i = 0; i < f.size(); ++i) {
                if (f[i] == '\\' && i + 4 < f.size() && isxdigit((unsigned char)f[i + 1]) && isxdigit((unsigned char)f[i + 4]) && f.compare(i + 1, 2, "00") == 0) { o += (char)strtol(f.substr(i + 1, 4).c_str(), nullptr, 16); i += 4; }
                else if (f[i] == '\\' && i + 1 < f.size()) { o += f[i + 1]; ++i; }
                else o += f[i];
            }
            if (o == "@") { if (text_fmt) *text_fmt = true; s.numfmt = "@"; }
            else if (!o.empty()) s.numfmt = o;
        } else if (k.rfind("border", 0) == 0 && lv.find("none") == std::string::npos) {
            BorderSide bs;
            bs.style = lv.find("double") != std::string::npos ? BorderStyle::Double : lv.find("dotted") != std::string::npos ? BorderStyle::Dotted :
                       lv.find("dashed") != std::string::npos ? BorderStyle::Dashed : (lv.find("2px") != std::string::npos || lv.find("1.5pt") != std::string::npos) ? BorderStyle::Medium :
                       lv.find("3px") != std::string::npos ? BorderStyle::Thick : BorderStyle::Thin;
            size_t hash = v.find('#');
            bs.color = hash != std::string::npos ? parse_css_color(v.substr(hash)) : 0xFF000000;
            if (k == "border" ) s.border.left = s.border.right = s.border.top = s.border.bottom = bs;
            else if (k == "border-left") s.border.left = bs; else if (k == "border-right") s.border.right = bs;
            else if (k == "border-top") s.border.top = bs; else if (k == "border-bottom") s.border.bottom = bs;
        }
    }
}

} // namespace

bool parse_html_table(const std::string& html, ClipBlock& out) {
    out = ClipBlock();
    // <style> classes (Excel emits .xlNN rules)
    std::unordered_map<std::string, std::string> classes;
    {
        std::string low = lower_utf8(html);
        size_t sp = low.find("<style");
        while (sp != std::string::npos) {
            size_t se = low.find("</style", sp);
            if (se == std::string::npos) break;
            std::string css = html.substr(sp, se - sp);
            size_t q = 0;
            while ((q = css.find('.', q)) != std::string::npos) {
                size_t ob = css.find('{', q), cb = css.find('}', q);
                if (ob == std::string::npos || cb == std::string::npos || cb < ob) break;
                std::string sel = css.substr(q + 1, ob - q - 1);
                while (!sel.empty() && isspace((unsigned char)sel.back())) sel.pop_back();
                if (sel.find_first_of(" ,:") == std::string::npos) classes[sel] += css.substr(ob + 1, cb - ob - 1) + ";";
                q = cb + 1;
            }
            sp = low.find("<style", se);
        }
    }
    size_t i = 0;
    bool in_table = false, in_cell = false;
    int depth = 0;
    std::vector<std::vector<std::pair<Cell, std::string>>> grid;   // (cell, formula text)
    std::vector<std::vector<char>> occupied;
    std::vector<Style> styles{Style()};
    std::unordered_map<std::string, uint32_t> style_ix;
    int row = -1, col = 0;
    std::string text;
    Style cur_style; bool cur_text_fmt = false; std::string cur_num, cur_fmla; int cur_rs = 1, cur_cs = 1;
    auto occupy = [&](int r, int c) {
        while ((int)occupied.size() <= r) occupied.emplace_back();
        if ((int)occupied[r].size() <= c) occupied[r].resize(c + 1, 0);
        occupied[r][c] = 1;
    };
    auto is_occ = [&](int r, int c) { return r < (int)occupied.size() && c < (int)occupied[r].size() && occupied[r][c]; };
    auto finish_cell = [&]() {
        if (!in_cell) return;
        in_cell = false;
        // collapse whitespace like a browser (keep <br> newlines)
        std::string t; bool sp = false;
        for (char ch : text) {
            if (ch == '\r' || ch == '\t' || ch == ' ' || ch == '\n') { if (ch == '\n' && !text.empty() && false) {} sp = true; continue; }
            if (ch == '\x01') { t += '\n'; sp = false; continue; }
            if (sp && !t.empty() && t.back() != '\n') t += ' ';
            sp = false; t += ch;
        }
        t = decode_entities(t);
        Cell cl;
        if (!cur_num.empty()) { double d; if (parse_number_strict(cur_num, d)) cl.v = Value::number(d); else cl.v = parse_input(t); }
        else if (cur_text_fmt) cl.v = t.empty() ? Value() : Value::text(t);
        else { std::string hint; cl.v = parse_input(t, &hint); if (!hint.empty() && cur_style.numfmt == "General") cur_style.numfmt = hint; }
        Style s = cur_style;
        std::string key;
        { char b[32]; snprintf(b, sizeof b, "%zu", styles.size()); }
        uint32_t sid = 0;
        if (!(s == Style())) {
            bool found = false;
            for (uint32_t k = 0; k < styles.size(); ++k) if (styles[k] == s) { sid = k; found = true; break; }
            if (!found) { styles.push_back(s); sid = (uint32_t)styles.size() - 1; }
        }
        cl.style = sid;
        while ((int)grid.size() <= row) grid.emplace_back();
        if ((int)grid[row].size() <= col) grid[row].resize(col + 1);
        std::string f = cur_fmla;
        if (!f.empty() && f[0] == '=') f.erase(0, 1);
        grid[row][col] = {cl, f};
        for (int a = 0; a < cur_rs; ++a) for (int b = 0; b < cur_cs; ++b) occupy(row + a, col + b);
        col += cur_cs;
        text.clear();
    };
    while (i < html.size()) {
        if (html[i] == '<') {
            Tag t;
            read_tag(html, i, t);
            if (t.name == "table") {
                if (!t.close) { if (depth++ == 0) in_table = true; }
                else if (--depth <= 0) { finish_cell(); break; }
                continue;
            }
            if (!in_table || depth != 1) continue;
            if (t.name == "tr" && !t.close) { finish_cell(); ++row; col = 0; continue; }
            if ((t.name == "td" || t.name == "th") && !t.close) {
                finish_cell();
                if (row < 0) row = 0;
                while (is_occ(row, col)) ++col;
                in_cell = true; text.clear();
                cur_style = Style(); cur_text_fmt = false;
                if (t.name == "th") cur_style.font.bold = true;
                std::string cls = attr(t, "class");
                if (!cls.empty()) { size_t sp = 0; while (sp < cls.size()) { size_t e = cls.find(' ', sp); std::string c1 = cls.substr(sp, e == std::string::npos ? std::string::npos : e - sp); auto it = classes.find(c1); if (it != classes.end()) apply_css(cur_style, it->second, &cur_text_fmt); if (e == std::string::npos) break; sp = e + 1; } }
                apply_css(cur_style, attr(t, "style"), &cur_text_fmt);
                std::string al = lower_utf8(attr(t, "align"));
                if (al == "center") cur_style.halign = HAlign::Center; else if (al == "right") cur_style.halign = HAlign::Right;
                std::string bg = attr(t, "bgcolor");
                if (!bg.empty()) if (Argb a = parse_css_color(bg)) { cur_style.fill.pattern = FillPattern::Solid; cur_style.fill.fg = a; }
                cur_num = attr(t, "x:num");
                cur_fmla = attr(t, "x:fmla");
                cur_rs = std::max(1, atoi(attr(t, "rowspan").c_str()));
                cur_cs = std::max(1, atoi(attr(t, "colspan").c_str()));
                if (attr(t, "rowspan").empty()) cur_rs = 1;
                if (attr(t, "colspan").empty()) cur_cs = 1;
                continue;
            }
            if ((t.name == "td" || t.name == "th") && t.close) { finish_cell(); continue; }
            if (t.name == "tr" && t.close) { finish_cell(); continue; }
            if (in_cell && t.name == "br") { text += '\x01'; continue; }
            if (in_cell && (t.name == "b" || t.name == "strong") && !t.close) cur_style.font.bold = true;
            if (in_cell && (t.name == "i" || t.name == "em") && !t.close) cur_style.font.italic = true;
            if (in_cell && t.name == "u" && !t.close) cur_style.font.underline = Underline::Single;
            if (in_cell && (t.name == "font" || t.name == "span") && !t.close) {
                std::string c1 = attr(t, "color");
                if (!c1.empty()) if (Argb a = parse_css_color(c1)) cur_style.font.color = a;
                apply_css(cur_style, attr(t, "style"), &cur_text_fmt);
            }
            if (in_cell && (t.name == "p" || t.name == "div") && t.close) text += '\x01';
            continue;
        }
        if (in_cell) text += html[i];
        ++i;
    }
    if (grid.empty()) return false;
    out.rows = (int)grid.size();
    for (auto& r : grid) out.cols = std::max(out.cols, (int)r.size());
    for (auto& r : occupied) out.cols = std::max(out.cols, (int)r.size());
    if (out.cols == 0) return false;
    out.cells.assign((size_t)out.rows * out.cols, Cell());
    out.formula_text.assign(out.cells.size(), std::string());
    for (int r = 0; r < out.rows; ++r)
        for (int c = 0; c < (int)grid[r].size(); ++c) {
            out.cells[(size_t)r * out.cols + c] = grid[r][c].first;
            out.formula_text[(size_t)r * out.cols + c] = grid[r][c].second;
        }
    out.styles = styles;
    // trailing blank rows (e.g. trailing whitespace rows) trimmed by the caller's paste
    while (out.rows > 0) {
        bool any = false;
        for (int c = 0; c < out.cols; ++c) { auto& cl = out.cells[(size_t)(out.rows - 1) * out.cols + c]; if (!cl.blank() || cl.style) any = true; }
        if (any) break;
        --out.rows;
        out.cells.resize((size_t)out.rows * out.cols);
        out.formula_text.resize(out.cells.size());
    }
    return out.rows > 0;
}

} // namespace pcbp::sheet
