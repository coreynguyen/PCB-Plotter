// juki_research_rules - the derived (non-copy) rules of the editor's database read, written as our own code from
// the observed behaviour of the read: which database columns become which record bytes, under which conditions,
// with which re-codings. Every rule works on the database-read buffer of one part (block 4, 1822 bytes), which the
// editor starts from all zeros.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pcbp::juki::rules {

// Database access for one part: false when the table has no row for the part or the column is NULL.
using Get = std::function<bool(const char* table, const char* column, long& value)>;
using GetReal = std::function<bool(const char* table, const char* column, double& value)>;
using Lookup = std::function<bool(const char* table, const char* key_column, long key, const char* column, long& value)>;
using RowCount = std::function<size_t(const char* table)>;
using GetRow = std::function<bool(const char* table, size_t row, const char* column, long& value)>;
using GetText = std::function<bool(const char* table, const char* column, std::string& value)>;
using LookupText = std::function<bool(const char* table, const char* key_column, long key, const char* column, std::string& value)>;

// Compo table: comment, class, outline and its set flags, mold line, use-switches, adhesive test, data state
// (block 4 +43, +47..+104, +113, +117, +168..+175, +226, +246, +392, +696, +1124).
void compo(uint8_t* b4, const Get& get, const GetText& text);

// Feeder table: feeder kind, tape / stick / tray / bulk data, pitch and angle codes (block 4 +80, +113 bit 0,
// +176..+225, +1130, +1135).
void feeder(uint8_t* b4, const Get& get);

// Mech table (parts with UseMech): mechanical centering nozzle, vacuum, levels, speed (block 4 +227..+245).
void mech(uint8_t* b4, const Get& get);

// Vision table (parts with UseVision): laser-align / IC switches, nozzles, vacuum levels, speeds, centre offsets,
// the FMLA set (block 4 +247..+309, +354..+357, +382..+385).
void vision(uint8_t* b4, const Get& get);

// Laser table (parts with UseLaser): nozzles, vacuum, algorithm, laser heights, offsets, speeds and their set
// flags (block 4 +393..+443, +466, +478, +488, +503..+506, +603..+606, +652 bit 6, +654 bit 3).
void laser(uint8_t* b4, const Get& get);

// Default table acceleration by component type, volume and thickness, from the machine software's SubCompo*.ini
// (SubCompo.ini, SubCompoFx1r.ini, SubCompoFx2.ini); used where the database leaves an acceleration at 0.
struct SubCompo {
    struct Entry { int type = 0; double vmax = 0, vmin = 0, tmax = 0, tmin = 0; int accel = 0; };
    std::vector<Entry> entries;   // [0] = fallback
    bool load(const std::string& ini_path);
    bool empty() const { return entries.empty(); }
    int accel(const uint8_t* b4) const;   // 0 when no table
};

// Expansion table (+ GripNozzle): retries, over-pick / over-place per head, pick offsets, accelerations, blow and
// vacuum timings, flux / scrap settings (block 4 +25..+42, +704..+937, +1139..+1142, +1340). Returns false when the
// part has no Expansion row. Accelerations the database leaves at 0 are kept as they are in b4 (the editor derives
// them from the outline).
// accel: SubCompo tables for +1139 (general), +1141 (FX-1R), +1340 (FX-2); any may be null / empty.
bool expansion(uint8_t* b4, const Get& get, const Lookup& lookup, const SubCompo* const accel[3] = nullptr);

// Machine-specific vision / laser tables (VisionGx5, VisionCX, VisionGx6 for vision parts; LaserGx5, LaserFX1,
// LaserCX, LaserGx6, LaserFx3, LaserFx2 for laser parts) and the Fluxer table.
void machine_tables(uint8_t* b4, const Get& get);

// Inspection table: chip-stand / coplanarity / verify / dimension / pitch / board-height / component-height
// checks (block 4 +944..+1107). The dimension check applies to laser parts and to vision class-251 parts.
void inspection(uint8_t* b4, const Get& get, const GetReal& real);

// VisionData table (+ Side and CoplaGX4 references) into the vision control record (block 7, 1052 bytes) of a
// vision part: state, contrast, the four lead sides, lead pitch, bend / viewing / check levels, sub class,
// corner leads, lead edge, coplanarity set, QFN base style. b4 = the part's component record (class, orientation).
// Returns the BGA pattern id (0 = none).
long vision_control(uint8_t* b7, const uint8_t* b4, const Get& get, const Lookup& lookup);

// The four vision recognition sets of a vision part (GX4Vision, VCSControlGx5, VCSControlCX, VCSControlGx6), each
// with its light control, light division assignment and light pattern rows: block 7 +179 / +293 / +393 / +493 and
// block 18 (5240 bytes) +0 / +1296 / +2592 / +3888.
void vision_sets(uint8_t* b7, uint8_t* b18, const Get& get, const RowCount& count, const GetRow& row);

// BGA classes (107 / 108 / 110): the BGA pattern row named by VisionData.PatternID (block 7 +121..+126, +136).
// For a custom pattern (type 255) also the ball grid record (block 16, 12830 bytes: state, columns, rows, name,
// one hex digit per ball position, 160 x 160) and the ball count (block 7 +125). bga stays empty otherwise.
void bga(uint8_t* b7, std::vector<uint8_t>& bga, const uint8_t* b4, long pattern_id, const Lookup& lookup, const LookupText& text);

// The database read's final vision checks: classes without vision data drop it, out-of-range offsets are cleared,
// unused light patterns are dropped and the device indices renumbered (block 7 / 18 of a vision part).
void vision_finish(uint8_t* b4, uint8_t* b7, uint8_t* b18);

// The editor's vision check after the read: the vision control state (block 7 +0: 1 incomplete, 2 complete) from
// which lead-side fields are set (the class decides which it needs), mirrored into block 4 +247 when it changes.
// elements_done: the part's element record (block 19 +0) is complete (class 501).
void vision_state(uint8_t* b4, uint8_t* b7, bool elements_done);

// Element-group parts (class 501): the Gx4ExtendVision header (block 19 +0..+5), element groups (ElementGroup + its
// MissPattern rows) and extended arrays (ExtendArray + ElemLoc), each with its Element rows and their Shape* row.
// Records are appended to groups (170 B), elements (112 B), arrays (3142 B) and linked by indices local to the part:
// block 19 +20/+22 first group / count, +24/+26 first array / count; group / array +8 prev, +10 next, +12 first
// element, +14 element count; element +12 prev, +14 next (0xFFFF = none). Returns false without a header row.
struct ElemDb {
    RowCount part_rows;                                                                              // part tables
    GetRow part_val;
    std::function<bool(const char* table, size_t row, const char* column, std::string& value)> part_text;
    std::function<std::vector<size_t>(const char* table, const char* key_column, long key)> ref_rows;   // reference tables
    std::function<bool(const char* table, size_t row, const char* column, long& value)> ref_val;
    std::function<bool(const char* table, size_t row, const char* column, std::string& value)> ref_text;
};
bool elements(uint8_t* b19, std::vector<uint8_t>& groups, std::vector<uint8_t>& elems, std::vector<uint8_t>& arrays,
              const Get& get, const ElemDb& db);

// The editor's completeness check after the read: mech / vision / laser state bytes (+227, +247, +393) and the
// record state (+117: 2 complete, 1 incomplete). fx2_supported: the line supports FX-2 laser data (default).
void check(uint8_t* b4, bool fx2_supported = true);

}  // namespace pcbp::juki::rules
