#pragma once
// ============================================================================
// gerber_match.h — R22b (REVIEW_V21 §6, §8, §9, §10): Gerber-side geometry helpers shared by the
// Gerber viewer tools. Portable (no UI), unit tested (tests/test_gerber_match.cpp).
//
//  * ElemSelection: the one selection model of the Gerber viewer. Click / box is ADDITIVE,
//    Ctrl (remove = true) deselects. Never toggles, never replaces.
//  * Feature: an element reduced to its world bounding box (centre + size) on its layer. For
//    rotations by multiples of 90 deg the bounding box transforms exactly, so features compare
//    exactly at 0 / 90 / 180 / 270 deg.
//  * pitch_from_centres: two-selection diagonal panel pitch (and one-row / one-column variants).
//  * find_pattern: searches every feature for rigid copies (0/90/180/270 deg) of a reference group.
//  * build_outline: picked line / arc / region elements -> exact vector polylines (endpoints chained
//    with no snapping; arcs flattened to points ON the arc; nothing is rasterised).
// ============================================================================
#include "core/geom.h"
#include "io/gerber.h"
#include <string>
#include <vector>

namespace pcbp::gmatch {

struct ElemRef {
    int layer = -1, obj = -1;
    bool operator==(const ElemRef& o) const { return layer == o.layer && obj == o.obj; }
    bool operator<(const ElemRef& o) const { return layer != o.layer ? layer < o.layer : obj < o.obj; }
};

struct ElemSelection {
    std::vector<ElemRef> items;   // insertion order, no duplicates
    // remove = false: adds every hit not selected yet; remove = true (Ctrl): removes the hits that are
    // selected. Returns the number of items actually added / removed.
    int apply(const std::vector<ElemRef>& hits, bool remove);
    bool contains(const ElemRef& r) const;
    void clear() { items.clear(); }
    bool empty() const { return items.empty(); }
    size_t size() const { return items.size(); }
};

enum FeatureKind : int { FFlash = 0, FDraw = 1, FArc = 2, FRegion = 3 };

struct Feature {
    Vec2 c;                 // world bbox centre (mm)
    double w = 0, h = 0;    // world bbox size (mm), aperture included
    int layer = 0;
    int kind = FFlash;
    int obj = -1;           // object index in its layer
};

// Every dark object of L (layer transform t applied). hidden (optional): per-object "skip" flags
// (crop, hidden D-codes) of the same length as L.objs.
std::vector<Feature> layer_features(const gerber::Layer& L, const Xform& t, int layer, const std::vector<char>* hidden = nullptr);
Feature object_feature(const gerber::Layer& L, const gerber::Object& o, const Xform& t, int layer, int obj);
BBox features_box(const std::vector<Feature>& f);
// geometric centre of a selection = centre of the union of its element boxes
Vec2 selection_centre(const std::vector<Feature>& f);

// ---- panel pitch (REVIEW_V21 §9) --------------------------------------------------------------
enum class PitchMode { Diagonal = 0, Across = 1, Up = 2 };
struct PitchResult {
    bool ok = false;
    double dx = 0, dy = 0;  // pitch (mm, signed: second - first)
    double off_axis = 0;    // Across: |dy| measured, Up: |dx| measured (should be ~0)
    std::string why;        // failure reason or warning (ok can still be true with a warning)
};
// a = centre of the reference selection, b = centre of the same features on the circuit one across
// and one up (Diagonal), one across only (Across: dy forced 0) or one up only (Up: dx forced 0).
PitchResult pitch_from_centres(Vec2 a, Vec2 b, PitchMode m, double min_pitch = 0.1, double axis_tol = 0.05);

// ---- repeated-pattern search (REVIEW_V21 §10) -----------------------------------------------
struct MatchOptions {
    double tol = 0.05;               // position tolerance (mm)
    double size_tol = 0.02;          // absolute size tolerance (mm) ...
    double size_rel = 0.02;          // ... plus this fraction of the size
    bool rot[4] = {true, true, true, true};   // 0, 90, 180, 270 deg
    double min_fraction = 1.0;       // fraction of the reference features that must match
};
struct Candidate {
    Vec2 offset;            // instance transform: p' = R(rot) p + offset (deg CCW, about the origin)
    double rot = 0;         // 0 / 90 / 180 / 270
    Vec2 centre;            // where the reference selection's centre lands
    BBox box;               // world box of the matched features
    int matched = 0, total = 0;
    double rms = 0;         // position residual of the matched features (mm)
    bool ambiguous = false; // another candidate at the same place with a different rotation (symmetric group)
    std::vector<int> feats; // indices into `all` of the matched features
};
// all = every searchable feature; ref = indices (into all) of the reference group. Returns the
// NON-reference candidates (the reference itself and its own self-symmetries are dropped), sorted
// bottom-to-top, left-to-right by centre. why (optional): reason when nothing was found.
std::vector<Candidate> find_pattern(const std::vector<Feature>& all, const std::vector<int>& ref, const MatchOptions& o,
                                    std::string* why = nullptr);
// exact quarter-turn rotation about the origin (q = 0..3)
Vec2 rot_quarter(Vec2 p, int q);

// ---- outline from elements (REVIEW_V21 §8.1) ----------------------------------------------------
struct OutlineBuild {
    std::vector<std::vector<Vec2>> polys;   // closed contours first, then open chains (world mm)
    bool closed = false;                    // polys[0] is a closed contour spanning the selection
    BBox box;
};
// element centre lines in world coordinates (Draw: a-b, Arc: points on the arc, Region: contour)
std::vector<Vec2> element_polyline(const gerber::Layer& L, const gerber::Object& o, const Xform& t, bool& closed, int arc_segs = 72);
bool outline_element(const gerber::Object& o);   // line / arc / region (not flashes)
// chains open polylines end to end (endpoints within tol, joined at the EXACT first endpoint)
OutlineBuild build_outline(const std::vector<std::vector<Vec2>>& open, const std::vector<std::vector<Vec2>>& closed, double tol = 0.02);

} // namespace pcbp::gmatch
