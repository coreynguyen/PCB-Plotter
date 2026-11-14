#pragma once
// ============================================================================
// dxf_outline.h — ASCII DXF board-outline import (pcbp::io), portable.
//
// ENTITIES: LINE, ARC, CIRCLE, LWPOLYLINE (bulges, closed flag), POLYLINE +
// VERTEX + SEQEND (bulges), SPLINE (control/fit points as a polyline). Only
// entities on `opt.layer` (comma-separated, case-insensitive; "" = all) are
// used. BLOCK/INSERT are not expanded. Mirrored OCS (210/220/230 extrusion
// with Z < 0) is handled for the 2D entities.
// Units from HEADER $INSUNITS (1 in, 2 ft, 4 mm, 5 cm, 6 m, 8 uin, 9 mil,
// 13 um, 14 dm; 0/absent -> mm unless opt.units overrides).
// Result: chained closed/open polylines replace (opt.replace) or are added to
// Project::board_frame. Components and packages are untouched.
// Binary DXF is not supported.
// ============================================================================
#include "cad_common.h"
#include <string>
#include <vector>

namespace pcbp::io {

int sniff_dxf(const std::string& path, const std::string& head);
std::vector<std::string> dxf_layers(const std::string& text);    // layers that carry entities
bool import_dxf_outline_text(const std::string& text, Project& prj, const CadImportOptions& opt,
                             CadImportResult& res, std::string& err);
bool import_dxf_outline_file(const std::string& path, Project& prj, const CadImportOptions& opt,
                             CadImportResult& res, std::string& err);

} // namespace pcbp::io
