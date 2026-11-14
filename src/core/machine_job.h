#pragma once
// ============================================================================
// machine_job.h — neutral machine-program description handed to exporters.
//
// Built from a Project + ModelPoint (+ its MachineSetup) by build_machine_job().
// All coordinates are already transformed into machine space:
//   board origin (virtual / machine origin) subtracted, board orientation and
//   flip applied, hard offset added. Units: mm, angles deg CCW (0..360).
// Exporters apply only their own angle convention / unit scaling.
// ============================================================================
#include "geom.h"
#include <map>
#include <string>
#include <vector>

namespace pcbp {

struct Project;
struct ModelPoint;
struct MachineSetup;
struct Component;

struct JobPlacement {
    std::string ref;
    std::string part;           // part number
    std::string package;
    std::string description;
    double x = 0, y = 0;        // machine coords, mm
    double rot = 0;             // machine angle, deg (export_angle(); CCW)
    double pre_rot = 0;         // Component::pre_rotation, deg CW (metadata; exporters
                                // write it only where the format has a field for it)
    int circuit = 1;            // 1-based panel image index
    bool skip = false;          // DNP / x-out
    bool polarized = false;
    bool smt = true;
    int leads = 0;
    double height = 0;
    bool has_ordered_pin_pair = false;
    bool has_scan_pair = false;
    Vec2 scan1, scan2;
    Vec2 pin1, pin2;            // machine coordinates, mm
};

struct JobPart {                // one row per distinct part number
    std::string part;
    std::string package;
    std::string description;
    double length = 0, width = 0, height = 0;   // body size mm (from package)
    // R24a (user v23 #2): where length/width came from: "body" = package body outline (what the machine's laser /
    // camera measures; leads excluded), "pads" = no real body outline, lead/pad extent used (approximate, includes
    // legs). height_src: "part" (part number / BOM), "package" (CAD / package editor), "" = no height (0 written).
    std::string size_src, height_src;
    int leads = 0;
    bool polarized = false;
    bool smt = true;
    int count = 0;              // placements using it
    int feeder_kind = 0;        // R18a JUKI +80 from PartNumber::packaging (0 = not written)
    double supply_angle = 0;    // R18a deg, effective_supply_angle(); never changes placement angles
    // R24b machine data (core/machine_assign.h; suggestion + package overrides). Written only where the format has a
    // CONFIRMED field (H8H: +78 type code, +176 tape code, +178 pitch code for tape parts). nozzle / centring are
    // carried for reports and warnings only (not written: H8H encoding unconfirmed).
    int type_code = 0, tape_code = 0, pitch_code = 0, nozzle = 0;
    std::string centring;
    double mach_conf = 0;
    bool mach_assigned = false;
};

struct JobFiducial {
    std::string name;
    double x = 0, y = 0;
    double diameter = 1.0;
    int circuit = 0;            // 0 = global (panel) fiducial
    bool local = false;         // image (circuit) or placement-local fiducial
    std::string owner;          // placement-local: owning component ref
    int number = 0;             // R21 Component::fid_number (Fiducial 1/2/3; 0 = unnumbered). Exporters
                                // select board / circuit marks by this number, storage order only as tie-break.
};

struct JobCircuit {             // panel image
    int index = 1;
    std::string name;
    double x = 0, y = 0, rot = 0;
    bool skip = false;
    // Placement-frame allocation (HLC_FORMAT.md §5): the rigid map that takes a
    // circuit-1 machine point m1 to this circuit: m = R(arot)*m1 + (ax, ay).
    // Circuit 1 is always (0, 0, 0). Computed from the real image + machine transform.
    double ax = 0, ay = 0, arot = 0;
};

struct MachineJob {
    std::string program_name;   // usually assembly number + point name
    std::string assembly, assembly_rev, customer;
    std::string point_name;
    std::string pcb_id;         // physical PCB / PWB part number (AssemblyInfo::pcb_id)
    std::string originator;     // author / originator (AssemblyInfo::originator) -> HLC line name
    // setup_signature() of the setup this job was built with. An importer stores the
    // signature of its own setup next to geometry it copied verbatim (juki.geom_sig);
    // a writer uses that geometry only while the signatures still match (flip/rotate
    // after import invalidates it).
    std::string setup_sig;
    std::string machine;        // exporter id
    double board_w = 0, board_h = 0;     // mm (machine orientation)
    double board_t = 1.6;
    bool bottom_side = false;
    bool single_board = false;
    // board (panel) lower-left corner in machine coords (board_w/h extent)
    double board_x0 = 0, board_y0 = 0;
    // circuit (image) frame of the root circuit, machine coords, lower-left + size.
    // has_circuit only when the project has a circuit frame on a panel.
    bool has_circuit = false;
    double circuit_x = 0, circuit_y = 0, circuit_w = 0, circuit_h = 0;
    std::vector<JobPlacement> placements;
    std::vector<JobPart> parts;
    std::vector<JobFiducial> fiducials;   // R24a: this side's own marks, else its linked layer's (transformed with this side's setup)
    std::string marks_from;               // R24a: non-empty = this side has no own marks; these are the linked layer's
    bool no_marks = false;                // R21 MachineSetup::no_marks: zero marks is intentional (suppresses the Error)
    std::vector<JobCircuit> circuits;
    std::map<std::string, std::string> options;
    std::vector<std::string> warnings;
};

// ---------------------------------------------------------------------------
// Angle convention (specs/ANGLES.md §5) — the ONLY place a placement rotation
// is turned into a machine angle:
//   a = Component::rot + PanelImage::rot             (board-relative, CCW)
//   if (flipped && mirror_angle_when_flipped) a = -a  (default: kept, see §5.3)
//   a = norm(a + orientation)                         (board orientation, CCW)
// No stored per-placement machine angle, no force-calc, no neutralize step.
// ---------------------------------------------------------------------------
double export_angle(const Project& prj, const Component& c, int image, const MachineSetup& ms);
// R16b feeder / tape orientation of the placement's package (0 when no package). R18a: informational only,
// NOT applied by export_angle() (specs/ANGLES.md §10).
int package_tape_rot(const Project& prj, const Component& c);
// R18a: one-time load notice for packages that still carry a non-zero (ignored) tape_rot; "" when none or
// already acknowledged (Project::tape_rot_notice_ack).
std::string tape_rot_notice(const Project& prj);
// one-line human description of the convention for this setup
std::string angle_convention_text(const MachineSetup& ms);

// Transform chain of one placement instance (§18 debug output).
struct TraceStep { std::string label, value; };
struct PlacementTrace {
    std::string ref;
    std::vector<TraceStep> steps;
    double x = 0, y = 0, rot = 0;   // final machine values (== build_machine_job)
};
bool trace_placement(const Project& prj, int point_index, size_t comp_index, int image,
                     PlacementTrace& out, std::string& err);
std::string trace_to_text(const PlacementTrace& t);

// Explicit pre-export validation (§19 P0; REVIEW_V12 P1-04, specs/HLC_FORMAT.md §13).
//   Error   = the machine program would be wrong or unreadable (blocks export):
//             no machine, nothing placed, duplicate/empty ref, invalid numbers, and
//             every Error of the exporter's own check (for JUKI: the program is
//             written in memory and run through validate_hlc -- fiducials enabled
//             without coordinates, placements outside board/circuit, ...).
//   Warning = output is valid but probably not what the operator wants; must be
//             confirmed: no P/N (package name used as part), not in BOM but placed,
//             package not in library (no outline / vision data), wrong side,
//             part names renamed by ANSI conversion.
//   R21: zero fiducial marks on the programmed side (or none the selected Board /
//             Circuit mode writes) is an Error unless MachineSetup::no_marks is set.
//   R18d/R21 policy: NOTHING hard-blocks export. Errors and warnings are listed in one
//             "Generate anyway?" confirmation (errors first, Cancel default); see
//             core/export_preflight.h for the single preflight every Generate path uses.
//   Info    = permitted differences, shown only: BOM DNP skipped, BOM package text
//             differs, ANSI transliteration, legacy-angle comparison, pre-rotation.
// "Ready to export" = no Error; show the Warning count beside it.
struct ExportIssue {
    enum Sev { Error = 0, Warning = 1, Info = 2 } sev = Warning;
    std::string ref;            // empty = job-level
    std::string msg;
};
std::vector<ExportIssue> validate_for_export(const Project& prj, int point_index);
struct ExportCounts { int errors = 0, warnings = 0, infos = 0; bool ready() const { return errors == 0; } };
inline ExportCounts count_issues(const std::vector<ExportIssue>& v) {
    ExportCounts c;
    for (auto& i : v) (i.sev == ExportIssue::Error ? c.errors : i.sev == ExportIssue::Warning ? c.warnings : c.infos)++;
    return c;
}
// Exporter-specific machine check, run by validate_for_export on the built job
// when the setup's exporter id starts with `exporter_prefix` (juki_writer.cpp
// registers "juki."). Kept as a hook so core does not link the writers.
using JobChecker = void (*)(const MachineJob& job, std::vector<ExportIssue>& out);
void register_job_checker(const char* exporter_prefix, JobChecker fn);
inline bool has_errors(const std::vector<ExportIssue>& v) {
    for (auto& i : v) if (i.sev == ExportIssue::Error) return true;
    return false;
}

std::string setup_signature(const MachineSetup& ms, Vec2 origin);
std::string project_program_name(const Project& p);   // R18d: Project Name, else Board ID, else legacy Assembly No.

// Builds a job; returns false + err when the point has no machine / nothing assigned.
bool build_machine_job(const Project& prj, int point_index, MachineJob& out, std::string& err);
// R24a: the other-side export layer sharing this layer's link ID (-1 = none)
int linked_point_index(const Project& prj, int point_index);

} // namespace pcbp
