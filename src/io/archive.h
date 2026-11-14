#pragma once
// ============================================================================
// archive.h — minimal read-only archive support (pcbp::io), portable.
//
// gzip (RFC 1952, multi-member, CRC-32 checked), POSIX/ustar/GNU tar (long
// names via 'L' records and pax 'path=' headers), ZIP (central directory,
// methods 0 = stored and 8 = deflate; no ZIP64, no encryption). Deflate comes
// from inflate.h. Used by the ODB++ importer (.tgz / .tar.gz / .tar / .zip),
// but generic.
//
// FileTree presents a directory on disk or an archive as one virtual tree of
// '/'-separated relative paths; lookups are case-insensitive.
// ============================================================================
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace pcbp::io {

struct ArchiveEntry {
    std::string name;               // '/'-separated path as stored
    std::vector<uint8_t> data;
    bool dir = false;
};

enum class ArchiveKind { None, Gzip, Tar, Zip };

uint32_t crc32_ieee(const uint8_t* p, size_t n, uint32_t crc = 0);

// R14: false for absolute paths, drive letters, ".." segments, control chars. read_tar/read_zip
// fail on such entries; decoded totals are capped at 1 GiB and 200000 entries.
bool archive_name_safe(const std::string& name);

ArchiveKind detect_archive(const uint8_t* p, size_t n);

bool gunzip(const uint8_t* p, size_t n, std::vector<uint8_t>& out, std::string& err);
bool read_tar(const uint8_t* p, size_t n, std::vector<ArchiveEntry>& out, std::string& err);
bool read_zip(const uint8_t* p, size_t n, std::vector<ArchiveEntry>& out, std::string& err);

// Auto-detect: zip, tar, gzip(tar) or gzip(single file -> one entry named `gz_name`).
bool read_archive(const std::vector<uint8_t>& data, std::vector<ArchiveEntry>& out, std::string& err,
                  const std::string& gz_name = "data");

class FileTree {
public:
    // `path` is a directory or an archive file.
    bool open(const std::string& path, std::string& err);
    bool open_entries(std::vector<ArchiveEntry> entries);
    // Relative path, case-insensitive, '/' or '\\' separators.
    bool get(const std::string& rel, std::string& out) const;
    bool exists(const std::string& rel) const;
    std::vector<std::string> list() const;       // all file paths (original case)
    bool is_archive() const { return archive_; }
private:
    static std::string key(const std::string& p);
    std::map<std::string, std::string> files_;   // key -> original path
    std::map<std::string, std::vector<uint8_t>> data_;
    std::string root_;                            // directory root (when !archive_)
    bool archive_ = false;
};

} // namespace pcbp::io
