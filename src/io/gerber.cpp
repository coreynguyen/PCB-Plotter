// ============================================================================
// gerber.cpp — Gerber RS-274X / RS-274D / Excellon reader (portable C++20).
//
// Derived from Corey's gerberlab_core parser (TrayTagPrinter), converted to
// millimetre doubles and extended with aperture macros, regions, arcs,
// step & repeat, image transforms, RS-274D aperture lists and format guessing.
// See gerber.h for the feature list.
// ============================================================================
#include "gerber.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sstream>
#include <unordered_map>
#include <cstdio>
#include <utility>

namespace pcbp::gerber {

namespace {

// ---------------------------------------------------------------------------
// small utils
// ---------------------------------------------------------------------------
std::string upper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}
bool parse_double(const std::string& s, double& v) {
    if (s.empty()) return false;
    char* end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end && *end == '\0' && end != s.c_str();
}
double pow10i(int n) {
    double r = 1;
    for (int i = 0; i < n; ++i) r *= 10;
    return r;
}

std::vector<Vec2> ngon(Vec2 c, double r, int n, double rot_deg) {
    std::vector<Vec2> p;
    if (n < 3) n = 3;
    p.reserve(n);
    for (int i = 0; i < n; ++i) {
        double a = deg2rad(rot_deg + 360.0 * i / n);
        p.push_back({c.x + r * std::cos(a), c.y + r * std::sin(a)});
    }
    return p;
}

double cross(Vec2 o, Vec2 a, Vec2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

std::vector<Vec2> convex_hull(std::vector<Vec2> pts) {
    if (pts.size() < 3) return pts;
    std::sort(pts.begin(), pts.end(), [](Vec2 a, Vec2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    std::vector<Vec2> h(2 * pts.size());
    size_t k = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
        while (k >= 2 && cross(h[k - 2], h[k - 1], pts[i]) <= 0) --k;
        h[k++] = pts[i];
    }
    for (size_t i = pts.size() - 1, t = k + 1; i > 0; --i) {
        while (k >= t && cross(h[k - 2], h[k - 1], pts[i - 1]) <= 0) --k;
        h[k++] = pts[i - 1];
    }
    h.resize(k - 1);
    return h;
}

double seg_dist(Vec2 p, Vec2 a, Vec2 b) {
    Vec2 ab = b - a, ap = p - a;
    double l2 = ab.x * ab.x + ab.y * ab.y;
    double t = l2 > 0 ? (ap.x * ab.x + ap.y * ab.y) / l2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    return dist(p, a + ab * t);
}

bool point_in_poly(Vec2 p, const std::vector<Vec2>& poly) {
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const Vec2 &a = poly[i], &b = poly[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) in = !in;
    }
    return in;
}

double poly_edge_dist(Vec2 p, const std::vector<Vec2>& poly) {
    double d = 1e300;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) d = std::min(d, seg_dist(p, poly[j], poly[i]));
    return d;
}

// ---------------------------------------------------------------------------
// Aperture-macro expression evaluator
//   expr := term (('+'|'-') term)*
//   term := factor (('x'|'X'|'/') factor)*
//   factor := ('+'|'-') factor | number | '$'n | '(' expr ')'
// ---------------------------------------------------------------------------
struct ExprEval {
    const std::string& s;
    size_t i = 0;
    const std::map<int, double>& vars;
    bool ok = true;
    ExprEval(const std::string& str, const std::map<int, double>& v) : s(str), vars(v) {}
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i; }
    double expr() {
        double v = term();
        for (;;) {
            ws();
            if (i < s.size() && s[i] == '+') { ++i; v += term(); }
            else if (i < s.size() && s[i] == '-') { ++i; v -= term(); }
            else return v;
        }
    }
    double term() {
        double v = factor();
        for (;;) {
            ws();
            if (i < s.size() && (s[i] == 'x' || s[i] == 'X')) { ++i; v *= factor(); }
            else if (i < s.size() && s[i] == '/') { ++i; double d = factor(); v = d != 0 ? v / d : 0; }
            else return v;
        }
    }
    double factor() {
        ws();
        if (i >= s.size()) { ok = false; return 0; }
        char c = s[i];
        if (c == '+') { ++i; return factor(); }
        if (c == '-') { ++i; return -factor(); }
        if (c == '(') {
            ++i;
            double v = expr();
            ws();
            if (i < s.size() && s[i] == ')') ++i; else ok = false;
            return v;
        }
        if (c == '$') {
            ++i;
            int n = 0;
            while (i < s.size() && std::isdigit((unsigned char)s[i])) n = n * 10 + (s[i++] - '0');
            auto it = vars.find(n);
            return it == vars.end() ? 0.0 : it->second;
        }
        size_t j = i;
        while (j < s.size() && (std::isdigit((unsigned char)s[j]) || s[j] == '.')) ++j;
        if (j == i) { ok = false; ++i; return 0; }
        double v = std::strtod(s.substr(i, j - i).c_str(), nullptr);
        i = j;
        return v;
    }
};

double eval_expr(const std::string& s, const std::map<int, double>& vars, bool* ok = nullptr) {
    ExprEval e(s, vars);
    double v = e.expr();
    if (ok) *ok = e.ok;
    return v;
}

// Expand a macro definition into polygons (mm, relative to flash point).
void expand_macro(const std::vector<std::string>& body, const std::vector<double>& params,
                  double unit_mm, Aperture& ap, std::vector<std::string>& warns, int segs) {
    std::map<int, double> vars;
    for (size_t k = 0; k < params.size(); ++k) vars[(int)k + 1] = params[k];
    for (const std::string& raw : body) {
        std::string st = trim(raw);
        if (st.empty()) continue;
        if (st[0] == '0' && (st.size() == 1 || st[1] == ' ' || st[1] == ',')) continue;  // comment
        if (st[0] == '$') {
            size_t eq = st.find('=');
            if (eq == std::string::npos) continue;
            int n = std::atoi(st.c_str() + 1);
            vars[n] = eval_expr(st.substr(eq + 1), vars);
            continue;
        }
        // primitive: code,mod,mod,...
        std::vector<double> m;
        std::stringstream ss(st);
        std::string tok;
        while (std::getline(ss, tok, ',')) m.push_back(eval_expr(trim(tok), vars));
        if (m.empty()) continue;
        int code = (int)std::lround(m[0]);
        auto M = [&](size_t i) { return i < m.size() ? m[i] : 0.0; };
        auto U = [&](size_t i) { return M(i) * unit_mm; };
        auto push = [&](std::vector<Vec2> poly, double expo) {
            bool clr = expo == 0;
            if ((int)expo == 2) clr = !ap.macro_clear.empty() && !ap.macro_clear.back();
            ap.macro_polys.push_back(std::move(poly));
            ap.macro_clear.push_back(clr);
        };
        switch (code) {
        case 1: {  // circle: exp, dia, cx, cy, [rot]
            Vec2 c = rotate({U(3), U(4)}, M(5));
            push(ngon(c, U(2) / 2, segs, 0), M(1));
            break;
        }
        case 2:
        case 20: {  // vector line: exp, width, sx, sy, ex, ey, rot
            Vec2 s{U(3), U(4)}, e{U(5), U(6)};
            double w = U(2) / 2;
            Vec2 d = e - s;
            double l = d.len();
            Vec2 n = l > 0 ? Vec2{-d.y / l * w, d.x / l * w} : Vec2{0, w};
            std::vector<Vec2> p{s - n, e - n, e + n, s + n};
            for (auto& v : p) v = rotate(v, M(7));
            push(p, M(1));
            break;
        }
        case 21: {  // centre line: exp, w, h, cx, cy, rot
            double w = U(2) / 2, h = U(3) / 2;
            Vec2 c{U(4), U(5)};
            std::vector<Vec2> p{{c.x - w, c.y - h}, {c.x + w, c.y - h}, {c.x + w, c.y + h}, {c.x - w, c.y + h}};
            for (auto& v : p) v = rotate(v, M(6));
            push(p, M(1));
            break;
        }
        case 22: {  // lower-left line: exp, w, h, llx, lly, rot
            double w = U(2), h = U(3);
            Vec2 c{U(4), U(5)};
            std::vector<Vec2> p{c, {c.x + w, c.y}, {c.x + w, c.y + h}, {c.x, c.y + h}};
            for (auto& v : p) v = rotate(v, M(6));
            push(p, M(1));
            break;
        }
        case 4: {  // outline: exp, n, x0,y0 .. xn,yn, rot
            int n = (int)std::lround(M(2));
            std::vector<Vec2> p;
            for (int k = 0; k <= n; ++k) p.push_back({U(3 + 2 * k), U(4 + 2 * k)});
            double rot = M(3 + 2 * (n + 1));
            if (p.size() > 1 && dist(p.front(), p.back()) < 1e-9) p.pop_back();
            for (auto& v : p) v = rotate(v, rot);
            push(p, M(1));
            break;
        }
        case 5: {  // polygon: exp, n, cx, cy, dia, rot
            int n = (int)std::lround(M(2));
            Vec2 c{U(3), U(4)};
            auto p = ngon(c, U(5) / 2, n, 0);
            for (auto& v : p) v = rotate(v, M(6));
            push(p, M(1));
            break;
        }
        case 6: {  // moire: cx, cy, od, thick, gap, nrings, cw, cl, rot
            Vec2 c{U(1), U(2)};
            double od = U(3), th = U(4), gp = U(5), cw = U(7), cl = U(8), rot = M(9);
            int nr = (int)std::lround(M(6));
            double r = od / 2;
            for (int k = 0; k < nr && r > 0; ++k) {
                auto o = ngon(c, r, segs, 0);
                for (auto& v : o) v = rotate(v, rot);
                push(o, 1);
                if (r - th > 0) {
                    auto in = ngon(c, r - th, segs, 0);
                    for (auto& v : in) v = rotate(v, rot);
                    push(in, 0);
                }
                r -= th + gp;
            }
            std::vector<Vec2> h{{c.x - cl / 2, c.y - cw / 2}, {c.x + cl / 2, c.y - cw / 2}, {c.x + cl / 2, c.y + cw / 2}, {c.x - cl / 2, c.y + cw / 2}};
            std::vector<Vec2> v{{c.x - cw / 2, c.y - cl / 2}, {c.x + cw / 2, c.y - cl / 2}, {c.x + cw / 2, c.y + cl / 2}, {c.x - cw / 2, c.y + cl / 2}};
            for (auto& q : h) q = rotate(q, rot);
            for (auto& q : v) q = rotate(q, rot);
            push(h, 1);
            push(v, 1);
            break;
        }
        case 7: {  // thermal: cx, cy, od, id, gap, rot
            Vec2 c{U(1), U(2)};
            double od = U(3), id = U(4), gap = U(5), rot = M(6);
            double ro = od / 2, ri = id / 2, g = gap / 2;
            if (ro <= g) break;
            int n = std::max(4, segs / 4);
            for (int q = 0; q < 4; ++q) {
                std::vector<Vec2> p;
                double ao0 = std::asin(std::min(1.0, g / ro)), ao1 = kPi / 2 - ao0;
                for (int k = 0; k <= n; ++k) {
                    double a = ao0 + (ao1 - ao0) * k / n;
                    p.push_back({ro * std::cos(a), ro * std::sin(a)});
                }
                if (ri > g * std::sqrt(2.0)) {
                    double ai0 = std::asin(g / ri), ai1 = kPi / 2 - ai0;
                    for (int k = n; k >= 0; --k) {
                        double a = ai0 + (ai1 - ai0) * k / n;
                        p.push_back({ri * std::cos(a), ri * std::sin(a)});
                    }
                } else {
                    p.push_back({g, g});
                }
                for (auto& v : p) v = rotate(rotate(v, 90.0 * q) + c, rot);
                push(p, 1);
            }
            break;
        }
        default:
            warns.push_back("AM " + ap.macro_name + ": unsupported primitive " + std::to_string(code));
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Gerber parser
// ---------------------------------------------------------------------------
struct GerberParser {
    const ParseOptions& opt;
    Layer& L;
    const std::string& t;
    size_t pos = 0;

    Format fmt;
    double unit_mm = 25.4;
    int cur_d = 0;
    int interp = 1;              // 1 linear, 2 cw, 3 ccw
    bool multi_quad = false;
    bool clear = false;
    Vec2 cur{0, 0};
    bool absolute = true;
    int last_dop = 0;
    bool warned_modal = false;
    bool done = false;

    bool in_region = false;
    std::vector<Vec2> contour;

    std::map<std::string, std::vector<std::string>> macros;
    std::map<int, Aperture> file_aps;
    std::set<int> used_d;

    bool sr_active = false;
    int sr_x = 1, sr_y = 1;
    double sr_i = 0, sr_j = 0;
    size_t sr_start = 0;

    Vec2 img_of{0, 0};
    bool mir_a = false, mir_b = false;
    double sf_a = 1, sf_b = 1;
    // per-axis %FS digits (Y differs from X), end-of-block mode, image rotate / axis swap
    int y_int = -1, y_frac = -1;
    bool eol_eob = false;
    double ir_deg = 0;
    bool as_swap = false;
    // aperture transformations (%LR %LM %LS)
    double lr = 0, ls = 1;
    bool lmx = false, lmy = false;
    std::map<std::string, int> derived;
    // block apertures (%AB)
    std::map<int, std::vector<Object>> blocks;
    std::vector<std::pair<int, size_t>> block_stack;

    GerberParser(const ParseOptions& o, Layer& l, const std::string& text) : opt(o), L(l), t(text) {
        fmt = o.fmt;
        unit_mm = fmt.inch ? 25.4 : 1.0;
        absolute = fmt.absolute;
    }

    void warn(const std::string& m) {
        if ((int)L.warnings.size() < opt.max_warnings) L.warnings.push_back(m);
    }

    double decode(const std::string& s, bool yaxis = false) const {
        if (s.find('.') != std::string::npos) return std::strtod(s.c_str(), nullptr) * unit_mm;
        const int fi = yaxis && y_int >= 0 ? y_int : fmt.int_digits, ff = yaxis && y_int >= 0 ? y_frac : fmt.frac_digits;
        size_t i = 0;
        bool neg = false;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; ++i; }
        int n = 0;
        double v = 0;
        for (; i < s.size(); ++i)
            if (std::isdigit((unsigned char)s[i])) { v = v * 10 + (s[i] - '0'); ++n; }
        int total = fi + ff;
        if (!fmt.lead_supp && n < total) v *= pow10i(total - n);
        v /= pow10i(ff);
        return (neg ? -v : v) * unit_mm;
    }

    // ---------------- parameter blocks ----------------
    void param_block() {
        std::vector<std::string> sub;
        std::string cur_s;
        while (pos < t.size() && t[pos] != '%') {
            char c = t[pos++];
            if (c == '\r' || c == '\n' || c == '\0') continue;
            if (c == '*') { sub.push_back(cur_s); cur_s.clear(); }
            else cur_s.push_back(c);
        }
        if (!trim(cur_s).empty()) sub.push_back(cur_s);
        if (pos < t.size()) ++pos;
        while (!sub.empty() && trim(sub.front()).empty()) sub.erase(sub.begin());
        if (sub.empty()) return;
        std::string first = trim(sub.front());
        if (first.rfind("AM", 0) == 0) {
            std::string name = first.substr(2);
            std::vector<std::string> body(sub.begin() + 1, sub.end());
            macros[name] = body;
            return;
        }
        for (auto& s : sub) {
            std::string st = trim(s);
            if (!st.empty()) param(st);
        }
    }

    void param(const std::string& s) {
        if (s.size() < 2) return;
        std::string k = s.substr(0, 2);
        L.rs274x = true;
        if (k == "FS") return fs(s);
        if (k == "MO") {
            if (opt.force_format) return;
            if (s.compare(2, 2, "IN") == 0) { fmt.inch = true; unit_mm = 25.4; }
            else if (s.compare(2, 2, "MM") == 0) { fmt.inch = false; unit_mm = 1.0; }
            else warn("MO: expected IN or MM");
            L.format_from_file = true;
            return;
        }
        if (k == "AD") return ad(s);
        if (k == "LP") { clear = s.size() > 2 && s[2] == 'C'; return; }
        if (k == "SR") return sr(s);
        if (k == "OF") {
            img_of = {axis_val(s, 'A') * unit_mm, axis_val(s, 'B') * unit_mm};
            return;
        }
        if (k == "MI") { mir_a = axis_val(s, 'A') != 0; mir_b = axis_val(s, 'B') != 0; return; }
        if (k == "SF") {
            double a = axis_val(s, 'A', 1), b = axis_val(s, 'B', 1);
            sf_a = a != 0 ? a : 1; sf_b = b != 0 ? b : 1;
            return;
        }
        if (k == "IP") { L.negative = s.find("NEG") != std::string::npos; return; }
        if (k == "IR") { ir_deg = std::atof(s.c_str() + 2); return; }
        if (k == "AS") { as_swap = s.find("AYBX") != std::string::npos; return; }
        if (k == "LR") { lr = std::atof(s.c_str() + 2); return; }
        if (k == "LS") { double v = std::atof(s.c_str() + 2); ls = v > 0 ? v : 1; return; }
        if (k == "LM") { std::string m = s.substr(2); lmx = m.find('X') != std::string::npos; lmy = m.find('Y') != std::string::npos; return; }
        if (k == "AB") {
            if (s.size() > 3 && (s[2] == 'D' || std::isdigit((unsigned char)s[2]))) {
                int d = std::atoi(s.c_str() + (s[2] == 'D' ? 3 : 2));
                block_stack.push_back({d, L.objs.size()});
            } else if (!block_stack.empty()) {
                auto [d, start] = block_stack.back();
                block_stack.pop_back();
                std::vector<Object> body(L.objs.begin() + (long)start, L.objs.end());
                L.objs.resize(start);
                blocks[d] = std::move(body);
            }
            return;
        }
        if (k == "IN" || k == "LN") { if (L.name.empty()) L.name = s.substr(2); return; }
        if (k == "TF" || k == "TA" || k == "TO" || k == "TD" || k == "AS" || k == "IJ" || k == "IO" ||
            k == "KO" || k == "PF" || k == "IC")
            return;  // X2 attributes and rarely-used parameters: ignored
        warn("Unknown parameter %" + s.substr(0, 16));
    }

    double axis_val(const std::string& s, char axis, double def = 0) {
        size_t p = s.find(axis, 2);
        if (p == std::string::npos) return def;
        size_t q = p + 1;
        while (q < s.size() && (std::isdigit((unsigned char)s[q]) || s[q] == '.' || s[q] == '-' || s[q] == '+')) ++q;
        if (q == p + 1) return def;
        return std::strtod(s.substr(p + 1, q - p - 1).c_str(), nullptr);
    }

    void fs(const std::string& s) {
        if (opt.force_format) return;
        L.format_from_file = true;
        for (size_t i = 2; i < s.size(); ++i) {
            char c = s[i];
            if (c == 'L') fmt.lead_supp = true;
            else if (c == 'T') fmt.lead_supp = false;
            else if (c == 'D') fmt.lead_supp = true;  // explicit decimal (rare)
            else if (c == 'A') fmt.absolute = true;
            else if (c == 'I') fmt.absolute = false;
            else if ((c == 'X' || c == 'Y') && i + 2 < s.size() &&
                     std::isdigit((unsigned char)s[i + 1]) && std::isdigit((unsigned char)s[i + 2])) {
                if (c == 'X') { fmt.int_digits = s[i + 1] - '0'; fmt.frac_digits = s[i + 2] - '0'; }
                else { y_int = s[i + 1] - '0'; y_frac = s[i + 2] - '0'; }
                i += 2;
            }
        }
        if (y_int == fmt.int_digits && y_frac == fmt.frac_digits) y_int = y_frac = -1;
        absolute = fmt.absolute;
    }

    void ad(const std::string& s) {
        size_t i = 2;
        if (i < s.size() && s[i] == 'D') ++i;
        int d = 0;
        size_t i0 = i;
        while (i < s.size() && std::isdigit((unsigned char)s[i])) d = d * 10 + (s[i++] - '0');
        if (i == i0) { warn("AD: missing D-code"); return; }
        size_t comma = s.find(',', i);
        std::string name = s.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
        std::vector<double> p;
        if (comma != std::string::npos) {
            std::string rest = s.substr(comma + 1);
            size_t a = 0;
            for (;;) {
                size_t b = rest.find('X', a);
                std::string tok = rest.substr(a, b == std::string::npos ? std::string::npos : b - a);
                p.push_back(std::strtod(tok.c_str(), nullptr));
                if (b == std::string::npos) break;
                a = b + 1;
            }
        }
        auto P = [&](size_t k) { return k < p.size() ? p[k] : 0.0; };
        Aperture ap;
        ap.dcode = d;
        if (name == "C") { ap.shape = Aperture::Circle; ap.w = ap.h = P(0) * unit_mm; ap.hole = P(1) * unit_mm; }
        else if (name == "R") { ap.shape = Aperture::Rect; ap.w = P(0) * unit_mm; ap.h = P(1) * unit_mm; ap.hole = P(2) * unit_mm; }
        else if (name == "O") { ap.shape = Aperture::Obround; ap.w = P(0) * unit_mm; ap.h = P(1) * unit_mm; ap.hole = P(2) * unit_mm; }
        else if (name == "P") {
            ap.shape = Aperture::Polygon; ap.w = ap.h = P(0) * unit_mm;
            ap.verts = (int)std::lround(P(1)); ap.rot = P(2); ap.hole = P(3) * unit_mm;
        } else {
            auto it = macros.find(name);
            ap.shape = Aperture::Macro;
            ap.macro_name = name;
            if (it == macros.end()) { warn("AD D" + std::to_string(d) + ": undefined macro '" + name + "'"); }
            else expand_macro(it->second, p, unit_mm, ap, L.warnings, 32);
            BBox bb = aperture_bbox(ap);
            ap.w = bb.empty() ? 0 : bb.w();
            ap.h = bb.empty() ? 0 : bb.h();
        }
        file_aps[d] = ap;
    }

    void sr_close() {
        if (!sr_active) return;
        size_t end = L.objs.size();
        for (int iy = 0; iy < sr_y; ++iy)
            for (int ix = 0; ix < sr_x; ++ix) {
                if (!ix && !iy) continue;
                Vec2 off{ix * sr_i, iy * sr_j};
                for (size_t k = sr_start; k < end; ++k) {
                    Object o = L.objs[k];
                    o.a += off; o.b += off; o.center += off;
                    for (auto& v : o.poly) v += off;
                    L.objs.push_back(std::move(o));
                }
            }
        sr_active = false;
    }

    void sr(const std::string& s) {
        sr_close();
        int x = (int)axis_val(s, 'X', 1), y = (int)axis_val(s, 'Y', 1);
        if (x > 1 || y > 1) {
            sr_active = true;
            sr_x = std::max(1, x); sr_y = std::max(1, y);
            sr_i = axis_val(s, 'I') * unit_mm; sr_j = axis_val(s, 'J') * unit_mm;
            sr_start = L.objs.size();
        }
    }

    // ---------------- data blocks ----------------
    void data_block() {
        std::string s;
        while (pos < t.size() && t[pos] != '*' && t[pos] != '%' && t[pos] != '$') {
            char c = t[pos++];
            if (eol_eob && (c == '\r' || c == '\n')) break;
            if (c == '\r' || c == '\n' || c == '\0' || c == ' ' || c == '\t') continue;
            s.push_back(c);
        }
        if (pos < t.size() && (t[pos] == '*' || t[pos] == '$')) ++pos;
        if (!s.empty()) data(s);
    }

    void data(const std::string& s) {
        if (s.rfind("G04", 0) == 0 || (s.rfind("G4", 0) == 0 && (s.size() == 2 || !std::isdigit((unsigned char)s[2])))) return;
        int g = -1, d = -1, m = -1;
        bool hx = false, hy = false, hi = false, hj = false;
        double x = 0, y = 0, ii = 0, jj = 0;
        size_t i = 0;
        while (i < s.size()) {
            char c = (char)std::toupper((unsigned char)s[i]);
            if (c == 'G' || c == 'D' || c == 'M' || c == 'N') {
                size_t j = ++i;
                while (j < s.size() && std::isdigit((unsigned char)s[j])) ++j;
                if (j == i) continue;
                int v = std::atoi(s.substr(i, j - i).c_str());
                i = j;
                if (c == 'G') { if (g >= 0) gcode(g); g = v; }
                else if (c == 'D') d = v;
                else if (c == 'M') m = v;
                continue;
            }
            if (c == 'X' || c == 'Y' || c == 'I' || c == 'J') {
                size_t j = ++i;
                if (j < s.size() && (s[j] == '+' || s[j] == '-')) ++j;
                while (j < s.size() && (std::isdigit((unsigned char)s[j]) || s[j] == '.')) ++j;
                if (j == i) continue;
                double v = decode(s.substr(i, j - i), c == 'Y' || c == 'J');
                i = j;
                if (c == 'X') { x = v; hx = true; }
                else if (c == 'Y') { y = v; hy = true; }
                else if (c == 'I') { ii = v; hi = true; }
                else { jj = v; hj = true; }
                continue;
            }
            ++i;
        }
        if (g >= 0) gcode(g);
        if (d >= 10 || (d > 3 && d < 10 && (file_aps.count(d) || opt.apertures.count(d) || blocks.count(d)))) {
            cur_d = d;
            if (!hx && !hy) return;
            d = -1;
        }
        if (d < 0 && (hx || hy)) {
            if (last_dop == 0) {
                if (!warned_modal) warn("Coordinates without D01/D02/D03 before any operation; treated as move");
                warned_modal = true;
                d = 2;
            } else d = last_dop;
        }
        if (d >= 1 && d <= 3) {
            Vec2 tgt = cur;
            if (absolute) { if (hx) tgt.x = x; if (hy) tgt.y = y; }
            else { if (hx) tgt.x += x; if (hy) tgt.y += y; }
            last_dop = d;
            if (d == 1) d01(tgt, ii, jj, hi, hj);
            else if (d == 2) d02(tgt);
            else d03(tgt);
            cur = tgt;
        } else if (d > 3 && d < 10) {
            warn("Unknown D code D" + std::to_string(d));
        }
        if (m == 0 || m == 2 || m == 30) done = true;
    }

    void gcode(int g) {
        switch (g) {
        case 1: interp = 1; break;
        case 2: interp = 2; break;
        case 3: interp = 3; break;
        case 36: in_region = true; contour.clear(); break;
        case 37: close_contour(); in_region = false; break;
        case 70: if (!opt.force_format) { fmt.inch = true; unit_mm = 25.4; } break;
        case 71: if (!opt.force_format) { fmt.inch = false; unit_mm = 1.0; } break;
        case 74: multi_quad = false; break;
        case 75: multi_quad = true; break;
        case 90: absolute = true; break;
        case 91: absolute = false; break;
        default: break;  // G54/G55/G10.. etc: no-op
        }
    }

    void close_contour() {
        if (contour.size() >= 3) {
            Object o;
            o.kind = Object::Region;
            o.clear = clear;
            if (dist(contour.front(), contour.back()) < 1e-9) contour.pop_back();
            o.poly = std::move(contour);
            if (o.poly.size() >= 3) L.objs.push_back(std::move(o));
        }
        contour.clear();
    }

    Object make_arc(Vec2 s, Vec2 e, double i, double j, bool cw) {
        Object o;
        o.kind = Object::Arc;
        o.a = s; o.b = e; o.cw = cw; o.dcode = cur_d; o.clear = clear;
        if (multi_quad) {
            o.center = {s.x + i, s.y + j};
            return o;
        }
        double best = 1e300;
        Vec2 bc{s.x + i, s.y + j};
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2) {
                Vec2 c{s.x + sx * std::fabs(i), s.y + sy * std::fabs(j)};
                Object tmp = o;
                tmp.center = c;
                double sw = std::fabs(arc_sweep_deg(tmp));
                if (dist(s, e) < 1e-9) sw = 0;
                double err = std::fabs(dist(c, s) - dist(c, e));
                if (sw > 90.5) err += 1e6;
                if (err < best) { best = err; bc = c; }
            }
        o.center = bc;
        if (dist(s, e) < 1e-9) { o.kind = Object::Draw; }   // degenerate single-quadrant
        return o;
    }

    void d01(Vec2 tgt, double i, double j, bool, bool) {
        if (in_region) {
            if (contour.empty()) contour.push_back(cur);
            if (interp == 1) contour.push_back(tgt);
            else {
                Object a = make_arc(cur, tgt, i, j, interp == 2);
                auto pts = arc_points(a, opt.arc_segs);
                for (size_t k = 1; k < pts.size(); ++k) contour.push_back(pts[k]);
            }
            return;
        }
        used_d.insert(cur_d);
        if (interp == 1) {
            Object o;
            o.kind = Object::Draw;
            o.a = cur; o.b = tgt; o.dcode = cur_d; o.clear = clear;
            L.objs.push_back(o);
        } else {
            L.objs.push_back(make_arc(cur, tgt, i, j, interp == 2));
        }
    }
    void d02(Vec2 tgt) {
        if (in_region) { close_contour(); contour.push_back(tgt); }
    }
    void d03(Vec2 tgt) {
        if (in_region) { warn("D03 inside region ignored"); return; }
        auto bl = blocks.find(cur_d);
        if (bl != blocks.end()) {   // %AB block aperture: replicate its objects at the flash point
            for (Object o : bl->second) {
                o.a += tgt; o.b += tgt; o.center += tgt;
                for (auto& v : o.poly) v += tgt;
                o.clear = o.clear != clear;
                L.objs.push_back(std::move(o));
            }
            return;
        }
        int d = cur_d;
        if (lr != 0 || ls != 1 || lmx || lmy) d = derived_ap(cur_d);
        used_d.insert(d);
        Object o;
        o.kind = Object::Flash;
        o.a = tgt; o.dcode = d; o.clear = clear;
        L.objs.push_back(o);
    }

    // aperture transformed by %LM/%LR/%LS -> derived D-code (base + 100000*k)
    int derived_ap(int base) {
        char key[96];
        std::snprintf(key, sizeof key, "%d|%.6f|%.6f|%d%d", base, lr, ls, lmx, lmy);
        auto it = derived.find(key);
        if (it != derived.end()) return it->second;
        const Aperture* src = nullptr;
        if (auto f = file_aps.find(base); f != file_aps.end()) src = &f->second;
        else if (auto g = opt.apertures.find(base); g != opt.apertures.end()) src = &g->second;
        if (!src) return base;
        int nd = base + 100000 * (int)(derived.size() + 1);
        Aperture a = *src;
        a.dcode = nd;
        if (src->shape == Aperture::Circle) {
            a.w *= ls; a.h *= ls; a.hole *= ls;
        } else {
            std::vector<bool> clr;
            auto polys = flash_polys(*src, {0, 0}, 48, &clr);
            const double c = std::cos(deg2rad(lr)), sn = std::sin(deg2rad(lr));
            a.shape = Aperture::Macro;
            a.macro_name = (src->macro_name.empty() ? std::string("D") + std::to_string(base) : src->macro_name) + " (transformed)";
            a.macro_polys.clear();
            a.macro_clear = clr;
            for (auto& poly : polys) {
                for (auto& v : poly) {
                    if (lmx) v.x = -v.x;
                    if (lmy) v.y = -v.y;
                    v = Vec2{v.x * ls, v.y * ls};
                    v = Vec2{v.x * c - v.y * sn, v.x * sn + v.y * c};
                }
                a.macro_polys.push_back(std::move(poly));
            }
            a.hole = 0;
            BBox bb = aperture_bbox(a);
            a.w = bb.empty() ? 0 : bb.w();
            a.h = bb.empty() ? 0 : bb.h();
        }
        file_aps[nd] = a;
        derived[key] = nd;
        return nd;
    }

    void apply_image_xform() {
        bool ident = !mir_a && !mir_b && sf_a == 1 && sf_b == 1 && img_of.x == 0 && img_of.y == 0 && ir_deg == 0 && !as_swap;
        if (ident) return;
        const double rc = std::cos(deg2rad(ir_deg)), rs = std::sin(deg2rad(ir_deg));
        auto f = [&](Vec2 p) {
            if (as_swap) std::swap(p.x, p.y);
            if (mir_a) p.x = -p.x;
            if (mir_b) p.y = -p.y;
            p = Vec2{p.x * sf_a + img_of.x, p.y * sf_b + img_of.y};
            if (ir_deg != 0) p = Vec2{p.x * rc - p.y * rs, p.x * rs + p.y * rc};
            return p;
        };
        bool flip = (mir_a != mir_b) != as_swap;
        for (auto& o : L.objs) {
            o.a = f(o.a); o.b = f(o.b); o.center = f(o.center);
            for (auto& v : o.poly) v = f(v);
            if (flip) o.cw = !o.cw;
        }
    }

    void run() {
        {   // 274D without '*': CR/LF ends a block (ViewMate EOB option)
            const size_t n = std::min<size_t>(t.size(), 4096);
            bool star = false, pct = false;
            for (size_t i = 0; i < n; ++i) { if (t[i] == '*' || t[i] == '$') star = true; if (t[i] == '%') pct = true; }
            eol_eob = !star && !pct;
        }
        while (pos < t.size() && !done) {
            char c = t[pos];
            if (c == '%') { ++pos; param_block(); }
            else if (c == '*' || (unsigned char)c <= ' ') ++pos;
            else data_block();
        }
        if (in_region) close_contour();
        sr_close();
        apply_image_xform();
        L.fmt = fmt;
        L.aps = opt.apertures;
        for (auto& [d, a] : file_aps) L.aps[d] = a;
        std::vector<int> missing;
        for (int d : used_d)
            if (!L.aps.count(d)) {
                Aperture a;
                a.dcode = d;
                a.defined = false;
                L.aps[d] = a;
                missing.push_back(d);
            }
        if (!missing.empty()) {
            std::string m = "Undefined apertures used (zero size):";
            for (int d : missing) m += " D" + std::to_string(d);
            warn(m);
        }
    }
};

// ---------------------------------------------------------------------------
// Excellon parser
// ---------------------------------------------------------------------------
struct DrillParser {
    const ParseOptions& opt;
    Layer& L;
    Format fmt;
    bool units_known = false;
    bool fmt_explicit = false;
    int tool = -1;
    Vec2 cur{0, 0};
    bool absolute = true;
    bool rout = false, tool_down = false;
    int gmode = 5;
    Vec2 org{0, 0};                  // G93 zero set
    bool pat_rec = false;            // M25 .. M01 pattern recording
    size_t pat_start = 0, pat_end = 0;
    Vec2 pat_off{0, 0};
    bool sieb = false;               // Sieb & Meyer (%%3000) X..Y..T# lines

    DrillParser(const ParseOptions& o, Layer& l) : opt(o), L(l) {
        fmt.inch = true; fmt.int_digits = 2; fmt.frac_digits = 4; fmt.lead_supp = true;
    }

    double unit() const { return fmt.inch ? 25.4 : 1.0; }

    double decode(const std::string& s) const {
        if (s.find('.') != std::string::npos) return std::strtod(s.c_str(), nullptr) * unit();
        bool neg = false;
        size_t i = 0;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) { neg = s[i] == '-'; ++i; }
        double v = 0;
        int n = 0;
        for (; i < s.size(); ++i)
            if (std::isdigit((unsigned char)s[i])) { v = v * 10 + (s[i] - '0'); ++n; }
        int total = fmt.int_digits + fmt.frac_digits;
        if (!fmt.lead_supp && n < total) v *= pow10i(total - n);
        v /= pow10i(fmt.frac_digits);
        return (neg ? -v : v) * unit();
    }

    void set_units(bool inch, const std::string& rest) {
        fmt.inch = inch;
        units_known = true;
        if (!fmt_explicit) { fmt.int_digits = inch ? 2 : 3; fmt.frac_digits = inch ? 4 : 3; }
        // Excellon: LZ = leading zeros present (trailing suppressed); TZ = trailing present
        if (rest.find("LZ") != std::string::npos) fmt.lead_supp = false;
        if (rest.find("TZ") != std::string::npos) fmt.lead_supp = true;
        size_t p = rest.find_first_of("0123456789");
        if (p != std::string::npos) {
            size_t dot = rest.find('.', p);
            if (dot != std::string::npos) {
                int a = 0, b = 0;
                for (size_t k = p; k < dot && std::isdigit((unsigned char)rest[k]); ++k) ++a;
                for (size_t k = dot + 1; k < rest.size() && std::isdigit((unsigned char)rest[k]); ++k) ++b;
                if (a && b) { fmt.int_digits = a; fmt.frac_digits = b; fmt_explicit = true; }
            }
        }
    }

    bool read_xy(const std::string& s, size_t& i, Vec2& p) {
        bool any = false;
        Vec2 np = absolute ? cur - org : Vec2{0, 0};
        while (i < s.size() && (s[i] == 'X' || s[i] == 'Y')) {
            char c = s[i++];
            size_t j = i;
            while (j < s.size() && (std::isdigit((unsigned char)s[j]) || s[j] == '.' || s[j] == '-' || s[j] == '+')) ++j;
            if (j > i) {
                double v = decode(s.substr(i, j - i));
                if (c == 'X') np.x = v; else np.y = v;
                any = true;
            }
            i = j;
        }
        p = absolute ? np + org : cur + np;
        return any;
    }

    void define_tool(int tn, double dia_mm) {
        Aperture a;
        a.dcode = 10 + tn - 1;
        a.shape = Aperture::Circle;
        a.w = a.h = a.hole = dia_mm;
        a.usage = "DRILL";
        a.comment = "T" + std::to_string(tn);
        L.aps[a.dcode] = a;
    }

    void line(std::string s) {
        s = upper(trim(s));
        if (s.empty()) return;
        if (s[0] == ';') {
            size_t ff = s.find("FILE_FORMAT=");   // Altium: ;FILE_FORMAT=2:5
            if (ff != std::string::npos && !opt.force_format) {
                int a = 0, b = 0;
                if (std::sscanf(s.c_str() + ff + 12, "%d:%d", &a, &b) == 2 && a > 0 && b > 0 && a < 8 && b < 8) {
                    fmt.int_digits = a; fmt.frac_digits = b; fmt_explicit = true;
                }
            }
            return;
        }
        if (s.rfind("%%3000", 0) == 0 || s.rfind("%%", 0) == 0) { sieb = true; if (!units_known) { fmt.inch = false; fmt.int_digits = 4; fmt.frac_digits = 2; } return; }
        if (s == "%" || s == "M95") return;
        if (s.rfind("M97", 0) == 0 || s.rfind("M98", 0) == 0) return;   // canned text: not drilled
        if (s == "M25") { pat_rec = true; pat_start = L.objs.size(); pat_off = {0, 0}; return; }
        if (s == "M01") { if (pat_rec) { pat_rec = false; pat_end = L.objs.size(); } return; }
        if (s == "M08") { pat_start = pat_end = 0; return; }
        if (s.rfind("M02", 0) == 0 && s.size() > 3 && (s[3] == 'X' || s[3] == 'Y')) {
            size_t i = 3;
            bool ab = absolute;
            absolute = false;
            Vec2 save = cur;
            cur = {0, 0};
            Vec2 d;
            read_xy(s, i, d);
            cur = save;
            absolute = ab;
            pat_off += d;
            for (size_t k = pat_start; k < pat_end && k < L.objs.size(); ++k) {
                Object o = L.objs[k];
                o.a += pat_off; o.b += pat_off; o.center += pat_off;
                L.objs.push_back(o);
            }
            return;
        }
        if (s[0] == 'R' && s.size() > 1 && std::isdigit((unsigned char)s[1])) {   // R## repeat hole
            size_t i = 1;
            int n = 0;
            while (i < s.size() && std::isdigit((unsigned char)s[i])) n = n * 10 + (s[i++] - '0');
            bool ab = absolute;
            absolute = false;
            Vec2 d;
            Vec2 save = cur;
            cur = {0, 0};
            read_xy(s, i, d);
            cur = save;
            absolute = ab;
            for (int k = 0; k < n && k < 100000; ++k) {
                cur += d;
                if (tool >= 0) { Object o; o.kind = Object::Flash; o.dcode = tool; o.a = cur; L.objs.push_back(o); }
            }
            return;
        }
        if (s.rfind("M48", 0) == 0) return;
        if (s.rfind("INCH", 0) == 0) { set_units(true, s.substr(4)); return; }
        if (s.rfind("METRIC", 0) == 0) { set_units(false, s.substr(6)); return; }
        if (s == "M72") { fmt.inch = true; units_known = true; return; }
        if (s == "M71") { fmt.inch = false; units_known = true; return; }
        if (s.rfind("FMAT", 0) == 0 || s.rfind("VER", 0) == 0 || s.rfind("ICI", 0) == 0) {
            if (s.rfind("ICI,ON", 0) == 0) absolute = false;
            return;
        }
        if (s == "M30" || s == "M00" || s == "M02") return;
        if (s == "M15") { tool_down = true; return; }
        if (s == "M16" || s == "M17") { tool_down = false; return; }
        if (s[0] == 'T') {
            size_t i = 1;
            int tn = 0;
            while (i < s.size() && std::isdigit((unsigned char)s[i])) tn = tn * 10 + (s[i++] - '0');
            size_t c = s.find('C', i);
            if (c != std::string::npos) {
                size_t j = c + 1;
                while (j < s.size() && (std::isdigit((unsigned char)s[j]) || s[j] == '.')) ++j;
                double dia = std::strtod(s.substr(c + 1, j - c - 1).c_str(), nullptr);
                if (tn > 0) define_tool(tn, dia * unit());
                tool = tn > 0 ? 10 + tn - 1 : -1;
            } else tool = tn > 0 ? 10 + tn - 1 : -1;
            return;
        }
        size_t i = 0;
        if (s[0] == 'G') {
            int g = std::atoi(s.c_str() + 1);
            i = 1;
            while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
            if (g == 93) { Vec2 p; size_t k = i; Vec2 sc = cur; cur = {0, 0}; Vec2 so = org; org = {0, 0}; bool ab = absolute; absolute = true;
                           if (read_xy(s, k, p)) org = p; else org = so; cur = sc; absolute = ab; return; }
            if (g == 90) { absolute = true; return; }
            if (g == 91) { absolute = false; return; }
            if (g == 0) { rout = true; gmode = 0; }
            else if (g == 1) { rout = true; gmode = 1; }
            else if (g == 5) { rout = false; gmode = 5; return; }
            else if (g == 2 || g == 3) { rout = true; gmode = g; }
            else if (i >= s.size() || (s[i] != 'X' && s[i] != 'Y')) return;
        }
        if (i < s.size() && (s[i] == 'X' || s[i] == 'Y')) {
            Vec2 p;
            if (!read_xy(s, i, p)) return;
            if (rout) {
                if (gmode != 0 && tool_down && tool >= 0) {
                    Object o; o.kind = Object::Draw; o.dcode = tool; o.a = cur; o.b = p;
                    if (gmode == 2 || gmode == 3) {   // rout arc: A radius or I/J centre offset
                        size_t pa = s.find('A', i), pi = s.find('I', i), pj = s.find('J', i);
                        auto num = [&](size_t at) { return at == std::string::npos ? 0.0 : decode(s.substr(at + 1, s.find_first_not_of("0123456789.+-", at + 1) - at - 1)); };
                        const bool cw = gmode == 2;
                        bool ok = false;
                        Vec2 c;
                        if (pa != std::string::npos) {
                            double r = std::fabs(num(pa));
                            Vec2 m = (cur + p) * 0.5, d = p - cur;
                            double h2 = r * r - (d.x * d.x + d.y * d.y) / 4;
                            double l = std::sqrt(d.x * d.x + d.y * d.y);
                            if (l > 1e-9) {
                                double h = std::sqrt(std::max(0.0, h2));
                                Vec2 n{-d.y / l, d.x / l};
                                c = cw ? m - n * h : m + n * h;   // minor arc
                                ok = true;
                            }
                        } else if (pi != std::string::npos || pj != std::string::npos) {
                            c = Vec2{cur.x + num(pi), cur.y + num(pj)};
                            ok = true;
                        }
                        if (ok) { o.kind = Object::Arc; o.center = c; o.cw = cw; }
                    }
                    L.objs.push_back(o);
                }
                cur = p;
                return;
            }
            // G85 slot?
            size_t g85 = s.find("G85", i);
            if (g85 != std::string::npos) {
                size_t k = g85 + 3;
                Vec2 q;
                Vec2 save = cur;
                cur = p;
                if (read_xy(s, k, q) && tool >= 0) {
                    Object o; o.kind = Object::Draw; o.dcode = tool; o.a = p; o.b = q;
                    L.objs.push_back(o);
                }
                cur = q;
                (void)save;
                return;
            }
            cur = p;
            if (sieb || (i < s.size() && s[i] == 'T')) {   // Sieb & Meyer: tool on the hit line
                size_t tpos = s.find('T', i);
                if (tpos != std::string::npos) {
                    int tn = std::atoi(s.c_str() + tpos + 1);
                    if (tn > 0) tool = 10 + tn - 1;
                }
            }
            if (tool >= 0) {
                Object o; o.kind = Object::Flash; o.dcode = tool; o.a = p;
                L.objs.push_back(o);
            } else {
                L.warnings.push_back("Drill hit before tool selection ignored");
            }
        }
    }

    void run(const std::string& text) {
        L.excellon = true;
        if (opt.force_format) { fmt = opt.fmt; fmt_explicit = true; }
        std::string ln;
        for (char c : text) {
            if (c == '\n' || c == '\r') { if (!ln.empty()) line(ln); ln.clear(); }
            else if (c) ln.push_back(c);
        }
        if (!ln.empty()) line(ln);
        if (!units_known && !opt.force_format) L.warnings.push_back("Excellon: no INCH/METRIC header; assumed inch 2.4");
        for (auto& o : L.objs)
            if (!L.aps.count(o.dcode)) {
                Aperture a; a.dcode = o.dcode; a.defined = false; L.aps[o.dcode] = a;
                L.warnings.push_back("Tool without diameter: D" + std::to_string(o.dcode));
            }
        L.fmt = fmt;
        L.format_from_file = units_known;
    }
};

// shape name helpers ------------------------------------------------------------
bool shape_word(const std::string& w0, Aperture::Shape& sh, int& verts, bool& square, std::string& note) {
    std::string w = upper(w0);
    square = false;
    verts = 0;
    if (w == "ROUND" || w == "CIRCLE" || w == "CIRC" || w == "RD" || w == "RND" || w == "C" || w == "CIR") { sh = Aperture::Circle; return true; }
    if (w == "SQUARE" || w == "SQ" || w == "SQR" || w == "S") { sh = Aperture::Rect; square = true; return true; }
    if (w == "RECT" || w == "RECTANGLE" || w == "RECTANGULAR" || w == "REC" || w == "R") { sh = Aperture::Rect; return true; }
    if (w == "OBLONG" || w == "OBROUND" || w == "OVAL" || w == "OB" || w == "OBL" || w == "O" || w == "FINGER") { sh = Aperture::Obround; return true; }
    if (w == "OCTAGON" || w == "OCT" || w == "OCTAGONAL") { sh = Aperture::Polygon; verts = 8; return true; }
    if (w == "POLYGON" || w == "POLY" || w == "P") { sh = Aperture::Polygon; verts = 6; return true; }
    if (w == "THERMAL" || w == "THERM" || w == "DONUT" || w == "ANNULUS" || w == "TARGET" || w == "MOIRE") {
        sh = Aperture::Circle; note = w; return true;
    }
    return false;
}
bool usage_word(const std::string& w0) {
    std::string w = upper(w0);
    return w == "DRAW" || w == "FLASH" || w == "LINE" || w == "PAD" || w == "BOTH" || w == "D/F" ||
           w == "MULTI" || w == "TRACK" || w == "STROKE";
}
// number, or AxB pair
bool size_token(const std::string& tok, std::vector<std::string>& out) {
    std::string u = upper(tok);
    size_t x = u.find('X');
    auto isnum = [](const std::string& s) {
        double v;
        return parse_double(s, v);
    };
    if (x != std::string::npos && x > 0 && x + 1 < u.size()) {
        std::string a = u.substr(0, x), b = u.substr(x + 1);
        if (isnum(a) && isnum(b)) { out.push_back(a); out.push_back(b); return true; }
        return false;
    }
    if (isnum(u)) { out.push_back(u); return true; }
    return false;
}

} // namespace

// ============================================================================
// public API
// ============================================================================
std::string Format::describe() const {
    std::string s = inch ? "inch " : "mm ";
    s += std::to_string(int_digits) + "." + std::to_string(frac_digits);
    s += lead_supp ? " leading-zero-suppressed" : " trailing-zero-suppressed";
    s += absolute ? " absolute" : " incremental";
    return s;
}

int Layer::flash_count() const {
    int n = 0;
    for (auto& o : objs) n += o.kind == Object::Flash;
    return n;
}
int Layer::draw_count() const {
    int n = 0;
    for (auto& o : objs) n += o.kind == Object::Draw || o.kind == Object::Arc;
    return n;
}
int Layer::region_count() const {
    int n = 0;
    for (auto& o : objs) n += o.kind == Object::Region;
    return n;
}
void Layer::recompute_extent() {
    extent = BBox{};
    for (auto& o : objs)
        if (!o.clear) extent.add(object_bbox(*this, o));
}

bool looks_like_excellon(const std::string& t) {
    std::string head = upper(t.substr(0, 4096));
    if (head.find("M48") != std::string::npos) return true;
    if (head.find("%FS") != std::string::npos || head.find("%AD") != std::string::npos) return false;
    // T01C0.035 style tool definitions, no '*' terminators
    bool tooldef = false;
    std::istringstream ss(head);
    std::string ln;
    int stars = 0;
    while (std::getline(ss, ln)) {
        ln = trim(ln);
        if (ln.find('*') != std::string::npos) ++stars;
        if (ln.size() > 3 && ln[0] == 'T' && std::isdigit((unsigned char)ln[1]) && ln.find('C') != std::string::npos) tooldef = true;
    }
    return tooldef && stars == 0;
}
bool looks_like_rs274x(const std::string& t) {
    std::string head = t.substr(0, 65536);
    return head.find("%FS") != std::string::npos || head.find("%AD") != std::string::npos ||
           head.find("%MO") != std::string::npos;
}

bool parse(const std::string& text, const ParseOptions& opt, Layer& layer, std::string& err) {
    layer = Layer{};
    if (text.empty()) { err = "empty input"; return false; }
    bool drill = opt.kind == ParseOptions::Kind::Excellon ||
                 (opt.kind == ParseOptions::Kind::Auto && (looks_like_excellon(text) || text.rfind("%%3000", 0) == 0));
    if (drill) {
        DrillParser p(opt, layer);
        p.run(text);
    } else {
        GerberParser p(opt, layer, text);
        p.run();
    }
    layer.recompute_extent();
    if (layer.objs.empty() && layer.aps.empty()) {
        // A syntactically valid but empty RS-274X file (e.g. KiCad's unused User_/Margin/Adhesive
        // layers: header + M02 only) is a legitimate empty layer, not a failure.
        if (!drill && looks_like_rs274x(text) && text.find("M02") != std::string::npos) { layer.empty_file = true; return true; }
        err = "no Gerber/Excellon content recognised";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Aperture list
// ---------------------------------------------------------------------------
ApertureListResult parse_aperture_list(const std::string& text, ListUnits units) {
    ApertureListResult R;
    struct Row {
        int d;
        std::vector<std::string> sizes;
        Aperture::Shape shape;
        int verts;
        bool square;
        bool shape_found;
        std::string usage, comment;
        int col_d, col_size, col_shape, col_usage;
    };
    std::vector<Row> rows;
    std::string header_hint;
    std::istringstream ss(text);
    std::string ln;
    int lineno = 0;
    std::vector<std::string> skipped_lines;
    while (std::getline(ss, ln)) {
        ++lineno;
        std::string comment;
        size_t sc = ln.find(';');
        if (sc != std::string::npos) { comment = ln.substr(sc + 1); ln = ln.substr(0, sc); }
        std::string up_c = upper(comment);
        if (up_c.find("CODE") != std::string::npos || up_c.find("SIZE") != std::string::npos) {
            // keep whole words only, space-delimited, so "COMMENT" does not read as "MM"
            std::string w;
            for (char c : up_c + " ") {
                if (std::isalpha((unsigned char)c)) w.push_back(c);
                else { if (!w.empty()) header_hint += " " + w + " "; w.clear(); }
            }
        }
        // tokenise
        std::vector<std::string> tok;
        std::string cur;
        for (char c : ln) {
            if ((unsigned char)c <= ' ' || c == ',' || c == '|' || c == '=' || c == ':') {
                if (!cur.empty()) tok.push_back(cur);
                cur.clear();
            } else cur.push_back(c);
        }
        if (!cur.empty()) tok.push_back(cur);
        if (tok.empty()) continue;
        Row r{};
        r.d = -1;
        r.col_size = r.col_shape = r.col_usage = -1;
        size_t k = 0;
        // D-code: first token "D12"/"12"
        for (; k < tok.size(); ++k) {
            std::string u = upper(tok[k]);
            std::string num = u;
            if (!num.empty() && num[0] == 'D') num = num.substr(1);
            if (!num.empty() && std::all_of(num.begin(), num.end(), [](char c) { return std::isdigit((unsigned char)c); })) {
                r.d = std::atoi(num.c_str());
                r.col_d = (int)k;
                ++k;
                break;
            }
            if (u.find_first_of("0123456789") != std::string::npos) break;  // junk with digits: give up
        }
        if (r.d < 0) {
            if (!trim(ln).empty()) { ++R.skipped; skipped_lines.push_back(std::to_string(lineno)); }
            continue;
        }
        bool prev_num = false;
        bool shape_before_size = false;
        for (; k < tok.size(); ++k) {
            std::string& w = tok[k];
            if (usage_word(w)) {
                r.usage = upper(w);
                r.col_usage = (int)k;
                ++k;
                break;
            }
            Aperture::Shape sh;
            int v;
            bool sq;
            std::string note;
            if (!r.shape_found && shape_word(w, sh, v, sq, note)) {
                r.shape_found = true; r.shape = sh; r.verts = v; r.square = sq; r.col_shape = (int)k;
                if (!note.empty()) r.comment = note;
                if (r.sizes.empty()) shape_before_size = true;
                prev_num = false;
                continue;
            }
            std::vector<std::string> sz;
            if (size_token(w, sz)) {
                bool accept = r.sizes.empty() || (r.sizes.size() == 1 && (prev_num || shape_before_size));
                if (accept && r.sizes.size() + sz.size() <= 2) {
                    if (r.sizes.empty()) r.col_size = (int)k;
                    for (auto& s : sz) r.sizes.push_back(s);
                    prev_num = true;
                    continue;
                }
            }
            break;  // anything else starts the comment
        }
        for (; k < tok.size(); ++k) { if (!r.comment.empty()) r.comment += " "; r.comment += tok[k]; }
        if (!comment.empty()) { if (!r.comment.empty()) r.comment += " "; r.comment += trim(comment); }
        if (r.sizes.empty()) { ++R.skipped; skipped_lines.push_back(std::to_string(lineno)); continue; }
        rows.push_back(std::move(r));
    }
    // units
    std::string unit_note;
    double scale = kMmPerMil;
    if (units == ListUnits::Auto) {
        bool any_dot = false;
        double mx = 0;
        for (auto& r : rows)
            for (auto& s : r.sizes) { any_dot |= s.find('.') != std::string::npos; mx = std::max(mx, std::atof(s.c_str())); }
        if (header_hint.find(" MIL") != std::string::npos) { scale = kMmPerMil; unit_note = "mil (header says MIL)"; }
        else if (header_hint.find(" MM ") != std::string::npos) { scale = 1.0; unit_note = "mm (header says MM)"; }
        else if (header_hint.find(" INCH") != std::string::npos) { scale = 25.4; unit_note = "inch (header says INCH)"; }
        else if (!any_dot || mx >= 2.0) {
            scale = kMmPerMil;
            unit_note = "mil (guessed: " + std::string(any_dot ? "values >= 2" : "integers without decimal point") +
                        ", max " + std::to_string((int)mx) + ")";
        } else { scale = 25.4; unit_note = "inch (guessed: decimal values < 2)"; }
    } else if (units == ListUnits::Mil) { scale = kMmPerMil; unit_note = "mil (caller)"; }
    else if (units == ListUnits::Inch) { scale = 25.4; unit_note = "inch (caller)"; }
    else { scale = 1.0; unit_note = "mm (caller)"; }

    int no_shape = 0;
    for (auto& r : rows) {
        Aperture a;
        a.dcode = r.d;
        double s0 = std::atof(r.sizes[0].c_str()) * scale;
        double s1 = r.sizes.size() > 1 ? std::atof(r.sizes[1].c_str()) * scale : s0;
        if (!r.shape_found) { ++no_shape; r.shape = r.sizes.size() > 1 ? Aperture::Rect : Aperture::Circle; }
        a.shape = r.shape;
        a.w = s0;
        a.h = (r.shape == Aperture::Circle || r.shape == Aperture::Polygon || r.square) ? s0 : s1;
        a.verts = r.verts;
        a.usage = r.usage;
        a.comment = r.comment;
        R.aps[a.dcode] = a;
    }
    R.rows = (int)rows.size();
    // explanation: column positions of the first good row
    std::ostringstream ex;
    if (!rows.empty()) {
        const Row& f = rows.front();
        auto col = [](int c) { return c < 0 ? std::string("-") : std::to_string(c + 1); };
        ex << "Columns guessed (token #): D-code=" << col(f.col_d) << ", size=" << col(f.col_size)
           << ", shape=" << col(f.col_shape) << ", usage=" << col(f.col_usage) << ", rest=comment. ";
    }
    ex << "Units: " << unit_note << ". " << R.rows << " apertures";
    if (no_shape) ex << ", " << no_shape << " without shape (assumed round/rect)";
    ex << ", " << R.skipped << " non-aperture lines skipped";
    if (!skipped_lines.empty()) {
        ex << " (lines";
        for (size_t i = 0; i < skipped_lines.size() && i < 8; ++i) ex << " " << skipped_lines[i];
        if (skipped_lines.size() > 8) ex << " ...";
        ex << ")";
    }
    ex << ".";
    R.explanation = ex.str();
    return R;
}

std::string aperture_list_to_text(const std::map<int, Aperture>& aps, bool mils) {
    std::ostringstream o;
    double k = mils ? 1.0 / kMmPerMil : 1.0;
    o << "; Aperture list (sizes in " << (mils ? "mils" : "mm") << ")\n";
    o << ";  D-Code  Size        Shape       Usage     Comment\n";
    auto num = [&](double v) {
        char b[32];
        if (mils) std::snprintf(b, sizeof b, "%g", std::round(v * k * 100) / 100);
        else std::snprintf(b, sizeof b, "%.4g", v * k);
        return std::string(b);
    };
    for (auto& [d, a] : aps) {
        std::string size, shape;
        switch (a.shape) {
        case Aperture::Circle: size = num(a.w); shape = "round"; break;
        case Aperture::Rect:
            if (std::fabs(a.w - a.h) < 1e-9) { size = num(a.w); shape = "square"; }
            else { size = num(a.w) + "x" + num(a.h); shape = "rect"; }
            break;
        case Aperture::Obround: size = num(a.w) + "x" + num(a.h); shape = "oblong"; break;
        case Aperture::Polygon: size = num(a.w); shape = a.verts == 8 ? "octagon" : "polygon"; break;
        case Aperture::Macro: size = num(a.w) + "x" + num(a.h); shape = "macro"; break;
        }
        char line[256];
        std::snprintf(line, sizeof line, "\t%-7d %-11s %-11s %-9s %s\n", d, size.c_str(), shape.c_str(),
                      a.usage.empty() ? "-" : a.usage.c_str(),
                      (a.macro_name.empty() ? a.comment : ("AM " + a.macro_name + " " + a.comment)).c_str());
        o << line;
    }
    return o.str();
}

// ---------------------------------------------------------------------------
// Format guessing
// ---------------------------------------------------------------------------
std::vector<FormatCandidate> guess_format(const std::string& text, const std::map<int, Aperture>& aps) {
    struct F { bool inch; int i, f; };
    const F base[] = {{true, 2, 3}, {true, 2, 4}, {true, 3, 3}, {true, 2, 5}, {true, 3, 4}, {false, 3, 3}, {false, 4, 3}, {false, 3, 2}};
    std::vector<FormatCandidate> out;
    for (const F& b : base)
        for (int lz = 1; lz >= 0; --lz) {
            ParseOptions po;
            po.kind = ParseOptions::Kind::Gerber;
            po.fmt.inch = b.inch; po.fmt.int_digits = b.i; po.fmt.frac_digits = b.f; po.fmt.lead_supp = lz;
            po.apertures = aps;
            po.max_warnings = 5;
            Layer L;
            std::string err;
            FormatCandidate c;
            c.fmt = po.fmt;
            if (!parse(text, po, L, err)) { c.score = 1e9; c.note = err; out.push_back(c); continue; }
            c.fmt = L.fmt;
            c.extent = L.extent;
            // flash overlap density
            std::vector<std::pair<Vec2, double>> fl;
            for (auto& o : L.objs)
                if (o.kind == Object::Flash) {
                    auto it = L.aps.find(o.dcode);
                    double s = it != L.aps.end() ? std::min(it->second.w, it->second.h) : 0;
                    if (s > 0) fl.push_back({o.a, s});
                }
            if (fl.size() > 4000) fl.resize(4000);
            int ov = 0;
            if (!fl.empty()) {
                double cell = 0;
                for (auto& f : fl) cell = std::max(cell, f.second);
                std::unordered_map<long long, std::vector<int>> grid;
                auto key = [&](long long gx, long long gy) { return gx * 1000003LL + gy; };
                for (int k = 0; k < (int)fl.size(); ++k)
                    grid[key((long long)std::floor(fl[k].first.x / cell), (long long)std::floor(fl[k].first.y / cell))].push_back(k);
                for (int k = 0; k < (int)fl.size(); ++k) {
                    long long gx = (long long)std::floor(fl[k].first.x / cell), gy = (long long)std::floor(fl[k].first.y / cell);
                    bool hit = false;
                    int nb = 0;
                    for (long long dx = -1; dx <= 1 && !hit; ++dx)
                        for (long long dy = -1; dy <= 1 && !hit; ++dy) {
                            auto it = grid.find(key(gx + dx, gy + dy));
                            if (it == grid.end()) continue;
                            for (int m : it->second) {
                                if (m == k) continue;
                                double d = dist(fl[k].first, fl[m].first);
                                double s = std::min(fl[k].second, fl[m].second);
                                if (d < 0.5 * s && ++nb >= 2) { hit = true; break; }
                            }
                        }
                    ov += hit;
                }
                c.overlap = (double)ov / fl.size();
            }
            BBox cen;  // extent of object positions (independent of aperture sizes)
            for (auto& o : L.objs) {
                if (o.kind == Object::Region) { for (auto& v : o.poly) cen.add(v); }
                else { cen.add(o.a); if (o.kind != Object::Flash) cen.add(o.b); }
            }
            double ext = std::max(cen.w(), cen.h());
            if (cen.empty() || ext <= 0) c.score = 1e8;
            else {
                c.score = std::fabs(std::log10(ext / 150.0)) + 4.0 * c.overlap;
                if (ext < 10 || ext > 1500) c.score += 5;
            }
            char b2[160];
            std::snprintf(b2, sizeof b2, "extent %.1f x %.1f mm, crowded flashes %.0f%%", cen.w(), cen.h(), c.overlap * 100);
            c.note = b2;
            out.push_back(c);
            if (L.format_from_file) {  // file defines its own format: nothing to guess
                out.back().note += " (format from file)";
                std::stable_sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.score < b.score; });
                return out;
            }
        }
    std::stable_sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.score < b.score; });
    return out;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------
const Aperture* find_ap(const Layer& L, int dcode) {
    auto it = L.aps.find(dcode);
    return it == L.aps.end() ? nullptr : &it->second;
}

BBox aperture_bbox(const Aperture& ap) {
    BBox b;
    switch (ap.shape) {
    case Aperture::Circle:
    case Aperture::Polygon:
        b.add({-ap.w / 2, -ap.w / 2}); b.add({ap.w / 2, ap.w / 2}); break;
    case Aperture::Rect:
    case Aperture::Obround:
        b.add({-ap.w / 2, -ap.h / 2}); b.add({ap.w / 2, ap.h / 2}); break;
    case Aperture::Macro:
        for (size_t i = 0; i < ap.macro_polys.size(); ++i)
            if (i >= ap.macro_clear.size() || !ap.macro_clear[i])
                for (auto& v : ap.macro_polys[i]) b.add(v);
        if (b.empty()) b.add({0, 0});
        break;
    }
    return b;
}

double arc_start_deg(const Object& o) { return rad2deg(std::atan2(o.a.y - o.center.y, o.a.x - o.center.x)); }

double arc_sweep_deg(const Object& o) {
    double a0 = std::atan2(o.a.y - o.center.y, o.a.x - o.center.x);
    double a1 = std::atan2(o.b.y - o.center.y, o.b.x - o.center.x);
    double r = dist(o.a, o.center);
    if (dist(o.a, o.b) < 1e-7 * std::max(1.0, r)) return o.cw ? -360.0 : 360.0;
    double sw = rad2deg(a1 - a0);
    if (o.cw) { while (sw >= 0) sw -= 360; while (sw < -360) sw += 360; }
    else { while (sw <= 0) sw += 360; while (sw > 360) sw -= 360; }
    return sw;
}

std::vector<Vec2> arc_points(const Object& o, int segs_per_circle) {
    double r0 = dist(o.a, o.center), r1 = dist(o.b, o.center);
    double a0 = arc_start_deg(o), sw = arc_sweep_deg(o);
    int n = std::max(2, (int)std::ceil(std::fabs(sw) / 360.0 * segs_per_circle));
    std::vector<Vec2> p;
    p.reserve(n + 1);
    p.push_back(o.a);
    for (int k = 1; k < n; ++k) {
        double t = (double)k / n;
        double a = deg2rad(a0 + sw * t), r = r0 + (r1 - r0) * t;
        p.push_back({o.center.x + r * std::cos(a), o.center.y + r * std::sin(a)});
    }
    p.push_back(o.b);
    return p;
}

static BBox arc_bbox(const Object& o) {
    BBox b;
    b.add(o.a); b.add(o.b);
    double r = dist(o.a, o.center), a0 = arc_start_deg(o), sw = arc_sweep_deg(o);
    for (int k = -4; k <= 8; ++k) {
        double ang = 90.0 * k;
        double rel = sw > 0 ? ang - a0 : a0 - ang;
        if (rel > 1e-9 && rel < std::fabs(sw) - 1e-9) b.add({o.center.x + r * std::cos(deg2rad(ang)), o.center.y + r * std::sin(deg2rad(ang))});
    }
    return b;
}

BBox object_bbox(const Layer& L, const Object& o) {
    const Aperture* ap = find_ap(L, o.dcode);
    BBox ab = ap ? aperture_bbox(*ap) : BBox{};
    if (ab.empty()) ab.add({0, 0});
    BBox b;
    switch (o.kind) {
    case Object::Flash:
        b.add(o.a + Vec2{ab.x0, ab.y0}); b.add(o.a + Vec2{ab.x1, ab.y1}); break;
    case Object::Draw:
        for (Vec2 p : {o.a, o.b}) { b.add(p + Vec2{ab.x0, ab.y0}); b.add(p + Vec2{ab.x1, ab.y1}); }
        break;
    case Object::Arc: {
        BBox c = arc_bbox(o);
        b.add(Vec2{c.x0 + ab.x0, c.y0 + ab.y0}); b.add(Vec2{c.x1 + ab.x1, c.y1 + ab.y1});
        break;
    }
    case Object::Region:
        for (auto& v : o.poly) b.add(v);
        break;
    }
    return b;
}

std::vector<std::vector<Vec2>> flash_polys(const Aperture& ap, Vec2 at, int segs, std::vector<bool>* clear) {
    std::vector<std::vector<Vec2>> out;
    if (clear) clear->clear();
    auto add = [&](std::vector<Vec2> p, bool c) {
        for (auto& v : p) v += at;
        out.push_back(std::move(p));
        if (clear) clear->push_back(c);
    };
    switch (ap.shape) {
    case Aperture::Circle: add(ngon({0, 0}, ap.w / 2, segs, 0), false); break;
    case Aperture::Rect: add({{-ap.w / 2, -ap.h / 2}, {ap.w / 2, -ap.h / 2}, {ap.w / 2, ap.h / 2}, {-ap.w / 2, ap.h / 2}}, false); break;
    case Aperture::Obround: {
        std::vector<Vec2> p;
        bool horiz = ap.w >= ap.h;
        double r = (horiz ? ap.h : ap.w) / 2, l = (horiz ? ap.w : ap.h) / 2 - r;
        int n = std::max(4, segs / 2);
        for (int k = 0; k <= n; ++k) {  // right/top cap
            double a = -90 + 180.0 * k / n;
            Vec2 v{r * std::cos(deg2rad(a)) + l, r * std::sin(deg2rad(a))};
            p.push_back(horiz ? v : Vec2{-v.y, v.x});
        }
        for (int k = 0; k <= n; ++k) {
            double a = 90 + 180.0 * k / n;
            Vec2 v{r * std::cos(deg2rad(a)) - l, r * std::sin(deg2rad(a))};
            p.push_back(horiz ? v : Vec2{-v.y, v.x});
        }
        add(p, false);
        break;
    }
    case Aperture::Polygon: add(ngon({0, 0}, ap.w / 2, std::max(3, ap.verts), ap.rot), false); break;
    case Aperture::Macro:
        for (size_t i = 0; i < ap.macro_polys.size(); ++i)
            add(ap.macro_polys[i], i < ap.macro_clear.size() && ap.macro_clear[i]);
        break;
    }
    if (ap.hole > 0 && ap.shape != Aperture::Macro) add(ngon({0, 0}, ap.hole / 2, segs, 0), true);
    return out;
}

std::vector<Vec2> flash_outline(const Aperture& ap, Vec2 at, int segs) {
    std::vector<bool> clr;
    auto polys = flash_polys(ap, at, segs, &clr);
    if (ap.shape != Aperture::Macro) return polys.empty() ? std::vector<Vec2>{} : polys[0];
    std::vector<Vec2> pts;
    for (size_t i = 0; i < polys.size(); ++i)
        if (!clr[i]) pts.insert(pts.end(), polys[i].begin(), polys[i].end());
    return convex_hull(pts);
}

int hit_test(const Layer& L, Vec2 p, double tol) {
    for (int i = (int)L.objs.size() - 1; i >= 0; --i) {
        const Object& o = L.objs[i];
        if (o.clear) continue;
        BBox bb = object_bbox(L, o);
        bb.inflate(tol);
        if (!bb.contains(p)) continue;
        const Aperture* ap = find_ap(L, o.dcode);
        double hw = ap ? (ap->shape == Aperture::Circle ? ap->w / 2 : std::max(ap->w, ap->h) / 2) : 0;
        switch (o.kind) {
        case Object::Flash: {
            if (!ap) { if (dist(p, o.a) <= tol) return i; break; }
            std::vector<bool> clr;
            auto polys = flash_polys(*ap, o.a, 32, &clr);
            for (size_t k = 0; k < polys.size(); ++k)
                if (!clr[k] && (point_in_poly(p, polys[k]) || poly_edge_dist(p, polys[k]) <= tol)) return i;
            break;
        }
        case Object::Draw:
            if (seg_dist(p, o.a, o.b) <= hw + tol) return i;
            break;
        case Object::Arc: {
            auto pts = arc_points(o, 72);
            for (size_t k = 1; k < pts.size(); ++k)
                if (seg_dist(p, pts[k - 1], pts[k]) <= hw + tol) return i;
            break;
        }
        case Object::Region:
            if (point_in_poly(p, o.poly) || poly_edge_dist(p, o.poly) <= tol) return i;
            break;
        }
    }
    return -1;
}

} // namespace pcbp::gerber

// ===========================================================================
// Layer function guess (file name conventions)
// ===========================================================================
namespace pcbp::gerber {
namespace {
std::string lc(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
bool contains(const std::string& s, const char* k) { return s.find(k) != std::string::npos; }
enum R { Other = 0, Cu, Paste, Mask, Silk, Assy, Drill, Outline };
}

LayerGuess guess_layer_function(const std::string& path, const std::string& content) {
    LayerGuess g;
    // ---- file name parts
    std::string p = path;
    for (auto& c : p) if (c == '\\') c = '/';
    size_t sl = p.rfind('/');
    std::string fname = lc(sl == std::string::npos ? p : p.substr(sl + 1));
    std::string ext, stem = fname;
    size_t dot = fname.rfind('.');
    if (dot != std::string::npos && dot > 0) { ext = fname.substr(dot + 1); stem = fname.substr(0, dot); }
    // generic container extensions: classify by the stem (KiCad board-F_Cu.gbr, name.top.gbr)
    const bool generic = ext.empty() || ext == "gbr" || ext == "ger" || ext == "gbx" || ext == "art" || ext == "pho" || ext == "274" ||
                         ext == "spl" || ext == "dat" || ext == "fph" || ext == "lgx";
    auto set = [&](int role, int side, const std::string& why) { g.role = role; g.side = side; g.known = true; g.why = why; };

    // ---- 1. X2 attributes
    size_t tf = content.find("TF.FileFunction,");
    if (tf != std::string::npos && tf < 20000) {
        size_t e = content.find_first_of("*\r\n", tf);
        std::string f = lc(content.substr(tf + 16, e == std::string::npos ? 80 : e - tf - 16));
        int side = contains(f, ",bot") ? 1 : 0;
        int role = -1;
        if (contains(f, "copper")) {
            role = Cu;
            size_t l = f.find(",l");
            if (l != std::string::npos && contains(f, ",inr")) { side = 2; g.inner = std::max(1, std::atoi(f.c_str() + l + 2) - 1); }   // X2 "Copper,Ln,Inr": L1 = top, so Ln -> inner n-1
        } else if (contains(f, "soldermask")) role = Mask;
        else if (contains(f, "paste")) role = Paste;
        else if (contains(f, "legend")) role = Silk;
        else if (contains(f, "profile")) role = Outline;
        else if (contains(f, "plated") || contains(f, "drill") || contains(f, "route")) role = Drill;
        else if (contains(f, "assembly") || contains(f, "component")) role = Assy;
        if (role >= 0) { set(role, side, "X2 FileFunction"); return g; }
    }

    // ---- 2. extension tables
    struct E { const char* ext; int role, side; const char* who; };
    static const E exts[] = {
        // Protel / Altium
        {"gtl", Cu, 0, "Altium"}, {"gbl", Cu, 1, "Altium"}, {"gts", Mask, 0, "Altium"}, {"gbs", Mask, 1, "Altium"},
        {"gto", Silk, 0, "Altium"}, {"gbo", Silk, 1, "Altium"}, {"gtp", Paste, 0, "Altium"}, {"gbp", Paste, 1, "Altium"},
        {"gko", Outline, 0, "Altium keep-out"}, {"gm1", Outline, 0, "Altium mech 1"}, {"gm", Outline, 0, "Altium mech"},
        {"gml", Outline, 0, "mill layer"}, {"gmb", Outline, 0, "mill layer"}, {"oln", Outline, 0, "outline"}, {"out", Outline, 0, "outline"},
        {"gta", Assy, 0, "Altium assembly"}, {"gba", Assy, 1, "Altium assembly"},
        // Eagle CAM
        {"cmp", Cu, 0, "Eagle"}, {"sol", Cu, 1, "Eagle"}, {"stc", Mask, 0, "Eagle"}, {"sts", Mask, 1, "Eagle"},
        {"plc", Silk, 0, "Eagle"}, {"pls", Silk, 1, "Eagle"}, {"crc", Paste, 0, "Eagle"}, {"crs", Paste, 1, "Eagle"},
        {"dim", Outline, 0, "Eagle"}, {"mil", Outline, 0, "Eagle"},
        // OrCAD Layout / PADS / generic
        {"top", Cu, 0, "OrCAD"}, {"bot", Cu, 1, "OrCAD"}, {"smt", Mask, 0, "OrCAD"}, {"smb", Mask, 1, "OrCAD"},
        {"sst", Silk, 0, "OrCAD"}, {"ssb", Silk, 1, "OrCAD"}, {"spt", Paste, 0, "OrCAD"}, {"spb", Paste, 1, "OrCAD"},
        {"ast", Assy, 0, "OrCAD"}, {"asb", Assy, 1, "OrCAD"}, {"fab", Assy, 0, "fab drawing"},
        // drill
        {"drl", Drill, 0, "drill"}, {"xln", Drill, 0, "drill"}, {"exc", Drill, 0, "drill"}, {"drd", Drill, 0, "drill"},
        {"ncd", Drill, 0, "drill"}, {"nc", Drill, 0, "drill"}, {"tap", Drill, 0, "drill"}, {"txt", -1, 0, ""}, {"tx1", Drill, 0, "drill"},
        {"xnc", Drill, 0, "drill"},
    };
    for (const E& e : exts) {
        if (ext != e.ext) continue;
        if (e.role < 0) break;
        set(e.role, e.side, std::string(e.who) + " ." + ext);
        break;
    }
    if (!g.known && ext.size() >= 2) {
        // numbered inner layers: .G1 .. .G30, .GP1 (plane), .In1/.Int1/.L2
        auto num_after = [&](size_t k) { return k < ext.size() && std::all_of(ext.begin() + (long)k, ext.end(), [](char c) { return std::isdigit((unsigned char)c); }) ? std::atoi(ext.c_str() + k) : -1; };
        int n = -1;
        if (ext[0] == 'g' && (n = num_after(1)) >= 1) {}
        else if (ext.rfind("gp", 0) == 0 && (n = num_after(2)) >= 1) {}
        else if (ext.rfind("int", 0) == 0 && (n = num_after(3)) >= 1) {}
        else if (ext.rfind("in", 0) == 0 && (n = num_after(2)) >= 1) {}
        else n = -1;
        if (n >= 1) { set(Cu, 2, "inner ." + ext); g.inner = n; }
    }

    // ---- 3. name tokens (stem; whole name when the extension is unknown / generic)
    if (!g.known) {
        std::string nm = generic || ext == "txt" ? stem : fname;
        std::string n2;
        for (char c : nm) n2 += (c == '.' || c == '_' || c == ' ' || c == '(' || c == ')') ? '-' : c;
        std::vector<std::string> tok;
        { std::string cur; for (char c : n2) { if (c == '-') { if (!cur.empty()) tok.push_back(cur); cur.clear(); } else cur += c; } if (!cur.empty()) tok.push_back(cur); }
        auto tk = [&](const char* t) { return std::find(tok.begin(), tok.end(), t) != tok.end(); };
        auto tkp = [&](const char* pre) { for (auto& t : tok) if (t.rfind(pre, 0) == 0) return true; return false; };
        // KiCad: F_Cu, B_Paste, In1_Cu, Edge_Cuts, F_SilkS, F_Mask, F_Fab
        for (size_t i = 0; i + 1 < tok.size() && !g.known; ++i) {
            const std::string& a = tok[i]; const std::string& b = tok[i + 1];
            int side = a == "f" ? 0 : a == "b" ? 1 : -1;
            if (a == "edge" && b == "cuts") { set(Outline, 0, "KiCad Edge_Cuts"); break; }
            if (side < 0) {
                if (a.size() >= 3 && a.rfind("in", 0) == 0 && std::isdigit((unsigned char)a[2]) && b == "cu") { set(Cu, 2, "KiCad In_Cu"); g.inner = std::atoi(a.c_str() + 2); }
                continue;
            }
            const char* s = side ? "B_" : "F_";
            if (b == "cu") set(Cu, side, std::string("KiCad ") + s + "Cu");
            else if (b == "paste") set(Paste, side, std::string("KiCad ") + s + "Paste");
            else if (b == "mask") set(Mask, side, std::string("KiCad ") + s + "Mask");
            else if (b.rfind("silk", 0) == 0) set(Silk, side, std::string("KiCad ") + s + "SilkS");
            else if (b == "fab" || b == "courtyard" || b == "crtyd") set(Assy, side, std::string("KiCad ") + s + "Fab");
        }
        if (!g.known) {
            // side words
            int side = 0;
            bool side_known = false;
            for (auto& t : tok) {
                if (t == "bot" || t == "bottom" || t == "bs" || t == "back" || t == "bott" || t.rfind("bot", 0) == 0 || (t.size() > 3 && t.compare(t.size() - 3, 3, "bot") == 0) ||
                    (t.size() > 6 && t.compare(t.size() - 6, 6, "bottom") == 0)) { side = 1; side_known = true; }
                else if (t == "top" || t == "ts" || t == "front" || t == "comp" || t == "component" || t.rfind("top", 0) == 0 || (t.size() > 3 && t.compare(t.size() - 3, 3, "top") == 0)) { if (!side_known) side_known = true; }
            }
            int inner = 0;
            for (auto& t : tok) {
                for (const char* pre : {"int", "inner", "in", "l", "layer", "sig", "signal", "plane", "gnd", "pwr", "vcc"}) {
                    size_t k = std::strlen(pre);
                    if (t.size() > k && t.rfind(pre, 0) == 0 && std::all_of(t.begin() + (long)k, t.end(), [](char c) { return std::isdigit((unsigned char)c); })) {
                        int n = std::atoi(t.c_str() + k);
                        if (n >= 1 && n < 64 && (std::strcmp(pre, "l") != 0 || n > 1)) inner = n;
                    }
                }
            }
            auto any = [&](std::initializer_list<const char*> ks) { for (const char* k : ks) if (contains(n2, k)) return true; return false; };
            int role = -1;
            std::string why;
            if (any({"paste", "cream", "stencil", "pastemask"}) || tk("spt") || tk("spb") || tk("sp")) { role = Paste; why = "name: paste"; }
            else if (any({"mask", "resist", "stop", "soldmask"}) || tk("smt") || tk("smb") || tk("sm") || tk("sms") || tk("smc")) { role = Mask; why = "name: mask"; if (tk("smb") || tk("sms")) side = 1, side_known = true; }
            else if (any({"silk", "legend", "overlay", "print", "ident", "screen"}) || tk("sst") || tk("ssb") || tk("ss")) { role = Silk; why = "name: silk"; if (tk("ssb")) side = 1, side_known = true; }
            else if (any({"assy", "assembly", "fab", "place", "component-outline"})) { role = Assy; why = "name: assembly"; }
            else if (any({"drill", "drl", "ncdrill", "hole", "excellon", "nc-drill"}) || tk("pth") || tk("npth")) { role = Drill; why = "name: drill"; }
            else if (any({"outline", "border", "profile", "edge", "contour", "mill", "route", "board-dim", "dimension", "keepout", "frame"})) { role = Outline; why = "name: outline"; }
            else if (any({"copper", "signal", "trace", "etch", "plane", "gnd", "pwr", "power", "ground"}) || tk("cu") || inner > 0 || tk("top") || tk("bot") || tk("bottom") ||
                     tk("cmp") || tk("sol") || tk("solder") || tk("component") || tkp("int")) { role = Cu; why = "name: copper"; }
            if (role >= 0) {
                set(role, side, why);
                if (role == Cu && inner > 0 && !side_known) { g.side = 2; g.inner = inner; }
                if (role == Cu && (tk("sol") || tk("solder"))) g.side = 1;
            } else if (side_known) {
                g.side = side; g.why = "name: side";
            }
        }
    }
    // ---- 4. content
    if ((!g.known || g.role == Other) && looks_like_excellon(content)) set(Drill, 0, g.known ? g.why + ", Excellon content" : "Excellon content");
    return g;
}


const char* file_kind_name(FileKind k) {
    switch (k) {
    case FileKind::Rs274x: return "RS-274X";
    case FileKind::Rs274d: return "RS-274D";
    case FileKind::Excellon: return "Excellon";
    case FileKind::ApertureList: return "Aperture list";
    case FileKind::Ipc356: return "IPC-D-356 netlist";
    default: return "Unknown";
    }
}

FileKind classify_file(const std::string& t0) {
    // trailing NUL / 0x1A padding (spooled / CP/M style files) is ignored
    size_t end = t0.size();
    while (end > 0 && (t0[end - 1] == 0 || t0[end - 1] == 0x1A)) --end;
    if (end == 0) return FileKind::Unknown;
    const std::string t = end == t0.size() ? t0 : t0.substr(0, end);
    size_t bin = 0;
    for (size_t i = 0; i < std::min<size_t>(t.size(), 65536); ++i) if ((unsigned char)t[i] < 9 || (unsigned char)t[i] == 0x7F) ++bin;
    if (bin > 8) return FileKind::Unknown;
    if (looks_like_rs274x(t)) return FileKind::Rs274x;
    if (looks_like_excellon(t)) return FileKind::Excellon;
    // walk the first lines once: IPC-D-356 records, Gerber-only character set
    int ipc = 0, lines = 0, glines = 0;
    for (size_t p = 0; p < t.size() && lines < 600;) {
        size_t e = t.find('\n', p);
        if (e == std::string::npos) e = t.size();
        const std::string l = t.substr(p, e - p);
        p = e + 1;
        size_t a = l.find_first_not_of(" \t\r");
        if (a == std::string::npos) continue;
        ++lines;
        if (l.rfind("P  JOB", 0) == 0 || l.rfind("P  UNITS", 0) == 0 || l.rfind("P  CODE", 0) == 0 || l.rfind("317", 0) == 0 ||
            l.rfind("327", 0) == 0 || l.rfind("367", 0) == 0 || l.rfind("999", 0) == 0)
            ++ipc;
        bool g = true;
        for (char c : l) {
            const char u = (char)std::toupper((unsigned char)c);
            if (!(std::isdigit((unsigned char)c) || std::strchr("XYIJDGMNT+-.*$% \t\r", u))) { g = false; break; }
        }
        glines += g;
    }
    if (ipc >= 3) return FileKind::Ipc356;
    // 274D: coordinate blocks with D01/D02/D03; nearly every line made of Gerber characters only
    int xy = 0, dop = 0;
    const size_t n = std::min<size_t>(t.size(), 200000);
    for (size_t i = 0; i + 2 < n; ++i) {
        const char c = (char)std::toupper((unsigned char)t[i]);
        if (c == 'X' && (std::isdigit((unsigned char)t[i + 1]) || t[i + 1] == '-' || t[i + 1] == '+')) {
            size_t j = i + 1;
            while (j < n && (std::isdigit((unsigned char)t[j]) || t[j] == '-' || t[j] == '+' || t[j] == '.')) ++j;
            if (j < n && std::toupper((unsigned char)t[j]) == 'Y') ++xy;
        }
        if (c == 'D' && t[i + 1] == '0' && (t[i + 2] == '1' || t[i + 2] == '2' || t[i + 2] == '3')) ++dop;
        else if (c == 'D' && (t[i + 1] == '1' || t[i + 1] == '2' || t[i + 1] == '3') && (i + 2 >= n || !std::isdigit((unsigned char)t[i + 2]))) ++dop;
    }
    const bool gerber_chars = lines > 0 && glines * 10 >= lines * 8;
    if (gerber_chars && xy >= 3 && dop >= 1) return FileKind::Rs274d;
    {
        // aperture list: most non-empty lines must be aperture rows (rejects GenCAD / CAD text with PAD tables)
        const auto al = parse_aperture_list(t);
        size_t nonempty = 0;
        for (size_t p = 0; p < t.size();) {
            size_t e = t.find('\n', p);
            if (e == std::string::npos) e = t.size();
            if (t.find_first_not_of(" \t\r", p) < e) ++nonempty;
            p = e + 1;
        }
        if (al.aps.size() >= 2 && al.aps.size() * 4 >= nonempty) return FileKind::ApertureList;
    }
    if (gerber_chars && xy >= 10) return FileKind::Rs274d;
    return FileKind::Unknown;
}

} // namespace pcbp::gerber

// ===========================================================================
// Writers
// ===========================================================================
namespace pcbp::gerber {
namespace {
std::string gnum(double mm) {   // 4.6 metric, leading zeros omitted, sign kept
    long long v = std::llround(mm * 1e6);
    return std::to_string(v);
}
std::string fnum(double v) {    // aperture parameters
    char b[40];
    std::snprintf(b, sizeof b, "%.6f", v);
    std::string s = b;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0" || s.empty()) s = "0";
    return s;
}
std::string sanitize_comment(const std::string& c) {
    std::string o;
    for (char ch : c) o += (ch == '*' || ch == '%' || ch == '\n' || ch == '\r') ? ' ' : ch;
    return o;
}
bool kept(const std::vector<char>* keep, size_t i) { return !keep || i >= keep->size() || (*keep)[i]; }
} // namespace

std::string write_rs274x(const Layer& L, const std::vector<char>* keep, const std::string& comment) {
    std::string o;
    o += "G04 PCB Plotter layer export*\n";
    if (!comment.empty()) o += "G04 " + sanitize_comment(comment) + "*\n";
    o += "%FSLAX46Y46*%\n%MOMM*%\n";
    if (L.negative) o += "%IPNEG*%\n";
    std::set<int> used;
    for (size_t i = 0; i < L.objs.size(); ++i)
        if (kept(keep, i) && L.objs[i].kind != Object::Region) used.insert(L.objs[i].dcode);
    std::map<int, int> remap;   // source dcode -> written dcode (>= 10)
    int next = 10;
    for (int d : used) remap[d] = d >= 10 ? d : 0;
    for (auto& [d, nd] : remap) if (nd == 0) { while (used.count(next) || std::any_of(remap.begin(), remap.end(), [&](auto& kv) { return kv.second == next; })) ++next; nd = next++; }
    for (int d : used) {
        const int nd = remap[d];
        auto it = L.aps.find(d);
        Aperture a = it != L.aps.end() ? it->second : Aperture{};
        auto hole = [&]() { return a.hole > 0 ? "X" + fnum(a.hole) : std::string(); };
        const std::string D = "%ADD" + std::to_string(nd);
        switch (a.shape) {
        case Aperture::Circle: o += D + "C," + fnum(std::max(a.w, 0.0)) + hole() + "*%\n"; break;
        case Aperture::Rect: o += D + "R," + fnum(a.w) + "X" + fnum(a.h) + hole() + "*%\n"; break;
        case Aperture::Obround: o += D + "O," + fnum(a.w) + "X" + fnum(a.h) + hole() + "*%\n"; break;
        case Aperture::Polygon:
            o += D + "P," + fnum(a.w) + "X" + std::to_string(std::max(3, a.verts)) + (a.rot != 0 || a.hole > 0 ? "X" + fnum(a.rot) : std::string()) + hole() + "*%\n";
            break;
        case Aperture::Macro: {
            const std::string mn = "PCBPM" + std::to_string(nd);
            std::string m = "%AM" + mn + "*\n";
            int prims = 0;
            for (size_t k = 0; k < a.macro_polys.size(); ++k) {
                const auto& poly = a.macro_polys[k];
                if (poly.size() < 3) continue;
                const bool clr = k < a.macro_clear.size() && a.macro_clear[k];
                m += "4," + std::string(clr ? "0" : "1") + "," + std::to_string(poly.size());
                for (auto& v : poly) m += "," + fnum(v.x) + "," + fnum(v.y);
                m += "," + fnum(poly[0].x) + "," + fnum(poly[0].y) + ",0*\n";
                ++prims;
            }
            if (!prims) m += "1,1,0.001,0,0*\n";
            m += "%\n";
            o += m + D + mn + "*%\n";
            break;
        }
        }
    }
    o += "G01*\nG75*\n";
    bool clear = false;
    int curd = -1;
    bool have_pos = false;
    Vec2 pos{};
    auto xy = [](Vec2 p) { return "X" + gnum(p.x) + "Y" + gnum(p.y); };
    auto moveto = [&](Vec2 p) {
        if (have_pos && std::fabs(p.x - pos.x) < 5e-7 && std::fabs(p.y - pos.y) < 5e-7) return;
        o += xy(p) + "D02*\n";
        pos = p; have_pos = true;
    };
    for (size_t i = 0; i < L.objs.size(); ++i) {
        if (!kept(keep, i)) continue;
        const Object& ob = L.objs[i];
        if (ob.clear != clear) { clear = ob.clear; o += clear ? "%LPC*%\n" : "%LPD*%\n"; }
        if (ob.kind == Object::Region) {
            if (ob.poly.size() < 3) continue;
            o += "G36*\n";
            o += xy(ob.poly[0]) + "D02*\nG01*\n";
            for (size_t k = 1; k < ob.poly.size(); ++k) o += xy(ob.poly[k]) + "D01*\n";
            const Vec2 f = ob.poly.front(), b = ob.poly.back();
            if (std::fabs(f.x - b.x) > 5e-7 || std::fabs(f.y - b.y) > 5e-7) o += xy(f) + "D01*\n";
            o += "G37*\n";
            have_pos = false;
            continue;
        }
        const int nd = remap[ob.dcode];
        if (nd != curd) { o += "D" + std::to_string(nd) + "*\n"; curd = nd; }
        switch (ob.kind) {
        case Object::Flash: o += xy(ob.a) + "D03*\n"; pos = ob.a; have_pos = true; break;
        case Object::Draw: moveto(ob.a); o += "G01" + xy(ob.b) + "D01*\n"; pos = ob.b; break;
        case Object::Arc:
            moveto(ob.a);
            o += std::string(ob.cw ? "G02" : "G03") + xy(ob.b) + "I" + gnum(ob.center.x - ob.a.x) + "J" + gnum(ob.center.y - ob.a.y) + "D01*\n";
            pos = ob.b;
            break;
        default: break;
        }
    }
    o += "M02*\n";
    return o;
}

std::string write_excellon(const Layer& L, const std::vector<char>* keep, const std::string& comment) {
    std::string o = "M48\n; PCB Plotter drill export\n";
    if (!comment.empty()) o += "; " + sanitize_comment(comment) + "\n";
    o += "METRIC,LZ,000.000\n";
    std::set<int> used;
    for (size_t i = 0; i < L.objs.size(); ++i)
        if (kept(keep, i) && L.objs[i].kind != Object::Region) used.insert(L.objs[i].dcode);
    auto tnum = [](int d) { return std::max(1, d - 9); };
    for (int d : used) {
        auto it = L.aps.find(d);
        double dia = it != L.aps.end() ? (it->second.hole > 0 ? it->second.hole : it->second.w) : 0;
        char b[64];
        std::snprintf(b, sizeof b, "T%02dC%.3f\n", tnum(d), dia);
        o += b;
    }
    o += "%\nG90\nG05\n";
    auto xy = [](Vec2 p) {
        char b[64];
        std::snprintf(b, sizeof b, "X%.3fY%.3f", p.x, p.y);
        return std::string(b);
    };
    for (int d : used) {
        char b[16];
        std::snprintf(b, sizeof b, "T%02d\n", tnum(d));
        o += b;
        for (size_t i = 0; i < L.objs.size(); ++i) {
            const Object& ob = L.objs[i];
            if (!kept(keep, i) || ob.dcode != d) continue;
            if (ob.kind == Object::Flash) o += xy(ob.a) + "\n";
            else if (ob.kind == Object::Draw || ob.kind == Object::Arc) o += xy(ob.a) + "G85" + xy(ob.b) + "\n";
        }
    }
    o += "T0\nM30\n";
    return o;
}

} // namespace pcbp::gerber
