#pragma once
// ============================================================================
// deflate.h — DEFLATE compressor (RFC 1951) written from the RFC: LZ77 with
// hash chains + lazy matching, fixed and dynamic Huffman blocks (length-limited
// code construction), stored blocks for incompressible data. zlib (RFC 1950)
// wrapper too. Decoder lives in inflate.h.
// ============================================================================
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pcbp {

// level 0 = stored blocks only, 1 = fast (short chains), 6 = default, 9 = best.
std::vector<uint8_t> deflate_raw(const uint8_t* src, size_t n, int level = 6);
std::vector<uint8_t> zlib_compress(const uint8_t* src, size_t n, int level = 6);

} // namespace pcbp
