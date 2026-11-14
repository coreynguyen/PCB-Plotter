#pragma once
// ============================================================================
// allegro_brd.h — Cadence Allegro binary board database (.brd) import
// (pcbp::io), portable, clean-room (see specs/ALLEGRO_BRD.md).
//
// Supported: database magic 0x0014xxxx (Allegro 17.x, layout verified on
// 0x00140400). Other versions fail with a clear "unsupported" message.
// Decoded: placed symbols (refdes, footprint name, device type, x/y,
// rotation, side), symbol pins (name, local position, pad rotation, pad size
// from the placed pad's extent, pad-stack name -> drill guess) and the board
// outline (BOARD GEOMETRY design outline / outline shape + cutouts).
// Every count, length and pointer is bounded; never reads past the buffer.
// ============================================================================
#include "cad_common.h"
#include <string>

namespace pcbp::io {

int sniff_allegro_brd(const std::string& path, const std::string& head);
bool import_allegro_brd_data(const std::string& data, Project& prj, const CadImportOptions& opt,
                             CadImportResult& res, std::string& err);
bool import_allegro_brd_file(const std::string& path, Project& prj, const CadImportOptions& opt,
                             CadImportResult& res, std::string& err);

} // namespace pcbp::io
