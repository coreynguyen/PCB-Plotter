// ============================================================================
// text_table.cpp — generic tabular text reader. See text_table.h.
// ============================================================================
#include "text_table.h"
#include "io_util.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <cmath>
#include <cstdlib>

namespace pcbp::io {

const char* delim_name(Delim d) {
    switch (d) {
    case Delim::Auto: return "auto";
    case Delim::Comma: return "comma";
    case Delim::Tab: return "tab";
    case Delim::Semicolon: return "semicolon";
    case Delim::Pipe: return "pipe";
    case Delim::Whitespace: return "whitespace";
    case Delim::Fixed: return "fixed-width";
    }
    return "?";
}

static char delim_char(Delim d) {
    switch (d) {
    case Delim::Comma: return ',';
    case Delim::Tab: return '\t';
    case Delim::Semicolon: return ';';
    case Delim::Pipe: return '|';
    default: return 0;
    }
}

std::vector<std::string> split_delimited(const std::string& line, char delim, char quote) {
    std::vector<std::string> out;
    std::string cur;
    bool inq = false, was_quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (inq) {
            if (c == quote) {
                if (i + 1 < line.size() && line[i + 1] == quote) { cur += quote; ++i; }
                else inq = false;
            } else cur += c;
        } else if (c == quote && quote && trim(cur).empty()) {
            inq = true; was_quoted = true; cur.clear();
        } else if (c == delim) {
            out.push_back(was_quoted ? cur : trim(cur));
            cur.clear(); was_quoted = false;
        } else cur += c;
    }
    out.push_back(was_quoted ? cur : trim(cur));
    return out;
}

int TextTable::col(const std::string& name) const {
    const std::string k = norm_key(name);
    for (size_t i = 0; i < columns.size(); ++i)
        if (norm_key(columns[i]) == k) return (int)i;
    return -1;
}

std::string TextTable::col_name(int i) const {
    if (i < 0 || i >= (int)columns.size()) return "?";
    return columns[i].empty() ? "Col" + std::to_string(i + 1) : columns[i];
}

std::string TextTable::describe() const {
    std::ostringstream s;
    s << "delimiter=" << delim_name(delim);
    if (delim == Delim::Fixed) {
        s << " cuts=[";
        for (size_t i = 0; i < cuts.size(); ++i) s << (i ? "," : "") << cuts[i];
        s << "]";
    }
    s << "; " << columns.size() << " columns; " << rows.size() << " rows";
    if (has_header)
        s << "; header " << (header_from_comment ? "from comment line " : "at line ") << header_line;
    else
        s << "; no header";
    s << ": ";
    for (size_t i = 0; i < columns.size(); ++i) s << (i ? " | " : "") << col_name((int)i);
    if (skipped_comment) s << "; " << skipped_comment << " comment lines";
    if (!preamble.empty()) s << "; " << preamble.size() << " preamble lines skipped";
    if (ragged_rows) s << "; " << ragged_rows << " ragged rows";
    return s.str();
}

// ---------------------------------------------------------------------------
namespace {

struct Line { std::string text; int lineno; };

const char* kHeaderWords[] = {
    "ref", "refdes", "reference", "designator", "designators", "references", "x", "y",
    "rot", "rotation", "angle", "side", "layer", "part", "package", "footprint", "value",
    "val", "description", "desc", "qty", "quantity", "number", "type", "process",
    "polarity", "comment", "posx", "posy", "midx", "midy", "centerx", "centery",
    "partnumber", "pn", "mount", "pattern", "shape", "refx", "refy", "padx", "pady",
    "locationx", "locationy", "xloc", "yloc", "mfr", "manufacturer", "vendor", "item",
};

bool is_header_word(const std::string& cell) {
    std::string k = norm_key(cell);
    if (k.empty()) return false;
    for (const char* w : kHeaderWords) if (k == w) return true;
    // common prefix forms: "Center-X(mm)", "Ref X", "Part Number", "PosX"
    static const char* pre[] = {"designator", "reference", "centerx", "centery", "midx", "midy",
                                "posx", "posy", "rotation", "partnumber", "footprint", "description",
                                "package", "layer", "refdes", "xmm", "ymm", "xmil", "ymil", "xin", "yin",
                                "locx", "locy", "partno", "manufacturer", "vendor", "customer", "polar"};
    for (const char* p : pre) if (k.rfind(p, 0) == 0) return true;
    return false;
}

bool cells_header_like(const std::vector<std::string>& h,
                       const std::vector<std::vector<std::string>>& below) {
    int named = 0, kw = 0;
    for (auto& c : h) {
        if (c.empty()) continue;
        if (looks_numeric(c)) return false;
        ++named;
        if (is_header_word(c)) ++kw;
    }
    if (named == 0) return false;
    if (kw >= 1 && kw * 3 >= named) return true;
    // a column that is text in the header but numeric in most rows below
    for (size_t j = 0; j < h.size(); ++j) {
        if (h[j].empty()) continue;
        int num = 0, tot = 0;
        for (size_t r = 0; r < below.size() && r < 20; ++r) {
            if (j >= below[r].size() || below[r][j].empty()) continue;
            ++tot;
            if (looks_numeric(below[r][j])) ++num;
        }
        if (tot > 0 && num * 5 >= tot * 4) return kw >= 1 || named * 2 >= (int)h.size();
    }
    return false;
}

std::string expand_tabs(const std::string& s, int ts) {
    if (s.find('\t') == std::string::npos) return s;
    std::string r;
    for (char c : s) {
        if (c == '\t') { do r += ' '; while ((int)r.size() % ts); }
        else r += c;
    }
    return r;
}

std::vector<std::string> cut_fixed(const std::string& s, const std::vector<int>& cuts) {
    std::vector<std::string> out;
    for (size_t i = 0; i < cuts.size(); ++i) {
        size_t a = (size_t)cuts[i];
        size_t b = i + 1 < cuts.size() ? (size_t)cuts[i + 1] : std::string::npos;
        if (a >= s.size()) { out.push_back(""); continue; }
        out.push_back(trim(s.substr(a, b == std::string::npos ? std::string::npos : b - a)));
    }
    return out;
}

// Header tokens: runs of text separated by 2+ spaces (single spaces stay in names).
struct Tok { std::string text; int a, b; };
std::vector<Tok> header_tokens(const std::string& s) {
    std::vector<Tok> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') ++i;
        if (i >= s.size()) break;
        size_t a = i;
        while (i < s.size()) {
            if (s[i] == ' ' && (i + 1 >= s.size() || s[i + 1] == ' ')) break;
            ++i;
        }
        out.push_back({trim(s.substr(a, i - a)), (int)a, (int)i});
    }
    return out;
}

// Map header tokens onto fixed columns by character overlap (nearest when none).
std::vector<std::string> map_fixed_header(const std::string& hdr, const std::vector<int>& cuts, int width) {
    std::vector<std::string> names(cuts.size());
    // Header aligned with the data columns (every cut falls on a word boundary): split at cuts.
    bool aligned = true;
    for (int c : cuts) {
        if (c <= 0 || c >= (int)hdr.size()) continue;
        if (hdr[c - 1] != ' ' || hdr[c] == ' ') { aligned = false; break; }
    }
    if (aligned) {
        auto v = cut_fixed(hdr, cuts);
        bool all = true;
        for (auto& s : v) if (s.empty()) all = false;
        if (all) return v;
    }
    for (auto& t : header_tokens(hdr)) {
        int best = -1; int best_ov = 0; int best_d = 1 << 30;
        for (size_t c = 0; c < cuts.size(); ++c) {
            int ca = cuts[c], cb = c + 1 < cuts.size() ? cuts[c + 1] : std::max(width, t.b) + 1;
            int ov = std::min(cb, t.b) - std::max(ca, t.a);
            int d = std::abs(t.a - ca);
            if (ov > best_ov || (ov == best_ov && ov <= 0 && d < best_d)) { best = (int)c; best_ov = ov; best_d = d; }
        }
        if (best >= 0) names[best] = names[best].empty() ? t.text : names[best] + " " + t.text;
    }
    return names;
}

} // namespace

// ---------------------------------------------------------------------------
bool read_text_table(const std::string& text, const TextTableOptions& opt,
                     TextTable& out, std::string& err) {
    out = TextTable{};
    auto raw = split_lines(text);
    std::vector<Line> data;          // non-comment, non-blank
    std::vector<Line> comments;
    // index into comments of the last non-empty comment before the first data line
    for (size_t i = (size_t)std::max(0, opt.skip_lines); i < raw.size(); ++i) {
        std::string t = trim(raw[i]);
        if (t.empty()) { ++out.skipped_blank; continue; }
        bool is_comment = false;
        for (auto& p : opt.comment_prefixes)
            if (!p.empty() && t.rfind(p, 0) == 0) { is_comment = true; break; }
        if (is_comment) {
            out.comments.push_back(raw[i]);
            ++out.skipped_comment;
            if (data.empty()) comments.push_back({raw[i], (int)i + 1});
            continue;
        }
        data.push_back({raw[i], (int)i + 1});
    }
    if (data.empty()) { err = "no data lines"; return false; }

    // Comment header candidate: last comment (before data) with real text after the prefix.
    Line chdr{"", 0};
    std::string chdr_text;           // prefix blanked out, positions preserved
    if (opt.header_from_comment) {
        for (auto it = comments.rbegin(); it != comments.rend(); ++it) {
            std::string s = it->text;
            size_t p = 0;
            while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
            for (auto& pre : opt.comment_prefixes)
                if (!pre.empty() && s.compare(p, pre.size(), pre) == 0) {
                    // blank the prefix (and repeated prefix chars like "##")
                    size_t q = p + pre.size();
                    while (q < s.size() && s.compare(q, pre.size(), pre) == 0) q += pre.size();
                    for (size_t k = p; k < q; ++k) s[k] = ' ';
                    break;
                }
            if (!trim(s).empty()) { chdr = *it; chdr_text = s; break; }
        }
    }

    // ---------------- delimiter detection ----------------
    Delim d = opt.delim;
    int mode_count = 0;
    size_t table_start = 0;
    auto eval_delim = [&](char ch, int& mcount, size_t& start, double& score) {
        std::vector<int> counts;
        const size_t n = std::min<size_t>(data.size(), 400);
        for (size_t i = 0; i < n; ++i) counts.push_back((int)split_delimited(data[i].text, ch, opt.quote).size());
        std::map<int, int> hist;
        for (int c : counts) ++hist[c];
        int m = 1, mc = 0;
        for (auto& [k, v] : hist) if (v > mc || (v == mc && k > m)) { m = k; mc = v; }
        mcount = m;
        score = 0;
        if (m < 2) return;
        size_t s0 = 0;
        while (s0 < counts.size() && counts[s0] != m) ++s0;
        int hit = 0;
        for (size_t i = s0; i < counts.size(); ++i) if (counts[i] == m) ++hit;
        start = s0;
        score = (double)hit / (double)(counts.size() - s0);
    };
    if (d == Delim::Auto) {
        const Delim cands[] = {Delim::Comma, Delim::Tab, Delim::Semicolon, Delim::Pipe};
        double best = 0; int bestm = 0;
        for (Delim c : cands) {
            int m = 0; size_t st = 0; double sc = 0;
            eval_delim(delim_char(c), m, st, sc);
            if (sc >= 0.8 && (sc > best + 0.05 || (sc > best - 0.05 && m > bestm))) {
                best = sc; bestm = m; d = c; mode_count = m; table_start = st;
            }
        }
    } else if (delim_char(d)) {
        double sc; eval_delim(delim_char(d), mode_count, table_start, sc);
        if (mode_count < 1) mode_count = 1;
    }

    if (delim_char(d)) {
        // ---------------- delimited ----------------
        const char ch = delim_char(d);
        for (size_t i = 0; i < table_start; ++i) out.preamble.push_back(data[i].text);
        std::vector<std::vector<std::string>> rows;
        std::vector<int> lines;
        for (size_t i = table_start; i < data.size(); ++i) {
            rows.push_back(split_delimited(data[i].text, ch, opt.quote));
            lines.push_back(data[i].lineno);
        }
        const size_t nc = (size_t)std::max(mode_count, 1);
        size_t first = 0;
        bool hdr = false;
        if (opt.header == 1) hdr = true;
        else if (opt.header == -1 && !rows.empty()) {
            std::vector<std::vector<std::string>> below(rows.begin() + 1, rows.end());
            hdr = cells_header_like(rows[0], below);
        }
        if (hdr) {
            out.columns = rows[0];
            out.has_header = true;
            out.header_line = lines[0];
            out.header_text = data[table_start].text;
            first = 1;
        } else if (opt.header != 0 && chdr.lineno) {
            auto c = split_delimited(trim(chdr_text), ch, opt.quote);
            std::vector<std::vector<std::string>> below(rows.begin(), rows.end());
            if (c.size() == nc && cells_header_like(c, below)) {
                out.columns = c; out.has_header = true; out.header_from_comment = true;
                out.header_line = chdr.lineno; out.header_text = chdr.text;
            }
        }
        out.columns.resize(nc);
        for (size_t r = first; r < rows.size(); ++r) {
            auto& row = rows[r];
            if (row.size() != nc) ++out.ragged_rows;
            row.resize(nc);
            out.rows.push_back(row);
            out.row_line.push_back(lines[r]);
        }
        out.delim = d;
        return true;
    }

    // ---------------- fixed-width / whitespace ----------------
    std::vector<std::string> exp;
    for (auto& l : data) exp.push_back(expand_tabs(l.text, opt.tab_size));

    // Uncommented header candidate: first line, all whitespace tokens non-numeric.
    bool first_is_hdr = false;
    if (opt.header == 1) first_is_hdr = true;
    else if (opt.header == -1 && exp.size() >= 2) {
        auto h = split_ws_quoted(exp[0]);
        std::vector<std::vector<std::string>> below;
        for (size_t i = 1; i < exp.size() && i < 21; ++i) below.push_back(split_ws_quoted(exp[i]));
        // positional numeric test is unreliable with spaces in names, so require keywords
        int kw = 0, num = 0;
        for (auto& t : h) { if (looks_numeric(t)) ++num; if (is_header_word(t)) ++kw; }
        first_is_hdr = num == 0 && kw >= 1;
        (void)below;
    }
    const size_t d0 = first_is_hdr ? 1 : 0;
    int width = 0;
    for (size_t i = d0; i < exp.size(); ++i) width = std::max(width, (int)exp[i].size());

    std::vector<int> cuts = opt.fixed_cuts;
    if (cuts.empty() && d != Delim::Whitespace) {
        std::vector<char> used(width, 0);
        for (size_t i = d0; i < exp.size(); ++i)
            for (size_t k = 0; k < exp[i].size(); ++k)
                if (exp[i][k] != ' ') used[k] = 1;
        const size_t nlines = exp.size() - d0;
        const int min_gap = nlines >= 10 ? 1 : 2;
        int k = 0;
        while (k < width && !used[k]) ++k;
        if (k < width) cuts.push_back(k);
        while (k < width) {
            while (k < width && used[k]) ++k;
            int g0 = k;
            while (k < width && !used[k]) ++k;
            if (k < width && k - g0 >= min_gap) cuts.push_back(k);
        }
        // Compare with whitespace token count: unaligned files give few cuts.
        std::map<int, int> hist;
        for (size_t i = d0; i < exp.size() && i < 400; ++i) ++hist[(int)split_ws_quoted(exp[i]).size()];
        int wm = 0, wc = 0;
        for (auto& [kk, v] : hist) if (v > wc) { wm = kk; wc = v; }
        const double wcons = (double)wc / (double)std::max<size_t>(1, std::min<size_t>(nlines, 400));
        if (d == Delim::Auto && (int)cuts.size() < wm && wm >= 2 && wcons >= 0.6) {
            d = Delim::Whitespace;
            mode_count = wm;
        } else {
            d = Delim::Fixed;
        }
    } else if (!cuts.empty()) {
        d = Delim::Fixed;
    }

    if (d == Delim::Fixed) {
        if (cuts.empty()) cuts.push_back(0);
        out.cuts = cuts;
        out.delim = Delim::Fixed;
        std::vector<std::string> names;
        if (first_is_hdr) {
            names = map_fixed_header(exp[0], cuts, width);
            out.has_header = true; out.header_line = data[0].lineno; out.header_text = data[0].text;
        } else if (opt.header != 0 && chdr.lineno) {
            std::string ht = expand_tabs(chdr_text, opt.tab_size);
            auto n2 = map_fixed_header(ht, cuts, width);
            int named = 0, num = 0, kw = 0;
            for (auto& n : n2) if (!n.empty()) { ++named; if (looks_numeric(n)) ++num; if (is_header_word(n)) ++kw; }
            if (named * 2 >= (int)cuts.size() && num == 0 && (kw >= 1 || named == (int)cuts.size())) {
                names = n2; out.has_header = true; out.header_from_comment = true;
                out.header_line = chdr.lineno; out.header_text = chdr.text;
            }
        }
        names.resize(cuts.size());
        out.columns = names;
        for (size_t i = d0; i < exp.size(); ++i) {
            out.rows.push_back(cut_fixed(exp[i], cuts));
            out.row_line.push_back(data[i].lineno);
        }
        return true;
    }

    // Whitespace split
    out.delim = Delim::Whitespace;
    const size_t nc = (size_t)std::max(mode_count, 1);
    std::vector<std::string> names;
    auto fit_header = [&](const std::string& h) {
        auto t = split_ws_quoted(h);
        if (t.size() == nc) return t;
        std::vector<std::string> t2;
        for (auto& x : header_tokens(h)) t2.push_back(x.text);
        if (t2.size() == nc) return t2;
        t.resize(nc);
        return t;
    };
    if (first_is_hdr) {
        names = fit_header(exp[0]);
        out.has_header = true; out.header_line = data[0].lineno; out.header_text = data[0].text;
    } else if (opt.header != 0 && chdr.lineno) {
        auto t = fit_header(trim(chdr_text));
        int num = 0, kw = 0;
        for (auto& n : t) { if (looks_numeric(n)) ++num; if (is_header_word(n)) ++kw; }
        if (num == 0 && kw >= 1) {
            names = t; out.has_header = true; out.header_from_comment = true;
            out.header_line = chdr.lineno; out.header_text = chdr.text;
        }
    }
    names.resize(nc);
    out.columns = names;
    for (size_t i = d0; i < exp.size(); ++i) {
        auto row = split_ws_quoted(exp[i]);
        if (row.size() != nc) {
            ++out.ragged_rows;
            // extra tokens are folded into the last column
            if (row.size() > nc && nc > 0) {
                for (size_t k = nc; k < row.size(); ++k) row[nc - 1] += " " + row[k];
            }
            row.resize(nc);
        }
        out.rows.push_back(row);
        out.row_line.push_back(data[i].lineno);
    }
    return true;
}

bool read_text_table_file(const std::string& path, const TextTableOptions& opt,
                          TextTable& out, std::string& err) {
    std::string text;
    if (!read_file(path, text, err)) return false;
    return read_text_table(text, opt, out, err);
}

} // namespace pcbp::io
