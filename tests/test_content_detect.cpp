// ============================================================================
// test_content_detect.cpp — content detection / import planning on synthetic
// nested archives and folders (src/core/content_detect.*).
//   * ODB++ job two levels deep: zip > release/odb.tgz > job/matrix/matrix
//   * Gerbers + Excellon in a sub-folder with an RS-274D aperture list
//   * IPC-2581 inside a zip
//   * mixed BOM + centroid text
//   * directory scan (with a nested zip) + materialize()
//   * single files: .pcbp, ViewMate job, JUKI program extension, xlsx
// ============================================================================
#include "../src/core/content_detect.h"
#include "../src/core/fileio.h"
#include "../src/io/archive.h"
#include "../src/io/deflate.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace pcbp;
using namespace pcbp::detect;

static int g_fail = 0, g_n = 0;
#define CHECK(c, ...) do { ++g_n; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::vector<uint8_t> tar_of(const std::vector<std::pair<std::string, std::string>>& files) {
    std::vector<uint8_t> t;
    for (auto& [name, data] : files) {
        uint8_t h[512] = {};
        std::memcpy(h, name.data(), std::min<size_t>(name.size(), 99));
        std::snprintf((char*)h + 100, 8, "%07o", 0644);
        std::snprintf((char*)h + 108, 8, "%07o", 0);
        std::snprintf((char*)h + 116, 8, "%07o", 0);
        std::snprintf((char*)h + 124, 12, "%011o", (unsigned)data.size());
        std::snprintf((char*)h + 136, 12, "%011o", 0);
        h[156] = '0';
        std::memcpy(h + 257, "ustar\0" "00", 8);
        std::memset(h + 148, ' ', 8);
        unsigned sum = 0;
        for (int i = 0; i < 512; ++i) sum += h[i];
        std::snprintf((char*)h + 148, 8, "%06o", sum);
        h[155] = ' ';
        t.insert(t.end(), h, h + 512);
        t.insert(t.end(), data.begin(), data.end());
        t.resize((t.size() + 511) / 512 * 512);
    }
    t.resize(t.size() + 1024);
    return t;
}

static std::vector<uint8_t> gzip_of(const std::vector<uint8_t>& d) {
    std::vector<uint8_t> g = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 3};
    auto c = deflate_raw(d.data(), d.size(), 6);
    g.insert(g.end(), c.begin(), c.end());
    uint32_t crc = io::crc32_ieee(d.data(), d.size()), n = (uint32_t)d.size();
    for (int i = 0; i < 4; ++i) g.push_back((uint8_t)(crc >> (8 * i)));
    for (int i = 0; i < 4; ++i) g.push_back((uint8_t)(n >> (8 * i)));
    return g;
}
// minimal ZIP writer (deflate when smaller, else stored) — enough for the reader under test
static std::vector<uint8_t> zip_write(const std::vector<std::pair<std::string, std::string>>& files) {
    std::vector<uint8_t> z, cd;
    auto u16 = [](std::vector<uint8_t>& v, unsigned x) { v.push_back(x & 255); v.push_back((x >> 8) & 255); };
    auto u32 = [&](std::vector<uint8_t>& v, uint32_t x) { u16(v, x & 0xFFFF); u16(v, x >> 16); };
    for (auto& [name, data] : files) {
        auto c = deflate_raw((const uint8_t*)data.data(), data.size(), 6);
        const bool st = c.size() >= data.size();
        const uint32_t crc = io::crc32_ieee((const uint8_t*)data.data(), data.size());
        const uint32_t off = (uint32_t)z.size();
        u32(z, 0x04034b50); u16(z, 20); u16(z, 0); u16(z, st ? 0 : 8); u16(z, 0); u16(z, 0x21);
        u32(z, crc); u32(z, st ? data.size() : c.size()); u32(z, data.size()); u16(z, name.size()); u16(z, 0);
        z.insert(z.end(), name.begin(), name.end());
        if (st) z.insert(z.end(), data.begin(), data.end()); else z.insert(z.end(), c.begin(), c.end());
        u32(cd, 0x02014b50); u16(cd, 20); u16(cd, 20); u16(cd, 0); u16(cd, st ? 0 : 8); u16(cd, 0); u16(cd, 0x21);
        u32(cd, crc); u32(cd, st ? data.size() : c.size()); u32(cd, data.size()); u16(cd, name.size()); u16(cd, 0); u16(cd, 0);
        u16(cd, 0); u16(cd, 0); u32(cd, 0); u32(cd, off);
        cd.insert(cd.end(), name.begin(), name.end());
    }
    const uint32_t cdo = (uint32_t)z.size();
    z.insert(z.end(), cd.begin(), cd.end());
    u32(z, 0x06054b50); u16(z, 0); u16(z, 0); u16(z, files.size()); u16(z, files.size()); u32(z, cd.size()); u32(z, cdo); u16(z, 0);
    return z;
}
static std::string S(const std::vector<uint8_t>& v) { return std::string(v.begin(), v.end()); }

static const char* kGbrX = "G04 top copper*\n%FSLAX24Y24*%\n%MOIN*%\n%ADD10C,0.0100*%\n%ADD11R,0.0600X0.0400*%\nD10*\nX0Y0D02*\nX10000Y0D01*\nD11*\nX5000Y5000D03*\nM02*\n";
static const char* kGbrD = "G54D10*\nX000100Y000100D02*\nX000500Y000100D01*\nX000500Y000500D01*\nG54D11*\nX000300Y000300D03*\nX000400Y000300D03*\nM02*\n";
static const char* kDrill = "M48\nINCH,TZ\nT01C0.0280\nT02C0.0400\n%\nT01\nX010000Y010000\nX020000Y010000\nT02\nX030000Y020000\nM30\n";
static const char* kApList = "D10  ROUND   10.0\nD11  RECT    60.0 X 40.0\nD12  ROUND   20.0\nD13  SQUARE  50.0\n";
static const char* kCentroid = "Designator,Footprint,Mid X,Mid Y,Layer,Rotation\nC1,0805,10.0,20.0,T,90\nC2,0805,12.0,20.0,T,0\nR1,0603,15.5,8.25,T,180\nU1,SOIC8,30.0,30.0,T,270\n";
static const char* kBom = "Item,Qty,Reference,Part Number,Description\n1,2,\"C1,C2\",CAP-100N-0805,100nF\n2,1,R1,RES-10K-0603,10k\n3,1,U1,LM358,Op amp\n";
static const char* kIpc = "<?xml version=\"1.0\"?>\n<IPC-2581 revision=\"B\" xmlns=\"http://webstds.ipc.org/2581\"><Content/><Ecad/></IPC-2581>\n";
static const char* kMatrix = "STEP {\n   COL=1\n   NAME=PCB\n}\nLAYER {\n   ROW=1\n   CONTEXT=BOARD\n   TYPE=SIGNAL\n   NAME=TOP\n}\n";

static void test_odb_nested() {
    auto inner = gzip_of(tar_of({{"job1/matrix/matrix", kMatrix}, {"job1/steps/pcb/profile", "S P 0\nOB 0 0 I\nOE\nSE\n"},
                                 {"job1/misc/info", "JOB_NAME=job1\n"}}));
    auto outer = zip_write({{"release/odb.tgz", S(inner)}, {"release/readme.txt", "hello"},
                            {"release/assembly/bom.csv", kBom}});
    Result r;
    CHECK(scan_entries({Entry{"customer.zip", "", outer}}, r));
    const Plan* b = r.best();
    CHECK(b && b->action == Action::ImportOdb);
    CHECK(b && b->files.size() == 1 && b->files[0] == "customer.zip/release/odb.tgz/job1");
    CHECK(b && b->bom.size() == 1);
    // materialize the job folder and check matrix lands at <dest>/matrix/matrix
    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() / "pcbp_cd_test_odb";
    fs::remove_all(tmp);
    std::vector<std::string> w; std::string err;
    CHECK(b && materialize(r, b->files[0], tmp.string(), w, err));
    CHECK(fs::exists(tmp / "matrix" / "matrix"));
    CHECK(fs::exists(tmp / "steps" / "pcb" / "profile"));
    CHECK(w.size() == 3);
    fs::remove_all(tmp);
    std::printf("%s", r.summary().c_str());
}

static void test_gerber_subfolder() {
    auto z = zip_write({{"fab/gerbers/top.gbr", kGbrX}, {"fab/gerbers/bot.gbr", kGbrX}, {"fab/gerbers/drill/nc.drl", kDrill},
                        {"fab/old274d/silk.pho", kGbrD}, {"fab/old274d/apertures/ApertureList.txt", kApList},
                        {"fab/notes.txt", "Board: test\nRev A\n"}});
    Result r;
    CHECK(scan_entries({Entry{"fab.zip", "", z}}, r));
    const Plan* b = r.best();
    CHECK(b && b->action == Action::LoadGerbers);
    CHECK(b && b->files.size() == 3);      // gerbers/ + gerbers/drill/ merged into one set
    int sets = 0, ap = 0;
    for (auto& p : r.plans) if (p.action == Action::LoadGerbers) { ++sets; if (p.files.size() == 1) ap = (int)p.aperture_lists.size(); }
    CHECK(sets == 2);
    CHECK(ap == 1);                        // the 274D set finds the aperture list in its sub-folder
    const Entry* e = r.find("fab.zip/fab/old274d/apertures/ApertureList.txt");
    CHECK(e && e->kind == Kind::ApertureList);
    e = r.find("fab.zip/fab/gerbers/drill/nc.drl");
    CHECK(e && e->kind == Kind::Drill);
    e = r.find("fab.zip/fab/old274d/silk.pho");
    CHECK(e && e->kind == Kind::Gerber);
    std::printf("%s", r.summary().c_str());
}

static void test_ipc_in_zip() {
    auto z = zip_write({{"out/board.xml", kIpc}, {"out/top.gbr", kGbrX}});
    Result r;
    CHECK(scan_entries({Entry{"ipc.zip", "", z}}, r));
    const Plan* b = r.best();
    CHECK(b && b->action == Action::ImportFile && b->importer == "ipc2581" && b->kind == Kind::Ipc2581);
    CHECK(b && b->gerbers.size() == 1);
}

static void test_bom_centroid() {
    auto z = zip_write({{"assy/pick_place.csv", kCentroid}, {"assy/BOM.csv", kBom}});
    Result r;
    CHECK(scan_entries({Entry{"assy.zip", "", z}}, r));
    const Plan* b = r.best();
    CHECK(b && b->action == Action::ImportCentroid);
    CHECK(b && b->files[0] == "assy.zip/assy/pick_place.csv");
    CHECK(b && b->bom.size() == 1 && b->bom[0] == "assy.zip/assy/BOM.csv");
    const Entry* e = r.find("assy.zip/assy/BOM.csv");
    CHECK(e && e->kind == Kind::Bom);
    // KiCad .pos style ('#' commented header, space separated)
    int s = 0; std::string id;
    Kind k = classify("board-top.pos", "### Module positions\n## Unit = mm\n# Ref     Val       Package        PosX       PosY       Rot  Side\nC1 100n C_0805 10.0 -20.0 90.0 top\nR1 10k R_0603 12.0 -21.0 0.0 top\n", s, id);
    CHECK(k == Kind::Centroid);
    // headerless centroid
    k = classify("xy.txt", "C1 10.0 20.0 90 0805\nC2 12.0 20.0 0 0805\nR1 15.5 8.25 180 0603\nU1 30 30 270 SOIC8\n", s, id);
    CHECK(k == Kind::Centroid);
}

static void test_singles() {
    int s = 0; std::string id;
    CHECK(classify("a.pcbp", "{\"format\":\"pcbp\"}", s, id) == Kind::Project && s >= 95);
    std::string jet(64, '\0'); std::memcpy(&jet[4], "Standard Jet DB", 15);
    CHECK(classify("old.cpf", jet, s, id) == Kind::LegacyCpf && id == "circuitcam.cpf");
    CHECK(classify("lib.mdb", jet, s, id) == Kind::LegacyMdb);
    std::string vm = std::string("Lavenir Technology V2001 JOB V12\r\n") + std::string(40, '\0');
    CHECK(classify("Sample1up.bin", vm, s, id) == Kind::ViewMateJob && id == "viewmate.bin");
    CHECK(classify("other.bin", std::string(64, '\x07'), s, id) == Kind::Unknown);
    CHECK(classify("prog.h8h", std::string(600, '\0'), s, id) == Kind::PnpProgram && id == "juki.hlc");
    CHECK(classify("bom.xlsx", "PK\x03\x04", s, id) == Kind::Spreadsheet);
    CHECK(classify("x.xml", kIpc, s, id) == Kind::Ipc2581);
    CHECK(classify("top.gtl", kGbrX, s, id) == Kind::Gerber);
    CHECK(classify("nc.drl", kDrill, s, id) == Kind::Drill);
    CHECK(classify("job/matrix/matrix", kMatrix, s, id) == Kind::Odb);
    // xlsx inside a zip is not expanded as an archive
    auto x = zip_write({{"[Content_Types].xml", "<Types/>"}});
    auto z = zip_write({{"bom.xlsx", S(x)}});
    Result r;
    CHECK(scan_entries({Entry{"s.zip", "", z}}, r));
    CHECK(r.entries.size() == 1 && r.entries[0].kind == Kind::Spreadsheet);
    CHECK(r.best() && r.best()->action == Action::OpenSheet);
    // registry sniff hook (GenCAD)
    Options o;
    o.sniff = [](const std::string&, const std::string& head, std::string& idd, Kind& kk) { if (head.rfind("$HEADER", 0) == 0) { idd = "gencad"; kk = Kind::Cad; return 90; } return 0; };
    Result r2;
    CHECK(scan_entries({Entry{"d.cad", "", std::vector<uint8_t>((const uint8_t*)"$HEADER\nGENCAD 1.4\n$ENDHEADER\n", (const uint8_t*)"$HEADER\nGENCAD 1.4\n$ENDHEADER\n" + 30)}}, r2, o));
    CHECK(r2.best() && r2.best()->importer == "gencad" && r2.best()->action == Action::ImportFile);
}

static void test_directory() {
    namespace fs = std::filesystem;
    fs::path d = fs::temp_directory_path() / "pcbp_cd_test_dir";
    fs::remove_all(d);
    fs::create_directories(d / "cam" / "odb_job" / "matrix");
    std::string err;
    write_file((d / "cam" / "odb_job" / "matrix" / "matrix").string(), kMatrix, std::strlen(kMatrix), err);
    auto z = zip_write({{"layers/top.gbr", kGbrX}});
    write_file((d / "cam" / "layers.zip").string(), z.data(), z.size(), err);
    write_file((d / "xy.csv").string(), kCentroid, std::strlen(kCentroid), err);
    Result r;
    CHECK(scan(d.string(), r, err));
    CHECK(r.source_is_dir);
    const Plan* b = r.best();
    CHECK(b && b->action == Action::ImportOdb && b->files[0] == "cam/odb_job");
    const Entry* e = r.find("cam/layers.zip/layers/top.gbr");
    CHECK(e && e->kind == Kind::Gerber && e->disk_path.empty());
    e = r.find("xy.csv");
    CHECK(e && !e->disk_path.empty() && e->kind == Kind::Centroid);
    bool g = false;
    for (auto& p : r.plans) g |= p.action == Action::LoadGerbers;
    CHECK(g);
    // plain archive file source
    Result r3;
    CHECK(scan((d / "cam" / "layers.zip").string(), r3, err));
    CHECK(r3.source_is_archive && r3.best() && r3.best()->action == Action::LoadGerbers && r3.best()->files[0] == "layers/top.gbr");
    std::printf("%s", r.summary().c_str());
    fs::remove_all(d);
}

int main() {
    test_odb_nested();
    test_gerber_subfolder();
    test_ipc_in_zip();
    test_bom_centroid();
    test_singles();
    test_directory();
    std::printf("content_detect: %d/%d checks passed\n", g_n - g_fail, g_n);
    return g_fail ? 1 : 0;
}
