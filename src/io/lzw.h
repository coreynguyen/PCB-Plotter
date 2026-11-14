#pragma once
// ============================================================================
// lzw.h — Unix `compress` (.Z, magic 1F 9D) decoder (pcbp::io), portable.
//
// Format (from the public compress(1) / ncompress description):
//   byte 0..1  1F 9D
//   byte 2     bit 0..4 = max code width (9..16), bit 7 = block mode
//   data       LSB-first variable-width codes, starting at 9 bits. In block
//              mode code 256 = CLEAR (table reset, width back to 9). The
//              width grows when the next free code exceeds the current
//              width's range. Codes are written in groups of 8 per width;
//              after a width change or CLEAR the reader skips to the end of
//              the current 8-code group (the classic "bit-group padding").
// ODB++ exporters write member files (features.Z, components.Z, data.Z)
// with this format.
// ============================================================================
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pcbp::io {

bool is_lzw_compress(const uint8_t* p, size_t n);
bool lzw_decompress(const uint8_t* p, size_t n, std::vector<uint8_t>& out, std::string& err);
// Encoder (block mode, given max bits) — used by tests to produce .Z data.
std::vector<uint8_t> lzw_compress(const uint8_t* p, size_t n, int maxbits = 16);

} // namespace pcbp::io
