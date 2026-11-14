// test_package_edit.cpp — package template vs placement instance edits (core/package_edit).
// usage: test_package_edit [file.cpf ...]  (optional real CircuitCAM projects: off-axis scan)
#include "../src/core/package_edit.h"
#include "../src/core/pkg_library.h"
#include "../src/core/project_io.h"
#include "../src/core/json.h"
#include "../src/plugins/plugin.h"
#include "pcbp_plugin.h"
#include <cmath>
#include <cstdio>
#include <fstream>

using namespace pcbp;
static Package rt(const Package& p) { json::Value v; std::string e; json::Value::parse(package_to_json(p).dump(), v, e); return package_from_json(v); }
static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static Package soic8() {
    Package p; p.name = "SOIC8"; p.polarized = false;
    for (int i = 0; i < 4; ++i) { Pad d; d.pos = {-2.7, 1.905 - 1.27 * i}; d.w = 1.5; d.h = 0.6; d.pin = i + 1; p.pads.push_back(d); }
    for (int i = 0; i < 4; ++i) { Pad d; d.pos = {2.7, -1.905 + 1.27 * i}; d.w = 1.5; d.h = 0.6; d.pin = 5 + i; p.pads.push_back(d); }
    p.auto_body();
    return p;
}
static Package sma() {
    Package p; p.name = "SMA"; p.polarized = true;
    Pad a; a.pos = {-2, 0}; a.w = 1.5; a.h = 1.7; a.pin = 1; p.pads.push_back(a);
    Pad b = a; b.pos = {2, 0}; b.pin = 2; p.pads.push_back(b);
    p.auto_body();
    return p;
}
static std::vector<Vec2> world_pads(const Project& prj, const Component& c) {
    std::vector<Vec2> v;
    const Package* p = prj.package_of(c);
    for (auto& d : p->pads) v.push_back(prj.comp_xform(c, 0).apply(d.pos));
    return v;
}
static bool same_pts(const std::vector<Vec2>& a, const std::vector<Vec2>& b, double tol = 1e-6) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (dist(a[i], b[i]) > tol) return false;
    return true;
}
static Project mk() {
    Project prj;
    prj.packages["SOIC8"] = soic8();
    prj.packages["SMA"] = sma();
    Component u1; u1.id = 1; u1.ref = "U1"; u1.package = "SOIC8"; u1.pos = {10, 20}; u1.rot = 90; prj.comps.push_back(u1);
    Component u2 = u1; u2.id = 2; u2.ref = "U2"; u2.pos = {30, 5}; u2.rot = 37; u2.side = Side::Bottom; prj.comps.push_back(u2);
    Component d1; d1.id = 3; d1.ref = "D1"; d1.package = "SMA"; d1.pos = {5, 5}; d1.rot = 0; prj.comps.push_back(d1);
    Component d2 = d1; d2.id = 4; d2.ref = "D2"; d2.pos = {15, 5}; d2.rot = 180; prj.comps.push_back(d2);
    // a placement resolving its package via the part number (no direct package)
    PartNumber pn; pn.number = "DIODE"; pn.package = "SMA"; prj.parts["DIODE"] = pn;
    Component d3 = d1; d3.id = 5; d3.ref = "D3"; d3.package = ""; d3.number = "DIODE"; d3.pos = {25, 5}; d3.rot = 90; prj.comps.push_back(d3);
    return prj;
}

static void test_gate() {
    Project prj = mk();
    const std::string before = package_canon(prj.packages["SOIC8"]);
    PkgEditOpts o; o.confirmed = false;
    auto r = pkg_rotate(prj, "SOIC8", 90, o);
    CHECK(!r.applied && r.needs_confirm && r.affected == 2);
    CHECK(r.message == "This changes 2 placements of SOIC8 (shared)");
    CHECK(package_canon(prj.packages["SOIC8"]) == before);
    CHECK(count_placements(prj, "SMA") == 3);     // includes D3 via part number
}

static void test_rotate_compensate() {
    for (double deg : {90.0, -30.0, 12.5}) {
        Project prj = mk();
        std::vector<std::vector<Vec2>> w0;
        for (auto& c : prj.comps) w0.push_back(world_pads(prj, c));
        PkgEditOpts o; o.confirmed = true; o.compensate = true;
        auto r = pkg_rotate(prj, "SOIC8", deg, o);
        CHECK(r.applied && r.compensated == 2);
        for (size_t i = 0; i < prj.comps.size(); ++i) CHECK(same_pts(world_pads(prj, prj.comps[i]), w0[i], 1e-6));
        // keep placements: board changes for SOIC8 only
        Project q = mk();
        o.compensate = false;
        pkg_rotate(q, "SOIC8", deg, o);
        CHECK(!same_pts(world_pads(q, q.comps[0]), w0[0]));
        CHECK(q.comps[0].rot == 90);
        CHECK(same_pts(world_pads(q, q.comps[2]), w0[2]));
    }
}

// §18: Package origin offset doesn't move finished placements
static void test_origin_compensate() {
    Project prj = mk();
    prj.comps[0].label_off = {1, 2};
    std::vector<std::vector<Vec2>> w0;
    for (auto& c : prj.comps) w0.push_back(world_pads(prj, c));
    const Vec2 lab0 = prj.comps[0].pos + prj.comps[0].label_off;
    PkgEditOpts o; o.confirmed = true;
    auto r = pkg_set_origin(prj, "SOIC8", prj.packages["SOIC8"].pads[0].pos, o);   // origin onto pin 1
    CHECK(r.applied && r.compensated == 2);
    for (size_t i = 0; i < prj.comps.size(); ++i) CHECK(same_pts(world_pads(prj, prj.comps[i]), w0[i], 1e-9));
    CHECK(dist(prj.comps[0].pos + prj.comps[0].label_off, lab0) < 1e-9);
    // back to the lead centroid ("Center Part Origin")
    r = pkg_set_origin(prj, "SOIC8", pkg_lead_centroid(prj.packages["SOIC8"]), o);
    for (size_t i = 0; i < prj.comps.size(); ++i) CHECK(same_pts(world_pads(prj, prj.comps[i]), w0[i], 1e-9));
    CHECK(dist(prj.comps[0].pos, {10, 20}) < 1e-9);
    // via part number (D3) too
    Project q = mk();
    auto wd3 = world_pads(q, q.comps[4]);
    pkg_set_origin(q, "SMA", {0.7, -0.3}, o);
    CHECK(same_pts(world_pads(q, q.comps[4]), wd3, 1e-9));
}

// §7 / §18: Pin-one assignment modifies no shared records
static void test_assign_pin_one() {
    Project prj = mk();
    const std::string pk0 = package_canon(prj.packages["SMA"]);
    const Component d1_0 = prj.comps[2], d3_0 = prj.comps[4];
    Component& d2 = prj.comps[3];
    auto w0 = world_pads(prj, d2);
    std::string msg;
    const Package& p = prj.packages.at("SMA");
    double d = placement_assign_pin_one(d2, p, 1, &msg);   // user clicks lead 2 of D2 (shown at 180)
    CHECK(d == 180);
    CHECK(d2.rot == 0);
    CHECK(package_canon(prj.packages["SMA"]) == pk0);      // package untouched
    CHECK(prj.packages["SMA"].pads[0].pin == 1 && prj.packages["SMA"].pads[1].pin == 2);
    CHECK(prj.comps[2].rot == d1_0.rot && dist(prj.comps[2].pos, d1_0.pos) < 1e-12);
    CHECK(prj.comps[4].rot == d3_0.rot);
    auto w1 = world_pads(prj, d2);
    CHECK(dist(w1[0], w0[1]) < 1e-9 && dist(w1[1], w0[0]) < 1e-9);   // pin 1 now where lead 2 was
    // SOIC8: clicking pin 5 (opposite corner) -> 180; clicking pin 4 -> no symmetric fit
    Component& u1 = prj.comps[0];
    const auto wu = world_pads(prj, u1);
    const Package& s = prj.packages.at("SOIC8");
    CHECK(placement_assign_pin_one(u1, s, 4, &msg) == 180);
    CHECK(dist(world_pads(prj, u1)[0], wu[4]) < 1e-9);
    CHECK(std::isnan(placement_assign_pin_one(u1, s, 3, &msg)));
    // bottom placement
    Component& u2 = prj.comps[1];
    const auto wb = world_pads(prj, u2);
    CHECK(placement_assign_pin_one(u2, s, 4, &msg) == 180);
    CHECK(dist(world_pads(prj, u2)[0], wb[4]) < 1e-9);
}

static void test_pins() {
    Project prj = mk();
    auto pins = pins_cyclic_from(prj.packages["SOIC8"], 4);
    CHECK(pins[4] == 1 && pins[5] == 2 && pins[3] == 8 && pins[0] == 5);
    PkgEditOpts o;
    CHECK(pkg_set_pins(prj, "SOIC8", pins, o).needs_confirm);
    o.confirmed = true;
    CHECK(pkg_set_pins(prj, "SOIC8", pins, o).applied);
    CHECK(prj.packages["SOIC8"].pads[4].pin == 1);
    CHECK(!pkg_set_pins(prj, "SOIC8", std::vector<int>(8, 1), o).applied);
    // transactional pin ordering: cancel restores
    Package s = soic8();
    PinOrderSession ps; ps.begin(s);
    ps.click(7); ps.click(6);
    ps.cancel();
    CHECK(ps.original[0] == 1 && ps.original[7] == 8);
    for (size_t i = 0; i < s.pads.size(); ++i) CHECK(s.pads[i].pin == (int)i + 1);
    ps.begin(s); ps.click(7); ps.click(6);
    auto r = ps.result();
    CHECK(r[7] == 1 && r[6] == 2 && r[0] == 3 && r[5] == 8);
    CHECK(!ps.complete());
}

// §18: Rotated imported artwork detected / preserved
static void test_offaxis() {
    Package s = soic8();
    CHECK(!pkg_axis_info(s).off_axis);
    Package r = s;
    pkg_geom_rotate(r, 30);
    auto ai = pkg_axis_info(r);
    CHECK(ai.off_axis);
    CHECK(std::fabs(ai.off90 - 30) < 0.01 || std::fabs(ai.off90 + 60) < 0.01);
    // preserved through save/load (no silent normalisation)
    Package back = rt(r);
    CHECK(package_same(back, r));
    CHECK(pkg_axis_info(back).off_axis);
    // normalise by -30 with compensation: board unchanged, now on-axis
    Project prj; prj.packages["ROT"] = r; prj.packages["ROT"].name = "ROT";
    Component c; c.ref = "U9"; c.package = "ROT"; c.pos = {3, 4}; c.rot = 0; prj.comps.push_back(c);
    auto w0 = world_pads(prj, prj.comps[0]);
    PkgEditOpts o; o.confirmed = true;
    pkg_rotate(prj, "ROT", -30, o);
    CHECK(!pkg_axis_info(prj.packages["ROT"]).off_axis);
    CHECK(std::fabs(prj.comps[0].rot - 30) < 1e-9);
    CHECK(same_pts(world_pads(prj, prj.comps[0]), w0, 1e-9));
    // marker / class round trip
    Package m = s; m.marker_kind = 2; m.marker_color = 0xFF00FF00; m.orient_class = 2; m.marker_user_pos = true; m.marker_pos = {1, 2};
    Package mb = rt(m);
    CHECK(mb.marker_kind == 2 && mb.marker_color == 0xFF00FF00 && mb.orient_class == 2 && mb.marker_user_pos && dist(mb.marker_pos, {1, 2}) < 1e-9);
    CHECK(!package_same(m, s));
    CHECK(orient_class_of(s) == OrientClass::Ambiguous);   // SOIC8 fits at 0/180
    CHECK(orient_class_of(sma()) == OrientClass::Polarized);
    CHECK(lead_symmetry_deg(s) == 180);
}

static void scan_cpf(const char* path) {
    if (!std::ifstream(path).good()) { std::printf("SKIP %s\n", path); return; }
    PluginRegistry reg; register_builtin_plugins(reg);
    Importer* imp = reg.importer("circuitcam.cpf");
    Project prj; ImportReport rep; std::string err;
    if (!imp || !imp->run(path, default_options(imp->info()), prj, rep, err)) { std::printf("import failed %s: %s\n", path, err.c_str()); return; }
    int off = 0;
    for (auto& kv : prj.packages) {
        auto ai = pkg_axis_info(kv.second);
        if (ai.off_axis) { ++off; std::printf("  off-axis %-20s %+7.2f deg (%s) pads=%zu users=%d\n", kv.first.c_str(), ai.off90, ai.why.c_str(), kv.second.pads.size(), count_placements(prj, kv.first)); }
    }
    int odd = 0;
    for (auto& c : prj.comps) if (std::fabs(std::remainder(c.rot, 90.0)) > 0.01) ++odd;
    std::printf("%s: %zu packages, %d off-axis; %zu placements, %d at non-90 rotation\n", path, prj.packages.size(), off, prj.comps.size(), odd);
}

// R12 §8 / §9
static Package chip(const char* name, double L, double W, double pitch) {
    Package p; p.name = name;
    Pad a; a.pos = {-pitch / 2, 0}; a.w = L; a.h = W; a.pin = 1; p.pads.push_back(a);
    Pad b = a; b.pos = {pitch / 2, 0}; b.pin = 2; p.pads.push_back(b);
    return p;
}
static Package qfp(const char* name, int per_side, double pitch, double span, double pw, double pl) {
    Package p; p.name = name;
    int pin = 1;
    const double o = (per_side - 1) * pitch / 2;
    for (int i = 0; i < per_side; ++i) { Pad d; d.pos = {-span / 2, o - i * pitch}; d.w = pl; d.h = pw; d.pin = pin++; p.pads.push_back(d); }
    for (int i = 0; i < per_side; ++i) { Pad d; d.pos = {-o + i * pitch, -span / 2}; d.w = pw; d.h = pl; d.pin = pin++; p.pads.push_back(d); }
    for (int i = 0; i < per_side; ++i) { Pad d; d.pos = {span / 2, -o + i * pitch}; d.w = pl; d.h = pw; d.pin = pin++; p.pads.push_back(d); }
    for (int i = 0; i < per_side; ++i) { Pad d; d.pos = {o - i * pitch, span / 2}; d.w = pw; d.h = pl; d.pin = pin++; p.pads.push_back(d); }
    return p;
}
static void test_similar() {
    const PkgMatchTol t;   // 0.05 mm / 5 %
    Package a = chip("chip_1.65x0.75", 1.65, 0.75, 3.0), b = chip("chip_1.67x0.80", 1.67, 0.80, 3.02);
    CHECK(pkg_similar(a, b, t));
    Package c = chip("chip_1.0x0.5", 1.0, 0.5, 1.6);
    CHECK(!pkg_similar(a, c, t));
    Package v = b; for (auto& d : v.pads) { d.pos = rotate(d.pos, 90); d.rot = 90; }   // vertical: other orientation reference
    CHECK(!pkg_similar(a, v, t));
    PkgMatchTol any = t; any.orientation_aware = false;
    double rr = -1; CHECK(pkg_similar(a, v, any, &rr) && (rr == 90 || rr == 270));
    // same bbox, different pad count / pitch: never similar
    Package q1 = qfp("QFP44", 11, 0.8, 11.0, 0.45, 1.5), q2 = qfp("QFP48", 12, 0.5, 11.0, 0.3, 1.5);
    CHECK(!pkg_similar(q1, q2, t));
    // origin offset only (same pads): similar; merge compensates positions
    Project prj;
    Package off = a; off.name = "chip_off"; for (auto& d : off.pads) d.pos += Vec2{0.3, -0.2};
    prj.packages[a.name] = a; prj.packages[b.name] = b; prj.packages[off.name] = off; prj.packages[c.name] = c;
    Component k; k.id = 1; k.ref = "C1"; k.package = off.name; k.pos = {10, 10}; k.rot = 90; prj.comps.push_back(k);
    Component k2 = k; k2.id = 2; k2.ref = "C2"; k2.package = a.name; prj.comps.push_back(k2);
    Component k3 = k; k3.id = 3; k3.ref = "C3"; k3.package = a.name; k3.side = Side::Bottom; k3.rot = 30; prj.comps.push_back(k3);
    auto w0 = world_pads(prj, prj.comps[0]);
    auto groups = pkg_similar_groups(prj, t);
    CHECK(groups.size() == 1 && groups[0].keep == a.name && groups[0].merge.size() == 2);
    CHECK(pkg_find_similar(prj, b, t, b.name) == a.name || pkg_find_similar(prj, b, t, b.name) == off.name);
    pkg_keep_separate(prj, a.name, b.name);
    CHECK(rt(prj.packages[a.name]).keep_separate.size() == 1);
    groups = pkg_similar_groups(prj, t);
    CHECK(groups.size() == 1 && groups[0].merge.size() == 1 && groups[0].merge[0] == off.name);
    const int n = pkg_merge(prj, a.name, {off.name});
    CHECK(n == 1 && prj.comps[0].package == a.name && !prj.packages.count(off.name));
    CHECK(same_pts(world_pads(prj, prj.comps[0]), w0, 1e-9));
}
static void test_orientation_needed() {
    Package q = qfp("LQFP-44", 11, 0.8, 11.0, 0.45, 1.5);
    CHECK(pkg_orientation_marking_needed(q));                       // QFP auto-detected
    Package sot; sot.name = "SOT-23";
    for (Vec2 v : {Vec2{-0.95, -1}, Vec2{0.95, -1}, Vec2{0, 1}}) { Pad d; d.pos = v; d.w = 0.6; d.h = 0.7; sot.pads.push_back(d); }
    sot.pads[0].pin = 1; sot.pads[1].pin = 2; sot.pads[2].pin = 3;
    CHECK(pkg_orientation_marking_needed(sot));
    CHECK(!pkg_orientation_marking_needed(chip("R0603", 0.9, 0.8, 1.6)));
    CHECK(pkg_orientation_marking_needed(chip("SOD-123", 0.9, 1.2, 3.3)));
    CHECK(pkg_electrically_polarized_guess(chip("TANT_CASE_B", 1.5, 2.4, 2.8)));
    CHECK(!pkg_electrically_polarized_guess(q));                     // marked, not polarized
    // marker sits next to pin 1, between the lead and the body; nominal size comparable for QFP and SOT
    MarkerGeom mq = marker_geom(q), ms = marker_geom(sot);
    CHECK(mq.valid && ms.valid);
    CHECK(dist(mq.at, q.pads[0].pos) < 2.0 && std::fabs(mq.at.x) < std::fabs(q.pads[0].pos.x));
    CHECK(mq.size / ms.size < 4.0 && ms.size / mq.size < 4.0);
}

int main(int argc, char** argv) {
    test_similar();
    test_orientation_needed();
    test_gate();
    test_rotate_compensate();
    test_origin_compensate();
    test_assign_pin_one();
    test_pins();
    test_offaxis();
    for (int i = 1; i < argc; ++i) scan_cpf(argv[i]);
    std::printf("package_edit: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
