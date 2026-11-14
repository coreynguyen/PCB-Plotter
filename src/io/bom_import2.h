#pragma once
// ============================================================================
// bom_import2.h — Bill of Materials import on the text_import engine
// (REVIEW_V9 §11 redesign, R10).
//
//   bom_load_source()  .xlsx/.xls/.ods/.csv/.txt/... -> BomSource (spreadsheets
//                      are flattened to tab-separated text, merged-row and
//                      sheet info kept). Implemented in bom_source.cpp.
//   bom_preview()      analyse + build BOM lines through the explicit row model
//                      (Header / First / Last data row, excluded rows, automatic
//                      category / blank / note / footer exclusion with reasons).
//   bom_rebuild()      row-model / role / option edits without re-analysis.
//   bom_apply()        write Project::parts and link components by reference.
//                      DNP refs keep identity + excluded status and get no
//                      active P/N or package link; board comps absent from the
//                      BOM become Orphan (never DNP); package mismatch only when
//                      the BOM supplied a package AND it disagrees.
//   BomWizardHistory   wizard-local undo / redo / reset over the row model and
//                      column mapping (always consistent: snapshots, not deltas).
//
// Results are reported in the existing BomReport (bom_import.h) plus a
// severity-categorised summary (bom_summary()).
// ============================================================================
#include "bom_import.h"
#include "text_import.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace pcbp::io {

enum class BomView { ByComponent, ByPartNumber };
enum class BomLinkMode {
    PartsOnly,          // only update the part-number library
    Link,               // library + link part numbers to components by ref (keep other comps' numbers)
    Relink              // clear every component's number first, then link
};
enum class DnpMode {
    Ignore,             // DNP flags ignored (DNP lines import like any other line)
    Mark,               // DNP refs: component kept, marked DNP, no P/N / package link (default)
    Skip                // DNP refs: component left untouched (reported only)
};

// ---- explicit row model (all raw line indices are 0-based, -1 = automatic) ----
struct BomRowModel {
    int header_row = -1;                // raw line of the column header
    int first_data = -1;                // first raw line that may hold data (-1 = line after the header)
    int last_data = -1;                 // last raw line that may hold data (-1 = end of table)
    std::set<int> excluded;             // raw lines the user excluded
    std::set<int> included;             // raw lines the user forced in (overrides automatic exclusion)
    bool auto_exclude = true;           // category / blank / note / footer rows excluded automatically
    bool operator==(const BomRowModel&) const = default;
};

enum class RowReason : uint8_t {
    Data,           // included
    ForcedIn,       // included by the user
    Header,         // the header row
    BeforeFirst,    // above First Data Row (title / preamble)
    AfterLast,      // below Last Data Row
    UserExcluded,
    Blank,
    Category,       // merged / single-cell section title ("CAPACITORS")
    Note,           // text without reference or part number (notes, alternates continuation)
    Footer,         // after the last row with references (totals, sign-off)
    Comment,
    RepeatedHeader
};
const char* row_reason_name(RowReason r);       // "excluded: category row"
const char* row_reason_badge(RowReason r);      // short badge "CAT"

struct BomRowStatus {
    int line = 0;                       // raw line (0-based)
    int src = -1;                       // index into ParsedText::rows (-1: line not a record)
    bool included = false;
    RowReason reason = RowReason::Data;
};

struct BomSource {
    std::string text;                   // UTF-8 text fed to the engine (spreadsheets: TSV)
    std::string kind;                   // "xlsx" / "xls" / "ods" / "text"
    std::string sheet;                  // chosen worksheet name ("" for text)
    std::vector<std::string> sheets;    // all worksheet names
    std::vector<uint8_t> merged;        // per raw line: 1 = row has a merged area spanning >= 3 columns
};
// sheet_index -1: the worksheet that looks most like a BOM (most ref-list cells).
bool bom_load_source(const std::string& path, BomSource& out, std::string& err, int sheet_index = -1);
bool bom_source_from_bytes(const std::string& bytes, const std::string& name_hint, BomSource& out,
                           std::string& err, int sheet_index = -1);

// A conservative profile: only exact header combinations qualify.  LCSC is a
// supplier stock number, not a manufacturer part number.
enum class BomProfile : uint8_t { Generic, KiCadNative, KiCadJlcpcb };
const char* bom_profile_name(BomProfile p);

// Verified supplier catalog fact.  Collected by the caller from a trusted
// catalog response or from the existing project parts.  Never guess MPNs.
struct BomLookup {
    std::string code, manufacturer, mpn, package, description, datasheet;
};

struct BomOptions2 {
    ImportLayout layout;
    BomRowModel rows;
    bool expand_refs = true;            // "Expand References (e.g. R1-R5)"
    bool upper_refs = true;
    RefExpandOptions ref_limits;        // max span per range / per cell
    bool multiple_pn_check = true;      // keep first PN per ref and report others (else last wins)
    bool qty_check = true;              // compare Qty column against expanded ref count
    bool dnp_from_text = true;          // DNP token in value / description / PN cells marks the line DNP
    // Recognised not-populated indicators (whole cell or word, case-insensitive).
    std::vector<std::string> dnp_tokens{"DNP", "DNI", "DNS", "NS", "DO NOT POPULATE", "DO NOT INSTALL",
                                        "DO NOT STUFF", "NO STUFF", "NOT POPULATED"};
    bool synth_pn = true;               // no PN column: PN = MPN; KiCad JLC uses LCSC; else value[_package]
    std::map<std::string, BomLookup> verified_lookups; // code -> checked catalog fact, optional
    bool replace_parts = false;         // clear Project::parts first
    bool overwrite_package = false;     // comps that already have a package take the BOM one
    bool commit_packages = false;       // "Commit BOM packages to library": BOM package -> PartNumber::package
    BomLinkMode link = BomLinkMode::Link;
    DnpMode dnp = DnpMode::Mark;
};

struct BomLine {
    int line = 0;                       // 1-based raw line
    int src = -1;                       // index into ParsedText::rows
    std::vector<std::string> refs;      // expanded (or single raw cell when expand off)
    std::string item, pn, package, value, description, manufacturer, mpn, vendor, vendor_pn, process, dnp_text;
    int qty = -1;                       // -1 = no qty column / unparsable
    bool qty_mismatch = false;
    bool dnp = false;
    bool pn_synth = false;
};

struct BomCompRow { std::string ref, pn, package, description; int line = 0; bool dnp = false; bool conflict = false; };
struct BomPartRow { std::string pn, package, description, value, mpn; std::vector<std::string> refs; int qty_file = -1; int lines = 0; bool dnp = false; };

enum class IssueSev : uint8_t { Info, Warning, Error };
const char* issue_sev_name(IssueSev s);
struct BomIssue {
    int line = 0;
    std::string msg;
    IssueSev sev = IssueSev::Warning;
};

struct BomPreview {
    BomProfile profile = BomProfile::Generic;
    ParsedText text;
    std::vector<uint8_t> merged;        // from BomSource (per raw line)
    std::vector<BomRowStatus> row_status;   // one per raw line in the table window
    std::vector<BomLine> lines;
    std::vector<BomCompRow> by_comp;    // View Mode: Component (sorted by ref)
    std::vector<BomPartRow> by_part;    // View Mode: Part Number
    std::vector<BomIssue> issues;
    std::vector<std::string> pn_conflicts, duplicate_refs;
    int qty_mismatches = 0;
    std::string board_pn;               // part number of a skipped bare-board line (class PCB), if any
    int effective_header = -1, effective_first = -1, effective_last = -1;   // raw lines actually used
    bool mapping_ok() const;            // Ref mapped and PN (or synthesisable)
    const BomRowStatus* status_of_line(int raw_line) const;
    int included_rows() const;
};

bool bom_preview(const std::string& bytes, const BomOptions2& opt, BomPreview& out, std::string& err);
// Export the reviewed/enriched BOM as an ordinary CSV; always preserves the
// original reference, value and footprint, even if supplier data is missing.
std::string bom_completed_csv(const BomPreview& p, const BomOptions2& opt);
bool bom_preview_src(const BomSource& src, const BomOptions2& opt, BomPreview& out, std::string& err);
bool bom_preview_file(const std::string& path, const BomOptions2& opt, BomPreview& out, std::string& err);
void bom_rebuild(BomPreview& p, const BomOptions2& opt);

// Not-populated test against the configured token list (whole cell or word-bounded).
bool bom_text_is_dnp(const std::string& s, const std::vector<std::string>& tokens, bool words_only_long = true);

// Roles offered by the BOM wizard (BOM fields only) and their BOM labels
// ("Internal Part Number", "Manufacturer Part Number", "DNP / Assembly Option" ...).
std::vector<Role> bom_wizard_roles();
const char* bom_role_label(Role r);
bool bom_role_numeric(Role r);          // only Quantity needs numeric parsing

bool bom_apply(const BomPreview& p, Project& prj, const BomOptions2& opt, BomReport& rep, std::string& err);
bool import_bom2_file(const std::string& path, Project& prj, const BomOptions2& opt, BomReport& rep,
                      BomPreview* preview, std::string& err);
// Explicit "Commit BOM packages to library": PartNumber::package <- the BOM package of
// its (non-DNP) refs where the BOM supplied one. Returns parts changed.
int bom_commit_packages(Project& prj);

// Severity-categorised review summary in operational language.
struct BomSummaryItem { IssueSev sev; std::string title, detail; };
std::vector<BomSummaryItem> bom_summary(const BomPreview& p, const BomReport* rep = nullptr);

// ---- wizard-local undo / redo / reset ----
struct BomWizardState {
    BomRowModel rows;
    std::map<int, Role> roles;          // ImportLayout::role_override
    bool operator==(const BomWizardState&) const = default;
};
class BomWizardHistory {
public:
    void reset_to(const BomWizardState& initial) { initial_ = initial; undo_.clear(); redo_.clear(); cur_ = initial; }
    // Record a new state (no-op when unchanged). Clears redo.
    void push(const BomWizardState& s) { if (s == cur_) return; undo_.push_back(cur_); if (undo_.size() > 200) undo_.erase(undo_.begin()); cur_ = s; redo_.clear(); }
    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
    const BomWizardState& undo() { if (!undo_.empty()) { redo_.push_back(cur_); cur_ = undo_.back(); undo_.pop_back(); } return cur_; }
    const BomWizardState& redo() { if (!redo_.empty()) { undo_.push_back(cur_); cur_ = redo_.back(); redo_.pop_back(); } return cur_; }
    // Reset is itself undoable.
    const BomWizardState& reset() { push(initial_); return cur_; }
    const BomWizardState& current() const { return cur_; }
    size_t depth() const { return undo_.size(); }
private:
    BomWizardState initial_, cur_;
    std::vector<BomWizardState> undo_, redo_;
};

} // namespace pcbp::io
