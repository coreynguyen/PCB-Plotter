// ============================================================================
// test_juki.cpp — JUKI HLC writer self-test.
//
// Builds a single-PWB MachineJob (10 placements, 3 parts, 2 fiducials),
// writes every supported variant, re-parses it and checks block-table
// geometry and value round-trip; then checks Single / Matrix / Non-Matrix
// circuit configuration, fiducial modes and the validator (HLC_FORMAT.md). With -DPCBP_TEST_WITH_TTP the H7H/H8H output
// is also loaded by Corey's TrayTagPrinter H8H parser (juki::load_h8h).
//
// Build (Linux):
//   g++ -std=c++20 -O1 -Wall -I src tests/test_juki.cpp src/io/juki_writer.cpp -o test_juki
//   (+ -DPCBP_TEST_WITH_TTP -I <TTP>/src <TTP>/src/juki/{juki_io_util,juki_variants,
//      juki_h8h_doc,juki_h8h_io}.cpp)
// ============================================================================
#include "io/juki_writer.h"
#include "core/project.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#ifdef PCBP_TEST_WITH_TTP
#include "juki/juki_h8h_doc.h"
#include "juki/juki_h8h_io.h"
#endif

using namespace pcbp;
namespace pj = pcbp::juki;
static uint16_t t_u16(const std::vector<uint8_t>& b, size_t o) { return (uint16_t)(b[o] | b[o + 1] << 8); }
static uint32_t t_u32(const std::vector<uint8_t>& b, size_t o) { return t_u16(b, o) | (uint32_t)t_u16(b, o + 2) << 16; }

static int g_fail = 0;
#define CHECK(c, ...)                                                   \
    do {                                                                \
        if (!(c)) {                                                     \
            ++g_fail;                                                   \
            std::printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #c);    \
            std::printf(__VA_ARGS__);                                   \
            std::printf("\n");                                          \
        }                                                               \
    } while (0)

static MachineJob make_job() {
    MachineJob j;
    j.program_name = "PCBP-TEST_RevA";
    j.assembly = "PCBP-TEST";
    j.board_w = 120.5;
    j.board_h = 80.25;
    j.board_t = 1.6;
    j.single_board = true;
    struct P { const char* ref; const char* part; double x, y, r; bool skip; };
    const P ps[] = {
        {"C1", "CAP-0603-100N", 10.0, 5.0, 0, false},
        {"C2", "CAP-0603-100N", 12.5, 5.0, 90, false},
        {"C3", "CAP-0603-100N", 15.0001, 5.00005, 180, false},
        {"R1", "RES-0402-10K", 20.0, 30.0, 270, false},
        {"R2", "RES-0402-10K", 22.0, 30.0, -90, true},
        {"R3", "RES-0402-10K", 24.0, 30.0, 45.5, false},
        {"R4", "RES-0402-10K", 26.0, 30.0, 359.9875, false},
        {"U1", "MCU-QFP64-LONG-PARTNUMBER-XYZ", 60.0, 40.0, 0, false},
        {"U2", "MCU-QFP64-LONG-PARTNUMBER-XYZ", 90.0, 40.0, 180, false},
        {"C4", "CAP-0603-100N", 100.0, 70.0, 0.0125, false},
    };
    for (const auto& p : ps) {
        JobPlacement q;
        q.ref = p.ref; q.part = p.part; q.x = p.x; q.y = p.y; q.rot = p.r; q.skip = p.skip;
        j.placements.push_back(q);
    }
    const char* parts[] = {"CAP-0603-100N", "RES-0402-10K", "MCU-QFP64-LONG-PARTNUMBER-XYZ"};
    const char* descs[] = {"Capacitor 100nF 0603", "Resistor 10k 0402", "Microcontroller QFP64 with a long description"};
    for (int i = 0; i < 3; ++i) {
        JobPart p; p.part = parts[i]; p.description = descs[i];
        j.parts.push_back(p);
    }
    JobFiducial f1; f1.name = "FID1"; f1.x = 3.0; f1.y = 3.0; f1.diameter = 1.0;
    JobFiducial f2; f2.name = "FID2"; f2.x = 117.5; f2.y = 77.25; f2.diameter = 1.5;
    j.fiducials = {f1, f2};
    for (auto& p : j.parts) { p.length = 1.6; p.width = 0.8; p.height = 0.5; }
    j.options["juki.timestamp"] = "1700000000";
    j.options["juki.local.U1"] = "FID1,FID2";
    j.options["juki.comment"] = "PCB Plotter test";
    return j;
}

static int32_t raw(double mm) { return (int32_t)std::lround(mm * 10000.0); }
static int16_t raw_a(double d) {
    double a = std::fmod(d, 360.0); if (a < 0) a += 360.0;
    long r = std::lround(a * 80.0); if (r >= 28800) r -= 28800;
    return (int16_t)r;
}

struct Expect { pj::HlcVariant v; size_t entries; const char* ver; uint32_t prec, crec, nrec; };

static void check_variant(const MachineJob& job, const Expect& ex) {
    const char* tag = pj::hlc_extension(ex.v);
    std::vector<uint8_t> buf;
    std::string err;
    bool ok = pj::write_hlc_variant(job, ex.v, buf, err);
    CHECK(ok, "%s write: %s", tag, err.c_str());
    if (!ok) return;
    pj::HlcParsed p;
    ok = pj::parse_hlc(buf, p, err);
    CHECK(ok, "%s parse: %s", tag, err.c_str());
    if (!ok) return;

    // ---- block table geometry ----
    CHECK(p.blocks.size() == ex.entries, "%s entries %zu", tag, p.blocks.size());
    CHECK(p.blocks[0].addr == 38 + 16 * ex.entries, "%s a0", tag);
    uint32_t end = p.blocks[0].addr;
    for (size_t i = 0; i < p.blocks.size(); ++i) {
        const auto& b = p.blocks[i];
        if (!b.addr) {
            // H8H: JUKI lists absent blocks with their record size / count (HLC_FORMAT §11)
            CHECK(ex.v == pj::HlcVariant::H8H || (!b.prefix && !b.recsize && !b.count), "%s empty entry %zu not zero", tag, i);
            continue;
        }
        CHECK(b.addr == end, "%s block %zu not contiguous (%u vs %u)", tag, i, b.addr, end);
        end = b.addr + b.prefix + b.recsize * b.count;
    }
    CHECK(end == buf.size(), "%s end %u size %zu", tag, end, buf.size());
    CHECK(p.version == ex.ver, "%s version '%s'", tag, p.version.c_str());
    CHECK(p.blocks[3].recsize == ex.prec && p.blocks[4].recsize == ex.crec &&
              p.blocks[12].recsize == ex.nrec, "%s rec sizes", tag);

    // ---- lineinfo / board ----
    CHECK(p.li_place_count == 10 && p.li_place_count2 == 10 && p.li_comp_count == 3, "%s lineinfo counts", tag);
    CHECK(p.board_name == "PCBP-TEST_RevA", "%s board name", tag);
    CHECK(p.pcb_w == raw(120.5) && p.pcb_h == raw(80.25), "%s pcb dims", tag);
    CHECK(p.layout_x == raw(120.5) && p.layout_y == 0, "%s layout corner", tag);
    CHECK(p.board_mode == 0x24, "%s mode 0x%X (single PWB + PWB BOC)", tag, p.board_mode);
    CHECK(p.ckt_w == raw(120.5) && p.ckt_h == raw(80.25) && p.ckt_x == 0 && p.ckt_y == 0, "%s circuit = board", tag);
    CHECK(p.circuit_count == 0, "%s no circuit list", tag);
    {
        const auto chk = pj::validate_hlc(buf);
        CHECK(pj::hlc_check_ok(chk), "%s validator: %s", tag, chk.empty() ? "" : chk[0].msg.c_str());
    }
    if (ex.crec >= 1792)
        CHECK(p.comp_w[0] == raw(1.6) && p.comp_l[0] == raw(0.8) && p.comp_h[0] == raw(0.5), "%s comp outline", tag);

    // ---- components ----
    CHECK(p.components.size() == 3, "%s comp count", tag);
    if (p.components.size() == 3) {
        CHECK(p.components[0] == "CAP-0603-100N" && p.comp_counts[0] == 4, "%s comp0", tag);
        CHECK(p.components[1] == "RES-0402-10K" && p.comp_counts[1] == 4, "%s comp1", tag);
        CHECK(p.components[2] == "MCU-QFP64-LONG-PARTN" && p.comp_counts[2] == 2, "%s comp2 '%s'", tag,
              p.components[2].c_str());
        CHECK(p.comp_desc[0] == "Capacitor 100nF 0603", "%s desc0", tag);
    }

    // ---- placements ----
    CHECK(p.placements.size() == job.placements.size(), "%s placement count", tag);
    CHECK(p.names.size() == job.placements.size(), "%s names count", tag);
    for (size_t i = 0; i < p.placements.size() && i < job.placements.size(); ++i) {
        const auto& q = p.placements[i];
        const auto& s = job.placements[i];
        CHECK(q.ref == s.ref && p.names[i] == s.ref, "%s ref %zu", tag, i);
        CHECK(q.x == raw(s.x) && q.y == raw(s.y), "%s xy %s", tag, s.ref.c_str());
        CHECK(q.angle == raw_a(s.rot), "%s angle %s %d", tag, s.ref.c_str(), q.angle);
        CHECK(q.status == 0x1F, "%s status %s", tag, s.ref.c_str());
        CHECK(q.comp < p.components.size() &&
                  s.part.compare(0, 20, p.components[q.comp]) == 0, "%s comp link %s", tag, s.ref.c_str());
        CHECK(((q.flags & 1) != 0) == s.skip, "%s skip %s", tag, s.ref.c_str());
        const bool has_local = s.ref == "U1";
        CHECK(((q.flags & 8) != 0) == has_local, "%s local flag %s", tag, s.ref.c_str());
        if (has_local) {
            const bool narrow = ex.prec == 71;
            const bool h8 = ex.v == pj::HlcVariant::H8H;
            CHECK(q.mark_set == (narrow ? 0 : h8 ? 3 : 1), "%s markset %u", tag, q.mark_set);
        }
    }
    // ---- angle spot checks (deg*80) ----
    CHECK(p.placements[4].angle == 21600, "%s -90 -> 270", tag);
    CHECK(p.placements[6].angle == 28799, "%s 359.9875", tag);
    CHECK(p.placements[9].angle == 1, "%s 0.0125", tag);

    // ---- marks ----
    if (ex.prec == 71) {
        CHECK(p.local_marks.size() == 2, "%s narrow local marks", tag);
    } else {
        const bool h8 = ex.v == pj::HlcVariant::H8H;
        const size_t nbo = h8 ? 3 : 1;   // H8H: one BOC element per station (block 11)
        CHECK(p.mark_sets.size() == nbo + 1, "%s mark sets %zu", tag, p.mark_sets.size());
        if (h8 && p.mark_sets.size() == 4) {
            for (size_t st = 0; st < 3; ++st) {
                CHECK(p.mark_sets[st].type == 1 && p.mark_sets[st].count == 2 &&
                          p.mark_sets[st].marks[0].x == p.mark_sets[0].marks[0].x, "%s station %zu BOC", tag, st);
                // board +2596 i16[station] -> element index
                const size_t b2 = t_u32(buf, 38 + 16 * 2);
                CHECK(t_u16(buf, b2 + 2596 + 2 * st) == st, "%s +2596[%zu]", tag, st);
            }
            const size_t b2 = t_u32(buf, 38 + 16 * 2);
            CHECK(t_u16(buf, b2 + 2596 + 6) == 0xFFFF, "%s +2596[3] none", tag);
            CHECK(p.mark_sets[3].type == 2 && p.mark_sets[3].name == "M004", "%s H8H local set", tag);
        }
        if (!h8 && p.mark_sets.size() == 2) {
            CHECK(p.mark_sets[0].type == 1 && p.mark_sets[0].name == "M001" && p.mark_sets[0].count == 2,
                  "%s global set", tag);
            CHECK(p.mark_sets[0].marks[1].x == raw(117.5) && p.mark_sets[0].marks[1].y == raw(77.25),
                  "%s global mark 2", tag);
            CHECK(p.mark_sets[1].type == 2 && p.mark_sets[1].name == "M002" && p.mark_sets[1].count == 2,
                  "%s local set", tag);
        }
    }

    std::string rep;
    CHECK(pj::read_hlc_summary(buf, rep), "%s summary", tag);
    if (std::getenv("JUKI_DUMP")) std::printf("==== %s ====\n%s", tag, rep.c_str());
    if (const char* dir = std::getenv("JUKI_OUT")) {
        std::string path = std::string(dir) + "/test." + tag;
        if (FILE* f = std::fopen(path.c_str(), "wb")) { std::fwrite(buf.data(), 1, buf.size(), f); std::fclose(f); }
    }

#ifdef PCBP_TEST_WITH_TTP
    if (ex.v == pj::HlcVariant::H7H || ex.v == pj::HlcVariant::H8H) {
        const char* dir = std::getenv("JUKI_OUT");
        std::string path = std::string(dir ? dir : ".") + "/ttp_check." + tag;
        FILE* f = std::fopen(path.c_str(), "wb");
        CHECK(f != nullptr, "open %s", path.c_str());
        if (!f) return;
        std::fwrite(buf.data(), 1, buf.size(), f);
        std::fclose(f);
        ::juki::H8hDoc doc;
        std::wstring wp(path.begin(), path.end());
        bool lok = ::juki::load_h8h(wp, doc);
        CHECK(lok, "TTP load %s: %s", tag, doc.variant.reject_reason.c_str());
        if (!lok) return;
        CHECK(doc.placement_count() == 10, "TTP %s placements", tag);
        CHECK(doc.component_count() == 3, "TTP %s comps", tag);
        CHECK(doc.refdes_count() == 10, "TTP %s refs", tag);
        for (size_t i = 0; i < doc.placement_count(); ++i) {
            const auto& s = job.placements[i];
            CHECK(doc.placement_x_raw(i) == raw(s.x) && doc.placement_y_raw(i) == raw(s.y), "TTP %s xy %zu", tag, i);
            CHECK(doc.placement_r_raw(i) == raw_a(s.rot), "TTP %s r %zu", tag, i);
            CHECK(doc.placement_skipped(i) == s.skip, "TTP %s skip %zu", tag, i);
            CHECK(doc.refdes(i) == s.ref, "TTP %s refdes %zu '%s'", tag, i, doc.refdes(i).c_str());
            int16_t ci = doc.placement_comp_id(i);
            CHECK(ci >= 0 && (size_t)ci < 3 && s.part.compare(0, 20, doc.component_partname((size_t)ci)) == 0,
                  "TTP %s comp link %zu", tag, i);
        }
        CHECK(doc.pcb_dim_x_raw() == raw(120.5) && doc.pcb_dim_y_raw() == raw(80.25), "TTP %s pcb", tag);
        CHECK(doc.circuit_dim_x_raw() == raw(120.5), "TTP %s ckt dim = board", tag);
        CHECK(doc.is_single() && doc.circuit_count_computed() == 1, "TTP %s circuits %d", tag,
              doc.circuit_count_computed());
        CHECK(doc.component_dim_x_raw(0) == raw(1.6) && doc.component_dim_y_raw(0) == raw(0.8), "TTP %s comp dim", tag);
        CHECK(doc.fiducial_machine_count() == 2 && doc.fiducial_effective_count(0) == 2, "TTP %s fid", tag);
        CHECK(doc.fiducial_boc_x_raw(0, 1) == raw(117.5), "TTP %s fid x", tag);
        CHECK(doc.board_last_edited() == 1700000000u, "TTP %s ts", tag);
        std::printf("TTP parser accepted %s (version 0x%08X, %zu placements)\n", tag, doc.version,
                    doc.placement_count());
    }
#endif
}


// ---------------------------------------------------------------------------
// Independent oracle: a port of the read sequence of Corey's Juki Tool
// (in5/juki_tool/src/fmtHLC.cpp: fmtHLC_Block, fmtHLC_PCBdata::read,
// fmtHLC_Centroid::read, fmtHLC_Component::read up to dim_z). Sequential
// stream reads, no shared offset constants with the writer.
// ---------------------------------------------------------------------------
struct Bs {
    const std::vector<uint8_t>& d; size_t p = 0;
    explicit Bs(const std::vector<uint8_t>& v) : d(v) {}
    uint8_t u8() { return d[p++]; }
    int16_t i16() { int16_t v = (int16_t)(d[p] | (d[p + 1] << 8)); p += 2; return v; }
    int32_t i32() { int32_t v = (int32_t)((uint32_t)d[p] | ((uint32_t)d[p + 1] << 8) | ((uint32_t)d[p + 2] << 16) | ((uint32_t)d[p + 3] << 24)); p += 4; return v; }
    std::string str(size_t n) { std::string s; for (size_t i = 0; i < n; ++i) { char c = (char)d[p + i]; if (!c) break; s += c; } p += n; return s; }
    void skip(size_t n) { p += n; }
};
struct OBoard {
    std::string id; uint16_t unk004 = 0; int32_t pcb_x = 0, pcb_y = 0, lay_x = 0, lay_y = 0;
    int32_t cdx = 0, cdy = 0, clx = 0, cly = 0; int16_t div_x = 0, div_y = 0;
    int32_t fcx = 0, fcy = 0, pitch_x = 0, pitch_y = 0; int16_t ncl = 0;
    struct C { int32_t x, y; int16_t r, flag; }; std::vector<C> circ;
};
struct OPlace { std::string ref; int32_t x, y; int16_t r; uint32_t flag; int16_t comp; };
struct ODoc { OBoard b; std::vector<OPlace> pl; std::vector<int32_t> dim_x, dim_y, dim_z; };
static bool oracle_read(const std::vector<uint8_t>& d, ODoc& o) {
    Bs f(d); f.skip(38);
    uint32_t a[28], sz[28], cnt[28];
    for (int i = 0; i < 28; ++i) { a[i] = (uint32_t)f.i32(); f.i32(); sz[i] = (uint32_t)f.i32(); cnt[i] = (uint32_t)f.i32(); }
    if (a[0] != 38 + 16 * 28) return false;
    Bs b(d); b.p = a[2];
    o.b.id = b.str(32); b.i16(); b.i32(); b.i32(); o.b.unk004 = (uint16_t)b.i16();
    o.b.pcb_x = b.i32(); o.b.pcb_y = b.i32(); b.i32(); b.i32(); o.b.lay_x = b.i32(); o.b.lay_y = b.i32(); b.i16();
    o.b.cdx = b.i32(); o.b.cdy = b.i32(); o.b.clx = b.i32(); o.b.cly = b.i32(); b.i32(); b.skip(70);
    o.b.div_x = b.i16(); o.b.div_y = b.i16(); o.b.fcx = b.i32(); o.b.fcy = b.i32(); o.b.pitch_x = b.i32(); o.b.pitch_y = b.i32();
    b.i16(); o.b.ncl = b.i16();
    for (int i = 0; i < 200; ++i) { OBoard::C c; c.x = b.i32(); c.y = b.i32(); c.r = b.i16(); c.flag = b.i16(); if (i < o.b.ncl) o.b.circ.push_back(c); }
    for (uint32_t i = 0; i < cnt[3]; ++i) {
        Bs c(d); c.p = a[3] + (size_t)sz[3] * i;
        OPlace q; q.ref = c.str(5); c.u8(); c.i16(); q.x = c.i32(); q.y = c.i32(); q.r = c.i16();
        c.skip(5); c.i32(); q.flag = (uint32_t)c.i32(); c.i16(); q.comp = c.i16();
        o.pl.push_back(q);
    }
    for (uint32_t i = 0; i < cnt[4]; ++i) {
        Bs c(d); c.p = a[4] + (size_t)sz[4] * i;
        c.str(20); c.skip(27); c.str(30); c.u8(); c.i16(); c.u8();
        o.dim_x.push_back(c.i32()); o.dim_y.push_back(c.i32()); o.dim_z.push_back(c.i32());
    }
    return true;
}

// Board-local placement position as Juki Tool's preview reconstructs it, here
// for the circuit-1 template: Single / Matrix use the stored point, Non-Matrix
// rotates by circuit[k] and adds its position (fmtHLC.cpp lines 1626-1634).
static void expand(const ODoc& o, std::vector<std::pair<double, double>>& pts) {
    const int cfg = (o.b.unk004 >> 3) & 3;
    for (const auto& q : o.pl) {
        if (cfg == 0) { pts.push_back({q.x / 1e4, q.y / 1e4}); continue; }
        if (cfg == 1) {
            for (int j = 0; j < o.b.div_y; ++j)
                for (int i = 0; i < o.b.div_x; ++i)
                    pts.push_back({(q.x + o.b.fcx + i * o.b.pitch_x) / 1e4, (q.y + o.b.fcy + j * o.b.pitch_y) / 1e4});
            continue;
        }
        for (const auto& c : o.b.circ) {
            const double t = c.r / 80.0 * 3.14159265358979323846 / 180.0;
            pts.push_back({(c.x + std::cos(t) * q.x - std::sin(t) * q.y) / 1e4, (c.y + std::sin(t) * q.x + std::cos(t) * q.y) / 1e4});
        }
    }
}

// 2-up / 3-up panel jobs: circuit k = R(rot_k) * circuit1 + t_k.
static MachineJob panel_job(int n, bool rotated) {
    MachineJob j;
    j.program_name = "PANEL"; j.board_w = 130; j.board_h = 50; j.single_board = false;
    j.has_circuit = true; j.circuit_x = 5; j.circuit_y = 5; j.circuit_w = 40; j.circuit_h = 40;
    struct P { const char* ref; double x, y, r; };
    const P ps[] = {{"R1", 10, 10, 0}, {"R2", 20, 30, 90}, {"U1", 40, 40, 45}};
    for (int k = 0; k < n; ++k) {
        JobCircuit c; c.index = k + 1;
        if (rotated && k == 1) { c.arot = 180; c.ax = 90; c.ay = 50; }   // circuit 2 turned 180 in [45,85]
        else { c.ax = 40.0 * k; }
        j.circuits.push_back(c);
        const double t = c.arot * 3.14159265358979323846 / 180.0;
        for (const auto& p : ps) {
            JobPlacement q; q.ref = p.ref; q.part = "P-" + std::string(p.ref).substr(0, 1);
            q.x = c.ax + std::cos(t) * p.x - std::sin(t) * p.y; q.y = c.ay + std::sin(t) * p.x + std::cos(t) * p.y;
            q.rot = std::fmod(p.r + c.arot, 360.0); q.circuit = k + 1;
            j.placements.push_back(q);
        }
    }
    JobPart a; a.part = "P-R"; a.length = 1; a.width = 0.5; JobPart b; b.part = "P-U"; b.length = 5; b.width = 5; b.height = 1;
    j.parts = {a, b};
    JobFiducial g1; g1.name = "G1"; g1.x = 2; g1.y = 2;
    JobFiducial g2; g2.name = "G2"; g2.x = 128; g2.y = 48;
    JobFiducial l1; l1.name = "L1"; l1.x = 6; l1.y = 6; l1.local = true; l1.circuit = 1;
    JobFiducial l2; l2.name = "L2"; l2.x = 44; l2.y = 44; l2.local = true; l2.circuit = 1;
    j.fiducials = {g1, g2, l1, l2};
    j.options["juki.timestamp"] = "1700000000";
    return j;
}

static void check_panels() {
    for (int kind = 0; kind < 4; ++kind) {
        // 0: single image   1: 3-up matrix   2: 2-up with circuit 2 rotated 180 (non-matrix)   3: matrix, circuit fiducials
        MachineJob j = kind == 0 ? panel_job(1, false) : kind == 2 ? panel_job(2, true) : panel_job(3, false);
        if (kind == 3) j.options["juki.fid_mode"] = "circuit";
        for (auto v : {pj::HlcVariant::H8H, pj::HlcVariant::H7H}) {
            std::vector<uint8_t> buf; std::string err; pj::HlcWriteInfo wi;
            const bool ok = pj::write_hlc_variant(j, v, buf, err, &wi);
            CHECK(ok, "panel %d write: %s", kind, err.c_str());
            if (!ok) continue;
            const auto chk = pj::validate_hlc(buf);
            for (const auto& c : chk) CHECK(!c.error, "panel %d validator: %s", kind, c.msg.c_str());
            ODoc o;
            CHECK(oracle_read(buf, o), "panel %d oracle read", kind);
            const int cfg = (o.b.unk004 >> 3) & 3;
            const int want = kind == 0 ? 0 : kind == 2 ? 2 : 1;
            CHECK(cfg == want, "panel %d circuit config %d want %d", kind, cfg, want);
            CHECK((int)wi.mode == want, "panel %d info mode", kind);
            CHECK(o.b.cdx > 0 && o.b.cdy > 0, "panel %d circuit dims", kind);
            CHECK(o.pl.size() == 3, "panel %d template placements %zu (circuit 1 only)", kind, o.pl.size());
            if (kind == 1 || kind == 3) CHECK(o.b.div_x == 3 && o.b.div_y == 1 && o.b.pitch_x == raw(40), "panel %d matrix", kind);
            if (kind == 2) CHECK(o.b.ncl == 2 && o.b.circ[1].r == 14400 && o.b.circ[1].x == raw(90), "panel %d nonmatrix list", kind);
            const uint16_t boc = (o.b.unk004 >> 5) & 3;
            CHECK(boc == (kind == 3 ? 2u : 1u), "panel %d BOC type %u", kind, boc);
            CHECK(o.dim_x.size() == 2 && o.dim_x[1] == raw(5) && o.dim_z[1] == raw(1), "panel %d comp outline", kind);
            // round trip: oracle expansion == every source placement
            std::vector<std::pair<double, double>> pts;
            expand(o, pts);
            CHECK(pts.size() == j.placements.size(), "panel %d expanded %zu vs %zu", kind, pts.size(), j.placements.size());
            for (const auto& s : j.placements) {
                bool f = false;
                for (const auto& q : pts) f |= std::hypot(q.first - s.x, q.second - s.y) < 2e-4;
                CHECK(f, "panel %d %s circuit %d (%.3f,%.3f) not reproduced", kind, s.ref.c_str(), s.circuit, s.x, s.y);
            }
            for (const auto& q : o.pl) CHECK((q.flag & 0x40) != 0, "panel %d placement attribute", kind);
        }
    }
    // A panel that is not a step & repeat of circuit 1 is refused, not mis-written.
    {
        MachineJob j = panel_job(2, false);
        j.placements[4].x += 0.5;   // circuit 2 R2 moved
        std::vector<uint8_t> b; std::string err;
        CHECK(!pj::write_hlc(j, pj::HlcFormat::H8H, b, err) && err.find("step-and-repeat") != std::string::npos,
              "non-repeating panel refused: %s", err.c_str());
        j.single_board = true;   // 'Treat all images as one board'
        for (auto& p : j.placements) p.circuit = 1;
        CHECK(pj::write_hlc(j, pj::HlcFormat::H8H, b, err), "single-board fallback: %s", err.c_str());
        ODoc o; oracle_read(b, o);
        CHECK(((o.b.unk004 >> 3) & 3) == 0 && o.pl.size() == 6, "single-board fallback writes all placements");
    }
    // The old failure mode is caught by the validator: one circuit as Non-Matrix
    // with no circuit dimensions.
    {
        MachineJob j = panel_job(1, false);
        std::vector<uint8_t> b; std::string err;
        pj::write_hlc(j, pj::HlcFormat::H8H, b, err);
        pj::HlcParsed p; pj::parse_hlc(b, p, err);
        const size_t bo = p.blocks[2].addr;
        b[bo + 42] = 0x34; b[bo + 182] = 1;                 // non-matrix, 1 circuit
        for (int k = 70; k < 88; ++k) b[bo + k] = 0;        // no circuit block
        const auto chk = pj::validate_hlc(b);
        bool dims = false, nm = false;
        for (const auto& c : chk) { dims |= c.msg.find("circuit dimensions missing") != std::string::npos;
                                    nm |= c.msg.find("Single PWB is expected") != std::string::npos; }
        CHECK(dims && nm, "validator flags the reported failure");
    }
    // Bottom side / mirrored jobs keep the PWB BOC bit (no CircuitCAM 0x54 quirk).
    {
        MachineJob j = panel_job(1, false); j.bottom_side = true;
        std::vector<uint8_t> b; std::string err;
        pj::write_hlc(j, pj::HlcFormat::H8H, b, err);
        ODoc o; oracle_read(b, o);
        CHECK(((o.b.unk004 >> 5) & 3) == 1, "bottom side keeps PWB BOC");
    }
}


// ---- R13: H8H mark block (HLC_FORMAT §6), ANSI text (§12), pre-export check (§13) ----
static void check_r13() {
    // legacy_text
    CHECK(pj::legacy_text("10\xC2\xB5" "F \xC2\xB1" "10% 50V") == "10uF +/-10% 50V", "translit mu/pm");
    CHECK(pj::legacy_text("4.7k\xE2\x84\xA6 25\xC2\xB0" "C") == "4.7kOhm 25degC", "translit ohm/deg");
    CHECK(pj::legacy_text("\xE2\x80\x9CQ\xE2\x80\x9D \xE2\x80\x98s\xE2\x80\x99 a\xE2\x80\x93" "b\xE2\x80\x94" "c 2\xC3\x97" "3") == "\"Q\" 's' a-b-c 2x3", "translit quotes");
    CHECK(pj::legacy_text("Caf\xC3\xA9 \xC5\x81\xC3\xB3" "d\xC5\xBA") == "Cafe Lodz", "translit accents");
    CHECK(pj::legacy_text("\xE6\x97\xA5") == "_", "unmappable -> _");
    CHECK(pj::legacy_text("\xB1" "5") == "+/-5", "cp1252 byte fallback");
    int ch = 0; pj::legacy_text("plain", &ch); CHECK(ch == 0, "plain unchanged");

    MachineJob job = make_job();
    job.options.erase("juki.local.U1");
    job.parts[0].description = "Cap 100nF \xC2\xB1" "10% X7R";
    std::vector<uint8_t> b; std::string err; pj::HlcWriteInfo wi;
    CHECK(pj::write_hlc_variant(job, pj::HlcVariant::H8H, b, err, &wi), "r13 write %s", err.c_str());
    if (b.empty()) return;
    const size_t a2 = t_u32(b, 38 + 16 * 2), a6 = t_u32(b, 38 + 16 * 6), li = t_u32(b, 38 + 16 * 1);
    const size_t a4 = t_u32(b, 38 + 16 * 4);
    // golden: one 3072-B BOC element per station (block 11 count 3), linked by board +2596
    CHECK(t_u32(b, 38 + 16 * 6 + 8) == 3072 && t_u32(b, 38 + 16 * 6 + 12) == 3, "blk6 3 x 3072");
    CHECK(t_u32(b, li + 154) == 3 && t_u32(b, li + 158) == 3, "line info mark counts");
    for (int st = 0; st < 3; ++st) {
        const size_t e = a6 + 3072u * st;
        char nm[8]; std::snprintf(nm, sizeof nm, "M%03d", st + 1);
        CHECK(b[e] == 1 && b[e + 1] == 1 && std::string((const char*)&b[e + 2]) == nm && t_u16(b, e + 18) == 2, "st%d header", st);
        CHECK((int32_t)t_u32(b, e + 40) == raw(3.0) && (int32_t)t_u32(b, e + 44) == raw(3.0) && t_u16(b, e + 48) == 10000 && b[e + 50] == 3, "st%d mark1", st);
        CHECK((int32_t)t_u32(b, e + 168) == raw(117.5) && (int32_t)t_u32(b, e + 172) == raw(77.25) && t_u16(b, e + 176) == 15000, "st%d mark2", st);
        CHECK(t_u16(b, e + 40 + 14) == 0 && b[e + 40 + 40] == 0, "st%d wMarkId 0, untaught", st);
        CHECK(t_u16(b, a2 + 2596 + 2 * st) == st, "+2596[%d]", st);
    }
    for (int k = 3; k < 14; ++k) CHECK(t_u16(b, a2 + 2596 + 2 * k) == 0xFFFF, "+2596[%d] none", k);
    CHECK((t_u16(b, a2 + 42) >> 5 & 3) == 1, "PWB BOC type");
    CHECK(pj::hlc_check_ok(pj::validate_hlc(b)), "r13 validates");
    // Juki Tool reader (fmtHLC_Fiducial::read): element 0 count @18, boc[k] @40+128k
    CHECK(b[a6 + 18] == 2 && (int32_t)t_u32(b, a6 + 40 + 128) == raw(117.5), "JT fiducial read");
    // ANSI: description written as ASCII
    CHECK(std::string((const char*)&b[a4 + 47]) == "Cap 100nF +/-10% X7R", "desc ascii '%s'", (const char*)&b[a4 + 47]);
    CHECK(wi.text_converted == 1, "text_converted %d", wi.text_converted);
    // validator: marks enabled but station not linked / no coordinates = Error (P0-01)
    {
        auto c = b; c[a2 + 2596] = 0xFF; c[a2 + 2597] = 0xFF;
        bool hit = false; for (auto& x : pj::validate_hlc(c)) hit |= x.error && x.msg.find("no mark record") != std::string::npos;
        CHECK(hit, "unlinked station -> error");
        auto z = b; for (int i = 0; i < 8; ++i) z[a6 + 3072 + 40 + i] = 0;
        hit = false; for (auto& x : pj::validate_hlc(z)) hit |= x.error && x.msg.find("no coordinates") != std::string::npos;
        CHECK(hit, "zero mark -> error");
    }
    // no fiducials: block 6 empty, +2596 all none
    {
        MachineJob j = job; j.fiducials.clear();
        std::vector<uint8_t> q;
        CHECK(pj::write_hlc_variant(j, pj::HlcVariant::H8H, q, err) && t_u32(q, 38 + 16 * 6 + 12) == 0 &&
              t_u16(q, t_u32(q, 38 + 32) + 2596) == 0xFFFF, "no marks");
    }
    // circuit marks: image fiducials of circuit 1, BOC type 2
    {
        MachineJob j = job;
        for (auto& f : j.fiducials) { f.local = true; f.circuit = 1; }
        j.options["juki.fid_mode"] = "circuit";
        std::vector<uint8_t> q;
        CHECK(pj::write_hlc_variant(j, pj::HlcVariant::H8H, q, err), "circuit marks write");
        const size_t q2 = t_u32(q, 38 + 32), q6 = t_u32(q, 38 + 96);
        CHECK((t_u16(q, q2 + 42) >> 5 & 3) == 2 && t_u16(q, q6 + 18) == 2 && t_u16(q, q2 + 2598) == 1, "circuit BOC");
        CHECK(pj::hlc_check_ok(pj::validate_hlc(q)), "circuit marks validate");
    }
    // local mark set follows the station elements
    {
        MachineJob j = make_job();
        std::vector<uint8_t> q; pj::HlcParsed p;
        CHECK(pj::write_hlc_variant(j, pj::HlcVariant::H8H, q, err) && pj::parse_hlc(q, p, err) &&
              p.mark_sets.size() == 4 && p.placements[7].mark_set == 3, "local set index 3");
    }
    // part-name collisions after conversion / truncation: deterministic suffix
    {
        MachineJob j = job;
        j.parts.clear(); j.placements.resize(4);
        const char* pn[] = {"10\xC2\xB5" "F-0603", "10uF-0603", "LONGPARTNUMBER-ABCDEF-1", "LONGPARTNUMBER-ABCDEF-2"};
        for (int i = 0; i < 4; ++i) { JobPart pp; pp.part = pn[i]; j.parts.push_back(pp); j.placements[i].part = pn[i]; }
        std::vector<uint8_t> q; pj::HlcWriteInfo w; pj::HlcParsed p;
        CHECK(pj::write_hlc_variant(j, pj::HlcVariant::H8H, q, err, &w) && pj::parse_hlc(q, p, err), "collide write %s", err.c_str());
        CHECK(w.renamed.size() == 2, "renamed %zu", w.renamed.size());
        std::set<std::string> shorts(p.components.begin(), p.components.end());
        CHECK(shorts.size() == 4, "unique short names");
        for (auto& r : w.renamed) std::printf("  renamed '%s' -> '%s'\n", r.first.c_str(), r.second.c_str());
        // determinism
        std::vector<uint8_t> q2; pj::write_hlc_variant(j, pj::HlcVariant::H8H, q2, err);
        CHECK(q == q2, "collision deterministic");
        std::vector<ExportIssue> iss; j.machine = "juki.h8h"; pj::check_job(j, iss);
        int nw = 0; for (auto& i : iss) nw += i.sev == ExportIssue::Warning && i.msg.find("is written as") != std::string::npos;
        CHECK(nw == 2, "check_job rename warnings %d", nw);
    }
    // line info: originator -> line name, no app text in the comment (P1-06)
    {
        MachineJob j = job; j.originator = "Demo SMT Assembly"; j.options["juki.comment"] = "Generated by PCB Plotter";
        j.pcb_id = "PWB-1234 Rev B";
        std::vector<uint8_t> q;
        pj::write_hlc_variant(j, pj::HlcVariant::H8H, q, err);
        const size_t ql = t_u32(q, 38 + 16), q2 = t_u32(q, 38 + 32);
        CHECK(std::string((const char*)&q[ql]) == "Demo SMT Assembly" && q[ql + 21] == 0, "line name / comment");
        CHECK(std::string((const char*)&q[q2]) == "PWB-1234 Rev B", "PWB ID");
    }
}

// ---- R13 §10: arbitrary layer, bottom flip of placements / fiducials / board, validation categories ----
static void check_flip_and_validation() {
    for (int mirror = 0; mirror < 2; ++mirror) {
        Project p;
        Package pk; pk.name = "0603"; pk.pads = {Pad{{-0.8, 0}, 0.8, 0.9}, Pad{{0.8, 0}, 0.8, 0.9}};
        p.packages["0603"] = pk;
        Package fp; fp.name = "FID1MM"; fp.pads = {Pad{{0, 0}, 1.0, 1.0}}; p.packages["FID1MM"] = fp;
        p.board_frame = {{{0, 0}, {50, 0}, {50, 40}, {0, 40}}};
        p.model.points.clear();
        ModelPoint mp; mp.name = "Glue + SMT side B (user layer)"; mp.has_machine = true; mp.machine.exporter_id = "juki.h8h";
        mp.machine.side = Side::Bottom; mp.machine.flipped = true; mp.machine.mirror_angle_when_flipped = mirror;
        p.model.points.push_back(mp);
        auto mk = [&](const char* r, double x, double y, double a, Feature f) {
            Component c; c.ref = r; c.pos = c.cad_pos = {x, y}; c.rot = c.rot_imported = a; c.side = Side::Bottom;
            c.package = f == Feature::Fiducial ? "FID1MM" : "0603"; c.number = f == Feature::Fiducial ? "" : "PN-0603";
            c.feature = f; c.assigned_point = f == Feature::Fiducial ? -1 : 0; p.add_component(c);
        };
        mk("R1", 10, 5, 30, Feature::Component);
        mk("R2", 40, 35, 90, Feature::Component);
        mk("FID1", 2, 2, 0, Feature::Fiducial);
        mk("FID2", 48, 38, 0, Feature::Fiducial);
        MachineJob j; std::string err;
        CHECK(build_machine_job(p, 0, j, err), "flip job %s", err.c_str());
        if (j.placements.size() != 2 || j.fiducials.size() != 2) { CHECK(false, "flip job contents"); continue; }
        // one mirror for everything: x -> -x about the origin, y kept
        CHECK(std::fabs(j.placements[0].x + 10) < 1e-9 && std::fabs(j.placements[0].y - 5) < 1e-9, "flip R1 xy");
        CHECK(std::fabs(j.fiducials[0].x + 2) < 1e-9 && std::fabs(j.fiducials[1].x + 48) < 1e-9 && std::fabs(j.fiducials[1].y - 38) < 1e-9, "flip fid xy");
        CHECK(std::fabs(j.board_x0 + 50) < 1e-9 && std::fabs(j.board_w - 50) < 1e-9 && std::fabs(j.board_h - 40) < 1e-9, "flip board");
        CHECK(std::fabs(j.placements[0].rot - (mirror ? 330 : 30)) < 1e-9, "flip angle rule mirror=%d rot %.3f", mirror, j.placements[0].rot);
        CHECK(std::fabs(j.fiducials[0].diameter - 1.0) < 1e-9, "fid diameter from package");
        // H8H: marks in the same frame as placements, inside the board, validator clean
        j.options["juki.timestamp"] = "1700000000";
        std::vector<uint8_t> b;
        CHECK(pj::write_hlc_variant(j, pj::HlcVariant::H8H, b, err), "flip h8h %s", err.c_str());
        const size_t a6 = t_u32(b, 38 + 96), a3 = t_u32(b, 38 + 48);
        CHECK((int32_t)t_u32(b, a6 + 40) == raw(-2) && (int32_t)t_u32(b, a3 + 8) == raw(-10), "flip mark/placement frame");
        const auto chk = pj::validate_hlc(b);
        CHECK(pj::hlc_check_ok(chk), "flip validates: %s", chk.empty() ? "" : chk[0].msg.c_str());
        // validation categories: clean project -> ready (no errors)
        auto v = validate_for_export(p, 0);
        auto n = count_issues(v);
        CHECK(n.ready(), "flip project ready, %d errors (%s)", n.errors, v.empty() ? "" : v[0].msg.c_str());
        // unresolved P/N = Warning, not Error; DNP = Info
        p.comps[0].number.clear();
        p.comps[1].bom_state = BomState::Dnp; p.comps[1].dnp = true;
        v = validate_for_export(p, 0); n = count_issues(v);
        bool w = false, inf = false;
        for (auto& i : v) { w |= i.sev == ExportIssue::Warning && i.msg.find("BOM unresolved") != std::string::npos;
                            inf |= i.sev == ExportIssue::Info && i.msg.find("DNP") != std::string::npos; }
        CHECK(n.ready() && w && inf, "categories ready=%d w=%d i=%d", n.ready(), w, inf);
        // a machine-program Error (fiducial outside the board) blocks export
        p.comps[3].pos = {80, 38};
        v = validate_for_export(p, 0); n = count_issues(v);
        CHECK(!n.ready(), "mark outside board -> Error");
        // R18d: single circuit inside a LARGER board/panel outline, board marks in the margin outside the
        // circuit: the marks are on the board -> no "outside the board" in the program or the project check
        p.comps[3].pos = {55, 45}; p.comps[3].fid_scope = FiducialScope::Global;
        p.comps[2].pos = {-5, -5}; p.comps[2].fid_scope = FiducialScope::Global;
        p.set_panel_frame_rect(BBox{-10, -10, 60, 50});
        p.comps[0].number = "PN-0603"; p.comps[1].bom_state = BomState::NotChecked; p.comps[1].dnp = false;
        MachineJob jm; std::string em;
        CHECK(build_machine_job(p, 0, jm, em) && std::fabs(jm.board_w - 70) < 1e-6 && std::fabs(jm.board_h - 60) < 1e-6, "panel margin: board = panel outline %.3f x %.3f", jm.board_w, jm.board_h);
        std::vector<uint8_t> bm;
        jm.options["juki.timestamp"] = "1700000000";
        CHECK(pj::write_hlc_variant(jm, pj::HlcVariant::H8H, bm, em), "panel margin h8h %s", em.c_str());
        bool outb = false;
        for (auto& x : pj::validate_hlc(bm)) if (x.error && x.msg.find("outside the board") != std::string::npos) { outb = true; std::printf("    %s\n", x.msg.c_str()); }
        CHECK(!outb, "panel margin: marks in the margin are on the board (program check)");
        bool outp = false;
        for (auto& i : validate_for_export(p, 0)) if (i.msg.find("outside the board") != std::string::npos) { outp = true; std::printf("    %s\n", i.msg.c_str()); }
        CHECK(!outp, "panel margin: project check");
    }
}

int main() {
    const MachineJob job = make_job();
    const Expect ex[] = {
        {pj::HlcVariant::H7H, 28, "900", 256, 1792, 32},
        {pj::HlcVariant::H8H, 28, "C00A", 256, 1822, 32},
        {pj::HlcVariant::H5H, 26, "702", 96, 788, 12},
        {pj::HlcVariant::H4H_v6, 26, "600", 96, 744, 12},
        {pj::HlcVariant::H4H_v5, 27, "510", 71, 504, 12},
        {pj::HlcVariant::E41, 27, "111", 71, 504, 12},
        {pj::HlcVariant::E44, 27, "111", 71, 504, 12},
        {pj::HlcVariant::E45, 26, "100", 96, 744, 12},
        {pj::HlcVariant::E46, 26, "100", 96, 744, 12},
        {pj::HlcVariant::E51, 26, "150", 96, 788, 12},
    };
    for (const auto& e : ex) check_variant(job, e);
    check_panels();
    check_r13();
    check_flip_and_validation();

    // HlcFormat front-end mapping
    {
        std::vector<uint8_t> b; std::string err; pj::HlcParsed p;
        CHECK(pj::write_hlc(job, pj::HlcFormat::H4H, b, err) && pj::parse_hlc(b, p, err) && p.version == "600",
              "H4H default -> HLC6");
        MachineJob j2 = job; j2.options["juki.h4h_hlc5"] = "1";
        CHECK(pj::write_hlc(j2, pj::HlcFormat::H4H, b, err) && pj::parse_hlc(b, p, err) && p.version == "510",
              "H4H hlc5");
        // Narrow layout without local marks omits block 6 entirely.
        j2.options.erase("juki.local.U1");
        CHECK(pj::write_hlc(j2, pj::HlcFormat::H4H, b, err) && pj::parse_hlc(b, p, err) && p.blocks[6].addr == 0,
              "H4H hlc5 no marks");
        // Transport R->L / back rail flips the layout corner and mode bits.
        MachineJob j3 = job; j3.options["juki.transport"] = "RL"; j3.options["juki.fixed_rail"] = "back";
        CHECK(pj::write_hlc(j3, pj::HlcFormat::H7H, b, err) && pj::parse_hlc(b, p, err) &&
                  p.layout_x == 0 && p.layout_y == raw(80.25) && p.board_mode == 0x27, "RL/back corner");
        CHECK(pj::hlc_check_ok(pj::validate_hlc(b)), "RL/back validates");
        // Deterministic output.
        std::vector<uint8_t> b2;
        pj::write_hlc(job, pj::HlcFormat::H7H, b, err);
        pj::write_hlc(job, pj::HlcFormat::H7H, b2, err);
        CHECK(b == b2, "deterministic");
        // Sort by part keeps names/placements parallel.
        MachineJob j5 = job; j5.options["juki.sort_by_part"] = "1";
        CHECK(pj::write_hlc(j5, pj::HlcFormat::H7H, b, err) && pj::parse_hlc(b, p, err) &&
                  p.placements[0].ref == "C1" && p.placements[4].ref == "U1" && p.names[4] == "U1" &&
                  p.placements[4].mark_set == 1, "sort by part");
        // Error paths
        MachineJob j6 = job; j6.options["juki.local.U2"] = "FID1";
        CHECK(!pj::write_hlc(j6, pj::HlcFormat::H7H, b, err), "bad local pair rejected");
    }

    if (g_fail) { std::printf("test_juki: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_juki: OK\n");
    return 0;
}
