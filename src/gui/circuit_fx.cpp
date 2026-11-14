// ============================================================================
// circuit_fx.cpp — procedurally routed PCB art with travelling "electricity"
// pulses (About box + dimmed landing background). Pure ImDrawList.
//
//   * three parallax layers; each layer is a cell grid with an occupancy map
//   * traces route in 45° steps (turns of ±45° only), never crossing a used cell
//   * IC footprints (QFP/SOIC) seed traces from their pins; trace ends get a
//     pad or a via; pulses glide along the polylines with a fading tail and a
//     layered thick translucent "bloom" head
// ============================================================================
#include "gapp.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace pcbp::gui {
namespace {

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) {}
    uint32_t u() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 11); }
    float f() { return (u() & 0xFFFFFF) / 16777216.f; }
    int i(int n) { return n > 0 ? (int)(u() % (uint32_t)n) : 0; }
};

struct Trace {
    std::vector<ImVec2> pts;       // in layer space (px, unscaled by parallax)
    std::vector<float> cum;        // cumulative length
    float len = 0;
    int end0 = 0, end1 = 0;        // 0 none, 1 pad, 2 via
    float w = 2;
};
struct Pulse { int trace; float s, speed, tail; float hue; };
struct Chip { ImVec2 a, b; int pins_x, pins_y; };
struct Layer {
    float depth;                   // parallax factor
    float alpha;
    std::vector<Trace> traces;
    std::vector<Chip> chips;
    std::vector<Pulse> pulses;
};

struct Scene {
    uint32_t seed = 0;
    ImVec2 size{0, 0};
    float cell = 0;
    Layer layers[3];
    double last_t = -1;
};

const int DX[8] = {1, 1, 0, -1, -1, -1, 0, 1};
const int DY[8] = {0, 1, 1, 1, 0, -1, -1, -1};

void finish(Trace& t) {
    t.cum.assign(t.pts.size(), 0.f);
    for (size_t i = 1; i < t.pts.size(); ++i) {
        ImVec2 d = t.pts[i] - t.pts[i - 1];
        t.cum[i] = t.cum[i - 1] + std::sqrt(d.x * d.x + d.y * d.y);
    }
    t.len = t.cum.empty() ? 0 : t.cum.back();
}

ImVec2 at(const Trace& t, float s) {
    if (s <= 0) return t.pts.front();
    if (s >= t.len) return t.pts.back();
    size_t lo = 1;
    while (lo < t.cum.size() - 1 && t.cum[lo] < s) ++lo;
    float a = t.cum[lo - 1], b = t.cum[lo];
    float k = b > a ? (s - a) / (b - a) : 0;
    return t.pts[lo - 1] + (t.pts[lo] - t.pts[lo - 1]) * k;
}

void build_layer(Layer& L, Rng& r, ImVec2 size, float cell, int ntraces, int nchips, float width) {
    // extend the area so parallax never reveals an edge
    const float pad = cell * 6;
    const int gw = (int)((size.x + pad * 2) / cell) + 1, gh = (int)((size.y + pad * 2) / cell) + 1;
    std::vector<uint8_t> occ((size_t)gw * gh, 0);
    auto used = [&](int x, int y) { return x < 0 || y < 0 || x >= gw || y >= gh || occ[(size_t)y * gw + x]; };
    auto mark = [&](int x, int y) { if (x >= 0 && y >= 0 && x < gw && y < gh) occ[(size_t)y * gw + x] = 1; };
    auto pos = [&](int x, int y) { return ImVec2(x * cell - pad, y * cell - pad); };

    auto route = [&](int x, int y, int dir, int maxlen, int end0) {
        Trace t;
        t.w = width;
        t.end0 = end0;
        t.pts.push_back(pos(x, y));
        mark(x, y);
        int steps = 0, since_turn = 0;
        while (steps < maxlen) {
            int nd = dir;
            if (since_turn > 2 && r.f() < 0.22f) nd = (dir + (r.f() < 0.5f ? 1 : 7)) & 7;
            int tries[3] = {nd, (dir + 1) & 7, (dir + 7) & 7};
            bool moved = false;
            for (int k = 0; k < 3 && !moved; ++k) {
                int d = tries[k];
                int nx = x + DX[d], ny = y + DY[d];
                // diagonal moves must not cut across a used corner (no crossing)
                if (used(nx, ny) || ((d & 1) && used(x + DX[d], y) && used(x, y + DY[d]))) continue;
                if (d != dir) { t.pts.push_back(pos(x, y)); since_turn = 0; }
                x = nx; y = ny; dir = d; mark(x, y);
                moved = true;
            }
            if (!moved) break;
            ++steps; ++since_turn;
        }
        t.pts.push_back(pos(x, y));
        // de-duplicate
        std::vector<ImVec2> q;
        for (auto& p : t.pts) if (q.empty() || q.back().x != p.x || q.back().y != p.y) q.push_back(p);
        t.pts = q;
        if (t.pts.size() < 2 || steps < 3) return;
        t.end1 = r.f() < 0.55f ? 2 : 1;
        finish(t);
        L.traces.push_back(std::move(t));
    };

    // chips first: reserve their body, then fan traces out from the pins
    for (int c = 0; c < nchips; ++c) {
        int cw = 3 + r.i(5), ch = 3 + r.i(5);
        bool soic = r.f() < 0.4f;
        if (soic) ch = 2 + r.i(2);
        int cx = 4 + r.i(std::max(1, gw - cw - 8)), cy = 4 + r.i(std::max(1, gh - ch - 8));
        bool clash = false;
        for (int y = cy - 1; y <= cy + ch + 1 && !clash; ++y)
            for (int x = cx - 1; x <= cx + cw + 1; ++x) if (used(x, y)) { clash = true; break; }
        if (clash) continue;
        for (int y = cy; y <= cy + ch; ++y) for (int x = cx; x <= cx + cw; ++x) mark(x, y);
        Chip k{pos(cx, cy), pos(cx + cw, cy + ch), cw, soic ? 0 : ch};
        L.chips.push_back(k);
        // pins: top/bottom always, left/right for QFP
        for (int x = cx + 1; x < cx + cw; ++x) {
            if (r.f() < 0.8f) route(x, cy - 1, r.f() < 0.5f ? 6 : (r.f() < 0.5f ? 5 : 7), 6 + r.i(28), 1);
            if (r.f() < 0.8f) route(x, cy + ch + 1, r.f() < 0.5f ? 2 : (r.f() < 0.5f ? 1 : 3), 6 + r.i(28), 1);
        }
        if (!soic) for (int y = cy + 1; y < cy + ch; ++y) {
            if (r.f() < 0.8f) route(cx - 1, y, r.f() < 0.5f ? 4 : (r.f() < 0.5f ? 3 : 5), 6 + r.i(28), 1);
            if (r.f() < 0.8f) route(cx + cw + 1, y, r.f() < 0.5f ? 0 : (r.f() < 0.5f ? 1 : 7), 6 + r.i(28), 1);
        }
    }
    // free traces (buses of 2-4 parallel tracks look very PCB-like)
    for (int i = 0; i < ntraces; ++i) {
        int x = r.i(gw), y = r.i(gh);
        if (used(x, y)) continue;
        int dir = r.i(8);
        int bus = r.f() < 0.35f ? 2 + r.i(3) : 1;
        int len = 8 + r.i(40);
        int px = DX[(dir + 2) & 7], py = DY[(dir + 2) & 7];
        for (int b = 0; b < bus; ++b) {
            int bx = x + px * b * 2, by = y + py * b * 2;   // keep 1 empty cell between bus tracks
            if (!used(bx, by)) route(bx, by, dir, len, r.f() < 0.5f ? 2 : 1);
        }
    }
    // pulses
    int np = std::max(4, (int)L.traces.size() / 3);
    for (int i = 0; i < np && !L.traces.empty(); ++i) {
        Pulse p;
        p.trace = r.i((int)L.traces.size());
        p.s = r.f() * L.traces[p.trace].len;
        p.speed = cell * (4.f + r.f() * 9.f);
        p.tail = cell * (2.5f + r.f() * 4.f);
        p.hue = r.f();
        L.pulses.push_back(p);
    }
}

Scene& scene(int slot) { static Scene s[4]; return s[slot & 3]; }

ImU32 col(float r, float g, float b, float a) {
    auto c = [](float v) { return (int)std::clamp(v * 255.f, 0.f, 255.f); };
    return IM_COL32(c(r), c(g), c(b), c(a));
}

} // namespace

void draw_circuit_fx(ImDrawList* dl, ImVec2 a, ImVec2 b, float intensity, uint32_t seed, int slot) {
    Scene& sc = scene(slot);
    ImVec2 size = b - a;
    if (size.x < 8 || size.y < 8) return;
    const float cell = S(16);
    if (sc.seed != seed || std::fabs(sc.size.x - size.x) > 1 || std::fabs(sc.size.y - size.y) > 1 || sc.cell != cell) {
        sc = Scene{};
        sc.seed = seed; sc.size = size; sc.cell = cell;
        Rng r(seed);
        const float area = size.x * size.y / (cell * cell);
        const float depth[3] = {0.35f, 0.65f, 1.f}, alpha[3] = {0.35f, 0.6f, 1.f}, width[3] = {S(1.2f), S(1.8f), S(2.6f)};
        for (int i = 0; i < 3; ++i) {
            sc.layers[i].depth = depth[i];
            sc.layers[i].alpha = alpha[i];
            build_layer(sc.layers[i], r, size, cell * (0.7f + 0.3f * depth[i]), (int)(area / (90 - i * 15)), 1 + (int)(area / 1400) * (i == 2 ? 1 : 0) + (i == 2 ? 1 : 0),
                        width[i]);
        }
    }
    const double t = ImGui::GetTime();
    float dt = sc.last_t < 0 ? 0.f : (float)std::min(0.1, t - sc.last_t);
    sc.last_t = t;

    const bool dark = th().dark;
    // palette: solder-mask green board, copper traces, cyan/amber electricity
    const ImU32 board = dark ? col(0.03f, 0.07f, 0.06f, intensity) : col(0.90f, 0.95f, 0.92f, intensity);
    dl->PushClipRect(a, b, true);
    dl->AddRectFilled(a, b, board);
    // subtle grid
    {
        const float g = cell * 2;
        ImU32 gc = dark ? col(0.3f, 0.9f, 0.7f, 0.05f * intensity) : col(0.1f, 0.4f, 0.3f, 0.07f * intensity);
        float ox = std::fmod((float)t * S(3), g);
        for (float x = a.x - ox; x < b.x; x += g) dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), gc);
        for (float y = a.y; y < b.y; y += g) dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), gc);
    }
    // parallax: mouse offset + slow drift
    ImVec2 m = ImGui::GetIO().MousePos;
    ImVec2 mc = (m.x > -1e6f) ? ImVec2((m.x - (a.x + b.x) * 0.5f) / size.x, (m.y - (a.y + b.y) * 0.5f) / size.y) : ImVec2(0, 0);
    mc.x = std::clamp(mc.x, -0.6f, 0.6f); mc.y = std::clamp(mc.y, -0.6f, 0.6f);
    for (int li = 0; li < 3; ++li) {
        Layer& L = sc.layers[li];
        ImVec2 off = a + ImVec2(-mc.x * cell * 5 * L.depth + std::sin((float)t * 0.07f) * cell * 2 * L.depth,
                                -mc.y * cell * 5 * L.depth + std::cos((float)t * 0.05f) * cell * 1.5f * L.depth);
        const float ia = intensity * L.alpha;
        const ImU32 copper = dark ? col(0.55f, 0.42f, 0.22f, 0.55f * ia) : col(0.62f, 0.45f, 0.20f, 0.55f * ia);
        const ImU32 copper_hi = dark ? col(0.85f, 0.65f, 0.35f, 0.75f * ia) : col(0.70f, 0.50f, 0.22f, 0.8f * ia);
        const ImU32 mask = dark ? col(0.02f, 0.05f, 0.05f, 0.95f * ia) : col(0.96f, 0.98f, 0.97f, 0.95f * ia);
        // chips
        for (auto& k : L.chips) {
            ImVec2 p0 = k.a + off, p1 = k.b + off;
            dl->AddRectFilled(p0 + ImVec2(S(3), S(3)), p1 + ImVec2(S(3), S(3)), col(0, 0, 0, 0.25f * ia), S(3));
            dl->AddRectFilled(p0, p1, dark ? col(0.10f, 0.11f, 0.12f, ia) : col(0.20f, 0.22f, 0.24f, ia), S(3));
            dl->AddRect(p0, p1, col(0.5f, 0.5f, 0.55f, 0.6f * ia), S(3));
            dl->AddCircleFilled(p0 + ImVec2(S(7), S(7)), S(2.5f), col(0.6f, 0.6f, 0.65f, 0.7f * ia));
        }
        // traces
        for (auto& tr : L.traces) {
            for (size_t i = 1; i < tr.pts.size(); ++i) dl->AddLine(tr.pts[i - 1] + off, tr.pts[i] + off, copper, tr.w);
            for (int e = 0; e < 2; ++e) {
                int kind = e ? tr.end1 : tr.end0;
                ImVec2 p = (e ? tr.pts.back() : tr.pts.front()) + off;
                if (kind == 2) { dl->AddCircleFilled(p, tr.w * 2.2f, copper_hi, 16); dl->AddCircleFilled(p, tr.w * 0.9f, mask, 12); }
                else if (kind == 1) dl->AddRectFilled(p - ImVec2(tr.w * 1.6f, tr.w * 1.6f), p + ImVec2(tr.w * 1.6f, tr.w * 1.6f), copper_hi, S(1));
            }
        }
        // pulses: fading tail + bloom head
        for (auto& pu : L.pulses) {
            const Trace& tr = L.traces[pu.trace];
            pu.s += pu.speed * dt * (0.6f + 0.4f * L.depth);
            if (pu.s - pu.tail > tr.len) {   // hop to a new trace
                pu.trace = (int)((pu.trace * 7919u + 13u + (uint32_t)(t * 1000)) % L.traces.size());
                pu.s = 0;
                continue;
            }
            // electric cyan on dark, deep teal/amber mix on light
            float hr = pu.hue < 0.75f ? 0.25f : 1.0f, hg = pu.hue < 0.75f ? 0.95f : 0.75f, hb = pu.hue < 0.75f ? 1.0f : 0.3f;
            if (!dark) { hr *= 0.5f; hg *= 0.6f; hb *= 0.7f; }
            const int N = 14;
            ImVec2 prev = at(tr, pu.s - pu.tail) + off;
            for (int k = 1; k <= N; ++k) {
                float f = (float)k / N;
                ImVec2 q = at(tr, pu.s - pu.tail * (1 - f)) + off;
                dl->AddLine(prev, q, col(hr, hg, hb, f * f * 0.22f * ia), tr.w * 4.f);
                dl->AddLine(prev, q, col(hr, hg, hb, f * f * 0.85f * ia), tr.w * 1.2f);
                prev = q;
            }
            ImVec2 h = at(tr, pu.s) + off;
            const float rs[4] = {7.f, 4.5f, 2.6f, 1.2f}, as[4] = {0.06f, 0.14f, 0.35f, 1.f};
            for (int k = 0; k < 4; ++k) dl->AddCircleFilled(h, tr.w * rs[k], col(hr, hg, hb, as[k] * ia), 20);
            dl->AddCircleFilled(h, tr.w * 0.6f, col(1, 1, 1, 0.9f * ia), 10);
        }
    }
    dl->PopClipRect();
    request_redraw(2);
}

} // namespace pcbp::gui
