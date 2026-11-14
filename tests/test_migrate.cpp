// test_migrate.cpp — batch migration (core/migrate): collect, convert + verify, collisions,
// mirror tree, dry run, zip archive + CRC verification, guarded deletion, CSV log, CLI.
// Works on COPIES of the real .cpf samples in a temp folder (skipped when the samples are absent).
#include "local_data.h"
#include "../src/core/migrate.h"
#include "../src/core/fileio.h"
#include "../src/core/project_io.h"
#include "../src/io/archive.h"
#include "../src/plugins/plugin.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace pcbp;
namespace fs = std::filesystem;
namespace mg = pcbp::migrate;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const std::string sA = pcbp_test::local("cpf_project1"); static const char* kCpfA = sA.c_str();
static const std::string sB = pcbp_test::local("cpf_b"); static const char* kCpfB = sB.c_str();
static const std::string sC = pcbp_test::local("cpf_c"); static const char* kCpfC = sC.c_str();
static const std::string sV = pcbp_test::local("vm_sample"); static const char* kVm = sV.c_str();

static std::string u(const fs::path& p) { auto s = p.u8string(); return std::string(s.begin(), s.end()); }
static bool have(const char* p) { return std::ifstream(p).good(); }
static void put(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream f(p, std::ios::binary); f << s; }

static const mg::Item* find(const std::vector<mg::Item>& v, const std::string& rel) {
    for (auto& i : v) if (i.rel == rel) return &i;
    return nullptr;
}

int main() {
    register_builtin_plugins(PluginRegistry::get());
    if (!have(kCpfA) || !have(kCpfB) || !have(kCpfC)) { std::printf("SKIP: sample .cpf files not found\npassed 0, failed 0\n"); return 0; }
    const bool vm = have(kVm);
    const fs::path T = fs::temp_directory_path() / "pcbp_migrate_test";
    fs::remove_all(T);
    const fs::path src = T / "src";
    fs::create_directories(src / "sub" / "deep");
    fs::copy_file(kCpfA, src / "Project1.cpf");
    fs::copy_file(kCpfB, src / "sub" / "Board B.cpf");
    fs::copy_file(kCpfC, src / "sub" / "deep" / "Board C V2,2.cpf");
    if (vm) fs::copy_file(kVm, src / "sub" / "Sample1up.bin");
    put(src / "firmware.bin", "not a viewmate job");
    put(src / "notes.txt", "hello");
    put(src / "sub" / "broken.cpf", "this is not a jet database at all");

    // ---- collect ----
    {
        mg::Options o; std::vector<std::string> errs;
        auto items = mg::collect({u(src)}, o, errs);
        CHECK(errs.empty());
        CHECK(items.size() == 4);                      // 3 real + broken.cpf; .bin excluded by default
        CHECK(find(items, "sub/deep/Board C V2,2.cpf") != nullptr);
        o.include_bin = true;
        items = mg::collect({u(src)}, o, errs);
        CHECK(items.size() == (vm ? 5u : 4u));        // firmware.bin rejected by signature
        o.recursive = false;
        items = mg::collect({u(src)}, o, errs);
        CHECK(items.size() == 1);
        // explicit file + duplicate via folder, missing input
        o.recursive = true;
        items = mg::collect({u(src / "Project1.cpf"), u(src), u(T / "nope")}, o, errs);
        CHECK(items.size() == (vm ? 5u : 4u));
        CHECK(!errs.empty());
        o.output = mg::OutputMode::MirrorTree; o.out_dir = u(T / "out");
        auto it = mg::collect({u(src)}, o, errs);
        const mg::Item* d = find(it, "sub/deep/Board C V2,2.cpf");
        CHECK(d && d->out == u(T / "out" / "sub" / "deep" / "Board C V2,2.pcbp"));
    }

    // ---- dry run: nothing written ----
    {
        mg::Options o; o.include_bin = true; o.dry_run = true; o.archive = true; o.archive_path = u(T / "dry.zip"); o.delete_originals = true;
        std::vector<std::string> errs;
        auto items = mg::collect({u(src)}, o, errs);
        auto s = mg::run(items, o, {});
        CHECK(s.dry == (vm ? 4 : 3));
        CHECK(s.failed == 1);
        CHECK(!fs::exists(src / "Project1.pcbp"));
        CHECK(!fs::exists(T / "dry.zip"));
        CHECK(fs::exists(src / "Project1.cpf"));
        const mg::Item* a = find(items, "Project1.cpf");
        CHECK(a && a->status == mg::Status::DryRun && a->comps == 458);
        const mg::Item* b = find(items, "sub/broken.cpf");
        CHECK(b && b->status == mg::Status::Failed && !b->error.empty());
    }

    // ---- convert next to source; verify counts by reloading ----
    int comps_a = 0;
    {
        mg::Options o; o.include_bin = true;
        std::vector<std::string> errs;
        auto items = mg::collect({u(src)}, o, errs);
        int calls = 0;
        auto s = mg::run(items, o, [&](int, int, const std::string&, float) { ++calls; return true; });
        CHECK(calls >= (int)items.size());
        CHECK(s.converted == (vm ? 4 : 3) && s.failed == 1);
        std::printf("%s\n", s.describe().c_str());
        for (auto& it : items) std::printf("  [%s] %s -> %s (%d comps, %d parts, %d pkgs, %d layers) %s\n", mg::status_name(it.status).c_str(),
                                           it.rel.c_str(), it.out.c_str(), it.comps, it.parts, it.packages, it.layers, it.error.c_str());
        for (auto& it : items) {
            if (it.status != mg::Status::Converted) continue;
            Project p; std::string err;
            CHECK(load_project(it.out, p, err));
            CHECK((int)p.comps.size() == it.comps && (int)p.parts.size() == it.parts && (int)p.packages.size() == it.packages &&
                  (int)p.layers.size() == it.layers);
            CHECK(!fs::exists(it.out + ".migrating"));
        }
        const mg::Item* a = find(items, "Project1.cpf");
        CHECK(a && a->out == u(src / "Project1.pcbp") && a->comps == 458);
        if (a) comps_a = a->comps;
        if (vm) { const mg::Item* v = find(items, "sub/Sample1up.bin"); CHECK(v && v->status == mg::Status::Converted && v->layers == 12 && v->importer == "viewmate.bin"); }
        std::string csv = mg::results_csv(items);
        CHECK(csv.rfind("Source,Status,Importer", 0) == 0);
        CHECK(csv.find("\"" + u(src / "sub" / "deep" / "Board C V2,2.cpf") + "\"") != std::string::npos);   // comma quoted
        CHECK(fs::exists(src / "Project1.cpf"));   // originals kept by default
    }

    // ---- collisions ----
    {
        std::vector<std::string> errs;
        mg::Options o; o.collision = mg::Collision::Skip;
        auto items = mg::collect({u(src / "Project1.cpf")}, o, errs);
        auto s = mg::run(items, o, {});
        CHECK(s.skipped == 1 && items[0].status == mg::Status::Skipped);
        o.collision = mg::Collision::Rename;
        items = mg::collect({u(src / "Project1.cpf")}, o, errs);
        s = mg::run(items, o, {});
        CHECK(s.converted == 1 && items[0].out == u(src / "Project1 (2).pcbp") && fs::exists(src / "Project1 (2).pcbp"));
        o.collision = mg::Collision::Overwrite;
        const auto before = fs::last_write_time(src / "Project1.pcbp");
        items = mg::collect({u(src / "Project1.cpf")}, o, errs);
        s = mg::run(items, o, {});
        CHECK(s.converted == 1 && items[0].out == u(src / "Project1.pcbp"));
        CHECK(fs::last_write_time(src / "Project1.pcbp") >= before);
        CHECK(!fs::exists(src / "Project1 (3).pcbp"));
    }

    // ---- mirror tree + archive + delete (on a fresh copy) ----
    {
        const fs::path s2 = T / "src2";
        fs::copy(src, s2, fs::copy_options::recursive);
        for (auto& e : fs::recursive_directory_iterator(s2)) if (e.path().extension() == ".pcbp") fs::remove(e.path());
        mg::Options o; o.include_bin = true; o.output = mg::OutputMode::MirrorTree; o.out_dir = u(T / "mirror");
        o.archive = true; o.archive_path = u(T / "arch" / "originals.zip"); o.delete_originals = true;
        std::vector<std::string> errs;
        auto items = mg::collect({u(s2)}, o, errs);
        auto s = mg::run(items, o, {});
        std::printf("%s\n", s.describe().c_str());
        CHECK(s.archive_ok && s.archived == (vm ? 4 : 3));
        CHECK(s.deleted == (vm ? 4 : 3) && s.delete_failed == 0);
        CHECK(fs::exists(T / "mirror" / "sub" / "deep" / "Board C V2,2.pcbp"));
        CHECK(fs::exists(T / "mirror" / "Project1.pcbp"));
        CHECK(!fs::exists(s2 / "Project1.cpf"));
        CHECK(!fs::exists(s2 / "sub" / "deep" / "Board C V2,2.cpf"));
        CHECK(fs::exists(s2 / "sub" / "broken.cpf"));          // failed conversion: never deleted
        CHECK(fs::exists(s2 / "firmware.bin") && fs::exists(s2 / "notes.txt"));
        // archive content: relative paths, byte-identical to the original samples
        std::vector<uint8_t> z; std::string err;
        CHECK(read_file(u(T / "arch" / "originals.zip"), z, err));
        std::vector<io::ArchiveEntry> ents;
        CHECK(io::read_zip(z.data(), z.size(), ents, err));
        CHECK(ents.size() == (vm ? 4u : 3u));
        bool found = false;
        for (auto& e : ents) if (e.name == "sub/deep/Board C V2,2.cpf") {
            std::vector<uint8_t> orig; read_file(kCpfC, orig, err);
            found = e.data == orig;
        }
        CHECK(found);
        // tamper detection: verify_archive compares with the source on disk
        std::vector<mg::Item> fake(1);
        fake[0].src = kCpfB; fake[0].rel = "x"; fake[0].zip_name = "sub/deep/Board C V2,2.cpf";
        std::vector<mg::Item*> fp{&fake[0]};
        CHECK(!mg::verify_archive(u(T / "arch" / "originals.zip"), fp, err));
        fake[0].src = kCpfC;
        CHECK(mg::verify_archive(u(T / "arch" / "originals.zip"), fp, err));
        fake[0].zip_name = "missing.cpf";
        CHECK(!mg::verify_archive(u(T / "arch" / "originals.zip"), fp, err));
    }

    // ---- archive failure => nothing deleted ----
    {
        const fs::path s3 = T / "src3";
        fs::create_directories(s3);
        fs::copy_file(kCpfB, s3 / "a.cpf");
        put(T / "blocker", "file, not a folder");
        mg::Options o; o.archive = true; o.archive_path = u(T / "blocker" / "x.zip"); o.delete_originals = true;
        std::vector<std::string> errs;
        auto items = mg::collect({u(s3)}, o, errs);
        auto s = mg::run(items, o, {});
        CHECK(s.converted == 1 && s.archive_attempted && !s.archive_ok && !s.archive_error.empty());
        CHECK(s.deleted == 0 && fs::exists(s3 / "a.cpf"));
        // cancellation before the first file
        fs::remove(s3 / "a.pcbp");
        items = mg::collect({u(s3)}, o, errs);
        s = mg::run(items, o, [](int, int, const std::string&, float) { return false; });
        CHECK(s.cancelled == 1 && !fs::exists(s3 / "a.pcbp") && fs::exists(s3 / "a.cpf"));
    }

    // ---- CLI ----
    {
        std::string out;
        CHECK(mg::cli_main({"--migrate"}, out) == 2);
        out.clear();
        CHECK(mg::cli_main({"--migrate", u(src), "--delete"}, out) == 2);          // needs --yes
        CHECK(fs::exists(src / "Project1.cpf"));
        out.clear();
        CHECK(mg::cli_main({"--migrate", u(src), "--bogus"}, out) == 2);
        out.clear();
        const fs::path log = T / "log.csv";
        int rc = mg::cli_main({"--migrate", u(src / "Project1.cpf"), "--dry-run", "--log", u(log)}, out);
        std::printf("%s", out.c_str());
        CHECK(rc == 0);
        CHECK(out.find("[Dry run]") != std::string::npos && out.find(std::to_string(comps_a) + " comps") != std::string::npos);
        std::string csv, err;
        CHECK(read_text_file(u(log), csv, err) && csv.find("Dry run") != std::string::npos);
        out.clear();
        rc = mg::cli_main({"--migrate", u(src), "--out", u(T / "cli"), "--collision", "overwrite"}, out);
        CHECK(rc == 1);                                                          // broken.cpf fails
        CHECK(fs::exists(T / "cli" / "sub" / "Board B.pcbp"));
    }

    fs::remove_all(T);
    std::printf("passed %d, failed %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
