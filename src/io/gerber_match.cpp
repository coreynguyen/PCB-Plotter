// gerber_match.cpp — R22b, see gerber_match.h
#include "io/gerber_match.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <unordered_map>

namespace pcbp::gmatch {

namespace gb = pcbp::gerber;

// ---- selection ----------------------------------------------------------------------------------
int ElemSelection::apply(const std::vector<ElemRef>& hits, bool remove) {
    int n = 0;
    for (const ElemRef& h : hits) {
        auto it = std::find(items.begin(), items.end(), h);
        if (remove) { if (it != items.end()) { items.erase(it); ++n; } }
        else if (it == items.end()) { items.push_back(h); ++n; }
    }
    return n;
}

bool ElemSelection::contains(const ElemRef& r) const { return std::find(items.begin(), items.end(), r) != items.end(); }

// ---- features -----------------------------------------------------------------------------------
static BBox xbox(const Xform& t, const BBox& b) {
    BBox r;
    if (b.empty()) return r;
    for (Vec2 q : {Vec2{b.x0, b.y0}, Vec2{b.x1, b.y0}, Vec2{b.x1, b.y1}, Vec2{b.x0, b.y1}}) r.add(t.apply(q));
    return r;
}

Feature object_feature(const gb::Layer& L, const gb::Object& o, const Xform& t, int layer, int obj) {
    Feature f;
    const BBox b = xbox(t, gb::object_bbox(L, o));
    f.c = b.center(); f.w = b.w(); f.h = b.h();
    f.layer = layer; f.obj = obj;
    f.kind = o.kind == gb::Object::Flash ? FFlash : o.kind == gb::Object::Draw ? FDraw : o.kind == gb::Object::Arc ? FArc : FRegion;
    return f;
}

std::vector<Feature> layer_features(const gb::Layer& L, const Xform& t, int layer, const std::vector<char>* hidden) {
    std::vector<Feature> v;
    v.reserve(L.objs.size());
    for (int i = 0; i < (int)L.objs.size(); ++i) {
        const gb::Object& o = L.objs[i];
        if (o.clear) continue;
        if (hidden && i < (int)hidden->size() && (*hidden)[i]) continue;
        Feature f = object_feature(L, o, t, layer, i);
        if (f.w <= 0 && f.h <= 0) continue;
        v.push_back(f);
    }
    return v;
}

BBox features_box(const std::vector<Feature>& f) {
    BBox b;
    for (auto& q : f) { b.add(Vec2{q.c.x - q.w / 2, q.c.y - q.h / 2}); b.add(Vec2{q.c.x + q.w / 2, q.c.y + q.h / 2}); }
    return b;
}

Vec2 selection_centre(const std::vector<Feature>& f) { return features_box(f).center(); }

// ---- pitch --------------------------------------------------------------------------------------
PitchResult pitch_from_centres(Vec2 a, Vec2 b, PitchMode m, double min_pitch, double axis_tol) {
    PitchResult r;
    const double dx = b.x - a.x, dy = b.y - a.y;
    char t[200];
    switch (m) {
    case PitchMode::Diagonal:
        r.dx = dx; r.dy = dy;
        if (std::fabs(dx) < min_pitch || std::fabs(dy) < min_pitch) {
            std::snprintf(t, sizeof t, "the second selection is not diagonal to the first (dX %.3f, dY %.3f mm): use One across or One up for a single row / column", dx, dy);
            r.why = t; return r;
        }
        break;
    case PitchMode::Across:
        r.dx = dx; r.dy = 0; r.off_axis = std::fabs(dy);
        if (std::fabs(dx) < min_pitch) { r.why = "the second selection is not one circuit across (dX too small)"; return r; }
        if (r.off_axis > axis_tol) { std::snprintf(t, sizeof t, "the circuits are %.3f mm out of line in Y (Gerbers rotated? align them first)", dy); r.why = t; }
        break;
    case PitchMode::Up:
        r.dx = 0; r.dy = dy; r.off_axis = std::fabs(dx);
        if (std::fabs(dy) < min_pitch) { r.why = "the second selection is not one circuit up (dY too small)"; return r; }
        if (r.off_axis > axis_tol) { std::snprintf(t, sizeof t, "the circuits are %.3f mm out of line in X (Gerbers rotated? align them first)", dx); r.why = t; }
        break;
    }
    r.ok = true;
    return r;
}

// ---- pattern search -----------------------------------------------------------------------------
Vec2 rot_quarter(Vec2 p, int q) {
    switch (((q % 4) + 4) % 4) {
    case 1: return {-p.y, p.x};
    case 2: return {-p.x, -p.y};
    case 3: return {p.y, -p.x};
    default: return p;
    }
}

namespace {
struct Grid {
    double cell = 1;
    std::unordered_multimap<long long, int> m;
    long long key(double x, double y) const {
        const long long ix = (long long)std::floor(x / cell), iy = (long long)std::floor(y / cell);
        return (ix << 32) ^ (iy & 0xFFFFFFFFLL);
    }
    void add(Vec2 p, int i) { m.emplace(key(p.x, p.y), i); }
    template <class F> void near(Vec2 p, F&& f) const {
        const long long ix = (long long)std::floor(p.x / cell), iy = (long long)std::floor(p.y / cell);
        for (long long dx = -1; dx <= 1; ++dx)
            for (long long dy = -1; dy <= 1; ++dy) {
                auto r = m.equal_range(((ix + dx) << 32) ^ ((iy + dy) & 0xFFFFFFFFLL));
                for (auto it = r.first; it != r.second; ++it) f(it->second);
            }
    }
};
bool size_ok(double a, double b, const MatchOptions& o) { return std::fabs(a - b) <= o.size_tol + o.size_rel * std::max(a, b); }
// does feature g match reference feature r turned by q quarter turns (sizes swap on odd q)?
bool same_shape(const Feature& r, const Feature& g, int q, const MatchOptions& o) {
    if (r.layer != g.layer || r.kind != g.kind) return false;
    const double w = (q & 1) ? r.h : r.w, h = (q & 1) ? r.w : r.h;
    return size_ok(w, g.w, o) && size_ok(h, g.h, o);
}
}

std::vector<Candidate> find_pattern(const std::vector<Feature>& all, const std::vector<int>& ref0, const MatchOptions& o, std::string* why) {
    std::vector<Candidate> out;
    auto fail = [&](const std::string& s) { if (why) *why = s; return out; };
    std::vector<int> ref;
    for (int i : ref0) if (i >= 0 && i < (int)all.size() && std::find(ref.begin(), ref.end(), i) == ref.end()) ref.push_back(i);
    if (ref.empty()) return fail("nothing selected");
    if (ref.size() < 2) return fail("select at least two elements (one element has no orientation)");
    std::vector<Feature> rf;
    for (int i : ref) rf.push_back(all[i]);
    const Vec2 rc = selection_centre(rf);
    const double tol = std::max(1e-6, o.tol);
    Grid g;
    g.cell = std::max(4 * tol, 0.25);
    for (int i = 0; i < (int)all.size(); ++i) g.add(all[i].c, i);
    // anchor = the reference feature whose shape is rarest in the artwork (fewest trial offsets)
    int anchor = 0;
    size_t best = SIZE_MAX;
    for (int k = 0; k < (int)rf.size(); ++k) {
        size_t n = 0;
        for (auto& f : all) if (same_shape(rf[k], f, 0, o) || same_shape(rf[k], f, 1, o)) ++n;
        if (n < best) { best = n; anchor = k; }
    }
    const int need = std::max(1, (int)std::ceil(o.min_fraction * rf.size() - 1e-9));
    std::vector<Candidate> raw;
    for (int q = 0; q < 4; ++q) {
        if (!o.rot[q]) continue;
        const Vec2 ra = rot_quarter(rf[anchor].c, q);
        for (int fi = 0; fi < (int)all.size(); ++fi) {
            const Feature& f = all[fi];
            if (!same_shape(rf[anchor], f, q, o)) continue;
            const Vec2 off = f.c - ra;
            bool dup = false;
            for (auto& c : raw) if ((int)std::lround(c.rot / 90) == q && dist(c.offset, off) <= tol) { dup = true; break; }
            if (dup) continue;
            Candidate c;
            c.offset = off; c.rot = q * 90.0; c.total = (int)rf.size();
            double se = 0;
            std::vector<int> used;
            for (int k = 0; k < (int)rf.size(); ++k) {
                if ((int)rf.size() - k + c.matched < need) break;   // cannot reach `need` any more
                const Vec2 p = rot_quarter(rf[k].c, q) + off;
                int bi = -1; double bd = tol;
                g.near(p, [&](int j) {
                    const double d = dist(all[j].c, p);
                    if (d <= bd && same_shape(rf[k], all[j], q, o) && std::find(used.begin(), used.end(), j) == used.end()) { bd = d; bi = j; }
                });
                if (bi < 0) continue;
                used.push_back(bi);
                ++c.matched; se += bd * bd;
            }
            if (c.matched < need) continue;
            c.rms = std::sqrt(se / std::max(1, c.matched));
            c.feats = used;
            std::vector<Feature> mf;
            for (int j : used) mf.push_back(all[j]);
            c.box = features_box(mf);
            c.centre = rot_quarter(rc, q) + off;
            raw.push_back(std::move(c));
        }
    }
    // drop the reference itself and its self-symmetries (a match landing on the reference selection)
    for (auto& c : raw) if (dist(c.centre, rc) > std::max(tol * 4, 0.1)) out.push_back(std::move(c));
    // same place, different rotation: the group is symmetric -> ambiguous (only the first stays preferred)
    for (size_t i = 0; i < out.size(); ++i)
        for (size_t j = 0; j < i; ++j)
            if (!out[j].ambiguous && dist(out[i].centre, out[j].centre) <= std::max(tol * 4, 0.1)) { out[i].ambiguous = true; break; }
    std::stable_sort(out.begin(), out.end(), [&](const Candidate& a, const Candidate& b) {
        if (std::fabs(a.centre.y - b.centre.y) > 1.0) return a.centre.y < b.centre.y;
        return a.centre.x < b.centre.x;
    });
    if (out.empty()) {
        char t[200];
        std::snprintf(t, sizeof t, "no other copy of the %d selected element(s) at 0/90/180/270\xC2\xB0 within %.3f mm", (int)rf.size(), tol);
        return fail(t);
    }
    return out;
}

// ---- outlines -----------------------------------------------------------------------------------
bool outline_element(const gb::Object& o) {
    return (o.kind == gb::Object::Draw && dist(o.a, o.b) > 1e-6) || o.kind == gb::Object::Arc || (o.kind == gb::Object::Region && o.poly.size() >= 3);
}

std::vector<Vec2> element_polyline(const gb::Layer&, const gb::Object& o, const Xform& t, bool& closed, int arc_segs) {
    std::vector<Vec2> pts;
    closed = false;
    if (o.kind == gb::Object::Draw) pts = {o.a, o.b};
    else if (o.kind == gb::Object::Arc) pts = gb::arc_points(o, arc_segs);
    else if (o.kind == gb::Object::Region) { pts = o.poly; closed = true; }
    if (o.kind == gb::Object::Arc && !pts.empty()) { pts.front() = o.a; pts.back() = o.b; }   // exact endpoints
    for (auto& q : pts) q = t.apply(q);
    if (closed && pts.size() > 1 && dist(pts.front(), pts.back()) < 1e-9) pts.pop_back();
    return pts;
}

OutlineBuild build_outline(const std::vector<std::vector<Vec2>>& segs, const std::vector<std::vector<Vec2>>& closed_in, double tol) {
    OutlineBuild r;
    for (auto& s : segs) for (auto& q : s) r.box.add(q);
    for (auto& s : closed_in) for (auto& q : s) r.box.add(q);
    std::vector<std::vector<Vec2>> closed = closed_in, open;
    std::vector<char> used(segs.size(), 0);
    auto take = [&](Vec2 p, bool& rev) -> int {
        int bi = -1; double bd = tol;
        for (int s = 0; s < (int)segs.size(); ++s) {
            if (used[s] || segs[s].size() < 2) continue;
            const double d0 = dist(segs[s].front(), p), d1 = dist(segs[s].back(), p);
            if (d0 <= bd) { bd = d0; bi = s; rev = false; }
            if (d1 < bd || (bi < 0 && d1 <= bd)) { bd = d1; bi = s; rev = true; }
        }
        return bi;
    };
    for (int s = 0; s < (int)segs.size(); ++s) {
        if (used[s] || segs[s].size() < 2) continue;
        used[s] = 1;
        std::vector<Vec2> chain = segs[s];
        for (size_t guard = 0; guard < segs.size(); ++guard) {
            if (chain.size() > 2 && dist(chain.back(), chain.front()) <= tol) break;
            bool rev = false;
            int nx = take(chain.back(), rev);
            if (nx < 0) {   // extend at the front too (the first segment may be in the middle of a chain)
                nx = take(chain.front(), rev);
                if (nx < 0) break;
                used[nx] = 1;
                std::vector<Vec2> add = segs[nx];
                if (!rev) std::reverse(add.begin(), add.end());   // add ends at chain.front()
                chain.insert(chain.begin(), add.begin(), add.end() - 1);
                continue;
            }
            used[nx] = 1;
            std::vector<Vec2> add = segs[nx];
            if (rev) std::reverse(add.begin(), add.end());
            chain.insert(chain.end(), add.begin() + 1, add.end());   // the chain keeps its exact endpoint
        }
        if (chain.size() > 3 && dist(chain.back(), chain.front()) <= tol) { chain.pop_back(); closed.push_back(chain); }
        else open.push_back(chain);
    }
    // a closed contour spanning the whole selection = the outline; otherwise every chain is returned
    int bi = -1; double ba = -1;
    for (int i = 0; i < (int)closed.size(); ++i) {
        BBox b; for (auto& q : closed[i]) b.add(q);
        if (b.w() >= r.box.w() - tol * 2 && b.h() >= r.box.h() - tol * 2 && b.w() * b.h() > ba) { ba = b.w() * b.h(); bi = i; }
    }
    if (bi >= 0) { r.polys = {closed[bi]}; r.closed = true; return r; }
    r.polys = closed;
    for (auto& c : open) r.polys.push_back(c);
    return r;
}

} // namespace pcbp::gmatch
