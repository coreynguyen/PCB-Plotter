#pragma once
// ============================================================================
// sheet_preview.h — R20 read-only spreadsheet preview loader (pcbp::io, portable).
//
// Loads .xlsx / .xlsm / .xls / .ods / .csv / .tsv BYTES (e.g. an entry of the job archive, never a
// temp file) with the existing sheet engine readers and flattens every visible worksheet into a
// grid of display strings (number formats applied, cached formula values; nothing recalculated
// or written back). Used by the Files tab preview modal; it never touches the spreadsheet
// workspace / File > Open path.
// ============================================================================
#include <cstdint>
#include <string>
#include <vector>

namespace pcbp::io {

struct SheetPreview {
    struct Tab {
        std::string name;
        int rows = 0, cols = 0;              // shown size (clipped to the limits)
        int total_rows = 0, total_cols = 0;  // used size in the file
        std::vector<std::string> cells;      // row-major, rows * cols display strings
        std::vector<float> col_chars;        // column widths (characters)
        std::vector<uint8_t> right;          // 1 = right aligned (numbers), row-major
        std::vector<uint8_t> bold;           // 1 = bold font, row-major
        const std::string& at(int r, int c) const { return cells[(size_t)r * cols + c]; }
        bool truncated() const { return rows < total_rows || cols < total_cols; }
    };
    std::vector<Tab> tabs;
    std::string how;                         // "Excel workbook (.xlsx)", "CSV", ...
};

// R23a: Jet3/Jet4 databases (.mdb / .cpf / .flx; detected by content) load as a "Tables" overview tab
// plus one tab per user table (row 0 = column names). ACCDB is reported as unsupported.
bool is_jet_bytes(const std::vector<uint8_t>& bytes);

// name: file name (extension decides CSV / TSV when the bytes are not a workbook).
bool load_sheet_preview(const std::vector<uint8_t>& bytes, const std::string& name, SheetPreview& out, std::string& err,
                        int max_rows = 20000, int max_cols = 256);

// R23a: selection / copy / find on a preview tab (used by the Files preview grid; portable + tested).
// TSV for the inclusive cell range (any corner order): cells joined by TAB, rows by CRLF; a cell with
// TAB / CR / LF / '"' is quoted Excel-style ("" escapes) so it pastes back into one cell.
std::string preview_tsv(const SheetPreview::Tab& t, int r0, int c0, int r1, int c1);
// Next (forward) / previous match of `q` after (before) cell (r, c), row-major, wrapping around; with
// include_start the start cell itself is tested first. Case-insensitive (ASCII + Latin-1/basic UTF-8
// letters unaffected) unless match_case. Returns false when nothing matches; r/c = the match otherwise.
bool preview_find(const SheetPreview::Tab& t, const std::string& q, bool match_case, bool forward, bool include_start, int& r, int& c);
// all cells matching (for highlights / "n of m")
int preview_count(const SheetPreview::Tab& t, const std::string& q, bool match_case);
bool preview_cell_matches(const std::string& cell, const std::string& q, bool match_case);

} // namespace pcbp::io
