#pragma once
// ============================================================================
// inflate.h — portable, dependency-free DEFLATE decoder (RFC 1951) plus the
// zlib container (RFC 1950: 2-byte header + deflate + Adler-32).
//
// Written from the RFCs for PCB Plotter.  Used for CircuitCAM "CC_COMPRESS"
// blobs (raw deflate, i.e. zlib wbits = -15) and anything else that needs it.
// All input reads are bounds-checked; output growth is capped.
// ============================================================================
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pcbp {

// Decode a raw DEFLATE stream.  `out` is replaced.  `expected_size_hint` is used
// only to reserve memory (0 = unknown).  `max_out` caps the output to protect
// against decompression bombs (default 1 GiB).
// On success `*consumed` (if given) receives the number of input bytes used
// (rounded up to whole bytes).
bool inflate_raw(const uint8_t* src, size_t n, std::vector<uint8_t>& out,
                 size_t expected_size_hint, std::string& err,
                 size_t* consumed = nullptr, size_t max_out = size_t(1) << 30);

// Decode a zlib stream (CMF/FLG header, deflate data, big-endian Adler-32).
// Preset dictionaries are rejected.
bool zlib_uncompress(const uint8_t* src, size_t n, std::vector<uint8_t>& out,
                     size_t expected_size_hint, std::string& err);

uint32_t adler32(const uint8_t* p, size_t n, uint32_t a = 1);

} // namespace pcbp
