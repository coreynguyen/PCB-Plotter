// ============================================================================
// inflate.cpp — RFC 1951 inflate / RFC 1950 zlib, written from the RFCs.
//
// Decoding uses canonical-Huffman "count/symbol" tables (the classic
// length-by-length walk), which is compact and easy to bounds-check.
// ============================================================================
#include "inflate.h"

#include <cstring>

namespace pcbp {
namespace {

constexpr int kMaxBits = 15;
constexpr int kMaxLitLen = 288;
constexpr int kMaxDist = 30;

struct BitReader {
    const uint8_t* p;
    size_t n;
    size_t pos = 0;      // next byte
    uint32_t buf = 0;    // bit buffer (LSB first)
    int cnt = 0;         // bits in buf
    bool overrun = false;

    bool need(int k) {
        while (cnt < k) {
            if (pos >= n) { overrun = true; return false; }
            buf |= uint32_t(p[pos++]) << cnt;
            cnt += 8;
        }
        return true;
    }
    // read k bits (k <= 24)
    bool bits(int k, uint32_t& v) {
        if (k == 0) { v = 0; return true; }
        if (!need(k)) return false;
        v = buf & ((1u << k) - 1);
        buf >>= k;
        cnt -= k;
        return true;
    }
    void align() { buf = 0; cnt = 0; }   // drop partial byte (whole bytes already fetched are kept)
};

struct Huffman {
    uint16_t count[kMaxBits + 1];
    uint16_t symbol[kMaxLitLen];
};

// Build a canonical Huffman decoder.  Returns 0 if complete, >0 if incomplete
// (allowed for some distance codes), <0 if over-subscribed.
int build(Huffman& h, const uint8_t* lengths, int n) {
    std::memset(h.count, 0, sizeof(h.count));
    for (int s = 0; s < n; ++s) h.count[lengths[s]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= kMaxBits; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return -1;
    }
    uint16_t offs[kMaxBits + 1];
    offs[1] = 0;
    for (int len = 1; len < kMaxBits; ++len) offs[len + 1] = uint16_t(offs[len] + h.count[len]);
    for (int s = 0; s < n; ++s)
        if (lengths[s]) h.symbol[offs[lengths[s]]++] = uint16_t(s);
    return left;
}

// Decode one symbol; returns -1 on input overrun, -2 on invalid code.
int decode_sym(BitReader& br, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= kMaxBits; ++len) {
        uint32_t b;
        if (!br.bits(1, b)) return -1;
        code |= int(b);
        const int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -2;
}

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                               35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                               3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                                4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

struct Inflater {
    BitReader br;
    std::vector<uint8_t>& out;
    size_t max_out;
    std::string& err;

    bool fail(const char* m) { err = m; return false; }

    bool codes(const Huffman& lit, const Huffman& dist) {
        for (;;) {
            int sym = decode_sym(br, lit);
            if (sym < 0) return fail(sym == -1 ? "inflate: input truncated" : "inflate: bad literal/length code");
            if (sym < 256) {
                if (out.size() >= max_out) return fail("inflate: output limit exceeded");
                out.push_back(uint8_t(sym));
            } else if (sym == 256) {
                return true;
            } else {
                sym -= 257;
                if (sym >= 29) return fail("inflate: invalid length symbol");
                uint32_t e;
                if (!br.bits(kLenExtra[sym], e)) return fail("inflate: input truncated");
                const size_t len = kLenBase[sym] + e;
                int ds = decode_sym(br, dist);
                if (ds < 0) return fail(ds == -1 ? "inflate: input truncated" : "inflate: bad distance code");
                if (ds >= 30) return fail("inflate: invalid distance symbol");
                if (!br.bits(kDistExtra[ds], e)) return fail("inflate: input truncated");
                const size_t d = kDistBase[ds] + e;
                if (d > out.size()) return fail("inflate: distance too far back");
                if (out.size() + len > max_out) return fail("inflate: output limit exceeded");
                size_t from = out.size() - d;
                for (size_t i = 0; i < len; ++i) out.push_back(out[from + i]);   // overlap-safe
            }
        }
    }

    bool stored() {
        br.align();
        if (br.pos + 4 > br.n) return fail("inflate: stored header truncated");
        const unsigned len = br.p[br.pos] | (br.p[br.pos + 1] << 8);
        const unsigned nlen = br.p[br.pos + 2] | (br.p[br.pos + 3] << 8);
        br.pos += 4;
        if ((len ^ 0xFFFFu) != nlen) return fail("inflate: stored length check failed");
        if (br.pos + len > br.n) return fail("inflate: stored block truncated");
        if (out.size() + len > max_out) return fail("inflate: output limit exceeded");
        out.insert(out.end(), br.p + br.pos, br.p + br.pos + len);
        br.pos += len;
        return true;
    }

    struct FixedTables { Huffman lit, dist; };
    static FixedTables make_fixed() {
        FixedTables t;
        uint8_t l[kMaxLitLen];
        int s = 0;
        for (; s < 144; ++s) l[s] = 8;
        for (; s < 256; ++s) l[s] = 9;
        for (; s < 280; ++s) l[s] = 7;
        for (; s < kMaxLitLen; ++s) l[s] = 8;
        build(t.lit, l, kMaxLitLen);
        for (s = 0; s < kMaxDist; ++s) l[s] = 5;
        build(t.dist, l, kMaxDist);
        return t;
    }

    bool fixed() {
        static const FixedTables t = make_fixed();   // thread-safe magic static
        return codes(t.lit, t.dist);
    }

    bool dynamic() {
        static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        uint32_t hlit, hdist, hclen;
        if (!br.bits(5, hlit) || !br.bits(5, hdist) || !br.bits(4, hclen))
            return fail("inflate: dynamic header truncated");
        hlit += 257; hdist += 1; hclen += 4;
        if (hlit > 286 || hdist > 30) return fail("inflate: bad dynamic code counts");
        uint8_t lengths[kMaxLitLen + kMaxDist] = {};
        for (uint32_t i = 0; i < hclen; ++i) {
            uint32_t v;
            if (!br.bits(3, v)) return fail("inflate: dynamic header truncated");
            lengths[order[i]] = uint8_t(v);
        }
        Huffman lencode;
        if (build(lencode, lengths, 19) != 0) return fail("inflate: bad code-length code");
        uint32_t idx = 0;
        while (idx < hlit + hdist) {
            int sym = decode_sym(br, lencode);
            if (sym < 0) return fail("inflate: bad code-length symbol");
            if (sym < 16) { lengths[idx++] = uint8_t(sym); continue; }
            uint8_t val = 0;
            uint32_t rep;
            if (sym == 16) {
                if (idx == 0) return fail("inflate: repeat with no previous length");
                val = lengths[idx - 1];
                if (!br.bits(2, rep)) return fail("inflate: input truncated");
                rep += 3;
            } else if (sym == 17) {
                if (!br.bits(3, rep)) return fail("inflate: input truncated");
                rep += 3;
            } else {
                if (!br.bits(7, rep)) return fail("inflate: input truncated");
                rep += 11;
            }
            if (idx + rep > hlit + hdist) return fail("inflate: code lengths overflow");
            while (rep--) lengths[idx++] = val;
        }
        if (lengths[256] == 0) return fail("inflate: no end-of-block code");
        Huffman lit, dist;
        int r = build(lit, lengths, int(hlit));
        if (r < 0 || (r > 0 && int(hlit) - lit.count[0] != 1)) return fail("inflate: bad literal/length lengths");
        r = build(dist, lengths + hlit, int(hdist));
        if (r < 0 || (r > 0 && int(hdist) - dist.count[0] != 1)) return fail("inflate: bad distance lengths");
        return codes(lit, dist);
    }

    bool run() {
        uint32_t last = 0, type = 0;
        do {
            if (!br.bits(1, last) || !br.bits(2, type)) return fail("inflate: block header truncated");
            bool ok;
            switch (type) {
                case 0: ok = stored(); break;
                case 1: ok = fixed(); break;
                case 2: ok = dynamic(); break;
                default: return fail("inflate: invalid block type 3");
            }
            if (!ok) return false;
        } while (!last);
        return true;
    }
};

} // namespace

uint32_t adler32(const uint8_t* p, size_t n, uint32_t a) {
    uint32_t s1 = a & 0xFFFF, s2 = a >> 16;
    while (n) {
        size_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) { s1 += *p++; s2 += s1; }
        s1 %= 65521; s2 %= 65521;
    }
    return (s2 << 16) | s1;
}

bool inflate_raw(const uint8_t* src, size_t n, std::vector<uint8_t>& out,
                 size_t expected_size_hint, std::string& err, size_t* consumed, size_t max_out) {
    out.clear();
    if (!src && n) { err = "inflate: null input"; return false; }
    if (expected_size_hint && expected_size_hint <= max_out) out.reserve(expected_size_hint);
    Inflater inf{BitReader{src, n}, out, max_out, err};
    if (!inf.run()) return false;
    if (consumed) *consumed = inf.br.pos;    // whole bytes fetched (partial last byte counted)
    return true;
}

bool zlib_uncompress(const uint8_t* src, size_t n, std::vector<uint8_t>& out,
                     size_t expected_size_hint, std::string& err) {
    if (n < 6) { err = "zlib: stream too short"; return false; }
    const uint8_t cmf = src[0], flg = src[1];
    if ((cmf & 0x0F) != 8) { err = "zlib: compression method is not deflate"; return false; }
    if ((cmf >> 4) > 7) { err = "zlib: window size too large"; return false; }
    if (((cmf << 8) | flg) % 31 != 0) { err = "zlib: header check failed"; return false; }
    if (flg & 0x20) { err = "zlib: preset dictionary not supported"; return false; }
    size_t used = 0;
    if (!inflate_raw(src + 2, n - 2, out, expected_size_hint, err, &used)) return false;
    const size_t at = 2 + used;
    if (at + 4 > n) { err = "zlib: missing Adler-32 trailer"; return false; }
    const uint32_t want = (uint32_t(src[at]) << 24) | (uint32_t(src[at + 1]) << 16) |
                          (uint32_t(src[at + 2]) << 8) | uint32_t(src[at + 3]);
    if (adler32(out.data(), out.size()) != want) { err = "zlib: Adler-32 mismatch"; return false; }
    return true;
}

} // namespace pcbp
