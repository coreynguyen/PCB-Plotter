// test_job_folder.cpp — R23 Files tab job folder: sidecar name matching / grouping / work orders, directory scan,
// ISS program view (synthetic XML incl. feeder slots), AOI fiducial alignment math. Synthetic data only.
#include "../src/core/job_archive.h"
#include "../src/core/aoi_align.h"
#include "../src/io/program_view.h"
#include "../src/io/juki_import.h"
#include "../src/io/juki_writer.h"
#include "../src/core/machine_job.h"
#include "../src/core/project.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

static int g_n = 0, g_fail = 0;
#define CHECK(c) do { ++g_n; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
using namespace pcbp;
namespace jz = pcbp::io::jobzip;

static std::vector<jz::SideFile> names(std::initializer_list<const char*> l) {
    std::vector<jz::SideFile> v; for (auto n : l) { jz::SideFile f; f.name = n; v.push_back(f); } return v;
}
static int idx(const std::vector<jz::SideFile>& v, const std::string& n) {
    for (size_t i = 0; i < v.size(); ++i) if (v[i].name == n) return (int)i; return -1;
}

static void test_match() {
    std::printf("== name matching\n");
    const std::vector<std::string> P{"ABC-100-01"};
    std::string rest;
    CHECK(jz::side_name_match("ABC-100-01 [SIDE1].h8h", P, &rest) && rest == " [SIDE1].h8h");
    CHECK(jz::side_name_match("abc-100-01.CPF", P));                       // case-insensitive
    CHECK(jz::side_name_match("ABC.100.01 (Side2).tbl", P));               // '.' == '-'
    CHECK(jz::side_name_match("ABC_100_01.err", P));
    CHECK(!jz::side_name_match("ABC-100-012.h8h", P));                     // no separator after the name
    CHECK(!jz::side_name_match("XYZ-100-01.h8h", P));
    CHECK(!jz::side_name_match("ABC-100", P));
    std::string id; int q = 0;
    CHECK(jz::parse_work_order("ABC-100-01 555123-02-01 x25.xlsx", P, id, q) && id == "555123-02-01" && q == 25);
    CHECK(jz::parse_work_order("abc-100-01 J7 X3.XLSX", P, id, q) && id == "J7" && q == 3);
    CHECK(!jz::parse_work_order("ABC-100-01 BOM.xlsx", P, id, q));
    CHECK(!jz::parse_work_order("ABC-100-01 555 x2.pdf", P, id, q));
    CHECK(!jz::parse_work_order("ABC-100-01 555 xx.xlsx", P, id, q));
}

static void test_group() {
    std::printf("== grouping\n");
    const std::vector<std::string> P{"ABC-100-01"};
    auto l = names({"ABC-100-01 [SIDE2].H82", "ABC-100-01 [SIDE1].h8h", "ABC-100-01 [SIDE1].H81", "ABC-100-01 [SIDE1].H82",
                    "ABC-100-01 [SIDE2].h8h", "ABC-100-01 [SIDE2].H81", "abc-100-01 [side1].jpg", "ABC-100-01 [SIDE2].JPG",
                    "ABC-100-01.iss", "ABC-100-01.i01", "ABC-100-01.I02", "ABC-100-01 555123-02-01 x2.xlsx",
                    "ABC-100-01 [SIDE1].OSL", "ABC-100-01 [SIDE1].mmd", "ABC-100-01.cpf", "ABC.100.01 (Side1).tbl",
                    "ABC-100-01.zip", "ABC-100-01.pcbp", "ABC-100-01.pcbp.autosave", "~$ABC-100-01 1 x1.xlsx",
                    "Other job.h8h", "notes.txt", "ABC-100-01X.h8h", "ABC-100-01 [SIDE3].H81"});
    auto v = jz::build_job_folder(l, P, "ABC-100-01.zip");
    CHECK(v.size() == 18);
    CHECK(idx(v, "ABC-100-01.pcbp") < 0 && idx(v, "Other job.h8h") < 0 && idx(v, "notes.txt") < 0 && idx(v, "ABC-100-01X.h8h") < 0);
    CHECK(idx(v, "ABC-100-01.pcbp.autosave") < 0 && idx(v, "~$ABC-100-01 1 x1.xlsx") < 0);
    const int m1 = idx(v, "ABC-100-01 [SIDE1].h8h"), m2 = idx(v, "ABC-100-01 [SIDE2].h8h"), mi = idx(v, "ABC-100-01.iss");
    CHECK(m1 >= 0 && v[(size_t)m1].kind == jz::SideKind::HlcMaster && v[(size_t)m1].side == "SIDE1");
    CHECK(v[(size_t)m1 + 1].master == m1 && v[(size_t)m1 + 2].master == m1 && v[(size_t)m1 + 1].kind == jz::SideKind::HlcSplit);   // splits nested right below
    CHECK(v[(size_t)idx(v, "ABC-100-01 [SIDE2].H82")].master == m2);
    CHECK(v[(size_t)idx(v, "ABC-100-01.I02")].master == mi && v[(size_t)idx(v, "ABC-100-01.I02")].kind == jz::SideKind::IssSplit);
    const int orphan = idx(v, "ABC-100-01 [SIDE3].H81");
    CHECK(orphan >= 0 && v[(size_t)orphan].master == -1 && v[(size_t)orphan].kind == jz::SideKind::HlcSplit);
    const auto& wo = v[(size_t)idx(v, "ABC-100-01 555123-02-01 x2.xlsx")];
    CHECK(wo.kind == jz::SideKind::WorkOrder && wo.job_id == "555123-02-01" && wo.qty == 2);
    CHECK(v[(size_t)idx(v, "ABC-100-01.zip")].kind == jz::SideKind::Archive && idx(v, "ABC-100-01.zip") == (int)v.size() - 1);
    CHECK(v[(size_t)idx(v, "ABC.100.01 (Side1).tbl")].kind == jz::SideKind::Other && v[(size_t)idx(v, "ABC.100.01 (Side1).tbl")].side == "Side1");
    for (size_t i = 1; i < v.size(); ++i) CHECK(jz::side_group(v[i - 1].kind) <= jz::side_group(v[i].kind));   // groups ordered
    CHECK(jz::aoi_for(v, m1) == idx(v, "abc-100-01 [side1].jpg"));
    CHECK(jz::aoi_for(v, idx(v, "ABC-100-01 [SIDE2].H81")) == idx(v, "ABC-100-01 [SIDE2].JPG"));   // split -> master's picture
    CHECK(jz::aoi_for(v, mi) == -1);                                                              // two pictures, none matches
}

static void test_scan() {
    std::printf("== directory scan\n");
    namespace fs = std::filesystem;
    const fs::path d = fs::temp_directory_path() / "pcbp_r23_jobfolder";
    fs::remove_all(d); fs::create_directories(d);
    for (const char* n : {"JOB-7 [SIDE1].h8h", "JOB-7 [SIDE1].H81", "JOB-7 [SIDE1].jpg", "JOB-7 J-1 x4.xlsx", "JOB-7.zip", "JOB-7.pcbp", "unrelated.cpf"})
        std::ofstream(d / n) << "x";
    fs::create_directories(d / "JOB-7 sub");
    auto v = jz::scan_job_folder(d.string(), {"Job-7"}, "JOB-7.zip");
    CHECK(v.size() == 5);
    CHECK(!v.empty() && v[0].kind == jz::SideKind::HlcMaster && v[1].master == 0 && v[0].size == 1 && !v[0].path.empty() && v[0].mtime > 0);
    CHECK(jz::scan_job_folder("", {"x"}, "").empty() && jz::scan_job_folder((d / "missing").string(), {"x"}, "").empty());
    fs::remove_all(d);
}

static void test_iss_view() {
    std::printf("== ISS program view\n");
    const std::string xml =
        "<?xml version=\"1.0\"?>\n<productionProgram>\n <pwbId>SYN-1</pwbId>\n <core>\n  <pwbData><pwbConfiguration><outline x=\"60\" y=\"40\"/></pwbConfiguration></pwbData>\n"
        "  <component index=\"0\"><componentName>C-100N</componentName><componentSize length=\"1.6\" witdh=\"0.8\" height=\"0.8\"/></component>\n"
        "  <placement index=\"0\"><placementId>C1</placementId><componentName>C-100N</componentName><placementPosition x=\"10\" y=\"12\"/><placementAngle angle=\"90\"/></placement>\n"
        "  <placement index=\"1\"><placementId>C2</placementId><componentName>C-100N</componentName><placementPosition x=\"20\" y=\"12\"/><placementAngle angle=\"0\"/><skip placement=\"YES\"/></placement>\n"
        "  <markPosition x=\"5\" y=\"5\"/><markPosition x=\"55\" y=\"35\"/>\n </core>\n"
        " <machine><pickData><feederPosition>\n  <feeder index=\"3\"><componentName>C-100N</componentName><position stationId=\"1\" bankPos=\"2\" holeNo=\"15\"/></feeder>\n"
        "  <feeder index=\"4\"><componentName>OTHER</componentName></feeder>\n </feederPosition></pickData></machine>\n</productionProgram>\n";
    io::progview::Program p; std::string err;
    CHECK(io::progview::load(std::vector<uint8_t>(xml.begin(), xml.end()), p, err));
    if (!err.empty()) std::printf("  err %s\n", err.c_str());
    CHECK(p.format == "ISS" && p.places.size() == 2 && p.fids.size() == 2);
    CHECK(p.places.size() == 2 && p.places[0].ref == "C1" && std::fabs(p.places[0].x - 10) < 1e-9 && p.places[0].rot == 90 && p.places[0].part == "C-100N");
    CHECK(p.places.size() == 2 && p.places[1].skip && p.places[0].feeder == "St1 Bank2 #15");
    auto f = io::progview::iss_feeders(xml);
    CHECK(f.size() == 2 && f["OTHER"] == "F4");
    CHECK(p.x1 > p.x0 && p.y1 > p.y0);
    CHECK(!io::progview::load({'x', 'y'}, p, err));
    // HLC: ISS -> project -> H8H (writer) -> program view: same placements and marks
    Project prj; juki::ImportResult res;
    CHECK(juki::import_iss(xml, prj, {}, res, err));
    MachineJob job;
    std::vector<uint8_t> h8h;
    CHECK(!prj.model.points.empty() && build_machine_job(prj, 0, job, err) && juki::write_hlc(job, juki::HlcFormat::H8H, h8h, err));
    io::progview::Program q;
    CHECK(io::progview::load(h8h, q, err));
    if (!err.empty()) std::printf("  hlc err %s\n", err.c_str());
    CHECK(q.format == "H8H" && q.places.size() == 2 && q.fids.size() == 2);
    if (q.places.size() == 2) {
        CHECK(q.places[0].ref == "C1" && q.places[0].part == "C-100N" && std::fabs(q.places[0].rot - 90) < 1e-6 && q.places[1].skip);
        CHECK(std::fabs((q.places[1].x - q.places[0].x) - 10) < 1e-3 && std::fabs(q.places[1].y - q.places[0].y) < 1e-3);
        std::printf("  H8H view: C1 at (%.3f, %.3f), board %.1f x %.1f\n", q.places[0].x, q.places[0].y, q.x1 - q.x0, q.y1 - q.y0);
    }
}

static void test_align() {
    std::printf("== AOI alignment\n");
    // board 0..100 x 0..50 mm, image 1000 x 500 px (10 px/mm), Y flipped
    aoi::Affine b = aoi::stretch_to_bbox(1000, 500, 0, 0, 100, 50);
    double x, y; b.apply(0, 50, x, y); CHECK(std::fabs(x) < 1e-9 && std::fabs(y) < 1e-9);
    b.apply(100, 0, x, y); CHECK(std::fabs(x - 1000) < 1e-9 && std::fabs(y - 500) < 1e-9);
    aoi::Affine inv; CHECK(b.inverse(inv)); inv.apply(500, 250, x, y); CHECK(std::fabs(x - 50) < 1e-9 && std::fabs(y - 25) < 1e-9);
    // manual pairs: the two fids map exactly onto the clicked pixels; rotation / scale is a similarity
    aoi::Affine a;
    CHECK(aoi::align_pairs(b, 10, 10, 90, 40, 120, 410, 905, 95, a));
    a.apply(10, 10, x, y); CHECK(std::fabs(x - 120) < 1e-6 && std::fabs(y - 410) < 1e-6);
    a.apply(90, 40, x, y); CHECK(std::fabs(x - 905) < 1e-6 && std::fabs(y - 95) < 1e-6);
    CHECK(!aoi::align_pairs(b, 10, 10, 10, 10, 1, 1, 2, 2, a));
    // synthetic image: dark board, two bright discs (1.35 mm) shifted +1 mm right, 0.5 mm down and rotated 0.5 deg
    const int W = 1000, H = 500;
    std::vector<uint8_t> px((size_t)W * H * 4, 40);
    const double th = 0.5 * 3.14159265358979 / 180.0;
    auto truth = [&](double mx, double my, double& ox, double& oy) {   // true pixel of a board point
        double bx, by; b.apply(mx, my, bx, by);
        const double cx = 500, cy = 250, dx = bx - cx, dy = by - cy;
        ox = cx + std::cos(th) * dx - std::sin(th) * dy + 10; oy = cy + std::sin(th) * dx + std::cos(th) * dy + 5;
    };
    const double F[2][2] = {{10, 10}, {90, 40}};
    for (auto& f : F) {
        double cx, cy; truth(f[0], f[1], cx, cy);
        for (int yy = 0; yy < H; ++yy) for (int xx = 0; xx < W; ++xx)
            if (std::hypot(xx - cx, yy - cy) <= 6.75) for (int k = 0; k < 3; ++k) px[(size_t)(yy * W + xx) * 4 + (size_t)k] = 240;
    }
    std::vector<aoi::FidHit> fids(2);
    fids[0].mm_x = 10; fids[0].mm_y = 10; fids[1].mm_x = 90; fids[1].mm_y = 40;
    CHECK(aoi::auto_align(px.data(), W, H, W * 4, 0, 0, 100, 50, fids, a));
    CHECK(fids[0].found && fids[1].found);
    double tx, ty;
    truth(50, 25, tx, ty); a.apply(50, 25, x, y);
    std::printf("  centre: aligned (%.2f, %.2f) truth (%.2f, %.2f)\n", x, y, tx, ty);
    CHECK(std::fabs(x - tx) < 1.0 && std::fabs(y - ty) < 1.0);
    truth(90, 10, tx, ty); a.apply(90, 10, x, y);
    CHECK(std::fabs(x - tx) < 1.5 && std::fabs(y - ty) < 1.5);
    // nothing to find -> stretch fallback, false
    std::vector<uint8_t> dark((size_t)W * H * 4, 30);
    CHECK(!aoi::auto_align(dark.data(), W, H, W * 4, 0, 0, 100, 50, fids, a));
    a.apply(0, 50, x, y); CHECK(std::fabs(x) < 1e-9 && std::fabs(y) < 1e-9);
}

int main() {
    test_match(); test_group(); test_scan(); test_iss_view(); test_align();
    std::printf("job folder: %d checks, %d failed\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
