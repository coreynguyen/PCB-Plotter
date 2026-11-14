#pragma once
// ============================================================================
// text_table.h — generic tabular text reader (pcbp::io).
//
// Reads the "ASCII" files CircuitCAM's ASCII Centroid / Fusion / BOM import
// dialogs accept: delimited (comma, tab, semicolon, pipe) or fixed-width
// columns, with comment lines, optional preamble, and an optional header row
// (which may itself be commented out, e.g. DemoBOM.prn's ";Reference  Part...").
//
// Auto-detection (each step can be overridden through TextTableOptions):
//   1. Lines: skip_lines first lines dropped; blank lines ignored; lines whose
//      first non-blank chars match a comment prefix are kept in `comments`.
//   2. Delimiter: for each candidate, the per-line field count (quote aware)
//      must have a mode >= 2 that covers >= 80% of the lines from the first
//      line having that count (earlier lines become `preamble`).
//   3. Otherwise fixed-width: character positions that are blank on every
//      data line form gaps; column cuts are the starts of non-gap runs. This
//      keeps fields with embedded spaces ("C1-C25, C27-C34") intact. If the
//      lines are not aligned, plain whitespace splitting is used instead.
//   4. Header: first table line if it looks like a header (no numeric cells
//      and either known keywords or numeric cells below), else the last
//      non-empty comment line before the data if it maps onto the columns.
//      Fixed-width header words are assigned to columns by text overlap.
// ============================================================================
#include <string>
#include <vector>

namespace pcbp::io {

enum class Delim { Auto, Comma, Tab, Semicolon, Pipe, Whitespace, Fixed };

const char* delim_name(Delim d);

struct TextTableOptions {
    Delim delim = Delim::Auto;
    std::vector<std::string> comment_prefixes{";", "#", "//"};
    int skip_lines = 0;             // raw lines dropped before anything else
    int header = -1;                // -1 auto, 0 = no header, 1 = first table line is header
    bool header_from_comment = true;// allow a commented-out header line
    std::vector<int> fixed_cuts;    // Fixed: user column start positions (0-based chars)
    char quote = '"';
    int tab_size = 8;               // tab expansion for fixed-width analysis
};

struct TextTable {
    std::vector<std::string> columns;               // header names ("" when unknown)
    std::vector<std::vector<std::string>> rows;     // trimmed cells, each row has columns.size() cells
    std::vector<int> row_line;                      // 1-based source line of each row
    std::vector<std::string> comments;              // comment lines (raw, without line ending)
    std::vector<std::string> preamble;              // non-comment lines before the table
    std::string header_text;                        // raw header line used (if any)
    Delim delim = Delim::Auto;                      // detected / used delimiter
    std::vector<int> cuts;                          // Fixed: column start positions
    bool has_header = false;
    bool header_from_comment = false;
    int header_line = 0;                            // 1-based, 0 = none
    int ragged_rows = 0;                            // delimited rows with != column count
    int skipped_blank = 0, skipped_comment = 0;

    size_t ncols() const { return columns.size(); }
    // Case/punctuation-insensitive column lookup by name; -1 when absent.
    int col(const std::string& name) const;
    // Column name or "Col<n>" when unnamed.
    std::string col_name(int i) const;
    std::string describe() const;
};

bool read_text_table(const std::string& text, const TextTableOptions& opt,
                     TextTable& out, std::string& err);
bool read_text_table_file(const std::string& path, const TextTableOptions& opt,
                          TextTable& out, std::string& err);

// Quote-aware split on a single delimiter character (doubled quote = literal).
std::vector<std::string> split_delimited(const std::string& line, char delim, char quote = '"');

} // namespace pcbp::io
