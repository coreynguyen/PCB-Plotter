// R12 §6: GCanvas view cache never shows blank viewport area during pan / zoom sequences.
// Simulates the v11 rule (texture = viewport, transformed until zoom drifts > 2x) and the R12 rule
// (1.5x texture, re-render when the viewport leaves it or zoom drifts beyond 0.8..1.3x).
#include "../src/gui/view_cache.h"
#include <cstdio>
#include <cmath>
#include <random>
using namespace pcbp::gui::vcache;

struct Result { double min_cov = 1, blank_frames = 0, renders = 0, frames = 0; };

static Result simulate(bool r12, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> U(-1, 1);
    View v{4, 50, 40, 1280, 800}, c;
    auto render = [&]() { c = r12 ? expanded(v, 0.25) : v; };
    render();
    Result r; r.renders = 1;
    for (int f = 0; f < 20000; ++f) {
        // rapid wheel zoom at a random cursor (1.25 / notch) or a pan drag of up to 60 px
        if (rng() % 3 == 0) {
            const double fz = std::pow(1.25, rng() % 2 ? 1 : -1);
            const double sx = U(rng) * v.w * 0.5, sy = U(rng) * v.h * 0.5;
            const double bx = v.cx + sx / v.scale, by = v.cy + sy / v.scale;
            v.scale = std::clamp(v.scale * fz, 0.05, 2000.0);
            v.cx = bx - sx / v.scale; v.cy = by - sy / v.scale;
        } else { v.cx += U(rng) * 60 / v.scale; v.cy += U(rng) * 60 / v.scale; }
        bool ok;
        if (r12) ok = covers(c, v, 0.8, 1.3);
        else { const double zr = v.scale / c.scale; ok = zr >= 0.5 && zr <= 2.0; }
        if (!ok) { render(); ++r.renders; }
        const double cov = coverage(c, v);
        r.min_cov = std::min(r.min_cov, cov);
        if (cov < 0.999999) ++r.blank_frames;
        ++r.frames;
    }
    return r;
}

int main() {
    int fails = 0;
    for (unsigned seed : {1u, 7u, 42u}) {
        Result a = simulate(false, seed), b = simulate(true, seed);
        std::printf("seed %u  v11: blank frames %.0f/%.0f (min coverage %.1f%%) renders %.0f | R12: blank %.0f (min %.1f%%) renders %.0f\n",
                    seed, a.blank_frames, a.frames, a.min_cov * 100, a.renders, b.blank_frames, b.min_cov * 100, b.renders);
        if (b.blank_frames != 0 || b.min_cov < 0.999999) ++fails;
    }
    // static view: cached texture always covers
    View v{4, 0, 0, 1000, 700};
    if (!covers(expanded(v, 0.25), v, 0.8, 1.3)) ++fails;
    std::printf(fails ? "FAIL %d\n" : "view_cache: all passed\n", fails);
    return fails ? 1 : 0;
}
