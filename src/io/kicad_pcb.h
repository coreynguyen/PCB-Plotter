#pragma once
// ============================================================================
// kicad_pcb.h — KiCad board (.kicad_pcb, s-expression; KiCad 4..9) import
// (pcbp::io), portable.
//
// footprint / module blocks: (at x y rot), (layer F.Cu|B.Cu), reference and
// value from fp_text or property, part number from MPN-like properties,
// pads (smd / thru_hole; np_thru_hole skipped) with shapes rect / circle /
// oval / roundrect / trapezoid / custom, (size), (at x y angle) where the
// pad angle is absolute (footprint rotation included), (drill d | oval w h).
// Edge.Cuts gr_line / gr_arc (start-mid-end, or legacy centre+angle) /
// gr_rect / gr_circle / gr_poly -> board outline.
// KiCad's Y axis points down: Y is negated; angles are already CCW as seen.
// Bottom footprints: pad geometry is reconstructed from absolute pad
// positions so it matches Project::comp_xform (rotate, mirror local X).
// ============================================================================
#include "cad_common.h"
#include <string>

namespace pcbp::io {

int sniff_kicad_pcb(const std::string& path, const std::string& head);
bool import_kicad_pcb_text(const std::string& text, Project& prj, const CadImportOptions& opt,
                           CadImportResult& res, std::string& err);
bool import_kicad_pcb_file(const std::string& path, Project& prj, const CadImportOptions& opt,
                           CadImportResult& res, std::string& err);

} // namespace pcbp::io
