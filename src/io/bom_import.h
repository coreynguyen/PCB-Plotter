#pragma once
// ============================================================================
// bom_import.h — Bill of Materials import into Project::parts and linking of
// part numbers to components by reference designator.
//
// Mirrors CircuitCAM "Import the Bill of Materials" (manual Part 3 §1.2):
// fixed or delimited ASCII BOM, column headings chosen per column,
// consolidated reference fields ("C1-C25, C27-C34") expanded automatically,
// "Multiple PN Check" (a reference given more than one part number).
// The Number library's Package field is what later links parts to shapes.
// ============================================================================
#include "../core/project.h"
#include "text_table.h"
#include <string>
#include <vector>

namespace pcbp::io {

struct BomMapping {
    int refs = -1, number = -1, package = -1, description = -1, process = -1;
    int cust_pn = -1, vend_pn = -1, manf_pn = -1, polarity = -1, soft_rev = -1;
    int part_rev = -1, qty = -1, socket = -1, height = -1, cost = -1;
    int stock_code = -1, bin = -1, line_item = -1, pop_type = -1, part_marking = -1;
    int value = -1, dnp = -1;
    bool valid() const { return refs >= 0 && number >= 0; }
};

struct BomOptions {
    TextTableOptions table;
    bool auto_map = true;               // fill unset mapping entries from header names / content
    BomMapping map;
    bool replace_parts = false;         // clear Project::parts first
    bool clear_comp_numbers = false;    // clear every component's number before linking
    bool overwrite_package = false;     // comps that already have a package take the BOM one
    bool multiple_pn_check = true;      // keep first PN for a ref and report conflicts (else last wins)
    bool dnp_unlisted = false;          // mark board comps absent from the BOM as DNP
};

struct BomReport {
    TextTable table;
    BomMapping map;
    int rows = 0, parts_added = 0, parts_updated = 0;
    int refs_total = 0;                 // expanded refs in BOM (unique)
    int linked = 0;                     // components that received a part number
    int board_components = 0;           // Feature::Component count
    std::vector<std::string> refs_not_on_board;
    std::vector<std::string> comps_not_in_bom;
    std::vector<std::string> duplicate_refs;     // ref listed on more than one row
    std::vector<std::string> pn_conflicts;       // "U3: 111 vs 222"
    std::vector<std::string> package_mismatch;   // "C27: board 08052, BOM 0805"
    std::vector<std::string> unmapped_columns;
    std::vector<std::string> messages;
    double coverage() const { return board_components ? 100.0 * linked / board_components : 0.0; }
    std::string describe() const;
};

BomMapping auto_map_bom(const TextTable& t);

// TRUE/FALSE, Y/N, YES/NO, 1/0, T/F, X, POL... -> bool; returns false if unrecognised.
bool parse_bool_cell(const std::string& s, bool& out);

bool import_bom_text(const std::string& text, Project& prj, const BomOptions& opt,
                     BomReport& rep, std::string& err);
bool import_bom_file(const std::string& path, Project& prj, const BomOptions& opt,
                     BomReport& rep, std::string& err);

} // namespace pcbp::io
