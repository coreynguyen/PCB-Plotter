#pragma once
// ============================================================================
// panel_auto.h — panelize from pallet (panel) Gerbers without UI (LLM_API §3A.4 G2).
//
// Input: a project with a single-circuit CAD (placements + packages) and the panel's
// Gerber layers. The CAD root's pads (top or bottom copper / paste side, quarter-turn
// components) are injected as the reference group of gmatch::find_pattern, shifted far
// away so no Gerber copy is mistaken for the reference itself. Every candidate is then a
// CAD -> panel-instance transform (0/90/180/270 + offset), found in one pass, with no
// prior alignment or picked reference group (the GUI's Detect Repeated Circuits needs
// both). Symmetric matches at one place are resolved by matched count / residual;
// only true ties are reported as ambiguous (pipeline gate panel.ambiguous).
//
// apply(): the panel layers are moved onto the CAD (the CAD keeps its coordinates), the
// lowest-left instance becomes the reference circuit, and the others become a linear
// array when they form an exact unrotated grid, else a nonlinear array.
// ============================================================================
#include "project.h"
#include "geom.h"
#include <string>
#include <vector>

namespace pcbp::panel_auto {

struct Options {
    int match_layer = -1;           // Gerber layer to match (-1: top copper, then top paste, then any copper/paste)
    double tol = 0.05;              // position tolerance (mm)
    double size_tol = 0.03;         // absolute size tolerance (mm) ...
    double size_rel = 0.05;         // ... plus this fraction of the size (copper pads ~ CAD pads)
    double min_fraction = 0.85;     // reference pads that must match per instance
    bool rot[4] = {true, true, true, true};
    int max_ref = 400;              // reference pads used (spread sample when the circuit has more)
    bool allow_linear = true;
};

struct Instance {
    double rot = 0;                 // CAD -> instance: p' = R(rot) p + offset (world of the match layer)
    Vec2 offset;
    Vec2 centre;                    // CAD circuit centre on the instance
    BBox box;
    int matched = 0, total = 0;
    double rms = 0;
    bool ambiguous = false;         // a different rotation matches equally well at the same place
    std::vector<double> alt_rots;   // the other rotations of an ambiguous instance
};

struct Detection {
    bool ok = false;
    std::string why;                // reason when !ok
    int match_layer = -1;
    Side side = Side::Top;
    int ref_pads = 0, ref_comps = 0;
    std::vector<Instance> found;    // every panel instance (the future root included), bottom-to-top, left-to-right
    int root = -1;                  // index into found: lowest-left unambiguous instance
    int ambiguous = 0;
    // result as the project will hold it (CAD frame; the reference circuit is the root instance)
    bool linear = false;
    int nx = 0, ny = 0, root_col = 0, root_row = 0;
    double dx = 0, dy = 0;
    std::vector<std::pair<Vec2, double>> images;   // non-root instances: offset, rot (CAD frame)
    BBox panel_box;                 // union of the instance boxes (CAD frame)
    std::string summary() const;
};

// Detect. Does not modify the project.
Detection detect(const Project& p, const Options& o = {});

struct ApplyOptions {
    std::vector<int> move_layers;   // layers moved onto the CAD (default: every layer)
    bool replace_array = false;     // a panel array already exists -> refuse unless set
    bool include_ambiguous = false; // ambiguous instances: false = left out (reported), true = best rotation
    std::vector<int> exclude;       // indices into Detection::found to leave out (e.g. coupons)
};
// Moves the panel layers by the inverse root transform and creates the array.
bool apply(Project& p, const Detection& d, const ApplyOptions& a, std::string& err);

} // namespace pcbp::panel_auto
