#pragma once
// sha256.h - SHA-256 (FIPS 180-4), portable. Used for the job archive's content hashes.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pcbp {

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t n);
    void final(uint8_t out[32]);
    std::string hex();                     // final() as 64 lower-case hex digits
private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t len_ = 0;
    uint64_t total_ = 0;
};

std::string sha256_hex(const void* data, size_t n);
inline std::string sha256_hex(const std::vector<uint8_t>& v) { return sha256_hex(v.data(), v.size()); }
// SHA-256 of a disk file (UTF-8 path), streamed; "" when it cannot be read.
std::string sha256_file(const std::string& path);

}  // namespace pcbp
