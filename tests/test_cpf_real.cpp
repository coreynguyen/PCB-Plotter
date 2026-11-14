// test_cpf_real.cpp - import a real CircuitCAM project (.cpf) and check it is faithful.
// usage: test_cpf_real [file.cpf]   (default: cpf_project1 from tests/local_data.map; skipped if absent)
// Build: see tests/run_all.sh (same link line as test_plugins).
#include "local_data.h"
#include "../src/core/fileio.h"
#include "../src/plugins/plugin.h"
#include "pcbp_plugin.h"
#include "../src/core/placement_check.h"
#include "../src/core/machine_job.h"
#include "../src/io/juki_writer.h"

#include <cmath>
#include <fstream>
#include <cstdio>
#include <map>
#include <set>
#include <string>

using namespace pcbp;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// Panel sample (cpf_panel): 2x5 panel, one circuit (Images.ImageOutline = circuit frame),
// Assembly.Outline = panel frame, 3 Master (global) fiducials at the panel corners.
static void test_panel(PluginRegistry& r) {
    const std::string path = pcbp_test::local("cpf_panel");
    if (!std::ifstream(path).good()) { std::printf("SKIP: %s not found\n", path.c_str()); return; }
    Importer* imp = r.importer("circuitcam.cpf");
    Project prj; ImportReport rep; std::string err;
    const bool ok = imp->run(path, default_options(imp->info()), prj, rep, err);
    CHECK(ok);
    std::printf("panel: %s %s\n", ok ? rep.summary().c_str() : "FAILED", err.c_str());
    for (auto& m : rep.messages) std::printf("    %s\n", m.c_str());
    if (!ok) return;
    int fids = 0, glob = 0, top = 0, bot = 0;
    for (auto& c : prj.comps) {
        if (c.feature == Feature::Fiducial) { ++fids; glob += c.fid_scope == FiducialScope::Global; CHECK(!prj.instanced(c)); CHECK(prj.instance_count(c) == 1); }
        else { CHECK(prj.instanced(c)); CHECK(prj.instance_count(c) == 10); (c.side == Side::Top ? top : bot)++; }
    }
    CHECK(prj.comps.size() == 57);
    CHECK(fids == 3 && glob == 3);
    CHECK(top == 31 && bot == 23);
    CHECK(prj.packages.size() == 9);
    for (auto& c : prj.comps) CHECK(prj.package_of(c) != nullptr);
    CHECK(prj.images.size() == 10);
    // ImageLoc 6: (2640, 0) mil, idx (1,0); ImageLoc 5: (0,-4400) mil
    int found = 0;
    for (auto& im : prj.images) {
        if (im.x_index == 1 && im.y_index == 0) { ++found; CHECK(std::fabs(im.offset.x - 67.056) < 1e-3 && std::fabs(im.offset.y) < 1e-3); }
        if (im.x_index == 0 && im.y_index == 4) { ++found; CHECK(std::fabs(im.offset.x) < 1e-3 && std::fabs(im.offset.y + 111.76) < 1e-3); }
    }
    CHECK(found == 2);
    // frames: circuit 261..2779 x 4661..5740 mil (closed), panel -1..5680 x 0..6000 mil
    CHECK(prj.board_frame.size() == 1);
    CHECK(prj.panel_frame.size() == 1);
    const BBox cb = prj.circuit_frame_bbox(), pb = prj.panel_frame_bbox();
    CHECK(std::fabs(cb.x0 - 6.6294) < 1e-3 && std::fabs(cb.x1 - 70.5866) < 1e-3);
    CHECK(std::fabs(cb.y0 - 118.3894) < 1e-3 && std::fabs(cb.y1 - 145.796) < 1e-3);
    CHECK(std::fabs(pb.x0 + 0.0254) < 1e-3 && std::fabs(pb.x1 - 144.272) < 1e-3 && std::fabs(pb.y1 - 152.4) < 1e-3);
    CHECK(point_in_frame(prj.board_frame, {30, 130}));
    CHECK(!point_in_frame(prj.board_frame, {100, 130}));
    // every image's circuit frame lies inside the panel frame
    for (int i = 0; i < (int)prj.images.size(); ++i)
        for (auto& poly : prj.circuit_frame_at(i)) for (auto& p : poly) CHECK(point_in_frame(prj.panel_frame, p));
    // no false outside-board errors
    auto iss = check_placements(prj);
    int outside = 0;
    for (auto& i : iss) if (i.code == CheckCode::OutsideBoard || i.code == CheckCode::PartlyOutsideBoard) { ++outside; std::printf("    unexpected: %s\n", i.message.c_str()); }
    CHECK(outside == 0);
    // a part moved off the circuit is still caught
    {
        Project q = prj;
        for (auto& c : q.comps) if (c.ref == "R17") c.pos = {100, 130};
        int n = 0; for (auto& i : check_placements(q)) n += i.code == CheckCode::OutsideBoard;
        CHECK(n == 1);
    }
    // machine job: 3 global marks (not instanced), circuit frame, 10 circuits
    CHECK(prj.model.points.size() == 1);
    if (prj.model.points.empty()) return;
    CHECK(prj.model.points[0].machine.orientation == 90);   // MachineSetups.Orientation = 1
    for (auto& c : prj.comps) if (c.feature == Feature::Component && c.side == Side::Top) c.assigned_point = 0;
    MachineJob job;
    CHECK(build_machine_job(prj, 0, job, err));
    CHECK(job.fiducials.size() == 3);
    for (auto& f : job.fiducials) CHECK(!f.local && f.circuit == 0);
    CHECK(job.circuits.size() == 10);
    CHECK(job.has_circuit);
    std::printf("    job: board %.3fx%.3f circuit %.3fx%.3f at (%.3f,%.3f) placements %zu\n", job.board_w, job.board_h,
                job.circuit_w, job.circuit_h, job.circuit_x, job.circuit_y, job.placements.size());
    CHECK(job.placements.size() == 310);
    // circuit frame survives H7H / H8H export
    for (auto fmt : {juki::HlcFormat::H7H, juki::HlcFormat::H8H, juki::HlcFormat::H5H}) {
        std::vector<uint8_t> bin;
        CHECK(juki::write_hlc(job, fmt, bin, err));
        juki::HlcParsed hp;
        CHECK(juki::parse_hlc(bin, hp, err));
        CHECK(std::abs(hp.ckt_w - (int32_t)std::lround(job.circuit_w * 1e4)) <= 1);
        CHECK(std::abs(hp.ckt_h - (int32_t)std::lround(job.circuit_h * 1e4)) <= 1);
        // 10 unrotated circuits on a regular grid -> Matrix (div_x*div_y), not a
        // Non-Matrix list (HLC_FORMAT.md §5); either way 10 circuits
        const int cfg = (hp.board_mode >> 3) & 3;
        CHECK((cfg == 1 && hp.div_x * hp.div_y == 10) || (cfg == 2 && hp.circuit_count == 10));
        if (fmt == juki::HlcFormat::H8H) std::printf("    H8H circuit config %d div %dx%d list %u\n", cfg, hp.div_x, hp.div_y, hp.circuit_count);
        CHECK(!hp.mark_sets.empty() || fmt == juki::HlcFormat::H5H);
    }
}

int main(int argc, char** argv) {
    if (argc <= 1) { PluginRegistry r0; register_builtin_plugins(r0); test_panel(r0); }
    const std::string path = argc > 1 ? std::string(argv[1]) : pcbp_test::local("cpf_project1");
    if (!std::ifstream(path).good()) { std::printf("SKIP: %s not found\npassed 0, failed 0\n", path.c_str()); return 0; }
    PluginRegistry r;
    register_builtin_plugins(r);
    Importer* imp = r.importer("circuitcam.cpf");
    CHECK(imp != nullptr);
    if (!imp) return 1;
    Project prj;
    ImportReport rep;
    std::string err;
    const bool ok = imp->run(path, default_options(imp->info()), prj, rep, err);
    CHECK(ok);
    std::printf("import: %s %s\n", ok ? rep.summary().c_str() : "FAILED", err.c_str());
    for (auto& m : rep.messages) std::printf("    %s\n", m.c_str());
    if (!ok) return 1;

    // ---- counts (Project1.cpf: 458 Locations, all components, 258 top / 200 bottom) ----
    int comps = 0, fids = 0, bottom = 0, assigned = 0, unresolved = 0;
    std::set<std::string> used_pk;
    BBox bb;
    for (auto& c : prj.comps) {
        if (c.feature == Feature::Fiducial) ++fids;
        else if (c.feature == Feature::Component) ++comps;
        if (c.side == Side::Bottom) ++bottom;
        if (c.assigned_point >= 0) ++assigned;
        if (!c.package.empty()) { used_pk.insert(c.package); if (!prj.packages.count(c.package)) ++unresolved; }
        bb.add(c.pos);
    }
    int with_pads = 0;
    for (auto& kv : prj.packages) if (!kv.second.pads.empty()) ++with_pads;
    const double w = bb.x1 - bb.x0, h = bb.y1 - bb.y0;
    std::printf("components %d, fiducials %d, bottom %d, assigned %d\n", comps, fids, bottom, assigned);
    std::printf("packages %zu (with pads %d), used %zu, unresolved refs %d\n", prj.packages.size(), with_pads,
                used_pk.size(), unresolved);
    std::printf("placement extent %.2f x %.2f mm  (%.2f,%.2f)-(%.2f,%.2f)\n", w, h, bb.x0, bb.y0, bb.x1, bb.y1);
    std::printf("model '%s' points %zu\n", prj.model.name.c_str(), prj.model.points.size());
    for (auto& p : prj.model.points)
        std::printf("  point '%s' type '%s' machine %s file '%s' cc.mach_type=%s\n", p.name.c_str(), p.process_type.c_str(),
                    p.has_machine ? p.machine.exporter_id.c_str() : "-", p.machine.target_file.c_str(),
                    p.machine.options.count("cc.mach_type") ? p.machine.options.at("cc.mach_type").c_str() : "-");
    for (size_t k = 0; k < prj.comps.size() && k < 10; ++k) {
        const Component& c = prj.comps[k];
        std::printf("  %-6s x=%9.3f y=%9.3f rot=%5.1f %s %-11s pt=%d\n", c.ref.c_str(), c.pos.x, c.pos.y, c.rot,
                    c.side == Side::Bottom ? "B" : "T", c.package.c_str(), c.assigned_point);
    }

    const bool is_p1 = path == pcbp_test::local("cpf_project1");
    if (is_p1) {
        CHECK(prj.comps.size() == 458);
        CHECK(comps == 458);
        CHECK(fids == 0);
        CHECK(bottom == 200);
        CHECK(prj.packages.size() == 32);           // every Packages row (32, each with a Rendering)
        CHECK(with_pads == 32);                     // rows with a Rendering blob
        CHECK(used_pk.size() == 32);
        // the ComponentAssignments blob of this file holds 0 records for its single model point:
        // with no explicit assignments every component is unassigned (as in CircuitCAM).
        CHECK(assigned == 0);
        CHECK(prj.model.points.size() == 1);
        if (!prj.model.points.empty()) {
            const ModelPoint& p = prj.model.points[0];
            CHECK(p.name == "Pick-n-Place");
            CHECK(p.has_machine);
            CHECK(p.machine.exporter_id == "juki.h5h");    // MachType 57 = Juki HLC, TargetFile JUKI.H5H
            CHECK(p.machine.target_file == "JUKI.H5H");
            CHECK(p.machine.options.at("cc.mach_type") == "57");
        }
        // R61: X=7248.03149607 mil, Y=5299.2125984 mil, rot 180
        bool found = false;
        for (auto& c : prj.comps)
            if (c.ref == "R61") { found = true; CHECK(std::fabs(c.pos.x - 184.1) < 0.01); CHECK(std::fabs(c.pos.y - 134.6) < 0.01); CHECK(c.rot == 180); }
        CHECK(found);
    }
    CHECK(unresolved == 0);
    CHECK(w > 10 && w < 1000 && h > 10 && h < 1000);
    std::printf("passed %d, failed %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
