#pragma once
// ============================================================================
// eagle_brd.h — Autodesk/CadSoft EAGLE board (.brd, XML, EAGLE 6..9) import
// (pcbp::io), portable.
//
// libraries/library/packages/package: <smd> (dx, dy, rot, roundness) and
// <pad> (drill, diameter (auto when 0), shape round|square|octagon|long|
// offset); elements: name, library, package, value, x, y, rot ("R90",
// "MR180": M = mirrored = bottom, S = spin ignored), <attribute> MPN-like ->
// part number. plain wires (incl. curve="deg" arcs), circles, rectangles
// and polygons on layer 20 (Dimension) -> board outline. Units: mm.
// Legacy binary .brd (EAGLE <= 5) is not supported.
// ============================================================================
#include "cad_common.h"
#include <string>

namespace pcbp::io {

int sniff_eagle_brd(const std::string& path, const std::string& head);
bool import_eagle_brd_text(const std::string& text, Project& prj, const CadImportOptions& opt,
                           CadImportResult& res, std::string& err);
bool import_eagle_brd_file(const std::string& path, Project& prj, const CadImportOptions& opt,
                           CadImportResult& res, std::string& err);

} // namespace pcbp::io
