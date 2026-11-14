#pragma once
// ============================================================================
// transform.h — R16c (REVIEW_V15 §9): one coordinate Transform with
// independently enabled sections (portable, no UI; Board > Align / Transform).
//
//   T = Tr(dx,dy) * Tr(pivot) * R(angle) * M * S(scale) * Tr(-pivot)
//
// applied in the order Scale -> Mirror -> Rotate (all about the pivot) ->
// Translate. Mirror X flips left/right (x -> 2*pivot.x - x), Mirror Y flips
// top/bottom. Placement angles follow the geometry: rotate adds the angle,
// a mirror reflects the part direction (X: 180 - rot, Y: -rot). With
// flip_side the mirror is a board flip instead: parts change side and take
// the bottom-view angle convention (same rule as Align: rot' = drot - rot).
// Scale moves positions only (package sizes stay; a unit fix is Rescale).
// ============================================================================
#include "project.h"

namespace pcbp {

struct TransformSpec {
    bool translate = false; double dx = 0, dy = 0;
    bool rotate = false;    double angle = 0;          // deg, CCW
    bool scale = false;     double sx = 1, sy = 1;     // sy ignored when uniform
    bool uniform = true;
    bool mirror = false;    int mirror_axis = 0;       // 0 = Mirror X (left <-> right), 1 = Mirror Y (top <-> bottom)
    bool flip_side = false;                            // mirror = turn the board over (parts change side)
    Vec2 pivot;                                        // rotate / mirror / scale centre
    int sides = 0;                                     // 0 both, 1 top only, 2 bottom only
    bool board_outline = true;                         // Board Outline (panel)
    bool circuit_outline = true;                       // Circuit Outline(s) + panel instances
    bool move_origin = true;                           // virtual origin moves with the data
    bool any() const { return translate || rotate || scale || mirror; }
};

Xform transform_xform(const TransformSpec& s);
// Placement update for one component (position, angle, label, side).
void transform_component(Component& c, const Xform& T, bool flip_side);
// Applies the spec to the project; returns the number of placements changed. Not undo-aware (callers checkpoint).
int apply_transform(Project& p, const TransformSpec& s);
// Angle of the direction (cos r, sin r) after the linear part of T (deg, [0,360)).
double transformed_angle(const Xform& T, double rot_deg);

} // namespace pcbp
