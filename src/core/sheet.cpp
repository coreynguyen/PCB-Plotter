// sheet.cpp — legacy string-grid helpers (Sheet/Workbook) + BOM clean-up ops. Engine: sheet_model/look/format.cpp, formula*.cpp
#include "sheet.h"
#include "../io/text_import.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace pcbp::sheet {

int Sheet::ncols() const {
    size_t n = 0;
    for (auto& r : rows) n = std::max(n, r.size());
    return (int)n;
}
const std::string& Sheet::at(int r, int c) const {
    static const std::string empty;
    if (r < 0 || c < 0 || r >= (int)rows.size() || c >= (int)rows[r].size()) return empty;
    return rows[r][c];
}
void Sheet::set(int r, int c, const std::string& v) {
    if (r < 0 || c < 0) return;
    if (r >= (int)rows.size()) { if (v.empty()) return; rows.resize(r + 1); }
    Row& R = rows[r];
    if (c >= (int)R.size()) { if (v.empty()) return; R.resize(c + 1); }
    R[c] = v;
}
void Sheet::trim_trailing() {
    for (auto& r : rows) while (!r.empty() && r.back().empty()) r.pop_back();
    while (!rows.empty() && rows.back().empty()) rows.pop_back();
}

int natural_cmp(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        unsigned char ca = a[i], cb = b[j];
        if (isdigit(ca) && isdigit(cb)) {
            size_t i0 = i, j0 = j;
            while (i0 < a.size() && a[i0] == '0') ++i0;
            while (j0 < b.size() && b[j0] == '0') ++j0;
            size_t i1 = i0, j1 = j0;
            while (i1 < a.size() && isdigit((unsigned char)a[i1])) ++i1;
            while (j1 < b.size() && isdigit((unsigned char)b[j1])) ++j1;
            if (i1 - i0 != j1 - j0) return (i1 - i0) < (j1 - j0) ? -1 : 1;
            int c = a.compare(i0, i1 - i0, b, j0, j1 - j0);
            if (c) return c < 0 ? -1 : 1;
            i = i1; j = j1;
        } else {
            int x = tolower(ca), y = tolower(cb);
            if (x != y) return x < y ? -1 : 1;
            ++i; ++j;
        }
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

std::vector<Row> parse_delimited(const std::string& t, char sep, char quote) {
    std::vector<Row> out;
    Row row;
    std::string cell;
    bool inq = false, any = false, was_quoted = false;
    size_t i = 0, n = t.size();
    auto end_cell = [&]() { row.push_back(std::move(cell)); cell.clear(); was_quoted = false; };
    while (i < n) {
        char ch = t[i];
        if (inq) {
            if (ch == quote) {
                if (i + 1 < n && t[i + 1] == quote) { cell += quote; i += 2; continue; }
                inq = false; ++i; continue;
            }
            cell += ch; ++i; continue;
        }
        if (ch == quote && cell.empty() && !was_quoted) { inq = true; was_quoted = true; any = true; ++i; continue; }
        if (ch == sep) { end_cell(); any = true; ++i; continue; }
        if (ch == '\r' || ch == '\n') {
            end_cell();
            out.push_back(std::move(row)); row.clear(); any = false;
            if (ch == '\r' && i + 1 < n && t[i + 1] == '\n') ++i;
            ++i; continue;
        }
        cell += ch; any = true; ++i;
    }
    if (any || !cell.empty() || !row.empty()) { end_cell(); out.push_back(std::move(row)); }
    // a lone empty cell row is an empty row
    for (auto& r : out) if (r.size() == 1 && r[0].empty()) r.clear();
    return out;
}

static bool needs_quote(const std::string& s, char sep, char quote) {
    if (s.empty()) return false;
    if (s.front() == ' ' || s.back() == ' ') return true;
    for (char c : s) if (c == sep || c == quote || c == '\n' || c == '\r') return true;
    return false;
}

std::string to_delimited(const std::vector<Row>& rows, char sep, bool crlf, char quote) {
    std::string o;
    for (auto& r : rows) {
        for (size_t c = 0; c < r.size(); ++c) {
            if (c) o += sep;
            const std::string& s = r[c];
            if (needs_quote(s, sep, quote)) {
                o += quote;
                for (char ch : s) { if (ch == quote) o += quote; o += ch; }
                o += quote;
            } else o += s;
        }
        o += crlf ? "\r\n" : "\n";
    }
    return o;
}

std::string to_clipboard(const std::vector<Row>& rows) {
    // Excel quotes only cells with tab / newline / quote; it does not quote leading blanks
    std::string o;
    for (auto& r : rows) {
        for (size_t c = 0; c < r.size(); ++c) {
            if (c) o += '\t';
            const std::string& s = r[c];
            bool q = s.find_first_of("\t\r\n\"") != std::string::npos;
            if (q) { o += '"'; for (char ch : s) { if (ch == '"') o += '"'; o += ch; } o += '"'; }
            else o += s;
        }
        o += "\r\n";
    }
    return o;
}

std::vector<Row> from_clipboard(const std::string& text) {
    std::vector<Row> v = parse_delimited(text, '\t');
    return v;
}

Sheet sheet_from_text(const std::string& bytes, std::string* how) {
    Sheet s;
    io::ImportLayout L;
    L.skip_summary = false;
    io::ParsedText t;
    std::string err;
    io::analyze_text(bytes, L, io::Purpose::Generic, t, err);
    std::string text;
    for (size_t i = 0; i < t.lines.size(); ++i) { text += t.lines[i]; text += '\n'; }
    if (t.lines.empty()) text = io::decode_text(bytes);
    auto is_comment = [&](const std::string& ln) {
        size_t k = ln.find_first_not_of(" \t");
        if (k == std::string::npos) return false;
        for (auto& p : L.comment_prefixes) if (!p.empty() && ln.compare(k, p.size(), p) == 0) return true;
        return false;
    };
    const bool single = t.mode == io::SplitMode::Delimited && t.delims.size() == 1 && t.other_delim.empty() &&
                        !t.consecutive_as_one && t.delims[0] != ' ';
    if (t.mode == io::SplitMode::Fixed && !t.cuts.empty()) {
        for (auto& ln0 : t.lines) {
            std::string ln;
            for (char ch : ln0) { if (ch == '\t') { do ln += ' '; while (ln.size() % 8); } else ln += ch; }
            Row r;
            if (is_comment(ln)) r.push_back(ln);
            else if (ln.find_first_not_of(' ') != std::string::npos) {
                for (size_t k = 0; k < t.cuts.size(); ++k) {
                    size_t a = (size_t)t.cuts[k], b = k + 1 < t.cuts.size() ? (size_t)t.cuts[k + 1] : std::string::npos;
                    std::string c = a < ln.size() ? ln.substr(a, b == std::string::npos ? std::string::npos : b - a) : "";
                    size_t x = c.find_first_not_of(' '), y = c.find_last_not_of(' ');
                    r.push_back(x == std::string::npos ? "" : c.substr(x, y - x + 1));
                }
                if (t.cuts[0] > 0) {   // text before the first cut
                    std::string pre = ln.substr(0, std::min(ln.size(), (size_t)t.cuts[0]));
                    if (pre.find_first_not_of(' ') != std::string::npos) r[0] = pre + r[0];
                }
            }
            s.rows.push_back(std::move(r));
        }
        if (how) *how = "fixed-width, " + std::to_string(t.cuts.size()) + " columns";
    } else if (single) {
        s.rows = parse_delimited(text, t.delims[0], t.quote ? t.quote : '"');
        if (how) { const char d = t.delims[0]; *how = std::string("delimited by ") + (d == '\t' ? "TAB" : d == ',' ? "comma" : d == ';' ? "semicolon" : std::string(1, d)); }
    } else {
        std::string delims = t.delims.empty() ? std::string(",") : t.delims;
        for (auto& ln : t.lines) {
            if (is_comment(ln)) { s.rows.push_back(Row{ln}); continue; }
            if (ln.find_first_not_of(" \t") == std::string::npos) { s.rows.push_back({}); continue; }
            s.rows.push_back(io::split_multi(ln, delims, t.other_delim, t.quote ? t.quote : '"', t.consecutive_as_one));
        }
        if (how) *how = t.consecutive_as_one ? "whitespace separated" : "delimited";
    }
    for (auto& r : s.rows) while (!r.empty() && r.back().empty()) r.pop_back();
    while (!s.rows.empty() && s.rows.back().empty()) s.rows.pop_back();
    return s;
}

bool is_plain_number(const std::string& s) {
    size_t i = 0, n = s.size();
    if (!n || n > 17) return false;
    if (s[i] == '-') ++i;
    if (i >= n || !isdigit((unsigned char)s[i])) return false;
    if (s[i] == '0' && i + 1 < n && isdigit((unsigned char)s[i + 1])) return false;   // "0805"
    while (i < n && isdigit((unsigned char)s[i])) ++i;
    if (i < n && s[i] == '.') {
        ++i;
        if (i >= n || !isdigit((unsigned char)s[i])) return false;
        while (i < n && isdigit((unsigned char)s[i])) ++i;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < n && (s[i] == '+' || s[i] == '-')) ++i;
        if (i >= n || !isdigit((unsigned char)s[i])) return false;
        while (i < n && isdigit((unsigned char)s[i])) ++i;
    }
    return i == n;
}

std::string expand_refs_cell(const std::string& cell, const std::string& sep) {
    auto v = io::expand_refs(cell, false);
    std::string o;
    for (auto& r : v) { if (!o.empty()) o += sep; o += r; }
    return o;
}

static bool split_ref(const std::string& r, std::string& pre, long& num) {
    size_t k = r.size();
    while (k > 0 && isdigit((unsigned char)r[k - 1])) --k;
    if (k == r.size() || k == 0 || r.size() - k > 9) return false;
    if (r[k] == '0' && k + 1 < r.size()) return false;   // leading zero: keep literal
    pre = r.substr(0, k);
    num = std::stol(r.substr(k));
    return true;
}

std::string compress_refs(const std::vector<std::string>& refs0, const std::string& sep) {
    std::vector<std::string> refs = refs0;
    std::stable_sort(refs.begin(), refs.end(), [](const std::string& a, const std::string& b) { return natural_cmp(a, b) < 0; });
    refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
    std::string o;
    auto add = [&](const std::string& s) { if (!o.empty()) o += sep; o += s; };
    size_t i = 0;
    while (i < refs.size()) {
        std::string p; long n;
        if (!split_ref(refs[i], p, n)) { add(refs[i]); ++i; continue; }
        size_t j = i + 1;
        long last = n;
        while (j < refs.size()) {
            std::string p2; long n2;
            if (!split_ref(refs[j], p2, n2) || p2 != p || n2 != last + 1) break;
            last = n2; ++j;
        }
        if (j - i >= 3) add(refs[i] + "-" + refs[j - 1]);
        else for (size_t k = i; k < j; ++k) add(refs[k]);
        i = j;
    }
    return o;
}

std::string compress_refs_cell(const std::string& cell, const std::string& sep) {
    return compress_refs(io::expand_refs(cell, false), sep);
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n\xA0"), b = s.find_last_not_of(" \t\r\n\xA0");
    if (a == std::string::npos) return {};
    std::string o = s.substr(a, b - a + 1);
    // collapse internal runs of blanks
    std::string r;
    bool sp = false;
    for (char c : o) { if (c == ' ' || c == '\t') { if (!sp) r += ' '; sp = true; } else { r += c; sp = false; } }
    return r;
}

int trim_cells(Sheet& s, int c0, int c1, int r0, int r1) {
    int n = 0;
    if (r1 < 0) r1 = s.nrows() - 1;
    for (int r = std::max(0, r0); r <= r1 && r < s.nrows(); ++r)
        for (int c = std::max(0, c0); c <= c1 && c < (int)s.rows[r].size(); ++c) {
            std::string t = trim(s.rows[r][c]);
            if (t != s.rows[r][c]) { s.rows[r][c] = t; ++n; }
        }
    return n;
}

int remove_empty_rows(Sheet& s) {
    size_t before = s.rows.size();
    s.rows.erase(std::remove_if(s.rows.begin(), s.rows.end(), [](const Row& r) {
        for (auto& c : r) if (c.find_first_not_of(" \t") != std::string::npos) return false;
        return true; }), s.rows.end());
    return (int)(before - s.rows.size());
}

int dedupe_rows(Sheet& s, int first_row) {
    std::set<Row> seen;
    std::vector<Row> out;
    int n = 0;
    for (int r = 0; r < s.nrows(); ++r) {
        if (r < first_row) { out.push_back(s.rows[r]); continue; }
        Row k = s.rows[r];
        while (!k.empty() && k.back().empty()) k.pop_back();
        if (!k.empty() && !seen.insert(k).second) { ++n; continue; }
        out.push_back(std::move(s.rows[r]));
    }
    s.rows = std::move(out);
    return n;
}

static std::vector<std::string> split_str(const std::string& s, const std::string& d) {
    std::vector<std::string> v;
    if (d.empty()) { v.push_back(s); return v; }
    size_t p = 0;
    for (;;) {
        size_t k = s.find(d, p);
        if (k == std::string::npos) { v.push_back(s.substr(p)); break; }
        v.push_back(s.substr(p, k - p));
        p = k + d.size();
    }
    return v;
}

int split_column(Sheet& s, int col, const std::string& delim, int first_row) {
    size_t maxp = 1;
    for (int r = first_row; r < s.nrows(); ++r) maxp = std::max(maxp, split_str(s.at(r, col), delim).size());
    if (maxp <= 1) return 0;
    insert_cols(s, col + 1, (int)maxp - 1);
    int n = 0;
    for (int r = first_row; r < s.nrows(); ++r) {
        auto parts = split_str(s.at(r, col), delim);
        if (parts.size() <= 1) continue;
        for (size_t k = 0; k < parts.size(); ++k) s.set(r, col + (int)k, trim(parts[k]));
        ++n;
    }
    return n;
}

int merge_columns(Sheet& s, int c0, int c1, const std::string& sep, int first_row) {
    if (c1 <= c0) return 0;
    int n = 0;
    for (int r = first_row; r < s.nrows(); ++r) {
        std::string m;
        for (int c = c0; c <= c1; ++c) {
            const std::string& v = s.at(r, c);
            if (v.empty()) continue;
            if (!m.empty()) m += sep;
            m += v;
        }
        if (m != s.at(r, c0)) ++n;
        s.set(r, c0, m);
        if (m.empty() && c0 < (int)s.rows[r].size()) s.rows[r][c0].clear();
    }
    delete_cols(s, c0 + 1, c1 - c0);
    return n;
}

void sort_rows(Sheet& s, int col, bool desc, int first_row, bool natural) {
    if (first_row >= s.nrows()) return;
    std::stable_sort(s.rows.begin() + first_row, s.rows.end(), [&](const Row& a, const Row& b) {
        const std::string& x = col < (int)a.size() ? a[col] : std::string();
        const std::string& y = col < (int)b.size() ? b[col] : std::string();
        if (x.empty() != y.empty()) return y.empty();            // empties last both ways
        int c = natural ? natural_cmp(x, y) : x.compare(y);
        return desc ? c > 0 : c < 0;
    });
}

void insert_rows(Sheet& s, int at, int n) {
    if (n <= 0) return;
    at = std::clamp(at, 0, s.nrows());
    s.rows.insert(s.rows.begin() + at, (size_t)n, Row{});
}
void delete_rows(Sheet& s, int at, int n) {
    if (n <= 0 || at >= s.nrows()) return;
    int e = std::min(s.nrows(), at + n);
    s.rows.erase(s.rows.begin() + at, s.rows.begin() + e);
}
void insert_cols(Sheet& s, int at, int n) {
    if (n <= 0) return;
    for (auto& r : s.rows) if (at < (int)r.size()) r.insert(r.begin() + at, (size_t)n, std::string());
}
void delete_cols(Sheet& s, int at, int n) {
    if (n <= 0) return;
    for (auto& r : s.rows) if (at < (int)r.size()) r.erase(r.begin() + at, r.begin() + std::min((int)r.size(), at + n));
}

} // namespace pcbp::sheet
