#pragma once
// ============================================================================
// ods.h — OpenDocument Spreadsheet (.ods) read/write (pcbp::io), from the
// OASIS ODF 1.2/1.3 specification: content.xml (table:table rows/cells with
// office:value-type, table:formula "of:=" syntax, spans, repeats, covered
// cells), styles.xml / automatic styles (fo:* text and cell properties,
// number:* data styles), settings.xml freeze panes. See specs/SHEET_ENGINE.md.
// ============================================================================
#include "../core/sheet.h"
#include <cstdint>
#include <string>
#include <vector>

namespace pcbp::io {

bool is_ods_bytes(const std::vector<uint8_t>& d);   // zip whose "mimetype" is ...opendocument.spreadsheet
bool ods_read(const std::vector<uint8_t>& data, sheet::Book& book, std::string& err);
std::vector<uint8_t> ods_write(const sheet::Book& book, int deflate_level = 6);

// OpenFormula <-> Excel A1 syntax ("of:=SUM([.A1:.B2])" <-> "SUM(A1:B2)")
std::string of_to_a1(const std::string& of);
std::string a1_to_of(const std::string& a1);

} // namespace pcbp::io
