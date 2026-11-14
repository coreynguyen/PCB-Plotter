#pragma once
// ============================================================================
// geom.h — basic 2D geometry for PCB Plotter (portable, header-only).
// Units: millimetres (double). Angles: degrees, CCW positive.
// ============================================================================
#include <algorithm>
#include <cmath>
#include <limits>

namespace pcbp {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMmPerInch = 25.4;
constexpr double kMmPerMil = 0.0254;

inline double deg2rad(double d) { return d * kPi / 180.0; }
inline double rad2deg(double r) { return r * 180.0 / kPi; }
inline double norm_deg(double a) {
    a = std::fmod(a, 360.0);
    if (a < 0) a += 360.0;
    if (a >= 359.9999995) a = 0.0;
    return a;
}

struct Vec2 {
    double x = 0, y = 0;
    Vec2() = default;
    constexpr Vec2(double x_, double y_) : x(x_), y(y_) {}
    Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(double s) const { return {x * s, y * s}; }
    Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(Vec2 o) { x -= o.x; y -= o.y; return *this; }
    bool operator==(const Vec2& o) const { return x == o.x && y == o.y; }
    double len() const { return std::sqrt(x * x + y * y); }
};

inline double dist(Vec2 a, Vec2 b) { return (a - b).len(); }

inline Vec2 rotate(Vec2 p, double deg) {
    if (deg == 0) return p;
    const double r = deg2rad(deg), c = std::cos(r), s = std::sin(r);
    return {p.x * c - p.y * s, p.x * s + p.y * c};
}
inline Vec2 rotate_about(Vec2 p, Vec2 c, double deg) { return rotate(p - c, deg) + c; }

struct BBox {
    BBox() {}
    BBox(double ax0, double ay0, double ax1, double ay1) : x0(ax0), y0(ay0), x1(ax1), y1(ay1) {}
    double x0 = std::numeric_limits<double>::max(), y0 = std::numeric_limits<double>::max();
    double x1 = -std::numeric_limits<double>::max(), y1 = -std::numeric_limits<double>::max();
    bool empty() const { return x0 > x1 || y0 > y1; }
    void add(Vec2 p) {
        x0 = std::min(x0, p.x); y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
    }
    void add(const BBox& b) { if (!b.empty()) { add({b.x0, b.y0}); add({b.x1, b.y1}); } }
    void inflate(double d) { if (!empty()) { x0 -= d; y0 -= d; x1 += d; y1 += d; } }
    double w() const { return empty() ? 0 : x1 - x0; }
    double h() const { return empty() ? 0 : y1 - y0; }
    Vec2 center() const { return {(x0 + x1) * 0.5, (y0 + y1) * 0.5}; }
    bool contains(Vec2 p) const { return p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1; }
    bool intersects(const BBox& b) const {
        return !(b.x0 > x1 || b.x1 < x0 || b.y0 > y1 || b.y1 < y0);
    }
};

// Affine 2D transform: p' = [a c; b d] p + [tx ty]
struct Xform {
    double a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
    static Xform translate(double x, double y) { Xform t; t.tx = x; t.ty = y; return t; }
    static Xform rotation(double deg) {
        Xform t; const double r = deg2rad(deg);
        t.a = std::cos(r); t.b = std::sin(r); t.c = -t.b; t.d = t.a; return t;
    }
    static Xform scale(double sx, double sy) { Xform t; t.a = sx; t.d = sy; return t; }
    Vec2 apply(Vec2 p) const { return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty}; }
    // this * o  (apply o first, then this)
    Xform operator*(const Xform& o) const {
        Xform r;
        r.a = a * o.a + c * o.b;  r.c = a * o.c + c * o.d;
        r.b = b * o.a + d * o.b;  r.d = b * o.c + d * o.d;
        r.tx = a * o.tx + c * o.ty + tx;
        r.ty = b * o.tx + d * o.ty + ty;
        return r;
    }
    double det() const { return a * d - b * c; }
    Xform inverse() const {
        const double k = det();
        Xform r;
        if (k == 0) return r;
        r.a = d / k; r.c = -c / k; r.b = -b / k; r.d = a / k;
        r.tx = -(r.a * tx + r.c * ty);
        r.ty = -(r.b * tx + r.d * ty);
        return r;
    }
};

} // namespace pcbp
