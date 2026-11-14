// ============================================================================
// thumb_render.cpp — see thumb_render.h
// ============================================================================
#include "thumb_render.h"
#include "../../core/json.h"
#include "../../core/project_io.h"
#include <algorithm>
#include <cmath>

namespace pcbp::thumb {

namespace {

struct Canvas {
    int w, h;
    std::vector<float> r, g, b, a;   // straight colour, alpha
    Canvas(int W, int H) : w(W), h(H), r((size_t)W * H), g((size_t)W * H), b((size_t)W * H), a((size_t)W * H) {}
    void put(int x, int y, uint32_t argb) {
        const float sa = ((argb >> 24) & 255) / 255.f;
        const float sr = ((argb >> 16) & 255) / 255.f, sg = ((argb >> 8) & 255) / 255.f, sb = (argb & 255) / 255.f;
        const size_t i = (size_t)y * w + x;
        const float da = a[i], oa = sa + da * (1 - sa);
        if (oa <= 0) return;
        r[i] = (sr * sa + r[i] * da * (1 - sa)) / oa;
        g[i] = (sg * sa + g[i] * da * (1 - sa)) / oa;
        b[i] = (sb * sa + b[i] * da * (1 - sa)) / oa;
        a[i] = oa;
    }
    // even-odd scanline fill of a set of rings (pixel centres)
    void fill(const std::vector<std::vector<Vec2>>& rings, uint32_t argb) {
        double y0 = 1e300, y1 = -1e300;
        for (auto& rg : rings) for (auto& p : rg) { y0 = std::min(y0, p.y); y1 = std::max(y1, p.y); }
        if (y0 > y1) return;
        const int ya = std::max(0, (int)std::floor(y0)), yb = std::min(h - 1, (int)std::ceil(y1));
        std::vector<double> xs;
        for (int y = ya; y <= yb; ++y) {
            const double sy = y + 0.5;
            xs.clear();
            for (auto& rg : rings) {
                const size_t n = rg.size();
                for (size_t i = 0; i < n; ++i) {
                    const Vec2 p = rg[i], q = rg[(i + 1) % n];
                    if ((p.y <= sy) != (q.y <= sy)) xs.push_back(p.x + (sy - p.y) * (q.x - p.x) / (q.y - p.y));
                }
            }
            std::sort(xs.begin(), xs.end());
            for (size_t k = 0; k + 1 < xs.size(); k += 2) {
                const int xa = std::max(0, (int)std::ceil(xs[k] - 0.5)), xb = std::min(w - 1, (int)std::floor(xs[k + 1] - 0.5));
                for (int x = xa; x <= xb; ++x) put(x, y, argb);
            }
        }
    }
    void fill(const std::vector<Vec2>& ring, uint32_t argb) { fill(std::vector<std::vector<Vec2>>{ring}, argb); }
};

std::vector<Vec2> rect_ring(Vec2 c, double w, double h, const Xform& x) {
    return {x.apply({c.x - w / 2, c.y - h / 2}), x.apply({c.x + w / 2, c.y - h / 2}), x.apply({c.x + w / 2, c.y + h / 2}), x.apply({c.x - w / 2, c.y + h / 2})};
}
std::vector<Vec2> circle_ring(Vec2 c, double r, const Xform& x, int n = 16) {
    std::vector<Vec2> v;
    for (int i = 0; i < n; ++i) { double t = i * 6.283185307179586 / n; v.push_back(x.apply({c.x + r * std::cos(t), c.y + r * std::sin(t)})); }
    return v;
}
// rounded rectangle (stadium for oblong pads)
std::vector<Vec2> oblong_ring(Vec2 c, double w, double h, const Xform& x) {
    std::vector<Vec2> v;
    const double r = std::min(w, h) / 2;
    const double hx = w / 2 - r, hy = h / 2 - r;
    const Vec2 cs[4] = {{c.x + hx, c.y + hy}, {c.x - hx, c.y + hy}, {c.x - hx, c.y - hy}, {c.x + hx, c.y - hy}};
    for (int q = 0; q < 4; ++q)
        for (int i = 0; i <= 4; ++i) { double t = (q * 90 + i * 22.5) * 3.141592653589793 / 180; v.push_back(x.apply({cs[q].x + r * std::cos(t), cs[q].y + r * std::sin(t)})); }
    return v;
}

} // namespace

bool render(const Project& prj, int cx, Image& out) {
    const int nimg = std::max(1, (int)prj.images.size());
    BBox bb;
    if (prj.is_panel()) bb.add(prj.panel_frame_bbox());
    for (int i = 0; i < nimg; ++i)
        for (auto& r : prj.board_frame) for (auto p : r) bb.add(prj.image_xform(i).apply(p));
    for (auto& c : prj.comps)
        for (int i = 0; i < prj.instance_count(c); ++i) {
            const Xform x = prj.instance_xform(c, i);
            const Package* pk = prj.package_of(c);
            BBox e = pk ? pk->extent() : BBox();
            if (e.empty()) e = BBox(-0.5, -0.5, 0.5, 0.5);
            for (Vec2 q : {Vec2{e.x0, e.y0}, Vec2{e.x1, e.y0}, Vec2{e.x1, e.y1}, Vec2{e.x0, e.y1}}) bb.add(x.apply(q));
        }
    if (bb.empty()) return false;
    if (bb.w() <= 0 || bb.h() <= 0) bb.inflate(1.0);
    cx = std::clamp(cx, 16, 2048);
    const double mw = bb.w() * 1.06, mh = bb.h() * 1.06;
    int W, H;
    if (mw >= mh) { W = cx; H = std::max(8, (int)std::lround(cx * mh / mw)); } else { H = cx; W = std::max(8, (int)std::lround(cx * mw / mh)); }
    const int SS = cx >= 512 ? 2 : 3;
    Canvas cv(W * SS, H * SS);
    const double s = std::min(cv.w / mw, cv.h / mh);
    const double ox = cv.w / 2.0 - (bb.x0 + bb.x1) / 2 * s, oy = cv.h / 2.0 + (bb.y0 + bb.y1) / 2 * s;
    Xform view; view.a = s; view.d = -s; view.tx = ox; view.ty = oy;   // y up -> screen down
    const double rim = std::max(1.0, cv.w / 160.0);

    auto board = [&](const std::vector<std::vector<Vec2>>& frame, const Xform& x, uint32_t edge, uint32_t fillc) {
        std::vector<std::vector<Vec2>> rings;
        for (auto& r : frame) { std::vector<Vec2> v; for (auto p : r) v.push_back((view * x).apply(p)); if (v.size() >= 3) rings.push_back(v); }
        if (rings.empty()) return;
        cv.fill(rings, edge);
        for (auto& r : rings) {   // shrink towards the centroid by `rim` px: dark edge
            Vec2 c{0, 0}; for (auto p : r) { c.x += p.x; c.y += p.y; } c.x /= r.size(); c.y /= r.size();
            for (auto& p : r) { double dx = p.x - c.x, dy = p.y - c.y, d = std::hypot(dx, dy); if (d > rim) { p.x -= dx / d * rim; p.y -= dy / d * rim; } }
        }
        cv.fill(rings, fillc);
    };
    // panel rails, then every image's board
    if (prj.is_panel() && !prj.panel_frame.empty()) board(prj.panel_frame, Xform{}, 0xFF0D3B24, 0xFF2A6B4A);
    if (prj.has_circuit_frame()) {
        for (int i = 0; i < nimg; ++i) board(prj.board_frame, prj.image_xform(i), 0xFF0D3B24, 0xFF1F7A4D);
    } else if (!prj.is_panel() || prj.panel_frame.empty()) {
        cv.fill(rect_ring({(bb.x0 + bb.x1) / 2, (bb.y0 + bb.y1) / 2}, bb.w() * 1.03, bb.h() * 1.03, view), 0xFF1F7A4D);
    }
    // components: bottom side first (dim), then top
    for (int pass = 0; pass < 2; ++pass) {
        const Side side = pass == 0 ? Side::Bottom : Side::Top;
        const uint32_t pad_col = pass == 0 ? 0x90B89A50 : 0xFFE8B931;
        const uint32_t body_col = pass == 0 ? 0x40202020 : 0x60101010;
        for (auto& c : prj.comps) {
            if (c.side != side) continue;
            for (int inst = 0; inst < prj.instance_count(c); ++inst) {
                const Xform x = view * prj.instance_xform(c, inst);
                if (c.feature == Feature::Fiducial) { cv.fill(circle_ring({0, 0}, 0.6, x), 0xFFF0F0F0); continue; }
                const Package* pk = prj.package_of(c);
                if (!pk || pk->pads.empty()) { cv.fill(rect_ring({0, 0}, 1.0, 0.6, x), pad_col); continue; }
                BBox e = pk->extent();
                if (!e.empty()) cv.fill(rect_ring({(e.x0 + e.x1) / 2, (e.y0 + e.y1) / 2}, e.w() * 0.8, e.h() * 0.8, x), body_col);
                for (auto& p : pk->pads) {
                    const Xform px = x * Xform::translate(p.pos.x, p.pos.y) * Xform::rotation(p.rot);
                    std::vector<Vec2> ring;
                    switch (p.shape) {
                    case PadShape::Round: ring = circle_ring({0, 0}, std::max(p.w, p.h) / 2, px); break;
                    case PadShape::Oblong: ring = oblong_ring({0, 0}, p.w, p.h, px); break;
                    case PadShape::Octagon: ring = circle_ring({0, 0}, std::max(p.w, p.h) / 2, px, 8); break;
                    default: ring = rect_ring({0, 0}, p.w, p.h, px); break;
                    }
                    cv.fill(ring, pad_col);
                    if (p.drill > 0 && pass == 1) cv.fill(circle_ring({0, 0}, p.drill / 2, px, 12), 0xFF0D3B24);
                }
            }
        }
    }
    // box filter down, premultiplied BGRA
    out.w = W; out.h = H;
    out.px.assign((size_t)W * H, 0);
    const float inv = 1.f / (SS * SS);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int j = 0; j < SS; ++j)
                for (int i = 0; i < SS; ++i) {
                    const size_t k = (size_t)(y * SS + j) * cv.w + (x * SS + i);
                    r += cv.r[k] * cv.a[k]; g += cv.g[k] * cv.a[k]; b += cv.b[k] * cv.a[k]; a += cv.a[k];
                }
            auto q = [&](float v) { return (uint32_t)std::clamp((int)std::lround(v * inv * 255.f), 0, 255); };
            out.px[(size_t)y * W + x] = (q(a) << 24) | (q(r) << 16) | (q(g) << 8) | q(b);
        }
    return true;
}

bool load_bytes(const std::string& bytes, Project& prj, std::string& err,
                bool (*cpf_loader)(const std::string&, Project&, std::string&)) {
    if (bytes.size() >= 19 && (bytes.compare(4, 15, "Standard Jet DB") == 0 || bytes.compare(4, 15, "Standard ACE DB") == 0)) {
        if (!cpf_loader) { err = "legacy project support not linked"; return false; }
        return cpf_loader(bytes, prj, err);
    }
    json::Value v;
    if (!json::Value::parse(bytes, v, err)) return false;
    return project_from_json(v, prj, err);
}

} // namespace pcbp::thumb
