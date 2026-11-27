#pragma once
// ============================================================================
// footprint_canon.h — recover a component's rotation from board-space pads and
// build its footprint in a canonical, axis-aligned package frame.
//
// Sources without component rotation (IPC-D-356 netlists, Gerber digitising) give
// pads in board orientation. Building the package straight from them bakes the
// placement rotation into the package: every placement reads 0 deg, the package is
// off-axis (a 45 deg part has diamond pads), the generated body is an axis-aligned box
// around them, and the same footprint at two angles becomes two packages.
//
// canon_footprint():
//   1. coarse angle  = modal pad rotation mod 90 over non-square pads (IPC-356A field A)
//                      else the principal axis of the pad centres
//   2. fine angle    = median deviation of near-axis pad-pair vectors (pad rotations are
//                      whole degrees; Allegro places at e.g. 44.955 deg)
//   3. quarter turn  = pin 1 nearest the upper-left (>= 3 leads) / left (2 leads) of the
//                      pad centroid (IPC-7351 zero orientation); no pin 1 -> pad order
//   4. origin        = centre of the pad extent in the package frame
//   5. pads          = rotation snapped, 90/270 folded into w/h, rounded (1e-4 mm)
// The placement rotation then carries the board angle (bottom side: comp_xform mirror).
//
// normalise_packages(): the same for an existing project, through pkg_set_origin /
// pkg_rotate with placement compensation (pads on the board do not move); generated
// bodies are rebuilt; identical generated packages are merged.
// ============================================================================
#include "project.h"
#include <string>
#include <vector>

namespace pcbp {

struct BoardPad {        // one pad in board coordinates (top view, mm, deg CCW)
    Vec2 pos;
    double w = 0, h = 0, rot = 0, drill = 0;
    PadShape shape = PadShape::Rect;
    int pin = 0;         // pin number (pkg pad.pin)
    bool pin1 = false;   // this pad is pin 1
};

struct CanonFootprint {
    std::vector<Pad> pads;       // package frame, same order as the input
    double rot = 0;              // placement rotation (deg, [0,360))
    Vec2 pos;                    // placement position (board) = package origin
    double coarse = 0, fine = 0; // diagnostics: angle parts (deg)
    int pin1 = -1;               // index of pin 1 in pads (-1: none)
    std::string how;             // "pad rotation", "pad centres", "none"
};

// side: the placement side. Bottom: the package is built mirrored as comp_xform expects.
CanonFootprint canon_footprint(const std::vector<BoardPad>& pads, Side side);

// Pads equal within tolerance (pin, position, size, rotation mod 180, drill, shape).
bool same_pad_pattern(const std::vector<Pad>& a, const std::vector<Pad>& b, double pos_tol = 0.005, double size_tol = 0.005);

// Canonical pad description: rotation in [0,180), 90 folded into w/h, values rounded.
void canon_pad(Pad& p);

struct NormaliseOptions {
    bool quarter = false;        // also choose the canonical quarter turn (pin 1 upper-left); false = only remove the off-axis part
    bool merge = true;           // merge identical generated packages afterwards
    std::string only_prefix;     // restrict to packages whose name starts with this ("" = all)
    double tol_deg = 0.05;       // residual below this is left alone
    bool only_off_axis = false;  // skip packages pkg_axis_info does not flag (never re-orient a good CAD package)
};
struct NormaliseReport {
    int packages = 0;            // packages rotated / re-centred
    int placements = 0;          // placements compensated
    int bodies = 0;              // generated bodies rebuilt
    int merged = 0;              // duplicate packages removed
    std::vector<std::string> notes;
};
// A package is "generated" when its body was built from the pads (body_source "pads" or empty body)
// and it is not shape_confirmed; only generated packages are merged.
NormaliseReport normalise_packages(Project& prj, const NormaliseOptions& o = {});
// Packages whose name starts with IPC356_ get the full treatment (quarter + merge); others only lose the off-axis part.
NormaliseReport normalise_imported_packages(Project& prj);

} // namespace pcbp
