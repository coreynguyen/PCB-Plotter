// ============================================================================
// placement_check.cpp — placement review. See placement_check.h.
// ============================================================================
#include "placement_check.h"
#include "package_edit.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

namespace pcbp {

const char* severity_name(Severity s) {
    switch (s) { case Severity::Info: return "info"; case Severity::Warning: return "warning"; default: return "error"; }
}

const char* check_code_name(CheckCode c) {
    switch (c) {
    case CheckCode::DuplicateRef: return "duplicate_ref";
    case CheckCode::EmptyRef: return "empty_ref";
    case CheckCode::IllegalRefChars: return "illegal_ref_chars";
    case CheckCode::Coincident: return "coincident";
    case CheckCode::NearCoincident: return "near_coincident";
    case CheckCode::Overlap: return "overlap";
    case CheckCode::OutsideBoard: return "outside_board";
    case CheckCode::PartlyOutsideBoard: return "partly_outside_board";
    case CheckCode::NoBoardFrame: return "no_board_frame";
    case CheckCode::MissingPackage: return "missing_package";
    case CheckCode::PackageNotInLibrary: return "package_not_in_library";
    case CheckCode::ZeroPadPackage: return "zero_pad_package";
    case CheckCode::MissingPartNumber: return "missing_part_number";
    case CheckCode::PartNotInLibrary: return "part_not_in_library";
    case CheckCode::PartPackageMismatch: return "part_package_mismatch";
    case CheckCode::Unassigned: return "unassigned";
    case CheckCode::DnpAssigned: return "dnp_assigned";
    case CheckCode::FewFiducials: return "few_fiducials";
    case CheckCode::CollinearFiducials: return "collinear_fiducials";
    case CheckCode::OffAxisRotation: return "off_axis_rotation";
    case CheckCode::OffAxisPackage: return "off_axis_package";
    case CheckCode::BomPackageDisagree: return "bom_package_disagree";
    case CheckCode::OrphanNotInBom: return "orphan_not_in_bom";
    case CheckCode::DnpInBom: return "dnp_in_bom";
    }
    return "?";
}

const char* check_code_badge(CheckCode c) {
    switch (c) {
    case CheckCode::BomPackageDisagree: return "PKG\xE2\x89\xA0";
    case CheckCode::PartPackageMismatch: return "PKG~";
    case CheckCode::OrphanNotInBom: return "ORPH";
    case CheckCode::DnpInBom: return "DNP";
    case CheckCode::MissingPartNumber: return "UNAS";
    default: return check_code_name(c);
    }
}

static std::string upper(const std::string& s) {
    std::string o = s;
    for (auto& c : o) c = (char)std::toupper((unsigned char)c);
    return o;
}
static bool ieq(const std::string& a, const std::string& b) { return upper(a) == upper(b); }
static std::string fmt(double v, int d = 3) {
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(d);
    o << v;
    return o.str();
}
static std::string name_of(const Component& c, int i) { return c.ref.empty() ? "#" + std::to_string(i) : c.ref; }

// Oriented box of a component body in board coordinates.
struct OBB { Vec2 c; Vec2 ax[2]; double h[2]; };

static OBB comp_obb(const Project& prj, const Component& c, double shrink) {
    BBox local;
    if (const Package* p = prj.package_of(c)) local = p->extent();
    else { local.add({-0.4, -0.4}); local.add({0.4, 0.4}); }
    Xform t = prj.comp_xform(c, 0);
    OBB o;
    o.c = t.apply(local.center());
    o.ax[0] = rotate({1, 0}, c.rot);
    o.ax[1] = rotate({0, 1}, c.rot);
    o.h[0] = std::max(0.0, local.w() * 0.5 - shrink);
    o.h[1] = std::max(0.0, local.h() * 0.5 - shrink);
    return o;
}

// Minimum penetration depth along SAT axes (<= 0 => separated).
static double obb_overlap(const OBB& a, const OBB& b) {
    double best = 1e300;
    const Vec2 axes[4] = {a.ax[0], a.ax[1], b.ax[0], b.ax[1]};
    Vec2 d = b.c - a.c;
    for (auto& n : axes) {
        auto proj = [&](const OBB& o) {
            return o.h[0] * std::fabs(o.ax[0].x * n.x + o.ax[0].y * n.y) + o.h[1] * std::fabs(o.ax[1].x * n.x + o.ax[1].y * n.y);
        };
        double dist = std::fabs(d.x * n.x + d.y * n.y);
        double pen = proj(a) + proj(b) - dist;
        if (pen <= 0) return pen;
        best = std::min(best, pen);
    }
    return best;
}

std::vector<PlacementIssue> check_placements(const Project& prj, const PlacementCheckOptions& o) {
    std::vector<PlacementIssue> out;
    auto add = [&](Severity s, CheckCode c, std::string m, std::vector<int> ix) { out.push_back({s, c, std::move(m), std::move(ix)}); };
    const auto& C = prj.comps;
    const int n = (int)C.size();

    // ---- references ----
    std::map<std::string, std::vector<int>> by_ref;
    for (int i = 0; i < n; ++i) {
        const Component& c = C[i];
        if (c.ref.empty()) {
            if (c.feature == Feature::Component) add(Severity::Error, CheckCode::EmptyRef, "component #" + std::to_string(i) + " has no reference", {i});
            continue;
        }
        // refdes is unique per context: the circuit (all instanced items, both
        // sides) or the panel (global fiducials, never instanced). Part numbers
        // may repeat freely. A panel mark "F1" next to fuse "F1" is legal.
        by_ref[(prj.instanced(c) ? "C:" : "P:") + upper(c.ref)].push_back(i);
        bool bad = false;
        for (unsigned char ch : c.ref)
            if (!std::isalnum(ch) && o.legal_ref_chars.find((char)ch) == std::string::npos) bad = true;
        if (bad) add(Severity::Warning, CheckCode::IllegalRefChars, c.ref + ": illegal characters in reference", {i});
    }
    for (auto& [k, v] : by_ref)
        if (v.size() > 1) {
            std::string m = "duplicate reference " + C[v[0]].ref + " (" + std::to_string(v.size()) + "x)";
            add(Severity::Error, CheckCode::DuplicateRef, m, v);
        }

    // ---- coincident (grid hash by near_tol) ----
    {
        const double cell = std::max(o.near_tol, o.coincident_tol) > 0 ? std::max(o.near_tol, o.coincident_tol) : 0.25;
        std::unordered_map<long long, std::vector<int>> g;
        auto key = [&](long long x, long long y) { return (x << 32) ^ (y & 0xffffffffLL); };
        for (int i = 0; i < n; ++i) {
            long long gx = (long long)std::floor(C[i].pos.x / cell), gy = (long long)std::floor(C[i].pos.y / cell);
            g[key(gx, gy)].push_back(i);
        }
        for (int i = 0; i < n; ++i) {
            long long gx = (long long)std::floor(C[i].pos.x / cell), gy = (long long)std::floor(C[i].pos.y / cell);
            for (long long dx = -1; dx <= 1; ++dx)
                for (long long dy = -1; dy <= 1; ++dy) {
                    auto it = g.find(key(gx + dx, gy + dy));
                    if (it == g.end()) continue;
                    for (int j : it->second) {
                        if (j <= i || C[i].side != C[j].side) continue;
                        double d = dist(C[i].pos, C[j].pos);
                        if (d <= o.coincident_tol)
                            add(Severity::Error, CheckCode::Coincident, name_of(C[i], i) + " and " + name_of(C[j], j) + " at the same position", {i, j});
                        else if (d <= o.near_tol)
                            add(Severity::Warning, CheckCode::NearCoincident,
                                name_of(C[i], i) + " and " + name_of(C[j], j) + " only " + fmt(d) + " mm apart", {i, j});
                    }
                }
        }
    }

    // ---- overlap (sweep on bbox x) ----
    if (o.check_overlap) {
        struct E { int i; BBox b; OBB ob; };
        std::vector<E> es;
        for (int i = 0; i < n; ++i) {
            const Component& c = C[i];
            if (c.feature != Feature::Component) continue;
            if (c.dnp && !o.overlap_include_dnp) continue;
            if (!prj.package_of(c)) continue;      // unknown size: reported elsewhere
            es.push_back({i, prj.comp_bbox(c), comp_obb(prj, c, o.overlap_shrink)});
        }
        std::sort(es.begin(), es.end(), [](const E& a, const E& b) { return a.b.x0 < b.b.x0; });
        for (size_t a = 0; a < es.size(); ++a)
            for (size_t b = a + 1; b < es.size() && es[b].b.x0 <= es[a].b.x1; ++b) {
                const Component& ca = C[es[a].i];
                const Component& cb = C[es[b].i];
                if (ca.side != cb.side || !es[a].b.intersects(es[b].b)) continue;
                if (dist(ca.pos, cb.pos) <= o.coincident_tol) continue;   // already reported
                double pen = obb_overlap(es[a].ob, es[b].ob);
                if (pen > o.overlap_min_depth)
                    add(Severity::Warning, CheckCode::Overlap,
                        name_of(ca, es[a].i) + " overlaps " + name_of(cb, es[b].i) + " (" + fmt(pen, 2) + " mm)", {es[a].i, es[b].i});
            }
    }

    // ---- board frame ----
    // Locations are root-image coordinates and image transforms are rigid, so
    // testing against the root circuit frame covers every instance. Global
    // (panel) fiducials are tested against the panel frame instead.
    if (o.check_board) {
        if (prj.board_frame.empty() && prj.panel_frame.empty()) {
            add(Severity::Info, CheckCode::NoBoardFrame, "no board frame: outside-board check skipped", {});
        } else {
            auto closed_any = [](const std::vector<std::vector<Vec2>>& f) {
                for (auto& p : f) if (p.size() > 2 && dist(p.front(), p.back()) <= 0.05) return true;
                return false;
            };
            auto inside = [&](const std::vector<std::vector<Vec2>>& f, Vec2 p) {
                if (closed_any(f)) return point_in_frame(f, p);
                BBox bb; for (auto& poly : f) for (auto& q : poly) bb.add(q);   // open outline: use its extent
                return !bb.empty() && p.x >= bb.x0 - 1e-6 && p.x <= bb.x1 + 1e-6 && p.y >= bb.y0 - 1e-6 && p.y <= bb.y1 + 1e-6;
            };
            for (int i = 0; i < n; ++i) {
                const Component& c = C[i];
                const auto* fr = prj.frame_for(c);
                if (!fr) continue;
                const char* what = fr == &prj.panel_frame ? "panel frame" : prj.is_panel() ? "circuit frame" : "board frame";
                if (!inside(*fr, c.pos)) {
                    add(Severity::Error, CheckCode::OutsideBoard, name_of(c, i) + " centroid outside the " + what, {i});
                    continue;
                }
                if (c.feature != Feature::Component || !prj.package_of(c)) continue;
                BBox b = prj.comp_bbox(c);
                b.inflate(-o.overlap_shrink);
                if (!b.empty() && (!inside(*fr, {b.x0, b.y0}) || !inside(*fr, {b.x1, b.y0}) || !inside(*fr, {b.x1, b.y1}) || !inside(*fr, {b.x0, b.y1})))
                    add(Severity::Warning, CheckCode::PartlyOutsideBoard, name_of(c, i) + " body extends beyond the " + what, {i});
            }
        }
    }

    // ---- library / BOM ----
    bool has_place_points = false;
    for (auto& p : prj.model.points) has_place_points |= p.kind == PointKind::Placement || p.kind == PointKind::Manual;
    for (int i = 0; i < n; ++i) {
        const Component& c = C[i];
        if (c.feature != Feature::Component) continue;
        const std::string nm = name_of(c, i);
        if (c.package.empty()) add(Severity::Warning, CheckCode::MissingPackage, nm + ": no package", {i});
        else {
            auto it = prj.packages.find(c.package);
            if (it == prj.packages.end()) add(Severity::Warning, CheckCode::PackageNotInLibrary, nm + ": package " + c.package + " not in library", {i});
            else if (it->second.pads.empty()) add(Severity::Warning, CheckCode::ZeroPadPackage, nm + ": package " + c.package + " has no pads", {i});
        }
        const CompState st = comp_state(c);
        if (st == CompState::Orphan)
            add(Severity::Warning, CheckCode::OrphanNotInBom, nm + ": on the board but not in the BOM (reconcile; not treated as DNP)", {i});
        else if (st == CompState::Dnp && c.bom_state == BomState::Dnp)
            add(Severity::Info, CheckCode::DnpInBom, nm + ": DNP in the BOM (kept, not placed)", {i});
        if (c.number.empty()) {
            if (st != CompState::Dnp && st != CompState::Orphan)
                add(Severity::Warning, CheckCode::MissingPartNumber, nm + ": unassigned (no part number yet)", {i});
        } else {
            auto it = prj.parts.find(c.number);
            if (it == prj.parts.end()) add(Severity::Warning, CheckCode::PartNotInLibrary, nm + ": part number " + c.number + " not in library", {i});
            // Package mismatch only when the BOM supplied a package AND it disagrees (REVIEW_V9 §11G).
            if (!c.bom_package.empty() && !c.package.empty() && !packages_equivalent(c.bom_package, c.package))
                add(Severity::Warning, CheckCode::BomPackageDisagree,
                    nm + ": BOM lists package " + c.bom_package + ", location has " + c.package, {i});
            else if (c.bom_package.empty() && it != prj.parts.end() && !it->second.package.empty() && !c.package.empty() &&
                     !packages_equivalent(it->second.package, c.package))
                add(Severity::Info, CheckCode::PartPackageMismatch,
                    nm + ": part library lists package " + it->second.package + " for " + c.number + ", location has " + c.package +
                        " (no BOM package to confirm)", {i});
        }
        if (c.dnp && c.assigned_point >= 0)
            add(Severity::Warning, CheckCode::DnpAssigned, nm + ": DNP but assigned to a model point", {i});
        if (o.check_unassigned && has_place_points && !c.dnp && c.assigned_point < 0)
            add(Severity::Info, CheckCode::Unassigned, nm + ": not assigned to a model point", {i});
        double r = std::fmod(std::fabs(c.rot), 90.0);
        if (std::min(r, 90.0 - r) > o.rot_tol)
            add(Severity::Info, CheckCode::OffAxisRotation, nm + ": rotation " + fmt(c.rot, 2) + " is not a multiple of 90", {i});
    }

    // ---- packages whose artwork / lead pattern is rotated off-axis (imported rotated footprints) ----
    for (auto& kv : prj.packages) {
        const AxisInfo ai = pkg_axis_info(kv.second);
        if (!ai.off_axis) continue;
        std::vector<int> users = placements_of(prj, kv.first);
        if (users.empty()) continue;
        add(Severity::Warning, CheckCode::OffAxisPackage,
            "package " + kv.first + ": " + ai.why + " rotated " + fmt(ai.off90, 2) + " deg off-axis; placement angles are unreliable "
            "(normalise in the Package Library with 'compensate placements')", users);
    }

    // ---- fiducials (global, per side) ----
    for (int s = 0; s < 2; ++s) {
        std::vector<int> f;
        bool side_has_comps = false;
        for (int i = 0; i < n; ++i) {
            if ((int)C[i].side != s) continue;
            if (C[i].feature == Feature::Fiducial && C[i].fid_scope == FiducialScope::Global) f.push_back(i);
            if (C[i].feature == Feature::Component && !C[i].dnp) side_has_comps = true;
        }
        if (!side_has_comps) continue;
        const char* sn = s ? "bottom" : "top";
        if ((int)f.size() < o.min_fiducials) {
            add(Severity::Warning, CheckCode::FewFiducials,
                std::string(sn) + " side: " + std::to_string(f.size()) + " global fiducial(s), need " + std::to_string(o.min_fiducials), f);
            continue;
        }
        if (f.size() >= 3) {
            // farthest pair defines the line; max deviation of others
            size_t a = 0, b = 1;
            double best = -1;
            for (size_t i = 0; i < f.size(); ++i)
                for (size_t j = i + 1; j < f.size(); ++j) {
                    double d = dist(C[f[i]].pos, C[f[j]].pos);
                    if (d > best) { best = d; a = i; b = j; }
                }
            Vec2 p = C[f[a]].pos, q = C[f[b]].pos, u = q - p;
            double L = u.len(), dev = 0;
            if (L > 0)
                for (int k : f) {
                    Vec2 w = C[k].pos - p;
                    dev = std::max(dev, std::fabs(u.x * w.y - u.y * w.x) / L);
                }
            if (dev <= o.collinear_tol)
                add(Severity::Warning, CheckCode::CollinearFiducials, std::string(sn) + " side: fiducials are collinear", f);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const PlacementIssue& a, const PlacementIssue& b) { return a.severity > b.severity; });
    return out;
}

std::string describe_issues(const std::vector<PlacementIssue>& v) {
    std::ostringstream o;
    for (auto& i : v) o << severity_name(i.severity) << " [" << check_code_name(i.code) << "] " << i.message << "\n";
    return o.str();
}

int count_issues(const std::vector<PlacementIssue>& v, Severity s) {
    int k = 0;
    for (auto& i : v) k += i.severity == s;
    return k;
}

int fix_duplicate_refs(Project& prj, const std::string& sep) {
    std::set<std::string> used;
    for (auto& c : prj.comps) if (!c.ref.empty()) used.insert(upper(c.ref));
    std::set<std::string> seen;
    int changed = 0;
    for (auto& c : prj.comps) {
        if (c.ref.empty()) continue;
        std::string k = upper(c.ref);
        if (seen.insert(k).second) continue;
        for (int nidx = 2;; ++nidx) {
            std::string cand = c.ref + sep + std::to_string(nidx);
            if (!used.count(upper(cand))) {
                c.ref = cand;
                used.insert(upper(cand));
                seen.insert(upper(cand));
                ++changed;
                break;
            }
        }
    }
    if (changed) prj.dirty = true;
    return changed;
}

int snap_rotations(Project& prj, double step, double tol) {
    if (step <= 0) return 0;
    int changed = 0;
    for (auto& c : prj.comps) {
        double s = std::round(c.rot / step) * step;
        double r = norm_deg(s);
        if (std::fabs(c.rot - s) <= tol && c.rot != r) { c.rot = r; ++changed; }
    }
    if (changed) prj.dirty = true;
    return changed;
}

int sanitize_refs(Project& prj, const PlacementCheckOptions& o, char rep, bool up) {
    int changed = 0;
    for (auto& c : prj.comps) {
        std::string r = c.ref;
        for (auto& ch : r)
            if (!std::isalnum((unsigned char)ch) && o.legal_ref_chars.find(ch) == std::string::npos) ch = rep;
        if (up) r = upper(r);
        if (r != c.ref) { c.ref = r; ++changed; }
    }
    if (changed) prj.dirty = true;
    return changed;
}

int fill_packages_from_parts(Project& prj) {
    int changed = 0;
    for (auto& c : prj.comps) {
        if (!c.package.empty() || c.number.empty()) continue;
        auto it = prj.parts.find(c.number);
        if (it != prj.parts.end() && !it->second.package.empty()) { c.package = it->second.package; ++changed; }
    }
    if (changed) prj.dirty = true;
    return changed;
}

int unassign_dnp(Project& prj) {
    int changed = 0;
    for (auto& c : prj.comps) if (c.dnp && c.assigned_point >= 0) { c.assigned_point = -1; ++changed; }
    if (changed) prj.dirty = true;
    return changed;
}

} // namespace pcbp
