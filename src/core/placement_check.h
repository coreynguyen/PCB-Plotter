#pragma once
// ============================================================================
// placement_check.h — placement review for a pcbp::Project (portable).
//
// Checks: duplicate refdes (case-insensitive), empty / illegal-character
// refs, coincident and near-coincident placements (same side), overlapping
// component bodies (package extents, rotated, same side; SAT on oriented
// boxes), components outside the board frame, missing package / part number,
// package or part number not in library, packages with no pads, BOM package
// != location package, unassigned components, global fiducials < 2 or
// collinear, rotation not a multiple of 90 (info), DNP but assigned.
// auto_fix helpers handle the mechanical ones.
// ============================================================================
#include "project.h"
#include "package_name.h"   // package_key / packages_equivalent (BOM <-> board names)
#include <string>
#include <vector>

namespace pcbp {

enum class Severity : uint8_t { Info, Warning, Error };
const char* severity_name(Severity s);

enum class CheckCode : uint8_t {
    DuplicateRef, EmptyRef, IllegalRefChars,
    Coincident, NearCoincident, Overlap,
    OutsideBoard, PartlyOutsideBoard, NoBoardFrame,
    MissingPackage, PackageNotInLibrary, ZeroPadPackage,
    MissingPartNumber, PartNotInLibrary,
    PartPackageMismatch,    // Info since R10: part LIBRARY package differs; no BOM value confirms it (inferred, not a mismatch)
    Unassigned, DnpAssigned,
    FewFiducials, CollinearFiducials,
    OffAxisRotation,
    OffAxisPackage,         // package artwork / leads rotated off-axis (core/package_edit pkg_axis_info)
    // ---- R10 BOM reconciliation (REVIEW_V9 §11F/G) ----
    BomPackageDisagree,     // Warning: BOM supplied a package value and it differs from the location's package
    OrphanNotInBom,         // Warning: on the board, missing from the imported BOM (never auto-DNP)
    DnpInBom                // Info: explicitly DNP in the BOM
};
// Short badge for review lists ("PKG≠", "ORPH", "DNP" ...).
const char* check_code_badge(CheckCode c);
const char* check_code_name(CheckCode c);

struct PlacementIssue {
    Severity severity = Severity::Warning;
    CheckCode code = CheckCode::DuplicateRef;
    std::string message;
    std::vector<int> comps;         // indices into Project::comps
};

struct PlacementCheckOptions {
    double coincident_tol = 0.01;   // mm: same XY
    double near_tol = 0.25;         // mm: near-coincident
    bool check_overlap = true;
    double overlap_shrink = 0.05;   // mm shrunk off every body edge (touching is fine)
    double overlap_min_depth = 0.02;// mm minimum penetration to report
    bool overlap_include_dnp = false;
    bool check_board = true;
    bool check_unassigned = true;   // only when the model has placement points
    int min_fiducials = 2;
    double collinear_tol = 0.5;     // mm: max distance from the fitted line
    double rot_tol = 0.01;          // deg: tolerance for "multiple of 90"
    std::string legal_ref_chars = "_-.$+/#";   // allowed besides [A-Za-z0-9]
};

std::vector<PlacementIssue> check_placements(const Project& prj, const PlacementCheckOptions& o = {});
std::string describe_issues(const std::vector<PlacementIssue>& v);
int count_issues(const std::vector<PlacementIssue>& v, Severity s);

// --- auto fixes (return number of changes) ---
// Second and later duplicates get "<ref><sep><n>" with the smallest unused n (n >= 2).
int fix_duplicate_refs(Project& prj, const std::string& sep = "_");
// Rotations within tol of a multiple of step are snapped (and normalised to [0,360)).
int snap_rotations(Project& prj, double step = 90.0, double tol = 1.0);
// Illegal characters replaced by `rep`; refs upper-cased when `upper`.
int sanitize_refs(Project& prj, const PlacementCheckOptions& o = {}, char rep = '_', bool upper = false);
// Fill missing component packages from their part number's package.
int fill_packages_from_parts(Project& prj);
// Clear model assignment of DNP components.
int unassign_dnp(Project& prj);

} // namespace pcbp
