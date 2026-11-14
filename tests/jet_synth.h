#pragma once
// ============================================================================
// jet_synth.h — R23a: builds a tiny synthetic Jet4 (.mdb) image in memory for tests (no customer data,
// no external tools). Layout: page 0 header (RC4-obfuscated block like Access writes it), page 1 empty,
// page 2 MSysObjects TDEF (Id, Type, Flags, Name), page 3 its data, then per user table one TDEF page and
// one data page. Usage maps point at page 0 (not a data page) so the reader falls back to scanning
// pages owned by the TDEF. Columns: Long / Double / Text only — enough for the preview path.
// ============================================================================
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace jet_synth {

struct Col { std::string name; int type; };   // 4 = Long, 7 = Double, 10 = Text
struct Val { bool null = false; int64_t i = 0; double d = 0; std::string s; };
struct Tbl { std::string name; std::vector<Col> cols; std::vector<std::vector<Val>> rows; };

inline void put16(std::vector<uint8_t>& b, size_t o, uint32_t v) { b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8); }
inline void put32(std::vector<uint8_t>& b, size_t o, uint32_t v) { for (int k = 0; k < 4; ++k) b[o + k] = (uint8_t)(v >> (8 * k)); }
inline void rc4(const uint8_t* key, size_t klen, uint8_t* d, size_t n) {
    uint8_t S[256]; for (int i = 0; i < 256; ++i) S[i] = (uint8_t)i;
    uint8_t j = 0; for (int i = 0; i < 256; ++i) { j = (uint8_t)(j + S[i] + key[i % klen]); std::swap(S[i], S[j]); }
    uint8_t a = 0, c = 0; for (size_t k = 0; k < n; ++k) { a++; c = (uint8_t)(c + S[a]); std::swap(S[a], S[c]); d[k] ^= S[(uint8_t)(S[a] + S[c])]; }
}
inline std::vector<uint8_t> ucs2(const std::string& s) {   // ASCII / Latin-1 test strings
    std::vector<uint8_t> o; for (unsigned char ch : s) { o.push_back(ch); o.push_back(0); } return o;
}
inline int fixed_size(int t) { return t == 4 ? 4 : t == 7 ? 8 : t == 3 ? 2 : 0; }

inline void write_tdef(std::vector<uint8_t>& img, uint32_t pg, const Tbl& t) {
    const size_t P = 4096, base = (size_t)pg * P;
    std::vector<uint8_t> b(P, 0);
    b[0] = 2;
    put32(b, 16, (uint32_t)t.rows.size());
    put16(b, 45, (uint32_t)t.cols.size());
    put32(b, 51, 0);           // no real indexes
    put32(b, 55, 0);           // usage map: row 0 of page 0 (not a data page -> full scan fallback)
    size_t o = 63;
    int var_i = 0, fixed_off = 0;
    for (size_t k = 0; k < t.cols.size(); ++k) {
        const Col& c = t.cols[k];
        const int fs = fixed_size(c.type);
        b[o] = (uint8_t)c.type;
        put16(b, o + 5, (uint32_t)k);
        put16(b, o + 7, fs ? 0 : (uint32_t)var_i);
        b[o + 15] = fs ? 0x01 : 0x00;
        put16(b, o + 21, fs ? (uint32_t)fixed_off : 0);
        put16(b, o + 23, fs ? (uint32_t)fs : 255);
        if (fs) fixed_off += fs; else ++var_i;
        o += 25;
    }
    for (const Col& c : t.cols) { auto u = ucs2(c.name); put16(b, o, (uint32_t)u.size()); o += 2; std::memcpy(&b[o], u.data(), u.size()); o += u.size(); }
    std::memcpy(&img[base], b.data(), P);
}

inline std::vector<uint8_t> encode_row(const Tbl& t, const std::vector<Val>& r) {
    const size_t n = t.cols.size();
    std::vector<uint8_t> fixed, var;
    std::vector<uint32_t> voff;
    std::vector<uint8_t> mask((n + 7) / 8, 0);
    for (size_t k = 0; k < n; ++k) {
        const Col& c = t.cols[k];
        const Val& v = r[k];
        if (!v.null) mask[k / 8] |= (uint8_t)(1u << (k % 8));
        const int fs = fixed_size(c.type);
        if (fs) {
            uint8_t tmp[8] = {};
            if (c.type == 4) { uint32_t x = (uint32_t)v.i; std::memcpy(tmp, &x, 4); }
            else if (c.type == 7) std::memcpy(tmp, &v.d, 8);
            else { uint16_t x = (uint16_t)v.i; std::memcpy(tmp, &x, 2); }
            fixed.insert(fixed.end(), tmp, tmp + fs);
        } else {
            voff.push_back((uint32_t)(2 + fixed.size() * 0));   // patched below
            auto u = v.null ? std::vector<uint8_t>() : ucs2(v.s);
            var.insert(var.end(), u.begin(), u.end());
            voff.back() = (uint32_t)u.size();                    // temporarily: length
        }
    }
    std::vector<uint8_t> row;
    row.push_back((uint8_t)n); row.push_back((uint8_t)(n >> 8));
    row.insert(row.end(), fixed.begin(), fixed.end());
    std::vector<uint32_t> off;   // var_cnt + 1 offsets from row start
    uint32_t pos = (uint32_t)row.size();
    for (uint32_t len : voff) { off.push_back(pos); pos += len; }
    off.push_back(pos);
    row.insert(row.end(), var.begin(), var.end());
    if (!voff.empty()) {
        // offset table (voff[var_cnt] first ... voff[0] last), then var_cnt
        for (size_t i = off.size(); i-- > 0;) { row.push_back((uint8_t)off[i]); row.push_back((uint8_t)(off[i] >> 8)); }
        row.push_back((uint8_t)voff.size()); row.push_back((uint8_t)(voff.size() >> 8));
    }
    row.insert(row.end(), mask.begin(), mask.end());
    return row;
}

inline void write_data(std::vector<uint8_t>& img, uint32_t pg, uint32_t owner, const Tbl& t) {
    const size_t P = 4096, base = (size_t)pg * P;
    img[base] = 1; img[base + 1] = 1;
    put32(img, base + 4, owner);
    put16(img, base + 12, (uint32_t)t.rows.size());
    size_t end = P;
    for (size_t r = 0; r < t.rows.size(); ++r) {
        auto row = encode_row(t, t.rows[r]);
        end -= row.size();
        std::memcpy(&img[base + end], row.data(), row.size());
        put16(img, base + 14 + 2 * r, (uint32_t)end);
    }
}

// user tables -> complete .mdb image
inline std::vector<uint8_t> build(const std::vector<Tbl>& tables) {
    const size_t P = 4096;
    const uint32_t npages = 4 + 2 * (uint32_t)tables.size();
    std::vector<uint8_t> img(P * npages, 0);
    img[1] = 1;
    std::memcpy(&img[4], "Standard Jet DB", 15);
    img[0x14] = 1;                         // Jet4
    std::vector<uint8_t> hdr(128, 0);
    hdr[0x3C - 0x18] = 0xE4; hdr[0x3D - 0x18] = 0x04;   // code page 1252, database key 0
    static const uint8_t key[4] = {0xC7, 0xDA, 0x39, 0x6B};
    rc4(key, 4, hdr.data(), hdr.size());
    std::memcpy(&img[0x18], hdr.data(), hdr.size());
    Tbl cat{"MSysObjects", {{"Id", 4}, {"Type", 3}, {"Flags", 4}, {"Name", 10}}, {}};
    cat.rows.push_back({Val{false, 2}, Val{false, 1}, Val{false, (int64_t)0x80000002u}, Val{false, 0, 0, "MSysObjects"}});
    for (size_t k = 0; k < tables.size(); ++k)
        cat.rows.push_back({Val{false, 4 + 2 * (int64_t)k}, Val{false, 1}, Val{false, 0}, Val{false, 0, 0, tables[k].name}});
    write_tdef(img, 2, cat);
    write_data(img, 3, 2, cat);
    for (size_t k = 0; k < tables.size(); ++k) {
        write_tdef(img, 4 + 2 * (uint32_t)k, tables[k]);
        write_data(img, 5 + 2 * (uint32_t)k, 4 + 2 * (uint32_t)k, tables[k]);
    }
    return img;
}

} // namespace jet_synth
