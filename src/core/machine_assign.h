#pragma once
// ============================================================================
// machine_assign.h — library-free machine component data assignment (R24b).
//
// From a package name / part description and (when known) the CAD body size, derive the
// pick-and-place component data a machine program carries: nozzle, packaging, tape width,
// feed pitch, carrier (paper / embossed), supply angle, centring intent, component class code.
// No vendor database is needed at run time: the rule tables are GENERIC size bands
// (body short / long edge -> value) fitted statistically at dev time by
// tools/derive_machine_tables.cpp and embedded as text (machine_tables.inc). See
// specs/MACHINE_COMPONENTS.md §7 for the provenance and the hold-out accuracy.
//
// House rules (user, hard; applied after the tables, never overridden by them):
//   * nozzles: only 500, 504, 505, 506, 507, 508 (anything else is remapped);
//   * every 8 mm tape is paper; wider tape is embossed;
//   * cut tape for everything except QFP- and BGA-type packages, which come on tray;
//     MTC (tray magazine) is never assigned (an explicit MTC packaging becomes Tray);
//   * supply angle: tape 0, tray 180, stick 270 (ANGLES.md §10).
// A user override (Package::mach) always wins over the suggestion, field by field.
// ============================================================================
#include <map>
#include <string>
#include <vector>

namespace pcbp::mach {

enum class Fam : int {
    Unknown = 0, Chip, Melf, Tant, Led, ECap, Inductor, Sod, Sot, Dpak, Sop, Qfn, Qfp, Bga, Plcc, Conn, Xtal, Other, Count
};
const char* fam_name(Fam f);
Fam fam_from_name(const std::string& s);
bool fam_is_tray(Fam f);    // QFP / BGA families (house rule: tray)

// Text identification result.
struct PkgId {
    Fam fam = Fam::Unknown;
    std::string token;           // what matched, e.g. "0603", "SOT-23", "QFN 5x5", "LQFP-64"
    int pins = 0;
    double l = 0, w = 0, h = 0;  // nominal body (mm) when the text implies it (l >= w), else 0
};
PkgId identify(const std::string& name, const std::string& desc);

// The fields we assign. 0 / "" = unset.
struct Fields {
    int nozzle = 0;                 // 500, 504..508
    std::string packaging;          // Tape, Tray, Stick, Bulk
    int tape_w = 0;                 // mm (tape only)
    int pitch = 0;                  // feed pitch mm (tape only)
    std::string carrier;            // Paper, Embossed (tape only)
    double supply_angle = 0;        // deg
    bool supply_angle_set = false;
    std::string centring;           // Laser, Vision (intent; not written to the program)
    int type_code = 0;              // component class code (H8H +78)
};

struct Input {
    std::string name;               // package name (CAD footprint)
    std::string desc;               // part description (BOM)
    std::string pn_packaging;       // PartNumber::packaging ("" unknown)
    double body_x = 0, body_y = 0;  // CAD BODY size mm at 0 deg (not leads); 0 = unknown
    double body_h = 0;
    int pins = 0;
};

struct Suggestion {
    Fields f;
    Fam fam = Fam::Unknown;
    std::string token;
    double body_s = 0, body_l = 0;  // short / long edge used (mm)
    std::string dims_src;           // "cad", "text", ""
    double conf = 0;                // 0..1
    std::string rule;               // human-readable trail of the rules that fired
};

// ---- rule tables -----------------------------------------------------------
// Step table: value = vals[i] for the first i with x <= bounds[i] (last bound = +inf).
struct Step {
    int feature = 0;                // 0 short edge, 1 long edge
    std::vector<double> bounds;
    std::vector<int> vals;
    std::vector<double> purity;     // training share of the value inside the band
    int n = 0;                      // training rows
    bool empty() const { return vals.empty(); }
    int eval(double s, double l, double* pur = nullptr) const;
};
struct Tables {
    std::map<std::string, Step> steps;   // "noz.<fam>", "noz.ALL", "tw.<fam>", "tw.ALL", "pitch.<w>"
    std::map<std::string, int> consts;   // "type.<fam>"
    std::string source;                  // provenance line
};
bool parse_tables(const std::string& text, Tables& out, std::string& err);
std::string format_tables(const Tables& t);
const Tables& default_tables();           // embedded (machine_tables.inc)

Suggestion suggest(const Input& in, const Tables& t = default_tables());
// Effective = override field when set, else suggestion.
Fields merge(const Fields& sugg, const Fields& ovr);

// House-rule helpers (also used by the derivation tool so training labels obey them).
int remap_nozzle(int n);                  // -> 500/504..508, 0 if unusable
bool nozzle_allowed(int n);
double supply_angle_for(const std::string& packaging);

// H8H encodings (HLC_FORMAT.md §8.2, confirmed from real machine programs):
// +176 tape code: 8 mm paper 1, 32 mm paper 6, embossed 0x100 | {8:2,12:3,16:4,24:5,32:7,44:8,56:9}.
// +178 pitch code: 2 -> 0x0002, 4 -> 0x0004, 8 -> 0x0108, 12 -> 0x010C, 16 -> 0x0310, 20 -> 0x0314, 24 -> 0x1118.
int h8h_tape_code(int tape_w, const std::string& carrier);
int h8h_pitch_code(int pitch);
void h8h_decode_tape(int code, int& tape_w, std::string& carrier);
int h8h_decode_pitch(int code);

} // namespace pcbp::mach

// ---- project glue ----------------------------------------------------------
namespace pcbp { struct Package; struct PartNumber; struct Project; }
namespace pcbp::mach {
// CAD body size at 0 deg from the package body artwork (NOT the pad / lead extent); false if the package has
// no body or the body was only derived from pads.
bool package_body(const Package& p, double& bx, double& by);
Input input_for(const Package& pk, const PartNumber* pn);
Fields override_fields(const Package& pk);
// Suggestion + overrides for one package (pn = a representative part number using it, may be null).
Fields effective_for(const Package& pk, const PartNumber* pn, Suggestion* sugg_out = nullptr);
// Bulk: mark packages assigned (keeps overrides). which: 0 all, 1 the names in `sel`, 2 unassigned only. Returns count.
int assign_all(Project& prj, int which, const std::vector<std::string>& sel);
// A representative part number for a package (first in name order), or null.
const PartNumber* part_for_package(const Project& prj, const std::string& pkg);
} // namespace pcbp::mach
