#pragma once
// ============================================================================
// group_ops.h — selection queries + group rotation / attribute edits (brief §9).
// Pure model operations (no UI); the GUI wraps each call in ONE undo checkpoint.
// Rotations follow specs/ANGLES.md: Component::rot is the one authoritative
// angle, deg CCW in [0,360) with fractional precision; nothing is snapped.
// ============================================================================
#include "project.h"
#include <string>
#include <vector>

namespace pcbp {

enum class SelMode { Replace = 0, Add = 1, Subtract = 2 };

// Matches a refdes against a pattern list: "R1-R10, C3, U*, TP?" (comma/space
// separated; ranges via expand_ref_list; * and ? globs; case-insensitive).
bool ref_matches(const std::string& ref, const std::string& pattern);

// Generic: apply `pred` to every Component-feature placement; returns matches.
template <class Pred>
int select_where(Project& prj, Pred pred, SelMode mode) {
    int n = 0;
    for (auto& c : prj.comps) {
        const bool m = c.feature == Feature::Component && pred(c);
        if (mode == SelMode::Replace) c.selected = m;
        else if (m) c.selected = mode == SelMode::Add;
        n += m;
    }
    return n;
}
int select_by_package(Project& prj, const std::vector<std::string>& pkgs, SelMode mode);
int select_by_ref(Project& prj, const std::string& pattern, SelMode mode);
int select_by_part(Project& prj, const std::vector<std::string>& parts, SelMode mode);
int select_by_process(Project& prj, Process p, SelMode mode);
int select_by_side(Project& prj, Side s, SelMode mode);

// indices of selected Component-feature placements
std::vector<int> selected_indices(const Project& prj);

// --- group rotation (each returns the number of placements changed) ---
int rotate_add(Project& prj, const std::vector<int>& idx, double delta);   // default op
int rotate_set(Project& prj, const std::vector<int>& idx, double absolute);
int rotate_reset(Project& prj, const std::vector<int>& idx);               // -> rot_imported
// --- attributes ---
int set_package(Project& prj, const std::vector<int>& idx, const std::string& pkg);
int set_process(Project& prj, const std::vector<int>& idx, Process p);
int set_group_label(Project& prj, const std::vector<int>& idx, const std::string& label);
int set_pre_rotation(Project& prj, const std::vector<int>& idx, double cw);

// Outliers: placements whose (rot mod 90) differs from the majority residue of
// their package (within 0.01 deg). Considers the packages of `idx` (all
// placements of each such package, not only the selected ones).
struct Outlier { int index; std::string package; double rot, majority_residue; };
std::vector<Outlier> find_outliers(const Project& prj, const std::vector<int>& idx);

} // namespace pcbp
