// ============================================================================
// text_import.cpp — robust tabular text import engine. See text_import.h.
// ============================================================================
#include "text_import.h"
#include "io_util.h"
#include "../core/geom.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <set>
#include <sstream>

namespace pcbp::io {

// ===========================================================================
// Enum names
// ===========================================================================
const char* len_unit_name(LenUnit u) {
    switch (u) {
    case LenUnit::Auto: return "auto";
    case LenUnit::Mil: return "mil";
    case LenUnit::Inch: return "inch";
    case LenUnit::MM: return "mm";
    case LenUnit::CM: return "cm";
    }
    return "?";
}
double len_unit_mm(LenUnit u) {
    switch (u) {
    case LenUnit::Mil: return kMmPerMil;
    case LenUnit::Inch: return kMmPerInch;
    case LenUnit::MM: return 1.0;
    case LenUnit::CM: return 10.0;
    default: return 0;
    }
}
LenUnit len_unit_from_word(const std::string& w0) {
    std::string w = to_lower(trim(w0));
    if (w == "mm" || w == "millimeter" || w == "millimeters" || w == "millimetre" || w == "millimetres" || w == "metric")
        return LenUnit::MM;
    if (w == "mil" || w == "mils" || w == "thou" || w == "th" || w == "thousandths") return LenUnit::Mil;
    if (w == "in" || w == "inch" || w == "inches" || w == "\"" || w == "imperial") return LenUnit::Inch;
    if (w == "cm") return LenUnit::CM;
    return LenUnit::Auto;
}

const char* split_mode_name(SplitMode m) {
    switch (m) {
    case SplitMode::Auto: return "auto";
    case SplitMode::Delimited: return "delimited";
    case SplitMode::Fixed: return "fixed";
    }
    return "?";
}
const char* col_format_name(ColFormat f) {
    switch (f) {
    case ColFormat::General: return "General";
    case ColFormat::Text: return "Text";
    case ColFormat::Number: return "Number";
    case ColFormat::Angle: return "Angle";
    case ColFormat::Skip: return "Do not import";
    }
    return "?";
}

const char* encoding_name(Encoding e) {
    switch (e) {
    case Encoding::Auto: return "auto";
    case Encoding::Utf8: return "UTF-8";
    case Encoding::Utf8Bom: return "UTF-8 (BOM)";
    case Encoding::Utf16LE: return "UTF-16LE";
    case Encoding::Utf16BE: return "UTF-16BE";
    case Encoding::Cp1252: return "Windows-1252";
    }
    return "?";
}

const char* sep_name(Sep s) {
    switch (s) {
    case Sep::Auto: return "auto";
    case Sep::Comma: return "comma";
    case Sep::Semicolon: return "semicolon";
    case Sep::Tab: return "tab";
    case Sep::Pipe: return "pipe";
    case Sep::Custom: return "custom";
    case Sep::Spaces: return "spaces";
    case Sep::Whitespace: return "whitespace";
    case Sep::Fixed: return "fixed";
    }
    return "?";
}

static const char* kRoleNames[] = {
    "Unused", "Reference", "X", "Y", "Rotation", "Side", "Part Number", "Package", "Value",
    "Description", "Qty", "Manufacturer", "Mfr Part Number", "Vendor", "Vendor Part Number",
    "Customer Part Number", "DNP", "Process", "Polarity", "Height", "Cost", "Software Rev",
    "Part Rev", "Stock Code", "Bin", "Line Item", "Pop Type", "Part Marking", "Socket", "Unit"};
static const char* kRoleKeys[] = {
    "unused", "ref", "x", "y", "rotation", "side", "part_number", "package", "value",
    "description", "qty", "manufacturer", "mpn", "vendor", "vendor_pn",
    "cust_pn", "dnp", "process", "polarity", "height", "cost", "soft_rev",
    "part_rev", "stock_code", "bin", "line_item", "pop_type", "part_marking", "socket", "unit"};
static_assert(sizeof(kRoleNames) / sizeof(kRoleNames[0]) == (size_t)Role::Count_);
static_assert(sizeof(kRoleKeys) / sizeof(kRoleKeys[0]) == (size_t)Role::Count_);

const char* role_name(Role r) { return (size_t)r < (size_t)Role::Count_ ? kRoleNames[(size_t)r] : "?"; }
const char* role_key(Role r) { return (size_t)r < (size_t)Role::Count_ ? kRoleKeys[(size_t)r] : "unused"; }
Role role_from_key(const std::string& k) {
    for (size_t i = 0; i < (size_t)Role::Count_; ++i)
        if (k == kRoleKeys[i] || ieq(k, kRoleNames[i])) return (Role)i;
    return Role::Unused;
}

std::vector<Role> roles_for(Purpose p) {
    if (p == Purpose::Pnp)
        return {Role::Unused, Role::Ref, Role::X, Role::Y, Role::Rotation, Role::Side, Role::PartNumber,
                Role::Package, Role::Value, Role::Description, Role::Dnp, Role::Unit, Role::Height};
    if (p == Purpose::Bom)
        return {Role::Unused, Role::Ref, Role::PartNumber, Role::Package, Role::Value, Role::Description,
                Role::Qty, Role::Manufacturer, Role::MPN, Role::Vendor, Role::VendorPN, Role::CustPN,
                Role::Dnp, Role::Process, Role::Polarity, Role::Height, Role::Cost, Role::SoftRev,
                Role::PartRev, Role::StockCode, Role::Bin, Role::LineItem, Role::PopType,
                Role::PartMarking, Role::Socket};
    std::vector<Role> v;
    for (size_t i = 0; i < (size_t)Role::Count_; ++i) v.push_back((Role)i);
    return v;
}

const char* vtype_name(VType t) {
    switch (t) {
    case VType::Empty: return "empty";
    case VType::Text: return "text";
    case VType::Integer: return "integer";
    case VType::Number: return "number";
    case VType::Length: return "length";
    case VType::Angle: return "angle";
    case VType::SideTok: return "side";
    case VType::Ref: return "ref";
    case VType::RefList: return "ref list";
    case VType::Bool: return "bool";
    case VType::PartNo: return "part number";
    case VType::PackageName: return "package";
    case VType::CompValue: return "value";
    case VType::Words: return "words";
    }
    return "?";
}

const char* row_kind_name(RowKind k) {
    switch (k) {
    case RowKind::Data: return "data";
    case RowKind::Header: return "header";
    case RowKind::Preamble: return "preamble";
    case RowKind::Comment: return "comment";
    case RowKind::Blank: return "blank";
    case RowKind::Summary: return "summary";
    case RowKind::RepeatedHeader: return "repeated header";
    case RowKind::Ragged: return "ragged";
    }
    return "?";
}

// ===========================================================================
// Decoding
// ===========================================================================
static void put_utf8(std::string& o, uint32_t cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
    } else {
        o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F));
        o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
    }
}

static std::string from_utf16(const std::string& b, size_t start, bool be) {
    std::string o;
    o.reserve(b.size() / 2);
    auto rd = [&](size_t i) -> uint32_t {
        uint8_t x = (uint8_t)b[i], y = (uint8_t)b[i + 1];
        return be ? (uint32_t)((x << 8) | y) : (uint32_t)((y << 8) | x);
    };
    for (size_t i = start; i + 1 < b.size(); i += 2) {
        uint32_t u = rd(i);
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < b.size()) {
            uint32_t l = rd(i + 2);
            if (l >= 0xDC00 && l < 0xE000) { put_utf8(o, 0x10000 + ((u - 0xD800) << 10) + (l - 0xDC00)); i += 2; continue; }
        }
        put_utf8(o, u ? u : ' ');
    }
    return o;
}

static bool valid_utf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        uint8_t c = (uint8_t)s[i];
        int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0) return false;
        if (i + n >= s.size() + (n ? 0 : 1)) return false;
        for (int k = 1; k <= n; ++k) if (((uint8_t)s[i + k] >> 6) != 2) return false;
        i += n + 1;
    }
    return true;
}

static std::string from_cp1252(const std::string& s) {
    static const uint16_t hi[32] = {0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030,
                                    0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C,
                                    0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,
                                    0x017E, 0x0178};
    std::string o;
    for (char ch : s) {
        uint8_t c = (uint8_t)ch;
        if (c < 0x80) o += ch;
        else if (c < 0xA0) put_utf8(o, hi[c - 0x80]);
        else put_utf8(o, c);
    }
    return o;
}

std::string decode_text(const std::string& b, Encoding forced, Encoding* detected) {
    Encoding e = forced;
    size_t start = 0;
    const auto u8 = [&](size_t i) { return (uint8_t)b[i]; };
    if (e == Encoding::Auto) {
        if (b.size() >= 3 && u8(0) == 0xEF && u8(1) == 0xBB && u8(2) == 0xBF) e = Encoding::Utf8Bom;
        else if (b.size() >= 2 && u8(0) == 0xFF && u8(1) == 0xFE) e = Encoding::Utf16LE;
        else if (b.size() >= 2 && u8(0) == 0xFE && u8(1) == 0xFF) e = Encoding::Utf16BE;
        else {
            // heuristic: many NULs on one parity => UTF-16 without BOM
            size_t n = std::min<size_t>(b.size(), 4096) & ~(size_t)1, z0 = 0, z1 = 0;
            for (size_t i = 0; i < n; i += 2) { z0 += b[i] == 0; z1 += b[i + 1] == 0; }
            if (n >= 4 && z1 > n / 2 * 0.3 && z0 < n / 2 * 0.05) e = Encoding::Utf16LE;
            else if (n >= 4 && z0 > n / 2 * 0.3 && z1 < n / 2 * 0.05) e = Encoding::Utf16BE;
            else e = valid_utf8(b) ? Encoding::Utf8 : Encoding::Cp1252;
        }
    }
    std::string s;
    switch (e) {
    case Encoding::Utf8Bom:
        start = (b.size() >= 3 && u8(0) == 0xEF && u8(1) == 0xBB && u8(2) == 0xBF) ? 3 : 0;
        s = b.substr(start);
        break;
    case Encoding::Utf16LE:
    case Encoding::Utf16BE: {
        bool be = e == Encoding::Utf16BE;
        if (b.size() >= 2 && ((be && u8(0) == 0xFE && u8(1) == 0xFF) || (!be && u8(0) == 0xFF && u8(1) == 0xFE))) start = 2;
        s = from_utf16(b, start, be);
        break;
    }
    case Encoding::Cp1252: s = from_cp1252(b); break;
    default:
        s = b;
        if (s.size() >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) s.erase(0, 3);
        break;
    }
    if (detected) *detected = e;
    // normalise line endings, drop NULs / ^Z
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\r') { o += '\n'; if (i + 1 < s.size() && s[i + 1] == '\n') ++i; }
        else if (c == 0 || c == 0x1A) continue;
        else o += c;
    }
    return o;
}

// ===========================================================================
// Value helpers
// ===========================================================================
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

std::optional<double> parse_number(const std::string& s0, bool dc) {
    std::string s = trim(s0);
    if (s.empty()) return std::nullopt;
    if (s[0] == '+') s.erase(0, 1);
    if (s.empty()) return std::nullopt;
    // reject anything with letters / spaces inside
    for (char c : s) if (!(is_digit(c) || c == '.' || c == ',' || c == '-' || c == 'e' || c == 'E' || c == '+')) return std::nullopt;
    size_t ncomma = std::count(s.begin(), s.end(), ','), ndot = std::count(s.begin(), s.end(), '.');
    if (ncomma) {
        if (dc) {
            // "1.234,5" -> 1234.5 ; "12,5" -> 12.5
            std::string t;
            for (char c : s) if (c != '.') t += (c == ',' ? '.' : c);
            if (std::count(t.begin(), t.end(), '.') > 1) return std::nullopt;
            s = t;
        } else if (ndot == 0 && ncomma == 1) {
            size_t p = s.find(',');
            size_t tail = s.size() - p - 1;
            if (tail == 3 && p > 0 && p <= 3 + (s[0] == '-')) {     // "1,000" thousands
                s.erase(p, 1);
            } else s[p] = '.';                                       // lenient decimal comma
        } else {
            // thousands separators "1,234,567.8"
            std::string t;
            for (char c : s) if (c != ',') t += c;
            s = t;
        }
    }
    double v = 0;
    const char* b = s.data();
    const char* e = b + s.size();
    auto r = std::from_chars(b, e, v);
    if (r.ec != std::errc() || r.ptr != e) return std::nullopt;
    if (!std::isfinite(v)) return std::nullopt;
    return v;
}

// split "<number><spaces><suffix>"; returns false if no leading number
static bool split_num_suffix(const std::string& s0, std::string& num, std::string& suf) {
    std::string s = trim(s0);
    size_t i = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    size_t d0 = i;
    while (i < s.size() && (is_digit(s[i]) || s[i] == '.' || s[i] == ',')) ++i;
    if (i == d0) return false;
    // exponent
    if (i + 1 < s.size() && (s[i] == 'e' || s[i] == 'E') && (is_digit(s[i + 1]) || s[i + 1] == '-' || s[i + 1] == '+')) {
        size_t j = i + 1;
        if (s[j] == '-' || s[j] == '+') ++j;
        if (j < s.size() && is_digit(s[j])) { i = j; while (i < s.size() && is_digit(s[i])) ++i; }
    }
    num = s.substr(0, i);
    suf = to_lower(trim(s.substr(i)));
    return true;
}

std::optional<double> parse_length(const std::string& s, bool dc, LenUnit* unit) {
    if (unit) *unit = LenUnit::Auto;
    std::string num, suf;
    if (!split_num_suffix(s, num, suf)) return std::nullopt;
    auto v = parse_number(num, dc);
    if (!v) return std::nullopt;
    if (suf.empty()) return v;
    if (suf == "um" || suf == "\xC2\xB5m" || suf == "micron") { if (unit) *unit = LenUnit::MM; return *v / 1000.0; }
    LenUnit u = len_unit_from_word(suf);
    if (u == LenUnit::Auto) return std::nullopt;
    if (unit) *unit = u;
    return v;
}

std::optional<double> parse_angle(const std::string& s0, bool dc) {
    std::string s = trim(s0);
    if (s.size() > 1 && (s[0] == 'R' || s[0] == 'r') && (is_digit(s[1]) || s[1] == '-')) s.erase(0, 1);
    std::string num, suf;
    if (!split_num_suffix(s, num, suf)) return std::nullopt;
    auto v = parse_number(num, dc);
    if (!v) return std::nullopt;
    if (suf.empty() || suf == "\xC2\xB0" || suf == "deg" || suf == "degree" || suf == "degrees" || suf == "d" ||
        suf == "\xC2\xBA")
        return v;
    return std::nullopt;
}

double normalize_angle(double a) {
    a = std::fmod(a, 360.0);
    if (a < 0) a += 360.0;
    if (a >= 359.9999995 || std::fabs(a) < 5e-7) a = 0.0;
    return a;
}

std::optional<bool> parse_flag(const std::string& s0) {
    std::string s = to_upper(trim(s0));
    static const char* yes[] = {"TRUE", "T", "Y", "YES", "1", "X", "ON", "+", "POL", "POLARIZED", "JA", "OUI", "SI",
                                "\xE6\x98\xAF"};
    static const char* no[] = {"FALSE", "F", "N", "NO", "0", "OFF", "-", "NON", "NEIN", "\xE5\x90\xA6"};
    for (auto y : yes) if (s == y) return true;
    for (auto n : no) if (s == n) return false;
    return std::nullopt;
}

static bool is_dnp_token(const std::string& u) {
    static const char* t[] = {"DNP", "DNI", "DNF", "DNM", "NP", "NF", "NOPOP", "NO POP", "NOT POPULATED", "NOT FITTED",
                              "NOT PLACED", "NO FIT", "NOFIT", "DO NOT POPULATE", "DO NOT PLACE", "DO NOT FIT",
                              "DO NOT INSTALL", "DO NOT MOUNT", "NOT MOUNTED", "NC", "N.C.", "EXCLUDE", "EXCLUDED",
                              "NO STUFF", "NOSTUFF", "NO-POP", "NOT ASSEMBLED", "UNPOPULATED", "OMIT", "OMITTED"};
    for (auto x : t) if (u == x) return true;
    return false;
}
static bool is_fit_token(const std::string& u) {
    static const char* t[] = {"FITTED", "FIT", "PLACE", "PLACED", "POPULATE", "POPULATED", "POP", "STUFF", "MOUNT",
                              "MOUNTED", "INSTALL", "INSTALLED", "ASSEMBLE", "ASSEMBLED"};
    for (auto x : t) if (u == x) return true;
    return false;
}

bool cell_means_dnp(const std::string& s, bool invert) {
    std::string u = to_upper(trim(s));
    if (u.empty()) return false;
    if (is_dnp_token(u)) return true;
    if (is_fit_token(u)) return false;
    auto f = parse_flag(u);
    if (!f) return false;
    return invert ? !*f : *f;
}

// ---------------------------------------------------------------------------
// Reference designators
// ---------------------------------------------------------------------------
struct RefParts { std::string prefix; long num = -1; std::string suffix; };

static bool split_ref(const std::string& s, RefParts& r) {
    size_t i = 0;
    r = RefParts{};
    while (i < s.size() && (is_alpha(s[i]) || s[i] == '$' || s[i] == '_')) ++i;
    if (i == 0 || i > 6) return false;
    if (!is_alpha(s[0])) return false;
    size_t d = i;
    while (i < s.size() && is_digit(s[i])) ++i;
    if (i == d || i - d > 6) return false;
    r.prefix = s.substr(0, d);
    r.num = std::atol(s.substr(d, i - d).c_str());
    r.suffix = s.substr(i);
    // optional suffix: letters, or "_n" / ".n" sub-part
    if (r.suffix.size() > 4) return false;
    for (char c : r.suffix) if (!(is_alpha(c) || is_digit(c) || c == '_' || c == '.')) return false;
    if (!r.suffix.empty() && is_digit(r.suffix[0])) return false;
    return true;
}

bool is_refdes(const std::string& s) {
    RefParts r;
    return s.size() <= 16 && split_ref(trim(s), r);
}

static std::string replace_all(std::string s, const std::string& a, const std::string& b) {
    size_t p = 0;
    while ((p = s.find(a, p)) != std::string::npos) { s.replace(p, a.size(), b); p += b.size(); }
    return s;
}

// ---------------------------------------------------------------------------
// Reference expansion (specs/REFEXPAND.md). Hard rules:
//   * every range is bounded (opt.max_span refs) and the whole cell is bounded
//     (opt.max_total refs): output size is O(max_total) whatever the input;
//   * the prefix must match on both ends (R1-C5 is an error, never "R1..R5");
//   * a suffix is allowed only when identical on both ends (R1A-R5A) or as a
//     single-letter range on one number (U1A-U1D);
//   * zero padding of the start is preserved (R01-R10 -> R01..R10); a padded
//     shorthand end without a padded start (C10-0805) is rejected;
//   * reversed ranges, malformed ends ("R1-", "R1-R"), numbers > 6 digits are
//     errors: the token is kept literally (it will not match a board ref) and
//     reported; nothing is guessed;
//   * duplicates inside one cell are dropped and reported.
// ---------------------------------------------------------------------------
const char* ref_issue_name(RefIssue k) {
    switch (k) {
    case RefIssue::Reversed: return "reversed range";
    case RefIssue::SpanTooLarge: return "range too large";
    case RefIssue::PrefixMismatch: return "prefix differs";
    case RefIssue::SuffixMismatch: return "suffix differs";
    case RefIssue::PadMismatch: return "zero padding differs";
    case RefIssue::Malformed: return "malformed range";
    case RefIssue::TotalLimit: return "too many references";
    case RefIssue::Duplicate: return "duplicate reference";
    case RefIssue::BareNumber: return "bare number";
    }
    return "?";
}

static std::string pad_num(long v, size_t width) {
    std::string s = std::to_string(v);
    if (s.size() < width) s.insert(0, width - s.size(), '0');
    return s;
}

RefExpansion expand_refs_ex(const std::string& in, const RefExpandOptions& opt) {
    RefExpansion R;
    const long max_span = std::max(1, opt.max_span);
    const size_t max_total = std::max<size_t>(1, opt.max_total);
    std::string s = in.size() > opt.max_input ? in.substr(0, opt.max_input) : in;
    if (in.size() > opt.max_input)
        R.issues.push_back({RefIssue::TotalLimit, "", "reference text longer than " + std::to_string(opt.max_input) + " characters; rest ignored", true});
    s = replace_all(s, "\xE2\x80\x93", "-");     // en dash
    s = replace_all(s, "\xE2\x80\x94", "-");     // em dash
    s = replace_all(s, "\xE2\x88\x92", "-");     // minus sign
    s = replace_all(s, "\xEF\xBC\x8C", ",");     // full-width comma
    s = replace_all(s, "\xE3\x80\x81", ",");     // ideographic comma
    s = replace_all(s, "..", "-");
    s = replace_all(s, "~", "-");
    for (size_t i = 1; i + 1 < s.size(); ++i)    // "R1:R5"
        if (s[i] == ':' && std::isalnum((unsigned char)s[i - 1]) && std::isalnum((unsigned char)s[i + 1])) s[i] = '-';
    std::vector<std::string> toks;
    std::string cur;
    for (char c : s) {
        if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/' || c == '|' || c == '"' || c == '\'') {
            if (!cur.empty()) toks.push_back(cur);
            cur.clear();
        } else cur += c;
    }
    if (!cur.empty()) toks.push_back(cur);
    std::vector<std::string> m;                  // "R1 - R5", "R1- R5", "R1 -R5", "R1 to R5"
    for (size_t i = 0; i < toks.size(); ++i) {
        const std::string& t = toks[i];
        std::string lt = to_lower(t);
        if ((t == "-" || lt == "to" || lt == "thru" || lt == "through" || lt == "bis") && !m.empty() && i + 1 < toks.size()) {
            m.back() += "-" + toks[++i];
            continue;
        }
        if (t.size() > 1 && t.back() == '-' && i + 1 < toks.size()) { m.push_back(t + toks[++i]); continue; }
        if (t.size() > 1 && t[0] == '-' && !m.empty()) { m.back() += t; continue; }
        m.push_back(t);
    }
    auto up = [&](const std::string& x) { return opt.upper ? to_upper(x) : x; };
    std::set<std::string> seen;
    bool full = false;
    auto emit = [&](const std::string& r) {
        if (full) return;
        if (R.refs.size() >= max_total) {
            full = true;
            R.issues.push_back({RefIssue::TotalLimit, r, "more than " + std::to_string(max_total) + " references in one cell; rest ignored", true});
            return;
        }
        std::string key = to_upper(r);
        if (!seen.insert(key).second) {
            R.duplicates.push_back(r);
            R.issues.push_back({RefIssue::Duplicate, r, r + " listed twice", false});
            return;
        }
        R.refs.push_back(r);
    };
    auto bad = [&](RefIssue k, const std::string& t, const std::string& why) {
        R.issues.push_back({k, t, "\"" + t + "\": " + why, true});
        emit(up(t));
    };
    std::string last_prefix;                     // for bare numbers "C1,3,5"
    for (auto& t : m) {
        if (full) break;
        bool all_digits = std::all_of(t.begin(), t.end(), [](char c) { return is_digit(c); });
        if (all_digits) {
            if (opt.bare_numbers && !last_prefix.empty() && t.size() <= 6) {
                R.issues.push_back({RefIssue::BareNumber, t, "bare number " + t + " read as " + up(last_prefix + t), false});
                emit(up(last_prefix + t));
            } else emit(up(t));
            continue;
        }
        size_t dash = t.find('-', 1);
        if (dash == std::string::npos) {
            RefParts rp;
            if (split_ref(t, rp) && rp.suffix.empty()) last_prefix = rp.prefix; else last_prefix.clear();
            emit(up(t));
            continue;
        }
        std::string a = t.substr(0, dash), b = t.substr(dash + 1);
        RefParts ra, rb;
        if (!split_ref(a, ra)) { last_prefix.clear(); emit(up(t)); continue; }   // "J-1", "PCB-1": a name, not a range
        if (b.empty()) { bad(RefIssue::Malformed, t, "range has no end"); continue; }
        const size_t da = a.size() - ra.prefix.size() - ra.suffix.size();       // digit count of the start
        const bool b_digits = std::all_of(b.begin(), b.end(), [](char c) { return is_digit(c); });
        size_t db = 0;
        if (b_digits) {
            if (b.size() > 6) { bad(RefIssue::Malformed, t, "range end has more than 6 digits"); continue; }
            if (!ra.suffix.empty()) { bad(RefIssue::SuffixMismatch, t, "start has a suffix, end has none"); continue; }
            rb.prefix = ra.prefix; rb.num = std::atol(b.c_str()); db = b.size();
        } else if (split_ref(b, rb)) {
            if (!ieq(rb.prefix, ra.prefix)) { bad(RefIssue::PrefixMismatch, t, "prefix " + ra.prefix + " vs " + rb.prefix); continue; }
            db = b.size() - rb.prefix.size() - rb.suffix.size();
        } else {
            // "U1-A", "J1-PWR": a hyphenated name unless the end starts like a ref
            bool alpha_then_nothing = std::all_of(b.begin(), b.end(), [](char c) { return is_alpha(c); });
            if (!alpha_then_nothing && is_alpha(b[0]) && std::any_of(b.begin(), b.end(), [](char c) { return is_digit(c); }) &&
                b.size() > 7) { bad(RefIssue::Malformed, t, "range end is not a reference"); continue; }
            if (alpha_then_nothing && ieq(b, ra.prefix)) { bad(RefIssue::Malformed, t, "range end has no number"); continue; }
            emit(up(t));
            continue;
        }
        last_prefix = ra.suffix.empty() ? ra.prefix : std::string();
        // single-letter range on one number "U1A-U1D"
        if (ra.num == rb.num && ra.suffix.size() == 1 && rb.suffix.size() == 1 && is_alpha(ra.suffix[0]) && is_alpha(rb.suffix[0])) {
            char c0 = (char)std::toupper((unsigned char)ra.suffix[0]), c1 = (char)std::toupper((unsigned char)rb.suffix[0]);
            if (c1 < c0) { bad(RefIssue::Reversed, t, "letters run backwards"); continue; }
            std::string base = a.substr(0, a.size() - 1);
            for (char c = c0; c <= c1 && !full; ++c) emit(up(base + (std::islower((unsigned char)ra.suffix[0]) ? (char)std::tolower((unsigned char)c) : c)));
            continue;
        }
        if (!ieq(ra.suffix, rb.suffix)) { bad(RefIssue::SuffixMismatch, t, "suffix \"" + ra.suffix + "\" vs \"" + rb.suffix + "\""); continue; }
        if (rb.num < ra.num) { bad(RefIssue::Reversed, t, "end is lower than start"); continue; }
        if (rb.num == ra.num) { emit(up(a)); continue; }
        if (rb.num - ra.num + 1 > max_span) {
            bad(RefIssue::SpanTooLarge, t, std::to_string(rb.num - ra.num + 1) + " references (limit " + std::to_string(max_span) + ")");
            continue;
        }
        const bool pa = da > 1 && a[ra.prefix.size()] == '0';
        const bool pb = db > 1 && (b_digits ? b[0] : b[rb.prefix.size()]) == '0';
        if (pb && !pa) { bad(RefIssue::PadMismatch, t, "end is zero-padded, start is not"); continue; }
        if (pa && pb && da != db) { bad(RefIssue::PadMismatch, t, "start and end have different widths"); continue; }
        const size_t width = pa ? da : 0;
        for (long k = ra.num; k <= rb.num && !full; ++k) emit(up(ra.prefix + pad_num(k, width) + ra.suffix));
    }
    return R;
}

std::vector<std::string> expand_refs(const std::string& in, bool upper, std::vector<std::string>* warn) {
    RefExpandOptions o;
    o.upper = upper;
    o.bare_numbers = false;
    RefExpansion r = expand_refs_ex(in, o);
    if (warn) for (auto& i : r.issues) if (i.error) warn->push_back(i.msg);
    // duplicates inside one cell are dropped (reported through expand_refs_ex)
    return r.refs;
}

bool is_ref_list(const std::string& s) {
    if (trim(s).empty()) return false;
    std::vector<std::string> w;
    auto v = expand_refs(s, true, &w);
    if (v.empty() || !w.empty()) return false;
    for (auto& r : v) if (!is_refdes(r)) return false;
    return true;
}

// ===========================================================================
// Cell / column classification
// ===========================================================================
static bool is_side_token(const std::string& u) {
    static const char* t[] = {"T", "B", "TOP", "BOTTOM", "BOT", "TOPLAYER", "BOTTOMLAYER", "TOP LAYER", "BOTTOM LAYER",
                              "F.CU", "B.CU", "FRONT", "BACK", "TOP SIDE", "BOTTOM SIDE", "TOPSIDE", "BOTTOMSIDE",
                              "COMPONENT", "SOLDER", "OBEN", "UNTEN", "DESSUS", "DESSOUS", "SUPERIOR", "INFERIOR",
                              "\xE9\xA1\xB6\xE5\xB1\x82", "\xE5\xBA\x95\xE5\xB1\x82", "L1", "TOP_SIDE", "BOT_SIDE",
                              "BOTTOM_SIDE", "PLACEMENT_TOP", "PLACEMENT_BOTTOM"};
    for (auto x : t) if (u == x) return true;
    return false;
}

static bool is_package_name(const std::string& s0) {
    std::string s = to_upper(trim(s0));
    if (s.empty() || s.size() > 60 || s.find(' ') != std::string::npos) return false;
    static const char* chip[] = {"01005", "0201", "0402", "0603", "0805", "1206", "1210", "1812", "2010", "2512", "2220",
                                 "1008", "0612", "1806", "2225"};
    for (auto c : chip) if (s == c) return true;
    static const char* pre[] = {"SOT", "SOD", "SOIC", "SOP", "SSOP", "TSSOP", "MSOP", "QFN", "QFP", "LQFP", "TQFP",
                                "BGA", "FBGA", "DFN", "DIP", "PDIP", "PLCC", "TO-", "TO2", "TO9", "SMA", "SMB", "SMC",
                                "MELF", "MINIMELF", "SC-", "SC7", "DPAK", "D2PAK", "R_", "C_", "L_", "D_", "LED_",
                                "LED-", "HDR", "CONN", "AXIAL", "RADIAL", "TQFN", "WSON", "VSON", "SON", "XTAL",
                                "CRYSTAL", "SIP", "SOJ", "LGA", "CSP", "WLCSP", "PINHEADER", "PIN_HEADER", "CAPC", "RESC",
                                "INDC", "CAPMP", "SODFL", "SOT23", "SOIC8", "TSOP", "HTSSOP", "UQFN", "VQFN", "WQFN",
                                "POWERPAK", "SMD", "SMT", "CASE", "EIA", "TP", "TESTPOINT", "FIDUCIAL", "MOUNTINGHOLE"};
    for (auto p : pre) if (s.rfind(p, 0) == 0) return true;
    if (s.find("METRIC") != std::string::npos || s.find("_HANDSOLDER") != std::string::npos) return true;
    // C0805 / R0603 / L1210 / CAP0603
    size_t i = 0;
    while (i < s.size() && is_alpha(s[i])) ++i;
    if (i >= 1 && i <= 4 && s.size() - i == 4) {
        std::string d = s.substr(i);
        for (auto c : chip) if (d == c) return true;
    }
    return false;
}

static bool is_comp_value(const std::string& s0) {
    std::string s = trim(s0);
    if (s.empty() || s.size() > 24 || !is_digit(s[0])) return false;
    // 10k, 4k7, 100nF, 0R1, 1uF/16V, 10R, 1%, 100n, 2.2uH, 4.7µF, 10 kOhm
    size_t i = 0;
    while (i < s.size() && (is_digit(s[i]) || s[i] == '.' || s[i] == ',')) ++i;
    if (i == s.size()) return false;            // plain number
    std::string rest = s.substr(i);
    if (!rest.empty() && rest[0] == ' ') rest = trim(rest);
    if (rest.empty()) return false;
    std::string r = to_lower(rest);
    static const char* mult[] = {"p", "n", "u", "\xC2\xB5", "m", "k", "meg", "g", "r", "f", "h", "v", "a", "w", "%",
                                 "ohm", "\xCE\xA9", "hz", "khz", "mhz"};
    for (auto m : mult) if (r.rfind(m, 0) == 0) return true;
    return false;
}

VType classify_cell(const std::string& c0, bool dc) {
    std::string c = trim(c0);
    if (c.empty()) return VType::Empty;
    std::string u = to_upper(c);
    if (is_side_token(u)) return VType::SideTok;
    bool all_digit = std::all_of(c.begin() + (c[0] == '-' || c[0] == '+'), c.end(), [](char x) { return is_digit(x); });
    if (all_digit && c.size() > (size_t)(c[0] == '-' || c[0] == '+')) return VType::Integer;
    if (parse_number(c, dc)) return VType::Number;
    if (is_refdes(c)) return VType::Ref;
    {
        LenUnit lu;
        if (parse_length(c, dc, &lu) && lu != LenUnit::Auto) return VType::Length;
        if (parse_angle(c, dc)) return VType::Angle;
    }
    if (parse_flag(u) || is_dnp_token(u) || is_fit_token(u)) return VType::Bool;
    if (is_ref_list(c)) return VType::RefList;
    if (is_package_name(c)) return VType::PackageName;
    if (is_comp_value(c)) return VType::CompValue;
    bool has_space = c.find(' ') != std::string::npos;
    bool has_digit = std::any_of(c.begin(), c.end(), [](char x) { return is_digit(x); });
    if (!has_space && has_digit && c.size() >= 3) return VType::PartNo;
    if (has_space) return VType::Words;
    return VType::Text;
}

VType classify_column(const std::vector<std::string>& cells, bool dc, double* conf) {
    std::map<VType, int> n;
    int ne = 0;
    std::vector<double> nums;
    bool leading_zero_int = false;
    for (auto& c : cells) {
        VType t = classify_cell(c, dc);
        if (t == VType::Empty) continue;
        ++ne;
        ++n[t];
        if (t == VType::Integer || t == VType::Number || t == VType::Angle) {
            if (auto v = parse_angle(c, dc)) nums.push_back(*v);
            std::string tc = trim(c);
            if (t == VType::Integer && tc.size() >= 2 && tc[0] == '0') leading_zero_int = true;
        }
    }
    if (conf) *conf = 0;
    if (!ne) return VType::Empty;
    auto f = [&](std::initializer_list<VType> ts) {
        int k = 0;
        for (auto t : ts) { auto it = n.find(t); if (it != n.end()) k += it->second; }
        return (double)k / ne;
    };
    auto ret = [&](VType t, double c) { if (conf) *conf = c; return t; };
    // package codes look like integers ("0805")
    {
        int pk = 0;
        for (auto& c : cells) if (!trim(c).empty() && is_package_name(c)) ++pk;
        if ((double)pk / ne >= 0.6 && (leading_zero_int || n[VType::Integer] < pk)) return ret(VType::PackageName, (double)pk / ne);
    }
    double fnum = f({VType::Integer, VType::Number, VType::Angle, VType::Length});
    if (fnum >= 0.8) {
        if (f({VType::Length}) >= 0.5) return ret(VType::Length, fnum);
        // angle: within +-360, mostly multiples of 45, at least one >= 45
        if (!nums.empty() && f({VType::Angle}) > 0 && f({VType::Length}) == 0) return ret(VType::Angle, fnum);
        int mult = 0, big = 0;
        bool range = true;
        for (double v : nums) {
            if (std::fabs(v) > 360.0) range = false;
            if (std::fabs(std::fmod(v, 45.0)) < 1e-6) ++mult;
            if (std::fabs(v) >= 45) ++big;
        }
        if (!nums.empty() && range && mult >= 0.8 * nums.size() && big > 0 && n[VType::Number] * 2 <= (int)nums.size())
            return ret(VType::Angle, fnum);
        if (f({VType::Integer}) >= 0.95) return ret(VType::Integer, fnum);
        return ret(VType::Number, fnum);
    }
    double fref = f({VType::Ref, VType::RefList});
    if (fref >= 0.7) return ret(n[VType::RefList] ? VType::RefList : VType::Ref, fref);
    double fs = f({VType::SideTok});
    if (fs >= 0.8) return ret(VType::SideTok, fs);
    double fb = f({VType::Bool});
    if (fb >= 0.8) return ret(VType::Bool, fb);
    double fp = f({VType::PackageName});
    if (fp >= 0.6) return ret(VType::PackageName, fp);
    double fv = f({VType::CompValue});
    if (fv >= 0.6) return ret(VType::CompValue, fv);
    double fpn = f({VType::PartNo, VType::Integer});
    if (fpn >= 0.6 && n[VType::PartNo]) return ret(VType::PartNo, fpn);
    double fw = f({VType::Words});
    if (fw >= 0.5) return ret(VType::Words, fw);
    return ret(VType::Text, 1.0 - fw);
}

// ===========================================================================
// Header synonyms
// ===========================================================================
std::string norm_header(const std::string& s0) {
    std::string s;
    // drop "(mm)", "[mil]" groups
    int depth = 0;
    for (char c : s0) {
        if (c == '(' || c == '[' || c == '{') { ++depth; continue; }
        if (c == ')' || c == ']' || c == '}') { if (depth) --depth; continue; }
        if (depth) continue;
        s += c;
    }
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x80) { o += (char)c; continue; }
        if (std::isalnum(c)) o += (char)std::tolower(c);
        else if (c == '#') o += "no";
    }
    // fold common accented letters (UTF-8 two-byte Latin-1 range) to ASCII
    static const std::pair<const char*, const char*> fold[] = {
        {"\xC3\xA9", "e"}, {"\xC3\xA8", "e"}, {"\xC3\xAA", "e"}, {"\xC3\xA0", "a"}, {"\xC3\xA1", "a"},
        {"\xC3\xA4", "a"}, {"\xC3\xB6", "o"}, {"\xC3\xB3", "o"}, {"\xC3\xBC", "u"}, {"\xC3\xBA", "u"},
        {"\xC3\xAD", "i"}, {"\xC3\xB4", "o"}, {"\xC3\xA7", "c"}, {"\xC3\xB1", "n"}, {"\xC3\x9F", "ss"},
        {"\xC3\x89", "e"}, {"\xC3\x84", "a"}, {"\xC3\x96", "o"}, {"\xC3\x9C", "u"}};
    for (auto& [a, b] : fold) o = replace_all(o, a, b);
    return o;
}

namespace {

struct Syn { const char* key; Role role; float conf; bool invert = false; };

// Exact keys (after norm_header). Sources: Altium, KiCad, Eagle, OrCAD/Allegro,
// PADS, Mentor/Xpedition, Zuken, DipTrace, EasyEDA/JLCPCB, Proteus, ERP exports,
// German / French / Spanish / Chinese common headers.
const std::vector<Syn>& syns() {
    static const std::vector<Syn> v = {
        // reference
        {"designator", Role::Ref, 1}, {"designators", Role::Ref, 1}, {"reference", Role::Ref, 1},
        {"references", Role::Ref, 1}, {"ref", Role::Ref, 1}, {"refs", Role::Ref, 1}, {"refdes", Role::Ref, 1},
        {"refdesignator", Role::Ref, 1}, {"referencedesignator", Role::Ref, 1}, {"referencedesignators", Role::Ref, 1},
        {"partreference", Role::Ref, 1}, {"reflist", Role::Ref, 1}, {"referencelist", Role::Ref, 1},
        {"refdesig", Role::Ref, 1}, {"refdesignators", Role::Ref, 1}, {"compref", Role::Ref, 0.9f},
        {"componentref", Role::Ref, 1}, {"componentreference", Role::Ref, 1}, {"partid", Role::Ref, 0.6f},
        {"instance", Role::Ref, 0.7f}, {"instancename", Role::Ref, 0.8f}, {"symbolname", Role::Ref, 0.5f},
        {"referenz", Role::Ref, 1}, {"bezeichner", Role::Ref, 0.9f}, {"kennzeichen", Role::Ref, 0.8f},
        {"bauteilbezeichner", Role::Ref, 1}, {"bestuckungsbezeichner", Role::Ref, 1}, {"repere", Role::Ref, 1},
        {"reperes", Role::Ref, 1}, {"referencia", Role::Ref, 1}, {"referencias", Role::Ref, 1},
        {"designador", Role::Ref, 1}, {"\xE4\xBD\x8D\xE5\x8F\xB7", Role::Ref, 1},
        {"\xE5\x85\x83\xE4\xBB\xB6\xE4\xBD\x8D\xE5\x8F\xB7", Role::Ref, 1}, {"\xE6\xA0\x87\xE5\x8F\xB7", Role::Ref, 1},
        {"\xE4\xBD\x8D\xE7\xBD\xAE\xE5\x8F\xB7", Role::Ref, 1},
        // X / Y
        {"x", Role::X, 1}, {"posx", Role::X, 1}, {"centerx", Role::X, 1}, {"centrex", Role::X, 1}, {"midx", Role::X, 1},
        {"locationx", Role::X, 1}, {"xlocation", Role::X, 1}, {"xpos", Role::X, 1}, {"xposition", Role::X, 1},
        {"positionx", Role::X, 1}, {"xcoord", Role::X, 1}, {"xcoordinate", Role::X, 1}, {"coordx", Role::X, 1},
        {"cx", Role::X, 0.8f}, {"centroidx", Role::X, 1}, {"placex", Role::X, 1}, {"pickx", Role::X, 0.9f},
        {"symx", Role::X, 1}, {"compx", Role::X, 1}, {"xcenter", Role::X, 1}, {"xmid", Role::X, 1}, {"xloc", Role::X, 1},
        {"locx", Role::X, 1}, {"originx", Role::X, 0.6f}, {"refx", Role::X, 0.6f}, {"padx", Role::X, 0.5f},
        {"xkoordinate", Role::X, 1}, {"xwert", Role::X, 1}, {"\xE5\x9D\x90\xE6\xA0\x87x", Role::X, 1},
        {"x\xE5\x9D\x90\xE6\xA0\x87", Role::X, 1}, {"\xE4\xB8\xAD\xE5\xBF\x83x", Role::X, 1}, {"placementx", Role::X, 1},
        {"y", Role::Y, 1}, {"posy", Role::Y, 1}, {"centery", Role::Y, 1}, {"centrey", Role::Y, 1}, {"midy", Role::Y, 1},
        {"locationy", Role::Y, 1}, {"ylocation", Role::Y, 1}, {"ypos", Role::Y, 1}, {"yposition", Role::Y, 1},
        {"positiony", Role::Y, 1}, {"ycoord", Role::Y, 1}, {"ycoordinate", Role::Y, 1}, {"coordy", Role::Y, 1},
        {"cy", Role::Y, 0.8f}, {"centroidy", Role::Y, 1}, {"placey", Role::Y, 1}, {"picky", Role::Y, 0.9f},
        {"symy", Role::Y, 1}, {"compy", Role::Y, 1}, {"ycenter", Role::Y, 1}, {"ymid", Role::Y, 1}, {"yloc", Role::Y, 1},
        {"locy", Role::Y, 1}, {"originy", Role::Y, 0.6f}, {"refy", Role::Y, 0.6f}, {"pady", Role::Y, 0.5f},
        {"ykoordinate", Role::Y, 1}, {"ywert", Role::Y, 1}, {"\xE5\x9D\x90\xE6\xA0\x87y", Role::Y, 1},
        {"y\xE5\x9D\x90\xE6\xA0\x87", Role::Y, 1}, {"\xE4\xB8\xAD\xE5\xBF\x83y", Role::Y, 1}, {"placementy", Role::Y, 1},
        // rotation
        {"rotation", Role::Rotation, 1}, {"rot", Role::Rotation, 1}, {"rotate", Role::Rotation, 1},
        {"angle", Role::Rotation, 1}, {"orientation", Role::Rotation, 1}, {"orient", Role::Rotation, 1},
        {"theta", Role::Rotation, 1}, {"symrotate", Role::Rotation, 1}, {"symrotation", Role::Rotation, 1},
        {"rotationangle", Role::Rotation, 1}, {"placementangle", Role::Rotation, 1}, {"compangle", Role::Rotation, 1},
        {"drehung", Role::Rotation, 1}, {"winkel", Role::Rotation, 1}, {"drehwinkel", Role::Rotation, 1},
        {"rotacion", Role::Rotation, 1}, {"angulo", Role::Rotation, 1}, {"\xE6\x97\x8B\xE8\xBD\xAC", Role::Rotation, 1},
        {"\xE8\xA7\x92\xE5\xBA\xA6", Role::Rotation, 1}, {"\xE6\x96\xB9\xE5\x90\x91", Role::Rotation, 0.8f},
        {"r", Role::Rotation, 0.5f}, {"a", Role::Rotation, 0.4f},
        // side
        {"side", Role::Side, 1}, {"layer", Role::Side, 1}, {"tb", Role::Side, 1}, {"topbottom", Role::Side, 1},
        {"boardside", Role::Side, 1}, {"mountside", Role::Side, 1}, {"placementside", Role::Side, 1},
        {"placelayer", Role::Side, 1}, {"pcbside", Role::Side, 1}, {"compside", Role::Side, 1},
        {"seite", Role::Side, 1}, {"lage", Role::Side, 0.8f}, {"bestuckungsseite", Role::Side, 1}, {"cote", Role::Side, 1},
        {"couche", Role::Side, 1}, {"face", Role::Side, 0.8f}, {"lado", Role::Side, 1}, {"capa", Role::Side, 1},
        {"\xE5\xB1\x82", Role::Side, 1}, {"\xE9\x9D\xA2", Role::Side, 1}, {"\xE5\xB1\x82\xE9\x9D\xA2", Role::Side, 1},
        {"mirror", Role::Side, 0.9f, true}, {"symmirror", Role::Side, 1, true}, {"mirrored", Role::Side, 0.9f, true},
        {"bottom", Role::Side, 0.7f, true}, {"flip", Role::Side, 0.7f, true}, {"flipped", Role::Side, 0.7f, true},
        // part number
        {"partnumber", Role::PartNumber, 1}, {"partno", Role::PartNumber, 1}, {"pn", Role::PartNumber, 1},
        {"partnum", Role::PartNumber, 1}, {"partnr", Role::PartNumber, 1}, {"internalpn", Role::PartNumber, 1},
        {"internalpartnumber", Role::PartNumber, 1}, {"companypn", Role::PartNumber, 1},
        {"companypartnumber", Role::PartNumber, 1}, {"itemnumber", Role::PartNumber, 0.9f},
        {"itemno", Role::PartNumber, 0.7f}, {"articlenumber", Role::PartNumber, 1}, {"artikelnummer", Role::PartNumber, 1},
        {"artikel", Role::PartNumber, 0.8f}, {"artikelnr", Role::PartNumber, 1}, {"sachnummer", Role::PartNumber, 1},
        {"teilenummer", Role::PartNumber, 1}, {"materialnumber", Role::PartNumber, 1},
        {"materialnummer", Role::PartNumber, 1}, {"material", Role::PartNumber, 0.8f}, {"materialno", Role::PartNumber, 1},
        {"numerodepiece", Role::PartNumber, 1}, {"numerodeparte", Role::PartNumber, 1},
        {"numeropieza", Role::PartNumber, 1}, {"\xE7\x89\xA9\xE6\x96\x99\xE7\xBC\x96\xE7\xA0\x81", Role::PartNumber, 1},
        {"\xE6\x96\x99\xE5\x8F\xB7", Role::PartNumber, 1}, {"\xE7\x89\xA9\xE6\x96\x99\xE5\x8F\xB7", Role::PartNumber, 1},
        {"\xE7\x89\xA9\xE6\x96\x99\xE7\xBC\x96\xE5\x8F\xB7", Role::PartNumber, 1},
        {"libref", Role::PartNumber, 0.9f}, {"libraryref", Role::PartNumber, 0.9f}, {"device", Role::PartNumber, 0.8f},
        {"parttype", Role::PartNumber, 0.9f}, {"compdevicetype", Role::PartNumber, 1}, {"devicetype", Role::PartNumber, 0.9f},
        {"partname", Role::PartNumber, 0.9f}, {"number", Role::PartNumber, 0.8f}, {"compname", Role::PartNumber, 0.7f},
        {"componentname", Role::PartNumber, 0.7f}, {"partcode", Role::PartNumber, 1}, {"code", Role::PartNumber, 0.6f},
        {"customerpartnumber", Role::CustPN, 1}, {"custpn", Role::CustPN, 1}, {"customerpn", Role::CustPN, 1},
        {"custpartnumber", Role::CustPN, 1}, {"cpn", Role::CustPN, 0.9f}, {"customerpartno", Role::CustPN, 1},
        // package
        {"package", Role::Package, 1}, {"packages", Role::Package, 1}, {"footprint", Role::Package, 1},
        {"footprints", Role::Package, 1}, {"pcbfootprint", Role::Package, 1}, {"pattern", Role::Package, 1},
        {"decal", Role::Package, 1}, {"pcbdecal", Role::Package, 1}, {"geometry", Role::Package, 1},
        {"geom", Role::Package, 1}, {"geometryname", Role::Package, 1}, {"case", Role::Package, 1},
        {"casepackage", Role::Package, 1}, {"packagecase", Role::Package, 1}, {"comppackage", Role::Package, 1},
        {"symname", Role::Package, 0.9f}, {"jedec", Role::Package, 0.9f}, {"jedectype", Role::Package, 0.9f},
        {"pkg", Role::Package, 1}, {"gehause", Role::Package, 1}, {"bauform", Role::Package, 1},
        {"boitier", Role::Package, 1}, {"empreinte", Role::Package, 1}, {"encapsulado", Role::Package, 1},
        {"huella", Role::Package, 1}, {"\xE5\xB0\x81\xE8\xA3\x85", Role::Package, 1},
        {"\xE5\xB0\x81\xE8\xA3\x85\xE5\xBD\xA2\xE5\xBC\x8F", Role::Package, 1}, {"outline", Role::Package, 0.8f},
        {"partpackage", Role::Package, 1}, {"footprintname", Role::Package, 1}, {"packagename", Role::Package, 1},
        {"cell", Role::Package, 0.6f}, {"cellname", Role::Package, 0.9f}, {"shape", Role::Package, 0.8f},
        {"shapename", Role::Package, 0.9f}, {"land", Role::Package, 0.6f}, {"landpattern", Role::Package, 1},
        {"pcbcomponent", Role::Package, 0.6f}, {"size", Role::Package, 0.5f},
        // value
        {"value", Role::Value, 1}, {"values", Role::Value, 1}, {"val", Role::Value, 1}, {"comment", Role::Value, 0.9f},
        {"wert", Role::Value, 1}, {"valeur", Role::Value, 1}, {"valor", Role::Value, 1}, {"\xE5\x80\xBC", Role::Value, 1},
        {"\xE5\x8F\x82\xE6\x95\xB0", Role::Value, 0.8f}, {"partvalue", Role::Value, 1}, {"compvalue", Role::Value, 1},
        {"componentvalue", Role::Value, 1},
        // description
        {"description", Role::Description, 1}, {"desc", Role::Description, 1}, {"partdescription", Role::Description, 1},
        {"descripcion", Role::Description, 1}, {"beschreibung", Role::Description, 1},
        {"bezeichnung", Role::Description, 0.9f}, {"designation", Role::Description, 0.8f},
        {"\xE6\x8F\x8F\xE8\xBF\xB0", Role::Description, 1}, {"\xE5\x90\x8D\xE7\xA7\xB0", Role::Description, 0.7f},
        {"\xE8\xA7\x84\xE6\xA0\xBC", Role::Description, 0.7f}, {"itemdescription", Role::Description, 1},
        {"longdescription", Role::Description, 1}, {"comments", Role::Description, 0.6f}, {"notes", Role::Description, 0.5f},
        {"remark", Role::Description, 0.5f}, {"remarks", Role::Description, 0.5f},
        // qty
        {"quantity", Role::Qty, 1}, {"qty", Role::Qty, 1}, {"count", Role::Qty, 0.9f}, {"quantityperboard", Role::Qty, 1},
        {"qtyper", Role::Qty, 1}, {"qtyperboard", Role::Qty, 1}, {"qtyperassembly", Role::Qty, 1}, {"qnty", Role::Qty, 1},
        {"anzahl", Role::Qty, 1}, {"menge", Role::Qty, 1}, {"stuck", Role::Qty, 1}, {"stuckzahl", Role::Qty, 1},
        {"quantite", Role::Qty, 1}, {"cantidad", Role::Qty, 1}, {"\xE6\x95\xB0\xE9\x87\x8F", Role::Qty, 1},
        {"\xE7\x94\xA8\xE9\x87\x8F", Role::Qty, 1}, {"pcs", Role::Qty, 0.8f}, {"amount", Role::Qty, 0.6f},
        {"numberofparts", Role::Qty, 1}, {"qtyea", Role::Qty, 1}, {"ea", Role::Qty, 0.5f},
        // manufacturer / MPN
        {"manufacturer", Role::Manufacturer, 1}, {"manufacturers", Role::Manufacturer, 1}, {"mfr", Role::Manufacturer, 1},
        {"mfg", Role::Manufacturer, 1}, {"mfgr", Role::Manufacturer, 1}, {"manufacturername", Role::Manufacturer, 1},
        {"manufacturer1", Role::Manufacturer, 1}, {"mfrname", Role::Manufacturer, 1}, {"mfgname", Role::Manufacturer, 1},
        {"hersteller", Role::Manufacturer, 1}, {"fabricant", Role::Manufacturer, 1}, {"fabricante", Role::Manufacturer, 1},
        {"\xE5\x88\xB6\xE9\x80\xA0\xE5\x95\x86", Role::Manufacturer, 1}, {"\xE5\x8E\x82\xE5\x95\x86", Role::Manufacturer, 1},
        {"\xE5\x8E\x82\xE5\xAE\xB6", Role::Manufacturer, 1}, {"\xE5\x93\x81\xE7\x89\x8C", Role::Manufacturer, 1},
        {"make", Role::Manufacturer, 0.7f}, {"brand", Role::Manufacturer, 0.8f}, {"cagecode", Role::Manufacturer, 0.4f},
        {"manufacturerpartnumber", Role::MPN, 1}, {"mpn", Role::MPN, 1}, {"mfrpartnumber", Role::MPN, 1},
        {"mfrpn", Role::MPN, 1}, {"mfgpn", Role::MPN, 1}, {"mfgpartnumber", Role::MPN, 1}, {"mfrpartno", Role::MPN, 1},
        {"mfrno", Role::MPN, 1}, {"mfgno", Role::MPN, 1}, {"manufacturerpartno", Role::MPN, 1}, {"manufacturerpn", Role::MPN, 1},
        {"manufacturerpart", Role::MPN, 1}, {"manufacturerpartnumber1", Role::MPN, 1}, {"mfrpart", Role::MPN, 1},
        {"herstellerteilenummer", Role::MPN, 1}, {"herstellernummer", Role::MPN, 1}, {"herstellerartikelnummer", Role::MPN, 1},
        {"herstellerbezeichnung", Role::MPN, 0.9f}, {"\xE5\x88\xB6\xE9\x80\xA0\xE5\x95\x86\xE6\x96\x99\xE5\x8F\xB7", Role::MPN, 1},
        {"\xE5\x8E\x9F\xE5\x8E\x82\xE5\x9E\x8B\xE5\x8F\xB7", Role::MPN, 1}, {"\xE5\x9E\x8B\xE5\x8F\xB7", Role::MPN, 0.8f},
        {"partnumbermfr", Role::MPN, 1}, {"mfgpartno", Role::MPN, 1}, {"manfpn", Role::MPN, 1},
        {"manfpartnumber", Role::MPN, 1}, {"manufacturerspartnumber", Role::MPN, 1}, {"mfrpno", Role::MPN, 1},
        {"referencefabricant", Role::MPN, 1}, {"numerodepartedelfabricante", Role::MPN, 1},
        // vendor
        {"vendor", Role::Vendor, 1}, {"supplier", Role::Vendor, 1}, {"distributor", Role::Vendor, 1},
        {"supplier1", Role::Vendor, 1}, {"lieferant", Role::Vendor, 1}, {"fournisseur", Role::Vendor, 1},
        {"proveedor", Role::Vendor, 1}, {"\xE4\xBE\x9B\xE5\xBA\x94\xE5\x95\x86", Role::Vendor, 1},
        {"vendorpartnumber", Role::VendorPN, 1}, {"vendorpn", Role::VendorPN, 1}, {"vendorpartno", Role::VendorPN, 1},
        {"supplierpartnumber", Role::VendorPN, 1}, {"supplierpn", Role::VendorPN, 1}, {"supplierpart", Role::VendorPN, 1},
        {"supplierpartno", Role::VendorPN, 1}, {"supplierpartnumber1", Role::VendorPN, 1},
        {"digikeypartnumber", Role::VendorPN, 1}, {"digikeypn", Role::VendorPN, 1}, {"digikey", Role::VendorPN, 0.9f},
        {"digikeyno", Role::VendorPN, 1}, {"mouserpartnumber", Role::VendorPN, 1}, {"mouserpn", Role::VendorPN, 1},
        {"mouserno", Role::VendorPN, 1}, {"mouser", Role::VendorPN, 0.9f}, {"lcsc", Role::VendorPN, 1},
        {"lcscpart", Role::VendorPN, 1}, {"lcscpartno", Role::VendorPN, 1}, {"lcscpartnumber", Role::VendorPN, 1},
        {"jlcpcbpart", Role::VendorPN, 1}, {"jlcpcbpartno", Role::VendorPN, 1}, {"jlcpcbpartnumber", Role::VendorPN, 1},
        {"farnell", Role::VendorPN, 0.9f}, {"farnellpn", Role::VendorPN, 1}, {"rspn", Role::VendorPN, 1},
        {"ordercode", Role::VendorPN, 1}, {"ordernumber", Role::VendorPN, 0.9f}, {"bestellnummer", Role::VendorPN, 1},
        {"bestellnr", Role::VendorPN, 1}, {"spn", Role::VendorPN, 1}, {"distributorpartnumber", Role::VendorPN, 1},
        {"distributorpn", Role::VendorPN, 1}, {"vpn", Role::VendorPN, 0.9f},
        // DNP / fitted
        {"dnp", Role::Dnp, 1}, {"dni", Role::Dnp, 1}, {"dnf", Role::Dnp, 1}, {"donotpopulate", Role::Dnp, 1},
        {"donotplace", Role::Dnp, 1}, {"donotfit", Role::Dnp, 1}, {"donotinstall", Role::Dnp, 1}, {"nopop", Role::Dnp, 1},
        {"nopopulate", Role::Dnp, 1}, {"notfitted", Role::Dnp, 1}, {"notplaced", Role::Dnp, 1}, {"nofit", Role::Dnp, 1},
        {"dnpdni", Role::Dnp, 1}, {"exclude", Role::Dnp, 0.8f}, {"excludefrombom", Role::Dnp, 0.9f},
        {"fitted", Role::Dnp, 1, true}, {"populate", Role::Dnp, 1, true}, {"populated", Role::Dnp, 1, true},
        {"fit", Role::Dnp, 0.9f, true}, {"place", Role::Dnp, 0.8f, true}, {"placed", Role::Dnp, 0.8f, true},
        {"install", Role::Dnp, 0.9f, true}, {"installed", Role::Dnp, 0.9f, true}, {"bestuckt", Role::Dnp, 1, true},
        {"bestuckung", Role::Dnp, 0.9f, true}, {"stuff", Role::Dnp, 0.8f, true}, {"assemble", Role::Dnp, 0.8f, true},
        {"variant", Role::Dnp, 0.4f},
        // process
        {"process", Role::Process, 1}, {"processtype", Role::Process, 1}, {"mounttype", Role::Process, 1},
        {"mountingtype", Role::Process, 1}, {"technology", Role::Process, 0.9f}, {"tech", Role::Process, 0.8f},
        {"smdtht", Role::Process, 1}, {"smttht", Role::Process, 1}, {"smdthd", Role::Process, 1},
        {"assemblytype", Role::Process, 1}, {"assytype", Role::Process, 1}, {"mounting", Role::Process, 0.8f},
        {"montageart", Role::Process, 1}, {"\xE5\xB7\xA5\xE8\x89\xBA", Role::Process, 1}, {"smt", Role::Process, 0.6f},
        // misc BOM
        {"polarity", Role::Polarity, 1}, {"polarized", Role::Polarity, 1}, {"pol", Role::Polarity, 0.9f},
        {"polar", Role::Polarity, 0.9f}, {"polaritat", Role::Polarity, 1}, {"polarite", Role::Polarity, 1},
        {"height", Role::Height, 1}, {"bodyheight", Role::Height, 1}, {"compheight", Role::Height, 1},
        {"hohe", Role::Height, 1}, {"maxheight", Role::Height, 1}, {"\xE9\xAB\x98\xE5\xBA\xA6", Role::Height, 1},
        {"z", Role::Height, 0.6f}, {"hauteur", Role::Height, 1}, {"altura", Role::Height, 1},
        {"cost", Role::Cost, 1}, {"price", Role::Cost, 1}, {"unitprice", Role::Cost, 1}, {"unitcost", Role::Cost, 1},
        {"preis", Role::Cost, 1}, {"prix", Role::Cost, 1}, {"precio", Role::Cost, 1}, {"\xE5\x8D\x95\xE4\xBB\xB7", Role::Cost, 1},
        {"\xE4\xBB\xB7\xE6\xA0\xBC", Role::Cost, 1}, {"costeach", Role::Cost, 1}, {"extprice", Role::Cost, 0.6f},
        {"softwarerev", Role::SoftRev, 1}, {"sofwarerev", Role::SoftRev, 1}, {"softrev", Role::SoftRev, 1},
        {"softwareversion", Role::SoftRev, 1}, {"swrev", Role::SoftRev, 1}, {"firmware", Role::SoftRev, 0.9f},
        {"fwrev", Role::SoftRev, 1}, {"softver", Role::SoftRev, 1}, {"firmwareversion", Role::SoftRev, 1},
        {"partrev", Role::PartRev, 1}, {"revision", Role::PartRev, 0.8f}, {"rev", Role::PartRev, 0.8f},
        {"partrevision", Role::PartRev, 1}, {"version", Role::PartRev, 0.5f},
        {"stockcode", Role::StockCode, 1}, {"stocknumber", Role::StockCode, 1}, {"sku", Role::StockCode, 1},
        {"stockno", Role::StockCode, 1}, {"internalsku", Role::StockCode, 1},
        {"bin", Role::Bin, 1}, {"binlocation", Role::Bin, 1}, {"storage", Role::Bin, 0.8f},
        {"storagelocation", Role::Bin, 1}, {"lagerort", Role::Bin, 1}, {"lagerplatz", Role::Bin, 1},
        {"itemid", Role::LineItem, 0.9f}, {"bomitem", Role::LineItem, 1}, {"bomline", Role::LineItem, 1},
        {"option", Role::Dnp, 0.8f}, {"assemblyoption", Role::Dnp, 0.9f}, {"assyoption", Role::Dnp, 0.9f}, {"dnpoption", Role::Dnp, 1},
        {"dns", Role::Dnp, 1}, {"donotstuff", Role::Dnp, 1}, {"nostuff", Role::Dnp, 1},
        {"mountingprocess", Role::Process, 1}, {"assemblyprocess", Role::Process, 1},
        {"internalpartnumber", Role::PartNumber, 1}, {"internalpn", Role::PartNumber, 1}, {"ourpartnumber", Role::PartNumber, 1},
        {"lineitem", Role::LineItem, 1}, {"line", Role::LineItem, 0.7f}, {"no", Role::LineItem, 0.8f},
        {"nr", Role::LineItem, 0.8f}, {"pos", Role::LineItem, 0.7f}, {"lfdnr", Role::LineItem, 1},
        {"findnumber", Role::LineItem, 1}, {"findno", Role::LineItem, 1}, {"\xE5\xBA\x8F\xE5\x8F\xB7", Role::LineItem, 1},
        {"positionnumber", Role::LineItem, 1}, {"posnr", Role::LineItem, 1}, {"zeile", Role::LineItem, 0.8f},
        {"index", Role::LineItem, 0.6f}, {"seq", Role::LineItem, 0.7f}, {"sequence", Role::LineItem, 0.7f},
        {"poptype", Role::PopType, 1}, {"populationtype", Role::PopType, 1},
        {"partmarking", Role::PartMarking, 1}, {"marking", Role::PartMarking, 0.9f}, {"topmark", Role::PartMarking, 1},
        {"markingcode", Role::PartMarking, 1},
        {"socket", Role::Socket, 1}, {"socketed", Role::Socket, 1},
        {"unit", Role::Unit, 0.8f}, {"units", Role::Unit, 0.8f}, {"einheit", Role::Unit, 0.8f},
    };
    return v;
}

// Ambiguous keys resolved by values (first role = default when values decide nothing).
struct Amb { const char* key; std::vector<Role> roles; };
const std::vector<Amb>& ambs() {
    static const std::vector<Amb> v = {
        {"part", {Role::PartNumber, Role::Ref}}, {"parts", {Role::Ref, Role::PartNumber}},
        {"name", {Role::Value, Role::Ref}}, {"location", {Role::Bin, Role::Ref}},
        {"locations", {Role::Ref, Role::Bin}}, {"position", {Role::LineItem, Role::Ref}},
        {"item", {Role::LineItem, Role::Ref, Role::PartNumber}}, {"component", {Role::PartNumber, Role::Ref}},
        {"components", {Role::Ref, Role::PartNumber}}, {"comp", {Role::PartNumber, Role::Ref}},
        {"bauteil", {Role::PartNumber, Role::Ref}}, {"bauteile", {Role::Ref, Role::PartNumber}},
        {"type", {Role::Package, Role::Process, Role::PartNumber}}, {"typ", {Role::Package, Role::Process, Role::PartNumber}},
        {"id", {Role::LineItem, Role::Ref}}, {"label", {Role::Ref, Role::Description}},
    };
    return v;
}

const Syn* find_syn(const std::string& k) {
    for (auto& s : syns()) if (k == s.key) return &s;
    return nullptr;
}
const Amb* find_amb(const std::string& k) {
    for (auto& a : ambs()) if (k == a.key) return &a;
    return nullptr;
}

} // namespace

static LenUnit unit_in_header(const std::string& h) {
    std::string l = to_lower(h);
    // "(mm)", "[mil]", "x mm", "x_mm", "centerxmm"
    std::string w;
    std::vector<std::string> words;
    for (char c : l) {
        if (std::isalnum((unsigned char)c) || c == '"') w += c;
        else { if (!w.empty()) words.push_back(w); w.clear(); }
    }
    if (!w.empty()) words.push_back(w);
    for (size_t i = 1; i < words.size(); ++i) {
        LenUnit u = len_unit_from_word(words[i]);
        if (u != LenUnit::Auto && words[i] != "in") return u;
        if (words[i] == "in" && i + 1 == words.size()) return LenUnit::Inch;
    }
    // glued suffix
    std::string k = norm_header(h);
    static const std::pair<const char*, LenUnit> suf[] = {{"mils", LenUnit::Mil}, {"mil", LenUnit::Mil},
                                                          {"mm", LenUnit::MM}, {"inch", LenUnit::Inch},
                                                          {"thou", LenUnit::Mil}};
    for (auto& [s, u] : suf) {
        size_t n = std::char_traits<char>::length(s);
        if (k.size() > n && k.compare(k.size() - n, n, s) == 0) {
            std::string base = k.substr(0, k.size() - n);
            if (auto sy = find_syn(base); sy && (sy->role == Role::X || sy->role == Role::Y || sy->role == Role::Height))
                return u;
        }
    }
    return LenUnit::Auto;
}

Role role_from_header(const std::string& header, Purpose purpose, double* conf, LenUnit* unit, bool* invert) {
    if (conf) *conf = 0;
    if (invert) *invert = false;
    if (unit) *unit = unit_in_header(header);
    std::string k = norm_header(header);
    if (k.empty()) return Role::Unused;
    auto hit = [&](const Syn* s, double c) {
        if (conf) *conf = c;
        if (invert) *invert = s->invert;
        return s->role;
    };
    if (auto s = find_syn(k)) return hit(s, s->conf);
    if (auto a = find_amb(k)) {
        if (conf) *conf = 0.5;
        Role r = a->roles[0];
        if (purpose == Purpose::Pnp && (k == "name" || k == "part" || k == "comp" || k == "component")) r = Role::Ref;
        return r;
    }
    // strip glued unit suffix ("centerxmm", "posxmil")
    for (const char* su : {"mils", "mil", "mm", "inch", "in", "deg", "degrees"}) {
        size_t n = std::char_traits<char>::length(su);
        if (k.size() > n && k.compare(k.size() - n, n, su) == 0)
            if (auto s = find_syn(k.substr(0, k.size() - n))) return hit(s, s->conf * 0.95);
    }
    // contains: longest synonym (>= 5 chars) inside the key
    const Syn* best = nullptr;
    size_t bl = 0;
    for (auto& s : syns()) {
        size_t n = std::char_traits<char>::length(s.key);
        if (n < 5 || n <= bl) continue;
        if (k.find(s.key) != std::string::npos) { best = &s; bl = n; }
    }
    if (best) return hit(best, std::min(0.9, (0.5 + 0.4 * (double)bl / k.size()) * best->conf));
    return Role::Unused;
}

static bool is_amb_header(const std::string& h) { return find_amb(norm_header(h)) != nullptr; }

// ===========================================================================
// Splitting
// ===========================================================================
std::vector<std::string> split_delim(const std::string& line, char sep, char quote) {
    std::vector<std::string> out;
    std::string cur;
    bool inq = false, field_start = true;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (inq) {
            if (c == quote) {
                if (i + 1 < line.size() && line[i + 1] == quote) { cur += quote; ++i; }
                else inq = false;
            } else cur += c;
            continue;
        }
        if (c == sep) { out.push_back(cur); cur.clear(); field_start = true; continue; }
        if (quote && c == quote && field_start && trim(cur).empty()) { cur.clear(); inq = true; field_start = false; continue; }
        if (c != ' ') field_start = false;
        cur += c;
    }
    out.push_back(cur);
    return out;
}

// length of the delimiter starting at s[i] (0 = none)
static size_t delim_at(const std::string& s, size_t i, const std::string& delims, const std::string& other) {
    if (!other.empty() && s.compare(i, other.size(), other) == 0) return other.size();
    if (delims.find(s[i]) != std::string::npos) return 1;
    return 0;
}

std::vector<std::string> split_multi(const std::string& line, const std::string& delims, const std::string& other,
                                     char quote, bool consecutive) {
    std::vector<std::string> out;
    std::string cur;
    bool inq = false, fs = true, quoted = false;
    size_t i = 0;
    if (consecutive) while (i < line.size()) { size_t k = delim_at(line, i, delims, other); if (!k) break; i += k; }
    while (i < line.size()) {
        char c = line[i];
        if (inq) {
            if (c == quote) {
                if (i + 1 < line.size() && line[i + 1] == quote) { cur += quote; i += 2; continue; }
                inq = false;
            } else cur += c;
            ++i;
            continue;
        }
        if (size_t k = delim_at(line, i, delims, other)) {
            out.push_back(cur); cur.clear(); fs = true; quoted = false;
            i += k;
            if (consecutive) while (i < line.size()) { size_t k2 = delim_at(line, i, delims, other); if (!k2) break; i += k2; }
            continue;
        }
        if (quote && c == quote && fs && trim(cur).empty()) { cur.clear(); inq = true; quoted = true; fs = false; ++i; continue; }
        if (c != ' ') fs = false;
        cur += c;
        ++i;
    }
    if (!(consecutive && cur.empty() && !quoted && !out.empty())) out.push_back(cur);
    return out;
}

std::string normalize_numeric_cell(const std::string& s0, char thousands, bool dc, bool trailing_minus, bool negate) {
    std::string s = trim(s0);
    if (s.empty()) return s0;
    bool neg = false;
    std::string b = s;
    if (trailing_minus && b.size() > 1 && b.back() == '-' && b[b.size() - 2] != '-') { b.pop_back(); b = trim(b); neg = true; }
    if (!b.empty() && (b[0] == '-' || b[0] == '+')) { if (b[0] == '-') neg = !neg; b.erase(0, 1); }
    if (b.size() >= 2 && b.front() == '(' && b.back() == ')') { neg = !neg; b = b.substr(1, b.size() - 2); }   // accounting negative
    // digits with separators, then optional unit / degree suffix
    size_t e = 0;
    while (e < b.size() && (is_digit(b[e]) || b[e] == '.' || b[e] == ',' || b[e] == ' ' || b[e] == '\'')) ++e;
    std::string num = trim(b.substr(0, e)), suf = b.substr(e);
    if (num.empty() || !is_digit(num[0])) return s0;
    if (thousands) {
        char dec = dc ? ',' : '.';
        if (thousands != dec) {
            std::string n2;
            for (char c : num) if (c != thousands) n2 += c;
            num = n2;
        }
    }
    if (num.find(' ') != std::string::npos) return s0;     // "12 34": not one number
    if (!suf.empty() && suf.find_first_of("0123456789") != std::string::npos) return s0;
    if (negate) neg = !neg;
    bool zero = num.find_first_not_of("0.,") == std::string::npos;
    return (neg && !zero ? "-" : "") + num + suf;
}

static std::string expand_tabs(const std::string& s, int tab, int replace) {
    if (s.find('\t') == std::string::npos) return s;
    std::string o;
    for (char c : s) {
        if (c == '\t') {
            int n = replace > 0 ? replace : (tab - (int)(o.size() % tab));
            o.append(n, ' ');
        } else o += c;
    }
    return o;
}

static std::string clean_cell(std::string c, bool do_trim) {
    if (do_trim) c = trim(c);
    // Excel text-protection ="0805"
    if (c.size() >= 3 && c[0] == '=' && c[1] == '"' && c.back() == '"') c = c.substr(2, c.size() - 3);
    if (c.size() >= 2 && c[0] == '\'' && is_digit(c[1]) && c.find('\'', 1) == std::string::npos) c.erase(0, 1);
    return c;
}

static std::vector<std::string> split_ws(const std::string& line, bool two_plus, char quote) {
    std::vector<std::string> out;
    size_t i = 0, n = line.size();
    while (i < n) {
        while (i < n && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= n) break;
        std::string cur;
        if (quote && line[i] == quote) {
            ++i;
            while (i < n && line[i] != quote) cur += line[i++];
            if (i < n) ++i;
            out.push_back(cur);
            continue;
        }
        while (i < n) {
            if (line[i] == '\t') break;
            if (line[i] == ' ') {
                if (!two_plus) break;
                if (i + 1 < n && (line[i + 1] == ' ' || line[i + 1] == '\t')) break;
                if (i + 1 >= n) break;
            }
            cur += line[i++];
        }
        out.push_back(cur);
    }
    return out;
}

// ===========================================================================
// Analysis
// ===========================================================================
namespace {

struct Rec {
    int line = 0, nlines = 1;
    std::vector<std::string> cells;
    bool comment = false, blank = false;
    std::string raw;            // first raw line (tab-expanded for fixed)
};

bool starts_with_comment(const std::string& l, const std::vector<std::string>& pre, size_t* plen) {
    size_t i = 0;
    while (i < l.size() && (l[i] == ' ' || l[i] == '\t')) ++i;
    for (auto& p : pre) {
        if (p.empty()) continue;
        if (l.compare(i, p.size(), p) == 0) {
            if (plen) *plen = i + p.size();
            return true;
        }
    }
    return false;
}

// Parse delimited records starting at raw line `start` (quote-aware, multi-line cells).
std::vector<Rec> parse_delimited(const std::vector<std::string>& L, int start, char sep, char quote,
                                 const std::vector<std::string>& cpre, bool do_trim) {
    std::vector<Rec> out;
    int i = start;
    const int n = (int)L.size();
    while (i < n) {
        Rec r;
        r.line = i;
        r.raw = L[i];
        if (trim(L[i]).empty()) { r.blank = true; out.push_back(r); ++i; continue; }
        size_t plen = 0;
        if (starts_with_comment(L[i], cpre, &plen)) {
            r.comment = true;
            for (auto& c : split_delim(L[i].substr(plen), sep, quote)) r.cells.push_back(clean_cell(c, do_trim));
            out.push_back(r);
            ++i;
            continue;
        }
        // state machine across lines
        std::vector<std::string> cells;
        std::string cur;
        bool inq = false, fs = true, ok = true;
        int li = i;
        size_t ci = 0;
        while (true) {
            const std::string& s = L[li];
            if (ci >= s.size()) {
                if (inq) {
                    if (li + 1 < n && li - i < 200) { cur += '\n'; ++li; ci = 0; continue; }
                    ok = false;
                }
                break;
            }
            char c = s[ci++];
            if (inq) {
                if (c == quote) {
                    if (ci < s.size() && s[ci] == quote) { cur += quote; ++ci; }
                    else inq = false;
                } else cur += c;
                continue;
            }
            if (c == sep) { cells.push_back(cur); cur.clear(); fs = true; continue; }
            if (quote && c == quote && fs && trim(cur).empty()) { cur.clear(); inq = true; fs = false; continue; }
            if (c != ' ') fs = false;
            cur += c;
        }
        if (!ok) {   // unterminated quote: take the line literally
            cells.clear();
            for (auto& c : split_delim(L[i], sep, 0)) cells.push_back(c);
            li = i;
        } else cells.push_back(cur);
        for (auto& c : cells) r.cells.push_back(clean_cell(c, do_trim));
        r.nlines = li - i + 1;
        out.push_back(r);
        i = li + 1;
    }
    return out;
}

// Explicit delimiter set (any char of `delims` or the string `other`), quote-aware incl. multi-line cells.
std::vector<Rec> parse_multi(const std::vector<std::string>& L, int start, const std::string& delims, const std::string& other,
                             char quote, bool consecutive, const std::vector<std::string>& cpre, bool do_trim) {
    std::vector<Rec> out;
    const int n = (int)L.size();
    int i = start;
    while (i < n) {
        Rec r;
        r.line = i;
        r.raw = L[i];
        if (trim(L[i]).empty()) { r.blank = true; out.push_back(r); ++i; continue; }
        size_t plen = 0;
        if (starts_with_comment(L[i], cpre, &plen)) {
            r.comment = true;
            for (auto& c : split_multi(L[i].substr(plen), delims, other, quote, consecutive)) r.cells.push_back(clean_cell(c, do_trim));
            out.push_back(r);
            ++i;
            continue;
        }
        // join physical lines while a quote is open
        int li = i;
        std::string rec = L[i];
        auto open_quote = [&](const std::string& s) {
            if (!quote) return false;
            bool inq = false, fs = true;
            for (size_t k = 0; k < s.size(); ++k) {
                char c = s[k];
                if (inq) { if (c == quote) { if (k + 1 < s.size() && s[k + 1] == quote) ++k; else inq = false; } continue; }
                if (size_t d = delim_at(s, k, delims, other)) { fs = true; k += d - 1; continue; }
                if (c == quote && fs) { inq = true; continue; }
                if (c != ' ') fs = false;
            }
            return inq;
        };
        while (open_quote(rec) && li + 1 < n && li - i < 200) rec += "\n" + L[++li];
        if (open_quote(rec)) { rec = L[i]; li = i; }
        auto cells = split_multi(rec, delims, other, open_quote(rec) ? 0 : quote, consecutive);
        for (auto& c : cells) r.cells.push_back(clean_cell(c, do_trim));
        r.nlines = li - i + 1;
        out.push_back(r);
        i = li + 1;
    }
    return out;
}

int mode_count(const std::vector<Rec>& recs, double* consistency, int* nrec) {
    std::map<int, int> h;
    int tot = 0;
    for (auto& r : recs) {
        if (r.blank || r.comment) continue;
        // ignore trailing empty cells (Excel pads rows with ",,,")
        int k = (int)r.cells.size();
        ++h[k];
        ++tot;
    }
    int best = 0, bc = 0;
    for (auto& [k, c] : h) if (k >= 2 && (c > bc || (c == bc && k > best))) { best = k; bc = c; }
    // consistency measured from the first record having the mode count (preamble excluded)
    int from = 0;
    bool started = false;
    for (auto& r : recs) {
        if (r.blank || r.comment) continue;
        if (!started && (int)r.cells.size() == best) started = true;
        if (started) ++from;
    }
    if (bc < 2 && tot > 1) best = 0;
    if (consistency) *consistency = from ? (double)bc / from : 0;
    if (nrec) *nrec = tot;
    return best;
}

bool cell_numeric(const std::string& c) {
    VType t = classify_cell(c, true);
    return t == VType::Integer || t == VType::Number || t == VType::Length || t == VType::Angle;
}

bool line_datalike(const std::string& l) {
    auto t = split_ws(l, false, '"');
    if (t.size() < 2) return false;
    int num = 0;
    for (auto& x : t) num += cell_numeric(x);
    if (is_refdes(t[0]) || is_ref_list(t[0]) || cell_numeric(t[0])) return true;
    return num >= 2 && num * 2 >= (int)t.size();
}

// Fixed-width cut detection on the given lines (already tab-expanded).
std::vector<int> detect_cuts(const std::vector<std::string>& lines) {
    size_t w = 0;
    for (auto& l : lines) w = std::max(w, l.size());
    if (w == 0 || lines.empty()) return {};
    std::vector<int> occ(w + 1, 0);
    for (auto& l : lines)
        for (size_t i = 0; i < l.size(); ++i) if (l[i] != ' ') ++occ[i];
    int tol = (int)(lines.size() * 0.04);
    std::vector<int> cuts;
    bool in = false;
    for (size_t i = 0; i < w; ++i) {
        bool filled = occ[i] > tol;
        if (filled && !in) cuts.push_back((int)i);
        in = filled;
    }
    return cuts;
}

// fraction of lines in which a cut splits a word
double cut_damage(const std::vector<std::string>& lines, const std::vector<int>& cuts) {
    if (lines.empty()) return 1;
    int bad = 0;
    for (auto& l : lines) {
        bool b = false;
        for (size_t k = 1; k < cuts.size(); ++k) {
            size_t c = (size_t)cuts[k];
            if (c < l.size() && c > 0 && l[c] != ' ' && l[c - 1] != ' ') { b = true; break; }
        }
        bad += b;
    }
    return (double)bad / lines.size();
}

std::vector<std::string> slice_fixed(const std::string& l, const std::vector<int>& cuts, bool do_trim) {
    std::vector<std::string> out;
    for (size_t k = 0; k < cuts.size(); ++k) {
        size_t a = (size_t)cuts[k];
        size_t b = k + 1 < cuts.size() ? (size_t)cuts[k + 1] : std::string::npos;
        std::string c = a < l.size() ? l.substr(a, b == std::string::npos ? std::string::npos : b - a) : std::string();
        out.push_back(clean_cell(c, do_trim));
    }
    // text before the first cut belongs to column 0
    if (!cuts.empty() && cuts[0] > 0 && !out.empty()) {
        std::string pre = trim(l.substr(0, std::min<size_t>(l.size(), cuts[0])));
        if (!pre.empty()) out[0] = trim(pre + " " + out[0]);
    }
    return out;
}

} // namespace

std::vector<int> suggest_fixed_cuts(const std::vector<std::string>& lines0, int tab_size) {
    std::vector<std::string> body, dl;
    for (auto& l0 : lines0) {
        std::string l = expand_tabs(l0, tab_size > 0 ? tab_size : 8, 0);
        if (trim(l).empty()) continue;
        body.push_back(l);
        if (line_datalike(l)) dl.push_back(l);
    }
    if (dl.size() < 2) dl = body;
    auto cuts = detect_cuts(dl);
    if (!cuts.empty()) cuts[0] = 0;
    return cuts;
}

namespace {

// Header phrases (split on 2+ blanks) assigned to fixed columns: monotone
// matching against the data extent of each column (DP maximising overlap
// minus distance), falling back to word midpoints when there are more
// phrases than columns. Keeps "Part Number" together and right-aligned
// numeric columns get the header that sits left of their data.
std::vector<std::string> header_fixed(const std::string& l, const std::vector<int>& cuts,
                                      const std::vector<std::pair<int, int>>& ext) {
    const int n = (int)cuts.size();
    std::vector<std::string> out(n);
    struct Ph { int a, b; std::string s; };
    std::vector<Ph> ph;
    size_t i = 0;
    while (i < l.size()) {
        while (i < l.size() && l[i] == ' ') ++i;
        if (i >= l.size()) break;
        size_t a = i;
        while (i < l.size() && !(l[i] == ' ' && (i + 1 >= l.size() || l[i + 1] == ' '))) ++i;
        ph.push_back({(int)a, (int)i, l.substr(a, i - a)});
    }
    auto colspan0 = [&](int c) -> std::pair<int, int> {
        if (c < (int)ext.size() && ext[c].first <= ext[c].second) return ext[c];
        return {cuts[c], c + 1 < n ? cuts[c + 1] : cuts[c] + 8};
    };
    if ((int)ph.size() < n) {
        // split phrases that cover the data of several columns into words
        std::vector<Ph> sp;
        for (auto& x : ph) {
            int cover = 0;
            for (int c = 0; c < n; ++c) {
                auto [ca, cb] = colspan0(c);
                if (std::min(x.b, cb) - std::max(x.a, ca) > 0) ++cover;
            }
            if (cover < 2) { sp.push_back(x); continue; }
            size_t q = (size_t)x.a;
            while (q < (size_t)x.b) {
                while (q < (size_t)x.b && l[q] == ' ') ++q;
                size_t a = q;
                while (q < (size_t)x.b && l[q] != ' ') ++q;
                if (q > a) sp.push_back({(int)a, (int)q, l.substr(a, q - a)});
            }
        }
        ph.swap(sp);
    }
    const int k = (int)ph.size();
    auto colspan = [&](int c) -> std::pair<int, int> {
        if (c < (int)ext.size() && ext[c].first <= ext[c].second) return ext[c];
        return {cuts[c], c + 1 < n ? cuts[c + 1] : cuts[c] + 8};
    };
    auto score = [&](int p, int c) {
        auto [ca, cb] = colspan(c);
        int ov = std::min(ph[p].b, cb) - std::max(ph[p].a, ca);
        return ov > 0 ? (double)ov : (double)ov * 1.01;   // ov<0 => -distance
    };
    if (k == 0) return out;
    if (k <= n) {
        const double NEG = -1e18;
        std::vector<std::vector<double>> dp(k + 1, std::vector<double>(n + 1, NEG));
        std::vector<std::vector<int>> take(k + 1, std::vector<int>(n + 1, 0));
        for (int c = 0; c <= n; ++c) dp[0][c] = 0;
        for (int p = 1; p <= k; ++p)
            for (int c = 1; c <= n; ++c) {
                double skip = dp[p][c - 1];
                double use = dp[p - 1][c - 1] > NEG ? dp[p - 1][c - 1] + score(p - 1, c - 1) : NEG;
                if (use >= skip && use > NEG) { dp[p][c] = use; take[p][c] = 1; }
                else dp[p][c] = skip;
            }
        int p = k, c = n;
        while (p > 0 && c > 0) {
            if (take[p][c]) { out[c - 1] = ph[p - 1].s; --p; --c; }
            else --c;
        }
        return out;
    }
    // more phrases than columns: monotone assignment with sharing
    const double NEG = -1e18, bonus = 0.5;
    std::vector<std::vector<double>> dp(k, std::vector<double>(n, NEG));
    std::vector<std::vector<int>> from(k, std::vector<int>(n, -1));
    for (int c = 0; c < n; ++c) dp[0][c] = score(0, c) + bonus;
    for (int p = 1; p < k; ++p) {
        double best = NEG;
        int bc = -1;
        for (int c = 0; c < n; ++c) {
            double share = dp[p - 1][c];
            double fresh = best > NEG ? best + bonus : NEG;
            if (share >= fresh && share > NEG) { dp[p][c] = share + score(p, c); from[p][c] = c; }
            else if (fresh > NEG) { dp[p][c] = fresh + score(p, c); from[p][c] = bc; }
            if (dp[p - 1][c] > best) { best = dp[p - 1][c]; bc = c; }
        }
    }
    int c = 0;
    for (int q = 1; q < n; ++q) if (dp[k - 1][q] > dp[k - 1][c]) c = q;
    std::vector<int> asg(k);
    for (int p = k - 1; p >= 0; --p) { asg[p] = c; if (p) c = from[p][c]; }
    for (int p = 0; p < k; ++p) {
        if (!out[asg[p]].empty()) out[asg[p]] += ' ';
        out[asg[p]] += ph[p].s;
    }
    return out;
}

bool is_unit_cell(const std::string& c) {
    std::string k = to_lower(trim(c));
    while (!k.empty() && (k.front() == '(' || k.front() == '[')) k.erase(0, 1);
    while (!k.empty() && (k.back() == ')' || k.back() == ']')) k.pop_back();
    return len_unit_from_word(k) != LenUnit::Auto || k == "deg" || k == "degrees" || k == "\xC2\xB0" || k == "um";
}

double header_score(const std::vector<std::string>& cells, int* numeric) {
    double s = 0;
    int num = 0;
    std::set<Role> seen;
    for (auto& c : cells) {
        if (trim(c).empty()) continue;
        if (cell_numeric(c)) { ++num; continue; }
        double cf = 0;
        Role r = role_from_header(c, Purpose::Generic, &cf);
        if (r != Role::Unused && cf >= 0.6 && !seen.count(r)) { s += 1; seen.insert(r); }
        else if (is_amb_header(c)) s += 0.5;
    }
    if (numeric) *numeric = num;
    return s;
}

bool is_summary_text(const std::string& row_text) {
    std::string l = to_lower(row_text);
    static const char* kw[] = {"total", "summe", "gesamt", "sum:", "subtotal", "grand total", "end of", "count:",
                               "\xE5\x90\x88\xE8\xAE\xA1", "\xE6\x80\xBB\xE8\xAE\xA1", "page ", "printed", "generated"};
    for (auto k : kw) if (l.find(k) != std::string::npos) return true;
    return false;
}

LenUnit unit_from_meta(const std::vector<std::string>& meta) {
    for (auto& m : meta) {
        std::string l = to_lower(m);
        size_t p = l.find("unit");
        if (p == std::string::npos) p = l.find("einheit");
        if (p == std::string::npos) continue;
        std::string w;
        std::vector<std::string> words;
        for (size_t i = p; i < l.size(); ++i) {
            char c = l[i];
            if (std::isalnum((unsigned char)c)) w += c;
            else { if (!w.empty()) words.push_back(w); w.clear(); }
        }
        if (!w.empty()) words.push_back(w);
        for (size_t i = 1; i < words.size() && i < 4; ++i) {
            LenUnit u = len_unit_from_word(words[i]);
            if (u != LenUnit::Auto) return u;
        }
    }
    return LenUnit::Auto;
}

} // namespace

int ParsedText::data_rows() const {
    int n = 0;
    for (auto& r : rows) n += r.kind == RowKind::Data;
    return n;
}
int ParsedText::col(Role r) const {
    for (size_t i = 0; i < cols.size(); ++i) if (cols[i].role == r) return (int)i;
    return -1;
}
std::vector<int> ParsedText::cols_with(Role r) const {
    std::vector<int> v;
    for (size_t i = 0; i < cols.size(); ++i) if (cols[i].role == r) v.push_back((int)i);
    return v;
}
std::string ParsedText::cell(const Row& row, Role r) const {
    int c = col(r);
    return (c >= 0 && c < (int)row.cells.size()) ? row.cells[c] : std::string();
}

std::string ParsedText::describe() const {
    std::ostringstream o;
    o << encoding_name(encoding) << ", " << sep_name(sep);
    if (sep_char && sep != Sep::Fixed && sep != Sep::Spaces && sep != Sep::Whitespace) o << " '" << (sep_char == '\t' ? std::string("\\t") : std::string(1, sep_char)) << "'";
    if (sep == Sep::Fixed) {
        o << " cuts";
        for (int c : cuts) o << ' ' << c;
    }
    if (decimal_comma) o << ", decimal comma";
    o << ", " << cols.size() << " cols, " << data_rows() << " rows";
    if (header_line >= 0) o << ", header line " << header_line + 1 << (header_rows > 1 ? " (+" + std::to_string(header_rows - 1) + ")" : "") << (header_commented ? " (commented)" : "");
    o << "\n";
    for (size_t i = 0; i < cols.size(); ++i) {
        auto& c = cols[i];
        o << "  [" << i << "] \"" << c.header << "\" " << vtype_name(c.type) << " -> " << role_name(c.role);
        if (c.invert) o << " (inverted)";
        if (c.unit != LenUnit::Auto) o << " [" << len_unit_name(c.unit) << "]";
        if (!c.samples.empty()) o << "  e.g. " << c.samples[0];
        o << "\n";
    }
    for (auto& n : notes) o << "  note: " << n << "\n";
    return o.str();
}

// ---------------------------------------------------------------------------
// Role assignment
// ---------------------------------------------------------------------------
static bool numeric_type(VType t) {
    return t == VType::Integer || t == VType::Number || t == VType::Length || t == VType::Angle;
}

void assign_roles(ParsedText& t, const ImportLayout& lay, Purpose purpose) {
    const int nc = (int)t.cols.size();
    struct Cand { Role r = Role::Unused; double conf = 0; RoleSource src = RoleSource::None; bool inv = false; };
    std::vector<Cand> cand(nc);
    for (int i = 0; i < nc; ++i) {
        Column& c = t.cols[i];
        c.role = Role::Unused; c.role_conf = 0; c.role_src = RoleSource::None; c.invert = false;
        if (auto it = lay.col_format.find(i); it != lay.col_format.end()) c.format = it->second;
        if (c.header.empty() || c.format == ColFormat::Skip) continue;
        double cf = 0;
        LenUnit u = LenUnit::Auto;
        bool inv = false;
        Role r = role_from_header(c.header, purpose, &cf, &u, &inv);
        if (u != LenUnit::Auto && !lay.unit_override.count(i)) c.unit = u;
        std::string k = norm_header(c.header);
        if (auto a = find_amb(k)) {
            // resolve by values
            bool refs = c.type == VType::Ref || c.type == VType::RefList;
            Role pick = r;
            bool has = [&](Role x) { return std::find(a->roles.begin(), a->roles.end(), x) != a->roles.end(); }(Role::Ref);
            // "Item" beside a real "Designator" column: the item column is the line item (I1, I2 ...)
            bool other_ref = false;
            if (purpose == Purpose::Bom && k == "item")
                for (int j = 0; j < nc; ++j) {
                    if (j == i || t.cols[j].header.empty()) continue;
                    double c2 = 0;
                    if (role_from_header(t.cols[j].header, purpose, &c2) == Role::Ref && c2 >= 1.0 && !find_amb(norm_header(t.cols[j].header))) other_ref = true;
                }
            if (other_ref) pick = Role::LineItem;
            else if (refs && has) pick = Role::Ref;
            else if (c.type == VType::Integer && std::find(a->roles.begin(), a->roles.end(), Role::LineItem) != a->roles.end())
                pick = Role::LineItem;
            else if (k == "type" || k == "typ") {
                int proc = 0, ne = 0;
                for (auto& s : c.samples) {
                    std::string u2 = to_upper(s);
                    ++ne;
                    if (u2 == "SMT" || u2 == "SMD" || u2 == "THT" || u2 == "TH" || u2 == "PTH" || u2 == "THD" ||
                        u2 == "AUTO" || u2 == "HAND" || u2 == "SLIDE" || u2 == "MANUAL" || u2 == "WAVE")
                        ++proc;
                }
                pick = (ne && proc * 2 > ne) ? Role::Process : Role::Package;
            } else {
                pick = a->roles[0] == Role::Ref ? (refs ? Role::Ref : a->roles[1]) : a->roles[0];
                if (pick == Role::Ref && !refs) pick = Role::Unused;
                if (purpose == Purpose::Pnp && pick == Role::LineItem) pick = Role::Unused;
            }
            r = pick;
            cf = refs && r == Role::Ref ? 0.85 : 0.55;
        }
        if (r == Role::Unused || cf < 0.4) continue;
        // purpose filter
        if (purpose == Purpose::Bom && (r == Role::X || r == Role::Y || r == Role::Rotation || r == Role::Side || r == Role::Unit)) {
            if (cf < 1.0) continue;
        }
        if (purpose == Purpose::Pnp && r == Role::Qty) continue;
        // value sanity
        if ((r == Role::X || r == Role::Y || r == Role::Rotation || r == Role::Qty || r == Role::Height || r == Role::Cost) &&
            c.non_empty > 0 && !numeric_type(c.type)) {
            if (!(r == Role::Rotation && c.type == VType::Empty)) { cf *= 0.3; }
        }
        if (r == Role::Ref && c.non_empty > 0 && !(c.type == VType::Ref || c.type == VType::RefList)) cf *= 0.7;
        cand[i] = {r, cf, RoleSource::Header, inv};
    }
    // resolve conflicts: one column per role (keep highest confidence, earliest on ties)
    for (int i = 0; i < nc; ++i) {
        if (cand[i].r == Role::Unused) continue;
        for (int j = 0; j < nc; ++j) {
            if (j == i || cand[j].r != cand[i].r) continue;
            if (cand[j].conf > cand[i].conf || (cand[j].conf == cand[i].conf && j < i)) { cand[i] = Cand{}; break; }
        }
    }
    auto taken = [&](Role r) { for (auto& c : cand) if (c.r == r) return true; return false; };
    // user overrides take precedence
    for (auto& [ci, r] : lay.role_override) {
        if (ci < 0 || ci >= nc) continue;
        if (r != Role::Unused) for (auto& c : cand) if (c.r == r && c.src != RoleSource::User) c = Cand{};
        bool inv = false;
        if (r == Role::Dnp || r == Role::Side) {
            double cf; LenUnit u;
            if (role_from_header(t.cols[ci].header, purpose, &cf, &u, &inv) != r) inv = false;
        }
        cand[ci] = {r, 1.0, RoleSource::User, inv};
    }
    // pattern fallback for remaining columns
    auto free_col = [&](int i) { return cand[i].r == Role::Unused && !lay.role_override.count(i) && t.cols[i].format != ColFormat::Skip; };
    auto pick_first = [&](Role r, std::initializer_list<VType> ts, double conf) {
        if (taken(r)) return;
        for (int i = 0; i < nc; ++i) {
            if (!free_col(i)) continue;
            for (auto tt : ts)
                if (t.cols[i].type == tt && t.cols[i].type_conf >= 0.6) { cand[i] = {r, conf, RoleSource::Pattern, false}; return; }
        }
    };
    if (purpose == Purpose::Pnp) {
        pick_first(Role::Ref, {VType::Ref, VType::RefList}, 0.6);
        // X, Y: first numeric columns after (or anywhere) ; rotation: next angle-like
        int refc = -1;
        for (int i = 0; i < nc; ++i) if (cand[i].r == Role::Ref) refc = i;
        std::vector<int> nums;
        for (int i = 0; i < nc; ++i) if (free_col(i) && numeric_type(t.cols[i].type) && t.cols[i].type_conf >= 0.8) nums.push_back(i);
        if (refc >= 0) std::stable_partition(nums.begin(), nums.end(), [&](int i) { return i > refc; });
        size_t k = 0;
        if (!taken(Role::X) && k < nums.size()) cand[nums[k++]] = {Role::X, 0.5, RoleSource::Pattern, false};
        if (!taken(Role::Y) && k < nums.size()) cand[nums[k++]] = {Role::Y, 0.5, RoleSource::Pattern, false};
        if (!taken(Role::Rotation)) {
            for (size_t q = k; q < nums.size(); ++q)
                if (t.cols[nums[q]].type == VType::Angle || t.cols[nums[q]].type == VType::Integer) {
                    cand[nums[q]] = {Role::Rotation, 0.5, RoleSource::Pattern, false};
                    break;
                }
        }
        pick_first(Role::Side, {VType::SideTok}, 0.6);
        pick_first(Role::Package, {VType::PackageName}, 0.5);
        pick_first(Role::Value, {VType::CompValue}, 0.5);
        pick_first(Role::PartNumber, {VType::PartNo}, 0.4);
        pick_first(Role::Description, {VType::Words}, 0.4);
    } else if (purpose == Purpose::Bom) {
        pick_first(Role::Ref, {VType::RefList, VType::Ref}, 0.6);
        // qty: integer column whose values equal ref counts
        int refc = -1;
        for (int i = 0; i < nc; ++i) if (cand[i].r == Role::Ref) refc = i;
        if (!taken(Role::Qty) && refc >= 0) {
            for (int i = 0; i < nc && !taken(Role::Qty); ++i) {
                if (!free_col(i) || t.cols[i].type != VType::Integer) continue;
                int ok = 0, tot = 0;
                for (auto& r : t.rows) {
                    if (r.kind != RowKind::Data) continue;
                    auto q = parse_number(r.cells[i], false);
                    if (!q) continue;
                    ++tot;
                    ok += (size_t)*q == expand_refs(r.cells[refc]).size();
                }
                if (tot && ok >= 0.8 * tot) cand[i] = {Role::Qty, 0.6, RoleSource::Pattern, false};
            }
        }
        pick_first(Role::PartNumber, {VType::PartNo}, 0.5);
        pick_first(Role::Package, {VType::PackageName}, 0.5);
        pick_first(Role::Value, {VType::CompValue}, 0.5);
        pick_first(Role::Description, {VType::Words}, 0.4);
        pick_first(Role::Dnp, {VType::Bool}, 0.3);
    }
    for (int i = 0; i < nc; ++i)      // "Do not import" columns never get a role
        if (t.cols[i].format == ColFormat::Skip) cand[i] = Cand{};
    for (int i = 0; i < nc; ++i) {
        Column& c = t.cols[i];
        c.role = cand[i].r;
        c.role_conf = cand[i].conf;
        c.role_src = cand[i].r == Role::Unused ? RoleSource::None : cand[i].src;
        c.invert = cand[i].inv;
        auto io = lay.invert_override.find(i);
        if (io != lay.invert_override.end()) c.invert = io->second;
    }
}

// ---------------------------------------------------------------------------
// analyze_text
// ---------------------------------------------------------------------------
bool analyze_text(const std::string& bytes, const ImportLayout& lay, Purpose purpose, ParsedText& out, std::string& err) {
    out = ParsedText{};
    std::string text = decode_text(bytes, lay.encoding, &out.encoding);
    {
        size_t p = 0;
        while (p <= text.size()) {
            size_t q = text.find('\n', p);
            if (q == std::string::npos) { if (p < text.size()) out.lines.push_back(text.substr(p)); break; }
            out.lines.push_back(text.substr(p, q - p));
            p = q + 1;
        }
    }
    // table window: [skip_lines, stop_line)
    std::vector<std::string> Lw;
    const bool stopped = lay.stop_line >= 0 && lay.stop_line < (int)out.lines.size();
    if (stopped) Lw.assign(out.lines.begin(), out.lines.begin() + std::max(0, lay.stop_line));
    const auto& L = stopped ? Lw : out.lines;
    const int nl = (int)L.size();
    out.stop_line = stopped ? lay.stop_line : -1;
    out.quote = lay.quote;
    int start = std::clamp(lay.skip_lines, 0, nl);
    // Excel "sep=;" hint
    char forced_sep = 0;
    if (start < nl) {
        std::string f = to_lower(trim(L[start]));
        if (f.size() == 5 && f.rfind("sep=", 0) == 0) { forced_sep = L[start][L[start].find('=') + 1]; out.excel_sep_hint = true; ++start; }
    }
    if (start >= nl) { err = "file is empty"; return false; }

    std::vector<Rec> recs;
    Sep sep = lay.sep;
    if (lay.mode == SplitMode::Fixed) sep = Sep::Fixed;
    else if (lay.mode == SplitMode::Delimited && (sep == Sep::Fixed)) sep = Sep::Auto;
    const bool explicit_delims = lay.mode == SplitMode::Delimited && (!lay.delims.empty() || !lay.other_delim.empty());
    if (explicit_delims) sep = Sep::Custom;
    char sc = 0;
    switch (sep) {
    case Sep::Comma: sc = ','; break;
    case Sep::Semicolon: sc = ';'; break;
    case Sep::Tab: sc = '\t'; break;
    case Sep::Pipe: sc = '|'; break;
    case Sep::Custom: sc = lay.custom_sep ? lay.custom_sep : ','; break;
    default: break;
    }
    if (explicit_delims) {
        sc = lay.delims.size() == 1 && lay.other_delim.empty() ? lay.delims[0] : 0;
        if (sc == ',') sep = Sep::Comma; else if (sc == ';') sep = Sep::Semicolon; else if (sc == '\t') sep = Sep::Tab; else if (sc == '|') sep = Sep::Pipe;
        if (!sc) sc = '\x01';   // marks "delimited" below; the real split uses the delimiter set
    }
    if (explicit_delims) {
        recs = parse_multi(L, start, lay.delims, lay.other_delim, lay.quote, lay.consecutive_as_one, lay.comment_prefixes, lay.trim);
        out.notes.push_back("delimiters set by user");
    } else if (sep == Sep::Auto && forced_sep) {
        sc = forced_sep;
        sep = sc == ',' ? Sep::Comma : sc == ';' ? Sep::Semicolon : sc == '\t' ? Sep::Tab : sc == '|' ? Sep::Pipe : Sep::Custom;
    }
    if (sep == Sep::Auto) {
        struct C { char c; Sep s; };
        const C cs[] = {{'\t', Sep::Tab}, {';', Sep::Semicolon}, {',', Sep::Comma}, {'|', Sep::Pipe}};
        double best = 0;
        int bestm = 0;
        for (auto& c : cs) {
            auto r = parse_delimited(L, start, c.c, lay.quote, lay.comment_prefixes, lay.trim);
            double cons = 0;
            int ntot = 0;
            int m = mode_count(r, &cons, &ntot);
            if (m < 2 || cons < 0.6) continue;
            // tab-separated data that is really aligned text has empty cells everywhere: still fine
            double score = cons + std::min(m, 12) * 0.002;
            if (score > best + 0.03 || (score > best && bestm == 0)) { best = score; bestm = m; sep = c.s; sc = c.c; recs = std::move(r); }
        }
        if (sep == Sep::Auto) {
            out.notes.push_back("no consistent delimiter; using whitespace / fixed columns");
            sep = Sep::Whitespace;   // refined below
        } else {
            out.notes.push_back(std::string("delimiter detected: ") + sep_name(sep));
        }
    } else if (sc && !explicit_delims) {
        recs = parse_delimited(L, start, sc, lay.quote, lay.comment_prefixes, lay.trim);
    }

    std::vector<std::string> exp;     // tab-expanded lines (fixed / whitespace)
    std::vector<std::pair<int, int>> fixed_ext;
    bool delimited = sc != 0;
    if (!delimited) {
        exp.resize(nl);
        for (int i = 0; i < nl; ++i) exp[i] = expand_tabs(L[i], lay.tab_size, lay.replace_tabs);
        std::vector<std::string> body;
        std::vector<std::string> dl;
        for (int i = start; i < nl; ++i) {
            if (trim(exp[i]).empty() || starts_with_comment(exp[i], lay.comment_prefixes, nullptr)) continue;
            body.push_back(exp[i]);
            if (line_datalike(exp[i])) dl.push_back(exp[i]);
        }
        if (dl.size() < 2) dl = body;
        std::vector<int> cuts;
        const bool fixed_req = lay.mode == SplitMode::Fixed || (lay.mode == SplitMode::Auto && lay.sep == Sep::Fixed);
        const bool auto_req = lay.mode == SplitMode::Auto && lay.sep == Sep::Auto;
        if (fixed_req && !lay.fixed_cuts.empty()) {
            cuts = lay.fixed_cuts;
            std::sort(cuts.begin(), cuts.end());
            cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
            if (cuts[0] != 0 && cuts[0] > 0) cuts.insert(cuts.begin(), 0);
            sep = Sep::Fixed;
        } else if (auto_req || fixed_req) {
            cuts = detect_cuts(dl);
            if (!cuts.empty() && cuts[0] > 0) cuts[0] = 0;
            double dmg = cut_damage(dl, cuts);
            // whitespace token count consistency
            std::map<int, int> h;
            for (auto& l : dl) ++h[(int)split_ws(l, false, lay.quote).size()];
            int wm = 0, wc = 0;
            for (auto& [k, c] : h) if (c > wc) { wm = k; wc = c; }
            bool ws_consistent = !dl.empty() && wc >= 0.9 * dl.size();
            if (fixed_req || (cuts.size() >= 2 && dmg <= 0.1 && (!ws_consistent || (int)cuts.size() >= wm))) {
                sep = Sep::Fixed;
            } else {
                // runs of 2+ spaces?
                std::map<int, int> h2;
                for (auto& l : dl) ++h2[(int)split_ws(l, true, lay.quote).size()];
                int m2 = 0, c2 = 0;
                for (auto& [k, c] : h2) if (c > c2) { m2 = k; c2 = c; }
                sep = (!ws_consistent && m2 >= 2 && c2 >= 0.9 * dl.size()) ? Sep::Spaces : Sep::Whitespace;
                cuts.clear();
            }
        }
        if (lay.sep == Sep::Spaces && !fixed_req) sep = Sep::Spaces;
        if (lay.sep == Sep::Whitespace && !fixed_req) sep = Sep::Whitespace;
        out.cuts = cuts;
        // data extent of every fixed column (for header placement)
        for (size_t c = 0; c < cuts.size(); ++c) {
            int lo = 1 << 30, hi = -1;
            int a = cuts[c], b = c + 1 < cuts.size() ? cuts[c + 1] : 1 << 30;
            for (auto& l : dl)
                for (int q = a; q < b && q < (int)l.size(); ++q)
                    if (l[q] != ' ') { lo = std::min(lo, q); hi = std::max(hi, q + 1); }
            fixed_ext.push_back({lo, hi});
        }
        for (int i = start; i < nl; ++i) {
            Rec r;
            r.line = i;
            r.raw = exp[i];
            if (trim(exp[i]).empty()) { r.blank = true; recs.push_back(r); continue; }
            size_t plen = 0;
            if (starts_with_comment(exp[i], lay.comment_prefixes, &plen)) {
                r.comment = true;
                std::string s = exp[i];
                for (size_t k = 0; k < plen; ++k) s[k] = ' ';   // keep alignment
                r.raw = s;
                if (sep == Sep::Fixed) r.cells = header_fixed(s, cuts, fixed_ext);
                else r.cells = split_ws(s, sep == Sep::Spaces, lay.quote);
                recs.push_back(r);
                continue;
            }
            if (sep == Sep::Fixed) r.cells = slice_fixed(exp[i], cuts, lay.trim);
            else for (auto& c : split_ws(exp[i], sep == Sep::Spaces, lay.quote)) r.cells.push_back(clean_cell(c, lay.trim));
            recs.push_back(r);
        }
    }
    out.sep = sep;
    out.sep_char = sc == '\x01' ? 0 : sc;
    // effective wizard description
    if (sep == Sep::Fixed) out.mode = SplitMode::Fixed;
    else {
        out.mode = SplitMode::Delimited;
        if (explicit_delims) { out.delims = lay.delims; out.other_delim = lay.other_delim; out.consecutive_as_one = lay.consecutive_as_one; }
        else if (sep == Sep::Spaces || sep == Sep::Whitespace) { out.delims = sep == Sep::Whitespace ? " \t" : " "; out.consecutive_as_one = true; }
        else if (sc) out.delims = std::string(1, sc);
    }

    // ---- column count ----
    int ncols = 0;
    if (sep == Sep::Fixed) ncols = (int)out.cuts.size();
    else {
        double cons = 0;
        ncols = mode_count(recs, &cons, nullptr);
        if (ncols < 1) {
            for (auto& r : recs) ncols = std::max(ncols, (int)r.cells.size());
        }
    }
    if (ncols < 1) { err = "no table found"; return false; }

    // ---- header detection ----
    int hdr = -1;       // index into recs
    auto header_cells = [&](const Rec& r) -> std::vector<std::string> {
        if (sep == Sep::Fixed) return header_fixed(r.raw, out.cuts, fixed_ext);
        return r.cells;
    };
    if (lay.header_line >= 0) {
        for (size_t i = 0; i < recs.size(); ++i)
            if (recs[i].line <= lay.header_line && lay.header_line < recs[i].line + recs[i].nlines) { hdr = (int)i; break; }
    } else if (lay.header_line == -1) {
        double best = 0;
        int scanned = 0;
        for (size_t i = 0; i < recs.size() && scanned < 60; ++i) {
            const Rec& r = recs[i];
            if (r.blank) continue;
            ++scanned;
            int num = 0;
            auto hc = header_cells(r);
            double s = header_score(hc, &num);
            int filled = 0;
            for (auto& c : hc) filled += !trim(c).empty();
            bool ok = (s >= 2 || (s >= 1 && ncols <= 2)) && num <= (filled >= 6 ? 1 : 0);
            if (ok && s > best) { best = s; hdr = (int)i; }
            // stop scanning once we are well into data
            if (hdr >= 0 && i > (size_t)hdr + 5) break;
        }
        if (hdr < 0) {
            // generic: first non-comment record with all-text cells followed by a record with numbers
            for (size_t i = 0; i + 1 < recs.size(); ++i) {
                const Rec& r = recs[i];
                if (r.blank || r.comment) continue;
                if ((int)r.cells.size() != ncols && sep != Sep::Fixed) continue;
                bool alltext = true, distinct = true;
                std::set<std::string> seen;
                int filled = 0;
                for (auto& c : r.cells) {
                    if (trim(c).empty()) continue;
                    ++filled;
                    VType vt = classify_cell(c, true);
                    if (numeric_type(vt) || vt == VType::Ref || vt == VType::RefList || vt == VType::SideTok) alltext = false;
                    if (!seen.insert(to_lower(c)).second) distinct = false;
                }
                size_t j = i + 1;
                while (j < recs.size() && (recs[j].blank || recs[j].comment)) ++j;
                bool next_num = false;
                if (j < recs.size())
                    for (size_t k = 0; k < recs[j].cells.size() && k < r.cells.size(); ++k)
                        if (cell_numeric(recs[j].cells[k]) && !cell_numeric(r.cells[k]) && !trim(r.cells[k]).empty()) next_num = true;
                if (alltext && distinct && filled >= std::max(2, ncols / 2) && next_num) hdr = (int)i;
                break;
            }
        }
    }
    std::vector<std::string> hcells;
    int hrows = 0;
    if (hdr >= 0) {
        hcells = header_cells(recs[hdr]);
        out.header_line = recs[hdr].line;
        out.header_commented = recs[hdr].comment;
        hrows = 1;
        // multi-row header
        int want = lay.header_rows > 0 ? lay.header_rows : 0;
        size_t j = hdr + 1;
        while (j < recs.size()) {
            const Rec& r = recs[j];
            if (r.blank) break;
            if (want) { if (hrows >= want) break; }
            else {
                if (hrows >= 3) break;
                auto hc2 = header_cells(r);
                int filled = 0, units = 0;
                bool bad = false;
                for (auto& c : hc2) {
                    if (trim(c).empty()) continue;
                    ++filled;
                    if (is_unit_cell(c)) { ++units; continue; }
                    VType vt = classify_cell(c, true);
                    if (numeric_type(vt) || vt == VType::Ref || vt == VType::RefList || vt == VType::SideTok ||
                        vt == VType::PackageName || vt == VType::CompValue || vt == VType::PartNo)
                        bad = true;
                }
                if (bad || filled == 0) break;
                // the following record must look like data
                size_t k = j + 1;
                while (k < recs.size() && (recs[k].blank || recs[k].comment)) ++k;
                if (k >= recs.size()) break;
                int nnum = 0;
                for (auto& c : recs[k].cells) nnum += cell_numeric(c) || is_refdes(c) || is_ref_list(c);
                if (!nnum) break;
                if (!units && header_score(hc2, nullptr) < 1 && filled > ncols / 2) break;
            }
            auto hc2 = header_cells(r);
            if (hc2.size() > hcells.size()) hcells.resize(hc2.size());
            for (size_t k = 0; k < hc2.size(); ++k) {
                if (trim(hc2[k]).empty()) continue;
                hcells[k] = trim(hcells[k] + " " + hc2[k]);
            }
            ++hrows;
            ++j;
        }
        if (sep != Sep::Fixed) {
            int hn = (int)hcells.size();
            while (hn > 0 && trim(hcells[hn - 1]).empty()) --hn;
            if (hn > ncols) ncols = hn;
        }
    }
    out.header_rows = hrows;
    if (hrows > 1) out.notes.push_back("multi-row header (" + std::to_string(hrows) + " lines)");

    // ---- rows ----
    const size_t data_from = hdr >= 0 ? hdr + hrows : 0;
    std::string hnorm;
    for (auto& c : hcells) hnorm += norm_header(c) + "|";
    bool seen_data = false;
    int last_good = -1;
    for (size_t i = 0; i < recs.size(); ++i) {
        const Rec& r = recs[i];
        Row row;
        row.line = r.line;
        row.line_count = r.nlines;
        row.cells = r.cells;
        if (r.blank) row.kind = RowKind::Blank;
        else if (r.comment) { row.kind = RowKind::Comment; out.meta.push_back(trim(L[r.line])); }
        else if (hdr >= 0 && i >= (size_t)hdr && i < data_from) row.kind = RowKind::Header;
        else if (i < data_from) { row.kind = RowKind::Preamble; out.meta.push_back(trim(L[r.line])); }
        else {
            int cnt = (int)r.cells.size();
            int trimmed = cnt;
            while (trimmed > 0 && trim(r.cells[trimmed - 1]).empty()) --trimmed;
            bool allempty = trimmed == 0;
            std::string rn;
            for (auto& c : r.cells) rn += norm_header(c) + "|";
            if (allempty) row.kind = RowKind::Blank;
            else if (hdr >= 0 && rn.size() >= 4 && rn.substr(0, std::min(rn.size(), hnorm.size())) == hnorm.substr(0, std::min(rn.size(), hnorm.size())) && rn == hnorm)
                row.kind = RowKind::RepeatedHeader;
            else if (!seen_data && hdr < 0 && delimited && cnt != ncols && trimmed < ncols) {
                row.kind = RowKind::Preamble;   // title rows before a headerless table
                out.meta.push_back(trim(L[r.line]));
            } else {
                row.kind = RowKind::Data;
                if (delimited && cnt != ncols && trimmed != ncols && trimmed > ncols) row.note = "ragged (" + std::to_string(cnt) + " cells)";
                if (delimited && trimmed < ncols && trimmed <= ncols / 2 && ncols > 2) row.note = "ragged (" + std::to_string(cnt) + " cells)";
                if (lay.skip_summary && is_summary_text(L[r.line]) &&
                    !(r.cells.size() && (is_refdes(r.cells[0]) || is_ref_list(r.cells[0])))) {
                    row.kind = RowKind::Summary;
                    row.note = "summary text";
                }
                if (row.kind == RowKind::Data) {
                    if (row.note.empty()) last_good = (int)out.rows.size();
                    seen_data = true;
                }
            }
        }
        // normalise cell count for data rows
        if (row.kind == RowKind::Data || row.kind == RowKind::Summary || row.kind == RowKind::RepeatedHeader) {
            if (!delimited && sep != Sep::Fixed && (int)row.cells.size() > ncols && ncols > 0) {
                // whitespace: overflow words join the last column
                std::string tail;
                for (size_t k = ncols - 1; k < row.cells.size(); ++k) tail += (tail.empty() ? "" : " ") + row.cells[k];
                row.cells.resize(ncols);
                row.cells[ncols - 1] = tail;
                if (row.note.empty()) row.note = "merged overflow words";
            }
            row.cells.resize(ncols);
        }
        out.rows.push_back(std::move(row));
    }
    // trailing ragged rows after the last good row are summaries
    if (lay.skip_summary)
        for (size_t i = last_good + 1; i < out.rows.size(); ++i)
            if (out.rows[i].kind == RowKind::Data && !out.rows[i].note.empty() && out.rows[i].note.rfind("ragged", 0) == 0) {
                out.rows[i].kind = RowKind::Summary;
            }

    // ---- decimal comma ----
    if (lay.decimal_comma >= 0) out.decimal_comma = lay.decimal_comma == 1;
    else if (lay.thousands_sep == '.') out.decimal_comma = true;
    else if (lay.thousands_sep == ',') out.decimal_comma = false;
    else if (sc != ',' && !(explicit_delims && lay.delims.find(',') != std::string::npos)) {
        int dcn = 0, dpn = 0;
        for (auto& r : out.rows) {
            if (r.kind != RowKind::Data) continue;
            for (auto& c : r.cells) {
                std::string num, suf;
                if (!split_num_suffix(c, num, suf) || (!suf.empty() && len_unit_from_word(suf) == LenUnit::Auto && suf != "\xC2\xB0")) continue;
                size_t cp = num.find(','), dp = num.find('.');
                if (cp != std::string::npos && dp == std::string::npos && num.size() - cp - 1 != 3) ++dcn;
                else if (cp != std::string::npos && dp == std::string::npos) ++dcn;
                if (dp != std::string::npos && cp == std::string::npos) ++dpn;
            }
        }
        out.decimal_comma = dcn > 0 && dpn == 0;
        if (out.decimal_comma) out.notes.push_back("decimal comma detected");
    }

    // ---- per-column formats: numeric clean-up (thousands, trailing minus, negate) ----
    auto fmt_of = [&](int c) { auto it = lay.col_format.find(c); return it == lay.col_format.end() ? ColFormat::General : it->second; };
    auto neg_of = [&](int c) { auto it = lay.negate.find(c); return it != lay.negate.end() && it->second; };
    for (int c = 0; c < ncols; ++c) {
        ColFormat f = fmt_of(c);
        bool ng = neg_of(c);
        bool apply = f == ColFormat::Number || f == ColFormat::Angle ||
                     (f == ColFormat::General && (lay.thousands_sep || lay.trailing_minus || ng));
        if (!apply) continue;
        for (auto& r : out.rows) {
            if (r.kind != RowKind::Data || c >= (int)r.cells.size()) continue;
            r.cells[c] = normalize_numeric_cell(r.cells[c], lay.thousands_sep, out.decimal_comma, lay.trailing_minus, ng);
        }
    }

    // ---- columns ----
    out.cols.resize(ncols);
    for (int c = 0; c < ncols; ++c) {
        Column& col = out.cols[c];
        if (c < (int)hcells.size()) col.header = trim(hcells[c]);
        if (sep == Sep::Fixed && c < (int)out.cuts.size()) {
            col.fixed_start = out.cuts[c];
            col.fixed_end = c + 1 < (int)out.cuts.size() ? out.cuts[c + 1] : -1;
        }
        std::vector<std::string> cells;
        std::map<LenUnit, int> units;
        for (auto& r : out.rows) {
            if (r.kind != RowKind::Data) continue;
            const std::string& v = r.cells[c];
            cells.push_back(v);
            if (trim(v).empty()) continue;
            ++col.non_empty;
            if (col.samples.size() < 8) col.samples.push_back(v);
            LenUnit lu;
            if (parse_length(v, out.decimal_comma, &lu) && lu != LenUnit::Auto) ++units[lu];
        }
        col.type = classify_column(cells, out.decimal_comma, &col.type_conf);
        for (auto& [u, k] : units) if (k * 2 >= col.non_empty) col.unit = u;
        col.format = fmt_of(c);
        col.negate = neg_of(c);
        if (col.format == ColFormat::Text) col.type = VType::Text;
        if (auto it = lay.unit_override.find(c); it != lay.unit_override.end() && it->second != LenUnit::Auto) col.unit = it->second;
    }
    out.unit_hint = unit_from_meta(out.meta);
    if (out.unit_hint == LenUnit::Auto && out.header_commented) out.unit_hint = unit_from_meta({L[out.header_line]});
    assign_roles(out, lay, purpose);
    return true;
}

bool analyze_file(const std::string& path, const ImportLayout& layout, Purpose purpose, ParsedText& out, std::string& err) {
    std::string bytes;
    if (!read_file(path, bytes, err)) return false;
    return analyze_text(bytes, layout, purpose, out, err);
}

// ===========================================================================
// Templates
// ===========================================================================
static const char* purpose_key(Purpose p) { return p == Purpose::Bom ? "bom" : p == Purpose::Pnp ? "pnp" : "generic"; }
static Purpose purpose_from(const std::string& s) { return s == "bom" ? Purpose::Bom : s == "pnp" ? Purpose::Pnp : Purpose::Generic; }

static json::Value str_array(const std::vector<std::string>& v) {
    json::Value a = json::Value::array();
    for (auto& s : v) a.push(s);
    return a;
}
static std::vector<std::string> str_vec(const json::Value& a) {
    std::vector<std::string> v;
    for (auto& x : a.items()) v.push_back(x.str());
    return v;
}

json::Value FormatTemplate::to_json() const {
    json::Value o = json::Value::object();
    o.set("name", name);
    o.set("purpose", purpose_key(purpose));
    o.set("encoding", (int)layout.encoding);
    o.set("sep", sep_name(layout.sep));
    o.set("custom_sep", std::string(layout.custom_sep ? 1 : 0, layout.custom_sep));
    json::Value cuts = json::Value::array();
    for (int c : layout.fixed_cuts) cuts.push(c);
    o.set("fixed_cuts", cuts);
    o.set("skip_lines", layout.skip_lines);
    o.set("header_line", layout.header_line);
    o.set("header_rows", layout.header_rows);
    o.set("decimal_comma", layout.decimal_comma);
    o.set("replace_tabs", layout.replace_tabs);
    o.set("comment_prefixes", str_array(layout.comment_prefixes));
    o.set("mode", split_mode_name(layout.mode));
    o.set("delims", layout.delims);
    o.set("other_delim", layout.other_delim);
    o.set("consecutive", layout.consecutive_as_one);
    o.set("quote", std::string(layout.quote ? 1 : 0, layout.quote));
    o.set("stop_line", layout.stop_line);
    o.set("tab_size", layout.tab_size);
    o.set("skip_summary", layout.skip_summary);
    o.set("trim", layout.trim);
    o.set("thousands", std::string(layout.thousands_sep ? 1 : 0, layout.thousands_sep));
    o.set("trailing_minus", layout.trailing_minus);
    json::Value cf = json::Value::object();
    for (auto& [i, f] : layout.col_format) cf.set(std::to_string(i), (int)f);
    o.set("col_format", cf);
    json::Value uo = json::Value::object();
    for (auto& [i, u] : layout.unit_override) uo.set(std::to_string(i), len_unit_name(u));
    o.set("col_unit", uo);
    json::Value ng = json::Value::object();
    for (auto& [i, b] : layout.negate) ng.set(std::to_string(i), b);
    o.set("negate", ng);
    json::Value hr = json::Value::array();
    for (auto& [h, r] : header_roles) {
        json::Value e = json::Value::object();
        e.set("header", h);
        e.set("role", role_key(r));
        hr.push(e);
    }
    o.set("header_roles", hr);
    json::Value ir = json::Value::array();
    for (auto r : index_roles) ir.push(role_key(r));
    o.set("index_roles", ir);
    json::Value inv = json::Value::object();
    for (auto& [i, b] : layout.invert_override) inv.set(std::to_string(i), b);
    o.set("invert", inv);
    o.set("signature", str_array(signature));
    o.set("ncols", ncols);
    o.set("units", len_unit_name(units));
    o.set("factor", factor);
    o.set("rot_offset", rot_offset);
    o.set("mirror_x", mirror_x);
    o.set("mirror_y", mirror_y);
    o.set("bottom_x", (int)bottom_x);
    o.set("bottom_rot", (int)bottom_rot);
    o.set("top_tokens", str_array(top_tokens));
    o.set("bottom_tokens", str_array(bottom_tokens));
    return o;
}

bool FormatTemplate::from_json(const json::Value& v, FormatTemplate& t) {
    if (!v.is_object()) return false;
    t = FormatTemplate{};
    t.name = v["name"].str();
    t.purpose = purpose_from(v["purpose"].str());
    t.layout.encoding = (Encoding)v["encoding"].i(0);
    std::string s = v["sep"].str("auto");
    for (int k = 0; k <= (int)Sep::Fixed; ++k) if (s == sep_name((Sep)k)) t.layout.sep = (Sep)k;
    std::string cs = v["custom_sep"].str();
    t.layout.custom_sep = cs.empty() ? 0 : cs[0];
    for (auto& c : v["fixed_cuts"].items()) t.layout.fixed_cuts.push_back(c.i());
    t.layout.skip_lines = v["skip_lines"].i(0);
    t.layout.header_line = v["header_line"].i(-1);
    t.layout.header_rows = v["header_rows"].i(0);
    t.layout.decimal_comma = v["decimal_comma"].i(-1);
    t.layout.replace_tabs = v["replace_tabs"].i(0);
    if (v.has("comment_prefixes")) t.layout.comment_prefixes = str_vec(v["comment_prefixes"]);
    std::string md = v["mode"].str("auto");
    for (auto m : {SplitMode::Auto, SplitMode::Delimited, SplitMode::Fixed}) if (md == split_mode_name(m)) t.layout.mode = m;
    t.layout.delims = v["delims"].str();
    t.layout.other_delim = v["other_delim"].str();
    t.layout.consecutive_as_one = v["consecutive"].b();
    if (v.has("quote")) { std::string q = v["quote"].str(); t.layout.quote = q.empty() ? 0 : q[0]; }
    t.layout.stop_line = v["stop_line"].i(-1);
    t.layout.tab_size = std::max(1, v["tab_size"].i(8));
    if (v.has("skip_summary")) t.layout.skip_summary = v["skip_summary"].b();
    if (v.has("trim")) t.layout.trim = v["trim"].b();
    { std::string th = v["thousands"].str(); t.layout.thousands_sep = th.empty() ? 0 : th[0]; }
    t.layout.trailing_minus = v["trailing_minus"].b();
    for (auto& [k, f] : v["col_format"].members()) t.layout.col_format[std::atoi(k.c_str())] = (ColFormat)std::clamp(f.i(0), 0, 4);
    for (auto& [k, u] : v["col_unit"].members())
        for (auto lu : {LenUnit::Auto, LenUnit::Mil, LenUnit::Inch, LenUnit::MM, LenUnit::CM})
            if (u.str() == len_unit_name(lu)) t.layout.unit_override[std::atoi(k.c_str())] = lu;
    for (auto& [k, b] : v["negate"].members()) t.layout.negate[std::atoi(k.c_str())] = b.b();
    for (auto& e : v["header_roles"].items()) t.header_roles.push_back({e["header"].str(), role_from_key(e["role"].str())});
    for (auto& e : v["index_roles"].items()) t.index_roles.push_back(role_from_key(e.str()));
    for (auto& [k, b] : v["invert"].members()) t.layout.invert_override[std::atoi(k.c_str())] = b.b();
    t.signature = str_vec(v["signature"]);
    t.ncols = v["ncols"].i(0);
    std::string u = v["units"].str("auto");
    for (auto lu : {LenUnit::Auto, LenUnit::Mil, LenUnit::Inch, LenUnit::MM, LenUnit::CM})
        if (u == len_unit_name(lu)) t.units = lu;
    t.factor = v["factor"].num(1.0);
    t.rot_offset = v["rot_offset"].num(0);
    t.mirror_x = v["mirror_x"].b();
    t.mirror_y = v["mirror_y"].b();
    t.bottom_x = (BottomX)std::clamp(v["bottom_x"].i(0), 0, 2);
    t.bottom_rot = (BottomRot)std::clamp(v["bottom_rot"].i(0), 0, 3);
    t.top_tokens = str_vec(v["top_tokens"]);
    t.bottom_tokens = str_vec(v["bottom_tokens"]);
    return true;
}

FormatTemplate make_template(const std::string& name, Purpose p, const ParsedText& t, const ImportLayout& layout) {
    FormatTemplate f;
    f.name = name;
    f.purpose = p;
    f.layout = layout;
    f.layout.role_override.clear();
    f.layout.sep = t.sep;
    if (t.sep == Sep::Custom || (t.sep_char && t.sep != Sep::Comma && t.sep != Sep::Semicolon && t.sep != Sep::Tab && t.sep != Sep::Pipe))
        f.layout.custom_sep = t.sep_char;
    if (t.sep == Sep::Fixed) f.layout.fixed_cuts = t.cuts;
    if (layout.mode == SplitMode::Fixed || t.sep == Sep::Fixed) f.layout.mode = SplitMode::Fixed;
    if (layout.decimal_comma < 0) f.layout.decimal_comma = t.decimal_comma ? 1 : 0;
    f.ncols = (int)t.ncols();
    for (auto& c : t.cols) {
        f.index_roles.push_back(c.role);
        std::string k = norm_header(c.header);
        if (!k.empty()) {
            f.signature.push_back(k);
            if (c.role != Role::Unused) f.header_roles.push_back({k, c.role});
        }
    }
    for (size_t i = 0; i < t.cols.size(); ++i) if (t.cols[i].invert) f.layout.invert_override[(int)i] = true;
    return f;
}

ImportLayout template_layout(const FormatTemplate& t) {
    ImportLayout l = t.layout;
    l.role_override.clear();
    return l;
}

void apply_template_roles(const FormatTemplate& tpl, ParsedText& t, ImportLayout& layout) {
    layout.role_override.clear();
    bool any_header = false;
    for (size_t i = 0; i < t.cols.size(); ++i) {
        std::string k = norm_header(t.cols[i].header);
        if (k.empty()) continue;
        any_header = true;
        Role r = Role::Unused;
        for (auto& [h, rr] : tpl.header_roles) if (h == k) { r = rr; break; }
        layout.role_override[(int)i] = r;
    }
    if (!any_header)
        for (size_t i = 0; i < t.cols.size() && i < tpl.index_roles.size(); ++i) layout.role_override[(int)i] = tpl.index_roles[i];
    Purpose p = tpl.purpose;
    assign_roles(t, layout, p);
    for (auto& c : t.cols) if (c.role_src == RoleSource::User) c.role_src = RoleSource::Template;
}

std::string templates_to_json(const std::vector<FormatTemplate>& v) {
    json::Value root = json::Value::object();
    root.set("format", "pcbp.import_templates");
    root.set("version", 1);
    json::Value a = json::Value::array();
    for (auto& t : v) a.push(t.to_json());
    root.set("templates", a);
    return root.dump(2);
}

bool templates_from_json(const std::string& text, std::vector<FormatTemplate>& v, std::string& err) {
    json::Value root;
    if (!json::Value::parse(text, root, err)) return false;
    v.clear();
    const json::Value& a = root.is_array() ? root : root["templates"];
    for (auto& e : a.items()) {
        FormatTemplate t;
        if (FormatTemplate::from_json(e, t)) v.push_back(std::move(t));
    }
    return true;
}

bool save_templates(const std::string& path, const std::vector<FormatTemplate>& v, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { err = "cannot write " + path; return false; }
    std::string s = templates_to_json(v);
    bool ok = std::fwrite(s.data(), 1, s.size(), f) == s.size();
    std::fclose(f);
    if (!ok) err = "write failed: " + path;
    return ok;
}

bool load_templates(const std::string& path, std::vector<FormatTemplate>& v, std::string& err) {
    std::string s;
    if (!read_file(path, s, err)) return false;
    return templates_from_json(s, v, err);
}

double template_score(const FormatTemplate& tpl, const ParsedText& t) {
    std::vector<std::string> sig;
    for (auto& c : t.cols) { std::string k = norm_header(c.header); if (!k.empty()) sig.push_back(k); }
    if (!tpl.signature.empty() && !sig.empty()) {
        std::set<std::string> a(tpl.signature.begin(), tpl.signature.end()), b(sig.begin(), sig.end());
        int inter = 0;
        for (auto& x : a) inter += b.count(x);
        int uni = (int)(a.size() + b.size()) - inter;
        double j = uni ? (double)inter / uni : 0;
        // order bonus
        if (tpl.signature == sig) j = std::min(1.0, j + 0.05);
        return j;
    }
    if (tpl.signature.empty() && sig.empty()) {
        double s = 0;
        if (tpl.ncols == (int)t.ncols()) s += 0.6;
        if (tpl.layout.sep == t.sep || tpl.layout.sep == Sep::Auto) s += 0.3;
        if (tpl.layout.sep == Sep::Fixed && t.sep == Sep::Fixed && tpl.layout.fixed_cuts == t.cuts) s += 0.1;
        return s;
    }
    return 0;
}

int match_template(const std::vector<FormatTemplate>& v, const ParsedText& t, Purpose p, double min_score, double* score) {
    int best = -1;
    double bs = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        if (p != Purpose::Generic && v[i].purpose != Purpose::Generic && v[i].purpose != p) continue;
        double s = template_score(v[i], t);
        if (s > bs) { bs = s; best = (int)i; }
    }
    if (score) *score = bs;
    return bs >= min_score ? best : -1;
}

} // namespace pcbp::io
