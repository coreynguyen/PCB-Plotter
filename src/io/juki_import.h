#pragma once
// ============================================================================
// juki_import.h — JUKI machine program -> PCB Plotter Project ("reverse").
//
// Reads binary HLC programs (H8H incl. 26/27/28-block variants, H7H, H5H,
// H4H HLC5/6, E41..E46, E51 — layouts in specs/JUKI_EXPORT.md) and JUKI ISS
// XML (productionProgram) and rebuilds an assembly:
//   board size        -> board_frame / panel_frame rectangle at the origin corner
//   circuits          -> PanelImage list (step & repeat) when placements repeat
//                        consistently, else every circuit as suffixed components
//   placements        -> Components (ref, x/y 1e-4 mm, angle 1/80 deg, skip -> dnp)
//   component records -> PartNumber + reconstructed Package (body from the
//                        record's length/width/height when present)
//   marks             -> fiducial Components (global + local pairs)
//   mode/origin       -> MachineSetup options (transport, fixed rail, board
//                        corner, circuit object, timestamp, comment, thickness)
// and a model point "Top SMT" whose exporter regenerates the program.
// Round-trip goal: program written by juki_writer -> import -> export gives
// the same placement records.
// ============================================================================
#include "core/project.h"
#include <cstdint>
#include <string>
#include <vector>

namespace pcbp::juki {

struct ImportOptions {
    std::string point_name = "Top SMT";
    bool reconstruct_packages = true;   // false: parts get no package
    double step_tol_mm = 0.0002;        // step & repeat consistency tolerance
};

struct ImportResult {
    std::string format;                 // "H8H", "H7H", "E41", "ISS", ...
    std::string exporter_id;            // matching export plugin id
    int placements = 0, components = 0, parts = 0, packages = 0;
    int fiducials = 0, images = 1;
    bool step_repeat = false;           // circuits folded into panel images
    std::vector<std::string> warnings;
};

// 0..100 likelihood that `data` is a JUKI program (binary HLC or ISS XML).
int sniff_juki(const std::vector<uint8_t>& head);

bool import_hlc(const std::vector<uint8_t>& data, Project& prj, const ImportOptions& opt,
                ImportResult& res, std::string& err);
bool import_iss(const std::string& xml, Project& prj, const ImportOptions& opt,
                ImportResult& res, std::string& err);
// Dispatch on content (XML vs binary).
bool import_juki(const std::vector<uint8_t>& data, Project& prj, const ImportOptions& opt,
                 ImportResult& res, std::string& err);

} // namespace pcbp::juki
