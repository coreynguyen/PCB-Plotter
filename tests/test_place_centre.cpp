// ============================================================================
// test_place_centre.cpp — R25: placements are exported at the package's placement point (body centre), not at the
// footprint origin.
//
// The case that showed it (262648-01): chip footprints drawn from pad 1 (origin on the pad, body from -0.381 to
// +1.397 mm). C125 at 0 degrees and R91 at 180 degrees sit in one column on the board; their CAD points are 1.016 mm
// apart. Exported at the origin they came out 1.016 mm apart; exported at the body centre they have the same X.
//
// Part 1: synthetic project (top, bottom, flipped, panel image, a centred package, the old behaviour by flag).
// Part 2 (optional): argv[1] = a real project; every placement must equal origin + R(rot) * pkg_place_offset.
//
// Build: see tests/run_all.sh
// ============================================================================
#include "core/machine_job.h"
#include "core/project.h"
#include "core/project_io.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>

using namespace pcbp;
static int g_pass = 0, g_fail = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool near(double a, double b, double t = 1e-6) { return std::fabs(a - b) < t; }

static Component mk(const char* ref, double x, double y, double rot, const char* pkg, Side s = Side::Top) {
    Component c; c.ref = ref; c.pos = {x, y}; c.rot = rot; c.rot_imported = rot; c.cad_pos = c.pos; c.side = s;
    c.package = pkg; c.number = std::string("PN-") + pkg; c.assigned_point = 0;
    return c;
}
static Project base(Side side = Side::Top, bool flipped = false) {
    Project p;
    Package pin1; pin1.name = "CHIP_PIN1";                      // origin on pad 1
    pin1.pads = {Pad{{0, 0}, 0.6096, 0.508}, Pad{{1.016, 0}, 0.6096, 0.508}}; pin1.pads[0].pin = 1; pin1.pads[1].pin = 2;
    GPrim r; r.kind = GKind::Rect; r.pts = {{-0.381, -0.381}, {1.397, 0.381}}; r.width = 0.1;
    pin1.body = {r}; pin1.body_source = "cad";
    p.packages[pin1.name] = pin1;
    Package mid; mid.name = "CHIP_MID";                         // origin at the centre
    mid.pads = {Pad{{-0.508, 0}, 0.6096, 0.508}, Pad{{0.508, 0}, 0.6096, 0.508}}; mid.pads[0].pin = 1; mid.pads[1].pin = 2;
    GPrim r2 = r; r2.pts = {{-0.889, -0.381}, {0.889, 0.381}}; mid.body = {r2}; mid.body_source = "cad";
    p.packages[mid.name] = mid;
    p.model.points.clear();
    ModelPoint mp; mp.name = "SMT"; mp.has_machine = true; mp.machine.exporter_id = "juki.h8h";
    mp.machine.side = side; mp.machine.flipped = flipped;
    p.model.points.push_back(mp);
    return p;
}
static std::map<std::string, JobPlacement> job(const Project& p) {
    MachineJob j; std::string err; std::map<std::string, JobPlacement> m;
    if (!build_machine_job(p, 0, j, err)) { std::printf("  job: %s\n", err.c_str()); return m; }
    for (auto& q : j.placements) if (q.circuit <= 1) m[q.ref] = q;
    return m;
}

int main(int argc, char** argv) {
    {   // the package itself
        Project p = base();
        const Vec2 a = pkg_place_offset(p.packages["CHIP_PIN1"]), b = pkg_place_offset(p.packages["CHIP_MID"]);
        CHECK(near(a.x, 0.508) && near(a.y, 0));
        CHECK(a.x != 0 && b.x == 0 && b.y == 0);
    }
    {   // top side: 0 / 90 / 180 / 270, and a centred package that must not move
        Project p = base();
        p.add_component(mk("C125", 113.728, 28.708, 0, "CHIP_PIN1"));
        p.add_component(mk("R91", 114.744, 27.438, 180, "CHIP_PIN1"));
        p.add_component(mk("R90", 112.458, 36.455, 90, "CHIP_PIN1"));
        p.add_component(mk("C152", 116.649, 36.709, 270, "CHIP_PIN1"));
        p.add_component(mk("M1", 50, 50, 37, "CHIP_MID"));
        const auto base0 = job(p);
        CHECK(base0.size() == 5);
        // relative to whatever origin the setup uses: differences between placements are what is checked
        const double ox = base0.at("M1").x - 50, oy = base0.at("M1").y - 50;
        CHECK(near(base0.at("C125").x - ox, 114.236) && near(base0.at("C125").y - oy, 28.708));
        CHECK(near(base0.at("R91").x - ox, 114.236) && near(base0.at("R91").y - oy, 27.438));
        CHECK(near(base0.at("C125").x, base0.at("R91").x));                     // one column
        CHECK(near(base0.at("R90").x - ox, 112.458) && near(base0.at("R90").y - oy, 36.963));
        CHECK(near(base0.at("C152").x - ox, 116.649) && near(base0.at("C152").y - oy, 36.201));
        CHECK(near(base0.at("C125").rot, 0) && near(std::fabs(base0.at("R91").rot), 180));   // angles untouched
        // the old behaviour, by flag: the footprint origins
        p.model.points[0].machine.place_at_origin = true;
        const auto old = job(p);
        CHECK(near(old.at("C125").x - ox, 113.728) && near(old.at("R91").x - ox, 114.744));
        CHECK(near(old.at("R91").x - old.at("C125").x, 1.016));
        CHECK(near(old.at("M1").x, base0.at("M1").x) && near(old.at("M1").y, base0.at("M1").y));
    }
    {   // bottom side, flipped: the footprint is mirrored, so is its offset; two parts of one column stay one column
        Project p = base(Side::Bottom, true);
        p.add_component(mk("A", 30.0, 10, 0, "CHIP_PIN1", Side::Bottom));
        p.add_component(mk("B", 28.984, 12, 180, "CHIP_PIN1", Side::Bottom));   // mirrored: centre = origin -/+ 0.508
        p.add_component(mk("M", 5, 5, 0, "CHIP_MID", Side::Bottom));
        const auto j = job(p);
        CHECK(j.size() == 3);
        CHECK(near(j.at("A").x, j.at("B").x));
        p.model.points[0].machine.place_at_origin = true;
        const auto o = job(p);
        CHECK(near(std::fabs(o.at("A").x - o.at("B").x), 1.016));
        CHECK(near(std::fabs(j.at("A").x - o.at("A").x), 0.508) && near(j.at("A").y, o.at("A").y));
    }
    {   // save / load keeps the flag; default is off and not written
        Project p = base(); p.add_component(mk("C1", 1, 1, 0, "CHIP_PIN1"));
        std::string err; const char* f = "build/tests/_place_centre.pcbp";
        CHECK(save_project(p, f, err)); Project q; CHECK(load_project(f, q, err));
        CHECK(!q.model.points.empty() && !q.model.points[0].machine.place_at_origin);
        p.model.points[0].machine.place_at_origin = true;
        CHECK(save_project(p, f, err)); Project q2; CHECK(load_project(f, q2, err));
        CHECK(!q2.model.points.empty() && q2.model.points[0].machine.place_at_origin);
        std::remove(f);
    }
    if (argc > 1) {   // a real project: every placement = origin point + R(rot) * offset, whatever the point's setup
        Project p; std::string err;
        if (!load_project(argv[1], p, err)) { std::printf("cannot load %s: %s\n", argv[1], err.c_str()); return 2; }
        for (size_t pt = 0; pt < p.model.points.size(); ++pt) {
            if (!p.model.points[pt].has_machine) continue;
            MachineJob jn, jo; std::string e1, e2;
            Project po = p; po.model.points[pt].machine.place_at_origin = true;
            if (!build_machine_job(p, (int)pt, jn, e1) || !build_machine_job(po, (int)pt, jo, e2)) continue;
            CHECK(jn.placements.size() == jo.placements.size());
            size_t moved = 0; double worst = 0;
            std::map<std::string, const JobPlacement*> col;
            for (size_t i = 0; i < jn.placements.size() && i < jo.placements.size(); ++i) {
                const JobPlacement& a = jn.placements[i]; const JobPlacement& b = jo.placements[i];
                CHECK(a.ref == b.ref && near(a.rot, b.rot));
                const double d = std::hypot(a.x - b.x, a.y - b.y);
                const Package* pk = nullptr; for (auto& c : p.comps) if (c.ref == a.ref) { pk = p.package_of(c); break; }
                const Vec2 o = pk ? pkg_place_offset(*pk) : Vec2{0, 0};
                CHECK(near(d, std::hypot(o.x, o.y), 1e-6));                      // moved by exactly the package's offset
                if (d > 1e-9) ++moved; if (d > worst) worst = d;
                if (a.circuit <= 1) col[a.ref] = &a;
            }
            std::printf("point %zu '%s': %zu placements, %zu moved, largest move %.3f mm\n", pt, p.model.points[pt].name.c_str(), jn.placements.size(), moved, worst);
            if (col.count("C125") && col.count("R91")) {
                std::printf("  C125 x %.3f  R91 x %.3f\n", col["C125"]->x, col["R91"]->x);
                CHECK(near(col["C125"]->x, col["R91"]->x, 0.02));
            }
        }
    }
    std::printf("test_place_centre: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
