// ============================================================================
// digitize.cpp — Gerber pad-group digitizer and auto-scan (portable C++20).
// Own algorithm, designed from the CircuitCAM manual's behavioural
// description ("Digitizing Tips", Gerber tasks). See digitize.h.
// ============================================================================
#include "digitize.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <numeric>
#include <set>
#include <utility>
#include <iterator>

namespace pcbp::digitize {

using gerber::Aperture;
using gerber::Layer;
using gerber::Object;

namespace {

long long gkey(long long x, long long y) { return x * 2000003LL + y; }

struct DSU {
    std::vector<int> p;
    explicit DSU(size_t n) : p(n) { std::iota(p.begin(), p.end(), 0); }
    int f(int x) { while (p[x] != x) x = p[x] = p[p[x]]; return x; }
    void u(int a, int b) { a = f(a); b = f(b); if (a != b) p[b] = a; }
};

double seg_dist(Vec2 p, Vec2 a, Vec2 b) {
    Vec2 ab = b - a, ap = p - a;
    double l2 = ab.x * ab.x + ab.y * ab.y;
    double t = l2 > 0 ? (ap.x * ab.x + ap.y * ab.y) / l2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    return dist(p, a + ab * t);
}

ShapeClass class_of(PadShape s) {
    switch (s) {
    case PadShape::Round: return ShapeClass::Round;
    case PadShape::Square:
    case PadShape::Rect: return ShapeClass::Rect;
    case PadShape::Oblong: return ShapeClass::Oblong;
    default: return ShapeClass::Other;
    }
}

PadShape shape_from_ap(const Aperture& a) {
    switch (a.shape) {
    case Aperture::Circle: return PadShape::Round;
    case Aperture::Rect: return std::fabs(a.w - a.h) < 1e-6 ? PadShape::Square : PadShape::Rect;
    case Aperture::Obround: return PadShape::Oblong;
    case Aperture::Polygon: return a.verts >= 8 ? PadShape::Octagon : PadShape::Round;
    default: return PadShape::Rect;
    }
}

bool box_inside(const BBox& in, const BBox& out) {
    return in.x0 >= out.x0 && in.x1 <= out.x1 && in.y0 >= out.y0 && in.y1 <= out.y1;
}

// cluster of 1D values within tol -> representative values + membership
std::vector<double> lines_of(const std::vector<double>& v, double tol, std::vector<int>* member = nullptr) {
    std::vector<int> idx(v.size());
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), [&](int a, int b) { return v[a] < v[b]; });
    std::vector<double> lines;
    std::vector<int> cnt;
    if (member) member->assign(v.size(), -1);
    for (int i : idx) {
        if (lines.empty() || v[i] - lines.back() > tol) { lines.push_back(v[i]); cnt.push_back(0); }
        if (member) (*member)[i] = (int)lines.size() - 1;
        cnt.back()++;
    }
    return lines;
}

double norm180(double a) { a = std::fmod(a, 180.0); if (a < 0) a += 180.0; return a; }
// signed difference of two axis directions modulo `period` (90 or 180), in (-period/2, period/2]
double axis_diff(double a, double b, double period) {
    double d = std::fmod(a - b, period);
    if (d <= -period / 2) d += period;
    if (d > period / 2) d -= period;
    return d;
}

std::vector<Vec2> convex_hull(std::vector<Vec2> p) {
    std::sort(p.begin(), p.end(), [](Vec2 a, Vec2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    p.erase(std::unique(p.begin(), p.end(), [](Vec2 a, Vec2 b) { return std::fabs(a.x - b.x) < 1e-12 && std::fabs(a.y - b.y) < 1e-12; }), p.end());
    if (p.size() < 3) return p;
    std::vector<Vec2> h(2 * p.size());
    size_t k = 0;
    auto cross = [](Vec2 o, Vec2 a, Vec2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };
    for (size_t i = 0; i < p.size(); ++i) { while (k >= 2 && cross(h[k - 2], h[k - 1], p[i]) <= 0) --k; h[k++] = p[i]; }
    for (size_t i = p.size() - 1, t = k + 1; i > 0; --i) { while (k >= t && cross(h[k - 2], h[k - 1], p[i - 1]) <= 0) --k; h[k++] = p[i - 1]; }
    h.resize(k - 1);
    return h;
}

// orientation of a pad feature from its source objects
void orient_feature(const Layer& L, PadFeature& f, std::map<int, OBox>& apcache, std::map<int, bool>& apround) {
    f.oriented = false;
    if (f.objs.size() != 1) {
        // stacked identical flashes at one point behave like one
        bool same = f.flash && !f.objs.empty();
        for (int o : f.objs) same &= dist(L.objs[o].a, L.objs[f.objs[0]].a) < 1e-6;
        if (!same) return;
    }
    const Object& o = L.objs[f.objs[0]];
    if (o.kind == Object::Flash) {
        const Aperture* ap = gerber::find_ap(L, o.dcode);
        if (!ap) return;
        if (ap->shape == Aperture::Circle || ap->shape == Aperture::Polygon) return;
        if (ap->shape == Aperture::Rect || ap->shape == Aperture::Obround) {
            f.oriented = true;
            f.ow = std::max(ap->w, ap->h); f.oh = std::min(ap->w, ap->h); f.oang = ap->w >= ap->h ? 0 : 90;
            return;
        }
        auto it = apcache.find(o.dcode);
        if (it == apcache.end()) {
            std::vector<bool> clr;
            auto polys = gerber::flash_polys(*ap, Vec2{0, 0}, 48, &clr);
            std::vector<Vec2> pts;
            for (size_t k = 0; k < polys.size(); ++k) if (k >= clr.size() || !clr[k]) pts.insert(pts.end(), polys[k].begin(), polys[k].end());
            OBox ob = min_area_rect(pts);
            // a round-ish macro (hull fills ~pi/4 of a near-square box) has no orientation
            bool round = ob.w > 0 && std::fabs(ob.w - ob.h) < 0.03 * ob.w && ob.hull_area < 0.86 * ob.w * ob.h;
            it = apcache.emplace(o.dcode, ob).first;
            apround[o.dcode] = round;
        }
        if (apround[o.dcode] || it->second.w <= 0) return;
        f.oriented = true;
        f.ow = it->second.w; f.oh = it->second.h; f.oang = it->second.ang;
        return;
    }
    if (o.kind == Object::Region) {
        OBox ob = min_area_rect(o.poly);
        if (ob.w <= 0) return;
        if (std::fabs(ob.w - ob.h) < 0.03 * ob.w && ob.hull_area < 0.86 * ob.w * ob.h) return;   // round
        f.oriented = true;
        f.ow = ob.w; f.oh = ob.h; f.oang = ob.ang;
        return;
    }
    if (o.kind == Object::Draw) {
        const Aperture* ap = gerber::find_ap(L, o.dcode);
        if (!ap || ap->shape != Aperture::Circle) return;
        Vec2 d = o.b - o.a;
        double len = d.len();
        if (len < 1e-6) return;
        f.oriented = true;
        f.ow = len + ap->w; f.oh = ap->w;
        f.oang = norm180(rad2deg(std::atan2(d.y, d.x)));
        if (std::fabs(axis_diff(f.oang, 0, 90)) < 0.02) f.oang = std::round(f.oang / 90) * 90;
        f.oang = norm180(f.oang);
    }
}

} // namespace

std::string fmt_mm(double v, int decimals) {
    char b[32];
    std::snprintf(b, sizeof b, "%.*f", decimals, v);
    std::string s = b;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

OBox min_area_rect(const std::vector<Vec2>& pts) {
    OBox best;
    auto h = convex_hull(pts);
    if (h.size() < 2) return best;
    double area = 0;
    for (size_t k = 0, j = h.size() - 1; k < h.size(); j = k++) area += h[j].x * h[k].y - h[k].x * h[j].y;
    area = std::fabs(area) / 2;
    double ba = 1e300;
    for (size_t k = 0; k < h.size(); ++k) {
        Vec2 e = h[(k + 1) % h.size()] - h[k];
        double l = e.len();
        if (l < 1e-12) continue;
        Vec2 u = e * (1.0 / l), v{-u.y, u.x};
        double u0 = 1e300, u1 = -1e300, v0 = 1e300, v1 = -1e300;
        for (Vec2 p : h) {
            double a = p.x * u.x + p.y * u.y, b = p.x * v.x + p.y * v.y;
            u0 = std::min(u0, a); u1 = std::max(u1, a); v0 = std::min(v0, b); v1 = std::max(v1, b);
        }
        double ar = (u1 - u0) * (v1 - v0);
        // prefer the axis-aligned solution on ties (flattened arcs make many near-equal boxes)
        double ang = norm180(rad2deg(std::atan2(u.y, u.x)));
        double tie = std::fabs(axis_diff(ang, 0, 90)) < 1e-6 ? -1e-9 * ar : 0;
        if (ar + tie < ba - 1e-12 * std::max(1.0, ar)) {
            ba = ar + tie;
            double cu = (u0 + u1) / 2, cv = (v0 + v1) / 2;
            best.c = u * cu + v * cv;
            best.w = u1 - u0; best.h = v1 - v0;
            best.ang = ang;
        }
    }
    if (best.w < best.h) { std::swap(best.w, best.h); best.ang = norm180(best.ang + 90); }
    // snap numerical noise
    double r = std::round(best.ang * 1000) / 1000;
    best.ang = norm180(r);
    if (std::fabs(axis_diff(best.ang, 0, 90)) < 0.01) best.ang = norm180(std::round(best.ang / 90) * 90);
    best.hull_area = area;
    return best;
}

// ---------------------------------------------------------------------------
// PadSet
// ---------------------------------------------------------------------------
std::vector<int> PadSet::query(const BBox& b) const {
    std::vector<int> out;
    if (b.empty()) return out;
    long long x0 = (long long)std::floor(b.x0 / cell), x1 = (long long)std::floor(b.x1 / cell);
    long long y0 = (long long)std::floor(b.y0 / cell), y1 = (long long)std::floor(b.y1 / cell);
    if ((x1 - x0 + 1) * (y1 - y0 + 1) > 250000) {  // huge window: brute force
        for (int i = 0; i < (int)pads.size(); ++i) if (b.contains(pads[i].c)) out.push_back(i);
        return out;
    }
    for (long long x = x0; x <= x1; ++x)
        for (long long y = y0; y <= y1; ++y) {
            auto it = grid.find(gkey(x, y));
            if (it == grid.end()) continue;
            for (int i : it->second) if (b.contains(pads[i].c)) out.push_back(i);
        }
    return out;
}

PadSet extract_pads(const Layer& L, const DigitizeOptions& opt) {
    PadSet ps;
    std::set<int> hidden(opt.hidden_dcodes.begin(), opt.hidden_dcodes.end());

    // ---- 1. flashes (merge stacked ones) ----
    std::vector<int> fl;
    for (int i = 0; i < (int)L.objs.size(); ++i) {
        const Object& o = L.objs[i];
        if (o.kind == Object::Flash && !o.clear) fl.push_back(i);
    }
    {
        DSU d(fl.size());
        if (opt.merge_stacked_flashes) {
            std::unordered_map<long long, std::vector<int>> g;
            const double cs = 1.0;
            for (int k = 0; k < (int)fl.size(); ++k) {
                Vec2 p = L.objs[fl[k]].a;
                g[gkey((long long)std::floor(p.x / cs), (long long)std::floor(p.y / cs))].push_back(k);
            }
            for (int k = 0; k < (int)fl.size(); ++k) {
                const Object& o = L.objs[fl[k]];
                BBox bk = gerber::object_bbox(L, o);
                Vec2 p = o.a;
                long long gx = (long long)std::floor(p.x / cs), gy = (long long)std::floor(p.y / cs);
                for (long long dx = -1; dx <= 1; ++dx)
                    for (long long dy = -1; dy <= 1; ++dy) {
                        auto it = g.find(gkey(gx + dx, gy + dy));
                        if (it == g.end()) continue;
                        for (int m : it->second) {
                            if (m <= k) continue;
                            const Object& q = L.objs[fl[m]];
                            if (q.dcode != o.dcode) continue;   // stacked copies of one aperture
                            BBox bm = gerber::object_bbox(L, q);
                            if (!(bk.contains(q.a) || bm.contains(o.a))) continue;
                            // macro apertures (rotated RoundRect / RotRect ...): the bbox of a 45 deg
                            // pad covers its neighbours at fine pitch; test the real shape
                            const Aperture* ma = gerber::find_ap(L, o.dcode);
                            if (ma && ma->shape == Aperture::Macro && !ma->macro_polys.empty()) {
                                bool inside = false;
                                const Vec2 rel = q.a - o.a;
                                for (size_t pi = 0; pi < ma->macro_polys.size() && !inside; ++pi) {
                                    if (pi < ma->macro_clear.size() && ma->macro_clear[pi]) continue;
                                    const auto& poly = ma->macro_polys[pi];
                                    bool in = false;
                                    for (size_t a2 = 0, b2 = poly.size() - 1; a2 < poly.size(); b2 = a2++)
                                        if ((poly[a2].y > rel.y) != (poly[b2].y > rel.y) &&
                                            rel.x < (poly[b2].x - poly[a2].x) * (rel.y - poly[a2].y) / (poly[b2].y - poly[a2].y) + poly[a2].x) in = !in;
                                    inside = in;
                                }
                                if (!inside) continue;
                            }
                            d.u(k, m);
                        }
                    }
            }
        }
        std::map<int, std::vector<int>> groups;
        for (int k = 0; k < (int)fl.size(); ++k) groups[d.f(k)].push_back(k);
        for (auto& [root, mem] : groups) {
            PadFeature f;
            f.flash = true;
            const Object& o0 = L.objs[fl[mem[0]]];
            f.dcode = o0.dcode;
            const Aperture* ap = gerber::find_ap(L, o0.dcode);
            for (int k : mem) { f.objs.push_back(fl[k]); f.box.add(gerber::object_bbox(L, L.objs[fl[k]])); }
            f.c = f.box.center();
            f.w = f.box.w();
            f.h = f.box.h();
            if (ap) {
                f.shape = shape_from_ap(*ap);
                f.hole = ap->hole;
                if (mem.size() > 1 && f.shape == PadShape::Round) f.shape = PadShape::Oblong;
                if (f.shape == PadShape::Square && std::fabs(f.w - f.h) > 0.02) f.shape = PadShape::Rect;
                if (f.shape == PadShape::Rect && std::fabs(f.w - f.h) <= 0.02) f.shape = PadShape::Square;
            }
            f.cls = class_of(f.shape);
            if (std::max(f.w, f.h) > opt.max_pad_mm || std::max(f.w, f.h) < opt.min_pad_mm) continue;
            if (hidden.count(f.dcode)) continue;
            ps.pads.push_back(std::move(f));
        }
    }

    // ---- 2. rastered pads: clusters of same-dcode draws ----
    struct Seg { Vec2 a, b; double hw, hh; bool round; int obj; BBox box; };
    std::map<int, std::vector<Seg>> by_d;
    for (int i = 0; i < (int)L.objs.size(); ++i) {
        const Object& o = L.objs[i];
        if (o.clear || (o.kind != Object::Draw && o.kind != Object::Arc)) continue;
        const Aperture* ap = gerber::find_ap(L, o.dcode);
        if (!ap || hidden.count(o.dcode)) continue;
        BBox bb = gerber::object_bbox(L, o);
        if (std::max(bb.w(), bb.h()) > opt.max_pad_mm) continue;   // long draws are traces
        bool round = ap->shape == Aperture::Circle || ap->shape == Aperture::Polygon || ap->shape == Aperture::Obround;
        double hw = ap->w / 2, hh = ap->h / 2;
        if (o.kind == Object::Draw) by_d[o.dcode].push_back({o.a, o.b, hw, hh, round, i, bb});
        else {
            auto pts = gerber::arc_points(o, 48);
            for (size_t k = 1; k < pts.size(); ++k) {
                BBox sb; sb.add(pts[k - 1]); sb.add(pts[k]); sb.inflate(std::max(hw, hh));
                by_d[o.dcode].push_back({pts[k - 1], pts[k], hw, hh, round, i, sb});
            }
        }
    }
    for (auto& [dcode, segs] : by_d) {
        const double cs = std::max(0.5, opt.max_pad_mm / 4);
        std::unordered_map<long long, std::vector<int>> g;
        for (int k = 0; k < (int)segs.size(); ++k) {
            const BBox& b = segs[k].box;
            for (long long x = (long long)std::floor(b.x0 / cs); x <= (long long)std::floor(b.x1 / cs); ++x)
                for (long long y = (long long)std::floor(b.y0 / cs); y <= (long long)std::floor(b.y1 / cs); ++y)
                    g[gkey(x, y)].push_back(k);
        }
        DSU d(segs.size());
        const double eps = 1e-4;
        for (auto& [key, mem] : g)
            for (size_t i = 0; i < mem.size(); ++i)
                for (size_t j = i + 1; j < mem.size(); ++j) {
                    const BBox &a = segs[mem[i]].box, &b = segs[mem[j]].box;
                    if (a.x0 <= b.x1 + eps && b.x0 <= a.x1 + eps && a.y0 <= b.y1 + eps && b.y0 <= a.y1 + eps) d.u(mem[i], mem[j]);
                }
        std::map<int, std::vector<int>> cl;
        for (int k = 0; k < (int)segs.size(); ++k) cl[d.f(k)].push_back(k);
        for (auto& [root, mem] : cl) {
            BBox bb;
            std::set<int> objs;
            for (int k : mem) { bb.add(segs[k].box); objs.insert(segs[k].obj); }
            double w = bb.w(), h = bb.h();
            double mx = std::max(w, h), mn = std::min(w, h);
            if (mx > opt.max_pad_mm || mn < opt.min_pad_mm) continue;
            const Seg& s0 = segs[mem[0]];
            PadFeature f;
            f.dcode = dcode;
            f.box = bb;
            f.c = bb.center();
            f.w = w;
            f.h = h;
            f.objs.assign(objs.begin(), objs.end());
            if (objs.size() == 1) {
                // a single stroke: a pad only if the pen is thick and the stroke short
                if (2 * std::min(s0.hw, s0.hh) < opt.single_draw_min_width) continue;
                if (mx / std::max(mn, 1e-9) > opt.single_draw_max_aspect) continue;
                f.raster = false;
                f.shape = s0.round ? (std::fabs(w - h) < 0.02 ? PadShape::Round : PadShape::Oblong)
                                   : (std::fabs(w - h) < 0.02 ? PadShape::Square : PadShape::Rect);
                f.cls = class_of(f.shape);
                ps.pads.push_back(std::move(f));
                continue;
            }
            if (mx / std::max(mn, 1e-9) > opt.raster_max_aspect) continue;
            // a raster pad is filled by several pen widths; a blob about one pen
            // wide is a trace fragment
            if (mn < 2.5 * 2 * std::min(s0.hw, s0.hh)) continue;
            // coverage on a grid
            const int N = 24;
            int nx = std::max(4, (int)std::round(N * w / mx)), ny = std::max(4, (int)std::round(N * h / mx));
            int covered = 0;
            for (int ix = 0; ix < nx; ++ix)
                for (int iy = 0; iy < ny; ++iy) {
                    Vec2 p{bb.x0 + (ix + 0.5) * w / nx, bb.y0 + (iy + 0.5) * h / ny};
                    bool in = false;
                    for (int k : mem) {
                        const Seg& s = segs[k];
                        if (!s.box.contains(p)) continue;
                        if (s.round) in = seg_dist(p, s.a, s.b) <= s.hw + 1e-9;
                        else if (std::fabs(s.a.x - s.b.x) < 1e-9 || std::fabs(s.a.y - s.b.y) < 1e-9) in = true;  // axis-aligned square pen: box test
                        else in = seg_dist(p, s.a, s.b) <= std::min(s.hw, s.hh) + 1e-9;
                        if (in) break;
                    }
                    covered += in;
                }
            double cover = (double)covered / (nx * ny);
            if (cover < opt.raster_min_cover) continue;
            f.raster = true;
            bool sq = std::fabs(w - h) < 0.03 * mx;
            f.shape = (cover < 0.86 && sq) ? PadShape::Round : (sq ? PadShape::Square : PadShape::Rect);
            f.cls = class_of(f.shape);
            ps.pads.push_back(std::move(f));
        }
    }

    // ---- 3. small regions ----
    for (int i = 0; i < (int)L.objs.size(); ++i) {
        const Object& o = L.objs[i];
        if (o.kind != Object::Region || o.clear) continue;
        BBox bb = gerber::object_bbox(L, o);
        double mx = std::max(bb.w(), bb.h()), mn = std::min(bb.w(), bb.h());
        if (mx > opt.max_pad_mm || mn < opt.min_pad_mm) continue;
        PadFeature f;
        f.region = true;
        f.box = bb; f.c = bb.center(); f.w = bb.w(); f.h = bb.h();
        f.objs = {i};
        // area ratio decides rect vs round
        double a = 0;
        for (size_t k = 0, j = o.poly.size() - 1; k < o.poly.size(); j = k++) a += o.poly[j].x * o.poly[k].y - o.poly[k].x * o.poly[j].y;
        // fill relative to the oriented (min-area) box so rotated rect regions stay Rect
        OBox ob = min_area_rect(o.poly);
        const double rw = ob.w > 0 ? ob.w : bb.w(), rh = ob.w > 0 ? ob.h : bb.h();
        double fill = std::fabs(a) / 2 / std::max(1e-12, rw * rh);
        bool sq = std::fabs(rw - rh) < 0.03 * std::max(rw, rh);
        f.shape = fill > 0.95 ? (sq ? PadShape::Square : PadShape::Rect) : (sq && fill > 0.7 ? PadShape::Round : PadShape::Octagon);
        f.cls = class_of(f.shape);
        ps.pads.push_back(std::move(f));
    }

    // ---- orientation (oriented boxes) ----
    {
        std::map<int, OBox> apc;
        std::map<int, bool> apr;
        for (auto& f : ps.pads) orient_feature(L, f, apc, apr);
    }

    // ---- index ----
    for (int i = 0; i < (int)ps.pads.size(); ++i) {
        Vec2 c = ps.pads[i].c;
        ps.grid[gkey((long long)std::floor(c.x / ps.cell), (long long)std::floor(c.y / ps.cell))].push_back(i);
    }
    return ps;
}

// ---------------------------------------------------------------------------
// signature
// ---------------------------------------------------------------------------
std::string pattern_signature(const std::vector<Pad>& pads) {
    const double q = 0.05;
    auto Q = [&](double v) { return (long long)std::llround(v / q); };
    std::string best;
    for (int k = 0; k < 4; ++k) {
        std::vector<std::string> items;
        for (const Pad& p : pads) {
            Vec2 r = rotate(p.pos, 90.0 * k);
            double w = (k & 1) ? p.h : p.w, h = (k & 1) ? p.w : p.h;
            char cls = "RSOX"[(int)class_of(p.shape)];
            char b[96];
            std::snprintf(b, sizeof b, "%c%lld,%lld,%lld,%lld", cls, Q(r.x), Q(r.y), Q(w), Q(h));
            items.push_back(b);
        }
        std::sort(items.begin(), items.end());
        std::string s;
        for (auto& it : items) { s += it; s += ';'; }
        if (k == 0 || s < best) best = s;
    }
    unsigned long long hsh = 1469598103934665603ULL;
    for (unsigned char c : best) { hsh ^= c; hsh *= 1099511628211ULL; }
    char out[64];
    std::snprintf(out, sizeof out, "P%zu-%016llx", pads.size(), hsh);
    return out;
}

// ---------------------------------------------------------------------------
// naming
// ---------------------------------------------------------------------------
std::string auto_package_name(const DigitizeResult& r) {
    const auto& pads = r.pkg.pads;
    int n = (int)pads.size();
    std::string p = fmt_mm(r.pitch);
    if (n == 0) return "EMPTY";
    if (n == 1) {
        const Pad& a = pads[0];
        if (a.shape == PadShape::Round) return "PAD_R" + fmt_mm(a.w);
        return "PAD_" + fmt_mm(a.w) + "x" + fmt_mm(a.h);
    }
    if (n == 2) {
        Vec2 d = pads[1].pos - pads[0].pos;
        bool alongx = std::fabs(d.x) >= std::fabs(d.y);
        if (r.smt) {
            BBox b;
            for (auto& q : pads) { b.add(q.pos - Vec2{q.w / 2, q.h / 2}); b.add(q.pos + Vec2{q.w / 2, q.h / 2}); }
            double L = alongx ? b.w() : b.h(), W = alongx ? b.h() : b.w();
            return "CHIP_" + fmt_mm(L) + "x" + fmt_mm(W);
        }
        return (r.pitch >= 7.5 ? "AXIAL_" : "RAD_") + p;
    }
    std::string nn = std::to_string(n);
    if (r.layout == "dual-x" || r.layout == "dual-y") {
        // row spacing
        double rs = 0;
        for (auto& q : pads) rs = std::max(rs, std::fabs(r.layout == "dual-x" ? q.pos.y : q.pos.x) * 2);
        if (!r.smt) return "DIP" + nn + (rs >= 12.0 ? "W" : "") + "_" + p;
        if (r.pitch < 1.0) return "SSOP" + nn + "_" + p;
        return "SOIC" + nn + (rs >= 7.5 ? "W" : "") + "_" + p;
    }
    if (r.layout == "row-x" || r.layout == "row-y") return (r.smt ? "SIL" : "SIP") + nn + "_" + p;
    if (r.layout == "quad") {
        if (!r.smt) return "QUAD" + nn + "_" + p;
        return (r.pitch < 1.0 ? "QFP" : "PLCC") + nn + "_" + p;
    }
    if (r.layout == "grid") return (r.smt ? "BGA" : "PGA") + nn + "_" + p;
    if (n == 3 && r.smt) return "SOT3_" + p;
    return (r.smt ? "SMT" : "TH") + nn + "_" + p;
}

// ---------------------------------------------------------------------------
// digitize
// ---------------------------------------------------------------------------
bool digitize_rect(const Layer& L, BBox sel, const DigitizeOptions& opt, DigitizeResult& out) {
    PadSet ps = extract_pads(L, opt);
    return digitize_rect(ps, sel, opt, out);
}

bool digitize_rect(const PadSet& ps, BBox sel, const DigitizeOptions& opt, DigitizeResult& out) {
    out = DigitizeResult{};
    std::set<int> hidden(opt.hidden_dcodes.begin(), opt.hidden_dcodes.end());
    std::vector<int> cand;
    BBox q = sel;
    q.inflate(opt.max_pad_mm);
    for (int i : ps.query(q)) {
        const PadFeature& f = ps.pads[i];
        if (!box_inside(f.box, sel)) continue;          // partially outside => ignored
        if (hidden.count(f.dcode)) { out.dropped.push_back(i); continue; }
        cand.push_back(i);
    }
    if (cand.empty()) { out.notes.push_back("no pad features fully inside the rectangle"); return false; }

    // stacked: centre inside a larger pad
    std::vector<int> keep;
    for (int i : cand) {
        const PadFeature& f = ps.pads[i];
        bool inner = false;
        for (int j : cand) {
            if (j == i) continue;
            const PadFeature& g = ps.pads[j];
            if (!g.box.contains(f.c)) continue;
            if (g.oriented) {   // rotated pads: the bbox covers fine-pitch neighbours; test the real box
                const Vec2 l = rotate(f.c - g.c, -g.oang);
                if (std::fabs(l.x) > g.ow / 2 + 1e-9 || std::fabs(l.y) > g.oh / 2 + 1e-9) continue;
            }
            double af = f.w * f.h, ag = g.w * g.h;
            bool dup = std::fabs(af - ag) <= 0.05 * std::max(af, ag) && dist(f.c, g.c) < 0.25 * std::min(f.w, f.h);
            if ((ag > af && !dup) || (dup && (g.flash > f.flash || (g.flash == f.flash && j < i)))) { inner = true; break; }
        }
        if (inner) out.dropped.push_back(i);
        else keep.push_back(i);
    }
    if (keep.size() != cand.size())
        out.notes.push_back("dropped " + std::to_string(cand.size() - keep.size()) + " feature(s) stacked on another pad");
    cand.swap(keep);

    // vias
    if (opt.drop_vias && cand.size() > 1) {
        double big = 0;
        for (int i : cand) big = std::max(big, std::max(ps.pads[i].w, ps.pads[i].h));
        int larger = 0;
        for (int i : cand) {
            const PadFeature& f = ps.pads[i];
            bool via = f.flash && f.cls == ShapeClass::Round && f.w < opt.via_ratio * big;
            if (!via) ++larger;
        }
        if (larger >= 2) {
            keep.clear();
            int nv = 0;
            for (int i : cand) {
                const PadFeature& f = ps.pads[i];
                if (f.flash && f.cls == ShapeClass::Round && f.w < opt.via_ratio * big) { out.dropped.push_back(i); ++nv; }
                else keep.push_back(i);
            }
            if (nv) out.notes.push_back("dropped " + std::to_string(nv) + " via(s) (round flash < " +
                                        fmt_mm(opt.via_ratio * big) + " mm)");
            cand.swap(keep);
        }
    }
    if (cand.empty()) return false;

    const int n = (int)cand.size();
    // working copies (centre/size) so extended pins can be truncated
    std::vector<Vec2> fc(n);
    std::vector<double> fw(n), fh(n);
    for (int k = 0; k < n; ++k) { fc[k] = ps.pads[cand[k]].c; fw[k] = ps.pads[cand[k]].w; fh[k] = ps.pads[cand[k]].h; }
    // truncate extended pins: a pad whose narrow side matches the modal pad but
    // whose long side is longer is cut back to the modal length, keeping the
    // edge nearest to the group centre
    if (n >= 4) {
        BBox pre;
        for (int k = 0; k < n; ++k) pre.add(fc[k]);
        Vec2 gc = pre.center();
        auto key = [](double a, double b) { return std::make_pair(std::llround(std::min(a, b) / 0.02), std::llround(std::max(a, b) / 0.02)); };
        std::map<std::pair<long long, long long>, int> hist;
        for (int k = 0; k < n; ++k) hist[key(fw[k], fh[k])]++;
        auto best = std::max_element(hist.begin(), hist.end(), [](auto& a, auto& b) { return a.second < b.second; });
        if (best->second >= 3) {
            double mshort = best->first.first * 0.02, mlong = best->first.second * 0.02;
            int ntr = 0;
            for (int k = 0; k < n; ++k) {
                bool horiz = fw[k] >= fh[k];
                double sh = horiz ? fh[k] : fw[k], lg = horiz ? fw[k] : fh[k];
                if (std::fabs(sh - mshort) > 0.03 || lg <= mlong + 0.05 || lg > 3 * mlong) continue;
                double cut = lg - mlong;
                if (horiz) { fc[k].x += (fc[k].x < gc.x ? 0.5 : -0.5) * cut; fw[k] = mlong; }
                else { fc[k].y += (fc[k].y < gc.y ? 0.5 : -0.5) * cut; fh[k] = mlong; }
                ++ntr;
            }
            if (ntr) out.notes.push_back("truncated " + std::to_string(ntr) + " extended pin(s)");
        }
    }
    // centroid = centre of pad extents
    BBox ext;
    for (int k = 0; k < n; ++k) { ext.add(fc[k] - Vec2{fw[k] / 2, fh[k] / 2}); ext.add(fc[k] + Vec2{fw[k] / 2, fh[k] / 2}); }
    out.centroid = ext.center();
    std::vector<Pad> pads(n);
    for (int k = 0; k < n; ++k) {
        const PadFeature& f = ps.pads[cand[k]];
        pads[k].pos = fc[k] - out.centroid;
        pads[k].w = fw[k];
        pads[k].h = fh[k];
        pads[k].shape = f.shape;
        pads[k].drill = f.hole;
    }
    // drill layer
    int drilled = 0;
    if (opt.drill) {
        for (int k = 0; k < n; ++k) {
            const PadFeature& f = ps.pads[cand[k]];
            double best = 0;
            for (auto& o : opt.drill->objs) {
                if (o.kind != Object::Flash) continue;
                if (dist(o.a, f.c) < std::min(f.w, f.h) / 2) {
                    const Aperture* a = gerber::find_ap(*opt.drill, o.dcode);
                    best = std::max(best, a ? a->w : 0.01);
                }
            }
            if (best > 0) { pads[k].drill = best; ++drilled; }
        }
    }

    // pitch: mode of nearest-neighbour distances (0.01 mm bins), ties -> smallest
    double pitch = 0;
    if (n >= 2) {
        // nearest-neighbour distances, 0.02 mm bins; pitch = mean of the modal bin +-1
        std::vector<double> nnd(n);
        std::map<long long, int> hist;
        for (int a = 0; a < n; ++a) {
            double best = 1e300;
            for (int b = 0; b < n; ++b) if (a != b) best = std::min(best, dist(pads[a].pos, pads[b].pos));
            nnd[a] = best;
            hist[std::llround(best / 0.02)]++;
        }
        int bc = -1;
        long long mode = 0;
        for (auto& [k, c] : hist) {
            int c3 = c + (hist.count(k - 1) ? hist[k - 1] : 0) + (hist.count(k + 1) ? hist[k + 1] : 0);
            if (c3 > bc) { bc = c3; mode = k; }
        }
        double sum = 0;
        int cnt = 0;
        for (double v : nnd) if (std::llabs(std::llround(v / 0.02) - mode) <= 1) { sum += v; ++cnt; }
        pitch = cnt ? sum / cnt : mode * 0.02;
    }
    out.pitch = pitch;

    // layout analysis
    const double lt = std::max(0.08, pitch * 0.2);
    std::vector<double> xs(n), ys(n);
    for (int k = 0; k < n; ++k) { xs[k] = pads[k].pos.x; ys[k] = pads[k].pos.y; }
    std::vector<int> mx_, my_;
    auto xl = lines_of(xs, lt, &mx_), yl = lines_of(ys, lt, &my_);
    auto count_in = [&](const std::vector<int>& m, int line) { return (int)std::count(m.begin(), m.end(), line); };
    std::string layout = "other";
    if (n == 1) layout = "single";
    else if (n == 2) layout = "two";
    else if (yl.size() == 1) layout = "row-x";
    else if (xl.size() == 1) layout = "row-y";
    else if (n >= 4 && yl.size() == 2 && count_in(my_, 0) == n / 2 && n % 2 == 0) layout = "dual-x";
    else if (n >= 4 && xl.size() == 2 && count_in(mx_, 0) == n / 2 && n % 2 == 0) layout = "dual-y";
    else {
        BBox cb;
        for (auto& p : pads) cb.add(p.pos);
        bool perim = true;
        int sides[4] = {0, 0, 0, 0};
        for (auto& p : pads) {
            bool l = std::fabs(p.pos.x - cb.x0) < lt, r = std::fabs(p.pos.x - cb.x1) < lt;
            bool b = std::fabs(p.pos.y - cb.y0) < lt, t = std::fabs(p.pos.y - cb.y1) < lt;
            if (!(l || r || b || t)) { perim = false; break; }
            sides[0] += l; sides[1] += r; sides[2] += b; sides[3] += t;
        }
        if (perim && n >= 8 && sides[0] >= 2 && sides[1] >= 2 && sides[2] >= 2 && sides[3] >= 2) layout = "quad";
        else if (xl.size() >= 3 && yl.size() >= 3 && n >= 0.5 * xl.size() * yl.size()) layout = "grid";
    }
    out.layout = layout;

    // pin one
    int pin1 = -1;
    {
        std::map<ShapeClass, std::vector<int>> by;
        for (int k = 0; k < n; ++k) by[class_of(pads[k].shape)].push_back(k);
        if (n >= 2 && by.size() == 2) {
            for (auto& [c, v] : by)
                if (v.size() == 1) { pin1 = v[0]; out.pin1_from_shape = true; }
            if (by.begin()->second.size() == 1 && std::next(by.begin())->second.size() == 1) {
                // two pads with different shapes: prefer the rect/square one
                pin1 = class_of(pads[by.begin()->second[0]].shape) == ShapeClass::Rect ? by.begin()->second[0]
                                                                                       : std::next(by.begin())->second[0];
            }
        }
    }
    auto pick = [&](auto better) {
        int b = 0;
        for (int k = 1; k < n; ++k) if (better(pads[k].pos, pads[b].pos)) b = k;
        return b;
    };
    const double e = lt;
    if (pin1 < 0) {
        if (layout == "dual-x" || layout == "row-x" || layout == "single")
            pin1 = pick([&](Vec2 a, Vec2 b) { return a.y < b.y - e || (std::fabs(a.y - b.y) <= e && a.x < b.x - e); });  // bottom-left
        else if (layout == "two") {
            Vec2 d = pads[1].pos - pads[0].pos;
            pin1 = std::fabs(d.x) >= std::fabs(d.y) ? (pads[0].pos.x < pads[1].pos.x ? 0 : 1) : (pads[0].pos.y > pads[1].pos.y ? 0 : 1);
        } else
            pin1 = pick([&](Vec2 a, Vec2 b) { return a.x < b.x - e || (std::fabs(a.x - b.x) <= e && a.y > b.y + e); });  // top-left
        out.notes.push_back("pin 1 guessed by position (" + layout + ")");
    } else {
        out.notes.push_back("pin 1 = unique-shape pad");
    }

    // numbering
    std::vector<int> order(n);
    std::iota(order.begin(), order.end(), 0);
    if (layout == "grid") {
        std::vector<int> mrow;
        lines_of(ys, lt, &mrow);
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            if (mrow[a] != mrow[b]) return mrow[a] > mrow[b];
            return pads[a].pos.x < pads[b].pos.x;
        });
        auto it = std::find(order.begin(), order.end(), pin1);
        std::rotate(order.begin(), it, order.end());
    } else {
        double a1 = std::atan2(pads[pin1].pos.y, pads[pin1].pos.x);
        auto rel = [&](int k) {
            if (k == pin1) return -1.0;
            double a = std::atan2(pads[k].pos.y, pads[k].pos.x) - a1;
            while (a < 0) a += 2 * kPi;
            while (a >= 2 * kPi) a -= 2 * kPi;
            return a;
        };
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            double ra = rel(a), rb = rel(b);
            if (std::fabs(ra - rb) > 1e-9) return ra < rb;
            return pads[a].pos.len() < pads[b].pos.len();
        });
    }
    out.pkg.pads.clear();
    for (int k = 0; k < n; ++k) {
        Pad p = pads[order[k]];
        p.pin = k + 1;
        out.pkg.pads.push_back(p);
        out.pad_indices.push_back(cand[order[k]]);
    }
    out.pin1 = 0;
    out.leads = n;

    // SMT / TH
    if (opt.drill) out.smt = drilled * 2 < n;
    else {
        bool any_hole = false, all_flash = true, any_round = false, iso = true;
        for (int k = 0; k < n; ++k) {
            const PadFeature& f = ps.pads[cand[k]];
            any_hole |= f.hole > 0;
            all_flash &= f.flash && !f.raster;
            any_round |= f.cls == ShapeClass::Round;
            iso &= std::fabs(f.w - f.h) <= 0.1 * std::max(f.w, f.h);
        }
        out.smt = !(any_hole || (all_flash && any_round && iso && n >= 2));
        out.notes.push_back(std::string("SMT/TH guessed without drill layer: ") + (out.smt ? "SMT" : "through-hole"));
    }

    // body estimate
    {
        const auto& P = out.pkg.pads;
        BBox outer, cen;
        double maxw = 0, maxh = 0;
        for (auto& p : P) {
            outer.add(p.pos - Vec2{p.w / 2, p.h / 2}); outer.add(p.pos + Vec2{p.w / 2, p.h / 2});
            cen.add(p.pos);
            maxw = std::max(maxw, p.w); maxh = std::max(maxh, p.h);
        }
        double bw = outer.w(), bh = outer.h();
        if (layout == "dual-x") { bw = outer.w(); bh = std::max(cen.h() - maxh, 0.4 * cen.h()); }
        else if (layout == "dual-y") { bh = outer.h(); bw = std::max(cen.w() - maxw, 0.4 * cen.w()); }
        else if (layout == "quad") { bw = std::max(cen.w() - maxw, 0.5 * cen.w()); bh = std::max(cen.h() - maxh, 0.5 * cen.h()); }
        else if (layout == "two") {
            Vec2 d = P[1].pos - P[0].pos;
            if (std::fabs(d.x) >= std::fabs(d.y)) { bw = cen.w(); bh = maxh; } else { bh = cen.h(); bw = maxw; }
        }
        out.body_w = bw; out.body_h = bh;
    }

    out.signature = pattern_signature(out.pkg.pads);
    out.name = auto_package_name(out);
    out.pkg.name = out.name;
    out.pkg.smt = out.smt;
    out.pkg.leads = n;
    out.pkg.pin_marked = out.pin1_from_shape;
    out.pkg.polarized = out.pin1_from_shape;
    out.pkg.description = std::to_string(n) + " pads, " + layout + ", pitch " + fmt_mm(pitch) + " mm" +
                          (out.smt ? ", SMT" : ", TH");
    // artwork body rectangle (0 deg, about centroid)
    GPrim body;
    body.kind = GKind::Rect;
    body.pts = {{-out.body_w / 2, -out.body_h / 2}, {out.body_w / 2, out.body_h / 2}};
    out.pkg.body = {body};
    return true;
}

// ---------------------------------------------------------------------------
// rotation
// ---------------------------------------------------------------------------
namespace {
struct AxisVotes {
    double sx = 0, sy = 0, wsum = 0;
    int n = 0;
    void add(double ang_deg, double w = 1) {   // modulo 90
        const double r = deg2rad(ang_deg * 4);
        sx += w * std::cos(r); sy += w * std::sin(r); wsum += w; ++n;
    }
    double mean() const {                      // (-45, 45]
        double a = rad2deg(std::atan2(sy, sx)) / 4;
        if (a <= -45 + 1e-9) a += 90;
        return a;
    }
    double resultant() const { return wsum > 0 ? std::hypot(sx, sy) / wsum : 0; }
    double spread() const {                    // circular std-dev in deg (mod 90 space)
        double R = std::clamp(resultant(), 1e-12, 1.0);
        return rad2deg(std::sqrt(-2 * std::log(R))) / 4;
    }
};
double snap01(double a) {
    a = std::round(a * 10) / 10;
    if (std::fabs(a) < 0.05) a = 0;
    if (a <= -45) a += 90;
    return a;
}
} // namespace

RotationGuess estimate_rotation(const PadSet& ps, const std::vector<int>& idx) {
    RotationGuess g;
    AxisVotes pv, rv;
    for (int i : idx) {
        if (i < 0 || i >= (int)ps.pads.size()) continue;
        const PadFeature& f = ps.pads[i];
        if (!f.oriented || f.cls == ShapeClass::Round) continue;
        const bool elong = f.ow > 1.05 * f.oh;
        pv.add(f.oang, elong ? 1.0 : 0.5);
    }
    // row directions from the non-round pads (vias / round test points inside the box add noise)
    std::vector<int> rowset;
    for (int i : idx) if (i >= 0 && i < (int)ps.pads.size() && ps.pads[i].cls != ShapeClass::Round) rowset.push_back(i);
    if (rowset.size() < 2) rowset = idx;
    const std::vector<int>& idx_r = rowset;
    const int n = (int)idx_r.size();
    if (n >= 2) {
        for (int a = 0; a < n; ++a) {
            const Vec2 ca = ps.pads[idx_r[a]].c;
            double best = 1e300;
            for (int b = 0; b < n; ++b) if (a != b) best = std::min(best, dist(ca, ps.pads[idx_r[b]].c));
            if (best <= 1e-9 || best >= 1e299) continue;
            for (int b = 0; b < n; ++b) {
                if (a == b) continue;
                Vec2 d = ps.pads[idx_r[b]].c - ca;
                if (d.len() > best * 1.05 + 1e-6) continue;
                rv.add(rad2deg(std::atan2(d.y, d.x)));
            }
        }
    }
    g.pad_votes = pv.n; g.row_votes = rv.n;
    if (pv.n) { g.pad_angle = snap01(pv.mean()); g.pad_spread = pv.spread(); }
    if (rv.n) { g.row_angle = snap01(rv.mean()); g.row_spread = rv.spread(); }
    char b[160];
    if (pv.n && rv.n) {
        const double dd = std::fabs(axis_diff(pv.mean(), rv.mean(), 90));
        if (dd <= 2.0 && pv.resultant() > 0.995) {
            // consistent pad shapes are exact geometry: they decide, rows only confirm
            g.angle = g.pad_angle;
            g.confidence = std::clamp(0.5 * (pv.resultant() + rv.resultant()), 0.0, 1.0);
            std::snprintf(b, sizeof b, "pads %.1f\xC2\xB0 and rows %.1f\xC2\xB0 agree", g.pad_angle, g.row_angle);
        } else if (dd <= 2.0) {
            AxisVotes all = pv;
            all.sx = pv.sx / pv.wsum + rv.sx / rv.wsum; all.sy = pv.sy / pv.wsum + rv.sy / rv.wsum; all.wsum = 2;
            g.angle = snap01(all.mean());
            g.confidence = std::clamp(0.5 * (pv.resultant() + rv.resultant()), 0.0, 1.0);
            std::snprintf(b, sizeof b, "pads %.1f\xC2\xB0 and rows %.1f\xC2\xB0 agree", g.pad_angle, g.row_angle);
        } else if (pv.resultant() >= rv.resultant()) {
            g.angle = g.pad_angle;
            g.confidence = 0.6 * pv.resultant();
            std::snprintf(b, sizeof b, "from pad shapes (%.1f\xC2\xB0); rows disagree (%.1f\xC2\xB0)", g.pad_angle, g.row_angle);
        } else {
            g.angle = g.row_angle;
            g.confidence = 0.6 * rv.resultant();
            std::snprintf(b, sizeof b, "from pad rows (%.1f\xC2\xB0); pad shapes disagree (%.1f\xC2\xB0)", g.row_angle, g.pad_angle);
        }
    } else if (pv.n) {
        g.angle = g.pad_angle; g.confidence = 0.8 * pv.resultant();
        std::snprintf(b, sizeof b, "from pad shapes (%.1f\xC2\xB0)", g.pad_angle);
    } else if (rv.n) {
        g.angle = g.row_angle; g.confidence = (n >= 3 ? 0.8 : 0.5) * rv.resultant();
        std::snprintf(b, sizeof b, "from pad rows (%.1f\xC2\xB0)", g.row_angle);
    } else {
        g.angle = 0; g.confidence = 0;
        std::snprintf(b, sizeof b, "no orientation cue (round pads): 0\xC2\xB0");
    }
    g.why = b;
    std::vector<double> opts{g.angle, 0, 45, -45, 90, 180, -90};
    double s15 = std::round(g.angle / 15) * 15;
    opts.push_back(s15);
    for (double o : opts) {
        bool dup = false;
        for (double q : g.options) dup |= std::fabs(q - o) < 0.05;
        if (!dup) g.options.push_back(o);
    }
    return g;
}

bool digitize_rotated(const PadSet& ps, const std::vector<int>& idx0, double angle, const DigitizeOptions& opt, DigitizeResult& out) {
    out = DigitizeResult{};
    std::vector<int> idx;
    for (int i : idx0) if (i >= 0 && i < (int)ps.pads.size()) idx.push_back(i);
    if (idx.empty()) return false;
    Vec2 G{0, 0};
    for (int i : idx) G += ps.pads[i].c;
    G = G * (1.0 / idx.size());
    PadSet sub;
    sub.cell = 1e-4;
    std::vector<double> prot(idx.size(), 0.0);
    BBox all;
    for (size_t k = 0; k < idx.size(); ++k) {
        PadFeature f = ps.pads[idx[k]];
        f.c = G + rotate(f.c - G, -angle);
        double w = f.w, h = f.h;
        if (f.cls == ShapeClass::Round && !f.oriented) { w = h = std::max(f.w, f.h); }
        else if (f.oriented) {
            const double rel = norm180(f.oang - angle);       // pad long axis in the canonical frame
            const double d90 = axis_diff(rel, 0, 90);
            if (std::fabs(d90) <= 0.5) {
                const bool along_x = std::fabs(axis_diff(rel, 0, 180)) <= 45;
                w = along_x ? f.ow : f.oh; h = along_x ? f.oh : f.ow;
            } else {
                w = f.ow; h = f.oh;
                prot[k] = rel > 90 ? rel - 180 : rel;
            }
        } else {
            // axis-aligned box only: its rotated extent
            const double c = std::fabs(std::cos(deg2rad(angle))), s2 = std::fabs(std::sin(deg2rad(angle)));
            w = f.w * c + f.h * s2; h = f.w * s2 + f.h * c;
        }
        f.w = w; f.h = h;
        f.box = BBox(f.c.x - w / 2, f.c.y - h / 2, f.c.x + w / 2, f.c.y + h / 2);
        all.add(f.box);
        sub.pads.push_back(std::move(f));
    }
    for (int i = 0; i < (int)sub.pads.size(); ++i) {
        Vec2 c = sub.pads[i].c;
        sub.grid[gkey((long long)std::floor(c.x / sub.cell), (long long)std::floor(c.y / sub.cell))].push_back(i);
    }
    all.inflate(0.01);
    DigitizeOptions o = opt;
    o.hidden_dcodes.clear();
    o.drill = nullptr;   // drill positions are board coordinates; matched below
    if (!digitize_rect(sub, all, o, out)) return false;
    for (size_t k = 0; k < out.pad_indices.size(); ++k) {
        const int si = out.pad_indices[k];
        if (prot[si] != 0) out.pkg.pads[k].rot = prot[si];
        out.pad_indices[k] = idx[si];
    }
    for (int& k : out.dropped) k = idx[k];
    if (opt.drill) {
        int drilled = 0;
        for (size_t k = 0; k < out.pad_indices.size(); ++k) {
            const PadFeature& f = ps.pads[out.pad_indices[k]];
            double best = 0;
            for (auto& d : opt.drill->objs) {
                if (d.kind != Object::Flash || dist(d.a, f.c) >= std::min(f.w, f.h) / 2) continue;
                const Aperture* a = gerber::find_ap(*opt.drill, d.dcode);
                best = std::max(best, a ? a->w : 0.01);
            }
            if (best > 0) { out.pkg.pads[k].drill = best; ++drilled; }
        }
        out.smt = drilled * 2 < (int)out.pad_indices.size();
        out.pkg.smt = out.smt;
        out.name = auto_package_name(out);
        out.pkg.name = out.name;
    }
    out.centroid = G + rotate(out.centroid - G, angle);
    out.rot = angle;
    out.signature = pattern_signature(out.pkg.pads);
    char b[64];
    std::snprintf(b, sizeof b, "rotated %.1f\xC2\xB0: package built in canonical orientation", angle);
    out.notes.push_back(b);
    return true;
}

std::string rot_invariant_signature(const std::vector<Pad>& pads, double q) {
    auto Q = [&](double v) { return (long long)std::llround(v / q); };
    std::vector<long long> d;
    for (size_t a = 0; a < pads.size(); ++a)
        for (size_t b = a + 1; b < pads.size(); ++b) d.push_back(Q(dist(pads[a].pos, pads[b].pos)));
    std::sort(d.begin(), d.end());
    std::vector<std::pair<long long, long long>> dims;
    for (auto& p : pads) dims.push_back({Q(std::max(p.w, p.h)), Q(std::min(p.w, p.h))});
    std::sort(dims.begin(), dims.end());
    unsigned long long h = 1469598103934665603ULL;
    auto mix = [&](long long v) { for (int k = 0; k < 8; ++k) { h ^= (unsigned char)(v >> (8 * k)); h *= 1099511628211ULL; } };
    for (auto v : d) mix(v);
    for (auto& [w, hh] : dims) { mix(w); mix(hh); }
    char out[64];
    std::snprintf(out, sizeof out, "R%zu-%016llx", pads.size(), h);
    return out;
}

namespace {
// size / orientation test of feature f against a pattern pad (pw x ph, pad rot prot) placed at angle th
bool pad_fits(const PadFeature& f, const Pad& p, double th, const ScanOptions& opt) {
    if (f.cls != class_of(p.shape)) return false;
    const double pl = std::max(p.w, p.h), psh = std::min(p.w, p.h);
    const double tl = std::max(opt.size_tol_abs, opt.size_tol_rel * pl), ts = std::max(opt.size_tol_abs, opt.size_tol_rel * psh);
    if (f.cls == ShapeClass::Round && !f.oriented) return std::fabs(std::max(f.w, f.h) - pl) <= tl;
    const double e = norm180(th + p.rot + (p.w >= p.h ? 0 : 90));   // expected long-axis direction
    if (f.oriented) {
        if (std::fabs(f.ow - pl) > tl || std::fabs(f.oh - psh) > ts) return false;
        const bool square = f.ow - f.oh < 0.05 * f.ow;
        return std::fabs(axis_diff(f.oang, e, square ? 90 : 180)) <= opt.angle_tol;
    }
    // only an axis-aligned box: compare its extents with the rotated pad's extents
    if (std::fabs(axis_diff(e, 0, 90)) > opt.angle_tol) return false;
    const bool along_x = std::fabs(axis_diff(e, 0, 180)) <= 45;
    const double ew = along_x ? pl : psh, eh = along_x ? psh : pl;
    return std::fabs(f.w - ew) <= (along_x ? tl : ts) && std::fabs(f.h - eh) <= (along_x ? ts : tl);
}
bool dims_fit(const PadFeature& f, const Pad& p, const ScanOptions& opt) {   // orientation-free prefilter
    if (f.cls != class_of(p.shape)) return false;
    const double pl = std::max(p.w, p.h), psh = std::min(p.w, p.h);
    const double tl = std::max(opt.size_tol_abs, opt.size_tol_rel * pl), ts = std::max(opt.size_tol_abs, opt.size_tol_rel * psh);
    const double fl = f.oriented ? f.ow : std::max(f.w, f.h), fs = f.oriented ? f.oh : std::min(f.w, f.h);
    return std::fabs(fl - pl) <= tl && std::fabs(fs - psh) <= ts;
}
} // namespace

static void any_angle_pass(const PadSet& ps, const DigitizeResult& pat, const ScanOptions& opt, std::vector<char>& used,
                           const std::vector<char>& source, std::vector<Match>& out) {
    const auto& P = pat.pkg.pads;
    const int n = (int)P.size();
    if (n < 2) return;
    // anchor = rarest dims; partner = the pad farthest from it (best angle resolution)
    int a = 0;
    {
        int best = 1 << 30;
        for (int i = 0; i < n; ++i) {
            int c = 0;
            for (int j = 0; j < n; ++j)
                if (class_of(P[j].shape) == class_of(P[i].shape) && std::fabs(std::max(P[j].w, P[j].h) - std::max(P[i].w, P[i].h)) < 0.05 &&
                    std::fabs(std::min(P[j].w, P[j].h) - std::min(P[i].w, P[i].h)) < 0.05) ++c;
            if (c < best) { best = c; a = i; }
        }
    }
    int b = -1;
    double D = 0;
    for (int j = 0; j < n; ++j) if (j != a && dist(P[j].pos, P[a].pos) > D) { D = dist(P[j].pos, P[a].pos); b = j; }
    if (b < 0 || D < 1e-6) return;
    const double base = std::atan2(P[b].pos.y - P[a].pos.y, P[b].pos.x - P[a].pos.x);
    const double vmax = opt.isolation_factor * std::max(pat.pitch, 0.2);
    std::vector<Vec2> disp;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            if (i == j) continue;
            Vec2 v = P[i].pos - P[j].pos;
            if (v.len() > vmax) continue;
            bool dup = false;
            for (auto& w : disp) dup |= dist(w, v) < opt.pos_tol;
            if (!dup) disp.push_back(v);
        }
    BBox pbox;
    for (auto& p : P) pbox.add(p.pos);

    auto verify = [&](Vec2 c, double th, std::vector<int>& got, double& rms) {
        got.assign(n, -1);
        std::set<int> taken;
        double se = 0;
        for (int i = 0; i < n; ++i) {
            Vec2 want = c + rotate(P[i].pos, th);
            BBox qb;
            qb.add(want);
            qb.inflate(opt.pos_tol);
            int best = -1;
            double bd = 1e300;
            for (int j : ps.query(qb)) {
                if (used[j] || taken.count(j) || (opt.skip_source && source[j])) continue;
                double d = dist(ps.pads[j].c, want);
                if (d > opt.pos_tol || !pad_fits(ps.pads[j], P[i], th, opt)) continue;
                if (d < bd) { bd = d; best = j; }
            }
            if (best < 0) return false;
            got[i] = best;
            taken.insert(best);
            se += bd * bd;
        }
        rms = std::sqrt(se / n);
        if (opt.isolation) {
            BBox gb;
            for (int j : got) gb.add(ps.pads[j].c);
            gb.inflate(vmax + opt.pos_tol);
            BBox inner = pbox;
            inner.inflate(-opt.pos_tol);
            for (int j : ps.query(gb)) {
                if (taken.count(j)) continue;
                const PadFeature& f = ps.pads[j];
                bool similar = false;
                for (const Pad& p : P) if (dims_fit(f, p, opt)) { similar = true; break; }
                if (!similar) continue;
                Vec2 l = rotate(f.c - c, -th);
                if (!inner.empty() && inner.contains(l)) return false;
                for (int k = 0; k < n; ++k)
                    for (const Vec2& v : disp)
                        if (dist(l, P[k].pos + v) <= opt.pos_tol) return false;
            }
        }
        return true;
    };

    for (int fi = 0; fi < (int)ps.pads.size(); ++fi) {
        if (used[fi] || (opt.skip_source && source[fi])) continue;
        const PadFeature& f = ps.pads[fi];
        if (!opt.limit.empty() && !opt.limit.contains(f.c)) continue;
        if (!dims_fit(f, P[a], opt)) continue;
        BBox ring(f.c.x - D - opt.pos_tol, f.c.y - D - opt.pos_tol, f.c.x + D + opt.pos_tol, f.c.y + D + opt.pos_tol);
        bool done = false;
        for (int gi : ps.query(ring)) {
            if (gi == fi || used[gi] || (opt.skip_source && source[gi])) continue;
            const PadFeature& g = ps.pads[gi];
            const double dd = dist(g.c, f.c);
            if (std::fabs(dd - D) > 2 * opt.pos_tol || !dims_fit(g, P[b], opt)) continue;
            double th = rad2deg(std::atan2(g.c.y - f.c.y, g.c.x - f.c.x) - base);
            th = std::remainder(th, 360.0);
            if (!pad_fits(f, P[a], th, opt) || !pad_fits(g, P[b], th, opt)) continue;
            // axis-aligned placements belong to the regular pass
            if (opt.rot90 && std::fabs(axis_diff(th, 0, 90)) < 0.25) continue;
            Vec2 c = f.c - rotate(P[a].pos, th);
            std::vector<int> got;
            double rms;
            if (!verify(c, th, got, rms)) continue;
            // least-squares refinement (2D Procrustes) of angle and centre
            Vec2 mp{0, 0}, mq{0, 0};
            for (int i = 0; i < n; ++i) { mp += P[i].pos; mq += ps.pads[got[i]].c; }
            mp = mp * (1.0 / n); mq = mq * (1.0 / n);
            double sxx = 0, sxy = 0;
            for (int i = 0; i < n; ++i) {
                Vec2 p = P[i].pos - mp, q = ps.pads[got[i]].c - mq;
                sxx += p.x * q.x + p.y * q.y;
                sxy += p.x * q.y - p.y * q.x;
            }
            double th2 = rad2deg(std::atan2(sxy, sxx));
            Vec2 c2 = mq - rotate(mp, th2);
            std::vector<int> got2;
            double rms2;
            if (verify(c2, th2, got2, rms2) && rms2 <= rms + 1e-9) { th = th2; c = c2; got = got2; rms = rms2; }
            Match m;
            m.centroid = c;
            m.rot = std::remainder(th, 360.0);
            if (std::fabs(m.rot) < 1e-9) m.rot = 0;
            m.pads = got;
            m.rms = rms;
            m.rotated = true;
            m.confidence = std::clamp(1.0 - rms / std::max(opt.pos_tol, 1e-9), 0.0, 1.0);
            for (int j : got) used[j] = 1;
            out.push_back(std::move(m));
            done = true;
            break;
        }
        (void)done;
    }
}

// ---------------------------------------------------------------------------
// auto scan
// ---------------------------------------------------------------------------
std::vector<Match> auto_scan(const Layer& L, const DigitizeResult& pattern, const ScanOptions& opt,
                             const DigitizeOptions& dopt) {
    PadSet ps = extract_pads(L, dopt);
    return auto_scan(ps, pattern, opt);
}

std::vector<Match> auto_scan(const PadSet& ps, const DigitizeResult& pat, const ScanOptions& opt) {
    std::vector<Match> out;
    const auto& P = pat.pkg.pads;
    const int n = (int)P.size();
    if (n == 0) return out;
    std::vector<char> used(ps.pads.size(), 0);
    for (int i : opt.exclude) if (i >= 0 && i < (int)used.size()) used[i] = 1;
    std::vector<char> source(ps.pads.size(), 0);
    for (int i : pat.pad_indices) if (i >= 0 && i < (int)source.size()) source[i] = 1;

    auto size_ok = [&](const PadFeature& f, double w, double h, ShapeClass c) {
        if (f.cls != c) return false;
        double tw = std::max(opt.size_tol_abs, opt.size_tol_rel * w), th = std::max(opt.size_tol_abs, opt.size_tol_rel * h);
        return std::fabs(f.w - w) <= tw && std::fabs(f.h - h) <= th;
    };
    struct RP { Vec2 pos; double w, h; ShapeClass c; };
    std::vector<std::vector<RP>> rots;
    std::vector<double> angles;
    for (int k = 0; k < (opt.rot90 ? 4 : 1); ++k) {
        std::vector<RP> v;
        for (auto& p : P) v.push_back({rotate(p.pos, 90.0 * k), (k & 1) ? p.h : p.w, (k & 1) ? p.w : p.h, class_of(p.shape)});
        // snap rotation noise
        for (auto& r : v) { r.pos.x = std::round(r.pos.x * 1e9) / 1e9; r.pos.y = std::round(r.pos.y * 1e9) / 1e9; }
        rots.push_back(std::move(v));
        angles.push_back(90.0 * k);
    }
    // anchor = pad whose (class,size) is rarest within the pattern
    int anchor = 0;
    {
        int best = 1 << 30;
        for (int a = 0; a < n; ++a) {
            int c = 0;
            for (int b = 0; b < n; ++b)
                if (class_of(P[b].shape) == class_of(P[a].shape) && std::fabs(P[b].w - P[a].w) < 0.05 && std::fabs(P[b].h - P[a].h) < 0.05) ++c;
            if (c < best) { best = c; anchor = a; }
        }
    }
    // short pattern displacement vectors per rotation (isolation / extension test)
    const double vmax = opt.isolation_factor * std::max(pat.pitch, 0.2);
    std::vector<std::vector<Vec2>> disp(rots.size());
    for (size_t r = 0; r < rots.size(); ++r)
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) {
                if (i == j) continue;
                Vec2 v = rots[r][i].pos - rots[r][j].pos;
                if (v.len() > vmax) continue;
                bool dup = false;
                for (auto& w : disp[r]) if (dist(w, v) < opt.pos_tol) { dup = true; break; }
                if (!dup) disp[r].push_back(v);
            }

    auto verify = [&](Vec2 c, int r, std::vector<int>& got, double& rms) {
        got.assign(n, -1);
        double se = 0;
        std::set<int> taken;
        for (int i = 0; i < n; ++i) {
            const RP& rp = rots[r][i];
            Vec2 want = c + rp.pos;
            BBox qb;
            qb.add(want);
            qb.inflate(opt.pos_tol);
            int best = -1;
            double bd = 1e300;
            for (int j : ps.query(qb)) {
                if (used[j] || taken.count(j)) continue;
                if (opt.skip_source && source[j]) continue;
                const PadFeature& f = ps.pads[j];
                double d = dist(f.c, want);
                if (d > opt.pos_tol || !size_ok(f, rp.w, rp.h, rp.c)) continue;
                if (d < bd) { bd = d; best = j; }
            }
            if (best < 0) return false;
            got[i] = best;
            taken.insert(best);
            se += bd * bd;
        }
        rms = std::sqrt(se / n);
        if (opt.isolation) {
            BBox gb;
            for (int j : got) gb.add(ps.pads[j].c);
            BBox qb = gb;
            qb.inflate(vmax + opt.pos_tol);
            for (int j : ps.query(qb)) {
                if (taken.count(j)) continue;
                const PadFeature& f = ps.pads[j];
                bool similar = false;
                for (const RP& rp : rots[r]) if (size_ok(f, rp.w, rp.h, rp.c)) { similar = true; break; }
                if (!similar) continue;
                BBox inner = gb;
                inner.inflate(-opt.pos_tol);
                if (!inner.empty() && inner.contains(f.c)) return false;     // foreign pad inside the group
                for (int k : got)
                    for (const Vec2& v : disp[r])
                        if (dist(f.c, ps.pads[k].c + v) <= opt.pos_tol) return false;   // grouping continues
            }
        }
        return true;
    };

    for (int fi = 0; fi < (int)ps.pads.size(); ++fi) {
        if (used[fi] || (opt.skip_source && source[fi])) continue;
        const PadFeature& f = ps.pads[fi];
        if (!opt.limit.empty() && !opt.limit.contains(f.c)) continue;
        for (int r = 0; r < (int)rots.size(); ++r) {
            const RP& ap = rots[r][anchor];
            if (!size_ok(f, ap.w, ap.h, ap.c)) continue;
            Vec2 c = f.c - ap.pos;
            std::vector<int> got;
            double rms;
            if (!verify(c, r, got, rms)) continue;
            // refine centroid from matched pads (mean residual)
            Vec2 off{0, 0};
            for (int i = 0; i < n; ++i) off += ps.pads[got[i]].c - (c + rots[r][i].pos);
            c += off * (1.0 / n);
            // canonical rotation for symmetric patterns: lowest angle that matches the same pads
            int rr = r;
            for (int r2 = 0; r2 < r; ++r2) {
                std::vector<int> g2;
                double rms2;
                if (verify(c, r2, g2, rms2)) {
                    std::set<int> a(got.begin(), got.end()), b(g2.begin(), g2.end());
                    if (a == b) { rr = r2; got = g2; rms = rms2; break; }
                }
            }
            Match m;
            m.centroid = c;
            m.rot = angles[rr];
            m.pads = got;
            m.rms = rms;
            for (int j : got) used[j] = 1;
            out.push_back(std::move(m));
            break;
        }
    }
    if (opt.any_angle) any_angle_pass(ps, pat, opt, used, source, out);
    return out;
}


// ===========================================================================
// orientation fit (package canonical orientation -> selected pads)
// ===========================================================================
namespace {
double n360(double a) { a = std::fmod(a, 360.0); if (a < 0) a += 360.0; if (a >= 360.0 - 1e-9) a = 0; return a; }
double cdiff(double a, double b) { return std::fabs(std::remainder(a - b, 360.0)); }
struct PadDir { double lo = 0, sh = 0, dir = 0; bool sq = true; };
PadDir pad_dir(const Pad& p) {
    PadDir d;
    d.lo = std::max(p.w, p.h); d.sh = std::min(p.w, p.h);
    d.dir = p.rot + (p.w >= p.h ? 0.0 : 90.0);
    d.sq = (d.lo - d.sh) <= std::max(0.02, 0.06 * d.lo) || p.shape == PadShape::Round;
    return d;
}
} // namespace

OrientFit fit_orientation(const std::vector<Pad>& pkg0, const std::vector<Pad>& inst, bool mirror, double tol, bool allow_any, double prefer) {
    OrientFit best;
    const size_t n = pkg0.size();
    if (n == 0 || n != inst.size()) return best;
    std::vector<Pad> P = pkg0;
    if (mirror) for (Pad& p : P) { p.pos.x = -p.pos.x; p.rot = -p.rot; }
    Vec2 cP{}, cI{};
    for (auto& p : P) cP = cP + p.pos;
    for (auto& p : inst) cI = cI + p.pos;
    cP = cP * (1.0 / (double)n);
    cI = cI * (1.0 / (double)n);
    std::vector<PadDir> dP(n), dI(n);
    for (size_t k = 0; k < n; ++k) { dP[k] = pad_dir(P[k]); dI[k] = pad_dir(inst[k]); }
    auto eval = [&](double R, OrientFit& f) {
        f = {};
        f.rot = n360(R);
        std::vector<char> used(n, 0);
        double ss = 0;
        f.match.assign(n, -1);
        for (size_t k = 0; k < n; ++k) {
            const Vec2 q = rotate(P[k].pos - cP, R);
            int bj = -1;
            double bd = 1e30;
            for (size_t j = 0; j < n; ++j) {
                if (used[j]) continue;
                const double d = dist(q, inst[j].pos - cI);
                if (d < bd) { bd = d; bj = (int)j; }
            }
            const double ptol = tol + 0.02 * std::max(dP[k].lo, 0.5);
            if (bj < 0 || bd > ptol) return false;
            const PadDir& a = dP[k];
            const PadDir& b = dI[bj];
            const double st = std::max(0.10, 0.15 * a.lo);
            if (std::fabs(a.lo - b.lo) > st || std::fabs(a.sh - b.sh) > st) return false;
            if (!a.sq && !b.sq && std::fabs(std::remainder(a.dir + R - b.dir, 180.0)) > 8.0) return false;
            used[bj] = 1;
            f.match[k] = bj;
            ss += bd * bd;
        }
        f.rms = std::sqrt(ss / (double)n);
        f.offset = cI - rotate(cP, R);
        f.ok = true;
        return true;
    };
    auto better = [&](const OrientFit& f) {
        if (!best.ok) return true;
        if (f.rms < best.rms - 0.02) return true;
        if (f.rms > best.rms + 0.02) return false;
        return cdiff(f.rot, prefer) < cdiff(best.rot, prefer) - 1e-6;
    };
    for (double R : {0.0, 90.0, 180.0, 270.0}) {
        OrientFit f;
        if (eval(R, f) && better(f)) best = f;
    }
    if (best.ok || !allow_any) return best;
    // any angle: seed from the pad farthest from the centroid
    size_t a = 0;
    double da = -1;
    for (size_t k = 0; k < n; ++k) { double d = dist(P[k].pos, cP); if (d > da) { da = d; a = k; } }
    std::vector<double> seeds;
    if (da < 1e-6) {   // single pad / concentric: from the pad direction
        if (!dP[0].sq && !dI[0].sq) { seeds.push_back(dI[0].dir - dP[0].dir); seeds.push_back(dI[0].dir - dP[0].dir + 180); }
    } else {
        const double aP = rad2deg(std::atan2(P[a].pos.y - cP.y, P[a].pos.x - cP.x));
        for (size_t j = 0; j < n; ++j) {
            const Vec2 v = inst[j].pos - cI;
            if (std::fabs(v.len() - da) > tol * 2) continue;
            seeds.push_back(rad2deg(std::atan2(v.y, v.x)) - aP);
        }
    }
    for (double R0 : seeds) {
        OrientFit f;
        if (!eval(R0, f)) continue;
        // least-squares refinement of the angle
        double sc = 0, ss = 0;
        for (size_t k = 0; k < n; ++k) {
            const Vec2 p = P[k].pos - cP, q = inst[f.match[k]].pos - cI;
            sc += p.x * q.x + p.y * q.y;
            ss += p.x * q.y - p.y * q.x;
        }
        double R = (sc != 0 || ss != 0) ? rad2deg(std::atan2(ss, sc)) : R0;
        R = std::round(R * 10) / 10;
        OrientFit g;
        if (eval(R, g)) f = g;
        f.axis = false;
        if (better(f)) best = f;
    }
    return best;
}

bool canonicalize_orientation(DigitizeResult& r) {
    if (r.pkg.pads.size() != 2) return false;
    const Vec2 d = r.pkg.pads[1].pos - r.pkg.pads[0].pos;
    if (std::fabs(d.y) <= std::fabs(d.x) + 1e-9) return false;
    // vertical pair: rotate the package +90 (top pad -> left) and the instance by -90
    for (Pad& p : r.pkg.pads) {
        p.pos = rotate(p.pos, 90.0);
        const double rm = std::fabs(std::remainder(p.rot, 90.0));
        if (rm < 1e-6) std::swap(p.w, p.h);
        else p.rot = n360(p.rot + 90.0);
        if (std::fabs(std::remainder(p.rot, 360.0)) < 1e-9) p.rot = 0;
    }
    // pin 1 = left pad
    if (r.pkg.pads[0].pos.x > r.pkg.pads[1].pos.x) {
        std::swap(r.pkg.pads[0], r.pkg.pads[1]);
        std::swap(r.pad_indices[0], r.pad_indices[1]);
        r.pkg.pads[0].pin = 1; r.pkg.pads[1].pin = 2;
    }
    std::swap(r.body_w, r.body_h);
    r.rot = std::remainder(r.rot - 90.0, 360.0);
    if (std::fabs(r.rot) < 1e-9) r.rot = 0;
    if (r.layout == "two-y") r.layout = "two-x";
    r.signature = pattern_signature(r.pkg.pads);
    return true;
}

} // namespace pcbp::digitize
