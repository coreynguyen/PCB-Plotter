#pragma once
// ============================================================================
// jet_mdb.h — dependency-free, READ-ONLY Microsoft Jet 3 / Jet 4 (.mdb) reader.
//
// CircuitCAM project files (*.cpf) and its library (cclib.mdb) are Access
// databases. This module reads them directly from the page format, without
// ODBC/ACE/DAO. Written from format knowledge (clean room); mdbtools is used
// only as an external test oracle (tests/test_jet.cpp).
//
// Supported: Jet3 (Access 97, 2 KiB pages) and Jet4 (Access 2000-2003, 4 KiB
// pages); RC4-obfuscated header; per-page RC4 when a database key is set;
// MSysObjects catalog; multi-page table definitions; usage maps type 0/1;
// overflow rows; all scalar types; Jet4 compressed-unicode text; MEMO/OLE long
// values (inline, single-page, chained multi-page).
// Not supported: ACCDB (Access 2007+, open() fails), password-encrypted
// Jet4 (MSISAM/RC4-with-password), indexes (not needed for full scans),
// Jet3 code pages other than 1252 (decoded as 1252).
//
// Value conventions:
//   Bool        -> i = 0/1, never null; as_string "1"/"0"
//   Byte/Int/Long -> i
//   Money       -> d = value (raw/10000), i = rounded value
//   Float/Double-> d
//   DateTime    -> d = days since 1899-12-30, s = "YYYY-MM-DD HH:MM:SS"
//   Text/Memo   -> s (UTF-8)
//   Binary/OLE  -> bin (raw bytes); as_string = uppercase hex
//   GUID        -> bin (16 bytes), s = "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}"
//   Numeric     -> d (approx), s = exact decimal string
// ============================================================================
#include <cstdint>
#include <string>
#include <vector>

namespace pcbp::jet {

enum class ColType : uint8_t {
    Bool = 1, Byte = 2, Int = 3, Long = 4, Money = 5, Float = 6, Double = 7, DateTime = 8,
    Binary = 9, Text = 10, OLE = 11, Memo = 12, GUID = 15, Numeric = 16, Complex = 18
};

struct Column {
    std::string name;
    ColType type = ColType::Text;
    int size = 0;
    int col_num = 0;
    bool fixed = false;
};

struct Value {
    bool null = true;
    ColType type = ColType::Text;
    int64_t i = 0;
    double d = 0;
    std::string s;
    std::vector<uint8_t> bin;
    double as_double() const;
    int64_t as_int() const;
    bool as_bool() const;
    std::string as_string() const;  // text as UTF-8; numbers formatted; DateTime as "YYYY-MM-DD HH:MM:SS"
};

struct Table {
    std::string name;
    std::vector<Column> cols;
    std::vector<std::vector<Value>> rows;
    int col(const std::string& name) const;                      // case-insensitive, -1 if missing
    const Value* get(size_t row, const std::string& col) const;  // nullptr if missing col
};

// Format a day count (days since 1899-12-30) as "YYYY-MM-DD HH:MM:SS".
std::string format_jet_date(double days);

class Database {
public:
    bool open(const std::string& utf8_path, std::string& err);
    bool open_memory(std::vector<uint8_t> data, std::string& err);
    int version() const;                      // 3 or 4 (0 if not open)
    int code_page() const { return code_page_; }
    std::vector<std::string> tables() const;  // user tables only (no MSys*)
    bool has_table(const std::string& name) const;
    bool read_table(const std::string& name, Table& out, std::string& err) const;

private:
    struct CatEntry { std::string name; uint32_t tdef_page = 0; bool system = false; };
    struct ColDef {
        Column pub;
        int var_index = 0;      // index into variable-offset table (variable columns)
        int fixed_offset = 0;   // offset inside fixed area (fixed columns)
        int scale = 0, prec = 0;
        int flags = 0;
    };
    struct TableDef {
        std::vector<ColDef> cols;  // sorted by col_num
        uint32_t usage_row = 0, usage_page = 0;
        uint32_t num_rows = 0;
    };

    bool jet4() const { return ver_ == 4; }
    const uint8_t* page(uint32_t pg) const;  // nullptr if out of range
    bool load_tdef(uint32_t pg, TableDef& td, std::string& err) const;
    bool row_bounds(const uint8_t* p, uint32_t row, uint32_t& start, uint32_t& size, uint16_t& flags) const;
    bool find_row(uint32_t pg, uint32_t row, const uint8_t*& data, uint32_t& size) const;
    bool used_pages(const TableDef& td, uint32_t tdef_pg, std::vector<uint32_t>& pages) const;
    bool crack_row(const TableDef& td, const uint8_t* r, uint32_t sz, std::vector<Value>& out) const;
    void decode_fixed(const ColDef& c, const uint8_t* p, uint32_t n, Value& v) const;
    void decode_var(const ColDef& c, const uint8_t* p, uint32_t n, Value& v) const;
    bool read_lval(const uint8_t* p, uint32_t n, std::vector<uint8_t>& out) const;
    std::string decode_text(const uint8_t* p, uint32_t n) const;
    bool scan_table(uint32_t tdef_pg, const TableDef& td, std::vector<std::vector<Value>>& rows,
                    std::string& err) const;
    bool load_catalog(std::string& err);
    const CatEntry* find(const std::string& name) const;

    std::vector<uint8_t> data_;
    uint32_t page_size_ = 0, num_pages_ = 0;
    int ver_ = 0;
    int code_page_ = 0;
    std::vector<CatEntry> catalog_;
};

} // namespace pcbp::jet
