// ============================================================================
// test_juki_import.cpp — JUKI program -> Project round trip.
//
// (a) The CircuitCAM demo (GenCAD + BOM + centroid) is exported with every
//     JUKI exporter, imported back with "juki.hlc", re-exported from the
//     imported project's "Top SMT" point and the placement records
//     (ref, x, y, angle, skip, component link, mark set) compared exactly.
//     Same for a 3-image panel (one rotated, one x-out) and for a program
//     whose circuits are not a consistent step & repeat.
// (b) No real machine programs are on disk (TTP tests reference
//     local program_dirs, unmapped here); any *.h8h / *.iss
//     found there is imported as a smoke test.
// (c) Synthetic ISS XML.
// ============================================================================
#include "local_data.h"
#include "../src/core/fileio.h"
#include "../src/core/machine_job.h"
#include "../src/io/juki_import.h"
#include "../src/io/juki_writer.h"
#include "../src/plugins/plugin.h"

#include <cmath>
#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <dirent.h>
#include <string>

using namespace pcbp;

static int g_fail = 0, g_pass = 0;
static bool near(double a, double b, double t = 1e-6) { return std::fabs(a - b) < t; }
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const std::string kRoot = pcbp_test::local("legacy_demo_root");   // legacy CAM demo install (local only)
static const std::string kBom = kRoot + "/Demonstration Bill of Materials/DemoBOM.prn";
static const std::string kRef = kRoot + "/Centroid File/Reference File.txt";
static const std::string kCad = kRoot + "/Demonstration CAD Data/CCDemo.cad";
static const std::string kTmp = "/tmp/claude-0/test_juki_import";

static bool imp(PluginRegistry& r, const char* id, const std::string& path, Project& p, const Options& extra = {}) {
    Importer* i = r.importer(id);
    if (!i) return false;
    Options o = default_options(i->info());
    for (auto& kv : extra) o[kv.first] = kv.second;
    ImportReport rep; std::string err;
    const bool ok = i->run(path, o, p, rep, err);
    if (!ok) std::printf("  import %s failed: %s\n", id, err.c_str());
    return ok;
}

// export the project's point like the app does: exporter defaults overlaid with setup options
static bool export_point(PluginRegistry& r, const Project& p, int pt, const std::string& id, std::vector<uint8_t>& out,
                         const Options& extra = {}) {
    MachineJob job; std::string err;
    if (!build_machine_job(p, pt, job, err)) { std::printf("  job: %s\n", err.c_str()); return false; }
    Exporter* e = r.exporter(id);
    if (!e) return false;
    Options o = default_options(e->info());
    for (auto& kv : p.model.points[(size_t)pt].machine.options) o[kv.first] = kv.second;
    for (auto& kv : extra) o[kv.first] = kv.second;
    if (!e->export_job(job, o, out, err)) { std::printf("  export %s: %s\n", id.c_str(), err.c_str()); return false; }
    return true;
}

static bool same_placements(const juki::HlcParsed& a, const juki::HlcParsed& b, std::string& why) {
    if (a.placements.size() != b.placements.size()) { why = "count " + std::to_string(a.placements.size()) + " vs " + std::to_string(b.placements.size()); return false; }
    for (size_t i = 0; i < a.placements.size(); ++i) {
        const auto& x = a.placements[i]; const auto& y = b.placements[i];
        if (x.ref != y.ref || x.x != y.x || x.y != y.y || x.angle != y.angle || x.flags != y.flags ||
            x.status != y.status || x.comp != y.comp || x.mark_set != y.mark_set) {
            char buf[256];
            std::snprintf(buf, sizeof buf, "#%zu %s(%d,%d,%d,f%X,s%X,c%u,m%u) vs %s(%d,%d,%d,f%X,s%X,c%u,m%u)", i, x.ref.c_str(), x.x, x.y,
                          x.angle, x.flags, x.status, x.comp, x.mark_set, y.ref.c_str(), y.x, y.y, y.angle, y.flags, y.status, y.comp, y.mark_set);
            why = buf; return false;
        }
    }
    if (a.components != b.components) { why = "component table differs"; return false; }
    if (a.names != b.names) { why = "names differ"; return false; }
    return true;
}

static void round_trip(PluginRegistry& r, const Project& src, const char* label, const Options& extra,
                       bool expect_bytes, int expect_images) {
    std::printf("== round trip: %s\n", label);
    for (auto& e : r.exporters()) {
        const std::string id = e->info().id;
        if (id.rfind("juki.", 0) != 0) continue;
        Project s = src;
        s.model.points[0].machine.exporter_id = id;
        std::vector<uint8_t> a;
        CHECK(export_point(r, s, 0, id, a, extra));
        const std::string path = kTmp + "/rt." + e->info().default_ext;
        std::string err;
        CHECK(write_file(path, a.data(), a.size(), err));
        CHECK(r.detect(path) == r.importer("juki.hlc"));
        Project q;
        ImportReport rep;
        Importer* ji = r.importer("juki.hlc");
        const bool ok = ji->run(path, default_options(ji->info()), q, rep, err);
        CHECK(ok);
        if (!ok) { std::printf("  %s import: %s\n", id.c_str(), err.c_str()); continue; }
        const int pt = q.point_index("Top SMT");
        CHECK(pt >= 0);
        CHECK(q.model.points[(size_t)pt].has_machine && q.model.points[(size_t)pt].machine.exporter_id == id);
        CHECK((int)q.images.size() == expect_images);
        std::vector<uint8_t> b;
        CHECK(export_point(r, q, pt, id, b));
        juki::HlcParsed pa, pb;
        CHECK(juki::parse_hlc(a, pa, err));
        CHECK(juki::parse_hlc(b, pb, err));
        std::string why;
        const bool same = same_placements(pa, pb, why);
        CHECK(same);
        const bool bytes = a == b;
        if (expect_bytes) CHECK(bytes);
        CHECK(pa.board_mode == pb.board_mode && pa.pcb_w == pb.pcb_w && pa.pcb_h == pb.pcb_h &&
              pa.layout_x == pb.layout_x && pa.layout_y == pb.layout_y && pa.circuits.size() == pb.circuits.size());
        CHECK(pa.mark_sets.size() == pb.mark_sets.size() && pa.local_marks.size() == pb.local_marks.size());
        std::printf("  %-9s %6zu B  %3zu pl  %2zu comp  %zu img  pkgs %zu  placements %s  bytes %s%s%s\n", id.c_str(), a.size(),
                    pa.placements.size(), pa.components.size(), q.images.size(), q.packages.size(), same ? "same" : "DIFF",
                    bytes ? "identical" : "differ", same ? "" : "  ", why.c_str());
        if (id == "juki.h8h") for (auto& m : rep.messages) std::printf("    %s\n", m.c_str());
    }
}

static void test_iss() {
    std::printf("== ISS\n");
    const std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<productionProgram version=\"1\">\n"
        " <pwbId>DEMO&amp;1</pwbId>\n <core>\n  <pwbData>\n   <pwbConfiguration><outline x=\"100\" y=\"80\"/></pwbConfiguration>\n"
        "   <circuitConfigurationData><circuitConfiguration index=\"0\"><circuitOutline x=\"45\" y=\"75\"/>\n"
        "     <matrix><divideNumber x=\"2\" y=\"1\"/><matrixPitch x=\"50\" y=\"0\"/><matrixReferencePosition x=\"2\" y=\"2\"/></matrix>\n"
        "   </circuitConfiguration></circuitConfigurationData>\n  </pwbData>\n"
        "  <component index=\"0\"><componentName>RES-10K</componentName><componentSize length=\"1.6\" witdh=\"0.8\" height=\"0.45\"/><comment>0603 10k</comment></component>\n"
        "  <component index=\"1\"><componentName>IC-555</componentName><componentSize length=\"5\" witdh=\"6\" height=\"1.7\"/></component>\n"
        "  <placement index=\"0\"><placementId>R1</placementId><componentName>RES-10K</componentName><placementPosition x=\"10.5\" y=\"20.25\"/><placementAngle angle=\"90\"/></placement>\n"
        "  <placement index=\"1\"><placementId>U1</placementId><componentName>IC-555</componentName><placementPosition x=\"30\" y=\"40\"/><placementAngle angle=\"270\"/><skip placement=\"YES\"/></placement>\n"
        "  <markPosition x=\"3\" y=\"3\"/><markPosition x=\"97\" y=\"77\"/><markPosition x=\"3\" y=\"3\"/>\n"
        " </core>\n</productionProgram>\n";
    std::vector<uint8_t> d(xml.begin(), xml.end());
    CHECK(juki::sniff_juki(d) >= 90);
    Project p; juki::ImportResult res; std::string err;
    CHECK(juki::import_juki(d, p, {}, res, err));
    if (!err.empty()) std::printf("  ISS error: %s\n", err.c_str());
    CHECK(res.format == "ISS" && res.placements == 2 && res.fiducials == 2 && res.parts == 2 && res.images == 2);
    const Component* r1 = p.find_ref("R1");
    const Component* u1 = p.find_ref("U1");
    CHECK(r1 && std::fabs(r1->pos.x - 10.5) < 1e-9 && std::fabs(r1->pos.y - 20.25) < 1e-9 && r1->rot == 90 && !r1->dnp);
    CHECK(u1 && u1->dnp && u1->rot == 270);
    CHECK(p.info.number == "DEMO&1");
    {   // v17: the imported program name (option juki.program_name) wins over Project Name / Board ID
        Project q = p; q.info.pcb_id = "PWB-999"; q.info.name = "Other";
        MachineJob job; std::string e2;
        CHECK(!q.model.points.empty() && build_machine_job(q, 0, job, e2) && job.program_name == "DEMO&1");
        for (auto& mp : q.model.points) mp.machine.options.erase("juki.program_name");
        CHECK(build_machine_job(q, 0, job, e2) && job.program_name == "Other");     // R18d: Project Name first
        q.info.name.clear();
        CHECK(build_machine_job(q, 0, job, e2) && job.program_name == "PWB-999");   // then Board ID
        q.info.pcb_id.clear();
        CHECK(build_machine_job(q, 0, job, e2) && job.program_name == "DEMO&1");   // legacy hidden Assembly No.
        q.info.number.clear();
        CHECK(build_machine_job(q, 0, job, e2) && job.program_name == "PROGRAM");
    }
    CHECK(p.images.size() == 2 && p.images[1].offset.x == 50);
    auto pk = p.packages.find(p.parts["RES-10K"].package);
    CHECK(pk != p.packages.end() && pk->second.pads.size() == 2 && std::fabs(pk->second.height - 0.45) < 1e-9);
    CHECK(p.parts["RES-10K"].description == "0603 10k");
    std::printf("  ISS: %d placements, %d fiducials, %d images, %d packages\n", res.placements, res.fiducials, res.images, res.packages);
}

static void test_samples(PluginRegistry& r) {
    std::printf("== real samples\n");
    int n = 0;
    for (const std::string& ds : pcbp_test::local_list("program_dirs")) {
        const char* dir = ds.c_str();
        DIR* d = opendir(dir);
        if (!d) continue;
        while (dirent* e = readdir(d)) {
            std::string f = e->d_name, ext = file_ext_lower(f);
            if (ext != ".h8h" && ext != ".h7h" && ext != ".h4h" && ext != ".h5h" && ext != ".iss") continue;
            Project p;
            const bool ok = imp(r, "juki.hlc", std::string(dir) + "/" + f, p);
            CHECK(ok);
            std::printf("  %s: %s, %zu comps\n", f.c_str(), ok ? "ok" : "FAILED", p.comps.size());
            ++n;
        }
        closedir(d);
    }
    if (!n) std::printf("  (no machine programs on disk; skipped)\n");
}

// JUKI-saved programs (user's Single / Matrix / Non-Matrix resaves of one program,
// tests/data/hlc/juki_resave, HLC_FORMAT.md §11): import -> export reproduces each
// byte for byte; re-exporting our own v11 file reproduces the JUKI Single resave.
static std::vector<uint8_t> slurp(const std::string& path) {
    std::vector<uint8_t> d; FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return d;
    std::fseek(f, 0, SEEK_END); d.resize((size_t)std::ftell(f)); std::fseek(f, 0, SEEK_SET);
    if (std::fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
    std::fclose(f);
    return d;
}
static size_t first_diff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) return i;
    return a.size() == b.size() ? (size_t)-1 : n;
}
static void test_juki_resaves(PluginRegistry& r) {
    std::printf("== JUKI resaves\n");
    const std::string dir = "tests/data/hlc/juki_resave/";
    for (const char* f : {"single.h8h", "matrix.h8h", "nonmatrix.h8h"}) {
        const auto src = slurp(dir + f);
        CHECK(!src.empty());
        if (src.empty()) continue;
        Project q;
        CHECK(imp(r, "juki.hlc", dir + f, q));
        const int pt = q.point_index("Top SMT");
        CHECK(pt >= 0);
        if (pt < 0) continue;
        std::vector<uint8_t> b;
        CHECK(export_point(r, q, pt, "juki.h8h", b));
        const size_t d = first_diff(src, b);
        CHECK(d == (size_t)-1);
        std::printf("  %-14s import->export %s", f, d == (size_t)-1 ? "byte-identical\n" : "differs at ");
        if (d != (size_t)-1) std::printf("%zu (%zu vs %zu B)\n", d, src.size(), b.size());
        // without the residual patch the writer alone must reproduce the file too
        // (only the line name is not in the project: the editor's line setting)
        q.model.points[(size_t)pt].machine.options.erase("juki.h8h.raw");
        std::vector<uint8_t> c;
        CHECK(export_point(r, q, pt, "juki.h8h", c, {{"juki.line_name", "Demo SMT Assembly"}}));
        const size_t dc = first_diff(src, c);
        CHECK(dc == (size_t)-1);
        if (dc != (size_t)-1) std::printf("    writer alone differs at %zu\n", dc);
        // the user's Matrix / Non-Matrix resaves repeat a full-size board 2x2: only
        // "outside the board" errors are expected there, nothing structural
        // R18d re-derived: the repeated CIRCUITS / their placements may leave the board; BOC (board) marks must not
        int other = 0, marks = 0;
        for (auto& c : juki::validate_hlc(b)) {
            if (!c.error) continue;
            if (c.msg.find("BOC mark") != std::string::npos) { ++marks; std::printf("    %s\n", c.msg.c_str()); }
            else if (c.msg.find("outside the board") == std::string::npos) { ++other; std::printf("    %s\n", c.msg.c_str()); }
            else std::printf("    (expected) %s\n", c.msg.c_str());
        }
        CHECK(other == 0);
        CHECK(marks == 0);
    }
    {   // the v11 export is flagged structurally (IS editor crash)
        bool flagged = false;
        for (auto& c : juki::validate_hlc(slurp(dir + "pcbp_v11.h8h")))
            flagged |= c.error && c.msg.find("JUKI editor always writes") != std::string::npos;
        CHECK(flagged);
    }
    // our v11 export, re-imported and re-exported = JUKI's Single resave except the
    // line name (the editor's line setting) and the edit timestamp, given here.
    const auto ref = slurp(dir + "single.h8h");
    Project q;
    CHECK(imp(r, "juki.hlc", dir + "pcbp_v11.h8h", q));
    const int pt = q.point_index("Top SMT");
    CHECK(pt >= 0);
    if (pt >= 0 && !ref.empty()) {
        auto& o = q.model.points[(size_t)pt].machine.options;
        o.erase("juki.h8h.raw");   // v11 bytes must not leak back (stale +38, +0 optimizer)
        std::vector<uint8_t> b;
        CHECK(export_point(r, q, pt, "juki.h8h", b,
                           {{"juki.line_name", "Demo SMT Assembly"}, {"juki.timestamp", std::to_string(
                               (uint32_t)ref[5760 + 34] | (uint32_t)ref[5760 + 35] << 8 | (uint32_t)ref[5760 + 36] << 16 |
                               (uint32_t)ref[5760 + 37] << 24)}}));
        const size_t d = first_diff(ref, b);
        CHECK(d == (size_t)-1);
        std::printf("  pcbp_v11 -> export vs JUKI single: %s", d == (size_t)-1 ? "byte-identical\n" : "differs at ");
        if (d != (size_t)-1) std::printf("%zu\n", d);
    }
}

// R18a: per-part-number packaging + supply angle in the H8H component record (HLC_FORMAT.md §8.1)
static void test_supply_angle(PluginRegistry& r) {
    std::printf("== H8H supply angle\n");
    const std::string src = "tests/data/hlc/juki_resave/single.h8h";
    const auto orig = slurp(src);
    Project q;
    CHECK(imp(r, "juki.hlc", src, q));
    const int pt = q.point_index("Top SMT");
    CHECK(pt >= 0 && !q.parts.empty());
    if (pt < 0 || q.parts.empty()) return;
    PartNumber& pn = q.parts.begin()->second;
    CHECK(pn.supply_angle_set && pn.supply_angle == 0 && pn.packaging.empty());   // imported as-is (0 / kind 0)
    auto comp_rec = [](const std::vector<uint8_t>& d) {
        auto u32 = [&](size_t o) { return (uint32_t)d[o] | (uint32_t)d[o + 1] << 8 | (uint32_t)d[o + 2] << 16 | (uint32_t)d[o + 3] << 24; };
        return (size_t)u32(38 + 16 * 4) + u32(38 + 16 * 4 + 4);   // block 4 addr + prefix -> record 0
    };
    auto i16 = [](const std::vector<uint8_t>& d, size_t o) { return (int16_t)(d[o] | d[o + 1] << 8); };
    std::vector<double> before_rot;
    { MachineJob j; std::string e; CHECK(build_machine_job(q, pt, j, e)); for (auto& p : j.placements) before_rot.push_back(p.rot); }
    struct Case { const char* pk; bool set; double ang; int kind; size_t off; double want; };
    const Case cases[] = {
        {"Stick", false, 0, 2, 178, 270}, {"Tray", false, 0, 3, 178, 180}, {"MTC", false, 0, 3, 178, 0},
        {"Tape", false, 0, 1, 180, 0}, {"Tape", true, 90, 1, 180, 90}, {"Stick", true, 45.5, 2, 178, 45.5}, {"", true, 270, 0, 178, 270},
    };
    for (const Case& k : cases) {
        Project t = q;
        PartNumber& tp = t.parts.begin()->second;
        tp.packaging = k.pk; tp.supply_angle_set = k.set; tp.supply_angle = k.ang;
        CHECK(near(effective_supply_angle(tp), k.want));
        std::vector<uint8_t> b;
        CHECK(export_point(r, t, pt, "juki.h8h", b));
        if (b.size() != orig.size()) { CHECK(false); continue; }
        const size_t rc = comp_rec(b);
        CHECK(b[rc + 80] == k.kind);
        CHECK(i16(b, rc + k.off) == (int16_t)std::lround(k.want * 80));
        // nothing else in the file moves
        size_t nd = 0; for (size_t i = 0; i < b.size(); ++i) if (b[i] != orig[i] && i != rc + 80 && i != rc + k.off && i != rc + k.off + 1) ++nd;
        CHECK(nd == 0);
        juki::HlcParsed hp; std::string e;
        CHECK(juki::parse_hlc(b, hp, e) && hp.comp_supply.size() == 1 && near(hp.comp_supply[0], k.want) && hp.comp_kind[0] == k.kind);
        // read back by the importer, then re-export byte-identical
        const std::string tmp = kTmp + "/supply.h8h";
        { std::ofstream f(tmp, std::ios::binary); f.write((const char*)b.data(), (std::streamsize)b.size()); }
        Project q2;
        CHECK(imp(r, "juki.hlc", tmp, q2));
        const PartNumber& p2 = q2.parts.begin()->second;
        CHECK(p2.supply_angle_set && near(p2.supply_angle, k.want));
        CHECK(p2.packaging == (k.kind == 1 ? "Tape" : k.kind == 2 ? "Stick" : k.kind == 3 ? "Tray" : ""));
        std::vector<uint8_t> b2;
        CHECK(export_point(r, q2, q2.point_index("Top SMT"), "juki.h8h", b2));
        CHECK(first_diff(b, b2) == (size_t)-1);
        // the supply angle never changes placement angles
        MachineJob j; std::string je; CHECK(build_machine_job(t, pt, j, je));
        bool same = j.placements.size() == before_rot.size();
        for (size_t i = 0; same && i < before_rot.size(); ++i) same = near(j.placements[i].rot, before_rot[i]);
        CHECK(same);
    }
    std::printf("  supply angle cases ok\n");
}

int main() {
    PluginRegistry r;
    register_builtin_plugins(r);
    CHECK(r.importer("juki.hlc") != nullptr);
    std::string err;
    if (std::system(("mkdir -p '" + kTmp + "'").c_str()) != 0) return 1;
    test_juki_resaves(r);
    test_supply_angle(r);

    Project demo;
    CHECK(imp(r, "gencad", kCad, demo));
    CHECK(imp(r, "bom", kBom, demo));
    CHECK(imp(r, "centroid", kRef, demo, {{"units", "mil"}}));
    int fids = 0;
    for (auto& c : demo.comps) {
        if (c.feature == Feature::Component) c.assigned_point = 0;
        if (c.feature == Feature::Fiducial) ++fids;
    }
    if (fids == 0) {   // make sure global marks are exercised
        Component f; f.feature = Feature::Fiducial; f.ref = "FID1"; f.pos = {2.54, 2.54}; demo.add_component(f);
        f.ref = "FID2"; f.pos = {96.52, 71.12}; demo.add_component(f);
    }
    std::printf("demo: %zu comps (%d fiducials)\n", demo.comps.size(), fids);
    Options base{{"juki.timestamp", "1700000000"}, {"juki.comment", "round trip"}};

    // single board, default corner
    round_trip(r, demo, "single board", base, true, 1);

    // RL transport, back rail, board corner, circuit object, local mark pair, thickness, sort by part
    {
        Options o = base;
        o["juki.transport"] = "RL"; o["juki.fixed_rail"] = "back";
        o["juki.board_x0"] = "5.5"; o["juki.board_y0"] = "-3.25";
        o["juki.circuit_w"] = "50"; o["juki.circuit_h"] = "40"; o["juki.circuit_x"] = "1"; o["juki.circuit_y"] = "2";
        o["juki.board_t"] = "0.8"; o["juki.sort_by_part"] = "1";
        Project s = demo;
        std::string fa, fb;
        for (auto& c : s.comps) if (c.feature == Feature::Fiducial) { (fa.empty() ? fa : fb) = c.ref; }
        const Component* first = nullptr;
        for (auto& c : s.comps) if (c.feature == Feature::Component) { first = &c; break; }
        if (first && !fb.empty()) o["juki.local." + first->ref] = fa + "," + fb;
        round_trip(r, s, "RL/back/corner/local marks/sorted", o, true, 1);
    }

    // panel: 3 images, one rotated 180, one x-out
    {
        Project s = demo;
        BBox bb = s.board_bbox();
        s.images.resize(3);
        s.images[1].name = "2"; s.images[1].root = false; s.images[1].offset = {bb.w() + 5.0, 0};
        s.images[2].name = "3"; s.images[2].root = false; s.images[2].offset = {3 * bb.w() + 10.0, bb.h()};
        s.images[2].rot = 180; s.images[2].x_out = true;
        s.panel_frame = {{{bb.x0, bb.y0}, {bb.x0 + 3 * bb.w() + 12, bb.y0}, {bb.x0 + 3 * bb.w() + 12, bb.y1 + 1}, {bb.x0, bb.y1 + 1}}};
        round_trip(r, s, "panel 3 images (rot 180, x-out)", base, true, 3);
    }

    // inconsistent circuits: perturb one placement of circuit 2 -> expanded import
    {
        Project s = demo;
        s.images.resize(2);
        s.images[1].root = false; s.images[1].offset = {120, 0};
        MachineJob job;
        CHECK(build_machine_job(s, 0, job, err));
        for (auto& p : job.placements) if (p.circuit == 2) { p.x += 0.5; break; }
        for (auto& kv : base) job.options[kv.first] = kv.second;
        // Round 11: circuit 1 is written once and repeated by the machine, so a panel
        // that is not an exact step & repeat is refused (HLC_FORMAT.md §5) instead of
        // being written CircuitCAM-style with every circuit expanded.
        std::vector<uint8_t> a;
        CHECK(!juki::write_hlc(job, juki::HlcFormat::H8H, a, err) && err.find("step-and-repeat") != std::string::npos);
        job.single_board = true;                       // 'Treat all images as one board'
        for (auto& p : job.placements) p.circuit = 1;
        CHECK(juki::write_hlc(job, juki::HlcFormat::H8H, a, err));
        Project q; juki::ImportResult res;
        CHECK(juki::import_hlc(a, q, {}, res, err));
        CHECK(!res.step_repeat && q.images.size() == 1 && res.placements == (int)job.placements.size());
        std::printf("== expanded (single board): %d placements imported, %zu images\n", res.placements, q.images.size());
    }

    test_iss();
    test_samples(r);
    CHECK(juki::sniff_juki(std::vector<uint8_t>(10, 0)) == 0);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
