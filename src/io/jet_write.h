#pragma once
// ============================================================================
// jet_write.h — Jet4 (.mdb, Access 2000-2003) table editor.
//
// Companion to the read-only jet_mdb reader. Supports exactly what the FlexLine
// component-database integration needs: load a table as raw cells, change /
// add / delete rows in memory, and write the table back. No DDL (tables,
// columns, indexes and relationships must already exist — e.g. a FlexLine
// empty template or an existing component database).
//
// Write strategy (per stored table):
//   * every row is re-encoded from cells (round-trip verified byte-exact on
//     real FlexLine databases, see tests/test_flexdb.cpp);
//   * the table's own data pages and index pages are recycled as one page
//     pool, extra pages are appended at the end of the file;
//   * every real index B-tree is rebuilt from scratch (leaf + intermediate
//     pages, prefix compression, entry bitmaps) with Jet's key encodings:
//     integers (sign-flipped big endian) and Text in the Jet4 "General" sort
//     order (table learned from real FlexLine indexes). Keys of rows whose
//     indexed values did not change are carried over from the old index;
//   * used / free page usage maps are rewritten (inline type-0 maps; a new map
//     page is appended when the old map row is too small);
//   * TDEF row count, autonumber and index row counters are updated;
//   * pages are re-encrypted with the database key on save.
// Long values (OLE/Memo) of existing rows are preserved as their original
// LVAL pointers; new long values can only be NULL or inline (<= 1 KiB).
// ============================================================================
#include "jet_mdb.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace pcbp::jet {

struct Cell {
    bool present = false;           // false = NULL (Bool: false)
    std::vector<uint8_t> b;         // raw column bytes (fixed: size bytes, var: stored bytes)
};

struct ECol {
    std::string name;
    ColType type = ColType::Text;
    int size = 0, col_num = 0, var_index = 0, fixed_offset = 0, flags = 0;
    bool fixed = false;
    bool autonum() const { return (flags & 0x04) != 0; }
};

struct ERow {
    std::vector<Cell> c;            // index = position in ETable::cols
    uint32_t src_ptr = 0;           // original (page << 8 | row), 0 for new rows
    uint16_t ncols = 0, nvar = 0;   // column / var-column counts stored in the row (0 = table's)
    uint16_t fixed_len = 0;         // fixed-area length stored in the row (0 = table's)
    std::vector<uint8_t> raw_fixed, raw_mask;   // original bytes (keeps data of deleted columns)
    bool dirty = true;              // new or changed since load
};

struct ETable {
    std::string name;
    uint32_t tdef_pg = 0;
    std::vector<ECol> cols;         // sorted by col_num
    std::vector<ERow> rows;
    int col(const std::string& name) const;   // case-insensitive, -1 if missing

    // typed access (by column position)
    Value get(size_t r, int c) const;
    std::string get_s(size_t r, int c) const { return c < 0 ? std::string() : get(r, c).as_string(); }
    int64_t get_i(size_t r, int c) const { return c < 0 ? 0 : get(r, c).as_int(); }
    bool set(size_t r, int c, const Value& v, std::string* err = nullptr);   // marks dirty
    void set_null(size_t r, int c);
    bool set_i(size_t r, int c, int64_t v) { Value x; x.null = false; x.i = v; x.d = (double)v; return set(r, c, x); }
    bool set_s(size_t r, int c, const std::string& s) { Value x; x.null = false; x.s = s; return set(r, c, x); }
    size_t add_row();                          // all NULL, returns index
};

// Jet4 General-sort index key for a text value. false if a character is not in the
// learned collation table (caller should sanitise names before writing).
bool jet4_text_key(const std::string& utf8, std::vector<uint8_t>& key);
// Jet4 text column encoding (compressed unicode when possible).
std::vector<uint8_t> jet4_encode_text(const std::string& utf8);

class Editor {
public:
    bool open(const std::string& utf8_path, std::string& err);
    bool open_memory(std::vector<uint8_t> file, std::string& err);
    bool save(const std::string& utf8_path, std::string& err) const;  // writes atomically (tmp + rename)
    std::vector<uint8_t> image() const;                               // encrypted file bytes
    bool has_table(const std::string& name) const;
    std::vector<std::string> tables() const;
    bool load(const std::string& name, ETable& t, std::string& err) const;
    bool store(ETable& t, std::string& err);   // rewrite table from t.rows; t.rows src_ptr updated
    uint32_t autonumber(const std::string& table) const;   // TDEF autonumber counter (last value used)

    // diagnostics (tests)
    struct Stats { size_t keys_reused = 0, keys_computed = 0, keys_mismatch = 0, pages_appended = 0; };
    Stats stats;
    // re-encode every row of a table and compare with the stored bytes (count mismatches)
    bool roundtrip_check(const std::string& name, size_t& rows, size_t& mismatches, std::string& err) const;
    // rebuild index entries in memory and compare with the stored index (count mismatches)
    bool index_check(const std::string& name, size_t& entries, size_t& mismatches, std::string& err) const;

private:
    struct RIdx {
        std::vector<std::pair<int, bool>> cols;   // (col_num, ascending)
        size_t def_off = 0;                        // offset of the 52-byte real-index def in tdef buffer
        size_t cnt_off = 0;                        // offset of the 12-byte counter entry
        uint32_t umap = 0, first_dp = 0;
    };
    struct TDef {
        uint32_t pg = 0;
        std::vector<uint32_t> chain;               // tdef pages
        std::vector<uint8_t> buf;                  // concatenated (page0 full, continuations from +8)
        std::vector<ECol> cols;
        std::vector<RIdx> ridx;
        uint16_t num_cols = 0, num_var = 0;
        uint32_t used_ptr = 0, free_ptr = 0;
    };
    bool load_tdef(uint32_t pg, TDef& td, std::string& err) const;
    void put_tdef(const TDef& td, size_t off, const uint8_t* src, size_t n);
    uint8_t* pg(uint32_t n) { return data_.data() + (size_t)n * ps_; }
    const uint8_t* pg(uint32_t n) const { return data_.data() + (size_t)n * ps_; }
    uint32_t num_pages() const { return (uint32_t)(data_.size() / ps_); }
    uint32_t append_page();
    bool map_pages(uint32_t ptr, std::vector<uint32_t>& out) const;
    bool row_at(uint32_t pg, uint32_t row, const uint8_t*& p, uint32_t& n, uint16_t& flags) const;
    bool scan(const TDef& td, std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& rows, std::string& err) const;
    bool crack(const TDef& td, const uint8_t* r, uint32_t n, ERow& out) const;
    std::vector<uint8_t> encode(const TDef& td, const ERow& row) const;
    bool index_key(const TDef& td, const RIdx& ix, const ERow& row, std::vector<uint8_t>& key) const;
    void index_pages(uint32_t root, std::vector<uint32_t>& out) const;
    bool old_entries(uint32_t root, std::vector<std::vector<uint8_t>>& ents) const;
    bool write_map(uint32_t& ptr, const std::vector<uint32_t>& pages, std::string& err);
    uint32_t new_map_row(const std::vector<uint8_t>& row);
    bool catalog(std::string& err);

    std::vector<uint8_t> data_;     // decrypted pages (page 0 header de-obfuscated region left as is)
    uint32_t ps_ = 4096;
    uint32_t dbkey_ = 0;
    std::map<std::string, uint32_t> cat_;          // upper-case name -> tdef page
    std::map<std::string, std::string> cat_name_;  // upper-case -> real name
    uint32_t map_page_ = 0;                         // page currently receiving new usage-map rows
};

} // namespace pcbp::jet
