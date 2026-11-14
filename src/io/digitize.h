#pragma once
// ============================================================================
// digitize.h — Gerber pad-group digitizer (CircuitCAM-style pattern
// recognition), portable C++20.
//
// Pipeline
//   1. extract_pads(): turn a Gerber layer into "pad features":
//        - dark flashes (stacked/overlapping flashes merged into one pad),
//        - rastered pads: clusters of same-D-code draws whose stroked boxes
//          touch, which are compact (max dim <= max_pad_mm, aspect bounded)
//          and fill most of their bounding box (coverage test on a grid),
//        - single thick draws (round/square pen, short) -> oblong/rect pad,
//        - small dark regions (G36/G37) -> pad.
//      Traces (thin or long draws, sparse clusters) never become features.
//   2. digitize_rect(): pads fully inside the drag rectangle (partially
//      outside ones are ignored), minus hidden D-codes, minus pads stacked
//      inside a larger pad, minus vias (round flashes smaller than
//      via_ratio x the largest pad, when >= 2 larger pads exist).
//      Produces a Package pattern relative to the centroid (= centre of the
//      pads' bounding box), pin numbering, pitch, SMT/TH, body estimate,
//      rotation-invariant signature and an automatic package name.
//   3. auto_scan(): finds every other placement of the same pad grouping at
//      0/90/180/270 deg within position/size tolerances.
//
// Pin one rule
//   - a single pad whose shape class (round / rect / oblong / other) differs
//     from all other pads, while the others share one class -> pin 1;
//   - otherwise by layout: dual rows along X -> bottom-left pad; dual rows
//     along Y, quad, grid, other -> top-left pad (min x, then max y);
//     single row along X -> left-most; along Y -> top-most; two pads ->
//     left (or top) pad.
//   Pins are numbered counter-clockwise around the centroid from pin 1
//   (DIP/SOIC/QFP convention); grids are numbered row-major from top-left.
//
// Auto-naming (mm, 2 decimals trimmed; pitch = most common nearest-neighbour
// centre distance)
//   1 pad          PAD_<w>x<h>         (round: PAD_R<d>)
//   2 pads         SMT: CHIP_<L>x<W>   (L = outer pad extent, W = pad width)
//                  TH : AXIAL_<pitch> (pitch >= 7.5 mm) else RAD_<pitch>
//   3 pads SMT     SOT3_<pitch>
//   dual row       TH : DIP<n>[W]_<pitch>   (W: row spacing >= 12 mm)
//                  SMT: SOIC<n>[W]_<pitch>  (pitch >= 1.0, W: rows >= 7.5 mm)
//                       SSOP<n>_<pitch>     (pitch < 1.0)
//   single row     TH : SIP<n>_<pitch>, SMT: SIL<n>_<pitch>
//   quad           SMT: QFP<n>_<pitch> (pitch < 1.0) else PLCC<n>_<pitch>
//                  TH : QUAD<n>_<pitch>
//   grid           SMT: BGA<n>_<pitch>, TH: PGA<n>_<pitch>
//   other          SMT<n>_<pitch> / TH<n>_<pitch>
//   e.g. CHIP_2.29x1.57, SOIC8_1.27, DIP16_2.54, QFP132_0.64
// ============================================================================
#include "../core/project.h"
#include "gerber.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace pcbp::digitize {

enum class ShapeClass : uint8_t { Round, Rect, Oblong, Other };

struct PadFeature {
    Vec2 c;                     // centre (mm)
    double w = 0, h = 0;        // extents at 0 deg
    BBox box;
    ShapeClass cls = ShapeClass::Rect;
    PadShape shape = PadShape::Rect;
    int dcode = 0;
    bool raster = false;        // built from a cluster of draws
    bool flash = false;
    bool region = false;
    double hole = 0;            // aperture hole / drill (mm)
    std::vector<int> objs;      // source object indices in the layer
    // Oriented bounding box (minimum-area rectangle of the exposed shape): ow >= oh,
    // oang = direction of the ow side in [0,180) deg. oriented = false for round /
    // raster / merged features (then only the axis-aligned w/h are meaningful).
    bool oriented = false;
    double ow = 0, oh = 0, oang = 0;
};

struct DigitizeOptions {
    std::vector<int> hidden_dcodes;     // user-hidden apertures (vias etc.)
    const gerber::Layer* drill = nullptr;   // optional drill layer (SMT/TH)
    bool drop_vias = true;
    double via_ratio = 0.7;             // round flash < ratio * largest pad => via
    double max_pad_mm = 8.0;            // larger features are not pads
    double min_pad_mm = 0.1;
    double raster_min_cover = 0.65;     // filled fraction of a raster pad bbox
    double raster_max_aspect = 10.0;
    double single_draw_min_width = 0.4; // single thick draw => pad if pen >= this
    double single_draw_max_aspect = 4.0;
    bool merge_stacked_flashes = true;
};

// Spatially indexed pad features of one layer.
struct PadSet {
    std::vector<PadFeature> pads;
    double cell = 2.0;
    std::unordered_map<long long, std::vector<int>> grid;
    std::vector<int> query(const BBox& b) const;   // candidates whose centre may be in b
};

PadSet extract_pads(const gerber::Layer& L, const DigitizeOptions& opt);

struct DigitizeResult {
    std::vector<int> pad_indices;       // into PadSet::pads, in pin order
    std::vector<int> dropped;           // features inside sel that were rejected
    std::vector<std::string> notes;     // why things were dropped / guessed
    Vec2 centroid;                      // board coordinates
    Package pkg;                        // pads relative to centroid, pin-numbered
    int pin1 = 0;                       // index into pkg.pads (always 0 after sort)
    bool pin1_from_shape = false;
    int leads = 0;
    double pitch = 0;                   // mm
    bool smt = true;
    double body_w = 0, body_h = 0;      // mm, estimate at 0 deg
    std::string layout;                 // "single","two","dual-x","dual-y","row-x","row-y","quad","grid","other"
    std::string signature;              // rotation-invariant (0/90/180/270)
    std::string name;                   // auto package name
    // Component rotation (deg, CCW, layer coordinates). pkg is in its canonical (unrotated)
    // orientation; the instance pads are rotate(pkg pad, rot) + centroid.
    double rot = 0;
};

// Rotation of a pad group, detected from the pad geometry (oriented boxes of rotated
// rect / RoundRect / RotRect macros and rotated regions) and from the pad-row directions
// (nearest-neighbour centre vectors). Angles are modulo 90 deg, reported in (-45, 45].
struct RotationGuess {
    double angle = 0;                   // combined, snapped to 0.1 deg
    double pad_angle = 0, row_angle = 0;
    int pad_votes = 0, row_votes = 0;
    double pad_spread = 0, row_spread = 0;   // deg, circular std-dev of the votes
    double confidence = 0;              // 0..1
    std::vector<double> options;        // detected + common angles to offer
    std::string why;
};
RotationGuess estimate_rotation(const PadSet& ps, const std::vector<int>& idx);

// Digitize a pad group as a component rotated by `angle` deg: the pads are rotated back
// about their centre into the canonical orientation (pad sizes from the oriented boxes),
// analysed with digitize_rect(), and the result carries centroid (board coords) and rot = angle.
// Pads whose own orientation differs from the component's get Pad::rot.
bool digitize_rotated(const PadSet& ps, const std::vector<int>& idx, double angle, const DigitizeOptions& opt, DigitizeResult& out);

bool digitize_rect(const PadSet& ps, BBox sel, const DigitizeOptions& opt, DigitizeResult& out);
bool digitize_rect(const gerber::Layer& L, BBox sel, const DigitizeOptions& opt, DigitizeResult& out);

struct ScanOptions {
    double pos_tol = 0.10;              // mm, pad centre tolerance
    double size_tol_abs = 0.10;         // mm
    double size_tol_rel = 0.15;         // fraction of pad size
    bool rot90 = true;                  // also try 90/180/270
    std::vector<int> exclude;           // pad indices already used (accepted comps)
    bool skip_source = true;            // do not report the digitized instance
    bool isolation = true;              // reject a match whose grouping continues:
    double isolation_factor = 1.5;      //   a foreign same-size pad lies at a pattern
                                        //   displacement (<= factor*pitch) from a matched
                                        //   pad, or inside the matched pad-centre box
    BBox limit;                         // optional search window (empty = all)
    // Any-angle matching (off by default): after the 0/90/180/270 pass, search the remaining
    // pads for the pattern at arbitrary angles (rotation-invariant pad dims + pairwise
    // distance seeding, angle solved from a pad pair, refined by least squares, every pad
    // verified incl. its orientation). Such matches have rotated = true.
    bool any_angle = false;
    double angle_tol = 3.0;             // deg, pad orientation tolerance
};

struct Match {
    Vec2 centroid;
    double rot = 0;                     // instance = rotate(pattern, rot) + centroid  (pattern.rot not included)
    std::vector<int> pads;              // PadSet indices, in pattern pin order
    double rms = 0;                     // mm, centre residual
    bool rotated = false;               // found by the any-angle pass (needs review)
    double confidence = 1;              // 0..1
};

std::vector<Match> auto_scan(const PadSet& ps, const DigitizeResult& pattern, const ScanOptions& opt);
std::vector<Match> auto_scan(const gerber::Layer& L, const DigitizeResult& pattern, const ScanOptions& opt,
                             const DigitizeOptions& dopt = {});

// ---------------------------------------------------------------------------
// Orientation of a new component relative to a package's canonical orientation.
// pkg: package pads (canonical, relative to the package origin). inst: the selected pads in
// board orientation (positions relative to their own centroid, Pad::rot in board degrees).
// Tries 0/90/180/270 first, then (allow_any) arbitrary angles seeded from pad pairs; for each
// angle the pads are matched one-to-one (nearest centre, pad long-side direction and size must
// agree). Among equally good angles (symmetric packages) the one nearest `prefer` wins.
// mirror = bottom-side placement (package viewed mirrored in X, as Project::comp_xform).
// The result's rot satisfies inst[match[k]] ~= rotate(mirror(pkg[k]), rot) + offset.
struct OrientFit {
    bool ok = false;
    double rot = 0;            // deg, [0,360)
    double rms = 0;            // mm, centre residual
    Vec2 offset;               // instance centroid - rotated package origin (mm)
    bool axis = true;          // found among 0/90/180/270
    std::vector<int> match;    // pkg pad k -> inst index
};
OrientFit fit_orientation(const std::vector<Pad>& pkg, const std::vector<Pad>& inst, bool mirror = false,
                          double tol = 0.15, bool allow_any = true, double prefer = 0);

// Package convention: two-pad parts (chips, axial/radial) are built horizontal, pin 1 left.
// If the digitized group is vertical its package is rotated into that canonical orientation
// and r.rot is adjusted so the instance is unchanged. Returns true when it changed r.
bool canonicalize_orientation(DigitizeResult& r);

// Helpers
std::string pattern_signature(const std::vector<Pad>& pads);   // pads relative to centroid
// Fully rotation-invariant signature: pad count, sorted pairwise centre distances and sorted
// (long, short) pad dims, quantised to q mm. Equal for copies at any angle.
std::string rot_invariant_signature(const std::vector<Pad>& pads, double q = 0.05);
// Oriented (min-area) rectangle of a point set: c centre, w >= h, ang = direction of w in [0,180).
struct OBox { Vec2 c; double w = 0, h = 0, ang = 0, hull_area = 0; };
OBox min_area_rect(const std::vector<Vec2>& pts);
std::string auto_package_name(const DigitizeResult& r);
std::string fmt_mm(double v, int decimals = 2);                 // "2.54", "0.64", "1.6"

} // namespace pcbp::digitize
