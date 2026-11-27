// ============================================================================
// panel_auto.cpp — see panel_auto.h.
// ============================================================================
#include "panel_auto.h"
#include "arrays.h"
#include "gerber_set.h"
#include "plot_align.h"
#include "../io/gerber.h"
#include "../io/gerber_match.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace pcbp::panel_auto {

namespace {
constexpr double kShift = 100000.0;   // CAD reference features live far from any artwork

double nrm(double a) { a = std::fmod(a, 360.0); if (a < 0) a += 360.0; if (a > 360.0 - 1e-9) a = 0; return a; }
bool quarter(double a, double tol = 0.01) { const double r = std::fmod(nrm(a), 90.0); return r < tol || 90.0 - r < tol; }
int quarter_index(double a) { return ((int)std::lround(nrm(a) / 90.0)) % 4; }

int pick_layer(const Project& p, const Options& o) {
    const int n = (int)p.layers.size();
    if (o.match_layer >= 0) return o.match_layer < n ? o.match_layer : -1;
    for (LayerRole r : {LayerRole::Copper, LayerRole::Paste})
        for (int i = 0; i < n; ++i) if (p.layers[i].role == r && p.layers[i].side == Side::Top) return i;
    for (LayerRole r : {LayerRole::Copper, LayerRole::Paste})
        for (int i = 0; i < n; ++i) if (p.layers[i].role == r) return i;
    return -1;
}
} // namespace

std::string Detection::summary() const {
    if (!ok) return "no panel instances: " + why;
    char b[256];
    std::snprintf(b, sizeof b, "%d instance(s) on layer %d (%d ref pads from %d parts); %s", (int)found.size(), match_layer, ref_pads, ref_comps,
                  linear ? "linear grid" : "nonlinear");
    std::string s = b;
    if (linear) { std::snprintf(b, sizeof b, " %dx%d, pitch %.3f x %.3f mm", nx, ny, dx, dy); s += b; }
    if (ambiguous) s += "; " + std::to_string(ambiguous) + " ambiguous";
    return s;
}

Detection detect(const Project& p, const Options& o) {
    Detection d;
    d.match_layer = pick_layer(p, o);
    if (d.match_layer < 0) { d.why = "no copper or paste Gerber layer to match"; return d; }
    GerberLayerRef lr = p.layers[d.match_layer];
    d.side = lr.side;
    gerber::Layer L;
    std::string err;
    if (!gset::parse_layer_ref(lr, L, err)) { d.why = "layer '" + lr.name + "' does not parse: " + err; return d; }
    std::vector<gmatch::Feature> all = gmatch::layer_features(L, layer_ref_xform(lr), d.match_layer);
    if (all.empty()) { d.why = "layer '" + lr.name + "' has no objects"; return d; }

    // reference group: CAD root pads on the matched side (component and pad at quarter turns), shifted far away
    struct RP { Vec2 c; double w, h; };
    std::vector<RP> pads;
    for (const Component& c : p.comps) {
        if (c.feature != Feature::Component || c.dnp) continue;
        const Package* pk = p.package_of(c);
        if (!pk || pk->pads.empty()) continue;
        const bool th = std::any_of(pk->pads.begin(), pk->pads.end(), [](const Pad& q) { return q.drill > 0; });
        if (c.side != d.side && !th) continue;     // through-hole lands are on both copper layers
        if (!quarter(c.rot)) continue;
        const Xform t = p.instance_xform(c, 0);
        bool any = false;
        for (const Pad& q : pk->pads) {
            if (!quarter(q.rot)) continue;
            const bool swap = (quarter_index(c.rot) + quarter_index(q.rot)) % 2 == 1;
            pads.push_back({t.apply(q.pos), swap ? q.h : q.w, swap ? q.w : q.h});
            any = true;
        }
        d.ref_comps += any;
    }
    if (pads.size() < 2) { d.why = "the CAD has fewer than two usable pads on the " + std::string(d.side == Side::Top ? "top" : "bottom") + " side"; return d; }
    if ((int)pads.size() > o.max_ref) {   // spread sample: sort by position, take every k-th
        std::stable_sort(pads.begin(), pads.end(), [](const RP& a, const RP& b) { return a.c.y != b.c.y ? a.c.y < b.c.y : a.c.x < b.c.x; });
        std::vector<RP> s;
        const double step = (double)pads.size() / o.max_ref;
        for (int k = 0; k < o.max_ref; ++k) s.push_back(pads[(size_t)(k * step)]);
        pads.swap(s);
    }
    std::vector<int> ref;
    const int n_art = (int)all.size();   // features [n_art, ...) are the shifted CAD reference
    for (auto& rp : pads) {
        gmatch::Feature f;
        f.c = {rp.c.x + kShift, rp.c.y + kShift};
        f.w = rp.w; f.h = rp.h;
        f.layer = d.match_layer; f.kind = gmatch::FFlash; f.obj = -1;   // same_shape compares layer + kind
        ref.push_back((int)all.size());
        all.push_back(f);
    }
    d.ref_pads = (int)pads.size();

    gmatch::MatchOptions mo;
    mo.tol = o.tol; mo.size_tol = o.size_tol; mo.size_rel = o.size_rel; mo.min_fraction = o.min_fraction;
    for (int q = 0; q < 4; ++q) mo.rot[q] = o.rot[q];
    std::string why;
    std::vector<gmatch::Candidate> cands = gmatch::find_pattern(all, ref, mo, &why);
    // un-shift: R(q)(x + S) + off = R(q)x + (off + R(q)S)
    std::vector<Instance> raw;
    for (auto& c : cands) {
        Instance in;
        in.rot = c.rot;
        in.offset = c.offset + gmatch::rot_quarter({kShift, kShift}, quarter_index(c.rot));
        in.centre = c.centre; in.box = c.box; in.matched = c.matched; in.total = c.total; in.rms = c.rms;
        bool cad_hit = false;   // a match inside the shifted CAD group itself (self-symmetry of the reference)
        for (int j : c.feats) cad_hit |= j >= n_art;
        if (!cad_hit) raw.push_back(in);
    }
    if (raw.empty()) { d.why = why.empty() ? "no copy of the CAD circuit in the Gerber layer" : why; return d; }
    // group by place: best (matched, rms) wins; a tie with a different rotation = ambiguous
    const double place_tol = std::max(4 * o.tol, 0.2);
    std::vector<char> used(raw.size(), 0);
    for (size_t i = 0; i < raw.size(); ++i) {
        if (used[i]) continue;
        std::vector<size_t> g{i};
        for (size_t j = i + 1; j < raw.size(); ++j) if (!used[j] && dist(raw[j].centre, raw[i].centre) <= place_tol) g.push_back(j);
        for (size_t k : g) used[k] = 1;
        std::sort(g.begin(), g.end(), [&](size_t a, size_t b) { return raw[a].matched != raw[b].matched ? raw[a].matched > raw[b].matched : raw[a].rms < raw[b].rms; });
        Instance best = raw[g[0]];
        for (size_t k = 1; k < g.size(); ++k) {
            const Instance& o2 = raw[g[k]];
            if (o2.matched == best.matched && std::fabs(o2.rms - best.rms) < 1e-4 && quarter_index(o2.rot) != quarter_index(best.rot)) {
                best.ambiguous = true;
                best.alt_rots.push_back(o2.rot);
            }
        }
        d.ambiguous += best.ambiguous;
        d.found.push_back(best);
    }
    std::stable_sort(d.found.begin(), d.found.end(), [](const Instance& a, const Instance& b) {
        if (std::fabs(a.centre.y - b.centre.y) > 1.0) return a.centre.y < b.centre.y;
        return a.centre.x < b.centre.x;
    });
    for (int i = 0; i < (int)d.found.size(); ++i) if (!d.found[i].ambiguous) { d.root = i; break; }
    if (d.root < 0) { d.why = "every instance is ambiguous (symmetric circuit)"; return d; }

    // images relative to the root, in the CAD frame: J_k = A_root^-1 * A_k
    const Instance& r0 = d.found[d.root];
    const Xform A0 = Xform::translate(r0.offset.x, r0.offset.y) * Xform::rotation(r0.rot);
    const Xform A0i = A0.inverse();
    bool all_unrot = true;
    for (int i = 0; i < (int)d.found.size(); ++i) {
        const Instance& k = d.found[i];
        BBox bb;
        bb.add(A0i.apply({k.box.x0, k.box.y0})); bb.add(A0i.apply({k.box.x1, k.box.y1}));
        bb.add(A0i.apply({k.box.x0, k.box.y1})); bb.add(A0i.apply({k.box.x1, k.box.y0}));
        d.panel_box.add(bb);
        if (i == d.root) continue;
        const Xform J = A0i * Xform::translate(k.offset.x, k.offset.y) * Xform::rotation(k.rot);
        Vec2 off; double rot = 0; bool m = false;
        decompose_xform(J, off, rot, m);
        rot = nrm(std::round(rot * 1e6) / 1e6);
        all_unrot &= quarter_index(rot) == 0;
        d.images.push_back({off, rot});
    }
    // exact unrotated grid -> linear array
    if (o.allow_linear && all_unrot && !d.images.empty() && d.ambiguous == 0) {
        std::vector<double> xs{0.0}, ys{0.0};
        auto add = [&](std::vector<double>& v, double x) { for (double e : v) if (std::fabs(e - x) <= o.tol * 2) return; v.push_back(x); };
        for (auto& im : d.images) { add(xs, im.first.x); add(ys, im.first.y); }
        std::sort(xs.begin(), xs.end()); std::sort(ys.begin(), ys.end());
        const int nx = (int)xs.size(), ny = (int)ys.size();
        auto pitch = [&](const std::vector<double>& v, double& out) {
            if (v.size() < 2) { out = 0; return true; }
            out = (v.back() - v.front()) / (v.size() - 1);
            for (size_t i = 1; i < v.size(); ++i) if (std::fabs(v[i] - v[i - 1] - out) > o.tol * 2) return false;
            return true;
        };
        double dx = 0, dy = 0;
        if (nx * ny == (int)d.images.size() + 1 && pitch(xs, dx) && pitch(ys, dy)) {
            d.linear = true;
            d.nx = nx; d.ny = ny; d.dx = dx; d.dy = dy;
            for (int i = 0; i < nx; ++i) if (std::fabs(xs[i]) <= o.tol * 2) d.root_col = i;
            for (int i = 0; i < ny; ++i) if (std::fabs(ys[i]) <= o.tol * 2) d.root_row = i;
        }
    }
    d.ok = true;
    return d;
}

bool apply(Project& p, const Detection& d, const ApplyOptions& a, std::string& err) {
    err.clear();
    if (!d.ok || d.root < 0) { err = d.why.empty() ? "nothing detected" : d.why; return false; }
    if (has_array(p) && !a.replace_array) { err = "the project already has a panel array (pass replace_array)"; return false; }
    // instances to keep
    std::vector<ArrayInstance> inst;
    const Instance& r0 = d.found[d.root];
    const Xform A0 = Xform::translate(r0.offset.x, r0.offset.y) * Xform::rotation(r0.rot);
    const Xform A0i = A0.inverse();
    bool changed_set = false;
    for (int i = 0; i < (int)d.found.size(); ++i) {
        if (i == d.root) continue;
        if (std::find(a.exclude.begin(), a.exclude.end(), i) != a.exclude.end()) { changed_set = true; continue; }
        const Instance& k = d.found[i];
        if (k.ambiguous && !a.include_ambiguous) { changed_set = true; continue; }
        const Xform J = A0i * Xform::translate(k.offset.x, k.offset.y) * Xform::rotation(k.rot);
        ArrayInstance ai; bool m = false;
        decompose_xform(J, ai.offset, ai.rot, m);
        ai.rot = nrm(std::round(ai.rot * 1e6) / 1e6);
        inst.push_back(ai);
    }
    if (inst.empty()) { err = "no instance besides the reference circuit"; return false; }
    // panel layers onto the CAD
    std::vector<int> mv = a.move_layers;
    if (mv.empty()) for (int i = 0; i < (int)p.layers.size(); ++i) mv.push_back(i);
    Project before = p;
    for (int i : mv) if (i >= 0 && i < (int)p.layers.size()) premultiply_layer(p.layers[i], A0i);
    bool ok;
    if (d.linear && !changed_set) ok = create_linear_array(p, d.nx, d.ny, d.dx, d.dy, d.root_col, d.root_row, &err);
    else ok = create_nonlinear_array(p, inst, &err);
    if (!ok) { p = std::move(before); return false; }
    p.dirty = true;
    return true;
}

} // namespace pcbp::panel_auto
