// ============================================================================
// archive.cpp — gzip / tar / zip readers and FileTree (see archive.h).
// Written from RFC 1952, the POSIX ustar description and PKWARE APPNOTE.
// ============================================================================
#include "archive.h"
#include "inflate.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace pcbp::io {

// R14: hard limits for untrusted archives (decompression bombs, path traversal).
static constexpr size_t kArcMaxTotal = size_t(1) << 30;   // 1 GiB of decoded data in all
static constexpr size_t kArcMaxEntries = 200000;

// Rejects absolute paths, drive letters, ".." segments and control characters. The tree is
// in-memory only, but a later "extract" or an on-disk join must never escape the job root.
bool archive_name_safe(const std::string& name) {
    if (name.empty() || name.size() > 4096) return false;
    if (name[0] == '/' || name[0] == '\\') return false;
    if (name.size() >= 2 && name[1] == ':') return false;
    for (unsigned char c : name) if (c < 0x20) return false;
    size_t i = 0;
    while (i <= name.size()) {
        size_t j = name.find_first_of("/\\", i);
        if (j == std::string::npos) j = name.size();
        if (name.compare(i, j - i, "..") == 0 && j - i == 2) return false;
        i = j + 1;
    }
    return true;
}

static bool check_entry(const ArchiveEntry& e, size_t& total, size_t count, std::string& err, const char* kind) {
    if (!archive_name_safe(e.name)) { err = std::string(kind) + ": unsafe entry path rejected: " + e.name.substr(0, 200); return false; }
    if (count >= kArcMaxEntries) { err = std::string(kind) + ": too many entries"; return false; }
    total += e.data.size();
    if (total > kArcMaxTotal) { err = std::string(kind) + ": decoded size exceeds 1 GiB limit"; return false; }
    return true;
}

uint32_t crc32_ieee(const uint8_t* p, size_t n, uint32_t crc) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

static bool is_tar_header(const uint8_t* p, size_t n) {
    if (n < 512) return false;
    if (std::equal(p + 257, p + 262, (const uint8_t*)"ustar")) return true;
    // checksum test for old v7 tar
    unsigned sum = 0;
    for (int i = 0; i < 512; ++i) sum += (i >= 148 && i < 156) ? ' ' : p[i];
    unsigned stored = 0; bool any = false;
    for (int i = 148; i < 156; ++i) {
        if (p[i] >= '0' && p[i] <= '7') { stored = stored * 8 + (p[i] - '0'); any = true; }
        else if (any) break;
    }
    return any && stored == sum && p[0] != 0;
}

ArchiveKind detect_archive(const uint8_t* p, size_t n) {
    if (n >= 3 && p[0] == 0x1F && p[1] == 0x8B && p[2] == 8) return ArchiveKind::Gzip;
    if (n >= 4 && p[0] == 'P' && p[1] == 'K' && (p[2] == 3 || p[2] == 5) && (p[3] == 4 || p[3] == 6)) return ArchiveKind::Zip;
    if (is_tar_header(p, n)) return ArchiveKind::Tar;
    return ArchiveKind::None;
}

bool gunzip(const uint8_t* p, size_t n, std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    size_t pos = 0;
    int members = 0;
    while (pos + 18 <= n && p[pos] == 0x1F && p[pos + 1] == 0x8B) {
        if (p[pos + 2] != 8) { err = "gzip: unsupported compression method"; return false; }
        const uint8_t flg = p[pos + 3];
        size_t q = pos + 10;
        if (flg & 4) { if (q + 2 > n) break; q += 2 + rd16(p + q); }
        if (flg & 8) { while (q < n && p[q]) ++q; ++q; }
        if (flg & 16) { while (q < n && p[q]) ++q; ++q; }
        if (flg & 2) q += 2;
        if (q >= n) { err = "gzip: truncated header"; return false; }
        std::vector<uint8_t> part;
        size_t used = 0;
        if (out.size() >= kArcMaxTotal) { err = "gzip: decoded size exceeds 1 GiB limit"; return false; }
        if (!inflate_raw(p + q, n - q, part, 0, err, &used, kArcMaxTotal - out.size())) { err = "gzip: " + err; return false; }
        q += used;
        if (q + 8 > n) { err = "gzip: truncated trailer"; return false; }
        const uint32_t crc = rd32(p + q), isz = rd32(p + q + 4);
        if (crc32_ieee(part.data(), part.size()) != crc) { err = "gzip: CRC mismatch"; return false; }
        if (uint32_t(part.size()) != isz) { err = "gzip: size mismatch"; return false; }
        out.insert(out.end(), part.begin(), part.end());
        pos = q + 8;
        ++members;
    }
    if (!members) { err = "gzip: bad signature"; return false; }
    return true;
}

static uint64_t tar_num(const uint8_t* f, size_t len) {
    if (f[0] & 0x80) {  // base-256 (GNU)
        uint64_t v = f[0] & 0x7F;
        for (size_t i = 1; i < len; ++i) v = (v << 8) | f[i];
        return v;
    }
    uint64_t v = 0;
    size_t i = 0;
    while (i < len && (f[i] == ' ' || f[i] == 0)) ++i;
    for (; i < len && f[i] >= '0' && f[i] <= '7'; ++i) v = v * 8 + (f[i] - '0');
    return v;
}

static std::string cstr(const uint8_t* f, size_t len) {
    size_t k = 0;
    while (k < len && f[k]) ++k;
    return std::string((const char*)f, k);
}

bool read_tar(const uint8_t* p, size_t n, std::vector<ArchiveEntry>& out, std::string& err) {
    size_t pos = 0, total = 0;
    std::string long_name;
    while (pos + 512 <= n) {
        const uint8_t* h = p + pos;
        bool zero = true;
        for (int i = 0; i < 512 && zero; ++i) zero = h[i] == 0;
        if (zero) break;
        if (!is_tar_header(h, 512)) { err = "tar: bad header at " + std::to_string(pos); return false; }
        const uint64_t size = tar_num(h + 124, 12);
        const char type = (char)h[156];
        pos += 512;
        if (size > n - pos) { err = "tar: truncated entry"; return false; }
        const uint8_t* d = p + pos;
        const size_t padded = size_t((size + 511) & ~uint64_t(511));
        if (type == 'L') {
            long_name = cstr(d, size_t(size));
        } else if (type == 'x') {
            // pax records "len key=value\n"
            std::string s((const char*)d, size_t(size));
            size_t i = 0;
            while (i < s.size()) {
                size_t sp = s.find(' ', i);
                if (sp == std::string::npos) break;
                size_t len = std::strtoul(s.c_str() + i, nullptr, 10);
                if (len == 0 || i + len > s.size()) break;
                std::string rec = s.substr(sp + 1, i + len - sp - 2);
                if (rec.rfind("path=", 0) == 0) long_name = rec.substr(5);
                i += len;
            }
        } else if (type == 'g') {
            // global pax header: ignore
        } else {
            ArchiveEntry e;
            if (!long_name.empty()) { e.name = long_name; long_name.clear(); }
            else {
                e.name = cstr(h, 100);
                if (std::equal(h + 257, h + 262, (const uint8_t*)"ustar")) {
                    std::string pre = cstr(h + 345, 155);
                    if (!pre.empty()) e.name = pre + "/" + e.name;
                }
            }
            e.dir = type == '5' || (!e.name.empty() && e.name.back() == '/');
            if (type == '0' || type == 0 || type == '7') e.data.assign(d, d + size);
            if (type == '0' || type == 0 || type == '7' || e.dir) {
                if (!check_entry(e, total, out.size(), err, "tar")) return false;
                out.push_back(std::move(e));
            }
        }
        pos += padded;
    }
    return true;
}

bool read_zip(const uint8_t* p, size_t n, std::vector<ArchiveEntry>& out, std::string& err) {
    if (n < 22) { err = "zip: too small"; return false; }
    size_t eocd = std::string::npos;
    const size_t lim = n >= 22 + 65535 ? n - 22 - 65535 : 0;
    for (size_t i = n - 22 + 1; i-- > lim;) {
        if (p[i] == 'P' && p[i + 1] == 'K' && p[i + 2] == 5 && p[i + 3] == 6) { eocd = i; break; }
    }
    if (eocd == std::string::npos) { err = "zip: end of central directory not found"; return false; }
    const uint16_t count = rd16(p + eocd + 10);
    const uint32_t cd_size = rd32(p + eocd + 12), cd_off = rd32(p + eocd + 16);
    if (cd_off == 0xFFFFFFFFu || count == 0xFFFF) { err = "zip: ZIP64 not supported"; return false; }
    if (size_t(cd_off) + cd_size > n) { err = "zip: bad central directory"; return false; }
    size_t q = cd_off, total = 0;
    for (unsigned k = 0; k < count; ++k) {
        if (q + 46 > n || rd32(p + q) != 0x02014b50u) { err = "zip: bad central header"; return false; }
        const uint16_t flags = rd16(p + q + 8), method = rd16(p + q + 10);
        const uint32_t crc = rd32(p + q + 16), csize = rd32(p + q + 20), usize = rd32(p + q + 24);
        const uint16_t nlen = rd16(p + q + 28), xlen = rd16(p + q + 30), clen = rd16(p + q + 32);
        const uint32_t loff = rd32(p + q + 42);
        if (q + 46 + nlen > n) { err = "zip: truncated name"; return false; }
        ArchiveEntry e;
        e.name.assign((const char*)p + q + 46, nlen);
        q += 46 + size_t(nlen) + xlen + clen;
        e.dir = !e.name.empty() && e.name.back() == '/';
        if (!archive_name_safe(e.name)) { err = "zip: unsafe entry path rejected: " + e.name.substr(0, 200); return false; }
        if (size_t(usize) > kArcMaxTotal - std::min(total, kArcMaxTotal)) { err = "zip: decoded size exceeds 1 GiB limit"; return false; }
        if (!e.dir) {
            if (flags & 1) { err = "zip: encrypted entry " + e.name; return false; }
            if (size_t(loff) + 30 > n || rd32(p + loff) != 0x04034b50u) { err = "zip: bad local header"; return false; }
            const size_t data = size_t(loff) + 30 + rd16(p + loff + 26) + rd16(p + loff + 28);
            if (data + csize > n) { err = "zip: truncated data"; return false; }
            if (method == 0) e.data.assign(p + data, p + data + csize);
            else if (method == 8) {
                if (!inflate_raw(p + data, csize, e.data, usize, err, nullptr, size_t(usize) + 1)) { err = "zip: " + e.name + ": " + err; return false; }
            } else { err = "zip: unsupported method " + std::to_string(method) + " (" + e.name + ")"; return false; }
            if (e.data.size() != usize || crc32_ieee(e.data.data(), e.data.size()) != crc) {
                err = "zip: CRC/size mismatch in " + e.name; return false;
            }
        }
        if (!check_entry(e, total, out.size(), err, "zip")) return false;
        out.push_back(std::move(e));
    }
    return true;
}

bool read_archive(const std::vector<uint8_t>& data, std::vector<ArchiveEntry>& out, std::string& err,
                  const std::string& gz_name) {
    switch (detect_archive(data.data(), data.size())) {
    case ArchiveKind::Zip: return read_zip(data.data(), data.size(), out, err);
    case ArchiveKind::Tar: return read_tar(data.data(), data.size(), out, err);
    case ArchiveKind::Gzip: {
        std::vector<uint8_t> raw;
        if (!gunzip(data.data(), data.size(), raw, err)) return false;
        if (detect_archive(raw.data(), raw.size()) == ArchiveKind::Tar) return read_tar(raw.data(), raw.size(), out, err);
        ArchiveEntry e; e.name = gz_name; e.data = std::move(raw);
        out.push_back(std::move(e));
        return true;
    }
    default: err = "not a recognised archive"; return false;
    }
}

static std::filesystem::path upath(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

// ---------------------------------------------------------------------------
std::string FileTree::key(const std::string& p) {
    std::string k;
    k.reserve(p.size());
    for (char c : p) k += c == '\\' ? '/' : (char)std::tolower((unsigned char)c);
    while (k.rfind("./", 0) == 0) k.erase(0, 2);
    while (!k.empty() && k[0] == '/') k.erase(0, 1);
    return k;
}

bool FileTree::open_entries(std::vector<ArchiveEntry> entries) {
    archive_ = true;
    files_.clear(); data_.clear();
    for (auto& e : entries) {
        if (e.dir) continue;
        std::string k = key(e.name);
        files_[k] = e.name;
        data_[k] = std::move(e.data);
    }
    return true;
}

bool FileTree::open(const std::string& path, std::string& err) {
    namespace fs = std::filesystem;
    std::error_code ec;
    files_.clear(); data_.clear();
    if (fs::is_directory(upath(path), ec)) {
        archive_ = false;
        root_ = path;
        size_t count = 0;
        for (auto it = fs::recursive_directory_iterator(upath(path), ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            auto u8 = fs::relative(it->path(), upath(path), ec).generic_u8string();
            std::string rel(u8.begin(), u8.end());
            files_[key(rel)] = rel;
            if (++count > 200000) break;
        }
        return true;
    }
    std::ifstream f(upath(path), std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    if (fs::file_size(upath(path), ec) > kArcMaxTotal) { err = "archive larger than 1 GiB: " + path; return false; }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<ArchiveEntry> ents;
    if (!read_archive(buf, ents, err)) return false;
    return open_entries(std::move(ents));
}

bool FileTree::exists(const std::string& rel) const { return files_.count(key(rel)) != 0; }

bool FileTree::get(const std::string& rel, std::string& out) const {
    const std::string k = key(rel);
    auto it = files_.find(k);
    if (it == files_.end()) return false;
    if (archive_) {
        auto d = data_.find(k);
        out.assign(d->second.begin(), d->second.end());
        return true;
    }
    namespace fs = std::filesystem;
    std::ifstream f(upath(root_) / upath(it->second), std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

std::vector<std::string> FileTree::list() const {
    std::vector<std::string> v;
    for (auto& kv : files_) v.push_back(kv.second);
    return v;
}

} // namespace pcbp::io
