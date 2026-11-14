#pragma once
// ============================================================================
// view_cache.h — pure math for the GCanvas transformed-texture view cache
// (R12 §6). Portable (no Windows deps) so tests/test_view_cache.cpp can check
// that pan / zoom sequences never show uncovered (blank) viewport area.
// A view: px-per-mm scale, world centre, pixel size. Mirror only flips x and
// is required to match, so it is ignored here.
// ============================================================================
#include <algorithm>
#include <cmath>

namespace pcbp::gui::vcache {

struct View { double scale = 1, cx = 0, cy = 0, w = 0, h = 0; };

// world rect of a view
inline void rect(const View& v, double& x0, double& y0, double& x1, double& y1) {
    x0 = v.cx - v.w * 0.5 / v.scale; x1 = v.cx + v.w * 0.5 / v.scale;
    y0 = v.cy - v.h * 0.5 / v.scale; y1 = v.cy + v.h * 0.5 / v.scale;
}

// fraction (0..1) of viewport `v` covered by the cached texture `c`
inline double coverage(const View& c, const View& v) {
    double a0, b0, a1, b1, p0, q0, p1, q1;
    rect(c, a0, b0, a1, b1); rect(v, p0, q0, p1, q1);
    const double ix = std::max(0.0, std::min(a1, p1) - std::max(a0, p0));
    const double iy = std::max(0.0, std::min(b1, q1) - std::max(b0, q0));
    const double va = (p1 - p0) * (q1 - q0);
    return va > 0 ? ix * iy / va : 1.0;
}

// the texture rendered for viewport v with `margin` (fraction of the viewport per side)
inline View expanded(const View& v, double margin) {
    View c = v;
    c.w = v.w + 2 * std::floor(v.w * margin);
    c.h = v.h + 2 * std::floor(v.h * margin);
    return c;
}

// may the cached texture `c` be shown (transformed) for viewport v?
inline bool covers(const View& c, const View& v, double zoom_out, double zoom_in) {
    if (c.scale <= 0) return false;
    const double zr = v.scale / c.scale;
    if (zr < zoom_out || zr > zoom_in) return false;
    double a0, b0, a1, b1, p0, q0, p1, q1;
    rect(c, a0, b0, a1, b1); rect(v, p0, q0, p1, q1);
    const double eps = 1e-9 / v.scale;
    return p0 >= a0 - eps && p1 <= a1 + eps && q0 >= b0 - eps && q1 <= b1 + eps;
}

} // namespace pcbp::gui::vcache
