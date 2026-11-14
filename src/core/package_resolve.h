#pragma once
// ============================================================================
// package_resolve.h — R16b centroid -> package pipeline (REVIEW_V15 §1, §2).
//
// A centroid-only import gives placements that name a package but carry no
// graphics. Every package name used by a placement gets an entry in
// Project::packages (an empty placeholder when nothing is known) so it shows
// in the Packages tab as "Unresolved / no graphics". It is resolved by the
// user: exact library match (case-insensitive), a template, a library
// package, or pads selected on the Gerbers (wizard). Suggestions from known
// families are suggestions only; nothing is generated without the user.
// Graphics are stored once per package, so an assignment applies to every
// placement that uses the package name.
// ============================================================================
#include "project.h"
#include <map>
#include <string>
#include <vector>

namespace pcbp {

// true when the package has graphics (pads or body artwork). Used by export validation.
bool package_resolved(const Package& p);
// Placement-level: false when the component names no package, the name has no entry, or it is unresolved.
// Fiducials / non-components are always resolved.
bool placement_package_resolved(const Project& prj, const Component& c);

// Unique package names referenced by component placements (natural order).
std::vector<std::string> referenced_packages(const Project& prj);
// Adds an empty placeholder package for every referenced name that has no entry. Returns the count added.
int ensure_package_entries(Project& prj);
// Referenced packages that are missing or unresolved (natural order).
std::vector<std::string> unresolved_packages(const Project& prj);
int placements_using(const Project& prj, const std::string& pkg);

// Case-insensitive exact name match (trimmed). nullptr = none.
const Package* library_exact_match(const std::map<std::string, Package>& lib, const std::string& name);

// Suggestion for an unresolved name. Never applied automatically.
struct PkgSuggestion {
    enum Kind { Library, Template } kind = Template;
    std::string name;       // Library: library package name
    std::string tpl;        // Template: template id (chip, melf, sod, sot23, sot223, sot89, soic, qfp, qfn, bga, plcc, dip, header, radial, axial, xtal4)
    std::string preset;     // Template: preset label of that template ("" = defaults)
    std::map<std::string, double> params;   // Template: parameters guessed from the name (e.g. pin count)
    std::string why;
    int score = 0;          // higher = better
};
// Library entries with the same package_key() (package_name.h) or the exact name, and template families
// recognised from the name. Sorted best first.
std::vector<PkgSuggestion> suggest_packages(const std::string& name, const std::map<std::string, Package>* lib);

// Replaces the graphics of package `name` with `src` (pads, body, flags, polarity marker), keeping the name,
// the tape orientation and the description when src has none. Creates the entry if missing. The polarity
// default is applied once (specs/POLARITY.md). Returns the number of placements using the package.
int assign_package_geometry(Project& prj, const std::string& name, const Package& src, const std::string& how);

// Builds a package from pads given in board coordinates (Pad::pos board mm, w/h along board X/Y, Pad::rot board deg)
// relative to the placement centroid and rotation: local = comp_xform(c)^-1 * board (de-rotated, bottom side
// un-mirrored). Pins are numbered counter-clockwise from pin 1 (left-most pad, then lowest; 2 pads: left).
Package package_from_board_pads(const std::vector<Pad>& board, const Component& c);

// ---- R17a (REVIEW_V16 §2): automatic resolution + report ----
// Built-in templates applied without asking (standard land patterns, verify in the Packages tab):
// chip resistors / capacitors 0201..2512 (package_key, e.g. "R_0603", "C0805", "RESC1608X55N") and
// SOT-23 (3 leads). Pad sizes are IPC-7351 nominal-density values; package 0 deg = pin 1 left.
bool builtin_template_package(const std::string& name, Package& out, std::string* why = nullptr);
struct PkgResolveEntry {
    std::string name;
    std::string how;            // "library", "project:<other package>", "template:chip", "template:sot23", "gerber", "" = unresolved
    std::string detail;         // human text
    int placements = 0;
    bool resolved = false;
    bool changed = false;       // resolved by this call
};
struct PkgResolveReport {
    std::vector<PkgResolveEntry> entries;   // every referenced package, natural order
    int resolved = 0, unresolved = 0, changed = 0;
    std::string summary() const;            // "6 of 7 packages resolved (3 now: ...); unresolved: SOIC-8 (2 placements)"
};
// Fills every unresolved referenced package, in this order: library entry with the same name; a resolved
// project package with an equivalent name (package_key); a library package with an equivalent name;
// a built-in template (when `templates`). Creates placeholder entries first. Never overwrites graphics.
PkgResolveReport auto_resolve_packages(Project& prj, const std::map<std::string, Package>* lib, bool templates = true);
PkgResolveReport package_report(const Project& prj);   // status only, nothing changes
// Pad bounding-box centre of a package (package coords): 0,0 when the placement centroid is the pad centre.
Vec2 package_pad_centre(const Package& p);

// ---- wizard (Project::pkg_wizard) ----
// Queue = unresolved packages not skipped, then skipped ones (revisit). Packages resolved meanwhile drop out.
std::vector<std::string> wizard_queue(const Project& prj);
struct WizardProgress { int total = 0, done = 0, skipped = 0, remaining = 0; };
WizardProgress wizard_progress(const Project& prj);
// Starts / resumes: keeps `current` when it is still unresolved, else the first of the queue. Returns it ("" = none left).
std::string wizard_resume(Project& prj);
void wizard_skip(Project& prj, const std::string& pkg);          // moves on; pkg goes to the end (revisit)
void wizard_mark_done(Project& prj, const std::string& pkg);     // after assign; advances `current`
std::string wizard_advance(Project& prj);                        // next in queue after current
// Index into prj.comps of the n-th (mod count) placement using pkg, -1 = none.
int wizard_placement(const Project& prj, const std::string& pkg, int n);

} // namespace pcbp
