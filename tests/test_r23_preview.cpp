// ============================================================================
// test_r23_preview.cpp — R23a Files preview (specs/DOCUMENTS.md "R23"):
//   * Jet4 .mdb / .cpf bytes (synthetic image from tests/jet_synth.h) preview as sheets: "Tables" tab +
//     one tab per user table, header row, numbers right aligned; system tables hidden; garbage refused
//   * preview_tsv (Excel-pasteable copy) and preview_find / preview_count (Find bar) semantics
// Synthetic fixtures only.
// ============================================================================
#include "../src/io/jet_mdb.h"
#include "../src/io/sheet_preview.h"
#include "jet_synth.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace pcbp;
static int g_fail = 0, g_n = 0;
#define CHECK(c) do { ++g_n; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::vector<uint8_t> fixture() {
    using namespace jet_synth;
    Tbl parts{"Parts", {{"Id", 4}, {"PartNo", 10}, {"Value", 10}, {"Qty", 4}, {"Pitch", 7}}, {}};
    parts.rows.push_back({Val{false, 1}, Val{false, 0, 0, "R0805-10K"}, Val{false, 0, 0, "10k"}, Val{false, 25}, Val{false, 0, 2.0}});
    parts.rows.push_back({Val{false, 2}, Val{false, 0, 0, "C0603-100N"}, Val{false, 0, 0, "100n\tX7R"}, Val{false, 40}, Val{false, 0, 4.0}});
    parts.rows.push_back({Val{false, 3}, Val{false, 0, 0, "U-SOIC8"}, Val{true}, Val{false, 2}, Val{false, 0, 8.0}});
    Tbl feeders{"Feeders", {{"Slot", 4}, {"Part", 10}}, {}};
    feeders.rows.push_back({Val{false, 11}, Val{false, 0, 0, "R0805-10K"}});
    return build({parts, feeders});
}

static void test_jet() {
    auto img = fixture();
    CHECK(io::is_jet_bytes(img));
    jet::Database db; std::string err;
    CHECK(db.open_memory(img, err));
    CHECK(db.tables().size() == 2);
    io::SheetPreview sp;
    CHECK(io::load_sheet_preview(img, "job.cpf", sp, err));
    if (sp.tabs.size() != 3) { std::printf("tabs=%zu err=%s\n", sp.tabs.size(), err.c_str()); CHECK(false); return; }
    CHECK(sp.tabs[0].name == "Tables");
    CHECK(sp.tabs[0].at(1, 0) == "Feeders" && sp.tabs[0].at(1, 1) == "1");
    CHECK(sp.tabs[0].at(2, 0) == "Parts" && sp.tabs[0].at(2, 1) == "3" && sp.tabs[0].at(2, 2) == "5");
    const auto& p = sp.tabs[2];
    CHECK(p.name == "Parts" && p.rows == 4 && p.cols == 5);
    CHECK(p.at(0, 1) == "PartNo" && p.bold[1] == 1);
    CHECK(p.at(1, 1) == "R0805-10K" && p.at(1, 3) == "25" && p.right[(size_t)1 * 5 + 3] == 1);
    CHECK(p.at(2, 4) == "4" && p.at(3, 2).empty());
    CHECK(sp.how.find("Jet 4") != std::string::npos);
    // the same bytes as .mdb, and a non-database refused
    io::SheetPreview sp2;
    CHECK(io::load_sheet_preview(img, "lib.mdb", sp2, err) && sp2.tabs.size() == 3);
    std::vector<uint8_t> bad(img.begin(), img.begin() + 4096);
    io::SheetPreview sp3;
    CHECK(!io::load_sheet_preview(bad, "x.mdb", sp3, err));
    // corrupted pages never crash
    for (int k = 0; k < 200; ++k) {
        auto c = img;
        for (int j = 0; j < 40; ++j) c[(size_t)((k * 7919 + j * 104729) % c.size())] ^= (uint8_t)(k + j * 31);
        io::SheetPreview sx; std::string e2;
        io::load_sheet_preview(c, "fuzz.mdb", sx, e2);
    }
}

static void test_copy_find() {
    auto img = fixture();
    io::SheetPreview sp; std::string err;
    CHECK(io::load_sheet_preview(img, "job.mdb", sp, err) && sp.tabs.size() == 3);
    const auto& p = sp.tabs[2];
    CHECK(io::preview_tsv(p, 0, 1, 1, 2) == "PartNo\tValue\r\nR0805-10K\t10k\r\n");
    CHECK(io::preview_tsv(p, 2, 2, 2, 1) == "C0603-100N\t\"100n\tX7R\"\r\n");   // corners in any order, quoted TAB
    int r = 0, c = 0;
    CHECK(io::preview_find(p, "r0805", false, true, true, r, c) && r == 1 && c == 1);
    CHECK(!io::preview_find(p, "r0805", true, true, true, r, c));                 // match case
    r = 1; c = 1;
    CHECK(io::preview_find(p, "0", false, true, false, r, c) && r == 1 && c == 2); // "10k" next in row
    r = 0; c = 0;
    CHECK(io::preview_find(p, "SOIC", false, false, false, r, c) && r == 3 && c == 1);   // backwards wraps
    CHECK(io::preview_count(p, "0", false) >= 4);
    r = 0; c = 0;
    CHECK(!io::preview_find(p, "zzz", false, true, true, r, c));
}

int main() {
    test_jet();
    test_copy_find();
    std::printf("test_r23_preview: %d/%d checks passed\n", g_n - g_fail, g_n);
    return g_fail ? 1 : 0;
}
