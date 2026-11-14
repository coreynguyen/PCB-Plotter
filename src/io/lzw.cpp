// ============================================================================
// lzw.cpp — Unix compress (.Z) LZW codec (see lzw.h).
// ============================================================================
#include "lzw.h"

#include <algorithm>

namespace pcbp::io {

bool is_lzw_compress(const uint8_t* p, size_t n) { return n >= 3 && p[0] == 0x1F && p[1] == 0x9D; }

bool lzw_decompress(const uint8_t* p, size_t n, std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    if (!is_lzw_compress(p, n)) { err = "not a compress (.Z) stream"; return false; }
    const int maxbits = p[2] & 0x1F;
    const bool block = (p[2] & 0x80) != 0;
    if (maxbits < 9 || maxbits > 16) { err = "compress: bad max bits " + std::to_string(maxbits); return false; }
    const uint32_t maxmax = 1u << maxbits;
    std::vector<uint16_t> prefix(maxmax);
    std::vector<uint8_t> suffix(maxmax), stack;
    stack.reserve(maxmax);
    for (uint32_t i = 0; i < 256; ++i) { prefix[i] = 0; suffix[i] = (uint8_t)i; }
    const uint8_t* d = p + 3;
    const size_t nbits = (n - 3) * 8;
    size_t bitpos = 0;
    int width = 9;
    uint32_t maxcode = (1u << width) - 1;
    uint32_t free_ent = block ? 257 : 256;
    size_t group_start = 0;      // bit position where the current width group started
    int32_t oldcode = -1;
    uint8_t finchar = 0;
    auto skip_group = [&]() {
        // advance bitpos to the end of the 8-code group that began at group_start
        const size_t gbits = (size_t)width * 8;
        const size_t used = bitpos - group_start;
        if (used % gbits) bitpos += gbits - used % gbits;
    };
    out.reserve(n * 3);
    while (true) {
        if (free_ent > maxcode && width < maxbits) {
            skip_group();
            ++width;
            maxcode = width == maxbits ? maxmax - 1 : (1u << width) - 1;
            group_start = bitpos;
        }
        if (bitpos + width > nbits) break;
        uint32_t code = 0;
        for (int b = 0; b < width; ++b, ++bitpos)
            code |= (uint32_t)((d[bitpos >> 3] >> (bitpos & 7)) & 1) << b;
        if (oldcode == -1) {
            if (code > 255) { err = "compress: bad first code"; return false; }
            finchar = (uint8_t)code;
            oldcode = (int32_t)code;
            out.push_back(finchar);
            continue;
        }
        if (code == 256 && block) {
            skip_group();
            for (uint32_t i = 0; i < 256; ++i) prefix[i] = 0;
            width = 9;
            maxcode = (1u << width) - 1;
            group_start = bitpos;
            oldcode = -1;      // next code is taken raw (no table entry), like the first code
            free_ent = 257;
            continue;
        }
        const uint32_t incode = code;
        stack.clear();
        if (code >= free_ent) {
            if (code > free_ent) { err = "compress: corrupt data (code " + std::to_string(code) + ")"; return false; }
            stack.push_back(finchar);
            code = (uint32_t)oldcode;
        }
        while (code >= 256) {
            stack.push_back(suffix[code]);
            code = prefix[code];
            if (stack.size() > maxmax) { err = "compress: corrupt chain"; return false; }
        }
        finchar = suffix[code];
        stack.push_back(finchar);
        for (size_t i = stack.size(); i-- > 0;) out.push_back(stack[i]);
        if (free_ent < maxmax) {
            prefix[free_ent] = (uint16_t)oldcode;
            suffix[free_ent] = finchar;
            ++free_ent;
        }
        oldcode = (int32_t)incode;
    }
    return true;
}

std::vector<uint8_t> lzw_compress(const uint8_t* p, size_t n, int maxbits) {
    if (maxbits < 9) maxbits = 9;
    if (maxbits > 16) maxbits = 16;
    std::vector<uint8_t> out = {0x1F, 0x9D, (uint8_t)(0x80 | maxbits)};
    const uint32_t maxmax = 1u << maxbits;
    std::vector<int32_t> dict((size_t)maxmax << 8, -1);   // (code<<8 | byte) -> code
    uint64_t acc = 0; int accn = 0;
    size_t bits_written = 0, group_start = 0;
    int width = 9;
    uint32_t maxcode = (1u << width) - 1, free_ent = 257;
    auto put = [&](uint32_t code) {
        acc |= (uint64_t)code << accn; accn += width; bits_written += width;
        while (accn >= 8) { out.push_back((uint8_t)acc); acc >>= 8; accn -= 8; }
    };
    auto pad_group = [&]() {
        const size_t gbits = (size_t)width * 8, used = bits_written - group_start;
        if (used % gbits) {
            size_t pad = gbits - used % gbits;
            while (pad--) { ++accn; ++bits_written; if (accn >= 8) { out.push_back((uint8_t)acc); acc >>= 8; accn -= 8; } }
        }
    };
    if (n == 0) return out;
    int32_t w = p[0];
    for (size_t i = 1; i < n; ++i) {
        const uint8_t c = p[i];
        const size_t k = ((size_t)w << 8) | c;
        if (dict[k] >= 0) { w = dict[k]; continue; }
        put((uint32_t)w);
        if (free_ent < maxmax) {
            dict[k] = (int32_t)free_ent++;
        } else {
            // table full: emit CLEAR (block mode), pad the group, restart at 9 bits
            put(256);
            pad_group();
            std::fill(dict.begin(), dict.end(), -1);
            width = 9; maxcode = (1u << width) - 1; free_ent = 257;
            group_start = bits_written;
            w = c;
            continue;
        }
        // decoder state: after reading this code it has free_ent_dec = free_ent (it adds the entry
        // on the *next* code). Widen when the decoder will widen before the next read.
        if (free_ent - 1 > maxcode && width < maxbits) {
            pad_group();
            ++width;
            maxcode = width == maxbits ? maxmax - 1 : (1u << width) - 1;
            group_start = bits_written;
        }
        w = c;
    }
    put((uint32_t)w);
    if (accn > 0) out.push_back((uint8_t)acc);
    return out;
}

} // namespace pcbp::io
