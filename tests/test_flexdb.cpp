// test_flexdb.cpp — Jet4 writer + FlexLine component DB integration + classifier accuracy.
//
// Data (skipped when absent; tests/local_data.map): flex_dir (part_db, package LM.flx, a job .mdb,
// samples/*.cpf) and empty_mdb (FlexLine empty template).
// Oracles: mdbtools (mdb-export) for the written files; the real Jet engine's output of FlexAssign
// (flex_db_after, written through ADO) for the profile-application differential test.
// Build: see tests/run_all.sh (same link line as test_plugins).
#include "local_data.h"
#include "../src/io/flexdb.h"
#include "../src/io/jet_mdb.h"
#include "../src/io/jet_write.h"
#include "../src/core/fileio.h"
#include "../src/plugins/plugin.h"
#include "pcbp_plugin.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>

using namespace pcbp;
using namespace pcbp::flexdb;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const std::string D = pcbp_test::local("flex_dir");
static const std::string TMP = "/tmp/claude-0/flex/";
static bool exists(const std::string& p) { return std::ifstream(p).good(); }

static std::string run(const std::string& cmd) {
    std::string out;
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return out;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    pclose(f);
    return out;
}
static bool have_mdbtools() { return run("command -v mdb-export").size() > 0; }
// sorted mdb-export of a table (hex for binary columns)
static std::string export_sorted(const std::string& file, const std::string& table) {
    return run("mdb-export -b hex '" + file + "' '" + table + "' 2>/dev/null | sort");
}

// ---------------------------------------------------------------------------
// 1. Jet writer
// ---------------------------------------------------------------------------
static const char* kAll[] = {"Compo", "Mech", "Feeder", "Vision", "Visiondata", "Inspection", "Expansion", "Laser", "LaserFX1",
    "LaserFX2", "LaserFx3", "LaserCX", "LaserGx5", "LaserGx6", "LightControlGx5", "LightControlGx6", "VCSControlGx5",
    "VCSControlGx6", "VisionGx5", "VisionGx6", "Side", "GripNozzle", "Fluxer"};

static void test_codec(const std::string& file) {
    if (!exists(file)) { std::printf("SKIP %s\n", file.c_str()); return; }
    jet::Editor e;
    std::string err;
    CHECK(e.open(file, err));
    size_t rows = 0, rt = 0, ent = 0, miss = 0;
    for (auto t : kAll) {
        if (!e.has_table(t)) continue;
        size_t n, m, en, em;
        CHECK(e.roundtrip_check(t, n, m, err));
        CHECK(e.index_check(t, en, em, err));
        rows += n; rt += m; ent += en; miss += em;
    }
    std::printf("codec %-28s rows %6zu re-encode mismatches %zu | index entries %6zu key mismatches %zu\n",
                file.substr(file.rfind('/') + 1).c_str(), rows, rt, ent, miss);
    CHECK(rt == 0);
    CHECK(miss == 0);
}

// Walk every index B-tree like a reader does (descend by key) and look every row up.
static void verify_tree_search(const std::string& file, const std::string& table) {
    jet::Editor e;
    std::string err;
    CHECK(e.open(file, err));
    size_t en = 0, em = 0;
    CHECK(e.index_check(table, en, em, err));
    CHECK(em == 0);
}

static void test_rewrite_existing() {
    const std::string src = pcbp_test::local("part_db");
    if (!exists(src)) return;
    jet::Editor e;
    std::string err;
    CHECK(e.open(src, err));
    const char* tabs[] = {"Compo", "Mech", "Feeder", "Expansion", "Side", "Laser"};
    for (auto t : tabs) {
        jet::ETable tb;
        CHECK(e.load(t, tb, err));
        CHECK(e.store(tb, err));
    }
    std::printf("rewrite part DB: keys reused %zu computed %zu mismatched %zu, pages appended %zu\n", e.stats.keys_reused,
                e.stats.keys_computed, e.stats.keys_mismatch, e.stats.pages_appended);
    CHECK(e.stats.keys_mismatch == 0);
    const std::string out = TMP + "partdb_rewrite.mdb";
    CHECK(e.save(out, err));
    // our reader
    jet::Database a, b;
    CHECK(a.open(src, err));
    CHECK(b.open(out, err));
    for (auto t : tabs) {
        jet::Table ta, tb;
        CHECK(a.read_table(t, ta, err));
        CHECK(b.read_table(t, tb, err));
        CHECK(ta.rows.size() == tb.rows.size());
        verify_tree_search(out, t);
    }
    // mdbtools oracle: identical table content (pictures included)
    if (have_mdbtools()) {
        int same = 0;
        for (auto t : tabs) same += export_sorted(src, t) == export_sorted(out, t);
        std::printf("rewrite part DB: mdb-export identical for %d/6 tables\n", same);
        CHECK(same == 6);
    }
}

// ---------------------------------------------------------------------------
// 2. Profile application: differential test against FlexAssign + real Jet (ADO)
// ---------------------------------------------------------------------------
static std::map<std::string, std::string> rows_by_name(const std::string& file, const std::string& table,
                                                       const std::map<int, std::string>& id2name, bool drop_id) {
    jet::Database db;
    std::string err;
    std::map<std::string, std::string> out;
    if (!db.open(file, err)) return out;
    jet::Table t;
    if (!db.read_table(table, t, err)) return out;
    int ci = t.col("CompoID");
    for (size_t r = 0; r < t.rows.size(); ++r) {
        int id = (int)t.rows[r][(size_t)ci].as_int();
        auto it = id2name.find(id);
        std::string key = it == id2name.end() ? "#" + std::to_string(id) : it->second;
        std::string line;
        for (size_t c = 0; c < t.cols.size(); ++c) {
            if (drop_id && (int)c == ci) continue;
            line += t.cols[c].name + "=" + (t.rows[r][c].null ? "<null>" : t.rows[r][c].as_string()) + ";";
        }
        out[key] += line + "\n";
    }
    return out;
}
static std::map<int, std::string> names_of(const std::string& file) {
    jet::Database db;
    std::string err;
    std::map<int, std::string> m;
    if (!db.open(file, err)) return m;
    jet::Table t;
    db.read_table("Compo", t, err);
    for (auto& r : t.rows) m[(int)r[(size_t)t.col("CompoID")].as_int()] = r[(size_t)t.col("CompoName")].s;
    return m;
}

static void test_apply_differential(Library& lib) {
    const std::string before = pcbp_test::local("flex_db_before"), after = pcbp_test::local("flex_db_after"), empty = pcbp_test::local("empty_mdb");
    if (!exists(before) || !exists(after) || !lib.is_open()) { std::printf("SKIP apply differential\n"); return; }
    std::string err;
    // what FlexAssign assigned (truth), by name
    ComponentDB truth;
    CHECK(truth.open(after, err));
    std::map<std::string, Part> want;
    for (auto& p : truth.parts()) want[p.name] = p;

    for (int mode = 0; mode < 2; ++mode) {
        ComponentDB db;
        std::string out;
        if (mode == 0) {  // update the pre-assignment copy in place (same CompoIDs)
            CHECK(db.open(before, err));
            out = TMP + "apply_update.mdb";
        } else {          // brand-new database from the FlexLine empty template
            if (!exists(empty)) continue;
            CHECK(db.create_from_template(empty, err));
            ComponentDB src;
            src.open(before, err);
            for (auto& p : src.parts()) {
                int id = db.add_part(p.name, p.desc, "", err);
                CHECK(id > 0);
                if (!id) std::printf("  add_part %s: %s\n", p.name.c_str(), err.c_str());
            }
            out = TMP + "apply_new.mdb";
        }
        int applied = 0;
        for (auto& p : db.parts()) {
            auto it = want.find(p.name);
            if (it == want.end() || !it->second.pkg_id) continue;
            bool ok = db.apply_profile(p.id, lib, it->second.pkg_id, it->second.cls != lib.find(it->second.pkg_id)->cls ? it->second.cls : 0, true, err);
            CHECK(ok);
            if (!ok) std::printf("  apply %s: %s\n", p.name.c_str(), err.c_str());
            applied += ok;
        }
        CHECK(db.save(out, err));
        if (!err.empty()) std::printf("  save: %s\n", err.c_str());
        // compare all parameter tables by component name
        auto n_out = names_of(out), n_truth = names_of(after);
        // parts FlexLine flagged in the machine check (_res.log) were re-taught by hand afterwards
        std::set<std::string> retaught;
        {
            std::string log;
            if (read_text_file(pcbp_test::local("flex_db_log"), log, err)) {
                size_t p = 0;
                while (p < log.size()) {
                    size_t e = log.find('\n', p); if (e == std::string::npos) e = log.size();
                    std::string ln = log.substr(p, e - p);
                    size_t c1 = ln.find(','), c2 = c1 == std::string::npos ? c1 : ln.find(',', c1 + 1);
                    if (c2 != std::string::npos) retaught.insert(ln.substr(c1 + 1, c2 - c1 - 1));
                    p = e + 1;
                }
            }
        }
        int tables = 0, same_tables = 0;
        size_t diff_rows = 0, total_rows = 0, flexline_only = 0, skipped_retaught = 0;
        for (int i = 0; i < kCopyTableCount; ++i) {
            jet::Database x;
            x.open(after, err);
            if (!x.has_table(kCopyTables[i])) continue;
            auto a = rows_by_name(after, kCopyTables[i], n_truth, true);
            auto b = rows_by_name(out, kCopyTables[i], n_out, true);
            ++tables;
            size_t d = 0;
            for (auto& kv : a) {
                if (!want.count(kv.first) || !want[kv.first].pkg_id) continue;
                if (retaught.count(kv.first)) { ++skipped_retaught; continue; }
                ++total_rows;
                if (!b.count(kv.first)) { ++flexline_only; continue; }   // row FlexLine created later (all NULL)
                if (b[kv.first] != kv.second) {
                    ++d;
                    std::printf("  %s %s differs\n    jet : %s    ours: %s", kCopyTables[i], kv.first.c_str(), kv.second.c_str(), b[kv.first].c_str());
                }
            }
            for (auto& kv : b)
                if (!a.count(kv.first) && want.count(kv.first) && want[kv.first].pkg_id && !retaught.count(kv.first)) {
                    ++d;
                    std::printf("  %s %s only in ours: %s", kCopyTables[i], kv.first.c_str(), kv.second.c_str());
                }
            diff_rows += d;
            same_tables += d == 0;
        }
        std::printf("apply %s: %d profiles applied; %d/%d parameter tables identical to FlexAssign+Jet; %zu/%zu rows differ, "
                    "%zu rows exist only in the reference (created later by FlexLine); %zu rows of %zu re-taught parts skipped\n",
                    mode == 0 ? "to existing DB" : "to new DB from template", applied, same_tables, tables, diff_rows, total_rows, flexline_only,
                    skipped_retaught, retaught.size());
        CHECK(diff_rows == 0);
        // Compo profile stamps
        auto ca = rows_by_name(after, "Compo", n_truth, true), cb = rows_by_name(out, "Compo", n_out, true);
        size_t cd = 0;
        for (auto& kv : ca) {
            if (!want.count(kv.first) || !want[kv.first].pkg_id) continue;   // hand-taught in FlexLine, not a profile copy
            // compare only profile-owned fields (PkgType is a stale text cache FlexLine blanks; not compared)
            auto pick = [](const std::string& s) {
                std::string o;
                for (const char* f : {"PkgClassID=", "PkgTypeID=", "Width=", "Length=", "Height=", "DataOk=", "UseMech=", "UseVision=", "UseLaser="}) {
                    size_t p = s.find(f);
                    if (p != std::string::npos) o += s.substr(p, s.find(';', p) - p) + ";";
                }
                return o;
            };
            if (pick(kv.second) != pick(cb[kv.first])) { if (++cd <= 2) std::printf("  Compo %s\n    jet : %s\n    ours: %s\n", kv.first.c_str(), pick(kv.second).c_str(), pick(cb[kv.first]).c_str()); }
        }
        std::printf("apply %s: Compo profile stamps differ for %zu/%d profile-assigned parts\n", mode == 0 ? "existing" : "new", cd, applied);
        CHECK(cd == 0);
        // structural checks on the written file: our reader + Jet codec + every index searchable
        for (auto t : kAll) {
            jet::Editor e;
            if (!e.open(out, err) || !e.has_table(t)) continue;
            size_t n, m, en, em;
            e.roundtrip_check(t, n, m, err);
            e.index_check(t, en, em, err);
            CHECK(m == 0);
            CHECK(em == 0);
        }
        if (have_mdbtools()) {
            std::string o = run("mdb-export '" + out + "' Compo 2>&1 | wc -l");
            CHECK(std::atoi(o.c_str()) >= 279);
            std::string ver = run("mdb-ver '" + out + "' 2>&1");
            CHECK(ver.find("JET4") != std::string::npos);
        }
    }
}

static void test_names_and_tape(Library& lib) {
    const std::string empty = pcbp_test::local("empty_mdb");
    if (!exists(empty) || !lib.is_open()) return;
    std::set<std::string> taken;
    auto tk = [&](const std::string& s) { return taken.count(s) > 0; };
    std::string a = machine_name("CRCW04021K00FKED-EXTRA-LONG", tk);
    CHECK(a == "CRCW04021K00FKED-EXT");
    taken.insert(a);
    std::string b = machine_name("CRCW04021K00FKED-EXTRA-LONGER", tk);
    CHECK(b == "CRCW04021K00FKED-E~1");
    CHECK(machine_name("ABC\xC2\xB5" "F", tk).size() <= 20);
    CHECK(sanitize_machine_chars("A\"B") == "A_B");

    ComponentDB db;
    std::string err;
    CHECK(db.create_from_template(empty, err));
    int id = db.add_part(a, "RES 1K 1% 0402", "CRCW04021K00FKED-EXTRA-LONG", err);
    CHECK(id == 1);
    CHECK(db.add_part(a, "dup", "", err) == 0);   // duplicate name refused
    const Profile* pp = lib.find_name("Cap0603(1608) 500 Pp");
    CHECK(pp != nullptr);
    if (!pp) return;
    int id2 = db.add_part("GRM188R71H104KA93D", "CAP CER 0.1UF 50V X7R 0603", "", err);
    CHECK(db.apply_profile(id2, lib, pp->id, 0, true, err));
    CHECK(db.set_tape(id2, false, err));
    const std::string out = TMP + "tape.mdb";
    CHECK(db.save(out, err));
    ComponentDB r;
    CHECK(r.open(out, err));
    bool seen = false;
    for (auto& p : r.parts())
        if (p.id == id2) { seen = true; CHECK(p.tape_type == 2); CHECK(p.auto_teach == 0); CHECK(p.pkg_id == pp->id); CHECK(std::fabs(p.w - pp->w) < 1e-6); }
    CHECK(seen);
    auto ps = r.parts();
    CHECK(ps.size() == 2);
    if (ps.size() == 2) CHECK(ps[0].ext_name == "CRCW04021K00FKED-EXTRA-LONG");

    // growing a new DB appends pages past the template's end; page 1 marks those free until claimed.
    // Unclaimed, the Jet engine reuses them and overwrites Compo (seen after FlexLine opened an export).
    {
        ComponentDB g;
        CHECK(g.create_from_template(empty, err));
        for (int i = 0; i < 300; ++i) {
            char nm[24]; std::snprintf(nm, sizeof nm, "GROW%04d", i);
            int gid = g.add_part(nm, "CAP CER 0.1UF 50V X7R 0603", "", err);
            CHECK(gid > 0);
            if (gid > 0 && i % 3 == 0) CHECK(g.apply_profile(gid, lib, pp->id, 0, true, err));
        }
        const std::string gout = TMP + "grow.mdb";
        CHECK(g.save(gout, err));
        jet::Editor e;
        CHECK(e.open(gout, err));
        CHECK(e.pages_wrongly_free() == 0);
        jet::Editor t;
        CHECK(t.open(empty, err) && t.pages_wrongly_free() == 0);
    }
    // no template: a new, empty DB from any FlexLine DB (here the house DB; the source is never written)
    const std::string house = pcbp_test::local("part_db");
    if (exists(house)) {
        ComponentDB n;
        CHECK(n.create_empty_from(house, err));
        CHECK(n.parts().empty());
        int nid = n.add_part("NEWPART-1", "CAP CER 0.1UF 50V X7R 0603", "", err);
        CHECK(nid == 1);
        CHECK(n.apply_profile(nid, lib, pp->id, 0, true, err));
        const std::string nout = TMP + "notemplate.mdb";
        CHECK(n.save(nout, err));
        jet::Editor e;
        CHECK(e.open(nout, err));
        CHECK(e.pages_wrongly_free() == 0);
        auto rows_of = [&](const char* t) { jet::ETable x; return e.load(t, x, err) ? x.rows.size() : (size_t)-1; };
        CHECK(rows_of("Compo") == 1);
        CHECK(rows_of("Side") >= 1);   // default row + the library lookups apply_profile copies
        CHECK(rows_of("PkgClass") > 0 && rows_of("FeederType") > 0 && rows_of("Lookup") > 0 && rows_of("VacuumDflt") > 0);
        CHECK(rows_of("FlexDBInfo") == 1 && rows_of("WriteLockInfo") == 1);
        CHECK(rows_of("LightPattern") == 0 && rows_of("ElemLoc") == 0 && rows_of("Manuftr") == 0);
        ComponentDB src;   // source untouched
        CHECK(src.open(house, err) && src.parts().size() > 1000);
    }
}

// ---------------------------------------------------------------------------
// 3. Classifier accuracy (5-fold CV on the hand-assigned rows of part DB)
// ---------------------------------------------------------------------------
struct Acc { int n = 0, cov = 0, exact = 0, size = 0, hi = 0, hi_exact = 0, hi_size = 0; };
static void print_acc(const char* what, const Acc& a) {
    std::printf("%-46s rows %5d  covered %5d (%.1f%%)  exact %.1f%%  size %.1f%%  (exact over all rows %.1f%%)\n", what, a.n, a.cov,
                100.0 * a.cov / std::max(1, a.n), 100.0 * a.exact / std::max(1, a.cov), 100.0 * a.size / std::max(1, a.cov),
                100.0 * a.exact / std::max(1, a.n));
    std::printf("%-46s conf>=0.85: %5d rows (%.1f%%)  exact %.1f%%  size %.1f%%\n", "", a.hi, 100.0 * a.hi / std::max(1, a.n),
                100.0 * a.hi_exact / std::max(1, a.hi), 100.0 * a.hi_size / std::max(1, a.hi));
}

static void test_accuracy(Library& lib) {
    const std::string partdb = pcbp_test::local("part_db");
    if (!exists(partdb) || !lib.is_open()) { std::printf("SKIP accuracy\n"); return; }
    ComponentDB db;
    std::string err;
    CHECK(db.open(partdb, err));
    std::vector<Part> rows;
    for (auto& p : db.parts()) if (lib.find(p.pkg_id)) rows.push_back(p);
    std::printf("part DB: %zu hand-assigned rows with a library profile\n", rows.size());
    Acc with_dims, text_only;
    for (int fold = 0; fold < 5; ++fold) {
        std::vector<Classifier::Row> train;
        for (size_t i = 0; i < rows.size(); ++i) if ((int)(i % 5) != fold) train.push_back({rows[i].name, rows[i].desc, rows[i].pkg_id});
        Classifier c;
        c.set_library(lib);
        c.learn(train);
        for (size_t i = 0; i < rows.size(); ++i) {
            if ((int)(i % 5) != fold) continue;
            const Part& p = rows[i];
            for (int mode = 0; mode < 2; ++mode) {
                Guess g = mode == 0 ? c.classify(p.name, p.desc, p.w, p.l) : c.classify(p.name, p.desc);
                Acc& a = mode == 0 ? with_dims : text_only;
                ++a.n;
                if (!g.pid) continue;
                ++a.cov;
                if (g.pid == p.pkg_id) ++a.exact;
                const Profile* gp = lib.find(g.pid);
                const Profile* tp = lib.find(p.pkg_id);
                bool sz = gp && tp && Classifier::base_size(gp->name) == Classifier::base_size(tp->name);
                if (sz) ++a.size;
                if (g.conf >= 0.85) { ++a.hi; a.hi_exact += g.pid == p.pkg_id; a.hi_size += sz; }
            }
        }
    }
    print_acc("5-fold CV, text + stored W/L (FlexAssign mode)", with_dims);
    print_acc("5-fold CV, text only (new part, no machine dims)", text_only);
    CHECK(with_dims.cov > 2000);
    CHECK(with_dims.exact * 100 >= with_dims.cov * 75);
}

// ---------------------------------------------------------------------------
// 4. Sample CircuitCAM projects: does footprint evidence help?
// ---------------------------------------------------------------------------
static void test_cpf_samples(Library& lib) {
    const std::string partdb = pcbp_test::local("part_db");
    if (!exists(partdb) || !lib.is_open()) return;
    std::string err;
    ComponentDB db;
    CHECK(db.open(partdb, err));
    std::map<std::string, Part> truth;
    std::vector<Classifier::Row> train;
    for (auto& p : db.parts()) {
        std::string k = p.name;
        for (auto& ch : k) ch = (char)std::toupper((unsigned char)ch);
        truth[k] = p;
        if (lib.find(p.pkg_id)) train.push_back({p.name, p.desc, p.pkg_id});
    }
    PluginRegistry reg;
    register_builtin_plugins(reg);
    Importer* imp = reg.importer("circuitcam.cpf");
    if (!imp) { std::printf("SKIP cpf (no importer)\n"); return; }
    std::vector<std::string> files = pcbp_test::local_list("flex_samples");
    int parts = 0, matched = 0, t_ok = 0, f_ok = 0, t_cov = 0, f_cov = 0, warn = 0, conflict = 0, fp_valid = 0, t_size = 0, f_size = 0;
    int truth_geo_ok = 0, truth_geo_n = 0, disputed = 0;
    for (auto& fn : files) {
        std::string path = D + "samples/" + fn;
        if (!exists(path)) continue;
        Project prj;
        ImportReport rep;
        if (!imp->run(path, default_options(imp->info()), prj, rep, err)) { std::printf("  %s: import failed %s\n", fn.c_str(), err.c_str()); continue; }
        // hold out this board's own part numbers from the conventions (they would leak the answer)
        std::set<std::string> board;
        for (auto& kv : prj.parts) { std::string k = kv.first.substr(0, 20); for (auto& ch : k) ch = (char)std::toupper((unsigned char)ch); board.insert(k); }
        std::vector<Classifier::Row> tr;
        for (auto& r : train) { std::string k = r.name; for (auto& ch : k) ch = (char)std::toupper((unsigned char)ch); if (!board.count(k)) tr.push_back(r); }
        Classifier c;
        c.set_library(lib);
        c.learn(tr);
        int bp = 0, bm = 0;
        for (auto& kv : prj.parts) {
            const PartNumber& pn = kv.second;
            ++parts; ++bp;
            std::string k = pn.number.substr(0, 20);
            for (auto& ch : k) ch = (char)std::toupper((unsigned char)ch);
            auto it = truth.find(k);
            const Package* pkg = nullptr;
            auto pit = prj.packages.find(pn.package);
            if (pit != prj.packages.end()) pkg = &pit->second;
            if (!pkg)
                for (auto& cc : prj.comps) if (cc.number == pn.number) { pkg = prj.package_of(cc); if (pkg) break; }
            if (it == truth.end() || !lib.find(it->second.pkg_id) || k == "DNP" || k == "NA") continue;
            ++matched; ++bm;
            std::string desc = pn.description.empty() ? it->second.desc : pn.description;
            Evaluation ev = evaluate(c, pn.number, desc, pkg);
            if (ev.fp.valid) ++fp_valid;
            const Profile* tp = lib.find(it->second.pkg_id);
            if (ev.fp.valid) {
                Footprint f = ev.fp;
                double pa = std::max(tp->w, tp->l), pb = std::min(tp->w, tp->l);
                if (pa > 0 && pb > 0) {
                    ++truth_geo_n;
                    double dx = (f.body_l - pa) / pa, dy = (f.body_w - pb) / pb, dx2 = (f.ext_l - pa) / pa, dy2 = (f.ext_w - pb) / pb;
                    double te = std::min(std::sqrt(dx * dx + dy * dy), std::sqrt(dx2 * dx2 + dy2 * dy2));
                    if (te <= 0.35) ++truth_geo_ok;
                    // hand assignment contradicted by the footprint AND the description-driven pick
                    if (te > 0.35 && !ev.sugg.empty() && ev.sugg[0].pid != tp->id && ev.sugg[0].geo_err >= 0 && ev.sugg[0].geo_err <= 0.25) {
                        ++disputed;
                        std::printf("    disputed existing: %-18s %-26s -> %s\n", pn.number.c_str(), tp->name.c_str(), lib.find(ev.sugg[0].pid)->name.c_str());
                    }
                }
            }
            if (ev.text.pid) { ++t_cov; t_ok += ev.text.pid == tp->id; const Profile* g = lib.find(ev.text.pid); t_size += g && Classifier::base_size(g->name) == Classifier::base_size(tp->name); }
            if (!ev.sugg.empty()) {
                ++f_cov;
                f_ok += ev.sugg[0].pid == tp->id;
                const Profile* g = lib.find(ev.sugg[0].pid);
                f_size += g && Classifier::base_size(g->name) == Classifier::base_size(tp->name);
            }
            if (std::getenv("FLEXDBG")) {
                const Profile* g = ev.sugg.empty() ? nullptr : lib.find(ev.sugg[0].pid);
                const Profile* tg = lib.find(ev.text.pid);
                std::printf("    %-20s pkg %-12s %-22s body %.2fx%.2f ext %.2fx%.2f pads %d | truth %-24s %.2fx%.2f | text %-24s | top %-24s %.2f %s\n",
                            pn.number.c_str(), pn.package.c_str(), ev.fp.shape.c_str(), ev.fp.body_l, ev.fp.body_w, ev.fp.ext_l, ev.fp.ext_w, ev.fp.pads,
                            tp->name.c_str(), tp->l, tp->w, tg ? tg->name.c_str() : "-", g ? g->name.c_str() : "-", ev.sugg.empty() ? 0 : ev.sugg[0].conf,
                            ev.sugg.empty() ? "" : ev.sugg[0].why.c_str());
            }
            warn += !ev.warnings.empty();
            conflict += ev.conflict;
        }
        std::printf("  %-24s %3d part numbers, %3d with a hand-assigned part DB profile\n", fn.c_str(), bp, bm);
    }
    std::printf("cpf samples: %d part numbers, %d with truth, footprint measured for %d\n", parts, matched, fp_valid);
    std::printf("  description only : covered %d  exact %d (%.1f%% of truth)  size %d\n", t_cov, t_ok, 100.0 * t_ok / std::max(1, matched), t_size);
    std::printf("  + footprint      : covered %d  exact %d (%.1f%% of truth)  size %d\n", f_cov, f_ok, 100.0 * f_ok / std::max(1, matched), f_size);
    std::printf("  truth profile body within 35%% of measured footprint: %d/%d; parts with warnings %d, conflicts %d\n",
                truth_geo_ok, truth_geo_n, warn, conflict);
    std::printf("  existing hand assignments disputed by footprint + description: %d\n", disputed);
    CHECK(f_cov >= t_cov);
}

// ---------------------------------------------------------------------------
static void test_footprint() {
    Package p;
    p.name = "0805";
    p.pads = {Pad{{-0.95, 0}, 1.0, 1.4, 0, PadShape::Rect, 0, 1}, Pad{{0.95, 0}, 1.0, 1.4, 0, PadShape::Rect, 0, 2}};
    Footprint f = measure_footprint(p);
    CHECK(f.valid && f.pads == 2);
    CHECK(f.chip_code == "0805");
    Package s;   // SOIC-8
    for (int i = 0; i < 4; ++i) {
        s.pads.push_back(Pad{{-2.7, 1.905 - i * 1.27}, 1.55, 0.6, 0, PadShape::Rect, 0, i + 1});
        s.pads.push_back(Pad{{2.7, 1.905 - i * 1.27}, 1.55, 0.6, 0, PadShape::Rect, 0, 8 - i});
    }
    Footprint g = measure_footprint(s);
    CHECK(g.sides == 2);
    CHECK(std::fabs(g.pitch - 1.27) < 1e-6);
    CHECK(std::fabs(g.body_w - 3.85) < 0.05);
    CHECK(std::fabs(g.body_l - 4.89) < 0.1);
    FeederFit ff = predict_feeder(2.0, 1.25);
    CHECK(ff.tape_mm == 8 && ff.nozzle == "500");
    CHECK(text_chip_sizes("CAP CER 0.1UF 50V X7R 0603").size() == 1);
    CHECK(family_of("RES 10K 1% 0402") == "RES");
}

int main() {
    if (std::system(("mkdir -p " + TMP).c_str()) != 0) return 1;
    const char* only = std::getenv("FLEXONLY");   // run one section: codec | apply | acc | cpf
    auto want = [&](const char* s) { return !only || std::strcmp(only, s) == 0; };
    test_footprint();
    if (want("codec")) {
    test_codec(pcbp_test::local("part_db"));
    test_codec(D + "package LM.flx");
    test_codec(pcbp_test::local("flex_db_after"));
    test_codec(D + "tulsar.mdb");
    test_codec(pcbp_test::local("empty_mdb"));
    test_rewrite_existing();
    }
    Library lib;
    std::string err;
    if (exists(D + "package LM.flx")) {
        CHECK(lib.open(D + "package LM.flx", err));
        std::printf("library: %zu profiles, %zu classes\n", lib.profiles.size(), lib.class_names.size());
    }
    if (want("apply")) { test_apply_differential(lib); test_names_and_tape(lib); }
    if (want("acc")) test_accuracy(lib);
    if (want("cpf")) test_cpf_samples(lib);
    std::printf("passed %d, failed %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
