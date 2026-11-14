// ============================================================================
// xlsx.cpp — spreadsheet file I/O for the sheet engine (pcbp::io).
//   XLSX: ECMA-376 Part 1 SpreadsheetML (workbook, sharedStrings §18.4,
//   styles §18.8, worksheet §18.3) packaged per Part 2 (OPC zip).
//   XLS : MS-XLS BIFF8 records inside an MS-CFB compound file (read only).
//   CSV : RFC 4180 with encoding options.
// Clean-room from the public specifications; see specs/SHEET_ENGINE.md.
// ============================================================================
#include "xlsx.h"
#include "archive.h"
#include "deflate.h"
#include "xlsx_xml.h"
#include "xml_lite.h"
#include "../core/formula_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>
#include <array>

namespace pcbp::io {

using namespace pcbp::sheet;
using sheet::Sheet;
using sheet::Workbook;
using xmlp::esc;

bool is_xlsx_bytes(const std::vector<uint8_t>& d) { return d.size() >= 4 && d[0] == 'P' && d[1] == 'K' && d[2] == 3 && d[3] == 4; }
bool is_ole2_bytes(const std::vector<uint8_t>& d) { return d.size() >= 8 && d[0] == 0xD0 && d[1] == 0xCF && d[2] == 0x11 && d[3] == 0xE0; }

// ===========================================================================
// zip writer (PKWARE APPNOTE: local headers, central directory, EOCD)
// ===========================================================================
static void put16(std::vector<uint8_t>& o, uint32_t v) { o.push_back(uint8_t(v)); o.push_back(uint8_t(v >> 8)); }
static void put32(std::vector<uint8_t>& o, uint32_t v) { put16(o, v & 0xFFFF); put16(o, v >> 16); }

std::vector<uint8_t> zip_write(const std::vector<std::pair<std::string, std::string>>& entries, int level, const std::string& first_stored) {
    std::vector<uint8_t> o, cd;
    const uint32_t dos_time = 0, dos_date = (uint32_t)((2024 - 1980) << 9 | 1 << 5 | 1);
    for (auto& [name, data] : entries) {
        const uint8_t* src = (const uint8_t*)data.data();
        uint32_t crc = crc32_ieee(src, data.size());
        std::vector<uint8_t> comp;
        uint16_t method = 0;
        if (level > 0 && name != first_stored && data.size() > 64) {
            comp = deflate_raw(src, data.size(), level);
            if (comp.size() < data.size()) method = 8;
        }
        const uint8_t* body = method ? comp.data() : src;
        uint32_t csize = method ? (uint32_t)comp.size() : (uint32_t)data.size();
        uint32_t off = (uint32_t)o.size();
        put32(o, 0x04034b50); put16(o, 20); put16(o, 0x0800 /*utf8*/); put16(o, method);
        put16(o, dos_time); put16(o, dos_date); put32(o, crc); put32(o, csize); put32(o, (uint32_t)data.size());
        put16(o, (uint32_t)name.size()); put16(o, 0);
        o.insert(o.end(), name.begin(), name.end());
        o.insert(o.end(), body, body + csize);
        put32(cd, 0x02014b50); put16(cd, 20); put16(cd, 20); put16(cd, 0x0800); put16(cd, method);
        put16(cd, dos_time); put16(cd, dos_date); put32(cd, crc); put32(cd, csize); put32(cd, (uint32_t)data.size());
        put16(cd, (uint32_t)name.size()); put16(cd, 0); put16(cd, 0); put16(cd, 0); put16(cd, 0); put32(cd, 0); put32(cd, off);
        cd.insert(cd.end(), name.begin(), name.end());
    }
    uint32_t cdo = (uint32_t)o.size();
    o.insert(o.end(), cd.begin(), cd.end());
    put32(o, 0x06054b50); put16(o, 0); put16(o, 0); put16(o, (uint32_t)entries.size()); put16(o, (uint32_t)entries.size());
    put32(o, (uint32_t)cd.size()); put32(o, cdo); put16(o, 0);
    return o;
}
std::vector<uint8_t> zip_store(const std::vector<std::pair<std::string, std::string>>& entries) { return zip_write(entries, 0); }

// ===========================================================================
// shared helpers
// ===========================================================================
namespace {

const char* const kBorderNames[] = {"none", "thin", "medium", "dashed", "dotted", "thick", "double", "hair", "mediumDashed", "dashDot", "mediumDashDot", "dashDotDot", "mediumDashDotDot", "slantDashDot"};
const char* const kFillNames[] = {"none", "solid", "mediumGray", "darkGray", "lightGray", "darkHorizontal", "darkVertical", "darkDown", "darkUp", "darkGrid", "darkTrellis", "lightHorizontal", "lightVertical", "lightDown", "lightUp", "lightGrid", "lightTrellis", "gray125", "gray0625"};
const char* const kHNames[] = {"general", "left", "center", "right", "fill", "justify", "centerContinuous", "distributed"};
const char* const kVNames[] = {"bottom", "center", "top", "justify", "distributed"};
const char* const kUNames[] = {"none", "single", "double", "singleAccounting", "doubleAccounting"};
const char* const kOpNames[] = {"between", "notBetween", "equal", "notEqual", "greaterThan", "lessThan", "greaterThanOrEqual", "lessThanOrEqual"};
const char* const kCfNames[] = {"cellIs", "expression", "containsText", "notContainsText", "beginsWith", "endsWith", "duplicateValues", "uniqueValues", "top10", "aboveAverage", "containsBlanks", "notContainsBlanks", "containsErrors", "notContainsErrors", "colorScale", "dataBar"};
const char* const kDvNames[] = {"none", "whole", "decimal", "list", "date", "time", "textLength", "custom"};
const char* const kDvErr[] = {"stop", "warning", "information"};
const char* const kCfvoNames[] = {"min", "max", "num", "percent", "percentile", "formula"};

template <size_t N> int name_index(const char* const (&tab)[N], const std::string& s, int def = 0) {
    for (size_t i = 0; i < N; ++i) if (s == tab[i]) return (int)i;
    return def;
}

std::string hex_argb(Argb c) { char b[16]; snprintf(b, sizeof b, "%08X", (unsigned)c); return b; }

std::string num_text(double v) {
    if (!std::isfinite(v)) return "0";
    char b[40];
    snprintf(b, sizeof b, "%.15g", v);
    if (atof(b) != v) snprintf(b, sizeof b, "%.17g", v);
    return b;
}

const Argb kIndexed[66] = {0xFF000000, 0xFFFFFFFF, 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00, 0xFFFF00FF, 0xFF00FFFF,
    0xFF000000, 0xFFFFFFFF, 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00, 0xFFFF00FF, 0xFF00FFFF,
    0xFF800000, 0xFF008000, 0xFF000080, 0xFF808000, 0xFF800080, 0xFF008080, 0xFFC0C0C0, 0xFF808080, 0xFF9999FF, 0xFF993366,
    0xFFFFFFCC, 0xFFCCFFFF, 0xFF660066, 0xFFFF8080, 0xFF0066CC, 0xFFCCCCFF, 0xFF000080, 0xFFFF00FF, 0xFFFFFF00, 0xFF00FFFF,
    0xFF800080, 0xFF800000, 0xFF008080, 0xFF0000FF, 0xFF00CCFF, 0xFFCCFFFF, 0xFFCCFFCC, 0xFFFFFF99, 0xFF99CCFF, 0xFFFF99CC,
    0xFFCC99FF, 0xFFFFCC99, 0xFF3366FF, 0xFF33CCCC, 0xFF99CC00, 0xFFFFCC00, 0xFFFF9900, 0xFFFF6600, 0xFF666699, 0xFF969696,
    0xFF003366, 0xFF339966, 0xFF003300, 0xFF333300, 0xFF993300, 0xFF993366, 0xFF333399, 0xFF333333, 0, 0};

Argb apply_tint(Argb c, double tint) {
    if (tint == 0) return c;
    double r = ((c >> 16) & 255) / 255.0, g = ((c >> 8) & 255) / 255.0, b = (c & 255) / 255.0;
    double mx = std::max({r, g, b}), mn = std::min({r, g, b});
    double h = 0, s = 0, l = (mx + mn) / 2;
    if (mx != mn) {
        double d = mx - mn;
        s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
        if (mx == r) h = (g - b) / d + (g < b ? 6 : 0);
        else if (mx == g) h = (b - r) / d + 2;
        else h = (r - g) / d + 4;
        h /= 6;
    }
    l = tint < 0 ? l * (1 + tint) : l * (1 - tint) + tint;
    auto hue = [](double p, double q, double t) {
        if (t < 0) t += 1;
        if (t > 1) t -= 1;
        if (t < 1.0 / 6) return p + (q - p) * 6 * t;
        if (t < 0.5) return q;
        if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
        return p;
    };
    double R, G, B;
    if (s == 0) R = G = B = l;
    else {
        double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
        R = hue(p, q, h + 1.0 / 3); G = hue(p, q, h); B = hue(p, q, h - 1.0 / 3);
    }
    auto to8 = [](double x) { return (Argb)std::lround(std::clamp(x, 0.0, 1.0) * 255); };
    return 0xFF000000u | to8(R) << 16 | to8(G) << 8 | to8(B);
}

// ARGB from an attribute: OOXML producers often write alpha 00 (or omit it); style colours are opaque
Argb opaque_rgb(const std::string& h0) {
    std::string h = h0;
    if (h.size() == 6) h = "FF" + h;
    Argb a = (Argb)strtoul(h.c_str(), nullptr, 16);
    return a | 0xFF000000u;
}

bool parse_a1(std::string_view ref, int& r, int& c) {
    size_t k = 0;
    int col = 0;
    while (k < ref.size() && (ref[k] == '$')) ++k;
    size_t c0 = k;
    while (k < ref.size() && isalpha((unsigned char)ref[k])) { col = col * 26 + (toupper((unsigned char)ref[k]) - 'A' + 1); ++k; }
    if (k == c0 || k - c0 > 3) return false;
    if (k < ref.size() && ref[k] == '$') ++k;
    long row = 0;
    size_t d0 = k;
    while (k < ref.size() && isdigit((unsigned char)ref[k])) { row = row * 10 + (ref[k] - '0'); ++k; }
    if (k == d0 || row < 1 || row > MAX_ROWS || col < 1 || col > MAX_COLS) return false;
    r = (int)row - 1; c = col - 1;
    return true;
}

std::vector<Range> parse_sqref(const std::string& s) {
    std::vector<Range> v;
    size_t p = 0;
    while (p < s.size()) {
        while (p < s.size() && s[p] == ' ') ++p;
        size_t e = s.find(' ', p);
        std::string part = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
        std::string clean;
        for (char ch : part) if (ch != '$') clean += ch;
        Range rg;
        if (!clean.empty() && parse_range_name(clean, rg)) v.push_back(rg);
        if (e == std::string::npos) break;
        p = e + 1;
    }
    return v;
}
std::string sqref_text(const std::vector<Range>& v) {
    std::string o;
    for (auto& r : v) { if (!o.empty()) o += ' '; o += range_name(r); }
    return o;
}

// Excel needs the _xlfn. prefix for functions introduced after 2007
const char* const kXlfn[] = {"IFS", "SWITCH", "XOR", "XLOOKUP", "XMATCH", "CONCAT", "TEXTJOIN", "MAXIFS", "MINIFS", "IFNA", "STDEV.S", "STDEV.P",
    "VAR.S", "VAR.P", "MODE.SNGL", "RANK.EQ", "PERCENTILE.INC", "PERCENTILE.EXC", "QUARTILE.INC", "CEILING.MATH", "FLOOR.MATH", "ISOWEEKNUM", "DAYS",
    "NUMBERVALUE", "UNICHAR", "UNICODE", "ISFORMULA", "FORMULATEXT", "SHEET", "SHEETS", "UNIQUE", "SORT", "SORTBY", "FILTER", "SEQUENCE",
    "TEXTSPLIT", "TEXTBEFORE", "TEXTAFTER"};
std::string add_xlfn(const std::string& f) {
    auto toks = tokenize_formula(f);
    std::string o;
    size_t last = 0;
    for (auto& t : toks) {
        if (t.kind != LexToken::Function) continue;
        std::string name = upper_ascii(f.substr(t.pos, t.len));
        bool need = false;
        for (const char* x : kXlfn) if (name == x) { need = true; break; }
        if (!need) continue;
        o += f.substr(last, t.pos - last);
        o += (name == "SORT" || name == "FILTER") ? "_xlfn._xlws." : "_xlfn.";
        last = t.pos;
    }
    o += f.substr(last);
    return o;
}

} // namespace

// ===========================================================================
// XLSX write
// ===========================================================================
namespace {

struct StyleWriter {
    const Book& b;
    std::vector<Font> fonts;
    std::vector<Fill> fills;
    std::vector<Border> borders;
    std::vector<std::pair<int, std::string>> numfmts;   // custom id -> code
    std::unordered_map<uint32_t, int> xf_of;            // style id -> xf index
    std::vector<std::string> xfs;
    explicit StyleWriter(const Book& bk) : b(bk) {
        fonts.push_back(Font());
        fills.push_back(Fill());
        Fill g; g.pattern = FillPattern::Gray125; fills.push_back(g);
        borders.push_back(Border());
        xf(0);
    }
    template <class T> static int find_or_add(std::vector<T>& v, const T& x) {
        for (size_t i = 0; i < v.size(); ++i) if (v[i] == x) return (int)i;
        v.push_back(x);
        return (int)v.size() - 1;
    }
    int numfmt_id(const std::string& code) {
        int id = builtin_numfmt_id(code);
        if (id >= 0) return id;
        for (auto& [i, c] : numfmts) if (c == code) return i;
        int nid = 164 + (int)numfmts.size();
        numfmts.push_back({nid, code});
        return nid;
    }
    int xf(uint32_t sid) {
        auto it = xf_of.find(sid);
        if (it != xf_of.end()) return it->second;
        const Style& s = b.styles.get(sid);
        int fo = find_or_add(fonts, s.font), fi = find_or_add(fills, s.fill), bo = find_or_add(borders, s.border);
        int nf = numfmt_id(s.numfmt);
        std::string x = "<xf numFmtId=\"" + std::to_string(nf) + "\" fontId=\"" + std::to_string(fo) + "\" fillId=\"" + std::to_string(fi) +
                        "\" borderId=\"" + std::to_string(bo) + "\" xfId=\"0\"";
        if (nf) x += " applyNumberFormat=\"1\"";
        if (fo) x += " applyFont=\"1\"";
        if (fi) x += " applyFill=\"1\"";
        if (bo) x += " applyBorder=\"1\"";
        bool align = s.halign != HAlign::General || s.valign != VAlign::Bottom || s.wrap || s.indent || s.rotation || s.shrink;
        bool prot = !s.locked || s.hidden;
        if (align) x += " applyAlignment=\"1\"";
        if (prot) x += " applyProtection=\"1\"";
        if (align || prot) {
            x += ">";
            if (align) {
                x += "<alignment";
                if (s.halign != HAlign::General) x += std::string(" horizontal=\"") + kHNames[(int)s.halign] + "\"";
                if (s.valign != VAlign::Bottom) x += std::string(" vertical=\"") + kVNames[(int)s.valign] + "\"";
                if (s.rotation) x += " textRotation=\"" + std::to_string(s.rotation < 0 && s.rotation != 255 ? 90 - s.rotation : s.rotation) + "\"";
                if (s.wrap) x += " wrapText=\"1\"";
                if (s.indent) x += " indent=\"" + std::to_string(s.indent) + "\"";
                if (s.shrink) x += " shrinkToFit=\"1\"";
                x += "/>";
            }
            if (prot) { x += "<protection"; if (!s.locked) x += " locked=\"0\""; if (s.hidden) x += " hidden=\"1\""; x += "/>"; }
            x += "</xf>";
        } else x += "/>";
        int idx = (int)xfs.size();
        xfs.push_back(x);
        xf_of[sid] = idx;
        return idx;
    }
    static std::string color_xml(const char* tag, Argb c) { return c ? std::string("<") + tag + " rgb=\"" + hex_argb(c) + "\"/>" : std::string(); }
    static std::string font_xml(const Font& f, bool dxf = false) {
        std::string o = "<font>";
        if (f.bold) o += "<b/>";
        if (f.italic) o += "<i/>";
        if (f.strike) o += "<strike/>";
        if (f.underline != Underline::None) o += f.underline == Underline::Single ? "<u/>" : std::string("<u val=\"") + kUNames[(int)f.underline] + "\"/>";
        if (!dxf) o += "<sz val=\"" + num_text(f.size) + "\"/>";
        o += color_xml("color", f.color);
        if (!dxf) { o += "<name val=\""; esc(o, f.name, true); o += "\"/><family val=\"2\"/>"; }
        return o + "</font>";
    }
    static std::string side_xml(const char* tag, const BorderSide& s) {
        if (s.style == BorderStyle::None) return std::string("<") + tag + "/>";
        return std::string("<") + tag + " style=\"" + kBorderNames[(int)s.style] + "\">" + (s.color ? "<color rgb=\"" + hex_argb(s.color) + "\"/>" : "<color auto=\"1\"/>") + "</" + tag + ">";
    }
    static std::string border_xml(const Border& b) {
        std::string o = "<border";
        if (b.diag_up) o += " diagonalUp=\"1\"";
        if (b.diag_down) o += " diagonalDown=\"1\"";
        o += ">" + side_xml("left", b.left) + side_xml("right", b.right) + side_xml("top", b.top) + side_xml("bottom", b.bottom) + side_xml("diagonal", b.diagonal) + "</border>";
        return o;
    }
    std::string xml() {
        // dxfs may add number formats: render them first
        std::string dx;
        for (auto& d : b.dxfs) {
            dx += "<dxf>";
            if (d.font_color || d.bold || d.italic || d.strike || d.underline) {
                dx += "<font>";
                if (d.bold) dx += *d.bold ? "<b/>" : "<b val=\"0\"/>";
                if (d.italic) dx += *d.italic ? "<i/>" : "<i val=\"0\"/>";
                if (d.strike) dx += *d.strike ? "<strike/>" : "<strike val=\"0\"/>";
                if (d.underline) dx += std::string("<u val=\"") + kUNames[(int)*d.underline] + "\"/>";
                if (d.font_color) dx += "<color rgb=\"" + hex_argb(*d.font_color) + "\"/>";
                dx += "</font>";
            }
            if (d.numfmt) { int id = numfmt_id(*d.numfmt); dx += "<numFmt numFmtId=\"" + std::to_string(id) + "\" formatCode=\"" + esc(*d.numfmt) + "\"/>"; }
            if (d.fill_color) dx += "<fill><patternFill patternType=\"solid\"><fgColor rgb=\"" + hex_argb(*d.fill_color) + "\"/><bgColor rgb=\"" + hex_argb(*d.fill_color) + "\"/></patternFill></fill>";
            if (d.border) dx += border_xml(*d.border);
            dx += "</dxf>";
        }
        std::string o = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">";
        if (!numfmts.empty()) {
            o += "<numFmts count=\"" + std::to_string(numfmts.size()) + "\">";
            for (auto& [id, code] : numfmts) o += "<numFmt numFmtId=\"" + std::to_string(id) + "\" formatCode=\"" + esc(code) + "\"/>";
            o += "</numFmts>";
        }
        o += "<fonts count=\"" + std::to_string(fonts.size()) + "\">";
        for (auto& f : fonts) o += font_xml(f);
        o += "</fonts><fills count=\"" + std::to_string(fills.size()) + "\">";
        for (auto& f : fills) {
            o += std::string("<fill><patternFill patternType=\"") + kFillNames[(int)f.pattern] + "\"";
            if (f.fg || f.bg) { o += ">"; if (f.fg) o += "<fgColor rgb=\"" + hex_argb(f.fg) + "\"/>"; o += f.bg ? "<bgColor rgb=\"" + hex_argb(f.bg) + "\"/>" : "<bgColor indexed=\"64\"/>"; o += "</patternFill></fill>"; }
            else o += "/></fill>";
        }
        o += "</fills><borders count=\"" + std::to_string(borders.size()) + "\">";
        for (auto& bd : borders) o += border_xml(bd);
        o += "</borders><cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>";
        o += "<cellXfs count=\"" + std::to_string(xfs.size()) + "\">";
        for (auto& x : xfs) o += x;
        o += "</cellXfs><cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>";
        o += "<dxfs count=\"" + std::to_string(b.dxfs.size()) + "\">" + dx + "</dxfs>";
        o += "</styleSheet>";
        return o;
    }
};

struct SST {
    std::unordered_map<std::string, int> idx;
    std::vector<const std::string*> v;
    size_t count = 0;
    int add(const std::string& s) {
        ++count;
        auto it = idx.find(s);
        if (it != idx.end()) return it->second;
        int i = (int)v.size();
        auto ins = idx.emplace(s, i);
        v.push_back(&ins.first->first);
        return i;
    }
};

std::string text_elem(const std::string& s) {
    std::string o = "<t";
    if (!s.empty() && (s.front() == ' ' || s.back() == ' ' || s.find('\n') != std::string::npos || s.find('\t') != std::string::npos)) o += " xml:space=\"preserve\"";
    o += ">";
    esc(o, s, false);
    return o + "</t>";
}

std::string cf_rule_formula_for_text(const CondRule& r, const std::string& tl) {
    std::string q = "\"";
    for (char ch : r.text) { if (ch == '"') q += '"'; q += ch; }
    q += "\"";
    switch (r.type) {
    case CondRule::ContainsText: return "NOT(ISERROR(SEARCH(" + q + "," + tl + ")))";
    case CondRule::NotContainsText: return "ISERROR(SEARCH(" + q + "," + tl + "))";
    case CondRule::BeginsWith: return "LEFT(" + tl + ",LEN(" + q + "))=" + q;
    case CondRule::EndsWith: return "RIGHT(" + tl + ",LEN(" + q + "))=" + q;
    case CondRule::ContainsBlanks: return "LEN(TRIM(" + tl + "))=0";
    case CondRule::NotContainsBlanks: return "LEN(TRIM(" + tl + "))>0";
    case CondRule::ContainsErrors: return "ISERROR(" + tl + ")";
    case CondRule::NotContainsErrors: return "NOT(ISERROR(" + tl + "))";
    default: return {};
    }
}

std::string sheet_xml(const Book& b, int si, StyleWriter& sw, SST& sst) {
    const Worksheet& ws = b.sheets[si];
    std::string x;
    x.reserve(1 << 16);
    x += "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
         "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">";
    if (ws.tab_color || ws.autofilter.active()) {
        x += "<sheetPr";
        if (ws.autofilter.active()) x += " filterMode=\"" + std::string(std::any_of(ws.autofilter.cols.begin(), ws.autofilter.cols.end(), [](const FilterColumn& f) { return f.active(); }) ? "1" : "0") + "\"";
        x += ">";
        if (ws.tab_color) x += "<tabColor rgb=\"" + hex_argb(ws.tab_color) + "\"/>";
        x += "</sheetPr>";
    }
    Range used = ws.used_range();
    for (auto& [k, arr] : ws.spills) {
        Range s(k.first, k.second, k.first + arr->rows - 1, k.second + arr->cols - 1);
        if (!used.valid()) used = s; else used = Range(std::min(used.r0, s.r0), std::min(used.c0, s.c0), std::max(used.r1, s.r1), std::max(used.c1, s.c1));
    }
    x += "<dimension ref=\"" + (used.valid() ? range_name(used) : std::string("A1")) + "\"/>";
    x += "<sheetViews><sheetView workbookViewId=\"0\"";
    if (si == b.active_sheet) x += " tabSelected=\"1\"";
    if (!ws.show_grid) x += " showGridLines=\"0\"";
    if (ws.zoom != 100) x += " zoomScale=\"" + std::to_string(ws.zoom) + "\"";
    x += ">";
    std::string active = cell_name(ws.active_r, ws.active_c);
    if (ws.freeze_rows > 0 || ws.freeze_cols > 0) {
        const char* pane = ws.freeze_rows && ws.freeze_cols ? "bottomRight" : ws.freeze_rows ? "bottomLeft" : "topRight";
        x += "<pane";
        if (ws.freeze_cols) x += " xSplit=\"" + std::to_string(ws.freeze_cols) + "\"";
        if (ws.freeze_rows) x += " ySplit=\"" + std::to_string(ws.freeze_rows) + "\"";
        x += " topLeftCell=\"" + cell_name(ws.freeze_rows, ws.freeze_cols) + "\" activePane=\"" + pane + "\" state=\"frozen\"/>";
        x += std::string("<selection pane=\"") + pane + "\" activeCell=\"" + active + "\" sqref=\"" + active + "\"/>";
    } else x += "<selection activeCell=\"" + active + "\" sqref=\"" + active + "\"/>";
    x += "</sheetView></sheetViews>";
    x += "<sheetFormatPr defaultRowHeight=\"" + num_text(ws.default_row_height) + "\"";
    if (std::fabs(ws.default_col_width - 8.43) > 1e-6) x += " defaultColWidth=\"" + num_text(ws.default_col_width) + "\"";
    x += "/>";
    // columns
    {
        std::string cx;
        int n = (int)ws.cols.size();
        for (int c = 0; c < n;) {
            const ColInfo& ci = ws.cols[c];
            if (ci.width < 0 && !ci.hidden && !ci.style && !ci.outline) { ++c; continue; }
            int e = c;
            while (e + 1 < n && ws.cols[e + 1].width == ci.width && ws.cols[e + 1].hidden == ci.hidden && ws.cols[e + 1].style == ci.style && ws.cols[e + 1].outline == ci.outline) ++e;
            cx += "<col min=\"" + std::to_string(c + 1) + "\" max=\"" + std::to_string(e + 1) + "\" width=\"" + num_text(ci.width >= 0 ? ci.width : ws.default_col_width + 0.71) + "\"";
            if (ci.width >= 0) cx += " customWidth=\"1\"";
            if (ci.hidden) cx += " hidden=\"1\"";
            if (ci.style) cx += " style=\"" + std::to_string(sw.xf(ci.style)) + "\"";
            if (ci.outline) cx += " outlineLevel=\"" + std::to_string(ci.outline) + "\"";
            cx += "/>";
            c = e + 1;
        }
        if (!cx.empty()) x += "<cols>" + cx + "</cols>";
    }
    // spills: written as array formulas (anchor) + cached values for the covered cells
    std::map<std::pair<int, int>, const Array*> spill_cells;   // covered (non-anchor) cell -> array
    for (auto& [k, arr] : ws.spills) for (int i = 0; i < arr->rows; ++i) for (int j = 0; j < arr->cols; ++j) if (i || j) spill_cells[{k.first + i, k.second + j}] = arr.get();
    auto spill_it = spill_cells.begin();
    x += "<sheetData>";
    int nrows = (int)ws.rows.size();
    if (!spill_cells.empty()) nrows = std::max(nrows, spill_cells.rbegin()->first.first + 1);
    for (int r = 0; r < nrows; ++r) {
        const RowStore* rs = r < (int)ws.rows.size() ? &ws.rows[r] : nullptr;
        bool row_attr = rs && (rs->height >= 0 || rs->hidden || rs->filtered || rs->style || rs->outline);
        bool any = false;
        if (rs) for (auto& c : rs->cells) if (!c.blank() || c.style) { any = true; break; }
        bool has_spill = spill_it != spill_cells.end() && spill_it->first.first == r;
        if (!any && !row_attr && !has_spill) continue;
        std::string rn = std::to_string(r + 1);
        x += "<row r=\"" + rn + "\"";
        if (rs && rs->height >= 0) x += " ht=\"" + num_text(rs->height) + "\" customHeight=\"1\"";
        if (rs && (rs->hidden || rs->filtered)) x += " hidden=\"1\"";
        if (rs && rs->style) x += " s=\"" + std::to_string(sw.xf(rs->style)) + "\" customFormat=\"1\"";
        if (rs && rs->outline) x += " outlineLevel=\"" + std::to_string(rs->outline) + "\"";
        x += ">";
        int nc = rs ? (int)rs->cells.size() : 0;
        int maxc = nc;
        for (auto it = spill_it; it != spill_cells.end() && it->first.first == r; ++it) maxc = std::max(maxc, it->first.second + 1);
        for (int c = 0; c < maxc; ++c) {
            const Cell* cl = c < nc ? &rs->cells[c] : nullptr;
            const Value* spill_v = nullptr;
            while (spill_it != spill_cells.end() && (spill_it->first.first < r || (spill_it->first.first == r && spill_it->first.second < c))) ++spill_it;
            if (spill_it != spill_cells.end() && spill_it->first == std::make_pair(r, c)) {
                auto [ar, ac] = ws.spill_anchor(r, c);
                spill_v = &spill_it->second->at(r - ar, c - ac);
            }
            if ((!cl || (cl->blank() && !cl->style)) && !spill_v) continue;
            std::string ref = col_name(c) + rn;
            int xf = cl && cl->style ? sw.xf(cl->style) : 0;
            std::string sattr = xf ? " s=\"" + std::to_string(xf) + "\"" : "";
            const Value& v = cl && (cl->f || !cl->v.is_empty()) ? cl->v : (spill_v ? *spill_v : cl->v);
            std::string tattr, vtext, ftext;
            if (cl && cl->f) {
                std::string ft = formula_text(*cl->f, b, si, r, c);
                ft = add_xlfn(ft);
                auto sp = ws.spills.find({r, c});
                if (sp != ws.spills.end()) {
                    Range a(r, c, r + sp->second->rows - 1, c + sp->second->cols - 1);
                    ftext = "<f t=\"array\" ref=\"" + range_name(a) + "\">" + esc(ft, false) + "</f>";
                } else ftext = "<f>" + esc(ft, false) + "</f>";
            }
            switch (v.type) {
            case Value::Empty: break;
            case Value::Number: vtext = num_text(v.num); break;
            case Value::Bool: tattr = " t=\"b\""; vtext = v.num != 0 ? "1" : "0"; break;
            case Value::Error:
                if (v.err == Err::Circ) { vtext = "0"; break; }
                tattr = " t=\"e\""; vtext = err_text(v.err); break;
            case Value::String:
                if (cl && cl->f) { tattr = " t=\"str\""; vtext = esc(v.str, false); }
                else { tattr = " t=\"s\""; vtext = std::to_string(sst.add(v.str)); }
                break;
            }
            x += "<c r=\"" + ref + "\"" + sattr + tattr;
            if (ftext.empty() && vtext.empty() && !(v.type == Value::String)) { x += "/>"; continue; }
            x += ">" + ftext;
            if (!vtext.empty() || v.type == Value::String) x += "<v>" + vtext + "</v>";
            x += "</c>";
        }
        x += "</row>";
    }
    x += "</sheetData>";
    if (ws.autofilter.active()) {
        x += "<autoFilter ref=\"" + range_name(ws.autofilter.range) + "\"";
        bool anycol = false;
        for (auto& fc : ws.autofilter.cols) if (fc.active()) anycol = true;
        if (!anycol) x += "/>";
        else {
            x += ">";
            static const char* fop[] = {"equal", "equal", "equal", "notEqual", "greaterThan", "lessThan", "greaterThanOrEqual", "lessThanOrEqual"};
            for (auto& fc : ws.autofilter.cols) {
                if (!fc.active()) continue;
                x += "<filterColumn colId=\"" + std::to_string(fc.col) + "\">";
                if (fc.use_custom) {
                    x += "<customFilters";
                    if (fc.and_ && !fc.v2.empty()) x += " and=\"1\"";
                    x += "><customFilter operator=\"" + std::string(fop[(int)fc.op1]) + "\" val=\"" + esc(fc.v1) + "\"/>";
                    if (!fc.v2.empty()) x += "<customFilter operator=\"" + std::string(fop[(int)fc.op2]) + "\" val=\"" + esc(fc.v2) + "\"/>";
                    x += "</customFilters>";
                } else {
                    x += "<filters";
                    if (fc.blanks) x += " blank=\"1\"";
                    x += ">";
                    for (auto& v : fc.values) x += "<filter val=\"" + esc(v) + "\"/>";
                    x += "</filters>";
                }
                x += "</filterColumn>";
            }
            x += "</autoFilter>";
        }
    }
    if (!ws.merges.empty()) {
        x += "<mergeCells count=\"" + std::to_string(ws.merges.size()) + "\">";
        for (auto& m : ws.merges) x += "<mergeCell ref=\"" + range_name(m) + "\"/>";
        x += "</mergeCells>";
    }
    for (auto& cf : ws.cond) {
        if (cf.ranges.empty() || cf.rules.empty()) continue;
        x += "<conditionalFormatting sqref=\"" + sqref_text(cf.ranges) + "\">";
        std::string tl = cell_name(cf.ranges[0].r0, cf.ranges[0].c0);
        for (auto& r : cf.rules) {
            x += std::string("<cfRule type=\"") + kCfNames[(int)r.type] + "\"";
            if (r.dxf >= 0 && r.type != CondRule::ColorScale && r.type != CondRule::DataBar) x += " dxfId=\"" + std::to_string(r.dxf) + "\"";
            x += " priority=\"" + std::to_string(r.priority) + "\"";
            if (r.stop) x += " stopIfTrue=\"1\"";
            if (r.type == CondRule::CellIs) x += std::string(" operator=\"") + kOpNames[(int)r.op] + "\"";
            if (r.type == CondRule::ContainsText) x += " operator=\"containsText\" text=\"" + esc(r.text) + "\"";
            if (r.type == CondRule::NotContainsText) x += " operator=\"notContains\" text=\"" + esc(r.text) + "\"";
            if (r.type == CondRule::BeginsWith) x += " operator=\"beginsWith\" text=\"" + esc(r.text) + "\"";
            if (r.type == CondRule::EndsWith) x += " operator=\"endsWith\" text=\"" + esc(r.text) + "\"";
            if (r.type == CondRule::Top10) { x += " rank=\"" + std::to_string(r.rank) + "\""; if (r.bottom) x += " bottom=\"1\""; if (r.percent) x += " percent=\"1\""; }
            if (r.type == CondRule::AboveAverage) { if (r.below) x += " aboveAverage=\"0\""; if (r.equal_avg) x += " equalAverage=\"1\""; }
            x += ">";
            auto cfvo = [&](const CfValueObject& o) {
                std::string s = std::string("<cfvo type=\"") + kCfvoNames[(int)o.type] + "\"";
                if (o.type != CfValueObject::Min && o.type != CfValueObject::Max) s += " val=\"" + esc(o.val) + "\"";
                return s + "/>";
            };
            if (r.type == CondRule::ColorScale) {
                x += "<colorScale>";
                for (auto& o : r.cfvo) x += cfvo(o);
                for (auto& o : r.cfvo) x += "<color rgb=\"" + hex_argb(o.color) + "\"/>";
                x += "</colorScale>";
            } else if (r.type == CondRule::DataBar) {
                x += "<dataBar>";
                for (auto& o : r.cfvo) x += cfvo(o);
                x += "<color rgb=\"" + hex_argb(r.bar_color) + "\"/></dataBar>";
            } else if (r.type == CondRule::CellIs || r.type == CondRule::Expression) {
                if (!r.f1.empty()) x += "<formula>" + esc(r.f1[0] == '=' ? r.f1.substr(1) : r.f1, false) + "</formula>";
                if (!r.f2.empty()) x += "<formula>" + esc(r.f2[0] == '=' ? r.f2.substr(1) : r.f2, false) + "</formula>";
            } else {
                std::string f = cf_rule_formula_for_text(r, tl);
                if (!f.empty()) x += "<formula>" + esc(f, false) + "</formula>";
            }
            x += "</cfRule>";
        }
        x += "</conditionalFormatting>";
    }
    if (!ws.validations.empty()) {
        x += "<dataValidations count=\"" + std::to_string(ws.validations.size()) + "\">";
        for (auto& dv : ws.validations) {
            x += std::string("<dataValidation type=\"") + kDvNames[(int)dv.type] + "\"";
            if (dv.err_style != DataValidation::Stop) x += std::string(" errorStyle=\"") + kDvErr[(int)dv.err_style] + "\"";
            if (dv.type != DataValidation::List && dv.type != DataValidation::Custom && dv.type != DataValidation::Any && dv.op != CmpOp::Between) x += std::string(" operator=\"") + kOpNames[(int)dv.op] + "\"";
            if (dv.allow_blank) x += " allowBlank=\"1\"";
            if (dv.type == DataValidation::List && !dv.dropdown) x += " showDropDown=\"1\"";   // attribute means "hide"
            if (dv.show_input) x += " showInputMessage=\"1\"";
            if (dv.show_error) x += " showErrorMessage=\"1\"";
            if (!dv.error_title.empty()) x += " errorTitle=\"" + esc(dv.error_title) + "\"";
            if (!dv.error.empty()) x += " error=\"" + esc(dv.error) + "\"";
            if (!dv.prompt_title.empty()) x += " promptTitle=\"" + esc(dv.prompt_title) + "\"";
            if (!dv.prompt.empty()) x += " prompt=\"" + esc(dv.prompt) + "\"";
            x += " sqref=\"" + sqref_text(dv.ranges) + "\">";
            auto fx = [](const std::string& f) { return f.empty() || f[0] != '=' ? f : f.substr(1); };
            if (!dv.f1.empty()) x += "<formula1>" + esc(fx(dv.f1), false) + "</formula1>";
            if (!dv.f2.empty()) x += "<formula2>" + esc(fx(dv.f2), false) + "</formula2>";
            x += "</dataValidation>";
        }
        x += "</dataValidations>";
    }
    x += "<pageMargins left=\"0.7\" right=\"0.7\" top=\"0.75\" bottom=\"0.75\" header=\"0.3\" footer=\"0.3\"/>";
    x += "</worksheet>";
    return x;
}

} // namespace

std::vector<uint8_t> xlsx_write(const Book& b, int level) {
    StyleWriter sw(b);
    SST sst;
    std::vector<std::pair<std::string, std::string>> e;
    int n = b.sheet_count();
    std::vector<std::string> sheet_parts;
    for (int i = 0; i < n; ++i) sheet_parts.push_back(sheet_xml(b, i, sw, sst));
    std::string ct = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
        "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
        "<Override PartName=\"/xl/sharedStrings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml\"/>"
        "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>"
        "<Override PartName=\"/docProps/app.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/>";
    for (int i = 0; i < n; ++i)
        ct += "<Override PartName=\"/xl/worksheets/sheet" + std::to_string(i + 1) + ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>";
    ct += "</Types>";
    e.push_back({"[Content_Types].xml", ct});
    e.push_back({"_rels/.rels", "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/>"
        "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties\" Target=\"docProps/app.xml\"/>"
        "</Relationships>"});
    e.push_back({"docProps/core.xml", "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:dcterms=\"http://purl.org/dc/terms/\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"><dc:creator>PCB Plotter</dc:creator></cp:coreProperties>"});
    e.push_back({"docProps/app.xml", "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\"><Application>PCB Plotter</Application></Properties>"});
    std::string w = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">";
    w += b.date1904 ? "<workbookPr date1904=\"1\"/>" : "<workbookPr/>";
    w += "<bookViews><workbookView activeTab=\"" + std::to_string(std::clamp(b.active_sheet, 0, n - 1)) + "\"/></bookViews><sheets>";
    std::string rels = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
    for (int i = 0; i < n; ++i) {
        std::string id = std::to_string(i + 1);
        std::string nm = b.sheets[i].name.empty() ? "Sheet" + id : b.sheets[i].name;
        w += "<sheet name=\"" + esc(nm.substr(0, 31)) + "\" sheetId=\"" + id + "\"";
        if (b.sheets[i].hidden) w += " state=\"hidden\"";
        w += " r:id=\"rId" + id + "\"/>";
        rels += "<Relationship Id=\"rId" + id + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet" + id + ".xml\"/>";
    }
    w += "</sheets>";
    std::string dn;
    for (int i = 0; i < n; ++i) {
        const Worksheet& ws = b.sheets[i];
        if (!ws.autofilter.active()) continue;
        const Range& g = ws.autofilter.range;
        dn += "<definedName name=\"_xlnm._FilterDatabase\" localSheetId=\"" + std::to_string(i) + "\" hidden=\"1\">" +
              esc(quote_sheet_name(ws.name) + "!" + cell_name(g.r0, g.c0, true, true) + ":" + cell_name(g.r1, g.c1, true, true), false) + "</definedName>";
    }
    for (auto& d : b.names) {
        dn += "<definedName name=\"" + esc(d.name) + "\"";
        if (d.scope >= 0) dn += " localSheetId=\"" + std::to_string(d.scope) + "\"";
        if (d.hidden) dn += " hidden=\"1\"";
        dn += ">" + esc(add_xlfn(d.formula), false) + "</definedName>";
    }
    if (!dn.empty()) w += "<definedNames>" + dn + "</definedNames>";
    w += "<calcPr calcId=\"191029\"/></workbook>";
    rels += "<Relationship Id=\"rId" + std::to_string(n + 1) + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>";
    rels += "<Relationship Id=\"rId" + std::to_string(n + 2) + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings\" Target=\"sharedStrings.xml\"/>";
    rels += "</Relationships>";
    e.push_back({"xl/workbook.xml", w});
    e.push_back({"xl/_rels/workbook.xml.rels", rels});
    e.push_back({"xl/styles.xml", sw.xml()});
    std::string ss = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" count=\"" +
                     std::to_string(sst.count) + "\" uniqueCount=\"" + std::to_string(sst.v.size()) + "\">";
    for (auto* s : sst.v) ss += "<si>" + text_elem(*s) + "</si>";
    ss += "</sst>";
    e.push_back({"xl/sharedStrings.xml", std::move(ss)});
    for (int i = 0; i < n; ++i) e.push_back({"xl/worksheets/sheet" + std::to_string(i + 1) + ".xml", std::move(sheet_parts[i])});
    return zip_write(e, level);
}

// ===========================================================================
// XLSX read
// ===========================================================================
namespace {

std::string resolve(const std::string& base_dir, const std::string& target) {
    if (!target.empty() && target[0] == '/') return target.substr(1);
    std::string t = base_dir + target;
    std::vector<std::string> parts;
    size_t p = 0;
    while (p <= t.size()) {
        size_t k = t.find('/', p);
        if (k == std::string::npos) k = t.size();
        std::string seg = t.substr(p, k - p);
        if (seg == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!seg.empty() && seg != ".") parts.push_back(seg);
        p = k + 1;
    }
    std::string o;
    for (auto& s : parts) { if (!o.empty()) o += '/'; o += s; }
    return o;
}

struct ThemeColors {
    Argb c[12] = {0xFFFFFFFF, 0xFF000000, 0xFFE7E6E6, 0xFF44546A, 0xFF4472C4, 0xFFED7D31, 0xFFA5A5A5, 0xFFFFC000, 0xFF5B9BD5, 0xFF70AD47, 0xFF0563C1, 0xFF954F72};
    void load(const std::string& xml) {
        XmlNode root; std::string err;
        if (!xml_parse(xml, root, err)) return;
        const XmlNode* cs = root.find("clrScheme");
        if (!cs) return;
        // file order dk1 lt1 dk2 lt2 accent1..6 hlink folHlink; SpreadsheetML theme index swaps 0<->1 and 2<->3
        static const char* names[] = {"lt1", "dk1", "lt2", "dk2", "accent1", "accent2", "accent3", "accent4", "accent5", "accent6", "hlink", "folHlink"};
        for (int i = 0; i < 12; ++i) {
            const XmlNode* n = cs->child(names[i]);
            if (!n || n->children.empty()) continue;
            const XmlNode& v = n->children[0];
            std::string hx = v.local() == "sysClr" ? v.attr("lastClr") : v.attr("val");
            if (hx.size() == 6) c[i] = 0xFF000000u | (Argb)strtoul(hx.c_str(), nullptr, 16);
        }
    }
};

struct ColorCtx { ThemeColors theme; Argb indexed[66]; ColorCtx() { std::memcpy(indexed, kIndexed, sizeof indexed); } };

Argb read_color(const XmlNode* n, const ColorCtx& cc) {
    if (!n) return 0;
    if (n->has_attr("rgb")) return opaque_rgb(n->attr("rgb"));
    double tint = n->attr_num("tint", 0);
    if (n->has_attr("theme")) { int t = (int)n->attr_num("theme"); if (t >= 0 && t < 12) return apply_tint(cc.theme.c[t], tint); }
    if (n->has_attr("indexed")) { int i = (int)n->attr_num("indexed"); if (i >= 0 && i < 64) return apply_tint(cc.indexed[i], tint); }
    return 0;
}

struct StylesIn {
    std::vector<Style> xf_styles;   // cellXfs index -> Style
    std::vector<Dxf> dxfs;
};

Border read_border(const XmlNode& b, const ColorCtx& cc) {
    Border o;
    auto side = [&](const char* nm, BorderSide& s) {
        const XmlNode* n = b.child(nm);
        if (!n) return;
        s.style = (BorderStyle)name_index(kBorderNames, n->attr("style", "none"));
        s.color = read_color(n->child("color"), cc);
    };
    side("left", o.left); side("right", o.right); side("top", o.top); side("bottom", o.bottom); side("diagonal", o.diagonal);
    if (o.left.style == BorderStyle::None) side("start", o.left);
    if (o.right.style == BorderStyle::None) side("end", o.right);
    o.diag_up = b.attr("diagonalUp") == "1" || b.attr("diagonalUp") == "true";
    o.diag_down = b.attr("diagonalDown") == "1" || b.attr("diagonalDown") == "true";
    return o;
}
Font read_font(const XmlNode& f, const ColorCtx& cc) {
    Font o;
    auto flag = [&](const char* nm) { const XmlNode* n = f.child(nm); if (!n) return false; std::string v = n->attr("val", "1"); return v != "0" && v != "false"; };
    o.bold = flag("b"); o.italic = flag("i"); o.strike = flag("strike");
    if (const XmlNode* u = f.child("u")) o.underline = (Underline)name_index(kUNames, u->attr("val", "single"), 1);
    if (const XmlNode* s = f.child("sz")) o.size = s->attr_num("val", 11);
    if (const XmlNode* n = f.child("name")) o.name = n->attr("val", "Calibri");
    o.color = read_color(f.child("color"), cc);
    if (o.color == 0xFF000000 && f.child("color") && f.child("color")->has_attr("theme")) o.color = 0;   // theme text colour = automatic
    return o;
}

void read_styles(const std::string& xml, const ColorCtx& cc, StylesIn& out) {
    XmlNode root; std::string err;
    if (!xml_parse(xml, root, err)) return;
    std::map<int, std::string> nf;
    if (const XmlNode* nfs = root.child("numFmts")) for (auto& n : nfs->children) if (n.local() == "numFmt") nf[(int)n.attr_num("numFmtId")] = n.attr("formatCode");
    auto fmt_of = [&](int id) { auto it = nf.find(id); if (it != nf.end()) return it->second; std::string b = builtin_numfmt(id); return b.empty() ? std::string("General") : b; };
    std::vector<Font> fonts; std::vector<Fill> fills; std::vector<Border> borders;
    if (const XmlNode* x = root.child("fonts")) for (auto& n : x->children) if (n.local() == "font") fonts.push_back(read_font(n, cc));
    if (const XmlNode* x = root.child("fills")) for (auto& n : x->children) if (n.local() == "fill") {
        Fill f;
        if (const XmlNode* p = n.child("patternFill")) {
            f.pattern = (FillPattern)name_index(kFillNames, p->attr("patternType", p->child("fgColor") ? "solid" : "none"));
            f.fg = read_color(p->child("fgColor"), cc);
            f.bg = read_color(p->child("bgColor"), cc);
        } else if (const XmlNode* g = n.child("gradientFill")) {
            std::vector<const XmlNode*> stops; g->find_all("color", stops);
            if (!stops.empty()) { f.pattern = FillPattern::Solid; f.fg = read_color(stops[0], cc); }
        }
        fills.push_back(f);
    }
    if (const XmlNode* x = root.child("borders")) for (auto& n : x->children) if (n.local() == "border") borders.push_back(read_border(n, cc));
    if (const XmlNode* x = root.child("cellXfs")) for (auto& n : x->children) {
        if (n.local() != "xf") continue;
        Style s;
        size_t fo = (size_t)n.attr_num("fontId"), fi = (size_t)n.attr_num("fillId"), bo = (size_t)n.attr_num("borderId");
        if (fo < fonts.size()) s.font = fonts[fo];
        if (fi < fills.size()) s.fill = fills[fi];
        if (s.fill.pattern == FillPattern::Gray125 && fi == 1) s.fill = Fill();
        if (bo < borders.size()) s.border = borders[bo];
        s.numfmt = fmt_of((int)n.attr_num("numFmtId"));
        if (const XmlNode* a = n.child("alignment")) {
            s.halign = (HAlign)name_index(kHNames, a->attr("horizontal", "general"));
            s.valign = (VAlign)name_index(kVNames, a->attr("vertical", "bottom"));
            s.wrap = a->attr("wrapText") == "1" || a->attr("wrapText") == "true";
            s.shrink = a->attr("shrinkToFit") == "1" || a->attr("shrinkToFit") == "true";
            s.indent = (uint8_t)a->attr_num("indent");
            int rot = (int)a->attr_num("textRotation");
            s.rotation = (int16_t)(rot > 90 && rot <= 180 ? 90 - rot : rot);
        }
        if (const XmlNode* p = n.child("protection")) { s.locked = p->attr("locked", "1") != "0"; s.hidden = p->attr("hidden") == "1"; }
        out.xf_styles.push_back(s);
    }
    if (const XmlNode* x = root.child("dxfs")) for (auto& n : x->children) {
        if (n.local() != "dxf") continue;
        Dxf d;
        if (const XmlNode* f = n.child("font")) {
            auto flag = [&](const char* nm, std::optional<bool>& o) { if (const XmlNode* k = f->child(nm)) { std::string v = k->attr("val", "1"); o = v != "0" && v != "false"; } };
            flag("b", d.bold); flag("i", d.italic); flag("strike", d.strike);
            if (const XmlNode* u = f->child("u")) d.underline = (Underline)name_index(kUNames, u->attr("val", "single"), 1);
            if (const XmlNode* c = f->child("color")) { Argb a = read_color(c, cc); if (a) d.font_color = a; }
        }
        if (const XmlNode* f = n.child("numFmt")) d.numfmt = f->attr("formatCode");
        if (const XmlNode* f = n.child("fill")) if (const XmlNode* p = f->child("patternFill")) {
            Argb a = read_color(p->child("bgColor"), cc);
            if (!a) a = read_color(p->child("fgColor"), cc);
            if (a) d.fill_color = a;
        }
        if (const XmlNode* b = n.child("border")) d.border = read_border(*b, cc);
        out.dxfs.push_back(d);
    }
}

struct PendingFormula {
    int si, r, c;
    std::string text;
    int shared = -1;       // shared index
    bool master = false;
    bool array = false;
    Range ref;
    Value cached;
    bool has_cached = false;
};

void read_sst(std::string_view xml, std::vector<std::string>& sst) {
    xmlp::Pull p(xml);
    int in_si = 0, in_rph = 0, in_t = 0;
    std::string cur;
    for (;;) {
        auto e = p.next();
        if (e == xmlp::Pull::Eof) break;
        if (e == xmlp::Pull::Start) {
            auto l = p.local();
            if (l == "si") { in_si = 1; cur.clear(); }
            else if (l == "rPh") ++in_rph;
            else if (l == "t" && in_si && !in_rph) { if (p.empty_element()) { p.next(); continue; } in_t = 1; }
        } else if (e == xmlp::Pull::End) {
            auto l = p.local();
            if (l == "si") { sst.push_back(cur); in_si = 0; }
            else if (l == "rPh") --in_rph;
            else if (l == "t") in_t = 0;
        } else if (e == xmlp::Pull::Text && in_t) cur += p.text();
    }
}

void read_sheet(std::string_view xml, Book& b, int si, const std::vector<std::string>& sst, const std::vector<uint32_t>& xf_map,
                const std::vector<Style>& xf_styles, std::vector<PendingFormula>& pf) {
    (void)xf_styles;
    Worksheet& ws = b.sheets[si];
    xmlp::Pull p(xml);
    int row = -1, col = -1;
    CondFormat* cur_cf = nullptr;
    CondRule* cur_rule = nullptr;
    DataValidation* cur_dv = nullptr;
    FilterColumn* cur_fc = nullptr;
    int in_cf_kind = 0;   // 1 colorScale, 2 dataBar
    int custom_n = 0;
    int color_i = 0;
    auto xf_style = [&](int xf) -> uint32_t { return xf >= 0 && xf < (int)xf_map.size() ? xf_map[xf] : 0; };
    for (;;) {
        auto e = p.next();
        if (e == xmlp::Pull::Eof) break;
        if (e == xmlp::Pull::End) {
            auto l = p.local();
            if (l == "cfRule") cur_rule = nullptr;
            else if (l == "conditionalFormatting") cur_cf = nullptr;
            else if (l == "dataValidation") cur_dv = nullptr;
            else if (l == "filterColumn") cur_fc = nullptr;
            else if (l == "colorScale" || l == "dataBar") in_cf_kind = 0;
            continue;
        }
        if (e != xmlp::Pull::Start) continue;
        auto l = p.local();
        if (l == "row") {
            row = p.attr("r") ? (int)p.attr_num("r") - 1 : row + 1;
            col = -1;
            if (row < 0 || row >= MAX_ROWS) { p.skip_element(); continue; }
            bool custom_h = p.attr_bool("customHeight");
            bool hidden = p.attr_bool("hidden");
            int s = p.attr_bool("customFormat") ? (int)p.attr_num("s", 0) : 0;
            int ol = (int)p.attr_num("outlineLevel", 0);
            if (custom_h || hidden || s || ol) {
                RowStore& rs = ws.row(row);
                if (custom_h && p.attr("ht")) rs.height = (float)p.attr_num("ht");
                rs.hidden = hidden;
                rs.style = xf_style(s);
                rs.outline = (uint8_t)ol;
            }
            continue;
        }
        if (l == "c") {
            int r = row, c = col + 1;
            if (const std::string* ref = p.attr("r")) parse_a1(*ref, r, c);
            col = c;
            if (r < 0 || c < 0) { p.skip_element(); continue; }
            std::string t = p.attr_or("t", "n");
            int s = (int)p.attr_num("s", 0);
            bool empty = p.empty_element();
            std::string vtext, ftext, istext;
            bool has_v = false, has_f = false;
            PendingFormula F;
            if (!empty) {
                int depth = 1;
                while (depth > 0) {
                    auto e2 = p.next();
                    if (e2 == xmlp::Pull::Eof) break;
                    if (e2 == xmlp::Pull::End) { --depth; continue; }
                    if (e2 != xmlp::Pull::Start) continue;
                    auto l2 = p.local();
                    if (l2 == "v") { vtext = p.read_text(); has_v = true; }
                    else if (l2 == "f") {
                        has_f = true;
                        std::string ft = p.attr_or("t");
                        if (ft == "shared") { F.shared = (int)p.attr_num("si", -1); if (p.attr("ref")) F.master = true; }
                        if (ft == "array") { F.array = true; std::string rr = p.attr_or("ref"); Range g; if (parse_range_name(rr, g)) F.ref = g; }
                        ftext = p.read_text();
                        if (F.shared >= 0 && !ftext.empty()) F.master = true;
                    } else if (l2 == "is") {
                        // inline string: gather <t> (skip rPh)
                        int d2 = 1, rph = 0;
                        while (d2 > 0) {
                            auto e3 = p.next();
                            if (e3 == xmlp::Pull::Eof) break;
                            if (e3 == xmlp::Pull::Start) { ++d2; if (p.local() == "rPh") ++rph; else if (p.local() == "t" && !rph) { istext += p.read_text(); --d2; } }
                            else if (e3 == xmlp::Pull::End) { --d2; if (p.local() == "rPh") --rph; }
                        }
                    } else { p.skip_element(); }
                }
            }
            Cell& cl = ws.at(r, c);
            cl.style = xf_style(s);
            Value v;
            if (t == "inlineStr") v = Value::text(istext);
            else if (has_v) {
                if (t == "s") { size_t k = (size_t)atol(vtext.c_str()); v = Value::text(k < sst.size() ? sst[k] : std::string()); }
                else if (t == "b") v = Value::boolean(vtext == "1" || vtext == "true");
                else if (t == "e") { Err er = err_from_text(vtext); v = er != Err::None ? Value::error(er) : Value::error(Err::Value); }
                else if (t == "str") v = Value::text(vtext);
                else if (t == "d") { Value pv = parse_input(vtext); v = pv.is_num() ? pv : Value::text(vtext); }
                else { char* end = nullptr; double d = strtod(vtext.c_str(), &end); v = (end && *end == 0 && !vtext.empty()) ? Value::number(d) : Value::text(vtext); }
            }
            cl.v = v;
            if (has_f) {
                F.si = si; F.r = r; F.c = c; F.text = ftext; F.cached = v;
                F.has_cached = (has_v && (!vtext.empty() || t == "str")) || t == "inlineStr";   // openpyxl writes <v></v>
                if (!F.has_cached) cl.v = Value();
                pf.push_back(std::move(F));
            }
            continue;
        }
        if (l == "tabColor") { if (p.attr("rgb")) ws.tab_color = opaque_rgb(p.attr_or("rgb")); continue; }
        if (l == "sheetView") {
            ws.show_grid = p.attr_or("showGridLines", "1") != "0";
            ws.zoom = (int)p.attr_num("zoomScale", 100);
            continue;
        }
        if (l == "pane") {
            std::string st = p.attr_or("state");
            if (st == "frozen" || st == "frozenSplit") { ws.freeze_cols = (int)p.attr_num("xSplit", 0); ws.freeze_rows = (int)p.attr_num("ySplit", 0); }
            continue;
        }
        if (l == "selection") { int r, c; std::string ac = p.attr_or("activeCell"); if (parse_a1(ac, r, c)) { ws.active_r = r; ws.active_c = c; } continue; }
        if (l == "sheetFormatPr") {
            if (p.attr("defaultRowHeight")) ws.default_row_height = p.attr_num("defaultRowHeight", 15);
            if (p.attr("defaultColWidth")) ws.default_col_width = p.attr_num("defaultColWidth", 8.43);
            else if (p.attr("baseColWidth")) ws.default_col_width = p.attr_num("baseColWidth", 8) + 0.43;
            continue;
        }
        if (l == "col") {
            int mn = (int)p.attr_num("min", 1) - 1, mx = (int)p.attr_num("max", 1) - 1;
            if (mn < 0 || mx < mn) continue;
            mx = std::min(mx, MAX_COLS - 1);
            bool hidden = p.attr_bool("hidden");
            double w = p.attr("width") ? p.attr_num("width") : -1;
            bool custom = p.attr_bool("customWidth") || w >= 0;
            uint32_t st = xf_style((int)p.attr_num("style", 0));
            int ol = (int)p.attr_num("outlineLevel", 0);
            if (mx - mn > 1000 && !hidden && !st) mx = mn + 1000;   // "all columns" ranges: keep it bounded
            for (int c = mn; c <= mx; ++c) {
                ColInfo& ci = ws.col(c);
                ci.width = custom ? w : -1;
                ci.hidden = hidden; ci.style = st; ci.outline = (uint8_t)ol;
            }
            continue;
        }
        if (l == "mergeCell") { Range g; std::string rr = p.attr_or("ref"); if (parse_range_name(rr, g)) ws.merges.push_back(g); continue; }
        if (l == "autoFilter") { Range g; if (parse_range_name(p.attr_or("ref"), g)) ws.autofilter.range = g; continue; }
        if (l == "filterColumn") { ws.autofilter.cols.push_back(FilterColumn()); cur_fc = &ws.autofilter.cols.back(); cur_fc->col = (int)p.attr_num("colId"); custom_n = 0; continue; }
        if (l == "filters" && cur_fc) { cur_fc->blanks = p.attr_bool("blank"); continue; }
        if (l == "filter" && cur_fc) { cur_fc->values.push_back(p.attr_or("val")); continue; }
        if (l == "customFilters" && cur_fc) { cur_fc->use_custom = true; cur_fc->and_ = p.attr_bool("and"); continue; }
        if (l == "customFilter" && cur_fc) {
            static const char* fop[] = {"", "", "equal", "notEqual", "greaterThan", "lessThan", "greaterThanOrEqual", "lessThanOrEqual"};
            CmpOp op = (CmpOp)std::max(2, name_index(fop, p.attr_or("operator", "equal"), 2));
            if (custom_n++ == 0) { cur_fc->op1 = op; cur_fc->v1 = p.attr_or("val"); }
            else { cur_fc->op2 = op; cur_fc->v2 = p.attr_or("val"); }
            continue;
        }
        if (l == "conditionalFormatting") { ws.cond.push_back(CondFormat()); cur_cf = &ws.cond.back(); cur_cf->ranges = parse_sqref(p.attr_or("sqref")); continue; }
        if (l == "cfRule" && cur_cf) {
            cur_cf->rules.push_back(CondRule());
            cur_rule = &cur_cf->rules.back();
            cur_rule->type = (CondRule::Type)name_index(kCfNames, p.attr_or("type"), 1);
            cur_rule->dxf = p.attr("dxfId") ? (int)p.attr_num("dxfId") : -1;
            cur_rule->priority = (int)p.attr_num("priority", 1);
            cur_rule->stop = p.attr_bool("stopIfTrue");
            cur_rule->op = (CmpOp)name_index(kOpNames, p.attr_or("operator", "equal"), 2);
            cur_rule->text = p.attr_or("text");
            cur_rule->rank = (int)p.attr_num("rank", 10);
            cur_rule->bottom = p.attr_bool("bottom");
            cur_rule->percent = p.attr_bool("percent");
            cur_rule->below = p.attr_or("aboveAverage", "1") == "0";
            cur_rule->equal_avg = p.attr_bool("equalAverage");
            color_i = 0;
            continue;
        }
        if (l == "formula" && cur_rule) { std::string f = p.read_text(); if (cur_rule->f1.empty()) cur_rule->f1 = f; else cur_rule->f2 = f; continue; }
        if (l == "colorScale") { in_cf_kind = 1; continue; }
        if (l == "dataBar") { in_cf_kind = 2; continue; }
        if (l == "cfvo" && cur_rule && in_cf_kind) {
            CfValueObject o;
            o.type = (CfValueObject::Type)name_index(kCfvoNames, p.attr_or("type", "min"));
            o.val = p.attr_or("val");
            cur_rule->cfvo.push_back(o);
            continue;
        }
        if (l == "color" && cur_rule && in_cf_kind) {
            Argb a = opaque_rgb(p.attr_or("rgb", "FF000000"));
            if (in_cf_kind == 1) { if (color_i < (int)cur_rule->cfvo.size()) cur_rule->cfvo[color_i].color = a; ++color_i; }
            else cur_rule->bar_color = a;
            continue;
        }
        if (l == "dataValidation") {
            ws.validations.push_back(DataValidation());
            cur_dv = &ws.validations.back();
            cur_dv->type = (DataValidation::Type)name_index(kDvNames, p.attr_or("type", "none"));
            cur_dv->op = (CmpOp)name_index(kOpNames, p.attr_or("operator", "between"));
            cur_dv->err_style = (DataValidation::ErrStyle)name_index(kDvErr, p.attr_or("errorStyle", "stop"));
            cur_dv->allow_blank = p.attr_bool("allowBlank");
            cur_dv->dropdown = !p.attr_bool("showDropDown");
            cur_dv->show_input = p.attr_bool("showInputMessage");
            cur_dv->show_error = p.attr_bool("showErrorMessage");
            cur_dv->prompt_title = p.attr_or("promptTitle"); cur_dv->prompt = p.attr_or("prompt");
            cur_dv->error_title = p.attr_or("errorTitle"); cur_dv->error = p.attr_or("error");
            cur_dv->ranges = parse_sqref(p.attr_or("sqref"));
            continue;
        }
        if (l == "formula1" && cur_dv) { cur_dv->f1 = p.read_text(); continue; }
        if (l == "formula2" && cur_dv) { cur_dv->f2 = p.read_text(); continue; }
        if (l == "sqref" && cur_dv) { cur_dv->ranges = parse_sqref(p.read_text()); continue; }   // x14 style
    }
    // drop empty CF / DV entries
    ws.cond.erase(std::remove_if(ws.cond.begin(), ws.cond.end(), [](const CondFormat& c) { return c.ranges.empty() || c.rules.empty(); }), ws.cond.end());
    ws.validations.erase(std::remove_if(ws.validations.begin(), ws.validations.end(), [](const DataValidation& d) { return d.ranges.empty(); }), ws.validations.end());
    // rows hidden by an active filter belong to the filter, not to manual hiding
    if (ws.autofilter.active()) {
        bool any = false;
        for (auto& fc : ws.autofilter.cols) if (fc.active()) any = true;
        if (any) for (int r = ws.autofilter.range.r0 + 1; r < (int)ws.rows.size(); ++r) if (ws.rows[r].hidden) { ws.rows[r].hidden = false; ws.rows[r].filtered = true; }
    }
}

} // namespace

bool xlsx_read(const std::vector<uint8_t>& data, Book& book, std::string& err) {
    std::vector<ArchiveEntry> ents;
    if (!read_zip(data.data(), data.size(), ents, err)) return false;
    std::map<std::string, const std::vector<uint8_t>*> files;
    for (auto& e : ents) {
        std::string k = e.name;
        for (auto& ch : k) ch = (char)tolower((unsigned char)ch);
        files[k] = &e.data;
    }
    auto get = [&](std::string name, std::string& out) {
        for (auto& ch : name) ch = (char)tolower((unsigned char)ch);
        auto it = files.find(name);
        if (it == files.end()) return false;
        out.assign(it->second->begin(), it->second->end());
        return true;
    };
    std::string wbx, relx;
    if (!get("xl/workbook.xml", wbx)) { err = "not an Excel workbook (xl/workbook.xml missing)"; return false; }
    get("xl/_rels/workbook.xml.rels", relx);
    XmlNode wroot, rroot;
    if (!xml_parse(wbx, wroot, err)) return false;
    std::map<std::string, std::string> rel;
    std::string sst_path = "xl/sharedStrings.xml", styles_path = "xl/styles.xml", theme_path = "xl/theme/theme1.xml";
    if (!relx.empty() && xml_parse(relx, rroot, err)) {
        std::vector<const XmlNode*> v; rroot.find_all("Relationship", v);
        for (auto* r : v) {
            std::string tgt = resolve("xl/", r->attr("Target")), ty = r->attr("Type");
            rel[r->attr("Id")] = tgt;
            if (ty.size() >= 13 && ty.compare(ty.size() - 13, 13, "sharedStrings") == 0) sst_path = tgt;
            else if (ty.size() >= 6 && ty.compare(ty.size() - 6, 6, "styles") == 0) styles_path = tgt;
            else if (ty.size() >= 5 && ty.compare(ty.size() - 5, 5, "theme") == 0) theme_path = tgt;
        }
    }
    Book b;
    b.sheets.clear();
    ColorCtx cc;
    std::string tmp;
    if (get(theme_path, tmp)) cc.theme.load(tmp);
    StylesIn sin;
    if (get(styles_path, tmp)) read_styles(tmp, cc, sin);
    std::vector<uint32_t> xf_map;
    for (auto& s : sin.xf_styles) xf_map.push_back(b.styles.intern(s));
    b.dxfs = sin.dxfs;
    std::vector<std::string> sst;
    if (get(sst_path, tmp)) read_sst(tmp, sst);
    if (const XmlNode* pr = wroot.find("workbookPr")) b.date1904 = pr->attr("date1904") == "1" || pr->attr("date1904") == "true";
    if (const XmlNode* bv = wroot.find("workbookView")) b.active_sheet = (int)bv->attr_num("activeTab", 0);
    std::vector<const XmlNode*> sheets;
    if (const XmlNode* ss = wroot.find("sheets")) for (auto& k : ss->children) if (k.local() == "sheet") sheets.push_back(&k);
    std::vector<std::string> paths;
    uint32_t id = 1;
    for (size_t i = 0; i < sheets.size(); ++i) {
        const XmlNode* sn = sheets[i];
        Worksheet ws;
        ws.name = sn->attr("name", "Sheet" + std::to_string(i + 1));
        ws.id = id++;
        ws.hidden = sn->attr("state", "visible") != "visible";
        std::string rid;
        for (auto& a : sn->attrs) { std::string_view k = a.first; if (k == "r:id" || (k.size() > 3 && k.substr(k.size() - 3) == ":id")) rid = a.second; }
        paths.push_back(rel.count(rid) ? rel[rid] : "xl/worksheets/sheet" + std::to_string(i + 1) + ".xml");
        b.sheets.push_back(std::move(ws));
    }
    if (b.sheets.empty()) { err = "workbook has no sheets"; return false; }
    std::vector<PendingFormula> pf;
    for (size_t i = 0; i < b.sheets.size(); ++i) {
        std::string sx;
        if (!get(paths[i], sx)) continue;   // chartsheet / missing part
        read_sheet(sx, b, (int)i, sst, xf_map, sin.xf_styles, pf);
    }
    // defined names
    if (const XmlNode* dn = wroot.find("definedNames"))
        for (auto& n : dn->children) {
            if (n.local() != "definedName") continue;
            std::string nm = n.attr("name");
            if (nm.rfind("_xlnm.", 0) == 0) continue;   // built-ins (filter database, print area) regenerate
            DefinedName d;
            d.name = nm;
            d.scope = n.has_attr("localSheetId") ? (int)n.attr_num("localSheetId") : -1;
            d.hidden = n.attr("hidden") == "1";
            d.formula = n.text;
            b.names.push_back(d);
        }
    // compile formulas now that every sheet exists
    std::map<std::pair<int, int>, std::pair<FormulaPtr, std::pair<int, int>>> shared;   // (si, shared index) -> formula, master pos
    bool need_calc = false;
    for (auto& F : pf) if (F.master || (F.shared < 0)) {
        auto f = std::make_shared<Formula>();
        std::string e2;
        compile_formula(F.text, b, F.si, F.r, F.c, *f, e2);
        if (F.array) { f->is_array = true; f->array_range = F.ref; }
        Cell& cl = b.sheets[F.si].at(F.r, F.c);
        cl.f = f;
        if (F.shared >= 0) shared[{F.si, F.shared}] = {f, {F.r, F.c}};
        if (!F.has_cached) need_calc = true;
        if (F.array && F.ref.valid() && (F.ref.rows() > 1 || F.ref.cols() > 1)) {
            // array results live in the anchor's spill; drop the cached copies in the covered cells
            Worksheet& ws = b.sheets[F.si];
            for (int r = F.ref.r0; r <= F.ref.r1; ++r)
                for (int c = F.ref.c0; c <= F.ref.c1; ++c)
                    if ((r != F.r || c != F.c) && r < (int)ws.rows.size() && c < (int)ws.rows[r].cells.size()) { Cell& x = ws.rows[r].cells[c]; if (!x.f) x.v = Value(); }
            need_calc = true;
        }
    }
    for (auto& F : pf) if (!F.master && F.shared >= 0) {
        auto it = shared.find({F.si, F.shared});
        Cell& cl = b.sheets[F.si].at(F.r, F.c);
        if (it != shared.end()) cl.f = it->second.first;   // relative offsets: identical code at every member
        if (!F.has_cached) need_calc = true;
    }
    for (auto& ws : b.sheets) for (auto& r : ws.rows) for (auto& c : r.cells) c.calc = 0;
    if (b.active_sheet < 0 || b.active_sheet >= b.sheet_count()) b.active_sheet = 0;
    b.invalidate_graph();
    if (need_calc) b.recalc_all();
    else b.calc().needs_full = false;   // trust cached values; the graph is built on the first edit
    b.clear_undo();
    book = std::move(b);
    return true;
}

// ===========================================================================
// OLE2 compound file
// ===========================================================================
namespace {
uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }

struct Ole {
    const std::vector<uint8_t>& d;
    uint32_t ss = 512, mss = 64, cutoff = 4096;
    std::vector<uint32_t> fat, minifat;
    std::vector<uint8_t> ministream;
    explicit Ole(const std::vector<uint8_t>& data) : d(data) {}
    const uint8_t* sec(uint32_t s) const {
        size_t off = (size_t)(s + 1) * ss;
        return off + ss <= d.size() ? d.data() + off : nullptr;
    }
    std::vector<uint8_t> chain(uint32_t start, size_t size_limit = SIZE_MAX) const {
        std::vector<uint8_t> o;
        uint32_t s = start;
        size_t guard = 0;
        while (s < 0xFFFFFFFA && s < fat.size() && guard++ < fat.size() + 1) {
            const uint8_t* p = sec(s);
            if (!p) break;
            o.insert(o.end(), p, p + ss);
            if (o.size() >= size_limit) break;
            s = fat[s];
        }
        if (o.size() > size_limit) o.resize(size_limit);
        return o;
    }
    std::vector<uint8_t> minichain(uint32_t start, size_t size) const {
        std::vector<uint8_t> o;
        uint32_t s = start;
        size_t guard = 0;
        while (s < 0xFFFFFFFA && s < minifat.size() && guard++ < minifat.size() + 1) {
            size_t off = (size_t)s * mss;
            if (off + mss > ministream.size()) break;
            o.insert(o.end(), ministream.begin() + off, ministream.begin() + off + mss);
            if (o.size() >= size) break;
            s = minifat[s];
        }
        if (o.size() > size) o.resize(size);
        return o;
    }
    bool open(std::string& err) {
        if (d.size() < 512 || !is_ole2_bytes(d)) { err = "not an OLE2 compound file"; return false; }
        ss = 1u << rd16(&d[0x1E]);
        mss = 1u << rd16(&d[0x20]);
        if (ss < 128 || ss > 65536) { err = "bad sector size"; return false; }
        uint32_t nfat = rd32(&d[0x2C]);
        cutoff = rd32(&d[0x38]);
        std::vector<uint32_t> difat;
        for (int i = 0; i < 109; ++i) difat.push_back(rd32(&d[0x4C + i * 4]));
        uint32_t ds = rd32(&d[0x44]), nd = rd32(&d[0x48]);
        for (uint32_t k = 0; k < nd && ds < 0xFFFFFFFA; ++k) {
            const uint8_t* p = sec(ds);
            if (!p) break;
            for (uint32_t i = 0; i + 1 < ss / 4; ++i) difat.push_back(rd32(p + i * 4));
            ds = rd32(p + ss - 4);
        }
        for (uint32_t i = 0; i < nfat && i < difat.size(); ++i) {
            const uint8_t* p = sec(difat[i]);
            if (!p) { err = "truncated FAT"; return false; }
            for (uint32_t k = 0; k < ss / 4; ++k) fat.push_back(rd32(p + k * 4));
        }
        auto mf = chain(rd32(&d[0x3C]), (size_t)rd32(&d[0x40]) * ss);
        for (size_t i = 0; i + 4 <= mf.size(); i += 4) minifat.push_back(rd32(&mf[i]));
        return true;
    }
    bool stream(const char* const* names, std::vector<uint8_t>& out, std::string& err) {
        auto dir = chain(rd32(&d[0x30]));
        uint32_t root_start = 0xFFFFFFFE; uint64_t root_size = 0;
        int found = -1;
        for (size_t i = 0; i + 128 <= dir.size(); i += 128) {
            const uint8_t* e = &dir[i];
            int nl = rd16(e + 0x40);
            std::string name;
            for (int k = 0; k + 2 <= nl - 2 && k < 64; k += 2) name += (char)e[k];
            uint8_t type = e[0x42];
            if (type == 5) { root_start = rd32(e + 0x74); root_size = rd32(e + 0x78); }
            if (type == 2) for (int n = 0; names[n]; ++n) if (name == names[n] && found < 0) found = (int)i;
        }
        if (found < 0) { err = "no Workbook stream (not an Excel .xls?)"; return false; }
        const uint8_t* e = &dir[found];
        uint32_t st = rd32(e + 0x74), size = rd32(e + 0x78);
        if (size < cutoff) {
            ministream = chain(root_start, (size_t)root_size);
            out = minichain(st, size);
        } else out = chain(st, size);
        if (out.size() < size) { err = "truncated stream"; return false; }
        return true;
    }
};

double rk_value(uint32_t rk) {
    double v;
    if (rk & 2) v = (double)((int32_t)rk >> 2);
    else { uint64_t b = (uint64_t)(rk & 0xFFFFFFFC) << 32; std::memcpy(&v, &b, 8); }
    if (rk & 1) v /= 100.0;
    return v;
}

void append_utf8(std::string& o, uint32_t cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | cp >> 6); o += (char)(0x80 | (cp & 63)); }
    else { o += (char)(0xE0 | cp >> 12); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
}

// reads across segment boundaries (SST + CONTINUE records)
struct SegReader {
    std::vector<std::pair<const uint8_t*, size_t>> seg;
    size_t si = 0, pos = 0;
    bool eof() const { return si >= seg.size(); }
    void norm() { while (si < seg.size() && pos >= seg[si].second) { ++si; pos = 0; } }
    bool u8(uint8_t& v) { norm(); if (eof()) return false; v = seg[si].first[pos++]; return true; }
    bool u16(uint16_t& v) { uint8_t a, b; if (!u8(a) || !u8(b)) return false; v = uint16_t(a | b << 8); return true; }
    bool u32(uint32_t& v) { uint16_t a, b; if (!u16(a) || !u16(b)) return false; v = uint32_t(a | (uint32_t)b << 16); return true; }
    void skip(size_t n) { while (n && !eof()) { norm(); if (eof()) break; size_t k = std::min(n, seg[si].second - pos); pos += k; n -= k; } }
    // XLUnicodeRichExtendedString
    bool str(std::string& out) {
        uint16_t cch; uint8_t fl;
        if (!u16(cch) || !u8(fl)) return false;
        uint16_t runs = 0; uint32_t ext = 0;
        if (fl & 8) u16(runs);
        if (fl & 4) u32(ext);
        bool wide = fl & 1;
        for (uint32_t i = 0; i < cch; ++i) {
            if (pos >= (si < seg.size() ? seg[si].second : 0)) {   // crossing into CONTINUE: new flag byte
                ++si; pos = 0;
                if (eof()) return false;
                uint8_t f2 = seg[si].first[pos++];
                wide = f2 & 1;
            }
            if (wide) { uint16_t c; if (!u16(c)) return false; append_utf8(out, c); }
            else { uint8_t c; if (!u8(c)) return false; append_utf8(out, c); }
        }
        skip((size_t)runs * 4 + ext);
        return true;
    }
};

std::string short_str(const uint8_t* p, size_t n, bool biff8, bool cch16) {
    std::string o;
    if (n < (cch16 ? 2u : 1u)) return o;
    size_t cch = cch16 ? rd16(p) : p[0];
    size_t k = cch16 ? 2 : 1;
    bool wide = false;
    if (biff8) { if (k >= n) return o; wide = p[k] & 1; ++k; }
    for (size_t i = 0; i < cch; ++i) {
        if (wide) { if (k + 2 > n) break; append_utf8(o, rd16(p + k)); k += 2; }
        else { if (k + 1 > n) break; append_utf8(o, p[k]); k += 1; }   // cp1252 ~ latin1
    }
    return o;
}
} // namespace


// ===========================================================================
// XLS (BIFF8) read
// ===========================================================================
namespace {

Value biff_err(uint8_t code) {
    switch (code) {
    case 0x00: return Value::error(Err::Null); case 0x07: return Value::error(Err::Div0); case 0x0F: return Value::error(Err::Value);
    case 0x17: return Value::error(Err::Ref); case 0x1D: return Value::error(Err::Name); case 0x24: return Value::error(Err::Num);
    default: return Value::error(Err::NA);
    }
}

} // namespace

bool xls_read(const std::vector<uint8_t>& data, Book& book, std::string& err) {
    Ole ole(data);
    if (!ole.open(err)) return false;
    std::vector<uint8_t> st;
    static const char* names[] = {"Workbook", "Book", "WORKBOOK", "BOOK", nullptr};
    if (!ole.stream(names, st, err)) return false;
    struct Bound { uint32_t pos; std::string name; bool worksheet; bool hidden; };
    std::vector<Bound> bounds;
    std::vector<std::string> sst;
    std::map<int, std::string> formats;
    std::vector<Font> fonts;
    struct XF { int font, fmt; Style s; };
    std::vector<XF> xfs;
    Argb palette[66];
    std::memcpy(palette, kIndexed, sizeof palette);
    bool biff8 = true, d1904 = false;
    size_t p = 0;
    bool first = true;
    std::vector<std::array<uint8_t, 20>> raw_xf;
    while (p + 4 <= st.size()) {
        uint16_t type = rd16(&st[p]), len = rd16(&st[p + 2]);
        const uint8_t* rec = &st[p + 4];
        if (p + 4 + len > st.size()) break;
        if (first) { if (type != 0x0809 && type != 0x0409 && type != 0x0209) { err = "unsupported BIFF version"; return false; } if (len >= 2) biff8 = rd16(rec) >= 0x0600; first = false; }
        if (type == 0x0085 && len >= 8) bounds.push_back({rd32(rec), short_str(rec + 6, len - 6, biff8, false), rec[5] == 0, (rec[4] & 3) != 0});
        else if (type == 0x0022 && len >= 2) d1904 = rd16(rec) != 0;
        else if (type == 0x041E && len >= 4) formats[rd16(rec)] = short_str(rec + 2, len - 2, biff8, true);
        else if (type == 0x0031 && len >= 15) {
            Font f;
            f.size = rd16(rec) / 20.0;
            uint16_t grbit = rd16(rec + 2);
            f.italic = grbit & 2; f.strike = grbit & 8;
            uint16_t icv = rd16(rec + 4);
            f.color = icv < 64 && icv > 0 ? palette[icv] : 0;
            if (icv == 8) f.color = 0;
            f.bold = rd16(rec + 6) >= 600;
            uint8_t u = rec[10];
            f.underline = u == 1 ? Underline::Single : u == 2 ? Underline::Double : u == 0x21 ? Underline::SingleAccounting : u == 0x22 ? Underline::DoubleAccounting : Underline::None;
            f.name = short_str(rec + 14, len - 14, biff8, false);
            fonts.push_back(f);
            if (fonts.size() == 4) fonts.push_back(f);   // font index 4 is never used in BIFF
        } else if (type == 0x00E0 && len >= 20) {
            std::array<uint8_t, 20> a; std::memcpy(a.data(), rec, 20); raw_xf.push_back(a);
        } else if (type == 0x0092 && len >= 2) {
            int n = rd16(rec);
            for (int i = 0; i < n && 2 + i * 4 + 3 < len && 8 + i < 64; ++i) palette[8 + i] = 0xFF000000u | rec[2 + i * 4] << 16 | rec[3 + i * 4] << 8 | rec[4 + i * 4];
        } else if (type == 0x00FC && len >= 8) {
            SegReader r;
            r.seg.push_back({rec + 8, (size_t)len - 8});
            uint32_t n = rd32(rec + 4);
            size_t q = p + 4 + len;
            while (q + 4 <= st.size() && rd16(&st[q]) == 0x003C) {
                uint16_t l2 = rd16(&st[q + 2]);
                if (q + 4 + l2 > st.size()) break;
                r.seg.push_back({&st[q + 4], l2});
                q += 4 + l2;
            }
            sst.reserve(n);
            for (uint32_t i = 0; i < n; ++i) { std::string s; if (!r.str(s)) break; sst.push_back(std::move(s)); }
        } else if (type == 0x000A) break;
        p += 4 + len;
    }
    // XF records -> styles (palette may have come after the FONTs: re-resolve font colours lazily)
    auto icv_color = [&](int icv) -> Argb { return icv >= 8 && icv < 64 ? palette[icv] : (icv >= 0 && icv < 8 ? palette[icv] : 0); };
    static const BorderStyle bmap[14] = {BorderStyle::None, BorderStyle::Thin, BorderStyle::Medium, BorderStyle::Dashed, BorderStyle::Dotted, BorderStyle::Thick, BorderStyle::Double,
        BorderStyle::Hair, BorderStyle::MediumDashed, BorderStyle::DashDot, BorderStyle::MediumDashDot, BorderStyle::DashDotDot, BorderStyle::MediumDashDotDot, BorderStyle::SlantDashDot};
    Book b;
    b.sheets.clear();
    b.date1904 = d1904;
    std::vector<uint32_t> xf_style;
    for (auto& a : raw_xf) {
        Style s;
        int ifnt = rd16(&a[0]), ifmt = rd16(&a[2]);
        if (ifnt < (int)fonts.size()) s.font = fonts[ifnt];
        auto fit = formats.find(ifmt);
        s.numfmt = fit != formats.end() ? fit->second : builtin_numfmt(ifmt);
        if (s.numfmt.empty()) s.numfmt = "General";
        uint8_t al = a[6];
        s.halign = (HAlign)std::min(al & 7, 7);
        s.wrap = al & 8;
        s.valign = (VAlign)std::min((al >> 4) & 7, 4);
        int rot = a[7];
        s.rotation = (int16_t)(rot > 90 && rot <= 180 ? 90 - rot : rot);
        s.indent = a[8] & 15;
        s.shrink = a[8] & 16;
        uint32_t b1 = rd32(&a[10]), b2 = rd32(&a[14]);
        auto side = [&](int st, int icv) { BorderSide x; x.style = st < 14 ? bmap[st] : BorderStyle::Thin; x.color = st ? icv_color(icv) : 0; return x; };
        s.border.left = side(b1 & 15, (b1 >> 16) & 127);
        s.border.right = side((b1 >> 4) & 15, (b1 >> 23) & 127);
        s.border.top = side((b1 >> 8) & 15, b2 & 127);
        s.border.bottom = side((b1 >> 12) & 15, (b2 >> 7) & 127);
        int fls = (b2 >> 26) & 63;
        uint16_t fillc = rd16(&a[18]);
        if (fls) {
            s.fill.pattern = fls == 1 ? FillPattern::Solid : (FillPattern)std::min(fls, 18);
            s.fill.fg = icv_color(fillc & 127);
            s.fill.bg = icv_color((fillc >> 7) & 127);
        }
        xf_style.push_back(b.styles.intern(s));
    }
    auto sty = [&](int ixfe) -> uint32_t { return ixfe >= 0 && ixfe < (int)xf_style.size() ? xf_style[ixfe] : 0; };
    uint32_t id = 1;
    for (auto& bd : bounds) {
        if (!bd.worksheet) continue;
        Worksheet ws;
        ws.name = bd.name;
        ws.id = id++;
        ws.hidden = bd.hidden;
        size_t q = bd.pos;
        bool pending_str = false; int pr = 0, pc = 0;
        bool bof = false, frozen = false;
        auto set = [&](int r, int c, Value v, int ixfe) {
            if (r < 0 || c < 0 || r >= MAX_ROWS || c >= MAX_COLS) return;
            Cell& cl = ws.at(r, c);
            cl.v = std::move(v);
            cl.style = sty(ixfe);
        };
        while (q + 4 <= st.size()) {
            uint16_t type = rd16(&st[q]), len = rd16(&st[q + 2]);
            if (q + 4 + len > st.size()) break;
            const uint8_t* rec = &st[q + 4];
            q += 4 + len;
            if (type == 0x0809 || type == 0x0409) { if (bof) break; bof = true; continue; }
            if (type == 0x000A) break;
            if (type == 0x007D && len >= 10) {   // COLINFO
                int c0 = rd16(rec), c1 = std::min<int>(rd16(rec + 2), 1000);
                double w = rd16(rec + 4) / 256.0;
                int ixfe = rd16(rec + 6), fl = rd16(rec + 8);
                for (int c = c0; c <= c1; ++c) { ColInfo& ci = ws.col(c); ci.width = std::max(0.0, w - 0.71); ci.hidden = fl & 1; ci.style = ixfe > 15 ? sty(ixfe) : 0; }
                continue;
            }
            if (type == 0x0208 && len >= 16) {   // ROW
                int r = rd16(rec);
                uint16_t h = rd16(rec + 6);
                uint32_t fl = rd32(rec + 12);
                if (!(h & 0x8000) && (fl & 0x40)) ws.row(r).height = (h & 0x7FFF) / 20.0f;
                if (fl & 0x20) ws.row(r).hidden = true;
                if (fl & 0x80) ws.row(r).style = sty((fl >> 16) & 0xFFF);
                continue;
            }
            if (type == 0x00E5 && len >= 2) {   // MERGEDCELLS
                int n = rd16(rec);
                for (int i = 0; i < n && 2 + i * 8 + 8 <= len; ++i) {
                    const uint8_t* m = rec + 2 + i * 8;
                    ws.merges.push_back(Range(rd16(m), rd16(m + 4), rd16(m + 2), rd16(m + 6)));
                }
                continue;
            }
            if (type == 0x023E && len >= 2) { frozen = rd16(rec) & 8; ws.show_grid = rd16(rec) & 2; continue; }   // WINDOW2
            if (type == 0x0041 && len >= 4) { if (frozen) { ws.freeze_cols = rd16(rec); ws.freeze_rows = rd16(rec + 2); } continue; }   // PANE
            if (len < 6 && type != 0x0207) continue;
            int r = len >= 2 ? rd16(rec) : 0, c = len >= 4 ? rd16(rec + 2) : 0, ixfe = len >= 6 ? rd16(rec + 4) : 0;
            switch (type) {
            case 0x00FD: if (len >= 10) { uint32_t k = rd32(rec + 6); set(r, c, Value::text(k < sst.size() ? sst[k] : std::string()), ixfe); } break;
            case 0x0203: if (len >= 14) { double v; std::memcpy(&v, rec + 6, 8); set(r, c, Value::number(v), ixfe); } break;
            case 0x027E: if (len >= 10) set(r, c, Value::number(rk_value(rd32(rec + 6))), ixfe); break;
            case 0x00BD: {   // MULRK
                int last = rd16(rec + len - 2);
                for (int k = 0; c + k <= last && 4 + k * 6 + 6 <= len - 2; ++k) set(r, c + k, Value::number(rk_value(rd32(rec + 4 + k * 6 + 2))), rd16(rec + 4 + k * 6));
            } break;
            case 0x0201: set(r, c, Value(), ixfe); break;   // BLANK
            case 0x00BE: {   // MULBLANK
                int last = rd16(rec + len - 2);
                for (int k = 0; c + k <= last && 4 + k * 2 + 2 <= len - 2 + 2; ++k) set(r, c + k, Value(), rd16(rec + 4 + k * 2));
            } break;
            case 0x0204: case 0x00D6: if (len >= 8) set(r, c, Value::text(short_str(rec + 6, len - 6, biff8, true)), ixfe); break;
            case 0x0205: if (len >= 8) set(r, c, rec[7] ? biff_err(rec[6]) : Value::boolean(rec[6] != 0), ixfe); break;
            case 0x0006: if (len >= 14) {
                const uint8_t* v = rec + 6;
                if (rd16(v + 6) == 0xFFFF) {
                    if (v[0] == 0) { pending_str = true; pr = r; pc = c; set(r, c, Value::text(""), ixfe); }
                    else if (v[0] == 1) set(r, c, Value::boolean(v[2] != 0), ixfe);
                    else if (v[0] == 2) set(r, c, biff_err(v[2]), ixfe);
                    else set(r, c, Value::text(""), ixfe);
                } else { double d; std::memcpy(&d, v, 8); set(r, c, Value::number(d), ixfe); }
            } break;
            case 0x0207: if (pending_str) { ws.at(pr, pc).v = Value::text(short_str(rec, len, biff8, true)); pending_str = false; } break;
            default: break;
            }
        }
        b.sheets.push_back(std::move(ws));
    }
    if (b.sheets.empty()) { err = "no worksheets found"; return false; }
    b.invalidate_graph();
    b.calc().needs_full = false;
    book = std::move(b);
    return true;
}

// ===========================================================================
// legacy string-grid API
// ===========================================================================
bool xlsx_read(const std::vector<uint8_t>& data, Workbook& wb, std::string& err) {
    Book b;
    if (!xlsx_read(data, b, err)) return false;
    wb = to_legacy(b);
    return true;
}
std::vector<uint8_t> xlsx_write(const Workbook& wb0) {
    Workbook wb = wb0;
    if (wb.sheets.empty()) wb.sheets.push_back(Sheet{});
    Book b = from_legacy(wb);
    return xlsx_write(b, 6);
}
bool xls_read(const std::vector<uint8_t>& data, Workbook& wb, std::string& err) {
    Book b;
    if (!xls_read(data, b, err)) return false;
    wb = to_legacy(b);
    return true;
}

// ===========================================================================
// text encodings + CSV
// ===========================================================================
static const uint16_t k1252[32] = {0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F,
    0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};

static bool valid_utf8(const std::vector<uint8_t>& d, size_t from) {
    for (size_t i = from; i < d.size();) {
        uint8_t c = d[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!n || i + n > d.size()) return false;
        for (int k = 1; k < n; ++k) if ((d[i + k] >> 6) != 2) return false;
        i += n;
    }
    return true;
}

std::string decode_text_bytes(const std::vector<uint8_t>& d, TextEncoding enc) {
    std::string o;
    size_t start = 0;
    if (enc == TextEncoding::Auto) {
        if (d.size() >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) enc = TextEncoding::Utf8Bom;
        else if (d.size() >= 2 && d[0] == 0xFF && d[1] == 0xFE) enc = TextEncoding::Utf16LE;
        else if (d.size() >= 2 && d[0] == 0xFE && d[1] == 0xFF) enc = TextEncoding::Utf16BE;
        else if (d.size() >= 4 && d[1] == 0 && d[3] == 0 && d[0] != 0) enc = TextEncoding::Utf16LE;
        else if (d.size() >= 4 && d[0] == 0 && d[2] == 0 && d[1] != 0) enc = TextEncoding::Utf16BE;
        else enc = valid_utf8(d, 0) ? TextEncoding::Utf8 : TextEncoding::Windows1252;
    }
    switch (enc) {
    case TextEncoding::Utf8Bom: start = d.size() >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF ? 3 : 0; [[fallthrough]];
    case TextEncoding::Utf8: case TextEncoding::Auto: o.assign(d.begin() + start, d.end()); break;
    case TextEncoding::Utf16LE: case TextEncoding::Utf16BE: {
        bool le = enc == TextEncoding::Utf16LE;
        start = d.size() >= 2 && ((le && d[0] == 0xFF && d[1] == 0xFE) || (!le && d[0] == 0xFE && d[1] == 0xFF)) ? 2 : 0;
        for (size_t i = start; i + 1 < d.size(); i += 2) {
            uint32_t u = le ? (d[i] | d[i + 1] << 8) : (d[i] << 8 | d[i + 1]);
            if (u >= 0xD800 && u < 0xDC00 && i + 3 < d.size()) {
                uint32_t u2 = le ? (d[i + 2] | d[i + 3] << 8) : (d[i + 2] << 8 | d[i + 3]);
                if (u2 >= 0xDC00 && u2 < 0xE000) { u = 0x10000 + ((u - 0xD800) << 10) + (u2 - 0xDC00); i += 2; }
            }
            xmlp::utf8_put(o, u);
        }
        break;
    }
    case TextEncoding::Windows1252: for (uint8_t c : d) xmlp::utf8_put(o, c >= 0x80 && c < 0xA0 ? k1252[c - 0x80] : c); break;
    case TextEncoding::Latin1: for (uint8_t c : d) xmlp::utf8_put(o, c); break;
    }
    return o;
}

std::vector<uint8_t> encode_text(const std::string& s, TextEncoding enc) {
    std::vector<uint8_t> o;
    auto cps = utf8_decode(s);
    switch (enc) {
    case TextEncoding::Auto: case TextEncoding::Utf8Bom: o = {0xEF, 0xBB, 0xBF}; [[fallthrough]];
    case TextEncoding::Utf8: o.insert(o.end(), s.begin(), s.end()); break;
    case TextEncoding::Utf16LE: case TextEncoding::Utf16BE: {
        bool le = enc == TextEncoding::Utf16LE;
        auto put = [&](uint32_t u) { if (le) { o.push_back((uint8_t)u); o.push_back((uint8_t)(u >> 8)); } else { o.push_back((uint8_t)(u >> 8)); o.push_back((uint8_t)u); } };
        put(0xFEFF);
        for (uint32_t c : cps) { if (c >= 0x10000) { c -= 0x10000; put(0xD800 + (c >> 10)); put(0xDC00 + (c & 1023)); } else put(c); }
        break;
    }
    case TextEncoding::Windows1252: case TextEncoding::Latin1:
        for (uint32_t c : cps) {
            if (c < 0x80 || (c >= 0xA0 && c < 0x100)) { o.push_back((uint8_t)c); continue; }
            if (c < 0xA0 && enc == TextEncoding::Latin1) { o.push_back((uint8_t)c); continue; }
            int k = -1;
            if (enc == TextEncoding::Windows1252) for (int i = 0; i < 32; ++i) if (k1252[i] == c) { k = i; break; }
            o.push_back(k >= 0 ? (uint8_t)(0x80 + k) : (uint8_t)'?');
        }
        break;
    }
    return o;
}

bool csv_read(const std::vector<uint8_t>& data, Book& book, int si, const CsvOptions& opt, std::string& err) {
    if (si < 0 || si >= book.sheet_count()) { err = "bad sheet index"; return false; }
    std::string text = decode_text_bytes(data, opt.encoding);
    char sep = opt.sep;
    if (!sep) {
        // pick the delimiter with the most consistent non-zero count over the first lines
        const char cands[] = {',', ';', '\t', '|'};
        int best = 0; double best_score = -1;
        std::vector<std::string> lines;
        size_t p = 0;
        while (p < text.size() && lines.size() < 20) { size_t e = text.find('\n', p); if (e == std::string::npos) e = text.size(); lines.push_back(text.substr(p, e - p)); p = e + 1; }
        for (int k = 0; k < 4; ++k) {
            std::vector<int> cnt;
            for (auto& l : lines) { if (l.empty()) continue; int n = 0; bool q = false; for (char ch : l) { if (ch == '"') q = !q; else if (ch == cands[k] && !q) ++n; } cnt.push_back(n); }
            if (cnt.empty()) continue;
            int mn = *std::min_element(cnt.begin(), cnt.end()), mx = *std::max_element(cnt.begin(), cnt.end());
            double score = mn > 0 ? mn * 10.0 - (mx - mn) : (mx > 0 ? mx * 0.1 : 0);
            if (score > best_score) { best_score = score; best = k; }
        }
        sep = cands[best];
    }
    auto rows = parse_delimited(text, sep, opt.quote ? opt.quote : '"');
    Worksheet& ws = book.sheets[si];
    for (size_t r = 0; r < rows.size() && r < (size_t)MAX_ROWS; ++r)
        for (size_t c = 0; c < rows[r].size() && c < (size_t)MAX_COLS; ++c) {
            const std::string& t = rows[r][c];
            if (t.empty()) continue;
            if (opt.formulas && t.size() > 1 && t[0] == '=') { book.set_formula_raw(si, (int)r, (int)c, t.substr(1)); continue; }
            Cell& cl = ws.at((int)r, (int)c);
            if (opt.detect_types) {
                std::string hint;
                cl.v = parse_input(t, &hint, book.date1904);
                if (!hint.empty()) { Style s; s.numfmt = hint; cl.style = book.styles.intern(s); }
            } else cl.v = Value::text(t);
        }
    book.invalidate_graph();
    if (opt.formulas) book.recalc_all();
    ++book.gen;
    return true;
}

std::vector<uint8_t> csv_write(const Book& book, int si, const CsvOptions& opt) {
    const Worksheet& ws = book.sheets[si];
    std::vector<Row> rows;
    int nr = ws.nrows();
    for (auto& [k, arr] : ws.spills) nr = std::max(nr, k.first + arr->rows);
    int nc = ws.ncols();
    for (auto& [k, arr] : ws.spills) nc = std::max(nc, k.second + arr->cols);
    for (int r = 0; r < nr; ++r) {
        Row row;
        for (int c = 0; c < nc; ++c) {
            const Cell* cl = ws.cell(r, c);
            if (opt.formulas && cl && cl->f) row.push_back("=" + formula_text(*cl->f, book, si, r, c));
            else row.push_back(book.display_text(si, r, c));
        }
        while (!row.empty() && row.back().empty()) row.pop_back();
        rows.push_back(std::move(row));
    }
    std::string t = to_delimited(rows, opt.sep ? opt.sep : ',', opt.crlf, opt.quote ? opt.quote : '"');
    return encode_text(t, opt.encoding);
}

} // namespace pcbp::io
