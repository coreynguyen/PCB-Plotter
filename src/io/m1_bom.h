#pragma once
// ============================================================================
// m1_bom.h — built-in "M1 BOM" header profile (ERP job-material export) for
// the BOM wizard, plus helpers shared by the file import and the live
// "Import from M1 by Job ID" fetch (src/app/m1_live.cpp). See specs/M1_BOM.md.
//
// Portable (no Windows API): compiled into the Linux unit tests.
// ============================================================================
#include "text_import.h"
#include <string>
#include <vector>

namespace pcbp::io {

inline constexpr const char* kM1BomTemplateName = "M1 BOM";

// The 16 export headers, in export / SELECT order.
const std::vector<std::string>& m1_bom_headers();

// Built-in template: jmmPartID -> Part Number, ujmmPCRefText -> Reference,
// jmmQuantityPerAssembly -> Quantity, jmmPartShortDescription -> Description,
// impPartClassID -> Process, uimpMfgPartID -> MPN, uimpManufacturer ->
// Manufacturer, jmmJobMaterialID -> Line Item, jmmPartBinID -> Bin; rest unused.
FormatTemplate m1_bom_template();

// Adds the built-in BOM profiles that are missing (matched by name + purpose)
// to a user template list. A user-saved template of the same name wins, so the
// profile is user-editable. Returns true when something was added.
bool ensure_builtin_bom_templates(std::vector<FormatTemplate>& v);

// True when the analysed header set is an M1 job-material export
// (jmmPartID + ujmmPCRefText + jmmQuantityPerAssembly present).
bool is_m1_bom(const ParsedText& t);

// First non-empty jmmJobID value in the data rows ("" if none / not M1).
std::string m1_job_id(const ParsedText& t);

// Job IDs are short ERP keys ("123456-10-01"): letters, digits, '-', '_', '.', '/'.
// Trims; returns false (and leaves out empty) for anything else.
bool m1_normalize_job_id(const std::string& in, std::string& out);

// Tab-separated text (header line + rows) suitable for the BOM wizard source.
// Tabs / newlines inside cells become spaces.
std::string m1_rows_to_tsv(const std::vector<std::string>& headers,
                           const std::vector<std::vector<std::string>>& rows);

// Part-class tokens that mean "bare board" (the PCB line of a job BOM).
bool m1_is_board_class(const std::string& cls);

} // namespace pcbp::io
