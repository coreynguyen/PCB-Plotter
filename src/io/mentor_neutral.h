#pragma once
// ============================================================================
// mentor_neutral.h — Mentor Graphics Boardstation / Expedition "neutral
// file" (.neu) import (pcbp::io), portable.
//
// Records (whitespace separated, double quotes allowed):
//   B_UNITS <Inches|Mils|Millimeters|Microns>      (default inches)
//   B_ATTR  <name> <value...>                      (BOARD_NAME kept)
//   C <ref> [<partnum>] <geom> <x> <y> <side> <rot>
//   P <ref> <pin> <x> <y> <side> <padstack> [<net>]   (absolute pin centre)
// Field order after the coordinates is resolved tolerantly: side words
// TOP/BOTTOM/T/B (or 1/2 when two numbers follow), the remaining number is
// the CCW rotation. Package pads are rebuilt from P records via the C
// placement; pad sizes come from the pad-stack names.
// ============================================================================
#include "cad_common.h"
#include <string>

namespace pcbp::io {

int sniff_mentor_neutral(const std::string& path, const std::string& head);
bool import_mentor_neutral_text(const std::string& text, Project& prj, const CadImportOptions& opt,
                                CadImportResult& res, std::string& err);
bool import_mentor_neutral_file(const std::string& path, Project& prj, const CadImportOptions& opt,
                                CadImportResult& res, std::string& err);

} // namespace pcbp::io
