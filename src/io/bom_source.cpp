// ============================================================================
// bom_source.cpp — BOM file loading for the BOM import wizard (REVIEW_V9 §11A):
// .xlsx / .xls / .ods are read with the spreadsheet engine and flattened to
// tab-separated text (cell text as typed: part numbers and references are never
// re-formatted), merged rows are recorded so section titles can be excluded.
// Any other file is passed through as text (CSV / TSV / PRN / TXT ...).
// ============================================================================
#include "bom_import2.h"
#include "io_util.h"
#include "ods.h"
#include "xlsx.h"
#include <algorithm>

namespace pcbp::io {

static std::string cell_text(const sheet::Book& bk, int si, int r, int c) {
    const sheet::Value& v = bk.sheet(si).value(r, c);
    std::string s;
    if (v.is_empty()) return s;
    if (v.is_num()) {
        // keep zero-padded displays ("0805" formatted as 0000) — digits only, same value
        std::string d = bk.display_text(si, r, c);
        bool digits = !d.empty() && std::all_of(d.begin(), d.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
        s = digits && d.size() > 1 && d[0] == '0' ? d : sheet::value_to_text(v);
    } else if (v.is_str()) s = v.str;
    else s = sheet::value_to_text(v);
    for (char& ch : s) if (ch == '\t' || ch == '\r' || ch == '\n') ch = ' ';
    return s;
}

static int sheet_score(const sheet::Book& bk, int si) {
    const auto& ws = bk.sheet(si);
    int nr = std::min(ws.nrows(), 3000), nc = std::min(ws.ncols(), 64), score = 0;
    for (int r = 0; r < nr; ++r)
        for (int c = 0; c < nc; ++c) {
            const sheet::Value& v = ws.value(r, c);
            if (v.is_str() && v.str.size() > 1 && is_ref_list(v.str)) { ++score; break; }
        }
    return score;
}

static bool from_book(const sheet::Book& bk, const std::string& kind, int sheet_index, BomSource& out, std::string& err) {
    out = BomSource{};
    out.kind = kind;
    for (auto& s : bk.sheets) out.sheets.push_back(s.name);
    if (bk.sheet_count() == 0) { err = "workbook has no sheets"; return false; }
    int si = sheet_index;
    if (si < 0 || si >= bk.sheet_count()) {
        int best = -1;
        si = 0;
        for (int i = 0; i < bk.sheet_count(); ++i) {
            if (bk.sheet(i).hidden) continue;
            int sc = sheet_score(bk, i);
            if (sc > best) { best = sc; si = i; }
        }
    }
    out.sheet = bk.sheet(si).name;
    const auto& ws = bk.sheet(si);
    const int nr = ws.nrows(), nc = std::min(ws.ncols(), 256);
    out.merged.assign(nr, 0);
    for (auto& m : ws.merges)
        if (m.cols() >= 3)
            for (int r = std::max(0, m.r0); r <= m.r1 && r < nr; ++r) out.merged[r] = 1;
    std::string& t = out.text;
    for (int r = 0; r < nr; ++r) {
        std::vector<std::string> cells(nc);
        int last = -1;
        for (int c = 0; c < nc; ++c) { cells[c] = cell_text(bk, si, r, c); if (!cells[c].empty()) last = c; }
        // keep every row the full width so columns line up; trailing blanks trimmed per row
        for (int c = 0; c <= last; ++c) { if (c) t += '\t'; t += cells[c]; }
        t += '\n';
    }
    return true;
}

bool bom_source_from_bytes(const std::string& bytes, const std::string& name_hint, BomSource& out, std::string& err, int sheet_index) {
    std::vector<uint8_t> d(bytes.begin(), bytes.end());
    sheet::Book bk;
    if (is_ods_bytes(d)) { if (!ods_read(d, bk, err)) return false; return from_book(bk, "ods", sheet_index, out, err); }
    if (is_xlsx_bytes(d)) { if (!xlsx_read(d, bk, err)) return false; return from_book(bk, "xlsx", sheet_index, out, err); }
    if (is_ole2_bytes(d)) { if (!xls_read(d, bk, err)) return false; return from_book(bk, "xls", sheet_index, out, err); }
    (void)name_hint;
    out = BomSource{};
    out.kind = "text";
    out.text = bytes;
    return true;
}

bool bom_load_source(const std::string& path, BomSource& out, std::string& err, int sheet_index) {
    std::string bytes;
    if (!read_file(path, bytes, err)) return false;
    return bom_source_from_bytes(bytes, path, out, err, sheet_index);
}

} // namespace pcbp::io
