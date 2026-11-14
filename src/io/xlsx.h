#pragma once
// ============================================================================
// xlsx.h — spreadsheet file I/O (pcbp::io).
//   * .xlsx read/write FULL (ECMA-376 SpreadsheetML): shared strings, styles
//     (numFmts/fonts/fills/borders/cellXfs/dxfs), merges, column widths, row
//     heights, hidden rows/cols, freeze panes, autoFilter, conditional
//     formatting, data validation, formulas + cached values, defined names,
//     tab colours, multiple sheets. Zip entries DEFLATE-compressed (deflate.h).
//   * .xls read (OLE2 + BIFF8): strings, numbers, RK/MULRK, formulas' cached
//     results, FORMAT/FONT/XF -> styles, column widths, merges.
//   * CSV/TSV read/write with encoding options.
// Legacy (string grid) overloads are kept for older callers.
// ============================================================================
#include "../core/sheet.h"
#include <cstdint>
#include <string>
#include <vector>

namespace pcbp::io {

// ---- full engine ----
bool xlsx_read(const std::vector<uint8_t>& data, sheet::Book& book, std::string& err);
std::vector<uint8_t> xlsx_write(const sheet::Book& book, int deflate_level = 6);
bool xls_read(const std::vector<uint8_t>& data, sheet::Book& book, std::string& err);

enum class TextEncoding { Auto, Utf8, Utf8Bom, Utf16LE, Utf16BE, Windows1252, Latin1 };
struct CsvOptions {
    char sep = ',';               // 0 on read = auto-detect (, ; TAB |)
    char quote = '"';
    TextEncoding encoding = TextEncoding::Auto;   // write: Auto = UTF-8 with BOM
    bool crlf = true;             // write line ending
    bool detect_types = true;     // read: numbers/dates via parse_input ("0805" stays text)
    bool formulas = false;        // read: "=..." becomes a formula; write: formulas instead of values
};
bool csv_read(const std::vector<uint8_t>& data, sheet::Book& book, int si, const CsvOptions& opt, std::string& err);
std::vector<uint8_t> csv_write(const sheet::Book& book, int si, const CsvOptions& opt);
std::string decode_text_bytes(const std::vector<uint8_t>& data, TextEncoding enc);   // -> UTF-8
std::vector<uint8_t> encode_text(const std::string& utf8, TextEncoding enc);

// ---- legacy string-grid API (values only) ----
bool xlsx_read(const std::vector<uint8_t>& data, sheet::Workbook& wb, std::string& err);
std::vector<uint8_t> xlsx_write(const sheet::Workbook& wb);
bool xls_read(const std::vector<uint8_t>& data, sheet::Workbook& wb, std::string& err);

// Zip writer. level 0 = stored; 1..9 = DEFLATE (falls back to stored when larger).
std::vector<uint8_t> zip_store(const std::vector<std::pair<std::string, std::string>>& entries);
std::vector<uint8_t> zip_write(const std::vector<std::pair<std::string, std::string>>& entries, int level = 6,
                               const std::string& first_stored_entry = "");   // e.g. ODF "mimetype"

// Sniffers
bool is_xlsx_bytes(const std::vector<uint8_t>& d);   // "PK\3\4"
bool is_ole2_bytes(const std::vector<uint8_t>& d);   // D0 CF 11 E0

} // namespace pcbp::io
