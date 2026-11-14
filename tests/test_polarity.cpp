// test_polarity.cpp — R15 polarity marker model (specs/POLARITY.md):
// legacy migration, defaults by package class, marker transform on rotated / bottom placements,
// and the "real body outline is never replaced by derived data" rule.
#include "../src/core/package_edit.h"
#include "../src/core/pkg_library.h"
#include "../src/core/project_io.h"
#include "../src/core/json.h"
#include <cmath>
#include <cstdio>

using namespace pcbp;
static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static Pad pad(double x, double y, double w, double h, int pin, PadShape s = PadShape::Rect) { Pad d; d.pos = {x, y}; d.w = w; d.h = h; d.pin = pin; d.shape = s; return d; }
static Package chip2(const char* name, const char* desc = "") {
    Package p; p.name = name; p.description = desc;
    p.pads = {pad(-0.75, 0, 0.8, 0.9, 1), pad(0.75, 0, 0.8, 0.9, 2)};
    p.auto_body();
    return p;
}
static Package soic(const char* name, int n) {
    Package p; p.name = name;
    for (int i = 0; i < n / 2; ++i) p.pads.push_back(pad(-2.7, 1.27 * (n / 4.0 - 0.5) - 1.27 * i, 1.5, 0.6, i + 1));
    for (int i = 0; i < n / 2; ++i) p.pads.push_back(pad(2.7, -1.27 * (n / 4.0 - 0.5) + 1.27 * i, 1.5, 0.6, n / 2 + i + 1));
    p.auto_body();
    return p;
}
static Package qfp(const char* name) {
    Package p; p.name = name; int pin = 1;
    for (int i = 0; i < 8; ++i) p.pads.push_back(pad(-4.5, 1.75 - 0.5 * i, 1.4, 0.3, pin++));
    for (int i = 0; i < 8; ++i) p.pads.push_back(pad(-1.75 + 0.5 * i, -4.5, 0.3, 1.4, pin++));
    for (int i = 0; i < 8; ++i) p.pads.push_back(pad(4.5, -1.75 + 0.5 * i, 1.4, 0.3, pin++));
    for (int i = 0; i < 8; ++i) p.pads.push_back(pad(1.75 - 0.5 * i, 4.5, 0.3, 1.4, pin++));
    p.auto_body();
    return p;
}
static Package sot23() {
    Package p; p.name = "SOT-23";
    p.pads = {pad(-0.95, -1.0, 0.6, 0.7, 1), pad(0.95, -1.0, 0.6, 0.7, 2), pad(0, 1.0, 0.6, 0.7, 3)};
    p.auto_body();
    return p;
}
// synthetic 5x5 BGA (1.0 mm pitch, ball field 4x4 mm) with a CAD body outline 8x10 mm, larger than the balls
static Package bga_cad() {
    Package p; p.name = "BGA25C100P5X5_800X1000";
    int pin = 1;
    for (int r = 0; r < 5; ++r) for (int c = 0; c < 5; ++c) p.pads.push_back(pad(-2 + c, 2 - r, 0.45, 0.45, pin++, PadShape::Round));
    GPrim o; o.kind = GKind::Rect; o.pts = {{-4, -5}, {4, 5}}; o.width = 0.12;
    GPrim ch; ch.kind = GKind::Line; ch.pts = {{-4, 4}, {-3, 5}}; ch.width = 0.12;     // chamfer line at pin-1 corner
    p.body = {o, ch};
    p.body_source = "cad";
    p.leads = 25;
    return p;
}
static bool same_body(const Package& a, const Package& b) {
    if (a.body.size() != b.body.size() || a.body_source != b.body_source) return false;
    for (size_t i = 0; i < a.body.size(); ++i) {
        if (a.body[i].kind != b.body[i].kind || a.body[i].pts.size() != b.body[i].pts.size()) return false;
        for (size_t k = 0; k < a.body[i].pts.size(); ++k) if (dist(a.body[i].pts[k], b.body[i].pts[k]) > 1e-12) return false;
    }
    return true;
}
static Project reload(const Project& p) {
    json::Value v; std::string e;
    json::Value::parse(project_to_json(p).dump(), v, e);
    Project q; project_from_json(v, q, e);
    return q;
}

static void test_defaults() {
    // ICs / asymmetric multi-lead packages: has polarity
    for (Package p : {soic("SOIC8", 8), soic("TSOP-16", 16), qfp("LQFP32"), qfp("QFN32"), sot23(), bga_cad()}) {
        CHECK(pkg_default_has_polarity(p));
        Package q = p; pkg_apply_polarity_default(q);
        CHECK(pkg_has_polarity(q) && q.polarity_set);
        CHECK(q.marker_kind == (uint8_t)MarkerKind::Dot && q.marker_color == 0xFFE03131 && q.marker_user_pos);
    }
    // 2-terminal chips R / C: no polarity
    for (Package p : {chip2("R0603"), chip2("C0402"), chip2("RESC1608X55N"), chip2("CAPC1005X55N"), chip2("IND0805")}) {
        CHECK(!pkg_default_has_polarity(p));
        Package q = p; pkg_apply_polarity_default(q);
        CHECK(!pkg_has_polarity(q) && q.polarity_set);
    }
    // ... except LED, diode, tantalum (name or description)
    for (Package p : {chip2("LED0603"), chip2("SOD-123"), chip2("TANT_CASE_B"), chip2("CHIP0805", "Red LED 0805"), chip2("DIODE_MINIMELF"), chip2("SMA")})
        CHECK(pkg_default_has_polarity(p));
    // radial 2-lead (electrolytic) by class
    { Package p = chip2("CP_RADIAL_5MM"); p.radial = true; p.smt = false; CHECK(pkg_default_has_polarity(p)); }
    { Package p = chip2("RAD2"); p.radial = true; p.smt = false; CHECK(pkg_default_has_polarity(p)); }

    // dot near lead 1: closer to lead 1 than to any other lead
    { Package q = qfp("LQFP32"); pkg_add_polarity_marker(q);
      double d1 = dist(q.marker_pos, q.pads[0].pos), dmin = 1e9;
      for (size_t i = 1; i < q.pads.size(); ++i) dmin = std::min(dmin, dist(q.marker_pos, q.pads[i].pos));
      CHECK(d1 < dmin); }
    // 2-terminal: just outside the outer end of lead 1, not on the pad
    { Package q = chip2("LED0603"); pkg_add_polarity_marker(q); CHECK(q.marker_pos.x < -0.75 - 0.4 && std::fabs(q.marker_pos.y) < 1e-9); }
    // no lead 1: near the FIRST lead
    { Package q = soic("SOIC8", 8); for (auto& d : q.pads) d.pin = 0; pkg_add_polarity_marker(q);
      double d0 = dist(q.marker_pos, q.pads[0].pos), dmin = 1e9;
      for (size_t i = 1; i < q.pads.size(); ++i) dmin = std::min(dmin, dist(q.marker_pos, q.pads[i].pos));
      CHECK(pkg_has_polarity(q) && d0 < dmin); }

    // never overwrite a marker the user placed; defaults never re-applied after the user removed one
    { Package q = soic("SOIC8", 8); pkg_add_polarity_marker(q); q.marker_pos = {7, 7}; q.marker_kind = (uint8_t)MarkerKind::Arrow; q.marker_color = 0xFF339AF0;
      CHECK(!pkg_add_polarity_marker(q)); CHECK(!pkg_apply_polarity_default(q));
      CHECK(dist(q.marker_pos, {7, 7}) < 1e-12 && q.marker_kind == (uint8_t)MarkerKind::Arrow && q.marker_color == 0xFF339AF0);
      pkg_remove_polarity_marker(q); CHECK(!pkg_has_polarity(q));
      Project prj; prj.packages[q.name] = q;
      CHECK(prj_apply_polarity_defaults(prj) == 0 && !pkg_has_polarity(prj.packages[q.name]));
      Project back = reload(prj);
      CHECK(!pkg_has_polarity(back.packages[q.name]) && back.packages[q.name].polarity_set);
      CHECK(prj_apply_polarity_defaults(back) == 0 && !pkg_has_polarity(back.packages[q.name])); }
    // project pass: new (undecided) packages get their default once
    { Project prj; prj.packages["SOIC8"] = soic("SOIC8", 8); prj.packages["R0603"] = chip2("R0603");
      CHECK(prj_apply_polarity_defaults(prj) == 2);
      CHECK(pkg_has_polarity(prj.packages["SOIC8"]) && !pkg_has_polarity(prj.packages["R0603"]));
      CHECK(prj_apply_polarity_defaults(prj) == 0); }
}

static void test_migration() {
    // build a pre-R15 project file: no "polarity_model" / "pol" keys, old flags
    Project old;
    Package a = soic("SOIC8", 8);                         // automatic pin-1 mark (fits two ways) -> marker
    Package b = chip2("R0603");                           // plain R -> no marker
    Package c = chip2("D_SMA"); c.polarized = true;       // "Polarized" ticked -> marker
    Package d = chip2("X2"); d.pin_marked = true;         // "Pin-1 mark" ticked -> marker
    Package e = qfp("LQFP32"); e.orient_class = (uint8_t)OrientClass::Symmetric;   // pin-1 mark explicitly OFF -> none
    Package f = soic("SOIC14", 14); f.marker_kind = (uint8_t)MarkerKind::Arrow; f.marker_color = 0xFF51CF66;   // custom arrow kept
    Package g = soic("SOIC16", 16); g.marker_kind = 1; g.marker_user_pos = true; g.marker_pos = {0.5, 0.25};  // user position kept
    for (Package* p : {&a, &b, &c, &d, &e, &f, &g}) old.packages[p->name] = *p;
    json::Value v; std::string err;
    json::Value::parse(project_to_json(old).dump(), v, err);
    // strip the R15 keys so the file looks like a v14 project
    std::string txt = v.dump();
    auto strip = [&](const std::string& k) { for (size_t at; (at = txt.find(k)) != std::string::npos;) txt.erase(at, k.size()); };
    strip(",\"polarity_model\":1"); strip("\"polarity_model\":1,");
    CHECK(txt.find("polarity_model") == std::string::npos);
    json::Value v2; CHECK(json::Value::parse(txt, v2, err));
    Project m; CHECK(project_from_json(v2, m, err));
    auto& ma = m.packages["SOIC8"]; auto& mb = m.packages["R0603"]; auto& mc = m.packages["D_SMA"]; auto& md = m.packages["X2"];
    auto& me = m.packages["LQFP32"]; auto& mf = m.packages["SOIC14"]; auto& mg = m.packages["SOIC16"];
    CHECK(pkg_has_polarity(ma) && ma.marker_kind == 1 && ma.polarity_set);
    CHECK(dist(ma.marker_pos, marker_geom(a).at) < 1e-9);           // exactly the old automatic position
    CHECK(!pkg_has_polarity(mb) && mb.polarity_set);
    CHECK(pkg_has_polarity(mc) && dist(mc.marker_pos, marker_geom(c).at) < 1e-9);
    CHECK(pkg_has_polarity(md));
    CHECK(!pkg_has_polarity(me));
    CHECK(mf.marker_kind == (uint8_t)MarkerKind::Arrow && mf.marker_color == 0xFF51CF66 && dist(mf.marker_pos, marker_geom(f).at) < 1e-9);
    CHECK(pkg_has_polarity(mg) && dist(mg.marker_pos, {0.5, 0.25}) < 1e-12);
    // migrated data is stable: save + load again changes nothing, and is not re-migrated
    Project m2 = reload(m);
    for (auto& kv : m.packages) {
        const Package& x = kv.second; const Package& y = m2.packages[kv.first];
        CHECK(x.marker_kind == y.marker_kind && dist(x.marker_pos, y.marker_pos) < 1e-12 && y.polarity_set);
        CHECK(std::fabs(x.marker_rot - y.marker_rot) < 1e-9);
    }
    // migration never touches bodies or machine-relevant data
    CHECK(same_body(ma, a) && ma.pads.size() == a.pads.size() && mc.polarized == true);
    // a moved marker survives later lead edits (it is fixed package data)
    Package moved = ma; moved.marker_pos = {9, 9};
    moved.pads[0].pos.x -= 0.1;
    Package back = moved; pkg_apply_polarity_default(back);
    CHECK(dist(back.marker_pos, {9, 9}) < 1e-12);
}

static void test_transform() {
    Project prj;
    Package p = soic("SOIC8", 8);
    pkg_add_polarity_marker(p);
    p.marker_pos = {1, 0}; p.marker_kind = (uint8_t)MarkerKind::Arrow; p.marker_rot = 0;
    prj.packages[p.name] = p;
    auto at = [&](double x, double y, double rot, Side s) {
        Component c; c.ref = "U1"; c.package = p.name; c.pos = {x, y}; c.rot = rot; c.side = s;
        return polarity_marker_on_board(prj, c, p);
    };
    auto near = [](Vec2 a, Vec2 b) { return dist(a, b) < 1e-9; };
    auto ang = [](double a, double b) { double d = std::fmod(std::fabs(a - b), 360.0); return std::min(d, 360 - d) < 1e-6; };
    PolarityMarkerOnBoard m;
    m = at(10, 20, 0, Side::Top);     CHECK(m.valid && near(m.at, {11, 20}) && ang(m.rot, 0));
    m = at(10, 20, 90, Side::Top);    CHECK(near(m.at, {10, 21}) && ang(m.rot, 90));
    m = at(10, 20, 180, Side::Top);   CHECK(near(m.at, {9, 20}) && ang(m.rot, 180));
    m = at(10, 20, 270, Side::Top);   CHECK(near(m.at, {10, 19}) && ang(m.rot, 270));
    m = at(10, 20, 0, Side::Bottom);  CHECK(near(m.at, {9, 20}) && ang(m.rot, 180));      // mirrored
    m = at(10, 20, 90, Side::Bottom); CHECK(near(m.at, {10, 19}) && ang(m.rot, 270));
    m = at(0, 0, 45, Side::Top);      CHECK(near(m.at, {std::sqrt(0.5), std::sqrt(0.5)}) && ang(m.rot, 45));
    // the marker follows lead 1 under any placement transform (same transform as the pads)
    Package q = qfp("LQFP32"); pkg_add_polarity_marker(q); prj.packages[q.name] = q;
    for (Side s : {Side::Top, Side::Bottom}) for (double r : {0.0, 90.0, 33.0, 270.0}) {
        Component c; c.package = q.name; c.pos = {5, -3}; c.rot = r; c.side = s;
        const Xform t = prj.comp_xform(c);
        PolarityMarkerOnBoard b = polarity_marker_on_board(prj, c, q);
        const Vec2 l1 = t.apply(q.pads[0].pos);
        double dmin = 1e9;
        for (size_t i = 1; i < q.pads.size(); ++i) dmin = std::min(dmin, dist(b.at, t.apply(q.pads[i].pos)));
        CHECK(dist(b.at, l1) < dmin);
    }
    // no marker -> nothing drawn
    Package r = chip2("R0603"); pkg_apply_polarity_default(r); prj.packages[r.name] = r;
    Component c; c.package = r.name;
    CHECK(!polarity_marker_on_board(prj, c, r).valid);
    // package rotation turns the arrow with the artwork
    Package w = p; pkg_geom_rotate(w, 90); CHECK(ang(w.marker_rot, 90) && near(w.marker_pos, {0, 1}));
}

static void test_bga_body_never_changed() {
    const Package bga = bga_cad();
    CHECK(!pkg_body_replaceable(bga));
    const BBox ob = gprims_bbox(bga.body);
    CHECK(std::fabs(ob.w() - 8) < 0.2 && std::fabs(ob.h() - 10) < 0.2);   // outline larger than the balls (4.45 x 4.45)
    Project prj; prj.packages[bga.name] = bga;
    Component u; u.ref = "U1"; u.package = bga.name; u.pos = {20, 20}; prj.comps.push_back(u);
    PkgEditOpts o; o.confirmed = true;
    // every polarity operation the UI performs (add / type / colour / move / size / rotate / delete / default / migrate / toggle x2)
    Package x = bga;
    pkg_apply_polarity_default(x); CHECK(same_body(x, bga));
    pkg_remove_polarity_marker(x); CHECK(same_body(x, bga));
    pkg_add_polarity_marker(x); CHECK(same_body(x, bga));
    x.marker_kind = (uint8_t)MarkerKind::Arrow; x.marker_rot = 135; x.marker_pos += Vec2{0.1, -0.2}; x.marker_size = 0.4; x.marker_color = 0xFF51CF66;
    CHECK(pkg_replace(prj, bga.name, x, o).applied); CHECK(same_body(prj.packages[bga.name], bga));
    Package y = prj.packages[bga.name]; pkg_remove_polarity_marker(y);
    CHECK(pkg_replace(prj, bga.name, y, o).applied); CHECK(same_body(prj.packages[bga.name], bga));
    Package z = prj.packages[bga.name]; pkg_add_polarity_marker(z);
    CHECK(pkg_replace(prj, bga.name, z, o).applied); CHECK(same_body(prj.packages[bga.name], bga));
    Package l = bga; l.pin_marked = true; l.polarized = true; pkg_migrate_polarity_legacy(l); CHECK(same_body(l, bga) && pkg_has_polarity(l));
    prj_apply_polarity_defaults(prj); CHECK(same_body(prj.packages[bga.name], bga));
    // save / load (old and new format) keeps the CAD outline
    Project back = reload(prj); CHECK(same_body(back.packages[bga.name], bga));
    // derived bodies stay replaceable suggestions, and are labelled as such
    Package dp = bga; dp.body.clear(); dp.body_source.clear(); CHECK(pkg_body_replaceable(dp));
    dp.auto_body(); CHECK(dp.body_source == "pads" && pkg_body_replaceable(dp));
    Package ext = bga; ext.body_source = "cad_extent"; CHECK(!pkg_body_replaceable(ext));
    Package usr = bga; usr.body_source = ""; CHECK(!pkg_body_replaceable(usr));          // drawn / unknown = real
}

int main() {
    test_defaults();
    test_migration();
    test_transform();
    test_bga_body_never_changed();
    std::printf("test_polarity: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
