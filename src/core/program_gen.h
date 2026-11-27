#pragma once
// ============================================================================
// program_gen.h — machine program generation without UI (LLM_API §3A.4 G4).
//
// The one path from a Project to verified program bytes / files. The GUI
// (gui/output_views.cpp: generate_program, export_all_sides) keeps its dialogs
// (review, folder choice, confirmations) and calls these; the headless CLI,
// MCP server and job runner call them directly. Nothing here blocks on
// validation: preflight issues are returned, never enforced (house rule).
//
//   generate_point / generate_side   Project -> MachineJob -> exporter bytes -> byte check
//   write_program                    atomic write (temp -> read-back -> replace) + post-write check
//   export_point / export_link       generate + write (+ placements CSV and sides report for a link)
// ============================================================================
#include "machine_job.h"
#include "export_preflight.h"
#include "sides.h"
#include "../plugins/plugin.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pcbp::progen {

// ---- names -------------------------------------------------------------------
std::string sanitize_filename(std::string s);                         // Windows-illegal chars -> '_', "" -> "program"
std::string exporter_ext(const Exporter* e);                          // default_ext, first extension, "txt"; "" when e == null
std::string default_program_file(const Project& p, int point, const Exporter* e);   // "<job base> <point name>.<ext>"
std::string program_file_for(const Project& p, int point, const Exporter* e);       // target_file, else default_program_file

// ---- exporters ---------------------------------------------------------------
bool is_machine_exporter(const Exporter* e);
Exporter* machine_exporter(PluginRegistry& reg, const std::string& id);   // null unless a MachineJob exporter
Exporter* machine_exporter(const std::string& id);                        // PluginRegistry::get()

// Exporter defaults <- point options (keys starting "__" are internal and dropped); also written into job.options.
Options merge_options(const Exporter& ex, const MachineSetup& ms, MachineJob& job);

// ---- checks ------------------------------------------------------------------
// Board/circuit marks the program actually carries (JUKI: the writer's selection; others: all job marks).
int written_mark_count(const MachineJob& j);
// false = not a readable program (fail says why). errors = format-check errors in a readable program.
bool check_program_bytes(const std::string& exporter_id, const std::vector<uint8_t>& b,
                         std::vector<std::string>& errors, std::string& fail);

// ---- generation --------------------------------------------------------------
struct Generated {
    MachineJob job;
    std::vector<uint8_t> bytes;
    std::string exporter_id, exporter_name;
    std::vector<std::string> byte_errors;   // check_program_bytes errors (readable program, format issues)
    int point = -1;                         // model point index
    std::string file;                       // program_file_for(...)
};

// Single export layer. false + err: no machine interface, empty job, exporter failure, unreadable bytes.
bool generate_point(const Project& p, int point, Generated& out, std::string& err, PluginRegistry* reg = nullptr);
// One side of a linked pair: build_side_job, side comment, description mode.
bool generate_side(const Project& p, const MfgSide& s, Generated& out, std::string& err, PluginRegistry* reg = nullptr);

// ---- writing -----------------------------------------------------------------
struct WriteResult {
    bool ok = false;
    std::string path;
    std::string stage;                      // write_file_verified stage on failure ("verify", "verify-temp", ...); "" = ok
    std::string err;
    std::vector<std::string> post_errors;   // format check of the bytes read back from disk
    bool verify_failed() const { return stage == "verify" || stage == "verify-temp"; }
};
std::string join_path(const std::string& dir, const std::string& file);   // '/' or '\\' kept as given in dir
bool write_program(const std::string& path, const Generated& g, WriteResult& r);

// ---- one-call exports (CLI / MCP / job runner) -------------------------------
struct PointExport {
    Generated gen;
    WriteResult write;
    std::vector<ExportIssue> preflight;
    bool ok = false;
    std::string err;
};
// dir "" = Project::model.export_dir. dry_run: generate + check, no file written (write.path still filled).
bool export_point(const Project& p, int point, const std::string& dir, PointExport& out, bool dry_run = false,
                  PluginRegistry* reg = nullptr);

struct LinkExport {
    std::vector<SideExportResult> sides;    // Side 1 first (run order)
    std::vector<Generated> gens;            // parallel to sides (bytes kept for callers / tests)
    std::vector<ExportIssue> preflight;
    std::string dir, report_html, report_txt;   // report paths ("" on dry run)
    bool all_ok = false;
};
// Byte errors in a readable program that the preflight did not list: return false to skip writing that side
// (GUI: a confirmation). Default (empty): write anyway; the errors are reported in the side's warnings.
using ByteErrorGate = std::function<bool(const MfgSide&, const std::vector<std::string>& fresh_errors)>;
bool export_link(const Project& p, int link, const std::string& dir, LinkExport& out, std::string& err,
                 bool dry_run = false, const ByteErrorGate& gate = {}, PluginRegistry* reg = nullptr);

// Byte-check errors not already present (as a message suffix) among the reviewed Error issues.
std::vector<std::string> fresh_byte_errors(const std::vector<ExportIssue>& reviewed, const std::vector<std::string>& errors);

} // namespace pcbp::progen
