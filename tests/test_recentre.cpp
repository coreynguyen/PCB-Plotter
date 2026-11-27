// test_recentre.cpp — R25: io/recentre on a real program + the project it came from.
//   test_recentre <program> <project.pcbp> [<ref A> <ref B>]     (A and B: two references that share a column)
// Checks: placements still on the CAD origin move to the centre of the lands; the file keeps its size and differs
// only in few bytes; a second run moves nothing; a placement moved by hand first is left alone; the CLI writes
// "<name> (centred).<ext>" and --dry-run writes nothing.
#include "io/recentre.h"
#include "io/pnp_program.h"
#include "core/fileio.h"
#include "core/project_io.h"
#include <cmath>
#include <cstdio>
#include <map>
using namespace pcbp; using namespace pcbp::io;
static int g_pass = 0, g_fail = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("test_recentre: no data given, skipped\n"); return 0; }
    std::vector<uint8_t> in, out, out2; std::string err; Project prj;
    CHECK(read_file(argv[1], in, err)); CHECK(load_project(argv[2], prj, err));
    RecentreReport r;
    CHECK(recentre_program(in, prj, -1, 0.02, out, r, err));
    std::printf("%s", recentre_text(r, false).c_str());
    CHECK(r.moved > 0 && r.elsewhere == 0 && r.already == 0);
    CHECK(out.size() == in.size());
    size_t diff = 0; for (size_t i = 0; i < in.size() && i < out.size(); ++i) if (in[i] != out[i]) ++diff;
    std::printf("bytes changed: %zu of %zu (%d placements moved)\n", diff, in.size(), r.moved);
    CHECK(diff > 0 && diff <= (size_t)r.moved * 8);                       // two 4-byte fields a placement at most
    PnpProgram a, b; CHECK(pnp_load(in, a, err)); CHECK(pnp_load(out, b, err));
    CHECK(a.placements.size() == b.placements.size() && a.parts.size() == b.parts.size());
    std::map<std::string, const PnpPlacement*> by;
    for (size_t i = 0; i < a.placements.size() && i < b.placements.size(); ++i) {
        CHECK(a.placements[i].ref == b.placements[i].ref && a.placements[i].rot == b.placements[i].rot && a.placements[i].part == b.placements[i].part && a.placements[i].skip == b.placements[i].skip);
        by[b.placements[i].ref] = &b.placements[i];
    }
    if (argc > 4 && by.count(argv[3]) && by.count(argv[4])) {
        std::printf("%s x %.3f   %s x %.3f\n", argv[3], by[argv[3]]->x, argv[4], by[argv[4]]->x);
        CHECK(std::fabs(by[argv[3]]->x - by[argv[4]]->x) < 0.02);
    }
    // again: nothing left to move, every moved one is now "already"
    RecentreReport r2; CHECK(recentre_program(out, prj, -1, 0.02, out2, r2, err));
    CHECK(r2.moved == 0 && r2.already == r.moved && out2 == out);
    // a placement taught at the machine (0.3 mm off the CAD point) is not touched
    {
        PnpProgram t = a; size_t k = 0; for (; k < r.rows.size(); ++k) if (r.rows[k].what == 0) break;
        t.placements[k].x += 0.3;
        std::vector<uint8_t> tin, tout; CHECK(pnp_save(t, tin, err));
        RecentreReport rt; CHECK(recentre_program(tin, prj, -1, 0.02, tout, rt, err));
        CHECK(rt.elsewhere == 1 && rt.moved == r.moved - 1 && rt.rows[k].what == 2 && rt.rows[k].x1 == rt.rows[k].x0);
    }
    // the command
    {
        const std::string f = "build/tests/_rc.h8h", c = "build/tests/_rc (centred).h8h"; std::string o;
        std::remove(c.c_str()); CHECK(write_file(f, in.data(), in.size(), err));
        CHECK(recentre_cli({"--recentre", f, "--project", argv[2], "--dry-run"}, o) == 0);
        std::vector<uint8_t> probe; CHECK(!read_file(c, probe, err));
        o.clear(); CHECK(recentre_cli({"--recentre", f, "--project", argv[2]}, o) == 0);
        CHECK(read_file(c, probe, err) && probe == out);
        o.clear(); CHECK(recentre_cli({"--recentre", f, "--project", argv[2], "--in-place"}, o) == 0);
        CHECK(read_file(f, probe, err) && probe == out); CHECK(read_file(f + ".orig", probe, err) && probe == in);
        o.clear(); CHECK(recentre_cli({"--recentre", f, "--project", argv[2], "--in-place"}, o) == 0 && o.find("nothing to move") != std::string::npos);
        CHECK(recentre_cli({"--recentre"}, o) == 2);
        std::remove(f.c_str()); std::remove(c.c_str()); std::remove((f + ".orig").c_str());
    }
    std::printf("test_recentre: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
