// package_edit.cpp — see package_edit.h
#include "package_edit.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <map>
#include <cstring>

namespace pcbp {

const char* orient_class_name(OrientClass c) {
    switch (c) {
    case OrientClass::Polarized: return "Polarized";
    case OrientClass::Ambiguous: return "Rotationally ambiguous";
    case OrientClass::Symmetric: return "Symmetric";
    default: return "Auto";
    }
}
const char* marker_kind_name(MarkerKind k) {
    switch (k) {
    case MarkerKind::Dot: return "Dot";
    case MarkerKind::Arrow: return "Arrow";
    case MarkerKind::Custom: return "Custom artwork";
    default: return "None";
    }
}

// R15: lead / orientation / marker geometry helpers moved to project.cpp (core, linked everywhere) together
// with the polarity-marker model (specs/POLARITY.md).
static bool pattern_maps(const Package& p, Vec2 c, double deg, double tol) {
    for (auto& a : p.pads) {
        Vec2 r = rotate_about(a.pos, c, deg);
        bool hit = false;
        for (auto& b : p.pads) if (dist(r, b.pos) < tol) { hit = true; break; }
        if (!hit) return false;
    }
    return true;
}

// ---- off-axis detector ------------------------------------------------------
static double fold90(double a) {   // -> (-45, 45]
    a = std::fmod(a, 90.0);
    if (a > 45) a -= 90;
    if (a <= -45) a += 90;
    return a;
}
static void edge_angles(const GPrim& g, std::vector<double>& out) {
    if (g.kind == GKind::Rect) { out.push_back(g.rot); return; }
    if (g.kind == GKind::Group) { for (auto& c : g.children) edge_angles(c, out); return; }
    if (g.kind == GKind::Line || g.kind == GKind::Polyline || g.kind == GKind::Polygon)
        for (size_t i = 1; i < g.pts.size(); ++i) {
            Vec2 d = g.pts[i] - g.pts[i - 1];
            if (std::hypot(d.x, d.y) > 0.2) out.push_back(rad2deg(std::atan2(d.y, d.x)));
        }
}
static double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

AxisInfo pkg_axis_info(const Package& p, double tol) {
    AxisInfo r;
    std::vector<double> lead;
    // nearest-neighbour directions of the lead pattern (pitch / span direction)
    for (size_t i = 0; i < p.pads.size(); ++i) {
        double bd = 1e300; Vec2 bv{};
        for (size_t j = 0; j < p.pads.size(); ++j) {
            if (i == j) continue;
            Vec2 v = p.pads[j].pos - p.pads[i].pos;
            double d = std::hypot(v.x, v.y);
            if (d > 1e-6 && d < bd) { bd = d; bv = v; }
        }
        if (bd < 1e299) lead.push_back(fold90(rad2deg(std::atan2(bv.y, bv.x))));
    }
    std::vector<double> padrot;
    for (auto& d : p.pads) if (std::fabs(d.w - d.h) > 1e-3) padrot.push_back(fold90(d.rot));
    std::vector<double> body;
    for (auto& g : p.body) edge_angles(g, body);
    for (auto& a : body) a = fold90(a);

    // principal axis of the lead positions (only meaningful when the pattern is anisotropic)
    bool aniso = false; double cov_axis = 0;
    if (p.pads.size() >= 2) {
        const Vec2 c = pkg_lead_centroid(p);
        double sxx = 0, syy = 0, sxy = 0;
        for (auto& d : p.pads) { Vec2 v = d.pos - c; sxx += v.x * v.x; syy += v.y * v.y; sxy += v.x * v.y; }
        const double l = std::hypot(sxx - syy, 2 * sxy), tr = sxx + syy;
        aniso = tr > 1e-12 && l > 0.15 * tr;
        cov_axis = fold90(rad2deg(0.5 * std::atan2(2 * sxy, sxx - syy)));
    }
    if (!lead.empty()) {
        r.off90 = median(lead); r.why = "lead pattern";
        // staggered rows / SOT-like triangles: nearest-neighbour direction is diagonal while the
        // pattern itself is on-axis -> trust the principal axis when the two disagree
        // -> require (a) most nearest-neighbour directions agree and (b) the principal axis agrees too
        int agree = 0;
        for (double a : lead) if (std::fabs(fold90(a - r.off90)) < 2) ++agree;
        const bool consistent = agree * 10 >= (int)lead.size() * 6;
        if (!consistent || (aniso && std::fabs(fold90(r.off90 - cov_axis)) > 3)) { r.off90 = 0; r.why = "lead pattern (inconclusive)"; }
    }
    else if (!padrot.empty()) { r.off90 = median(padrot); r.why = "pad rotation"; }
    else if (!body.empty()) { r.off90 = median(body); r.why = "artwork"; }
    else return r;
    // body artwork disagreeing with leads is also worth flagging
    if (std::fabs(r.off90) <= tol && !body.empty() && std::fabs(median(body)) > tol && !lead.empty()) {
        r.off90 = median(body); r.why = "artwork (leads on-axis)";
    }
    if (std::fabs(r.off90) <= tol && !padrot.empty() && std::fabs(median(padrot)) > tol) {
        r.off90 = median(padrot); r.why = "pad rotation";
    }
    r.angle = std::fmod(r.off90 + 180.0, 180.0);
    r.off_axis = std::fabs(r.off90) > tol;
    return r;
}

// ---- placements ---------------------------------------------------------------
std::vector<int> placements_of(const Project& prj, const std::string& pkg) {
    std::vector<int> v;
    for (size_t i = 0; i < prj.comps.size(); ++i) {
        const Component& c = prj.comps[i];
        const Package* p = prj.package_of(c);
        if (p && p->name == pkg) v.push_back((int)i);
        else if (!p && c.package == pkg) v.push_back((int)i);
    }
    return v;
}
int count_placements(const Project& prj, const std::string& pkg) { return (int)placements_of(prj, pkg).size(); }
std::string shared_warning(const Project& prj, const std::string& pkg) {
    const int n = count_placements(prj, pkg);
    if (!n) return {};
    return "This changes " + std::to_string(n) + " placement" + (n == 1 ? "" : "s") + " of " + pkg + " (shared)";
}

std::string shared_warning_many(const Project& prj, const std::vector<std::string>& pkgs) {
    if (pkgs.size() == 1) return shared_warning(prj, pkgs[0]);
    int n = 0, used = 0;
    for (auto& k : pkgs) { const int c = count_placements(prj, k); n += c; used += c > 0; }
    if (!n) return {};
    return "This changes " + std::to_string(n) + " placement" + (n == 1 ? "" : "s") + " of " + std::to_string(used) + " shared package" +
           (used == 1 ? "" : "s");
}

bool pkg_detect_th(const Package& p) {
    for (auto& d : p.pads) if (d.drill > 0) return true;
    return false;
}

// ---- geometry -------------------------------------------------------------------
static void rot_prim(GPrim& g, double deg, bool ortho) {
    switch (g.kind) {
    case GKind::Rect:
        if (g.pts.size() >= 2) {
            if (ortho) {
                Vec2 c = (g.pts[0] + g.pts[1]) * 0.5, h = (g.pts[1] - g.pts[0]) * 0.5;
                const long q = std::lround(deg / 90.0);
                if (q % 2) std::swap(h.x, h.y);     // odd quarter turn: box extents swap, own rot kept
                Vec2 c2 = rotate(c, deg);
                g.pts[0] = c2 - h; g.pts[1] = c2 + h;
            } else {
                // arbitrary angle: become a polygon (exact geometry, no reliance on Rect.rot)
                Vec2 a = g.pts[0], b = g.pts[1], c = (a + b) * 0.5;
                std::vector<Vec2> q = {{a.x, a.y}, {b.x, a.y}, {b.x, b.y}, {a.x, b.y}};
                for (auto& v : q) v = rotate(rotate_about(v, c, g.rot), deg);
                g.kind = GKind::Polygon; g.pts = q; g.rot = 0;
            }
        }
        break;
    case GKind::Circle: case GKind::Arc:
        if (!g.pts.empty()) g.pts[0] = rotate(g.pts[0], deg);
        if (g.kind == GKind::Arc) g.a0 = norm_deg(g.a0 + deg);
        break;
    case GKind::Text:
        if (!g.pts.empty()) g.pts[0] = rotate(g.pts[0], deg);
        g.rot = norm_deg(g.rot + deg);
        break;
    case GKind::Group: for (auto& c : g.children) rot_prim(c, deg, ortho); break;
    default: for (auto& p : g.pts) p = rotate(p, deg); break;
    }
}
static void off_prim(GPrim& g, Vec2 d) {
    for (auto& p : g.pts) p += d;
    for (auto& c : g.children) off_prim(c, d);
}

void pkg_geom_rotate(Package& p, double deg) {
    const bool ortho = std::fabs(std::remainder(deg, 90.0)) < 1e-9;
    for (auto& d : p.pads) {
        d.pos = rotate(d.pos, deg);
        d.rot = norm_deg(d.rot + deg);
        if (ortho && std::fabs(d.rot) < 1e-9) d.rot = 0;
    }
    for (auto& g : p.body) rot_prim(g, deg, ortho);
    for (auto& g : p.marker_art) rot_prim(g, deg, ortho);
    if (p.marker_user_pos) p.marker_pos = rotate(p.marker_pos, deg);
    p.marker_rot = norm_deg(p.marker_rot + deg);   // R15: arrow turns with the artwork
}
void pkg_geom_offset(Package& p, Vec2 o) {
    for (auto& d : p.pads) d.pos -= o;
    for (auto& g : p.body) off_prim(g, Vec2{} - o);
    for (auto& g : p.marker_art) off_prim(g, Vec2{} - o);
    if (p.marker_user_pos) p.marker_pos -= o;
}

// local linear part of a placement: R(rot) * (bottom ? mirror X : I)
static Vec2 local_vec(double rot, Side s, Vec2 v) {
    if (s == Side::Bottom) v.x = -v.x;
    return rotate(v, rot);
}

static bool gate(const Project& prj, const std::string& pkg, const PkgEditOpts& o, PkgEditResult& r, Package*& out, Project& mp) {
    auto it = mp.packages.find(pkg);
    if (it == mp.packages.end()) { r.message = "Package " + pkg + " not found"; return false; }
    r.affected = count_placements(prj, pkg);
    if (r.affected > 0 && !o.confirmed) { r.needs_confirm = true; r.message = shared_warning(prj, pkg); return false; }
    out = &it->second;
    return true;
}

PkgEditResult pkg_rotate(Project& prj, const std::string& pkg, double deg, const PkgEditOpts& o) {
    PkgEditResult r; Package* p = nullptr;
    if (!gate(prj, pkg, o, r, p, prj)) return r;
    if (std::fabs(deg) < 1e-12) { r.message = "Nothing to rotate"; return r; }
    pkg_geom_rotate(*p, deg);
    if (o.compensate)
        for (int i : placements_of(prj, pkg)) {
            Component& c = prj.comps[i];
            const double d = c.side == Side::Bottom ? deg : -deg;
            c.rot = norm_deg(c.rot + d);
            c.rot_imported = norm_deg(c.rot_imported + d);
            ++r.compensated;
        }
    prj.dirty = true;
    r.applied = true;
    r.message = "Package " + pkg + " rotated " + std::to_string(deg) + " deg" +
                (r.affected ? (o.compensate ? "; " + std::to_string(r.compensated) + " placements compensated (board unchanged)"
                                            : "; " + std::to_string(r.affected) + " placements now drawn rotated") : "");
    return r;
}

PkgEditResult pkg_set_origin(Project& prj, const std::string& pkg, Vec2 at, const PkgEditOpts& o) {
    PkgEditResult r; Package* p = nullptr;
    if (!gate(prj, pkg, o, r, p, prj)) return r;
    if (std::hypot(at.x, at.y) < 1e-9) { r.message = "Origin unchanged"; return r; }
    pkg_geom_offset(*p, at);
    if (o.compensate)
        for (int i : placements_of(prj, pkg)) {
            Component& c = prj.comps[i];
            const Vec2 d = local_vec(c.rot, c.side, at);
            c.pos += d;
            c.label_off -= d;
            c.cad_pos += local_vec(c.rot_imported, c.side, at);
            ++r.compensated;
        }
    prj.dirty = true;
    r.applied = true;
    r.message = "Origin of " + pkg + " moved" + (r.affected ? (o.compensate ? "; " + std::to_string(r.compensated) + " placements compensated (pads unchanged on board)"
                                                                          : "; " + std::to_string(r.affected) + " placements shifted on board") : "");
    return r;
}

PkgEditResult pkg_set_pins(Project& prj, const std::string& pkg, const std::vector<int>& pins, const PkgEditOpts& o) {
    PkgEditResult r; Package* p = nullptr;
    if (!gate(prj, pkg, o, r, p, prj)) return r;
    if (pins.size() != p->pads.size()) { r.message = "Pin list size mismatch"; return r; }
    std::multiset<int> a(pins.begin(), pins.end()), b, seq;
    for (auto& d : p->pads) b.insert(d.pin);
    for (int i = 1; i <= (int)pins.size(); ++i) seq.insert(i);
    if (a != b && a != seq) { r.message = "Pin list is not a permutation"; return r; }
    for (size_t i = 0; i < pins.size(); ++i) p->pads[i].pin = pins[i];
    prj.dirty = true;
    r.applied = true;
    r.message = "Leads of " + pkg + " renumbered" + (r.affected ? " (" + std::to_string(r.affected) + " placements share it)" : "");
    return r;
}

std::vector<int> pins_cyclic_from(const Package& p, int pad) {
    const int n = (int)p.pads.size();
    std::vector<int> order(n);
    for (int i = 0; i < n; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        int pa = p.pads[a].pin > 0 ? p.pads[a].pin : 1 << 30, pb = p.pads[b].pin > 0 ? p.pads[b].pin : 1 << 30;
        return pa < pb;
    });
    int k = 0;
    for (int i = 0; i < n; ++i) if (order[i] == pad) k = i;
    std::vector<int> pins(n);
    for (int i = 0; i < n; ++i) pins[order[(k + i) % n]] = i + 1;
    return pins;
}

PkgEditResult pkg_replace(Project& prj, const std::string& pkg, const Package& nv, const PkgEditOpts& o) {
    PkgEditResult r; Package* p = nullptr;
    if (!gate(prj, pkg, o, r, p, prj)) return r;
    *p = nv;
    p->name = pkg;
    prj.dirty = true;
    r.applied = true;
    r.message = "Package " + pkg + " updated";
    return r;
}

// ---- pin ordering session ----------------------------------------------------------
void PinOrderSession::begin(const Package& p) {
    original.clear();
    for (auto& d : p.pads) original.push_back(d.pin);
    work = original;
    done.assign(p.pads.size(), false);
    next = 1;
    active = true;
}
bool PinOrderSession::click(int pad) {
    if (!active || pad < 0 || pad >= (int)work.size() || done[pad]) return complete();
    work[pad] = next++;
    done[pad] = true;
    return complete();
}
bool PinOrderSession::complete() const {
    for (bool b : done) if (!b) return false;
    return !done.empty();
}
std::vector<int> PinOrderSession::result() const {
    std::vector<int> r = work;
    // unclicked pads: continue numbering in their original relative order
    std::vector<int> rest;
    for (size_t i = 0; i < done.size(); ++i) if (!done[i]) rest.push_back((int)i);
    std::stable_sort(rest.begin(), rest.end(), [&](int a, int b) {
        int pa = original[a] > 0 ? original[a] : 1 << 30, pb = original[b] > 0 ? original[b] : 1 << 30;
        return pa < pb;
    });
    int k = next;
    for (int i : rest) r[i] = k++;
    return r;
}

// ---- instance ops ---------------------------------------------------------------------
void placement_rotate_local(Component& c, double d) {
    const double w = c.side == Side::Bottom ? -d : d;
    c.rot = norm_deg(c.rot + w);
}

double placement_assign_pin_one(Component& c, const Package& p, int k, std::string* msg) {
    const int i1 = pkg_pin1(p);
    if (k < 0 || k >= (int)p.pads.size()) { if (msg) *msg = "No lead clicked"; return std::nan(""); }
    if (i1 < 0) { if (msg) *msg = "Package " + p.name + " has no pin 1 defined; set it in the Package Library"; return std::nan(""); }
    if (k == i1) { if (msg) *msg = "That lead is already pin 1"; return 0; }
    double mind = 1e300;
    for (auto& d : p.pads) mind = std::min(mind, std::min(d.w, d.h));
    const double tol = std::max(0.05, std::min(0.5, mind * 0.35));
    const Vec2 q1 = p.pads[i1].pos, qk = p.pads[k].pos;
    double best = std::nan("");
    for (double d : {180.0, 90.0, 270.0}) {
        if (dist(rotate(q1, d), qk) < tol && pattern_maps(p, {0, 0}, d, tol)) { best = d; break; }
    }
    if (std::isnan(best)) {
        if (msg) *msg = "No rotation of " + p.name + " puts pin 1 on that lead with the leads still on their pads";
        return best;
    }
    placement_rotate_local(c, best);
    c.pin1_set = false;     // pin 1 comes from the package; the placement just rotated
    if (msg) *msg = c.ref + " rotated " + std::to_string((int)best) + " deg so pin 1 is on the clicked lead (package unchanged)";
    return best;
}

// ---- R12 §8: tolerance-aware package grouping ------------------------------------------------------
static double tol_of(const PkgMatchTol& t, double x) { return std::max(t.abs_mm, t.rel * std::fabs(x)) + 1e-6; }

static double min_pitch(const std::vector<Pad>& pads) {
    double best = 0;
    for (size_t i = 0; i < pads.size(); ++i)
        for (size_t j = i + 1; j < pads.size(); ++j) {
            const double d = dist(pads[i].pos, pads[j].pos);
            if (d > 1e-6 && (best == 0 || d < best)) best = d;
        }
    return best;
}

// pad extents in the package frame (w along x, h along y), shape class
static void pad_dims(const Pad& d, double& w, double& h) {
    const double r = std::fmod(std::fabs(d.rot), 180.0);
    const bool q = std::fabs(r - 90) < 1;
    w = q ? d.h : d.w; h = q ? d.w : d.h;
}
static int shape_class(const Pad& d) {
    if (d.shape == PadShape::Round && std::fabs(d.w - d.h) > 1e-6) return (int)PadShape::Oblong;   // oval ~ oblong
    return (int)d.shape;
}

static bool match_at(const Package& a, const Package& b, double deg, const PkgMatchTol& t, double& worst) {
    const size_t n = a.pads.size();
    // compare about each package's lead centroid: a pure origin offset is not a geometry difference
    const Vec2 ca = pkg_lead_centroid(a), cb = pkg_lead_centroid(b);
    std::vector<bool> used(n, false);
    worst = 0;
    for (size_t i = 0; i < n; ++i) {
        const Pad& pb = b.pads[i];
        Pad r = pb;
        r.pos = rotate_about(pb.pos - cb, Vec2{}, deg);
        r.rot = pb.rot + deg;
        double rw, rh; pad_dims(r, rw, rh);
        int best = -1; double bd = 1e300;
        for (size_t j = 0; j < n; ++j) {
            if (used[j]) continue;
            const Pad& pa = a.pads[j];
            const double d = dist(r.pos, pa.pos - ca);
            if (d < bd) { bd = d; best = (int)j; }
        }
        if (best < 0) return false;
        const Pad& pa = a.pads[best];
        double aw, ah; pad_dims(pa, aw, ah);
        const double span = std::max(std::hypot(pa.pos.x - ca.x, pa.pos.y - ca.y), std::max(aw, ah));
        if (bd > tol_of(t, span)) return false;
        if (shape_class(pa) != shape_class(r)) return false;
        if (std::fabs(aw - rw) > tol_of(t, aw) || std::fabs(ah - rh) > tol_of(t, ah)) return false;
        if ((pa.drill > 0) != (r.drill > 0)) return false;
        if (pa.drill > 0 && std::fabs(pa.drill - r.drill) > tol_of(t, pa.drill)) return false;
        worst = std::max({worst, bd, std::fabs(aw - rw), std::fabs(ah - rh)});
        used[best] = true;
    }
    return true;
}

bool pkg_similar(const Package& a, const Package& b, const PkgMatchTol& t, double* rot_deg, double* dev) {
    if (a.pads.empty() || a.pads.size() != b.pads.size()) return false;
    if (a.smt != b.smt) return false;
    // pad-shape multiset must agree
    {
        std::vector<int> sa, sb;
        for (auto& d : a.pads) sa.push_back(shape_class(d));
        for (auto& d : b.pads) sb.push_back(shape_class(d));
        std::sort(sa.begin(), sa.end()); std::sort(sb.begin(), sb.end());
        if (sa != sb) return false;
    }
    const double pa = min_pitch(a.pads), pb = min_pitch(b.pads);
    if (std::fabs(pa - pb) > tol_of(t, pa)) return false;
    const double rots[4] = {0, 90, 180, 270};
    const int nr = t.orientation_aware ? 1 : 4;
    for (int k = 0; k < nr; ++k) {
        double w = 0;
        if (match_at(a, b, rots[k], t, w)) { if (rot_deg) *rot_deg = rots[k]; if (dev) *dev = w; return true; }
    }
    return false;
}

bool pkg_kept_separate(const Package& a, const Package& b) {
    return std::find(a.keep_separate.begin(), a.keep_separate.end(), b.name) != a.keep_separate.end()
        || std::find(b.keep_separate.begin(), b.keep_separate.end(), a.name) != b.keep_separate.end();
}

std::string pkg_find_similar(const Project& prj, const Package& p, const PkgMatchTol& t, const std::string& self) {
    std::string best; double bd = 1e300;
    for (auto& [n, q] : prj.packages) {
        if (n == self || n == p.name || pkg_kept_separate(p, q)) continue;
        double dv = 0;
        if (pkg_similar(q, p, t, nullptr, &dv) && dv < bd) { bd = dv; best = n; }
    }
    return best;
}

std::vector<SimilarGroup> pkg_similar_groups(const Project& prj, const PkgMatchTol& t) {
    std::vector<std::string> names;
    for (auto& kv : prj.packages) names.push_back(kv.first);
    std::map<std::string, int> uses;
    for (auto& c : prj.comps) if (const Package* pk = prj.package_of(c)) ++uses[pk->name];
    std::vector<bool> taken(names.size(), false);
    std::vector<SimilarGroup> out;
    for (size_t i = 0; i < names.size(); ++i) {
        if (taken[i]) continue;
        const Package& a = prj.packages.at(names[i]);
        std::vector<size_t> grp{i};
        double worst = 0;
        for (size_t j = i + 1; j < names.size(); ++j) {
            if (taken[j]) continue;
            const Package& b = prj.packages.at(names[j]);
            double dv = 0;
            bool ok = pkg_similar(a, b, t, nullptr, &dv);
            for (size_t g : grp) if (ok && pkg_kept_separate(prj.packages.at(names[g]), b)) ok = false;
            if (ok) { grp.push_back(j); worst = std::max(worst, dv); }
        }
        if (grp.size() < 2) continue;
        for (size_t g : grp) taken[g] = true;
        size_t keep = grp[0];
        for (size_t g : grp) {
            const int ug = uses[names[g]], uk = uses[names[keep]];
            if (ug > uk || (ug == uk && names[g].size() < names[keep].size())) keep = g;
        }
        SimilarGroup sg; sg.keep = names[keep]; sg.worst_dev = worst;
        for (size_t g : grp) if (g != keep) sg.merge.push_back(names[g]);
        out.push_back(std::move(sg));
    }
    return out;
}

int pkg_merge(Project& prj, const std::string& into, const std::vector<std::string>& from) {
    auto it = prj.packages.find(into);
    if (it == prj.packages.end()) return 0;
    const Vec2 ci = pkg_lead_centroid(it->second);
    int moved = 0;
    for (const std::string& f : from) {
        auto fi = prj.packages.find(f);
        if (f == into || fi == prj.packages.end()) continue;
        // origin difference: keep each placement's pads where they are (pos += L * (c_into - c_from) in package frame)
        const Vec2 d = ci - pkg_lead_centroid(fi->second);
        for (auto& c : prj.comps) {
            const Package* pk = prj.package_of(c);
            if (!pk || pk->name != f) continue;
            Vec2 l = d;
            if (c.side == Side::Bottom) l.x = -l.x;
            const double r = deg2rad(c.rot);
            c.pos += Vec2{l.x * std::cos(r) - l.y * std::sin(r), l.x * std::sin(r) + l.y * std::cos(r)} * -1.0;
            c.package = into;
            ++moved;
        }
        for (auto& [pn, part] : prj.parts) if (part.package == f) part.package = into;
        prj.packages.erase(fi);
    }
    return moved;
}

void pkg_keep_separate(Project& prj, const std::string& a, const std::string& b) {
    auto ia = prj.packages.find(a), ib = prj.packages.find(b);
    if (ia == prj.packages.end() || ib == prj.packages.end()) return;
    if (!pkg_kept_separate(ia->second, ib->second)) ia->second.keep_separate.push_back(b);
}

} // namespace pcbp
