#pragma once
// ============================================================================
// migrate.h — batch migration of legacy projects to native .pcbp (portable).
//
// Inputs are files and/or folders (optionally recursive), filtered to legacy
// CAM projects (*.cpf) and optionally ViewMate jobs (*.bin, signature checked).
// Each file is converted with the registered Project importer, written to a
// temporary .pcbp, re-loaded and compared (components, packages, part numbers,
// layers, images, model points) and only then moved to its final name.
//
// After conversion the originals can be kept, archived into ONE .zip
// (relative paths preserved; the archive is re-read and every entry's CRC and
// size compared with the source file before anything is deleted) and/or
// deleted (only verified conversions, and only verified-archived files when
// archiving; Recycle Bin on Windows). Dry-run imports but never writes.
//
// Used by the GUI (gui/migrate_dialog.cpp, worker thread + progress) and the
// command line (PCBPlotter.exe --migrate ...).
// ============================================================================
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pcbp::migrate {

enum class OutputMode { NextToSource, MirrorTree };
enum class Collision { Skip, Overwrite, Rename };
enum class Status { Pending, Converted, DryRun, Skipped, Failed, Cancelled };

struct Options {
    bool recursive = true;
    bool include_cpf = true;
    bool include_bin = false;          // ViewMate *.bin jobs (non-jobs are ignored by signature)
    OutputMode output = OutputMode::NextToSource;
    std::string out_dir;               // MirrorTree: destination root
    Collision collision = Collision::Rename;
    bool archive = false;              // archive originals into archive_path (.zip)
    std::string archive_path;
    bool delete_originals = false;     // after verified conversion (+ verified archive when archiving)
    bool recycle = true;               // Windows: Recycle Bin (FOF_ALLOWUNDO); elsewhere ignored
    bool dry_run = false;
};

struct Item {
    std::string src;                   // absolute source path (UTF-8)
    std::string root;                  // input root it was found under ("" = given as a file)
    std::string rel;                   // path relative to root ('/' separators); file name for single files
    std::string out;                   // planned / written .pcbp
    Status status = Status::Pending;
    std::string importer;              // plugin id
    int comps = 0, parts = 0, packages = 0, layers = 0, points = 0;
    std::vector<std::string> warnings;
    std::string error;
    uint64_t size = 0;
    std::string zip_name;              // entry name in the archive
    bool archived = false, deleted = false;
    double secs = 0;
};

struct Summary {
    int total = 0, converted = 0, dry = 0, skipped = 0, failed = 0, cancelled = 0;
    bool archive_attempted = false, archive_ok = false;
    std::string archive_error;
    int archived = 0, deleted = 0, delete_failed = 0;
    std::string describe() const;
};

// Progress callback: (index, total, message, fraction 0..1). Return false to cancel.
using ProgressFn = std::function<bool(int, int, const std::string&, float)>;

// Expand inputs into items (sorted, de-duplicated) and plan output paths. Errors for missing inputs.
std::vector<Item> collect(const std::vector<std::string>& inputs, const Options& o, std::vector<std::string>& errors);

// Final output path for an item (before collision handling).
std::string planned_output(const Item& it, const Options& o);

// Convert one item (import -> temp .pcbp -> reload + compare -> rename). Honours dry_run / collision.
bool convert_item(Item& it, const Options& o);

// Whole batch: convert all, then archive and delete per options. `cancel` checked between files.
Summary run(std::vector<Item>& items, const Options& o, const ProgressFn& progress);

// Zip archive of the given files (stored names = rel, de-duplicated) + verification by re-reading.
bool write_archive(const std::vector<Item*>& items, const std::string& zip_path, std::string& err, const ProgressFn& progress = {});
bool verify_archive(const std::string& zip_path, const std::vector<Item*>& items, std::string& err);

// Delete one original (Recycle Bin on Windows when `recycle`).
bool remove_file(const std::string& path, bool recycle, std::string& err);

std::string status_name(Status s);
std::string results_csv(const std::vector<Item>& items);

// Command line: args after "--migrate". Writes a human report to `out`. Returns process exit code
// (0 = all ok, 1 = some failed, 2 = usage error).
int cli_main(const std::vector<std::string>& args, std::string& out);
std::string cli_usage();

} // namespace pcbp::migrate
