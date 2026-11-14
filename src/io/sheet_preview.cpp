// ============================================================================
// sheet_preview.cpp — see sheet_preview.h.
// ============================================================================
#include "sheet_preview.h"
#include "ods.h"
#include "xlsx.h"
#include "jet_mdb.h"
#include "../core/sheet.h"
#include <algorithm>
#include <array>
#include <cctype>

namespace pcbp::io {

namespace {
std::string lower_ext(const std::string& name) {
    size_t d = name.find_last_of('.');
    std::string e = d == std::string::npos ? std::string() : name.substr(d + 1);
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

// R23a: Jet3/Jet4 databases (.mdb, CircuitCAM .cpf, FlexLine .flx/.mdb) as read-only sheets: a
// "Tables" overview tab, then one tab per user table (header row in bold, numbers right aligned).
bool load_jet(const std::vector<uint8_t>& bytes, SheetPreview& out, std::string& err, int max_rows, int max_cols) {
    jet::Database db;
    if (!db.open_memory(bytes, err)) return false;
    out.how = db.version() == 3 ? "Access 97 database (Jet 3)" : "Access 2000-2003 database (Jet 4)";
    std::vector<std::string> names = db.tables();
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        std::string x = a, y = b;
        for (auto& c : x) c = (char)std::tolower((unsigned char)c);
        for (auto& c : y) c = (char)std::tolower((unsigned char)c);
        return x < y;
    });
    SheetPreview::Tab ov;
    ov.name = "Tables";
    std::vector<std::array<std::string, 4>> ovrows;
    const size_t cell_budget = 2'000'000;
    size_t used = 0;
    for (const std::string& n : names) {
        jet::Table t;
        std::string e;
        if (!db.read_table(n, t, e)) { ovrows.push_back({n, "", "", "could not be read: " + e}); continue; }
        ovrows.push_back({n, std::to_string(t.rows.size()), std::to_string(t.cols.size()), ""});
        SheetPreview::Tab tb;
        tb.name = n;
        tb.total_cols = (int)t.cols.size();
        tb.total_rows = (int)t.rows.size() + 1;
        tb.cols = std::min(tb.total_cols, std::max(1, max_cols));
        tb.rows = std::min(tb.total_rows, std::max(1, max_rows));
        if (tb.cols > 0 && used + (size_t)tb.rows * tb.cols > cell_budget)
            tb.rows = std::max(1, (int)((cell_budget > used ? cell_budget - used : 0) / tb.cols));
        if (tb.cols == 0) tb.rows = 0;
        used += (size_t)tb.rows * tb.cols;
        tb.cells.resize((size_t)tb.rows * tb.cols);
        tb.right.assign(tb.cells.size(), 0);
        tb.bold.assign(tb.cells.size(), 0);
        for (int c = 0; c < tb.cols; ++c) {
            tb.cells[(size_t)c] = t.cols[(size_t)c].name;
            tb.bold[(size_t)c] = 1;
            size_t w = t.cols[(size_t)c].name.size();
            for (int r = 1; r < tb.rows && r < 200; ++r) w = std::max(w, t.rows[(size_t)r - 1][(size_t)c].as_string().size());
            tb.col_chars.push_back((float)std::clamp<size_t>(w + 1, 6, 48));
        }
        for (int r = 1; r < tb.rows; ++r)
            for (int c = 0; c < tb.cols; ++c) {
                const jet::Value& v = t.rows[(size_t)r - 1][(size_t)c];
                const size_t k = (size_t)r * tb.cols + c;
                std::string str = v.as_string();
                if (str.size() > 4096) str = str.substr(0, 4096) + "...";   // long OLE/memo values
                tb.cells[k] = std::move(str);
                switch (v.type) {
                case jet::ColType::Byte: case jet::ColType::Int: case jet::ColType::Long: case jet::ColType::Money:
                case jet::ColType::Float: case jet::ColType::Double: case jet::ColType::Numeric: tb.right[k] = !v.null; break;
                default: break;
                }
            }
        out.tabs.push_back(std::move(tb));
    }
    const char* hdr[4] = {"Table", "Rows", "Columns", "Note"};
    ov.cols = ov.total_cols = 4;
    ov.rows = ov.total_rows = (int)ovrows.size() + 1;
    ov.cells.resize((size_t)ov.rows * 4);
    ov.right.assign(ov.cells.size(), 0);
    ov.bold.assign(ov.cells.size(), 0);
    ov.col_chars = {28, 9, 9, 40};
    for (int c = 0; c < 4; ++c) { ov.cells[(size_t)c] = hdr[c]; ov.bold[(size_t)c] = 1; }
    for (size_t r = 0; r < ovrows.size(); ++r)
        for (int c = 0; c < 4; ++c) {
            ov.cells[(r + 1) * 4 + c] = ovrows[r][(size_t)c];
            ov.right[(r + 1) * 4 + c] = c == 1 || c == 2;
        }
    out.tabs.insert(out.tabs.begin(), std::move(ov));
    if (names.empty()) { err = "the database has no user tables"; out.tabs.clear(); return false; }
    return true;
}
}

bool is_jet_bytes(const std::vector<uint8_t>& b) {
    return b.size() >= 0x20 && b[0] == 0 && b[1] == 1 && b[2] == 0 && b[3] == 0 &&
           (std::equal(b.begin() + 4, b.begin() + 19, "Standard Jet DB") || std::equal(b.begin() + 4, b.begin() + 19, "Standard ACE DB"));
}

bool load_sheet_preview(const std::vector<uint8_t>& bytes, const std::string& name, SheetPreview& out, std::string& err,
                        int max_rows, int max_cols) {
    out = SheetPreview{};
    sheet::Book bk;
    const std::string e = lower_ext(name);
    if (bytes.empty()) { err = "empty file"; return false; }
    if (is_jet_bytes(bytes)) return load_jet(bytes, out, err, max_rows, max_cols);
    if (is_ods_bytes(bytes)) { if (!ods_read(bytes, bk, err)) return false; out.how = "OpenDocument spreadsheet (.ods)"; }
    else if (is_xlsx_bytes(bytes)) { if (!xlsx_read(bytes, bk, err)) return false; out.how = "Excel workbook (.xlsx)"; }
    else if (is_ole2_bytes(bytes)) { if (!xls_read(bytes, bk, err)) return false; out.how = "Excel 97-2003 workbook (.xls)"; }
    else if (e == "csv" || e == "tsv" || e == "txt" || e == "prn") {
        CsvOptions o;
        o.sep = e == "tsv" ? '\t' : 0;   // 0 = auto-detect (, ; TAB |)
        if (!csv_read(bytes, bk, 0, o, err)) return false;
        std::string stem = name.substr(0, name.find_last_of('.'));
        size_t sl = stem.find_last_of("/\\");
        if (sl != std::string::npos) stem = stem.substr(sl + 1);
        if (!stem.empty()) bk.sheets[0].name = stem.substr(0, 31);
        out.how = e == "tsv" ? "Tab separated text" : "CSV text";
    } else { err = "not a spreadsheet (unknown format)"; return false; }

    const size_t cell_budget = 2'000'000;   // keep huge sheets responsive
    for (int si = 0; si < bk.sheet_count(); ++si) {
        const sheet::Worksheet& ws = bk.sheet(si);
        if (ws.hidden) continue;
        SheetPreview::Tab t;
        t.name = ws.name;
        t.total_rows = std::max(0, ws.nrows());
        t.total_cols = std::max(0, ws.ncols());
        t.cols = std::min(t.total_cols, std::max(1, max_cols));
        t.rows = std::min(t.total_rows, std::max(1, max_rows));
        if (t.cols > 0 && (size_t)t.rows * t.cols > cell_budget) t.rows = (int)(cell_budget / t.cols);
        if (t.total_rows == 0 || t.total_cols == 0) { t.rows = t.cols = 0; }
        t.cells.resize((size_t)t.rows * t.cols);
        t.right.assign(t.cells.size(), 0);
        t.bold.assign(t.cells.size(), 0);
        for (int c = 0; c < t.cols; ++c) t.col_chars.push_back((float)ws.col_width(c));
        for (int r = 0; r < t.rows; ++r)
            for (int c = 0; c < t.cols; ++c) {
                const sheet::Cell* cl = ws.cell(r, c);
                if (!cl) continue;
                const size_t k = (size_t)r * t.cols + c;
                sheet::CellLook lk = bk.look(si, r, c);
                t.cells[k] = lk.text;
                t.right[k] = lk.align == sheet::HAlign::Right ? 1 : 0;
                t.bold[k] = lk.style.font.bold ? 1 : 0;
            }
        out.tabs.push_back(std::move(t));
    }
    if (out.tabs.empty()) { err = "the workbook has no visible sheets"; return false; }
    return true;
}

// ---- R23a selection / copy / find ------------------------------------------------------------------
std::string preview_tsv(const SheetPreview::Tab& t, int r0, int c0, int r1, int c1) {
    if (t.rows <= 0 || t.cols <= 0) return {};
    if (r0 > r1) std::swap(r0, r1);
    if (c0 > c1) std::swap(c0, c1);
    r0 = std::clamp(r0, 0, t.rows - 1); r1 = std::clamp(r1, 0, t.rows - 1);
    c0 = std::clamp(c0, 0, t.cols - 1); c1 = std::clamp(c1, 0, t.cols - 1);
    std::string o;
    for (int r = r0; r <= r1; ++r) {
        for (int c = c0; c <= c1; ++c) {
            if (c > c0) o += '\t';
            const std::string& v = t.at(r, c);
            if (v.find_first_of("\t\r\n\"") != std::string::npos) {
                o += '"';
                for (char ch : v) { if (ch == '"') o += '"'; o += ch; }
                o += '"';
            } else o += v;
        }
        o += "\r\n";
    }
    return o;
}

bool preview_cell_matches(const std::string& cell, const std::string& q, bool match_case) {
    if (q.empty() || cell.size() < q.size()) return false;
    if (match_case) return cell.find(q) != std::string::npos;
    auto lo = [](unsigned char ch) { return (ch >= 'A' && ch <= 'Z') ? (unsigned char)(ch + 32) : ch; };
    for (size_t i = 0; i + q.size() <= cell.size(); ++i) {
        size_t k = 0;
        while (k < q.size() && lo((unsigned char)cell[i + k]) == lo((unsigned char)q[k])) ++k;
        if (k == q.size()) return true;
    }
    return false;
}

bool preview_find(const SheetPreview::Tab& t, const std::string& q, bool match_case, bool forward, bool include_start, int& r, int& c) {
    const long long n = (long long)t.rows * t.cols;
    if (n <= 0 || q.empty()) return false;
    long long start = (long long)std::clamp(r, 0, std::max(0, t.rows - 1)) * t.cols + std::clamp(c, 0, std::max(0, t.cols - 1));
    for (long long step = include_start ? 0 : 1; step <= n; ++step) {
        long long k = forward ? (start + step) % n : ((start - step) % n + n) % n;
        if (preview_cell_matches(t.cells[(size_t)k], q, match_case)) { r = (int)(k / t.cols); c = (int)(k % t.cols); return true; }
        if (include_start && step == n - 1) break;
    }
    return false;
}

int preview_count(const SheetPreview::Tab& t, const std::string& q, bool match_case) {
    int n = 0;
    for (const auto& s : t.cells) n += preview_cell_matches(s, q, match_case) ? 1 : 0;
    return n;
}

} // namespace pcbp::io
