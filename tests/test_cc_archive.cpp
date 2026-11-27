// ============================================================================
// test_cc_archive.cpp — decodes every blob extracted from cclib.mdb by
// tools/cc_extract_blobs.py (tests/data/*.bin + index.tsv):
//   * every Packages.Rendering decodes with no trailing bytes, pad count ==
//     Packages.nLeads, pins 1..n unique, pin 1 present;
//   * known geometry (DIP16 100-mil pitch, SOIC8 50-mil pitch, 0805) in mm;
//   * every other CC_COMPRESS graphics blob decodes completely;
//   * inflate/zlib self-checks.
// usage: test_cc_archive [data_dir]   (default: tests/data)
// Build: g++ -std=c++20 -I src tests/test_cc_archive.cpp src/io/cc_archive.cpp
//        src/io/inflate.cpp src/core/project.cpp(optional)
// ============================================================================
#include "../src/io/cc_archive.h"
#include "../src/io/inflate.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace pcbp;

static int g_fail = 0;
#define CHECK(c, ...)                                                    \
    do {                                                                 \
        if (!(c)) {                                                      \
            ++g_fail;                                                    \
            std::printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #c);     \
            std::printf(__VA_ARGS__);                                    \
            std::printf("\n");                                           \
        }                                                                \
    } while (0)

static bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// pull "key": "value" out of the row JSON written by cc_extract_blobs.py
static std::string json_field(const std::string& js, const std::string& key) {
    const std::string k = "\"" + key + "\": \"";
    const size_t a = js.find(k);
    if (a == std::string::npos) return {};
    const size_t b = js.find('"', a + k.size());
    return b == std::string::npos ? std::string() : js.substr(a + k.size(), b - a - k.size());
}

static bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

static void test_inflate() {
    // fixed-Huffman deflate of "hello hello hello" made by zlib (wbits=15)
    const uint8_t z[] = {0x78, 0xda, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0xc8, 0x40, 0x90,
                         0x00, 0x3a, 0x2e, 0x06, 0x7d};
    std::vector<uint8_t> out;
    std::string err;
    CHECK(zlib_uncompress(z, sizeof z, out, 0, err), "%s", err.c_str());
    CHECK(std::string(out.begin(), out.end()) == "hello hello hello", "got '%s'",
          std::string(out.begin(), out.end()).c_str());
    // stored block: BFINAL=1, BTYPE=00, LEN=3, NLEN=~3, "abc"
    const uint8_t st[] = {0x01, 0x03, 0x00, 0xfc, 0xff, 'a', 'b', 'c'};
    CHECK(inflate_raw(st, sizeof st, out, 0, err) && out.size() == 3 && out[2] == 'c', "%s", err.c_str());
    // corrupt / truncated inputs must fail cleanly
    CHECK(!inflate_raw(st, 4, out, 0, err), "truncated stored block accepted");
    const uint8_t bad[] = {0x07};   // BTYPE=3
    CHECK(!inflate_raw(bad, 1, out, 0, err), "block type 3 accepted");
    uint8_t zb[sizeof z];
    std::copy(z, z + sizeof z, zb);
    zb[sizeof zb - 1] ^= 1;
    CHECK(!zlib_uncompress(zb, sizeof zb, out, 0, err), "bad adler accepted");
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data";
    test_inflate();

    std::ifstream idx(dir + "/index.tsv");
    if (!idx) {
        // the extracted blobs are local-only (not in the public repo): the synthetic inflate test above still ran
        std::printf("SKIP blob decode: no %s/index.tsv (local data from tools/cc_extract_blobs.py)\n", dir.c_str());
        return 0;
    }
    int pkgs = 0, pkg_ok = 0, other = 0, other_ok = 0;
    std::map<std::string, cc::PackageRendering> byname;
    std::string line;
    while (std::getline(idx, line)) {
        std::vector<std::string> col;
        std::stringstream ss(line);
        std::string c;
        while (std::getline(ss, c, '\t')) col.push_back(c);
        if (col.size() < 6) continue;
        const std::string &table = col[0], &column = col[1], &key = col[2], &file = col[3];
        std::vector<uint8_t> blob;
        if (!read_file(dir + "/" + file, blob)) { CHECK(false, "missing %s", file.c_str()); continue; }

        if (table == "Packages" && column == "Rendering") {
            ++pkgs;
            std::string err;
            cc::GrObject root;
            const bool ok = cc::decode_blob(blob.data(), blob.size(), root, err, "CGrGroup");
            CHECK(ok, "%s: %s", key.c_str(), err.c_str());
            if (!ok) continue;
            cc::PackageRendering r;
            CHECK(cc::decode_package_rendering(blob.data(), blob.size(), r, err), "%s: %s", key.c_str(), err.c_str());
            const int nleads = std::atoi(json_field(col[5], "nLeads").c_str());
            CHECK(int(r.pads.size()) == nleads, "%s: %zu pads, nLeads=%d", key.c_str(), r.pads.size(), nleads);
            std::set<int> pins;
            for (const auto& p : r.pads) {
                pins.insert(p.pin);
                CHECK(p.w > 0 && p.h > 0, "%s pin %d has empty size", key.c_str(), p.pin);
            }
            CHECK(int(pins.size()) == nleads && (pins.empty() || (*pins.begin() == 1 && *pins.rbegin() == nleads)),
                  "%s: pins not 1..%d", key.c_str(), nleads);
            CHECK(r.pin_one_index == 0, "%s: pin one index %d", key.c_str(), r.pin_one_index);
            CHECK(!r.artwork.empty(), "%s: no artwork", key.c_str());
            Package pkg;
            CHECK(cc::decode_package_rendering(blob.data(), blob.size(), pkg, err), "%s", err.c_str());
            CHECK(pkg.pads.size() == r.pads.size(), "%s: Package pads", key.c_str());
            if (ok && int(r.pads.size()) == nleads) ++pkg_ok;
            byname[key] = std::move(r);
        } else if (blob.size() >= 32 && std::string(blob.begin(), blob.begin() + 11) == "CC_COMPRESS" &&
                   column != "MachProp") {
            ++other;
            std::string err;
            cc::GrObject root;
            const bool ok = cc::decode_blob(blob.data(), blob.size(), root, err);
            CHECK(ok, "%s.%s[%s]: %s", table.c_str(), column.c_str(), key.c_str(), err.c_str());
            std::vector<GPrim> prims;
            CHECK(cc::decode_graphics(blob.data(), blob.size(), prims, err) && !prims.empty(),
                  "%s.%s[%s]: no prims", table.c_str(), column.c_str(), key.c_str());
            if (ok) ++other_ok;
        }
    }

    // ---- geometry spot checks (units: mils -> mm, +Y up) ----
    if (byname.count("DIP16")) {
        const auto& r = byname["DIP16"];
        CHECK(r.pads.size() == 16, "DIP16 pads");
        if (r.pads.size() == 16) {
            CHECK(near(r.pads[0].pos.x, 3.81) && near(r.pads[0].pos.y, -8.89), "DIP16 pin1 at (%g,%g)",
                  r.pads[0].pos.x, r.pads[0].pos.y);
            CHECK(near(r.pads[1].pos.y - r.pads[0].pos.y, 2.54), "DIP16 pitch %g", r.pads[1].pos.y - r.pads[0].pos.y);
            CHECK(near(r.pads[8].pos.x, -3.81), "DIP16 row spacing");
            CHECK(r.pads[0].shape == PadShape::Square && r.pads[1].shape == PadShape::Round, "DIP16 pad shapes");
            CHECK(near(r.pads[1].w, 70 * 0.0254), "DIP16 pad dia %g", r.pads[1].w);
        }
    }
    if (byname.count("SOIC8")) {
        const auto& r = byname["SOIC8"];
        if (r.pads.size() == 8) {
            CHECK(near(r.pads[0].pos.x, -2.54) && near(r.pads[0].pos.y, 1.905), "SOIC8 pin1 (%g,%g)",
                  r.pads[0].pos.x, r.pads[0].pos.y);
            CHECK(near(r.pads[0].pos.y - r.pads[1].pos.y, 1.27), "SOIC8 pitch");
            CHECK(near(r.pads[0].w, 84 * 0.0254) && near(r.pads[0].h, 34 * 0.0254), "SOIC8 pad size");
            CHECK(near(r.pads[4].pos.x, 2.54) && near(r.pads[4].pos.y, -1.905), "SOIC8 pin5 (CCW numbering)");
        }
    }
    if (byname.count("0805")) {
        const auto& r = byname["0805"];
        if (r.pads.size() == 2) {
            CHECK(near(r.pads[0].pos.x, 55 * 0.0254) && near(r.pads[1].pos.x, -55 * 0.0254), "0805 pads");
            CHECK(r.highlight.size() == 1 && r.highlight[0].kind == GKind::Rect, "0805 highlight");
        }
    }

    std::printf("packages: %d/%d decoded with pad count == nLeads; other graphics blobs: %d/%d\n",
                pkg_ok, pkgs, other_ok, other);
    CHECK(pkgs == 25, "expected 25 package renderings, saw %d", pkgs);
    std::printf(g_fail ? "FAILED (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
