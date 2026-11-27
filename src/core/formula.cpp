// ============================================================================
// formula.cpp — values, A1 addressing, formula lexer / parser (to RPN),
// printer, reference adjustment for structural edits. Clean-room from
// ECMA-376 Part 1 §18.17 (formula grammar) and the Excel operator reference
// (precedence: ref ops, negation, %, ^, * /, + -, &, comparisons).
// ============================================================================
#include "formula_internal.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace pcbp::sheet {

// ------------------------------------------------------------------ errors --
static const char* const k_err_names[] = {"", "#NULL!", "#DIV/0!", "#VALUE!", "#REF!", "#NAME?", "#NUM!", "#N/A", "#SPILL!", "#CALC!", "#CIRC!"};
const char* err_text(Err e) { return k_err_names[(int)e < 11 ? (int)e : 0]; }
Err err_from_text(std::string_view s) {
    for (int i = 1; i < 11; ++i) {
        size_t n = strlen(k_err_names[i]);
        if (s.size() == n) {
            bool eq = true;
            for (size_t k = 0; k < n; ++k) if (toupper((unsigned char)s[k]) != k_err_names[i][k]) { eq = false; break; }
            if (eq) return (Err)i;
        }
    }
    return Err::None;
}

bool Value::operator==(const Value& o) const {
    if (type != o.type) return false;
    switch (type) {
    case Empty: return true;
    case Number: case Bool: return num == o.num;
    case String: return str == o.str;
    case Error: return err == o.err;
    }
    return false;
}

// ------------------------------------------------------------ string utils --
std::string upper_ascii(std::string s) { for (auto& ch : s) ch = (char)toupper((unsigned char)ch); return s; }

void utf8_append(std::string& o, uint32_t cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
    else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 63)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
}
std::vector<uint32_t> utf8_decode(const std::string& s) {
    std::vector<uint32_t> o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        uint32_t cp; int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 31; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 15; n = 3; }
        else if ((c >> 3) == 30) { cp = c & 7; n = 4; }
        else { o.push_back(c); ++i; continue; }
        if (i + n > s.size()) { o.push_back(c); ++i; continue; }
        bool ok = true;
        for (int k = 1; k < n; ++k) { unsigned char d = s[i + k]; if ((d >> 6) != 2) { ok = false; break; } cp = (cp << 6) | (d & 63); }
        if (!ok) { o.push_back(c); ++i; continue; }
        o.push_back(cp); i += n;
    }
    return o;
}
std::string utf8_encode(const std::vector<uint32_t>& cps, size_t from, size_t n) {
    std::string o;
    size_t e = n == (size_t)-1 ? cps.size() : std::min(cps.size(), from + n);
    for (size_t i = from; i < e; ++i) utf8_append(o, cps[i]);
    return o;
}
size_t utf8_len(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}
static uint32_t cp_lower(uint32_t c) {
    if (c < 128) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7)) return c + 32;
    if (c >= 0x100 && c <= 0x17F && !(c & 1) && c != 0x130 && c != 0x138) return c + 1;
    if (c >= 0x391 && c <= 0x3AB && c != 0x3A2) return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}
static uint32_t cp_upper(uint32_t c) {
    if (c < 128) return (c >= 'a' && c <= 'z') ? c - 32 : c;
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 32;
    if (c >= 0x101 && c <= 0x17F && (c & 1) && c != 0x131 && c != 0x149) return c - 1;
    if (c >= 0x3B1 && c <= 0x3CB && c != 0x3C2) return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    return c;
}
std::string lower_utf8(const std::string& s) {
    bool ascii = true;
    for (unsigned char c : s) if (c >= 0x80) { ascii = false; break; }
    if (ascii) { std::string o = s; for (auto& ch : o) ch = (char)tolower((unsigned char)ch); return o; }
    std::string o;
    for (uint32_t c : utf8_decode(s)) utf8_append(o, cp_lower(c));
    return o;
}
std::string upper_utf8(const std::string& s) {
    bool ascii = true;
    for (unsigned char c : s) if (c >= 0x80) { ascii = false; break; }
    if (ascii) return upper_ascii(s);
    std::string o;
    for (uint32_t c : utf8_decode(s)) utf8_append(o, cp_upper(c));
    return o;
}
int ci_compare(const std::string& a, const std::string& b) {
    size_t n = std::min(a.size(), b.size());
    size_t i = 0;
    for (; i < n; ++i) {
        unsigned char x = a[i], y = b[i];
        if (x >= 0x80 || y >= 0x80) break;
        int lx = tolower(x), ly = tolower(y);
        if (lx != ly) return lx < ly ? -1 : 1;
    }
    if (i == n) return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
    std::string la = lower_utf8(a.substr(i)), lb = lower_utf8(b.substr(i));
    int c = la.compare(lb);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}
bool ci_equal(const std::string& a, const std::string& b) { return ci_compare(a, b) == 0; }

bool wildcard_match(const std::string& pat, const std::string& txt, bool ci) {
    std::vector<uint32_t> p = utf8_decode(ci ? lower_utf8(pat) : pat), t = utf8_decode(ci ? lower_utf8(txt) : txt);
    // tokens: literal / ? / *
    struct PT { uint32_t c; int k; };   // k: 0 lit, 1 ?, 2 *
    std::vector<PT> pt;
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] == '~' && i + 1 < p.size() && (p[i + 1] == '*' || p[i + 1] == '?' || p[i + 1] == '~')) { pt.push_back({p[i + 1], 0}); ++i; }
        else if (p[i] == '*') pt.push_back({0, 2});
        else if (p[i] == '?') pt.push_back({0, 1});
        else pt.push_back({p[i], 0});
    }
    size_t i = 0, j = 0, star = (size_t)-1, mark = 0;
    while (j < t.size()) {
        if (i < pt.size() && (pt[i].k == 1 || (pt[i].k == 0 && pt[i].c == t[j]))) { ++i; ++j; }
        else if (i < pt.size() && pt[i].k == 2) { star = i++; mark = j; }
        else if (star != (size_t)-1) { i = star + 1; j = ++mark; }
        else return false;
    }
    while (i < pt.size() && pt[i].k == 2) ++i;
    return i == pt.size();
}

std::string fmt_general(double d, int max_sig) {
    if (std::isnan(d)) return "#NUM!";
    if (std::isinf(d)) return "#NUM!";
    if (d == 0) return "0";
    double a = std::fabs(d);
    int e10 = (int)std::floor(std::log10(a));
    char buf[64];
    // round to max_sig significant digits first
    snprintf(buf, sizeof buf, "%.*e", max_sig - 1, d);
    double r = atof(buf);
    a = std::fabs(r);
    if (a == 0) return "0";
    e10 = (int)std::floor(std::log10(a) + 1e-12);
    if (e10 >= 15 || e10 < -9) {
        // scientific: mantissa trimmed
        snprintf(buf, sizeof buf, "%.*E", max_sig - 1, r);
        std::string s = buf;
        size_t ep = s.find('E');
        std::string m = s.substr(0, ep), ex = s.substr(ep + 1);
        if (m.find('.') != std::string::npos) { while (m.back() == '0') m.pop_back(); if (m.back() == '.') m.pop_back(); }
        int ev = atoi(ex.c_str());
        char eb[16];
        snprintf(eb, sizeof eb, "E%c%02d", ev < 0 ? '-' : '+', std::abs(ev));
        return m + eb;
    }
    int decimals = std::max(0, max_sig - 1 - e10);
    snprintf(buf, sizeof buf, "%.*f", decimals, r);
    std::string s = buf;
    if (s.find('.') != std::string::npos) { while (s.back() == '0') s.pop_back(); if (s.back() == '.') s.pop_back(); }
    if (s == "-0") s = "0";
    return s;
}

bool parse_number_strict(const std::string& s0, double& out) {
    size_t a = s0.find_first_not_of(" \t"), b = s0.find_last_not_of(" \t");
    if (a == std::string::npos) return false;
    std::string s = s0.substr(a, b - a + 1);
    size_t i = 0, n = s.size();
    if (s[i] == '+' || s[i] == '-') ++i;
    bool dig = false;
    while (i < n && isdigit((unsigned char)s[i])) { ++i; dig = true; }
    if (i < n && s[i] == '.') { ++i; while (i < n && isdigit((unsigned char)s[i])) { ++i; dig = true; } }
    if (!dig) return false;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        size_t k = i + 1;
        if (k < n && (s[k] == '+' || s[k] == '-')) ++k;
        if (k >= n || !isdigit((unsigned char)s[k])) return false;
        while (k < n && isdigit((unsigned char)s[k])) ++k;
        i = k;
    }
    if (i != n) return false;
    out = strtod(s.c_str(), nullptr);
    return std::isfinite(out);
}

std::string value_to_text(const Value& v) {
    switch (v.type) {
    case Value::Empty: return {};
    case Value::Number: return fmt_general(v.num);
    case Value::String: return v.str;
    case Value::Bool: return v.num != 0 ? "TRUE" : "FALSE";
    case Value::Error: return err_text(v.err);
    }
    return {};
}

// --------------------------------------------------------------- addresses --
Range Range::norm() const { return Range(std::min(r0, r1), std::min(c0, c1), std::max(r0, r1), std::max(c0, c1)); }

std::string col_name(int c) {
    std::string s;
    ++c;
    while (c > 0) { int m = (c - 1) % 26; s.insert(s.begin(), char('A' + m)); c = (c - 1) / 26; }
    return s;
}
int col_index(const std::string& l) {
    if (l.empty()) return -1;
    int v = 0;
    for (char ch : l) {
        if (!isalpha((unsigned char)ch)) return -1;
        v = v * 26 + (toupper((unsigned char)ch) - 'A' + 1);
        if (v > 1 << 24) return -1;
    }
    return v - 1;
}
std::string cell_name(int r, int c, bool ar, bool ac) {
    std::string s;
    if (ac) s += '$';
    s += col_name(c);
    if (ar) s += '$';
    s += std::to_string(r + 1);
    return s;
}
// parse "$A$1" at p; returns chars consumed or 0
static size_t scan_cell(std::string_view s, size_t p, int& r, int& c, bool& ar, bool& ac) {
    size_t i = p;
    ac = ar = false;
    if (i < s.size() && s[i] == '$') { ac = true; ++i; }
    size_t l0 = i;
    while (i < s.size() && isalpha((unsigned char)s[i]) && i - l0 < 4) ++i;
    size_t nl = i - l0;
    if (nl < 1 || nl > 3) return 0;
    if (i < s.size() && isalpha((unsigned char)s[i])) return 0;
    int col = col_index(std::string(s.substr(l0, nl)));
    if (col < 0 || col >= MAX_COLS) return 0;
    if (i < s.size() && s[i] == '$') { ar = true; ++i; }
    size_t d0 = i;
    while (i < s.size() && isdigit((unsigned char)s[i]) && i - d0 < 8) ++i;
    if (i == d0 || s[d0] == '0') return 0;
    long row = atol(std::string(s.substr(d0, i - d0)).c_str());
    if (row < 1 || row > MAX_ROWS) return 0;
    r = (int)row - 1; c = col;
    return i - p;
}
static size_t scan_col(std::string_view s, size_t p, int& c, bool& ac) {
    size_t i = p; ac = false;
    if (i < s.size() && s[i] == '$') { ac = true; ++i; }
    size_t l0 = i;
    while (i < s.size() && isalpha((unsigned char)s[i]) && i - l0 < 4) ++i;
    if (i - l0 < 1 || i - l0 > 3) return 0;
    int col = col_index(std::string(s.substr(l0, i - l0)));
    if (col < 0 || col >= MAX_COLS) return 0;
    c = col;
    return i - p;
}
static size_t scan_row(std::string_view s, size_t p, int& r, bool& ar) {
    size_t i = p; ar = false;
    if (i < s.size() && s[i] == '$') { ar = true; ++i; }
    size_t d0 = i;
    while (i < s.size() && isdigit((unsigned char)s[i]) && i - d0 < 8) ++i;
    if (i == d0 || s[d0] == '0') return 0;
    long row = atol(std::string(s.substr(d0, i - d0)).c_str());
    if (row < 1 || row > MAX_ROWS) return 0;
    r = (int)row - 1;
    return i - p;
}
bool parse_cell_name(std::string_view s, int& r, int& c) {
    bool a, b;
    return !s.empty() && scan_cell(s, 0, r, c, a, b) == s.size();
}
std::string range_name(const Range& rg) {
    if (rg.r0 == 0 && rg.r1 >= MAX_ROWS - 1) return col_name(rg.c0) + ":" + col_name(rg.c1);
    if (rg.c0 == 0 && rg.c1 >= MAX_COLS - 1) return std::to_string(rg.r0 + 1) + ":" + std::to_string(rg.r1 + 1);
    if (rg.r0 == rg.r1 && rg.c0 == rg.c1) return cell_name(rg.r0, rg.c0);
    return cell_name(rg.r0, rg.c0) + ":" + cell_name(rg.r1, rg.c1);
}
bool parse_range_name(std::string_view s, Range& rg) {
    int r, c, r2, c2; bool a, b;
    size_t n = scan_cell(s, 0, r, c, a, b);
    if (n) {
        if (n == s.size()) { rg = Range(r, c, r, c); return true; }
        if (s[n] != ':') return false;
        size_t m = scan_cell(s, n + 1, r2, c2, a, b);
        if (!m || n + 1 + m != s.size()) return false;
        rg = Range(r, c, r2, c2).norm();
        return true;
    }
    n = scan_col(s, 0, c, a);
    if (n && n < s.size() && s[n] == ':') {
        size_t m = scan_col(s, n + 1, c2, b);
        if (m && n + 1 + m == s.size()) { rg = Range(0, std::min(c, c2), MAX_ROWS - 1, std::max(c, c2)); return true; }
    }
    n = scan_row(s, 0, r, a);
    if (n && n < s.size() && s[n] == ':') {
        size_t m = scan_row(s, n + 1, r2, b);
        if (m && n + 1 + m == s.size()) { rg = Range(std::min(r, r2), 0, std::max(r, r2), MAX_COLS - 1); return true; }
    }
    return false;
}
std::string quote_sheet_name(const std::string& name) {
    bool need = name.empty() || isdigit((unsigned char)name[0]);
    for (unsigned char ch : name) if (!(isalnum(ch) || ch == '_' || ch == '.' || ch >= 0x80)) { need = true; break; }
    if (!need) {   // names that look like a cell ref (e.g. "A1", "R1C1") must be quoted
        int r, c;
        if (parse_cell_name(name, r, c)) need = true;
        std::string u = upper_ascii(name);
        if (u == "TRUE" || u == "FALSE") need = true;
    }
    if (!need) return name;
    std::string o = "'";
    for (char ch : name) { if (ch == '\'') o += '\''; o += ch; }
    return o + "'";
}

// ------------------------------------------------------------------- lexer --
namespace {

struct LTok {
    enum K : uint8_t { End, Space, Number, String, Bool, Error, Ref, Ident, Func, Op, LParen, RParen, Comma, Semi, LBrace, RBrace, Hash, Bad } k = End;
    size_t pos = 0, len = 0;
    std::string text;        // op text / ident / string value / sheet name
    double num = 0;
    // Ref
    std::string sheet, sheet2;   // empty = host
    bool has_sheet = false;
    int kind = 0;                // 0 cell, 1 area, 2 cols, 3 rows
    int r0 = 0, c0 = 0, r1 = 0, c1 = 0;
    bool ar0 = false, ac0 = false, ar1 = false, ac1 = false;
};

bool ident_start(unsigned char c) { return isalpha(c) || c == '_' || c == '\\' || c >= 0x80; }
bool ident_char(unsigned char c) { return isalnum(c) || c == '_' || c == '.' || c == '\\' || c >= 0x80 || c == '?'; }

// Try to parse a reference body (after an optional sheet prefix) at p. Returns consumed length.
size_t scan_ref_body(std::string_view s, size_t p, LTok& t) {
    int r, c, r2, c2; bool a, b, a2, b2;
    size_t n = scan_cell(s, p, r, c, a, b);
    auto boundary = [&](size_t q) { return q >= s.size() || !(ident_char((unsigned char)s[q]) || s[q] == '(' || s[q] == '$'); };
    if (n) {
        if (p + n < s.size() && s[p + n] == ':') {
            size_t m = scan_cell(s, p + n + 1, r2, c2, a2, b2);
            if (m && boundary(p + n + 1 + m)) {
                t.kind = 1; t.r0 = r; t.c0 = c; t.ar0 = a; t.ac0 = b; t.r1 = r2; t.c1 = c2; t.ar1 = a2; t.ac1 = b2;
                return n + 1 + m;
            }
        }
        if (boundary(p + n)) { t.kind = 0; t.r0 = r; t.c0 = c; t.ar0 = a; t.ac0 = b; return n; }
    }
    n = scan_col(s, p, c, b);
    if (n && p + n < s.size() && s[p + n] == ':') {
        size_t m = scan_col(s, p + n + 1, c2, b2);
        if (m && boundary(p + n + 1 + m)) { t.kind = 2; t.c0 = c; t.ac0 = b; t.c1 = c2; t.ac1 = b2; return n + 1 + m; }
    }
    n = scan_row(s, p, r, a);
    if (n && p + n < s.size() && s[p + n] == ':') {
        size_t m = scan_row(s, p + n + 1, r2, a2);
        if (m && boundary(p + n + 1 + m)) { t.kind = 3; t.r0 = r; t.ar0 = a; t.r1 = r2; t.ar1 = a2; return n + 1 + m; }
    }
    return 0;
}

std::vector<LTok> lex(std::string_view s) {
    std::vector<LTok> out;
    size_t i = 0;
    if (!s.empty() && s[0] == '=') i = 1;
    int brace = 0;
    while (i < s.size()) {
        LTok t; t.pos = i;
        unsigned char ch = s[i];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            size_t j = i; while (j < s.size() && (s[j] == ' ' || s[j] == '\t' || s[j] == '\r' || s[j] == '\n')) ++j;
            t.k = LTok::Space; t.len = j - i; out.push_back(t); i = j; continue;
        }
        if (ch == '"') {
            size_t j = i + 1; std::string v;
            bool closed = false;
            while (j < s.size()) {
                if (s[j] == '"') { if (j + 1 < s.size() && s[j + 1] == '"') { v += '"'; j += 2; continue; } closed = true; ++j; break; }
                v += s[j++];
            }
            t.k = closed ? LTok::String : LTok::Bad; t.text = v; t.len = j - i; out.push_back(t); i = j; continue;
        }
        if (ch == '#') {
            // error literal?
            static const char* errs[] = {"#NULL!", "#DIV/0!", "#VALUE!", "#REF!", "#NAME?", "#NUM!", "#N/A", "#SPILL!", "#CALC!", "#GETTING_DATA"};
            bool done = false;
            for (const char* e : errs) {
                size_t n = strlen(e);
                if (i + n <= s.size()) {
                    bool eq = true;
                    for (size_t k = 0; k < n; ++k) if (toupper((unsigned char)s[i + k]) != e[k]) { eq = false; break; }
                    if (eq) { t.k = LTok::Error; t.text = e; t.len = n; out.push_back(t); i += n; done = true; break; }
                }
            }
            if (done) continue;
            t.k = LTok::Hash; t.len = 1; out.push_back(t); ++i; continue;
        }
        // sheet-qualified reference: 'name'!  or  name!
        if (ch == '\'') {
            size_t j = i + 1; std::string nm;
            while (j < s.size()) {
                if (s[j] == '\'') { if (j + 1 < s.size() && s[j + 1] == '\'') { nm += '\''; j += 2; continue; } ++j; break; }
                nm += s[j++];
            }
            std::string nm2;
            // 'Sheet1:Sheet3'!A1 (3-D inside quotes)
            size_t colon = nm.find(':');
            if (colon != std::string::npos) { nm2 = nm.substr(colon + 1); nm = nm.substr(0, colon); }
            if (j < s.size() && s[j] == '!') {
                size_t n = scan_ref_body(s, j + 1, t);
                t.k = LTok::Ref; t.has_sheet = true; t.sheet = nm; t.sheet2 = nm2;
                if (!n) {
                    // 'Sheet'!#REF!
                    if (s.substr(j + 1, 5) == "#REF!") { t.k = LTok::Error; t.text = "#REF!"; t.len = j + 6 - i; out.push_back(t); i = j + 6; continue; }
                    t.k = LTok::Bad; t.len = j + 1 - i; out.push_back(t); i = j + 1; continue;
                }
                t.len = j + 1 + n - i; out.push_back(t); i += t.len; continue;
            }
            t.k = LTok::Bad; t.len = j - i; out.push_back(t); i = j; continue;
        }
        if (ident_start(ch) || ch == '$' || isdigit(ch)) {
            // sheet prefix: run of ident chars followed by '!' (or name:name!)
            size_t j = i;
            while (j < s.size() && (ident_char((unsigned char)s[j]))) ++j;
            if (j < s.size() && s[j] == '!' && j > i) {
                std::string nm(s.substr(i, j - i));
                size_t n = scan_ref_body(s, j + 1, t);
                if (n) { t.k = LTok::Ref; t.has_sheet = true; t.sheet = nm; t.len = j + 1 + n - i; out.push_back(t); i += t.len; continue; }
                if (s.substr(j + 1, 5) == "#REF!") { t.k = LTok::Error; t.text = "#REF!"; t.len = j + 6 - i; out.push_back(t); i = j + 6; continue; }
            }
            if (j < s.size() && s[j] == ':' && j > i) {   // Sheet1:Sheet3!A1
                size_t k = j + 1;
                while (k < s.size() && ident_char((unsigned char)s[k])) ++k;
                if (k < s.size() && s[k] == '!' && k > j + 1) {
                    LTok t2;
                    size_t n = scan_ref_body(s, k + 1, t2);
                    if (n) {
                        t2.pos = i; t2.k = LTok::Ref; t2.has_sheet = true; t2.sheet = std::string(s.substr(i, j - i)); t2.sheet2 = std::string(s.substr(j + 1, k - j - 1));
                        t2.len = k + 1 + n - i; out.push_back(t2); i += t2.len; continue;
                    }
                }
            }
            // plain reference?
            size_t n = scan_ref_body(s, i, t);
            if (n) { t.k = LTok::Ref; t.len = n; out.push_back(t); i += n; continue; }
            if (isdigit(ch) || (ch == '.' && i + 1 < s.size() && isdigit((unsigned char)s[i + 1]))) {
                size_t k = i;
                while (k < s.size() && isdigit((unsigned char)s[k])) ++k;
                if (k < s.size() && s[k] == '.') { ++k; while (k < s.size() && isdigit((unsigned char)s[k])) ++k; }
                if (k < s.size() && (s[k] == 'e' || s[k] == 'E')) {
                    size_t q = k + 1;
                    if (q < s.size() && (s[q] == '+' || s[q] == '-')) ++q;
                    if (q < s.size() && isdigit((unsigned char)s[q])) { while (q < s.size() && isdigit((unsigned char)s[q])) ++q; k = q; }
                }
                t.k = LTok::Number; t.num = strtod(std::string(s.substr(i, k - i)).c_str(), nullptr); t.text = std::string(s.substr(i, k - i)); t.len = k - i;
                out.push_back(t); i = k; continue;
            }
            if (ch == '$') { t.k = LTok::Bad; t.len = 1; out.push_back(t); ++i; continue; }
            // identifier
            std::string id(s.substr(i, j - i));
            size_t q = j;
            if (q < s.size() && s[q] == '(') { t.k = LTok::Func; t.text = id; t.len = j - i; out.push_back(t); i = j; continue; }
            std::string u = upper_ascii(id);
            if (u == "TRUE" || u == "FALSE") { t.k = LTok::Bool; t.num = u == "TRUE"; t.text = u; t.len = j - i; out.push_back(t); i = j; continue; }
            t.k = LTok::Ident; t.text = id; t.len = j - i; out.push_back(t); i = j; continue;
        }
        if (ch == '.' && i + 1 < s.size() && isdigit((unsigned char)s[i + 1])) {
            size_t k = i + 1;
            while (k < s.size() && isdigit((unsigned char)s[k])) ++k;
            if (k < s.size() && (s[k] == 'e' || s[k] == 'E')) {
                size_t q = k + 1;
                if (q < s.size() && (s[q] == '+' || s[q] == '-')) ++q;
                if (q < s.size() && isdigit((unsigned char)s[q])) { while (q < s.size() && isdigit((unsigned char)s[q])) ++q; k = q; }
            }
            t.k = LTok::Number; t.text = std::string(s.substr(i, k - i)); t.num = strtod(t.text.c_str(), nullptr); t.len = k - i;
            out.push_back(t); i = k; continue;
        }
        switch (ch) {
        case '(': t.k = LTok::LParen; break;
        case ')': t.k = LTok::RParen; break;
        case ',': t.k = LTok::Comma; break;
        case ';': t.k = LTok::Semi; break;
        case '{': t.k = LTok::LBrace; ++brace; break;
        case '}': t.k = LTok::RBrace; --brace; break;
        case '+': case '-': case '*': case '/': case '^': case '&': case '%': case ':':
            t.k = LTok::Op; t.text = std::string(1, (char)ch); break;
        case '=': t.k = LTok::Op; t.text = "="; break;
        case '<':
            if (i + 1 < s.size() && (s[i + 1] == '=' || s[i + 1] == '>')) { t.k = LTok::Op; t.text = std::string(s.substr(i, 2)); t.len = 2; out.push_back(t); i += 2; continue; }
            t.k = LTok::Op; t.text = "<"; break;
        case '>':
            if (i + 1 < s.size() && s[i + 1] == '=') { t.k = LTok::Op; t.text = ">="; t.len = 2; out.push_back(t); i += 2; continue; }
            t.k = LTok::Op; t.text = ">"; break;
        default: t.k = LTok::Bad; break;
        }
        t.len = 1; out.push_back(t); ++i;
    }
    return out;
}

// ------------------------------------------------------------------ parser --
struct Parser {
    const std::vector<LTok>& t;
    const Book& book;
    int si, hr, hc;
    size_t p = 0;
    std::vector<Tok> out;
    std::string err;
    bool is_volatile = false, dyn = false;

    Parser(const std::vector<LTok>& tk, const Book& b, int s, int r, int c) : t(tk), book(b), si(s), hr(r), hc(c) {}

    void skip_sp() { while (p < t.size() && t[p].k == LTok::Space) ++p; }
    const LTok* peek() { skip_sp(); return p < t.size() ? &t[p] : nullptr; }
    bool peek_op(const char* o) { const LTok* x = peek(); return x && x->k == LTok::Op && x->text == o; }
    bool fail(const std::string& e) { if (err.empty()) err = e; return false; }

    void emit_op(Tok::OpCode op, int argc) { Tok k; k.kind = Tok::Op; k.op = op; k.argc = (uint8_t)argc; out.push_back(std::move(k)); }

    bool expr() { return cmp(); }
    bool cmp() {
        if (!concat()) return false;
        for (;;) {
            const LTok* x = peek();
            if (!x || x->k != LTok::Op) break;
            Tok::OpCode op;
            if (x->text == "=") op = Tok::Eq; else if (x->text == "<>") op = Tok::Ne; else if (x->text == "<") op = Tok::Lt;
            else if (x->text == "<=") op = Tok::Le; else if (x->text == ">") op = Tok::Gt; else if (x->text == ">=") op = Tok::Ge; else break;
            ++p;
            if (!concat()) return false;
            emit_op(op, 2);
        }
        return true;
    }
    bool concat() {
        if (!add()) return false;
        while (peek_op("&")) { ++p; if (!add()) return false; emit_op(Tok::Concat, 2); }
        return true;
    }
    bool add() {
        if (!mul()) return false;
        for (;;) {
            if (peek_op("+")) { ++p; if (!mul()) return false; emit_op(Tok::Add, 2); }
            else if (peek_op("-")) { ++p; if (!mul()) return false; emit_op(Tok::Sub, 2); }
            else break;
        }
        return true;
    }
    bool mul() {
        if (!pow()) return false;
        for (;;) {
            if (peek_op("*")) { ++p; if (!pow()) return false; emit_op(Tok::Mul, 2); }
            else if (peek_op("/")) { ++p; if (!pow()) return false; emit_op(Tok::Div, 2); }
            else break;
        }
        return true;
    }
    bool pow() {
        if (!pct()) return false;
        while (peek_op("^")) { ++p; if (!pct()) return false; emit_op(Tok::Pow, 2); }
        return true;
    }
    bool pct() {
        if (!neg()) return false;
        while (peek_op("%")) { ++p; emit_op(Tok::Percent, 1); }
        return true;
    }
    bool neg() {
        if (peek_op("-")) { ++p; if (!neg()) return false; emit_op(Tok::Neg, 1); return true; }
        if (peek_op("+")) { ++p; if (!neg()) return false; emit_op(Tok::Plus, 1); return true; }
        return isect();
    }
    static bool starts_operand(const LTok& x) {
        return x.k == LTok::Ref || x.k == LTok::Ident || x.k == LTok::Func || x.k == LTok::LParen;
    }
    bool isect() {
        if (!range()) return false;
        for (;;) {
            // intersection: whitespace between two reference-ish operands
            if (p < t.size() && t[p].k == LTok::Space) {
                size_t q = p; while (q < t.size() && t[q].k == LTok::Space) ++q;
                if (q < t.size() && starts_operand(t[q]) && last_is_refish()) { p = q; if (!range()) return false; emit_op(Tok::Isect, 2); continue; }
            }
            break;
        }
        return true;
    }
    bool last_is_refish() const {
        if (out.empty()) return false;
        const Tok& k = out.back();
        if (k.kind == Tok::Ref || k.kind == Tok::Area || k.kind == Tok::Name || k.kind == Tok::BadRef) return true;
        if (k.kind == Tok::Paren) return true;
        if (k.kind == Tok::Op && (k.op == Tok::RangeOp || k.op == Tok::Isect || k.op == Tok::Union || k.op == Tok::SpillRef)) return true;
        if (k.kind == Tok::Func) {
            static const char* rf[] = {"INDEX", "OFFSET", "INDIRECT", "IF", "CHOOSE", "XLOOKUP"};
            const auto& tab = fn_table();
            if (k.fn < tab.size()) for (auto* n : rf) if (!strcmp(tab[k.fn].info.name, n)) return true;
        }
        return false;
    }
    bool range() {
        if (!postfix()) return false;
        while (p < t.size() && t[p].k == LTok::Op && t[p].text == ":") { ++p; skip_sp(); if (!postfix()) return false; emit_op(Tok::RangeOp, 2); }
        return true;
    }
    bool postfix() {
        if (!primary()) return false;
        while (p < t.size() && t[p].k == LTok::Hash) { ++p; emit_op(Tok::SpillRef, 1); }
        return true;
    }
    int sheet_id_of(const std::string& nm, bool& ok) {
        int ix = book.find_sheet(nm);
        if (ix < 0) { ok = false; return -1; }
        ok = true;
        return (int)book.sheets[ix].id;
    }
    bool ref_tok(const LTok& x) {
        Tok k;
        bool bad = false;
        if (x.has_sheet) {
            bool ok;
            k.sheet_id = sheet_id_of(x.sheet, ok);
            if (!ok) bad = true;
            if (!x.sheet2.empty()) { k.sheet_id2 = sheet_id_of(x.sheet2, ok); if (!ok) bad = true; }
        }
        if (bad) { Tok b; b.kind = Tok::BadRef; out.push_back(b); return true; }
        auto part = [&](int r, int c, bool ar, bool ac) {
            RefPart rp; rp.abs_r = ar; rp.abs_c = ac;
            rp.row = ar ? r : r - hr; rp.col = ac ? c : c - hc;
            return rp;
        };
        if (x.kind == 0) { k.kind = Tok::Ref; k.a = part(x.r0, x.c0, x.ar0, x.ac0); }
        else if (x.kind == 1) { k.kind = Tok::Area; k.a = part(x.r0, x.c0, x.ar0, x.ac0); k.b = part(x.r1, x.c1, x.ar1, x.ac1); }
        else if (x.kind == 2) { k.kind = Tok::Area; k.whole_cols = true; k.a = part(0, x.c0, true, x.ac0); k.b = part(MAX_ROWS - 1, x.c1, true, x.ac1); }
        else { k.kind = Tok::Area; k.whole_rows = true; k.a = part(x.r0, 0, x.ar0, true); k.b = part(x.r1, MAX_COLS - 1, x.ar1, true); }
        out.push_back(std::move(k));
        return true;
    }
    bool array_const() {
        // after '{'
        std::vector<std::vector<Value>> rows(1);
        for (;;) {
            const LTok* x = peek();
            if (!x) return fail("unterminated array constant");
            Value v;
            bool negv = false;
            if (x->k == LTok::Op && (x->text == "-" || x->text == "+")) { negv = x->text == "-"; ++p; x = peek(); if (!x) return fail("bad array constant"); }
            if (x->k == LTok::Number) v = Value::number(negv ? -x->num : x->num);
            else if (x->k == LTok::String && !negv) v = Value::text(x->text);
            else if (x->k == LTok::Bool && !negv) v = Value::boolean(x->num != 0);
            else if (x->k == LTok::Error && !negv) v = Value::error(err_from_text(x->text));
            else return fail("bad array constant element");
            ++p;
            rows.back().push_back(v);
            x = peek();
            if (!x) return fail("unterminated array constant");
            if (x->k == LTok::Comma) { ++p; continue; }
            if (x->k == LTok::Semi) { ++p; rows.emplace_back(); continue; }
            if (x->k == LTok::RBrace) { ++p; break; }
            return fail("bad array constant separator");
        }
        size_t nc = rows[0].size();
        for (auto& r : rows) if (r.size() != nc) return fail("array constant rows differ in length");
        auto a = std::make_shared<Array>((int)rows.size(), (int)nc);
        for (size_t r = 0; r < rows.size(); ++r) for (size_t c = 0; c < nc; ++c) a->at((int)r, (int)c) = rows[r][c];
        Tok k; k.kind = Tok::ArrayConst; k.arr = a;
        out.push_back(std::move(k));
        return true;
    }
    bool primary() {
        const LTok* x = peek();
        if (!x) return fail("unexpected end of formula");
        switch (x->k) {
        case LTok::Number: { Tok k; k.kind = Tok::Num; k.num = x->num; out.push_back(k); ++p; return true; }
        case LTok::String: { Tok k; k.kind = Tok::Str; k.str = x->text; out.push_back(k); ++p; return true; }
        case LTok::Bool: { Tok k; k.kind = Tok::Bool; k.num = x->num; out.push_back(k); ++p; return true; }
        case LTok::Error: {
            Tok k; k.kind = Tok::ErrLit; k.num = (double)(int)err_from_text(x->text);
            if (x->text == "#REF!") k.kind = Tok::BadRef;
            if (x->text == "#GETTING_DATA") k.num = (double)(int)Err::NA;
            out.push_back(k); ++p; return true;
        }
        case LTok::Ref: ++p; return ref_tok(*x);
        case LTok::Ident: {
            Tok k; k.kind = Tok::Name; k.str = x->text; out.push_back(k); ++p; return true;
        }
        case LTok::Func: {
            std::string name = x->text;
            ++p;   // name
            ++p;   // '('
            std::string u = upper_ascii(name);
            for (const char* pre : {"_XLFN._XLWS.", "_XLFN.", "_XLWS.", "COM.MICROSOFT.", "ORG.OPENOFFICE."})
                if (u.rfind(pre, 0) == 0) { u = u.substr(strlen(pre)); break; }
            int argc = 0;
            skip_sp();
            if (p < t.size() && t[p].k == LTok::RParen) { ++p; }
            else {
                for (;;) {
                    skip_sp();
                    if (p < t.size() && (t[p].k == LTok::Comma || t[p].k == LTok::RParen)) { Tok m; m.kind = Tok::Missing; out.push_back(m); }
                    else if (!expr()) return false;
                    ++argc;
                    skip_sp();
                    if (p >= t.size()) return fail("missing ')'");
                    if (t[p].k == LTok::Comma) { ++p; continue; }
                    if (t[p].k == LTok::RParen) { ++p; break; }
                    return fail("expected ',' or ')' in function arguments");
                }
            }
            if (argc > 255) return fail("too many arguments");
            Tok k; k.kind = Tok::Func; k.argc = (uint8_t)argc;
            int id = fn_lookup(u);
            if (id < 0) { k.fn = 0xFFFF; k.str = u; }
            else {
                const FnDef& d = fn_table()[id];
                k.fn = (uint16_t)id;
                if (argc < d.info.min_args || (d.info.max_args >= 0 && argc > d.info.max_args))
                    return fail(std::string("wrong number of arguments for ") + d.info.name);
                if (d.flags & FN_VOLATILE) is_volatile = true;
                if (d.flags & FN_DYNREF) { dyn = true; is_volatile = true; }
            }
            out.push_back(std::move(k));
            return true;
        }
        case LTok::LParen: {
            ++p;
            if (!expr()) return false;
            int n = 1;
            for (;;) {
                skip_sp();
                if (p < t.size() && t[p].k == LTok::Comma) { ++p; if (!expr()) return false; emit_op(Tok::Union, 2); ++n; continue; }
                break;
            }
            skip_sp();
            if (p >= t.size() || t[p].k != LTok::RParen) return fail("missing ')'");
            ++p;
            Tok k; k.kind = Tok::Paren; out.push_back(k);
            return true;
        }
        case LTok::LBrace: ++p; return array_const();
        default: return fail("unexpected '" + std::string(1, '?') + "' in formula");
        }
    }
};

} // namespace

bool compile_formula(const std::string& text, const Book& book, int si, int r, int c, Formula& out, std::string& err) {
    out = Formula();
    std::string s = text;
    if (!s.empty() && s[0] == '=') s.erase(0, 1);
    auto toks = lex(s);
    for (auto& x : toks) if (x.k == LTok::Bad) { err = "invalid character in formula at " + std::to_string(x.pos + 1); out.bad_text = s; return false; }
    Parser P(toks, book, si, r, c);
    bool ok = P.expr();
    if (ok) { P.skip_sp(); if (P.p < toks.size()) ok = P.fail("unexpected text after expression"); }
    if (!ok) { err = P.err; out.bad_text = s; return false; }
    out.code = std::move(P.out);
    out.is_volatile = P.is_volatile;
    out.dynamic_refs = P.dyn;
    return true;
}

// ----------------------------------------------------------------- printer --
static std::string sheet_prefix(const Book& b, const Tok& k) {
    if (k.sheet_id < 0) return {};
    int ix = b.sheet_index_of_id((uint32_t)k.sheet_id);
    if (ix < 0) return "#REF!";
    std::string nm = b.sheets[ix].name;
    if (k.sheet_id2 >= 0) {
        int j = b.sheet_index_of_id((uint32_t)k.sheet_id2);
        if (j >= 0) {
            std::string a = quote_sheet_name(nm), c = quote_sheet_name(b.sheets[j].name);
            if (a[0] == '\'' || c[0] == '\'') {
                std::string q = "'";
                for (char ch : nm + ":" + b.sheets[j].name) { if (ch == '\'') q += '\''; q += ch; }
                return q + "'!";
            }
            return a + ":" + c + "!";
        }
    }
    return quote_sheet_name(nm) + "!";
}

static std::string ref_text(const Tok& k, int hr, int hc) {
    auto rr = [&](const RefPart& p) { return abs_row(p, hr); };
    auto cc = [&](const RefPart& p) { return abs_col(p, hc); };
    if (k.kind == Tok::Ref) {
        int r = rr(k.a), c = cc(k.a);
        if (r < 0 || c < 0 || r >= MAX_ROWS || c >= MAX_COLS) return "#REF!";
        return cell_name(r, c, k.a.abs_r, k.a.abs_c);
    }
    if (k.whole_cols) {
        int c0 = cc(k.a), c1 = cc(k.b);
        if (c0 < 0 || c1 < 0 || c0 >= MAX_COLS || c1 >= MAX_COLS) return "#REF!";
        return std::string(k.a.abs_c ? "$" : "") + col_name(c0) + ":" + (k.b.abs_c ? "$" : "") + col_name(c1);
    }
    if (k.whole_rows) {
        int r0 = rr(k.a), r1 = rr(k.b);
        if (r0 < 0 || r1 < 0 || r0 >= MAX_ROWS || r1 >= MAX_ROWS) return "#REF!";
        return std::string(k.a.abs_r ? "$" : "") + std::to_string(r0 + 1) + ":" + (k.b.abs_r ? "$" : "") + std::to_string(r1 + 1);
    }
    int r0 = rr(k.a), c0 = cc(k.a), r1 = rr(k.b), c1 = cc(k.b);
    if (r0 < 0 || c0 < 0 || r1 < 0 || c1 < 0 || r0 >= MAX_ROWS || r1 >= MAX_ROWS || c0 >= MAX_COLS || c1 >= MAX_COLS) return "#REF!";
    return cell_name(r0, c0, k.a.abs_r, k.a.abs_c) + ":" + cell_name(r1, c1, k.b.abs_r, k.b.abs_c);
}

static std::string num_literal(double d) {
    std::string s = fmt_general(d, 15);
    // keep full precision when 15 digits lose information
    char buf[40];
    if (atof(s.c_str()) != d) { snprintf(buf, sizeof buf, "%.17g", d); s = buf; }
    return s;
}

static std::string quote_str(const std::string& s) {
    std::string o = "\"";
    for (char ch : s) { if (ch == '"') o += '"'; o += ch; }
    return o + "\"";
}

std::string formula_text(const Formula& f, const Book& book, int si, int r, int c) {
    (void)si;
    if (!f.bad_text.empty() && f.code.empty()) return f.bad_text;
    std::vector<std::string> st;
    const auto& tab = fn_table();
    for (const Tok& k : f.code) {
        switch (k.kind) {
        case Tok::Num: st.push_back(num_literal(k.num)); break;
        case Tok::Str: st.push_back(quote_str(k.str)); break;
        case Tok::Bool: st.push_back(k.num != 0 ? "TRUE" : "FALSE"); break;
        case Tok::ErrLit: st.push_back(err_text((Err)(int)k.num)); break;
        case Tok::BadRef: st.push_back("#REF!"); break;
        case Tok::Ref: case Tok::Area: {
            std::string rt = ref_text(k, r, c);
            st.push_back(rt == "#REF!" ? rt : sheet_prefix(book, k) + rt);
            break;
        }
        case Tok::ArrayConst: {
            std::string o = "{";
            for (int i = 0; i < k.arr->rows; ++i) {
                if (i) o += ';';
                for (int j = 0; j < k.arr->cols; ++j) {
                    if (j) o += ',';
                    const Value& v = k.arr->at(i, j);
                    if (v.is_str()) o += quote_str(v.str);
                    else if (v.is_num()) o += num_literal(v.num);
                    else o += value_to_text(v);
                }
            }
            st.push_back(o + "}");
            break;
        }
        case Tok::Name: st.push_back(k.str); break;
        case Tok::Missing: st.push_back(""); break;
        case Tok::Paren: if (!st.empty()) st.back() = "(" + st.back() + ")"; break;
        case Tok::Func: {
            size_t n = k.argc;
            if (st.size() < n) return f.bad_text.empty() ? "#ERR" : f.bad_text;
            std::string o = k.fn < tab.size() ? tab[k.fn].info.name : k.str;
            o += '(';
            for (size_t i = st.size() - n; i < st.size(); ++i) { if (i > st.size() - n) o += ','; o += st[i]; }
            o += ')';
            st.resize(st.size() - n);
            st.push_back(o);
            break;
        }
        case Tok::Op: {
            static const char* ops[] = {"+", "-", "*", "/", "^", "&", "=", "<>", "<", "<=", ">", ">=", "-", "+", "%", ":", ",", " ", "#"};
            if (k.op == Tok::Neg || k.op == Tok::Plus) { if (st.empty()) return "#ERR"; st.back() = ops[k.op] + st.back(); }
            else if (k.op == Tok::Percent || k.op == Tok::SpillRef) { if (st.empty()) return "#ERR"; st.back() += ops[k.op]; }
            else {
                if (st.size() < 2) return "#ERR";
                std::string b = st.back(); st.pop_back();
                st.back() = st.back() + ops[k.op] + b;
            }
            break;
        }
        }
    }
    return st.empty() ? std::string() : st.back();
}

// ------------------------------------------------------- tokens for the UI --
std::vector<LexToken> tokenize_formula(std::string_view text) {
    std::vector<LexToken> out;
    int depth = 0;
    for (auto& x : lex(text)) {
        LexToken t; t.pos = (int)x.pos; t.len = (int)x.len;
        switch (x.k) {
        case LTok::Space: t.kind = LexToken::Space; break;
        case LTok::Number: t.kind = LexToken::Number; break;
        case LTok::String: t.kind = LexToken::String; break;
        case LTok::Bool: t.kind = LexToken::Bool; break;
        case LTok::Error: t.kind = LexToken::Error; break;
        case LTok::Ref: t.kind = LexToken::Ref; break;
        case LTok::Ident: t.kind = LexToken::Name; break;
        case LTok::Func: t.kind = LexToken::Function; break;
        case LTok::Op: case LTok::Hash: t.kind = LexToken::Operator; break;
        case LTok::LParen: t.kind = LexToken::OpenParen; t.depth = depth++; break;
        case LTok::RParen: t.kind = LexToken::CloseParen; t.depth = depth > 0 ? --depth : 0; break;
        case LTok::Comma: t.kind = LexToken::Comma; break;
        case LTok::Semi: t.kind = LexToken::Semicolon; break;
        case LTok::LBrace: t.kind = LexToken::OpenBrace; break;
        case LTok::RBrace: t.kind = LexToken::CloseBrace; break;
        default: t.kind = LexToken::Unknown; break;
        }
        if (t.kind != LexToken::OpenParen && t.kind != LexToken::CloseParen) t.depth = depth;
        out.push_back(t);
    }
    return out;
}

std::vector<UsedRef> formula_refs(std::string_view text, const Book& book, int si, int r, int c) {
    (void)r; (void)c;
    std::vector<UsedRef> out;
    for (auto& x : lex(text)) {
        if (x.k != LTok::Ref) continue;
        UsedRef u; u.si = -1; u.text_pos = (int)x.pos; u.text_len = (int)x.len;
        if (x.has_sheet) { u.si = book.find_sheet(x.sheet); if (u.si < 0) continue; if (u.si == si) u.si = -1; }
        if (x.kind == 0) u.rg = Range(x.r0, x.c0, x.r0, x.c0);
        else if (x.kind == 1) u.rg = Range(x.r0, x.c0, x.r1, x.c1).norm();
        else if (x.kind == 2) u.rg = Range(0, std::min(x.c0, x.c1), MAX_ROWS - 1, std::max(x.c0, x.c1));
        else u.rg = Range(std::min(x.r0, x.r1), 0, std::max(x.r0, x.r1), MAX_COLS - 1);
        out.push_back(u);
    }
    return out;
}

// ------------------------------------------------------ structural adjust --
static bool adjust_part_axis(int& lo, int& hi, const StructOp& op) {
    // returns false when the whole span was deleted
    if (op.n > 0) {
        if (lo >= op.at) lo += op.n;
        if (hi >= op.at) hi += op.n;
        return true;
    }
    int d0 = op.at, d1 = op.at - op.n;   // [d0, d1)
    int nlo = lo < d0 ? lo : (lo < d1 ? d0 : lo + op.n);
    int nhi = hi < d0 ? hi : (hi < d1 ? d0 - 1 : hi + op.n);
    if (nhi < nlo) return false;
    lo = nlo; hi = nhi;
    return true;
}

FormulaPtr adjust_formula(const Formula& f, uint32_t host_sheet_id, int old_r, int old_c, int new_r, int new_c, const StructOp& op) {
    bool changed = false;
    auto nf = std::make_shared<Formula>(f);
    for (Tok& k : nf->code) {
        if (k.kind != Tok::Ref && k.kind != Tok::Area) continue;
        uint32_t sid = k.sheet_id < 0 ? host_sheet_id : (uint32_t)k.sheet_id;
        bool on_op_sheet = sid == op.sheet_id && k.sheet_id2 < 0;
        RefPart pa = k.a, pb = k.kind == Tok::Area ? k.b : k.a;
        int ra = abs_row(pa, old_r), ca = abs_col(pa, old_c), rb = abs_row(pb, old_r), cb = abs_col(pb, old_c);
        if (on_op_sheet) {
            if (k.kind == Tok::Ref) {
                int& v = op.rows ? ra : ca;
                int m = op.map(v);
                if (m < 0 || m >= (op.rows ? MAX_ROWS : MAX_COLS)) { Tok b; b.kind = Tok::BadRef; k = b; changed = true; continue; }
                v = m; rb = ra; cb = ca;
            } else {
                bool whole_axis = op.rows ? k.whole_cols : k.whole_rows;
                if (!whole_axis) {
                    int lo = op.rows ? std::min(ra, rb) : std::min(ca, cb), hi = op.rows ? std::max(ra, rb) : std::max(ca, cb);
                    bool swapped = op.rows ? ra > rb : ca > cb;
                    if (!adjust_part_axis(lo, hi, op)) { Tok b; b.kind = Tok::BadRef; k = b; changed = true; continue; }
                    int lim = op.rows ? MAX_ROWS : MAX_COLS;
                    if (hi >= lim) hi = lim - 1;
                    if (lo >= lim) { Tok b; b.kind = Tok::BadRef; k = b; changed = true; continue; }
                    if (op.rows) { ra = swapped ? hi : lo; rb = swapped ? lo : hi; }
                    else { ca = swapped ? hi : lo; cb = swapped ? lo : hi; }
                }
            }
        }
        RefPart na = pa, nb = pb;
        na.row = pa.abs_r ? ra : ra - new_r; na.col = pa.abs_c ? ca : ca - new_c;
        nb.row = pb.abs_r ? rb : rb - new_r; nb.col = pb.abs_c ? cb : cb - new_c;
        if (na.row != k.a.row || na.col != k.a.col) changed = true;
        k.a = na;
        if (k.kind == Tok::Area) { if (nb.row != k.b.row || nb.col != k.b.col) changed = true; k.b = nb; }
    }
    if (!changed) return nullptr;
    return nf;
}

FormulaPtr rehost_formula(const Formula& f, int old_r, int old_c, int new_r, int new_c) {
    auto nf = std::make_shared<Formula>(f);
    for (Tok& k : nf->code) {
        if (k.kind != Tok::Ref && k.kind != Tok::Area) continue;
        auto fix = [&](RefPart& p) {
            if (!p.abs_r) p.row = old_r + p.row - new_r;
            if (!p.abs_c) p.col = old_c + p.col - new_c;
        };
        fix(k.a);
        if (k.kind == Tok::Area) fix(k.b);
    }
    return nf;
}

FormulaPtr drop_sheet_refs(const Formula& f, uint32_t sheet_id) {
    bool changed = false;
    auto nf = std::make_shared<Formula>(f);
    for (Tok& k : nf->code)
        if ((k.kind == Tok::Ref || k.kind == Tok::Area) && (k.sheet_id == (int)sheet_id || k.sheet_id2 == (int)sheet_id)) { Tok b; b.kind = Tok::BadRef; k = b; changed = true; }
    return changed ? nf : nullptr;
}

void formula_precedents(const Book& b, const Formula& f, int si, int r, int c, std::vector<Area>& out, bool& has_names) {
    has_names = false;
    for (const Tok& k : f.code) {
        if (k.kind == Tok::Name) { has_names = true; continue; }
        if (k.kind != Tok::Ref && k.kind != Tok::Area) continue;
        int s0 = k.sheet_id < 0 ? si : b.sheet_index_of_id((uint32_t)k.sheet_id);
        if (s0 < 0) continue;
        int s1 = s0;
        if (k.sheet_id2 >= 0) { s1 = b.sheet_index_of_id((uint32_t)k.sheet_id2); if (s1 < 0) continue; if (s1 < s0) std::swap(s0, s1); }
        int r0 = abs_row(k.a, r), c0 = abs_col(k.a, c);
        int r1 = k.kind == Tok::Area ? abs_row(k.b, r) : r0, c1 = k.kind == Tok::Area ? abs_col(k.b, c) : c0;
        Range rg = Range(r0, c0, r1, c1).norm();
        if (rg.r0 < 0 || rg.c0 < 0) continue;
        for (int s = s0; s <= s1; ++s) out.push_back({s, rg});
    }
}

} // namespace pcbp::sheet
