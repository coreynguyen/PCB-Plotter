#pragma once
// ============================================================================
// export_preflight.h — R21 (AUDIT_V20 P0): the ONE export preflight.
//
// Every Generate path asks this function for its review list before anything is
// written: the shell Generate command (Ctrl+G / menus), the Export view's single
// layer and linked pair, export_all_sides and plugin (DLL) machine exporters.
// The GUI shows the result in a single "Generate anyway?" confirmation
// (app::confirm_generate): errors listed apart from warnings, Cancel the default
// when errors exist. User policy: nothing hard-blocks export.
//
// The entry point never changes the issues: a request is reduced to its target
// (one export layer, or the linked pair the layer belongs to) and the target alone
// decides the list. tests/test_export_preflight.cpp checks that.
// ============================================================================
#include "machine_job.h"
#include "project.h"
#include <string>
#include <vector>

namespace pcbp {

enum class ExportEntry {
    GenerateCommand,      // shell Generate Program command (menu, toolbar, Ctrl+G)
    ExportViewLayer,      // Export view: Generate program / This layer only
    ExportViewPair,       // Export view: Generate linked pair (validation list of a linked layer)
    ExportAllSides,       // app::export_all_sides
    PluginExport,         // a plugin (DLL) machine exporter, through generate_program
};
const char* export_entry_name(ExportEntry e);

struct ExportRequest {
    ExportEntry entry = ExportEntry::GenerateCommand;
    int point = -1;       // the export layer the command acts on (any member of a linked pair)
};

// What a request validates: one layer, or (pair entries on a configured link) the whole link.
struct ExportTarget {
    int point = -1;       // single layer; -1 when link > 0
    int link = 0;         // linked pair
    bool operator==(const ExportTarget& o) const { return point == o.point && link == o.link; }
};
ExportTarget export_target(const Project& p, const ExportRequest& r);

// The review list for a target: validate_sides() (pair) merged with validate_for_export() per program,
// sorted Error, Warning, Info.
std::vector<ExportIssue> export_preflight(const Project& p, const ExportTarget& t);
inline std::vector<ExportIssue> export_preflight(const Project& p, const ExportRequest& r) { return export_preflight(p, export_target(p, r)); }
std::vector<ExportIssue> export_preflight_point(const Project& p, int point);
std::vector<ExportIssue> export_preflight_link(const Project& p, int link);

// Confirmation text: errors and warnings in separate sections (Info is never listed).
std::string export_review_text(const std::vector<ExportIssue>& v, size_t max_lines_per_group = 12, bool group_headers = true);

} // namespace pcbp
