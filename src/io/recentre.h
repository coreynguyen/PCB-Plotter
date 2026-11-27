#pragma once
// ============================================================================
// recentre.h — R25: move the placements of an existing machine program from the CAD footprint origin to the
// package's placement point (centre of its lands), using the project the program was made from.
//
// Programs exported before R25 carry the footprint origin of every part. Where a footprint was not drawn around its
// centre (chips with the origin on pad 1) that is not where the part goes. This corrects such a program in place:
// the file is loaded with pnp_program (the original bytes are kept, only changed fields are re-encoded), X / Y of
// the placements concerned are moved, nothing else is touched.
//
// What is moved: a placement whose stored X / Y still equals the project's origin point for that reference (after
// the one constant shift between project and program, within tol). What is left alone and reported:
//   * a placement that is already at the corrected point (the program was exported with R25, or run through this)
//   * a placement that is somewhere else (taught or edited at the machine): moving it would undo that
//   * a reference the project does not have, or whose package is centred (nothing to do)
// Running it twice changes nothing the second time.
// ============================================================================
#include "../core/project.h"
#include <cstdint>
#include <string>
#include <vector>

namespace pcbp::io {

struct RecentreRow {
    std::string ref, part, package;
    int circuit = 0;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // before / after (program units, mm)
    int what = 0;                            // 0 moved, 1 already corrected, 2 elsewhere (left), 3 not in the project, 4 centred package
};
struct RecentreReport {
    std::string format, point;               // program format; the project's process point it matched
    double shift_x = 0, shift_y = 0;         // program minus project, mm
    int placements = 0, matched = 0, moved = 0, already = 0, elsewhere = 0, unknown = 0, centred = 0;
    double largest = 0;                      // largest move, mm
    std::vector<RecentreRow> rows;           // every placement, in program order
    std::vector<std::string> notes;
};

// in: the program file's bytes. out: the corrected file (same format; equal to `in` when nothing moved).
// point_index < 0: the project's process point that fits the program best is found.
bool recentre_program(const std::vector<uint8_t>& in, const Project& prj, int point_index, double tol_mm,
                      std::vector<uint8_t>& out, RecentreReport& rep, std::string& err);
std::string recentre_text(const RecentreReport& rep, bool all_rows);   // the report as plain text

// Headless:  --recentre <program> [<program> ...] --project <file.pcbp> [--out <file>] [--in-place] [--dry-run]
//            [--point <n>] [--tol <mm>] [--all]
// Without --out / --in-place the corrected program is written beside the original as "<name> (centred).<ext>".
// --in-place keeps the original as "<name>.<ext>.orig" (never overwritten if it exists). --dry-run writes nothing.
// Returns 0 = done (or nothing to do), 1 = a file failed, 2 = bad arguments. `out` receives the report.
int recentre_cli(const std::vector<std::string>& args, std::string& out);

} // namespace pcbp::io
