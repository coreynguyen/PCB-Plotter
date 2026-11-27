// Teaching FlexLine components from machine history + body size.
// Synthetic checks always run; the effectiveness report runs on private data when tests/local_data.map has
//   teach_db       FlexLine DB with hand-taught parts (DataOk = 1 with a Feeder row = ground truth)
//   teach_lib      profile library (.flx)
//   teach_history  pnp_harvest *_components.csv list (|-separated)
//   teach_cpf      cpf_harvest *_parts.csv (optional)
//   part_db        house DB the text classifier learns conventions from (optional)
#include "io/flexdb.h"
#include "io/machine_history.h"
#include "local_data.h"

#include <cmath>
#include <cstdlib>
#include <vector>
#include <cstdio>
#include <map>
#include <string>

using namespace pcbp;
static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static void synthetic() {
    using namespace history;
    CHECK(tape_from_code("0x0101") == 1);
    CHECK(tape_from_code("0x0003") == 3);
    CHECK(tape_from_code("8MMEMBOSS") == 2);
    CHECK(tape_from_code("") == 0);
    CHECK(pitch_from_code("0x0004") == 4);
    CHECK(pitch_from_code("0x02CC") == 204);
    CHECK(pitch_from_code("12") == 12);
    CHECK(nozzle_from_field(".e48", "501") == 500);
    CHECK(nozzle_from_field(".x52", "506 0") == 505);
    CHECK(nozzle_from_field(".iss", "505") == 505);
    CHECK(nozzle_from_field(".s01x", "7501") == 500);
    CHECK(nozzle_from_field(".h8h", "KE:103 GX:507 FX3:0") == 506);
    CHECK(nozzle_from_field(".e48", "0") == 0);
    CHECK(chip_code_in("CAP CER 22PF 50V NP0 0402") == "0402");
    CHECK(chip_code_in("RES 10.0603K") == "");
    double l, w;
    CHECK(chip_dims("0603", l, w) && l == 1.6 && w == 0.8);

    // package evidence
    {
        PkgBody b;
        CHECK(package_code("DIODE SWITCH 100V 0.15A SOD123", b) && b.code == "SOD123");
        CHECK(package_code("DIODE ZENER 14V 310MW SOD323F", b) && b.code == "SOD323");
        CHECK(package_code("IC REG LDO 3.3V SOT-23-5", b) && b.code == "SOT23-5");
        CHECK(package_code("TVS DIODE 5V SMAJ5.0A", b) && b.code == "SMA");
        CHECK(!package_code("SMALL SIGNAL RELAY", b));
        CHECK(!package_code("SOD1234", b));
        CHECK(ipc_body("SOD3717X135N", b) && std::fabs(b.w - 1.7) < 1e-9 && std::fabs(b.h - 1.35) < 1e-9);
        CHECK(ipc_body("CAPC1005X56N", b) && std::fabs(b.l - 1.0) < 1e-9 && std::fabs(b.w - 0.5) < 1e-9 && std::fabs(b.h - 0.56) < 1e-9);
        CHECK(ipc_body("CAPC1005N", b) && std::fabs(b.l - 1.0) < 1e-9 && b.h == 0);
        CHECK(ipc_body("INDM5552X300N", b) && std::fabs(b.l - 5.5) < 1e-9 && std::fabs(b.w - 5.2) < 1e-9 && std::fabs(b.h - 3.0) < 1e-9);
        CHECK(ipc_body("SOT95P280X145-5N", b) && b.code == "SOT23-5" && b.pins == 5 && std::fabs(b.l - 2.9) < 0.01);
        CHECK(ipc_body("SOIC127P600X175-8N", b) && std::fabs(b.l - 4.91) < 0.01 && std::fabs(b.w - 3.72) < 0.01);
        CHECK(ipc_body("QFN50P400X400X100-21N", b) && b.l == 4.0 && b.w == 4.0 && b.pins == 21);
        CHECK(!ipc_body("MAZ8000", b));
        CHECK(fake_height(1.6, 0.8, true) > 0.6 && fake_height(1.6, 0.8, true) < 0.8);
    }

    History h;
    auto rec = [&](const char* n, const char* ext, double L, double W, double H, int t, int p, int nz) {
        Record r; r.name = n; r.ext = ext; r.l = L; r.w = W; r.h = H; r.tape = t; r.pitch = p; r.nozzle = nz;
        r.measured = std::string(ext) != ".h8h"; h.add(r);
    };
    // the master program says 2.7 x 1.1 (pads), the machine measured the body
    rec("R1", ".h8h", 2.7, 1.1, 0.5, 1, 4, 500);
    rec("R1", ".e48", 1.58, 0.81, 0.45, 1, 4, 500);
    rec("R1", ".x52", 1.60, 0.80, 0.45, 1, 4, 503);
    rec("R1", ".e47", 1.61, 0.79, 0.46, 1, 4, 500);
    for (int i = 0; i < 5; ++i) rec(("C" + std::to_string(i)).c_str(), ".e48", 1.6, 0.8, 0.8, 1, 4, 500);
    for (int i = 0; i < 4; ++i) rec(("Q" + std::to_string(i)).c_str(), ".e48", 7.0, 7.0, 1.4, 4, 12, 506);
    h.build();
    CHECK(h.parts() == 10);
    const PartSummary* s = h.find("r1");
    CHECK(s && s->measured && std::fabs(s->l - 1.60) < 0.02 && s->nozzle.value == 500 && std::fabs(s->nozzle.share - 0.75) < 1e-9);
    Proposal p = h.propose("R1", "RES 10K 0603", 0, 0, 0);
    CHECK(p.from_history && p.tape == 1 && p.pitch == 4 && p.nozzle == 500);
    Proposal q = h.propose("NEWQFP", "IC MCU 48LQFP", 8.8, 8.7, 1.4);   // land pattern; body ~ x0.8
    CHECK(!q.from_history && q.tape == 4 && q.pitch == 12 && q.nozzle == 506);
    Proposal c = h.propose("NEWCAP", "CAP CER 1UF 0603", 2.7, 1.1, 0.8);   // pad extent given, description size wins
    CHECK(std::fabs(c.l - 1.6) < 1e-9 && c.tape == 1 && c.nozzle == 500);
    // nothing ran at this size: still a complete proposal (carrier from the body, nozzle ladder, estimated height)
    Proposal z = h.propose("ODDPART", "MODULE", 30, 20, 0);
    CHECK(z.l > 0 && z.h > 0 && z.tape >= 5 && z.pitch > 0 && z.nozzle == 506);   // 506 is the biggest nozzle that ran here
    BodyHint hb{2.7, 1.6, 1.1, "package SOD123"};
    Proposal d = h.propose("NEWDIODE", "DIODE SOD123", 3.8, 0.9, 0, true, &hb);
    CHECK(std::fabs(d.l - 2.7) < 1e-9 && std::fabs(d.h - 1.1) < 1e-9);
    CHECK(h.owned_nozzles().count(500) && h.owned_nozzles().count(506));
    h.set_owned_nozzles({500, 504});
    CHECK(h.owned_nozzles().size() == 2);
    h.set_owned_nozzles({});
}

// ------------------------------------------------------------------ effectiveness on real data
static void real() {
    const std::string dbp = pcbp_test::local("teach_db"), libp = pcbp_test::local("teach_lib");
    auto hist = pcbp_test::local_list("teach_history");
    if (dbp.empty() || libp.empty() || hist.empty()) { std::printf("(real-data teach report skipped: no local data)\n"); return; }
    std::string err;
    flexdb::Library lib;
    flexdb::ComponentDB db;
    if (!lib.open(libp, err) || !db.open(dbp, err)) { std::printf("open: %s\n", err.c_str()); ++g_fail; return; }
    history::History H;
    for (auto& f : hist) { size_t n = H.load_csv(f, err); std::printf("history %s: %zu records\n", f.c_str(), n); }
    std::string cpf = pcbp_test::local("teach_cpf");
    if (!cpf.empty()) std::printf("cpf bodies: %zu\n", H.load_cpf_parts(cpf, err));
    H.build();
    std::printf("history: %zu records, %zu parts\n", H.records(), H.parts());

    // what the job itself knows before teaching: CAD land-pattern extent per part (FlexAssign export), mm
    std::map<std::string, std::pair<double, double>> given;
    {
        std::string gp = pcbp_test::local("teach_given");
        FILE* f = gp.empty() ? nullptr : std::fopen(gp.c_str(), "rb");
        if (f) {
            char line[4096];
            if (!std::fgets(line, sizeof line, f)) { std::fclose(f); f = nullptr; }
            while (f && std::fgets(line, sizeof line, f)) {
                // Tab,CompoID,CompoName,Description,W,L,...  (quoted strings, no embedded commas in names)
                std::vector<std::string> c; std::string cur; bool q = false;
                for (char* p = line; *p && *p != '\n'; ++p) { if (*p == '"') q = !q; else if (*p == ',' && !q) { c.push_back(cur); cur.clear(); } else cur += *p; }
                c.push_back(cur);
                if (c.size() > 5) given[c[2]] = {std::atof(c[4].c_str()), std::atof(c[5].c_str())};
            }
            if (f) std::fclose(f);
            std::printf("given land-pattern extents: %zu\n", given.size());
        }
    }

    // CAD package name per part number (cpf_harvest Package column), as the job would carry it
    std::map<std::string, std::string> pkgname;
    if (!cpf.empty())
        if (FILE* f = std::fopen(cpf.c_str(), "rb")) {
            char line[8192];
            if (std::fgets(line, sizeof line, f))
                while (std::fgets(line, sizeof line, f)) {
                    std::vector<std::string> c; std::string cur; bool q = false;
                    for (char* p = line; *p && *p != '\n' && *p != '\r'; ++p) { if (*p == '"') q = !q; else if (*p == ',' && !q) { c.push_back(cur); cur.clear(); } else cur += *p; }
                    c.push_back(cur);
                    if (c.size() > 5 && !c[5].empty()) pkgname[c[0]] = c[5];
                }
            std::fclose(f);
        }

    // ground truth
    struct Truth { int tape = 0, pitch = 0, nozzle = 0; };
    std::map<int, Truth> T;
    if (auto* fe = db.table("Feeder")) {
        int k = fe->col("CompoID"), t = fe->col("TapeTypeID"), p = fe->col("Pitch");
        for (size_t r = 0; r < fe->rows.size(); ++r) { T[(int)fe->get_i(r, k)].tape = (int)fe->get_i(r, t); T[(int)fe->get_i(r, k)].pitch = (int)fe->get_i(r, p); }
    }
    if (auto* lx = db.table("LaserFx3")) {
        int k = lx->col("CompoID"), n = lx->col("NozzleNo");
        for (size_t r = 0; r < lx->rows.size(); ++r) if (T.count((int)lx->get_i(r, k))) T[(int)lx->get_i(r, k)].nozzle = (int)lx->get_i(r, n);
    }

    // text classifier trained on the house DB (minus the evaluated names)
    flexdb::Classifier cls;
    cls.set_library(lib);
    std::map<std::string, int> eval_names;
    for (auto& p : db.parts()) if (p.data_ok && T.count(p.id)) eval_names[p.name] = 1;
    std::string house = pcbp_test::local("part_db");
    if (!house.empty()) {
        flexdb::ComponentDB hdb;
        if (hdb.open(house, err)) {
            std::vector<flexdb::Classifier::Row> rows;
            for (auto& p : hdb.parts()) if (p.pkg_id && !eval_names.count(p.name)) rows.push_back({p.name, p.desc, p.pkg_id});
            cls.learn(rows);
            std::printf("text classifier learned %zu house rows\n", rows.size());
        }
    }

    struct Score { int ok = 0, n = 0; void add(bool b) { ++n; ok += b; } double pct() const { return n ? 100.0 * ok / n : 0; } };
    for (int mode = 0; mode < 3; ++mode) {   // 0 full, 1 size tables only, 2 full + CAD package names
        Score tape, pitch, noz, prof_body, prof_text, prof_text_dims, prof_combo, all3;
        int hist_parts = 0;
        for (auto& p : db.parts()) {
            if (!p.data_ok || !T.count(p.id)) continue;
            const Truth& t = T[p.id];
            double gl = 0, gw = 0;
            if (auto g = given.find(p.name); g != given.end()) { gl = std::max(g->second.first, g->second.second); gw = std::min(g->second.first, g->second.second); }
            const std::string pk = mode == 2 && pkgname.count(p.name) ? pkgname[p.name] : std::string();
            history::Proposal pr = mode == 2 ? flexdb::teach_part(cls, lib, H, p.name, p.desc, gl, gw, true, pk).machine
                                             : H.propose(p.name, p.desc, gl, gw, 0, mode != 1);
            hist_parts += pr.from_history;
            if (t.tape) tape.add(pr.tape == t.tape);
            if (mode == 2 && !pk.empty() && std::getenv("TEACH_VERBOSE")) std::printf("  pkg %-20s %s\n", p.name.c_str(), pk.c_str());
            if (mode == 0 && std::getenv("TEACH_VERBOSE") && (pr.tape != t.tape || pr.nozzle != t.nozzle || pr.pitch != t.pitch))
                std::printf("  t/p/n %-20s %-30.30s %.2fx%.2fx%.2f got %d/%d/%d want %d/%d/%d  [%s]\n", p.name.c_str(), p.desc.c_str(), pr.l, pr.w, pr.h,
                            pr.tape, pr.pitch, pr.nozzle, t.tape, t.pitch, t.nozzle, pr.why.c_str());
            if (t.pitch) pitch.add(pr.pitch == t.pitch);
            if (t.nozzle) noz.add(pr.nozzle == t.nozzle);
            if (t.tape && t.pitch && t.nozzle) all3.add(pr.tape == t.tape && pr.pitch == t.pitch && pr.nozzle == t.nozzle);
            if (!p.pkg_id) continue;
            flexdb::BodyPick bp = flexdb::pick_by_body(lib, p.desc, pr.l, pr.w, pr.h, pr.tape, pr.nozzle);
            flexdb::Guess g0 = cls.classify(p.name, p.desc);
            flexdb::Guess g1 = cls.classify(p.name, p.desc, pr.w, pr.l);
            prof_body.add(bp.pid == p.pkg_id);
            prof_text.add(g0.pid == p.pkg_id);
            prof_text_dims.add(g1.pid == p.pkg_id);
            const int pick = flexdb::teach_part(cls, lib, H, p.name, p.desc, gl, gw, mode != 1, pk).pid;
            prof_combo.add(pick == p.pkg_id);
            if (mode == 0 && pick != p.pkg_id && std::getenv("TEACH_VERBOSE")) {
                const flexdb::Profile* tp = lib.find(p.pkg_id);
                const flexdb::Profile* pp = lib.find(pick);
                std::printf("  miss %-20s %-34.34s body %.2fx%.2fx%.2f -> %-26s want %s\n", p.name.c_str(), p.desc.c_str(), pr.l, pr.w, pr.h,
                            pp ? pp->name.c_str() : "-", tp ? tp->name.c_str() : "?");
            }
        }
        std::printf("\n== %s (%d of %d with part history)\n", mode == 0 ? "full: part history + size tables" : mode == 1 ? "size tables only (part history hidden)" : "full + CAD package names",
                    hist_parts, tape.n);
        std::printf("  tape type   %3d/%3d %5.1f%%\n", tape.ok, tape.n, tape.pct());
        std::printf("  feed pitch  %3d/%3d %5.1f%%\n", pitch.ok, pitch.n, pitch.pct());
        std::printf("  nozzle      %3d/%3d %5.1f%%\n", noz.ok, noz.n, noz.pct());
        std::printf("  all three   %3d/%3d %5.1f%%\n", all3.ok, all3.n, all3.pct());
        std::printf("  profile: text only %5.1f%% | text+dims %5.1f%% | body only %5.1f%% | teach_part %5.1f%%  (n=%d)\n", prof_text.pct(),
                    prof_text_dims.pct(), prof_body.pct(), prof_combo.pct(), prof_body.n);
        if (mode == 0) {
            CHECK(tape.pct() >= 85);
            CHECK(noz.pct() >= 85);
            CHECK(prof_combo.pct() >= prof_text.pct());
            CHECK(pitch.pct() >= 85);
        }
    }
}

int main() {
    synthetic();
    real();
    std::printf("\ntest_flex_teach: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
