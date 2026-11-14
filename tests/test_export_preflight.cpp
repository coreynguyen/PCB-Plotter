// ============================================================================
// test_export_preflight.cpp — R21 (specs/AUDIT_V20.md): one shared export preflight for every Generate
// path (P0), strict programmed-side fiducials (P0), marks selected by fiducial number (P1), generated-bytes
// check + verified write (P1). Synthetic data only.
// ============================================================================
#include "../src/core/export_preflight.h"
#include "../src/core/fileio.h"
#include "../src/core/json.h"
#include "../src/core/machine_job.h"
#include "../src/core/project.h"
#include "../src/core/project_io.h"
#include "../src/core/sides.h"
#include "../src/io/juki_writer.h"
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace pcbp;
static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static bool has_msg(const std::vector<ExportIssue>& v, const char* s, ExportIssue::Sev sev) {
    for (auto& i : v) if (i.sev == sev && i.msg.find(s) != std::string::npos) return true;
    return false;
}
static bool same(const std::vector<ExportIssue>& a, const std::vector<ExportIssue>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (a[i].sev != b[i].sev || a[i].ref != b[i].ref || a[i].msg != b[i].msg) return false;
    return true;
}

static void fid(Project& p, const char* r, double x, double y, Side s, int number = 0) {
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
static Project base() {
    Project p;
    p.model.points.clear();
    Package pk; pk.name = "0603"; pk.pads = {Pad{{-0.8, 0}, 0.8, 0.9}, Pad{{0.8, 0}, 0.8, 0.9}}; p.packages["0603"] = pk;
    Package fp; fp.name = "FID"; fp.pads = {Pad{{0, 0}, 1.0, 1.0}}; p.packages["FID"] = fp;
    PartNumber pn; pn.number = "PN1"; pn.package = "0603"; p.parts["PN1"] = pn;
    return p;
}

int main() {
    // ---------------------------------------------------------------- P0: bottom never inherits top marks
    {
        Project p = base();
        p.model.points.push_back(layer("Top SMT", Side::Top));
        p.model.points.push_back(layer("Bottom SMT", Side::Bottom));
        part(p, "R1", 10, 10, Side::Top, 0);
        part(p, "R2", 20, 15, Side::Bottom, 1);
        fid(p, "FT1", 2, 2, Side::Top, 1); fid(p, "FT2", 40, 2, Side::Top, 2); fid(p, "FT3", 40, 30, Side::Top, 3);
        MachineJob j; std::string err;
        CHECK(build_machine_job(p, 1, j, err));
        CHECK(j.fiducials.empty());                                   // was: the three top marks (fallback)
        auto v = validate_for_export(p, 1);
        // R24a (user v23): unlinked side without marks = a Warning (export proceeds), never the other side's marks
        CHECK(has_msg(v, "No fiducial marks on the bottom side and no linked layer", ExportIssue::Warning));
        CHECK(!has_msg(v, "No fiducial marks", ExportIssue::Error));
        // the generated program carries no marks either
        std::vector<uint8_t> buf; juki::HlcWriteInfo wi;
        j.machine = "juki.h8h";
        CHECK(juki::write_hlc_variant(j, juki::HlcVariant::H8H, buf, err, &wi) && wi.marks == 0);
        // explicit "No marks (intentional)" suppresses the error, never borrows marks
        p.model.points[1].machine.no_marks = true;
        v = validate_for_export(p, 1);
        CHECK(!has_msg(v, "No fiducial marks", ExportIssue::Error));
        CHECK(has_msg(v, "intentional", ExportIssue::Info));
        CHECK(build_machine_job(p, 1, j, err) && j.fiducials.empty() && j.no_marks);
        // persisted
        json::Value jv; Project r; std::string e2;
        CHECK(json::Value::parse(project_to_json(p).dump(), jv, e2) && project_from_json(jv, r, e2));
        CHECK(r.model.points[1].machine.no_marks && !r.model.points[0].machine.no_marks);
        p.model.points[1].machine.no_marks = false;
        // a bottom mark: only that one is taken
        fid(p, "FB1", 5, 25, Side::Bottom, 1);
        CHECK(build_machine_job(p, 1, j, err) && j.fiducials.size() == 1 && j.fiducials[0].name == "FB1");
        v = validate_for_export(p, 1);
        CHECK(!has_msg(v, "No fiducial marks", ExportIssue::Error));
        CHECK(has_msg(v, "Marks written (Board mode): FB1", ExportIssue::Info));
        // the top program never sees the bottom mark
        CHECK(build_machine_job(p, 0, j, err) && j.fiducials.size() == 3);
        for (auto& f : j.fiducials) CHECK(f.name != "FB1");
        // build_side_job (export_all_sides) follows the same rule
        MfgSide sd; sd.seq = 2; sd.physical = Side::Bottom; sd.point = "Bottom SMT";
        CHECK(build_side_job(p, sd, j, err) && j.fiducials.size() == 1 && j.fiducials[0].name == "FB1");
    }
    // ---------------------------------------------------------------- P0: Board / Circuit mode validated as written
    {
        MachineJob j;
        j.machine = "juki.h8h"; j.board_w = 100; j.board_h = 60; j.program_name = "T";
        j.circuits.resize(2);
        j.circuits[0].index = 1; j.circuits[1].index = 2; j.circuits[1].ax = 50; j.circuits[1].x = 50;
        for (int c = 1; c <= 2; ++c) {
            JobPlacement pl; pl.ref = "R1"; pl.part = "PN1"; pl.package = "0603"; pl.x = 10 + (c - 1) * 50; pl.y = 10; pl.circuit = c;
            j.placements.push_back(pl);
        }
        JobPart jp; jp.part = "PN1"; jp.package = "0603"; jp.count = 2; jp.length = 1.6; jp.width = 0.8; j.parts.push_back(jp);
        for (int c = 1; c <= 2; ++c) {   // circuit (image) marks only
            JobFiducial f; f.name = "C" + std::to_string(c); f.x = 2 + (c - 1) * 50; f.y = 2; f.local = true; f.circuit = c; f.number = 1;
            j.fiducials.push_back(f);
        }
        auto ms = juki::select_marks(j);   // default Board mode: no board marks qualify
        CHECK(!ms.fid_circuit && ms.marks.empty());
        std::vector<ExportIssue> v;
        juki::check_job(j, v);
        CHECK(has_msg(v, "Board fiducial mode, but no board", ExportIssue::Error));
        j.options["juki.fid_mode"] = "circuit";
        ms = juki::select_marks(j);
        CHECK(ms.fid_circuit && ms.marks.size() == 1 && ms.marks[0]->name == "C1");
        v.clear(); juki::check_job(j, v);
        CHECK(!has_msg(v, "fiducial mode, but", ExportIssue::Error));
        j.no_marks = true; j.options["juki.fid_mode"] = "board";
        v.clear(); juki::check_job(j, v);
        CHECK(!has_msg(v, "Board fiducial mode, but no board", ExportIssue::Error) && has_msg(v, "Board fiducial mode, but no board", ExportIssue::Info));
    }
    // ---------------------------------------------------------------- P1: marks by Fiducial number
    {
        Project p = base();
        p.model.points.push_back(layer("Top SMT", Side::Top));
        part(p, "R1", 10, 10, Side::Top, 0);
        // storage order: #3, unnumbered A, #1, unnumbered B, #2
        fid(p, "F3", 30, 30, Side::Top, 3); fid(p, "UA", 1, 1, Side::Top, 0); fid(p, "F1", 10, 30, Side::Top, 1);
        fid(p, "UB", 2, 2, Side::Top, 0); fid(p, "F2", 20, 30, Side::Top, 2);
        MachineJob j; std::string err;
        CHECK(build_machine_job(p, 0, j, err) && j.fiducials.size() == 5);
        bool numbers = false; for (auto& f : j.fiducials) numbers |= f.name == "F2" && f.number == 2;
        CHECK(numbers);                                               // the builder keeps the number
        auto ms = juki::select_marks(j);
        CHECK(ms.candidates == 5 && ms.marks.size() == 3);
        CHECK(ms.marks.size() == 3 && ms.marks[0]->name == "F1" && ms.marks[1]->name == "F2" && ms.marks[2]->name == "F3");
        // the writer emits exactly that selection, in that order
        std::vector<uint8_t> buf; juki::HlcWriteInfo wi;
        j.machine = "juki.h8h";
        CHECK(juki::write_hlc_variant(j, juki::HlcVariant::H8H, buf, err, &wi) && wi.marks == 3);
        juki::HlcParsed hp;
        CHECK(juki::parse_hlc(buf, hp, err) && !hp.mark_sets.empty() && hp.mark_sets[0].marks.size() >= 3);
        if (!hp.mark_sets.empty() && hp.mark_sets[0].marks.size() >= 3)
            for (int k = 0; k < 3; ++k) {
                const auto& f = *ms.marks[k];
                CHECK(std::abs(hp.mark_sets[0].marks[k].x - (int)std::lround(f.x * 10000)) <= 1 &&
                      std::abs(hp.mark_sets[0].marks[k].y - (int)std::lround(f.y * 10000)) <= 1);
            }
        CHECK(juki::marks_text(ms).find("F1 (#1") == 0);
        // the more-than-3 note becomes a review warning naming what is written
        auto v = validate_for_export(p, 0);
        CHECK(has_msg(v, "the first 3 by fiducial number are written (F1, F2, F3)", ExportIssue::Warning));
        // unnumbered marks: storage order is the tie-break, after numbered ones
        Project q = base();
        q.model.points.push_back(layer("Top SMT", Side::Top));
        part(q, "R1", 10, 10, Side::Top, 0);
        fid(q, "UA", 1, 1, Side::Top, 0); fid(q, "UB", 2, 2, Side::Top, 0); fid(q, "N2", 3, 3, Side::Top, 2); fid(q, "UC", 4, 4, Side::Top, 0);
        CHECK(build_machine_job(q, 0, j, err));
        ms = juki::select_marks(j);
        CHECK(ms.marks.size() == 3 && ms.marks[0]->name == "N2" && ms.marks[1]->name == "UA" && ms.marks[2]->name == "UB");
        // explicit juki.global_marks keeps the operator's order
        j.options["juki.global_marks"] = "UC,UA";
        ms = juki::select_marks(j);
        CHECK(ms.explicit_list && ms.marks.size() == 2 && ms.marks[0]->name == "UC");
    }
    // ---------------------------------------------------------------- P0: one preflight for every entry point
    {
        Project p = base();
        p.model.points.push_back(layer("Top SMT", Side::Top));
        p.model.points.push_back(layer("Bottom SMT", Side::Bottom));
        p.model.points.push_back(layer("Plugin layer", Side::Top, "acme.plugin"));
        part(p, "R1", 10, 10, Side::Top, 0);
        part(p, "R1", 12, 10, Side::Top, 0);                          // duplicate ref -> Error
        part(p, "R2", 20, 15, Side::Bottom, 1);
        part(p, "R3", 25, 15, Side::Bottom, 0);                       // wrong side -> Error + Warning
        part(p, "R4", 30, 15, Side::Top, 2);
        fid(p, "FT1", 2, 2, Side::Top, 1);
        // single layer: every single-layer entry gets the identical list
        for (int pt : {0, 1, 2}) {
            const auto ref = export_preflight(p, ExportRequest{ExportEntry::GenerateCommand, pt});
            CHECK(!ref.empty());
            CHECK(same(ref, export_preflight(p, ExportRequest{ExportEntry::ExportViewLayer, pt})));
            CHECK(same(ref, export_preflight(p, ExportRequest{ExportEntry::PluginExport, pt})));
            CHECK(same(ref, export_preflight_point(p, pt)));
            // unlinked layer: the pair entries reduce to the same single-layer target
            CHECK(same(ref, export_preflight(p, ExportRequest{ExportEntry::ExportViewPair, pt})));
            CHECK(same(ref, export_preflight(p, ExportRequest{ExportEntry::ExportAllSides, pt})));
            // the list is exactly validate_for_export of that layer (sorted)
            CHECK(ref.size() == validate_for_export(p, pt).size());
        }
        auto r0 = export_preflight(p, ExportRequest{ExportEntry::GenerateCommand, 0});
        CHECK(has_msg(r0, "Duplicate reference", ExportIssue::Error) && has_msg(r0, "Top/bottom separation", ExportIssue::Error));
        auto r1 = export_preflight(p, ExportRequest{ExportEntry::GenerateCommand, 1});
        CHECK(has_msg(r1, "No fiducial marks on the bottom side", ExportIssue::Warning));   // R24a: warning, unlinked
        auto r2 = export_preflight(p, ExportRequest{ExportEntry::PluginExport, 2});   // non-JUKI exporter: core rules apply
        CHECK(!has_msg(r2, "No fiducial marks", ExportIssue::Error));
        // errors sort before warnings before notes
        for (size_t i = 1; i < r0.size(); ++i) CHECK(r0[i - 1].sev <= r0[i].sev);
        // linked pair: Export view (from either member) and export_all_sides give the identical list
        p.model.points[0].link = p.model.points[1].link = 1;
        const ExportTarget t0 = export_target(p, ExportRequest{ExportEntry::ExportViewPair, 0});
        const ExportTarget t1 = export_target(p, ExportRequest{ExportEntry::ExportViewPair, 1});
        CHECK(t0.link == 1 && t0.point == -1 && t0 == t1);
        CHECK(export_target(p, ExportRequest{ExportEntry::ExportAllSides, 1}) == t0);
        CHECK(export_target(p, ExportRequest{ExportEntry::GenerateCommand, 1}).point == 1);   // single layer stays single
        const auto pr = export_preflight(p, ExportRequest{ExportEntry::ExportAllSides, link_members(p, 1)[0]});
        CHECK(same(pr, export_preflight(p, ExportRequest{ExportEntry::ExportViewPair, 0})));
        CHECK(same(pr, export_preflight(p, ExportRequest{ExportEntry::ExportViewPair, 1})));
        CHECK(same(pr, export_preflight_link(p, 1)));
        // R24a: the linked bottom layer takes the top's pass-through mark (mirrored): no marks warning / error at all
        CHECK(!has_msg(pr, "No fiducial marks", ExportIssue::Error) && !has_msg(pr, "No fiducial marks", ExportIssue::Warning));
        CHECK(has_msg(pr, "mark(s) of linked layer", ExportIssue::Info));
        // review text: errors section first, distinct from warnings, notes never listed
        const std::string txt = export_review_text(pr);
        const size_t e = txt.find("ERRORS ("), w = txt.find("Warnings (");
        CHECK(e != std::string::npos && (w == std::string::npos || e < w));
        CHECK(txt.find("[Error]") != std::string::npos && txt.find("Marks written") == std::string::npos);
        CHECK(export_review_text(pr, 12, false).find("ERRORS (") == std::string::npos);
        // a target without any layer is never an empty (silently passing) list
        CHECK(has_errors(export_preflight_link(p, 7)));
        CHECK(has_errors(export_preflight_point(p, -1)));
    }
    // ---------------------------------------------------------------- P1: generated bytes / verified write
    {
        Project p = base();
        p.model.points.push_back(layer("Top SMT", Side::Top));
        part(p, "R1", 10, 10, Side::Top, 0);
        fid(p, "F1", 2, 2, Side::Top, 1); fid(p, "F2", 40, 30, Side::Top, 2);
        MachineJob j; std::string err;
        CHECK(build_machine_job(p, 0, j, err));
        j.machine = "juki.h8h";
        std::vector<uint8_t> buf;
        CHECK(juki::write_hlc_variant(j, juki::HlcVariant::H8H, buf, err));
        std::vector<juki::HlcCheck> ck;
        CHECK(juki::verify_hlc_bytes(buf, ck, err) && juki::hlc_check_ok(ck));
        std::vector<uint8_t> cut(buf.begin(), buf.begin() + buf.size() / 3);
        CHECK(!juki::verify_hlc_bytes(cut, ck, err) && !err.empty());
        std::vector<uint8_t> none;
        CHECK(!juki::verify_hlc_bytes(none, ck, err));

        char dir[] = "/tmp/pcbp_r21_wtestXXXXXX";
        CHECK(mkdtemp(dir) != nullptr);
        const std::string f = std::string(dir) + "/prog.h8h";
        std::string stage;
        const char old_txt[] = "OLD PROGRAM";
        CHECK(write_file(f, old_txt, sizeof old_txt - 1, err));
        CHECK(write_file_verified(f, buf.data(), buf.size(), err, &stage) && stage.empty());
        std::vector<uint8_t> back;
        CHECK(read_file(f, back, err) && back == buf);
        // replace failure keeps the previous destination and reports the stage
        const std::string d2 = std::string(dir) + "/isdir";
        CHECK(mkdir(d2.c_str(), 0755) == 0);
        const std::string inner = d2 + "/keep.txt";
        CHECK(write_file(inner, "x", 1, err));
        CHECK(!write_file_verified(d2, buf.data(), buf.size(), err, &stage) && stage == "replace");
        struct stat st{};
        CHECK(stat(inner.c_str(), &st) == 0);                          // old content untouched
        CHECK(access((d2 + ".tmp").c_str(), F_OK) != 0);               // temp cleaned up
        // missing folder: nothing written
        CHECK(!write_file_verified(std::string(dir) + "/nope/x.h8h", buf.data(), buf.size(), err, &stage) && stage == "write");
        std::remove(inner.c_str()); rmdir(d2.c_str()); std::remove(f.c_str()); rmdir(dir);
    }
    std::printf("test_export_preflight: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
