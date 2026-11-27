// ============================================================================
// test_gerber_set.cpp — LLM_API §3A.4 G1: headless Gerber set import (core/gerber_set).
// Folder walk (nested), classification, role/side guess, 274D + aperture list pairing,
// skip reasons, stack order, add_layers naming / digitize layer, parse_layer_ref round trip.
// Synthetic files only.
// ============================================================================
#include "../src/core/gerber_set.h"
#include "../src/core/fileio.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

using namespace pcbp;
namespace fs = std::filesystem;
static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void put(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}
static std::string rs274x(const char* body) {
    return std::string("%FSLAX24Y24*%\n%MOIN*%\n%ADD10C,0.0100*%\n%ADD11R,0.0600X0.0400*%\n") + body + "M02*\n";
}
static const gset::Loaded* find(const gset::SetResult& r, const char* stem) {
    for (auto& l : r.layers) if (l.ref.name == stem) return &l;
    return nullptr;
}
static bool skipped(const gset::SetResult& r, const char* name_part, const char* reason) {
    for (auto& s : r.skipped) if (s.path.find(name_part) != std::string::npos && s.reason == reason) return true;
    return false;
}

int main() {
    const fs::path d = fs::temp_directory_path() / "pcbp_test_gerber_set";
    std::error_code ec;
    fs::remove_all(d, ec);
    put(d / "demo.GTL", rs274x("D11*\nX10000Y10000D03*\nX20000Y10000D03*\nD10*\nX10000Y10000D02*\nX20000Y10000D01*\n"));
    put(d / "demo.GTP", rs274x("D11*\nX10000Y10000D03*\nX20000Y10000D03*\n"));
    put(d / "demo.GBL", rs274x("D11*\nX15000Y15000D03*\n"));
    put(d / "demo.GKO", rs274x("D10*\nX0Y0D02*\nX30000Y0D01*\nX30000Y20000D01*\nX0Y20000D01*\nX0Y0D01*\n"));
    put(d / "demo.GTO", rs274x(""));                                             // valid, no objects
    put(d / "notes.txt", "Board notes: use lead-free paste.\n");
    put(d / "drill" / "demo.drl", "M48\nINCH,LZ\nT1C0.035\n%\nT1\nX010000Y010000\nX020000Y010000\nM30\n");
    // RS-274D + its aperture list (paired by stem)
    put(d / "legacy" / "old.pho", "D11*\r\nX1000Y1000D03*\r\nX2000Y1000D03*\r\nD10*\r\nX1000Y1000D02*\r\nX2000Y2000D01*\r\nM02*\r\n");
    put(d / "legacy" / "old_apertures.txt", "; D code  size  shape\nD10 10 round DRAW\nD11 60X40 rect FLASH\n");

    gset::SetResult r;
    std::string err;
    CHECK(gset::load_gerber_set({d.string()}, r, err));
    for (auto& l : r.layers) std::printf("  %-10s role=%d side=%d objs=%d why=%s\n", l.ref.name.c_str(), (int)l.ref.role, (int)l.ref.side, l.objects, l.why.c_str());
    for (auto& s : r.skipped) std::printf("  skipped %s: %s\n", s.path.c_str(), s.reason.c_str());

    CHECK(r.layers.size() == 6);
    const gset::Loaded* gtl = find(r, "demo");   // several layers share the stem; check by role below
    CHECK(gtl != nullptr);
    int top_cu = 0, top_paste = 0, bot_cu = 0, outline = 0, drill = 0;
    for (auto& l : r.layers) {
        top_cu += l.ref.role == LayerRole::Copper && l.ref.side == Side::Top && l.ref.path.find(".GTL") != std::string::npos;
        top_paste += l.ref.role == LayerRole::Paste && l.ref.side == Side::Top;
        bot_cu += l.ref.role == LayerRole::Copper && l.ref.side == Side::Bottom;
        outline += l.ref.role == LayerRole::Outline;
        drill += l.ref.role == LayerRole::Drill && l.excellon;
        CHECK(!l.ref.content.empty() && l.objects > 0);
    }
    CHECK(top_cu == 1 && top_paste == 1 && bot_cu == 1 && outline == 1 && drill == 1);
    CHECK(skipped(r, "demo.GTO", "empty layer"));
    CHECK(skipped(r, "notes.txt", "not a Gerber"));
    // 274D got its aperture list and a guessed format, and parsed into 3 objects (2 flashes, 1 draw)
    const gset::Loaded* old = find(r, "old");
    CHECK(old && !old->ref.aperture_list.empty() && !old->ref.format.empty());
    CHECK(old && old->flashes == 2 && old->draws == 1);
    // stack order: bottom first, drill last
    CHECK(r.layers.front().ref.side == Side::Bottom || r.layers.front().ref.role == LayerRole::Other);
    CHECK(r.layers.back().ref.role == LayerRole::Drill);
    for (size_t i = 1; i < r.layers.size(); ++i) CHECK(gset::stack_rank(r.layers[i - 1].ref) <= gset::stack_rank(r.layers[i].ref));

    // ---- add_layers: unique names, prefix, digitize = top paste
    Project p;
    const int first = gset::add_layers(p, r);
    CHECK(first == 0 && p.layers.size() == r.layers.size());
    int digit = -1, demo_names = 0;
    for (int i = 0; i < (int)p.layers.size(); ++i) {
        if (p.layers[i].digitize) { CHECK(digit < 0); digit = i; }
        demo_names += p.layers[i].name.rfind("demo", 0) == 0;
    }
    CHECK(digit >= 0 && p.layers[digit].role == LayerRole::Paste && p.layers[digit].side == Side::Top);
    CHECK(demo_names == 5);
    std::set<std::string> uniq;
    for (auto& l : p.layers) uniq.insert(l.name);
    CHECK(uniq.size() == p.layers.size());
    // second set = pallet Gerbers with a prefix; digitize layer unchanged
    const int f2 = gset::add_layers(p, r, "Panel ");
    CHECK(f2 == (int)r.layers.size() && p.layers.size() == 2 * r.layers.size());
    CHECK(p.layers[f2].name.rfind("Panel ", 0) == 0);
    int nd = 0;
    for (auto& l : p.layers) nd += l.digitize;
    CHECK(nd == 1 && p.layers[digit].digitize);

    // ---- parse_layer_ref / format strings
    gerber::Format f;
    CHECK(gset::parse_format_string("in 2.3 L", f) && f.inch && f.int_digits == 2 && f.frac_digits == 3 && f.lead_supp);
    CHECK(gset::parse_format_string("mm 3.4 T", f) && !f.inch && !f.lead_supp);
    CHECK(gset::format_string(f) == "mm 3.4 T");
    CHECK(!gset::parse_format_string("", f));
    gerber::Layer L;
    GerberLayerRef copy = p.layers[digit];
    CHECK(gset::parse_layer_ref(copy, L, err) && L.flash_count() == 2);
    CHECK(gset::aperture_stem_key("C:\\x\\Board_Apertures.TXT") == "board");

    // ---- nothing readable
    gset::SetResult none;
    CHECK(!gset::load_gerber_set({(d / "does-not-exist").string()}, none, err) && !err.empty());

    fs::remove_all(d, ec);
    std::printf("test_gerber_set: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
