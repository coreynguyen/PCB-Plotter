#pragma once
// ============================================================================
// autosave.h — crash-safe project recovery (REVIEW_V11 §4, R12).
//
//   %LOCALAPPDATA%\PCBPlotter\Recovery  (layout below)
//       sessions\<session>.lock      held open (no sharing) while the app runs;
//                                    deleted on a clean exit. A lock that can be
//                                    opened exclusively belongs to a dead session.
//       <project-id>\meta.json       original path, display name, owning session,
//                                    snapshot list (newest last)
//       <project-id>\snap<k>.pcbp    rotating snapshots (keep 3), each written
//                                    atomically (temp + flush + replace)
//       crash\                       minidumps + text logs (platform/crash.cpp)
//
// R13 retention (REVIEW_V12 §7): snapshots are user-accessible (File > Recovery snapshots...). Per project the
// last kKeepSnapshots are kept. A document saved/closed cleanly keeps its snapshots marked "clean" (not offered as
// crash recovery); clean entries older than kCleanRetentionDays are deleted at startup.
//
// The original project file is never touched by autosave. Override the root with
// PCBP_RECOVERY_DIR (tests / automation).
// ============================================================================
#include <cstdint>
#include <string>
#include <vector>

namespace pcbp::recovery {

constexpr int kKeepSnapshots = 3;
constexpr int kCleanRetentionDays = 7;

std::string root();                 // UTF-8 path, created on demand
std::string crash_dir();
const std::string& session_id();    // this process ("<pid>-<yyyymmddhhmmss>")

bool begin_session(std::string& err);   // create + hold the session lock
void end_session();                     // clean exit: release + delete the lock

// Stable id for a document: hash of its file path, or "untitled-<session>-<n>".
std::string project_id(const std::string& original_path, int untitled_serial);

struct Snapshot { std::string file, time; };
struct Entry {
    std::string id, dir, original_path, display_name, session;
    std::vector<Snapshot> snaps;    // oldest first
    bool alive = false;             // owning session still running
    bool clean = false;             // R13: closed/saved cleanly (kept as a checkpoint, not offered as crash recovery)
    std::string closed;             // time of the clean close
};

// Write one rotating snapshot of `json` (an already-serialised project). Thread-safe per id.
bool write_snapshot(const std::string& id, const std::string& original_path, const std::string& display_name,
                    const std::string& json, std::string& err);
// Re-own a recovered entry for this session (after Restore).
bool adopt(const std::string& id, std::string& err);
void remove_project(const std::string& id);     // document closed cleanly / discarded
std::vector<Entry> list_entries();              // every project dir (alive flag set)
void close_clean(const std::string& id);        // R13: saved / closed cleanly: keep, mark clean
int prune_clean(int days = kCleanRetentionDays); // R13: delete clean entries older than `days`; returns count
bool delete_snapshot(const std::string& id, const std::string& file);   // removes the entry when it was the last one
uint64_t snapshot_bytes(const Entry& e, const Snapshot& s);
void remove_all();                              // every entry not owned by a running session
std::vector<Entry> find_unfinished();           // entries of dead sessions with >= 1 snapshot
void cleanup_stale_sessions();                  // delete lock files of dead sessions without entries

// Crash markers written by the crash handler: "last_crash.txt" -> shown once on the next launch.
bool take_crash_marker(std::string& text);

std::string now_iso();

} // namespace pcbp::recovery
