// ============================================================================
// test_program_gen.cpp — LLM_API §3A.4 G4: headless program generation (core/program_gen).
// Same bytes as the direct exporter path, verified write + read-back, dry run, linked pair
// ([Side 1]/[Side 2] files, placements CSV, sides report), naming, failure modes. Synthetic data only.
// ============================================================================
#include "../src/core/program_gen.h"
#include "../src/core/fileio.h"
#include "../src/core/project.h"
#include "../src/core/sides.h"
#include "../src/io/juki_writer.h"
#include "../src/plugins/plugin.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace pcbp;
namespace fs = std::filesystem;
static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void fid(Project& p, const char* r, double x, double y, Side s, int number) {
    Component c; c.ref = r; c.pos = {x, y}; c.feature = Feature::Fiducial; c.side = s; c.fid_number = number; c.package = "FID";
    p.add_component(c);
}
static void part(Project& p, const char* r, double x, double y, Side s, int point) {
    Component c; c.ref = r; c.pos = {x, y}; c.package = "0603"; c.number = "PN1"; c.side = s; c.assigned_point = point;
    p.add_component(c);
}
static ModelPoint layer(const char* name, Side s, const char* exporter = "juki.h8h") {
    ModelPoint mp; mp.name = name; mp.process_type = "SMT"; mp.has_machine = true; mp.machine.exporter_id = exporter;
    mp.side = mp.machine.side = s; mp.machine.flipped = s == Side::Bottom;
    return mp;
}
static Project board() {
    Project p;
    p.info.name = "DEMO-100";
    p.model.points.clear();
    Package pk; pk.name = "0603"; pk.pads = {Pad{{-0.8, 0}, 0.8, 0.9}, Pad{{0.8, 0}, 0.8, 0.9}}; p.packages["0603"] = pk;
    Package fp; fp.name = "FID"; fp.pads = {Pad{{0, 0}, 1.0, 1.0}}; p.packages["FID"] = fp;
    PartNumber pn; pn.number = "PN1"; pn.package = "0603"; p.parts["PN1"] = pn;
    p.model.points.push_back(layer("Top SMT", Side::Top));
    p.model.points.push_back(layer("Bottom SMT", Side::Bottom));
    part(p, "R1", 10, 10, Side::Top, 0); part(p, "R2", 20, 12, Side::Top, 0);
    part(p, "R3", 30, 15, Side::Bottom, 1);
    fid(p, "FT1", 2, 2, Side::Top, 1); fid(p, "FT2", 40, 2, Side::Top, 2); fid(p, "FT3", 40, 30, Side::Top, 3);
    fid(p, "FB1", 2, 28, Side::Bottom, 1); fid(p, "FB2", 38, 28, Side::Bottom, 2);
    return p;
}

int main() {
    PluginRegistry reg;
    register_builtin_plugins(reg);
    const fs::path tmp = fs::temp_directory_path() / "pcbp_test_program_gen";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    const std::string dir = tmp.string();

    // ---- names
    CHECK(progen::sanitize_filename("a<b>:c\"d/e\\f|g?h*i. ") == "a_b__c_d_e_f_g_h_i");
    CHECK(progen::sanitize_filename("...") == "program");
    CHECK(progen::join_path("C:\\Jobs", "x.h8h") == "C:\\Jobs\\x.h8h");
    CHECK(progen::join_path("/tmp/", "x.h8h") == "/tmp/x.h8h");
    {
        Project p = board();
        Exporter* ex = progen::machine_exporter(reg, "juki.h8h");
        CHECK(ex && progen::exporter_ext(ex) == "h8h");
        CHECK(progen::default_program_file(p, 0, ex) == job_base_name(p) + " Top SMT.h8h");
        p.model.points[0].machine.target_file = "custom.h8h";
        CHECK(progen::program_file_for(p, 0, ex) == "custom.h8h");
        CHECK(progen::machine_exporter(reg, "") == nullptr);
        CHECK(progen::machine_exporter(reg, "no.such") == nullptr);
    }

    // ---- generate_point == direct exporter path (the GUI's pre-G4 sequence), byte for byte
    {
        Project p = board();
        progen::Generated g; std::string err;
        CHECK(progen::generate_point(p, 0, g, err, &reg));
        CHECK(err.empty() && !g.bytes.empty() && g.exporter_id == "juki.h8h" && g.point == 0);
        CHECK(g.job.placements.size() == 2);
        CHECK(progen::written_mark_count(g.job) == 3);
        // reference: build_machine_job + merged options + export_job, as generate_program did inline
        MachineJob j; std::string e2;
        CHECK(build_machine_job(p, 0, j, e2));
        Exporter* ex = reg.exporter("juki.h8h");
        Options o = default_options(ex->info());
        for (auto& kv : p.model.points[0].machine.options) if (kv.first.rfind("__", 0) != 0) o[kv.first] = kv.second;
        for (auto& kv : o) j.options[kv.first] = kv.second;
        j.machine = "juki.h8h";
        std::vector<uint8_t> ref;
        CHECK(ex->export_job(j, o, ref, e2));
        CHECK(ref == g.bytes);
        std::vector<juki::HlcCheck> ck;
        CHECK(juki::verify_hlc_bytes(g.bytes, ck, e2));
    }

    // ---- failure modes (no hard block from validation; only "cannot generate")
    {
        Project p = board();
        progen::Generated g; std::string err;
        CHECK(!progen::generate_point(p, 7, g, err, &reg) && err == "Invalid process point.");
        p.model.points[0].has_machine = false;
        CHECK(!progen::generate_point(p, 0, g, err, &reg) && err.find("no machine interface") != std::string::npos);
        p.model.points[0].has_machine = true; p.model.points[0].machine.exporter_id = "no.such";
        CHECK(!progen::generate_point(p, 0, g, err, &reg) && err.find("not installed") != std::string::npos);
        p.model.points[0].machine.exporter_id.clear();
        CHECK(!progen::generate_point(p, 0, g, err, &reg) && err.find("No machine type") != std::string::npos);
        std::vector<std::string> be; std::string fail;
        CHECK(!progen::check_program_bytes("juki.h8h", {}, be, fail) && !fail.empty());
        CHECK(!progen::check_program_bytes("juki.h8h", std::vector<uint8_t>(16, 0), be, fail));
        CHECK(progen::check_program_bytes("generic.csv", {'x'}, be, fail) && be.empty());
    }

    // ---- export_point: dry run writes nothing; real run = verified file identical to the generated bytes
    {
        Project p = board();
        progen::PointExport pe;
        CHECK(progen::export_point(p, 0, dir, pe, true, &reg) && pe.ok);
        CHECK(!pe.write.path.empty() && !fs::exists(pe.write.path));
        CHECK(!pe.preflight.empty());   // the review list is returned (Info lines at least), never enforced
        const bool okw = progen::export_point(p, 0, dir, pe, false, &reg);
        if (!okw) std::printf("  export_point: %s [stage %s]\n", pe.err.c_str(), pe.write.stage.c_str());
        CHECK(okw && pe.ok && pe.write.ok && pe.write.post_errors.empty());
        std::vector<uint8_t> back; std::string err;
        CHECK(read_file(pe.write.path, back, err) && back == pe.gen.bytes);
        // no folder anywhere -> clear error, nothing written
        progen::PointExport pn;
        CHECK(!progen::export_point(p, 0, "", pn, false, &reg) && pn.err.find("No export folder") != std::string::npos);
        // project export folder is used when dir is ""
        p.model.export_dir = dir;
        CHECK(progen::export_point(p, 1, "", pn, false, &reg) && pn.ok && fs::exists(pn.write.path));
        // a bottom layer without its own marks still exports (warning, not a block)
        Project q = board();
        for (auto it = q.comps.begin(); it != q.comps.end();) it = (it->feature == Feature::Fiducial && it->side == Side::Bottom) ? q.comps.erase(it) : std::next(it);
        progen::PointExport pq;
        CHECK(progen::export_point(q, 1, dir, pq, true, &reg) && pq.ok);
        bool warned = false;
        for (auto& i : pq.preflight) warned |= i.sev == ExportIssue::Warning && i.msg.find("No fiducial marks") != std::string::npos;
        CHECK(warned);
    }

    // ---- export_link: linked pair -> two verified programs + CSVs + sides report
    {
        Project p = board();
        p.model.points[0].link = p.model.points[1].link = 1;
        p.model.points[0].machine.target_file = "DEMO-100 [SIDE1].h8h";
        p.model.points[1].machine.target_file = "DEMO-100 [SIDE2].h8h";
        progen::LinkExport le; std::string err;
        CHECK(progen::export_link(p, 1, dir, le, err, true, {}, &reg));   // dry run
        CHECK(le.all_ok && le.sides.size() == 2 && le.report_html.empty() && !fs::exists(fs::path(dir) / "DEMO-100 [SIDE1].h8h"));
        CHECK(progen::export_link(p, 1, dir, le, err, false, {}, &reg));
        CHECK(le.all_ok && err.empty());
        CHECK(le.sides[0].program_file == "DEMO-100 [SIDE1].h8h" && le.sides[1].program_file == "DEMO-100 [SIDE2].h8h");
        CHECK(le.sides[0].placements == 2 && le.sides[1].placements == 1);
        for (int i = 0; i < 2; ++i) {
            const fs::path f = fs::path(dir) / le.sides[i].program_file;
            std::vector<uint8_t> back;
            CHECK(fs::exists(f) && read_file(f.string(), back, err) && back == le.gens[i].bytes);
            CHECK(fs::exists(fs::path(dir) / le.sides[i].csv_file));
            CHECK(le.gens[i].job.options["juki.comment"] == mfg_label(le.sides[i].side));
        }
        CHECK(!le.report_html.empty() && fs::exists(le.report_html) && fs::exists(le.report_txt));
        // gate refusing byte errors only matters when there are fresh errors; none here -> gate never called
        bool called = false;
        progen::LinkExport l2;
        CHECK(progen::export_link(p, 1, dir, l2, err, true, [&](const MfgSide&, const std::vector<std::string>&) { called = true; return false; }, &reg));
        CHECK(!called && l2.all_ok);
        // unlinked id -> error, nothing written
        CHECK(!progen::export_link(p, 9, dir, l2, err, false, {}, &reg) && err.find("Link 9") != std::string::npos);
    }

    // ---- fresh_byte_errors: suffix match against reviewed Error issues only
    {
        std::vector<ExportIssue> rev = {{ExportIssue::Error, "", "Top SMT: Machine program check: X"}, {ExportIssue::Warning, "", "Machine program check: Y"}};
        auto f = progen::fresh_byte_errors(rev, {"Machine program check: X", "Machine program check: Y"});
        CHECK(f.size() == 1 && f[0] == "Machine program check: Y");
    }

    fs::remove_all(tmp, ec);
    std::printf("test_program_gen: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
