// ============================================================================
// sheet_format.cpp — number format codes (ECMA-376 Part 1 §18.8.31 numFmt,
// format-code grammar as documented for Excel "Number format codes"), serial
// dates (1900 + 1904 systems, incl. the documented 1900 leap-year quirk),
// typed input recognition.
// ============================================================================
#include "formula_internal.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace pcbp::sheet {

// ------------------------------------------------------------------- dates --
static long days_from_civil(long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}
static void civil_from_days(long z, int& y, int& m, int& d) {
    z += 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = (long)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y = (int)(yy + (m <= 2));
}
static const long k_epoch1900 = days_from_civil(1899, 12, 31);
static const long k_epoch1904 = days_from_civil(1904, 1, 1);

double date_serial(int y, int m, int d, bool d1904) {
    // normalise month overflow like DATE()
    long mm = m - 1;
    long yy = y + (mm >= 0 ? mm / 12 : (mm - 11) / 12);
    mm = ((mm % 12) + 12) % 12;
    long days = days_from_civil(yy, (unsigned)(mm + 1), 1) + (d - 1);
    if (d1904) return (double)(days - k_epoch1904);
    long s = days - k_epoch1900;
    if (s >= 60) ++s;   // phantom 1900-02-29
    return (double)s;
}
void serial_to_ymd(double serial, int& y, int& m, int& d, bool d1904) {
    long s = (long)std::floor(serial);
    if (d1904) { civil_from_days(s + k_epoch1904, y, m, d); return; }
    if (s == 60) { y = 1900; m = 2; d = 29; return; }
    if (s == 0) { y = 1900; m = 1; d = 0; return; }
    if (s > 60) --s;
    civil_from_days(s + k_epoch1900, y, m, d);
}
double time_serial(int h, int mi, double s) { return (h * 3600.0 + mi * 60.0 + s) / 86400.0; }

// --------------------------------------------------------- builtin formats --
std::string builtin_numfmt(int id) {
    switch (id) {
    case 0: return "General"; case 1: return "0"; case 2: return "0.00"; case 3: return "#,##0"; case 4: return "#,##0.00";
    case 5: return "\"$\"#,##0_);(\"$\"#,##0)"; case 6: return "\"$\"#,##0_);[Red](\"$\"#,##0)";
    case 7: return "\"$\"#,##0.00_);(\"$\"#,##0.00)"; case 8: return "\"$\"#,##0.00_);[Red](\"$\"#,##0.00)";
    case 9: return "0%"; case 10: return "0.00%"; case 11: return "0.00E+00"; case 12: return "# ?/?"; case 13: return "# ?\?/?\?";
    case 14: return "m/d/yyyy"; case 15: return "d-mmm-yy"; case 16: return "d-mmm"; case 17: return "mmm-yy";
    case 18: return "h:mm AM/PM"; case 19: return "h:mm:ss AM/PM"; case 20: return "h:mm"; case 21: return "h:mm:ss";
    case 22: return "m/d/yyyy h:mm";
    case 37: return "#,##0_);(#,##0)"; case 38: return "#,##0_);[Red](#,##0)"; case 39: return "#,##0.00_);(#,##0.00)";
    case 40: return "#,##0.00_);[Red](#,##0.00)";
    case 41: return "_(* #,##0_);_(* \\(#,##0\\);_(* \"-\"_);_(@_)";
    case 42: return "_(\"$\"* #,##0_);_(\"$\"* \\(#,##0\\);_(\"$\"* \"-\"_);_(@_)";
    case 43: return "_(* #,##0.00_);_(* \\(#,##0.00\\);_(* \"-\"??_);_(@_)";
    case 44: return "_(\"$\"* #,##0.00_);_(\"$\"* \\(#,##0.00\\);_(\"$\"* \"-\"??_);_(@_)";
    case 45: return "mm:ss"; case 46: return "[h]:mm:ss"; case 47: return "mm:ss.0"; case 48: return "##0.0E+0"; case 49: return "@";
    default: return {};
    }
}
int builtin_numfmt_id(const std::string& code) {
    if (code.empty() || code == "General" || code == "general") return 0;
    for (int i = 1; i <= 49; ++i) { std::string b = builtin_numfmt(i); if (!b.empty() && b == code) return i; }
    return -1;
}

// ---------------------------------------------------------- format parsing --
namespace {

struct Section {
    std::string body;          // without [color]/[cond]/[$-...] tags
    Argb color = 0;
    bool has_cond = false;
    int cond_op = 0;           // 1 <, 2 <=, 3 >, 4 >=, 5 =, 6 <>
    double cond_val = 0;
};

Argb named_color(const std::string& n) {
    std::string u = upper_ascii(n);
    static const struct { const char* n; Argb c; } named[] = {{"BLACK", 0xFF000000}, {"WHITE", 0xFFFFFFFF}, {"RED", 0xFFFF0000},
        {"GREEN", 0xFF00FF00}, {"BLUE", 0xFF0000FF}, {"YELLOW", 0xFFFFFF00}, {"MAGENTA", 0xFFFF00FF}, {"CYAN", 0xFF00FFFF}};
    for (auto& x : named) if (u == x.n) return x.c;
    if (u.rfind("COLOR", 0) == 0) {
        static const Argb pal[56] = {0xFF000000, 0xFFFFFFFF, 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00, 0xFFFF00FF, 0xFF00FFFF,
            0xFF800000, 0xFF008000, 0xFF000080, 0xFF808000, 0xFF800080, 0xFF008080, 0xFFC0C0C0, 0xFF808080, 0xFF9999FF, 0xFF993366,
            0xFFFFFFCC, 0xFFCCFFFF, 0xFF660066, 0xFFFF8080, 0xFF0066CC, 0xFFCCCCFF, 0xFF000080, 0xFFFF00FF, 0xFFFFFF00, 0xFF00FFFF,
            0xFF800080, 0xFF800000, 0xFF008080, 0xFF0000FF, 0xFF00CCFF, 0xFFCCFFFF, 0xFFCCFFCC, 0xFFFFFF99, 0xFF99CCFF, 0xFFFF99CC,
            0xFFCC99FF, 0xFFFFCC99, 0xFF3366FF, 0xFF33CCCC, 0xFF99CC00, 0xFFFFCC00, 0xFFFF9900, 0xFFFF6600, 0xFF666699, 0xFF969696,
            0xFF003366, 0xFF339966, 0xFF003300, 0xFF333300, 0xFF993300, 0xFF993366, 0xFF333399, 0xFF333333};
        int k = atoi(u.c_str() + 5);
        if (k >= 1 && k <= 56) return pal[k - 1];
    }
    return 0;
}

std::vector<Section> split_sections(const std::string& code) {
    std::vector<Section> out(1);
    size_t i = 0;
    while (i < code.size()) {
        char ch = code[i];
        Section& s = out.back();
        if (ch == '"') {
            size_t j = code.find('"', i + 1);
            if (j == std::string::npos) j = code.size() - 1;
            s.body.append(code, i, j - i + 1); i = j + 1; continue;
        }
        if (ch == '\\' && i + 1 < code.size()) {
            // keep escape with its (possibly multibyte) char
            size_t j = i + 2;
            while (j < code.size() && ((unsigned char)code[j] & 0xC0) == 0x80) ++j;
            s.body.append(code, i, j - i); i = j; continue;
        }
        if ((ch == '_' || ch == '*') && i + 1 < code.size()) {
            size_t j = i + 2;
            while (j < code.size() && ((unsigned char)code[j] & 0xC0) == 0x80) ++j;
            s.body.append(code, i, j - i); i = j; continue;
        }
        if (ch == '[') {
            size_t j = code.find(']', i);
            if (j == std::string::npos) { s.body += ch; ++i; continue; }
            std::string tag = code.substr(i + 1, j - i - 1);
            std::string ut = upper_ascii(tag);
            if (!tag.empty() && (tag[0] == '<' || tag[0] == '>' || tag[0] == '=')) {
                size_t k = 0;
                int op = 0;
                if (tag.compare(0, 2, "<=") == 0) { op = 2; k = 2; } else if (tag.compare(0, 2, ">=") == 0) { op = 4; k = 2; }
                else if (tag.compare(0, 2, "<>") == 0) { op = 6; k = 2; } else if (tag[0] == '<') { op = 1; k = 1; }
                else if (tag[0] == '>') { op = 3; k = 1; } else { op = 5; k = 1; }
                s.has_cond = true; s.cond_op = op; s.cond_val = atof(tag.c_str() + k);
            } else if (!tag.empty() && tag[0] == '$') {
                // [$€-407] currency+locale: keep the symbol part as literal
                size_t dash = tag.find('-');
                std::string sym = tag.substr(1, dash == std::string::npos ? std::string::npos : dash - 1);
                if (!sym.empty()) { s.body += '"'; s.body += sym; s.body += '"'; }
            } else if (ut == "H" || ut == "HH" || ut == "M" || ut == "MM" || ut == "S" || ut == "SS") {
                s.body.append(code, i, j - i + 1);   // elapsed time token, keep
            } else if (Argb c = named_color(tag)) {
                s.color = c;
            }
            i = j + 1; continue;
        }
        if (ch == ';') { out.emplace_back(); ++i; continue; }
        s.body += ch; ++i;
    }
    return out;
}

bool section_is_date(const std::string& b) {
    for (size_t i = 0; i < b.size(); ++i) {
        char ch = b[i];
        if (ch == '"') { size_t j = b.find('"', i + 1); if (j == std::string::npos) break; i = j; continue; }
        if (ch == '\\' || ch == '_' || ch == '*') { ++i; continue; }
        if (ch == '[') { size_t j = b.find(']', i); if (j == std::string::npos) break; std::string t = upper_ascii(b.substr(i + 1, j - i - 1)); if (t == "H" || t == "HH" || t == "M" || t == "MM" || t == "S" || t == "SS") return true; i = j; continue; }
        char u = (char)toupper((unsigned char)ch);
        if (u == 'Y' || u == 'D' || u == 'H' || u == 'S' || u == 'M') return true;
        if (u == 'E' && (i + 1 >= b.size() || (b[i + 1] != '+' && b[i + 1] != '-'))) return true;   // year 'e'
        if (u == 'A' && (upper_ascii(b.substr(i, 5)) == "AM/PM" || upper_ascii(b.substr(i, 3)) == "A/P")) return true;
    }
    return false;
}

const char* k_mon[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
const char* k_day[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

std::string render_date(const std::string& b, double v, bool d1904, bool& bad) {
    bad = false;
    if (v < 0 || v > 2958465.99999) { bad = true; return {}; }
    // fractional seconds digits requested?
    int fs_digits = 0;
    for (size_t i = 0; i + 1 < b.size(); ++i)
        if ((b[i] == 's' || b[i] == 'S') && b[i + 1] == '.') { size_t k = i + 2; while (k < b.size() && b[k] == '0') { ++fs_digits; ++k; } }
    double scale = 86400.0 * std::pow(10.0, fs_digits);
    double tot = std::floor(v * scale + 0.5);      // in units
    double day = std::floor(tot / scale);
    double rem = tot - day * scale;                // units within day
    double unit = std::pow(10.0, fs_digits);
    long secs = (long)std::floor(rem / unit);
    long frac = (long)(rem - secs * unit);
    int y, mo, d;
    serial_to_ymd(day, y, mo, d, d1904);
    int hh = (int)(secs / 3600), mi = (int)(secs / 60 % 60), ss = (int)(secs % 60);
    long dow = ((long)day + (d1904 ? 5 : 6)) % 7;   // 1900: serial 1 = Sunday (Excel's WEEKDAY(1)=1)
    if (!d1904 && day < 60) dow = ((long)day + 6) % 7;
    else if (!d1904) dow = ((long)day + 6) % 7;
    double total_h = std::floor(tot / unit / 3600), total_m = std::floor(tot / unit / 60), total_s = std::floor(tot / unit);
    bool ampm = false;
    {
        std::string u = upper_ascii(b);
        size_t q = 0; bool inq = false;
        for (q = 0; q < u.size(); ++q) { if (u[q] == '"') inq = !inq; if (!inq && (u.compare(q, 5, "AM/PM") == 0 || u.compare(q, 3, "A/P") == 0)) { ampm = true; break; } }
    }
    std::string o;
    // tokenise; decide m = month or minute: minute if preceded by h or followed by s
    struct T { char k; int n; std::string lit; };
    std::vector<T> ts;
    for (size_t i = 0; i < b.size();) {
        char ch = b[i];
        char u = (char)tolower((unsigned char)ch);
        if (ch == '"') { size_t j = b.find('"', i + 1); if (j == std::string::npos) j = b.size(); ts.push_back({'L', 0, b.substr(i + 1, j - i - 1)}); i = j + 1; continue; }
        if (ch == '\\') { size_t j = i + 2; while (j < b.size() && ((unsigned char)b[j] & 0xC0) == 0x80) ++j; ts.push_back({'L', 0, b.substr(i + 1, j - i - 1)}); i = j; continue; }
        if (ch == '_') { ts.push_back({'L', 0, " "}); i += 2; while (i < b.size() && ((unsigned char)b[i] & 0xC0) == 0x80) ++i; continue; }
        if (ch == '*') { i += 2; while (i < b.size() && ((unsigned char)b[i] & 0xC0) == 0x80) ++i; continue; }
        if (ch == '[') { size_t j = b.find(']', i); std::string t = b.substr(i + 1, j - i - 1); ts.push_back({'[', (int)t.size(), std::string(1, (char)tolower((unsigned char)t[0]))}); i = j + 1; continue; }
        if (u == 'y' || u == 'm' || u == 'd' || u == 'h' || u == 's' || u == 'e') {
            size_t j = i; while (j < b.size() && tolower((unsigned char)b[j]) == u) ++j;
            ts.push_back({u, (int)(j - i), ""}); i = j; continue;
        }
        if (u == 'a') {
            std::string up = upper_ascii(b.substr(i, 5));
            if (up == "AM/PM") { ts.push_back({'A', 5, b.substr(i, 5)}); i += 5; continue; }
            if (upper_ascii(b.substr(i, 3)) == "A/P") { ts.push_back({'A', 3, b.substr(i, 3)}); i += 3; continue; }
        }
        if (ch == '.' && i + 1 < b.size() && b[i + 1] == '0' && !ts.empty() && (ts.back().k == 's' || (ts.back().k == '[' && ts.back().lit == "s"))) {
            size_t j = i + 1; while (j < b.size() && b[j] == '0') ++j;
            ts.push_back({'f', (int)(j - i - 1), ""}); i = j; continue;
        }
        if (ch == '0') { ++i; continue; }
        size_t j = i + 1; while (j < b.size() && ((unsigned char)b[j] & 0xC0) == 0x80) ++j;
        ts.push_back({'L', 0, b.substr(i, j - i)}); i = j;
    }
    for (size_t i = 0; i < ts.size(); ++i) {
        if (ts[i].k != 'm' || ts[i].n > 2) continue;
        bool minute = false;
        for (int k = (int)i - 1; k >= 0; --k) { if (ts[k].k == 'L') continue; if (ts[k].k == 'h' || (ts[k].k == '[' && ts[k].lit == "h")) minute = true; break; }
        for (size_t k = i + 1; k < ts.size() && !minute; ++k) { if (ts[k].k == 'L') continue; if (ts[k].k == 's' || (ts[k].k == '[' && ts[k].lit == "s")) minute = true; break; }
        if (minute) ts[i].k = 'M';
    }
    char buf[32];
    for (auto& t : ts) {
        switch (t.k) {
        case 'L': o += t.lit; break;
        case 'y': case 'e':
            if (t.n <= 2 && t.k == 'y') { snprintf(buf, sizeof buf, "%02d", y % 100); o += buf; }
            else { snprintf(buf, sizeof buf, "%d", y); o += buf; }
            break;
        case 'm':
            if (t.n == 1) o += std::to_string(mo);
            else if (t.n == 2) { snprintf(buf, sizeof buf, "%02d", mo); o += buf; }
            else if (t.n == 3) o += std::string(k_mon[mo - 1]).substr(0, 3);
            else if (t.n == 5) o += k_mon[mo - 1][0];
            else o += k_mon[mo - 1];
            break;
        case 'd':
            if (t.n == 1) o += std::to_string(d);
            else if (t.n == 2) { snprintf(buf, sizeof buf, "%02d", d); o += buf; }
            else if (t.n == 3) o += std::string(k_day[dow]).substr(0, 3);
            else o += k_day[dow];
            break;
        case 'h': {
            int h = hh;
            if (ampm) { h = hh % 12; if (h == 0) h = 12; }
            if (t.n >= 2) { snprintf(buf, sizeof buf, "%02d", h); o += buf; } else o += std::to_string(h);
            break;
        }
        case 'M': if (t.n >= 2) { snprintf(buf, sizeof buf, "%02d", mi); o += buf; } else o += std::to_string(mi); break;
        case 's': if (t.n >= 2) { snprintf(buf, sizeof buf, "%02d", ss); o += buf; } else o += std::to_string(ss); break;
        case 'f': { snprintf(buf, sizeof buf, ".%0*ld", t.n, frac / (long)std::pow(10.0, fs_digits - t.n)); o += buf; break; }
        case '[': {
            double val = t.lit == "h" ? total_h : t.lit == "m" ? total_m : total_s;
            snprintf(buf, sizeof buf, "%0*.0f", t.n, val); o += buf;
            break;
        }
        case 'A':
            if (t.n == 5) o += hh < 12 ? (t.lit[0] == 'a' ? "am" : "AM") : (t.lit[0] == 'a' ? "pm" : "PM");
            else o += hh < 12 ? (t.lit[0] == 'a' ? "a" : "A") : (t.lit[0] == 'a' ? "p" : "P");
            break;
        }
    }
    return o;
}

std::string general_number(double v) {
    if (v == 0) return "0";
    std::string s = fmt_general(v, 15);
    if (s.size() <= 11) return s;
    double a = std::fabs(v);
    int e10 = (int)std::floor(std::log10(a));
    int avail = 11 - (v < 0 ? 1 : 0);
    if (e10 >= -5 && e10 < 10) {
        int intd = e10 >= 0 ? e10 + 1 : 1;
        int dec = avail - intd - 1;
        if (dec >= 0) {
            char buf[64]; snprintf(buf, sizeof buf, "%.*f", dec, v);
            std::string r = buf;
            if (r.find('.') != std::string::npos) { while (r.back() == '0') r.pop_back(); if (r.back() == '.') r.pop_back(); }
            if (r.size() <= 11 && r != "0" && r != "-0") return r;
        }
    }
    // scientific with as many mantissa digits as fit
    for (int md = 5; md >= 0; --md) {
        char buf[64]; snprintf(buf, sizeof buf, "%.*E", md, v);
        std::string r = buf;
        size_t ep = r.find('E');
        std::string m = r.substr(0, ep);
        int ev = atoi(r.c_str() + ep + 1);
        if (m.find('.') != std::string::npos) { while (m.back() == '0') m.pop_back(); if (m.back() == '.') m.pop_back(); }
        char eb[16]; snprintf(eb, sizeof eb, "E%c%02d", ev < 0 ? '-' : '+', std::abs(ev));
        std::string out = m + eb;
        if ((int)out.size() <= 11 || md == 0) return out;
    }
    return s;
}

// numeric (non-date) section renderer
std::string render_number(const std::string& b, double v, bool& bad) {
    bad = false;
    // tokens
    struct T { char k; std::string lit; };   // k: '0','#','?','.',',','%','E','/','L','@','G'
    std::vector<T> ts;
    bool in_exp = false;
    for (size_t i = 0; i < b.size();) {
        char ch = b[i];
        if (ch == '"') { size_t j = b.find('"', i + 1); if (j == std::string::npos) j = b.size(); ts.push_back({'L', b.substr(i + 1, j - i - 1)}); i = j + 1; continue; }
        if (ch == '\\') { size_t j = i + 2; while (j < b.size() && ((unsigned char)b[j] & 0xC0) == 0x80) ++j; ts.push_back({'L', b.substr(i + 1, j - i - 1)}); i = j; continue; }
        if (ch == '_') { ts.push_back({'L', " "}); i += 2; while (i < b.size() && ((unsigned char)b[i] & 0xC0) == 0x80) ++i; continue; }
        if (ch == '*') { i += 2; while (i < b.size() && ((unsigned char)b[i] & 0xC0) == 0x80) ++i; continue; }
        if (ch == '0' || ch == '#' || ch == '?' || ch == '.' || ch == ',' || ch == '%' || ch == '/' || ch == '@') { ts.push_back({ch, ""}); ++i; continue; }
        if ((ch == 'E' || ch == 'e') && i + 1 < b.size() && (b[i + 1] == '+' || b[i + 1] == '-')) { ts.push_back({'E', std::string(1, b[i + 1])}); i += 2; in_exp = true; continue; }
        if (upper_ascii(b.substr(i, 7)) == "GENERAL") { ts.push_back({'G', ""}); i += 7; continue; }
        if (isdigit((unsigned char)ch) && !ts.empty() && ts.back().k == '/') {   // fixed denominator
            size_t j = i; while (j < b.size() && isdigit((unsigned char)b[j])) ++j;
            ts.push_back({'D', b.substr(i, j - i)}); i = j; continue;
        }
        size_t j = i + 1; while (j < b.size() && ((unsigned char)b[j] & 0xC0) == 0x80) ++j;
        ts.push_back({'L', b.substr(i, j - i)}); i = j;
    }
    (void)in_exp;
    for (auto& t : ts) if (t.k == 'G') {
        std::string o;
        for (auto& x : ts) o += x.k == 'G' ? general_number(v) : x.k == 'L' ? x.lit : std::string(1, x.k);
        return o;
    }
    // percent scaling
    for (auto& t : ts) if (t.k == '%') v *= 100;
    // locate structure
    int ndot = -1, nexp = -1, nslash = -1;
    for (int i = 0; i < (int)ts.size(); ++i) {
        if (ts[i].k == '.' && ndot < 0 && nexp < 0) ndot = i;
        if (ts[i].k == 'E' && nexp < 0) nexp = i;
        if (ts[i].k == '/' && nslash < 0) nslash = i;
    }
    auto is_ph = [](char k) { return k == '0' || k == '#' || k == '?'; };
    // ---- fraction ----
    if (nslash >= 0) {
        // numerator placeholders: contiguous run before '/', whole-part placeholders before that run (separated by literal)
        int ne = nslash - 1;
        while (ne >= 0 && !is_ph(ts[ne].k)) --ne;
        int ns = ne;
        while (ns > 0 && is_ph(ts[ns - 1].k)) --ns;
        bool has_whole = false;
        for (int i = 0; i < ns; ++i) if (is_ph(ts[i].k)) has_whole = true;
        int dd = 0; long fixed_den = 0;
        int de = nslash + 1;
        for (int i = nslash + 1; i < (int)ts.size(); ++i) { if (is_ph(ts[i].k)) { ++dd; de = i; } else if (ts[i].k == 'D') { fixed_den = atol(ts[i].lit.c_str()); de = i; } else if (dd || fixed_den) break; }
        bool neg = v < 0;
        double a = std::fabs(v);
        double whole = has_whole ? std::floor(a) : 0;
        double fr = a - whole;
        long num = 0, den = 1;
        if (fixed_den > 0) { den = fixed_den; num = (long)std::floor(fr * den + 0.5); }
        else {
            long maxd = dd <= 0 ? 9 : (long)std::pow(10.0, std::min(dd, 4)) - 1;
            double best = 2;
            for (long d = 1; d <= maxd; ++d) { long n = (long)std::floor(fr * d + 0.5); double e = std::fabs(fr - (double)n / d); if (e < best - 1e-12) { best = e; num = n; den = d; } }
        }
        if (has_whole && num == den) { whole += 1; num = 0; }
        std::string o;
        if (neg) o += '-';
        // whole part
        std::string ws = whole > 0 || !has_whole ? fmt_general(whole, 15) : "";
        bool zero_frac = num == 0;
        for (int i = 0; i < (int)ts.size(); ++i) {
            const T& t = ts[i];
            if (i < ns) {
                if (is_ph(t.k)) {
                    // emit whole digits at the last whole placeholder
                    bool last = true;
                    for (int k = i + 1; k < ns; ++k) if (is_ph(ts[k].k)) last = false;
                    if (last) { if (!ws.empty() || zero_frac) o += ws.empty() ? "0" : ws; }
                } else if (t.k == 'L') o += t.lit;
                continue;
            }
            if (i >= ns && i <= de) {
                if (zero_frac && has_whole) { if (i == ns) { for (int k = ns; k <= de; ++k) o += ' '; } continue; }
                if (i == ns) { std::string s = std::to_string(has_whole ? num : num + (long)whole * den); int w = ne - ns + 1; while ((int)s.size() < w && ts[ns].k == '?') s = " " + s; o += s; }
                else if (i == nslash) o += '/';
                else if (i == nslash + 1) { std::string s = std::to_string(den); o += s; int w = dd - (int)s.size(); while (w-- > 0) o += ' '; }
                continue;
            }
            if (t.k == 'L') o += t.lit;
            else if (t.k == '%') o += '%';
        }
        return o;
    }
    // ---- plain / scientific ----
    int int_end = ndot >= 0 ? ndot : (nexp >= 0 ? nexp : (int)ts.size());
    // integer placeholders & thousands commas
    std::vector<int> iph;
    bool thousands = false;
    int last_ph = -1;
    for (int i = 0; i < int_end; ++i) if (is_ph(ts[i].k)) last_ph = i;
    int scale_commas = 0;
    for (int i = 0; i < int_end; ++i) {
        if (is_ph(ts[i].k)) iph.push_back(i);
        else if (ts[i].k == ',') {
            bool before_ph = false, after_ph = false;
            for (int k = 0; k < i; ++k) if (is_ph(ts[k].k)) before_ph = true;
            for (int k = i + 1; k < int_end; ++k) if (is_ph(ts[k].k)) after_ph = true;
            if (before_ph && after_ph) thousands = true;
            else if (before_ph && i > last_ph) ++scale_commas;
        }
    }
    // commas right after the decimals (e.g. "0.0,,") also scale
    if (ndot >= 0) {
        int k = (int)ts.size() - 1;
        int lastdp = -1;
        for (int i = ndot + 1; i < (nexp >= 0 ? nexp : (int)ts.size()); ++i) if (is_ph(ts[i].k)) lastdp = i;
        for (int i = lastdp + 1; lastdp >= 0 && i <= k && ts[i].k == ','; ++i) ++scale_commas;
    }
    for (int i = 0; i < scale_commas; ++i) v /= 1000;
    std::vector<int> dph;
    int dec_end = nexp >= 0 ? nexp : (int)ts.size();
    if (ndot >= 0) for (int i = ndot + 1; i < dec_end; ++i) if (is_ph(ts[i].k)) dph.push_back(i);
    std::vector<int> eph;
    if (nexp >= 0) for (int i = nexp + 1; i < (int)ts.size(); ++i) if (is_ph(ts[i].k)) eph.push_back(i);
    bool neg = v < 0;
    double a = std::fabs(v);
    int expo = 0;
    if (nexp >= 0 && a != 0) {
        int ni = std::max<int>(1, (int)iph.size());
        bool eng = iph.size() > 1 && ts[iph[0]].k == '#';
        int e10 = (int)std::floor(std::log10(a));
        if (eng) { int step = ni; expo = (int)std::floor((double)e10 / step) * step; }
        else expo = e10 - (int)(std::max<size_t>(1, std::count_if(iph.begin(), iph.end(), [&](int i) { return ts[i].k == '0'; })) - 1);
        a = a / std::pow(10.0, expo);
        // rounding may push mantissa up
        double rr = std::floor(a * std::pow(10.0, (double)dph.size()) + 0.5) / std::pow(10.0, (double)dph.size());
        if (!eng && rr >= std::pow(10.0, std::max<int>(1, (int)std::count_if(iph.begin(), iph.end(), [&](int i) { return ts[i].k == '0'; })))) { expo += 1; a /= 10; }
    }
    // digits
    char buf[400];
    int nd = (int)dph.size();
    if (a >= 1e300) { bad = true; return {}; }
    snprintf(buf, sizeof buf, "%.*f", nd, a);
    std::string full = buf;
    std::string ip = full, fp;
    size_t dp = full.find('.');
    if (dp != std::string::npos) { ip = full.substr(0, dp); fp = full.substr(dp + 1); }
    if (ip == "0") ip = "";
    // decide sign: if everything rounds to zero, no minus
    bool allzero = ip.empty() && fp.find_first_not_of('0') == std::string::npos;
    if (allzero) neg = false;
    // integer emission plan: assign digits to placeholders right-to-left
    std::vector<std::string> assign(ts.size());
    {
        int di = (int)ip.size() - 1;
        for (int k = (int)iph.size() - 1; k >= 0; --k) {
            int ti = iph[k];
            if (di >= 0) {
                if (k == 0) { assign[ti] = ip.substr(0, di + 1); di = -1; }
                else assign[ti] = std::string(1, ip[di--]);
            } else {
                char pk = ts[ti].k;
                assign[ti] = pk == '0' ? "0" : pk == '?' ? " " : "";
            }
        }
        if (iph.empty() && !ip.empty()) { /* no integer placeholders: digits dropped (Excel shows .5 for 0.5 with ".00") */ }
    }
    // thousands grouping: count digits from right among integer output
    std::string o;
    if (neg) o += '-';
    // count total integer digits emitted (only digit chars)
    int total_int_digits = 0;
    for (int ti : iph) for (char ch : assign[ti]) if (isdigit((unsigned char)ch)) ++total_int_digits;
    int emitted = 0;
    for (int i = 0; i < (int)ts.size(); ++i) {
        const T& t = ts[i];
        if (i < int_end) {
            if (is_ph(t.k)) {
                for (char ch : assign[i]) {
                    o += ch;
                    if (isdigit((unsigned char)ch)) {
                        ++emitted;
                        int right = total_int_digits - emitted;
                        if (thousands && right > 0 && right % 3 == 0) o += ',';
                    }
                }
            } else if (t.k == ',') {
            } else if (t.k == 'L') o += t.lit;
            else if (t.k == '%') o += '%';
            else if (t.k == '@') {}
            continue;
        }
        if (i == ndot) {
            o += '.';
            continue;
        }
        if (ndot >= 0 && i > ndot && i < dec_end) {
            if (is_ph(t.k)) {
                int k = (int)(std::find(dph.begin(), dph.end(), i) - dph.begin());
                char dch = k < (int)fp.size() ? fp[k] : '0';
                // trailing zeros for # / ?
                bool rest_zero = true;
                for (int q = k; q < (int)fp.size(); ++q) if (fp[q] != '0') rest_zero = false;
                if (dch == '0' && rest_zero && t.k != '0') { if (t.k == '?') o += ' '; }
                else o += dch;
            } else if (t.k == 'L') o += t.lit;
            else if (t.k == '%') o += '%';
            continue;
        }
        if (i == nexp) {
            o += 'E';
            if (expo < 0) o += '-'; else if (t.lit == "+") o += '+';
            std::string es = std::to_string(std::abs(expo));
            int need = (int)eph.size();
            while ((int)es.size() < need) es = "0" + es;
            o += es;
            continue;
        }
        if (nexp >= 0 && i > nexp) { if (is_ph(t.k)) continue; if (t.k == 'L') o += t.lit; else if (t.k == '%') o += '%'; continue; }
        if (t.k == 'L') o += t.lit; else if (t.k == '%') o += '%';
    }
    // drop the trailing '.' when there are no decimal placeholders and nothing follows ("0." keeps)
    return o;
}

std::string render_text_section(const std::string& b, const std::string& text) {
    std::string o;
    for (size_t i = 0; i < b.size();) {
        char ch = b[i];
        if (ch == '"') { size_t j = b.find('"', i + 1); if (j == std::string::npos) j = b.size(); o += b.substr(i + 1, j - i - 1); i = j + 1; continue; }
        if (ch == '\\') { size_t j = i + 2; while (j < b.size() && ((unsigned char)b[j] & 0xC0) == 0x80) ++j; o += b.substr(i + 1, j - i - 1); i = j; continue; }
        if (ch == '_') { o += ' '; i += 2; while (i < b.size() && ((unsigned char)b[i] & 0xC0) == 0x80) ++i; continue; }
        if (ch == '*') { i += 2; while (i < b.size() && ((unsigned char)b[i] & 0xC0) == 0x80) ++i; continue; }
        if (ch == '@') { o += text; ++i; continue; }
        o += ch; ++i;
    }
    return o;
}

bool cond_ok(const Section& s, double v) {
    switch (s.cond_op) {
    case 1: return v < s.cond_val; case 2: return v <= s.cond_val; case 3: return v > s.cond_val;
    case 4: return v >= s.cond_val; case 5: return v == s.cond_val; case 6: return v != s.cond_val;
    }
    return true;
}

struct FmtCache {
    std::unordered_map<std::string, std::vector<Section>> m;
    const std::vector<Section>& get(const std::string& code) {
        auto it = m.find(code);
        if (it != m.end()) return it->second;
        if (m.size() > 4096) m.clear();
        return m.emplace(code, split_sections(code)).first->second;
    }
};

} // namespace

bool is_date_format(const std::string& code) {
    if (code.empty() || code == "General") return false;
    auto secs = split_sections(code);
    return section_is_date(secs[0].body);
}

Formatted format_value(const Value& v, const std::string& code, bool d1904) {
    Formatted f;
    thread_local FmtCache cache;
    if (v.type == Value::Empty) return f;
    if (v.type == Value::Error) { f.text = err_text(v.err); return f; }
    if (v.type == Value::Bool) { f.text = v.num != 0 ? "TRUE" : "FALSE"; return f; }
    const bool general = code.empty() || code == "General" || code == "general" || code == "GENERAL";
    if (v.type == Value::String) {
        if (general) { f.text = v.str; return f; }
        const auto& secs = cache.get(code);
        const Section* s = nullptr;
        if (secs.size() >= 4) s = &secs[3];
        else if (secs.size() == 1 && secs[0].body.find('@') != std::string::npos) s = &secs[0];
        if (!s) { f.text = v.str; return f; }
        f.text = render_text_section(s->body, v.str);
        f.color = s->color;
        return f;
    }
    // number
    f.is_number = true;
    double x = v.num;
    if (general) { f.text = general_number(x); return f; }
    const auto& secs = cache.get(code);
    const Section* s = &secs[0];
    bool absval = false;
    bool any_cond = false;
    for (auto& q : secs) if (q.has_cond) any_cond = true;
    if (any_cond) {
        if (secs[0].has_cond && cond_ok(secs[0], x)) s = &secs[0];
        else if (secs.size() > 1 && secs[1].has_cond && cond_ok(secs[1], x)) s = &secs[1];
        else if (secs.size() > 1 && !secs[1].has_cond && !secs[0].has_cond) s = &secs[0];
        else if (secs.size() > 2) s = &secs[2];
        else if (secs.size() > 1 && !secs[1].has_cond) s = &secs[1];
        else { s = &secs[0]; }
        // Excel: negative numbers in a conditioned non-first section lose sign only when cond is "<0"-like
        if (s != &secs[0] && x < 0 && s->cond_op != 0 && (s->cond_op == 1 || s->cond_op == 2) && s->cond_val <= 0) absval = true;
        if (s == &secs[1] && !s->has_cond && x < 0) absval = true;
    } else {
        if (secs.size() >= 3 && x == 0) s = &secs[2];
        else if (secs.size() >= 2 && x < 0) { s = &secs[1]; absval = true; }
    }
    if (absval) x = std::fabs(x);
    f.color = s->color;
    bool bad = false;
    std::string body = s->body;
    if (body.find_first_not_of(' ') == std::string::npos && !body.empty()) { f.text = body; return f; }
    if (body.empty()) { f.text = ""; return f; }
    if (section_is_date(body)) f.text = render_date(body, x, d1904, bad);
    else if (body.find_first_of("0#?") == std::string::npos && upper_ascii(body).find("GENERAL") == std::string::npos) {
        // literal-only section (e.g. "-" or "\"zero\""): no number shown
        f.text = render_text_section(body, "");
        if (!secs.empty() && s == &secs[0] && secs.size() == 1 && x < 0) f.text = "-" + f.text;
    } else {
        f.text = render_number(body, x, bad);
        if (absval == false && x < 0 && s != &secs[0]) {}   // section handles sign
    }
    if (bad) { f.text = std::string(11, '#'); }
    return f;
}

// ------------------------------------------------------------- input parse --
static bool month_from_name(const std::string& s, int& m) {
    std::string l = lower_utf8(s);
    for (int i = 0; i < 12; ++i) {
        std::string n = lower_utf8(k_mon[i]);
        if (l == n || (l.size() >= 3 && n.compare(0, l.size(), l) == 0)) { m = i + 1; return true; }
    }
    return false;
}

static bool parse_time_part(const std::string& s, double& out, bool& with_sec) {
    // h:mm[:ss[.fff]] [AM|PM]
    int h = 0, mi = 0; double sec = 0;
    size_t i = 0;
    auto digits = [&](int& v) { size_t k = i; while (i < s.size() && isdigit((unsigned char)s[i])) ++i; if (i == k || i - k > 4) return false; v = atoi(s.substr(k, i - k).c_str()); return true; };
    if (!digits(h)) return false;
    if (i >= s.size() || s[i] != ':') return false;
    ++i;
    if (!digits(mi)) return false;
    with_sec = false;
    if (i < s.size() && s[i] == ':') {
        ++i;
        size_t k = i;
        while (i < s.size() && (isdigit((unsigned char)s[i]) || s[i] == '.')) ++i;
        if (i == k) return false;
        sec = atof(s.substr(k, i - k).c_str());
        with_sec = true;
    }
    while (i < s.size() && s[i] == ' ') ++i;
    if (i < s.size()) {
        std::string ap = upper_ascii(s.substr(i));
        if (ap == "AM" || ap == "A") { if (h > 12) return false; if (h == 12) h = 0; }
        else if (ap == "PM" || ap == "P") { if (h > 12) return false; if (h < 12) h += 12; }
        else return false;
    }
    if (mi >= 60 || sec >= 60) return false;
    out = time_serial(h, mi, sec);
    return true;
}

static bool parse_date_part(const std::string& s, double& out, bool d1904, std::string& fmt) {
    int y = 0, m = 0, d = 0;
    std::vector<std::string> parts;
    std::string cur;
    char sep = 0;
    for (char ch : s) {
        if (ch == '/' || ch == '-' || ch == ' ' || ch == ',' || ch == '.') {
            if (!cur.empty()) { parts.push_back(cur); cur.clear(); }
            if (ch == '/' || ch == '-' || ch == '.') { if (sep && sep != ch) return false; sep = ch; }
            continue;
        }
        cur += ch;
    }
    if (!cur.empty()) parts.push_back(cur);
    if (parts.size() < 2 || parts.size() > 3) return false;
    auto isnum = [](const std::string& x) { if (x.empty() || x.size() > 4) return false; for (char c : x) if (!isdigit((unsigned char)c)) return false; return true; };
    if (parts.size() == 3 && isnum(parts[0]) && parts[0].size() == 4 && isnum(parts[1]) && isnum(parts[2])) {   // yyyy-mm-dd
        y = atoi(parts[0].c_str()); m = atoi(parts[1].c_str()); d = atoi(parts[2].c_str()); fmt = sep == '-' ? "yyyy-mm-dd" : "m/d/yyyy";
    } else if (parts.size() == 3 && isnum(parts[0]) && isnum(parts[1]) && isnum(parts[2])) {   // m/d/yy(yy) (en-US)
        if (sep == '.') return false;
        m = atoi(parts[0].c_str()); d = atoi(parts[1].c_str()); y = atoi(parts[2].c_str());
        if (parts[2].size() <= 2) y += y < 30 ? 2000 : 1900;
        fmt = "m/d/yyyy";
    } else if (parts.size() == 3 && isnum(parts[0]) && !isnum(parts[1]) && isnum(parts[2])) {   // d-mmm-yyyy
        if (!month_from_name(parts[1], m)) return false;
        d = atoi(parts[0].c_str()); y = atoi(parts[2].c_str());
        if (parts[2].size() <= 2) y += y < 30 ? 2000 : 1900;
        fmt = "d-mmm-yy";
    } else if (parts.size() == 3 && !isnum(parts[0]) && isnum(parts[1]) && isnum(parts[2])) {   // mmm d yyyy
        if (!month_from_name(parts[0], m)) return false;
        d = atoi(parts[1].c_str()); y = atoi(parts[2].c_str());
        if (parts[2].size() <= 2) y += y < 30 ? 2000 : 1900;
        fmt = "d-mmm-yy";
    } else if (parts.size() == 2 && !isnum(parts[0]) && isnum(parts[1]) && parts[1].size() == 4) {   // mmm yyyy
        if (!month_from_name(parts[0], m)) return false;
        d = 1; y = atoi(parts[1].c_str()); fmt = "mmm-yy";
    } else return false;
    if (m < 1 || m > 12 || d < 1 || y < 1900 || y > 9999) return false;
    static const int dim[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (d > dim[m - 1]) return false;
    if (m == 2 && d == 29 && !((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return false;
    out = date_serial(y, m, d, d1904);
    return true;
}

bool text_to_number(const std::string& s0, double& d, bool d1904) {
    std::string fmt;
    Value v = parse_input(s0, &fmt, d1904);
    if (v.is_num()) { d = v.num; return true; }
    // leading-zero numerics ("0805") still coerce to numbers in arithmetic
    if (parse_number_strict(s0, d)) return true;
    return false;
}

Value parse_input(const std::string& text, std::string* fmt_hint, bool d1904) {
    if (fmt_hint) fmt_hint->clear();
    if (text.empty()) return Value();
    if (text[0] == '\'') return Value::text(text.substr(1));
    size_t a = text.find_first_not_of(" \t"), b = text.find_last_not_of(" \t");
    if (a == std::string::npos) return Value::text(text);
    std::string s = text.substr(a, b - a + 1);
    std::string u = upper_ascii(s);
    if (u == "TRUE") return Value::boolean(true);
    if (u == "FALSE") return Value::boolean(false);
    if (s[0] == '#') { Err e = err_from_text(s); if (e != Err::None) return Value::error(e); }
    // numbers: [sign][$](digits[,ddd]*)[.digits][E±n][%]  or (number) negative
    {
        std::string t = s;
        bool neg = false, pct = false, cur = false, thou = false, expo = false, paren = false;
        if (t.size() >= 2 && t.front() == '(' && t.back() == ')') { paren = true; t = t.substr(1, t.size() - 2); }
        if (!t.empty() && (t[0] == '-' || t[0] == '+')) { neg = t[0] == '-'; t.erase(0, 1); }
        if (!t.empty() && t[0] == '$') { cur = true; t.erase(0, 1); }
        else if (t.compare(0, 3, "\xE2\x82\xAC") == 0 || t.compare(0, 2, "\xC2\xA3") == 0) { cur = true; t.erase(0, t[0] == '\xE2' ? 3 : 2); }
        if (!cur && !neg && !t.empty() && (t[0] == '-' || t[0] == '+')) {}
        if (!t.empty() && t.back() == '%') { pct = true; t.pop_back(); while (!t.empty() && t.back() == ' ') t.pop_back(); }
        // validate digits/grouping
        size_t i = 0, n = t.size();
        std::string digits;
        bool ok = n > 0;
        int group_len = -1;
        bool lead_zero = false;
        size_t int_start = i;
        while (i < n && (isdigit((unsigned char)t[i]) || t[i] == ',')) {
            if (t[i] == ',') {
                if (group_len < 0) { if (i == int_start || i - int_start > 3) { ok = false; break; } }
                else if (group_len != 3) { ok = false; break; }
                group_len = 0; thou = true;
            } else { digits += t[i]; if (group_len >= 0) ++group_len; }
            ++i;
        }
        if (thou && group_len != 3) ok = false;
        size_t int_digits = digits.size();
        if (int_digits > 1 && digits[0] == '0') lead_zero = true;
        if (ok && i < n && t[i] == '.') { digits += '.'; ++i; while (i < n && isdigit((unsigned char)t[i])) digits += t[i++]; }
        if (ok && i < n && (t[i] == 'e' || t[i] == 'E')) {
            size_t k = i + 1;
            if (k < n && (t[k] == '+' || t[k] == '-')) ++k;
            if (k < n && isdigit((unsigned char)t[k])) { digits += 'e'; digits += t.substr(i + 1, k - i - 1); while (k < n && isdigit((unsigned char)t[k])) digits += t[k++]; i = k; expo = true; }
        }
        bool has_digit = digits.find_first_of("0123456789") != std::string::npos;
        if (ok && i == n && has_digit && !(paren && neg)) {
            if (lead_zero && !thou && !cur && !pct && !expo) return Value::text(text);   // "0805" stays text (BOM part numbers)
            double d = strtod(digits.c_str(), nullptr);
            if (neg || paren) d = -d;
            if (pct) d /= 100;
            if (fmt_hint) {
                bool has_dec = digits.find('.') != std::string::npos;
                if (pct) *fmt_hint = has_dec ? "0.00%" : "0%";
                else if (cur) *fmt_hint = has_dec ? "\"$\"#,##0.00" : "\"$\"#,##0";
                else if (expo) *fmt_hint = "0.00E+00";
                else if (thou) *fmt_hint = has_dec ? "#,##0.00" : "#,##0";
                else if (paren) *fmt_hint = "#,##0_);(#,##0)";
            }
            if (std::isfinite(d)) return Value::number(d);
        }
    }
    // date / time
    {
        double dv = 0, tv = 0;
        std::string dfmt;
        bool ws = false;
        if (parse_time_part(s, tv, ws)) { if (fmt_hint) *fmt_hint = ws ? "h:mm:ss" : "h:mm"; if (upper_ascii(s).find('M') != std::string::npos) { if (fmt_hint) *fmt_hint = ws ? "h:mm:ss AM/PM" : "h:mm AM/PM"; } return Value::number(tv); }
        if (parse_date_part(s, dv, d1904, dfmt)) { if (fmt_hint) *fmt_hint = dfmt; return Value::number(dv); }
        // date + time "2024-03-15 13:45[:00]" / "3/15/2024 1:45 PM"
        size_t sp = s.find(' ');
        while (sp != std::string::npos) {
            std::string dp = s.substr(0, sp), tp = s.substr(sp + 1);
            if (parse_date_part(dp, dv, d1904, dfmt) && parse_time_part(tp, tv, ws)) { if (fmt_hint) *fmt_hint = dfmt == "yyyy-mm-dd" ? "yyyy-mm-dd hh:mm" : "m/d/yyyy h:mm"; return Value::number(dv + tv); }
            sp = s.find(' ', sp + 1);
        }
        if (s.size() >= 16 && s[10] == 'T' && parse_date_part(s.substr(0, 10), dv, d1904, dfmt)) {   // ISO 8601
            std::string tp = s.substr(11);
            if (!tp.empty() && tp.back() == 'Z') tp.pop_back();
            if (parse_time_part(tp, tv, ws)) { if (fmt_hint) *fmt_hint = "yyyy-mm-dd hh:mm:ss"; return Value::number(dv + tv); }
        }
    }
    return Value::text(text);
}

} // namespace pcbp::sheet
