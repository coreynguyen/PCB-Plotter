// plot_align.cpp — see plot_align.h / specs/PANELIZE.md
#include "plot_align.h"
#include <algorithm>
#include <map>
#include <cfloat>
#include <cmath>
#include <string>

namespace pcbp {

Vec2 centroid(const std::vector<Vec2>& v) {
    Vec2 s;
    if (v.empty()) return s;
    for (auto& p : v) { s.x += p.x; s.y += p.y; }
    return {s.x / v.size(), s.y / v.size()};
}

static Xform mir(bool m) { return m ? Xform::scale(-1, 1) : Xform{}; }

Xform align_xform(Vec2 c, Vec2 g, double rot, bool mirror) {
    return Xform::translate(g.x, g.y) * Xform::rotation(rot) * mir(mirror) * Xform::translate(-c.x, -c.y);
}

static double d2(Vec2 a, Vec2 b) { const double x = a.x - b.x, y = a.y - b.y; return x * x + y * y; }

static bool fit_once(const std::vector<Vec2>& cad, const std::vector<Vec2>& ger, double rot_hint, bool mirror,
                     bool refine_rot, AlignFit& out) {
    if (cad.empty() || ger.empty()) return false;
    const Vec2 cc = centroid(cad), gc = centroid(ger);
    double rot = rot_hint;
    Xform T = align_xform(cc, gc, rot, mirror);
    // mirrored CAD points (fixed), so the fit is a proper rotation + translation
    std::vector<Vec2> m(cad.size());
    for (size_t i = 0; i < cad.size(); ++i) m[i] = mir(mirror).apply(cad[i]);
    double rms = 0; int used = 0;
    for (int it = 0; it < 30; ++it) {
        // nearest Gerber pad for every CAD pad, mutual nearest only
        std::vector<int> nn(cad.size(), -1);
        for (size_t i = 0; i < cad.size(); ++i) {
            const Vec2 q = T.apply(cad[i]);
            double bd = DBL_MAX; int bj = -1;
            for (size_t j = 0; j < ger.size(); ++j) { double e = d2(q, ger[j]); if (e < bd) { bd = e; bj = (int)j; } }
            nn[i] = bj;
        }
        std::vector<int> back(ger.size(), -1);
        for (size_t j = 0; j < ger.size(); ++j) {
            double bd = DBL_MAX; int bi = -1;
            for (size_t i = 0; i < cad.size(); ++i) { double e = d2(T.apply(cad[i]), ger[j]); if (e < bd) { bd = e; bi = (int)i; } }
            back[j] = bi;
        }
        std::vector<Vec2> A, B;
        for (size_t i = 0; i < cad.size(); ++i) if (nn[i] >= 0 && back[nn[i]] == (int)i) { A.push_back(m[i]); B.push_back(ger[nn[i]]); }
        if (A.empty()) break;
        const Vec2 ac = centroid(A), bc = centroid(B);
        if (refine_rot && A.size() >= 2) {
            double sxx = 0, sxy = 0;
            for (size_t k = 0; k < A.size(); ++k) {
                const Vec2 a = A[k] - ac, b = B[k] - bc;
                sxx += a.x * b.x + a.y * b.y;       // cos term
                sxy += a.x * b.y - a.y * b.x;       // sin term
            }
            if (std::fabs(sxx) + std::fabs(sxy) > 1e-12) rot = rad2deg(std::atan2(sxy, sxx));
        }
        const Xform R = Xform::rotation(rot);
        const Vec2 ra = R.apply(ac);
        const Xform Tn = Xform::translate(bc.x - ra.x, bc.y - ra.y) * R * mir(mirror);
        rms = 0;
        for (size_t k = 0; k < A.size(); ++k) rms += d2(R.apply(A[k]) + (bc - ra), B[k]);
        rms = std::sqrt(rms / A.size());
        used = (int)A.size();
        const bool same = std::fabs(Tn.tx - T.tx) < 1e-12 && std::fabs(Tn.ty - T.ty) < 1e-12 && std::fabs(Tn.a - T.a) < 1e-15 && std::fabs(Tn.b - T.b) < 1e-15;
        T = Tn;
        if (same && it > 0) break;
    }
    // unmatched pads count as a poor fit (penalty for the rotation detector)
    out.T = T; out.rot = norm_deg(rot); out.mirror = mirror; out.rms = rms; out.matched = used;
    if (out.rot > 180) out.rot -= 360;
    return used > 0;
}

static double fit_score(const AlignFit& f, size_t nc, size_t ng) {
    return f.rms + ((int)std::min(nc, ng) - f.matched) * 10.0;
}

bool fit_cad_to_gerber(const std::vector<Vec2>& cad, const std::vector<Vec2>& ger, double rot_hint, bool mirror,
                       bool refine_rot, AlignFit& out) {
    if (!fit_once(cad, ger, rot_hint, mirror, refine_rot, out)) return false;
    if (!refine_rot || out.rms < 1e-4) return true;
    // arbitrary angle: the nearest-neighbour start can lock onto the wrong pads when the true angle is
    // far from the hint, so sweep start angles within +-45 deg of the hint and keep the best fit
    const long work = (long)cad.size() * (long)ger.size();
    const double step = work > 40000 ? 9.0 : 3.0;
    double best = fit_score(out, cad.size(), ger.size());
    for (double a = -45; a <= 45 + 1e-9; a += step) {
        AlignFit f;
        if (!fit_once(cad, ger, rot_hint + a, mirror, true, f)) continue;
        const double sc = fit_score(f, cad.size(), ger.size());
        if (sc < best - 1e-9) { best = sc; out = f; }
    }
    return true;
}

bool detect_cad_to_gerber(const std::vector<Vec2>& cad, const std::vector<Vec2>& ger, bool try_mirror, bool refine_rot, AlignFit& out) {
    bool any = false;
    double best = DBL_MAX;
    for (int m = 0; m < (try_mirror ? 2 : 1); ++m)
        for (int r = 0; r < 4; ++r) {
            AlignFit f;
            if (!fit_cad_to_gerber(cad, ger, r * 90.0, m != 0, refine_rot, f)) continue;
            const int miss = (int)std::min(cad.size(), ger.size()) - f.matched;
            const double score = f.rms + miss * 10.0;
            if (score < best - 1e-9) { best = score; out = f; any = true; }
        }
    return any;
}

void decompose_xform(const Xform& t, Vec2& off, double& rot, bool& mirror) {
    off = {t.tx, t.ty};
    mirror = t.det() < 0;
    // t.linear = R * S  =>  R = t.linear * S  (S = S^-1)
    const double a = mirror ? -t.a : t.a, b = mirror ? -t.b : t.b;
    rot = rad2deg(std::atan2(b, a));
    if (std::fabs(rot) < 1e-12) rot = 0;
}

Xform layer_ref_xform(const GerberLayerRef& r) {
    Xform t = Xform::translate(r.offset.x, r.offset.y) * Xform::rotation(r.rot);
    if (r.mirror) t = t * Xform::scale(-1, 1);
    return t;
}

void premultiply_layer(GerberLayerRef& r, const Xform& pre) {
    decompose_xform(pre * layer_ref_xform(r), r.offset, r.rot, r.mirror);
    r.rot = norm_deg(r.rot);
}

static void move_comp(Component& c, const Xform& T) {
    const bool m = T.det() < 0;
    Xform L = T; L.tx = L.ty = 0;
    Vec2 o; double drot = 0; bool mm = false;
    decompose_xform(T, o, drot, mm);
    c.pos = T.apply(c.pos);
    c.label_off = L.apply(c.label_off);
    if (m) {
        // footprint X-mirrored: viewed-from-top orientation flips, the part changes side
        // T.lin = R(drot) S and S R(r) = R(-r) S, so T * Tr R(r) [S] = Tr' R(drot - r) S [S]
        c.rot = norm_deg(drot - c.rot);
        c.side = c.side == Side::Top ? Side::Bottom : Side::Top;
        c.label_rot = norm_deg(drot - c.label_rot);
    } else {
        c.rot = norm_deg(c.rot + drot);
        c.label_rot = norm_deg(c.label_rot + drot);
    }
}

void move_plot(Project& p, const Xform& T, bool include_gerber_built) {
    for (auto& c : p.comps) if (include_gerber_built || !c.from_gerber) move_comp(c, T);
    if (include_gerber_built || !p.board_frame_from_gerber) for (auto& pl : p.board_frame) for (auto& q : pl) q = T.apply(q);
    if (include_gerber_built || !p.panel_frame_from_gerber) for (auto& pl : p.panel_frame) for (auto& q : pl) q = T.apply(q);
    const Xform Ti = T.inverse();
    for (size_t i = 1; i < p.images.size(); ++i) {
        PanelImage& im = p.images[i];
        const Xform I = Xform::translate(im.offset.x, im.offset.y) * Xform::rotation(im.rot);
        const Xform J = T * I * Ti;     // rigid (mirror cancels)
        bool m = false;
        decompose_xform(J, im.offset, im.rot, m);
        im.rot = norm_deg(std::round(im.rot * 1e9) / 1e9);
    }
    p.virtual_origin = T.apply(p.virtual_origin);
    p.dirty = true;
}

void move_gerber_built(Project& p, const Xform& M) {
    for (auto& c : p.comps) if (c.from_gerber) move_comp(c, M);
    if (p.board_frame_from_gerber) for (auto& pl : p.board_frame) for (auto& q : pl) q = M.apply(q);
    if (p.panel_frame_from_gerber) for (auto& pl : p.panel_frame) for (auto& q : pl) q = M.apply(q);
    p.dirty = true;
}

// ---- R17a alignment session (REVIEW_V16 §1; see plot_align.h) ---------------------------------
Vec2 pad_selection_centre(const std::vector<Vec2>& v) {
    if (v.empty()) return {};
    BBox b;
    for (auto& q : v) b.add(q);
    return b.center();
}

Vec2 pad_selection_centre(const std::vector<Vec2>& v, double axis) {
    if (v.empty()) return {};
    BBox b;
    for (auto& q : v) b.add(rotate(q, -axis));
    return rotate(b.center(), axis);
}

// selection centres boxed along each part's axis as mapped by T (exact for any rotation)
static void refine_centres(std::vector<AlignRef*>& use, const Xform& T) {
    Xform L = T; L.tx = L.ty = 0;
    for (auto* r : use) {
        const Vec2 d = L.apply(r->xdir);
        r->ger_centre = pad_selection_centre(r->ger_pads, rad2deg(std::atan2(d.y, d.x)));
    }
}

AlignRef make_align_ref(const Project& p, int comp, int image, const std::vector<Vec2>& ger) {
    AlignRef r;
    r.comp = comp; r.image = image;
    r.ger_pads = ger;
    r.ger_centre = pad_selection_centre(ger);
    if (comp < 0 || comp >= (int)p.comps.size()) return r;
    const Component& c = p.comps[comp];
    const Xform t = p.instance_xform(c, image);
    r.plot = t.apply({0, 0});
    r.xdir = t.apply({1, 0}) - r.plot;
    if (const Package* pk = p.package_of(c)) for (auto& d : pk->pads) r.cad_pads.push_back(t.apply(d.pos));
    return r;
}

static void ref_residuals(std::vector<AlignRef>& refs, AlignFit& f) {
    double s = 0;
    int n = 0;
    for (auto& r : refs) {
        if (r.comp < 0 || r.ger_pads.empty()) continue;
        r.residual = dist(f.T.apply(r.plot), r.ger_centre);
        s += r.residual * r.residual; ++n;
    }
    f.rms = n ? std::sqrt(s / n) : 0;
    f.matched = n;
}

// rigid fit of centroid -> selection-centre pairs (>= 2)
static bool solve_pairs(const std::vector<Vec2>& P, const std::vector<Vec2>& G, int rot_mode, bool mirror, AlignFit& f) {
    std::vector<Vec2> A;
    for (auto& q : P) A.push_back(mir(mirror).apply(q));
    const Vec2 ac = centroid(A), bc = centroid(G);
    double rot = rot_mode < 4 ? rot_mode * 90.0 : 0.0;
    if (rot_mode >= 4) {
        double sxx = 0, sxy = 0;
        for (size_t k = 0; k < A.size(); ++k) { const Vec2 a = A[k] - ac, b = G[k] - bc; sxx += a.x * b.x + a.y * b.y; sxy += a.x * b.y - a.y * b.x; }
        if (std::fabs(sxx) + std::fabs(sxy) < 1e-9) return false;
        rot = rad2deg(std::atan2(sxy, sxx));
    }
    const Xform R = Xform::rotation(rot);
    const Vec2 ra = R.apply(ac);
    f.T = Xform::translate(bc.x - ra.x, bc.y - ra.y) * R * mir(mirror);
    f.rot = norm_deg(rot);
    if (f.rot > 180) f.rot -= 360;
    f.mirror = mirror;
    double s = 0;
    for (size_t k = 0; k < P.size(); ++k) s += d2(f.T.apply(P[k]), G[k]);
    f.rms = std::sqrt(s / P.size());
    f.matched = (int)P.size();
    return true;
}

bool solve_alignment(std::vector<AlignRef>& refs, int rot_mode, bool mirror, AlignFit& out, std::string* why) {
    auto say = [&](const std::string& m) { if (why) *why = m; };
    say("");
    std::vector<AlignRef*> use;
    for (auto& r : refs) if (r.comp >= 0 && !r.ger_pads.empty()) use.push_back(&r);
    if (use.empty()) { say("Pick a placement, then box its pads on the Gerber."); return false; }
    AlignFit f;
    f.mirror = mirror;
    if (use.size() == 1) {
        AlignRef& r = *use[0];
        if (r.cad_pads.size() >= 2 && r.ger_pads.size() >= 2) {
            // package pads known: pad-to-pad fit (rotation from the pads when rot_mode == 4)
            bool ok;
            if (rot_mode < 4) ok = fit_cad_to_gerber(r.cad_pads, r.ger_pads, rot_mode * 90.0, mirror, false, f);
            else {
                AlignFit d;
                const double hint = detect_cad_to_gerber(r.cad_pads, r.ger_pads, false, false, d) ? d.rot : 0.0;
                ok = fit_cad_to_gerber(r.cad_pads, r.ger_pads, hint, mirror, true, f);
            }
            if (!ok) { say("The package pads could not be matched to the selected Gerber pads."); return false; }
            if (f.matched < (int)std::min(r.cad_pads.size(), r.ger_pads.size()))
                say("Not every package pad found a Gerber pad: box all pads of the placement.");
            const double rms = f.rms; const int m = f.matched;
            ref_residuals(refs, f);
            f.rms = rms; f.matched = m;
            out = f;
            return true;
        }
        // centroid only (or a single pad): the centroid goes to the centre of the selection
        const double rot = rot_mode < 4 ? rot_mode * 90.0 : 0.0;
        if (rot_mode >= 4) say("One reference without package pads sets only the offset: choose 0/90/180/270, or add a second reference.");
        f.rot = rot > 180 ? rot - 360 : rot;
        f.T = align_xform(r.plot, r.ger_centre, rot, mirror);
        refine_centres(use, f.T);
        f.T = align_xform(r.plot, r.ger_centre, rot, mirror);
        ref_residuals(refs, f);
        out = f;
        return true;
    }
    std::vector<Vec2> P, G;
    for (auto* r : use) { r->ger_centre = pad_selection_centre(r->ger_pads); P.push_back(r->plot); G.push_back(r->ger_centre); }
    if (!solve_pairs(P, G, rot_mode, mirror, f)) { say("The references are on the same spot: pick placements far apart."); return false; }
    for (int it = 0; it < 3; ++it) {   // re-box each selection along its part's axis, then re-fit
        refine_centres(use, f.T);
        for (size_t k = 0; k < use.size(); ++k) G[k] = use[k]->ger_centre;
        solve_pairs(P, G, rot_mode, mirror, f);
    }
    ref_residuals(refs, f);
    if (use.size() >= 3) {   // with 3+ references the other mirror state can be told apart
        AlignFit g;
        if (solve_pairs(P, G, rot_mode, !mirror, g) && g.rms + 0.05 < f.rms * 0.5)
            say(mirror ? "The references fit much better without Mirror." : "The references fit much better with Mirror: the Gerbers may be mirrored.");
    }
    if (f.rms > 0.25 && (!why || why->empty()))
        say(rot_mode < 4 ? "References disagree (large residual): try another rotation, Mirror, or \"From references\"."
                         : "References disagree (large residual): check the placements picked and their pads.");
    out = f;
    return true;
}

// ---- R17a: flattened panels ----------------------------------------------------------------------
bool detect_repeated_instances(const Project& p, InstanceFold& out, double tol) {
    out = InstanceFold{};
    std::map<std::string, std::vector<int>> groups;
    std::vector<std::string> order;
    for (int i = 0; i < (int)p.comps.size(); ++i) {
        const Component& c = p.comps[i];
        if (c.ref.empty() || !p.instanced(c)) continue;
        std::string u = c.ref;
        for (auto& ch : u) ch = (char)std::toupper((unsigned char)ch);
        auto& g = groups[u];
        if (g.empty()) order.push_back(u);
        g.push_back(i);
    }
    if (order.empty()) { out.why = "no references"; return false; }
    const size_t n = groups[order[0]].size();
    for (auto& r : order) if (groups[r].size() != n) { out.why = r + " occurs " + std::to_string(groups[r].size()) + " times, " + order[0] + " " + std::to_string(n) + " times"; return false; }
    if (n < 2) { out.why = "references are unique"; return false; }
    if (p.images.size() > 1) { out.why = "the project already has circuit instances"; return false; }
    out.n = (int)n;
    out.members.assign(n, {});
    for (size_t k = 0; k < n; ++k) for (auto& r : order) out.members[k].push_back(groups[r][k]);
    PanelImage root; root.name = "1"; root.root = true;
    out.images.push_back(root);
    std::vector<Vec2> P;
    for (int i : out.members[0]) P.push_back(p.comps[i].pos);
    for (size_t k = 1; k < n; ++k) {
        std::vector<Vec2> G;
        for (int i : out.members[k]) G.push_back(p.comps[i].pos);
        AlignFit f;
        if (P.size() >= 2) { if (!solve_pairs(P, G, 4, false, f)) { out.why = "degenerate circuit"; return false; } }
        else f.T = Xform::translate(G[0].x - P[0].x, G[0].y - P[0].y);
        double rot = std::round(f.rot * 1e6) / 1e6;
        if (std::fabs(rot - std::round(rot)) < 1e-4) rot = std::round(rot);
        f.T = Xform::translate(f.T.tx, f.T.ty) * Xform::rotation(rot);
        for (size_t j = 0; j < P.size(); ++j) {
            const Component& a = p.comps[out.members[0][j]];
            const Component& b = p.comps[out.members[k][j]];
            const double dv = dist(f.T.apply(a.pos), b.pos);
            out.max_dev = std::max(out.max_dev, dv);
            if (dv > tol) { out.why = "circuit " + std::to_string(k + 1) + ": " + b.ref + " is " + std::to_string(dv).substr(0, 6) + " mm off the repeated pattern"; return false; }
            if (a.side != b.side || std::fabs(std::remainder(b.rot - a.rot - rot, 360.0)) > 0.05 || a.package != b.package) {
                out.why = "circuit " + std::to_string(k + 1) + ": " + b.ref + " differs (side, rotation or package)";
                return false;
            }
        }
        PanelImage im;
        im.name = std::to_string(k + 1);
        im.root = false;
        im.offset = {f.T.tx, f.T.ty};
        im.rot = norm_deg(rot);
        im.x_index = (int)k;
        out.images.push_back(im);
    }
    return true;
}

int fold_repeated_instances(Project& p, const InstanceFold& f) {
    if (f.n < 2 || f.members.size() != (size_t)f.n) return 0;
    std::vector<char> drop(p.comps.size(), 0);
    for (size_t k = 1; k < f.members.size(); ++k) for (int i : f.members[k]) if (i >= 0 && i < (int)drop.size()) drop[i] = 1;
    std::vector<Component> keep;
    int removed = 0;
    for (size_t i = 0; i < p.comps.size(); ++i) { if (drop[i]) ++removed; else keep.push_back(p.comps[i]); }
    p.comps = std::move(keep);
    p.images = f.images;
    p.dirty = true;
    return removed;
}

int nearest_instance(const Project& p, const Component& c, Vec2 w) {
    const int n = std::max(1, p.instance_count(c));
    int best = 0;
    double bd = DBL_MAX;
    for (int i = 0; i < n; ++i) {
        if (i < (int)p.images.size() && p.images[i].x_out) continue;
        const double e = d2(p.instance_xform(c, i).apply({0, 0}), w);
        if (e < bd) { bd = e; best = i; }
    }
    return best;
}

std::vector<GhostPlacement> preview_placements(const Project& p, const Xform& T, bool with_pads) {
    std::vector<GhostPlacement> v;
    for (size_t ci = 0; ci < p.comps.size(); ++ci) {
        const Component& c = p.comps[ci];
        if (c.from_gerber) continue;   // Gerber-built elements do not move with the plot
        const Package* pk = with_pads ? p.package_of(c) : nullptr;
        for (int i = 0; i < std::max(1, p.instance_count(c)); ++i) {
            const Xform t = T * p.instance_xform(c, i);
            GhostPlacement g;
            g.comp = (int)ci; g.image = i;
            g.pos = t.apply({0, 0});
            g.xdir = t.apply({1, 0}) - g.pos;
            g.bottom = t.det() < 0;
            if (pk) for (auto& d : pk->pads) g.pads.push_back(t.apply(d.pos));
            v.push_back(std::move(g));
        }
    }
    return v;
}

void transform_gerbers(Project& p, double rot, bool mirror, Vec2 about) {
    const Xform M = Xform::translate(about.x, about.y) * Xform::rotation(rot) * mir(mirror) * Xform::translate(-about.x, -about.y);
    for (auto& L : p.layers) premultiply_layer(L, M);
    move_gerber_built(p, M);
    p.plot_aligned = false;
    p.dirty = true;
}

PitchPick pitch_from_points(Vec2 ref, Vec2 xn, Vec2 yn, bool hx, bool hy) {
    PitchPick r;
    if (hx) { r.dx = xn.x - ref.x; r.skew.y = xn.y - ref.y; }
    if (hy) { r.dy = yn.y - ref.y; r.skew.x = yn.x - ref.x; }
    return r;
}

GridSuggest suggest_grid(const BBox& c, const BBox& g, double dx, double dy, double tol) {
    GridSuggest s;
    auto axis = [&](double c0, double c1, double g0, double g1, double d, int& n, int& root) {
        n = 1; root = 0;
        if (std::fabs(d) < 1e-6 || c0 > c1 || g0 > g1) return;
        const double ad = std::fabs(d);
        int lo = (int)std::floor((c0 - g0 + tol) / ad), hi = (int)std::floor((g1 - c1 + tol) / ad);
        lo = std::clamp(lo, 0, 200); hi = std::clamp(hi, 0, 200);
        if (d < 0) std::swap(lo, hi);   // negative pitch: "next" is towards smaller coordinates
        root = lo; n = lo + 1 + hi;
    };
    axis(c.x0, c.x1, g.x0, g.x1, dx, s.nx, s.root_col);
    axis(c.y0, c.y1, g.y0, g.y1, dy, s.ny, s.root_row);
    return s;
}

std::vector<PanelImage> step_repeat_grid(int nx, int ny, double dx, double dy, int rc, int rr) {
    std::vector<PanelImage> out;
    nx = std::max(1, nx); ny = std::max(1, ny);
    rc = std::clamp(rc, 0, nx - 1); rr = std::clamp(rr, 0, ny - 1);
    PanelImage root; root.name = "1"; root.root = true; root.x_index = rc; root.y_index = rr;
    out.push_back(root);
    int n = 2;
    for (int x = 0; x < nx; ++x)
        for (int y = 0; y < ny; ++y) {
            if (x == rc && y == rr) continue;
            PanelImage im;
            im.name = std::to_string(n++);
            im.root = false;
            im.offset = {(x - rc) * dx, (y - rr) * dy};
            im.x_index = x; im.y_index = y;
            out.push_back(im);
        }
    return out;
}

PanelImage instance_from_points(Vec2 p1, Vec2 q1, Vec2 p2, Vec2 q2, bool two, double rot_hint, bool snap90) {
    double rot = rot_hint;
    if (two) {
        const Vec2 a = p2 - p1, b = q2 - q1;
        if (std::hypot(a.x, a.y) > 1e-9 && std::hypot(b.x, b.y) > 1e-9) rot = rad2deg(std::atan2(b.y, b.x) - std::atan2(a.y, a.x));
    }
    if (snap90) rot = std::round(rot / 90.0) * 90.0;
    rot = norm_deg(rot);
    PanelImage im;
    im.root = false;
    im.rot = rot;
    const Vec2 o1 = q1 - rotate(p1, rot);
    if (two) { const Vec2 o2 = q2 - rotate(p2, rot); im.offset = {(o1.x + o2.x) * 0.5, (o1.y + o2.y) * 0.5}; }
    else im.offset = o1;
    return im;
}

int add_instance(Project& p, const PanelImage& im0) {
    if (p.images.empty()) p.images.push_back(PanelImage{});
    PanelImage im = im0;
    im.root = false;
    int mx = 0;
    for (auto& e : p.images) mx = std::max(mx, e.x_index);
    im.x_index = mx + 1; im.y_index = 0;
    im.name = std::to_string(p.images.size() + 1);
    p.images.push_back(im);
    p.dirty = true;
    return (int)p.images.size() - 1;
}


bool poly_contains(const std::vector<Vec2>& poly, Vec2 p) {
    bool in = false;
    const size_t n = poly.size();
    if (n < 3) return false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const Vec2& a = poly[i];
        const Vec2& b = poly[j];
        if (((a.y > p.y) != (b.y > p.y)) && (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)) in = !in;
    }
    return in;
}

static std::vector<Vec2> shrink(const std::vector<Vec2>& poly, double t) {
    const Vec2 c = centroid(poly);
    std::vector<Vec2> o;
    for (auto& q : poly) {
        const Vec2 d = q - c;
        const double l = std::hypot(d.x, d.y);
        o.push_back(l > t ? c + d * ((l - t) / l) : c);
    }
    return o;
}

static bool seg_cross(Vec2 p1, Vec2 p2, Vec2 q1, Vec2 q2) {
    auto cr = [](Vec2 o, Vec2 a, Vec2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };
    const double d1 = cr(q1, q2, p1), d2 = cr(q1, q2, p2), d3 = cr(p1, p2, q1), d4 = cr(p1, p2, q2);
    return ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0));
}

bool polys_overlap(const std::vector<Vec2>& a0, const std::vector<Vec2>& b0, double tol) {
    if (a0.size() < 3 || b0.size() < 3) return false;
    const auto a = shrink(a0, tol), b = shrink(b0, tol);
    for (size_t i = 0; i < a.size(); ++i)
        for (size_t j = 0; j < b.size(); ++j)
            if (seg_cross(a[i], a[(i + 1) % a.size()], b[j], b[(j + 1) % b.size()])) return true;
    return poly_contains(b, a[0]) || poly_contains(a, b[0]);
}

static std::vector<Vec2> open_poly(std::vector<Vec2> v) {
    if (v.size() > 3 && dist(v.front(), v.back()) < 1e-6) v.pop_back();
    return v;
}

// largest closed polygon of an outline (open chains are closed implicitly); the outline a placement must lie in
static std::vector<Vec2> main_poly(const std::vector<std::vector<Vec2>>& fr) {
    std::vector<Vec2> best;
    double ba = -1;
    for (auto& pl0 : fr) {
        auto pl = open_poly(pl0);
        if (pl.size() < 3) continue;
        double a = 0;
        for (size_t i = 0, j = pl.size() - 1; i < pl.size(); j = i++) a += pl[j].x * pl[i].y - pl[i].x * pl[j].y;
        a = std::fabs(a);
        if (a > ba) { ba = a; best = pl; }
    }
    return best;
}

// R22 (REVIEW_V21 §2.3): array validation. The reference circuit's outline (board_frame, reference coordinates)
// is transformed onto EVERY instance; each instance's placements are tested against that instance's own
// transformed outline, and each transformed outline against the board outline (panel_frame). An instance is
// never compared with the reference's untransformed outline. A board outline that is only Inferred (old
// generated outline, see arrays.h migration) cannot prove a violation: the containment finding is a Warning
// that names it as inferred, not an Error.
std::vector<FrameIssue> check_outlines(const Project& p, double max_w, double max_h) {
    std::vector<FrameIssue> v;
    const int n = std::max<int>(1, (int)p.images.size());
    const bool arr = n > 1;
    auto cname = [&](int i) { return i == 0 ? std::string("circuit 1 (reference)") : "circuit " + std::to_string(i + 1); };
    std::vector<std::vector<Vec2>> circ;   // each instance's own transformed circuit outline
    if (!p.board_frame.empty())
        for (int i = 0; i < n; ++i) circ.push_back(main_poly(p.circuit_frame_at(i)));
    // placements inside their own instance's circuit outline
    if (!circ.empty())
        for (auto& c : p.comps) {
            if (c.feature == Feature::Hole) continue;
            if (!p.instanced(c)) continue;
            for (int i = 0; i < n; ++i) {
                if (i < (int)p.images.size() && p.images[i].x_out) continue;
                const Vec2 q = p.instance_xform(c, i).apply({0, 0});
                if (!circ[i].empty() && !poly_contains(shrink(circ[i], -0.001), q)) {
                    v.push_back({false, c.ref, "outside the circuit outline" + (arr ? " of " + cname(i) : std::string()) +
                                 ": the machine may place it off the board or onto a neighbouring circuit"});
                    break;   // a rigid copy: one instance outside means all are
                }
            }
        }
    // global (panel) fiducials inside the board outline
    std::vector<Vec2> board = main_poly(p.panel_frame);
    const bool board_inferred = !p.panel_frame.empty() && p.panel_outline_src == OutlineSource::Inferred;
    if (!board.empty())
        for (auto& c : p.comps)
            if (!p.instanced(c) && !poly_contains(shrink(board, -0.001), c.pos))
                v.push_back({!board_inferred, c.ref, std::string("outside the board outline") + (board_inferred ? " (the board outline is only inferred)" : "")});
    // every instance's transformed circuit outline inside the board outline
    if (!board.empty())
        for (int i = 0; i < (int)circ.size(); ++i) {
            if (circ[i].empty()) continue;
            for (auto& q : circ[i])
                if (!poly_contains(shrink(board, -0.01), q)) {
                    v.push_back({!board_inferred, "", cname(i) + " outline extends beyond the board outline" +
                                 (board_inferred ? std::string(" (the board outline is only inferred: define the measured board outline)") : std::string())});
                    break;
                }
        }
    // instance outlines must not overlap each other
    for (int i = 0; i < (int)circ.size(); ++i)
        for (int j = i + 1; j < (int)circ.size(); ++j)
            if (polys_overlap(circ[i], circ[j])) v.push_back({true, "", "circuit outlines " + std::to_string(i + 1) + " and " + std::to_string(j + 1) + " overlap"});
    // board size vs machine limits
    BBox bb = p.panel_frame_bbox();
    if (p.panel_frame.empty() && n == 1) bb = p.circuit_frame_bbox();
    if (bb.empty()) v.push_back({false, "", "no board outline: the conveyor width and board length cannot be set from it"});
    else {
        if (max_w > 0 && bb.w() > max_w + 1e-6) v.push_back({true, "", "board length " + std::to_string(bb.w()).substr(0, 7) + " mm exceeds the machine limit " + std::to_string(max_w).substr(0, 7) + " mm"});
        if (max_h > 0 && bb.h() > max_h + 1e-6) v.push_back({true, "", "board width " + std::to_string(bb.h()).substr(0, 7) + " mm exceeds the machine limit " + std::to_string(max_h).substr(0, 7) + " mm"});
    }
    return v;
}

BBox rect_from_ref_offsets(Vec2 r, Vec2 tl, Vec2 br) {
    BBox b;
    b.add({r.x + tl.x, r.y + tl.y});
    b.add({r.x + br.x, r.y + br.y});
    return b;
}

} // namespace pcbp
