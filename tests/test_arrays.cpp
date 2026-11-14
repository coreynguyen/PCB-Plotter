// ============================================================================
// test_arrays.cpp — R22 (REVIEW_V21 §1-§3): reference circuit + linked instances, per-instance outline
// validation, units conversion, delete array, no generated board outline, migration of old generated
// outlines, dialog parameter persistence. Synthetic data only.
// ============================================================================
#include "../src/core/arrays.h"
#include "../src/core/panelize.h"
#include "../src/core/plot_align.h"
#include "../src/core/project_io.h"
#include "../src/core/machine_job.h"
#include <cmath>
#include <cstdio>

using namespace pcbp;
using pcbp::json::Value;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool nearv(Vec2 a, Vec2 b, double t = 1e-6) { return std::fabs(a.x - b.x) <= t && std::fabs(a.y - b.y) <= t; }
static bool neard(double a, double b, double t = 1e-9) { return std::fabs(a - b) <= t; }
static bool has(const std::vector<FrameIssue>& v, const char* k) { for (auto& i : v) if (i.msg.find(k) != std::string::npos) return true; return false; }
static bool has_err(const std::vector<FrameIssue>& v) { for (auto& i : v) if (i.error) return true; return false; }

// 40 x 30 reference circuit, two 0603 parts and an image fiducial, all inside (0,0)-(40,30)
static Project make_circuit() {
    Project p;
    Package r; r.name = "0603"; r.pads = {Pad{{-0.8, 0}, 0.8, 0.9}, Pad{{0.8, 0}, 0.8, 0.9}};
    p.packages["0603"] = r;
    PartNumber pn; pn.number = "RES-10K"; pn.package = "0603"; p.parts[pn.number] = pn;
    auto mk = [&](const char* ref, double x, double y, double a) {
        Component c; c.ref = ref; c.pos = {x, y}; c.rot = a; c.package = "0603"; c.number = "RES-10K"; p.add_component(c);
    };
    mk("R1", 8, 6, 0);
    mk("R2", 30, 22, 90);
    Component f; f.ref = "FID1"; f.pos = {3, 3}; f.feature = Feature::Fiducial; f.fid_scope = FiducialScope::Image; p.add_component(f);
    p.board_frame = {{{0, 0}, {40, 0}, {40, 30}, {0, 30}, {0, 0}}};
    p.board_outline_src = OutlineSource::Confirmed;
    return p;
}

static void test_units() {
    CHECK(neard(units_to_mm(1, Units::MM), 1));
    CHECK(neard(units_to_mm(1, Units::Inch), 25.4));
    CHECK(neard(units_to_mm(1000, Units::Mil), 25.4));
    CHECK(neard(mm_to_units(25.4, Units::Inch), 1));
    CHECK(neard(mm_to_units(25.4, Units::Mil), 1000, 1e-9));
    for (Units u : {Units::MM, Units::Inch, Units::Mil}) CHECK(neard(mm_to_units(units_to_mm(1.2345, u), u), 1.2345, 1e-12));
    // a pitch typed in inches / mils lands in mm on the instances
    Project p = make_circuit();
    CHECK(create_linear_array(p, 2, 2, units_to_mm(1.8, Units::Inch), units_to_mm(1400, Units::Mil)));
    bool found = false;
    for (auto& im : p.images) if (im.x_index == 1 && im.y_index == 1) { found = true; CHECK(nearv(im.offset, {45.72, 35.56}, 1e-9)); }
    CHECK(found);
}

static void test_propagation() {
    Project p = make_circuit();
    const size_t ncomp = p.comps.size();
    CHECK(create_linear_array(p, 3, 1, 45, 0));
    CHECK(p.images.size() == 3 && p.comps.size() == ncomp);   // instances are not copies of the placements
    CHECK(p.panel_frame.empty());                               // and no board outline was made
    const Component& r1 = *p.find_ref("R1");
    CHECK(nearv(p.instance_xform(r1, 2).apply({0, 0}), {98, 6}));
    // edit the reference: placement position, package, part number, circuit outline -> every instance follows
    p.find_ref("R1")->pos = {10, 7};
    p.packages["0603"].pads[0].pos = {-1.0, 0};
    p.find_ref("R1")->number = "RES-22K";
    p.board_frame = {{{-1, -1}, {41, -1}, {41, 31}, {-1, 31}, {-1, -1}}};
    CHECK(nearv(p.instance_xform(*p.find_ref("R1"), 2).apply({0, 0}), {100, 7}));
    CHECK(nearv(p.instance_xform(*p.find_ref("R1"), 2).apply(p.package_of(*p.find_ref("R1"))->pads[0].pos), {99, 7}));
    BBox f2; for (auto& pl : p.circuit_frame_at(2)) for (auto& q : pl) f2.add(q);
    CHECK(neard(f2.x0, 89) && neard(f2.x1, 131) && neard(f2.y0, -1) && neard(f2.y1, 31));
    CHECK(p.instance_count(*p.find_ref("R1")) == 3);
    // a nonlinear (rotated) instance follows too
    CHECK(create_nonlinear_array(p, {ArrayInstance{{130, 30}, 180}}));
    CHECK(p.images.size() == 2 && neard(p.images[1].rot, 180));
    CHECK(nearv(p.instance_xform(*p.find_ref("R1"), 1).apply({0, 0}), {120, 23}));
    // identity entries are the reference itself and are dropped
    CHECK(create_nonlinear_array(p, {ArrayInstance{{0, 0}, 0}, ArrayInstance{{50, 0}, 0}}) && p.images.size() == 2);
    CHECK(!create_nonlinear_array(p, {ArrayInstance{{0, 0}, 360}}));
    // save / load keeps one reference + linked instances (no expansion into copies)
    std::string err;
    Project q;
    CHECK(project_from_json(project_to_json(p), q, err));
    CHECK(q.comps.size() == ncomp && q.images.size() == 2 && nearv(q.images[1].offset, {50, 0}));
}

static void test_validation() {
    // (a) valid neighbouring circuits never warn against the reference's untransformed outline
    Project p = make_circuit();
    CHECK(create_linear_array(p, 2, 1, 45, 0));
    commit_outline_rect(p, OutlineKind::Board, BBox(-5, -5, 90, 35));
    auto v = check_outlines(p);
    CHECK(v.empty());
    // instance 2 placements lie at x = 53 / 75: outside the REFERENCE outline, inside their own -> no finding
    CHECK(!has(v, "outside the circuit outline") && !has(v, "beyond"));
    // (b) a rotated instance is checked against its own rotated outline
    CHECK(create_nonlinear_array(p, {ArrayInstance{{85, 30}, 180}}));
    v = check_outlines(p);
    CHECK(!has(v, "outside the circuit outline") && !has(v, "beyond") && !has(v, "overlap"));
    // (c) a real violation is still found, naming the instance whose outline is crossed
    commit_outline_rect(p, OutlineKind::Board, BBox(-5, -5, 80, 35));
    v = check_outlines(p);
    CHECK(has(v, "circuit 2 outline extends beyond the board outline") && has_err(v));
    CHECK(!has(v, "circuit 1 (reference) outline extends"));
    commit_outline_rect(p, OutlineKind::Board, BBox(-5, -5, 90, 35));
    p.find_ref("R2")->pos = {41, 22};   // outside the reference outline -> outside every instance's outline
    v = check_outlines(p);
    CHECK(has(v, "outside the circuit outline") && v[0].ref == "R2");
    p.find_ref("R2")->pos = {30, 22};
    // (d) the reported false positive: the old panelize generated the board outline around the PLACEMENT extents
    // (+2 mm, + 5 mm margin); defining the measured circuit outline afterwards made "circuit 2 extends beyond".
    {
        Project o = make_circuit();
        o.board_frame.clear();
        CHECK(create_linear_array(o, 2, 1, 45, 0));
        BBox e = o.placement_extents(2.0), all;
        for (int i = 0; i < 2; ++i) { const Xform t = o.image_xform(i); all.add(t.apply({e.x0, e.y0})); all.add(t.apply({e.x1, e.y1})); }
        all.inflate(5.0);
        o.set_panel_frame_rect(all);                       // what "Create board outline around all circuits" stored
        o.panel_outline_src = OutlineSource::Confirmed;
        o.board_frame = {{{-2, -2}, {42, -2}, {42, 32}, {-2, 32}, {-2, -2}}};   // the measured circuit outline (44 wide, pitch 45)
        CHECK(has_err(check_outlines(o)));                  // old behaviour: an Error from a fabricated board outline
        CHECK(has(check_outlines(o), "circuit 2 outline extends beyond the board outline"));
        CHECK(panel_frame_is_generated(o));                // circuit-outline base no longer matches, the placement-extent base does
        Project m = o;
        auto notes = migrate_array_outlines(m);
        CHECK(notes.size() == 1 && m.panel_outline_src == OutlineSource::Inferred);
        auto w = check_outlines(m);
        CHECK(!has_err(w) && has(w, "only inferred"));
        // through the loader: a project saved before R22 is migrated, one saved now is not
        std::string err;
        Value js = project_to_json(o);
        Project l1;
        CHECK(project_from_json(js, l1, err) && l1.panel_outline_src == OutlineSource::Confirmed && l1.migration_notes.empty());
        Value old = Value::object();
        for (auto& kv : js.members()) if (kv.first != "arrays_model") old.set(kv.first, kv.second);
        Project l2;
        CHECK(project_from_json(old, l2, err) && l2.panel_outline_src == OutlineSource::Inferred && l2.migration_notes.size() == 1 && l2.dirty);
    }
    // (e) old machine-program import fallback: circuit outline == board outline on an array -> removed on migration
    {
        Project o = make_circuit();
        CHECK(create_linear_array(o, 2, 1, 45, 0));
        o.set_panel_frame_rect(BBox(-5, -5, 90, 35)); o.panel_outline_src = OutlineSource::Imported;
        o.board_frame = o.panel_frame; o.board_outline_src = OutlineSource::Imported;
        CHECK(has(check_outlines(o), "beyond"));
        auto n = migrate_array_outlines(o);
        CHECK(n.size() == 1 && o.board_frame.empty() && o.panel_outline_src == OutlineSource::Imported);
        CHECK(!has(check_outlines(o), "beyond"));
    }
    // (f) a measured board with uniform rails around measured circuits is not demoted when it was entered after R22
    //     (only projects without "arrays_model" are migrated); panel_frame_is_generated just reports the shape
    {
        Project o = make_circuit();
        CHECK(create_linear_array(o, 2, 1, 45, 0));
        commit_outline_rect(o, OutlineKind::Board, BBox(-5, -5, 90, 35));
        std::string err; Project l;
        CHECK(project_from_json(project_to_json(o), l, err) && l.panel_outline_src == OutlineSource::Confirmed);
    }
    // (g) export validation: global fiducial on an array without a board outline does not crash; board size
    //     covers every instance, not only the reference circuit
    {
        Project o = make_circuit();
        Component g; g.ref = "GF1"; g.pos = {-3, -3}; g.feature = Feature::Fiducial; g.fid_scope = FiducialScope::Global; o.add_component(g);
        CHECK(create_linear_array(o, 3, 1, 45, 0));
        o.model.points.clear();
        ModelPoint mp; mp.name = "Top"; mp.has_machine = true; mp.machine.exporter_id = "juki.h8h"; o.model.points.push_back(mp);
        auto iss = validate_for_export(o, 0);
        CHECK(!iss.empty());
    }
}

static void test_delete_array() {
    Project p = make_circuit();
    const auto comps = p.comps.size();
    const auto frame = p.board_frame;
    CHECK(!delete_array(p));
    CHECK(create_linear_array(p, 2, 3, 45, 35, 1, 1));
    CHECK(p.images.size() == 6 && p.images[0].root && p.images[0].x_index == 1 && p.images[0].y_index == 1);
    commit_outline_rect(p, OutlineKind::Board, BBox(-50, -40, 90, 70));
    CHECK(delete_array(p));
    CHECK(p.images.size() == 1 && p.images[0].root && nearv(p.images[0].offset, {0, 0}));
    CHECK(p.comps.size() == comps && p.board_frame == frame && !p.panel_frame.empty());   // reference + outlines kept
    CHECK(!p.is_panel());
    CHECK(create_linear_array(p, 2, 1, 45, 0) && p.images.size() == 2);   // a new array without rebuilding anything
    // invalid input leaves the project unchanged
    std::string err;
    const auto before = p.images.size();
    CHECK(!create_linear_array(p, 0, 1, 45, 0, 0, 0, &err) && !err.empty() && p.images.size() == before);
    CHECK(!create_linear_array(p, 2, 1, 0, 0, 0, 0, &err) && p.images.size() == before);
    CHECK(!create_linear_array(p, 2, 1, 45, 0, 5, 0, &err) && p.images.size() == before);
    CHECK(!create_linear_array(p, 1, 1, 45, 0, 0, 0, &err));
}

static void test_no_auto_outline() {
    Project p = make_circuit();
    p.board_frame.clear();
    p.board_outline_src = OutlineSource::Inferred;
    CHECK(create_linear_array(p, 3, 2, 45, 35));
    CHECK(p.panel_frame.empty() && p.board_frame.empty());
    CHECK(create_nonlinear_array(p, {ArrayInstance{{60, 0}, 90}, ArrayInstance{{0, 60}, 270}}));
    CHECK(p.panel_frame.empty() && p.board_frame.empty());
    PanelSetup s; s.reset();
    std::string err;
    CHECK(step_and_repeat_all(s, 2, 2, 45, 35, err));
    apply_panel(p, s);
    CHECK(p.images.size() == 4 && p.panel_frame.empty() && p.board_frame.empty());
    CHECK(delete_array(p) && p.panel_frame.empty() && p.board_frame.empty());
    // the outline API rejects Inferred and empty geometry; joins open segments; deletes explicitly
    CHECK(!commit_outline(p, OutlineKind::Board, {{{0, 0}, {10, 0}}}, OutlineSource::Inferred));
    CHECK(!commit_outline(p, OutlineKind::Board, {}));
    CHECK(!commit_outline_rect(p, OutlineKind::Board, BBox(0, 0, 0, 5)));
    CHECK(p.panel_frame.empty());
    CHECK(commit_outline(p, OutlineKind::Circuit, {{{0, 0}, {40, 0}}, {{40, 0}, {40, 30}}, {{40, 30}, {0, 30}}, {{0, 30}, {0, 0}}}, OutlineSource::Imported, true));
    CHECK(p.board_frame.size() == 1 && p.board_frame[0].size() == 5 && p.board_outline_src == OutlineSource::Imported && p.board_frame_from_gerber);
    CHECK(outline_exists(p, OutlineKind::Circuit) && !outline_exists(p, OutlineKind::Board));
    CHECK(delete_outline(p, OutlineKind::Circuit) && p.board_frame.empty() && !delete_outline(p, OutlineKind::Circuit));
    // confirm-as-measured: explicit user action only (freezes the inferred extent, then Confirmed)
    CHECK(p.circuit_outline_source() == OutlineSource::Inferred);
    CHECK(confirm_inferred_outline(p) && p.circuit_outline_source() == OutlineSource::Confirmed && !p.board_frame.empty());
    CHECK(!confirm_inferred_outline(p));
}

static void test_params() {
    Project p = make_circuit();
    PanelSetup s; s.reset();
    s.set(1, 0, 0); s.set(2, 0, 180); s.at(1, 0)->x_out = true;
    s.groups[0].red_x = 1.8; s.groups[2].red_x = 3.6; s.groups[2].red_y = 1.2;
    s.units = Units::Inch; s.ref_location = "R1";
    panel_setup_to_params(s, p.array_params);
    p.array_params.lin_saved = true; p.array_params.lin_nx = 4; p.array_params.lin_ny = 2;
    p.array_params.lin_pitch_x = 45.72; p.array_params.lin_pitch_y = -35; p.array_params.lin_units = Units::Mil; p.array_params.lin_root_col = 3;
    CHECK(p.images.size() == 1);   // saving parameters never touches the array
    std::string err; Project q;
    CHECK(project_from_json(project_to_json(p), q, err));
    PanelSetup r;
    CHECK(panel_setup_from_params(q.array_params, r));
    CHECK(r.count() == 3 && r.at(0, 0) && r.at(0, 0)->root && r.at(2, 0) && r.at(2, 0)->rot == 180 && r.at(1, 0)->x_out);
    CHECK(neard(r.groups[2].red_x, 3.6) && neard(r.groups[2].red_y, 1.2) && r.units == Units::Inch && r.ref_location == "R1");
    const auto& a = q.array_params;
    CHECK(a.lin_saved && a.lin_nx == 4 && a.lin_ny == 2 && neard(a.lin_pitch_x, 45.72) && neard(a.lin_pitch_y, -35) && a.lin_units == Units::Mil && a.lin_root_col == 3);
    // reopening the nonlinear dialog on an existing array: back-filled entries reproduce the same offsets
    {
        Project o = make_circuit();
        CHECK(create_nonlinear_array(o, {ArrayInstance{{45, 0}, 0}, ArrayInstance{{125, 30}, 180}}));
        o.images[1].x_index = 1; o.images[2].x_index = 2;
        PanelSetup b = PanelSetup::from_images(o.images);
        b.units = Units::Inch; b.ref_location = "R1";
        Vec2 rp; CHECK(panel_ref_pos(o, "R1", rp));
        panel_backfill_offsets(b, rp);
        std::string e2;
        CHECK(panelize(b, rp, e2));
        CHECK(b.at(1, 0) && nearv(b.at(1, 0)->offset, {45, 0}, 1e-9) && b.at(2, 0) && nearv(b.at(2, 0)->offset, {125, 30}, 1e-9));
    }
    Project e; PanelSetup t;
    CHECK(!panel_setup_from_params(e.array_params, t));
}

int main() {
    test_units();
    test_propagation();
    test_validation();
    test_delete_array();
    test_no_auto_outline();
    test_params();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
