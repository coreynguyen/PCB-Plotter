#pragma once
// ============================================================================
// package_edit.h — the ONLY sanctioned way to change a shared Package template
// that placements reference, plus placement-instance operations that by
// construction cannot touch a Package.
//
// Template vs instance (specs/PACKAGES_RE.md §5, LEAD_BRIEF §6/§7):
//   * Package edits (rotate artwork+leads, move origin, renumber leads, set
//     pin 1, pin ordering) go through pkg_* below. Each first counts the
//     placements that resolve to the package (Project::package_of, i.e. also
//     via the part number). When N > 0 and opts.confirmed is false, NOTHING is
//     changed and the result carries needs_confirm + the warning text
//     "This changes N placements of PKG (shared)".
//   * Compensation (default ON) keeps every placement's absolute pad
//     positions: rotation -> placement rot adjusted, origin -> placement
//     pos (and label offset / CAD pos) adjusted. This is what CircuitCAM's
//     package-update path does (placement rotation reduced by
//     the package rotation, plus a per-location X/Y shift by the rotated
//     origin offset).
//   * Placement operations take `const Package&` and a `Component&`; they
//     can never mutate a Package. Assign Pin One on a placement ROTATES the
//     placement (never renumbers leads; CircuitCAM's SetNewPinOne does, on the
//     shared package — the bug this design removes).
// ============================================================================
#include "project.h"
#include <functional>
#include <string>
#include <vector>

namespace pcbp {

enum class OrientClass : uint8_t { Unset = 0, Polarized = 1, Ambiguous = 2, Symmetric = 3 };
enum class MarkerKind : uint8_t { None = 0, Dot = 1, Arrow = 2, Custom = 3 };
const char* orient_class_name(OrientClass c);
const char* marker_kind_name(MarkerKind k);

// effective class: explicit field, else derived (polarized flag / pin_marked -> Polarized,
// >2 leads -> Ambiguous when the lead pattern is rotation-symmetric, else Symmetric for 2-lead non-polar)
OrientClass orient_class_of(const Package& p);
// R12 §9: two separate concepts.
//  * Orientation marking needed (pin 1): the part can be placed wrongly rotated, so the operator needs a pin-1 /
//    orientation indicator (QFP, QFN, SOIC, SOT, BGA, LGA, DPAK, diodes, electrolytics, tantalum, LEDs ...).
//    = orient_class_of() is Polarized or Ambiguous.
//  * Electrically polarized (Package::polarized): the part has a + / - or anode / cathode (diodes, electrolytic /
//    tantalum caps, LEDs). Every polarized part needs orientation marking; not every marked part is polarized.
// name hint: 0 = no opinion, 1 = orientation-sensitive family, 2 = electrically polarized family, -1 = known
// non-oriented 2-lead (R / C / L / ferrite / fuse chips)
int pkg_name_orientation_hint(const std::string& name);
bool pkg_orientation_marking_needed(const Package& p, std::string* why = nullptr);
bool pkg_electrically_polarized_guess(const Package& p);   // name / pad-asymmetry guess (for auto flags on import)
// fill orientation fields of a freshly created / imported package (no-op for explicit user choices):
// sets polarized from the name guess; returns true when it changed something
bool pkg_auto_orientation(Package& p);
// marker on-screen radius (px) clamp: consistent, readable look at any zoom
constexpr double kMarkerMinPx = 3.5, kMarkerMaxPx = 9.0;

// pattern symmetry: smallest rotation k*90 (90/180) that maps the lead pattern onto itself; 360 = none
int lead_symmetry_deg(const Package& p, double tol = 0.02);

// pin-1 pad index (pad.pin == 1), -1 if none
int pkg_pin1(const Package& p);
Vec2 pkg_lead_centroid(const Package& p);
Vec2 pkg_body_centre(const Package& p);    // body bbox centre (pads extent when no body)

// marker geometry in package coords: anchor + suggested radius
struct MarkerGeom { bool valid = false; Vec2 at; double size = 0; Vec2 dir{1, 0}; };
MarkerGeom marker_geom(const Package& p);

// ---- R15 polarity marker (specs/POLARITY.md); implemented in project.cpp ----------------------
// One concept: the package has a polarity marker (Dot / Arrow, colour, position, size, arrow rotation) or not.
// Metadata only (silkscreen verification, BOM / validation); machine export is unaffected.
bool pkg_has_polarity(const Package& p);
// R15 body rule: a derived ("pads") body is a suggestion; it may fill an empty body or replace an earlier
// derived one. CAD outline / CAD extent / user-drawn bodies are real data and are never auto-replaced.
bool pkg_body_replaceable(const Package& p);
// default for NEW packages: ICs / >= 3 leads -> yes; 2-terminal chips -> no, except LED / diode / tantalum /
// electrolytic (name, description, radial, unequal pads). returns 1 yes / 0 no; *why = short reason
int pkg_default_polarity_reason(const Package& p, std::string* why);
bool pkg_default_has_polarity(const Package& p);
MarkerGeom pkg_marker_auto(const Package& p);               // near lead 1, else near the first lead
// adds a red dot (or k) at the auto position; NO-OP when a marker already exists (never overwrites)
bool pkg_add_polarity_marker(Package& p, MarkerKind k = MarkerKind::Dot);
void pkg_remove_polarity_marker(Package& p);
// once per package (polarity_set false): apply the class default; returns true when it set polarity_set
bool pkg_apply_polarity_default(Package& p);
int prj_apply_polarity_defaults(Project& prj);
// pre-R15 files: pin-1 mark / polarized flags -> marker at the old automatic position
bool pkg_migrate_polarity_legacy(Package& p);
struct PolarityMarkerOnBoard { bool valid = false; Vec2 at; double rot = 0; };
// marker position / arrow direction in board coordinates for a placement (rotation, bottom-side mirror, image)
PolarityMarkerOnBoard polarity_marker_on_board(const Project& prj, const Component& c, const Package& p, int image = 0);

// ---- off-axis detector ------------------------------------------------------
struct AxisInfo {
    double angle = 0;       // principal axis of lead pattern (deg, 0..180) (or of body when < 2 leads)
    double off90 = 0;       // signed deviation from nearest multiple of 90 (deg, -45..45)
    bool off_axis = false;  // |off90| > tolerance and pattern is anisotropic / edges agree
    std::string why;
};
AxisInfo pkg_axis_info(const Package& p, double tol_deg = 1.0);

// ---- package (template) edits ------------------------------------------------
struct PkgEditOpts {
    bool confirmed = false;     // required when the package has placements
    bool compensate = true;     // keep placements' absolute pads (rot / pos adjusted)
};
struct PkgEditResult {
    bool applied = false;
    bool needs_confirm = false;
    int affected = 0;           // placements that use the package
    int compensated = 0;        // placements adjusted
    std::string message;
};
int count_placements(const Project& prj, const std::string& pkg);
std::vector<int> placements_of(const Project& prj, const std::string& pkg);
std::string shared_warning(const Project& prj, const std::string& pkg);  // "" when unused
// R18c multi-select: one aggregated warning for several packages ("" when none is used by placements).
std::string shared_warning_many(const Project& prj, const std::vector<std::string>& pkgs);
// R18c: SMT / through-hole as detected from the leads (any drilled lead => through-hole).
bool pkg_detect_th(const Package& p);

// pure geometry (no placements; for master-library packages / previews)
void pkg_geom_rotate(Package& p, double deg);              // artwork + leads + marker about package origin
void pkg_geom_offset(Package& p, Vec2 new_origin);         // origin moves to new_origin (package coords)

// rotate artwork+leads by deg (CCW). compensate: placement rot -= deg (top) / += deg (bottom).
PkgEditResult pkg_rotate(Project& prj, const std::string& pkg, double deg, const PkgEditOpts& o);
// move the origin to `new_origin` (current package coords). compensate: pos += L*new_origin.
PkgEditResult pkg_set_origin(Project& prj, const std::string& pkg, Vec2 new_origin, const PkgEditOpts& o);
// renumber: pins[i] = new pin of pad i (must be a permutation of current pins or 1..n)
PkgEditResult pkg_set_pins(Project& prj, const std::string& pkg, const std::vector<int>& pins, const PkgEditOpts& o);
// make pad `pad` pin 1 by cyclic shift of the existing order (keeps CCW sequence; 2-lead = swap)
std::vector<int> pins_cyclic_from(const Package& p, int pad);
// generic package replace (properties, marker, class...) through the same gate
PkgEditResult pkg_replace(Project& prj, const std::string& pkg, const Package& nv, const PkgEditOpts& o);

// transactional pin ordering (CircuitCAM "Pin Ordering Mode"): clicks number pads 1,2,3..;
// nothing is written until commit(); cancel() restores the original numbering.
struct PinOrderSession {
    std::vector<int> original, work;
    std::vector<bool> done;
    int next = 1;
    bool active = false;
    void begin(const Package& p);
    bool click(int pad);                // returns true when every pad has been numbered
    bool complete() const;
    std::vector<int> result() const;    // unclicked pads keep relative order after the clicked ones
    void cancel() { active = false; }
};

// ---- R12 §8: tolerance-aware package grouping --------------------------------------------------
// Two packages are "similar" (same physical package, measured / exported slightly differently) when:
// same pad count, same through-hole / SMT kind, the pads can be paired one-to-one with the same pad
// shape, every pad centre within tol and every pad w / h within tol, and the lead pitch within tol;
// tol(x) = max(abs, rel * x). Orientation-aware (default): pads are compared in each package's own
// 0 deg frame only (a package rotated 90 deg is a different orientation reference -> not merged).
// Not orientation-aware: also tries 90 / 180 / 270 (report only).
// Reference: CircuitCAM Express "Package Tolerance" option (Options page, 0..100 mils, default
// 5.0 mils = 0.127 mm): one absolute tolerance only.
struct PkgMatchTol {
    double abs_mm = 0.05;       // absolute tolerance (mm)
    double rel = 0.05;          // relative tolerance (fraction of the dimension)
    bool orientation_aware = true;
};
// returns true when similar; *rot_deg = rotation of b onto a that matched (0 when orientation-aware),
// *dev = worst deviation found (mm)
bool pkg_similar(const Package& a, const Package& b, const PkgMatchTol& t = {}, double* rot_deg = nullptr, double* dev = nullptr);
bool pkg_kept_separate(const Package& a, const Package& b);     // user override ("keep separate") either way
// best similar existing project package for a new / imported package (not itself, not kept separate); "" if none
std::string pkg_find_similar(const Project& prj, const Package& p, const PkgMatchTol& t = {}, const std::string& self = "");
struct SimilarGroup { std::string keep; std::vector<std::string> merge; double worst_dev = 0; };
// candidate groups for "Merge similar packages...": keep = the most-used (then shortest name) package
std::vector<SimilarGroup> pkg_similar_groups(const Project& prj, const PkgMatchTol& t = {});
// re-point placements + part numbers of `from` packages to `into` and remove them; returns placements moved.
// Placement rotation / position are compensated when the matched packages differ only in origin (pads
// are within tolerance, so no compensation is needed beyond that). Undo = caller's checkpoint.
int pkg_merge(Project& prj, const std::string& into, const std::vector<std::string>& from);
void pkg_keep_separate(Project& prj, const std::string& a, const std::string& b);

// ---- placement (instance) operations: never take a mutable Package ----------
// rotate the placement by delta in its LOCAL frame (top: rot += d, bottom: rot -= d), keeps centroid
void placement_rotate_local(Component& c, double delta_local);
// Assign Pin One on a placement: rotate it so pin 1 lands on the clicked pad's board position.
// Snaps to a rotation mapping the lead pattern onto itself (k*90; 2-lead parts -> 180).
// Returns the local delta applied (0 when clicked pad is pin 1) or NaN when no symmetric rotation
// maps pin 1 onto the clicked pad (message set).
double placement_assign_pin_one(Component& c, const Package& p, int clicked_pad, std::string* msg = nullptr);

} // namespace pcbp
