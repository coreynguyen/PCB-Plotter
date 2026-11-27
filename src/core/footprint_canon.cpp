// ============================================================================
// footprint_canon.cpp — see footprint_canon.h.
// ============================================================================
#include "footprint_canon.h"
#include "package_edit.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace pcbp {

namespace {
double rem90(double a) { return std::remainder(a, 90.0); }   // [-45, 45]
double r4(double v) { double r = std::round(v * 1e4) / 1e4; return r == 0 ? 0.0 : r; }   // no -0
Vec2 mirx(Vec2 v) { return {-v.x, v.y}; }
double ang_dist(double a, double b) { return std::fabs(std::remainder(a - b, 360.0)); }

// weighted median of (value, weight)
double wmedian(std::vector<std::pair<double, double>> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    double tot = 0;
    for (auto& e : v) tot += e.second;
    double acc = 0;
    for (auto& e : v) { acc += e.second; if (acc >= tot * 0.5) return e.first; }
    return v.back().first;
}

// mode of values in [-45, 45] (1-degree bins, wrap at +-45), mean of the winning bin
bool mode90(const std::vector<double>& v, double& out) {
    if (v.empty()) return false;
    std::map<int, std::vector<double>> bins;
    for (double r : v) { int b = (int)std::lround(r); if (b == -45) b = 45; bins[b].push_back(b == 45 && r < 0 ? r + 90 : r); }
    auto best = bins.begin();
    for (auto it = bins.begin(); it != bins.end(); ++it) if (it->second.size() > best->second.size()) best = it;
    double s = 0;
    for (double r : best->second) s += r;
    out = rem90(s / best->second.size());
    return true;
}

double coarse_angle(const std::vector<BoardPad>& pads, std::string& how) {
    // 1. rotation of pads whose orientation is observable (non-square, non-round)
    std::vector<double> rr, rs;
    for (auto& p : pads) {
        const bool iso = p.shape == PadShape::Round || p.shape == PadShape::Square || p.shape == PadShape::Octagon || std::fabs(p.w - p.h) < 0.01;
        (iso ? rs : rr).push_back(rem90(p.rot));
    }
    double a = 0;
    if (mode90(rr, a)) { how = "pad rotation"; return a; }
    // 2. nearest-neighbour directions of the pad centres (rows, grids, BGAs)
    if (pads.size() >= 2) {
        std::vector<double> nn;
        for (size_t i = 0; i < pads.size(); ++i) {
            double bd = 1e18; Vec2 bv{};
            for (size_t j = 0; j < pads.size(); ++j) {
                if (i == j) continue;
                const Vec2 v = pads[j].pos - pads[i].pos;
                const double d = v.x * v.x + v.y * v.y;
                if (d > 1e-8 && d < bd) { bd = d; bv = v; }
            }
            if (bd < 1e17) nn.push_back(rem90(rad2deg(std::atan2(bv.y, bv.x))));
        }
        if (mode90(nn, a)) { how = "pad centres"; return a; }
    }
    // 3. rotation recorded on square / round pads (padstack rotation = placement rotation in most CAD)
    if (mode90(rs, a)) { how = "pad rotation (square pads)"; return a; }
    how = "none";
    return 0;
}

// median deviation of near-axis pad-pair vectors after the coarse rotation
double fine_angle(const std::vector<BoardPad>& pads, double coarse) {
    std::vector<Vec2> c;
    for (auto& p : pads) c.push_back(rotate(p.pos, -coarse));
    std::vector<std::pair<double, double>> dev;
    const size_t n = c.size(), span = n > 200 ? 8 : n;   // big parts: neighbours in pin order only
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n && j <= i + span; ++j) {
            const Vec2 v = c[j] - c[i];
            const double L = std::hypot(v.x, v.y);
            if (L < 0.2) continue;
            const double d = rem90(rad2deg(std::atan2(v.y, v.x)));
            if (std::fabs(d) < 3.0) dev.push_back({d, L});
        }
    return wmedian(dev);
}

// placement-frame position of a board point for rotation th (bottom: comp_xform mirrors X)
Vec2 to_local(Vec2 w, Vec2 g, double th, Side s) { const Vec2 v = rotate(w - g, -th); return s == Side::Bottom ? mirx(v) : v; }
Vec2 to_board(Vec2 l, double th, Side s) { return rotate(s == Side::Bottom ? mirx(l) : l, th); }

CanonFootprint canon_impl(const std::vector<BoardPad>& pads, Side side, bool quarter) {
    CanonFootprint out;
    if (pads.empty()) return out;
    out.coarse = coarse_angle(pads, out.how);
    out.fine = pads.size() >= 2 ? fine_angle(pads, out.coarse) : 0.0;
    const double th0 = out.coarse + out.fine;
    Vec2 g{};
    for (auto& p : pads) g += p.pos;
    g = g * (1.0 / pads.size());
    // pin 1: flagged pad, else pin number 1, else the first pad
    int p1 = -1;
    for (size_t i = 0; i < pads.size() && p1 < 0; ++i) if (pads[i].pin1) p1 = (int)i;
    for (size_t i = 0; i < pads.size() && p1 < 0; ++i) if (pads[i].pin == 1) p1 = (int)i;
    out.pin1 = p1;
    const int key = p1 >= 0 ? p1 : 0;
    double th = th0;
    if (quarter && pads.size() >= 2) {
        const double target = pads.size() >= 3 ? 135.0 : 180.0;
        double best = 1e9;
        for (int k = 0; k < 4; ++k) {
            const double t = th0 + 90.0 * k;
            const Vec2 l = to_local(pads[key].pos, g, t, side);
            if (std::hypot(l.x, l.y) < 1e-3) continue;   // pin 1 at the centre: no information
            const double s = ang_dist(rad2deg(std::atan2(l.y, l.x)), target);
            if (s < best - 1e-6) { best = s; th = t; }
        }
    }
    // local pads, then re-centre on the pad extent
    BBox bb;
    for (auto& p : pads) {
        Pad q;
        q.pos = to_local(p.pos, g, th, side);
        q.w = p.w; q.h = p.h; q.drill = p.drill; q.shape = p.shape; q.pin = p.pin;
        q.rot = side == Side::Bottom ? th - p.rot : p.rot - th;
        // IPC-356 pad rotations are whole degrees while the placement angle is finer (e.g. 44.955):
        // a local pad rotation within 1 deg of a quarter turn is that quarter turn
        if (std::fabs(rem90(q.rot)) < 1.0) q.rot = norm_deg(std::round(q.rot / 90.0) * 90.0);
        const double r = deg2rad(q.rot), cs = std::fabs(std::cos(r)), sn = std::fabs(std::sin(r));
        const Vec2 hs{q.w * 0.5 * cs + q.h * 0.5 * sn, q.w * 0.5 * sn + q.h * 0.5 * cs};
        bb.add(q.pos - hs); bb.add(q.pos + hs);
        out.pads.push_back(q);
    }
    const Vec2 cc = bb.center();
    for (auto& q : out.pads) { q.pos = q.pos - cc; canon_pad(q); }
    out.pos = g + to_board(cc, th, side);
    out.rot = norm_deg(std::round(th * 1000.0) / 1000.0);
    if (out.rot >= 360.0 - 1e-9) out.rot = 0;
    return out;
}

bool generated(const Package& p) { return !p.shape_confirmed && (p.body.empty() || p.body_source == "pads"); }
} // namespace

void canon_pad(Pad& p) {
    double r = norm_deg(p.rot);
    const bool quad = p.shape == PadShape::Square || p.shape == PadShape::Octagon || (p.shape == PadShape::Round && std::fabs(p.w - p.h) < 1e-9);
    r = std::fmod(r, quad ? 90.0 : 180.0);
    if (!quad && std::fabs(r - 90.0) < 0.01) { std::swap(p.w, p.h); r = 0; }
    if (r < 0.01 || r > (quad ? 89.99 : 179.99)) r = 0;
    p.rot = std::round(r * 1000.0) / 1000.0;
    p.pos = {r4(p.pos.x), r4(p.pos.y)};
    p.w = r4(p.w); p.h = r4(p.h);
}

CanonFootprint canon_footprint(const std::vector<BoardPad>& pads, Side side) { return canon_impl(pads, side, true); }

bool same_pad_pattern(const std::vector<Pad>& a, const std::vector<Pad>& b, double pos_tol, double size_tol) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const Pad &x = a[i], &y = b[i];
        if (x.pin != y.pin || x.shape != y.shape) return false;
        if (std::fabs(x.pos.x - y.pos.x) > pos_tol || std::fabs(x.pos.y - y.pos.y) > pos_tol) return false;
        if (std::fabs(x.w - y.w) > size_tol || std::fabs(x.h - y.h) > size_tol || std::fabs(x.drill - y.drill) > size_tol) return false;
        const bool quad = x.shape == PadShape::Square || x.shape == PadShape::Octagon || std::fabs(x.w - x.h) < 1e-9;
        const double per = quad ? 90.0 : 180.0;   // pad symmetry
        if (std::fabs(std::remainder(x.rot - y.rot, per)) > 0.1) return false;
    }
    return true;
}

NormaliseReport normalise_packages(Project& prj, const NormaliseOptions& o) {
    NormaliseReport rep;
    std::vector<std::string> names;
    for (auto& kv : prj.packages)
        if (o.only_prefix.empty() || kv.first.rfind(o.only_prefix, 0) == 0) names.push_back(kv.first);
    for (const std::string& nm : names) {
        Package& pk = prj.packages[nm];
        if (pk.pads.empty()) continue;
        if (o.only_off_axis && !pkg_axis_info(pk).off_axis) continue;
        std::vector<BoardPad> bp;
        for (auto& d : pk.pads) bp.push_back({d.pos, d.w, d.h, d.rot, d.drill, d.shape, d.pin, false});
        const CanonFootprint cf = canon_impl(bp, Side::Top, o.quarter);   // package frame, as a top-side "board"
        double th = std::remainder(cf.rot, 360.0);
        if (std::fabs(rem90(th)) < o.tol_deg && !o.quarter) th = std::round(th / 90.0) * 90.0;
        const bool gen = generated(pk);
        const bool turn = std::fabs(std::remainder(th, 360.0)) > 1e-6;
        // re-centring is only done for generated packages (a CAD package keeps its library origin)
        const bool move = (gen || o.quarter) && std::hypot(cf.pos.x, cf.pos.y) > 1e-4;
        if (!turn && !move) {
            if (gen) for (auto& d : pk.pads) canon_pad(d);
            continue;
        }
        PkgEditOpts eo; eo.confirmed = true; eo.compensate = true;
        int comp = 0;
        if (move) comp = std::max(comp, pkg_set_origin(prj, nm, cf.pos, eo).compensated);
        if (turn) comp = std::max(comp, pkg_rotate(prj, nm, -th, eo).compensated);
        Package& p2 = prj.packages[nm];
        // generated: the exact canonical pads (frame = cf.pos, which is the origin now or within 1e-4 mm of it)
        if ((move || gen) && p2.pads.size() == cf.pads.size()) p2.pads = cf.pads;
        else for (auto& d : p2.pads) {   // CAD package rotated about its library origin: clean the rotation residue only
            const double q = std::round(d.rot / 90.0) * 90.0;
            if (std::fabs(d.rot - q) < 0.01) d.rot = norm_deg(q);
            d.pos = {r4(d.pos.x), r4(d.pos.y)};
        }
        if (gen) { p2.auto_body(); ++rep.bodies; }
        ++rep.packages;
        rep.placements += comp;
    }
    if (o.merge) {
        // identical generated packages -> the first by name
        std::vector<std::string> keep;
        std::map<std::string, std::string> to;
        for (auto& kv : prj.packages) {
            if (!(o.only_prefix.empty() || kv.first.rfind(o.only_prefix, 0) == 0) || !generated(kv.second)) continue;
            bool merged = false;
            for (auto& k : keep) {
                const Package& a = prj.packages[k];
                if (a.smt == kv.second.smt && same_pad_pattern(a.pads, kv.second.pads)) { to[kv.first] = k; merged = true; break; }
            }
            if (!merged) keep.push_back(kv.first);
        }
        if (!to.empty()) {
            for (auto& c : prj.comps) { auto it = to.find(c.package); if (it != to.end()) c.package = it->second; }
            for (auto& kv : prj.parts) { auto it = to.find(kv.second.package); if (it != to.end()) kv.second.package = it->second; }
            for (auto& kv : to) prj.packages.erase(kv.first);
            rep.merged = (int)to.size();
        }
    }
    if (rep.packages || rep.merged) prj.dirty = true;
    return rep;
}

NormaliseReport normalise_imported_packages(Project& prj) {
    NormaliseOptions a; a.quarter = true; a.merge = true; a.only_prefix = "IPC356_";
    NormaliseReport r = normalise_packages(prj, a);
    NormaliseOptions b; b.quarter = false; b.merge = false; b.only_off_axis = true;
    NormaliseReport r2 = normalise_packages(prj, b);   // other off-axis packages: residue only
    r.packages += r2.packages; r.placements += r2.placements; r.bodies += r2.bodies;
    return r;
}

} // namespace pcbp
