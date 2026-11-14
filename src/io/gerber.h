#pragma once
// ============================================================================
// gerber.h — Gerber (RS-274X / RS-274D) and Excellon drill reader (portable).
//
// Output is a flat, ordered list of drawing objects in millimetres (board
// coordinates, +Y up). Object order is preserved so dark/clear polarity can
// be rendered in file order.
//
// Supported
//   RS-274X : %FS %MO %AD(C,R,O,P,macro) %AM (primitives 1,4,5,6,7,20,21,22,
//             $n variables, + - x / ( ) expressions) %LP %SR %OF %IP(warn)
//             %MI %SF, G01/G02/G03 G74/G75 G36/G37 G54 G70/G71 G90/G91 G04,
//             D01/D02/D03 Dnn, modal D-codes, M00/M02. X2 attributes
//             (%TF %TA %TO %TD) are ignored.
//   RS-274D : same data stream without parameter blocks; the caller supplies
//             the coordinate format (ParseOptions::fmt) and an aperture table
//             (usually from parse_aperture_list()). guess_format() tries the
//             common legacy formats and picks the most plausible one.
//   Excellon: M48 header, INCH/METRIC with LZ/TZ, FMAT, T##C## tools (header
//             or inline), modal X/Y, explicit decimals, G85 slots, G90/G91.
//             Holes become Flash objects with a Circle aperture
//             (D-code = 10 + tool - 1, hole = diameter).
// ============================================================================
#include "../core/geom.h"
#include <map>
#include <string>
#include <vector>

namespace pcbp::gerber {

struct Aperture {
    int dcode = 0;
    enum Shape { Circle, Rect, Obround, Polygon, Macro } shape = Circle;
    double w = 0, h = 0;        // mm. Circle/Polygon: w = h = outer diameter
    double hole = 0;            // mm (0 = none). Drill tools: hole = w
    int verts = 0;              // Polygon vertex count
    double rot = 0;             // Polygon rotation (deg)
    // Macro: flattened polygons relative to the flash point. Exposure-off
    // primitives are flagged in macro_clear (same index), drawn in order.
    std::vector<std::vector<Vec2>> macro_polys;
    std::vector<bool> macro_clear;
    std::string macro_name;
    // Aperture-list metadata (RS-274D lists); informational only.
    std::string usage;          // "FLASH", "DRAW", ...
    std::string comment;
    bool defined = true;        // false => referenced but never defined
};

struct Object {
    enum Kind { Flash, Draw, Arc, Region } kind = Flash;
    int dcode = 0;              // Region: 0
    bool clear = false;         // LPC polarity
    Vec2 a, b, center;          // Flash: a. Draw: a->b. Arc: a->b around center
    bool cw = false;            // Arc direction
    std::vector<Vec2> poly;     // Region: closed contour (arcs flattened)
};

struct Format {
    bool inch = true;           // false => millimetres
    int int_digits = 2;
    int frac_digits = 4;
    bool lead_supp = true;      // true: leading zeros omitted (L); false: trailing (T)
    bool absolute = true;
    std::string describe() const;   // e.g. "inch 2.3 LZ-suppressed absolute"
};

struct Layer {
    std::map<int, Aperture> aps;
    std::vector<Object> objs;
    BBox extent;                // dark objects incl. aperture size
    std::vector<std::string> warnings;
    Format fmt;                 // format actually used
    bool rs274x = false;        // had %FS / %AD parameter blocks
    bool excellon = false;
    bool format_from_file = false;
    bool negative = false;      // %IPNEG: image polarity negative (render inverted)
    bool empty_file = false;    // valid file with no objects (e.g. unused KiCad user layer)
    std::string name;           // %IN / %LN or caller-provided
    int flash_count() const;
    int draw_count() const;     // Draw + Arc
    int region_count() const;
    void recompute_extent();
};

struct ParseOptions {
    enum class Kind { Auto, Gerber, Excellon } kind = Kind::Auto;
    Format fmt;                 // used when the file has no %FS/%MO (274D)
    bool force_format = false;  // ignore %FS/%MO in the file
    std::map<int, Aperture> apertures;   // external table (274D); file %AD wins
    int arc_segs = 72;          // per full circle when flattening region arcs
    int max_warnings = 200;
};

// Parse a Gerber or Excellon text. Returns false only on hard failure
// (empty input, nothing recognisable). Soft problems go to layer.warnings.
bool parse(const std::string& text, const ParseOptions& opt, Layer& layer, std::string& err);

// Heuristic text sniffing.
bool looks_like_excellon(const std::string& text);
bool looks_like_rs274x(const std::string& text);

// ---------------------------------------------------------------------------
// Free-form aperture list (RS-274D companion files)
// ---------------------------------------------------------------------------
enum class ListUnits { Auto, Mil, Inch, MM };

struct ApertureListResult {
    std::map<int, Aperture> aps;
    std::string explanation;    // which columns were guessed, units, skipped rows
    int rows = 0, skipped = 0;
};

ApertureListResult parse_aperture_list(const std::string& text, ListUnits units = ListUnits::Auto);

// Inverse: produce a CircuitCAM-style text table ("D-code Size Shape Usage"),
// sizes in mils (inch=false => mm).
std::string aperture_list_to_text(const std::map<int, Aperture>& aps, bool mils = true);

// ---------------------------------------------------------------------------
// RS-274D format guessing
// ---------------------------------------------------------------------------
struct FormatCandidate {
    Format fmt;
    BBox extent;
    double overlap = 0;         // fraction of flashes overlapping a neighbour
    double score = 0;           // lower is better
    std::string note;
};

// Try inch 2.3/2.4/3.3/2.5, mm 3.3/4.3 (L and T suppression), parse with the
// supplied aperture table, score each by board-extent plausibility
// (target ~ 25..600 mm, prefer ~150 mm) and flash overlap density.
// Returns candidates sorted best-first (never empty).
std::vector<FormatCandidate> guess_format(const std::string& text,
                                          const std::map<int, Aperture>& aps);

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------
const Aperture* find_ap(const Layer& L, int dcode);
BBox aperture_bbox(const Aperture& ap);                 // relative to flash point
BBox object_bbox(const Layer& L, const Object& o);

// Outline of a flashed aperture (single closed polygon, CCW). For macros the
// convex hull of all exposed macro polygons is returned; use flash_polys()
// for the exact set.
std::vector<Vec2> flash_outline(const Aperture& ap, Vec2 at, int segs = 32);
std::vector<std::vector<Vec2>> flash_polys(const Aperture& ap, Vec2 at, int segs = 32,
                                           std::vector<bool>* clear = nullptr);

// Arc helpers (degrees, CCW positive)
double arc_start_deg(const Object& o);
double arc_sweep_deg(const Object& o);   // signed: CCW > 0, CW < 0; full circle +-360
std::vector<Vec2> arc_points(const Object& o, int segs_per_circle = 72);

// Topmost dark object (highest index) covering p within tol mm; -1 if none.
int hit_test(const Layer& L, Vec2 p, double tol);

// ---------------------------------------------------------------------------
// Layer function guess from the file name / extension / X2 attributes.
// role uses the LayerRole order of core/project.h:
//   0 Other, 1 Copper, 2 Paste, 3 Mask, 4 Silk, 5 Assembly, 6 Drill, 7 Outline
// Rules, first match wins: X2 %TF.FileFunction -> extension tables (Protel /
// Altium .GTL.., Eagle .cmp/.sol/.stc.., OrCAD .top/.bot/.smt/.sst..,
// numbered inner .G1/.GP1/.In1) -> name tokens (KiCad -F_Cu/-B_Paste/
// -Edge_Cuts/-In1_Cu, Allegro / generic words top/bot/pastetop/smt/int1/
// silk/mask/paste/drill/outline...) -> Excellon content = Drill.
// ---------------------------------------------------------------------------
struct LayerGuess {
    int role = 0;
    int side = 0;           // 0 top, 1 bottom, 2 inner
    int inner = 0;          // inner layer number (side 2)
    bool known = false;     // something matched (else role Other / top)
    std::string why;        // short reason ("ext .GBL", "KiCad -F_Paste", "X2 FileFunction")
};
LayerGuess guess_layer_function(const std::string& path, const std::string& content);

// Content sniffing for the Load Gerber Layers dialog (ViewMate Guess order: Gerber / drill /
// netlist / aperture list). Binary data, CAD text (GenCAD, centroid tables...) -> Unknown.
enum class FileKind { Rs274x, Rs274d, Excellon, ApertureList, Ipc356, Unknown };
FileKind classify_file(const std::string& content);
const char* file_kind_name(FileKind k);

// ---------------------------------------------------------------------------
// Writers (layer extraction / export). Coordinates are written as stored in the layer
// (layer coordinates, mm), metric 4.6 absolute, leading zeros omitted.
//   keep: optional per-object mask (size == objs.size(); 0 = skip), e.g. hidden D-codes / crops.
// RS-274X: %AD for every used aperture (Circle/Rect/Obround/Polygon, holes kept; macros as
// flattened outline primitives, exposure-off polys kept), LPD/LPC polarity, G01 draws,
// G75 + G02/G03 arcs (I/J incremental), G36/G37 regions (contours already flattened).
// Excellon: METRIC,LZ header with T## tools (T = dcode - 9, as read), hits and G85 slots.
// ---------------------------------------------------------------------------
std::string write_rs274x(const Layer& L, const std::vector<char>* keep = nullptr, const std::string& comment = {});
std::string write_excellon(const Layer& L, const std::vector<char>* keep = nullptr, const std::string& comment = {});

} // namespace pcbp::gerber
