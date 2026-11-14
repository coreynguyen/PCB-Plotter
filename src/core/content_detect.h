#pragma once
// ============================================================================
// content_detect.h — "what is in this file / folder / archive?" (portable).
//
// Used by `PCBPlotter.exe --import <path>`, the Explorer verb "Import into
// PCB Plotter" on .zip/.tgz/folders, and drag & drop of archives. The source is
// expanded into one virtual tree: a directory is walked recursively, archives
// (zip / tar / tgz / gz) are opened, and archives found INSIDE are expanded too
// (bounded depth / size), so "job.zip/outer/odb.tgz/design/matrix/matrix" is
// found. Every file is classified from its name + first bytes; the result is
// an import Plan (best action first, alternatives listed) that the UI executes.
//
// Virtual paths use '/' and name a nested archive like a folder:
//   "release/cam.zip/gerbers/top.gbr".
// ============================================================================
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pcbp::detect {

enum class Kind {
    Unknown,
    Project,        // .pcbp (native JSON)
    LegacyCpf,      // legacy CAM project (.cpf, Jet database)
    LegacyMdb,      // legacy package library (.mdb)
    ViewMateJob,    // ViewMate job database (.bin) — importer "viewmate.bin"
    Odb,            // ODB++ job (directory with matrix/matrix)
    Ipc2581,        // IPC-2581 XML (.xml / .cvg)
    Cad,            // other CAD format a registered importer claims (GenCAD, KiCad, ...)
    PnpProgram,     // pick-and-place program (.h8h/.h7h/.h5h/.h4h/.e4x/.iss)
    Gerber,         // RS-274X / RS-274D layer
    Drill,          // Excellon
    ApertureList,   // RS-274D aperture list
    Centroid,       // XY / pick-and-place text
    Bom,            // bill of materials text
    Spreadsheet,    // .xlsx/.xls/.ods (BOM / centroid usually; opened in the sheet editor)
    Archive,        // nested archive that could not be expanded
};
const char* kind_name(Kind k);

struct Entry {
    std::string vpath;              // virtual path ('/'), relative to the scanned source
    std::string disk_path;          // non-empty when the entry is a real file (directory scan)
    std::vector<uint8_t> data;      // bytes for archive members (empty for disk files: read on demand)
    uint64_t size = 0;
    Kind kind = Kind::Unknown;
    int score = 0;                  // classification confidence 0..100
    std::string importer;           // registry id when known ("odbpp", "ipc2581", "juki.hlc", ...)
};

enum class Action {
    None,
    OpenProject,        // open .pcbp
    ImportFile,         // run the importer `importer` on files[0] (Project / Merge mode decided by the importer)
    ImportOdb,          // ODB++: files = job root folder (materialised) — importer "odbpp"
    LoadGerbers,        // open "Load Gerber Layers" prefilled with files (+ aperture lists)
    ImportCentroid,     // centroid wizard on files[0] (then BOM if `bom` set)
    ImportBom,          // BOM wizard on files[0]
    OpenSheet,          // open in the spreadsheet editor
    Unsupported,        // recognised but no reader: explain
};
const char* action_name(Action a);

struct Plan {
    Action action = Action::None;
    Kind kind = Kind::Unknown;
    std::string importer;                  // registry id for ImportFile / ImportOdb
    std::vector<std::string> files;        // vpaths the action consumes (ODB: root folder vpath, "" = source root)
    std::vector<std::string> aperture_lists;
    std::vector<std::string> centroid, bom;   // follow-up text imports found next to the main data
    std::vector<std::string> gerbers;         // follow-up Gerber/drill layers next to CAD data
    int score = 0;
    std::string title;                     // one line for menus / prompts
};

struct Result {
    std::string source;                    // scanned path
    bool source_is_dir = false, source_is_archive = false;
    std::vector<Entry> entries;            // every file (archives expanded)
    std::vector<Plan> plans;               // best first; empty when nothing importable
    std::vector<std::string> warnings;
    const Plan* best() const { return plans.empty() ? nullptr : &plans.front(); }
    const Entry* find(const std::string& vpath) const;
    std::string summary() const;           // multi-line human-readable report
};

struct Options {
    int max_depth = 3;                     // nested archive depth
    uint64_t max_total = 1024ull << 20;    // stop expanding beyond this many bytes in memory
    size_t max_files = 20000;
    // Optional: ask the plugin registry about a file the built-in rules do not know.
    // Return a score 0..100 and fill `id` (importer id) / `kind` (Cad / Centroid / Bom ...).
    std::function<int(const std::string& name, const std::string& head, std::string& id, Kind& kind)> sniff;
};

// Scan a file, a directory or an archive.
bool scan(const std::string& path, Result& out, std::string& err, const Options& opt = {});
// Scan in-memory entries (tests; also used for nested archives). `name` is the display name.
bool scan_entries(std::vector<Entry> entries, Result& out, const Options& opt = {});

// Classify a single blob (exposed for tests and the Explorer thumbnail/preview code).
Kind classify(const std::string& name, const std::string& head, int& score, std::string& importer);

// Bytes of one entry (reads disk files on demand).
bool entry_bytes(const Entry& e, std::string& out, std::string& err);

// Write the entries under `vprefix` ("" = all) to `dest_dir`, keeping relative paths below vprefix.
// Returns the destination path of every written file (UTF-8, native separators on Windows).
bool materialize(const Result& r, const std::string& vprefix, const std::string& dest_dir,
                 std::vector<std::string>& written, std::string& err);
// Destination path of `vpath` after materialize(r, vprefix, dest_dir).
std::string materialized_path(const std::string& vprefix, const std::string& dest_dir, const std::string& vpath);

} // namespace pcbp::detect
