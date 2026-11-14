#pragma once
// ============================================================================
// cad_common.h — shared plumbing for the "v2" CAD importers (pcbp::io):
// ODB++, IPC-2581, KiCad, Eagle, PADS, Allegro, Mentor neutral, FABmaster,
// DXF outline.
//
// Every importer parses into neutral CadPackage / CadComp records plus an
// OutlineBuilder, then CadBuilder::commit() applies them to a Project:
//   * packages deduplicated by name; same name + different pad geometry gets
//     a "<name>~2" suffix (warning) so no component silently changes shape;
//   * pin numbers: numeric names -> number, "A1" -> 1, other names -> 0;
//     Component::pin1_index points at the pad numbered 1;
//   * SMT vs TH: any pad with drill > 0 (or an explicit TH hint) -> TH;
//   * fiducials: ref FID* / package name containing "FID" (option);
//   * rotation normalised to [0,360) CCW; bottom-side placement follows
//     Project::comp_xform (rotate, then mirror X in the local frame).
//   * board outline: segments/arcs chained into closed polylines (mm).
// ============================================================================
#include "../core/project.h"
#include <string>
#include <vector>

namespace pcbp::io {

struct CadImportOptions {
    bool replace = true;                // clear comps/packages/parts (+frame when importing board)
    bool import_board = true;
    bool import_packages = true;
    bool fiducial_by_name = true;
    double arc_seg_deg = 10.0;
    std::string step;                   // ODB++ step / IPC-2581 step ("" = first/auto)
    std::string layer;                  // DXF: outline layer filter ("" = all)
    std::string units;                  // override: "" auto, "mm", "inch", "mil", "0.1mm", "um", or mm-per-unit number
    // Arbitrary user scale applied AFTER unit conversion (1 = none). Kept apart
    // from `units` on purpose: a unit fix is exact (25.4, 0.0254 ...), a scale
    // is a user correction (REVIEW_V9 §6C).
    double scale = 1.0;
};

// Import result classification (REVIEW_V9 §6D). Error = import failed (no result).
enum class CadImportStatus : uint8_t { Error = 0, Complete, ArtworkOnly, Partial };
const char* cad_status_name(CadImportStatus s);         // "Error" | "Complete" | "Artwork-only" | "Partial"

struct CadImportResult {
    std::string format, units, source_name;
    double unit_mm = 1.0;
    int packages = 0, components = 0, top = 0, bottom = 0, smt = 0, th = 0;
    int fiducials = 0, parts = 0, outline_items = 0, outline_polylines = 0;
    int layers = 0;                     // artwork layers added (Gerber/Excellon)
    int missing_packages = 0;           // components whose package is not defined
    int body_cad = 0, body_extent = 0, body_derived = 0;   // package body provenance counts
    CadImportStatus status = CadImportStatus::Error;
    std::string status_reason;          // one line: why this classification
    std::string units_evidence;         // where the source units came from
    std::vector<std::string> warnings;
    std::string partial_reason;         // set by an importer: placements present but data missing
    // Sets status/status_reason from the counts (idempotent; call again after
    // adding layers). `extra_partial_reason` forces Partial when placements exist.
    void classify(const std::string& extra_partial_reason = {});
    std::string status_line() const;    // "Import result: Complete \u2014 ..."
    std::string describe() const;
};

struct CadPin {
    std::string name;
    Vec2 pos;                           // package-local, mm, top view
    double w = 0, h = 0, rot = 0;
    PadShape shape = PadShape::Rect;
    double drill = 0;
    bool th_hint = false;               // format says through-hole even without drill size
    bool smd_hint = false;              // R18c: format declares the pin surface-mount: board holes near it are vias
};

struct CadPackage {
    std::string name;
    std::vector<CadPin> pins;
    std::vector<GPrim> body;
    std::string body_source;            // "cad" (default when body given) | "cad_extent"; empty body => derived from pads
    double height = 0;
    bool th_hint = false;
};

struct CadComp {
    std::string ref, package, part, value;
    Vec2 pos;                           // mm
    double rot = 0;                     // CCW deg (any range; normalised on commit)
    bool bottom = false;
    bool fiducial = false;              // explicit fiducial from the format
};

// Collects outline geometry; finish() chains pieces into polylines.
class OutlineBuilder {
public:
    double seg_deg = 10.0;
    void add_line(Vec2 a, Vec2 b);
    // arc around c, radius r, start angle a0 (deg), signed sweep (deg, + = CCW)
    void add_arc(Vec2 c, double r, double a0, double sweep);
    void add_arc_ends(Vec2 start, Vec2 end, Vec2 c, bool ccw);   // full turn when start==end
    void add_arc_3pt(Vec2 a, Vec2 mid, Vec2 b);
    void add_bulge(Vec2 a, Vec2 b, double bulge);                // DXF bulge
    void add_circle(Vec2 c, double r);
    void add_polyline(const std::vector<Vec2>& pts, bool closed);
    int items() const { return items_; }
    bool empty() const { return pieces_.empty(); }
    std::vector<std::vector<Vec2>> finish(double tol = 0.005) const;
private:
    std::vector<std::vector<Vec2>> pieces_;
    int items_ = 0;
};

class CadBuilder {
public:
    CadBuilder(const CadImportOptions& o, CadImportResult& r) : opt(o), res(r) { outline.seg_deg = o.arc_seg_deg; }
    // Returns the name actually used (dedup suffix if geometry differs).
    std::string add_package(const CadPackage& p);
    bool has_package(const std::string& name) const;
    void add_component(const CadComp& c);
    OutlineBuilder outline;
    // Applies everything to `prj`. Components referencing unknown packages
    // are kept (package name set) with a warning.
    bool commit(Project& prj, std::string& err);
    void warn(const std::string& w);

    const CadImportOptions& opt;
    CadImportResult& res;
private:
    struct PkgVariant { uint64_t hash; std::string final_name; };
    std::vector<std::pair<std::string, std::vector<PkgVariant>>> variants_;
    std::vector<Package> pkgs_;
    std::vector<CadComp> comps_;
    int warn_count_ = 0;
};

// ---- helpers --------------------------------------------------------------
int pin_number(const std::string& name);                 // "1"->1, "A1"->1, "12"->12, else 0
uint64_t pad_geometry_hash(const std::vector<Pad>& pads);
// absolute -> package local (inverse of Project::comp_xform without images)
Vec2 abs_to_local(Vec2 abs, Vec2 pos, double rot, bool mirror);
Vec2 local_to_abs(Vec2 local, Vec2 pos, double rot, bool mirror);
// Units word -> mm per unit ("MM","MILLIMETER","INCH","MILS","MIL","THOU","MICRON","UM","CM","METRIC"); 0 if unknown.
double unit_scale_word(std::string_view w);
// Guess pad size/shape/drill from a pad-stack name like "SMD60X30", "RECT1_0X0_5",
// "C60D35", "PAD60CIR35D", "R1.5", "S1.7D1.0" (dimensions in `unit_mm`).
bool guess_padstack(const std::string& name, double unit_mm, CadPin& pin);
// Split on a single delimiter character (no quoting).
std::vector<std::string> split_char(std::string_view s, char d);
// Parse double; returns def if not numeric.
double num_or(std::string_view s, double def = 0);
// Collect pads from absolute pin positions of one instance into a package (sorted by pin name).
void sort_pins_natural(std::vector<CadPin>& pins);
bool natural_less(const std::string& a, const std::string& b);

// Unit override word -> mm per unit: "mm","inch","mil","0.1mm","um", or a plain
// number (mm per source unit, custom). 0 = auto / unknown.
double units_override_mm(const std::string& s);

// ---- post-import rescale (Board > Rescale Coordinates, REVIEW_V9 §6C) -----
// Multiplies every board-space coordinate by `f` about (0,0): placements (pos,
// cad_pos, label offsets), package pads / drills / body / marker artwork, board
// and panel frames, panel image offsets, the virtual origin, board-image
// registrations and Gerber/Excellon layer text (RS-274X with omitted leading
// zeros; Excellon decimal or integer). Layers that cannot be rewritten are
// listed in `notes` and left unchanged. Returns the number of layers scaled.
int rescale_project(Project& prj, double f, std::vector<std::string>& notes);
// RS-274X text: scales X/Y/I/J coordinates and standard aperture sizes. False
// (text unchanged, reason in err) for trailing-zero formats or macro apertures.
bool scale_rs274x(std::string& text, double f, std::string& err);
bool scale_excellon(std::string& text, double f, std::string& err);
// Pretty name of a Package::body_source value for UI ("CAD outline", ...).
const char* body_source_label(const std::string& src);

} // namespace pcbp::io
