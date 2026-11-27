// juki_research - fill H8H component data from a FlexLine component database, the way the HLC editor's
// "Database -> Research" does, without the editor.
//
// research_part() reads one part's rows (Compo, Feeder, Expansion, Mech, Vision, Laser and the machine-specific
// Vision* / Laser* tables, Fluxer, Inspection, VisionData with its Side / CoplaGX4 / BGA references, the four
// recognition sets with their light tables) into the records the editor's database read produces, starting from
// empty records; the rules are in juki_research_rules. The H8H writer then merges the component record into the
// program's own record (research_merge_b4) as Research does.
//
// Research records per component:
//   block 4  (1822 B) component record (name, outline, feeder, nozzle, laser, speeds, ...)
//   block 7  (1052 B) vision control (all zero for parts without vision)
//   block 18 (5240 B) recognition-set lights, all zero without vision
//   block 19 (36 B)   fixed record
//   block 16 (12830 B, shared by name) the ball grid of a custom-pattern BGA
// Block 4 +338 and block 7 +291/+391/+491/+591 hold the component's own index when it has vision data; block 7
// +135 the index of its ball grid in block 16 (set by the writer).
#pragma once
#include <cstdint>
#include "juki_record.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace pcbp::juki {


// Rows of a FlexLine component DB (every table keyed by CompoID), read once.
class ResearchDb {
public:
    bool open(const std::string& path, std::string& err);
    bool is_open() const { return !ids_.empty(); }
    long id_of(const std::string& name) const;                  // case-insensitive CompoName, 0 = missing
    // the part a (possibly longer than 20 characters) part number names: CompoName == part, else ExtCompoName ==
    // part, else CompoName == the first 20 characters when that part's ExtCompoName is empty or the part number.
    // machine_name: the part's CompoName. 0 = none.
    long find_part(const std::string& part, std::string* machine_name = nullptr) const;
    std::vector<std::string> names() const;                     // every CompoName (upper case), sorted
    // value of Table.Column for the part (first row of that table); false when the part has no row / NULL
    bool value(long id, const std::string& table, const std::string& column, double& v) const;
    // text column (Compo.Description ...); false when the part has no row / NULL
    bool text(long id, const std::string& table, const std::string& column, std::string& v) const;
    // tables with several rows per part (LightPattern, LightDivAsgn ...): row count and value of row n (table order)
    size_t rows(long id, const std::string& table) const;
    bool value_n(long id, const std::string& table, size_t n, const std::string& column, double& v) const;
    bool text_n(long id, const std::string& table, size_t n, const std::string& column, std::string& v) const;
    // rows of a reference table whose key_column equals key (table order), and their values
    std::vector<size_t> ref_rows(const std::string& table, const std::string& key_column, long key) const;
    bool ref_value(const std::string& table, size_t row, const std::string& column, double& v) const;
    bool ref_text(const std::string& table, size_t row, const std::string& column, std::string& v) const;
    // value of Table.Column in the row of a reference table (GripNozzle, ...) whose key_column equals key
    bool lookup(const std::string& table, const std::string& key_column, long key, const std::string& column, double& v) const;
    bool lookup_text(const std::string& table, const std::string& key_column, long key, const std::string& column, std::string& v) const;
    size_t parts() const { return ids_.size(); }
    // the machine software's folder (C:\HLC): its SubCompo*.ini give the default table accelerations the editor uses
    // for parts whose database row leaves them at 0. Returns the number of tables found (0..3).
    int load_machine_folder(const std::string& folder);
    const rules::SubCompo* accel_tables() const { return accel_; }

private:
    std::map<std::string, long> ids_;                          // upper name -> CompoID
    std::map<std::string, long> ext_;                          // upper ExtCompoName -> CompoID
    std::map<long, std::string> name_of_, ext_of_;             // CompoID -> CompoName / ExtCompoName
    rules::SubCompo accel_[3];                                 // SubCompo.ini, SubCompoFx1r.ini, SubCompoFx2.ini
    struct Row { std::vector<double> v; std::vector<uint8_t> null; std::vector<std::string> s; std::vector<uint8_t> snull; };
    struct Tab { std::map<std::string, int> cols, scols; std::map<long, std::vector<Row>> rows; std::vector<Row> list; };
    std::map<std::string, Tab> tabs_;                          // upper table name
};


// Research one part (CompoName, case-insensitive). false + err when the part is not in the database.
bool research_part(const ResearchDb& db, const std::string& name, ResearchResult& out, std::string& err);


// Research a part made from a template: the rows of template part `template_id` of db (a package profile of a
// FlexLine package library, or any part), with `over` put over its single-row columns where the template has that
// table's row (as applying a profile and then the part's own body / carrier / nozzle to a component DB does).
// template_id 0: no template; the part is exactly `over` (its tables are the tables it names), db only supplies
// reference tables and acceleration tables (may be an empty ResearchDb).
bool research_rows(const ResearchDb& db, long template_id, const PartRows& over, ResearchResult& out, std::string& err);


}  // namespace pcbp::juki
