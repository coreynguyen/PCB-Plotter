// ============================================================================
// test_allegro_brd.cpp — Allegro binary .brd importer (src/io/allegro_brd.*).
// Synthetic fixtures are generated in memory (no customer data in the repo).
// Local-only: if PCBP_ALLEGRO_SAMPLE (a .brd) and PCBP_ALLEGRO_ODB (the same
// board as an ODB++ job dir) are set, every placement is compared against the
// ODB++ import.
// ============================================================================
#include "../src/core/project.h"
#include "../src/io/allegro_brd.h"
#include "../src/io/odbpp.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

using namespace pcbp;

static int g_pass = 0, g_fail = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECKM(c, m) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s [%s]\n", __FILE__, __LINE__, #c, std::string(m).c_str()); } } while (0)

static bool near(double a, double b, double t = 1e-3) { return std::fabs(a - b) < t; }

// ---- synthetic database writer --------------------------------------------
struct W {
    std::string b;
    void at32(size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) b[o + i] = (char)(v >> (8 * i)); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((char)(v >> (8 * i))); }
    void hdr(uint8_t t, uint8_t a, uint8_t c, uint32_t key) { b.push_back((char)t); b.push_back(0); b.push_back((char)a); b.push_back((char)c); u32(key); }
    void pad_to(size_t start, size_t size) { while (b.size() < start + size) b.push_back(0); }
    void str(uint32_t id, const char* s) { u32(id); b += s; b.push_back(0); while (b.size() % 4) b.push_back(0); }
    void f64s(double d) { uint64_t v; std::memcpy(&v, &d, 8); u32((uint32_t)(v >> 32)); u32((uint32_t)v); }
};

// strings: 1 "R1", 2 "U5", 3 "RES0603", 4 "SOT23", 5 "1", 6 "2", 7 "3", 8 "SMD30_40", 9 "RES_DEV"
static std::string make_fixture(uint32_t magic = 0x00140400) {
    W w;
    w.b.assign(0x1200, '\0');
    w.at32(0, magic);
    w.at32(0x194, 9);
    w.at32(0x268, 1);      // mils
    w.at32(0x26c, 100);    // 0.01 mil db units
    const char* S[] = {"R1", "U5", "RES0603", "SOT23", "1", "2", "3", "SMD30_40", "RES_DEV"};
    for (uint32_t i = 0; i < 9; ++i) w.str(i + 1, S[i]);
    // padstack 0x500
    { size_t s = w.b.size(); w.hdr(0x1C, 0, 0, 0x500); w.u32(0); w.u32(8); w.pad_to(s, 0x40); }
    // footprint defs 0x100 (RES0603), 0x101 (SOT23)
    { size_t s = w.b.size(); w.hdr(0x2B, 0, 0, 0x100); w.u32(3); w.pad_to(s, 0x40); }
    { size_t s = w.b.size(); w.hdr(0x2B, 0, 0, 0x101); w.u32(4); w.pad_to(s, 0x40); }
    // device 0x300
    { size_t s = w.b.size(); w.hdr(0x06, 0, 0, 0x300); w.u32(0); w.u32(9); w.pad_to(s, 0x20); }
    // pins (0x0D): key, name, next, x, y, padstack, 0, 0, flags, rot
    auto pin = [&](uint32_t key, uint32_t name, int32_t x, int32_t y, uint32_t rot_md) {
        size_t s = w.b.size(); w.hdr(0x0D, 0, 0, key); w.u32(name); w.u32(0); w.u32((uint32_t)x); w.u32((uint32_t)y);
        w.u32(0x500); w.u32(0); w.u32(0); w.u32(2); w.u32(rot_md); w.pad_to(s, 0x2c);
    };
    pin(0x400, 5, -3000, 0, 0);       // R1 pin1 at -30 mil
    pin(0x401, 6, 3000, 0, 0);
    pin(0x410, 5, -3700, -4500, 90000);   // U5
    pin(0x411, 6, 3700, -4500, 90000);
    pin(0x412, 7, 0, 4500, 90000);
    // placed instances (0x2D): side, key, next, fp, 0, 0, flags, rot, x, y, 0, 0, compinst, 0, firstpad
    auto placed = [&](uint32_t key, uint8_t side, uint32_t fp, uint32_t rot, int32_t x, int32_t y, uint32_t ci, uint32_t pad) {
        size_t s = w.b.size(); w.hdr(0x2D, side, 0, key); w.u32(fp); w.u32(fp); w.u32(0); w.u32(0); w.u32(0);
        w.u32(rot); w.u32((uint32_t)x); w.u32((uint32_t)y); w.u32(ci); w.u32(0); w.u32(pad); w.pad_to(s, 0x48);
    };
    placed(0x200, 0, 0x100, 90000, 100000, 50000, 0x210, 0x600);       // R1 top, 90 deg at (1000,500) mil
    placed(0x201, 1, 0x101, 180000, -20000, 30000, 0x211, 0x610);      // U5 bottom, 180 deg
    placed(0x202, 0, 0x100, 0, 0, 0, 0, 0);                            // logo: no refdes, no pins -> skipped
    // component instances (0x07): key, next, ?, 0, 0, placed, refdes, device, 0, 0, firstpad
    auto ci = [&](uint32_t key, uint32_t placed, uint32_t ref) {
        size_t s = w.b.size(); w.hdr(0x07, 0, 0, key); w.u32(0); w.u32(0); w.u32(0); w.u32(0);
        w.u32(placed); w.u32(ref); w.u32(0x300); w.pad_to(s, 0x30);
    };
    ci(0x210, 0x200, 1);
    ci(0x211, 0x201, 2);
    // placed pads (0x32): key, ... +0x18 next, +0x1c parent, +0x24 pin, bbox at +0x44
    auto pad = [&](uint32_t key, uint32_t next, uint32_t parent, uint32_t pinkey, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
        size_t s = w.b.size(); w.hdr(0x32, 0, 0, key); w.pad_to(s, 0x18); w.u32(next); w.u32(parent); w.u32(0); w.u32(pinkey);
        w.pad_to(s, 0x44); w.u32((uint32_t)x0); w.u32((uint32_t)y0); w.u32((uint32_t)x1); w.u32((uint32_t)y1); w.pad_to(s, 0x58);
    };
    // R1 at 90 deg: local pad 30x40 mil (w x h) -> absolute extent 40 x 30
    pad(0x600, 0x601, 0x200, 0x400, 98000, 45500, 102000, 48500);
    pad(0x601, 0, 0x200, 0x401, 98000, 51500, 102000, 54500);
    pad(0x610, 0x611, 0x201, 0x410, 0, 0, 2000, 3000);
    pad(0x611, 0x612, 0x201, 0x411, 0, 0, 2000, 3000);
    pad(0x612, 0, 0x201, 0x412, 0, 0, 2000, 3000);
    // board outline: shape class 1 / design outline owned by board (0x7000), 4 lines + 1 arc corner
    { size_t s = w.b.size(); w.hdr(0x28, 1, 0xEA, 0x700); w.u32(0); w.u32(0x7000); w.pad_to(s, 0x28); w.u32(0x701); w.pad_to(s, 0x48); }
    auto line = [&](uint8_t t, uint32_t key, uint32_t next, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
        size_t s = w.b.size(); w.hdr(t, 0, 0, key); w.u32(next); w.u32(0x700); w.pad_to(s, 0x1c);
        w.u32((uint32_t)x0); w.u32((uint32_t)y0); w.u32((uint32_t)x1); w.u32((uint32_t)y1); w.pad_to(s, 0x2c);
    };
    line(0x15, 0x701, 0x702, -100000, -100000, 100000, -100000);
    line(0x17, 0x702, 0x703, 100000, -100000, 100000, 90000);
    { // arc (100000,90000) -> (90000,100000) centre (90000,90000), CCW
        size_t s = w.b.size(); w.hdr(0x01, 0, 0, 0x703); w.u32(0x704); w.u32(0x700); w.pad_to(s, 0x1c);
        w.u32(100000); w.u32(90000); w.u32(90000); w.u32(100000); w.f64s(90000); w.f64s(90000); w.f64s(10000); w.pad_to(s, 0x4c);
    }
    line(0x15, 0x704, 0x705, 90000, 100000, -100000, 100000);
    line(0x17, 0x705, 0x700, -100000, 100000, -100000, -100000);
    return w.b;
}

static void test_synthetic() {
    const std::string data = make_fixture();
    char h[16]; std::memcpy(h, data.data(), 8);
    CHECK(io::sniff_allegro_brd("x.brd", std::string(data.data(), 64)) >= 90);
    CHECK(io::sniff_allegro_brd("x.brd", "<?xml version=\"1.0\"?><eagle>") == 0);
    Project p; io::CadImportOptions o; io::CadImportResult r; std::string err;
    CHECKM(io::import_allegro_brd_data(data, p, o, r, err), err);
    CHECK(p.comps.size() == 2);
    const Component* r1 = p.find_ref("R1");
    const Component* u5 = p.find_ref("U5");
    CHECK(r1 && u5);
    if (r1) {
        CHECK(near(r1->pos.x, 25.4) && near(r1->pos.y, 12.7));
        CHECK(near(r1->rot, 90));
        CHECK(r1->side == Side::Top);
        CHECK(r1->package == "RES0603");
        const Package* pk = p.package_of(*r1);
        CHECK(pk && pk->pads.size() == 2);
        if (pk && pk->pads.size() == 2) {
            CHECK(near(pk->pads[0].pos.x, -0.762) && near(pk->pads[0].pos.y, 0));
            CHECK(near(pk->pads[0].w, 0.762) && near(pk->pads[0].h, 1.016));   // de-rotated extent
            CHECK(pk->pads[0].pin == 1);
        }
    }
    if (u5) {
        CHECK(u5->side == Side::Bottom);
        CHECK(near(u5->rot, 180));
        CHECK(near(u5->pos.x, -5.08) && near(u5->pos.y, 7.62));
        const Package* pk = p.package_of(*u5);
        CHECK(pk && pk->pads.size() == 3);
    }
    CHECK(p.board_frame.size() == 1);
    if (!p.board_frame.empty()) {
        double xmax = -1e9, ymax = -1e9;
        for (auto& v : p.board_frame[0]) { xmax = std::max(xmax, v.x); ymax = std::max(ymax, v.y); }
        CHECK(near(xmax, 25.4, 0.01) && near(ymax, 25.4, 0.01));
        CHECK(p.board_frame[0].size() > 6);   // arc segmented
    }
}

static void test_failures() {
    Project p; io::CadImportOptions o; io::CadImportResult r; std::string err;
    // unsupported version
    std::string v16 = make_fixture(0x00130C00);
    CHECK(!io::import_allegro_brd_data(v16, p, o, r, err));
    CHECKM(err.find("unsupported") != std::string::npos, err);
    CHECK(io::sniff_allegro_brd("x.brd", v16.substr(0, 64)) > 0);
    std::string v18 = make_fixture(0x00160000);
    CHECK(!io::import_allegro_brd_data(v18, p, o, r, err) && err.find("unsupported") != std::string::npos);
    // not allegro / too small
    CHECK(!io::import_allegro_brd_data(std::string(16, 'x'), p, o, r, err));
    CHECK(!io::import_allegro_brd_data(std::string(0x2000, 'x'), p, o, r, err));
    // bad string count
    std::string bad = make_fixture();
    bad[0x194] = (char)0xff; bad[0x195] = (char)0xff; bad[0x196] = (char)0xff; bad[0x197] = (char)0x7f;
    CHECK(!io::import_allegro_brd_data(bad, p, o, r, err));
    // truncations and random corruption must never crash or hang
    const std::string good = make_fixture();
    for (size_t n = 0; n < good.size(); n += 37) {
        Project q; io::CadImportResult rr; std::string e;
        io::import_allegro_brd_data(good.substr(0, n), q, o, rr, e);
    }
    std::mt19937 rng(11);
    for (int it = 0; it < 400; ++it) {
        std::string c = good;
        for (int k = 0; k < 8; ++k) c[0x1200 + rng() % (c.size() - 0x1200)] = (char)rng();
        if (it % 5 == 0) c[0x194 + rng() % 4] = (char)rng();
        Project q; io::CadImportResult rr; std::string e;
        io::import_allegro_brd_data(c, q, o, rr, e);
    }
    ++g_pass;   // survived
}

// local-only: compare against the ODB++ export of the same board
static void test_sample() {
    const char* brd = std::getenv("PCBP_ALLEGRO_SAMPLE");
    const char* odb = std::getenv("PCBP_ALLEGRO_ODB");
    if (!brd || !odb) { std::printf("   (sample comparison skipped: PCBP_ALLEGRO_SAMPLE / PCBP_ALLEGRO_ODB not set)\n"); return; }
    Project a, b; io::CadImportOptions o; io::CadImportResult ra, rb; std::string err;
    CHECKM(io::import_allegro_brd_file(brd, a, o, ra, err), err);
    std::printf("   brd: %s\n", ra.describe().c_str());
    for (auto& w : ra.warnings) std::printf("     warn: %s\n", w.c_str());
    CHECKM(io::import_odbpp(odb, b, o, rb, err), err);
    std::printf("   odb: %s\n", rb.describe().c_str());
    int missing_named = 0, match = 0, rotmis = 0, sidemis = 0, pkgmis = 0, padmis = 0, missing = 0;
    double maxd = 0;
    for (auto& cb : b.comps) {
        const bool unnamed = cb.ref.rfind("no_refdes", 0) == 0;
        const Component* ca = unnamed ? nullptr : a.find_ref(cb.ref);
        if (unnamed) {   // unnamed symbols: match by position
            for (auto& x : a.comps)
                if (x.ref.rfind("no_refdes", 0) == 0 && std::hypot(x.pos.x - cb.pos.x, x.pos.y - cb.pos.y) < 0.01) ca = &x;
        }
        if (!ca) { ++missing; if (!unnamed) ++missing_named; std::printf("     missing %s\n", cb.ref.c_str()); continue; }
        ++match;
        maxd = std::max(maxd, std::hypot(ca->pos.x - cb.pos.x, ca->pos.y - cb.pos.y));
        double dr = std::fabs(std::fmod(ca->rot - cb.rot + 720.0, 360.0)); if (dr > 180) dr = 360 - dr;
        if (dr > 0.01) { if (++rotmis <= 5) std::printf("     rot %s brd=%g odb=%g\n", cb.ref.c_str(), ca->rot, cb.rot); }
        if (ca->side != cb.side) ++sidemis;
        const Package* pa = a.package_of(*ca); const Package* pb = b.package_of(cb);
        if (!unnamed && ca->package.substr(0, ca->package.find('~')) != cb.package.substr(0, cb.package.find('~'))) {
            if (++pkgmis <= 5) std::printf("     pkg %s brd=%s odb=%s\n", cb.ref.c_str(), ca->package.c_str(), cb.package.c_str());
        }
        if (pa && pb && pa->pads.size() != pb->pads.size()) { if (++padmis <= 5) std::printf("     pads %s brd=%zu odb=%zu\n", cb.ref.c_str(), pa->pads.size(), pb->pads.size()); }
    }
    std::printf("   compare: odb=%zu brd=%zu matched=%d missing=%d max_dpos=%.6f mm rot_mismatch=%d side_mismatch=%d pkg_name_mismatch=%d pad_count_mismatch=%d\n",
                b.comps.size(), a.comps.size(), match, missing, maxd, rotmis, sidemis, pkgmis, padmis);
    std::printf("   outline: brd polylines=%zu odb polylines=%zu\n", a.board_frame.size(), b.board_frame.size());
    CHECK(missing_named == 0 && rotmis == 0 && sidemis == 0 && maxd < 0.01);
}

int main() {
    test_synthetic();
    test_failures();
    test_sample();
    std::printf("test_allegro_brd: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
