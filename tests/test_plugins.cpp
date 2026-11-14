// ============================================================================
// test_plugins.cpp — built-in plugin registry test (Linux; plugin_dll_win.cpp
// excluded). Optionally exercises the SDK sample plugin built as a shared
// object (pass its path as argv[1]).
//
// Build:
//   g++ -std=c++20 -O1 -Wall -Isrc -Isdk tests/test_plugins.cpp src/plugins/plugin_registry.cpp
//       src/plugins/builtin_plugins.cpp src/plugins/juki_import_plugin.cpp src/core/{fileio,json,machine_job,project,project_io}.cpp
//       src/io/{bom_import,cc_archive,centroid_import,gencad,gerber,inflate,io_util,ipc356,jet_mdb,juki_writer,text_table}.cpp
//       -ldl -o build/test_plugins
//   g++ -std=c++20 -shared -fPIC -Isdk sdk/sample_plugin/sample_plugin.cpp -o build/sample_plugin.so
//   build/test_plugins build/sample_plugin.so
// ============================================================================
#include "local_data.h"
#include "../src/core/fileio.h"
#include "../src/core/json.h"
#include "../src/core/machine_job.h"
#include "../src/core/project_io.h"
#include "../src/io/juki_writer.h"
#include "../src/plugins/plugin.h"
#include "pcbp_plugin.h"

#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>

using namespace pcbp;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const std::string kRoot = pcbp_test::local("legacy_demo_root");   // legacy CAM demo install (local only)
static const std::string kLib = kRoot + "/cclib.mdb";
static const std::string kBom = kRoot + "/Demonstration Bill of Materials/DemoBOM.prn";
static const std::string kRef = kRoot + "/Centroid File/Reference File.txt";
static const std::string kCad = kRoot + "/Demonstration CAD Data/CCDemo.cad";
static const std::string kSilk = kRoot + "/Demonstration CAD Data/Top Silk Gerber Layer.spl";

static bool run_import(PluginRegistry& r, const char* id, const std::string& path, Project& prj,
                       ImportReport& rep, const Options& extra = {}) {
    Importer* imp = r.importer(id);
    CHECK(imp != nullptr);
    if (!imp) return false;
    Options o = default_options(imp->info());
    for (auto& kv : extra) o[kv.first] = kv.second;
    std::string err;
    const bool ok = imp->run(path, o, prj, rep, err);
    std::printf("[%s] %s: %s%s\n", id, file_stem(path).c_str(), ok ? rep.summary().c_str() : "FAILED ",
                ok ? "" : err.c_str());
    for (auto& m : rep.messages) std::printf("    %s\n", m.c_str());
    return ok;
}

static void test_registry(PluginRegistry& r) {
    std::printf("== registry: %zu importers, %zu exporters\n", r.importers().size(), r.exporters().size());
    for (auto& i : r.importers()) std::printf("  imp %-15s %-9s %s\n", i->info().id.c_str(), i->info().category.c_str(), i->info().name.c_str());
    for (auto& e : r.exporters()) std::printf("  exp %-15s %-9s %s (.%s)\n", e->info().id.c_str(), e->info().category.c_str(), e->info().name.c_str(), e->info().default_ext.c_str());
    CHECK(r.importers().size() == 9 + 9 + 2);   // + 9 v2 CAD importers (cad_formats_v2.cpp) + viewmate.bin
    CHECK(r.exporters().size() >= 16);   // 16 core + third-party machine formats (strict input validation)
    // detection
    auto det = [&](const std::string& p) { Importer* i = r.detect(p); return i ? i->info().id : std::string("(none)"); };
    std::printf("  detect cclib.mdb=%s CCDemo.cad=%s DemoBOM.prn=%s Reference=%s silk=%s\n", det(kLib).c_str(),
                det(kCad).c_str(), det(kBom).c_str(), det(kRef).c_str(), det(kSilk).c_str());
    CHECK(det(kLib) == "circuitcam.cpf");
    CHECK(det(kCad) == "gencad");
    CHECK(det(kBom) == "bom");
    CHECK(det(kRef) == "centroid");
    CHECK(det(kSilk) == "gerber");
}

static void test_cpf(PluginRegistry& r) {
    std::printf("== circuitcam.cpf\n");
    {
        Project p; p.packages.clear(); p.parts.clear();
        ImportReport rep;
        CHECK(run_import(r, "circuitcam.cpf", kLib, p, rep, {{"library_only", "1"}}));
        CHECK(p.packages.size() == 25);
        CHECK(p.parts.size() == 26);
        CHECK(p.comps.empty());
        auto it = p.packages.find("SOIC8");
        CHECK(it != p.packages.end());
        if (it != p.packages.end()) {
            CHECK(it->second.pads.size() == 8);
            CHECK(it->second.smt);
            CHECK(!it->second.body.empty());
        }
        auto dip = p.packages.find("DIP16");
        CHECK(dip != p.packages.end() && !dip->second.smt && dip->second.pads.size() == 16);
        int with_pads = 0;
        for (auto& kv : p.packages) if (!kv.second.pads.empty()) ++with_pads;
        std::printf("  packages with pads: %d / %zu\n", with_pads, p.packages.size());
        CHECK(with_pads == 25);
    }
    {
        Project p;
        ImportReport rep;
        CHECK(run_import(r, "circuitcam.cpf", kLib, p, rep));
        CHECK(p.packages.size() == 25);
        CHECK(p.images.size() == 1);
        CHECK(p.model.points.size() == 1);
        if (!p.model.points.empty()) {
            std::printf("  point '%s' process '%s' kind %d machine %d (%s)\n", p.model.points[0].name.c_str(),
                        p.model.points[0].process_type.c_str(), (int)p.model.points[0].kind,
                        (int)p.model.points[0].has_machine, p.model.points[0].machine.exporter_id.c_str());
            CHECK(p.model.points[0].kind == PointKind::Placement);
        }
    }
}

static void build_demo(PluginRegistry& r, Project& p) {
    std::printf("== demo project (gencad + bom + centroid + gerber)\n");
    ImportReport a, b, c, d;
    CHECK(run_import(r, "gencad", kCad, p, a));
    CHECK(a.components_added > 50);
    CHECK(run_import(r, "bom", kBom, p, b));
    CHECK(b.components_updated > 50);
    CHECK(run_import(r, "centroid", kRef, p, c, {{"units", "mil"}}));
    CHECK(c.components_updated + c.components_added > 50);
    CHECK(run_import(r, "gerber", kSilk, p, d));
    CHECK(p.layers.size() == 1 && !p.layers[0].content.empty() && p.layers[0].role == LayerRole::Silk);
    std::printf("  project: %zu comps, %zu packages, %zu parts, %zu layers\n", p.comps.size(), p.packages.size(),
                p.parts.size(), p.layers.size());
}

static void test_machine(PluginRegistry& r, Project& p) {
    std::printf("== machine exporters\n");
    int n = 0;
    for (auto& c : p.comps) if (c.feature == Feature::Component) { c.assigned_point = 0; ++n; }
    MachineJob job;
    std::string err;
    CHECK(build_machine_job(p, 0, job, err));
    std::printf("  job: %zu placements, %zu parts, %zu fiducials (%d assigned)\n", job.placements.size(),
                job.parts.size(), job.fiducials.size(), n);
    CHECK(!job.placements.empty());
    for (auto& e : r.exporters()) {
        if (e->info().export_input != ExportInput::MachineJob) continue;
        std::vector<uint8_t> out;
        Options o = default_options(e->info());
        const bool ok = e->export_job(job, o, out, err);
        const std::string& id = e->info().id;
        const bool core = id.rfind("juki.", 0) == 0 || id.find("csv") != std::string::npos || id.find("xy") != std::string::npos ||
                          id.rfind("pcbp", 0) == 0 || id.rfind("generic.csv", 0) == 0;
        if (core) CHECK(ok);
        else CHECK(ok || (out.empty() && !err.empty()));   // vendor writers may refuse this generic job, cleanly
        std::string extra;
        if (ok && e->info().id.rfind("juki.", 0) == 0) {
            juki::HlcParsed hp;
            std::string e2;
            const bool pok = juki::parse_hlc(out, hp, e2);
            CHECK(pok);
            CHECK(hp.placements.size() == job.placements.size());
            extra = " parsed: " + std::to_string(hp.placements.size()) + " placements, " +
                    std::to_string(hp.components.size()) + " components, ver " + hp.version;
        } else if (ok) {
            std::string s(out.begin(), out.end());
            extra = " first line: " + s.substr(0, s.find('\r'));
        }
        std::printf("  %-12s %s %zu bytes%s\n", e->info().id.c_str(), ok ? "ok" : ("FAILED " + err).c_str(), out.size(), extra.c_str());
    }
    // option handling
    Exporter* csv = r.exporter("generic.csv");
    std::vector<uint8_t> out;
    Options o = default_options(csv->info());
    o["columns"] = "ref,x,y,rot"; o["units"] = "mil"; o["delimiter"] = "semicolon"; o["header"] = "0"; o["angle"] = "CW";
    CHECK(csv->export_job(job, o, out, err));
    std::string s(out.begin(), out.end());
    std::printf("  csv(mil;CW): %s\n", s.substr(0, s.find('\r')).c_str());
    o["columns"] = "ref,bogus";
    CHECK(!csv->export_job(job, o, out, err));
}

static void test_project_exporters(PluginRegistry& r, const Project& p) {
    std::printf("== project exporters\n");
    for (const char* id : {"gencad", "bom.csv", "pcbp.json"}) {
        Exporter* e = r.exporter(id);
        CHECK(e != nullptr);
        if (!e) continue;
        std::vector<uint8_t> out;
        std::string err;
        const bool ok = e->export_project(p, default_options(e->info()), out, err);
        CHECK(ok && !out.empty());
        std::string s(out.begin(), out.end());
        int lines = 0; for (char c : s) lines += c == '\n';
        std::printf("  %-10s %zu bytes, %d lines\n", id, out.size(), lines);
        if (std::string(id) == "bom.csv") std::printf("    %s\n", s.substr(s.find('\n') + 1, s.find('\n', s.find('\n') + 1) - s.find('\n') - 2).c_str());
        if (std::string(id) == "gencad" && ok) {
            // re-import our own GenCAD
            const std::string tmp = "/tmp/claude-0/test_plugins_rt.cad";
            CHECK(write_file(tmp, s.data(), s.size(), err));
            Project q; ImportReport rep;
            CHECK(r.importer("gencad")->run(tmp, default_options(r.importer("gencad")->info()), q, rep, err));
            std::printf("    re-import: %s\n", rep.summary().c_str());
            CHECK(q.comps.size() == p.comps.size());
        }
        if (std::string(id) == "pcbp.json" && ok) {
            json::Value v;
            CHECK(json::Value::parse(s, v, err));
            Project q;
            CHECK(project_from_json(v, q, err));
            CHECK(q.comps.size() == p.comps.size());
            CHECK(q.packages.size() == p.packages.size());
            CHECK(q.parts.size() == p.parts.size());
            CHECK(q.layers.size() == p.layers.size());
            const std::string again = project_to_json(q).dump(1);
            CHECK(again == s);
            std::printf("    JSON round trip %s\n", again == s ? "identical" : "DIFFERS");
            // and through the pcbp.json importer
            const std::string tmp = "/tmp/claude-0/test_plugins_rt.pcbp";
            CHECK(write_file(tmp, s.data(), s.size(), err));
            Project z; ImportReport rep;
            CHECK(r.detect(tmp) == r.importer("pcbp.json"));
            CHECK(r.importer("pcbp.json")->run(tmp, {}, z, rep, err));
            CHECK(z.comps.size() == p.comps.size());
        }
    }
}

// ---- SDK sample plugin, loaded as a shared object --------------------------
static void test_sample(const char* so, const Project& p) {
    std::printf("== sample plugin %s\n", so);
    void* h = dlopen(so, RTLD_NOW);
    CHECK(h != nullptr);
    if (!h) { std::printf("  dlopen: %s\n", dlerror()); return; }
    auto en = (pcbp_plugin_enumerate_fn)dlsym(h, "pcbp_plugin_enumerate");
    CHECK(en != nullptr);
    if (!en) return;
    const pcbp_plugin_desc* list = nullptr;
    uint32_t count = 0;
    CHECK(en(PCBP_PLUGIN_API_VERSION, &list, &count) == 1);
    CHECK(count == 2);
    pcbp_host host{PCBP_PLUGIN_API_VERSION, nullptr,
                   [](void*, int lvl, const char* m) { std::printf("    plugin log[%d]: %s\n", lvl, m); },
                   [](void*, double, const char*) {}};
    for (uint32_t i = 0; i < count; ++i) {
        const pcbp_plugin_desc& d = list[i];
        CHECK(d.struct_size == sizeof(pcbp_plugin_desc) && d.api_version == PCBP_PLUGIN_API_VERSION);
        std::printf("  %s (%s) kind %u\n", d.id, d.name, d.kind);
        if (d.kind == PCBP_KIND_EXPORTER) {
            MachineJob job;
            std::string err;
            Project q = p;
            for (auto& c : q.comps) if (c.feature == Feature::Component) c.assigned_point = 0;
            CHECK(build_machine_job(q, 0, job, err));
            const std::string js = machine_job_to_json(job);
            uint8_t* data = nullptr; size_t size = 0; char* perr = nullptr;
            CHECK(d.export_file(&host, js.c_str(), "{}", &data, &size, &perr) == 1);
            std::string s((const char*)data, size);
            int lines = 0; for (char c : s) lines += c == '\n';
            std::printf("    %d lines, first: %s\n", lines, s.substr(0, s.find('\n')).c_str());
            CHECK(lines == (int)job.placements.size());
            d.free_mem(data);
        } else {
            const char* text = "# sample\nR1 10.0 20.5 90 RES-10K\nC2 -3 4 0\nU7 1e1 2 270.5 IC-555\n";
            char* out = nullptr; char* perr = nullptr;
            CHECK(d.sniff((const char*)"x.sxy", (const uint8_t*)text, std::strlen(text)) > 0);
            CHECK(d.import_file(&host, "x.sxy", (const uint8_t*)text, std::strlen(text), "{}", &out, &perr) == 1);
            json::Value v; std::string err;
            CHECK(json::Value::parse(out, v, err));
            Project q;
            CHECK(project_from_json(v, q, err));
            CHECK(q.comps.size() == 3);
            if (q.comps.size() == 3) {
                CHECK(q.comps[0].ref == "R1" && q.comps[0].pos.y == 20.5 && q.comps[0].rot == 90 && q.comps[0].number == "RES-10K");
                CHECK(q.comps[2].pos.x == 10 && q.comps[2].rot == 270.5);
            }
            std::printf("    imported %zu components\n", q.comps.size());
            d.free_mem(out);
            // malformed line -> error
            out = nullptr; perr = nullptr;
            CHECK(d.import_file(&host, "x.sxy", (const uint8_t*)"R1 abc 2 3\n", 11, "{}", &out, &perr) == 0);
            if (perr) { std::printf("    expected error: %s\n", perr); d.free_mem(perr); }
        }
    }
}

int main(int argc, char** argv) {
    PluginRegistry r;
    register_builtin_plugins(r);
    test_registry(r);
    test_cpf(r);
    Project demo;
    build_demo(r, demo);
    test_machine(r, demo);
    test_project_exporters(r, demo);
    if (argc > 1) test_sample(argv[1], demo);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
