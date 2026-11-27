// juki_record - one H8H part's research records (block 4 / 7 / 18 / 19 / 16 / 20-22) from its database rows, the way
// the HLC editor's database read builds them (rules: juki_research_rules). Shared by PCB Plotter (with a FlexLine DB:
// juki_research) and Juki Tool (from generated rows: juki_builtin / juki_resize). Depends on nothing but the C++
// standard library and juki_research_rules.
#pragma once
#include "juki_research_rules.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace pcbp::juki {

struct ResearchRecord {
    std::vector<uint8_t> b4, b7, b18, b19;   // empty b7/b18/b19 = none (written as zeros)
    std::vector<uint8_t> bga;                // custom BGA ball pattern (block 16, 12830 B), empty = none
    // element-group parts (class 501): element groups (block 20, 170 B each), elements (block 21, 112 B) and extended
    // arrays (block 22, 3142 B), concatenated, linked by indices local to the part (the writer renumbers them);
    // b19 holds the part's element header (first group / array and counts)
    std::vector<uint8_t> groups, elements, arrays;
    bool has_vision() const;                 // b7 not all zero
};

struct ResearchResult {
    ResearchRecord rec;
    std::string why;
};

// A part's single-row column values (Compo, Feeder, Laser, ...), by table and column (case-insensitive). NaN = NULL.
struct PartRows {
    std::map<std::string, std::map<std::string, double>> num;
    std::map<std::string, std::map<std::string, std::string>> text;
    void set(const std::string& table, const std::string& column, double v);
    void set_null(const std::string& table, const std::string& column);
    void set_text(const std::string& table, const std::string& column, const std::string& v);
    bool get(const std::string& table, const std::string& column, double& v, bool& null) const;   // false = not given
    bool get_text(const std::string& table, const std::string& column, std::string& v) const;
    bool has_table(const std::string& table) const;
};

// Set the per-component index fields (block 4 +338, block 7 +291/+391/+491/+591) for component `index`.
void research_set_index(ResearchRecord& r, int index);

// What a research reads: the part's single-row columns, its multi-row tables, the reference tables, the
// acceleration tables (accel: SubCompo[3], may be null). Unset multi-row / reference callbacks read as empty.
struct RecordSource {
    rules::Get get;
    rules::GetReal real;
    rules::GetText text;
    rules::Lookup lookup;
    rules::LookupText ltext;
    rules::RowCount count;
    rules::GetRow row;
    rules::ElemDb edb;   // element-group tables (class 501); part_rows / part_val default to count / row
    const rules::SubCompo* accel = nullptr;
};
bool research_with(const RecordSource& s, ResearchResult& out);

// A part made only of `rows` (no database: no reference tables). accel: SubCompo[3] or null.
bool research_record(const PartRows& rows, ResearchResult& out, const rules::SubCompo* accel = nullptr);

}  // namespace pcbp::juki
