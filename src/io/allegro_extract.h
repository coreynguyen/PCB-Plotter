#pragma once
// ============================================================================
// allegro_extract.h — Cadence Allegro "extracta" text report import
// (pcbp::io), portable.
//
// Line kinds: "J!" job line (design units: MILS / MILLIMETERS / INCHES /
// MICRONS / CENTIMETER), "A!" column headers, "S!" data rows ('!'
// delimiter; trailing '!' allowed). A pipe-delimited variant without the
// A!/S! prefixes ("REFDES|SYM_X|...") is accepted too; several header
// blocks may appear in one file.
// Recognised blocks (by columns):
//   component: REFDES SYM_X SYM_Y SYM_ROTATE SYM_MIRROR SYM_NAME
//              [COMP_DEVICE_TYPE COMP_VALUE COMP_PART_NUMBER]
//   pin:       REFDES PIN_NUMBER PIN_X PIN_Y [PAD_STACK_NAME DRILL_SIZE ...]
//              (absolute pin positions -> package pads via the symbol
//              placement; pad size guessed from the pad-stack name)
//   graphic:   CLASS SUBCLASS GRAPHIC_DATA_NAME GRAPHIC_DATA_1..9 —
//              BOARD GEOMETRY / OUTLINE|DESIGN_OUTLINE LINE/ARC -> outline
// Rotation CCW; SYM_MIRROR YES = bottom.
// ============================================================================
#include "cad_common.h"
#include <string>

namespace pcbp::io {

int sniff_allegro_extract(const std::string& path, const std::string& head);
bool import_allegro_extract_text(const std::string& text, Project& prj, const CadImportOptions& opt,
                                 CadImportResult& res, std::string& err);
bool import_allegro_extract_file(const std::string& path, Project& prj, const CadImportOptions& opt,
                                 CadImportResult& res, std::string& err);

} // namespace pcbp::io
