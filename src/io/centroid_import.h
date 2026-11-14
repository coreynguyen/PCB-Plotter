#pragma once
// ============================================================================
// centroid_import.h — ASCII centroid ("XY", "pick & place", "component data
// report", "mount", ".pos") import into a pcbp::Project.
//
// Mirrors CircuitCAM's ASCII Centroid Import dialog (manual Part 2 §2.1-2.11:
// column headings Reference/X/Y/Rotation/Side/Part Number/Package, Units
// MIL/in/mm/cm + Factor, rotation offset, X/Y mirror, Top/Bottom/Both with
// side filters) and the Reference Fusion utility (Part 3 §1.1: attach refs
// from a centroid file to already digitized components by position, using a
// key component to compute the origin delta, a tolerance, Clear Refs, and
// Centroid / Pin matching methods).
//
// Coordinates end up in mm, rotation in degrees CCW (pcbp conventions).
// ============================================================================
#include "../core/project.h"
#include "text_table.h"
#include "text_import.h"
#include <string>
#include <vector>

namespace pcbp::io {

// LenUnit / len_unit_name / len_unit_mm live in text_import.h

// Column indices into the TextTable (-1 = unused).
struct CentroidMapping {
    int ref = -1, x = -1, y = -1, rot = -1, side = -1, part = -1, package = -1, value = -1;
    bool valid() const { return ref >= 0 && x >= 0 && y >= 0; }
};

enum class CentroidMode {
    Replace,    // remove existing components, add all rows
    Merge,      // update matching refs (position/rot/side/part/package), add new refs
    Fusion      // only attach refs to existing components by nearest position
};
enum class SideSelect { Both, Top, Bottom };
enum class FusionMethod { Centroid, Pin };   // Pin also matches any pad position

struct CentroidOptions {
    TextTableOptions table;
    bool auto_map = true;               // fill unset mapping entries from headers/content
    CentroidMapping map;
    LenUnit units = LenUnit::Auto;
    double factor = 1.0;                // CircuitCAM "Factor" (e.g. 0.1 for 1/10 mil)
    double rot_offset = 0;              // added to every rotation (deg)
    bool mirror_x = false;              // whole file: x -> -x, rot -> 180 - rot
    bool mirror_y = false;              // whole file: y -> -y, rot -> -rot
    bool bottom_mirror_x = false;       // bottom-side rows only: x -> -x (viewed-from-bottom exports)
    bool bottom_negate_rot = false;     // bottom-side rows only: rot -> -rot
    SideSelect side_select = SideSelect::Both;
    std::vector<std::string> top_filters{"T", "TOP", "TOPLAYER", "F", "FRONT", "F.CU", "COMPONENT", "1"};
    std::vector<std::string> bottom_filters{"B", "BOT", "BOTTOM", "BOTTOMLAYER", "BACK", "B.CU", "SOLDER", "2"};
    Side default_side = Side::Top;
    CentroidMode mode = CentroidMode::Merge;
    bool set_part = true, set_package = true;   // apply Part/Package columns (Merge/Fusion)
    // Fusion
    double fusion_tol_mm = 0.5;
    std::string key_ref;                // key component (must exist in project and file)
    bool clear_refs = false;            // clear all project refs before fusing
    bool auto_align = true;             // no key/common refs: search the best origin delta
    FusionMethod fusion_method = FusionMethod::Centroid;
    bool fusion_update_rot = false;     // copy file rotation into matched comps
};

struct CentroidRow {
    std::string ref, part, package, value;
    Vec2 pos;                           // mm, after units/mirror
    double rot = 0;
    Side side = Side::Top;
    bool has_rot = false, has_side = false;
    bool dnp = false;                   // DNP / not-fitted flag column (pnp_import)
    int line = 0;
};

struct CentroidReport {
    TextTable table;                    // parsed table (for preview UIs)
    CentroidMapping map;                // mapping actually used
    LenUnit units = LenUnit::Auto;      // units actually used
    std::string units_reason;           // why (header text, comment, magnitude ...)
    std::vector<CentroidRow> rows_parsed;
    int rows = 0, imported = 0, added = 0, updated = 0, skipped = 0, filtered = 0;
    // fusion
    int fused = 0;
    std::vector<std::string> unmatched_refs;    // file refs with no component in tolerance
    int unmatched_comps = 0;                    // project comps that got no ref
    Vec2 fusion_delta;                          // project = file + delta (mm)
    int fusion_delta_votes = 0;
    double fusion_max_dev = 0;                  // mm, worst matched distance
    std::vector<std::string> messages;
    std::string describe() const;
};

// Guess column mapping from header names (and cell contents when unnamed).
// units_hint receives a unit guessed from header text such as "(mm)".
CentroidMapping auto_map_centroid(const TextTable& t, LenUnit* units_hint, std::string* reason);

// Parse a side cell using the filter lists; returns false when not recognised.
bool parse_side(const std::string& cell, const CentroidOptions& o, Side& out);

bool import_centroid_text(const std::string& text, Project& prj, const CentroidOptions& opt,
                          CentroidReport& rep, std::string& err);
// Apply already parsed rows (rep.rows_parsed) to the project using opt.mode
// (Replace / Merge / Fusion). Used by parse_centroid_text callers and pnp_import.
bool apply_centroid_rows(Project& prj, const CentroidOptions& opt, CentroidReport& rep);
bool import_centroid_file(const std::string& path, Project& prj, const CentroidOptions& opt,
                          CentroidReport& rep, std::string& err);

// Parse only (no project changes): fills rep.table/map/units/rows_parsed.
bool parse_centroid_text(const std::string& text, const CentroidOptions& opt,
                         CentroidReport& rep, std::string& err);

} // namespace pcbp::io
