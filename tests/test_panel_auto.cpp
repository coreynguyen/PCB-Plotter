// ============================================================================
// test_panel_auto.cpp — LLM_API §3A.4 G2: panelize from pallet Gerbers (core/panel_auto).
// Panel artwork is synthesised from the CAD under known transforms; detect() must recover
// them exactly, apply() must produce the matching array and move the layers onto the CAD.
// Synthetic data only.
// ============================================================================
#include "../src/core/panel_auto.h"
#include "../src/core/arrays.h"
#include "../src/core/plot_align.h"
#include "../src/core/project.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace pcbp;
static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool near(double a, double b, double t = 1e-3) { return std::fabs(a - b) <= t; }

static void comp(Project& p, const char* ref, const char* pkg, double x, double y, double rot, Side s = Side::Top) {
    Component c; c.ref = ref; c.package = pkg; c.pos = {x, y}; c.rot = rot; c.side = s; c.number = pkg;
    p.add_component(c);
}
static Project circuit(bool symmetric = false) {
    Project p;
    Package chip; chip.name = "0603"; chip.pads = {Pad{{-0.8, 0}, 0.8, 0.9}, Pad{{0.8, 0}, 0.8, 0.9}}; p.packages["0603"] = chip;
    Package so; so.name = "SOIC8";
    for (int i = 0; i < 4; ++i) { so.pads.push_back(Pad{{-1.905 + 1.27 * i, -2.7}, 0.6, 1.55}); so.pads.push_back(Pad{{-1.905 + 1.27 * i, 2.7}, 0.6, 1.55}); }
    so.pads[0].w = 0.9;   // pin-1 pad wider: the package itself is asymmetric
    p.packages["SOIC8"] = so;
    if (symmetric) { comp(p, "R1", "0603", 0, 0, 0); return p; }
    comp(p, "R1", "0603", 10, 10, 0);
    comp(p, "C1", "0603", 5, 20, 90);
    comp(p, "U1", "SOIC8", 25, 15, 90);
    comp(p, "R2", "0603", 30, 4, 270);
    comp(p, "R9", "0603", 18, 22, 37);            // off-quarter: not used as reference
    comp(p, "Q1", "0603", 12, 3, 0, Side::Bottom); // other side: not used for a top layer
    return p;
}

struct Inst { double ox, oy, rot; };
// RS-274X (mm) with one rectangular flash per top pad of every instance
static std::string panel_gerber(const Project& p, const std::vector<Inst>& inst, double scale = 1.0) {
    std::map<std::pair<long, long>, int> ap;
    std::string body;
    auto um = [](double v) { return (long)std::lround(v * 1e6); };
    for (auto& I : inst) {
        const Xform A = Xform::translate(I.ox, I.oy) * Xform::rotation(I.rot);
        for (auto& c : p.comps) {
            if (c.side != Side::Top) continue;
            const Package* pk = p.package_of(c);
            const Xform t = A * p.instance_xform(c, 0);
            for (auto& q : pk->pads) {
                const Vec2 w = t.apply(q.pos);
                const double cr = std::fmod(c.rot + I.rot + q.rot + 3600, 180.0);
                double pw = q.w * scale, ph = q.h * scale;
                if (std::fabs(cr - 90) < 1e-6) std::swap(pw, ph);
                else if (std::fabs(cr) > 1e-6 && std::fabs(cr - 180) > 1e-6) { pw = ph = std::max(pw, ph); }   // off-quarter: any shape
                auto key = std::make_pair(um(pw), um(ph));
                if (!ap.count(key)) { const int d = 10 + (int)ap.size(); ap[key] = d; }
                char b[96];
                std::snprintf(b, sizeof b, "D%d*\nX%ldY%ldD03*\n", ap[key], um(w.x), um(w.y));
                body += b;
            }
        }
    }
    std::string head = "%FSLAX46Y46*%\n%MOMM*%\n";
    for (auto& [k, d] : ap) { char b[96]; std::snprintf(b, sizeof b, "%%ADD%dR,%.6fX%.6f*%%\n", d, k.first / 1e6, k.second / 1e6); head += b; }
    return head + body + "M02*\n";
}
static void add_layer(Project& p, const std::string& gbr, const char* name, LayerRole role = LayerRole::Copper, Side side = Side::Top) {
    GerberLayerRef r; r.name = name; r.path = std::string(name) + ".gbr"; r.content = gbr; r.role = role; r.side = side;
    p.layers.push_back(r);
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // ---- 3 x 2 grid, CAD away from the panel
    {
        Project p = circuit();
        std::vector<Inst> g;
        for (int j = 0; j < 2; ++j) for (int i = 0; i < 3; ++i) g.push_back({5 + 40.0 * i, 7 + 30.0 * j, 0});
        add_layer(p, panel_gerber(p, g), "Panel Top Copper");
        panel_auto::Detection d = panel_auto::detect(p);
        std::printf("  grid: %s\n", d.summary().c_str());
        CHECK(d.ok && d.found.size() == 6 && d.ambiguous == 0);
        CHECK(d.ref_comps == 4);   // R1 C1 U1 R2 (R9 off-quarter, Q1 bottom)
        CHECK(d.linear && d.nx == 3 && d.ny == 2 && near(d.dx, 40) && near(d.dy, 30) && d.root_col == 0 && d.root_row == 0);
        CHECK(!d.found.empty() && d.root == 0 && near(d.found[0].offset.x, 5) && near(d.found[0].offset.y, 7) && near(d.found[0].rot, 0));
        for (auto& f : d.found) CHECK(f.matched == f.total && f.rms < 1e-3);
        std::string err;
        const Vec2 cad_before = p.comps[0].pos;
        CHECK(panel_auto::apply(p, d, {}, err));
        CHECK(p.images.size() == 6 && p.is_panel());
        CHECK(near(p.comps[0].pos.x, cad_before.x) && near(p.comps[0].pos.y, cad_before.y));   // CAD keeps its coordinates
        CHECK(near(p.layers[0].offset.x, -5) && near(p.layers[0].offset.y, -7) && near(p.layers[0].rot, 0));
        int at = 0;
        for (auto& im : p.images) at += (near(im.offset.x, 80) && near(im.offset.y, 30));
        CHECK(at == 1);
        // second apply refuses to replace silently
        CHECK(!panel_auto::apply(p, d, {}, err) && err.find("replace_array") != std::string::npos);
        // re-detect after the move: the root now coincides with the CAD (shift trick keeps it)
        panel_auto::Detection d2 = panel_auto::detect(p);
        CHECK(d2.ok && d2.found.size() == 6 && d2.root >= 0 && near(d2.found[d2.root].offset.x, 0) && near(d2.found[d2.root].offset.y, 0));
    }
    // ---- two-up with the second circuit turned 180 (nonlinear), CAD exactly on the first instance
    {
        Project p = circuit();
        add_layer(p, panel_gerber(p, {{0, 0, 0}, {100, 60, 180}}), "Panel Top Copper");
        panel_auto::Detection d = panel_auto::detect(p);
        std::printf("  180: %s\n", d.summary().c_str());
        CHECK(d.ok && d.found.size() == 2 && !d.linear && d.images.size() == 1);
        if (d.images.empty()) return 1;
        CHECK(near(d.images[0].first.x, 100) && near(d.images[0].first.y, 60) && near(d.images[0].second, 180));
        std::string err;
        CHECK(panel_auto::apply(p, d, {}, err));
        CHECK(p.images.size() == 2 && near(p.images[1].rot, 180) && near(p.images[1].offset.x, 100) && near(p.images[1].offset.y, 60));
        CHECK(near(p.layers[0].offset.x, 0) && near(p.layers[0].offset.y, 0));
    }
    // ---- panel rotated 90 relative to the CAD (CAD drawn landscape, panel portrait)
    {
        Project p = circuit();
        std::vector<Inst> g;
        for (int i = 0; i < 3; ++i) g.push_back({50, 10 + 45.0 * i, 90});
        add_layer(p, panel_gerber(p, g), "Panel Top Copper");
        panel_auto::Detection d = panel_auto::detect(p);
        std::printf("  rot90: %s\n", d.summary().c_str());
        CHECK(d.ok && d.found.size() == 3 && d.root >= 0 && near(d.found[d.root].rot, 90));
        // in the CAD frame the stack runs along -X (R(-90) * (0, 45)) with no rotation between instances
        CHECK(d.linear && d.nx == 3 && d.ny == 1 && near(d.dx, 45));
        std::string err;
        CHECK(panel_auto::apply(p, d, {}, err) && p.images.size() == 3);
        CHECK(near(p.layers[0].rot, 270) || near(p.layers[0].rot, -90));
    }
    // ---- exclude an instance (coupon / x-out) -> nonlinear with the rest
    {
        Project p = circuit();
        std::vector<Inst> g;
        for (int i = 0; i < 3; ++i) g.push_back({40.0 * i, 0, 0});
        add_layer(p, panel_gerber(p, g), "Panel Top Copper");
        panel_auto::Detection d = panel_auto::detect(p);
        panel_auto::ApplyOptions a; a.exclude = {2};
        std::string err;
        CHECK(d.ok && panel_auto::apply(p, d, a, err) && p.images.size() == 2);
    }
    // ---- symmetric circuit (one 0603): every match is ambiguous -> not ok (gate)
    {
        Project p = circuit(true);
        add_layer(p, panel_gerber(p, {{10, 10, 0}, {50, 10, 0}}), "Panel Top Copper");
        panel_auto::Detection d = panel_auto::detect(p);
        std::printf("  symmetric: %s\n", d.summary().c_str());
        CHECK(!d.ok && d.ambiguous == 2 && d.why.find("ambiguous") != std::string::npos);
    }
    // ---- layer choice and failures
    {
        Project p = circuit();
        std::string other = panel_gerber(circuit(true), {{300, 300, 0}});
        add_layer(p, other, "Silk", LayerRole::Silk);
        add_layer(p, panel_gerber(p, {{0, 0, 0}, {40, 0, 0}}), "Panel Top Copper");
        panel_auto::Detection d = panel_auto::detect(p);
        CHECK(d.ok && d.match_layer == 1);   // top copper preferred over silk
        panel_auto::Options o; o.match_layer = 0;
        panel_auto::Detection d0 = panel_auto::detect(p, o);
        CHECK(!d0.ok && !d0.why.empty());
        Project e = circuit();
        CHECK(!panel_auto::detect(e).ok);   // no layers
        Project q = circuit();
        add_layer(q, panel_gerber(q, {{0, 0, 0}, {40, 0, 0}}, 0.80), "Paste 80%", LayerRole::Paste);   // reduced apertures
        panel_auto::Options loose; loose.size_rel = 0.25;
        CHECK(!panel_auto::detect(q).ok && panel_auto::detect(q, loose).ok);
    }
    // ---- scale: 300 parts (600 pads, ref sampled to max_ref), 4 x 3 panel = 7200 flashes
    {
        Project p = circuit(true);
        p.comps.clear();
        unsigned s = 12345;
        auto rnd = [&]() { s = s * 1103515245u + 12345u; return (s >> 8) % 10000 / 10000.0; };
        for (int i = 0; i < 300; ++i) comp(p, ("R" + std::to_string(i)).c_str(), "0603", 2 + rnd() * 56, 2 + rnd() * 36, (int)(rnd() * 4) * 90.0);
        std::vector<Inst> g;
        for (int j = 0; j < 3; ++j) for (int i = 0; i < 4; ++i) g.push_back({1000 + 65.0 * i, 500 + 45.0 * j, 0});
        add_layer(p, panel_gerber(p, g), "Panel Top Copper");
        const auto t0 = std::chrono::steady_clock::now();
        panel_auto::Detection d = panel_auto::detect(p);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  scale: %s (%.0f ms)\n", d.summary().c_str(), ms);
        CHECK(d.ok && d.found.size() == 12 && d.ref_pads == 400 && d.linear && d.nx == 4 && d.ny == 3 && near(d.dx, 65) && near(d.dy, 45));
        CHECK(ms < 5000);
    }
    std::printf("test_panel_auto: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
