// ============================================================================
// test_outline_bridge.cpp — R26: Add Outline from a routed edge split at breakaway tabs.
// gmatch::build_outline bridges the open rout paths end-to-nearest-end into one simple
// loop (gaps <= limit), refuses gaps that are too large and loops that cross themselves.
// Before: the open chains were committed and drawn closed, each with its own chord
// (lenses under arcs, triangles at V-notches). Synthetic geometry only.
// ============================================================================
#include "../src/io/gerber_match.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <cstdio>
#include <vector>

using namespace pcbp;
static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

using Path = std::vector<Vec2>;
static Path arc(Vec2 c, double r, double a0, double a1, int n = 48) {
    Path p;
    for (int i = 0; i <= n; ++i) { const double a = (a0 + (a1 - a0) * i / n) * M_PI / 180.0; p.push_back({c.x + r * std::cos(a), c.y + r * std::sin(a)}); }
    return p;
}
// the closed fan-shaped edge (CCW), as one polyline
static Path fan() {
    Path loop = arc({100, -40}, 140, 30, 150, 96);   // top arc: right (221.2, 30) -> left (-21.2, 30)
    // left V notch, bottom-left edge, bottom with notches, bottom-right edge, right V notch (back to the arc start)
    const Path rest = {{-5, 10}, {-30, 0}, {40, -40}, {55, -38}, {60, -50}, {75, -42}, {125, -42}, {140, -50}, {145, -38}, {160, -40},
                       {230, 0}, {205, 10}};
    loop.insert(loop.end(), rest.begin(), rest.end());
    return loop;
}
// cut the loop into open paths, removing `gap` mm of edge at each cut (breakaway tabs)
static std::vector<Path> cut(const Path& loop, std::vector<int> at, double gap) {
    std::vector<Path> out;
    const int n = (int)loop.size();
    for (size_t k = 0; k < at.size(); ++k) {
        const int s = at[k], e = at[(k + 1) % at.size()];
        Path p;
        for (int i = s;; i = (i + 1) % n) { p.push_back(loop[i]); if (i == e) break; }
        // shorten both ends by gap/2 along the path
        auto trim = [&](Path& q) {
            const Vec2 a = q[0], b = q[1];
            const double L = dist(a, b);
            if (L > gap) q[0] = {a.x + (b.x - a.x) * (gap / 2) / L, a.y + (b.y - a.y) * (gap / 2) / L};
        };
        trim(p); std::reverse(p.begin(), p.end()); trim(p); std::reverse(p.begin(), p.end());
        out.push_back(p);
    }
    return out;
}

int main() {
    const Path loop = fan();
    CHECK(gmatch::simple_polygon(loop));
    const int n = (int)loop.size();

    // ---- complete chain (no tabs): closes, nothing bridged
    {
        Path open = loop; open.push_back(loop.front());
        auto ob = gmatch::build_outline({open}, {});
        CHECK(ob.closed && ob.gaps == 0 && ob.polys.size() == 1);
    }
    // ---- four tabs of 3 mm, chains in scrambled order and directions -> one simple loop, 4 gaps
    {
        auto ch = cut(loop, {10, 60, n - 9, n - 4}, 3.0);
        std::swap(ch[0], ch[2]);
        std::reverse(ch[1].begin(), ch[1].end());
        auto ob = gmatch::build_outline(ch, {});
        std::printf("  tabs: closed=%d gaps=%d max_gap=%.2f why=%s\n", ob.closed, ob.gaps, ob.max_gap, ob.why.c_str());
        CHECK(ob.closed && ob.gaps == 4 && ob.max_gap > 2.9 && ob.max_gap < 3.1);
        CHECK(ob.polys.size() == 1 && gmatch::simple_polygon(ob.polys[0]));
        // every input vertex is on the result (no chords replaced geometry)
        size_t total = 0; for (auto& c : ch) total += c.size();
        CHECK(ob.polys[0].size() == total);
    }
    // ---- a gap larger than the limit: refused with a reason
    {
        auto ch = cut(loop, {10, 60, n - 9}, 3.0);
        auto ob = gmatch::build_outline(ch, {}, 0.02, 2.0);
        CHECK(!ob.closed && ob.why.find("gap") != std::string::npos);
    }
    // ---- one edge not picked (a whole side missing): refused
    {
        auto ch = cut(loop, {10, 60, 96, n - 4}, 3.0);
        ch.erase(ch.begin() + 1);   // the top arc piece is missing
        auto ob = gmatch::build_outline(ch, {});
        CHECK(!ob.closed && !ob.why.empty());
    }
    // ---- two separate contours picked (outer edge + an inner cut-out): no bogus merged loop
    {
        auto ch = cut(loop, {10, 60, n - 9, n - 4}, 3.0);
        Path slot = {{80, -10}, {120, -10}, {120, 5}, {80, 5}};   // open slot path inside
        ch.push_back(slot);
        auto ob = gmatch::build_outline(ch, {});
        CHECK(!ob.closed);
    }
    // ---- a closed region spanning the selection still wins (unchanged behaviour)
    {
        Path reg = {{0, 0}, {50, 0}, {50, 30}, {0, 30}};
        auto ob = gmatch::build_outline({}, {reg});
        CHECK(ob.closed && ob.gaps == 0);
    }
    // ---- simple_polygon
    CHECK(!gmatch::simple_polygon({{0, 0}, {10, 10}, {10, 0}, {0, 10}}));   // bow tie
    CHECK(gmatch::simple_polygon({{0, 0}, {10, 0}, {10, 10}, {0, 10}}));
    std::printf("test_outline_bridge: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
