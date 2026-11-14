// test_origin.cpp - "assign origin to a component" must make that component read exactly (0,0)
// everywhere the UI shows coordinates (all go through Project::user_pos / to_user).
// Uses the real demo data when present: panel .cpf (cpf_panel, 2x5) and CCDemo.cad (GenCAD).
#include "local_data.h"
#include "../src/core/fileio.h"
#include "../src/core/machine_job.h"
#include "../src/plugins/plugin.h"
#include "pcbp_plugin.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
using namespace pcbp;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void check_project(Project& prj, const char* tag) {
    int n = 0;
    for (auto& c : prj.comps) {
        if (c.feature != Feature::Component) continue;
        prj.virtual_origin = {12.345, -6.789};     // some previous origin
        prj.set_origin_at(c);
        const Vec2 u = prj.user_pos(c);
        CHECK(u.x == 0.0 && u.y == 0.0);
        CHECK(prj.from_user({0, 0}).x == c.pos.x && prj.from_user({0, 0}).y == c.pos.y);
        // round trip of an edit typed relative to the origin
        Vec2 w = prj.from_user({1.5, -2.25});
        CHECK(std::fabs(prj.to_user(w).x - 1.5) < 1e-12 && std::fabs(prj.to_user(w).y + 2.25) < 1e-12);
        ++n;
    }
    std::printf("%s: origin checked on %d components\n", tag, n);
    CHECK(n > 0);
}

int main(int argc, char** argv) {
    // synthetic
    {
        Project p;
        Component c; c.ref = "U1"; c.pos = {37.1, 12.9}; p.add_component(c);
        Component c2; c2.ref = "R1"; c2.pos = {40.1, 10.9}; p.add_component(c2);
        p.set_origin_at(p.comps[0]);
        CHECK(p.user_pos(p.comps[0]).x == 0 && p.user_pos(p.comps[0]).y == 0);
        CHECK(std::fabs(p.user_pos(p.comps[1]).x - 3.0) < 1e-9 && std::fabs(p.user_pos(p.comps[1]).y + 2.0) < 1e-9);
    }
    PluginRegistry r;
    register_builtin_plugins(r);
    const std::string cpf = argc > 1 ? std::string(argv[1]) : pcbp_test::local("cpf_panel");
    const std::string cad = argc > 2 ? argv[2] : pcbp_test::local("legacy_demo_cad");
    for (const std::string& path : {cpf, cad}) {
        if (!std::ifstream(path).good()) { std::printf("SKIP: %s not found\n", path.c_str()); continue; }
        Importer* imp = r.detect(path);
        CHECK(imp != nullptr);
        if (!imp) continue;
        Project prj; ImportReport rep; std::string err;
        bool ok = imp->run(path, default_options(imp->info()), prj, rep, err);
        CHECK(ok);
        if (ok) check_project(prj, path.c_str());
    }
    std::printf("test_origin: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
