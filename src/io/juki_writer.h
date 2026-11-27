#pragma once
#include <functional>
// ============================================================================
// juki_writer.h — JUKI HLC machine-program exporter (H4H / H5H / H7H / H8H,
// plus the per-machine E41..E46 / E51 binary variants).
//
// Clean-room implementation of the file layout documented in
// specs/JUKI_EXPORT.md. Every byte emitted is either a documented field or a
// documented default; nothing is copied from an external template file.
//
// All JUKI program formats CircuitCAM writes are binary (the "JUKI.E4x"
// strings are per-machine binary programs with the same block-table
// container), so there is no ASCII writer.
//
// Units on disk: lengths i32 in 1e-4 mm, angles i16 in 1/80 degree.
//
// MachineJob::options keys consumed (all optional):
//   juki.transport        "LR" (default) | "RL"   board transport direction
//   juki.fixed_rail       "front" (default) | "back"
//   juki.sort_by_part     "1"  sort placements by part number (stable)
//   juki.timestamp        unix seconds written to the board block (default now)
//   juki.comment          Lineinfo comment string (32 chars max)
//   juki.h4h_hlc5         "1"  H4H written as HLC 5.x (default HLC 6.x)
//   juki.board_x0/_y0     board lower-left in machine coords, mm (default 0)
//   juki.circuit_w/_h     single-circuit size, mm (board block +70/+74)
//   juki.circuit_x/_y     single-circuit origin, mm (board block +78/+82)
//   juki.global_marks     "F1,F2,F3" explicit global-mark order (by name)
//   juki.local.<REF>      "FA,FB" two fiducial names = local mark pair of REF
//   juki.h8h.version      4 chars for the H8H header version (default "C00A")
//   juki.board_t          board thickness, mm (overrides MachineJob::board_t)
//   juki.circuit_mode     "auto" (default) | "single" | "matrix" | "nonmatrix"
//   juki.fid_mode         "board" (default, PWB BOC) | "circuit" (circuit BOC)
//   juki.line_name        H8H line info +0 line name (the JUKI editor's line setting)
//   juki.circuit_list     "x,y,deg;..." exact Non-Matrix entries (mm), from an import
//   juki.h8h.raw          residual bytes of an imported H8H (h8h_residual, §11)
//
// Board / circuit / placement / component semantics: specs/HLC_FORMAT.md (from
// JUKI's own HLC<->IS converter, Juki.Iss.MemoryConversion.dll).
// ============================================================================
#include "core/machine_job.h"
#include <cstdint>
#include <string>
#include <vector>

namespace pcbp::juki {

enum class HlcFormat { H4H, H5H, H7H, H8H };

// HLC text fields are ANSI char[] (HLC_FORMAT.md §12). Converts UTF-8 (or
// cp1252 bytes) to printable 7-bit ASCII: +/- for the plus-minus sign, u for
// micro, Ohm, deg, straight quotes, '-' for dashes, x for the multiplication
// sign, accents stripped, anything else '_'. `changed` = characters replaced.
// The project keeps the Unicode text; only the written bytes are converted.
std::string legacy_text(const std::string& utf8, int* changed = nullptr);

// Part names of a machine program: the 20-character name field and the long-name field (60) of every part, in
// order, unique (case-insensitive) across the program. A part number of up to 20 characters is its own name. A
// longer one keeps its first 20 characters when no other name of the program has them; the beginning of an SMD
// part number identifies the part (the end is mostly packaging), so on a clash the name keeps as much of the
// beginning as it can, then "~" and the shortest end of the part number that makes it unique
// ("ABCDEFGHIJKLMNOPQRSTUV-TR" -> "ABCDEFGHIJKLMNOPQ~TR"), else "~" and a number. preferred[i] (optional): the
// name the part already has in the component DB; used when it is free.
struct MachineName {
    std::string name;   // <= 20 characters
    std::string full;   // long-name field (legacy text, <= 60)
    enum How { Same, Truncated, Renamed } how = Same;
};
std::vector<MachineName> machine_names(const std::vector<std::string>& parts, const std::vector<std::string>& preferred = {});

// Full variant list. Values 0..12 are CircuitCAM's internal format code
// (machine-setup +152); H8H is not produced by CircuitCAM (see spec).
enum class HlcVariant : int {
    H7H = 0,      // HLC 9.x
    H5H = 1,      // HLC 7.x
    H4H_v6 = 2,   // HLC 6.x
    H4H_v5 = 3,   // HLC 5.x
    E41 = 4, E42 = 5, E43 = 6, E44 = 7,   // KE-2010/2020/2030/2040
    E45 = 8, E46 = 9,                     // KE-2050/2060
    E51 = 12,                             // FX-1
    H8H = 100,                            // HLC 10+ (TTP-derived)
};

HlcVariant  variant_for(HlcFormat f, const MachineJob& job);
const char* hlc_extension(HlcVariant v);   // "H7H", "E41", ...

// Circuit configuration = board +42 bits 3-4 (HLC_FORMAT.md §5).
enum class CircuitMode : int { Single = 0, Matrix = 1, NonMatrix = 2 };
const char* circuit_mode_name(CircuitMode m);   // "Single PWB", "Matrix", "Non-Matrix"

struct CircuitPlan {
    CircuitMode mode = CircuitMode::Single;
    int nx = 1, ny = 1;          // matrix divisions (columns, rows)
    double px = 0, py = 0;       // matrix pitch, mm
    std::vector<std::string> notes;
    std::string error;           // non-empty: the requested mode is impossible
};
CircuitPlan plan_circuits(const MachineJob& job);

// What the writer decided (shown in Export > Diagnostics).
struct HlcWriteInfo {
    CircuitMode mode = CircuitMode::Single;
    uint16_t board_mode = 0;
    bool fid_circuit = false;
    int marks = 0;                       // board / circuit marks written (max 3)
    double ckt_x = 0, ckt_y = 0, ckt_w = 0, ckt_h = 0;   // circuit outline, placement coords
    int outside = 0;                     // placements flagged outside the circuit (+31 bit5)
    int comps_researched = 0;            // H8H components filled from the component DB (research hook)
    int comps_without_size = 0;          // H7H/H8H component records with no outline
    int comps_size_from_pads = 0;        // R24a: outline from the lead / pad extent (package has no real body outline)
    int comps_without_height = 0;        // R24a: no height in the CAD / BOM / package data (+89 left 0, flag b3 clear)
    int text_converted = 0;              // text fields that needed ANSI transliteration
    // Part names changed so every name stays unique (machine_names): {original, written}. Deterministic.
    std::vector<std::pair<std::string, std::string>> renamed;
    // part numbers longer than 20 characters written under their first 20 (full number in the long name)
    std::vector<std::pair<std::string, std::string>> truncated;
    // H8H machine data by source: the component DB (comps_researched), a package-library profile, built-in rules
    int comps_from_library = 0, comps_from_rules = 0;
    std::vector<std::pair<std::string, std::string>> generated;   // {part, "library: <profile>" / "rules: ..."}
    std::vector<std::string> notes;
};

// H8H: bytes of the optimizer / line-info / board / blocks 11,15,17,26 that
// differ from the JUKI defaults and are not derived from the job, encoded as
// "blk.off=hex;..." for option juki.h8h.raw (HLC_FORMAT.md §11). The importer
// stores it so import -> export reproduces a JUKI-saved program byte for byte.
std::string h8h_residual(const std::vector<uint8_t>& file);

// R21 (AUDIT_V20 P1): board / circuit mark selection, shared by the writer, check_job and the Export review so
// the UI shows exactly the marks written. Candidates by juki.fid_mode (board: global marks; circuit: circuit-1
// image marks; single PWB falls back between them), then ordered by JobFiducial::number (Fiducial 1, 2, 3),
// unnumbered marks after the numbered ones in storage order; at most 3 are written. juki.global_marks
// (explicit name list) keeps its own order. The job's fiducials are already only the programmed side's.
struct MarkSelection {
    bool fid_circuit = false;                 // circuit (image) BOC marks instead of board (PWB) marks
    bool explicit_list = false;               // juki.global_marks
    int candidates = 0;                       // marks that qualified before the 3-mark limit
    std::vector<const JobFiducial*> marks;    // written, in order (pointers into job.fiducials)
    std::vector<std::string> notes;
    std::string error;                        // unknown name in juki.global_marks
};
MarkSelection select_marks(const MachineJob& job);
std::string marks_text(const MarkSelection& m);   // "FID1 (#1, X 1.000, Y 2.000); ..."

bool write_hlc(const MachineJob& job, HlcFormat fmt,
               std::vector<uint8_t>& out, std::string& err, HlcWriteInfo* info = nullptr);
// H8H research hook (set by the application): fill JobPart::h8h_* for one part, true when it did; sets
// h8h_source "library" (+ h8h_note: the profile) when the data came from a package-library profile, and h8h_name to
// the part's DB name. Parts the hook does not fill get the built-in rules (io/juki_autopart) unless the job option
// "juki.autopart" is "off". Neither runs when "juki.research" is "off".
using H8hResearchHook = std::function<bool(JobPart&)>;
void set_h8h_research_hook(H8hResearchHook hook);
bool h8h_research_available();
// The built-in rules (registered by io/juki_autopart.cpp when it is linked; sets h8h_note to what was assumed).
void set_h8h_autopart(H8hResearchHook rules);
bool h8h_autopart_available();
bool write_hlc_variant(const MachineJob& job, HlcVariant v,
                       std::vector<uint8_t>& out, std::string& err, HlcWriteInfo* info = nullptr);

// Structural + geometric validation of an HLC file, mirroring what the JUKI
// editors check (block table, line-info counts, component links, placement
// inside board and circuit, circuit configuration). Reads raw bytes only.
struct HlcCheck { bool error = false; std::string msg; };
std::vector<HlcCheck> validate_hlc(const std::vector<uint8_t>& data);
inline bool hlc_check_ok(const std::vector<HlcCheck>& v) {
    for (auto& c : v) if (c.error) return false;
    return true;
}

// ---- Reader (self-check / diagnostics) ------------------------------------
struct HlcBlockEntry { uint32_t addr = 0, prefix = 0, recsize = 0, count = 0; };

struct HlcPlacementRec {
    std::string ref;            // 5-char field at +0
    int32_t x = 0, y = 0;       // 1e-4 mm
    int16_t angle = 0;          // 1/80 deg
    uint16_t flags = 0;         // bit0 skip, bit3 has local marks
    uint16_t status = 0;        // 0x1F linked / 0x0F no component
    uint16_t comp = 0;          // component index (0-based)
    uint16_t mark_set = 0;      // local mark element index
};

struct HlcMarkRec { int32_t x = 0, y = 0; };
struct HlcMarkSet { uint8_t type = 0; std::string name; uint8_t count = 0;
                    std::vector<HlcMarkRec> marks; };

struct HlcParsed {
    std::string name;
    uint8_t station = 0, flag = 0;
    std::string version;        // 3 or 4 chars from header +34
    std::vector<HlcBlockEntry> blocks;
    // Lineinfo
    std::string li_version;     // 7 chars at +55
    uint32_t li_place_count = 0, li_comp_count = 0, li_place_count2 = 0;
    // Board
    std::string board_name;
    uint16_t board_mode = 0;
    int32_t pcb_w = 0, pcb_h = 0, layout_x = 0, layout_y = 0;
    int32_t ckt_w = 0, ckt_h = 0, ckt_x = 0, ckt_y = 0;
    int16_t div_x = 0, div_y = 0;         // matrix
    int32_t first_x = 0, first_y = 0, pitch_x = 0, pitch_y = 0;
    uint16_t circuit_count = 0;
    std::vector<HlcMarkRec> circuits;     // x,y of each circuit entry
    std::vector<int16_t> circuit_angles;
    // Records
    std::vector<HlcPlacementRec> placements;
    std::vector<std::string> components;
    std::vector<std::string> comp_desc;
    std::vector<uint16_t> comp_counts;
    std::vector<int32_t> comp_w, comp_l, comp_h;   // H7H/H8H outline (+81/+85/+89)
    std::vector<int> comp_kind;                    // R18a H8H +80 feeder kind (0 elsewhere)
    std::vector<double> comp_supply;               // R18a H8H supply angle deg (+180 tape / +178 other)
    std::vector<int> comp_type, comp_tape, comp_pitch;   // R24b H8H +78 class code, +176 tape code, +178 pitch code (tape)
    std::vector<std::string> names;       // block 12 short name
    std::vector<HlcMarkSet> mark_sets;    // block 6 (wide layouts)
    std::vector<HlcMarkRec> local_marks;  // block 6 (narrow layout)
};

bool parse_hlc(const std::vector<uint8_t>& data, HlcParsed& out, std::string& err);
// R21 (AUDIT_V20 P1): generated-bytes check before publishing: parse_hlc + validate_hlc. false = the bytes are
// not a readable program (err set); otherwise `checks` holds validate_hlc's findings.
bool verify_hlc_bytes(const std::vector<uint8_t>& data, std::vector<HlcCheck>& checks, std::string& err);

// Exporter id ("juki.h8h", "juki.e45", ...) -> variant.
bool variant_from_exporter(const std::string& exporter_id, HlcVariant& v);
// Pre-export machine check: writes `job` in memory, runs validate_hlc and reports
// renames / conversions as ExportIssues. Registered with register_job_checker("juki.")
// at static-init time, so validate_for_export() runs it for every JUKI setup.
void check_job(const MachineJob& job, std::vector<ExportIssue>& out);
bool read_hlc_summary(const std::vector<uint8_t>& data, std::string& report);

// Merge a researched component record (`db`, what the database read produced) into the program's record `rec`
// (block 4, 1822 B), the way the editor's Research does: the identity, feeder slot assignment, placement counters and
// long-name fields stay as the program had them; outline, class, feeder/tape, nozzle, laser, vision, speeds and the
// rest come from `db`; the comment (+47, 30 characters, line breaks dropped) is taken from `db` and repeated at
// +1205. Sets the researched mark (+148).
void research_merge_b4(uint8_t* rec, const uint8_t* db);

// Move a part's element lists (block 19 header, groups 170 B, elements 112 B, arrays 3142 B, indices local to the
// part) to program positions: groups from g0, elements from e0, arrays from a0.
void h8h_relocate_elements(std::vector<uint8_t>& b19, std::vector<uint8_t>& groups, std::vector<uint8_t>& elements,
                           std::vector<uint8_t>& arrays, uint16_t g0, uint16_t e0, uint16_t a0);

} // namespace pcbp::juki
