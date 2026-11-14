// ============================================================================
// arrays.cpp — R22 outline / array API (see arrays.h, specs/PANELIZE.md, specs/OUTLINES.md)
// ============================================================================
#include "arrays.h"
#include <algorithm>
#include <cmath>

namespace pcbp {

double units_to_mm(double v, Units u) { return u == Units::Inch ? v * 25.4 : u == Units::Mil ? v * 0.0254 : v; }
double mm_to_units(double mm, Units u) { return u == Units::Inch ? mm / 25.4 : u == Units::Mil ? mm / 0.0254 : mm; }

bool outline_exists(const Project& p, OutlineKind k) { return k == OutlineKind::Board ? !p.panel_frame.empty() : !p.board_frame.empty(); }

static double poly_area(const std::vector<Vec2>& v) {
    double a = 0;
    for (size_t i = 0, j = v.size() ? v.size() - 1 : 0; i < v.size(); j = i++) a += v[j].x * v[i].y - v[i].x * v[j].y;
    return std::fabs(a) * 0.5;
}

bool commit_outline(Project& p, OutlineKind k, const std::vector<std::vector<Vec2>>& polys, OutlineSource src, bool from_gerber, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    if (src == OutlineSource::Inferred) return fail("an outline committed by the user cannot be Inferred");
    std::vector<std::vector<Vec2>> in;
    for (auto& pl : polys) {
        std::vector<Vec2> q;
        for (auto& v : pl) if (std::isfinite(v.x) && std::isfinite(v.y)) q.push_back(v);
        if (q.size() >= 2) in.push_back(std::move(q));
    }
    if (in.empty()) return fail("no outline geometry");
    auto joined = join_frame_segments(in);
    BBox b;
    double area = 0;
    for (auto& pl : joined) {
        for (auto& v : pl) b.add(v);
        if (pl.size() >= 3) area = std::max(area, poly_area(pl));
    }
    if (b.empty() || b.w() < 0.01 || b.h() < 0.01) return fail("the outline has no area");
    (void)area;
    if (k == OutlineKind::Board) { p.panel_frame = std::move(joined); p.panel_outline_src = src; p.panel_frame_from_gerber = from_gerber; }
    else { p.board_frame = std::move(joined); p.board_outline_src = src; p.board_frame_from_gerber = from_gerber; }
    p.dirty = true;
    return true;
}

bool commit_outline_rect(Project& p, OutlineKind k, const BBox& r, OutlineSource src, bool from_gerber, std::string* err) {
    if (r.empty() || !(r.w() >= 0.01) || !(r.h() >= 0.01)) { if (err) *err = "the outline has no area"; return false; }
    return commit_outline(p, k, {{{r.x0, r.y0}, {r.x1, r.y0}, {r.x1, r.y1}, {r.x0, r.y1}, {r.x0, r.y0}}}, src, from_gerber, err);
}

bool delete_outline(Project& p, OutlineKind k) {
    if (!outline_exists(p, k)) return false;
    if (k == OutlineKind::Board) { p.panel_frame.clear(); p.panel_frame_from_gerber = false; p.panel_outline_src = OutlineSource::Inferred; }
    else { p.board_frame.clear(); p.board_frame_from_gerber = false; p.board_outline_src = OutlineSource::Inferred; }
    p.dirty = true;
    return true;
}

bool confirm_inferred_outline(Project& p) {
    bool any = false;
    if (!p.board_frame.empty() && p.board_outline_src == OutlineSource::Inferred) { p.board_outline_src = OutlineSource::Confirmed; any = true; }
    if (!p.panel_frame.empty() && p.panel_outline_src == OutlineSource::Inferred) { p.panel_outline_src = OutlineSource::Confirmed; any = true; }
    if (!any && p.board_frame.empty() && p.panel_frame.empty()) {
        // no outline at all: the inferred one is the live placement extent (+2 mm); freeze it, then confirm
        p.infer_circuit_outline();
        if (!p.board_frame.empty()) { p.board_outline_src = OutlineSource::Confirmed; any = true; }
    }
    if (any) p.dirty = true;
    return any;
}

bool has_array(const Project& p) { return p.images.size() > 1; }
int array_instance_count(const Project& p) { return std::max<int>(1, (int)p.images.size()); }

// images[0] is the reference circuit; its geometry is always identity (Project::image_xform(0)), but its
// metadata (name, legacy rot used for machine-angle comparison) is kept.
static PanelImage reference_image(const Project& p) {
    PanelImage r = p.images.empty() ? PanelImage{} : p.images[0];
    if (r.name.empty()) r.name = "1";
    r.root = true; r.offset = {}; r.x_out = false; r.x_index = r.y_index = 0;
    return r;
}

bool create_linear_array(Project& p, int nx, int ny, double px, double py, int rc, int rr, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    if (nx < 1 || ny < 1) return fail("counts must be 1 or more");
    if ((long long)nx * ny > 2000) return fail("at most 2000 circuits");
    if (nx * ny < 2) return fail("X count or Y count must be greater than 1");
    if (!std::isfinite(px) || !std::isfinite(py)) return fail("pitch is not a number");
    if ((nx > 1 && std::fabs(px) < 1e-6) || (ny > 1 && std::fabs(py) < 1e-6)) return fail("pitch must not be 0");
    if (rc < 0 || rc >= nx || rr < 0 || rr >= ny) return fail("the reference circuit cell must be inside the grid");
    std::vector<PanelImage> out;
    PanelImage root = reference_image(p);
    root.x_index = rc; root.y_index = rr;
    out.push_back(root);
    int n = 2;
    for (int x = 0; x < nx; ++x)
        for (int y = 0; y < ny; ++y) {
            if (x == rc && y == rr) continue;
            PanelImage im;
            im.name = std::to_string(n++);
            im.root = false;
            im.offset = {(x - rc) * px, (y - rr) * py};
            im.x_index = x; im.y_index = y;
            out.push_back(im);
        }
    p.images = std::move(out);
    p.dirty = true;
    return true;
}

bool create_nonlinear_array(Project& p, const std::vector<ArrayInstance>& inst, std::string* err) {
    std::vector<PanelImage> out{reference_image(p)};
    bool auto_idx = true;
    for (auto& a : inst) auto_idx &= a.x_index == 0 && a.y_index == 0;
    for (auto& a : inst) {
        if (!std::isfinite(a.offset.x) || !std::isfinite(a.offset.y) || !std::isfinite(a.rot)) { if (err) *err = "instance offset or rotation is not a number"; return false; }
        const double r = norm_deg(a.rot);
        const bool ident = std::hypot(a.offset.x, a.offset.y) < 1e-6 && (std::fabs(r) < 1e-6 || std::fabs(r - 360.0) < 1e-6);
        if (ident) continue;
        PanelImage im;
        im.root = false;
        im.offset = a.offset;
        im.rot = r;
        im.x_out = a.x_out;
        im.x_index = auto_idx ? (int)out.size() : a.x_index;
        im.y_index = auto_idx ? 0 : a.y_index;
        im.name = std::to_string(out.size() + 1);
        out.push_back(im);
    }
    if (out.size() < 2) { if (err) *err = "no instance besides the reference circuit"; return false; }
    p.images = std::move(out);
    p.dirty = true;
    return true;
}

bool delete_array(Project& p) {
    if (!has_array(p)) return false;
    p.images = {reference_image(p)};
    p.dirty = true;
    return true;
}

// ---- migration -----------------------------------------------------------------------------
static bool axis_rect(const std::vector<std::vector<Vec2>>& fr, BBox& out) {
    if (fr.size() != 1) return false;
    const auto& pl = fr[0];
    if (pl.size() != 4 && pl.size() != 5) return false;
    BBox f; for (auto& q : pl) f.add(q);
    for (auto& q : pl) {
        const bool ex = std::fabs(q.x - f.x0) < 1e-6 || std::fabs(q.x - f.x1) < 1e-6;
        const bool ey = std::fabs(q.y - f.y0) < 1e-6 || std::fabs(q.y - f.y1) < 1e-6;
        if (!ex || !ey) return false;
    }
    out = f;
    return !f.empty();
}

bool panel_frame_is_generated(const Project& p) {
    BBox F;
    if (!axis_rect(p.panel_frame, F)) return false;
    std::vector<BBox> bases;
    if (!p.board_frame.empty()) bases.push_back(p.circuit_frame_bbox());
    bases.push_back(p.placement_extents(2.0));
    bases.push_back(p.placement_extents(0.0));
    for (const BBox& b0 : bases) {
        if (b0.empty()) continue;
        BBox all;
        for (int i = 0; i < (int)p.images.size(); ++i) {
            const Xform t = p.image_xform(i);
            all.add(t.apply({b0.x0, b0.y0})); all.add(t.apply({b0.x1, b0.y0}));
            all.add(t.apply({b0.x1, b0.y1})); all.add(t.apply({b0.x0, b0.y1}));
        }
        if (all.empty()) continue;
        const double m[4] = {all.x0 - F.x0, all.y0 - F.y0, F.x1 - all.x1, F.y1 - all.y1};
        const double t = 0.01;
        if (m[0] < -t || m[0] > 50.0) continue;
        if (std::fabs(m[1] - m[0]) < t && std::fabs(m[2] - m[0]) < t && std::fabs(m[3] - m[0]) < t) return true;
    }
    return false;
}

std::vector<std::string> migrate_array_outlines(Project& p) {
    std::vector<std::string> notes;
    if (p.is_panel() && !p.panel_frame.empty() && p.panel_outline_src == OutlineSource::Confirmed && !p.panel_frame_from_gerber &&
        panel_frame_is_generated(p)) {
        p.panel_outline_src = OutlineSource::Inferred;
        notes.push_back("board outline was generated around the circuits (circuit extents + margin): marked Inferred, confirm or define the measured board");
    }
    if (p.is_panel() && !p.board_frame.empty() && !p.panel_frame.empty()) {
        BBox a, b;
        if (axis_rect(p.board_frame, a) && axis_rect(p.panel_frame, b) && std::fabs(a.x0 - b.x0) < 0.01 && std::fabs(a.y0 - b.y0) < 0.01 &&
            std::fabs(a.x1 - b.x1) < 0.01 && std::fabs(a.y1 - b.y1) < 0.01) {
            p.board_frame.clear();
            p.board_frame_from_gerber = false;
            p.board_outline_src = OutlineSource::Inferred;
            notes.push_back("circuit outline was a copy of the board outline (every circuit instance would be the whole board): removed, define the circuit outline");
        }
    }
    if (!notes.empty()) p.dirty = true;
    return notes;
}

} // namespace pcbp
