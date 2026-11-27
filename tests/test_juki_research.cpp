// H8H research: rule sanity, writer integration (merge, blocks 7/16/18/19, line info), and - with private data in
// tests/local_data.map - how close PCB Plotter's research comes to the HLC editor's own:
//   research_db        FlexLine component DB (house)
//   research_truth     a program the editor researched (ground truth)
//   research_before    the same program before Research (its records are what Research merges into)
//   research_machine   the HLC software folder (SubCompo*.ini)
#include "core/fileio.h"
#include "core/machine_job.h"
#include "io/juki_autopart.h"
#include "io/juki_research.h"
#include "io/juki_resize.h"
#include "io/juki_writer.h"
#include "local_data.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>

using namespace pcbp;
using namespace pcbp::juki;
static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static MachineJob job_with(int n) {
    MachineJob j;
    j.program_name = "RT";
    j.machine = "juki.h8h";
    j.board_w = 100; j.board_h = 80;
    JobCircuit jc; jc.index = 1; jc.name = "1"; j.circuits.push_back(jc);
    j.fiducials.push_back({"F1", 5, 5, 1.0, 0, false, "", 1});
    j.fiducials.push_back({"F2", 95, 75, 1.0, 0, false, "", 2});
    for (int i = 0; i < n; ++i) {
        JobPart p; p.part = "PART" + std::to_string(i); p.description = "D" + std::to_string(i); p.count = 1;
        p.width = 1; p.length = 2; p.height = 0.5;
        j.parts.push_back(p);
        JobPlacement pl; pl.ref = "R" + std::to_string(i); pl.part = p.part; pl.x = 10 + i * 5; pl.y = 10;
        j.placements.push_back(pl);
    }
    return j;
}

static void synthetic() {
    // rules on synthetic rows: a tape chip with a 12 mm tape at pitch 4 mm (code 4), a tray part
    {
        std::map<std::string, long> row = {{"Compo.CompoID", 1}, {"Compo.PkgClassID", 1}, {"Compo.Width", 10000}, {"Compo.Length", 20000},
                                           {"Compo.Height", 5000}, {"Compo.DataOk", 1}, {"Feeder.CompoID", 1}, {"Feeder.FdrTypeID", 1},
                                           {"Feeder.TapeTypeID", 4}, {"Feeder.Pitch", 12}, {"Feeder.TapeAngle", -90}};
        auto get = [&](const char* t, const char* c, long& v) { auto it = row.find(std::string(t) + "." + c); if (it == row.end()) return false; v = it->second; return true; };
        auto text = [](const char*, const char*, std::string&) { return false; };
        std::vector<uint8_t> b(1822, 0);
        rules::compo(b.data(), get, text);
        rules::feeder(b.data(), get);
        CHECK(b[78] == 1 && b[81] == 0x10 && b[82] == 0x27 && (b[113] & 0x0F) == 0x0F && b[117] == 2);
        CHECK(b[80] == 1 && b[176] == 4 && b[178] == 12 && b[179] == 1 && b[177] == 1 && (b[183] & 3) == 3);
        CHECK((int16_t)(b[180] | b[181] << 8) == -90);
        row["Feeder.FdrTypeID"] = 4; row["Feeder.TrayTypeID"] = 3; row["Feeder.MTCPadPickUp"] = 1; row["Feeder.MTCPadShuttle"] = 2;
        std::fill(b.begin(), b.end(), 0);
        rules::compo(b.data(), get, text);
        rules::feeder(b.data(), get);
        CHECK(b[80] == 3 && b[176] == 2 && b[1130] == 0x12 && (b[224] & 0x40));
        rules::check(b.data());
        CHECK(b[117] == 1);   // no recognition switch: incomplete
    }
    // the record merge keeps the program's identity / counters and takes the database part
    {
        std::vector<uint8_t> prog(1822, 0x22), db(1822, 0x33);
        std::memcpy(prog.data(), "PROGNAME", 9);
        std::memcpy(db.data() + 47, "DESC\r\nX", 8);
        db[77] = 0;
        db[117] = 2;
        research_merge_b4(prog.data(), db.data());
        CHECK(!std::memcmp(prog.data(), "PROGNAME", 9));
        CHECK(!std::memcmp(prog.data() + 47, "DESC", 5) && prog[52] == 0 && !std::memcmp(prog.data() + 1205, "DESC", 5));
        CHECK(prog[500] == 0x33 && prog[120] == 0x22 && prog[1500] == 0x22 && prog[118] == 1 && prog[119] == 0);
    }

    // writer: without research data nothing changes; with it blocks 7/18/19 appear and the record is copied
    std::string err;
    MachineJob j = job_with(3);
    j.options["juki.autopart"] = "off";   // the built-in rules are tested in autopart()
    std::vector<uint8_t> plain, plain2, res;
    CHECK(write_hlc_variant(j, HlcVariant::H8H, plain, err));
    set_h8h_research_hook(nullptr);
    CHECK(write_hlc_variant(j, HlcVariant::H8H, plain2, err));
    CHECK(plain == plain2);
    HlcParsed hp;
    CHECK(parse_hlc(plain, hp, err) && hp.blocks[7].count == 0 && hp.blocks[18].count == 0);

    set_h8h_research_hook([](JobPart& p) {
        if (p.part == "PART2") return false;   // not in the DB: default record
        p.h8h_b4.assign(1822, 0x11);
        p.h8h_b7.assign(1052, 0);
        p.h8h_b18.assign(5240, 0);
        p.h8h_b19.assign(36, 0);
        if (p.part == "PART1") { p.h8h_b7[291] = 1; p.h8h_b7[0] = 5; p.h8h_b18[0] = 7; }   // has vision data
        return true;
    });
    HlcWriteInfo wi;
    CHECK(write_hlc_variant(j, HlcVariant::H8H, res, err, &wi));
    CHECK(wi.comps_researched == 2);
    HlcParsed h;
    CHECK(parse_hlc(res, h, err));
    CHECK(h.blocks[7].count == 3 && h.blocks[18].count == 3 && h.blocks[19].count == 3);
    CHECK(h.components.size() == 3 && h.components[0] == "PART0");
    std::vector<HlcCheck> ch;
    std::string e2;
    CHECK(verify_hlc_bytes(res, ch, e2) && hlc_check_ok(ch));
    auto b4 = [&](size_t i, size_t o) { return res[h.blocks[4].addr + 1822 * i + o]; };
    CHECK(b4(0, 500) == 0x11);                                 // researched record copied
    CHECK(b4(2, 500) != 0x11);                                 // not researched: default record
    CHECK(b4(1, 338) == 1 && b4(1, 339) == 0);                 // own index (vision part)
    CHECK(b4(0, 338) == 0);                                    // no vision: no index
    const size_t r7 = h.blocks[7].addr + 1052 * 1;
    CHECK(res[r7 + 291] == 1 && res[r7] == 5);
    CHECK(res[h.blocks[18].addr + 5240 * 1] == 7);
    // line info: in-use counts of blocks 7 / 18
    const size_t li = h.blocks[1].addr;
    auto u32 = [&](size_t o) { uint32_t x; std::memcpy(&x, &res[li + o], 4); return x; };
    CHECK(u32(162) == 1052 && u32(162 + 12) == 1);
    CHECK(u32(402) == 5240 && u32(402 + 12) == 1);
    // opt-out
    j.options["juki.research"] = "off";
    std::vector<uint8_t> off;
    CHECK(write_hlc_variant(j, HlcVariant::H8H, off, err) && off == plain);
    set_h8h_research_hook(nullptr);

    // custom-pattern BGAs: one ball grid per pattern name in block 16, the part's index at block 7 +135
    {
        MachineJob jb = job_with(3);
        set_h8h_research_hook([](JobPart& p) {
            p.h8h_b4.assign(1822, 0);
            p.h8h_b7.assign(1052, 0);
            p.h8h_b18.assign(5240, 0);
            p.h8h_b19.assign(36, 0);
            p.h8h_b7[0] = 2; p.h8h_b7[1] = 2; p.h8h_b7[121] = 0xFF;
            p.h8h_bga.assign(12830, 0);
            p.h8h_bga[0] = 1;
            std::memcpy(p.h8h_bga.data() + 4, p.part == "PART1" ? "PatternB" : "patterna", 8);
            if (p.part == "PART2") std::memcpy(p.h8h_bga.data() + 4, "PATTERNA", 8);   // same name, other case
            return true;
        });
        std::vector<uint8_t> o;
        CHECK(write_hlc_variant(jb, HlcVariant::H8H, o, err));
        HlcParsed hb;
        CHECK(parse_hlc(o, hb, err) && hb.blocks[16].count == 2 && hb.blocks[16].recsize == 12830);
        const size_t r7 = hb.blocks[7].addr;
        CHECK(o[r7 + 135] == 0 && o[r7 + 1052 + 135] == 1 && o[r7 + 2104 + 135] == 0);
        CHECK(!std::memcmp(&o[hb.blocks[16].addr + 4], "patterna", 8) && !std::memcmp(&o[hb.blocks[16].addr + 12830 + 4], "PatternB", 8));
        CHECK(hb.blocks[17].addr == hb.blocks[16].addr + 2 * 12830);
        std::vector<HlcCheck> c2;
        std::string e3;
        CHECK(verify_hlc_bytes(o, c2, e3) && hlc_check_ok(c2));
        const size_t li = hb.blocks[1].addr;
        uint32_t n16;
        std::memcpy(&n16, &o[li + 362 + 12], 4);
        CHECK(n16 == 2);
        set_h8h_research_hook(nullptr);
    }

    // element-group parts: lists renumbered program-wide into blocks 20 / 21 / 22, counted in the line info
    {
        MachineJob je = job_with(2);
        set_h8h_research_hook([](JobPart& p) {
            p.h8h_b4.assign(1822, 0);
            p.h8h_b7.assign(1052, 0);
            p.h8h_b18.assign(5240, 0);
            p.h8h_b19.assign(36, 0);
            uint8_t* h = p.h8h_b19.data();
            h[0] = 2; h[1] = 1; h[4] = 1; h[5] = 0;
            h[20] = 0; h[21] = 0; h[22] = 2; h[23] = 0; h[24] = h[25] = 0xFF;
            p.h8h_groups.assign(340, 0);
            p.h8h_elements.assign(112, 0);
            for (int g = 0; g < 2; ++g) {
                uint8_t* r = &p.h8h_groups[170 * (size_t)g];
                r[0] = 2; r[1] = 1;
                r[8] = g ? 0 : 0xFF; r[9] = g ? 0 : 0xFF;
                r[10] = g ? 0xFF : 1; r[11] = g ? 0xFF : 0;
                r[12] = g ? 0xFF : 0; r[13] = g ? 0xFF : 0;
                r[14] = g ? 0 : 1;
            }
            p.h8h_elements[0] = 2; p.h8h_elements[1] = 1;
            p.h8h_elements[12] = p.h8h_elements[13] = p.h8h_elements[14] = p.h8h_elements[15] = 0xFF;
            return true;
        });
        std::vector<uint8_t> o;
        CHECK(write_hlc_variant(je, HlcVariant::H8H, o, err));
        HlcParsed he;
        CHECK(parse_hlc(o, he, err) && he.blocks[20].count == 4 && he.blocks[21].count == 2 && he.blocks[22].count == 0);
        auto w16 = [&](size_t at) { return (uint16_t)(o[at] | o[at + 1] << 8); };
        const size_t b19 = he.blocks[19].addr, g = he.blocks[20].addr, el = he.blocks[21].addr;
        CHECK(w16(b19 + 20) == 0 && w16(b19 + 36 + 20) == 2 && w16(b19 + 36 + 22) == 2);
        CHECK(w16(g + 170 * 2 + 8) == 0xFFFF && w16(g + 170 * 2 + 10) == 3 && w16(g + 170 * 3 + 8) == 2 && w16(g + 170 * 2 + 12) == 1);
        CHECK(w16(el + 112 + 12) == 0xFFFF && w16(el + 112 + 14) == 0xFFFF);
        CHECK(he.blocks[22].addr == 0 && he.blocks[20].addr == b19 + 72 && el == g + 4 * 170);
        std::vector<HlcCheck> c3;
        std::string e4;
        CHECK(verify_hlc_bytes(o, c3, e4) && hlc_check_ok(c3));
        const size_t li = he.blocks[1].addr;
        uint32_t n;
        std::memcpy(&n, &o[li + 442 + 12], 4); CHECK(n == 4);
        std::memcpy(&n, &o[li + 462 + 16], 4); CHECK(n == 2);
        std::memcpy(&n, &o[li + 422 + 12], 4); CHECK(n == 2);
        set_h8h_research_hook(nullptr);
    }

    // index helper
    ResearchRecord r;
    r.b4.assign(1822, 0); r.b7.assign(1052, 0); r.b7[391] = 9;
    research_set_index(r, 300);
    CHECK(r.b4[338] == (300 & 0xFF) && r.b4[339] == 1 && r.b7[391] == (300 & 0xFF));
}

static void real() {
    const std::string dbp = pcbp_test::local("research_db"), truth = pcbp_test::local("research_truth");
    const std::string before = pcbp_test::local("research_before"), machine = pcbp_test::local("research_machine");
    if (dbp.empty() || truth.empty()) { std::printf("(real-data research report skipped: no local data)\n"); return; }
    std::string err;
    ResearchDb db;
    if (!db.open(dbp, err)) { std::printf("db: %s\n", err.c_str()); ++g_fail; return; }
    const int tables = machine.empty() ? 0 : db.load_machine_folder(machine);
    std::vector<uint8_t> t, t0;
    HlcParsed h, h0;
    if (!read_file(truth, t, err) || !parse_hlc(t, h, err)) { std::printf("truth: %s\n", err.c_str()); ++g_fail; return; }
    const bool have_before = !before.empty() && read_file(before, t0, err) && parse_hlc(t0, h0, err) && h0.components == h.components;
    std::printf("db %zu parts, %d acceleration tables, truth %zu components%s\n", db.parts(), tables, h.components.size(),
                have_before ? " (with the program before research)" : "");
    double tot[4] = {0, 0, 0, 0};
    int n = 0, exact = 0;
    int bga = 0;
    size_t el_bytes = 0, el_bad = 0;
    uint16_t g0 = 0, e0 = 0, a0 = 0;
    for (size_t i = 0; i < h.components.size(); ++i) {
        ResearchResult rr;
        if (!research_part(db, h.components[i], rr, err)) continue;
        research_set_index(rr.rec, (int)i);
        if (rr.rec.b19.size() == 36 && rr.rec.b19[1]) {
            std::vector<uint8_t> g = rr.rec.groups, e = rr.rec.elements, a = rr.rec.arrays;
            h8h_relocate_elements(rr.rec.b19, g, e, a, g0, e0, a0);
            const struct { const std::vector<uint8_t>* v; int blk; size_t rs; uint16_t at; } parts[3] = {{&g, 20, 170, g0}, {&e, 21, 112, e0}, {&a, 22, 3142, a0}};
            for (const auto& q : parts) {
                const auto& B = h.blocks[q.blk];
                for (size_t k = 0; k < q.v->size(); ++k) {
                    const size_t rec = q.at + k / q.rs, off = k % q.rs;
                    ++el_bytes;
                    if (rec >= B.count) { ++el_bad; continue; }
                    const uint8_t w = t[B.addr + B.prefix + q.rs * rec + off];
                    if (w == (*q.v)[k]) continue;
                    // name tail of an extended array: after the NUL of the name at +24
                    const uint8_t* nm = &(*q.v)[k - off + 24];
                    const size_t len = strnlen((const char*)nm, 31);
                    if (q.blk == 22 && off >= 24 + len + 1 && off < 55) continue;
                    ++el_bad;
                }
            }
            g0 = (uint16_t)(g0 + g.size() / 170); e0 = (uint16_t)(e0 + e.size() / 112); a0 = (uint16_t)(a0 + a.size() / 3142);
        }
        std::vector<uint8_t> b4 = rr.rec.b4;
        if (have_before) {
            const auto& B = h0.blocks[4];
            b4.assign(t0.begin() + B.addr + B.prefix + (size_t)B.recsize * i, t0.begin() + B.addr + B.prefix + (size_t)B.recsize * (i + 1));
            research_merge_b4(b4.data(), rr.rec.b4.data());
            b4[338] = rr.rec.has_vision() ? (uint8_t)i : 0;
            b4[339] = 0;
        }
        std::vector<uint8_t> b7 = rr.rec.b7;
        if (!rr.rec.bga.empty() && b7.size() == 1052) b7[135] = (uint8_t)bga++;   // ball grids in program order
        const int blks[4] = {4, 7, 18, 19};
        const std::vector<uint8_t>* g[4] = {&b4, &b7, &rr.rec.b18, &rr.rec.b19};
        size_t miss = 0;
        for (int b = 0; b < 4; ++b) {
            const auto& B = h.blocks[blks[b]];
            if (B.count <= i) continue;
            const uint8_t* w = &t[B.addr + B.prefix + (size_t)B.recsize * i];
            size_t m = 0, s = 0;
            for (uint32_t k = 0; k < B.recsize; ++k) {
                // without the program before research only the database part of the record can be compared
                if (!have_before && blks[b] == 4 && (k < 21 || (k >= 118 && k < 168) || (k >= 1143 && k < 1205) || (k >= 1333 && k < 1340) || k >= 1342)) continue;
                ++m;
                s += (k < g[b]->size() ? (*g[b])[k] : 0) == w[k];
            }
            miss += m - s;
            tot[b] += m ? 100.0 * s / m : 100.0;
        }
        exact += miss == 0;
        ++n;
    }
    if (!n) return;
    std::printf("  block 4 %.2f%%  block 7 %.2f%%  block 18 %.2f%%  block 19 %.2f%%  byte-exact parts %d/%d\n", tot[0] / n, tot[1] / n,
                tot[2] / n, tot[3] / n, exact, n);
    CHECK(tot[0] / n >= (have_before && tables == 3 ? 100.0 : 99.9));
    CHECK(tot[1] / n >= 99.99);
    CHECK(tot[2] / n >= 99.99);
    CHECK(tot[3] / n >= 99.99);
    // element lists (blocks 20 / 21 / 22) in program order; the only bytes allowed to differ are what an extended
    // array's name buffer holds after its terminating NUL (left-over heap text in the editor)
    std::printf("  element lists: %zu bytes compared, %zu differ outside name tails\n", el_bytes, el_bad);
    CHECK(el_bytes == 0 || el_bad == 0);
}

// the mapping runs: a DB where each part varies one column, researched by the editor (research_map_db +
// research_map = before|after;before|after...): every part that is researched must be byte-exact
static void mapping_runs() {
    const std::string dbp = pcbp_test::local("research_map_db"), runs = pcbp_test::local("research_map");
    const std::string machine = pcbp_test::local("research_machine");
    if (dbp.empty() || runs.empty()) return;
    std::string err;
    ResearchDb db;
    if (!db.open(dbp, err)) { std::printf("map db: %s\n", err.c_str()); ++g_fail; return; }
    if (!machine.empty()) db.load_machine_folder(machine);
    for (size_t a = 0, z; a < runs.size(); a = z + 1) {
        z = runs.find(';', a);
        if (z == std::string::npos) z = runs.size();
        const std::string pair = runs.substr(a, z - a);
        const size_t bar = pair.find('|');
        if (bar == std::string::npos) continue;
        std::vector<uint8_t> t0, t;
        HlcParsed h0, h;
        if (!read_file(pair.substr(0, bar), t0, err) || !parse_hlc(t0, h0, err) || !read_file(pair.substr(bar + 1), t, err) ||
            !parse_hlc(t, h, err) || h0.components.size() < h.components.size()) { std::printf("map run %s: %s\n", pair.c_str(), err.c_str()); ++g_fail; continue; }
        int n = 0, exact = 0, skipped = 0, renamed = 0;
        size_t el_bad = 0, el_n = 0;
        uint16_t g0 = 0, e0 = 0, a0 = 0;
        for (size_t i = 0; i < h.components.size(); ++i) {
            ResearchResult rr;
            if (!research_part(db, h.components[i], rr, err)) { ++skipped; continue; }
            research_set_index(rr.rec, (int)i);
            if (rr.rec.b19.size() == 36 && rr.rec.b19[1]) {
                std::vector<uint8_t> g = rr.rec.groups, e = rr.rec.elements, a = rr.rec.arrays;
                h8h_relocate_elements(rr.rec.b19, g, e, a, g0, e0, a0);
                const struct { const std::vector<uint8_t>* v; int blk; size_t rs; uint16_t at; } ls[3] = {{&g, 20, 170, g0}, {&e, 21, 112, e0}, {&a, 22, 3142, a0}};
                for (const auto& q : ls) {
                    const auto& B = h.blocks[q.blk];
                    for (size_t k = 0; k < q.v->size(); ++k) {
                        const size_t rec = q.at + k / q.rs;
                        ++el_n;
                        el_bad += rec >= B.count || t[B.addr + B.prefix + q.rs * rec + k % q.rs] != (*q.v)[k];
                    }
                }
                g0 = (uint16_t)(g0 + g.size() / 170); e0 = (uint16_t)(e0 + e.size() / 112); a0 = (uint16_t)(a0 + a.size() / 3142);
            }
            if (h0.components[i] != h.components[i]) { ++renamed; continue; }   // renamed in the editor: no record to merge into
            const auto& B0 = h0.blocks[4];
            std::vector<uint8_t> b4(t0.begin() + B0.addr + B0.prefix + 1822 * i, t0.begin() + B0.addr + B0.prefix + 1822 * (i + 1));
            research_merge_b4(b4.data(), rr.rec.b4.data());
            b4[338] = rr.rec.has_vision() ? (uint8_t)i : 0;
            b4[339] = rr.rec.has_vision() ? (uint8_t)(i >> 8) : 0;
            const int blks[4] = {4, 7, 18, 19};
            const std::vector<uint8_t>* g[4] = {&b4, &rr.rec.b7, &rr.rec.b18, &rr.rec.b19};
            bool same = true;
            for (int b = 0; b < 4 && same; ++b) {
                const auto& B = h.blocks[blks[b]];
                if (B.count <= i) continue;
                const uint8_t* w = &t[B.addr + B.prefix + (size_t)B.recsize * i];
                for (uint32_t k = 0; k < B.recsize && same; ++k) same = (k < g[b]->size() ? (*g[b])[k] : 0) == w[k];
            }
            exact += same;
            ++n;
        }
        std::printf("  mapping run %s: %d/%d byte-exact, element lists %zu bytes / %zu differ (%d renamed, %d not in the DB)\n",
                    pair.substr(bar + 1).c_str(), exact, n, el_n, el_bad, renamed, skipped);
        CHECK(exact == n);
        CHECK(el_bad == 0);
    }
}

// part names: <= 20 characters stay; longer keep their first 20 when free, else the beginning + "~" + the end
static void names() {
    auto nm = [](std::vector<std::string> v, std::vector<std::string> pref = {}) {
        std::vector<std::string> o;
        for (auto& m : machine_names(v, pref)) o.push_back(m.name);
        return o;
    };
    // short names and a unique long one
    auto a = nm({"R1K", "GRM188R71H104KA93D", "CL10A106MQ8NNNC-ABCDEF"});
    CHECK(a[0] == "R1K" && a[1] == "GRM188R71H104KA93D" && a[2] == "CL10A106MQ8NNNC-ABCD");
    // two long part numbers with the same first 20: the beginning stays, the ends tell them apart
    auto b = nm({"ABCDEFGHIJKLMNOPQRSTUV-TR", "ABCDEFGHIJKLMNOPQRSTUV-CT"});
    CHECK(b[0] == "ABCDEFGHIJKLMNOPQ~TR" && b[1] == "ABCDEFGHIJKLMNOPQ~CT");
    CHECK(b[0].size() == 20 && b[1].size() == 20);
    // a long part number whose first 20 are another part's whole name: never takes that name
    auto c = nm({"ABCDEFGHIJKLMNOPQRSTU1", "ABCDEFGHIJKLMNOPQRST"});
    CHECK(c[1] == "ABCDEFGHIJKLMNOPQRST" && c[0] == "ABCDEFGHIJKLMNOPQR~1");
    // ends that are the same: the shortest end that differs
    auto d = nm({"XXXXXXXXXXXXXXXXXXXXA-REEL", "XXXXXXXXXXXXXXXXXXXXB-REEL"});
    CHECK(d[0] != d[1] && d[0].size() <= 20 && d[0].rfind("XXXXXXXX", 0) == 0 && d[0].find('~') != std::string::npos);
    CHECK(d[0] == "XXXXXXXXXXXXX~A-REEL" && d[1] == "XXXXXXXXXXXXX~B-REEL");
    // ends equal all the way: numbered
    auto e = nm({"SAMESAMESAMESAMESAMESAME1X", "SAMESAMESAMESAMESAMESAME1X"});
    CHECK(e[0] != e[1] && e[0].size() <= 20 && e[1].size() <= 20);
    // case-insensitive, and a name clash after text conversion
    auto f = nm({"abc", "ABC"});
    CHECK(f[0] == "abc" && f[1] == "ABC~1");
    // the DB's machine name wins when free
    auto g = nm({"LONGPARTNUMBER-1234567-XYZ"}, {"LONGPART~XYZ"});
    CHECK(g[0] == "LONGPART~XYZ");
    // full names stay in the long-name field; how they were changed
    auto m = machine_names({"ABCDEFGHIJKLMNOPQRSTUV-TR", "ABCDEFGHIJKLMNOPQRSTUV-CT", "SHORT", "UNIQUE-LONG-PART-NUMBER-1"});
    CHECK(m[0].full == "ABCDEFGHIJKLMNOPQRSTUV-TR" && m[0].how == MachineName::Renamed);
    CHECK(m[2].how == MachineName::Same && m[3].how == MachineName::Truncated && m[3].name == "UNIQUE-LONG-PART-NUM");
    // every name unique on a larger set built to clash
    std::vector<std::string> many;
    for (int i = 0; i < 300; ++i) many.push_back("RC0402FR-0710KL-" + std::to_string(1000 + i) + (i % 2 ? "-T" : "-C"));
    many.push_back("RC0402FR-0710KL-1000");
    std::set<std::string> u;
    for (auto& x : machine_names(many)) { std::string k = x.name; for (auto& ch : k) ch = (char)std::toupper((unsigned char)ch); u.insert(k); CHECK(x.name.size() <= 20); }
    CHECK(u.size() == many.size());
}

// built-in rules: complete records for every family, without any database
static void autopart() {
    struct T { const char* pkg; double l, w, h; int leads, cls, fk; int want_cls, want_fdr; };
    const T ts[] = {{"0402", 1.0, 0.5, 0.35, 2, 0, 0, 1, 1}, {"0603", 1.6, 0.8, 0, 2, 0, 0, 1, 1}, {"SOT-23", 2.9, 1.3, 1.0, 3, 0, 0, 11, 1},
                    {"SOIC-8", 4.9, 3.9, 1.5, 8, 0, 0, 101, 1}, {"LQFP-64", 10, 10, 1.4, 64, 0, 0, 102, 3},
                    {"QFN-32 5x5", 5, 5, 0.9, 32, 0, 0, 120, 1}, {"BGA-256", 17, 17, 1.5, 256, 0, 0, 107, 3},
                    {"MELF", 3.5, 1.4, 1.4, 2, 0, 0, 3, 1}, {"CONN", 20, 6, 5, 10, 201, 0, 201, 1}, {"SOIC-16", 10, 3.9, 1.5, 16, 0, 2, 101, 2},
                    {"QFP-208", 28, 28, 3.4, 208, 0, 0, 102, 3}, {"ODD", 7, 3, 2, 0, 0, 0, 101, 1}};
    for (const T& t : ts) {
        JobPart p;
        p.part = p.package = t.pkg; p.length = t.l; p.width = t.w; p.height = t.h; p.leads = t.leads; p.type_code = t.cls; p.feeder_kind = t.fk;
        ResearchResult r;
        std::string why;
        CHECK(builtin_research(p, r, why));
        const uint8_t* b = r.rec.b4.data();
        CHECK(r.rec.b4.size() == 1822 && b[117] == 2);           // complete
        CHECK((b[78] | b[79] << 8) == t.want_cls && b[80] == t.want_fdr);
        CHECK(b[392] == 1 && b[246] == 0);                        // laser centred, no vision data
        if (t.want_fdr == 1) CHECK(b[178] > 0);                   // a pitch the feeder can feed
        if (!(t.h > 0)) CHECK(why.find("height") != std::string::npos);
    }
    JobPart none;
    none.part = "NOSIZE";
    ResearchResult r;
    std::string why;
    CHECK(!builtin_research(none, r, why));

    // the writer: every part gets machine data; names stay unique; the check reports what was generated
    MachineJob j = job_with(3);
    j.parts[0].part = j.placements[0].part = "ABCDEFGHIJKLMNOPQRSTUV-TR";
    j.parts[1].part = j.placements[1].part = "ABCDEFGHIJKLMNOPQRSTUV-CT";
    j.parts[2].width = j.parts[2].length = 0;                    // no size: no data
    std::vector<uint8_t> out;
    std::string err;
    HlcWriteInfo wi;
    set_h8h_research_hook(nullptr);
    CHECK(h8h_autopart_available());
    CHECK(write_hlc_variant(j, HlcVariant::H8H, out, err, &wi));
    CHECK(wi.comps_from_rules == 2 && wi.comps_researched == 0 && wi.generated.size() == 2);
    CHECK(wi.renamed.size() == 2);
    HlcParsed h;
    CHECK(parse_hlc(out, h, err) && h.components.size() == 3);
    CHECK(h.components[0] == "ABCDEFGHIJKLMNOPQ~TR" && h.components[1] == "ABCDEFGHIJKLMNOPQ~CT");
    const size_t r0 = h.blocks[4].addr;
    CHECK(out[r0 + 117] == 2 && out[r0 + 1822 + 117] == 2);       // complete records
    CHECK(out[r0 + 1143] == 1 && !std::memcmp(&out[r0 + 1144], "ABCDEFGHIJKLMNOPQRSTUV-TR", 26));   // full name kept
    std::vector<HlcCheck> ch;
    std::string e2;
    CHECK(verify_hlc_bytes(out, ch, e2) && hlc_check_ok(ch));
    std::vector<ExportIssue> issues;
    check_job(j, issues);
    int gen = 0, ren = 0, none_w = 0;
    for (auto& i : issues) {
        if (i.msg.find("built-in rules") != std::string::npos) ++gen;
        if (i.msg.find("clash") != std::string::npos) ++ren;
        if (i.msg.find("no machine data") != std::string::npos) ++none_w;
    }
    CHECK(gen == 1 && ren == 2 && none_w == 1);
    // opt-out
    j.options["juki.autopart"] = "off";
    CHECK(write_hlc_variant(j, HlcVariant::H8H, out, err, &wi) && wi.comps_from_rules == 0);

    // a library template: the profile's rows with the part's own body / carrier over them (template_rows)
    PartRows ov = template_rows("NEW PART", 2.0, 1.25, 0.6, 2, 4, 504);
    double v = 0;
    bool null = false;
    CHECK(ov.get("compo", "width", v, null) && v == 20000 && !null);
    CHECK(ov.get("Feeder", "TapeTypeID", v, null) && v == 2);
    CHECK(ov.get("Expansion", "AutoTeach", v, null) && v == 0);
    // over built-in rows (no DB): the override wins where the table exists, absent tables stay absent
    AutoPart ap;
    JobPart chip;
    chip.part = "C1"; chip.package = "0805"; chip.length = 2.0; chip.width = 1.25; chip.height = 0.6;
    CHECK(builtin_rows(builtin_input(chip), ap, why));
    for (auto& [t, cols] : ov.num) for (auto& [c, x] : cols) ap.rows.num[t][c] = x;
    CHECK(research_record(ap.rows, r) && research_rows(ResearchDb(), 0, ap.rows, r, err) && r.rec.b4[117] == 2 && r.rec.b4[176] == 2 && r.rec.b4[178] == 4);
}

// private: every profile of the package library as a template, with a part's body / carrier / nozzle put over it
static void library_templates() {
    const std::string lp = pcbp_test::local("teach_lib");
    ResearchDb lib;
    std::string err;
    if (lp.empty() || !lib.open(lp, err)) { std::printf("  (no teach_lib: library templates skipped)\n"); return; }
    int n = 0, same = 0, complete_as_is = 0, complete = 0;
    for (const std::string& nm : lib.names()) {
        const long id = lib.id_of(nm);
        ResearchResult a, b;
        if (!research_rows(lib, id, PartRows(), a, err)) continue;
        ++n;
        if (a.rec.b4[117] == 2) ++complete_as_is;
        // no override: exactly the profile's research
        CHECK(research_rows(lib, id, PartRows(), b, err) && a.rec.b4 == b.rec.b4);
        double w = 0, l = 0, ok = 0;
        lib.value(id, "Compo", "Width", w);
        lib.value(id, "Compo", "Length", l);
        lib.value(id, "Compo", "DataOk", ok);
        // the profile's own body as the part's body (long edge as Width, as teaching writes it): no change
        const PartRows ov = template_rows("", w / 1e4, l / 1e4, 0, 0, 0, 0);
        CHECK(research_rows(lib, id, ov, b, err));
        if (w >= l && ok == 1) { ++same; CHECK(b.rec.b4 == a.rec.b4); }
        // a body 10 % larger: still complete when the profile was
        const PartRows ov2 = template_rows("X", w / 1e4 * 1.1, l / 1e4 * 1.1, 0, 0, 0, 0);
        CHECK(research_rows(lib, id, ov2, b, err));
        if (b.rec.b4[117] == 2) ++complete;
    }
    std::printf("  library templates: %d profiles (%d checked against their own body), %d complete (%d as is)\n", n, same, complete, complete_as_is);
    CHECK(n > 0 && same > 0 && complete >= complete_as_is);
}

// size edits (Juki Tool): nozzle / vacuum / laser height follow the size; nothing else in the record moves
static void resize() {
    BuiltinInput in; in.cls = 1; in.x = 2.0; in.y = 1.25; in.h = 0.6;   // 0805 chip
    ResearchResult a, fresh;
    std::string why;
    CHECK(builtin_research(in, a, why));
    std::vector<uint8_t> b = a.rec.b4;
    ResizeResult r;
    CHECK(resize_component(b.data(), 3.2, 1.6, 0.6, r));          // -> 1206
    CHECK(r.before.gx4_nozzle == 500 && r.after.gx4_nozzle == 504 && !r.changes.empty());
    BuiltinInput in2 = in; in2.x = 3.2; in2.y = 1.6;
    CHECK(builtin_research(in2, fresh, why) && b == fresh.rec.b4);  // the same record as a part made at that size
    // a record the rules did not make: only the size-dependent fields change
    std::vector<uint8_t> c = a.rec.b4;
    for (size_t o : {704u, 705u, 709u, 176u, 178u, 1135u}) c[o] ^= 0x5A;   // retries, over-pick, feeder, ... (ours)
    std::vector<uint8_t> c0 = c;
    CHECK(resize_component(c.data(), 6.4, 3.2, 1.0, r));        // -> 2512
    CHECK(r.after.gx4_nozzle == 505 && r.after.laser_height == -5000);
    for (size_t o : {704u, 705u, 709u, 176u, 178u, 1135u}) CHECK(c[o] == c0[o]);
    int moved = 0; for (size_t o = 0; o < c.size(); ++o) moved += c[o] != c0[o];
    CHECK(moved > 0 && moved < 40);
    CHECK(component_info(c.data()).x == 6.4 && component_info(c.data()).h == 1.0);
    // the feeder: a suggestion only; apply_feeder writes it
    CHECK(r.suggest_tape_w == 12 && r.suggest_pitch == 8 && !r.suggest_paper);
    std::vector<uint8_t> d = a.rec.b4;
    CHECK(resize_component(d.data(), 6.4, 3.2, 1.0, r) && r.feeder_differs && component_info(d.data()).tape_w == 8);
    CHECK(apply_feeder(d.data(), 12, 8, false));
    ComponentInfo ci = component_info(d.data());
    CHECK(ci.tape_w == 12 && ci.pitch == 8 && !ci.paper);
    CHECK(!r.feeder_off);                                          // 8 -> 12 mm: one width, not worth a warning
    {   // a 56 mm feeder on a 2 mm part, and a laser part with no nozzle: flagged
        std::vector<uint8_t> g = a.rec.b4;
        CHECK(apply_feeder(g.data(), 56, 8, false));
        ResizeResult q;
        CHECK(preview_resize(g.data(), 2.0, 1.25, 0.6, q) && q.feeder_off && q.suggest_tape_w == 8);
        g[424] = g[425] = 0;
        CHECK(preview_resize(g.data(), 2.0, 1.25, 0.6, q) && q.nozzle_off);
        std::vector<uint8_t> k = a.rec.b4;
        CHECK(preview_resize(k.data(), 2.0, 1.25, 0.6, q) && !q.nozzle_off && !q.feeder_off);
    }
    std::string e;
    CHECK(!apply_feeder(d.data(), 8, 8, true, &e) && !e.empty());   // 8 mm tape cannot feed 8 mm
    // out of range, and a preview leaves the record alone
    CHECK(!resize_component(d.data(), 0, 1, 1, r));
    std::vector<uint8_t> p = d;
    CHECK(preview_resize(p.data(), 1.0, 0.5, 0.35, r) && p == d && r.after.gx4_nozzle == 500);
    // a vision-only part keeps its nozzle
    std::vector<uint8_t> v = a.rec.b4;
    v[392] = 0; v[226] = 0; v[246] = 1;
    std::vector<uint8_t> v0 = v;
    CHECK(resize_component(v.data(), 12, 12, 1.4, r) && r.why.find("vision") != std::string::npos);
    CHECK(std::equal(v.begin() + 392, v.end(), v0.begin() + 392));
}

// private: the nozzle the rules give each part of the house DB at its own size vs the one the DB has
static void resize_vs_db() {
    const std::string dbp = pcbp_test::local("research_db");
    ResearchDb db;
    std::string err;
    if (dbp.empty() || !db.open(dbp, err)) { std::printf("  (no research_db: nozzle agreement skipped)\n"); return; }
    int n = 0, same = 0;
    for (const std::string& nm : db.names()) {
        ResearchResult r;
        if (!research_part(db, nm, r, err)) continue;
        const ComponentInfo ci = component_info(r.rec.b4.data());
        if (!ci.laser || ci.x <= 0 || ci.y <= 0 || !ci.gx4_nozzle) continue;
        ResizeResult rr;
        if (!preview_resize(r.rec.b4.data(), ci.x, ci.y, ci.h, rr)) continue;
        ++n;
        same += rr.after.gx4_nozzle == ci.gx4_nozzle;
    }
    std::printf("  resize nozzle vs house DB: %d of %d laser parts the same (%.1f%%)\n", same, n, n ? 100.0 * same / n : 0.0);
    CHECK(n == 0 || same * 100 >= n * 85);
}

int main() {
    resize();
    resize_vs_db();
    names();
    library_templates();
    autopart();
    synthetic();
    real();
    mapping_runs();
    std::printf("\ntest_juki_research: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
