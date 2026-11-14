// ============================================================================
// deflate.cpp — DEFLATE (RFC 1951) encoder written from the RFC.
//   * LZ77 over a 32 KiB window: 3-byte hash, head/prev chains, lazy matching
//   * per block: cost of stored / fixed-Huffman / dynamic-Huffman is computed
//     and the cheapest representation is emitted
//   * Huffman code lengths limited to 15 (lit/len, dist) and 7 (code-length
//     alphabet) by redistributing over-long codes until Kraft's sum is exact
// ============================================================================
#include "deflate.h"
#include "inflate.h"
#include <algorithm>
#include <cstring>

namespace pcbp {

namespace {

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
const uint8_t kClOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

struct Tables {
    uint8_t len_code[259];     // length 3..258 -> code index 0..28
    uint8_t dist_code[512];    // see dcode()
    Tables() {
        for (int c = 0; c < 29; ++c) {
            int n = 1 << kLenExtra[c];
            for (int k = 0; k < n && kLenBase[c] + k <= 258; ++k) len_code[kLenBase[c] + k] = (uint8_t)c;
        }
        len_code[258] = 28;
        for (int c = 0; c < 30; ++c) {
            int n = 1 << kDistExtra[c];
            for (int k = 0; k < n; ++k) {
                int d = kDistBase[c] + k - 1;   // 0-based
                if (d < 256) dist_code[d] = (uint8_t)c;
                else if ((d >> 7) < 256) dist_code[256 + (d >> 7)] = (uint8_t)c;
            }
        }
    }
    int dcode(int dist) const { int d = dist - 1; return d < 256 ? dist_code[d] : dist_code[256 + (d >> 7)]; }
};
const Tables& T() { static const Tables t; return t; }

struct BitWriter {
    std::vector<uint8_t>& out;
    uint64_t acc = 0;
    int n = 0;
    explicit BitWriter(std::vector<uint8_t>& o) : out(o) {}
    void put(uint32_t bits, int cnt) {
        acc |= (uint64_t)bits << n;
        n += cnt;
        while (n >= 8) { out.push_back((uint8_t)acc); acc >>= 8; n -= 8; }
    }
    void align() { if (n > 0) { out.push_back((uint8_t)acc); acc = 0; n = 0; } }
};

uint32_t reverse_bits(uint32_t c, int len) { uint32_t r = 0; for (int i = 0; i < len; ++i) { r = (r << 1) | (c & 1); c >>= 1; } return r; }

// Build length-limited Huffman code lengths for `freq` (n symbols).
void build_lengths(const uint32_t* freq, int n, int max_len, uint8_t* lens) {
    std::fill(lens, lens + n, 0);
    std::vector<int> syms;
    for (int i = 0; i < n; ++i) if (freq[i]) syms.push_back(i);
    if (syms.empty()) return;
    if (syms.size() == 1) { lens[syms[0]] = 1; return; }
    // Huffman tree via two-queue method on sorted leaves
    std::sort(syms.begin(), syms.end(), [&](int a, int b) { return freq[a] < freq[b] || (freq[a] == freq[b] && a < b); });
    size_t m = syms.size();
    std::vector<uint64_t> w(2 * m);
    std::vector<int> parent(2 * m, -1);
    for (size_t i = 0; i < m; ++i) w[i] = freq[syms[i]];
    size_t leaf = 0, node = m, next = m;
    auto pick = [&]() -> size_t {
        if (leaf < m && (node >= next || w[leaf] <= w[node])) return leaf++;
        return node++;
    };
    while (next < 2 * m - 1) {
        size_t a = pick(), b = pick();
        w[next] = w[a] + w[b];
        parent[a] = parent[b] = (int)next;
        ++next;
    }
    // depths
    std::vector<int> depth(2 * m - 1, 0);
    for (int i = (int)(2 * m - 2) - 1; i >= 0; --i) depth[i] = depth[parent[i]] + 1;
    int cnt[64] = {0};
    for (size_t i = 0; i < m; ++i) ++cnt[std::min(depth[i], 63)];
    // limit
    for (int i = max_len + 1; i < 64; ++i) { cnt[max_len] += cnt[i]; cnt[i] = 0; }
    uint64_t total = 0;
    for (int i = 1; i <= max_len; ++i) total += (uint64_t)cnt[i] << (max_len - i);
    while (total > (1ull << max_len)) {
        --cnt[max_len];
        for (int i = max_len - 1; i > 0; --i) if (cnt[i]) { --cnt[i]; cnt[i + 1] += 2; break; }
        --total;
    }
    // assign: most frequent symbols get the shortest lengths (syms ascending by freq)
    int len = max_len;
    size_t k = 0;
    for (int L = max_len; L >= 1; --L)
        for (int c = 0; c < cnt[L]; ++c) lens[syms[k++]] = (uint8_t)L;
    (void)len;
}

void canonical(const uint8_t* lens, int n, uint32_t* codes) {
    int bl_count[16] = {0};
    for (int i = 0; i < n; ++i) if (lens[i]) ++bl_count[lens[i]];
    uint32_t next[16] = {0};
    uint32_t code = 0;
    for (int b = 1; b < 16; ++b) { code = (code + bl_count[b - 1]) << 1; next[b] = code; }
    for (int i = 0; i < n; ++i) if (lens[i]) codes[i] = reverse_bits(next[lens[i]]++, lens[i]);
}

struct Sym { uint16_t lit_or_len; uint16_t dist; };   // dist == 0 -> literal

struct BlockEncoder {
    BitWriter& bw;
    explicit BlockEncoder(BitWriter& b) : bw(b) {}

    void emit_stored(const uint8_t* p, size_t n, bool last) {
        do {
            size_t k = std::min<size_t>(n, 65535);
            bw.put(last && k == n ? 1 : 0, 1);
            bw.put(0, 2);
            bw.align();
            bw.out.push_back((uint8_t)k); bw.out.push_back((uint8_t)(k >> 8));
            bw.out.push_back((uint8_t)~k); bw.out.push_back((uint8_t)(~k >> 8));
            bw.out.insert(bw.out.end(), p, p + k);
            p += k; n -= k;
        } while (n > 0);
    }

    static uint64_t sym_cost(const std::vector<Sym>& syms, const uint8_t* ll, const uint8_t* dl) {
        uint64_t bits = 0;
        const Tables& t = T();
        for (auto& s : syms) {
            if (!s.dist) { bits += ll[s.lit_or_len]; continue; }
            int lc = t.len_code[s.lit_or_len];
            int dc = t.dcode(s.dist);
            bits += ll[257 + lc] + kLenExtra[lc] + dl[dc] + kDistExtra[dc];
        }
        return bits + ll[256];
    }

    void emit_syms(const std::vector<Sym>& syms, const uint8_t* ll, const uint32_t* lc_codes, const uint8_t* dl, const uint32_t* dc_codes) {
        const Tables& t = T();
        for (auto& s : syms) {
            if (!s.dist) { bw.put(lc_codes[s.lit_or_len], ll[s.lit_or_len]); continue; }
            int lc = t.len_code[s.lit_or_len];
            bw.put(lc_codes[257 + lc], ll[257 + lc]);
            if (kLenExtra[lc]) bw.put(s.lit_or_len - kLenBase[lc], kLenExtra[lc]);
            int dc = t.dcode(s.dist);
            bw.put(dc_codes[dc], dl[dc]);
            if (kDistExtra[dc]) bw.put(s.dist - kDistBase[dc], kDistExtra[dc]);
        }
        bw.put(lc_codes[256], ll[256]);
    }

    void encode(const std::vector<Sym>& syms, const uint8_t* raw, size_t raw_n, bool last) {
        const Tables& t = T();
        uint32_t lf[286] = {0}, df[30] = {0};
        for (auto& s : syms) {
            if (!s.dist) ++lf[s.lit_or_len];
            else { ++lf[257 + t.len_code[s.lit_or_len]]; ++df[t.dcode(s.dist)]; }
        }
        lf[256] = 1;
        // fixed
        static uint8_t fll[288], fdl[30];
        static uint32_t fllc[288], fdlc[30];
        static bool init = false;
        if (!init) {
            for (int i = 0; i < 144; ++i) fll[i] = 8;
            for (int i = 144; i < 256; ++i) fll[i] = 9;
            for (int i = 256; i < 280; ++i) fll[i] = 7;
            for (int i = 280; i < 288; ++i) fll[i] = 8;
            for (int i = 0; i < 30; ++i) fdl[i] = 5;
            canonical(fll, 288, fllc);
            canonical(fdl, 30, fdlc);
            init = true;
        }
        uint64_t fixed_bits = 3 + sym_cost(syms, fll, fdl);
        // dynamic
        uint8_t dll[286], ddl[30];
        build_lengths(lf, 286, 15, dll);
        build_lengths(df, 30, 15, ddl);
        bool any_d = false;
        for (int i = 0; i < 30; ++i) if (ddl[i]) any_d = true;
        if (!any_d) ddl[0] = 1;   // at least one distance code
        int hlit = 286; while (hlit > 257 && !dll[hlit - 1]) --hlit;
        int hdist = 30; while (hdist > 1 && !ddl[hdist - 1]) --hdist;
        // run-length encode code lengths
        std::vector<uint8_t> all(dll, dll + hlit);
        all.insert(all.end(), ddl, ddl + hdist);
        struct RL { uint8_t sym, extra; };
        std::vector<RL> rl;
        for (size_t i = 0; i < all.size();) {
            uint8_t v = all[i];
            size_t j = i; while (j < all.size() && all[j] == v) ++j;
            size_t run = j - i;
            if (v == 0) {
                while (run >= 11) { size_t k = std::min<size_t>(run, 138); rl.push_back({18, (uint8_t)(k - 11)}); run -= k; }
                if (run >= 3) { rl.push_back({17, (uint8_t)(run - 3)}); run = 0; }
                while (run--) rl.push_back({0, 0});
            } else {
                rl.push_back({v, 0}); --run;
                while (run >= 3) { size_t k = std::min<size_t>(run, 6); rl.push_back({16, (uint8_t)(k - 3)}); run -= k; }
                while (run--) rl.push_back({v, 0});
            }
            i = j;
        }
        uint32_t cf[19] = {0};
        for (auto& x : rl) ++cf[x.sym];
        uint8_t cl[19];
        build_lengths(cf, 19, 7, cl);
        int hclen = 19; while (hclen > 4 && !cl[kClOrder[hclen - 1]]) --hclen;
        uint64_t dyn_bits = 3 + 5 + 5 + 4 + 3 * hclen;
        for (auto& x : rl) dyn_bits += cl[x.sym] + (x.sym == 16 ? 2 : x.sym == 17 ? 3 : x.sym == 18 ? 7 : 0);
        dyn_bits += sym_cost(syms, dll, ddl);
        uint64_t stored_bits = (raw_n + 5 * ((raw_n + 65534) / 65535 + 1)) * 8 + 8;
        if (raw && stored_bits <= fixed_bits && stored_bits <= dyn_bits) { emit_stored(raw, raw_n, last); return; }
        if (fixed_bits <= dyn_bits) {
            bw.put(last ? 1 : 0, 1); bw.put(1, 2);
            emit_syms(syms, fll, fllc, fdl, fdlc);
            return;
        }
        bw.put(last ? 1 : 0, 1); bw.put(2, 2);
        bw.put(hlit - 257, 5); bw.put(hdist - 1, 5); bw.put(hclen - 4, 4);
        for (int i = 0; i < hclen; ++i) bw.put(cl[kClOrder[i]], 3);
        uint32_t clc[19];
        canonical(cl, 19, clc);
        for (auto& x : rl) {
            bw.put(clc[x.sym], cl[x.sym]);
            if (x.sym == 16) bw.put(x.extra, 2); else if (x.sym == 17) bw.put(x.extra, 3); else if (x.sym == 18) bw.put(x.extra, 7);
        }
        uint32_t llc[286], dlc[30];
        canonical(dll, 286, llc);
        canonical(ddl, 30, dlc);
        emit_syms(syms, dll, llc, ddl, dlc);
    }
};

} // namespace

std::vector<uint8_t> deflate_raw(const uint8_t* src, size_t n, int level) {
    std::vector<uint8_t> out;
    out.reserve(n / 3 + 64);
    BitWriter bw(out);
    BlockEncoder enc(bw);
    if (level <= 0 || n == 0) {
        if (n == 0) { bw.put(1, 1); bw.put(1, 2); bw.put(0, 7); bw.align(); return out; }   // fixed block with only EOB
        enc.emit_stored(src, n, true);
        return out;
    }
    level = std::min(level, 9);
    const int max_chain = level <= 1 ? 4 : level <= 3 ? 16 : level <= 6 ? 128 : level <= 8 ? 1024 : 4096;
    const int nice = level <= 3 ? 32 : level <= 6 ? 128 : 258;
    const bool lazy = level >= 4;
    const size_t WSIZE = 32768, WMASK = WSIZE - 1;
    const int HBITS = 15;
    std::vector<int32_t> head(1 << HBITS, -1);
    std::vector<int32_t> prev(WSIZE, -1);
    auto hash3 = [&](size_t i) -> uint32_t { return ((uint32_t)src[i] << 10 ^ (uint32_t)src[i + 1] << 5 ^ src[i + 2]) * 2654435761u >> (32 - HBITS); };
    auto insert = [&](size_t i) {
        if (i + 2 >= n) return;
        uint32_t h = hash3(i);
        prev[i & WMASK] = head[h];
        head[h] = (int32_t)i;
    };
    auto longest = [&](size_t i, int& best_dist) -> int {
        if (i + 2 >= n) return 0;
        int best = 2;
        int chain = max_chain;
        int32_t cand = head[hash3(i)];
        size_t maxlen = std::min<size_t>(258, n - i);
        while (cand >= 0 && chain-- > 0) {
            size_t c = (size_t)cand;
            if (i - c > WSIZE - 262 + 256 || c >= i) break;
            if (src[c + best] == src[i + best] && src[c] == src[i] && src[c + 1] == src[i + 1]) {
                size_t L = 0;
                while (L < maxlen && src[c + L] == src[i + L]) ++L;
                if ((int)L > best) { best = (int)L; best_dist = (int)(i - c); if (L >= (size_t)nice) break; }
            }
            int32_t nx = prev[c & WMASK];
            if (nx >= (int32_t)c) break;
            cand = nx;
        }
        return best >= 3 ? best : 0;
    };
    std::vector<Sym> syms;
    syms.reserve(1 << 15);
    size_t block_start = 0;
    size_t i = 0;
    auto flush = [&](bool last) {
        enc.encode(syms, src + block_start, i - block_start, last);
        syms.clear();
        block_start = i;
    };
    while (i < n) {
        int dist = 0;
        int len = longest(i, dist);
        if (len && lazy && len < nice && i + 1 < n) {
            insert(i);
            int d2 = 0;
            int l2 = longest(i + 1, d2);
            if (l2 > len) {
                syms.push_back({src[i], 0});
                ++i;
                len = l2; dist = d2;
                // fallthrough: emit match at i (hash for i already inserted above for i-1)
                for (int k = 0; k < len; ++k) insert(i + k);
                syms.push_back({(uint16_t)len, (uint16_t)dist});
                i += len;
                if (syms.size() >= 16384) flush(false);
                continue;
            }
            for (int k = 1; k < len; ++k) insert(i + k);
            syms.push_back({(uint16_t)len, (uint16_t)dist});
            i += len;
        } else if (len) {
            for (int k = 0; k < len; ++k) insert(i + k);
            syms.push_back({(uint16_t)len, (uint16_t)dist});
            i += len;
        } else {
            insert(i);
            syms.push_back({src[i], 0});
            ++i;
        }
        if (syms.size() >= 16384) flush(false);
    }
    flush(true);
    bw.align();
    return out;
}

std::vector<uint8_t> zlib_compress(const uint8_t* src, size_t n, int level) {
    std::vector<uint8_t> out = {0x78, (uint8_t)(level >= 7 ? 0xDA : level <= 1 ? 0x01 : 0x9C)};
    auto body = deflate_raw(src, n, level);
    out.insert(out.end(), body.begin(), body.end());
    uint32_t a = adler32(src, n);
    out.push_back((uint8_t)(a >> 24)); out.push_back((uint8_t)(a >> 16)); out.push_back((uint8_t)(a >> 8)); out.push_back((uint8_t)a);
    return out;
}

} // namespace pcbp
