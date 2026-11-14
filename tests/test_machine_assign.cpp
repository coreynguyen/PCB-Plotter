// test_machine_assign.cpp — R24b library-free machine component data (core/machine_assign.*).
// Synthetic packages only: identification, embedded size bands, house rules (nozzle remap, tray for QFP/BGA
// variants, 8 mm paper, supply angles, no MTC), overrides, project JSON round trip, H8H write/read of the
// confirmed fields (+78 class, +80 kind, +176 tape code, +178 pitch code).
#include "core/machine_assign.h"
#include "core/machine_job.h"
#include "core/project.h"
#include "core/project_io.h"
#include "io/juki_writer.h"
#include <cmath>
#include <cstdio>
#include <string>

using namespace pcbp;
using namespace pcbp::mach;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static Suggestion sug(const char* name, const char* desc = "", double bx = 0, double by = 0, const char* pk = "") {
    Input in; in.name = name; in.desc = desc; in.body_x = bx; in.body_y = by; in.pn_packaging = pk;
    return suggest(in);
}

static void test_identify() {
    CHECK(identify("C0603", "").fam == Fam::Chip && identify("C0603", "").token == "0603");
    CHECK(identify("CAPC1608X55N", "").token == "0603");                 // IPC-7351 metric
    CHECK(identify("RESC1005X40N", "").token == "0402");
    CHECK(identify("", "RES 10K 1% 0805 SMD").token == "0805");
    CHECK(identify("SOT-23-5", "").fam == Fam::Sot && identify("SOT-23-5", "").token == "SOT235");
    CHECK(identify("SOT223", "").fam == Fam::Sot);
    CHECK(identify("", "IC REG LDO SOT-223").token == "SOT223");
    CHECK(identify("SOIC-8", "").fam == Fam::Sop && identify("SOIC-8", "").pins == 8);
    CHECK(identify("", "IC TXRX 8SOIC").fam == Fam::Sop);
    CHECK(identify("TSSOP-20", "").fam == Fam::Sop && identify("TSSOP-20", "").pins == 20);
    CHECK(identify("QFN-32_5x5mm", "").fam == Fam::Qfn && identify("QFN-32_5x5mm", "").l == 5);
    CHECK(identify("SMA", "").fam == Fam::Sod);
    CHECK(identify("SOD-123", "").fam == Fam::Sod);
    CHECK(identify("", "CAP TANT 10UF 16V CASE B").fam == Fam::Tant);
    for (const char* q : {"LQFP-64", "TQFP44", "PQFP-208", "QFP100", "VQFP-100"}) CHECK(identify(q, "").fam == Fam::Qfp);
    for (const char* b : {"BGA256", "FBGA-96", "LFBGA-144", "TFBGA64", "uBGA-48", "CSP-BGA-36", "WLCSP-9"}) CHECK(identify(b, "").fam == Fam::Bga);
    CHECK(identify("LQFP-64", "").l == 10);                               // MS-026 body by pin count
    CHECK(identify("FOO123", "").fam == Fam::Unknown);
}

static void test_rules() {
    // nozzle remap: never 501 / 503 / others
    CHECK(remap_nozzle(501) == 500 && remap_nozzle(503) == 504 && remap_nozzle(502) == 504);
    CHECK(remap_nozzle(510) == 508 && remap_nozzle(500) == 500 && remap_nozzle(507) == 507);
    for (int n = 400; n < 600; ++n) { int r = remap_nozzle(n); CHECK(r == 0 || nozzle_allowed(r)); }
    // tray for QFP / BGA variants, tape for everything else (also when the BOM said MTC)
    for (const char* q : {"LQFP-64", "TQFP44", "PQFP-208", "BGA256", "FBGA-96", "LFBGA-144", "TFBGA64", "uBGA-48", "CSP-BGA-36"}) {
        Suggestion s = sug(q);
        CHECK(s.f.packaging == "Tray" && s.f.supply_angle == 180 && s.f.tape_w == 0);
    }
    CHECK(sug("BGA256", "", 0, 0, "MTC").f.packaging == "Tray");
    CHECK(sug("C0603", "", 0, 0, "MTC").f.packaging == "Tray");          // explicit MTC -> Tray (never MTC)
    for (const char* q : {"C0402", "SOT-23", "SOIC-8", "QFN-32_5x5", "SMA", "SOT223"}) {
        Suggestion s = sug(q);
        CHECK(s.f.packaging == "Tape" && s.f.supply_angle == 0);
    }
    CHECK(sug("SOIC-16", "", 0, 0, "Stick").f.packaging == "Stick" && sug("SOIC-16", "", 0, 0, "Stick").f.supply_angle == 270);
    // 8 mm is paper
    Suggestion c = sug("C0603");
    CHECK(c.f.tape_w == 8 && c.f.carrier == "Paper" && c.f.pitch == 4 && c.f.nozzle == 500 && c.f.type_code == 1);
    Suggestion c2 = sug("C0402");
    CHECK(c2.f.tape_w == 8 && c2.f.pitch == 2);                          // EIA-481: 0402 on 2 mm pockets
    CHECK(sug("SOT-23").f.tape_w == 8 && sug("SOT-23").f.carrier == "Paper" && sug("SOT-23").f.type_code == 11);
    Suggestion so = sug("SOIC-8");
    CHECK(so.f.tape_w == 12 && so.f.carrier == "Embossed" && so.f.pitch == 8 && so.f.type_code == 101);
    // CAD body beats nominal; big bodies -> big nozzles; every nozzle allowed
    Suggestion big = sug("QFN-64", "", 9, 9);
    CHECK(big.dims_src == "cad" && big.f.nozzle >= 506 && big.f.tape_w >= 16 && big.f.type_code == 120);
    for (double s = 0.2; s < 40; s *= 1.3) { Suggestion q = sug("", "", s, s * 1.4); CHECK(nozzle_allowed(q.f.nozzle)); }
    // confidence: CAD-consistent > text only > unknown
    CHECK(sug("C0603", "", 1.6, 0.8).conf > sug("C0603").conf);
    CHECK(sug("C0603").conf > sug("FOO", "", 0, 0).conf);
    CHECK(!sug("C0603").rule.empty());
    // overrides win, 8 mm stays paper
    Fields o; o.nozzle = 506; o.packaging = "Tape"; o.tape_w = 8; o.carrier = "Embossed";
    Fields m = merge(c.f, o);
    CHECK(m.nozzle == 506 && m.tape_w == 8 && m.carrier == "Paper");
    Fields o2; o2.nozzle = 503; CHECK(merge(c.f, o2).nozzle == 504);
    Fields o3; o3.packaging = "Tray"; Fields m3 = merge(c.f, o3); CHECK(m3.supply_angle == 180 && m3.tape_w == 0);
    Fields o4; o4.packaging = "MTC"; CHECK(merge(c.f, o4).packaging == "Tray");
}

static void test_codes() {
    CHECK(h8h_tape_code(8, "Paper") == 1 && h8h_tape_code(8, "Embossed") == 0x102 && h8h_tape_code(12, "Embossed") == 0x103);
    CHECK(h8h_tape_code(24, "") == 0x105 && h8h_tape_code(32, "Paper") == 6);
    CHECK(h8h_pitch_code(4) == 4 && h8h_pitch_code(8) == 0x108 && h8h_pitch_code(12) == 0x10C && h8h_pitch_code(2) == 2);
    for (int w : {8, 12, 16, 24, 44, 56}) { int ww; std::string ca; h8h_decode_tape(h8h_tape_code(w, w == 8 ? "Paper" : "Embossed"), ww, ca); CHECK(ww == w); }
    for (int p : {2, 4, 8, 12, 16, 20, 24}) CHECK(h8h_decode_pitch(h8h_pitch_code(p)) == p);
    CHECK(h8h_decode_pitch(0x2CC) == 12);
    Tables t; std::string err;
    CHECK(parse_tables(format_tables(default_tables()), t, err) && t.steps.size() == default_tables().steps.size());
    CHECK(!default_tables().steps.empty() && default_tables().steps.count("noz.ALL"));
}

static Package rect_pkg(const char* name, double bx, double by) {
    Package p; p.name = name;
    p.pads = {Pad{{-bx * 0.6, 0}, 0.4, 0.5}, Pad{{bx * 0.6, 0}, 0.4, 0.5}}; p.pads[0].pin = 1; p.pads[1].pin = 2;
    GPrim g; g.kind = GKind::Rect; g.pts = {{-bx / 2, -by / 2}, {bx / 2, by / 2}}; p.body.push_back(g); p.body_source = "cad";
    return p;
}

static void test_project_and_writer() {
    Project p;
    p.packages["C0603"] = rect_pkg("C0603", 1.6, 0.8);
    p.packages["SOIC-8"] = rect_pkg("SOIC-8", 4.9, 3.9);
    p.packages["LQFP-64"] = rect_pkg("LQFP-64", 10, 10);
    p.packages["SOIC-8"].mach.nozzle = 506;      // user override
    p.packages["SOIC-8"].mach_assigned = true;
    p.model.points.clear();
    ModelPoint mp; mp.name = "SMT"; mp.has_machine = true; mp.machine.exporter_id = "juki.h8h"; p.model.points.push_back(mp);
    auto add = [&](const char* ref, const char* pkg, const char* pn, double x) {
        Component c; c.ref = ref; c.pos = {x, 10}; c.cad_pos = c.pos; c.package = pkg; c.number = pn; c.assigned_point = 0;
        p.add_component(c);
        PartNumber n; n.number = pn; n.package = pkg; p.parts[pn] = n;
    };
    add("C1", "C0603", "PN-C", 10); add("U1", "SOIC-8", "PN-U", 20); add("U2", "LQFP-64", "PN-Q", 40);
    Component f; f.ref = "FID1"; f.pos = {2, 2}; f.feature = Feature::Fiducial; f.assigned_point = -1; p.add_component(f);
    // JSON round trip of overrides
    {
        json::Value js = project_to_json(p);
        Project q; std::string err;
        CHECK(project_from_json(js, q, err));
        CHECK(q.packages["SOIC-8"].mach.nozzle == 506 && q.packages["SOIC-8"].mach_assigned && q.packages["C0603"].mach.empty());
    }
    MachineJob j; std::string err;
    CHECK(build_machine_job(p, 0, j, err));
    const JobPart* jc = nullptr; const JobPart* ju = nullptr; const JobPart* jq = nullptr;
    for (auto& x : j.parts) { if (x.part == "PN-C") jc = &x; if (x.part == "PN-U") ju = &x; if (x.part == "PN-Q") jq = &x; }
    CHECK(jc && ju && jq);
    if (!jc || !ju || !jq) return;
    CHECK(jc->feeder_kind == 1 && jc->tape_code == 1 && jc->pitch_code == 4 && jc->type_code == 1 && jc->nozzle == 500);
    CHECK(ju->nozzle == 506 && ju->tape_code == 0x103 && ju->pitch_code == 0x108 && ju->type_code == 101);
    CHECK(jq->feeder_kind == 3 && jq->supply_angle == 180 && jq->tape_code == 0 && jq->type_code == 102);
    std::vector<uint8_t> buf;
    CHECK(juki::write_hlc_variant(j, juki::HlcVariant::H8H, buf, err));
    juki::HlcParsed hp;
    CHECK(juki::parse_hlc(buf, hp, err));
    int seen = 0;
    for (size_t i = 0; i < hp.components.size(); ++i) {
        const std::string& n = hp.components[i];
        if (n == "PN-C") { ++seen; CHECK(hp.comp_kind[i] == 1 && hp.comp_tape[i] == 1 && hp.comp_pitch[i] == 4 && hp.comp_type[i] == 1 && hp.comp_supply[i] == 0); }
        if (n == "PN-U") { ++seen; CHECK(hp.comp_kind[i] == 1 && hp.comp_tape[i] == 0x103 && hp.comp_pitch[i] == 0x108 && hp.comp_type[i] == 101); }
        if (n == "PN-Q") { ++seen; CHECK(hp.comp_kind[i] == 3 && hp.comp_type[i] == 102 && std::fabs(hp.comp_supply[i] - 180) < 1e-9); }
    }
    CHECK(seen == 3);
    // export never blocks on machine data: low confidence is a warning only
    std::vector<ExportIssue> iss; juki::check_job(j, iss);
    for (auto& e : iss) if (e.msg.find("machine data") != std::string::npos) CHECK(e.sev != ExportIssue::Error);
}

int main() {
    test_identify();
    test_rules();
    test_codes();
    test_project_and_writer();
    std::printf("test_machine_assign: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
