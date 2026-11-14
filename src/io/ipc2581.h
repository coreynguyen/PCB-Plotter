#pragma once
// ============================================================================
// ipc2581.h — IPC-2581 (DPMX, rev A/B/C) import (pcbp::io), portable.
//
// Reads Content/DictionaryStandard (pad primitives), Bom (BomItem
// OEMDesignNumberRef -> part number, Textual VALUE -> value),
// Ecad/CadHeader units, CadData/Layer sides, Step (first or opt.step):
// Profile (Polygon PolyBegin/PolyStepSegment/PolyStepCurve + Cutout),
// PadStackDef holes, Package (Pin + LandPattern pads, height), Component
// (refDes, packageRef, layerRef, part, Xform rotation/mirror, Location),
// LayerFeature drill holes (matched to pins) and Global/Local fiducials.
// StepRepeat is ignored. Rotations are CCW; mirror or bottom layer = bottom.
// ============================================================================
#include "cad_common.h"
#include <string>

namespace pcbp::io {

int sniff_ipc2581(const std::string& path, const std::string& head);
bool import_ipc2581_text(const std::string& text, Project& prj, const CadImportOptions& opt,
                         CadImportResult& res, std::string& err);
bool import_ipc2581_file(const std::string& path, Project& prj, const CadImportOptions& opt,
                         CadImportResult& res, std::string& err);

} // namespace pcbp::io
