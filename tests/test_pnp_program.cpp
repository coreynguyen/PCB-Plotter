// ============================================================================
// test_pnp_program.cpp — Program Editor model (src/io/pnp_program.*).
//
//  (a) every HLC flavour juki_writer produces (H8H/H7H/H5H/H4H v6+v5/E41/E45/E51):
//      load -> save unmodified is byte-identical; field edits land only where
//      expected (parse_hlc oracle); delete / clone / reorder keep block 3, block 12,
//      component counts and line-info totals consistent.
//  (b) an H8H with extra opaque blocks (feeder / station / MTC stand-ins) and a
//      matrix board: unknown blocks survive a resize byte-for-byte.
//  (c) synthetic ISS XML (CRLF): unedited round trip identical; edits patch only
//      their values; part rename cascades to every <componentName>.
//  (d) conversion H7H -> H8H / H5H / H4H keeps placements; validation finds issues.
//  (e) any real *.h?h / *.iss found under the local program_dirs is
//      round-tripped (unmodified -> identical).
// ============================================================================
#include "local_data.h"
#include "../src/core/fileio.h"
#include "../src/core/machine_job.h"
#include "../src/io/juki_writer.h"
#include "../src/io/pnp_program.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <functional>
#include <string>
#include <sys/stat.h>

using namespace pcbp;
using namespace pcbp::io;
namespace pj = pcbp::juki;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static MachineJob make_job() {
    MachineJob j;
    j.program_name = "PNP-EDIT_RevB";
    j.assembly = "PNP-EDIT";
    j.board_w = 120.5; j.board_h = 80.25; j.board_t = 1.6;
    // Written as one Single PWB holding all 12 placements (the editor tests need
    // every record); Matrix / Non-Matrix templates are covered in test_juki.
    j.single_board = true;
    struct P { const char* ref; const char* part; double x, y, r; bool skip; };
    const P ps[] = {
        {"C1", "CAP-0603-100N", 10.0, 5.0, 0, false}, {"C2", "CAP-0603-100N", 12.5, 5.0, 90, false},
        {"R1", "RES-0402-10K", 20.0, 30.0, 270, false}, {"R2", "RES-0402-10K", 22.0, 30.0, 0, true},
        {"U1", "MCU-QFP64", 40.0, 40.0, 0, false}, {"C3", "CAP-0603-100N", 50.0, 70.0, 45, false},
        // circuit 2 (rotated 180 about origin, offset)
        {"C1", "CAP-0603-100N", 50.25, -5.0, 180, false}, {"C2", "CAP-0603-100N", 47.75, -5.0, 270, false},
        {"R1", "RES-0402-10K", 40.25, -30.0, 90, false}, {"R2", "RES-0402-10K", 38.25, -30.0, 180, true},
        {"U1", "MCU-QFP64", 20.25, -40.0, 180, false}, {"C3", "CAP-0603-100N", 10.25, -70.0, 225, false},
    };
    int k = 0;
    for (const auto& p : ps) {
        JobPlacement q; q.ref = p.ref; q.part = p.part; q.x = p.x; q.y = p.y; q.rot = p.r; q.skip = p.skip; q.circuit = k++ < 6 ? 1 : 2;
        j.placements.push_back(q);
    }
    const char* parts[] = {"CAP-0603-100N", "RES-0402-10K", "MCU-QFP64", "UNUSED-PART"};
    for (auto* n : parts) { JobPart p; p.part = n; p.description = std::string("desc ") + n; j.parts.push_back(p); }
    JobFiducial f1; f1.name = "FID1"; f1.x = 3.0; f1.y = 3.0;
    JobFiducial f2; f2.name = "FID2"; f2.x = 57.0; f2.y = 77.0;
    JobFiducial f3; f3.name = "LM1"; f3.x = 35.0; f3.y = 35.0; f3.local = true; f3.owner = "U1";
    JobFiducial f4; f4.name = "LM2"; f4.x = 45.0; f4.y = 45.0; f4.local = true; f4.owner = "U1";
    j.fiducials = {f1, f2, f3, f4};
    JobCircuit c1; c1.index = 1; JobCircuit c2; c2.index = 2; c2.x = 60.25; c2.y = 0; c2.rot = 180;
    c2.ax = 60.25; c2.ay = 0; c2.arot = 180;   // circuit 1 -> 2 map (HLC_FORMAT.md §5)
    j.circuits = {c1, c2};
    j.options["juki.timestamp"] = "1700000000";
    j.options["juki.local.U1"] = "LM1,LM2";
    j.options["juki.circuit_w"] = "60";
    j.options["juki.circuit_h"] = "80.25";
    j.options["juki.comment"] = "editor test";
    return j;
}

static bool roundtrip_identical(const std::vector<uint8_t>& d, const char* label) {
    PnpProgram p; std::string err;
    if (!pnp_load(d, p, err)) { std::printf("  %s: load failed: %s\n", label, err.c_str()); return false; }
    std::vector<uint8_t> o;
    if (!pnp_save(p, o, err)) { std::printf("  %s: save failed: %s\n", label, err.c_str()); return false; }
    if (o != d) {
        size_t k = 0; while (k < o.size() && k < d.size() && o[k] == d[k]) ++k;
        std::printf("  %s: not identical (first diff @%zu, %zu vs %zu bytes)\n", label, k, o.size(), d.size());
        return false;
    }
    return !pnp_modified(p);
}

static size_t count_diff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t n = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) n += a[i] != b[i];
    return n + (a.size() > b.size() ? a.size() - b.size() : b.size() - a.size());
}

static void test_variant(pj::HlcVariant v) {
    const char* ext = pj::hlc_extension(v);
    std::vector<uint8_t> d; std::string err;
    MachineJob job = make_job();
    CHECK(pj::write_hlc_variant(job, v, d, err));
    CHECK(roundtrip_identical(d, ext));
    if (getenv("PNP_DUMP")) { std::string e2; write_file(std::string("/tmp/claude-0/r8c/v.") + ext, d.data(), d.size(), e2); }
    PnpProgram p;
    CHECK(pnp_load(d, p, err));
    CHECK(p.placements.size() == 12);
    CHECK(p.parts.size() == 4);
    CHECK(p.circuits.size() <= 1);   // Single PWB: no circuit list
    CHECK(p.placements[3].skip);
    CHECK(p.placements[4].has_local);
    int glob = 0, loc = 0;
    for (auto& m : p.marks) { glob += m.kind == MarkKind::Global; loc += m.kind == MarkKind::Local; }
    if (!(glob == 2 && loc == 4)) std::printf("glob=%d loc=%d\n", glob, loc);
    CHECK(glob == 2 && loc == 4);   // one local pair per U1 (2 circuits)
    CHECK(p.board.comment == "editor test" || std::string(ext) != "H7H");

    // ---- field edits: only the edited values change
    PnpProgram e = p;
    e.placements[1].x = 13.25; e.placements[1].rot = 180; e.placements[1].skip = true;
    e.placements[2].ref = "R10";
    e.placements[5].part = 1;     // C3 -> RES
    e.parts[0].desc = "Cap 100n";
    e.board.w = 121.0;
    std::vector<uint8_t> o;
    std::vector<std::string> notes;
    CHECK(pnp_save(e, o, err, &notes));
    CHECK(o.size() == d.size());
    pj::HlcParsed a, b;
    CHECK(pj::parse_hlc(d, a, err) && pj::parse_hlc(o, b, err));
    CHECK(b.placements[1].x == 132500 && b.placements[1].angle == 14400 && (b.placements[1].flags & 1));
    CHECK(b.names[2] == "R10" && b.placements[2].ref == "R10");
    CHECK(b.placements[5].comp == 1);
    CHECK(b.comp_counts[0] == a.comp_counts[0] - 1 && b.comp_counts[1] == a.comp_counts[1] + 1);
    CHECK(b.comp_desc[0] == "Cap 100n");
    CHECK(b.pcb_w == 1210000);
    PnpProgram r;
    CHECK(pnp_load(o, r, err));
    CHECK(r.placements[1].x == 13.25 && r.placements[2].ref == "R10" && r.placements[5].part == 1 && r.parts[0].desc == "Cap 100n");
    // untouched placement records identical
    for (int i : {0, 3, 4, 6, 7, 8, 9, 10, 11}) CHECK(a.placements[(size_t)i].x == b.placements[(size_t)i].x && a.placements[(size_t)i].comp == b.placements[(size_t)i].comp);

    // ---- structure: delete 2 rows, clone one, reorder
    PnpProgram s = p;
    CHECK(s.can_restructure());
    s.placements.erase(s.placements.begin() + 9);   // R2 circuit 2
    s.placements.erase(s.placements.begin() + 0);   // C1 circuit 1
    PnpPlacement cl = s.placements[0];               // clone C2 as C9
    cl.ref = "C9"; cl.x += 1.0;
    s.placements.push_back(cl);
    std::swap(s.placements[1], s.placements[2]);
    CHECK(pnp_save(s, o, err, &notes));
    CHECK(pj::parse_hlc(o, b, err));
    CHECK(b.placements.size() == 11 && b.names.size() == 11);
    CHECK(b.li_place_count == 11 && b.li_place_count2 == 11);
    CHECK(b.names[10] == "C9" && b.placements[10].x == a.placements[1].x + 10000 && b.placements[10].comp == a.placements[1].comp);
    CHECK(b.names[1] == p.placements[3].ref && b.names[2] == p.placements[2].ref);
    {
        int cap = 0;
        for (auto& q : b.placements) cap += q.comp == 0;
        CHECK(b.comp_counts[0] == cap);
    }
    CHECK(pnp_load(o, r, err));
    CHECK(r.placements.size() == 11 && r.placements[10].ref == "C9");
    CHECK(roundtrip_identical(o, "restructured"));
    // local mark link survives the reorder
    bool u1_local = false;
    for (auto& q : r.placements) if (q.ref == "U1" && q.has_local) u1_local = true;
    CHECK(u1_local);
    std::printf("  %s ok (%zu bytes, edit diff %zu bytes)\n", ext, d.size(), count_diff(d, o));
}

// ---- (b) H8H with opaque extra blocks + matrix board ------------------------
static std::vector<uint8_t> add_blocks(const std::vector<uint8_t>& in) {
    // rebuild the file with blocks 5 (feeders), 11 (station) and 14 (MTC) filled with a pattern
    pj::HlcParsed hp; std::string err;
    pj::parse_hlc(in, hp, err);
    const size_t nb = hp.blocks.size();
    std::vector<std::vector<uint8_t>> pay(nb);
    for (size_t i = 0; i < nb; ++i) {
        const auto& b = hp.blocks[i];
        if (b.addr) pay[i].assign(in.begin() + b.addr, in.begin() + b.addr + b.prefix + (size_t)b.recsize * b.count);
    }
    auto fill = [&](size_t i, uint32_t rec, uint32_t cnt, uint8_t seed) {
        hp.blocks[i] = {1, 0, rec, cnt};
        pay[i].resize((size_t)rec * cnt);
        for (size_t k = 0; k < pay[i].size(); ++k) pay[i][k] = (uint8_t)(seed + k * 7);
    };
    fill(5, 28416, 1, 3); fill(11, 2427, 1, 9); fill(14, 256, 40, 21);
    std::vector<uint8_t> o(in.begin(), in.begin() + 38 + 16 * nb);
    for (size_t i = 0; i < nb; ++i) {
        auto& b = hp.blocks[i];
        if (!b.addr) continue;
        b.addr = (uint32_t)o.size();
        o.insert(o.end(), pay[i].begin(), pay[i].end());
    }
    for (size_t i = 0; i < nb; ++i) {
        const size_t e = 38 + 16 * i;
        const uint32_t v[4] = {hp.blocks[i].addr, hp.blocks[i].prefix, hp.blocks[i].recsize, hp.blocks[i].count};
        for (int k = 0; k < 4; ++k) for (int j = 0; j < 4; ++j) o[e + 4 * k + j] = (uint8_t)(v[k] >> (8 * j));
    }
    return o;
}

static void test_h8h_opaque() {
    MachineJob job = make_job();
    // root circuit only + matrix layout (TTP style): keep the first 6 placements
    job.placements.resize(6);
    job.circuits.clear();
    job.single_board = true;
    std::vector<uint8_t> d; std::string err;
    CHECK(pj::write_hlc(job, pj::HlcFormat::H8H, d, err));
    pj::HlcParsed hp; pj::parse_hlc(d, hp, err);
    const size_t b = hp.blocks[2].addr;
    d[b + 42] = (uint8_t)(d[b + 42] | 0x08);                    // matrix
    auto w32 = [&](size_t o, int32_t v) { for (int j = 0; j < 4; ++j) d[o + j] = (uint8_t)((uint32_t)v >> (8 * j)); };
    d[b + 160] = 3; d[b + 161] = 0; d[b + 162] = 2; d[b + 163] = 0;  // 3 x 2
    w32(b + 164, 0); w32(b + 168, 0); w32(b + 172, 400000); w32(b + 176, 300000);
    const size_t c0 = hp.blocks[4].addr;                         // component 0 body 1.6 x 0.8 x 0.45, skip comp 2
    w32(c0 + 81, 16000); w32(c0 + 85, 8000); w32(c0 + 89, 4500);
    d[c0 + 1822 * 2 + 716] |= 1;
    d = add_blocks(d);
    CHECK(roundtrip_identical(d, "H8H+opaque"));
    PnpProgram p;
    CHECK(pnp_load(d, p, err));
    CHECK(p.board.layout == 1 && p.circuits.size() == 6 && p.circuits[4].x == 40.0 && p.circuits[4].y == 30.0);
    CHECK(p.parts[0].has_body && std::fabs(p.parts[0].len - 1.6) < 1e-9 && p.parts[2].skip);
    p.placements.erase(p.placements.begin() + 2);
    p.parts[2].skip = false;
    p.parts[0].wid = 0.85;
    p.board.pitch_x = 41.0;
    std::vector<uint8_t> o;
    CHECK(pnp_save(p, o, err));
    pj::HlcParsed a2, b2;
    pj::parse_hlc(d, a2, err);
    CHECK(pj::parse_hlc(o, b2, err));
    CHECK(b2.placements.size() == 5);
    for (int i : {5, 11, 14}) {
        const auto& x = a2.blocks[(size_t)i]; const auto& y = b2.blocks[(size_t)i];
        CHECK(x.recsize == y.recsize && x.count == y.count);
        CHECK(std::equal(d.begin() + x.addr, d.begin() + x.addr + (size_t)x.recsize * x.count, o.begin() + y.addr));
    }
    PnpProgram r;
    CHECK(pnp_load(o, r, err));
    CHECK(!r.parts[2].skip && std::fabs(r.parts[0].wid - 0.85) < 1e-9 && std::fabs(r.board.pitch_x - 41.0) < 1e-9);
    CHECK(o.size() == d.size() - 256 - 32);
    std::printf("  H8H opaque blocks ok (%zu -> %zu bytes)\n", d.size(), o.size());
}

// ---- (c) ISS -------------------------------------------------------------------
static const char* kIss =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
    "<productionProgram version=\"1\">\r\n"
    "  <headerData>\r\n    <targetMachine>FX-3</targetMachine>\r\n"
    "    <lineConfiguration>\r\n      <lineName id=\"0\">MainLine</lineName>\r\n    </lineConfiguration>\r\n  </headerData>\r\n"
    "  <pwbId>ISS-TEST_A</pwbId>\r\n"
    "  <core>\r\n    <pwbData>\r\n      <pwbConfiguration><outline x=\"150.5\" y=\"100\" /></pwbConfiguration>\r\n"
    "      <circuitConfigurationData>\r\n        <circuitConfiguration index=\"0\">\r\n"
    "          <circuitConfiguration>NONMATRIX</circuitConfiguration>\r\n"
    "          <circuitOutline x=\"70\" y=\"90\" />\r\n"
    "          <nonMatrix>\r\n            <totalCount count=\"2\" />\r\n"
    "            <allocation index=\"0\" x=\"0\" y=\"0\" angle=\"0\" rangeOver=\"0\" />\r\n"
    "            <allocation index=\"1\" x=\"75\" y=\"0\" angle=\"0\" rangeOver=\"0\" />\r\n"
    "          </nonMatrix>\r\n        </circuitConfiguration>\r\n      </circuitConfigurationData>\r\n    </pwbData>\r\n"
    "    <componentData>\r\n"
    "      <component><componentName>R-10K</componentName><componentBasic><comment>Res 10k &amp; 1%</comment>"
    "<componentSize length=\"1\" witdh=\"0.5\" height=\"0.35\" /></componentBasic></component>\r\n"
    "      <component><componentName>C-100N</componentName><componentBasic><comment>Cap</comment>"
    "<componentSize length=\"1.6\" witdh=\"0.8\" height=\"0.8\" /></componentBasic></component>\r\n"
    "    </componentData>\r\n"
    "    <placementData>\r\n"
    "      <placement index=\"1\"><placementId>R1</placementId><componentName>R-10K</componentName><baseCircuitId>1</baseCircuitId>"
    "<placementPosition x=\"10.5\" y=\"20\" rangeOver=\"False\" /><placementAngle angle=\"90\" /><skip placement=\"NO\" />"
    "<attribute><station placement=\"1\" /><headUnit placement=\"1\" /><head placement=\"2\" /></attribute></placement>\r\n"
    "      <placement index=\"2\"><placementId>C1</placementId><componentName>C-100N</componentName><baseCircuitId>1</baseCircuitId>"
    "<placementPosition x=\"30\" y=\"40.25\" rangeOver=\"False\" /><placementAngle angle=\"0\" /><skip placement=\"NO\" /></placement>\r\n"
    "      <placement index=\"3\"><placementId>R2</placementId><componentName>R-10K</componentName><baseCircuitId>1</baseCircuitId>"
    "<placementPosition x=\"35\" y=\"40.25\" rangeOver=\"False\" /><placementAngle angle=\"180\" /><skip placement=\"YES\" /></placement>\r\n"
    "    </placementData>\r\n"
    "  </core>\r\n"
    "  <model>\r\n    <pwbData><pwbConfiguration><pwbLayoutOffset x=\"0\" y=\"0\" /></pwbConfiguration>"
    "<circuitConfigurationData><circuitConfiguration index=\"0\"><circuitLayoutOffset x=\"5\" y=\"5\" /></circuitConfiguration></circuitConfigurationData></pwbData>\r\n"
    "    <componentData>\r\n"
    "      <component><componentName>R-10K</componentName><packageData><package>TAPE</package></packageData>"
    "<placeCondition><componentSkip>NO</componentSkip></placeCondition></component>\r\n"
    "      <component><componentName>C-100N</componentName><packageData><package>TAPE</package></packageData></component>\r\n"
    "    </componentData>\r\n"
    "    <placementData>\r\n"
    "      <placement index=\"1\"><placementId>R1</placementId><layer placement=\"1\" /><action placement=\"Place\" /></placement>\r\n"
    "      <placement index=\"2\"><placementId>C1</placementId><layer placement=\"1\" /><action placement=\"Place\" /></placement>\r\n"
    "      <placement index=\"3\"><placementId>R2</placementId><layer placement=\"1\" /><action placement=\"Place\" /></placement>\r\n"
    "    </placementData>\r\n"
    "    <bocMarkData><bocMark><markPosition x=\"3\" y=\"3\" /><markPosition x=\"147.5\" y=\"97\" /></bocMark>"
    "<badMarkData><badMark><markPosition x=\"60\" y=\"5\" /></badMark></badMarkData></bocMarkData>\r\n"
    "  </model>\r\n"
    "  <productionProgram>\r\n    <machine><pickData><pickPositionData index=\"0\"><feeder index=\"0\"><componentName>R-10K</componentName>"
    "<position stationId=\"1\" bankKind=\"0\" holeNo=\"3\" /></feeder></pickPositionData></pickData></machine>\r\n  </productionProgram>\r\n"
    "</productionProgram>\r\n";

static void test_iss() {
    std::vector<uint8_t> d(kIss, kIss + std::strlen(kIss));
    CHECK(pnp_sniff(d) >= 90);
    CHECK(roundtrip_identical(d, "ISS"));
    if (getenv("PNP_DUMP")) { std::string e2; write_file("/tmp/claude-0/r8c/v.iss", d.data(), d.size(), e2); }
    PnpProgram p; std::string err;
    CHECK(pnp_load(d, p, err));
    CHECK(p.kind == PnpKind::Iss && p.placements.size() == 3 && p.parts.size() == 2);
    CHECK(p.board.program_name == "ISS-TEST_A" && p.board.w == 150.5 && p.board.line_name == "MainLine");
    CHECK(p.board.layout == 2 && p.circuits.size() == 2 && p.circuits[1].x == 75);
    CHECK(p.parts[0].desc == "Res 10k & 1%" && p.parts[0].wid == 0.5 && p.parts[0].package == "TAPE" && p.parts[0].skip_known);
    CHECK(p.placements[0].rot == 90 && p.placements[0].station == 1 && p.placements[0].head == 102);
    CHECK(p.placements[2].skip && !p.placements[1].skip);
    CHECK(p.marks.size() == 3 && p.marks[2].kind == MarkKind::Bad && p.marks[0].kind == MarkKind::Global);
    CHECK(!p.can_restructure());
    PnpProgram e = p;
    e.placements[1].x = 31.125;
    e.placements[1].ref = "C10";
    e.placements[2].skip = false;
    e.placements[0].part = 1;
    e.parts[0].name = "R-10K-1%";
    e.parts[1].desc = "Cap <100n>";
    e.parts[0].skip = true;
    e.circuits[1].x = 76.5;
    e.board.w = 151;
    e.marks[1].y = 96.5;
    std::vector<uint8_t> o; std::vector<std::string> notes;
    CHECK(pnp_save(e, o, err, &notes));
    std::string s(o.begin(), o.end());
    CHECK(s.find("<placementPosition x=\"31.125\" y=\"40.25\"") != std::string::npos);
    CHECK(s.find("<placementId>C10</placementId><layer") != std::string::npos);   // model record follows
    CHECK(s.find("<componentName>R-10K-1%</componentName>") != std::string::npos);
    CHECK(s.find("<componentName>R-10K</componentName>") == std::string::npos);    // every reference renamed (feeder too)
    CHECK(s.find("<comment>Cap &lt;100n&gt;</comment>") != std::string::npos);
    CHECK(s.find("<componentSkip>YES</componentSkip>") != std::string::npos);
    CHECK(s.find("x=\"76.5\"") != std::string::npos && s.find("<outline x=\"151\"") != std::string::npos);
    CHECK(s.find("<markPosition x=\"147.5\" y=\"96.5\"") != std::string::npos);
    CHECK(s.find("\r\n") != std::string::npos && s.find("\n  <model>") != std::string::npos);
    PnpProgram r;
    CHECK(pnp_load(o, r, err));
    CHECK(r.placements[1].ref == "C10" && r.placements[1].x == 31.125 && !r.placements[2].skip);
    CHECK(r.parts[0].name == "R-10K-1%" && r.placements[0].part == 1 && r.placements[2].part == 0 && r.parts[0].skip);
    // structural edits are refused for ISS
    PnpProgram bad = p;
    bad.placements.pop_back();
    CHECK(!pnp_save(bad, o, err));
    // ISS -> H8H conversion
    std::vector<std::string> lossy;
    CHECK(pnp_convert(p, "H8H", o, lossy, err));
    PnpProgram c;
    CHECK(pnp_load(o, c, err) && c.format == "H8H" && c.placements.size() == 3 && !lossy.empty());
    std::printf("  ISS ok (%zu bytes, %zu notes)\n", d.size(), notes.size());
}

static void test_convert_validate() {
    std::vector<uint8_t> d; std::string err;
    CHECK(pj::write_hlc(make_job(), pj::HlcFormat::H7H, d, err));
    PnpProgram p;
    CHECK(pnp_load(d, p, err));
    for (const auto& t : pnp_convert_targets(p)) {
        std::vector<uint8_t> o; std::vector<std::string> lossy;
        CHECK(pnp_convert(p, t, o, lossy, err));
        PnpProgram c;
        CHECK(pnp_load(o, c, err));
        CHECK(c.format == t && c.placements.size() == p.placements.size());
        bool same = true;
        for (size_t i = 0; i < p.placements.size() && i < c.placements.size(); ++i)
            if (std::fabs(c.placements[i].x - p.placements[i].x) > 1e-9 || c.placements[i].ref != p.placements[i].ref) same = false;
        CHECK(same);
        int loc = 0;
        for (auto& m : c.marks) loc += m.kind == MarkKind::Local;
        CHECK(loc == 4);
    }
    // validation
    auto issues = pnp_validate(p);
    bool unused = false;
    for (auto& i : issues) if (i.table == 1 && i.text.find("UNUSED-PART") != std::string::npos) unused = true;
    CHECK(unused);
    PnpProgram e = p;
    e.placements[1].ref = "C1";            // duplicate in circuit 1
    e.placements[2].x = 500;               // off board
    e.placements[3].part = -1;
    int dup = 0, off = 0, nolink = 0;
    for (auto& i : pnp_validate(e)) {
        if (i.text.find("duplicate reference C1") != std::string::npos && i.row == 1) ++dup;
        if (i.text.find("outside the board") != std::string::npos && i.row == 2) ++off;
        if (i.text.find("no component") != std::string::npos && i.row == 3) ++nolink;
    }
    CHECK(dup == 1 && off == 1 && nolink == 1);
    const std::string csv = pnp_csv(p);
    CHECK(csv.find("Step,Ref,Part") == 0 && csv.find("\r\n12,C3,CAP-0603-100N") != std::string::npos);
    CHECK(pnp_circuit_frames(p).size() <= 1);   // Single PWB
}

// Circuit frames and placements use the same circuit transform (rotation about the machine
// origin + offset): every circuit-2 placement (rotated 180) lies in frame 2 exactly when its
// circuit-1 counterpart lies in frame 1. A skipped (x-out) unlinked placement is a warning only.
static void test_circuit_frames() {
    // Non-Matrix program: circuit 1 template + circuit list (HLC_FORMAT.md §5).
    std::vector<uint8_t> d; std::string err;
    MachineJob job = make_job();
    job.single_board = false;
    job.options.erase("juki.circuit_w"); job.options.erase("juki.circuit_h");
    job.has_circuit = true; job.circuit_x = 2; job.circuit_y = 1; job.circuit_w = 58; job.circuit_h = 78;
    CHECK(pj::write_hlc(job, pj::HlcFormat::H8H, d, err));
    PnpProgram p;
    CHECK(pnp_load(d, p, err));
    auto fr = pnp_circuit_frames(p);
    CHECK(fr.size() == 2);
    CHECK(p.placements.size() == 6);
    Vec2 off; double rot = 0;
    CHECK(pnp_circuit_xform(p, 1, off, rot));
    for (size_t i = 0; i < 6 && i < p.placements.size(); ++i) {
        const auto& a = p.placements[i];
        const auto& b = job.placements[i + 6];
        const Vec2 t = rotate({a.x, a.y}, rot) + off;
        CHECK(std::fabs(t.x - b.x) < 1e-4 && std::fabs(t.y - b.y) < 1e-4);
    }
    // this legacy test panel turns circuit 2 about the machine origin, off the board:
    // the validator must say so
    bool offb = false;
    for (auto& c : pj::validate_hlc(d)) offb |= c.error && c.msg.find("circuit 2 lies outside the board") != std::string::npos;
    CHECK(offb);
}

static void scan_dir(const std::string& dir, int depth, const std::function<void(const std::string&)>& fn) {
    if (depth > 5) return;
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        std::string p = dir + "/" + n;
        struct stat st;
        if (lstat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (n != "proc" && n != "pfx" && n != "obj") scan_dir(p, depth + 1, fn); continue; }
        std::string ext;
        size_t k = n.find_last_of('.');
        if (k != std::string::npos) for (char c : n.substr(k + 1)) ext += (char)std::tolower((unsigned char)c);
        if (pnp_is_program_ext(ext)) fn(p);
    }
    closedir(d);
}

int main() {
    std::printf("== HLC variants\n");
    for (auto v : {pj::HlcVariant::H8H, pj::HlcVariant::H7H, pj::HlcVariant::H5H, pj::HlcVariant::H4H_v6, pj::HlcVariant::H4H_v5,
                   pj::HlcVariant::E41, pj::HlcVariant::E45, pj::HlcVariant::E51})
        test_variant(v);
    std::printf("== H8H opaque blocks\n");
    test_h8h_opaque();
    std::printf("== ISS\n");
    test_iss();
    std::printf("== convert / validate\n");
    test_convert_validate();
    test_circuit_frames();
    std::printf("== programs on disk\n");
    int found = 0;
    for (const std::string& root : pcbp_test::local_list("program_dirs"))
        scan_dir(root.c_str(), 0, [&](const std::string& f) {
            std::vector<uint8_t> d; std::string err;
            if (!read_file(f, d, err) || d.size() < 64 || pnp_sniff(d) < 50) return;
            ++found;
            const bool ok = roundtrip_identical(d, f.c_str());
            CHECK(ok);
        });
    std::printf("  %d program file(s) round-tripped\n", found);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
