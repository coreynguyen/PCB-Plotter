// test_viewmate_bin.cpp — ViewMate job (.bin) reader, Gerber rendering, importer plugin.
// Real sample: ViewMate Pro 11.18 Samples/Sample1up.bin (skipped if absent).
// Synthetic jobs (write_job) cover arcs, polygons with arc segments and holes, routes,
// thermal / diamond / octagon / rounded-rect apertures and a metric database.
#include "local_data.h"
#include "../src/io/viewmate_bin.h"
#include "../src/io/gerber.h"
#include "../src/core/fileio.h"
#include "../src/plugins/plugin.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

using namespace pcbp;
namespace vm = pcbp::io::vm;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool near(double a, double b, double t = 1e-6) { return std::fabs(a - b) <= t; }

static const std::string sSample = pcbp_test::local("vm_sample"); static const char* kSample = sSample.c_str();

static gerber::Layer parse(const std::string& g) {
    gerber::Layer L; gerber::ParseOptions po; std::string err;
    bool ok = gerber::parse(g, po, L, err);
    if (!ok) std::printf("  gerber parse error: %s\n", err.c_str());
    return L;
}

static void test_sample() {
    if (!std::ifstream(kSample).good()) { std::printf("SKIP: %s not found\n", kSample); return; }
    vm::Job j; std::string err;
    CHECK(vm::read_job_file(kSample, j, err));
    CHECK(j.version == 12);
    CHECK(j.units == 0 && !j.metric());
    CHECK(j.resolution == 100 && (j.flags & 1));
    CHECK(near(j.unit_mm, 2.54e-5, 1e-12));
    CHECK(j.layers.size() == 13);
    CHECK(j.apertures.size() == 127);
    CHECK(j.elements.size() == 32325);
    CHECK(j.warnings.empty());
    for (auto& w : j.warnings) std::printf("  warning: %s\n", w.c_str());
    CHECK(j.has_palette);
    CHECK(j.tool_sizes.size() == 256 && j.tool_sizes[4] == 20000);
    // layer table
    struct L { int idx; const char* name; int color, type; const char* code; bool neg; };
    const L want[] = {{1, "Sample1up.1", 10, 5, "CPU", false}, {2, "Sample1up.2", 2, 0, "SIG", false},
                      {3, "Sample1up.g3f", 7, 13, "GD1", true}, {4, "Sample1up.p4f", 7, 23, "PW1", true},
                      {6, "Sample1up.6ss", 9, 6, "CPL", false}, {7, "Sample1up.dri", 12, 2, "DRI", false},
                      {8, "Sample1up.m1", 7, 9, "MSU", false}, {11, "Sample1up.s1", 14, 7, "SKU", false},
                      {12, "Sample1up.oln", 4, 34, "BOL", false}};
    for (auto& w : want) {
        const vm::Layer* l = j.layer(w.idx);
        CHECK(l != nullptr);
        if (!l) continue;
        CHECK(l->name == w.name); CHECK(l->color == w.color); CHECK(l->type == w.type);
        CHECK(vm::type_code(l->type) == w.code); CHECK(l->negative() == w.neg); CHECK(l->visible());
        CHECK(l->bnum == w.idx);
    }
    CHECK(j.layer(13) && j.layer(13)->name.empty() && j.count(13, vm::Element::Pad) == 0);
    // apertures
    auto& a10 = j.apertures.at(10); CHECK(a10.shape == vm::ShRound && a10.w == 100000);
    auto& a55 = j.apertures.at(55); CHECK(a55.shape == vm::ShRect && a55.w == 100000 && a55.h == 33000);
    auto& a80 = j.apertures.at(80); CHECK(a80.shape == vm::ShSquare && a80.w == 47000);
    auto& a38 = j.apertures.at(38);
    CHECK(a38.shape == vm::ShThermal && a38.w == 60000 && a38.h == 15000 && a38.th_inner == 40000 && a38.th_spokes == 4 && near(a38.th_angle, 45));
    auto& t = j.apertures.at(5004); CHECK(t.shape == vm::ShTool && t.h == 8 && t.w == 45000);
    CHECK(std::string(vm::shape_letter(17)) == "Q" && std::string(vm::shape_letter(15)) == "N");
    // per-layer element counts
    CHECK(j.count(1, vm::Element::Pad) == 2742 && j.count(1, vm::Element::Trace) == 2462);
    CHECK(j.count(7, vm::Element::Pad) == 1668 && j.count(7, vm::Element::Trace) == 0);
    CHECK(j.count(11, vm::Element::Trace) == 4807);
    CHECK(j.count(12, vm::Element::Trace) == 4);
    // Gerber rendering round trip: object counts and extents
    for (auto& l : j.layers) {
        int pads = j.count(l.index, vm::Element::Pad), tr = j.count(l.index, vm::Element::Trace);
        if (!pads && !tr) continue;
        std::vector<std::string> w;
        std::string g = vm::to_gerber(j, l.index, &w);
        gerber::Layer G = parse(g);
        CHECK(G.flash_count() == pads);
        CHECK(G.draw_count() == tr);
        CHECK(G.warnings.empty());
        if (!G.warnings.empty()) std::printf("  L%d gerber warning: %s\n", l.index, G.warnings[0].c_str());
        CHECK(G.negative == l.negative());
        CHECK(G.extent.w() > 150 && G.extent.w() < 260 && G.extent.h() > 100 && G.extent.h() < 160);
    }
    // a known pad: layer 1 D10 first pad at (7535300, 4086900) uin = (191.39662, 103.80726) mm, 0.1 in round
    {
        gerber::Layer G = parse(vm::to_gerber(j, 1, nullptr));
        bool found = false;
        for (auto& o : G.objs) if (o.kind == gerber::Object::Flash && near(o.a.x, 191.39662, 1e-4) && near(o.a.y, 103.80726, 1e-4)) {
            const gerber::Aperture* ap = gerber::find_ap(G, o.dcode);
            found = ap && ap->shape == gerber::Aperture::Circle && near(ap->w, 2.54, 1e-6);
            if (found) break;
        }
        CHECK(found);
        // thermal macro on the ground plane flattens to polygons
        gerber::Layer P = parse(vm::to_gerber(j, 3, nullptr));
        const gerber::Aperture* th = gerber::find_ap(P, 38);
        CHECK(th && th->shape == gerber::Aperture::Macro && !th->macro_polys.empty());
    }
    // importer plugin
    auto& reg = PluginRegistry::get();
    Importer* im = reg.detect(kSample);
    CHECK(im && im->info().id == "viewmate.bin");
    if (im) {
        Project prj; ImportReport rep;
        CHECK(im->run(kSample, default_options(im->info()), prj, rep, err));
        CHECK(prj.layers.size() == 12);
        CHECK(rep.layers_added == 12);
        if (prj.layers.size() == 12) {
            CHECK(prj.layers[0].role == LayerRole::Copper && prj.layers[0].side == Side::Top);
            CHECK(prj.layers[5].role == LayerRole::Copper && prj.layers[5].side == Side::Bottom);
            CHECK(prj.layers[6].role == LayerRole::Drill);
            CHECK(prj.layers[7].role == LayerRole::Mask && prj.layers[7].side == Side::Top);
            CHECK(prj.layers[8].role == LayerRole::Mask && prj.layers[8].side == Side::Bottom);
            CHECK(prj.layers[9].role == LayerRole::Paste);
            CHECK(prj.layers[10].role == LayerRole::Silk);
            CHECK(prj.layers[11].role == LayerRole::Outline);
            CHECK(prj.layers[0].color == 0xFF00FF00u);    // palette 10 = green
            CHECK(prj.layers[6].color == 0xFFFF0000u);    // palette 12 = red
            CHECK(prj.layers[2].content.find("%IPNEG*%") != std::string::npos);
            CHECK(prj.layers[0].name == "Sample1up.1");
        }
        for (auto& m : rep.messages) std::printf("    %s\n", m.c_str());
    }
}

// --- synthetic jobs -------------------------------------------------------
static int32_t kArcWord() { return 0x7FFFFFFF; }
static vm::Element pad(int layer, int d, int x, int y) { vm::Element e; e.kind = vm::Element::Pad; e.layer = layer; e.dcode = d; e.v[0] = x; e.v[1] = y; return e; }

static vm::Job synth(bool metric) {
    vm::Job j;
    j.version = 12; j.units = metric ? 2 : 0; j.resolution = 100; j.flags = 1;
    j.bbox[2] = 1000000; j.bbox[3] = 1000000;
    vm::Layer a; a.index = 1; a.name = "top.gtl"; a.color = 10; a.state = 97; a.type = 5; a.bnum = 1;
    vm::Layer b; b.index = 2; b.name = "plane.g2"; b.color = 7; b.state = 99; b.type = 13; b.bnum = 2;
    vm::Layer c; c.index = 3; c.name = "rout.rou"; c.color = 4; c.state = 96; c.type = 139; c.bnum = 3;
    j.layers = {a, b, c};
    auto ap = [&](int d, int shape, int w, int h, int p0 = 0) { vm::Aperture x; x.dcode = d; x.shape = shape; x.w = w; x.h = h; x.p0 = p0; j.apertures[d] = x; };
    ap(10, vm::ShRound, 50000, 0);
    ap(11, vm::ShRect, 60000, 30000);
    ap(12, vm::ShObround, 80000, 40000);
    ap(13, vm::ShDiamond, 40000, 40000);
    ap(14, vm::ShOctagon, 50000, 50000);
    ap(15, vm::ShRoundRect, 60000, 40000, 10000);
    ap(16, vm::ShThermal, 70000, 12000);
    j.apertures[16].th_inner = 50000; j.apertures[16].th_spokes = 4; j.apertures[16].th_angle = 45;
    ap(5, vm::ShRound, 8000, 0);           // D-code below 10 -> renumbered in Gerber
    j.tool_sizes.assign(256, 0); j.tool_sizes[3] = 93000;
    for (int d = 10; d <= 16; ++d) j.elements.push_back(pad(1, d, 100000 * (d - 9), 200000));
    vm::Element t; t.kind = vm::Element::Trace; t.layer = 1; t.dcode = 5; t.v[0] = 0; t.v[1] = 0; t.v[2] = 500000; t.v[3] = 0; j.elements.push_back(t);
    // clockwise quarter arc around (300000,300000), radius 100000: from (0,+r) to (+r,0) relative
    vm::Element ar; ar.kind = vm::Element::Arc; ar.layer = 1; ar.dcode = 10;
    ar.v[0] = 0; ar.v[1] = 100000; ar.v[2] = 100000; ar.v[3] = 0; ar.v[4] = 300000; ar.v[5] = 300000; j.elements.push_back(ar);
    // full circle
    vm::Element fc = ar; fc.v[0] = 50000; fc.v[1] = 0; fc.v[2] = 50000; fc.v[3] = 0; j.elements.push_back(fc);
    // polygon: square 0..400000 with an arc segment (CCW bulge on the right side) + square hole; scratch flag off
    vm::Element pg; pg.kind = vm::Element::Polygon; pg.layer = 2; pg.dcode = 0;
    vm::Contour out;
    out.words = {0, 0, kArcWord(), 400000, 200000, 200000, 2, 400000, 0, 400000, 400000, 0, 400000};
    vm::Contour hole; hole.flags = 0x10;
    hole.words = {100000, 100000, 100000, 200000, 200000, 200000, 200000, 100000};
    pg.contours = {out, hole};
    j.elements.push_back(pg);
    // a scratch pad on the plane
    vm::Element sp = pad(2, 16, 600000, 600000); sp.clear = true; j.elements.push_back(sp);
    // route with tool 3
    vm::Element rt; rt.kind = vm::Element::Route; rt.layer = 3; rt.tool = 3;
    vm::Contour path; path.words = {0, 0, 0, 900000, 900000, 900000};
    rt.contours = {path};
    j.elements.push_back(rt);
    return j;
}

static void test_synthetic(bool metric) {
    vm::Job j = synth(metric);
    std::vector<uint8_t> bin = vm::write_job(j);
    CHECK(vm::is_job(bin.data(), bin.size()));
    vm::Job r; std::string err;
    CHECK(vm::read_job(bin.data(), bin.size(), r, err));
    CHECK(r.warnings.empty());
    for (auto& w : r.warnings) std::printf("  warning: %s\n", w.c_str());
    CHECK(r.metric() == metric);
    CHECK(r.layers.size() == 3 && r.layers[1].negative() && !r.layers[2].visible());
    CHECK(r.elements.size() == j.elements.size());
    CHECK(r.apertures.size() == j.apertures.size());
    CHECK(r.apertures[16].th_inner == 50000 && r.apertures[16].th_spokes == 4 && near(r.apertures[16].th_angle, 45));
    if (r.elements.size() == j.elements.size())
        for (size_t i = 0; i < r.elements.size(); ++i) {
            const auto &a = j.elements[i], &b = r.elements[i];
            CHECK(a.kind == b.kind && a.layer == b.layer && a.dcode == b.dcode && a.clear == b.clear);
            CHECK(std::memcmp(a.v, b.v, sizeof a.v) == 0);
            CHECK(a.contours.size() == b.contours.size());
            for (size_t k = 0; k < a.contours.size() && k < b.contours.size(); ++k) CHECK(a.contours[k].words == b.contours[k].words);
        }
    const double mm = metric ? 1e-5 : 2.54e-5;
    // layer 1: pads, trace, arcs
    {
        std::vector<std::string> w;
        gerber::Layer G = parse(vm::to_gerber(r, 1, &w));
        CHECK(G.flash_count() == 7);
        CHECK(G.warnings.empty());
        for (auto& x : G.warnings) std::printf("  gw %s\n", x.c_str());
        int arcs = 0, draws = 0;
        for (auto& o : G.objs) {
            if (o.kind == gerber::Object::Arc) {
                ++arcs;
                CHECK(o.cw);
                CHECK(near(o.center.x, 300000 * mm, 1e-6) && near(o.center.y, 300000 * mm, 1e-6));
                const double sweep = gerber::arc_sweep_deg(o);
                CHECK(near(sweep, -90, 1e-3) || near(sweep, -360, 1e-3));
            }
            if (o.kind == gerber::Object::Draw) { ++draws; CHECK(near(o.b.x, 500000 * mm, 1e-6)); }
        }
        CHECK(arcs == 2 && draws == 1);
        // aperture shapes
        auto ap = [&](int d) { return gerber::find_ap(G, d); };
        CHECK(ap(10) && ap(10)->shape == gerber::Aperture::Circle && near(ap(10)->w, 50000 * mm));
        CHECK(ap(11) && ap(11)->shape == gerber::Aperture::Rect && near(ap(11)->w, 60000 * mm) && near(ap(11)->h, 30000 * mm));
        CHECK(ap(12) && ap(12)->shape == gerber::Aperture::Obround);
        CHECK(ap(13) && ap(13)->shape == gerber::Aperture::Macro);
        CHECK(ap(14) && ap(14)->shape == gerber::Aperture::Polygon && ap(14)->verts == 8);
        CHECK(ap(15) && ap(15)->shape == gerber::Aperture::Macro);
        CHECK(ap(16) && ap(16)->shape == gerber::Aperture::Macro);
        // diamond extents = w x h
        if (ap(13)) { BBox bb = gerber::aperture_bbox(*ap(13)); CHECK(near(bb.w(), 40000 * mm, 1e-6) && near(bb.h(), 40000 * mm, 1e-6)); }
        if (ap(15)) { BBox bb = gerber::aperture_bbox(*ap(15)); CHECK(near(bb.w(), 60000 * mm, 1e-6) && near(bb.h(), 40000 * mm, 1e-6)); }
    }
    // layer 2: polygon with arc + hole (clear), scratch pad, negative
    {
        std::string g = vm::to_gerber(r, 2, nullptr);
        CHECK(g.find("%IPNEG*%") != std::string::npos);
        gerber::Layer G = parse(g);
        CHECK(G.negative);
        CHECK(G.region_count() == 2);
        int clear_regions = 0, clear_flash = 0;
        for (auto& o : G.objs) {
            if (o.kind == gerber::Object::Region && o.clear) ++clear_regions;
            if (o.kind == gerber::Object::Flash && o.clear) ++clear_flash;
            if (o.kind == gerber::Object::Region && !o.clear) {
                // the CCW arc bulges right of x=400000 to x=600000
                double maxx = -1e9; for (auto& p : o.poly) maxx = std::max(maxx, p.x);
                CHECK(near(maxx, 600000 * mm, 2e-3));
            }
        }
        CHECK(clear_regions == 1 && clear_flash == 1);
    }
    // layer 3: route drawn with the tool diameter
    {
        gerber::Layer G = parse(vm::to_gerber(r, 3, nullptr));
        CHECK(G.draw_count() == 2);
        if (!G.objs.empty()) { const gerber::Aperture* a = gerber::find_ap(G, G.objs[0].dcode); CHECK(a && near(a->w, 93000 * mm, 1e-6)); }
    }
}

static void test_robustness() {
    vm::Job j = synth(false);
    std::vector<uint8_t> bin = vm::write_job(j);
    // every truncation must fail cleanly or return a partial job
    for (size_t n = 0; n < bin.size(); n += 7) {
        vm::Job r; std::string err;
        bool ok = vm::read_job(bin.data(), n, r, err);
        if (ok) CHECK(r.elements.size() <= j.elements.size());
    }
    std::mt19937 rng(12345);
    for (int it = 0; it < 400; ++it) {
        std::vector<uint8_t> c = bin;
        for (int k = 0; k < 6; ++k) c[45 + rng() % (c.size() - 45)] = (uint8_t)rng();
        vm::Job r; std::string err;
        if (vm::read_job(c.data(), c.size(), r, err)) for (auto& l : r.layers) (void)vm::to_gerber(r, l.index, nullptr);
    }
    ++g_pass;   // no crash
    const char junk[] = "\x7f" "ELF not a job at all";
    CHECK(!vm::is_job((const uint8_t*)junk, sizeof junk));
    // a non-job .bin file is not claimed by the importer
    auto tmp = std::filesystem::temp_directory_path() / "pcbp_vm_junk.bin";
    { std::ofstream f(tmp, std::ios::binary); f << "random firmware image bytes\x01\x02\x03"; }
    Importer* im = PluginRegistry::get().detect(tmp.string());
    CHECK(!im || im->info().id != "viewmate.bin");
    std::filesystem::remove(tmp);
    // a synthetic job through the importer
    auto tj = std::filesystem::temp_directory_path() / "pcbp_vm_synth.bin";
    { std::ofstream f(tj, std::ios::binary); f.write((const char*)bin.data(), (std::streamsize)bin.size()); }
    im = PluginRegistry::get().detect(tj.string());
    CHECK(im && im->info().id == "viewmate.bin");
    if (im) {
        Project prj; ImportReport rep; std::string err;
        CHECK(im->run(tj.string(), default_options(im->info()), prj, rep, err));
        CHECK(prj.layers.size() == 3);
        if (prj.layers.size() == 3) { CHECK(prj.layers[2].role == LayerRole::Outline); CHECK(!prj.layers[2].visible); }
    }
    std::filesystem::remove(tj);
}

int main() {
    register_builtin_plugins(PluginRegistry::get());
    test_sample();
    test_synthetic(false);
    test_synthetic(true);
    test_robustness();
    std::printf("passed %d, failed %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
