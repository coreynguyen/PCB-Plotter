#pragma once
// ============================================================================
// arrays.h — R22 (REVIEW_V21 §1-§3): the one API for outlines and circuit arrays.
// Portable, no UI. Used by the Plot view Board menu dialogs AND the Gerber viewer.
//
// Array model (specs/PANELIZE.md §0): ONE reference circuit (Project::comps, the circuit
// outline Project::board_frame, packages, part numbers, properties) plus linked instances
// (Project::images[1..], each = offset + rotation, p' = R(rot) * p + offset). Instances hold no
// placement / package / outline data of their own: everything is read from the reference through
// Project::instance_xform / circuit_frame_at, so an edit to the reference propagates to every
// instance. images[0] is always the reference (root, identity transform).
//
// Outlines (specs/OUTLINES.md): Board outline = Project::panel_frame (whole PCB / panel on the
// conveyor, panel coordinates, never instanced). Circuit outline = Project::board_frame (the
// reference circuit's boundary, reference coordinates, instanced on every array instance).
// Nothing in this API fabricates an outline from placements or margins.
// ============================================================================
#include "project.h"
#include <string>
#include <vector>

namespace pcbp {

enum class OutlineKind : uint8_t { Board = 0, Circuit = 1 };

// --- units (dialog inputs: mm / in / mil) ---------------------------------------------------
double units_to_mm(double v, Units u);     // MM 1, Inch 25.4, Mil 0.0254
double mm_to_units(double mm, Units u);

// --- outlines --------------------------------------------------------------------------------
// true when an outline of this kind exists (any provenance). The CALLER asks for replace
// confirmation when this is true, then calls commit_outline.
bool outline_exists(const Project& p, OutlineKind k);
// Commits vector geometry as the outline (replaces any existing one of that kind). polys are
// closed or open polylines in plot (reference / panel) coordinates, mm; open chains are joined
// (join_frame_segments). Circuit outlines are given in REFERENCE-circuit coordinates (pick them on
// the reference circuit). src = Confirmed (typed / user-drawn) or Imported (taken from Gerber
// geometry); Inferred is rejected. Returns false (project unchanged) when the geometry is empty
// or has no area. Sets panel_frame_from_gerber / board_frame_from_gerber = from_gerber.
bool commit_outline(Project& p, OutlineKind k, const std::vector<std::vector<Vec2>>& polys,
                    OutlineSource src = OutlineSource::Confirmed, bool from_gerber = false, std::string* err = nullptr);
bool commit_outline_rect(Project& p, OutlineKind k, const BBox& r, OutlineSource src = OutlineSource::Confirmed,
                         bool from_gerber = false, std::string* err = nullptr);
// Deletes the outline of that kind (Board -> panel_frame, Circuit -> board_frame). Returns false
// when there was none.
bool delete_outline(Project& p, OutlineKind k);
// "Confirm as measured": an Inferred circuit outline becomes Confirmed with its current geometry
// (explicit user action only). Returns false when there is no inferred outline.
bool confirm_inferred_outline(Project& p);

// --- arrays ----------------------------------------------------------------------------------
// One linked instance of the reference circuit: p' = R(rot) * p + offset (rot deg CCW).
struct ArrayInstance {
    Vec2 offset;            // mm
    double rot = 0;         // deg CCW (any angle; 0/90/180/270 typical)
    bool x_out = false;     // skipped / bad circuit
    int x_index = 0, y_index = 0;   // grid cell (informational; nonlinear: auto when both 0)
};
bool has_array(const Project& p);          // images.size() > 1
int array_instance_count(const Project& p); // including the reference (>= 1)

// Linear (step & repeat) array: nx * ny circuits, pitch in mm (signed). The reference circuit
// sits in cell (root_col, root_row) (0-based, the "origin" cell); every other cell gets offset
// ((col - root_col) * pitch_x, (row - root_row) * pitch_y), rotation 0. Replaces any existing
// array; never creates or changes an outline. err: reason on failure (project unchanged).
bool create_linear_array(Project& p, int nx, int ny, double pitch_x, double pitch_y,
                         int root_col = 0, int root_row = 0, std::string* err = nullptr);
// Nonlinear array from an instance list. `instances` are the NON-reference instances (the
// reference itself is implicit, identity); an entry equal to identity (offset ~0, rot ~0) is
// dropped. Replaces any existing array; never creates or changes an outline.
bool create_nonlinear_array(Project& p, const std::vector<ArrayInstance>& instances, std::string* err = nullptr);
// Removes every instance; keeps the reference circuit (placements, circuit outline) untouched.
// The board outline is kept too (it is physical data the user entered). Returns false when there
// was no array.
bool delete_array(Project& p);

// --- migration (projects saved before R22, project_io: no "arrays_model" key) ----------------
// Fabricated outlines from older versions are demoted, never silently trusted:
//  * a board outline that is exactly the union of the circuit extents plus a uniform margin (the retired
//    "Create board outline around all circuits" / "rough board outline" generators) -> Inferred;
//  * on an array, a circuit outline identical to the board outline (old machine-program import fallback:
//    every instance would then be the whole board) -> removed (circuit outline unknown).
// Returns a note per change (empty = nothing changed).
std::vector<std::string> migrate_array_outlines(Project& p);
// true when the board outline is a single axis-aligned rect equal to the union of every instance's circuit
// extent (circuit outline, else placement extents + 2 mm) grown by one uniform margin in [0, 50] mm.
bool panel_frame_is_generated(const Project& p);

// --- validation (REVIEW_V21 §2.3) ------------------------------------------------------------
// Re-exported from plot_align.h: check_outlines(p) checks each instance's placements against
// ITS OWN transformed circuit outline, every transformed circuit outline against the board
// outline, and instance overlap. It never compares an instance against the reference's
// untransformed outline.

} // namespace pcbp
