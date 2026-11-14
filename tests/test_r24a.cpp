// ============================================================================
// test_r24a.cpp — Round 24a (user v23 report), synthetic data only.
//  #1 fiducial removal: every model-level delete / un-fiducial path, followed by everything that reads fiducials
//     afterwards (machine job, mark selection, export validation, placement review), must terminate. A watchdog
//     alarm turns a hang into a failure.
//  #2 export dims: the component record carries the package BODY (not the lead extent), the CAD height
//     (ODB++ .comp_height attribute on CMP and PKG, IPC-2581 Package@height + Package/Outline body), and nothing
//     is fabricated when the data has no height.
// ============================================================================
#include "../src/core/machine_job.h"
#include "../src/core/placement_check.h"
#include "../src/core/project.h"
#include "../src/core/project_io.h"
#include "../src/io/cad_common.h"
#include "../src/io/ipc2581.h"
#include "../src/io/juki_writer.h"
#include "../src/io/odbpp.h"

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace pcbp;
static int g_pass = 0, g_fail = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool near(double a, double b, double t = 0.02) { return std::fabs(a - b) <= t; }

static void wfile(const std::string& path, const std::string& text) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream(path, std::ios::binary) << text;
}
static ModelPoint layer(const char* name, Side s) {
    ModelPoint mp; mp.name = name; mp.process_type = "SMT"; mp.has_machine = true; mp.machine.exporter_id = "juki.h8h";
    mp.side = mp.machine.side = s;
    return mp;
}

// ---------------------------------------------------------------- #1 fiducial removal
static Project fid_project() {
    Project p;
    p.model.points.clear();
    p.model.points.push_back(layer("Top SMT", Side::Top));
    Package pk; pk.name = "0603"; pk.pads = {Pad{{-0.8, 0}, 0.8, 0.9}, Pad{{0.8, 0}, 0.8, 0.9}}; p.packages["0603"] = pk;
    Package fp; fp.name = "PAD_R1.52"; fp.pads = {Pad{{0, 0}, 1.52, 1.52}}; p.packages[fp.name] = fp;
    PartNumber pn; pn.number = "PN1"; pn.package = "0603"; p.parts["PN1"] = pn;
    for (int i = 0; i < 6; ++i) {
        Component c; c.ref = "R" + std::to_string(i + 1); c.pos = {10.0 + 5 * i, 10}; c.package = "0603"; c.number = "PN1"; c.assigned_point = 0;
        p.add_component(c);
    }
    // fiducials as a real CAD import produces them: EMPTY references, numbers 4,1,2,3 (one duplicate-free extra)
    const double fx[4] = {3.365, 126.555, 3.365, 139.255}, fy[4] = {97.872, 97.872, -7.538, -7.538};
    const int fn[4] = {4, 1, 2, 3};
    for (int i = 0; i < 4; ++i) {
        Component c; c.pos = {fx[i], fy[i]}; c.feature = Feature::Fiducial; c.fid_number = fn[i]; c.package = "PAD_R1.52";
        p.add_component(c);
    }
    // a component fiducial owned by R1 and a circuit fiducial
    Component lf; lf.ref = "LF1"; lf.pos = {12, 12}; lf.feature = Feature::Fiducial; lf.fid_scope = FiducialScope::Local;
    lf.local_fid_owner = (int)p.comps[0].id; lf.package = "PAD_R1.52"; p.add_component(lf);
    Component cf; cf.ref = "CF1"; cf.pos = {1, 1}; cf.feature = Feature::Fiducial; cf.fid_scope = FiducialScope::Image; cf.fid_number = 1;
    cf.package = "PAD_R1.52"; p.add_component(cf);
    return p;
}
static void consumers(const Project& p) {   // everything the UI recomputes after a geometry change
    MachineJob j; std::string err;
    if (build_machine_job(p, 0, j, err)) {
        j.machine = "juki.h8h";
        (void)juki::select_marks(j);
        std::vector<uint8_t> buf; juki::HlcWriteInfo wi;
        (void)juki::write_hlc_variant(j, juki::HlcVariant::H8H, buf, err, &wi);
        j.options["juki.global_marks"] = "FID1,FID2";   // explicit list naming deleted / renamed marks
        (void)juki::select_marks(j);
        j.options["juki.fid_mode"] = "circuit";
        (void)juki::select_marks(j);
    }
    (void)validate_for_export(p, 0);
    (void)check_placements(p);
    (void)project_to_json(p);
}
static void test_fiducial_removal() {
    std::printf("-- #1 fiducial removal paths\n");
    alarm(60);   // a hang anywhere below kills the test (SIGALRM) instead of passing silently
    const Project base = fid_project();
    consumers(base);
    // (a) Plots Delete key / context Delete Placement / Gerber Delete Components: erase the selected locations
    for (size_t k = 0; k < base.comps.size(); ++k) {
        if (base.comps[k].feature != Feature::Fiducial) continue;
        Project p = base;
        p.comps[k].selected = true;
        auto& v = p.comps;
        v.erase(std::remove_if(v.begin(), v.end(), [](const Component& c) { return c.selected; }), v.end());
        CHECK(p.comps.size() == base.comps.size() - 1);
        consumers(p);
    }
    // (b) all fiducials at once (multi-select delete)
    {
        Project p = base;
        auto& v = p.comps;
        v.erase(std::remove_if(v.begin(), v.end(), [](const Component& c) { return c.feature == Feature::Fiducial; }), v.end());
        consumers(p);
        MachineJob j; std::string err;
        CHECK(build_machine_job(p, 0, j, err) && j.fiducials.empty());
    }
    // (c) Remove Fiducial (context menu / Fiducial checkbox / Data table type cell): back to a component
    for (size_t k = 0; k < base.comps.size(); ++k) {
        if (base.comps[k].feature != Feature::Fiducial) continue;
        Project p = base;
        p.comps[k].feature = Feature::Component;
        consumers(p);
    }
    // (d) the owner of a component fiducial is deleted (dangling local_fid_owner)
    {
        Project p = base;
        p.comps.erase(p.comps.begin());
        consumers(p);
    }
    alarm(0);
    CHECK(true);
}

// ---------------------------------------------------------------- #2 body dims + height
static const char* kSot = "SOT23";
static void sot_package(Package& p, bool with_body) {
    // SOT-23: body 2.9 x 1.3; pads 0.6 x 0.7 at y = +-1.1 -> lead extent ~ 2.5 x 2.9 (legs included)
    p.name = kSot;
    p.pads = {Pad{{-0.95, -1.1}, 0.6, 0.7}, Pad{{0.95, -1.1}, 0.6, 0.7}, Pad{{0, 1.1}, 0.6, 0.7}};
    if (with_body) { GPrim r; r.kind = GKind::Rect; r.pts = {{-1.45, -0.65}, {1.45, 0.65}}; p.body = {r}; p.body_source = "cad"; }
    else p.auto_body();   // "pads": not a body
}
static bool write_and_parse(const Project& p, juki::HlcParsed& hp) {
    MachineJob j; std::string err;
    if (!build_machine_job(p, 0, j, err)) { std::printf("   job: %s\n", err.c_str()); return false; }
    j.machine = "juki.h8h";
    std::vector<uint8_t> buf; juki::HlcWriteInfo wi;
    if (!juki::write_hlc_variant(j, juki::HlcVariant::H8H, buf, err, &wi)) { std::printf("   write: %s\n", err.c_str()); return false; }
    return juki::parse_hlc(buf, hp, err) && !hp.comp_w.empty();
}
static void test_body_dims() {
    std::printf("-- #2 body dims / height in the component record\n");
    for (int body = 0; body < 2; ++body) {
        Project p;
        p.model.points.clear();
        p.model.points.push_back(layer("Top SMT", Side::Top));
        Package pk; sot_package(pk, body == 1); p.packages[kSot] = pk;
        PartNumber pn; pn.number = "Q"; pn.package = kSot; p.parts["Q"] = pn;
        Component c; c.ref = "Q1"; c.pos = {10, 10}; c.package = kSot; c.number = "Q"; c.assigned_point = 0; p.add_component(c);
        Component f; f.ref = "F1"; f.pos = {1, 1}; f.feature = Feature::Fiducial; f.fid_number = 1; p.add_component(f);
        MachineJob j; std::string err;
        CHECK(build_machine_job(p, 0, j, err) && j.parts.size() == 1);
        if (j.parts.empty()) continue;
        if (body) {   // the BODY: 2.9 x 1.3, not the 2.5 x 2.9 lead extent
            CHECK(near(j.parts[0].length, 2.9) && near(j.parts[0].width, 1.3) && j.parts[0].size_src == "body");
        } else {      // no body outline
            CHECK(near(j.parts[0].length, 2.5) && near(j.parts[0].width, 2.9) && j.parts[0].size_src == "pads");
        }
        CHECK(j.parts[0].height == 0 && j.parts[0].height_src.empty());   // nothing invented
        juki::HlcParsed hp;
        CHECK(write_and_parse(p, hp));
        if (!hp.comp_h.empty()) CHECK(hp.comp_h[0] == 0);
        // height from the package (CAD / package editor) -> +89, 1e-4 mm
        p.packages[kSot].height = 1.1; p.packages[kSot].height_src = "cad";
        CHECK(write_and_parse(p, hp));
        if (!hp.comp_h.empty()) CHECK(hp.comp_h[0] == 11000);
        if (body && !hp.comp_w.empty()) CHECK(hp.comp_w[0] == 29000 && hp.comp_l[0] == 13000);
        // part-number height (BOM) wins
        p.parts["Q"].height = 1.0;
        CHECK(write_and_parse(p, hp));
        if (!hp.comp_h.empty()) CHECK(hp.comp_h[0] == 10000);
        // provenance survives save / load
        Project q; std::string e2;
        CHECK(project_from_json(project_to_json(p), q, e2) && q.packages[kSot].height_src == "cad");
    }
    // the export review says so when the size had to come from the leads
    {
        Project p;
        p.model.points.clear();
        p.model.points.push_back(layer("Top SMT", Side::Top));
        Package pk; sot_package(pk, false); p.packages[kSot] = pk;
        Component c; c.ref = "Q1"; c.pos = {10, 10}; c.package = kSot; c.number = "Q"; c.assigned_point = 0; p.add_component(c);
        Component f; f.ref = "F1"; f.pos = {1, 1}; f.feature = Feature::Fiducial; f.fid_number = 1; p.add_component(f);
        bool legs = false, noh = false;
        for (auto& i : validate_for_export(p, 0)) {
            legs |= i.msg.find("no body outline") != std::string::npos;
            noh |= i.msg.find("have no height") != std::string::npos;
        }
        CHECK(legs && noh);
    }
}

// ODB++: .comp_height system attribute on CMP (inch file) and on the eda PKG record (mm file); body outline records
static void test_odb_height() {
    std::printf("-- #2 ODB++ .comp_height + body outline\n");
    for (int mm = 0; mm < 2; ++mm) {
        const double s = mm ? 1.0 : 1.0 / 25.4;
        auto n = [&](double v) { char b[32]; std::snprintf(b, sizeof b, "%.6f", v * s); return std::string(b); };
        const std::string U = mm ? "UNITS=MM\n" : "UNITS=INCH\n";
        const std::string dir = std::string("build/tests/odb_r24a/") + (mm ? "mm" : "inch");
        std::filesystem::remove_all(dir);
        wfile(dir + "/matrix/matrix", "STEP {\n COL=1\n NAME=PCB\n}\nLAYER {\n ROW=1\n CONTEXT=BOARD\n TYPE=COMPONENT\n NAME=COMP_+_TOP\n POLARITY=POSITIVE\n}\n");
        wfile(dir + "/misc/info", "JOB_NAME=r24a\nODB_VERSION_MAJOR=7\n" + U);
        wfile(dir + "/steps/pcb/profile", U + "S P 0\nOB 0 0 I\nOS " + n(40) + " 0\nOS " + n(40) + " " + n(30) + "\nOS 0 " + n(30) + "\nOE\nSE\n");
        std::string e = U + "@0 .comp_height\n";
        // SOT23: PKG extent includes the pins; CT outline = the body 2.9 x 1.3
        e += "PKG SOT23 " + n(0.95) + " " + n(-1.45) + " " + n(-1.45) + " " + n(1.45) + " " + n(1.45) + (mm ? ";0=1.1" : "") + "\n";
        e += "CT\nOB " + n(-1.45) + " " + n(-0.65) + " I\nOS " + n(1.45) + " " + n(-0.65) + "\nOS " + n(1.45) + " " + n(0.65) + "\nOS " + n(-1.45) + " " + n(0.65) + "\nOE\nCE\n";
        e += "PIN 1 S " + n(-0.95) + " " + n(-1.1) + " 0 E S\nRC " + n(-1.25) + " " + n(-1.45) + " " + n(0.6) + " " + n(0.7) + "\n";
        e += "PIN 2 S " + n(0.95) + " " + n(-1.1) + " 0 E S\nRC " + n(0.65) + " " + n(-1.45) + " " + n(0.6) + " " + n(0.7) + "\n";
        e += "PIN 3 S 0 " + n(1.1) + " 0 E S\nRC " + n(-0.3) + " " + n(0.75) + " " + n(0.6) + " " + n(0.7) + "\n";
        wfile(dir + "/steps/pcb/eda/data", e);
        std::string c = U + "@0 .comp_height\n@1 .no_pop\n";
        c += "CMP 0 " + n(10) + " " + n(10) + " 0 N Q1 BC847;1," + (mm ? "" : "0=" + n(1.0)) + "\n";
        if (mm) c.back() = '\n';
        wfile(dir + "/steps/pcb/layers/comp_+_top/components", c);
        Project p; io::CadImportOptions o; io::CadImportResult r; std::string err;
        CHECK(io::import_odbpp(dir, p, o, r, err));
        CHECK(p.packages.count("SOT23") == 1);
        if (!p.packages.count("SOT23")) continue;
        const Package& pk = p.packages["SOT23"];
        std::string src;
        BBox b = pkg_body_box(pk, &src);
        CHECK(src == "body" && near(b.w(), 2.9, 0.01) && near(b.h(), 1.3, 0.01));
        if (mm) CHECK(near(pk.height, 1.1, 1e-6) && pk.height_src == "cad");
        else { CHECK(pk.height == 0); CHECK(p.parts.count("BC847") && near(p.parts["BC847"].height, 1.0, 1e-4)); }
    }
}

// IPC-2581: Package@height and Package/Outline/Polygon as the body
static void test_ipc_height() {
    std::printf("-- #2 IPC-2581 Package height / Outline\n");
    const std::string f = "build/tests/odb_r24a/ipc.xml";
    wfile(f, R"(<?xml version="1.0"?>
<IPC-2581 revision="B" xmlns="http://webstds.ipc.org/2581">
 <Content><DictionaryStandard units="MILLIMETER"><EntryStandard id="RECT1"><RectCenter width="0.6" height="0.7"/></EntryStandard></DictionaryStandard></Content>
 <Ecad><CadHeader units="MILLIMETER"/><CadData>
  <Layer name="TOP" layerFunction="SIGNAL" side="TOP"/>
  <Step name="pcb">
   <Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="40" y="0"/><PolyStepSegment x="40" y="30"/><PolyStepSegment x="0" y="30"/><PolyStepSegment x="0" y="0"/></Polygon></Profile>
   <Package name="SOT23" type="SOT23" height="1.1">
    <Outline><Polygon><PolyBegin x="-1.45" y="-0.65"/><PolyStepSegment x="1.45" y="-0.65"/><PolyStepSegment x="1.45" y="0.65"/><PolyStepSegment x="-1.45" y="0.65"/><PolyStepSegment x="-1.45" y="-0.65"/></Polygon><LineDesc lineWidth="0.1"/></Outline>
    <Pin number="1" type="SURFACE"><Location x="-0.95" y="-1.1"/><StandardPrimitiveRef id="RECT1"/></Pin>
    <Pin number="2" type="SURFACE"><Location x="0.95" y="-1.1"/><StandardPrimitiveRef id="RECT1"/></Pin>
    <Pin number="3" type="SURFACE"><Location x="0" y="1.1"/><StandardPrimitiveRef id="RECT1"/></Pin>
   </Package>
   <Component refDes="Q1" packageRef="SOT23" part="BC847" layerRef="TOP"><Xform rotation="0"/><Location x="10" y="10"/></Component>
  </Step>
 </CadData></Ecad>
</IPC-2581>)");
    Project p; io::CadImportOptions o; io::CadImportResult r; std::string err;
    const bool ok = io::import_ipc2581_file(f, p, o, r, err);
    CHECK(ok);
    if (!ok) { std::printf("   %s\n", err.c_str()); return; }
    CHECK(p.packages.count("SOT23") == 1);
    if (!p.packages.count("SOT23")) return;
    const Package& pk = p.packages["SOT23"];
    std::string src;
    BBox b = pkg_body_box(pk, &src);
    CHECK(src == "body" && near(b.w(), 2.9, 0.01) && near(b.h(), 1.3, 0.01));
    CHECK(near(pk.height, 1.1, 1e-6) && pk.height_src == "cad");
}

// ---------------------------------------------------------------- linked-layer (pass-through) fiducials
static void test_linked_fids() {
    std::printf("-- linked-layer fiducials\n");
    auto mk = [](bool link, bool bottom_own, bool top_fids) {
        Project p;
        p.model.points.clear();
        ModelPoint t = layer("Top SMT", Side::Top), b = layer("Bottom SMT", Side::Bottom);
        b.machine.flipped = true;
        if (link) t.link = b.link = 1;
        p.model.points.push_back(t); p.model.points.push_back(b);
        Package pk; pk.name = "0603"; pk.pads = {Pad{{-0.8, 0}, 0.8, 0.9}, Pad{{0.8, 0}, 0.8, 0.9}}; p.packages["0603"] = pk;
        auto part = [&](const char* r, double x, double y, Side s, int pt) {
            Component c; c.ref = r; c.pos = {x, y}; c.package = "0603"; c.number = "PN"; c.side = s; c.assigned_point = pt; p.add_component(c);
        };
        part("R1", 10, 10, Side::Top, 0);
        part("R2", 30, 20, Side::Bottom, 1);
        part("RP", 3, 4, Side::Bottom, 1);   // a bottom placement exactly at the top mark's board position
        if (top_fids) {
            const double fx[3] = {3, 47, 3}, fy[3] = {4, 4, 36};
            for (int i = 0; i < 3; ++i) {
                Component f; f.ref = "FT" + std::to_string(i + 1); f.pos = {fx[i], fy[i]}; f.side = Side::Top; f.feature = Feature::Fiducial; f.fid_number = i + 1;
                p.add_component(f);
            }
        }
        if (bottom_own) { Component f; f.ref = "FB1"; f.pos = {45, 30}; f.side = Side::Bottom; f.feature = Feature::Fiducial; f.fid_number = 1; p.add_component(f); }
        return p;
    };
    auto has = [](const std::vector<ExportIssue>& v, const char* s, ExportIssue::Sev sev) {
        for (auto& i : v) if (i.sev == sev && i.msg.find(s) != std::string::npos) return true;
        return false;
    };
    {   // (a) a side with its own marks uses only its own
        Project p = mk(true, true, true);
        MachineJob j; std::string err;
        CHECK(build_machine_job(p, 1, j, err) && j.fiducials.size() == 1 && j.fiducials[0].name == "FB1" && j.marks_from.empty());
    }
    {   // (b) no own marks -> the linked top layer's marks, through the BOTTOM transform (== a bottom placement there)
        Project p = mk(true, false, true);
        MachineJob j; std::string err;
        CHECK(build_machine_job(p, 1, j, err) && j.fiducials.size() == 3 && j.marks_from == "Top SMT");
        const JobPlacement* rp = nullptr;
        for (auto& q : j.placements) if (q.ref == "RP") rp = &q;
        CHECK(rp && !j.fiducials.empty() && near(j.fiducials[0].x, rp->x, 1e-9) && near(j.fiducials[0].y, rp->y, 1e-9));
        MachineJob jt;
        CHECK(build_machine_job(p, 0, jt, err) && !jt.fiducials.empty());
        if (!jt.fiducials.empty() && !j.fiducials.empty())   // mirrored, not copied raw from the top program
            CHECK(!(near(jt.fiducials[0].x, j.fiducials[0].x, 1e-6) && near(jt.fiducials[1].x, j.fiducials[1].x, 1e-6)));
        auto v = validate_for_export(p, 1);
        CHECK(!has(v, "No fiducial marks", ExportIssue::Warning) && !has(v, "No fiducial marks", ExportIssue::Error));
        CHECK(has(v, "linked layer 'Top SMT'", ExportIssue::Info));
        std::vector<uint8_t> buf; juki::HlcWriteInfo wi; j.machine = "juki.h8h";
        CHECK(juki::write_hlc_variant(j, juki::HlcVariant::H8H, buf, err, &wi) && wi.marks == 3);
    }
    {   // (c) neither side has marks -> one warning, export proceeds
        Project p = mk(true, false, false);
        auto v = validate_for_export(p, 1);
        int n = 0; for (auto& i : v) n += i.msg.find("No fiducial marks") != std::string::npos;
        CHECK(n == 1 && has(v, "or on its linked layer", ExportIssue::Warning) && !has(v, "No fiducial marks", ExportIssue::Error));
        MachineJob j; std::string err; std::vector<uint8_t> buf; juki::HlcWriteInfo wi;
        CHECK(build_machine_job(p, 1, j, err));
        j.machine = "juki.h8h";
        CHECK(juki::write_hlc_variant(j, juki::HlcVariant::H8H, buf, err, &wi) && !buf.empty());
    }
    {   // (d) unlinked side without marks -> warning, never the other side's marks
        Project p = mk(false, false, true);
        MachineJob j; std::string err;
        CHECK(build_machine_job(p, 1, j, err) && j.fiducials.empty());
        CHECK(has(validate_for_export(p, 1), "no linked layer", ExportIssue::Warning));
    }
}

int main() {
    test_linked_fids();
    test_fiducial_removal();
    test_body_dims();
    test_odb_height();
    test_ipc_height();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
