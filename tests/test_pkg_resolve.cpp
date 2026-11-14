// R16b: centroid -> package pipeline, wizard state persistence, tape orientation -> export angle.
#include "core/package_resolve.h"
#include "core/machine_job.h"
#include "core/project_io.h"
#include <cmath>
#include <cstdio>
using namespace pcbp;

static int fails = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++fails; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool near(double a, double b, double t = 1e-6) { return std::fabs(a - b) < t; }

static Component comp(const char* ref, const char* pkg, Vec2 p, double rot, Side s = Side::Top) {
    Component c; c.ref = ref; c.package = pkg; c.pos = p; c.rot = rot; c.rot_imported = rot; c.side = s; return c;
}

int main() {
    Project prj;
    prj.add_component(comp("R1", "0402", {10, 10}, 0));
    prj.add_component(comp("R2", "0402", {20, 10}, 90));
    prj.add_component(comp("U1", "SOT-23-5", {30, 10}, 180));
    prj.add_component(comp("Y1", "XTAL_3225", {40, 10}, 0));
    prj.add_component(comp("Q1", "SOT23", {50, 10}, 0));
    Component f = comp("FID1", "FID", {0, 0}, 0); f.feature = Feature::Fiducial; prj.add_component(f);

    // pipeline: every referenced package listed, placeholders unresolved
    auto refd = referenced_packages(prj);
    CHECK(refd.size() == 4);
    CHECK(ensure_package_entries(prj) == 4);
    CHECK(ensure_package_entries(prj) == 0);
    CHECK(prj.packages.count("0402") && !package_resolved(prj.packages["0402"]));
    CHECK(unresolved_packages(prj).size() == 4);
    CHECK(!placement_package_resolved(prj, prj.comps[0]));
    CHECK(placement_package_resolved(prj, prj.comps[5]));   // fiducial

    // library exact match (case-insensitive) + family suggestions
    std::map<std::string, Package> lib;
    Package lp; lp.name = "sot23"; Pad a; a.pos = {-1, 0}; a.pin = 1; lp.pads = {a, a, a}; lp.pads[1].pos = {1, 0}; lp.pads[2].pos = {0, 1};
    lib["sot23"] = lp;
    CHECK(library_exact_match(lib, "SOT23") != nullptr);
    CHECK(library_exact_match(lib, "SOT-23") == nullptr);
    auto s1 = suggest_packages("SOT23", &lib);
    CHECK(!s1.empty() && s1[0].kind == PkgSuggestion::Library && s1[0].score == 100);
    auto s2 = suggest_packages("0402", nullptr);
    CHECK(!s2.empty() && s2[0].tpl == "chip" && s2[0].preset == "0402 / 1005M");
    auto s3 = suggest_packages("SOT-23-5", nullptr);
    CHECK(!s3.empty() && s3[0].tpl == "sot23" && s3[0].preset == "SOT-23-5");
    auto s4 = suggest_packages("XTAL_3225", nullptr);
    CHECK(!s4.empty() && s4[0].tpl == "xtal4" && s4[0].preset == "3225 (3.2x2.5)");
    auto s5 = suggest_packages("LQFP-64", nullptr);
    CHECK(!s5.empty() && s5[0].tpl == "qfp" && s5[0].params.count("n") && s5[0].params.at("n") == 16);
    CHECK(prj.packages["0402"].pads.empty());   // suggestions never change the project

    // assignment applies to every placement using the package
    CHECK(assign_package_geometry(prj, "SOT23", lp, "library") == 1);
    CHECK(package_resolved(prj.packages["SOT23"]) && prj.packages["SOT23"].name == "SOT23" && prj.packages["SOT23"].resolved_by == "library");
    CHECK(prj.packages["SOT23"].polarity_set);   // polarity default applied (3 leads -> marker)
    CHECK(prj.packages["SOT23"].marker_kind != 0);

    // build from board pads: R2 rotated 90 -> de-rotated horizontal chip
    {
        const Component& r2 = prj.comps[1];
        Pad p1; p1.pos = {20, 9.5}; p1.w = 0.6; p1.h = 0.5;   // board: pads stacked along Y
        Pad p2 = p1; p2.pos = {20, 10.5};
        Package pk = package_from_board_pads({p2, p1}, r2);
        CHECK(pk.pads.size() == 2);
        CHECK(near(pk.pads[0].pos.x, -0.5) && near(pk.pads[0].pos.y, 0) && pk.pads[0].pin == 1);
        CHECK(near(pk.pads[1].pos.x, 0.5) && pk.pads[1].pin == 2);
        CHECK(near(pk.pads[0].w, 0.5) && near(pk.pads[0].h, 0.6));
        // round trip: the package placed by comp_xform lands on the board pads
        prj.packages["0402"] = Package{};
        assign_package_geometry(prj, "0402", pk, "gerber");
        Xform t = prj.comp_xform(r2);
        CHECK(near(t.apply(prj.packages["0402"].pads[0].pos).y, 10.5, 1e-9) || near(t.apply(prj.packages["0402"].pads[0].pos).y, 9.5, 1e-9));
    }
    // bottom side: mirrored
    {
        Component b = comp("B1", "X", {0, 0}, 0, Side::Bottom);
        Pad p1; p1.pos = {1, 0}; Pad p2; p2.pos = {-1, 0};
        Package pk = package_from_board_pads({p1, p2}, b);
        Project q; q.add_component(b);
        Xform t = q.comp_xform(q.comps[0]);
        CHECK(near(t.apply(pk.pads[0].pos).x, 1) || near(t.apply(pk.pads[0].pos).x, -1));
        CHECK(near(pk.pads[0].pos.x, -1) && near(t.apply(pk.pads[0].pos).x, 1));   // pin 1 left in package coords
    }

    // wizard queue / skip / done + persistence
    {
        auto q = wizard_queue(prj);
        CHECK(q.size() == 2);   // SOT-23-5, XTAL_3225
        std::string cur = wizard_resume(prj);
        CHECK(cur == "SOT-23-5");
        wizard_skip(prj, cur);
        CHECK(prj.pkg_wizard.current == "XTAL_3225");
        CHECK(wizard_queue(prj).back() == "SOT-23-5");
        prj.pkg_wizard.done = {"0402"};
        WizardProgress w = wizard_progress(prj);
        CHECK(w.remaining == 2 && w.skipped == 1 && w.done == 1);
        CHECK(wizard_placement(prj, "0402", 1) == 1 && wizard_placement(prj, "0402", 2) == 0);
        prj.packages["XTAL_3225"].tape_rot = 270;
        prj.plot_aligned = true;
        std::string js = project_to_json(prj).dump(), err;
        json::Value v; CHECK(json::Value::parse(js, v, err));
        Project r;
        CHECK(project_from_json(v, r, err));
        CHECK(r.pkg_wizard.current == "XTAL_3225" && r.pkg_wizard.skipped.size() == 1 && r.pkg_wizard.done.size() == 1 && r.pkg_wizard.started);
        CHECK(r.plot_aligned && r.packages["XTAL_3225"].tape_rot == 270 && r.packages["0402"].resolved_by == "gerber");
        CHECK(wizard_resume(r) == "XTAL_3225");
        assign_package_geometry(r, "XTAL_3225", lp, "gerber");
        CHECK(r.packages["XTAL_3225"].tape_rot == 270);   // tape orientation kept on re-assign
        wizard_mark_done(r, "XTAL_3225");
        CHECK(r.pkg_wizard.current == "SOT-23-5");
    }

    // R18a: tape orientation is IGNORED by the export angle (kept in the file, one-time notice on load)
    {
        MachineSetup ms;
        Component& u = prj.comps[2];   // rot 180
        prj.packages["SOT-23-5"].tape_rot = 90;
        CHECK(near(export_angle(prj, u, 0, ms), 180));
        CHECK(near(u.rot, 180));
        ms.orientation = 90;
        CHECK(near(export_angle(prj, u, 0, ms), 270));
        ms.orientation = 0; ms.flipped = true; ms.mirror_angle_when_flipped = true;
        CHECK(near(export_angle(prj, u, 0, ms), norm_deg(-180)));
        ms.mirror_angle_when_flipped = false;
        CHECK(near(export_angle(prj, u, 0, ms), 180));
        prj.packages["SOT-23-5"].tape_rot = 270;
        CHECK(near(export_angle(prj, u, 0, MachineSetup{}), 180));
        CHECK(angle_convention_text(MachineSetup{}).find("tape") == std::string::npos);
        // migration notice: lists the affected packages once; acknowledged -> silent; persisted
        prj.tape_rot_notice_ack = false;
        const std::string n = tape_rot_notice(prj);
        CHECK(n.find("SOT-23-5") != std::string::npos && n.find("no longer applied") != std::string::npos);
        CHECK(n.find("0402") == std::string::npos);
        prj.tape_rot_notice_ack = true;
        CHECK(tape_rot_notice(prj).empty());
        std::string js = project_to_json(prj).dump(), err;
        json::Value v; CHECK(json::Value::parse(js, v, err));
        Project r; CHECK(project_from_json(v, r, err));
        CHECK(r.tape_rot_notice_ack && r.packages["SOT-23-5"].tape_rot == 270 && tape_rot_notice(r).empty());
        r.tape_rot_notice_ack = false; CHECK(!tape_rot_notice(r).empty());
        for (auto& kv : r.packages) kv.second.tape_rot = 0;
        CHECK(tape_rot_notice(r).empty());   // nothing to report
    }
    std::printf("%d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
