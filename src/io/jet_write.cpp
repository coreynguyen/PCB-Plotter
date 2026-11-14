// jet_write.cpp — Jet4 table editor (see jet_write.h for the write strategy).
#include "jet_write.h"
#include "../core/fileio.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace pcbp::jet {

namespace {

inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline void wr16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
inline void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

void rc4(const uint8_t* key, size_t klen, uint8_t* data, size_t n) {
    uint8_t s[256];
    for (int i = 0; i < 256; ++i) s[i] = (uint8_t)i;
    uint8_t j = 0;
    for (int i = 0; i < 256; ++i) { j = (uint8_t)(j + s[i] + key[i % klen]); std::swap(s[i], s[j]); }
    uint8_t a = 0, b = 0;
    for (size_t k = 0; k < n; ++k) {
        a = (uint8_t)(a + 1); b = (uint8_t)(b + s[a]); std::swap(s[a], s[b]);
        data[k] ^= s[(uint8_t)(s[a] + s[b])];
    }
}
void page_crypt(uint8_t* p, size_t n, uint32_t dbkey, uint32_t pg) {
    uint32_t k = dbkey ^ pg;
    uint8_t kb[4] = {(uint8_t)k, (uint8_t)(k >> 8), (uint8_t)(k >> 16), (uint8_t)(k >> 24)};
    rc4(kb, 4, p, n);
}

bool ieq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::toupper((unsigned char)a[i]) != std::toupper((unsigned char)b[i])) return false;
    return true;
}
std::string upper(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }

// UTF-8 -> code points (invalid bytes as Latin-1)
std::vector<uint32_t> cps_of(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        uint8_t c = (uint8_t)s[i];
        uint32_t cp = c; int n = 0;
        if (c >= 0xF0 && c < 0xF8) { cp = c & 7; n = 3; }
        else if (c >= 0xE0) { cp = c & 15; n = 2; }
        else if (c >= 0xC0) { cp = c & 31; n = 1; }
        bool ok = c < 0x80 || n > 0;
        if (n && i + n < s.size() + 0) {
            for (int k = 1; k <= n; ++k) {
                if (i + k >= s.size() || ((uint8_t)s[i + k] & 0xC0) != 0x80) { ok = false; break; }
                cp = (cp << 6) | ((uint8_t)s[i + k] & 0x3F);
            }
        } else if (n) ok = false;
        if (!ok) { out.push_back(c); ++i; continue; }
        out.push_back(cp);
        i += n + 1;
    }
    return out;
}

std::string decode_text4(const uint8_t* p, size_t n) {
    std::string out;
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
        bool comp = true;
        size_t i = 2;
        while (i < n) {
            if (p[i] == 0) { comp = !comp; ++i; }
            else if (comp) { utf8_append(out, p[i]); ++i; }
            else if (i + 1 < n) { utf8_append(out, rd16(p + i)); i += 2; }
            else break;
        }
    } else {
        for (size_t i = 0; i + 1 < n; i += 2) utf8_append(out, rd16(p + i));
    }
    return out;
}

// Jet4 "General" sort order, primary weights (learned from FlexLine CompoName indexes).
int prim_weight(uint32_t c) {
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c >= '0' && c <= '9') return 0x36 + 2 * (int)(c - '0');
    static const uint8_t letters[26] = {0x4a, 0x4c, 0x4d, 0x4f, 0x51, 0x53, 0x55, 0x57, 0x59, 0x5b, 0x5c, 0x5e, 0x60,
                                        0x62, 0x64, 0x66, 0x68, 0x69, 0x6b, 0x6d, 0x6f, 0x71, 0x73, 0x75, 0x76, 0x78};
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    switch (c) {
    case ' ': return 0x07; case '#': return 0x0c; case '%': return 0x10; case '&': return 0x12;
    case '(': return 0x14; case ')': return 0x16; case '*': return 0x18; case ',': return 0x1a;
    case '.': return 0x1c; case '/': return 0x1e; case ':': return 0x20; case ';': return 0x22;
    case '?': return 0x24; case '[': return 0x27; case ']': return 0x2a; case '_': return 0x2b03;
    case '+': return 0x2c;
    default: return -1;
    }
}

} // namespace

// ---------------------------------------------------------------------------
bool jet4_text_key(const std::string& utf8, std::vector<uint8_t>& key) {
    key.clear();
    auto cps = cps_of(utf8);
    while (!cps.empty() && cps.back() == ' ') cps.pop_back();
    if (cps.empty()) return false;
    key.push_back(0x7F);
    std::vector<std::pair<int, uint8_t>> specials;   // (position code, kind byte)
    int n = 0;
    for (uint32_t c : cps) {
        if (c == '-') { specials.push_back({4 * n + 7, 0x82}); continue; }
        if (c == '\'') { specials.push_back({4 * n + 7, 0x80}); continue; }
        int w = prim_weight(c);
        if (w < 0) return false;
        if (w > 0xFF) { key.push_back((uint8_t)(w >> 8)); key.push_back((uint8_t)w); }
        else key.push_back((uint8_t)w);
        ++n;
    }
    if (specials.empty()) { key.push_back(0x01); key.push_back(0x00); return true; }
    key.insert(key.end(), {0x01, 0x01, 0x01, 0x01});
    for (auto& sp : specials) {
        if (sp.first > 0x7F) return false;  // encoding beyond this position not observed
        key.insert(key.end(), {0x80, (uint8_t)sp.first, 0x06, sp.second});
    }
    key.push_back(0x00);
    return true;
}

std::vector<uint8_t> jet4_encode_text(const std::string& utf8) {
    auto cps = cps_of(utf8);
    bool comp = !cps.empty();
    for (uint32_t c : cps) if (c == 0 || c > 0xFF) comp = false;
    std::vector<uint8_t> out;
    if (comp) {
        out.push_back(0xFF); out.push_back(0xFE);
        for (uint32_t c : cps) out.push_back((uint8_t)c);
    } else {
        for (uint32_t c : cps) { if (c > 0xFFFF) c = 0xFFFD; out.push_back((uint8_t)c); out.push_back((uint8_t)(c >> 8)); }
    }
    return out;
}

// ---------------------------------------------------------------------------
// ETable
// ---------------------------------------------------------------------------
int ETable::col(const std::string& n) const {
    for (size_t i = 0; i < cols.size(); ++i) if (ieq(cols[i].name, n)) return (int)i;
    return -1;
}

Value ETable::get(size_t r, int c) const {
    Value v;
    if (c < 0 || (size_t)c >= cols.size() || r >= rows.size()) return v;
    const ECol& col = cols[(size_t)c];
    const Cell& cell = rows[r].c[(size_t)c];
    v.type = col.type;
    if (col.type == ColType::Bool) { v.null = false; v.i = cell.present ? 1 : 0; return v; }
    if (!cell.present) return v;
    const auto& b = cell.b;
    v.null = false;
    switch (col.type) {
    case ColType::Byte: if (b.size() >= 1) v.i = b[0]; v.d = (double)v.i; break;
    case ColType::Int: if (b.size() >= 2) v.i = (int16_t)rd16(b.data()); v.d = (double)v.i; break;
    case ColType::Long: if (b.size() >= 4) v.i = (int32_t)rd32(b.data()); v.d = (double)v.i; break;
    case ColType::Float: if (b.size() >= 4) { float f; std::memcpy(&f, b.data(), 4); v.d = f; v.i = (int64_t)f; } break;
    case ColType::Double: case ColType::DateTime:
        if (b.size() >= 8) { std::memcpy(&v.d, b.data(), 8); v.i = (int64_t)v.d; }
        if (col.type == ColType::DateTime) v.s = format_jet_date(v.d);
        break;
    case ColType::Money: if (b.size() >= 8) { int64_t m; std::memcpy(&m, b.data(), 8); v.d = (double)m / 10000.0; v.i = m / 10000; } break;
    case ColType::Text: v.s = decode_text4(b.data(), b.size()); break;
    default: v.bin = b; break;
    }
    return v;
}

void ETable::set_null(size_t r, int c) {
    if (c < 0 || r >= rows.size()) return;
    rows[r].c[(size_t)c].present = false;
    rows[r].c[(size_t)c].b.clear();
    rows[r].dirty = true;
}

bool ETable::set(size_t r, int c, const Value& v, std::string* err) {
    if (c < 0 || (size_t)c >= cols.size() || r >= rows.size()) { if (err) *err = "bad cell"; return false; }
    const ECol& col = cols[(size_t)c];
    Cell& cell = rows[r].c[(size_t)c];
    rows[r].dirty = true;
    if (v.null && col.type != ColType::Bool) { cell.present = false; cell.b.clear(); return true; }
    cell.present = true;
    cell.b.clear();
    auto le = [&](int64_t x, int n) { for (int k = 0; k < n; ++k) cell.b.push_back((uint8_t)(x >> (8 * k))); };
    switch (col.type) {
    case ColType::Bool: cell.present = !v.null && v.i != 0; break;
    case ColType::Byte: le(v.i, 1); break;
    case ColType::Int: le(v.i, 2); break;
    case ColType::Long: le(v.i, 4); break;
    case ColType::Float: { float f = (float)v.d; cell.b.resize(4); std::memcpy(cell.b.data(), &f, 4); break; }
    case ColType::Double: case ColType::DateTime: { double d = v.d; cell.b.resize(8); std::memcpy(cell.b.data(), &d, 8); break; }
    case ColType::Money: le((int64_t)(v.d * 10000.0 + (v.d >= 0 ? 0.5 : -0.5)), 8); break;
    case ColType::Text: {
        auto cps = cps_of(v.s);
        if (col.size > 0 && (int)cps.size() * 2 > col.size) {
            if (err) *err = col.name + ": text longer than " + std::to_string(col.size / 2) + " characters";
            cell.present = false;
            return false;
        }
        cell.b = jet4_encode_text(v.s);
        break;
    }
    case ColType::OLE: case ColType::Memo: {
        std::vector<uint8_t> data = v.bin;
        if (col.type == ColType::Memo) data = jet4_encode_text(v.s);
        if (data.size() > 1024) { if (err) *err = col.name + ": long values > 1 KiB not supported"; cell.present = false; return false; }
        le((int64_t)(data.size() | 0x80000000u), 4);
        le(0, 4); le(0, 4);
        cell.b.insert(cell.b.end(), data.begin(), data.end());
        break;
    }
    default:
        cell.b = v.bin;
        break;
    }
    return true;
}

size_t ETable::add_row() {
    ERow r;
    r.c.resize(cols.size());
    for (size_t k = 0; k < cols.size(); ++k)
        if (cols[k].fixed && cols[k].type != ColType::Bool) r.c[k].b.assign((size_t)std::max(0, cols[k].size), 0);
    rows.push_back(std::move(r));
    return rows.size() - 1;
}

// ---------------------------------------------------------------------------
// Editor: file level
// ---------------------------------------------------------------------------
bool Editor::open(const std::string& path, std::string& err) {
    std::vector<uint8_t> buf;
    if (!read_file(path, buf, err)) return false;
    return open_memory(std::move(buf), err);
}

bool Editor::open_memory(std::vector<uint8_t> f, std::string& err) {
    data_.clear(); cat_.clear(); cat_name_.clear(); map_page_ = 0; stats = {};
    if (f.size() < 4096 * 3 || std::memcmp(&f[4], "Standard Jet DB", 15) != 0) { err = "not a Jet database"; return false; }
    if (f[0x14] != 1) { err = "only Jet4 (Access 2000-2003) databases can be written"; return false; }
    if (f.size() % 4096) { err = "file size is not a multiple of the 4 KiB page size"; return false; }
    ps_ = 4096;
    uint8_t hdr[128];
    std::memcpy(hdr, &f[0x18], 128);
    static const uint8_t hkey[4] = {0xC7, 0xDA, 0x39, 0x6B};
    rc4(hkey, 4, hdr, 128);
    dbkey_ = rd32(hdr + (0x3E - 0x18));
    uint32_t n = (uint32_t)(f.size() / ps_);
    if (dbkey_)
        for (uint32_t p = 1; p < n; ++p) page_crypt(&f[(size_t)p * ps_], ps_, dbkey_, p);
    data_ = std::move(f);
    return catalog(err);
}

std::vector<uint8_t> Editor::image() const {
    std::vector<uint8_t> out = data_;
    uint32_t n = (uint32_t)(out.size() / ps_);
    if (dbkey_)
        for (uint32_t p = 1; p < n; ++p) page_crypt(&out[(size_t)p * ps_], ps_, dbkey_, p);
    return out;
}

bool Editor::save(const std::string& path, std::string& err) const {
    auto img = image();
    return write_file(path, img.data(), img.size(), err);
}

uint32_t Editor::append_page() {
    data_.resize(data_.size() + ps_, 0);
    ++stats.pages_appended;
    return num_pages() - 1;
}

bool Editor::row_at(uint32_t p, uint32_t row, const uint8_t*& d, uint32_t& n, uint16_t& flags) const {
    if (p == 0 || p >= num_pages()) return false;
    const uint8_t* q = pg(p);
    if (q[0] != 0x01) return false;
    uint32_t nr = rd16(q + 12);
    if (row >= nr) return false;
    uint16_t o = rd16(q + 14 + 2 * row);
    flags = o & 0xE000;
    uint32_t st = o & 0x1FFF;
    uint32_t end = row == 0 ? ps_ : (rd16(q + 14 + 2 * (row - 1)) & 0x1FFF);
    if (end > ps_ || st >= end || st < 14 + 2 * nr) return false;
    d = q + st; n = end - st;
    return true;
}

bool Editor::map_pages(uint32_t ptr, std::vector<uint32_t>& out) const {
    out.clear();
    const uint8_t* m; uint32_t n; uint16_t fl;
    if (!row_at(ptr >> 8, ptr & 0xFF, m, n, fl) || n < 1) return false;
    if (m[0] == 0) {
        if (n < 5) return false;
        uint32_t start = rd32(m + 1);
        for (uint32_t i = 5; i < n; ++i)
            for (int b = 0; b < 8; ++b)
                if (m[i] & (1u << b)) out.push_back(start + (i - 5) * 8 + b);
    } else if (m[0] == 1) {
        uint32_t per = (ps_ - 4) * 8;
        for (uint32_t k = 0; 1 + 4 * (k + 1) <= n; ++k) {
            uint32_t mp = rd32(m + 1 + 4 * k);
            if (!mp || mp >= num_pages()) continue;
            const uint8_t* bp = pg(mp);
            for (uint32_t i = 4; i < ps_; ++i)
                for (int b = 0; b < 8; ++b)
                    if (bp[i] & (1u << b)) out.push_back(k * per + (i - 4) * 8 + b);
        }
    } else return false;
    out.erase(std::remove_if(out.begin(), out.end(), [&](uint32_t p) { return p >= num_pages(); }), out.end());
    return true;
}

bool Editor::load_tdef(uint32_t p, TDef& td, std::string& err) const {
    td = TDef{};
    if (p == 0 || p >= num_pages() || pg(p)[0] != 0x02) { err = "bad tdef page " + std::to_string(p); return false; }
    td.pg = p;
    td.chain.push_back(p);
    td.buf.assign(pg(p), pg(p) + ps_);
    uint32_t nx = rd32(pg(p) + 4);
    while (nx) {
        if (nx >= num_pages() || pg(nx)[0] != 0x02 || td.chain.size() > 64) { err = "bad tdef chain"; return false; }
        td.chain.push_back(nx);
        td.buf.insert(td.buf.end(), pg(nx) + 8, pg(nx) + ps_);
        nx = rd32(pg(nx) + 4);
    }
    const uint8_t* b = td.buf.data();
    td.num_var = rd16(b + 43);
    td.num_cols = rd16(b + 45);
    uint32_t nidx = rd32(b + 47);
    uint32_t nridx = rd32(b + 51);
    td.used_ptr = rd32(b + 55);
    td.free_ptr = rd32(b + 59);
    if (nridx > 500 || nidx > 1000 || td.num_cols > 4096) { err = "implausible tdef"; return false; }
    size_t off = 63;
    std::vector<size_t> cnt_offs;
    for (uint32_t k = 0; k < nridx; ++k) { cnt_offs.push_back(off); off += 12; }
    size_t coloff = off;
    off += (size_t)td.num_cols * 25;
    if (off > td.buf.size()) { err = "tdef truncated"; return false; }
    std::vector<ECol> cols(td.num_cols);
    for (uint32_t k = 0; k < td.num_cols; ++k) {
        const uint8_t* c = b + coloff + 25 * k;
        ECol& e = cols[k];
        e.type = (ColType)c[0];
        e.col_num = rd16(c + 5);
        e.var_index = rd16(c + 7);
        e.flags = c[15];
        e.fixed_offset = rd16(c + 21);
        e.size = rd16(c + 23);
        e.fixed = (e.flags & 1) != 0;
    }
    for (uint32_t k = 0; k < td.num_cols; ++k) {
        if (off + 2 > td.buf.size()) { err = "tdef truncated (names)"; return false; }
        size_t len = rd16(b + off); off += 2;
        if (off + len > td.buf.size()) { err = "tdef truncated (names)"; return false; }
        std::string nm;
        for (size_t i = 0; i + 1 < len; i += 2) utf8_append(nm, rd16(b + off + i));
        cols[k].name = nm;
        off += len;
    }
    for (uint32_t k = 0; k < nridx; ++k) {
        if (off + 52 > td.buf.size()) { err = "tdef truncated (indexes)"; return false; }
        RIdx ix;
        ix.def_off = off;
        ix.cnt_off = cnt_offs[k];
        for (int j = 0; j < 10; ++j) {
            uint16_t cn = rd16(b + off + 4 + 3 * j);
            if (cn == 0xFFFF) continue;
            ix.cols.push_back({cn, b[off + 6 + 3 * j] != 2});
        }
        ix.umap = rd32(b + off + 34);
        ix.first_dp = rd32(b + off + 38);
        td.ridx.push_back(ix);
        off += 52;
    }
    std::stable_sort(cols.begin(), cols.end(), [](const ECol& a, const ECol& c) { return a.col_num < c.col_num; });
    td.cols = std::move(cols);
    return true;
}

void Editor::put_tdef(const TDef& td, size_t off, const uint8_t* src, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        size_t o = off + i;
        uint32_t page; size_t po;
        if (o < ps_) { page = td.chain[0]; po = o; }
        else { size_t k = (o - ps_) / (ps_ - 8) + 1; page = td.chain[k]; po = 8 + (o - ps_) % (ps_ - 8); }
        pg(page)[po] = src[i];
    }
}

bool Editor::crack(const TDef& td, const uint8_t* r, uint32_t sz, ERow& out) const {
    out.c.assign(td.cols.size(), Cell{});
    if (sz < 3) return false;
    uint32_t row_cols = rd16(r);
    uint32_t bm = (row_cols + 7) / 8;
    if (bm + 2 > sz) return false;
    const uint8_t* mask = r + sz - bm;
    auto present = [&](int cn) { return cn >= 0 && (uint32_t)cn < row_cols && (mask[cn / 8] & (1u << (cn % 8))) != 0; };
    bool has_var = false;
    for (auto& c : td.cols) if (!c.fixed) has_var = true;
    uint32_t var_cnt = 0;
    std::vector<uint32_t> voff;
    uint32_t var_end = sz - bm;
    if (has_var) {
        if (sz < bm + 4) return false;
        var_cnt = rd16(r + sz - bm - 2);
        uint64_t tbl = 2ull + 2ull * (var_cnt + 1);
        if (tbl + bm + 2 > sz) return false;
        voff.resize(var_cnt + 1);
        for (uint32_t i = 0; i <= var_cnt; ++i) voff[i] = rd16(r + sz - bm - 2 - 2 * (i + 1));
        var_end = (uint32_t)(sz - bm - tbl);
    }
    out.ncols = (uint16_t)row_cols;
    out.nvar = (uint16_t)var_cnt;
    out.fixed_len = (uint16_t)((has_var && !voff.empty() ? voff[0] : var_end) - 2);
    if (2u + out.fixed_len <= sz) out.raw_fixed.assign(r + 2, r + 2 + out.fixed_len);
    out.raw_mask.assign(mask, mask + bm);
    uint32_t row_fixed = row_cols >= var_cnt ? row_cols - var_cnt : 0;
    uint32_t seen = 0;
    for (size_t k = 0; k < td.cols.size(); ++k) {
        const ECol& c = td.cols[k];
        Cell& cell = out.c[k];
        if (c.type == ColType::Bool) { if (c.fixed) ++seen; cell.present = present(c.col_num); continue; }
        if (c.fixed) {
            bool in_row = seen < row_fixed;
            ++seen;
            uint64_t st = 2ull + (uint64_t)c.fixed_offset;
            if (in_row && st + (uint64_t)c.size <= var_end) cell.b.assign(r + st, r + st + c.size);
            else cell.b.assign((size_t)std::max(0, c.size), 0);
            cell.present = in_row && present(c.col_num);
        } else {
            if (!present(c.col_num) || c.var_index >= (int)var_cnt) continue;
            uint32_t a = voff[(size_t)c.var_index], e = voff[(size_t)c.var_index + 1];
            if (a > e || e > var_end) continue;
            cell.present = true;
            cell.b.assign(r + a, r + e);
        }
    }
    return true;
}

std::vector<uint8_t> Editor::encode(const TDef& td, const ERow& row) const {
    // a new row spans every column number (tables with deleted columns have gaps: Jet writes
    // max(col_num)+1 columns and keeps the gap bits) and every variable-column slot
    uint32_t max_cn = 0, max_vi = 0;
    bool has_var = false;
    for (auto& c : td.cols) {
        max_cn = std::max<uint32_t>(max_cn, (uint32_t)c.col_num + 1);
        if (!c.fixed) { has_var = true; max_vi = std::max<uint32_t>(max_vi, (uint32_t)c.var_index + 1); }
    }
    uint32_t ncols = row.ncols ? row.ncols : std::max<uint32_t>(td.num_cols, max_cn);
    uint32_t nvar = has_var ? (row.ncols ? row.nvar : std::max<uint32_t>(td.num_var, max_vi)) : 0;
    uint32_t row_fixed = ncols >= nvar ? ncols - nvar : 0;
    // fixed area
    uint32_t flen = 0;
    {
        uint32_t seen = 0;
        for (auto& c : td.cols) {
            if (!c.fixed) continue;
            if (seen++ >= row_fixed) break;
            if (c.type != ColType::Bool) flen = std::max(flen, (uint32_t)(c.fixed_offset + c.size));
        }
    }
    if (row.ncols && row.fixed_len >= flen) flen = row.fixed_len;
    std::vector<uint8_t> r(2 + flen, 0);
    if (row.raw_fixed.size() == flen) std::memcpy(r.data() + 2, row.raw_fixed.data(), flen);
    wr16(r.data(), ncols);
    uint32_t bm = (ncols + 7) / 8;
    std::vector<uint8_t> mask(bm, 0);
    {
        std::vector<bool> known(ncols, false);
        for (auto& c : td.cols) if (c.col_num >= 0 && (uint32_t)c.col_num < ncols) known[(size_t)c.col_num] = true;
        bool have_raw = row.raw_mask.size() == bm;
        for (uint32_t cn = 0; cn < ncols; ++cn) {
            if (known[cn]) continue;   // deleted column: keep the stored bit (Jet sets it on insert)
            bool bit = have_raw ? (row.raw_mask[cn / 8] >> (cn % 8)) & 1 : true;
            if (bit) mask[cn / 8] |= (uint8_t)(1u << (cn % 8));
        }
    }
    auto setbit = [&](int cn) { if (cn >= 0 && (uint32_t)cn < ncols) mask[cn / 8] |= (uint8_t)(1u << (cn % 8)); };
    uint32_t seen = 0;
    for (size_t k = 0; k < td.cols.size(); ++k) {
        const ECol& c = td.cols[k];
        const Cell& cell = row.c[k];
        if (c.type == ColType::Bool) { if (c.fixed) ++seen; if (cell.present) setbit(c.col_num); continue; }
        if (!c.fixed) continue;
        if (seen++ >= row_fixed) continue;
        if (cell.b.size() == (size_t)c.size) std::memcpy(&r[2 + c.fixed_offset], cell.b.data(), cell.b.size());
        if (cell.present) setbit(c.col_num);
    }
    if (has_var) {
        std::vector<const ECol*> vcols(nvar, nullptr);
        std::vector<const Cell*> vcells(nvar, nullptr);
        for (size_t k = 0; k < td.cols.size(); ++k)
            if (!td.cols[k].fixed && td.cols[k].var_index < (int)nvar) {
                vcols[(size_t)td.cols[k].var_index] = &td.cols[k];
                vcells[(size_t)td.cols[k].var_index] = &row.c[k];
            }
        std::vector<uint32_t> voff(nvar + 1);
        for (uint32_t i = 0; i < nvar; ++i) {
            voff[i] = (uint32_t)r.size();
            if (vcells[i] && vcells[i]->present) {
                r.insert(r.end(), vcells[i]->b.begin(), vcells[i]->b.end());
                setbit(vcols[i]->col_num);
            }
        }
        voff[nvar] = (uint32_t)r.size();
        for (int i = (int)nvar; i >= 0; --i) { uint8_t t[2]; wr16(t, voff[(size_t)i]); r.insert(r.end(), t, t + 2); }
        uint8_t t[2]; wr16(t, nvar); r.insert(r.end(), t, t + 2);
    }
    r.insert(r.end(), mask.begin(), mask.end());
    return r;
}

bool Editor::scan(const TDef& td, std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& rows, std::string& err) const {
    std::vector<uint32_t> pages;
    if (!map_pages(td.used_ptr, pages)) { err = "cannot read usage map"; return false; }
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    for (uint32_t p : pages) {
        const uint8_t* q = pg(p);
        if (q[0] != 0x01 || rd32(q + 4) != td.pg) continue;
        uint32_t nr = rd16(q + 12);
        for (uint32_t r = 0; r < nr; ++r) {
            const uint8_t* d; uint32_t n; uint16_t fl;
            if (!row_at(p, r, d, n, fl)) continue;
            if (fl & 0x8000) continue;
            int hops = 0;
            bool ok = true;
            while (fl & 0x4000) {
                if (n < 4 || ++hops > 8) { ok = false; break; }
                uint32_t ptr = rd32(d);
                if (!row_at(ptr >> 8, ptr & 0xFF, d, n, fl)) { ok = false; break; }
            }
            if (!ok) continue;
            rows.push_back({(p << 8) | r, std::vector<uint8_t>(d, d + n)});
        }
    }
    return true;
}

bool Editor::catalog(std::string& err) {
    TDef td;
    if (!load_tdef(2, td, err)) { err = "MSysObjects: " + err; return false; }
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> raw;
    if (!scan(td, raw, err)) return false;
    ETable t;
    t.cols = td.cols;
    int ci = t.col("Id"), cn = t.col("Name"), ct = t.col("Type");
    if (ci < 0 || cn < 0 || ct < 0) { err = "MSysObjects lacks Id/Name/Type"; return false; }
    for (auto& rr : raw) {
        ERow row;
        if (!crack(td, rr.second.data(), (uint32_t)rr.second.size(), row)) continue;
        t.rows.push_back(std::move(row));
        size_t i = t.rows.size() - 1;
        if ((t.get_i(i, ct) & 0x7FFF) != 1) continue;
        std::string name = t.get_s(i, cn);
        cat_[upper(name)] = (uint32_t)t.get_i(i, ci) & 0xFFFFFF;
        cat_name_[upper(name)] = name;
    }
    return true;
}

uint32_t Editor::autonumber(const std::string& name) const {
    auto it = cat_.find(upper(name));
    if (it == cat_.end() || it->second >= num_pages()) return 0;
    return rd32(pg(it->second) + 20);
}

bool Editor::has_table(const std::string& n) const { return cat_.count(upper(n)) != 0; }
std::vector<std::string> Editor::tables() const {
    std::vector<std::string> v;
    for (auto& kv : cat_name_) if (upper(kv.second).rfind("MSYS", 0) != 0) v.push_back(kv.second);
    return v;
}

bool Editor::load(const std::string& name, ETable& t, std::string& err) const {
    t = ETable{};
    auto it = cat_.find(upper(name));
    if (it == cat_.end()) { err = "no such table: " + name; return false; }
    TDef td;
    if (!load_tdef(it->second, td, err)) { err = name + ": " + err; return false; }
    t.name = cat_name_.at(upper(name));
    t.tdef_pg = td.pg;
    t.cols = td.cols;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> raw;
    if (!scan(td, raw, err)) { err = name + ": " + err; return false; }
    for (auto& rr : raw) {
        ERow row;
        if (!crack(td, rr.second.data(), (uint32_t)rr.second.size(), row)) continue;
        row.src_ptr = rr.first;
        row.dirty = false;
        t.rows.push_back(std::move(row));
    }
    return true;
}

// ---------------------------------------------------------------------------
// indexes
// ---------------------------------------------------------------------------
bool Editor::index_key(const TDef& td, const RIdx& ix, const ERow& row, std::vector<uint8_t>& key) const {
    key.clear();
    for (auto& ic : ix.cols) {
        int k = -1;
        for (size_t i = 0; i < td.cols.size(); ++i) if (td.cols[i].col_num == ic.first) k = (int)i;
        if (k < 0 || !ic.second) return false;   // descending keys: not supported
        const ECol& c = td.cols[(size_t)k];
        const Cell& cell = row.c[(size_t)k];
        if (c.type == ColType::Bool) { key.push_back(0x7F); key.push_back(cell.present ? 0x00 : 0xFF); continue; } // unverified
        if (!cell.present) { key.push_back(0x00); continue; }
        switch (c.type) {
        case ColType::Byte: key.push_back(0x7F); key.push_back(cell.b[0]); break;
        case ColType::Int: key.push_back(0x7F); key.push_back((uint8_t)(cell.b[1] ^ 0x80)); key.push_back(cell.b[0]); break;
        case ColType::Long:
            key.push_back(0x7F); key.push_back((uint8_t)(cell.b[3] ^ 0x80));
            key.push_back(cell.b[2]); key.push_back(cell.b[1]); key.push_back(cell.b[0]);
            break;
        case ColType::Text: {
            std::vector<uint8_t> tk;
            if (!jet4_text_key(decode_text4(cell.b.data(), cell.b.size()), tk)) return false;
            key.insert(key.end(), tk.begin(), tk.end());
            break;
        }
        default: return false;
        }
    }
    return true;
}

// Decode entries of an index page (each entry = key + 4-byte pointer [+ 4-byte child page]).
static void page_entries(const uint8_t* q, uint32_t ps, std::vector<std::vector<uint8_t>>& out) {
    uint32_t pref = rd16(q + 0x18);
    uint32_t st = 0x1E0;
    std::vector<uint8_t> first;
    size_t count = 0;
    for (uint32_t i = 0; i < 0x1E0 - 0x1B; ++i) {
        uint8_t b = q[0x1B + i];
        if (!b) continue;
        for (int k = 0; k < 8; ++k) {
            if (!(b & (1u << k))) continue;
            uint32_t end = 0x1E0 + i * 8 + k;
            if (end > ps || end <= st) continue;
            std::vector<uint8_t> e;
            if (count++ == 0) first.assign(q + st, q + end), e = first;
            else { e.assign(first.begin(), first.begin() + std::min<size_t>(pref, first.size())); e.insert(e.end(), q + st, q + end); }
            out.push_back(std::move(e));
            st = end;
        }
    }
}

void Editor::index_pages(uint32_t root, std::vector<uint32_t>& out) const {
    std::vector<uint32_t> stack{root};
    std::unordered_set<uint32_t> seen;
    while (!stack.empty()) {
        uint32_t p = stack.back(); stack.pop_back();
        if (!p || p >= num_pages() || !seen.insert(p).second) continue;
        const uint8_t* q = pg(p);
        if (q[0] != 0x03 && q[0] != 0x04) continue;
        out.push_back(p);
        if (q[0] == 0x03) {
            std::vector<std::vector<uint8_t>> ents;
            page_entries(q, ps_, ents);
            for (auto& e : ents) if (e.size() >= 4) stack.push_back((uint32_t)e[e.size() - 4] << 24 | (uint32_t)e[e.size() - 3] << 16 | (uint32_t)e[e.size() - 2] << 8 | e[e.size() - 1]);
            stack.push_back(rd32(q + 0x14));
        } else {
            stack.push_back(rd32(q + 0x10));
        }
    }
}

bool Editor::old_entries(uint32_t root, std::vector<std::vector<uint8_t>>& ents) const {
    ents.clear();
    uint32_t p = root;
    int guard = 0;
    while (p && p < num_pages() && pg(p)[0] == 0x03 && ++guard < 16) {
        std::vector<std::vector<uint8_t>> e;
        page_entries(pg(p), ps_, e);
        if (e.empty()) p = rd32(pg(p) + 0x14);
        else { auto& f = e[0]; p = (uint32_t)f[f.size() - 4] << 24 | (uint32_t)f[f.size() - 3] << 16 | (uint32_t)f[f.size() - 2] << 8 | f[f.size() - 1]; }
    }
    std::unordered_set<uint32_t> seen;
    while (p && p < num_pages() && pg(p)[0] == 0x04 && seen.insert(p).second) {
        page_entries(pg(p), ps_, ents);
        p = rd32(pg(p) + 0x10);
    }
    return true;
}

// ---------------------------------------------------------------------------
// usage maps
// ---------------------------------------------------------------------------
uint32_t Editor::new_map_row(const std::vector<uint8_t>& row) {
    auto room = [&](uint32_t p) {
        const uint8_t* q = pg(p);
        return (uint32_t)rd16(q + 2);
    };
    if (!map_page_ || room(map_page_) < row.size() + 2 || rd16(pg(map_page_) + 12) >= 255) {
        map_page_ = append_page();
        uint8_t* q = pg(map_page_);
        q[0] = 0x01; q[1] = 0x01;
        wr16(q + 2, ps_ - 14);
        wr32(q + 4, 0);
        wr16(q + 12, 0);
    }
    uint8_t* q = pg(map_page_);
    uint32_t nr = rd16(q + 12);
    uint32_t top = nr ? (rd16(q + 14 + 2 * (nr - 1)) & 0x1FFF) : ps_;
    uint32_t st = top - (uint32_t)row.size();
    std::memcpy(q + st, row.data(), row.size());
    wr16(q + 14 + 2 * nr, st);
    wr16(q + 12, nr + 1);
    wr16(q + 2, rd16(q + 2) - (uint32_t)row.size() - 2);
    return (map_page_ << 8) | nr;
}

bool Editor::write_map(uint32_t& ptr, const std::vector<uint32_t>& pages_in, std::string& err) {
    std::vector<uint32_t> pages = pages_in;
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    uint32_t start = pages.empty() ? 0 : (pages.front() & ~7u);
    uint32_t span = pages.empty() ? 0 : pages.back() - start + 1;
    uint32_t need = 5 + (span + 7) / 8;
    // in place?
    const uint8_t* m; uint32_t n; uint16_t fl;
    if (row_at(ptr >> 8, ptr & 0xFF, m, n, fl) && n >= 5 && n >= need) {
        uint8_t* w = pg(ptr >> 8) + (m - pg(ptr >> 8));
        std::memset(w, 0, n);
        w[0] = 0;
        wr32(w + 1, start);
        for (uint32_t p : pages) w[5 + (p - start) / 8] |= (uint8_t)(1u << ((p - start) % 8));
        return true;
    }
    uint32_t len = std::max<uint32_t>(69, ((need + 63) / 64) * 64 + 5);
    if (len > ps_ - 14 - 2 - 64) { err = "usage map too large (table spans more than ~30000 pages)"; return false; }
    std::vector<uint8_t> row(len, 0);
    wr32(&row[1], start);
    for (uint32_t p : pages) row[5 + (p - start) / 8] |= (uint8_t)(1u << ((p - start) % 8));
    ptr = new_map_row(row);
    return true;
}

// ---------------------------------------------------------------------------
// store
// ---------------------------------------------------------------------------
bool Editor::store(ETable& t, std::string& err) {
    TDef td;
    if (!load_tdef(t.tdef_pg, td, err)) return false;
    if (td.cols.size() != t.cols.size()) { err = t.name + ": column layout changed"; return false; }
    const uint32_t tp = td.pg;

    // ---- 1. old pages -> pool
    std::vector<uint32_t> used;
    if (!map_pages(td.used_ptr, used)) { err = t.name + ": cannot read usage map"; return false; }
    std::set<uint32_t> pool, keep_used;
    for (uint32_t p : used) {
        const uint8_t* q = pg(p);
        if (q[0] == 0x01 && rd32(q + 4) == tp) pool.insert(p);
        else keep_used.insert(p);
    }
    std::vector<uint32_t> roots;
    for (auto& ix : td.ridx) {
        std::vector<uint32_t> ip;
        index_pages(ix.first_dp, ip);
        std::vector<uint32_t> mp;
        if (map_pages(ix.umap, mp)) ip.insert(ip.end(), mp.begin(), mp.end());
        for (uint32_t p : ip) {
            const uint8_t* q = pg(p);
            if ((q[0] == 0x03 || q[0] == 0x04) && rd32(q + 4) == tp && p != ix.first_dp) pool.insert(p);
        }
        roots.push_back(ix.first_dp);
    }
    for (uint32_t r : roots) pool.erase(r);

    // ---- 2. long values referenced by kept rows must stay where they are
    std::map<uint32_t, std::set<uint32_t>> retained;   // page -> rows
    for (auto& row : t.rows) {
        for (size_t k = 0; k < td.cols.size(); ++k) {
            if (td.cols[k].type != ColType::OLE && td.cols[k].type != ColType::Memo) continue;
            const Cell& c = row.c[k];
            if (!c.present || c.b.size() < 12) continue;
            uint32_t hdr = rd32(c.b.data());
            if (hdr & 0x80000000u) continue;
            uint32_t ptr = rd32(c.b.data() + 4);
            int guard = 0;
            while (ptr && ++guard < 100000) {
                retained[ptr >> 8].insert(ptr & 0xFF);
                if (hdr & 0x40000000u) break;
                const uint8_t* d; uint32_t n; uint16_t fl;
                if (!row_at(ptr >> 8, ptr & 0xFF, d, n, fl) || n < 4) break;
                ptr = rd32(d);
            }
        }
    }
    for (auto& kv : retained) {
        if (!pool.count(kv.first)) continue;
        pool.erase(kv.first);
        keep_used.insert(kv.first);
        uint8_t* q = pg(kv.first);
        uint32_t nr = rd16(q + 12);
        for (uint32_t r = 0; r < nr; ++r)
            if (!kv.second.count(r)) wr16(q + 14 + 2 * r, rd16(q + 14 + 2 * r) | 0xC000);  // Jet: deleted
    }

    // ---- 3. old keys
    std::vector<std::unordered_map<uint32_t, std::vector<uint8_t>>> oldkeys(td.ridx.size());
    for (size_t i = 0; i < td.ridx.size(); ++i) {
        std::vector<std::vector<uint8_t>> ents;
        old_entries(td.ridx[i].first_dp, ents);
        for (auto& e : ents) {
            if (e.size() < 4) continue;
            uint32_t ptr = ((uint32_t)e[e.size() - 4] << 16 | (uint32_t)e[e.size() - 3] << 8 | e[e.size() - 2]) << 8 | e[e.size() - 1];
            oldkeys[i][ptr] = std::vector<uint8_t>(e.begin(), e.end() - 4);
        }
    }

    // ---- 4. autonumber + encode
    uint32_t autonum = rd32(td.buf.data() + 20);
    for (size_t k = 0; k < td.cols.size(); ++k) {
        if (!td.cols[k].autonum() || td.cols[k].type != ColType::Long) continue;
        for (auto& row : t.rows)
            if (row.c[k].present) autonum = std::max(autonum, rd32(row.c[k].b.data()));
        for (size_t r = 0; r < t.rows.size(); ++r)
            if (!t.rows[r].c[k].present) t.set_i(r, (int)k, (int64_t)++autonum);
    }
    std::vector<std::vector<uint8_t>> enc(t.rows.size());
    for (size_t r = 0; r < t.rows.size(); ++r) {
        enc[r] = encode(td, t.rows[r]);
        if (enc[r].size() > ps_ - 16) { err = t.name + ": row too large"; return false; }
    }

    std::vector<uint32_t> poolv(pool.begin(), pool.end());
    size_t pool_i = 0;
    auto take = [&]() -> uint32_t { return pool_i < poolv.size() ? poolv[pool_i++] : append_page(); };

    // ---- 5. data pages
    std::vector<uint32_t> data_pages, free_pages;
    std::vector<uint32_t> newptr(t.rows.size());
    {
        size_t r = 0;
        while (r < t.rows.size()) {
            uint32_t p = take();
            uint8_t* q = pg(p);
            std::memset(q, 0, ps_);
            q[0] = 0x01; q[1] = 0x01;
            wr32(q + 4, tp);
            uint32_t nr = 0, top = ps_;
            while (r < t.rows.size() && nr < 255) {
                uint32_t len = (uint32_t)enc[r].size();
                if (top < len || top - len < 14 + 2 * (nr + 1)) break;
                top -= len;
                std::memcpy(q + top, enc[r].data(), len);
                wr16(q + 14 + 2 * nr, top);
                newptr[r] = (p << 8) | nr;
                ++nr; ++r;
            }
            wr16(q + 12, nr);
            uint32_t freeb = top - (14 + 2 * nr);
            wr16(q + 2, freeb);
            data_pages.push_back(p);
            if (freeb >= 64) free_pages.push_back(p);
        }
    }

    // ---- 6. indexes
    std::vector<std::vector<uint32_t>> idx_pages(td.ridx.size());
    for (size_t i = 0; i < td.ridx.size(); ++i) {
        const RIdx& ix = td.ridx[i];
        struct Ent { std::vector<uint8_t> key; uint32_t ptr; };
        std::vector<Ent> ents;
        ents.reserve(t.rows.size());
        for (size_t r = 0; r < t.rows.size(); ++r) {
            Ent e; e.ptr = newptr[r];
            std::vector<uint8_t> comp;
            bool have = index_key(td, ix, t.rows[r], comp);
            auto it = t.rows[r].src_ptr ? oldkeys[i].find(t.rows[r].src_ptr) : oldkeys[i].end();
            if (!t.rows[r].dirty && it != oldkeys[i].end()) {
                e.key = it->second;
                ++stats.keys_reused;
                if (have && comp != it->second) ++stats.keys_mismatch;
            } else if (have) {
                e.key = std::move(comp);
                ++stats.keys_computed;
                if (it != oldkeys[i].end() && e.key != it->second && t.rows[r].dirty) { /* value changed */ }
            } else {
                err = t.name + ": cannot build index key for row " + std::to_string(r) +
                      " (unsupported character or column type in an indexed column)";
                return false;
            }
            ents.push_back(std::move(e));
        }
        std::sort(ents.begin(), ents.end(), [](const Ent& a, const Ent& b) {
            if (a.key != b.key) return a.key < b.key;
            return a.ptr < b.ptr;
        });
        auto full = [](const Ent& e) {
            std::vector<uint8_t> v = e.key;
            v.push_back((uint8_t)(e.ptr >> 24)); v.push_back((uint8_t)(e.ptr >> 16));
            v.push_back((uint8_t)(e.ptr >> 8)); v.push_back((uint8_t)e.ptr);
            return v;
        };
        // level 0 = leaves
        struct PageOut { uint32_t page; std::vector<uint8_t> last; };
        auto write_level = [&](std::vector<std::vector<uint8_t>>& items, bool leaf, std::vector<uint32_t>* child,
                               bool is_root_level, std::vector<PageOut>& out) -> bool {
            // pack items into pages
            const uint32_t cap = ps_ - 0x1E0;
            std::vector<std::pair<size_t, size_t>> ranges;   // [a,b)
            size_t a = 0;
            while (a < items.size() || (items.empty() && ranges.empty())) {
                if (items.empty()) { ranges.push_back({0, 0}); break; }
                size_t b = a;
                uint32_t used = 0;
                while (b < items.size()) {
                    size_t pref = 0;
                    if (leaf && b > a) {
                        auto& f = items[a]; auto& l = items[b];
                        size_t lim = std::min(f.size(), l.size()) - 4;
                        while (pref < lim && f[pref] == l[pref]) ++pref;
                    }
                    uint32_t tot = (uint32_t)items[a].size();
                    for (size_t k = a + 1; k <= b; ++k) tot += (uint32_t)(items[k].size() - pref);
                    if (tot > cap) break;
                    used = tot;
                    ++b;
                }
                (void)used;
                if (b == a) { err = t.name + ": index entry too large"; return false; }
                // intermediate pages need >= 2 children per page (one entry + tail)
                ranges.push_back({a, b});
                a = b;
            }
            std::vector<uint32_t> pages(ranges.size());
            for (size_t k = 0; k < ranges.size(); ++k)
                pages[k] = (is_root_level && ranges.size() == 1) ? td.ridx[i].first_dp : take();
            for (size_t k = 0; k < ranges.size(); ++k) {
                uint8_t* q = pg(pages[k]);
                std::memset(q, 0, ps_);
                q[0] = leaf ? 0x04 : 0x03; q[1] = 0x01;
                wr32(q + 4, tp);
                wr32(q + 0x0C, k ? pages[k - 1] : 0);
                wr32(q + 0x10, k + 1 < ranges.size() ? pages[k + 1] : 0);
                size_t ra = ranges[k].first, rb = ranges[k].second;
                std::vector<const std::vector<uint8_t>*> list;
                uint32_t tail = 0;
                for (size_t m = ra; m < rb; ++m) list.push_back(&items[m]);
                if (!leaf && !list.empty()) {           // last child -> tail page
                    tail = (*child)[rb - 1];
                    list.pop_back();
                }
                wr32(q + 0x14, tail);
                size_t pref = 0;
                if (leaf && list.size() > 1) {
                    auto& f = *list.front(); auto& l = *list.back();
                    size_t lim = std::min(f.size(), l.size()) - 4;
                    while (pref < lim && f[pref] == l[pref]) ++pref;
                }
                wr16(q + 0x18, (uint32_t)pref);
                uint32_t off = 0x1E0;
                for (size_t m = 0; m < list.size(); ++m) {
                    const auto& e = *list[m];
                    size_t skip = m ? pref : 0;
                    std::memcpy(q + off, e.data() + skip, e.size() - skip);
                    off += (uint32_t)(e.size() - skip);
                    uint32_t bit = off - 0x1E0;
                    q[0x1B + bit / 8] |= (uint8_t)(1u << (bit % 8));
                }
                wr16(q + 2, ps_ - off);
                idx_pages[i].push_back(pages[k]);
                out.push_back({pages[k], rb > ra ? items[rb - 1] : std::vector<uint8_t>()});
            }
            return true;
        };
        std::vector<std::vector<uint8_t>> items;
        items.reserve(ents.size());
        for (auto& e : ents) items.push_back(full(e));
        std::vector<PageOut> level;
        // decide whether the leaf level is also the root (single page)
        {
            std::vector<PageOut> tmp;
            // pack leaves; if single page it becomes the root page
            if (!write_level(items, true, nullptr, true, tmp)) return false;
            level = tmp;
        }
        while (level.size() > 1) {
            std::vector<std::vector<uint8_t>> up;
            std::vector<uint32_t> child;
            for (auto& po : level) {
                std::vector<uint8_t> e = po.last;
                e.push_back((uint8_t)(po.page >> 24)); e.push_back((uint8_t)(po.page >> 16));
                e.push_back((uint8_t)(po.page >> 8)); e.push_back((uint8_t)po.page);
                up.push_back(std::move(e));
                child.push_back(po.page);
            }
            // an intermediate page must carry at least one child before its tail
            std::vector<PageOut> next;
            if (!write_level(up, false, &child, true, next)) return false;
            // propagate the true last leaf entry of each intermediate page (strip child ptr)
            for (auto& po : next) if (po.last.size() >= 4) po.last.resize(po.last.size() - 4);
            level = next;
        }
        // root must be first_dp: if the single top page is not first_dp, move it
        if (!level.empty() && level[0].page != td.ridx[i].first_dp) {
            uint32_t src = level[0].page;
            std::memcpy(pg(td.ridx[i].first_dp), pg(src), ps_);
            std::memset(pg(src), 0, ps_);
            for (auto& p : idx_pages[i]) if (p == src) p = td.ridx[i].first_dp;
            // src page becomes free for the data pool (leftover)
            poolv.push_back(src);
        }
        // index row counter (keeps Jet's semantics: never below the entry count)
        uint32_t cnt = rd32(td.buf.data() + td.ridx[i].cnt_off + 4);
        cnt = std::max<uint32_t>(cnt, (uint32_t)ents.size());
        uint8_t cb[4]; wr32(cb, cnt);
        put_tdef(td, td.ridx[i].cnt_off + 4, cb, 4);
    }

    // ---- 7. leftover pool pages -> empty data pages of the table
    for (size_t k = pool_i; k < poolv.size(); ++k) {
        uint32_t p = poolv[k];
        uint8_t* q = pg(p);
        std::memset(q, 0, ps_);
        q[0] = 0x01; q[1] = 0x01;
        wr16(q + 2, ps_ - 16);
        wr32(q + 4, tp);
        wr16(q + 12, 1);                       // one deleted, zero-length row (what Jet leaves behind)
        wr16(q + 14, 0xC000 | ps_);
        data_pages.push_back(p);
        free_pages.push_back(p);
    }
    pool_i = poolv.size();

    // ---- 8. usage maps
    std::vector<uint32_t> umap(data_pages);
    umap.insert(umap.end(), keep_used.begin(), keep_used.end());
    uint32_t used_ptr = td.used_ptr, free_ptr = td.free_ptr;
    if (!write_map(used_ptr, umap, err)) return false;
    if (!write_map(free_ptr, free_pages, err)) return false;
    for (size_t i = 0; i < td.ridx.size(); ++i) {
        uint32_t up = td.ridx[i].umap;
        if (!write_map(up, idx_pages[i], err)) return false;
        if (up != td.ridx[i].umap) { uint8_t b[4]; wr32(b, up); put_tdef(td, td.ridx[i].def_off + 34, b, 4); }
    }
    uint8_t b4[4];
    if (used_ptr != td.used_ptr) { wr32(b4, used_ptr); put_tdef(td, 55, b4, 4); }
    if (free_ptr != td.free_ptr) { wr32(b4, free_ptr); put_tdef(td, 59, b4, 4); }
    wr32(b4, (uint32_t)t.rows.size()); put_tdef(td, 16, b4, 4);
    wr32(b4, autonum); put_tdef(td, 20, b4, 4);

    for (size_t r = 0; r < t.rows.size(); ++r) { t.rows[r].src_ptr = newptr[r]; t.rows[r].dirty = false; }
    return true;
}

// ---------------------------------------------------------------------------
// diagnostics
// ---------------------------------------------------------------------------
bool Editor::roundtrip_check(const std::string& name, size_t& rows, size_t& mism, std::string& err) const {
    rows = mism = 0;
    auto it = cat_.find(upper(name));
    if (it == cat_.end()) { err = "no such table"; return false; }
    TDef td;
    if (!load_tdef(it->second, td, err)) return false;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> raw;
    if (!scan(td, raw, err)) return false;
    for (auto& rr : raw) {
        ERow row;
        if (!crack(td, rr.second.data(), (uint32_t)rr.second.size(), row)) { ++mism; continue; }
        ++rows;
        if (encode(td, row) != rr.second) ++mism;
    }
    return true;
}

bool Editor::index_check(const std::string& name, size_t& entries, size_t& mism, std::string& err) const {
    entries = mism = 0;
    auto it = cat_.find(upper(name));
    if (it == cat_.end()) { err = "no such table"; return false; }
    TDef td;
    if (!load_tdef(it->second, td, err)) return false;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> raw;
    if (!scan(td, raw, err)) return false;
    std::vector<ERow> rows;
    for (auto& rr : raw) { ERow r; if (crack(td, rr.second.data(), (uint32_t)rr.second.size(), r)) { r.src_ptr = rr.first; rows.push_back(std::move(r)); } }
    for (auto& ix : td.ridx) {
        std::vector<std::vector<uint8_t>> old;
        old_entries(ix.first_dp, old);
        std::vector<std::vector<uint8_t>> mine;
        for (auto& r : rows) {
            std::vector<uint8_t> k;
            if (!index_key(td, ix, r, k)) { k = {0xEE}; }
            k.push_back((uint8_t)(r.src_ptr >> 24)); k.push_back((uint8_t)(r.src_ptr >> 16));
            k.push_back((uint8_t)(r.src_ptr >> 8)); k.push_back((uint8_t)r.src_ptr);
            mine.push_back(std::move(k));
        }
        std::sort(mine.begin(), mine.end());
        // compare as multisets keyed on entries that point at live rows
        std::set<std::vector<uint8_t>> oldset(old.begin(), old.end());
        for (auto& m : mine) { ++entries; if (!oldset.count(m)) ++mism; }
    }
    return true;
}

} // namespace pcbp::jet
