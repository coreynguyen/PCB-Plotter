// ============================================================================
// ods.cpp — OpenDocument Spreadsheet read/write, from OASIS ODF 1.2/1.3
// (Part 1 §9 tables, §16 styles / data styles, §19 attributes; Part 2
// OpenFormula for table:formula). Clean-room: no LibreOffice code used.
//   * content.xml : tables, columns (widths/hidden), rows (heights/hidden/
//     repeats), cells (value types, formulas "of:=", spans/covered cells,
//     validations), named ranges, database range (autofilter), automatic
//     cell styles + number/date/time/percentage/text data styles
//   * styles.xml  : default cell style, CF target styles
//   * settings.xml: active sheet, frozen panes (HorizontalSplitMode=2 ...)
//   * calcext:conditional-formats (de-facto extension namespace used by
//     ODF producers) for conditional formatting round trips
// ============================================================================
#include "ods.h"
#include "archive.h"
#include "xlsx.h"
#include "xlsx_xml.h"
#include "../core/formula_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>

namespace pcbp::io {

using namespace pcbp::sheet;
using xmlp::esc;

namespace {

const char* kMime = "application/vnd.oasis.opendocument.spreadsheet";

std::string hexcol(Argb c) { char b[16]; snprintf(b, sizeof b, "#%06x", (unsigned)(c & 0xFFFFFF)); return b; }
Argb parse_hex(const std::string& s) { if (s.size() == 7 && s[0] == '#') return 0xFF000000u | (Argb)strtoul(s.c_str() + 1, nullptr, 16); return 0; }
std::string numtxt(double v) { char b[40]; snprintf(b, sizeof b, "%.15g", v); if (atof(b) != v) snprintf(b, sizeof b, "%.17g", v); return b; }

// length "2.5cm" / "1in" / "72pt" / "0.5mm" -> points
double len_pt(const std::string& s) {
    double v = atof(s.c_str());
    if (s.find("cm") != std::string::npos) return v * 72 / 2.54;
    if (s.find("mm") != std::string::npos) return v * 72 / 25.4;
    if (s.find("in") != std::string::npos) return v * 72;
    if (s.find("pc") != std::string::npos) return v * 12;
    if (s.find("px") != std::string::npos) return v * 0.75;
    return v;   // pt
}
// Excel column width (chars) <-> points, using a 7 px max digit width at 96 dpi
double chars_to_pt(double w) { return (std::trunc(w * 7 + 5)) * 0.75; }
double pt_to_chars(double pt) { double px = pt / 0.75; return std::max(0.0, std::trunc((px - 5) / 7 * 100 + 0.5) / 100); }

// ----------------------------------------------------------------- formulas --
bool is_ident(char c) { return isalnum((unsigned char)c) || c == '_' || c == '.' || (unsigned char)c >= 0x80; }

} // namespace

std::string a1_to_of(const std::string& a1) {
    std::string f = a1;
    if (!f.empty() && f[0] == '=') f.erase(0, 1);
    auto toks = tokenize_formula(f);
    std::string o = "of:=";
    int brace = 0;
    std::vector<int> paren_is_func;   // stack: 1 = function call parens
    for (size_t i = 0; i < toks.size(); ++i) {
        const auto& t = toks[i];
        std::string s = f.substr(t.pos, t.len);
        switch (t.kind) {
        case LexToken::Ref: {
            // split sheet prefix
            std::string sheet, body = s;
            size_t bang = s.rfind('!');
            if (bang != std::string::npos) { sheet = s.substr(0, bang); body = s.substr(bang + 1); }
            std::string a = body, b;
            size_t colon = body.find(':');
            if (colon != std::string::npos) { a = body.substr(0, colon); b = body.substr(colon + 1); }
            o += "[";
            if (!sheet.empty()) {
                bool q = sheet[0] == '\'';
                std::string nm = q ? sheet.substr(1, sheet.size() - 2) : sheet;
                std::string qn = "'";
                for (char ch : (q ? nm : nm)) { qn += ch; }
                qn += "'";
                bool need = false;
                for (char ch : nm) if (!is_ident(ch)) need = true;
                o += "$" + (q || need ? (q ? sheet : qn) : nm);
            }
            o += "." + a;
            if (!b.empty()) o += ":." + b;
            o += "]";
            break;
        }
        case LexToken::OpenBrace: ++brace; o += s; break;
        case LexToken::CloseBrace: --brace; o += s; break;
        case LexToken::Comma:
            if (brace) o += ';';
            else if (!paren_is_func.empty() && paren_is_func.back()) o += ';';
            else o += '~';   // union inside plain parentheses
            break;
        case LexToken::Semicolon: o += brace ? "|" : ";"; break;
        case LexToken::OpenParen: {
            bool fn = i > 0 && toks[i - 1].kind == LexToken::Function;
            paren_is_func.push_back(fn);
            o += s;
            break;
        }
        case LexToken::CloseParen: if (!paren_is_func.empty()) paren_is_func.pop_back(); o += s; break;
        case LexToken::Space: {
            // intersection operator between two operands -> '!'
            bool prev_ref = i > 0 && (toks[i - 1].kind == LexToken::Ref || toks[i - 1].kind == LexToken::CloseParen || toks[i - 1].kind == LexToken::Name);
            bool next_ref = i + 1 < toks.size() && (toks[i + 1].kind == LexToken::Ref || toks[i + 1].kind == LexToken::OpenParen || toks[i + 1].kind == LexToken::Name);
            o += prev_ref && next_ref ? "!" : s;
            break;
        }
        case LexToken::Function: {
            std::string u = upper_ascii(s);
            for (const char* pre : {"_XLFN._XLWS.", "_XLFN."}) if (u.rfind(pre, 0) == 0) { s = s.substr(strlen(pre)); break; }
            o += s;
            break;
        }
        default: o += s;
        }
    }
    return o;
}

std::string of_to_a1(const std::string& of0) {
    std::string f = of0;
    size_t eq = f.find(":=");
    if (f.rfind("of:", 0) == 0 || f.rfind("oooc:", 0) == 0 || f.rfind("msoxl:", 0) == 0) f = f.substr(f.find(':') + 1);
    (void)eq;
    if (!f.empty() && f[0] == '=') f.erase(0, 1);
    std::string o;
    int brace = 0;
    for (size_t i = 0; i < f.size(); ++i) {
        char ch = f[i];
        if (ch == '"') {
            size_t j = i + 1;
            while (j < f.size()) { if (f[j] == '"') { if (j + 1 < f.size() && f[j + 1] == '"') { j += 2; continue; } break; } ++j; }
            o += f.substr(i, j - i + 1);
            i = j;
            continue;
        }
        if (ch == '[') {
            size_t j = f.find(']', i);
            if (j == std::string::npos) { o += f.substr(i); break; }
            std::string in = f.substr(i + 1, j - i - 1);
            // [$Sheet.A1:.B2] / [.A1] / ['Sheet 2'.A1:'Sheet 2'.B5]
            auto part = [&](const std::string& p, std::string& sheet, std::string& cell) {
                size_t dot = std::string::npos;
                bool q = false;
                for (size_t k = 0; k < p.size(); ++k) { if (p[k] == '\'') q = !q; else if (p[k] == '.' && !q) dot = k; }
                std::string s = dot == std::string::npos ? std::string() : p.substr(0, dot);
                cell = dot == std::string::npos ? p : p.substr(dot + 1);
                if (!s.empty() && s[0] == '$') s.erase(0, 1);
                sheet = s;
            };
            size_t colon = std::string::npos;
            bool q = false;
            for (size_t k = 0; k < in.size(); ++k) { if (in[k] == '\'') q = !q; else if (in[k] == ':' && !q) { colon = k; break; } }
            std::string s1, c1, s2, c2;
            part(colon == std::string::npos ? in : in.substr(0, colon), s1, c1);
            if (colon != std::string::npos) part(in.substr(colon + 1), s2, c2);
            if (!s1.empty()) {
                std::string nm = s1;
                if (nm.size() >= 2 && nm[0] == '\'') { std::string u; for (size_t k = 1; k + 1 < nm.size(); ++k) { if (nm[k] == '\'' && nm[k + 1] == '\'') { u += '\''; ++k; } else u += nm[k]; } nm = u; }
                o += quote_sheet_name(nm) + "!";
            }
            if (c1.find("#REF!") != std::string::npos) { o += "#REF!"; i = j; continue; }
            o += c1;
            if (!c2.empty()) o += ":" + c2;
            i = j;
            continue;
        }
        if (ch == '{') { ++brace; o += ch; continue; }
        if (ch == '}') { --brace; o += ch; continue; }
        if (ch == ';') { o += ','; continue; }
        if (ch == '|' && brace) { o += ';'; continue; }
        if (ch == '~') { o += ','; continue; }
        if (ch == '!' ) { o += ' '; continue; }
        o += ch;
    }
    return o;
}

bool is_ods_bytes(const std::vector<uint8_t>& d) {
    if (!is_xlsx_bytes(d) || d.size() < 38 + 46) return false;
    // first entry is "mimetype", stored
    if (memcmp(&d[30], "mimetype", 8) != 0) {
        std::vector<ArchiveEntry> ents; std::string err;
        if (!read_zip(d.data(), d.size(), ents, err)) return false;
        for (auto& e : ents) if (e.name == "mimetype") return std::string(e.data.begin(), e.data.end()).rfind(kMime, 0) == 0;
        return false;
    }
    size_t n = strlen(kMime);
    return d.size() >= 38 + n && memcmp(&d[38], kMime, n) == 0;
}

// ===========================================================================
// number format <-> ODF data style
// ===========================================================================
namespace {

std::string ods_data_style(const std::string& name, const std::string& code0, bool d1904) {
    (void)d1904;
    std::string code = code0;
    // first section only (plus colour for negative handled by LibreOffice's own map; skipped)
    {
        bool q = false;
        for (size_t i = 0; i < code.size(); ++i) { if (code[i] == '"') q = !q; else if (code[i] == '\\') ++i; else if (code[i] == ';' && !q) { code = code.substr(0, i); break; } }
    }
    // strip [Red]/[$-409]
    std::string c2;
    for (size_t i = 0; i < code.size(); ++i) {
        if (code[i] == '[') {
            size_t j = code.find(']', i);
            if (j == std::string::npos) break;
            std::string tag = code.substr(i + 1, j - i - 1);
            if (!tag.empty() && tag[0] == '$') { size_t d = tag.find('-'); std::string sym = tag.substr(1, d == std::string::npos ? std::string::npos : d - 1); if (!sym.empty()) c2 += "\"" + sym + "\""; }
            else if (tag == "h" || tag == "hh" || tag == "mm" || tag == "ss" || tag == "H" || tag == "HH") c2 += code.substr(i, j - i + 1);
            i = j;
            continue;
        }
        c2 += code[i];
    }
    code = c2;
    auto text_el = [](const std::string& t) { return t.empty() ? std::string() : "<number:text>" + esc(t, false) + "</number:text>"; };
    if (code == "@") return "<number:text-style style:name=\"" + name + "\"><number:text-content/></number:text-style>";
    if (is_date_format(code)) {
        std::string body, lit;
        auto flush = [&]() { body += text_el(lit); lit.clear(); };
        bool has_h = false, ampm = upper_ascii(code).find("AM/PM") != std::string::npos;
        bool time_only = true;
        for (size_t i = 0; i < code.size();) {
            char ch = code[i];
            char l = (char)tolower((unsigned char)ch);
            if (ch == '"') { size_t j = code.find('"', i + 1); if (j == std::string::npos) j = code.size(); lit += code.substr(i + 1, j - i - 1); i = j + 1; continue; }
            if (ch == '\\') { lit += code.substr(i + 1, 1); i += 2; continue; }
            if (ch == '[') { size_t j = code.find(']', i); flush(); std::string t = lower_utf8(code.substr(i + 1, j - i - 1)); if (t[0] == 'h') body += "<number:hours number:style=\"long\"/>"; else if (t[0] == 'm') body += "<number:minutes number:style=\"long\"/>"; else body += "<number:seconds number:style=\"long\"/>"; i = j + 1; has_h = true; continue; }
            if (l == 'y' || l == 'm' || l == 'd' || l == 'h' || l == 's' || l == 'a') {
                size_t j = i; while (j < code.size() && tolower((unsigned char)code[j]) == l) ++j;
                int n = (int)(j - i);
                if (l == 'a') {
                    if (upper_ascii(code.substr(i, 5)) == "AM/PM") { flush(); body += "<number:am-pm/>"; i += 5; continue; }
                    if (upper_ascii(code.substr(i, 3)) == "A/P") { flush(); body += "<number:am-pm/>"; i += 3; continue; }
                    lit += ch; ++i; continue;
                }
                flush();
                if (l == 'y') { time_only = false; body += n >= 3 ? "<number:year number:style=\"long\"/>" : "<number:year/>"; }
                else if (l == 'd') { time_only = false; body += n == 1 ? "<number:day/>" : n == 2 ? "<number:day number:style=\"long\"/>" : n == 3 ? "<number:day-of-week/>" : "<number:day-of-week number:style=\"long\"/>"; }
                else if (l == 'h') { has_h = true; body += n >= 2 ? "<number:hours number:style=\"long\"/>" : "<number:hours/>"; }
                else if (l == 's') {
                    int dec = 0;
                    if (j < code.size() && code[j] == '.') { size_t k = j + 1; while (k < code.size() && code[k] == '0') { ++dec; ++k; } j = k; }
                    body += std::string("<number:seconds") + (n >= 2 ? " number:style=\"long\"" : "") + (dec ? " number:decimal-places=\"" + std::to_string(dec) + "\"" : "") + "/>";
                } else {   // m: month or minute
                    bool minute = false;
                    size_t k = i; while (k > 0) { --k; char p = (char)tolower((unsigned char)code[k]); if (p == 'h' || p == ']') { minute = true; break; } if (isalpha((unsigned char)p)) break; }
                    size_t q = j; while (q < code.size()) { char p = (char)tolower((unsigned char)code[q]); if (p == 's') { minute = true; break; } if (isalpha((unsigned char)p)) break; ++q; }
                    if (minute && n <= 2) body += n == 2 ? "<number:minutes number:style=\"long\"/>" : "<number:minutes/>";
                    else { time_only = false; body += n == 1 ? "<number:month/>" : n == 2 ? "<number:month number:style=\"long\"/>" : n == 3 ? "<number:month number:textual=\"true\"/>" : "<number:month number:style=\"long\" number:textual=\"true\"/>"; }
                }
                i = j;
                continue;
            }
            lit += ch; ++i;
        }
        flush();
        (void)has_h; (void)ampm;
        const char* tag = time_only ? "number:time-style" : "number:date-style";
        return std::string("<") + tag + " style:name=\"" + name + "\"" + (time_only && code.find('[') != std::string::npos ? " number:truncate-on-overflow=\"false\"" : "") + ">" + body + "</" + tag + ">";
    }
    // numeric
    bool pct = false, sci = false, grouping = false;
    int dec = 0, minint = 0, minexp = 0;
    std::string pre, post;
    bool seen_digit = false, after_dot = false, in_exp = false, frac = false;
    int num_digits = 0, den_digits = 0;
    for (size_t i = 0; i < code.size(); ++i) {
        char ch = code[i];
        if (ch == '"') { size_t j = code.find('"', i + 1); if (j == std::string::npos) j = code.size(); (seen_digit ? post : pre) += code.substr(i + 1, j - i - 1); i = j; continue; }
        if (ch == '\\') { if (i + 1 < code.size()) (seen_digit ? post : pre) += code[i + 1]; ++i; continue; }
        if (ch == '_' || ch == '*') { ++i; continue; }
        if (ch == '0' || ch == '#' || ch == '?') {
            seen_digit = true;
            if (frac) { ++den_digits; continue; }
            if (in_exp) { if (ch == '0') ++minexp; continue; }
            if (after_dot) ++dec; else { if (ch == '0') ++minint; ++num_digits; }
            continue;
        }
        if (ch == '.') { after_dot = true; continue; }
        if (ch == ',') { if (seen_digit && !after_dot) grouping = true; continue; }
        if (ch == '%') { pct = true; post += '%'; continue; }
        if ((ch == 'E' || ch == 'e') && i + 1 < code.size() && (code[i + 1] == '+' || code[i + 1] == '-')) { sci = true; in_exp = true; ++i; continue; }
        if (ch == '/') { frac = true; continue; }
        if (ch == ' ' && seen_digit && !frac) { continue; }
        (seen_digit ? post : pre) += ch;
    }
    if (upper_ascii(code).find("GENERAL") != std::string::npos) return {};
    std::string num;
    if (frac) num = "<number:fraction number:min-integer-digits=\"0\" number:min-numerator-digits=\"1\" number:min-denominator-digits=\"" + std::to_string(std::max(1, den_digits)) + "\"/>";
    else if (sci) num = "<number:scientific-number number:decimal-places=\"" + std::to_string(dec) + "\" number:min-integer-digits=\"" + std::to_string(std::max(1, minint)) + "\" number:min-exponent-digits=\"" + std::to_string(std::max(1, minexp)) + "\"/>";
    else num = "<number:number number:decimal-places=\"" + std::to_string(dec) + "\" number:min-decimal-places=\"" + std::to_string(dec) + "\" number:min-integer-digits=\"" + std::to_string(minint) + "\"" + (grouping ? " number:grouping=\"true\"" : "") + "/>";
    if (pct) { std::string p2 = post; p2.erase(std::remove(p2.begin(), p2.end(), '%'), p2.end()); return "<number:percentage-style style:name=\"" + name + "\">" + text_el(pre) + num + text_el(p2 + "%") + "</number:percentage-style>"; }
    return "<number:number-style style:name=\"" + name + "\">" + text_el(pre) + num + text_el(post) + "</number:number-style>";
}

// Rebuild an Excel-style code from a parsed ODF data style (subset).
struct DataStyleBuilder {
    std::string code;
    bool date = false;
};

} // namespace

// ===========================================================================
// write
// ===========================================================================
std::vector<uint8_t> ods_write(const Book& b, int level) {
    // ---- collect styles ----
    std::map<uint32_t, int> cell_style;       // style id -> ce index
    std::map<std::string, int> data_style;    // numfmt -> N index
    std::vector<std::string> data_xml;
    auto ds_of = [&](const std::string& code) -> int {
        if (code.empty() || code == "General") return -1;
        auto it = data_style.find(code);
        if (it != data_style.end()) return it->second;
        int n = (int)data_xml.size();
        std::string x = ods_data_style("N" + std::to_string(n + 100), code, b.date1904);
        if (x.empty()) { data_style[code] = -1; return -1; }
        data_xml.push_back(x);
        data_style[code] = n;
        return n;
    };
    auto ce_of = [&](uint32_t sid) -> int {
        if (sid == 0) return -1;
        auto it = cell_style.find(sid);
        if (it != cell_style.end()) return it->second;
        int n = (int)cell_style.size();
        cell_style[sid] = n;
        ds_of(b.styles.get(sid).numfmt);
        return n;
    };
    for (auto& ws : b.sheets) {
        for (auto& r : ws.rows) { if (r.style) ce_of(r.style); for (auto& c : r.cells) if (c.style) ce_of(c.style); }
        for (auto& c : ws.cols) if (c.style) ce_of(c.style);
    }
    auto border_css = [](const BorderSide& s) -> std::string {
        if (s.style == BorderStyle::None) return {};
        const char* w = "0.74pt"; const char* k = "solid";
        switch (s.style) {
        case BorderStyle::Medium: case BorderStyle::MediumDashed: case BorderStyle::MediumDashDot: case BorderStyle::MediumDashDotDot: w = "1.76pt"; break;
        case BorderStyle::Thick: w = "2.49pt"; break;
        case BorderStyle::Double: w = "2.01pt"; k = "double"; break;
        case BorderStyle::Hair: w = "0.06pt"; break;
        default: break;
        }
        switch (s.style) {
        case BorderStyle::Dashed: case BorderStyle::MediumDashed: case BorderStyle::DashDot: case BorderStyle::MediumDashDot: case BorderStyle::DashDotDot: case BorderStyle::MediumDashDotDot: case BorderStyle::SlantDashDot: k = "dashed"; break;
        case BorderStyle::Dotted: k = "dotted"; break;
        default: break;
        }
        return std::string(w) + " " + k + " " + hexcol(s.color ? s.color : 0xFF000000);
    };
    auto cell_props = [&](const Style& s, bool dxf_like) {
        std::string tc = "<style:table-cell-properties";
        if (s.fill.pattern != FillPattern::None && (s.fill.fg || s.fill.bg)) tc += " fo:background-color=\"" + hexcol(s.fill.pattern == FillPattern::Solid ? (s.fill.fg ? s.fill.fg : s.fill.bg) : (s.fill.bg ? s.fill.bg : s.fill.fg)) + "\"";
        std::string bl = border_css(s.border.left), br = border_css(s.border.right), bt = border_css(s.border.top), bb = border_css(s.border.bottom);
        if (!bl.empty() && bl == br && bl == bt && bl == bb) tc += " fo:border=\"" + bl + "\"";
        else {
            if (!bl.empty()) tc += " fo:border-left=\"" + bl + "\"";
            if (!br.empty()) tc += " fo:border-right=\"" + br + "\"";
            if (!bt.empty()) tc += " fo:border-top=\"" + bt + "\"";
            if (!bb.empty()) tc += " fo:border-bottom=\"" + bb + "\"";
        }
        if (s.wrap) tc += " fo:wrap-option=\"wrap\"";
        if (s.shrink) tc += " style:shrink-to-fit=\"true\"";
        if (s.valign == VAlign::Top) tc += " style:vertical-align=\"top\"";
        else if (s.valign == VAlign::Center) tc += " style:vertical-align=\"middle\"";
        else if (!dxf_like && s.valign == VAlign::Bottom) tc += " style:vertical-align=\"bottom\"";
        if (s.rotation && s.rotation != 255) tc += " style:rotation-angle=\"" + std::to_string((s.rotation + 360) % 360) + "\"";
        if (!s.locked || s.hidden) tc += std::string(" style:cell-protect=\"") + (!s.locked && s.hidden ? "formula-hidden" : s.hidden ? "protected formula-hidden" : "none") + "\"";
        if (s.halign != HAlign::General) tc += " style:text-align-source=\"fix\"";
        tc += "/>";
        std::string pp;
        if (s.halign != HAlign::General || s.indent) {
            pp = "<style:paragraph-properties";
            static const char* al[] = {"start", "start", "center", "end", "start", "justify", "center", "justify"};
            if (s.halign != HAlign::General) pp += std::string(" fo:text-align=\"") + al[(int)s.halign] + "\"";
            if (s.indent) pp += " fo:margin-left=\"" + numtxt(s.indent * 2.5) + "mm\"";
            pp += "/>";
        }
        std::string tp = "<style:text-properties";
        if (!dxf_like || s.font.name != "Calibri") tp += " fo:font-family=\"" + esc(s.font.name) + "\"";
        if (!dxf_like || s.font.size != 11) tp += " fo:font-size=\"" + numtxt(s.font.size) + "pt\"";
        if (s.font.bold) tp += " fo:font-weight=\"bold\"";
        if (s.font.italic) tp += " fo:font-style=\"italic\"";
        if (s.font.underline != Underline::None) tp += std::string(" style:text-underline-style=\"solid\" style:text-underline-width=\"auto\" style:text-underline-color=\"font-color\"") + (s.font.underline == Underline::Double || s.font.underline == Underline::DoubleAccounting ? " style:text-underline-type=\"double\"" : "");
        if (s.font.strike) tp += " style:text-line-through-style=\"solid\" style:text-line-through-type=\"single\"";
        if (s.font.color) tp += " fo:color=\"" + hexcol(s.font.color) + "\"";
        tp += "/>";
        return tc + pp + tp;
    };
    std::string auto_styles;
    for (auto& [sid, n] : cell_style) {
        const Style& s = b.styles.get(sid);
        int ds = ds_of(s.numfmt);
        auto_styles += "<style:style style:name=\"ce" + std::to_string(n + 1) + "\" style:family=\"table-cell\" style:parent-style-name=\"Default\"";
        if (ds >= 0) auto_styles += " style:data-style-name=\"N" + std::to_string(ds + 100) + "\"";
        auto_styles += ">" + cell_props(s, false) + "</style:style>";
    }
    // column / row / table styles
    std::map<std::string, std::string> colw, rowh;   // key -> style name
    std::string col_styles, row_styles, table_styles;
    auto col_style = [&](double chars) {
        char k[32]; snprintf(k, sizeof k, "%.4f", chars_to_pt(chars) / 72.0);
        auto it = colw.find(k);
        if (it != colw.end()) return it->second;
        std::string nm = "co" + std::to_string(colw.size() + 1);
        colw[k] = nm;
        col_styles += "<style:style style:name=\"" + nm + "\" style:family=\"table-column\"><style:table-column-properties fo:break-before=\"auto\" style:column-width=\"" + std::string(k) + "in\"/></style:style>";
        return nm;
    };
    auto row_style = [&](double pt, bool custom) {
        char k[48]; snprintf(k, sizeof k, "%.4f%c", pt / 72.0, custom ? 'c' : 'o');
        auto it = rowh.find(k);
        if (it != rowh.end()) return it->second;
        std::string nm = "ro" + std::to_string(rowh.size() + 1);
        rowh[k] = nm;
        char v[32]; snprintf(v, sizeof v, "%.4fin", pt / 72.0);
        row_styles += "<style:style style:name=\"" + nm + "\" style:family=\"table-row\"><style:table-row-properties style:row-height=\"" + v + "\" fo:break-before=\"auto\" style:use-optimal-row-height=\"" + (custom ? "false" : "true") + "\"/></style:style>";
        return nm;
    };
    for (int si = 0; si < b.sheet_count(); ++si) {
        const Worksheet& ws = b.sheets[si];
        table_styles += "<style:style style:name=\"ta" + std::to_string(si + 1) + "\" style:family=\"table\" style:master-page-name=\"Default\"><style:table-properties table:display=\"" +
                        std::string(ws.hidden ? "false" : "true") + "\" style:writing-mode=\"lr-tb\"" + (ws.tab_color ? " tableooo:tab-color=\"" + hexcol(ws.tab_color) + "\"" : "") + "/></style:style>";
    }
    // ---- tables ----
    std::string body;
    std::string validations;
    int nval = 0;
    std::map<std::pair<int, int>, std::string> dv_name;   // (sheet, dv index)
    for (int si = 0; si < b.sheet_count(); ++si) {
        const Worksheet& ws = b.sheets[si];
        for (int k = 0; k < (int)ws.validations.size(); ++k) {
            const DataValidation& dv = ws.validations[k];
            std::string nm = "val" + std::to_string(++nval);
            dv_name[{si, k}] = nm;
            std::string cond;
            auto f1 = of_to_a1("of:" + dv.f1);   // normalise
            (void)f1;
            auto of = [&](const std::string& f) { std::string x = a1_to_of(f); return x.substr(4); };   // drop "of:="
            static const char* ops[] = {"between", "not-between", "=", "!=", ">", "<", ">=", "<="};
            auto cmp = [&](const std::string& lhs) {
                if (dv.op == CmpOp::Between || dv.op == CmpOp::NotBetween) return std::string(lhs) + (dv.op == CmpOp::Between ? "cell-content-is-between(" : "cell-content-is-not-between(") + of(dv.f1) + "," + of(dv.f2) + ")";
                return lhs + "cell-content()" + ops[(int)dv.op] + of(dv.f1);
            };
            switch (dv.type) {
            case DataValidation::List: {
                std::string f = dv.f1;
                if (f.size() >= 2 && f.front() == '"') {
                    std::string items, cur;
                    std::string inner = f.substr(1, f.size() - 2);
                    std::vector<std::string> v; for (char ch : inner) { if (ch == ',') { v.push_back(cur); cur.clear(); } else cur += ch; } v.push_back(cur);
                    for (size_t i = 0; i < v.size(); ++i) { if (i) items += ';'; std::string it = v[i]; size_t a = it.find_first_not_of(' '); it = a == std::string::npos ? "" : it.substr(a); items += "\"" + it + "\""; }
                    cond = "of:cell-content-is-in-list(" + items + ")";
                } else cond = "of:cell-content-is-in-list(" + of(f) + ")";
                break;
            }
            case DataValidation::Whole: cond = "of:" + cmp("cell-content-is-whole-number() and "); break;
            case DataValidation::Decimal: cond = "of:" + cmp("cell-content-is-decimal-number() and "); break;
            case DataValidation::Date: cond = "of:" + cmp("cell-content-is-date() and "); break;
            case DataValidation::Time: cond = "of:" + cmp("cell-content-is-time() and "); break;
            case DataValidation::TextLength: {
                if (dv.op == CmpOp::Between || dv.op == CmpOp::NotBetween) cond = std::string("of:") + (dv.op == CmpOp::Between ? "cell-content-text-length-is-between(" : "cell-content-text-length-is-not-between(") + of(dv.f1) + "," + of(dv.f2) + ")";
                else cond = std::string("of:cell-content-text-length()") + ops[(int)dv.op] + of(dv.f1);
                break;
            }
            case DataValidation::Custom: cond = "of:is-true-formula(" + of(dv.f1) + ")"; break;
            default: break;
            }
            Range g = dv.ranges.empty() ? Range(0, 0, 0, 0) : dv.ranges[0];
            validations += "<table:content-validation table:name=\"" + nm + "\"" + (cond.empty() ? "" : " table:condition=\"" + esc(cond) + "\"") +
                           " table:allow-empty-cell=\"" + (dv.allow_blank ? "true" : "false") + "\"" + (dv.type == DataValidation::List ? std::string(" table:display-list=\"") + (dv.dropdown ? "unsorted" : "none") + "\"" : "") +
                           " table:base-cell-address=\"" + esc(quote_sheet_name(ws.name)) + "." + cell_name(g.r0, g.c0) + "\">";
            if (!dv.prompt.empty() || !dv.prompt_title.empty()) validations += "<table:help-message table:title=\"" + esc(dv.prompt_title) + "\" table:display=\"" + (dv.show_input ? "true" : "false") + "\"><text:p>" + esc(dv.prompt, false) + "</text:p></table:help-message>";
            static const char* em[] = {"stop", "warning", "information"};
            validations += "<table:error-message table:title=\"" + esc(dv.error_title) + "\" table:display=\"" + (dv.show_error ? "true" : "false") + "\" table:message-type=\"" + em[(int)dv.err_style] + "\"><text:p>" + esc(dv.error, false) + "</text:p></table:error-message>";
            validations += "</table:content-validation>";
        }
    }
    std::string db_ranges, named;
    for (int si = 0; si < b.sheet_count(); ++si) {
        const Worksheet& ws = b.sheets[si];
        std::string qn = quote_sheet_name(ws.name);
        body += "<table:table table:name=\"" + esc(ws.name) + "\" table:style-name=\"ta" + std::to_string(si + 1) + "\">";
        // columns
        int nc = std::max(ws.ncols(), (int)ws.cols.size());
        for (auto& [k, arr] : ws.spills) nc = std::max(nc, k.second + arr->cols);
        nc = std::max(nc, 1);
        for (int c = 0; c < nc;) {
            double w = ws.col_width(c);
            bool hid = ws.col_hidden(c);
            uint32_t st = c < (int)ws.cols.size() ? ws.cols[c].style : 0;
            int e = c;
            while (e + 1 < nc && ws.col_width(e + 1) == w && ws.col_hidden(e + 1) == hid && (e + 1 < (int)ws.cols.size() ? ws.cols[e + 1].style : 0) == st) ++e;
            body += "<table:table-column table:style-name=\"" + col_style(w) + "\"";
            if (e > c) body += " table:number-columns-repeated=\"" + std::to_string(e - c + 1) + "\"";
            if (hid) body += " table:visibility=\"collapse\"";
            int ce = ce_of(st);
            body += " table:default-cell-style-name=\"" + (ce >= 0 ? "ce" + std::to_string(ce + 1) : std::string("Default")) + "\"/>";
            c = e + 1;
        }
        // rows
        int nr = ws.nrows();
        for (auto& [k, arr] : ws.spills) nr = std::max(nr, k.first + arr->rows);
        std::map<std::pair<int, int>, const Range*> merge_anchor;
        std::vector<std::vector<char>> covered;
        for (auto& m : ws.merges) merge_anchor[{m.r0, m.c0}] = &m;
        auto is_cov = [&](int r, int c) { for (auto& m : ws.merges) if (m.contains(r, c) && !(m.r0 == r && m.c0 == c)) return true; return false; };
        int empty_run = 0;
        auto flush_empty = [&]() {
            if (!empty_run) return;
            body += "<table:table-row table:style-name=\"" + row_style(ws.default_row_height, false) + "\"";
            if (empty_run > 1) body += " table:number-rows-repeated=\"" + std::to_string(empty_run) + "\"";
            body += "><table:table-cell table:number-columns-repeated=\"" + std::to_string(nc) + "\"/></table:table-row>";
            empty_run = 0;
        };
        for (int r = 0; r < nr; ++r) {
            const RowStore* rs = r < (int)ws.rows.size() ? &ws.rows[r] : nullptr;
            bool any = false;
            int last = -1;
            int rcells = rs ? (int)rs->cells.size() : 0;
            for (int c = 0; c < std::max(rcells, nc); ++c) {
                const Cell* cl = c < rcells ? &rs->cells[c] : nullptr;
                bool in_dv = false;
                for (auto& dv : ws.validations) for (auto& g : dv.ranges) if (g.contains(r, c)) in_dv = true;
                if ((cl && (!cl->blank() || cl->style)) || !ws.value(r, c).is_empty() || merge_anchor.count({r, c}) || in_dv) { any = true; last = c; }
            }
            bool row_attr = rs && (rs->height >= 0 || rs->hidden || rs->filtered || rs->style);
            if (!any && !row_attr) { ++empty_run; continue; }
            flush_empty();
            body += "<table:table-row table:style-name=\"" + row_style(ws.row_height(r), rs && rs->height >= 0) + "\"";
            if (rs && rs->hidden) body += " table:visibility=\"collapse\"";
            else if (rs && rs->filtered) body += " table:visibility=\"filter\"";
            if (rs && rs->style) { int ce = ce_of(rs->style); if (ce >= 0) body += " table:default-cell-style-name=\"ce" + std::to_string(ce + 1) + "\""; }
            body += ">";
            int blank_run = 0;
            auto flush_blank = [&]() {
                if (!blank_run) return;
                body += "<table:table-cell";
                if (blank_run > 1) body += " table:number-columns-repeated=\"" + std::to_string(blank_run) + "\"";
                body += "/>";
                blank_run = 0;
            };
            for (int c = 0; c <= last; ++c) {
                if (is_cov(r, c)) { flush_blank(); body += "<table:covered-table-cell/>"; continue; }
                const Cell* cl = c < rcells ? &rs->cells[c] : nullptr;
                const Value& v = ws.value(r, c);
                bool has_f = cl && cl->f;
                uint32_t sid = cl ? cl->style : 0;
                int dvk = -1;
                for (int k = 0; k < (int)ws.validations.size() && dvk < 0; ++k) for (auto& g : ws.validations[k].ranges) if (g.contains(r, c)) { dvk = k; break; }
                auto ma = merge_anchor.find({r, c});
                if (v.is_empty() && !has_f && !sid && dvk < 0 && ma == merge_anchor.end()) { ++blank_run; continue; }
                flush_blank();
                body += "<table:table-cell";
                if (sid) { int ce = ce_of(sid); if (ce >= 0) body += " table:style-name=\"ce" + std::to_string(ce + 1) + "\""; }
                if (dvk >= 0) body += " table:content-validation-name=\"" + dv_name[{si, dvk}] + "\"";
                if (ma != merge_anchor.end()) {
                    if (ma->second->cols() > 1) body += " table:number-columns-spanned=\"" + std::to_string(ma->second->cols()) + "\"";
                    if (ma->second->rows() > 1) body += " table:number-rows-spanned=\"" + std::to_string(ma->second->rows()) + "\"";
                }
                if (has_f) {
                    std::string ft = formula_text(*cl->f, b, si, r, c);
                    body += " table:formula=\"" + esc(a1_to_of(ft)) + "\"";
                    auto sp = ws.spills.find({r, c});
                    if (sp != ws.spills.end()) body += " table:number-matrix-columns-spanned=\"" + std::to_string(sp->second->cols) + "\" table:number-matrix-rows-spanned=\"" + std::to_string(sp->second->rows) + "\"";
                }
                std::string disp = b.display_text(si, r, c);
                const std::string& nf = b.styles.get(ws.style_at(r, c)).numfmt;
                switch (v.type) {
                case Value::Number:
                    if (is_date_format(nf)) {
                        if (v.num < 1 && v.num >= 0) {
                            double secs = v.num * 86400;
                            int h = (int)(secs / 3600), m = (int)(std::fmod(secs, 3600) / 60);
                            double s = std::fmod(secs, 60);
                            char t[64]; snprintf(t, sizeof t, "PT%02dH%02dM%02.0fS", h, m, s);
                            body += std::string(" office:value-type=\"time\" office:time-value=\"") + t + "\"";
                        } else {
                            int y, mo, d; serial_to_ymd(v.num, y, mo, d, b.date1904);
                            double fr = v.num - std::floor(v.num);
                            char t[64];
                            if (fr > 1e-9) { long secs = std::lround(fr * 86400); snprintf(t, sizeof t, "%04d-%02d-%02dT%02ld:%02ld:%02ld", y, mo, d, secs / 3600, secs / 60 % 60, secs % 60); }
                            else snprintf(t, sizeof t, "%04d-%02d-%02d", y, mo, d);
                            body += std::string(" office:value-type=\"date\" office:date-value=\"") + t + "\"";
                        }
                    } else if (nf.find('%') != std::string::npos) body += " office:value-type=\"percentage\" office:value=\"" + numtxt(v.num) + "\"";
                    else body += " office:value-type=\"float\" office:value=\"" + numtxt(v.num) + "\"";
                    break;
                case Value::Bool: body += std::string(" office:value-type=\"boolean\" office:boolean-value=\"") + (v.num != 0 ? "true" : "false") + "\""; break;
                case Value::String: body += " office:value-type=\"string\""; if (has_f) body += " office:string-value=\"" + esc(v.str) + "\""; break;
                case Value::Error: if (has_f) body += " office:value-type=\"string\" office:string-value=\"\""; else body += " office:value-type=\"string\""; break;
                default: break;
                }
                body += ">";
                if (!disp.empty()) {
                    // paragraphs per line; runs of spaces as text:s
                    size_t p = 0;
                    for (;;) {
                        size_t e = disp.find('\n', p);
                        std::string line = disp.substr(p, e == std::string::npos ? std::string::npos : e - p);
                        body += "<text:p>";
                        for (size_t i = 0; i < line.size();) {
                            if (line[i] == ' ' && (i == 0 || line[i - 1] == ' ' || i + 1 == line.size())) {
                                size_t j = i; while (j < line.size() && line[j] == ' ') ++j;
                                body += j - i > 1 ? "<text:s text:c=\"" + std::to_string(j - i) + "\"/>" : "<text:s/>";
                                i = j; continue;
                            }
                            if (line[i] == '\t') { body += "<text:tab/>"; ++i; continue; }
                            size_t j = i; while (j < line.size() && line[j] != '\t' && !(line[j] == ' ' && (j + 1 == line.size() || line[j + 1] == ' '))) ++j;
                            body += esc(line.substr(i, j - i), false);
                            i = j;
                        }
                        body += "</text:p>";
                        if (e == std::string::npos) break;
                        p = e + 1;
                    }
                }
                body += "</table:table-cell>";
            }
            flush_blank();
            if (last + 1 < nc) body += "<table:table-cell table:number-columns-repeated=\"" + std::to_string(nc - last - 1) + "\"/>";
            body += "</table:table-row>";
        }
        flush_empty();
        // conditional formats (calcext)
        if (!ws.cond.empty()) {
            body += "<calcext:conditional-formats>";
            for (auto& cf : ws.cond) {
                std::string addr;
                for (auto& g : cf.ranges) { if (!addr.empty()) addr += ' '; addr += qn + "." + cell_name(g.r0, g.c0) + ":" + qn + "." + cell_name(g.r1, g.c1); }
                body += "<calcext:conditional-format calcext:target-range-address=\"" + esc(addr) + "\">";
                std::string base = qn + "." + cell_name(cf.ranges[0].r0, cf.ranges[0].c0);
                std::vector<const CondRule*> rules;
                for (auto& r : cf.rules) rules.push_back(&r);
                std::stable_sort(rules.begin(), rules.end(), [](const CondRule* a, const CondRule* b) { return a->priority < b->priority; });
                for (auto* rp : rules) {
                    const CondRule& r = *rp;
                    std::string sn = r.dxf >= 0 ? "PCBP_CF_" + std::to_string(r.dxf) : "Default";
                    auto of = [&](const std::string& f) { return a1_to_of(f).substr(4); };
                    auto cond = [&](const std::string& v) { body += "<calcext:condition calcext:apply-style-name=\"" + sn + "\" calcext:value=\"" + esc(v) + "\" calcext:base-cell-address=\"" + esc(base) + "\"/>"; };
                    static const char* ops[] = {"between", "not-between", "=", "!=", ">", "<", ">=", "<="};
                    std::string q = "\"" + r.text + "\"";
                    switch (r.type) {
                    case CondRule::CellIs:
                        if (r.op == CmpOp::Between || r.op == CmpOp::NotBetween) cond(std::string(ops[(int)r.op]) + "(" + of(r.f1) + "," + of(r.f2) + ")");
                        else cond(std::string(ops[(int)r.op]) + of(r.f1));
                        break;
                    case CondRule::Expression: cond("formula-is(" + of(r.f1) + ")"); break;
                    case CondRule::ContainsText: cond("contains-text(" + q + ")"); break;
                    case CondRule::NotContainsText: cond("not-contains-text(" + q + ")"); break;
                    case CondRule::BeginsWith: cond("begins-with(" + q + ")"); break;
                    case CondRule::EndsWith: cond("ends-with(" + q + ")"); break;
                    case CondRule::Duplicate: cond("duplicate"); break;
                    case CondRule::Unique: cond("unique"); break;
                    case CondRule::ContainsBlanks: cond("is-blank"); break;
                    case CondRule::NotContainsBlanks: cond("is-no-blank"); break;
                    case CondRule::ContainsErrors: cond("is-error"); break;
                    case CondRule::NotContainsErrors: cond("is-no-error"); break;
                    case CondRule::Top10: cond(std::string(r.bottom ? "bottom" : "top") + (r.percent ? "-percent(" : "-elements(") + std::to_string(r.rank) + ")"); break;
                    case CondRule::AboveAverage: cond(r.below ? (r.equal_avg ? "below-equal-average" : "below-average") : (r.equal_avg ? "above-equal-average" : "above-average")); break;
                    case CondRule::ColorScale: case CondRule::DataBar: {
                        static const char* ty[] = {"minimum", "maximum", "number", "percent", "percentile", "formula"};
                        if (r.type == CondRule::ColorScale) {
                            body += "<calcext:color-scale>";
                            for (auto& o : r.cfvo) body += std::string("<calcext:color-scale-entry calcext:value=\"") + esc(o.val.empty() ? "0" : o.val) + "\" calcext:type=\"" + ty[(int)o.type] + "\" calcext:color=\"" + hexcol(o.color) + "\"/>";
                            body += "</calcext:color-scale>";
                        } else {
                            body += "<calcext:data-bar calcext:positive-color=\"" + hexcol(r.bar_color) + "\" calcext:negative-color=\"#ff0000\" calcext:axis-color=\"#000000\">";
                            for (auto& o : r.cfvo) body += std::string("<calcext:formatting-entry calcext:value=\"") + esc(o.val.empty() ? "0" : o.val) + "\" calcext:type=\"" + ty[(int)o.type] + "\"/>";
                            body += "</calcext:data-bar>";
                        }
                        break;
                    }
                    }
                }
                body += "</calcext:conditional-format>";
            }
            body += "</calcext:conditional-formats>";
        }
        body += "</table:table>";
        if (ws.autofilter.active()) {
            const Range& g = ws.autofilter.range;
            db_ranges += "<table:database-range table:name=\"__Anonymous_Sheet_DB__" + std::to_string(si) + "\" table:target-range-address=\"" +
                         esc(qn + "." + cell_name(g.r0, g.c0) + ":" + qn + "." + cell_name(g.r1, g.c1)) + "\" table:display-filter-buttons=\"true\">";
            std::string conds;
            int nconds = 0;
            static const char* fo[] = {"=", "=", "=", "!=", ">", "<", ">=", "<="};
            for (auto& fc : ws.autofilter.cols) {
                if (!fc.active()) continue;
                if (fc.use_custom) {
                    auto one = [&](CmpOp op, const std::string& v) {
                        double d; bool num = parse_number_strict(v, d);
                        conds += "<table:filter-condition table:field-number=\"" + std::to_string(fc.col) + "\" table:value=\"" + esc(v) + "\" table:operator=\"" + fo[(int)op] + "\"" + (num ? " table:data-type=\"number\"" : "") + "/>";
                        ++nconds;
                    };
                    if (!fc.v2.empty()) { conds += fc.and_ ? "<table:filter-and>" : "<table:filter-or>"; one(fc.op1, fc.v1); one(fc.op2, fc.v2); conds += fc.and_ ? "</table:filter-and>" : "</table:filter-or>"; }
                    else one(fc.op1, fc.v1);
                } else {
                    std::string sv;
                    for (auto& v : fc.values) sv += "<table:filter-set-item table:value=\"" + esc(v) + "\"/>";
                    if (fc.blanks) sv += "<table:filter-set-item table:value=\"\"/>";
                    conds += "<table:filter-condition table:field-number=\"" + std::to_string(fc.col) + "\" table:value=\"" + esc(fc.values.empty() ? std::string() : fc.values[0]) + "\" table:operator=\"=\">" + sv + "</table:filter-condition>";
                    ++nconds;
                }
            }
            if (nconds) db_ranges += "<table:filter>" + (nconds > 1 ? "<table:filter-and>" + conds + "</table:filter-and>" : conds) + "</table:filter>";
            db_ranges += "</table:database-range>";
        }
    }
    for (auto& d : b.names) {
        std::string of = a1_to_of(d.formula).substr(4);
        int host = d.scope >= 0 ? d.scope : 0;
        std::string base = "$" + quote_sheet_name(b.sheets[host].name) + ".$A$1";
        Range rg;
        // plain range -> named-range, else named-expression
        std::string t = d.formula;
        size_t bang = t.rfind('!');
        std::string tail = bang == std::string::npos ? t : t.substr(bang + 1);
        std::string clean; for (char ch : tail) if (ch != '$') clean += ch;
        if (bang != std::string::npos && parse_range_name(clean, rg) && of.size() > 2 && of.front() == '[' && of.back() == ']')
            named += "<table:named-range table:name=\"" + esc(d.name) + "\" table:base-cell-address=\"" + esc(base) + "\" table:cell-range-address=\"" + esc(of.substr(1, of.size() - 2)) + "\"/>";
        else named += "<table:named-expression table:name=\"" + esc(d.name) + "\" table:base-cell-address=\"" + esc(base) + "\" table:expression=\"" + esc("of:=" + of) + "\"/>";
    }
    const char* ns =
        " xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\""
        " xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\""
        " xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\" xmlns:number=\"urn:oasis:names:tc:opendocument:xmlns:datastyle:1.0\""
        " xmlns:of=\"urn:oasis:names:tc:opendocument:xmlns:of:1.2\" xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\""
        " xmlns:config=\"urn:oasis:names:tc:opendocument:xmlns:config:1.0\" xmlns:xlink=\"http://www.w3.org/1999/xlink\""
        " xmlns:calcext=\"urn:org:documentfoundation:names:experimental:calc:xmlns:calcext:1.0\" xmlns:tableooo=\"http://openoffice.org/2009/table\""
        " xmlns:loext=\"urn:org:documentfoundation:names:experimental:office:xmlns:loext:1.0\"";
    std::string content = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-content") + ns + " office:version=\"1.3\">";
    content += "<office:automatic-styles>" + col_styles + row_styles + table_styles;
    for (auto& d : data_xml) content += d;
    content += auto_styles + "</office:automatic-styles><office:body><office:spreadsheet>";
    if (!validations.empty()) content += "<table:content-validations>" + validations + "</table:content-validations>";
    content += body;
    if (!named.empty()) content += "<table:named-expressions>" + named + "</table:named-expressions>";
    if (!db_ranges.empty()) content += "<table:database-ranges>" + db_ranges + "</table:database-ranges>";
    content += "</office:spreadsheet></office:body></office:document-content>";
    // styles.xml: default cell style + CF target styles
    std::string styles = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-styles") + ns + " office:version=\"1.3\"><office:styles>"
        "<style:default-style style:family=\"table-cell\"><style:text-properties fo:font-family=\"Calibri\" fo:font-size=\"11pt\"/></style:default-style>"
        "<style:style style:name=\"Default\" style:family=\"table-cell\"><style:text-properties fo:font-family=\"Calibri\" fo:font-size=\"11pt\"/></style:style>";
    for (size_t i = 0; i < b.dxfs.size(); ++i) {
        const Dxf& d = b.dxfs[i];
        Style s;
        s.font.name = "Calibri";
        if (d.bold) s.font.bold = *d.bold;
        if (d.italic) s.font.italic = *d.italic;
        if (d.strike) s.font.strike = *d.strike;
        if (d.underline) s.font.underline = *d.underline;
        if (d.font_color) s.font.color = *d.font_color;
        if (d.fill_color) { s.fill.pattern = FillPattern::Solid; s.fill.fg = *d.fill_color; }
        if (d.border) s.border = *d.border;
        int ds = d.numfmt ? ds_of(*d.numfmt) : -1;
        styles += "<style:style style:name=\"PCBP_CF_" + std::to_string(i) + "\" style:family=\"table-cell\" style:parent-style-name=\"Default\"";
        if (ds >= 0) styles += " style:data-style-name=\"N" + std::to_string(ds + 100) + "\"";
        styles += ">" + cell_props(s, true) + "</style:style>";
    }
    for (auto& d : data_xml) styles += d;   // data styles referenced by common styles must live here too
    styles += "</office:styles><office:automatic-styles><style:page-layout style:name=\"pm1\"/></office:automatic-styles>"
              "<office:master-styles><style:master-page style:name=\"Default\" style:page-layout-name=\"pm1\"/></office:master-styles></office:document-styles>";
    // settings.xml
    std::string settings = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-settings") + ns + " office:version=\"1.3\"><office:settings>"
        "<config:config-item-set config:name=\"ooo:view-settings\"><config:config-item-map-indexed config:name=\"Views\"><config:config-item-map-entry>"
        "<config:config-item config:name=\"ViewId\" config:type=\"string\">view1</config:config-item><config:config-item-map-named config:name=\"Tables\">";
    for (auto& ws : b.sheets) {
        settings += "<config:config-item-map-entry config:name=\"" + esc(ws.name) + "\">";
        auto item = [&](const char* n, const char* ty, const std::string& v) { settings += std::string("<config:config-item config:name=\"") + n + "\" config:type=\"" + ty + "\">" + v + "</config:config-item>"; };
        item("CursorPositionX", "int", std::to_string(ws.active_c));
        item("CursorPositionY", "int", std::to_string(ws.active_r));
        item("HorizontalSplitMode", "short", ws.freeze_cols ? "2" : "0");
        item("VerticalSplitMode", "short", ws.freeze_rows ? "2" : "0");
        item("HorizontalSplitPosition", "int", std::to_string(ws.freeze_cols));
        item("VerticalSplitPosition", "int", std::to_string(ws.freeze_rows));
        item("ActiveSplitRange", "short", ws.freeze_rows ? "2" : "3");
        item("PositionLeft", "int", "0"); item("PositionRight", "int", std::to_string(ws.freeze_cols));
        item("PositionTop", "int", "0"); item("PositionBottom", "int", std::to_string(ws.freeze_rows));
        item("ZoomValue", "int", std::to_string(ws.zoom));
        item("ShowGrid", "boolean", ws.show_grid ? "true" : "false");
        settings += "</config:config-item-map-entry>";
    }
    settings += "</config:config-item-map-named><config:config-item config:name=\"ActiveTable\" config:type=\"string\">" +
                esc(b.sheets[std::clamp(b.active_sheet, 0, b.sheet_count() - 1)].name, false) + "</config:config-item>"
                "</config:config-item-map-entry></config:config-item-map-indexed></config:config-item-set></office:settings></office:document-settings>";
    std::string manifest = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.3\">"
        "<manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.3\" manifest:media-type=\"application/vnd.oasis.opendocument.spreadsheet\"/>"
        "<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
        "<manifest:file-entry manifest:full-path=\"styles.xml\" manifest:media-type=\"text/xml\"/>"
        "<manifest:file-entry manifest:full-path=\"settings.xml\" manifest:media-type=\"text/xml\"/>"
        "<manifest:file-entry manifest:full-path=\"meta.xml\" manifest:media-type=\"text/xml\"/></manifest:manifest>";
    std::string meta = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-meta xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
        "xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\" office:version=\"1.3\"><office:meta><meta:generator>PCB Plotter</meta:generator></office:meta></office:document-meta>");
    std::vector<std::pair<std::string, std::string>> e = {
        {"mimetype", kMime}, {"META-INF/manifest.xml", manifest}, {"meta.xml", meta}, {"settings.xml", settings}, {"styles.xml", styles}, {"content.xml", content}};
    return zip_write(e, level, "mimetype");
}

// ===========================================================================
// read
// ===========================================================================
namespace {

struct OdsStyles {
    std::unordered_map<std::string, Style> cell;            // style name -> style
    std::unordered_map<std::string, std::string> data;      // data style name -> excel code
    std::unordered_map<std::string, std::string> cell_ds;   // cell style -> data style name
    std::unordered_map<std::string, std::string> parent;    // cell style -> parent
    std::unordered_map<std::string, double> col_width;      // pt
    std::unordered_map<std::string, double> row_height;     // pt
    std::unordered_map<std::string, bool> row_custom;
    std::unordered_map<std::string, Argb> tab_color;
    std::unordered_map<std::string, bool> table_hidden;
};

BorderSide parse_border(const std::string& v) {
    BorderSide s;
    std::string l = lower_utf8(v);
    if (l.empty() || l == "none" || l.find("hidden") != std::string::npos) return s;
    double w = len_pt(l);
    s.style = l.find("double") != std::string::npos ? BorderStyle::Double : l.find("dotted") != std::string::npos ? BorderStyle::Dotted :
              l.find("dash") != std::string::npos ? (w > 1.2 ? BorderStyle::MediumDashed : BorderStyle::Dashed) :
              w >= 2.2 ? BorderStyle::Thick : w > 1.2 ? BorderStyle::Medium : w < 0.3 ? BorderStyle::Hair : BorderStyle::Thin;
    size_t h = l.find('#');
    if (h != std::string::npos) s.color = parse_hex(l.substr(h, 7));
    return s;
}

// Parse <style:style>, data styles, column/row/table styles from a styles container.
void parse_styles(std::string_view xml, OdsStyles& S) {
    xmlp::Pull p(xml);
    std::string cur_name, cur_family, ds_name, ds_kind;
    std::string ds_code;
    bool ds_textual = false;
    Style cur;
    int depth_ds = 0;
    for (;;) {
        auto e = p.next();
        if (e == xmlp::Pull::Eof) break;
        if (e == xmlp::Pull::End) {
            auto l = p.local();
            if (l == "style" && !cur_name.empty()) { if (cur_family == "table-cell") S.cell[cur_name] = cur; cur_name.clear(); }
            else if (!ds_name.empty() && (l == "number-style" || l == "percentage-style" || l == "currency-style" || l == "date-style" || l == "time-style" || l == "text-style" || l == "boolean-style")) {
                if (depth_ds == 1) { S.data[ds_name] = ds_code.empty() ? "General" : ds_code; ds_name.clear(); }
                --depth_ds;
            }
            continue;
        }
        if (e == xmlp::Pull::Text) { continue; }
        auto l = p.local();
        std::string_view nm = p.name();
        bool in_number_ns = nm.rfind("number:", 0) == 0;
        if (nm == "style:style" || nm == "style:default-style") {
            cur_name = p.attr_or("style:name", nm == "style:default-style" ? "__default" : "");
            cur_family = p.attr_or("style:family");
            std::string par = p.attr_or("style:parent-style-name");
            cur = Style();
            if (!par.empty()) { S.parent[cur_name] = par; auto it = S.cell.find(par); if (it != S.cell.end()) cur = it->second; }
            else { auto it = S.cell.find("__default"); if (it != S.cell.end() && cur_family == "table-cell") cur = it->second; }
            std::string ds = p.attr_or("style:data-style-name");
            if (!ds.empty()) S.cell_ds[cur_name] = ds;
            if (p.empty_element() && cur_family == "table-cell") { S.cell[cur_name] = cur; p.next(); cur_name.clear(); }
            continue;
        }
        if (nm == "style:table-column-properties" && !cur_name.empty()) { if (p.attr("style:column-width")) S.col_width[cur_name] = len_pt(p.attr_or("style:column-width")); continue; }
        if (nm == "style:table-row-properties" && !cur_name.empty()) {
            if (p.attr("style:row-height")) S.row_height[cur_name] = len_pt(p.attr_or("style:row-height"));
            S.row_custom[cur_name] = p.attr_or("style:use-optimal-row-height", "true") == "false";
            continue;
        }
        if (nm == "style:table-properties" && !cur_name.empty()) {
            if (const std::string* tc = p.attr("tableooo:tab-color")) S.tab_color[cur_name] = parse_hex(*tc);
            S.table_hidden[cur_name] = p.attr_or("table:display", "true") == "false";
            continue;
        }
        if (nm == "style:table-cell-properties" && !cur_name.empty()) {
            if (const std::string* bg = p.attr("fo:background-color")) { if (*bg != "transparent") { cur.fill.pattern = FillPattern::Solid; cur.fill.fg = parse_hex(*bg); } }
            if (const std::string* b = p.attr("fo:border")) cur.border.left = cur.border.right = cur.border.top = cur.border.bottom = parse_border(*b);
            if (const std::string* b = p.attr("fo:border-left")) cur.border.left = parse_border(*b);
            if (const std::string* b = p.attr("fo:border-right")) cur.border.right = parse_border(*b);
            if (const std::string* b = p.attr("fo:border-top")) cur.border.top = parse_border(*b);
            if (const std::string* b = p.attr("fo:border-bottom")) cur.border.bottom = parse_border(*b);
            if (p.attr_or("fo:wrap-option") == "wrap") cur.wrap = true;
            if (p.attr_or("style:shrink-to-fit") == "true") cur.shrink = true;
            std::string va = p.attr_or("style:vertical-align");
            if (va == "top") cur.valign = VAlign::Top; else if (va == "middle") cur.valign = VAlign::Center; else if (va == "bottom") cur.valign = VAlign::Bottom;
            if (p.attr("style:rotation-angle")) { int a = (int)p.attr_num("style:rotation-angle"); if (a > 180) a -= 360; cur.rotation = (int16_t)std::clamp(a, -90, 90); }
            std::string cp = p.attr_or("style:cell-protect");
            if (!cp.empty()) { cur.locked = cp.find("protected") != std::string::npos; cur.hidden = cp.find("formula-hidden") != std::string::npos; if (cp == "none") cur.locked = false; }
            continue;
        }
        if (nm == "style:paragraph-properties" && !cur_name.empty()) {
            std::string ta = p.attr_or("fo:text-align");
            if (ta == "center") cur.halign = HAlign::Center; else if (ta == "end" || ta == "right") cur.halign = HAlign::Right;
            else if (ta == "start" || ta == "left") cur.halign = HAlign::Left; else if (ta == "justify") cur.halign = HAlign::Justify;
            if (p.attr("fo:margin-left")) { double mm = len_pt(p.attr_or("fo:margin-left")) / 72 * 25.4; if (mm > 0.5) cur.indent = (uint8_t)std::lround(mm / 2.5); }
            continue;
        }
        if (nm == "style:text-properties" && !cur_name.empty()) {
            if (const std::string* f = p.attr("fo:font-family")) { std::string x = *f; x.erase(std::remove(x.begin(), x.end(), '\''), x.end()); cur.font.name = x; }
            else if (const std::string* f2 = p.attr("style:font-name")) cur.font.name = *f2;
            if (p.attr("fo:font-size")) { std::string fs = p.attr_or("fo:font-size"); if (fs.find('%') == std::string::npos) cur.font.size = len_pt(fs); }
            if (p.attr("fo:font-weight")) { std::string w = p.attr_or("fo:font-weight"); cur.font.bold = w == "bold" || atoi(w.c_str()) >= 600; }
            if (p.attr("fo:font-style")) cur.font.italic = p.attr_or("fo:font-style") == "italic";
            if (p.attr("style:text-underline-style")) { std::string u = p.attr_or("style:text-underline-style"); cur.font.underline = u == "none" ? Underline::None : (p.attr_or("style:text-underline-type") == "double" ? Underline::Double : Underline::Single); }
            if (p.attr("style:text-line-through-style")) cur.font.strike = p.attr_or("style:text-line-through-style") != "none";
            if (p.attr("fo:color")) cur.font.color = parse_hex(p.attr_or("fo:color"));
            continue;
        }
        // data styles
        if (in_number_ns && (l == "number-style" || l == "percentage-style" || l == "currency-style" || l == "date-style" || l == "time-style" || l == "text-style" || l == "boolean-style")) {
            ++depth_ds;
            if (depth_ds == 1) { ds_name = p.attr_or("style:name"); ds_kind = std::string(l); ds_code.clear(); }
            continue;
        }
        if (!ds_name.empty() && in_number_ns) {
            if (l == "number") {
                int dec = (int)p.attr_num("number:decimal-places", 0), mi = (int)p.attr_num("number:min-integer-digits", 1);
                bool grp = p.attr_or("number:grouping") == "true";
                std::string s = grp ? (mi >= 1 ? "#,##0" : "#,###") : (mi >= 1 ? std::string((size_t)mi, '0') : "#");
                if (grp && mi > 1) s = "#,##" + std::string((size_t)mi, '0');
                if (dec) s += "." + std::string((size_t)dec, '0');
                ds_code += s;
            } else if (l == "scientific-number") {
                int dec = (int)p.attr_num("number:decimal-places", 2), me = (int)p.attr_num("number:min-exponent-digits", 2);
                ds_code += "0" + (dec ? "." + std::string((size_t)dec, '0') : std::string()) + "E+" + std::string((size_t)std::max(1, me), '0');
            } else if (l == "fraction") {
                int dd = (int)p.attr_num("number:min-denominator-digits", 1);
                ds_code += "# " + std::string((size_t)dd, '?') + "/" + std::string((size_t)dd, '?');
            } else if (l == "text") {
                std::string t = p.read_text();
                bool sep_only = !t.empty() && t.find_first_not_of("/-:., ") == std::string::npos;
                if (t == "%") ds_code += "%";
                else if (sep_only && (ds_kind == "date-style" || ds_kind == "time-style")) ds_code += t;
                else if (!t.empty()) { ds_code += "\""; for (char ch : t) { if (ch == '"') ds_code += "\\\""; else ds_code += ch; } ds_code += "\""; }
                continue;
            } else if (l == "currency-symbol") { std::string t = p.read_text(); if (!t.empty()) ds_code += "\"" + t + "\""; continue; }
            else if (l == "text-content") ds_code += "@";
            else if (l == "year") ds_code += p.attr_or("number:style") == "long" ? "yyyy" : "yy";
            else if (l == "month") { bool lg = p.attr_or("number:style") == "long", tx = p.attr_or("number:textual") == "true"; ds_code += tx ? (lg ? "mmmm" : "mmm") : (lg ? "mm" : "m"); }
            else if (l == "day") ds_code += p.attr_or("number:style") == "long" ? "dd" : "d";
            else if (l == "day-of-week") ds_code += p.attr_or("number:style") == "long" ? "dddd" : "ddd";
            else if (l == "hours") ds_code += (ds_kind == "time-style" && p.attr_or("number:truncate-on-overflow", "true") == "false") ? "[h]" : (p.attr_or("number:style") == "long" ? "hh" : "h");
            else if (l == "minutes") ds_code += p.attr_or("number:style") == "long" ? "mm" : "m";
            else if (l == "seconds") { ds_code += p.attr_or("number:style") == "long" ? "ss" : "s"; int d = (int)p.attr_num("number:decimal-places", 0); if (d) ds_code += "." + std::string((size_t)d, '0'); }
            else if (l == "am-pm") ds_code += "AM/PM";
            else if (l == "boolean") ds_code += "\"TRUE\";\"TRUE\";\"FALSE\"";
            (void)ds_textual;
            continue;
        }
    }
}

std::string resolve_numfmt(const OdsStyles& S, const std::string& style) {
    std::string s = style;
    for (int guard = 0; guard < 8 && !s.empty(); ++guard) {
        auto it = S.cell_ds.find(s);
        if (it != S.cell_ds.end()) { auto d = S.data.find(it->second); return d != S.data.end() ? d->second : "General"; }
        auto p = S.parent.find(s);
        if (p == S.parent.end()) break;
        s = p->second;
    }
    return "General";
}

// "Sheet1.A1:Sheet1.B5", "'My Sheet'.$A$1:.$B$5", "$Sheet1.A1" -> sheet name + range
bool parse_odf_range(const std::string& addr, std::string& sheet, Range& rg) {
    auto part = [&](const std::string& p, std::string& sh, int& r, int& c) {
        size_t dot = std::string::npos; bool q = false;
        for (size_t k = 0; k < p.size(); ++k) { if (p[k] == '\'') q = !q; else if (p[k] == '.' && !q) dot = k; }
        std::string s = dot == std::string::npos ? std::string() : p.substr(0, dot), cell = dot == std::string::npos ? p : p.substr(dot + 1);
        if (!s.empty() && s[0] == '$') s.erase(0, 1);
        if (s.size() >= 2 && s[0] == '\'') { std::string u; for (size_t k = 1; k + 1 < s.size(); ++k) { if (s[k] == '\'' && s[k + 1] == '\'') { u += '\''; ++k; } else u += s[k]; } s = u; }
        sh = s;
        std::string cl; for (char ch : cell) if (ch != '$') cl += ch;
        return parse_cell_name(cl, r, c);
    };
    size_t colon = std::string::npos; bool q = false;
    for (size_t k = 0; k < addr.size(); ++k) { if (addr[k] == '\'') q = !q; else if (addr[k] == ':' && !q) { colon = k; break; } }
    std::string s1, s2; int r0, c0, r1, c1;
    if (!part(colon == std::string::npos ? addr : addr.substr(0, colon), s1, r0, c0)) return false;
    if (colon == std::string::npos) { r1 = r0; c1 = c0; }
    else if (!part(addr.substr(colon + 1), s2, r1, c1)) return false;
    sheet = s1;
    rg = Range(r0, c0, r1, c1).norm();
    return true;
}

double parse_iso_duration(const std::string& t) {
    // PT12H30M15.5S (also P1DT...)
    double days = 0, h = 0, m = 0, s = 0;
    size_t i = 0; bool time = false;
    while (i < t.size()) {
        char ch = t[i];
        if (ch == 'P' || ch == '-') { ++i; continue; }
        if (ch == 'T') { time = true; ++i; continue; }
        size_t j = i; while (j < t.size() && (isdigit((unsigned char)t[j]) || t[j] == '.')) ++j;
        double v = atof(t.substr(i, j - i).c_str());
        if (j >= t.size()) break;
        char u = t[j];
        if (u == 'D') days = v; else if (u == 'H' && time) h = v; else if (u == 'M' && time) m = v; else if (u == 'S') s = v;
        i = j + 1;
    }
    return days + (h * 3600 + m * 60 + s) / 86400;
}
double parse_iso_date(const std::string& t, bool d1904) {
    int y = 0, mo = 1, d = 1, h = 0, mi = 0; double s = 0;
    if (sscanf(t.c_str(), "%d-%d-%d", &y, &mo, &d) < 3) return 0;
    size_t T = t.find('T');
    if (T != std::string::npos) sscanf(t.c_str() + T + 1, "%d:%d:%lf", &h, &mi, &s);
    return date_serial(y, mo, d, d1904) + time_serial(h, mi, s);
}

} // namespace

bool ods_read(const std::vector<uint8_t>& data, Book& book, std::string& err) {
    std::vector<ArchiveEntry> ents;
    if (!read_zip(data.data(), data.size(), ents, err)) return false;
    std::string content, styles, settings;
    for (auto& e : ents) {
        if (e.name == "content.xml") content.assign(e.data.begin(), e.data.end());
        else if (e.name == "styles.xml") styles.assign(e.data.begin(), e.data.end());
        else if (e.name == "settings.xml") settings.assign(e.data.begin(), e.data.end());
    }
    if (content.empty()) { err = "not an OpenDocument file (content.xml missing)"; return false; }
    OdsStyles S;
    if (!styles.empty()) parse_styles(styles, S);
    parse_styles(content, S);
    Book b;
    b.sheets.clear();
    std::unordered_map<std::string, uint32_t> style_id;
    auto sid_of = [&](const std::string& nm) -> uint32_t {
        if (nm.empty() || nm == "Default") return 0;
        auto it = style_id.find(nm);
        if (it != style_id.end()) return it->second;
        Style s;
        auto c = S.cell.find(nm);
        if (c != S.cell.end()) s = c->second;
        s.numfmt = resolve_numfmt(S, nm);
        uint32_t id = b.styles.intern(s);
        style_id[nm] = id;
        return id;
    };
    struct PF { int si, r, c; std::string f; Value cached; };
    std::vector<PF> pf;
    std::vector<std::pair<int, Range>> matrices;   // array/spill results: covered cells hold cached copies
    struct PDV { std::string name; int si; int r, c; };
    std::vector<PDV> dv_cells;
    struct DVDef { std::string cond, base; bool allow_blank = true, list = true; std::string err_title, err_msg, help_title, help_msg; int etype = 0; bool show_err = true, show_help = false; };
    std::unordered_map<std::string, DVDef> dv_defs;
    struct DbRange { std::string target; std::vector<FilterColumn> cols; };
    std::vector<DbRange> dbr;
    std::vector<std::pair<int, std::pair<std::string, std::string>>> cf_pending;   // unused placeholder
    xmlp::Pull p(content);
    int si = -1, row = 0, col = 0;
    std::vector<std::string> col_default_style;
    bool in_table = false;
    // per-row state
    int row_repeat = 1;
    std::string row_style_default;
    // validations parse state
    DVDef* cur_dv = nullptr;
    int msg_kind = 0;
    // conditional formats
    CondFormat* cur_cf = nullptr;
    CondRule* cur_rule = nullptr;
    std::unordered_map<std::string, int> dxf_of_style;
    // filter
    DbRange* cur_db = nullptr;
    std::vector<int> filter_bool_stack;
    // named
    while (true) {
        auto e = p.next();
        if (e == xmlp::Pull::Eof) break;
        std::string_view nm = p.name();
        if (e == xmlp::Pull::End) {
            if (nm == "table:table") { in_table = false; }
            else if (nm == "table:content-validation") cur_dv = nullptr;
            else if (nm == "calcext:conditional-format") cur_cf = nullptr;
            else if (nm == "calcext:color-scale" || nm == "calcext:data-bar") cur_rule = nullptr;
            else if (nm == "table:database-range") cur_db = nullptr;
            else if (nm == "table:filter-and" || nm == "table:filter-or") { if (!filter_bool_stack.empty()) filter_bool_stack.pop_back(); }
            continue;
        }
        if (e != xmlp::Pull::Start) continue;
        if (nm == "table:content-validation") {
            std::string vn = p.attr_or("table:name");
            DVDef d;
            d.cond = p.attr_or("table:condition");
            d.base = p.attr_or("table:base-cell-address");
            d.allow_blank = p.attr_or("table:allow-empty-cell", "true") == "true";
            d.list = p.attr_or("table:display-list", "unsorted") != "none";
            dv_defs[vn] = d;
            cur_dv = &dv_defs[vn];
            continue;
        }
        if (cur_dv && (nm == "table:help-message" || nm == "table:error-message")) {
            msg_kind = nm == "table:help-message" ? 1 : 2;
            if (msg_kind == 1) { cur_dv->help_title = p.attr_or("table:title"); cur_dv->show_help = p.attr_or("table:display") == "true"; }
            else { cur_dv->err_title = p.attr_or("table:title"); cur_dv->show_err = p.attr_or("table:display", "true") == "true"; std::string t = p.attr_or("table:message-type", "stop"); cur_dv->etype = t == "warning" ? 1 : t == "information" ? 2 : 0; }
            std::string txt = p.read_text();
            (msg_kind == 1 ? cur_dv->help_msg : cur_dv->err_msg) = txt;
            continue;
        }
        if (nm == "table:table") {
            Worksheet ws;
            ws.name = p.attr_or("table:name", "Sheet" + std::to_string(b.sheet_count() + 1));
            ws.id = (uint32_t)b.sheet_count() + 1;
            std::string ts = p.attr_or("table:style-name");
            if (S.tab_color.count(ts)) ws.tab_color = S.tab_color[ts];
            if (S.table_hidden.count(ts)) ws.hidden = S.table_hidden[ts];
            b.sheets.push_back(std::move(ws));
            si = b.sheet_count() - 1;
            row = 0; col = 0;
            col_default_style.clear();
            in_table = true;
            continue;
        }
        if (!in_table) {
            if (nm == "table:named-range" || nm == "table:named-expression") {
                DefinedName d;
                d.name = p.attr_or("table:name");
                std::string ex = nm == "table:named-range" ? "[" + p.attr_or("table:cell-range-address") + "]" : p.attr_or("table:expression");
                d.formula = of_to_a1(ex);
                if (!d.name.empty()) b.names.push_back(d);
                continue;
            }
            if (nm == "table:database-range") {
                dbr.push_back(DbRange());
                cur_db = &dbr.back();
                cur_db->target = p.attr_or("table:target-range-address");
                if (p.attr_or("table:display-filter-buttons") != "true") { dbr.pop_back(); cur_db = nullptr; }
                continue;
            }
            if (cur_db && (nm == "table:filter-and" || nm == "table:filter-or")) { filter_bool_stack.push_back(nm == "table:filter-and"); continue; }
            if (cur_db && nm == "table:filter-condition") {
                int field = (int)p.attr_num("table:field-number");
                std::string v = p.attr_or("table:value"), op = p.attr_or("table:operator", "=");
                FilterColumn* fc = nullptr;
                for (auto& x : cur_db->cols) if (x.col == field) fc = &x;
                if (!fc) { cur_db->cols.push_back(FilterColumn()); fc = &cur_db->cols.back(); fc->col = field; }
                bool set_items = false;
                if (!p.empty_element()) {
                    int d = 1;
                    while (d > 0) {
                        auto e2 = p.next();
                        if (e2 == xmlp::Pull::Eof) break;
                        if (e2 == xmlp::Pull::Start) { ++d; if (p.name() == "table:filter-set-item") { set_items = true; std::string iv = p.attr_or("table:value"); if (iv.empty()) fc->blanks = true; else fc->values.push_back(iv); } }
                        else if (e2 == xmlp::Pull::End) --d;
                    }
                } else p.next();
                if (!set_items) {
                    static const char* fo[] = {"", "", "=", "!=", ">", "<", ">=", "<="};
                    int k = 2;
                    for (int q = 2; q < 8; ++q) if (op == fo[q]) k = q;
                    if (op == "=" && filter_bool_stack.empty()) { fc->values.push_back(v); }
                    else {
                        fc->use_custom = true;
                        if (fc->v1.empty()) { fc->op1 = (CmpOp)k; fc->v1 = v; }
                        else { fc->op2 = (CmpOp)k; fc->v2 = v; fc->and_ = filter_bool_stack.empty() || filter_bool_stack.back(); }
                    }
                }
                continue;
            }
            continue;
        }
        Worksheet& ws = b.sheets[si];
        if (nm == "table:table-column") {
            int rep = std::max(1, (int)p.attr_num("table:number-columns-repeated", 1));
            std::string cs = p.attr_or("table:style-name"), ds = p.attr_or("table:default-cell-style-name");
            bool hid = p.attr_or("table:visibility", "visible") != "visible";
            int start = (int)col_default_style.size();
            int n = std::min(rep, MAX_COLS - start);
            auto w = S.col_width.find(cs);
            uint32_t dsid = sid_of(ds);
            for (int k = 0; k < n; ++k) {
                col_default_style.push_back(ds);
                int c = start + k;
                bool any = hid || dsid || (w != S.col_width.end() && std::fabs(pt_to_chars(w->second) - ws.default_col_width) > 0.05);
                if (!any || c > 1023) continue;
                ColInfo& ci = ws.col(c);
                if (w != S.col_width.end()) ci.width = pt_to_chars(w->second);
                ci.hidden = hid;
                ci.style = dsid;
            }
            continue;
        }
        if (nm == "table:table-row") {
            row_repeat = std::max(1, (int)p.attr_num("table:number-rows-repeated", 1));
            std::string rsn = p.attr_or("table:style-name");
            std::string vis = p.attr_or("table:visibility", "visible");
            row_style_default = p.attr_or("table:default-cell-style-name");
            col = 0;
            // read cells of this row into a temp list, then replicate for repeats
            struct TC { int c, rep; Cell cell; std::string formula; int span_c = 1, span_r = 1, mat_c = 0, mat_r = 0; std::string dv; bool covered; bool has_content; };
            std::vector<TC> cells;
            bool any_content = false;
            if (!p.empty_element()) {
                int depth = 1;
                while (depth > 0) {
                    auto e2 = p.next();
                    if (e2 == xmlp::Pull::Eof) break;
                    if (e2 == xmlp::Pull::End) { --depth; continue; }
                    if (e2 != xmlp::Pull::Start) continue;
                    std::string_view cn = p.name();
                    if (cn != "table:table-cell" && cn != "table:covered-table-cell") { ++depth; continue; }
                    TC tc;
                    tc.c = col;
                    tc.covered = cn == "table:covered-table-cell";
                    tc.rep = std::max(1, (int)p.attr_num("table:number-columns-repeated", 1));
                    std::string sn = p.attr_or("table:style-name");
                    if (sn.empty()) sn = col < (int)col_default_style.size() ? col_default_style[col] : std::string();
                    if (sn.empty()) sn = row_style_default;
                    tc.cell.style = sid_of(sn);
                    std::string vt = p.attr_or("office:value-type");
                    tc.formula = p.attr_or("table:formula");
                    tc.span_c = (int)p.attr_num("table:number-columns-spanned", 1);
                    tc.span_r = (int)p.attr_num("table:number-rows-spanned", 1);
                    tc.dv = p.attr_or("table:content-validation-name");
                    tc.mat_c = (int)p.attr_num("table:number-matrix-columns-spanned", 0);
                    tc.mat_r = (int)p.attr_num("table:number-matrix-rows-spanned", 0);
                    Value v;
                    if (vt == "float" || vt == "percentage" || vt == "currency") v = Value::number(p.attr_num("office:value"));
                    else if (vt == "date") v = Value::number(parse_iso_date(p.attr_or("office:date-value"), b.date1904));
                    else if (vt == "time") v = Value::number(parse_iso_duration(p.attr_or("office:time-value")));
                    else if (vt == "boolean") { std::string bv = p.attr_or("office:boolean-value"); v = Value::boolean(bv == "true" || bv == "1"); }
                    std::string sv = p.attr_or("office:string-value");
                    bool has_sv = p.attr("office:string-value") != nullptr;
                    // text content
                    std::string txt;
                    int paras = 0;
                    if (!p.empty_element()) {
                        int d2 = 1;
                        while (d2 > 0) {
                            auto e3 = p.next();
                            if (e3 == xmlp::Pull::Eof) break;
                            if (e3 == xmlp::Pull::Start) {
                                std::string_view tn = p.name();
                                if (tn == "text:p") { if (paras++) txt += '\n'; ++d2; if (p.empty_element()) { p.next(); --d2; } continue; }
                                if (tn == "text:s") { int n = std::max(1, (int)p.attr_num("text:c", 1)); txt.append((size_t)std::min(n, 1000), ' '); if (p.empty_element()) p.next(); else ++d2; continue; }
                                if (tn == "text:tab") { txt += '\t'; if (p.empty_element()) p.next(); else ++d2; continue; }
                                if (tn == "text:line-break") { txt += '\n'; if (p.empty_element()) p.next(); else ++d2; continue; }
                                if (tn == "office:annotation") { p.skip_element(); continue; }
                                ++d2;
                            } else if (e3 == xmlp::Pull::End) --d2;
                            else if (e3 == xmlp::Pull::Text) txt += p.text();
                        }
                    } else p.next();
                    if (vt == "string") v = Value::text(has_sv ? sv : txt);
                    else if (vt.empty() && !txt.empty()) v = Value::text(txt);
                    if (!tc.formula.empty() && vt == "string" && !txt.empty()) { Err er = err_from_text(txt); if (er != Err::None) v = Value::error(er); }
                    tc.cell.v = v;
                    tc.has_content = !v.is_empty() || !tc.formula.empty() || tc.cell.style || !tc.dv.empty() || tc.span_c > 1 || tc.span_r > 1;
                    if (tc.has_content) any_content = true;
                    col += tc.rep;
                    cells.push_back(std::move(tc));
                }
            } else p.next();
            int reps = any_content || vis != "visible" || S.row_custom[rsn] ? std::min(row_repeat, std::max(1, MAX_ROWS - row)) : row_repeat;
            if (any_content && reps > 10000) reps = 10000;
            for (int k = 0; k < reps && row + k < MAX_ROWS; ++k) {
                int r = row + k;
                if (vis != "visible") { if (vis == "filter") ws.row(r).filtered = true; else ws.row(r).hidden = true; }
                auto rh = S.row_height.find(rsn);
                if (rh != S.row_height.end() && S.row_custom[rsn]) ws.row(r).height = (float)rh->second;
                if (!any_content) continue;
                for (auto& tc : cells) {
                    if (!tc.has_content) continue;
                    int crep = std::min(tc.rep, 1024);
                    for (int q = 0; q < crep && tc.c + q < MAX_COLS; ++q) {
                        int c = tc.c + q;
                        Cell& cl = ws.at(r, c);
                        cl = tc.cell;
                        if (!tc.formula.empty()) pf.push_back({si, r, c, tc.formula, tc.cell.v});
                        if (!tc.formula.empty() && (tc.mat_c > 1 || tc.mat_r > 1)) matrices.push_back({si, Range(r, c, r + std::max(1, tc.mat_r) - 1, c + std::max(1, tc.mat_c) - 1)});
                        if (tc.span_c > 1 || tc.span_r > 1) ws.merges.push_back(Range(r, c, r + tc.span_r - 1, c + tc.span_c - 1));
                        if (!tc.dv.empty()) dv_cells.push_back({tc.dv, si, r, c});
                    }
                }
            }
            row += row_repeat;
            continue;
        }
        // conditional formats
        if (nm == "calcext:conditional-format") {
            ws.cond.push_back(CondFormat());
            cur_cf = &ws.cond.back();
            std::string addr = p.attr_or("calcext:target-range-address");
            size_t q = 0;
            while (q < addr.size()) {
                size_t e3 = addr.find(' ', q);
                std::string part = addr.substr(q, e3 == std::string::npos ? std::string::npos : e3 - q);
                std::string sh; Range rg;
                if (parse_odf_range(part, sh, rg)) cur_cf->ranges.push_back(rg);
                if (e3 == std::string::npos) break;
                q = e3 + 1;
            }
            continue;
        }
        if (cur_cf && (nm == "calcext:condition")) {
            CondRule r;
            r.priority = (int)cur_cf->rules.size() + 1;
            std::string sn = p.attr_or("calcext:apply-style-name"), v = p.attr_or("calcext:value");
            if (sn.rfind("PCBP_CF_", 0) == 0) r.dxf = atoi(sn.c_str() + 8);
            else if (!sn.empty() && sn != "Default") {
                auto it = dxf_of_style.find(sn);
                if (it == dxf_of_style.end()) {
                    Dxf d;
                    auto cs = S.cell.find(sn);
                    if (cs != S.cell.end()) {
                        const Style& st = cs->second;
                        if (st.font.bold) d.bold = true;
                        if (st.font.italic) d.italic = true;
                        if (st.font.color) d.font_color = st.font.color;
                        if (st.fill.pattern != FillPattern::None) d.fill_color = st.fill.fg;
                    }
                    b.dxfs.push_back(d);
                    it = dxf_of_style.emplace(sn, (int)b.dxfs.size() - 1).first;
                }
                r.dxf = it->second;
            }
            auto args = [&](const std::string& s) { size_t a = s.find('('), z = s.rfind(')'); return a == std::string::npos || z == std::string::npos ? std::string() : s.substr(a + 1, z - a - 1); };
            auto unq = [](std::string s) { if (s.size() >= 2 && s.front() == '"') s = s.substr(1, s.size() - 2); return s; };
            static const struct { const char* p; CmpOp op; } ops[] = {{"<=", CmpOp::LessEqual}, {">=", CmpOp::GreaterEqual}, {"!=", CmpOp::NotEqual}, {"<", CmpOp::Less}, {">", CmpOp::Greater}, {"=", CmpOp::Equal}};
            if (v.rfind("between(", 0) == 0 || v.rfind("not-between(", 0) == 0) {
                r.type = CondRule::CellIs; r.op = v[0] == 'n' ? CmpOp::NotBetween : CmpOp::Between;
                std::string a = args(v);
                // split at top-level ',' or ';'
                int dep = 0; size_t sp = std::string::npos; bool qq = false;
                for (size_t k = 0; k < a.size(); ++k) { if (a[k] == '"') qq = !qq; if (qq) continue; if (a[k] == '(') ++dep; else if (a[k] == ')') --dep; else if ((a[k] == ',' || a[k] == ';') && !dep) { sp = k; break; } }
                r.f1 = of_to_a1(a.substr(0, sp)); r.f2 = sp == std::string::npos ? "" : of_to_a1(a.substr(sp + 1));
            } else if (v.rfind("formula-is(", 0) == 0) { r.type = CondRule::Expression; r.f1 = of_to_a1(args(v)); }
            else if (v.rfind("contains-text(", 0) == 0) { r.type = CondRule::ContainsText; r.text = unq(args(v)); }
            else if (v.rfind("not-contains-text(", 0) == 0) { r.type = CondRule::NotContainsText; r.text = unq(args(v)); }
            else if (v.rfind("begins-with(", 0) == 0) { r.type = CondRule::BeginsWith; r.text = unq(args(v)); }
            else if (v.rfind("ends-with(", 0) == 0) { r.type = CondRule::EndsWith; r.text = unq(args(v)); }
            else if (v == "duplicate") r.type = CondRule::Duplicate;
            else if (v == "unique") r.type = CondRule::Unique;
            else if (v == "is-blank") r.type = CondRule::ContainsBlanks;
            else if (v == "is-no-blank") r.type = CondRule::NotContainsBlanks;
            else if (v == "is-error") r.type = CondRule::ContainsErrors;
            else if (v == "is-no-error") r.type = CondRule::NotContainsErrors;
            else if (v.rfind("top-", 0) == 0 || v.rfind("bottom-", 0) == 0) { r.type = CondRule::Top10; r.bottom = v[0] == 'b'; r.percent = v.find("percent") != std::string::npos; r.rank = atoi(args(v).c_str()); }
            else if (v.find("average") != std::string::npos) { r.type = CondRule::AboveAverage; r.below = v.rfind("below", 0) == 0; r.equal_avg = v.find("equal") != std::string::npos; }
            else {
                r.type = CondRule::CellIs;
                for (auto& o : ops) if (v.rfind(o.p, 0) == 0) { r.op = o.op; r.f1 = of_to_a1(v.substr(strlen(o.p))); break; }
            }
            cur_cf->rules.push_back(r);
            continue;
        }
        if (cur_cf && (nm == "calcext:color-scale" || nm == "calcext:data-bar")) {
            CondRule r;
            r.priority = (int)cur_cf->rules.size() + 1;
            r.type = nm == "calcext:color-scale" ? CondRule::ColorScale : CondRule::DataBar;
            if (r.type == CondRule::DataBar) r.bar_color = parse_hex(p.attr_or("calcext:positive-color", "#638ec6"));
            cur_cf->rules.push_back(r);
            cur_rule = &cur_cf->rules.back();
            continue;
        }
        if (cur_rule && (nm == "calcext:color-scale-entry" || nm == "calcext:formatting-entry")) {
            CfValueObject o;
            static const char* ty[] = {"minimum", "maximum", "number", "percent", "percentile", "formula"};
            std::string t = p.attr_or("calcext:type", "minimum");
            for (int k = 0; k < 6; ++k) if (t == ty[k]) o.type = (CfValueObject::Type)k;
            if (t == "auto-minimum") o.type = CfValueObject::Min;
            if (t == "auto-maximum") o.type = CfValueObject::Max;
            o.val = p.attr_or("calcext:value");
            if (o.type == CfValueObject::Min || o.type == CfValueObject::Max) o.val.clear();
            o.color = parse_hex(p.attr_or("calcext:color", "#000000"));
            cur_rule->cfvo.push_back(o);
            continue;
        }
    }
    if (b.sheets.empty()) { err = "no tables in content.xml"; return false; }
    // validations
    std::unordered_map<std::string, int> dv_index;   // name|si -> index in ws.validations
    for (auto& c : dv_cells) {
        auto def = dv_defs.find(c.name);
        if (def == dv_defs.end()) continue;
        std::string key = c.name + "|" + std::to_string(c.si);
        auto it = dv_index.find(key);
        Worksheet& ws = b.sheets[c.si];
        if (it == dv_index.end()) {
            DataValidation dv;
            const DVDef& d = def->second;
            std::string cond = d.cond;
            if (cond.rfind("of:", 0) == 0) cond = cond.substr(3);
            else if (cond.rfind("oooc:", 0) == 0) cond = cond.substr(5);
            auto args = [&](const std::string& s, size_t from) { size_t a = s.find('(', from), z = s.rfind(')'); return a == std::string::npos || z == std::string::npos ? std::string() : s.substr(a + 1, z - a - 1); };
            auto split2 = [&](const std::string& a, std::string& x, std::string& y) {
                int dep = 0; bool qq = false;
                for (size_t k = 0; k < a.size(); ++k) { if (a[k] == '"') qq = !qq; if (qq) continue; if (a[k] == '(') ++dep; else if (a[k] == ')') --dep; else if ((a[k] == ',' || a[k] == ';') && !dep) { x = of_to_a1(a.substr(0, k)); y = of_to_a1(a.substr(k + 1)); return; } }
                x = of_to_a1(a);
            };
            auto cmp_part = [&](const std::string& s) {
                size_t k = s.find("cell-content-is-between");
                if (k != std::string::npos) { dv.op = CmpOp::Between; split2(args(s, k), dv.f1, dv.f2); return; }
                k = s.find("cell-content-is-not-between");
                if (k != std::string::npos) { dv.op = CmpOp::NotBetween; split2(args(s, k), dv.f1, dv.f2); return; }
                k = s.find("cell-content()");
                size_t kk = s.find("cell-content-text-length()");
                size_t base = kk != std::string::npos ? kk + 26 : (k != std::string::npos ? k + 14 : std::string::npos);
                if (base == std::string::npos) return;
                std::string rest = s.substr(base);
                static const struct { const char* p; CmpOp op; } ops[] = {{"<=", CmpOp::LessEqual}, {">=", CmpOp::GreaterEqual}, {"!=", CmpOp::NotEqual}, {"<", CmpOp::Less}, {">", CmpOp::Greater}, {"=", CmpOp::Equal}};
                for (auto& o : ops) if (rest.rfind(o.p, 0) == 0) { dv.op = o.op; dv.f1 = of_to_a1(rest.substr(strlen(o.p))); break; }
            };
            if (cond.rfind("cell-content-is-in-list", 0) == 0) {
                dv.type = DataValidation::List;
                std::string a = args(cond, 0);
                if (!a.empty() && a[0] == '"') {
                    std::string items; bool qq = false; std::string cur;
                    for (size_t k = 0; k < a.size(); ++k) {
                        if (a[k] == '"') { if (qq && k + 1 < a.size() && a[k + 1] == '"') { cur += '"'; ++k; continue; } qq = !qq; continue; }
                        if (!qq && a[k] == ';') { items += (items.empty() ? "" : ",") + cur; cur.clear(); continue; }
                        if (qq) cur += a[k];
                    }
                    items += (items.empty() ? "" : ",") + cur;
                    dv.f1 = "\"" + items + "\"";
                } else dv.f1 = of_to_a1(a);
            } else if (cond.find("cell-content-text-length") != std::string::npos) { dv.type = DataValidation::TextLength; if (cond.find("is-between") != std::string::npos || cond.find("is-not-between") != std::string::npos) { size_t k = cond.find("text-length-is"); dv.op = cond.find("not-between") != std::string::npos ? CmpOp::NotBetween : CmpOp::Between; split2(args(cond, k), dv.f1, dv.f2); } else cmp_part(cond); }
            else if (cond.find("whole-number") != std::string::npos) { dv.type = DataValidation::Whole; cmp_part(cond.substr(cond.find("and") + 3)); }
            else if (cond.find("decimal-number") != std::string::npos) { dv.type = DataValidation::Decimal; cmp_part(cond.substr(cond.find("and") + 3)); }
            else if (cond.find("is-date") != std::string::npos) { dv.type = DataValidation::Date; cmp_part(cond.substr(cond.find("and") + 3)); }
            else if (cond.find("is-time") != std::string::npos) { dv.type = DataValidation::Time; cmp_part(cond.substr(cond.find("and") + 3)); }
            else if (cond.rfind("is-true-formula", 0) == 0) { dv.type = DataValidation::Custom; dv.f1 = of_to_a1(args(cond, 0)); }
            dv.allow_blank = d.allow_blank;
            dv.dropdown = d.list;
            dv.error_title = d.err_title; dv.error = d.err_msg; dv.prompt_title = d.help_title; dv.prompt = d.help_msg;
            dv.err_style = (DataValidation::ErrStyle)d.etype;
            dv.show_error = d.show_err; dv.show_input = d.show_help;
            ws.validations.push_back(dv);
            it = dv_index.emplace(key, (int)ws.validations.size() - 1).first;
        }
        DataValidation& dv = ws.validations[it->second];
        // extend an existing rectangle when contiguous, else add a range
        bool merged = false;
        for (auto& g : dv.ranges) {
            if (g.contains(c.r, c.c)) { merged = true; break; }
            if (c.c >= g.c0 && c.c <= g.c1 && c.r == g.r1 + 1 && g.c0 == g.c1) { g.r1 = c.r; merged = true; break; }
            if (c.r == g.r0 && c.r == g.r1 && c.c == g.c1 + 1) { g.c1 = c.c; merged = true; break; }
        }
        if (!merged) dv.ranges.push_back(Range(c.r, c.c, c.r, c.c));
    }
    // autofilter
    for (auto& d : dbr) {
        std::string sheet; Range rg;
        if (!parse_odf_range(d.target, sheet, rg)) continue;
        int si2 = b.find_sheet(sheet);
        if (si2 < 0) continue;
        b.sheets[si2].autofilter.range = rg;
        b.sheets[si2].autofilter.cols = d.cols;
    }
    // settings.xml: freeze panes, active table
    if (!settings.empty()) {
        xmlp::Pull q(settings);
        int depth_tables = 0;
        std::string cur_table;
        int hmode = 0, vmode = 0, hpos = 0, vpos = 0;
        auto commit = [&]() {
            int k = b.find_sheet(cur_table);
            if (k < 0) return;
            if (hmode == 2) b.sheets[k].freeze_cols = hpos;
            if (vmode == 2) b.sheets[k].freeze_rows = vpos;
        };
        for (;;) {
            auto e = q.next();
            if (e == xmlp::Pull::Eof) break;
            if (e == xmlp::Pull::End) {
                if (q.name() == "config:config-item-map-entry" && depth_tables == 2) { commit(); cur_table.clear(); depth_tables = 1; }
                else if (q.name() == "config:config-item-map-named" && depth_tables == 1) depth_tables = 0;
                continue;
            }
            if (e != xmlp::Pull::Start) continue;
            if (q.name() == "config:config-item-map-named" && q.attr_or("config:name") == "Tables") { depth_tables = 1; continue; }
            if (q.name() == "config:config-item-map-entry" && depth_tables == 1) { cur_table = q.attr_or("config:name"); depth_tables = 2; hmode = vmode = hpos = vpos = 0; continue; }
            if (q.name() == "config:config-item") {
                std::string n = q.attr_or("config:name");
                std::string v = q.read_text();
                if (depth_tables == 2) {
                    if (n == "HorizontalSplitMode") hmode = atoi(v.c_str());
                    else if (n == "VerticalSplitMode") vmode = atoi(v.c_str());
                    else if (n == "HorizontalSplitPosition") hpos = atoi(v.c_str());
                    else if (n == "VerticalSplitPosition") vpos = atoi(v.c_str());
                    else if (n == "ShowGrid") { int k = b.find_sheet(cur_table); if (k >= 0) b.sheets[k].show_grid = v == "true"; }
                    else if (n == "ZoomValue") { int k = b.find_sheet(cur_table); if (k >= 0) b.sheets[k].zoom = std::clamp(atoi(v.c_str()), 10, 400); }
                } else if (n == "ActiveTable") { int k = b.find_sheet(v); if (k >= 0) b.active_sheet = k; }
            }
        }
    }
    // formulas
    for (auto& [msi, g] : matrices) {
        Worksheet& ws = b.sheets[msi];
        for (int r = g.r0; r <= g.r1 && r < (int)ws.rows.size(); ++r)
            for (int c = g.c0; c <= g.c1 && c < (int)ws.rows[r].cells.size(); ++c)
                if (r != g.r0 || c != g.c0) { Cell& x = ws.rows[r].cells[c]; x.v = Value(); x.f.reset(); }
    }
    for (auto& F : pf) {
        std::string a1 = of_to_a1(F.f);
        b.set_formula_raw(F.si, F.r, F.c, a1, &F.cached);
    }
    for (auto& ws : b.sheets) for (auto& r : ws.rows) for (auto& c : r.cells) c.calc = 0;
    b.invalidate_graph();
    if (!pf.empty()) b.recalc_all();
    b.clear_undo();
    book = std::move(b);
    return true;
}

} // namespace pcbp::io
