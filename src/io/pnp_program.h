#pragma once
// ============================================================================
// pnp_program.h — editable pick-and-place machine program (JUKI HLC binary
// .h8h/.h7h/.h5h/.h4h/.e4x/.e51 and JUKI ISS XML) for the Program Editor.
//
// Design (ported from the author's TrayTagPrinter juki/ H8hDoc + IssDoc):
//   * The original file is kept untouched in PnpSource. The editable model
//     (placements / parts / circuits / marks / board) is decoded from it.
//   * Saving in the SAME format re-encodes from the source: every record starts
//     as the original bytes of its source record and only fields whose decoded
//     value changed are written. Unknown bytes (feeder / vision / nozzle /
//     station data, padding quirks) survive; an unedited program saves
//     byte-identical.
//   * HLC is index-linked (placement -> component index), so placement rows can
//     be deleted / cloned / reordered (block 3 + block 12 move in lockstep,
//     component counts and Line-info totals follow). ISS is text-patched
//     (string-keyed): field edits only; renaming a part cascades to every
//     <componentName> that held the old name.
//   * Converting to another HLC flavour goes through MachineJob + juki_writer
//     (lossy: machine data the editor does not model is replaced by defaults).
//
// Units: mm, degrees CCW. Format facts: specs/JUKI_EXPORT.md, TTP H8H_EDIT_FORMAT.md.
// ============================================================================
#include "../core/machine_job.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pcbp::io {

enum class PnpKind { None, Hlc, Iss };

struct PnpPlacement {
    std::string ref;
    int part = -1;              // index into PnpProgram::parts (-1 = no component link)
    double x = 0, y = 0, rot = 0;
    bool skip = false;
    int circuit = 0;            // 0-based, derived (HLC stores none; ISS baseCircuitId)
    int station = 0, layer = 0; // HLC +19 / +18; ISS station / layer
    int head = 0;               // ISS head unit*100 + head (0 = n/a)
    bool has_local = false;     // HLC flag bit 3: local mark pair
    int mark_set = -1;          // HLC local mark element / pair index
    int status = 0;             // HLC status byte (0x1F linked, +0x20 outside circuit, 0x0F unlinked)
    std::string action;         // ISS model action (display)
    int src = -1;               // source record index (-1 = none)
};

struct PnpPart {
    std::string name, desc;
    std::string package;        // ISS <package> (TAPE / TRAY / ...), HLC feeder type text
    double len = 0, wid = 0, hgt = 0;   // body mm (has_body)
    bool has_body = false;
    int type = -1;              // HLC component type (+78)
    bool skip = false;          // H8H +716 bit0, ISS componentSkip
    bool skip_known = false;    // field exists in this format
    int stored_count = 0;       // HLC +118 placement count as stored
    int src = -1;
};

struct PnpCircuit {
    double x = 0, y = 0, rot = 0;   // offset (mm) and angle of the circuit
    bool derived = false;           // generated from matrix pitch (edit the board instead)
};

enum class MarkKind { Global, Local, Bad };

struct PnpMark {
    std::string name;
    double x = 0, y = 0, dia = 0;
    MarkKind kind = MarkKind::Global;
    int set = 0, slot = 0;      // HLC block-6 element / entry (narrow: -1 board, pair index)
    int src = -1;
};

struct PnpBoard {
    std::string program_name;   // HLC header +0 / ISS pwbId
    std::string board_name;     // HLC board +0
    std::string comment;        // HLC line info +21
    std::string line_name;      // HLC line info +0 / ISS lineName
    std::string target;         // station / target machine (display)
    double w = 0, h = 0;                // board size
    double origin_x = 0, origin_y = 0;  // HLC PCB layout corner (+60/+64) / ISS pwbLayoutOffset
    double ckt_w = 0, ckt_h = 0, ckt_x = 0, ckt_y = 0;   // circuit object
    double thick = 0; bool has_thick = false;
    uint16_t mode = 0;          // HLC board mode flags (+42)
    bool rl = false, back = false;      // transport right->left, fixed rail back
    int layout = 0;             // 0 single, 1 matrix, 2 non-matrix
    int div_x = 1, div_y = 1;
    double pitch_x = 0, pitch_y = 0, first_x = 0, first_y = 0;
    uint32_t timestamp = 0;
};

struct PnpSource;   // original bytes + layout (pnp_program.cpp)

struct PnpProgram {
    PnpKind kind = PnpKind::None;
    std::string format;         // "H8H", "H7H", "H5H", "H4H", "E41".."E51", "ISS"
    PnpBoard board;
    std::vector<PnpPlacement> placements;
    std::vector<PnpPart> parts;
    std::vector<PnpCircuit> circuits;
    std::vector<PnpMark> marks;
    std::vector<std::string> notes;     // load diagnostics
    std::shared_ptr<const PnpSource> src;

    // capabilities of the loaded format
    bool can_restructure() const;       // delete / clone / reorder placements
    bool can_rename_part() const;
    bool part_body_editable() const;
    bool circuits_editable() const;     // explicit (non-matrix) circuit list
    int ref_cap() const;                // chars a placement ref can hold
    int part_cap() const;               // chars a part name can hold (long name)
    Vec2 board_corner() const;          // lower-left of the board rectangle (inferred, see .cpp)
    void derive_circuits();             // PnpPlacement::circuit for HLC panels
};

// ---- load / save -----------------------------------------------------------
int pnp_sniff(const std::vector<uint8_t>& head);       // 0..100
bool pnp_load(const std::vector<uint8_t>& data, PnpProgram& out, std::string& err);
// Same format. notes: what was adjusted (truncations, recomputed counts).
bool pnp_save(const PnpProgram& p, std::vector<uint8_t>& out, std::string& err, std::vector<std::string>* notes = nullptr);
bool pnp_modified(const PnpProgram& p);                  // differs from the source (field compare)

// ---- conversion (lossy) ------------------------------------------------------
std::vector<std::string> pnp_convert_targets(const PnpProgram& p);   // e.g. {"H8H","H7H","H5H","H4H"}
bool pnp_to_job(const PnpProgram& p, MachineJob& job, std::vector<std::string>& lossy);
bool pnp_convert(const PnpProgram& p, const std::string& target, std::vector<uint8_t>& out,
                 std::vector<std::string>& lossy, std::string& err);

// ---- checks / exports ------------------------------------------------------
struct PnpIssue {
    int severity = 1;           // 0 info, 1 warning, 2 error
    int table = 0;              // 0 placements, 1 parts, 2 circuits, 3 marks, 4 board
    int row = -1;
    std::string text;
};
std::vector<PnpIssue> pnp_validate(const PnpProgram& p);
std::string pnp_csv(const PnpProgram& p);
// Circuit frames (lower-left + size + angle) for drawing; empty when no circuit object.
struct PnpFrame { Vec2 corners[4]; int index = 0; };
std::vector<PnpFrame> pnp_circuit_frames(const PnpProgram& p);
// offset of circuit k relative to circuit 0 (+ its angle) for ghost drawing
bool pnp_circuit_xform(const PnpProgram& p, int k, Vec2& off, double& rot);
const char* pnp_mark_kind_name(MarkKind k);
bool pnp_is_program_ext(const std::string& ext_lower_no_dot);

} // namespace pcbp::io
