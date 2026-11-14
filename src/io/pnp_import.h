#pragma once
// ============================================================================
// pnp_import.h — pick-and-place (centroid / XY / CPL / .pos / .mnt) import on
// top of the text_import engine. Two steps, matching CircuitCAM's #508 "ASCII
// Import" dialog flow:
//
//   pnp_preview()  analyse the file and build parsed rows (Results list):
//                  raw lines, columns + roles (for tagging), units with the
//                  reason, per-row issues. A UI changes PnpOptions::layout
//                  (delimiter, fixed cuts, role_override ...) and calls
//                  pnp_preview() again, or pnp_rebuild() after role-only edits.
//   pnp_apply()    push the preview rows into the Project using the
//                  CentroidOptions modes (Replace / Merge / Fusion).
//
// Units: column header "(mm)", cell suffix "12.5mm"/"500mil", per-row unit
// column, file comment ("Unit = mm"), else magnitude / board-size guess.
// Bottom side: optional X un-mirroring (negate or W - x) and rotation
// convention (as is / -rot / 180-rot / rot+180), applied before rot_offset.
// ============================================================================
#include "centroid_import.h"
#include "text_import.h"
#include <map>
#include <string>
#include <vector>

namespace pcbp::io {

struct PnpOptions {
    ImportLayout layout;                // structure + role overrides
    LenUnit units = LenUnit::Auto;      // Auto = header / suffix / comment / magnitude
    double factor = 1.0;                // CircuitCAM "Factor"
    double rot_offset = 0;              // added after side conventions
    bool mirror_x = false;              // whole file: x -> -x, rot -> 180 - rot
    bool mirror_y = false;              // whole file: y -> -y, rot -> -rot
    BottomX bottom_x = BottomX::AsIs;   // bottom rows: X as exported by CAD
    double board_width = 0;             // BottomX::MirrorWidth: W (mm); 0 = max X in file
    BottomRot bottom_rot = BottomRot::AsIs;
    SideSelect side_select = SideSelect::Both;
    std::vector<std::string> top_tokens{"T", "TOP", "TOPLAYER", "TOP LAYER", "F", "FRONT", "F.CU", "COMPONENT", "1", "L1",
                                        "TOP_SIDE", "TOPSIDE", "OBEN", "DESSUS", "SUPERIOR", "PLACEMENT_TOP"};
    std::vector<std::string> bottom_tokens{"B", "BOT", "BOTTOM", "BOTTOMLAYER", "BOTTOM LAYER", "BACK", "B.CU", "SOLDER",
                                           "2", "BOT_SIDE", "BOTTOM_SIDE", "BOTTOMSIDE", "UNTEN", "DESSOUS", "INFERIOR",
                                           "PLACEMENT_BOTTOM"};
    Side default_side = Side::Top;      // also the side of rows whose side cell is blank
    std::map<std::string, int> side_override;   // R16c: side cell value (upper-case, trimmed) -> 0 top / 1 bottom
    bool upper_refs = true;             // normalise refs to upper case
    bool skip_dnp = false;              // rows flagged DNP are not imported (else imported with dnp=true)
    CentroidOptions apply;              // mode Replace/Merge/Fusion + fusion parameters (units/mirror fields ignored)
};

// R16c (REVIEW_V15 §7): how the Side column is read. Every distinct cell value gets an automatic
// side (words Top/Bottom/TopLayer/BottomLayer, letters T/B, M = mirrored = bottom, mirror-flag columns,
// numeric layer numbers: the lowest layer is top, the highest bottom), a user override wins, and a
// blank cell takes PnpOptions::default_side.
enum class SideConv : uint8_t { NoColumn, Words, Letters, Mirror, Numeric, Mixed };
const char* side_conv_name(SideConv c);
struct SideValue {
    std::string token;          // upper-case, trimmed; "" = blank cells
    int count = 0;              // rows with this value
    int auto_side = -1;         // 0 top, 1 bottom, -1 not understood
    int side = -1;              // effective (override, else auto; blank -> default side)
    bool overridden = false;
};
struct SideAnalysis {
    int col = -1;
    SideConv conv = SideConv::NoColumn;
    std::vector<SideValue> values;   // sorted by count (desc), blank last
    int top = 0, bottom = 0, blank = 0, unknown = 0;   // data-row counts (blank rows are included in top/bottom)
    int side_of(const std::string& cell) const;        // -1 unknown
};
SideAnalysis analyze_sides(const ParsedText& t, int col, bool invert_column, const PnpOptions& o);

// R16c: content sniffing for a dropped / opened delimited text file.
enum class CsvKind : uint8_t { Unknown, Bom, Centroid };
const char* csv_kind_name(CsvKind k);
// Centroid = Reference + numeric X and Y columns; BOM = Reference (lists) + Qty / part number / value
// and no coordinate pair. why (optional) gets a one-line explanation.
CsvKind sniff_csv_kind(const std::string& bytes, std::string* why = nullptr);

struct PnpIssue { int line = 0; std::string msg; };   // line: 1-based raw line, 0 = general

struct PnpPreview {
    ParsedText text;
    LenUnit units = LenUnit::Auto;      // effective file unit (per-cell suffixes may override)
    std::string units_reason;
    std::vector<CentroidRow> rows;      // parsed rows (mm / deg, conventions applied)
    std::vector<int> row_src;           // index into text.rows for each parsed row
    std::vector<PnpIssue> issues;
    int skipped = 0, filtered = 0, dnp = 0;
    SideAnalysis sides;                 // R16c: side column interpretation (preview counts + override table)
    bool mapping_ok() const;            // Ref, X, Y mapped
};

int parse_side_token(const std::string& cell, const PnpOptions& o, bool invert_column);   // 0 top, 1 bottom, -1 ?

bool pnp_preview(const std::string& bytes, const PnpOptions& opt, PnpPreview& out, std::string& err);
bool pnp_preview_file(const std::string& path, const PnpOptions& opt, PnpPreview& out, std::string& err);
// Re-assign roles (opt.layout.role_override) and rebuild rows without re-analysing structure.
void pnp_rebuild(PnpPreview& p, const PnpOptions& opt);

bool pnp_apply(const PnpPreview& p, Project& prj, const PnpOptions& opt, CentroidReport& rep, std::string& err);
bool import_pnp_file(const std::string& path, Project& prj, const PnpOptions& opt, CentroidReport& rep,
                     PnpPreview* preview, std::string& err);

} // namespace pcbp::io
