// ============================================================================
// jet_mdb.cpp — read-only Jet3/Jet4 .mdb reader. See jet_mdb.h.
//
// Format notes (clean-room, from public format knowledge + oracle testing):
//  * Page 0: magic 00 01 00 00, "Standard Jet DB" at 4, version byte at 0x14
//    (0 = Jet3, 1 = Jet4, >=2 = ACCDB). Bytes 0x18.. (126 B Jet3 / 128 B Jet4)
//    are RC4-obfuscated with the fixed key C7 DA 39 6B. Decoded: code page
//    u16 @0x3C, database key u32 @0x3E (non-zero => every page > 0 is RC4'd
//    with key = LE32(dbkey ^ page_number)).
//  * Data page (type 0x01): tdef page u32 @4; row count u16 @8 (Jet3) / @12
//    (Jet4) followed by u16 row offsets. Offset bits: 0x1FFF position,
//    0x4000 = row holds a 4-byte pointer (row byte, 24-bit page) to the
//    real row, 0x8000 = not a primary row (deleted, or overflow target).
//    Row i spans [off(i), off(i-1)) with off(-1) = page size.
//  * Table definition (type 0x02): chained via u32 @4; continuation pages
//    contribute bytes from offset 8. Column entries 18 B (Jet3) / 25 B (Jet4)
//    after the real-index entries (8 / 12 B), then the column names.
//  * Usage map: a row (pointer in tdef) whose first byte is the map type:
//    0 = inline bitmap with u32 start page, 1 = list of bitmap pages (type
//    0x05, bitmap from offset 4).
//  * Row: column count (u8 Jet3 / u16 Jet4), fixed area, variable data,
//    variable offset table (reverse order; Jet3 1-byte + jump table, Jet4
//    u16), variable column count, null mask (bit set = present). Bool values
//    live in the null mask.
//  * Long values (MEMO/OLE): 12-byte header: u32 length | flags (0x80000000
//    inline, 0x40000000 single LVAL row, 0 = chained rows whose first 4 bytes
//    point to the next row), u32 row/page pointer, u32 unused.
// ============================================================================
#include "jet_mdb.h"

#include "../core/fileio.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <utility>

namespace pcbp::jet {

// ---------------------------------------------------------------------------
// little helpers
// ---------------------------------------------------------------------------
namespace {

inline uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t rd64(const uint8_t* p) {
    return static_cast<uint64_t>(rd32(p)) | (static_cast<uint64_t>(rd32(p + 4)) << 32);
}
inline double rd_double(const uint8_t* p) {
    uint64_t u = rd64(p);
    double d;
    std::memcpy(&d, &u, 8);
    return d;
}
inline float rd_float(const uint8_t* p) {
    uint32_t u = rd32(p);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

void rc4(const uint8_t* key, size_t klen, uint8_t* data, size_t n) {
    uint8_t S[256];
    for (int i = 0; i < 256; ++i) S[i] = static_cast<uint8_t>(i);
    uint8_t j = 0;
    for (int i = 0; i < 256; ++i) {
        j = static_cast<uint8_t>(j + S[i] + key[static_cast<size_t>(i) % klen]);
        std::swap(S[i], S[j]);
    }
    uint8_t a = 0, b = 0;
    for (size_t k = 0; k < n; ++k) {
        a = static_cast<uint8_t>(a + 1);
        b = static_cast<uint8_t>(b + S[a]);
        std::swap(S[a], S[b]);
        data[k] ^= S[static_cast<uint8_t>(S[a] + S[b])];
    }
}

// Windows-1252 0x80..0x9F -> Unicode (0 = undefined, mapped to U+FFFD).
const uint16_t kCp1252Hi[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};

void append_ucs2(std::string& out, const uint8_t* p, size_t nchars) {
    for (size_t i = 0; i < nchars; ++i) {
        uint32_t c = rd16(p + 2 * i);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < nchars) {
            uint32_t lo = rd16(p + 2 * (i + 1));
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        utf8_append(out, c);
    }
}

std::string fmt_double(double d) {
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d < 0 ? "-Inf" : "Inf";
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof(buf), d);
    return std::string(buf, r.ptr);
}
std::string fmt_float(float f) {
    if (std::isnan(f)) return "NaN";
    if (std::isinf(f)) return f < 0 ? "-Inf" : "Inf";
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof(buf), f);
    return std::string(buf, r.ptr);
}

std::string to_hex(const std::vector<uint8_t>& b) {
    static const char* hx = "0123456789ABCDEF";
    std::string s;
    s.resize(b.size() * 2);
    for (size_t i = 0; i < b.size(); ++i) {
        s[2 * i] = hx[b[i] >> 4];
        s[2 * i + 1] = hx[b[i] & 15];
    }
    return s;
}

bool ieq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char x = static_cast<unsigned char>(a[i]), y = static_cast<unsigned char>(b[i]);
        if (x >= 'A' && x <= 'Z') x = static_cast<unsigned char>(x + 32);
        if (y >= 'A' && y <= 'Z') y = static_cast<unsigned char>(y + 32);
        if (x != y) return false;
    }
    return true;
}

// Days since 1970-01-01 -> civil date (proleptic Gregorian).
void civil_from_days(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) ++y;
}

// 128-bit unsigned (hi, lo) to decimal string.
std::string u128_to_dec(uint64_t hi, uint64_t lo) {
    if (hi == 0 && lo == 0) return "0";
    uint32_t w[4] = {static_cast<uint32_t>(hi >> 32), static_cast<uint32_t>(hi),
                     static_cast<uint32_t>(lo >> 32), static_cast<uint32_t>(lo)};
    std::string digits;
    for (int guard = 0; guard < 64; ++guard) {
        bool zero = true;
        uint64_t rem = 0;
        for (int k = 0; k < 4; ++k) {
            uint64_t cur = (rem << 32) | w[k];
            w[k] = static_cast<uint32_t>(cur / 10);
            rem = cur % 10;
            if (w[k]) zero = false;
        }
        digits.push_back(static_cast<char>('0' + rem));
        if (zero) break;
    }
    std::reverse(digits.begin(), digits.end());
    return digits;
}

} // namespace

// ---------------------------------------------------------------------------
// Value / Table
// ---------------------------------------------------------------------------
std::string format_jet_date(double days) {
    if (!std::isfinite(days) || std::fabs(days) > 3.0e6) return "";
    double whole = std::trunc(days);
    double frac = std::fabs(days - whole);
    int64_t day = static_cast<int64_t>(whole);
    int64_t secs = static_cast<int64_t>(std::floor(frac * 86400.0 + 0.5));
    if (secs >= 86400) {
        secs -= 86400;
        day += (days < 0) ? -1 : 1;
    }
    int64_t y;
    unsigned m, d;
    civil_from_days(day - 25569, y, m, d);  // 1899-12-30 is day -25569 from 1970-01-01
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02u %02d:%02d:%02d", static_cast<long long>(y), m, d,
                  static_cast<int>(secs / 3600), static_cast<int>((secs / 60) % 60), static_cast<int>(secs % 60));
    return buf;
}

double Value::as_double() const {
    if (null) return 0.0;
    switch (type) {
    case ColType::Bool: case ColType::Byte: case ColType::Int: case ColType::Long:
        return static_cast<double>(i);
    case ColType::Money: case ColType::Float: case ColType::Double: case ColType::DateTime:
    case ColType::Numeric:
        return d;
    case ColType::Text: case ColType::Memo:
        return std::strtod(s.c_str(), nullptr);
    default:
        return 0.0;
    }
}

int64_t Value::as_int() const {
    if (null) return 0;
    switch (type) {
    case ColType::Bool: case ColType::Byte: case ColType::Int: case ColType::Long: case ColType::Money:
        return i;
    case ColType::Float: case ColType::Double: case ColType::DateTime: case ColType::Numeric:
        if (!std::isfinite(d) || std::fabs(d) > 9.2e18) return 0;
        return static_cast<int64_t>(std::llround(d));
    case ColType::Text: case ColType::Memo:
        return std::strtoll(s.c_str(), nullptr, 10);
    default:
        return 0;
    }
}

bool Value::as_bool() const {
    if (null) return false;
    switch (type) {
    case ColType::Text: case ColType::Memo:
        return ieq(s, "true") || ieq(s, "yes") || (std::strtoll(s.c_str(), nullptr, 10) != 0);
    case ColType::Float: case ColType::Double: case ColType::DateTime: case ColType::Numeric:
    case ColType::Money:
        return d != 0.0;
    case ColType::Binary: case ColType::OLE: case ColType::GUID:
        return !bin.empty();
    default:
        return i != 0;
    }
}

std::string Value::as_string() const {
    if (null) return "";
    switch (type) {
    case ColType::Bool: return i ? "1" : "0";
    case ColType::Byte: case ColType::Int: case ColType::Long: return std::to_string(i);
    case ColType::Money: return fmt_double(d);
    case ColType::Float: return fmt_float(static_cast<float>(d));
    case ColType::Double: return fmt_double(d);
    case ColType::DateTime: case ColType::Text: case ColType::Memo: case ColType::GUID:
    case ColType::Numeric:
        return s;
    case ColType::Binary: case ColType::OLE: case ColType::Complex: return to_hex(bin);
    }
    return s.empty() ? to_hex(bin) : s;  // unknown type ids (e.g. 17): raw bytes as hex
}

int Table::col(const std::string& n) const {
    for (size_t k = 0; k < cols.size(); ++k)
        if (ieq(cols[k].name, n)) return static_cast<int>(k);
    return -1;
}

const Value* Table::get(size_t row, const std::string& c) const {
    int k = col(c);
    if (k < 0 || row >= rows.size()) return nullptr;
    const auto& r = rows[row];
    if (static_cast<size_t>(k) >= r.size()) return nullptr;
    return &r[static_cast<size_t>(k)];
}

// ---------------------------------------------------------------------------
// Database: open / pages
// ---------------------------------------------------------------------------
bool Database::open(const std::string& utf8_path, std::string& err) {
    std::vector<uint8_t> buf;
    if (!read_file(utf8_path, buf, err)) return false;
    return open_memory(std::move(buf), err);
}

bool Database::open_memory(std::vector<uint8_t> data, std::string& err) {
    data_.clear();
    catalog_.clear();
    ver_ = 0;
    page_size_ = num_pages_ = 0;
    code_page_ = 0;
    if (data.size() < 0x18 + 128) { err = "file too small to be a Jet database"; return false; }
    if (!(data[0] == 0 && data[1] == 1 && data[2] == 0 && data[3] == 0)) {
        err = "not a Jet database (bad magic)";
        return false;
    }
    bool jet_sig = std::memcmp(&data[4], "Standard Jet DB", 15) == 0;
    bool ace_sig = std::memcmp(&data[4], "Standard ACE DB", 15) == 0;
    if (!jet_sig && !ace_sig) { err = "not a Jet database (bad signature)"; return false; }
    uint8_t v = data[0x14];
    if (ace_sig || v >= 2) {
        err = "Access 2007+ ACCDB format (version id " + std::to_string(v) + ") is not supported";
        return false;
    }
    int ver = (v == 0) ? 3 : 4;
    uint32_t ps = (ver == 3) ? 2048u : 4096u;
    if (data.size() < ps * 3) { err = "Jet database truncated"; return false; }

    // De-obfuscate the header region (fixed RC4 key).
    static const uint8_t hkey[4] = {0xC7, 0xDA, 0x39, 0x6B};
    rc4(hkey, 4, &data[0x18], ver == 3 ? 126 : 128);
    code_page_ = rd16(&data[0x3C]);
    uint32_t dbkey = rd32(&data[0x3E]);

    num_pages_ = static_cast<uint32_t>(data.size() / ps);
    if (dbkey != 0) {
        for (uint32_t pg = 1; pg < num_pages_; ++pg) {
            uint32_t k = dbkey ^ pg;
            uint8_t kb[4] = {static_cast<uint8_t>(k), static_cast<uint8_t>(k >> 8), static_cast<uint8_t>(k >> 16),
                             static_cast<uint8_t>(k >> 24)};
            rc4(kb, 4, &data[static_cast<size_t>(pg) * ps], ps);
        }
    }
    data_ = std::move(data);
    page_size_ = ps;
    ver_ = ver;
    if (!load_catalog(err)) {
        data_.clear();
        ver_ = 0;
        num_pages_ = page_size_ = 0;
        return false;
    }
    return true;
}

int Database::version() const { return ver_; }

const uint8_t* Database::page(uint32_t pg) const {
    if (pg >= num_pages_) return nullptr;
    return data_.data() + static_cast<size_t>(pg) * page_size_;
}

// Row extents inside a data/LVAL page. flags = top 3 bits of the offset word.
bool Database::row_bounds(const uint8_t* p, uint32_t row, uint32_t& start, uint32_t& size, uint16_t& flags) const {
    uint32_t cnt_off = jet4() ? 12u : 8u;
    uint32_t nrows = rd16(p + cnt_off);
    uint32_t tab = cnt_off + 2;
    if (row >= nrows || tab + 2 * (row + 1) > page_size_) return false;
    uint16_t o = rd16(p + tab + 2 * row);
    flags = static_cast<uint16_t>(o & 0xE000);
    start = o & 0x1FFFu;
    uint32_t end = (row == 0) ? page_size_ : (rd16(p + tab + 2 * (row - 1)) & 0x1FFFu);
    if (end > page_size_) end = page_size_;
    if (start < tab + 2 * nrows || start >= end) return false;
    size = end - start;
    return true;
}

bool Database::find_row(uint32_t pg, uint32_t row, const uint8_t*& data, uint32_t& size) const {
    const uint8_t* p = page(pg);
    if (!p || p[0] != 0x01) return false;
    uint32_t st, sz;
    uint16_t fl;
    if (!row_bounds(p, row, st, sz, fl)) return false;
    data = p + st;
    size = sz;
    return true;
}

// ---------------------------------------------------------------------------
// table definition
// ---------------------------------------------------------------------------
bool Database::load_tdef(uint32_t pg, TableDef& td, std::string& err) const {
    const uint8_t* p = page(pg);
    if (!p || p[0] != 0x02) { err = "page " + std::to_string(pg) + " is not a table definition"; return false; }
    std::vector<uint8_t> buf(p, p + page_size_);
    std::unordered_set<uint32_t> seen{pg};
    uint32_t next = rd32(p + 4);
    while (next) {
        if (!seen.insert(next).second || seen.size() > num_pages_) { err = "tdef chain loop"; return false; }
        const uint8_t* q = page(next);
        if (!q || q[0] != 0x02) { err = "bad tdef continuation page"; return false; }
        buf.insert(buf.end(), q + 8, q + page_size_);
        next = rd32(q + 4);
    }
    const uint8_t* b = buf.data();
    size_t n = buf.size();
    auto need = [&](size_t off, size_t len) { return off <= n && len <= n - off; };

    uint32_t num_ridx;
    size_t cols_off, entry, ridx_entry;
    uint16_t num_cols;
    if (jet4()) {
        td.num_rows = rd32(b + 16);
        num_cols = rd16(b + 45);
        num_ridx = rd32(b + 51);
        td.usage_row = b[55];
        td.usage_page = rd32(b + 55) >> 8;
        ridx_entry = 12; cols_off = 63; entry = 25;
    } else {
        td.num_rows = rd32(b + 12);
        num_cols = rd16(b + 25);
        num_ridx = rd32(b + 31);
        td.usage_row = b[35];
        td.usage_page = rd32(b + 35) >> 8;
        ridx_entry = 8; cols_off = 43; entry = 18;
    }
    if (num_ridx > 1000 || num_cols > 4096) { err = "implausible tdef header"; return false; }
    cols_off += static_cast<size_t>(num_ridx) * ridx_entry;
    size_t names_off = cols_off + static_cast<size_t>(num_cols) * entry;
    if (!need(cols_off, static_cast<size_t>(num_cols) * entry)) { err = "tdef truncated (columns)"; return false; }

    td.cols.clear();
    td.cols.reserve(num_cols);
    for (uint32_t k = 0; k < num_cols; ++k) {
        const uint8_t* c = b + cols_off + static_cast<size_t>(k) * entry;
        ColDef cd;
        cd.pub.type = static_cast<ColType>(c[0]);
        if (jet4()) {
            cd.pub.col_num = rd16(c + 5);
            cd.var_index = rd16(c + 7);
            cd.prec = c[11];
            cd.scale = c[12];
            cd.flags = c[15];
            cd.fixed_offset = rd16(c + 21);
            cd.pub.size = rd16(c + 23);
        } else {
            cd.pub.col_num = rd16(c + 1);
            cd.var_index = rd16(c + 3);
            cd.prec = c[11];
            cd.scale = c[12];
            cd.flags = c[13];
            cd.fixed_offset = rd16(c + 14);
            cd.pub.size = rd16(c + 16);
        }
        cd.pub.fixed = (cd.flags & 0x01) != 0;
        td.cols.push_back(cd);
    }
    size_t pos = names_off;
    for (auto& cd : td.cols) {
        if (jet4()) {
            if (!need(pos, 2)) { err = "tdef truncated (names)"; return false; }
            size_t len = rd16(b + pos);
            pos += 2;
            if (!need(pos, len)) { err = "tdef truncated (names)"; return false; }
            std::string nm;
            append_ucs2(nm, b + pos, len / 2);
            cd.pub.name = nm;
            pos += len;
        } else {
            if (!need(pos, 1)) { err = "tdef truncated (names)"; return false; }
            size_t len = b[pos++];
            if (!need(pos, len)) { err = "tdef truncated (names)"; return false; }
            cd.pub.name = decode_text(b + pos, static_cast<uint32_t>(len));
            pos += len;
        }
    }
    std::stable_sort(td.cols.begin(), td.cols.end(),
                     [](const ColDef& a, const ColDef& c) { return a.pub.col_num < c.pub.col_num; });
    return true;
}

// ---------------------------------------------------------------------------
// usage map
// ---------------------------------------------------------------------------
bool Database::used_pages(const TableDef& td, uint32_t tdef_pg, std::vector<uint32_t>& pages) const {
    pages.clear();
    const uint8_t* m = nullptr;
    uint32_t n = 0;
    auto accept = [&](uint64_t pg) {
        if (pg >= num_pages_) return;
        const uint8_t* p = page(static_cast<uint32_t>(pg));
        if (p[0] == 0x01 && rd32(p + 4) == tdef_pg) pages.push_back(static_cast<uint32_t>(pg));
    };
    bool ok = find_row(td.usage_page, td.usage_row, m, n) && n >= 1;
    if (ok && m[0] == 0 && n >= 5) {
        uint32_t start = rd32(m + 1);
        for (uint32_t i = 5; i < n; ++i)
            for (int bit = 0; bit < 8; ++bit)
                if (m[i] & (1u << bit)) accept(static_cast<uint64_t>(start) + (i - 5) * 8ull + bit);
    } else if (ok && m[0] == 1) {
        uint64_t per = static_cast<uint64_t>(page_size_ - 4) * 8;
        for (uint32_t k = 0; 1 + 4 * (k + 1) <= n; ++k) {
            uint32_t mp = rd32(m + 1 + 4 * k);
            if (!mp) continue;
            const uint8_t* bp = page(mp);
            if (!bp || bp[0] != 0x05) continue;
            for (uint32_t i = 4; i < page_size_; ++i) {
                if (!bp[i]) continue;
                for (int bit = 0; bit < 8; ++bit)
                    if (bp[i] & (1u << bit)) accept(k * per + (i - 4) * 8ull + bit);
            }
        }
    } else {
        // Corrupt / unknown map: fall back to scanning every data page owned by the table.
        for (uint32_t pg = 1; pg < num_pages_; ++pg) accept(pg);
    }
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    return true;
}

// ---------------------------------------------------------------------------
// text / long values
// ---------------------------------------------------------------------------
std::string Database::decode_text(const uint8_t* p, uint32_t n) const {
    std::string out;
    if (jet4()) {
        if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
            bool comp = true;
            uint32_t i = 2;
            while (i < n) {
                if (p[i] == 0) { comp = !comp; ++i; }
                else if (comp) { utf8_append(out, p[i]); ++i; }
                else if (i + 1 < n) { append_ucs2(out, p + i, 1); i += 2; }
                else break;
            }
        } else {
            append_ucs2(out, p, n / 2);
        }
    } else {
        // Jet3: single-byte code page (decoded as Windows-1252).
        for (uint32_t i = 0; i < n; ++i) {
            uint8_t c = p[i];
            uint32_t cp = c;
            if (c >= 0x80 && c <= 0x9F) cp = kCp1252Hi[c - 0x80] ? kCp1252Hi[c - 0x80] : 0xFFFD;
            utf8_append(out, cp);
        }
    }
    return out;
}

bool Database::read_lval(const uint8_t* p, uint32_t n, std::vector<uint8_t>& out) const {
    out.clear();
    if (n < 12) return false;
    uint32_t hdr = rd32(p);
    uint32_t len = hdr & 0x3FFFFFFFu;
    uint32_t ptr = rd32(p + 4);
    if (hdr & 0x80000000u) {
        uint32_t avail = n - 12;
        out.assign(p + 12, p + 12 + std::min(len, avail));
        return out.size() == len;
    }
    if (len > data_.size()) return false;  // cannot be larger than the file
    if (hdr & 0x40000000u) {
        const uint8_t* d;
        uint32_t sz;
        if (!find_row(ptr >> 8, ptr & 0xFF, d, sz)) return false;
        out.assign(d, d + std::min(len, sz));
        return out.size() == len;
    }
    // Multi-page chain.
    out.reserve(len);
    std::unordered_set<uint32_t> seen;
    while (ptr && out.size() < len) {
        if (!seen.insert(ptr).second || seen.size() > num_pages_ * 64ull) return false;
        const uint8_t* d;
        uint32_t sz;
        if (!find_row(ptr >> 8, ptr & 0xFF, d, sz) || sz < 4) return false;
        uint32_t take = std::min<uint32_t>(sz - 4, len - static_cast<uint32_t>(out.size()));
        out.insert(out.end(), d + 4, d + 4 + take);
        ptr = rd32(d);
    }
    return out.size() == len;
}

// ---------------------------------------------------------------------------
// column decoding
// ---------------------------------------------------------------------------
void Database::decode_fixed(const ColDef& c, const uint8_t* p, uint32_t n, Value& v) const {
    v.null = false;
    switch (c.pub.type) {
    case ColType::Byte:
        if (n < 1) break;
        v.i = p[0];
        return;
    case ColType::Int:
        if (n < 2) break;
        v.i = static_cast<int16_t>(rd16(p));
        return;
    case ColType::Long:
        if (n < 4) break;
        v.i = static_cast<int32_t>(rd32(p));
        return;
    case ColType::Money: {
        if (n < 8) break;
        int64_t raw = static_cast<int64_t>(rd64(p));
        v.d = static_cast<double>(raw) / 10000.0;
        v.i = (raw >= 0 ? raw + 5000 : raw - 5000) / 10000;
        return;
    }
    case ColType::Float:
        if (n < 4) break;
        v.d = rd_float(p);
        return;
    case ColType::Double:
        if (n < 8) break;
        v.d = rd_double(p);
        return;
    case ColType::DateTime:
        if (n < 8) break;
        v.d = rd_double(p);
        v.s = format_jet_date(v.d);
        return;
    case ColType::GUID: {
        if (n < 16) break;
        v.bin.assign(p, p + 16);
        char buf[48];
        std::snprintf(buf, sizeof(buf), "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", rd32(p), rd16(p + 4),
                      rd16(p + 6), p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
        v.s = buf;
        return;
    }
    case ColType::Numeric: {
        if (n < 17) break;
        // sign byte, then four little-endian dwords, most significant first.
        uint64_t hi = (static_cast<uint64_t>(rd32(p + 1)) << 32) | rd32(p + 5);
        uint64_t lo = (static_cast<uint64_t>(rd32(p + 9)) << 32) | rd32(p + 13);
        bool neg = (p[0] & 0x80) != 0;
        std::string dig = u128_to_dec(hi, lo);
        int scale = std::clamp(c.scale, 0, 38);
        if (scale > 0) {
            if (dig.size() <= static_cast<size_t>(scale)) dig.insert(0, static_cast<size_t>(scale) + 1 - dig.size(), '0');
            dig.insert(dig.size() - static_cast<size_t>(scale), ".");
        }
        v.s = (neg ? "-" : "") + dig;
        v.d = std::strtod(v.s.c_str(), nullptr);
        return;
    }
    default:
        // Unknown fixed type: expose raw bytes.
        v.bin.assign(p, p + n);
        return;
    }
    v.null = true;  // too short
}

void Database::decode_var(const ColDef& c, const uint8_t* p, uint32_t n, Value& v) const {
    v.null = false;
    switch (c.pub.type) {
    case ColType::Text:
        v.s = decode_text(p, n);
        return;
    case ColType::Memo: {
        std::vector<uint8_t> raw;
        if (!read_lval(p, n, raw)) { v.null = true; return; }
        v.s = decode_text(raw.data(), static_cast<uint32_t>(raw.size()));
        return;
    }
    case ColType::OLE:
        if (!read_lval(p, n, v.bin)) { v.bin.clear(); v.null = true; }
        return;
    case ColType::Binary:
        v.bin.assign(p, p + n);
        return;
    case ColType::Bool: case ColType::Byte: case ColType::Int: case ColType::Long: case ColType::Money:
    case ColType::Float: case ColType::Double: case ColType::DateTime: case ColType::GUID:
    case ColType::Numeric:
        // Fixed-size types can be stored in the variable area (seen in Access 2007-created
        // system tables, column flag 0x01 clear). Decode the same way.
        decode_fixed(c, p, n, v);
        return;
    default:
        v.bin.assign(p, p + n);
        return;
    }
}

bool Database::crack_row(const TableDef& td, const uint8_t* r, uint32_t sz, std::vector<Value>& out) const {
    out.assign(td.cols.size(), Value{});
    for (size_t k = 0; k < td.cols.size(); ++k) out[k].type = td.cols[k].pub.type;

    uint32_t hdr = jet4() ? 2u : 1u;
    if (sz < hdr + 1) return false;
    uint32_t row_cols = jet4() ? rd16(r) : r[0];
    uint32_t bm = (row_cols + 7) / 8;
    if (bm + hdr > sz) return false;
    const uint8_t* mask = r + sz - bm;
    auto present = [&](int cn) {
        if (cn < 0 || static_cast<uint32_t>(cn) >= row_cols) return false;
        return (mask[cn / 8] & (1u << (cn % 8))) != 0;
    };

    bool table_has_var = false;
    for (const auto& c : td.cols)
        if (!c.pub.fixed) table_has_var = true;

    // Variable-column offset table.
    uint32_t var_cnt = 0;
    std::vector<uint32_t> voff;
    uint32_t var_area_end = sz - bm;  // fixed area must end before this
    if (table_has_var) {
        if (jet4()) {
            if (sz < bm + hdr + 2) return false;
            var_cnt = rd16(r + sz - bm - 2);
            uint64_t tbl = 2ull + 2ull * (var_cnt + 1);
            if (tbl + bm + hdr > sz) return false;
            voff.resize(var_cnt + 1);
            for (uint32_t i = 0; i <= var_cnt; ++i) voff[i] = rd16(r + sz - bm - 2 - 2 * (i + 1));
            var_area_end = static_cast<uint32_t>(sz - bm - tbl);
        } else {
            if (sz < bm + hdr + 1) return false;
            var_cnt = r[sz - bm - 1];
            // Solve for the jump-table size: jumps = (end of variable data) / 256.
            uint32_t nj = 0;
            bool found = false;
            for (uint32_t cand = 0; cand <= sz / 256 + 1; ++cand) {
                int64_t ve = static_cast<int64_t>(sz) - bm - 1 - cand - (var_cnt + 1);
                if (ve < 0) break;
                if (static_cast<uint32_t>(ve / 256) == cand) { nj = cand; found = true; break; }
            }
            if (!found) {
                nj = (sz - 1) / 256;
            }
            uint64_t tbl = 1ull + nj + (var_cnt + 1);
            if (tbl + bm + hdr > sz) return false;
            const uint8_t* jumps = r + sz - bm - 2;  // jumps[-j]
            int64_t col_ptr = static_cast<int64_t>(sz) - bm - 2 - nj;
            voff.resize(var_cnt + 1);
            uint32_t used = 0;
            for (uint32_t i = 0; i <= var_cnt; ++i) {
                while (used < nj && i == *(jumps - used)) ++used;
                voff[i] = r[col_ptr - i] + 256u * used;
            }
            var_area_end = static_cast<uint32_t>(sz - bm - tbl);
        }
    }

    uint32_t row_fixed = row_cols >= var_cnt ? row_cols - var_cnt : 0;
    uint32_t fixed_seen = 0;
    for (size_t k = 0; k < td.cols.size(); ++k) {
        const ColDef& c = td.cols[k];
        Value& v = out[k];
        if (c.pub.type == ColType::Bool) {
            if (c.pub.fixed) ++fixed_seen;
            v.null = false;
            v.i = present(c.pub.col_num) ? 1 : 0;
            continue;
        }
        if (c.pub.fixed) {
            bool in_row = fixed_seen < row_fixed;
            ++fixed_seen;
            if (!in_row || !present(c.pub.col_num)) continue;
            uint64_t st = hdr + static_cast<uint64_t>(c.fixed_offset);
            uint32_t len = static_cast<uint32_t>(c.pub.size);
            if (st + len > var_area_end) continue;  // corrupt: leave null
            decode_fixed(c, r + st, len, v);
        } else {
            if (!present(c.pub.col_num)) continue;
            if (c.var_index < 0 || static_cast<uint32_t>(c.var_index) >= var_cnt) continue;
            uint32_t a = voff[static_cast<size_t>(c.var_index)], b = voff[static_cast<size_t>(c.var_index) + 1];
            if (a > b || b > var_area_end) continue;
            decode_var(c, r + a, b - a, v);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// table scan / catalog
// ---------------------------------------------------------------------------
bool Database::scan_table(uint32_t tdef_pg, const TableDef& td, std::vector<std::vector<Value>>& rows,
                          std::string& err) const {
    std::vector<uint32_t> pages;
    if (!used_pages(td, tdef_pg, pages)) { err = "cannot read usage map"; return false; }
    for (uint32_t pg : pages) {
        const uint8_t* p = page(pg);
        uint32_t nrows = rd16(p + (jet4() ? 12 : 8));
        for (uint32_t r = 0; r < nrows; ++r) {
            uint32_t st, sz;
            uint16_t fl;
            if (!row_bounds(p, r, st, sz, fl)) continue;
            if (fl & 0x8000) continue;  // deleted, or the target of an overflow pointer
            const uint8_t* rp = p + st;
            int hops = 0;
            bool ok = true;
            while (fl & 0x4000) {  // overflow pointer -> real row elsewhere
                if (sz < 4 || ++hops > 8) { ok = false; break; }
                uint32_t ptr = rd32(rp);
                const uint8_t* q = page(ptr >> 8);
                if (!q || q[0] != 0x01 || !row_bounds(q, ptr & 0xFF, st, sz, fl)) { ok = false; break; }
                rp = q + st;
            }
            if (!ok) continue;
            std::vector<Value> vals;
            if (crack_row(td, rp, sz, vals)) rows.push_back(std::move(vals));
        }
    }
    return true;
}

bool Database::load_catalog(std::string& err) {
    TableDef td;
    if (!load_tdef(2, td, err)) { err = "MSysObjects: " + err; return false; }
    std::vector<std::vector<Value>> rows;
    if (!scan_table(2, td, rows, err)) return false;
    int ci = -1, cn = -1, ct = -1, cf = -1;
    for (size_t k = 0; k < td.cols.size(); ++k) {
        const std::string& n = td.cols[k].pub.name;
        if (ieq(n, "Id")) ci = static_cast<int>(k);
        else if (ieq(n, "Name")) cn = static_cast<int>(k);
        else if (ieq(n, "Type")) ct = static_cast<int>(k);
        else if (ieq(n, "Flags")) cf = static_cast<int>(k);
    }
    if (ci < 0 || cn < 0 || ct < 0) { err = "MSysObjects lacks Id/Name/Type columns"; return false; }
    for (const auto& r : rows) {
        if ((r[static_cast<size_t>(ct)].as_int() & 0x7FFF) != 1) continue;  // 1 = local table
        CatEntry e;
        e.name = r[static_cast<size_t>(cn)].s;
        e.tdef_page = static_cast<uint32_t>(r[static_cast<size_t>(ci)].as_int()) & 0x00FFFFFFu;
        uint32_t flags = cf >= 0 ? static_cast<uint32_t>(r[static_cast<size_t>(cf)].as_int()) : 0;
        e.system = (flags & 0x80000002u) != 0 || (e.name.size() >= 4 && ieq(e.name.substr(0, 4), "MSys"));
        catalog_.push_back(std::move(e));
    }
    return true;
}

const Database::CatEntry* Database::find(const std::string& name) const {
    for (const auto& e : catalog_)
        if (ieq(e.name, name)) return &e;
    return nullptr;
}

std::vector<std::string> Database::tables() const {
    std::vector<std::string> v;
    for (const auto& e : catalog_)
        if (!e.system) v.push_back(e.name);
    return v;
}

bool Database::has_table(const std::string& name) const { return find(name) != nullptr; }

bool Database::read_table(const std::string& name, Table& out, std::string& err) const {
    out = Table{};
    if (!ver_) { err = "database not open"; return false; }
    const CatEntry* e = find(name);
    if (!e) { err = "no such table: " + name; return false; }
    TableDef td;
    if (!load_tdef(e->tdef_page, td, err)) { err = name + ": " + err; return false; }
    out.name = e->name;
    for (const auto& c : td.cols) out.cols.push_back(c.pub);
    if (!scan_table(e->tdef_page, td, out.rows, err)) { err = name + ": " + err; return false; }
    return true;
}

} // namespace pcbp::jet
