// Native test: recovery snapshots (rotation, atomic files, dead-session detection) and atomic project save.
#include "../src/core/autosave.h"
#include "../src/core/project_io.h"
#include "../src/core/fileio.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
static int fails = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++fails; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
using namespace pcbp;
int main() {
    char tmpl[] = "/tmp/pcbp_rec_XXXXXX";
    std::string dir = mkdtemp(tmpl);
    setenv("PCBP_RECOVERY_DIR", dir.c_str(), 1);
    std::string err;
    // a child session that "crashes" (exits without end_session) after writing 5 snapshots
    pid_t pid = fork();
    if (pid == 0) {
        recovery::begin_session(err);
        Project p; p.info.name = "Board";
        for (int i = 0; i < 5; ++i) {
            Component c; c.ref = "R" + std::to_string(i + 1); p.comps.push_back(c);
            std::string js = project_to_json(p).dump(1);
            recovery::write_snapshot(recovery::project_id("C:\\jobs\\board.pcbp", 0), "C:\\jobs\\board.pcbp", "board.pcbp", js, err);
        }
        _exit(9);   // no end_session: like kill -9
    }
    int st = 0; waitpid(pid, &st, 0);
    recovery::begin_session(err);
    auto un = recovery::find_unfinished();
    CHECK(un.size() == 1);
    if (!un.empty()) {
        CHECK(un[0].original_path == "C:\\jobs\\board.pcbp");
        CHECK((int)un[0].snaps.size() == recovery::kKeepSnapshots);
        std::string text; CHECK(read_text_file(un[0].dir + "/" + un[0].snaps.back().file, text, err));
        json::Value v; CHECK(json::Value::parse(text, v, err));
        Project q; CHECK(project_from_json(v, q, err));
        CHECK(q.comps.size() == 5);   // newest snapshot holds all 5 edits
        CHECK(recovery::adopt(un[0].id, err));
        CHECK(recovery::find_unfinished().empty());   // re-owned by the live session
        recovery::remove_project(un[0].id);
        CHECK(recovery::list_entries().empty());
    }
    // live session entries are not offered
    recovery::write_snapshot(recovery::project_id("", 1), "", "Untitled", "{}", err);
    CHECK(recovery::find_unfinished().empty());
    recovery::cleanup_stale_sessions();
    recovery::end_session();
    // atomic save: no temp left behind, content replaced
    std::string pf = dir + "/x.pcbp";
    CHECK(atomic_write_file(pf, "old", 3, err));
    CHECK(atomic_write_file(pf, "new!", 4, err));
    std::string t; read_text_file(pf, t, err); CHECK(t == "new!");
    CHECK(access((pf + ".tmp" + std::to_string(getpid())).c_str(), F_OK) != 0);
    // R17d: restorable revision checkpoints (RevisionEntry::snapshot) survive save / load as a nested object
    {
        Project p; p.info.name = "Board"; p.info.pcb_id = "PWB-1"; p.info.originator = "Line 2";
        Component c; c.ref = "C1"; c.pos = {1.5, 2.5}; p.comps.push_back(c);
        std::string snap = project_to_json(p, false).dump(-1);
        RevisionEntry r; r.rev = "R1"; r.kind = "checkpoint"; r.snapshot = snap; p.history.push_back(r);
        p.comps.clear(); p.info.name = "Changed";
        json::Value v; CHECK(json::Value::parse(project_to_json(p).dump(1), v, err));
        CHECK(v["history"][0]["snapshot"].is_object());
        Project q; CHECK(project_from_json(v, q, err));
        CHECK(q.history.size() == 1 && !q.history[0].snapshot.empty());
        json::Value sv; CHECK(json::Value::parse(q.history[0].snapshot, sv, err));
        Project s2; CHECK(project_from_json(sv, s2, err));
        CHECK(s2.comps.size() == 1 && s2.comps[0].ref == "C1" && s2.info.name == "Board" && s2.info.pcb_id == "PWB-1" && s2.info.originator == "Line 2");
        RevisionEntry old; old.rev = "R0"; q.history.insert(q.history.begin(), old);   // pre-R17d entry: no snapshot key
        json::Value v2; CHECK(json::Value::parse(project_to_json(q).dump(1), v2, err));
        CHECK(!v2["history"][0].has("snapshot"));
    }
    std::printf("test_autosave: %d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
