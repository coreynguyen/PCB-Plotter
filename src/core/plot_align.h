#pragma once
// ============================================================================
// plot_align.h — R14: plot <-> Gerber alignment and Gerber-driven panel helpers
// (portable, no UI). See specs/PANELIZE.md.
//
// Coordinate spaces
//   plot   : project coordinates (Component::pos, frames, PanelImage offsets)
//   gerber : Gerber world = layer_xform(layer) * file coordinates
// An alignment T maps plot -> gerber world:  g = T(p) = Tr(o) * R(rot) * [S] (p)
// where S = scale(-1, 1) when mirrored.
// ============================================================================
#include "project.h"
#include <vector>

namespace pcbp {

struct AlignFit {
    Xform T;                // plot -> gerber world
    double rot = 0;         // deg (CCW), applied after the optional mirror
    bool mirror = false;
    double rms = 0;         // pad residual after the fit (mm)
    int matched = 0;        // pad pairs used by the fit
};

Vec2 centroid(const std::vector<Vec2>& v);
Xform align_xform(Vec2 cad_c, Vec2 ger_c, double rot, bool mirror);   // T with T(cad_c) = ger_c

// Fits the reference component's CAD pads (plot coords) to the Gerber pads the user rectangle-selected.
// Starts from the centroid match with rotation rot_hint (+ mirror), then iterates nearest-neighbour
// correspondences. refine_rot: also solve an arbitrary rotation (least squares), else rotation stays rot_hint.
bool fit_cad_to_gerber(const std::vector<Vec2>& cad, const std::vector<Vec2>& ger, double rot_hint, bool mirror,
                       bool refine_rot, AlignFit& out);
// Tries 0/90/180/270 (and, when try_mirror, mirrored) and returns the fit with the lowest residual.
bool detect_cad_to_gerber(const std::vector<Vec2>& cad, const std::vector<Vec2>& ger, bool try_mirror, bool refine_rot, AlignFit& out);

// ---- R17a alignment session (REVIEW_V16 §1) ----------------------------------------------------
// Workflow: pick a placement (any panel instance) -> box its pads on a Gerber layer -> preview -> Apply.
// Selection centre = BOUNDING-BOX centre of the selected pad centres (not their mean): it equals the
// footprint centroid for asymmetric pad counts (SOT-23: 2 pads on one side, 1 on the other) and does
// not depend on how many pads were boxed on each side. It is matched to the placement centroid.
// A centroid-only placement (package without pads) is ONE point: it is never matched pad-by-pad
// (that anchored the plot to the single nearest pad, R16 bug). With package pads, the pads are fitted.
// One reference gives the offset; rotation / mirror come from the user (0/90/180/270, Mirror) or, with
// two or more references, from the references themselves (rot_mode 4 = "From references").
Vec2 pad_selection_centre(const std::vector<Vec2>& pads);
// Same, with the box taken along the part's axes (angle deg of the part's +X on the Gerber): a part at an
// arbitrary angle (not a multiple of 90) is boxed in its own frame. solve_alignment refines with it.
Vec2 pad_selection_centre(const std::vector<Vec2>& pads, double axis_deg);
struct AlignRef {
    int comp = -1, image = 0;               // placement (index into comps) and panel instance
    Vec2 plot;                              // placement centroid of that instance (plot coords)
    std::vector<Vec2> cad_pads;             // package pads of that instance (plot); empty = unresolved package
    std::vector<Vec2> ger_pads;             // selected Gerber pad centres (Gerber world)
    Vec2 ger_centre;                        // pad_selection_centre(ger_pads[, part axis on the Gerber])
    Vec2 xdir{1, 0};                        // the placement's +X axis in plot coords (rotation, side, instance)
    double residual = 0;                    // |T(plot) - ger_centre| after solve_alignment (mm)
};
AlignRef make_align_ref(const Project& p, int comp, int image, const std::vector<Vec2>& ger_pads);
// rot_mode 0..3 = 0/90/180/270 fixed, 4 = from the references (>= 2 refs, or package pads). mirror is used as given.
// Fills out (T plot -> gerber) and every ref's residual. why = reason when it fails or is ambiguous.
bool solve_alignment(std::vector<AlignRef>& refs, int rot_mode, bool mirror, AlignFit& out, std::string* why = nullptr);
// Which panel instance of comp is nearest to world point w (0 when not instanced / single circuit).
int nearest_instance(const Project& p, const Component& c, Vec2 w);
// Placement preview after T: one entry per placement instance (centroid + local axes), for ghost drawing.
struct GhostPlacement { int comp = -1, image = 0; Vec2 pos; Vec2 xdir; bool bottom = false; std::vector<Vec2> pads; };
std::vector<GhostPlacement> preview_placements(const Project& p, const Xform& T, bool with_pads = true);
// Global Gerber rotate / mirror (before alignment): every layer (and Gerber-built elements) about `about`.
void transform_gerbers(Project& p, double rot, bool mirror, Vec2 about);

// Splits a rigid (optionally mirrored) transform into Gerber-layer parameters
// t = Tr(off) * R(rot) * [scale(-1,1) if mirror]  (the GerberLayerRef convention).
void decompose_xform(const Xform& t, Vec2& off, double& rot, bool& mirror);
Xform layer_ref_xform(const GerberLayerRef& r);
// layer' = pre * layer (e.g. pre = T^-1 to bring a Gerber layer onto the plot).
void premultiply_layer(GerberLayerRef& r, const Xform& pre);

// Moves plot elements by T (plot -> gerber). Elements marked from_gerber (components and frames)
// are already in Gerber coordinates and stay put unless include_gerber_built. Panel image transforms
// are conjugated so the whole panel moves rigidly; the virtual origin moves with the plot.
void move_plot(Project& p, const Xform& T, bool include_gerber_built = false);
// Moves only the from_gerber elements by M (used with "Move Gerbers to plot": they follow the Gerbers).
void move_gerber_built(Project& p, const Xform& M);

// ---- panel helpers ---------------------------------------------------------------------------
// Pitch from three clicked points: reference circuit, same point one circuit over in X, one up in Y.
// Returns the pitch (dx from x_next, dy from y_next). skew = the off-axis components (mm) for warnings.
struct PitchPick { double dx = 0, dy = 0; Vec2 skew; };
PitchPick pitch_from_points(Vec2 ref, Vec2 x_next, Vec2 y_next, bool has_x, bool has_y);
// Suggests counts and the root (reference circuit) cell from the circuit extent and the Gerber extent.
struct GridSuggest { int nx = 1, ny = 1, root_col = 0, root_row = 0; };
GridSuggest suggest_grid(const BBox& circuit, const BBox& gerber, double dx, double dy, double tol = 0.5);
// Step & Repeat: nx * ny instances, root at (root_col, root_row). Offsets (col-root_col)*dx, (row-root_row)*dy.
std::vector<PanelImage> step_repeat_grid(int nx, int ny, double dx, double dy, int root_col = 0, int root_row = 0);
// One rotated / moved instance from point pairs: p1,p2 on the reference circuit (plot), q1,q2 the same
// features on the instance. snap90 rounds the angle to 90 deg. With only one pair (has_second=false) the
// rotation is taken as rot_hint. The image maps p -> R(rot) p + offset (Project::image_xform).
PanelImage instance_from_points(Vec2 p1, Vec2 q1, Vec2 p2, Vec2 q2, bool has_second, double rot_hint, bool snap90);
// Adds an instance; returns its index. x_index / y_index are placed after the existing grid.
int add_instance(Project& p, const PanelImage& im);

// ---- R17a: flattened panels (REVIEW_V16 §14) ----------------------------------------------------
// A panelised centroid / CAD export repeats every reference once per circuit (U1, U1, ...). When every
// instanced reference occurs exactly N >= 2 times and the n-th occurrences form a rigid copy (rotation
// allowed, no mirror) of the first ones, the panel can be folded into N circuit instances: the first
// occurrence of each reference stays, the others become Project::images. Global fiducials are kept as is.
struct InstanceFold {
    int n = 1;
    std::vector<PanelImage> images;             // images[0] = root
    std::vector<std::vector<int>> members;      // members[k] = comp indices of circuit k (same order for every k)
    double max_dev = 0;                         // worst position residual (mm)
    std::string why;                            // reason when not foldable
};
bool detect_repeated_instances(const Project& p, InstanceFold& out, double tol = 0.05);
int fold_repeated_instances(Project& p, const InstanceFold& f);   // returns placements removed (now instances)

// ---- physical outline checks (specs/PANELIZE.md §6) ----------------------------------------
// Board outline (panel) = the physical board on the conveyor; circuit outline = one circuit's boundary.
// Checks: every placement inside its circuit outline (all instances), every circuit outline inside the
// board outline, circuit outlines do not overlap, board size within max_w x max_h (0 = unknown).
struct FrameIssue { bool error = false; std::string ref, msg; };
std::vector<FrameIssue> check_outlines(const Project& p, double max_w = 0, double max_h = 0);
bool poly_contains(const std::vector<Vec2>& poly, Vec2 q);          // implicit closing edge
bool polys_overlap(const std::vector<Vec2>& a, const std::vector<Vec2>& b, double tol = 0.01);   // touching is not overlap
// Rectangle from two corners given as offsets from a reference point (signed: +X right, +Y up).
BBox rect_from_ref_offsets(Vec2 ref, Vec2 top_left_off, Vec2 bottom_right_off);

} // namespace pcbp
