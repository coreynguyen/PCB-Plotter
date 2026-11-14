#pragma once
// ============================================================================
// sides.h — manufacturing sides (production sequence Side 1 / Side 2) and the
// per-side export bundle (program + placements CSV + report). LEAD_BRIEF 11/12.
//
// Concepts
//   physical side : Top / Bottom of the PCB (Component::side)
//   Side N        : production order on the line (MfgSide::seq), chosen by
//                   the user. NEVER derived from Top/Bottom automatically:
//                   paste, adhesive, component weight and later operations
//                   decide the order. suggest_bottom_first() is only a hint.
//   export profile: the MachineSetup of the ModelPoint the side maps to
//                   (machine type/format, origin, board rotation, mirroring,
//                   folder, options). Re-used, not duplicated.
// Program file names: "<Job> [Side 1].h7h" (suffix overridable per side).
// ============================================================================
#include "project.h"
#include "machine_job.h"
#include <string>
#include <vector>

namespace pcbp {

struct SideCounts { int top = 0, bottom = 0; };
SideCounts side_counts(const Project& p);          // Feature::Component per physical side
bool is_two_sided(const Project& p);
const char* side_name(Side s);                      // "Top" / "Bottom"
std::string mfg_suffix(const MfgSide& s);           // "[Side 1]" unless overridden
std::string mfg_label(const MfgSide& s);            // "Side 1 (Bottom)"
// R18d: program / file base name = project_program_name() (core/machine_job.h: Project Name, else Board ID,
// else the legacy hidden assembly number), else "program".
std::string project_program_name(const Project& p);   // defined in machine_job.cpp
std::string job_base_name(const Project& p);
std::string sanitize_file_part(const std::string& s);
std::string side_file_base(const Project& p, const MfgSide& s);   // "<Job> [Side 1]"

// Hint only (tooltip text in suggest_hint()). Bottom-first is the common
// reflow order for light bottom parts, but it is NOT a rule.
inline bool suggest_bottom_first() { return true; }
const char* suggest_hint();

// Create the user-chosen mapping. point: placement point to use (index);
// separate=true clones it so each side has its own export profile and
// re-assigns that side's components to the clone. Sets configured=true.
void setup_sides(Project& p, bool bottom_first, int point, bool separate);
void swap_side_order(Project& p);
void clear_sides(Project& p);
const MfgSide* side_by_seq(const Project& p, int seq);
int side_point_index(const Project& p, const MfgSide& s);   // -1 = missing

struct SideIssue { bool blocking = false; std::string text; };
std::vector<SideIssue> validate_sides(const Project& p);
std::vector<SideIssue> validate_sides(const Project& p, const MfgSides& m);   // R18d: for a linked pair

// ---- R18d export-layer links (replace the Production sides block) -------------------------------
// ModelPoint::link: 0 = none. Two layers with the same ID form one two-sided SMT job; the UPPER layer in the
// layer list is Side 1 (run first), the lower is Side 2. Physical side = the layer's Board side setting.
int max_link(const Project& p, int exclude_point = -1);
int cycle_link(Project& p, int point);                       // None -> 1 -> .. -> max(other)+1 -> None; returns new ID
std::vector<int> link_members(const Project& p, int link);   // point indices in layer-list order
MfgSides link_sides(const Project& p, int link);             // configured pair, or empty when not exactly 2 members
bool auto_link_smt_pair(Project& p);                         // CAD import: exactly two SMT layers (top + bottom) -> link them
void migrate_sides_to_links(Project& p);                     // load: legacy mfg_sides -> links, then clears p.mfg
std::string link_label(int link);                            // "None" / "1" ...
std::string validation_summary(const std::vector<SideIssue>& v);

// Job for one side: the point's job filtered to components on the physical
// side (fiducials filtered by build_machine_job via machine.side).
bool build_side_job(const Project& p, const MfgSide& s, MachineJob& out, std::string& err);

// MPN in the program: only real HLC fields are used (spec 4.4 component
// description +47 [30] / H7H long description +1177 [127]; Lineinfo comment
// +21 [32]). mode: "desc" (default, unchanged), "desc_mpn" -> "desc | MPN",
// "mpn" -> MPN replaces description (when present).
void apply_desc_mode(const Project& p, MachineJob& job, const std::string& mode);

std::string placements_csv(const Project& p, const MfgSide& s, const MachineJob& job);

struct SideExportResult {
    MfgSide side;
    std::string point, exporter, program_file, csv_file, err;
    bool ok = false;
    int placements = 0, parts = 0, fiducials = 0, circuits = 0;
    int orientation = 0; bool flipped = false;
    double origin_x = 0, origin_y = 0;
    std::vector<std::string> warnings;
};
std::string sides_report_html(const Project& p, const std::vector<SideExportResult>& r);
std::string sides_report_txt(const Project& p, const std::vector<SideExportResult>& r);

} // namespace pcbp
