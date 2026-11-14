#pragma once
// ============================================================================
// pads_ascii.h — PADS Layout / PowerPCB ASCII (.asc) import (pcbp::io).
//
// Header "!PADS-POWERPCB-Vx.y-<UNITS>!" with MILS | METRIC | INCHES | BASIC.
// *PARTDECAL*: decal header lines, drawing pieces (skipped by count),
//   terminals "T<x> <y> <nmx> <nmy> [<pin>]", pad stacks
//   "PAD <pin|0> <nlayers> [P|N <drill>]" + layer lines
//   "<layer> <size> <shape R|S|A|OF|RF|..> [<ori> <len> <offset>]";
//   layer -2 (top) wins. Decal coordinates are taken in file units.
// *PARTTYPE*: "<type> <decal>[:<alt>...]" maps part types to decals.
// *PART*: "<ref> <type>[@<decal>] <x> <y> <ori> <glue> <M|N> ... <nlabels>"
//   (M = mirrored = bottom; 2 label lines per label skipped).
// *LINES*: items of type BOARD -> outline pieces; arc corners
//   "x y ab aa ax1 ay1 ax2 ay2" (tenths of a degree, bbox of the circle).
// ============================================================================
#include "cad_common.h"
#include <string>

namespace pcbp::io {

int sniff_pads_ascii(const std::string& path, const std::string& head);
bool import_pads_ascii_text(const std::string& text, Project& prj, const CadImportOptions& opt,
                            CadImportResult& res, std::string& err);
bool import_pads_ascii_file(const std::string& path, Project& prj, const CadImportOptions& opt,
                            CadImportResult& res, std::string& err);

} // namespace pcbp::io
