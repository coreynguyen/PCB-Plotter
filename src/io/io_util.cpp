// ============================================================================
// io_util.cpp — shared helpers for pcbp::io importers.
// ============================================================================
#include "io_util.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace pcbp::io {

static inline bool is_sp(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && is_sp(s[a])) ++a;
    while (b > a && is_sp(s[b - 1])) --b;
    return std::string(s.substr(a, b - a));
}

std::string to_lower(std::string_view s) {
    std::string r(s);
    for (auto& c : r) c = (char)std::tolower((unsigned char)c);
    return r;
}
std::string to_upper(std::string_view s) {
    std::string r(s);
    for (auto& c : r) c = (char)std::toupper((unsigned char)c);
    return r;
}

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}
bool istarts_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && ieq(s.substr(0, p.size()), p);
}
bool icontains(std::string_view hay, std::string_view needle) {
    return to_lower(hay).find(to_lower(needle)) != std::string::npos;
}

std::string norm_key(std::string_view s) {
    std::string r;
    for (char c : s) if (std::isalnum((unsigned char)c)) r += (char)std::tolower((unsigned char)c);
    return r;
}

// Locale-independent decimal parser. Returns chars consumed (0 = fail).
static size_t scan_number(std::string_view s, double& v) {
    size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; ++i; }
    double ip = 0; int nd = 0;
    while (i < s.size() && std::isdigit((unsigned char)s[i])) { ip = ip * 10 + (s[i] - '0'); ++i; ++nd; }
    double fp = 0, sc = 1;
    if (i < s.size() && (s[i] == '.' || s[i] == ',')) {
        size_t j = i + 1; int fd = 0;
        while (j < s.size() && std::isdigit((unsigned char)s[j])) { fp = fp * 10 + (s[j] - '0'); sc *= 10; ++j; ++fd; }
        if (s[i] == ',' && fd == 0) { /* trailing comma: not part of number */ }
        else { i = j; nd += fd; }
    }
    if (nd == 0) return 0;
    double r = ip + fp / sc;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        size_t j = i + 1; bool en = false; int ex = 0, ed = 0;
        if (j < s.size() && (s[j] == '+' || s[j] == '-')) { en = s[j] == '-'; ++j; }
        while (j < s.size() && std::isdigit((unsigned char)s[j])) { ex = ex * 10 + (s[j] - '0'); ++j; ++ed; }
        if (ed) { r *= std::pow(10.0, en ? -ex : ex); i = j; }
    }
    v = neg ? -r : r;
    return i;
}

std::optional<double> parse_num(std::string_view in) {
    std::string s = trim(in);
    if (s.empty()) return std::nullopt;
    // "1,234.5" thousands separators are not supported; ',' only as decimal mark.
    if (s.find('.') != std::string::npos && s.find(',') != std::string::npos) return std::nullopt;
    double v = 0;
    size_t n = scan_number(s, v);
    if (n == 0 || n != s.size()) return std::nullopt;
    return v;
}

std::optional<double> parse_num_suffix(std::string_view in, std::string* suffix) {
    std::string s = trim(in);
    if (s.empty()) return std::nullopt;
    double v = 0;
    size_t n = scan_number(s, v);
    if (n == 0) return std::nullopt;
    std::string suf = to_lower(trim(std::string_view(s).substr(n)));
    if (!suf.empty()) {
        for (char c : suf) if (!std::isalpha((unsigned char)c) && c != '"' && c != '\'') return std::nullopt;
        if (suf.size() > 4) return std::nullopt;
    }
    if (suffix) *suffix = suf;
    return v;
}

bool looks_numeric(std::string_view s) { return parse_num_suffix(s, nullptr).has_value(); }

bool looks_like_refdes(std::string_view in) {
    std::string s = trim(in);
    if (s.size() < 2 || s.size() > 16) return false;
    size_t i = 0;
    while (i < s.size() && (std::isalpha((unsigned char)s[i]) || s[i] == '_')) ++i;
    if (i == 0 || i > 6) return false;
    size_t j = i;
    while (j < s.size() && std::isdigit((unsigned char)s[j])) ++j;
    if (j == i) return false;
    for (size_t k = j; k < s.size(); ++k)
        if (!std::isalnum((unsigned char)s[k]) && s[k] != '_') return false;
    return true;
}

std::vector<std::string> split_ws_quoted(std::string_view line) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && is_sp(line[i])) ++i;
        if (i >= line.size()) break;
        std::string tok;
        if (line[i] == '"') {
            ++i;
            while (i < line.size() && line[i] != '"') tok += line[i++];
            if (i < line.size()) ++i;
        } else {
            while (i < line.size() && !is_sp(line[i])) tok += line[i++];
        }
        out.push_back(tok);
    }
    return out;
}

std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> out;
    size_t i = 0;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) i = 3;
    std::string cur;
    for (; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\r') {
            out.push_back(cur); cur.clear();
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else if (c == '\n') {
            out.push_back(cur); cur.clear();
        } else if (c == '\x1a') {
            break;  // DOS EOF marker
        } else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool read_file(const std::string& path, std::string& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open file: " + path; return false; }
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

std::string fmt_fixed(double v, int decimals) {
    if (std::fabs(v) < 0.5 * std::pow(10.0, -decimals)) v = 0;   // avoid "-0.0000"
    // Manual formatting so the decimal mark is always '.'
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    for (char* p = buf; *p; ++p) if (*p == ',') *p = '.';
    return buf;
}

} // namespace pcbp::io
