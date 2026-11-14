// ============================================================================
// html_rich.cpp — minimal, safe HTML -> RichDoc converter (see richdoc.h) and
// the RichDoc helpers. Not a browser: a tolerant tag scanner that maps the
// formatting subset email bodies use. Scripts, styles, forms, frames, objects
// and comments are dropped; no URL is ever fetched; links are data only.
// ============================================================================
#include "richdoc.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace pcbp::io {

std::string RichPara::text() const {
    std::string s;
    for (auto& r : runs) s += r.text;
    return s;
}

static void plain_para(const RichDoc& d, const RichPara& p, std::string& o) {
    if (p.table >= 0 && p.table < (int)d.tables.size()) {
        for (auto& row : d.tables[p.table].rows) {
            for (size_t c = 0; c < row.size(); ++c) {
                if (c) o += '\t';
                for (size_t k = 0; k < row[c].paras.size(); ++k) { if (k) o += ' '; o += row[c].paras[k].text(); }
            }
            o += '\n';
        }
        return;
    }
    if (p.rule) { o += "----------------------------------------\n"; return; }
    o += std::string(p.level * 2, ' ');
    if (p.list == 1) o += "\xE2\x80\xA2 ";
    else if (p.list == 2) o += std::to_string(p.num) + ". ";
    o += p.text();
    o += '\n';
}

std::string RichDoc::plain() const {
    std::string o;
    for (auto& p : paras) plain_para(*this, p, o);
    return o;
}

RichDoc rich_from_plain(const std::string& s) {
    RichDoc d;
    size_t i = 0;
    while (i <= s.size()) {
        size_t e = s.find('\n', i);
        if (e == std::string::npos) e = s.size();
        std::string line = s.substr(i, e - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        RichPara p;
        if (!line.empty()) { RichRun r; r.text = line; p.runs.push_back(r); }
        d.paras.push_back(std::move(p));
        if (e >= s.size()) break;
        i = e + 1;
    }
    while (!d.paras.empty() && d.paras.back().runs.empty()) d.paras.pop_back();
    return d;
}

namespace {

void put_utf8(std::string& o, uint32_t cp) {
    if (cp == 0 || (cp >= 0xD800 && cp < 0xE000) || cp > 0x10FFFF) cp = 0xFFFD;
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | cp >> 6); o += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += (char)(0xE0 | cp >> 12); o += (char)(0x80 | (cp >> 6 & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    else { o += (char)(0xF0 | cp >> 18); o += (char)(0x80 | (cp >> 12 & 0x3F)); o += (char)(0x80 | (cp >> 6 & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
}

struct Ent { const char* n; uint32_t c; };
const Ent kEnt[] = {{"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0}, {"copy", 0xA9}, {"reg", 0xAE},
                    {"trade", 0x2122}, {"hellip", 0x2026}, {"mdash", 0x2014}, {"ndash", 0x2013}, {"lsquo", 0x2018}, {"rsquo", 0x2019},
                    {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"bull", 0x2022}, {"middot", 0xB7}, {"deg", 0xB0}, {"plusmn", 0xB1},
                    {"micro", 0xB5}, {"times", 0xD7}, {"divide", 0xF7}, {"euro", 0x20AC}, {"pound", 0xA3}, {"yen", 0xA5}, {"cent", 0xA2},
                    {"sect", 0xA7}, {"para", 0xB6}, {"laquo", 0xAB}, {"raquo", 0xBB}, {"ohm", 0x3A9}, {"Omega", 0x3A9}, {"mu", 0x3BC},
                    {"le", 0x2264}, {"ge", 0x2265}, {"ne", 0x2260}, {"asymp", 0x2248}, {"rarr", 0x2192}, {"larr", 0x2190}, {"uarr", 0x2191},
                    {"darr", 0x2193}, {"shy", 0xAD}, {"zwnj", 0x200C}, {"zwj", 0x200D}, {"ensp", 0x2002}, {"emsp", 0x2003}, {"thinsp", 0x2009},
                    {"iexcl", 0xA1}, {"iquest", 0xBF}, {"frac12", 0xBD}, {"frac14", 0xBC}, {"frac34", 0xBE}, {"sup2", 0xB2}, {"sup3", 0xB3},
                    {"auml", 0xE4}, {"ouml", 0xF6}, {"uuml", 0xFC}, {"Auml", 0xC4}, {"Ouml", 0xD6}, {"Uuml", 0xDC}, {"szlig", 0xDF},
                    {"eacute", 0xE9}, {"egrave", 0xE8}, {"ecirc", 0xEA}, {"aacute", 0xE1}, {"agrave", 0xE0}, {"acirc", 0xE2}, {"ccedil", 0xE7},
                    {"Eacute", 0xC9}, {"iacute", 0xED}, {"oacute", 0xF3}, {"uacute", 0xFA}, {"ntilde", 0xF1}, {"Ntilde", 0xD1}, {"aring", 0xE5},
                    {"Aring", 0xC5}, {"oslash", 0xF8}, {"Oslash", 0xD8}, {"aelig", 0xE6}, {"AElig", 0xC6}, {"ocirc", 0xF4}, {"ucirc", 0xFB},
                    {"icirc", 0xEE}, {"euml", 0xEB}, {"iuml", 0xEF}, {"yuml", 0xFF}, {"otilde", 0xF5}, {"atilde", 0xE3}};

uint32_t named_color(const std::string& n) {
    static const std::pair<const char*, uint32_t> C[] = {{"black", 0x000000}, {"white", 0xFFFFFF}, {"red", 0xFF0000}, {"green", 0x008000},
        {"blue", 0x0000FF}, {"navy", 0x000080}, {"gray", 0x808080}, {"grey", 0x808080}, {"silver", 0xC0C0C0}, {"maroon", 0x800000},
        {"purple", 0x800080}, {"fuchsia", 0xFF00FF}, {"magenta", 0xFF00FF}, {"lime", 0x00FF00}, {"olive", 0x808000}, {"yellow", 0xFFFF00},
        {"teal", 0x008080}, {"aqua", 0x00FFFF}, {"cyan", 0x00FFFF}, {"orange", 0xFFA500}, {"darkred", 0x8B0000}, {"darkblue", 0x00008B},
        {"darkgreen", 0x006400}, {"brown", 0xA52A2A}, {"darkgray", 0xA9A9A9}, {"darkgrey", 0xA9A9A9}, {"lightgray", 0xD3D3D3}};
    for (auto& c : C) if (n == c.first) return 0xFF000000u | c.second;
    return 0;
}

uint32_t parse_color(std::string v) {
    for (auto& c : v) c = (char)std::tolower((unsigned char)c);
    while (!v.empty() && v.front() == ' ') v.erase(0, 1);
    while (!v.empty() && (v.back() == ' ' || v.back() == ';')) v.pop_back();
    if (v.size() > 10 && v.find("!important") != std::string::npos) v = v.substr(0, v.find("!important"));
    while (!v.empty() && v.back() == ' ') v.pop_back();
    if (!v.empty() && v[0] == '#') {
        std::string h = v.substr(1);
        if (h.size() == 3) h = std::string{h[0], h[0], h[1], h[1], h[2], h[2]};
        if (h.size() >= 6) { char* e; unsigned long x = std::strtoul(h.substr(0, 6).c_str(), &e, 16); if (*e == 0) return 0xFF000000u | (uint32_t)x; }
        return 0;
    }
    if (v.rfind("rgb", 0) == 0) {
        size_t a = v.find('(');
        if (a == std::string::npos) return 0;
        int r = 0, g = 0, b = 0;
        if (std::sscanf(v.c_str() + a + 1, "%d%*[ ,]%d%*[ ,]%d", &r, &g, &b) == 3)
            return 0xFF000000u | (uint32_t)std::clamp(r, 0, 255) << 16 | (uint32_t)std::clamp(g, 0, 255) << 8 | (uint32_t)std::clamp(b, 0, 255);
        return 0;
    }
    return named_color(v);
}

std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

struct Style {
    bool b = false, i = false, u = false, s = false, mono = false, pre = false;
    float size = 0;
    uint32_t color = 0;
    std::string link;
    bool hidden = false;
};

struct Tag {
    std::string name;       // lower case, no '/'
    bool close = false, self = false;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::string attr(const char* k) const { for (auto& a : attrs) if (a.first == k) return a.second; return {}; }
};

// css "a: b; c: d" -> apply to style
void apply_css(const std::string& css, Style& st, int* align) {
    size_t i = 0;
    while (i < css.size()) {
        size_t e = css.find(';', i);
        if (e == std::string::npos) e = css.size();
        std::string decl = css.substr(i, e - i);
        size_t c = decl.find(':');
        if (c != std::string::npos) {
            std::string k = lower(decl.substr(0, c)), v = lower(decl.substr(c + 1));
            auto tr = [](std::string& s) { while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(0, 1); while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back(); };
            tr(k); tr(v);
            if (k == "font-weight") st.b = v == "bold" || v == "bolder" || std::atoi(v.c_str()) >= 600;
            else if (k == "font-style") st.i = v == "italic" || v == "oblique";
            else if (k == "text-decoration" || k == "text-decoration-line") { if (v.find("underline") != std::string::npos) st.u = true; if (v.find("line-through") != std::string::npos) st.s = true; if (v == "none") st.u = st.s = false; }
            else if (k == "color") { uint32_t col = parse_color(v); if (col) st.color = col; }
            else if (k == "font-size") {
                float f = (float)std::atof(v.c_str());
                if (v.find("pt") != std::string::npos) st.size = f;
                else if (v.find("px") != std::string::npos) st.size = f * 0.75f;
                else if (v.find("em") != std::string::npos) st.size = f * (st.size > 0 ? st.size : 11.f);
                else if (v == "small") st.size = 10; else if (v == "large") st.size = 14; else if (v == "x-large") st.size = 18; else if (v == "x-small") st.size = 8;
                if (st.size > 72) st.size = 72;
                if (st.size < 0) st.size = 0;
            } else if (k == "font-family") { if (v.find("mono") != std::string::npos || v.find("courier") != std::string::npos || v.find("consol") != std::string::npos) st.mono = true; }
            else if (k == "display" && v == "none") st.hidden = true;
            else if ((k == "visibility" && v == "hidden") || (k == "mso-hide" && v == "all")) st.hidden = true;
            else if (k == "text-align" && align) *align = v == "center" ? 1 : v == "right" ? 2 : 0;
        }
        i = e + 1;
    }
}

class Conv {
public:
    RichDoc& d;
    const std::function<int(const std::string&)>& cid_image;
    Conv(RichDoc& doc, const std::function<int(const std::string&)>& f) : d(doc), cid_image(f) {}

    std::vector<std::pair<std::string, Style>> stack;   // open inline / block elements
    Style st;
    RichPara para;
    bool para_open = false;
    bool last_space = true;
    struct ListCtx { bool ordered; int n; };
    std::vector<ListCtx> lists;
    int quote = 0;
    // tables
    struct TCtx { int index; std::vector<RichCell>* row = nullptr; bool in_cell = false; RichDoc saved; };
    struct TableBuild { RichTable t; std::vector<RichCell> row; bool in_cell = false; RichCell cell; std::vector<RichPara> outer_paras; RichPara outer_para; bool outer_open; };
    std::vector<TableBuild> tables;

    std::vector<RichPara>& target() { return tables.empty() ? d.paras : tables.back().cell.paras; }

    void flush_para() {
        if (!para_open) return;
        // trim trailing spaces
        while (!para.runs.empty()) {
            std::string& t = para.runs.back().text;
            while (!t.empty() && t.back() == ' ') t.pop_back();
            if (t.empty() && para.runs.back().image < 0) para.runs.pop_back(); else break;
        }
        if (!para.runs.empty() || para.rule) target().push_back(std::move(para));
        para = RichPara{};
        para_open = false;
        last_space = true;
    }
    void open_para() {
        if (para_open) return;
        para = RichPara{};
        para.level = (int)lists.size() + quote - (lists.empty() ? 0 : 1);
        if (para.level < 0) para.level = 0;
        para.quote = quote > 0;
        para_open = true;
        last_space = true;
    }
    void text(const std::string& u) {
        if (st.hidden || u.empty()) return;
        std::string t;
        if (st.pre) t = u;
        else {
            for (unsigned char c : u) {
                bool sp = c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
                if (sp) { if (!last_space) { t += ' '; last_space = true; } }
                else { t += (char)c; last_space = false; }
            }
            // nbsp (U+00A0) is preserved as a non-collapsing char: keep as-is
        }
        if (t.empty()) return;
        if (!para_open) { if (!st.pre && t == " ") return; open_para(); if (!st.pre && t[0] == ' ') t.erase(0, 1); if (t.empty()) return; }
        if (st.pre) {   // split lines into paragraphs
            size_t i = 0;
            while (i < t.size()) {
                size_t e = t.find('\n', i);
                std::string seg = t.substr(i, e == std::string::npos ? std::string::npos : e - i);
                if (!seg.empty() && seg.back() == '\r') seg.pop_back();
                add_run(seg);
                if (e == std::string::npos) break;
                bool keep = true; (void)keep;
                para_open = true;
                RichPara np; np.level = para.level; np.quote = para.quote;
                target().push_back(std::move(para));
                para = np;
                i = e + 1;
            }
            return;
        }
        add_run(t);
    }
    void add_run(const std::string& t) {
        if (t.empty()) return;
        RichRun r;
        r.b = st.b; r.i = st.i; r.u = st.u || !st.link.empty(); r.s = st.s; r.mono = st.mono || st.pre; r.size = st.size; r.color = st.color; r.link = st.link;
        if (!para.runs.empty() && para.runs.back().same_style(r)) para.runs.back().text += t;
        else { r.text = t; para.runs.push_back(std::move(r)); }
    }
    void line_break() {
        if (!para_open) open_para();
        // a <br> ends the paragraph visually; keep list/quote level for continuation
        RichPara np; np.level = para.level; np.quote = para.quote;
        target().push_back(std::move(para));
        para = np;
        para_open = true;
        last_space = true;
    }
    void image(const Tag& t) {
        if (st.hidden) return;
        std::string src = t.attr("src"), alt = t.attr("alt");
        int w = std::atoi(t.attr("width").c_str()), h = std::atoi(t.attr("height").c_str());
        if (w == 1 && h == 1) return;   // tracking pixel
        int idx = -1;
        if (lower(src).rfind("cid:", 0) == 0 && cid_image) idx = cid_image(src.substr(4));
        if (!para_open) open_para();
        RichRun r;
        r.link = st.link;
        if (idx >= 0) { r.image = idx; r.text = alt.empty() ? "[image]" : alt; }
        else {
            r.text = "[image" + std::string(alt.empty() ? "" : ": " + alt) + (lower(src).rfind("http", 0) == 0 ? " \xE2\x80\x94 remote, not loaded" : "") + "]";
            r.i = true;
            r.color = 0xFF808080;
        }
        para.runs.push_back(std::move(r));
        last_space = false;
    }

    static bool is_block(const std::string& n) {
        static const char* B[] = {"p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "li", "blockquote", "pre", "table", "tr", "td", "th",
                                  "hr", "section", "article", "header", "footer", "center", "dl", "dt", "dd", "address", "form", "tbody", "thead", "tfoot", "caption", "nav", "main"};
        for (const char* b : B) if (n == b) return true;
        return false;
    }

    void push(const Tag& t) {
        stack.emplace_back(t.name, st);
        int align = -1;
        const std::string& n = t.name;
        if (n == "b" || n == "strong") st.b = true;
        else if (n == "i" || n == "em" || n == "cite" || n == "var") st.i = true;
        else if (n == "u" || n == "ins") st.u = true;
        else if (n == "s" || n == "strike" || n == "del") st.s = true;
        else if (n == "code" || n == "tt" || n == "kbd" || n == "samp") st.mono = true;
        else if (n == "pre") { st.pre = true; st.mono = true; }
        else if (n == "a") { std::string h = t.attr("href"); if (!h.empty() && lower(h).rfind("javascript:", 0) != 0) st.link = h; }
        else if (n == "font") {
            uint32_t c = parse_color(t.attr("color")); if (c) st.color = c;
            std::string sz = t.attr("size");
            if (!sz.empty()) { static const float S[] = {0, 7.5f, 10, 12, 13.5f, 18, 24, 36}; int k = std::atoi(sz.c_str()); if (sz[0] == '+') k = 3 + k; else if (sz[0] == '-') k = 3 + k; k = std::clamp(k, 1, 7); st.size = S[k]; }
            std::string face = lower(t.attr("face")); if (face.find("courier") != std::string::npos || face.find("mono") != std::string::npos || face.find("consol") != std::string::npos) st.mono = true;
        } else if (n.size() == 2 && n[0] == 'h' && n[1] >= '1' && n[1] <= '6') {
            st.b = true;
            static const float HS[] = {0, 20, 16, 14, 12, 11, 10};
            st.size = HS[n[1] - '0'];
        } else if (n == "small") st.size = 9;
        else if (n == "big") st.size = 14;
        else if (n == "th") st.b = true;
        std::string css = t.attr("style");
        if (!css.empty()) apply_css(css, st, &align);
        std::string al = lower(t.attr("align"));
        if (n == "center") align = 1;
        if (!al.empty()) align = al == "center" ? 1 : al == "right" ? 2 : 0;
        if (is_block(n) && align >= 0 && n != "table" && n != "tr") { pending_align = align; }
    }
    int pending_align = -1;
    void pop_to(const std::string& n) {
        for (size_t k = stack.size(); k-- > 0;) {
            if (stack[k].first == n) {
                st = stack[k].second;
                stack.resize(k);
                return;
            }
        }
    }
    bool open(const std::string& n) const {
        for (auto& s : stack) if (s.first == n) return true;
        return false;
    }

    void start_block(const Tag& t) {
        flush_para();
        const std::string& n = t.name;
        if (n == "ul" || n == "ol") { lists.push_back({n == "ol", std::max(0, std::atoi(t.attr("start").c_str()) - 1)}); push(t); return; }
        if (n == "blockquote") { ++quote; push(t); return; }
        if (n == "hr") { RichPara p; p.rule = true; target().push_back(p); return; }
        if (n == "table") { TableBuild tb; tables.push_back(std::move(tb)); push(t); return; }
        if (n == "tr") { if (!tables.empty()) { close_cell(); if (!tables.back().row.empty()) { tables.back().t.rows.push_back(std::move(tables.back().row)); tables.back().row.clear(); } } push(t); return; }
        if (n == "td" || n == "th") {
            if (!tables.empty()) { close_cell(); tables.back().in_cell = true; tables.back().cell = RichCell{}; tables.back().cell.header = n == "th"; }
            push(t);
            return;
        }
        push(t);
        if (n == "li") {
            open_para();
            if (!lists.empty()) { auto& L = lists.back(); para.list = L.ordered ? 2 : 1; if (L.ordered) para.num = ++L.n; para.level = (int)lists.size() - 1 + quote; }
            else para.list = 1;
        } else if (n.size() == 2 && n[0] == 'h' && n[1] >= '1' && n[1] <= '6') { open_para(); para.heading = n[1] - '0'; }
        if (pending_align >= 0) { open_para(); para.align = pending_align; pending_align = -1; }
    }
    void close_cell() {
        if (tables.empty() || !tables.back().in_cell) return;
        flush_para();
        tables.back().row.push_back(std::move(tables.back().cell));
        tables.back().cell = RichCell{};
        tables.back().in_cell = false;
    }
    void end_block(const std::string& n) {
        flush_para();
        if (n == "ul" || n == "ol") { if (!lists.empty()) lists.pop_back(); pop_to(n); return; }
        if (n == "blockquote") { if (quote) --quote; pop_to(n); return; }
        if (n == "td" || n == "th") { close_cell(); pop_to(n); return; }
        if (n == "tr") { close_cell(); if (!tables.empty() && !tables.back().row.empty()) { tables.back().t.rows.push_back(std::move(tables.back().row)); tables.back().row.clear(); } pop_to(n); return; }
        if (n == "table") {
            if (tables.empty()) { pop_to(n); return; }
            close_cell();
            TableBuild tb = std::move(tables.back());
            tables.pop_back();
            if (!tb.row.empty()) tb.t.rows.push_back(std::move(tb.row));
            pop_to(n);
            if (tb.t.rows.empty()) return;
            // layout tables with one cell per row are unwrapped (common in HTML mail)
            bool single = true;
            for (auto& r : tb.t.rows) if (r.size() > 1) { single = false; break; }
            if (single) { for (auto& r : tb.t.rows) for (auto& c : r) for (auto& p : c.paras) target().push_back(std::move(p)); return; }
            RichPara blk;
            blk.table = (int)d.tables.size();
            d.tables.push_back(std::move(tb.t));
            target().push_back(std::move(blk));
            return;
        }
        pop_to(n);
    }

    void run(const std::string& h) {
        size_t i = 0, n = h.size();
        while (i < n) {
            if (h[i] == '<') {
                if (h.compare(i, 4, "<!--") == 0) { size_t e = h.find("-->", i + 4); i = e == std::string::npos ? n : e + 3; continue; }
                if (i + 1 < n && (h[i + 1] == '!' || h[i + 1] == '?')) { size_t e = h.find('>', i); i = e == std::string::npos ? n : e + 1; continue; }
                Tag t;
                size_t j = i + 1;
                if (j < n && h[j] == '/') { t.close = true; ++j; }
                size_t a = j;
                while (j < n && (std::isalnum((unsigned char)h[j]) || h[j] == ':' || h[j] == '-')) ++j;
                if (j == a) { text("<"); ++i; continue; }
                t.name = lower(h.substr(a, j - a));
                size_t c = t.name.find(':');
                if (c != std::string::npos) t.name = t.name.substr(c + 1);   // o:p, v:shape
                // attributes
                while (j < n && h[j] != '>') {
                    while (j < n && (std::isspace((unsigned char)h[j]) || h[j] == '/')) { if (h[j] == '/' && j + 1 < n && h[j + 1] == '>') t.self = true; ++j; }
                    if (j >= n || h[j] == '>') break;
                    size_t ka = j;
                    while (j < n && !std::isspace((unsigned char)h[j]) && h[j] != '=' && h[j] != '>' && h[j] != '/') ++j;
                    std::string k = lower(h.substr(ka, j - ka));
                    std::string v;
                    while (j < n && std::isspace((unsigned char)h[j])) ++j;
                    if (j < n && h[j] == '=') {
                        ++j;
                        while (j < n && std::isspace((unsigned char)h[j])) ++j;
                        if (j < n && (h[j] == '"' || h[j] == '\'')) { char q = h[j++]; size_t va = j; while (j < n && h[j] != q) ++j; v = h.substr(va, j - va); if (j < n) ++j; }
                        else { size_t va = j; while (j < n && !std::isspace((unsigned char)h[j]) && h[j] != '>') ++j; v = h.substr(va, j - va); }
                    }
                    if (k.empty()) { if (j < n && h[j] != '>') ++j; continue; }
                    t.attrs.emplace_back(k, html_unescape(v));
                }
                i = j < n ? j + 1 : n;
                handle(t, h, i);
                continue;
            }
            size_t e = h.find('<', i);
            if (e == std::string::npos) e = n;
            text(html_unescape(h.substr(i, e - i)));
            i = e;
        }
        flush_para();
        while (!tables.empty()) end_block("table");
    }

    void handle(const Tag& t, const std::string& h, size_t& i) {
        const std::string& n = t.name;
        // raw-text / dangerous elements: skip content entirely
        static const char* SKIP[] = {"script", "style", "head", "title", "noscript", "template", "iframe", "object", "embed", "applet", "svg", "math", "xml", "select", "textarea", "frameset", "noframes"};
        for (const char* s : SKIP) if (n == s) {
            if (t.close || t.self) return;
            if (n == "title") {
                size_t e = lower(h.substr(i, std::min<size_t>(h.size() - i, 2000))).find("</title");
                if (e != std::string::npos) d.title = html_unescape(h.substr(i, e));
            }
            std::string endtag = "</" + n;
            std::string lo;
            // case-insensitive search for the end tag
            size_t k = i;
            for (;;) {
                size_t p = h.find("</", k);
                if (p == std::string::npos) { i = h.size(); return; }
                if (lower(h.substr(p, endtag.size())) == endtag) { size_t g = h.find('>', p); i = g == std::string::npos ? h.size() : g + 1; return; }
                k = p + 2;
            }
        }
        if (n == "input" || n == "button" || n == "form" || n == "link" || n == "meta" || n == "base" || n == "area" || n == "map" || n == "wbr") return;
        if (n == "br") { if (!t.close) line_break(); return; }
        if (n == "img") { if (!t.close) image(t); return; }
        if (n == "html" || n == "body") {
            if (!t.close) { std::string css = t.attr("style"); (void)css; }
            return;
        }
        if (is_block(n)) {
            if (t.close) end_block(n);
            else { start_block(t); if (t.self || n == "hr") { if (n != "hr") end_block(n); } }
            return;
        }
        if (t.close) { pop_to(n); return; }
        if (t.self) return;
        push(t);
    }
};

} // namespace

std::string html_unescape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { o += s[i]; continue; }
        size_t e = s.find(';', i);
        if (e == std::string::npos || e - i > 12) {
            // tolerate "&nbsp" without ';'
            if (s.compare(i, 5, "&nbsp") == 0) { o += "\xC2\xA0"; i += 4; continue; }
            if (s.compare(i, 4, "&amp") == 0) { o += '&'; i += 3; continue; }
            o += '&';
            continue;
        }
        std::string ent = s.substr(i + 1, e - i - 1);
        if (!ent.empty() && ent[0] == '#') {
            uint32_t cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? (uint32_t)std::strtoul(ent.c_str() + 2, nullptr, 16) : (uint32_t)std::strtoul(ent.c_str() + 1, nullptr, 10);
            // HTML5: C1 range maps to 1252
            static const uint16_t k1252[32] = {0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F,
                                               0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};
            if (cp >= 0x80 && cp < 0xA0) cp = k1252[cp - 0x80];
            put_utf8(o, cp);
            i = e;
            continue;
        }
        bool found = false;
        for (const Ent& en : kEnt) if (ent == en.n) { put_utf8(o, en.c); found = true; break; }
        if (!found) { o += '&'; continue; }
        i = e;
    }
    return o;
}

std::string html_meta_charset(const std::string& html) {
    std::string lo = lower(html.substr(0, std::min<size_t>(html.size(), 4096)));
    size_t k = lo.find("charset=");
    if (k == std::string::npos) return {};
    k += 8;
    while (k < lo.size() && (lo[k] == '"' || lo[k] == '\'' || lo[k] == ' ')) ++k;
    size_t e = k;
    while (e < lo.size() && (std::isalnum((unsigned char)lo[e]) || lo[e] == '-' || lo[e] == '_')) ++e;
    return lo.substr(k, e - k);
}

void html_to_rich(const std::string& html, RichDoc& out, const std::function<int(const std::string&)>& cid_image) {
    Conv c(out, cid_image);
    c.run(html);
    while (!out.paras.empty() && out.paras.back().table < 0 && out.paras.back().runs.empty() && !out.paras.back().rule) out.paras.pop_back();
}

} // namespace pcbp::io
