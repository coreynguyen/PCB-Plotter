// test_angles.cpp — the one-rotation model (specs/ANGLES.md) + group ops (§9)
// + export validation / transform chain (§18/§19) + real .cpf angle cases.
#include "local_data.h"
#include "../src/core/group_ops.h"
#include "../src/core/machine_job.h"
#include "../src/core/project_io.h"
#include "../src/core/json.h"
#include "../src/io/juki_writer.h"
#include "../src/io/juki_import.h"
#include "../src/plugins/plugin.h"
#include "pcbp_plugin.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>

using namespace pcbp;
static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool near(double a, double b, double t = 1e-9) { return std::fabs(a - b) < t; }
static bool near_deg(double a, double b, double t = 1e-9) { double d = std::fabs(norm_deg(a - b)); return d < t || 360.0 - d < t; }

static Component mk(const char* ref, double x, double y, double rot, Side s = Side::Top, const char* pkg = "0603") {
    Component c; c.ref = ref; c.pos = {x, y}; c.rot = rot; c.rot_imported = rot; c.cad_pos = c.pos; c.side = s;
    c.package = pkg; c.number = std::string("PN-") + pkg; c.assigned_point = 0;
    return c;
}

static Project base(bool bottom = false, bool flipped = false, int orient = 0, bool mirror = false) {
    Project p;
    Package pk; pk.name = "0603"; pk.pads = {Pad{{-0.8, 0}, 0.8, 0.9}, Pad{{0.8, 0}, 0.8, 0.9}}; pk.pads[0].pin = 1; pk.pads[1].pin = 2;
    p.packages["0603"] = pk; Package so = pk; so.name = "SOT23"; p.packages["SOT23"] = so;
    p.model.points.clear();
    ModelPoint mp; mp.name = "SMT"; mp.has_machine = true; mp.machine.exporter_id = "juki.h7h";
    mp.machine.side = bottom ? Side::Bottom : Side::Top; mp.machine.flipped = flipped; mp.machine.orientation = orient;
    mp.machine.mirror_angle_when_flipped = mirror;
    p.model.points.push_back(mp);
    Component f = mk("FID1", 1, 1, 0, bottom ? Side::Bottom : Side::Top); f.feature = Feature::Fiducial; f.assigned_point = -1; p.add_component(f);
    return p;
}

static std::map<std::string, double> job_angles(const Project& p) {
    MachineJob j; std::string err; std::map<std::string, double> m;
    if (!build_machine_job(p, 0, j, err)) { std::printf("  job: %s\n", err.c_str()); return m; }
    for (auto& q : j.placements) m[q.ref] = q.rot;
    return m;
}

// §4 example: R1=0, R2=180, package family +90 -> 90/270 (relative 180 kept)
static void test_group_delta() {
    Project p = base();
    p.add_component(mk("R1", 10, 10, 0)); p.add_component(mk("R2", 20, 10, 180));
    p.add_component(mk("Q1", 30, 10, 45, Side::Top, "SOT23"));
    CHECK(select_by_package(p, {"0603"}, SelMode::Replace) == 2);
    auto idx = selected_indices(p);
    CHECK(rotate_add(p, idx, 90) == 2);
    CHECK(near(p.find_ref("R1")->rot, 90) && near(p.find_ref("R2")->rot, 270));
    CHECK(near(p.find_ref("Q1")->rot, 45));
    auto a = job_angles(p);
    CHECK(near(a["R1"], 90) && near(a["R2"], 270) && near(a["Q1"], 45));
    CHECK(rotate_add(p, idx, 180) == 2 && near(p.find_ref("R2")->rot, 90) && near(p.find_ref("R1")->rot, 270));   // wraps, normalised
    CHECK(rotate_set(p, idx, -90) == 1 && near(p.find_ref("R1")->rot, 270) && near(p.find_ref("R2")->rot, 270));
    CHECK(rotate_reset(p, idx) == 2 && near(p.find_ref("R1")->rot, 0) && near(p.find_ref("R2")->rot, 180));
    CHECK(p.find_ref("R1")->rot_imported == 0);
}

// arbitrary angles survive (never snapped), through job + H7H/H8H + re-import
static void test_fractional_roundtrip() {
    for (auto fmt : {juki::HlcFormat::H7H, juki::HlcFormat::H8H}) {
        Project p = base();
        const double want[] = {0, 90, 180, 270, 25, 33.5, 359, 12.3375};
        const char* refs[] = {"C1", "C2", "C3", "C4", "C5", "C6", "C7", "C8"};
        for (int i = 0; i < 8; ++i) p.add_component(mk(refs[i], 5 + 4 * i, 7, want[i]));
        auto a = job_angles(p);
        for (int i = 0; i < 8; ++i) CHECK(near(a[refs[i]], want[i]));
        MachineJob j; std::string err; CHECK(build_machine_job(p, 0, j, err));
        std::vector<uint8_t> bin; CHECK(juki::write_hlc(j, fmt, bin, err));
        juki::HlcParsed hp; CHECK(juki::parse_hlc(bin, hp, err));
        std::map<std::string, int> raw; for (auto& q : hp.placements) raw[q.ref] = q.angle;
        for (int i = 0; i < 8; ++i) CHECK(raw[refs[i]] == (int)std::lround(want[i] * 80));
        Project back; juki::ImportOptions io; juki::ImportResult ir;
        CHECK(juki::import_hlc(bin, back, io, ir, err));
        for (int i = 0; i < 8; ++i) {
            const Component* c = back.find_ref(refs[i]);
            CHECK(c && near_deg(c->rot, want[i], 1.0 / 160));
        }
    }
}

// bottom side: default keeps the Board View angle (CircuitCAM-validated); mirror option negates
static void test_bottom() {
    {
        Project p = base(true, true, 0, false);
        p.add_component(mk("U1", 10, 10, 30, Side::Bottom)); p.add_component(mk("U2", 20, 10, 270, Side::Bottom));
        auto a = job_angles(p);
        CHECK(near(a["U1"], 30) && near(a["U2"], 270));
        MachineJob j; std::string err; build_machine_job(p, 0, j, err);
        for (auto& q : j.placements) if (q.ref == "U1") CHECK(near(q.x, -(10 - p.virtual_origin.x)));   // X mirrored
    }
    {
        Project p = base(true, true, 0, true);
        p.add_component(mk("U1", 10, 10, 30, Side::Bottom)); p.add_component(mk("U2", 20, 10, 270, Side::Bottom));
        auto a = job_angles(p);
        CHECK(near(a["U1"], 330) && near(a["U2"], 90));
    }
    {   // orientation is added (explicit, CCW)
        Project p = base(false, false, 90);
        p.add_component(mk("R1", 10, 10, 33.5));
        CHECK(near(job_angles(p)["R1"], 123.5));
    }
}

// legacy flags: force_angle_calc / use_cad_coords / legacy machine angle never change output
static void test_legacy_flags() {
    Project p = base();
    Component c = mk("R1", 10, 10, 90); c.has_legacy_mach_rot = true; c.legacy_mach_rot = 180; c.cad_pos = {99, 99};
    p.add_component(c);
    auto a0 = job_angles(p);
    p.model.points[0].machine.force_angle_calc = true; auto a1 = job_angles(p);
    p.model.points[0].machine.use_cad_coords = true; auto a2 = job_angles(p);
    CHECK(near(a0["R1"], 90) && near(a1["R1"], 90) && near(a2["R1"], 90));
    // migration: old native "mach_rot" -> legacy only
    json::Value v; std::string err;
    (void)err;
    Project q = p;
    q.comps.back().has_legacy_mach_rot = false;
    std::string text = project_to_json(q).dump();
    const std::string key = "\"cad_rot\"";
    size_t at = text.find(key, text.find("\"R1\""));
    CHECK(at != std::string::npos);
    text.insert(at, "\"mach_rot\": 200, ");
    Project r;
    CHECK(json::Value::parse(text, v, err) && project_from_json(v, r, err));
    const Component* rc = r.find_ref("R1");
    CHECK(rc && rc->has_legacy_mach_rot && near(rc->legacy_mach_rot, 200) && near(rc->rot, 90));
    CHECK(near(job_angles(r)["R1"], 90));
    // validation reports the legacy difference as info, and nothing blocks (R16: with a confirmed outline)
    r.set_circuit_frame_rect(BBox(-50, -50, 50, 50)); r.board_outline_src = OutlineSource::Confirmed;
    auto iss = validate_for_export(r, 0);
    bool info = false; for (auto& i : iss) info |= i.sev == ExportIssue::Info && i.ref == "R1";
    CHECK(info && !has_errors(iss));
}

static void test_trace_validate() {
    Project p = base(true, true, 90);
    p.virtual_origin = {1, 2};
    p.add_component(mk("U7", 11, 12, 25, Side::Bottom));
    PlacementTrace t; std::string err;
    CHECK(trace_placement(p, 0, p.comps.size() - 1, 0, t, err));
    MachineJob j; build_machine_job(p, 0, j, err);
    CHECK(j.placements.size() == 1 && near(t.x, j.placements[0].x) && near(t.y, j.placements[0].y) && near(t.rot, j.placements[0].rot));
    CHECK(near(t.rot, 115) && t.steps.size() >= 9);
    std::printf("%s", trace_to_text(t).c_str());
    Project e = base(); e.model.points[0].has_machine = false;
    auto iss = validate_for_export(e, 0);
    CHECK(has_errors(iss));
    Project d = base(); d.add_component(mk("R1", 1, 1, 0)); d.add_component(mk("R1", 2, 1, 0));
    CHECK(has_errors(validate_for_export(d, 0)));
}

static void test_select_outliers() {
    Project p = base();
    p.add_component(mk("R1", 1, 1, 0)); p.add_component(mk("R2", 2, 1, 180)); p.add_component(mk("R3", 3, 1, 90));
    p.add_component(mk("R4", 4, 1, 33.5)); p.add_component(mk("R10", 5, 1, 270, Side::Bottom));
    CHECK(ref_matches("R10", "R1-R10") && ref_matches("r4", "R*") && !ref_matches("R10", "R?") && ref_matches("C7", "C5-9"));
    CHECK(select_by_ref(p, "R1-R3, R10", SelMode::Replace) == 4);
    CHECK(select_by_side(p, Side::Bottom, SelMode::Subtract) == 1 && selected_indices(p).size() == 3);
    CHECK(select_by_process(p, Process::SMT, SelMode::Replace) == 5);
    auto idx = selected_indices(p);
    CHECK(set_process(p, {idx[0]}, Process::Hardware) == 1 && p.process_of(p.comps[idx[0]]) == Process::Hardware);
    auto o = find_outliers(p, idx);
    CHECK(o.size() == 1 && p.comps[o[0].index].ref == "R4" && near(o[0].rot, 33.5));
    CHECK(set_group_label(p, idx, "rework") == 5 && set_package(p, {idx[1]}, "SOT23") == 1);
}

// Real CircuitCAM files: Board View angle (Locations.Rot) is the exported angle; the legacy
// per-point MachRotation is kept for comparison. Where the user had run Set Neutralized
// Angles (legacy == rot) our export equals CircuitCAM's written angle exactly.
static void test_cpf_real() {
    struct Case { std::string path; const char* ref; double board, legacy; };
    const Case cases[] = {
        {pcbp_test::local("cpf_b"), "Q1", 180, 270},   // stale calculated SOT-23 angle
        {pcbp_test::local("cpf_b"), "D1", 270, 0},
        {pcbp_test::local("cpf_b"), "C3", 270, 270},
        {pcbp_test::local("cpf_c"), "Y2", 0, 90},
        {pcbp_test::local("cpf_d"), "R1", 180, 180},       // bottom, flipped
        {pcbp_test::local("cpf_d"), "J3", 270, 270},
        {pcbp_test::local("cpf_e"), "R31", 270, 270},
    };
    PluginRegistry r; register_builtin_plugins(r);
    Importer* imp = r.importer("circuitcam.cpf");
    CHECK(imp != nullptr);
    if (!imp) return;
    std::map<std::string, Project> cache;
    for (auto& k : cases) {
        if (!std::ifstream(k.path).good()) { std::printf("SKIP %s\n", k.path.c_str()); continue; }
        if (!cache.count(k.path)) {
            Project prj; ImportReport rep; std::string err;
            CHECK(imp->run(k.path, default_options(imp->info()), prj, rep, err));
            cache[k.path] = prj;
        }
        Project& prj = cache[k.path];
        const Component* c = prj.find_ref(k.ref);
        CHECK(c != nullptr);
        if (!c) continue;
        CHECK(near(c->rot, k.board));
        CHECK(c->has_legacy_mach_rot && near_deg(c->legacy_mach_rot, k.legacy));
        if (c->assigned_point >= 0 && prj.model.points[c->assigned_point].has_machine) {
            MachineJob j; std::string err;
            if (build_machine_job(prj, c->assigned_point, j, err))
                for (auto& q : j.placements) if (q.ref == k.ref && q.circuit == 1) {
                    CHECK(near(q.rot, k.board));
                    if (k.board == k.legacy) CHECK(near(q.rot, k.legacy));   // == CircuitCAM's output after neutralize
                }
        }
    }
}

// R18a (ANGLES.md §10): tape_rot is ignored; the supply angle is per part number, defaults by packaging,
// and never changes the placement / machine angle.
static void test_supply_and_tape() {
    Project p = base();
    Component c = mk("U1", 10, 10, 90, Side::Top, "SOT23");
    p.add_component(c);
    p.packages["SOT23"].tape_rot = 270;
    const Component& u = *p.find_ref("U1");
    CHECK(near(export_angle(p, u, 0, p.model.points[0].machine), 90));
    CHECK(near(supply_angle_default("Tape"), 0) && near(supply_angle_default("stick"), 270) && near(supply_angle_default("Tray"), 180) &&
          near(supply_angle_default("MTC"), 0) && near(supply_angle_default("Bulk"), 0) && near(supply_angle_default(""), 0));
    CHECK(packaging_feeder_kind("Tape") == 1 && packaging_feeder_kind("Stick") == 2 && packaging_feeder_kind("Tray") == 3 &&
          packaging_feeder_kind("MTC") == 3 && packaging_feeder_kind("Bulk") == 4 && packaging_feeder_kind("") == 0);
    // two part numbers sharing one package get their own supply angles
    PartNumber a; a.number = "PN-SOT23"; a.package = "SOT23"; a.packaging = "Stick";
    PartNumber b; b.number = "PN-B"; b.package = "SOT23"; b.packaging = "Tape";
    p.parts[a.number] = a; p.parts[b.number] = b;
    Component c2 = mk("U2", 20, 10, 180, Side::Top, "SOT23"); c2.number = "PN-B"; p.add_component(c2);
    MachineJob j; std::string err;
    CHECK(build_machine_job(p, 0, j, err));
    for (auto& q : j.placements) CHECK(near(q.rot, q.ref == "U1" ? 90 : 180));   // supply angle / tape_rot not added
    for (auto& jp : j.parts) {
        if (jp.part == "PN-SOT23") CHECK(jp.feeder_kind == 2 && near(jp.supply_angle, 270));
        if (jp.part == "PN-B") CHECK(jp.feeder_kind == 1 && near(jp.supply_angle, 0));
    }
    p.parts["PN-B"].supply_angle_set = true; p.parts["PN-B"].supply_angle = 90;
    CHECK(near(effective_supply_angle(p.parts["PN-B"]), 90));
    // project file round trip; unset stays unset (follows the packaging)
    std::string js = project_to_json(p).dump();
    json::Value v; CHECK(json::Value::parse(js, v, err));
    Project r; CHECK(project_from_json(v, r, err));
    CHECK(r.parts["PN-SOT23"].packaging == "Stick" && !r.parts["PN-SOT23"].supply_angle_set);
    CHECK(r.parts["PN-B"].supply_angle_set && near(r.parts["PN-B"].supply_angle, 90) && r.packages["SOT23"].tape_rot == 270);
    r.parts["PN-SOT23"].packaging = "Tray";
    CHECK(near(effective_supply_angle(r.parts["PN-SOT23"]), 180));
}

int main() {
    test_group_delta();
    test_fractional_roundtrip();
    test_bottom();
    test_legacy_flags();
    test_trace_validate();
    test_select_outliers();
    test_cpf_real();
    test_supply_and_tape();
    std::printf("passed %d, failed %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
