// ============================================================================
// job_archive.cpp — job archive zip reader / atomic rewriter (see job_archive.h).
// Written from PKWARE APPNOTE 6.3 (sections 4.3, 4.4 and 4.5.3 Zip64).
// ============================================================================
#include "job_archive.h"
#include "../io/archive.h"
#include "../io/deflate.h"
#include "../io/inflate.h"
#include "content_detect.h"
#include "../io/gerber.h"
#include "sha256.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace pcbp::io::jobzip {
namespace fs = std::filesystem;

const LegacyFolder kLegacyFolders[8] = {
    {"Single Circuit", Category::Original, nullptr},   {"Panelized Circuit", Category::Panel, nullptr},
    {"Bill of Materials", Category::Bom, nullptr},     {"Datasheets", Category::Other, "Datasheet"},
    {"Images", Category::Image, nullptr},              {"Emails", Category::Email, nullptr},
    {"Drawings", Category::Drawing, nullptr},          {"Other", Category::Other, nullptr}};

namespace {
fs::path upath(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t rd64(const uint8_t* p) { return uint64_t(rd32(p)) | uint64_t(rd32(p + 4)) << 32; }
void w16(std::vector<uint8_t>& b, uint32_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
void w32(std::vector<uint8_t>& b, uint32_t v) { w16(b, v & 0xFFFF); w16(b, v >> 16); }
void w64(std::vector<uint8_t>& b, uint64_t v) { w32(b, uint32_t(v)); w32(b, uint32_t(v >> 32)); }
std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string ext_of(const std::string& n) {
    size_t s = n.find_last_of("/\\"), d = n.find_last_of('.');
    if (d == std::string::npos || (s != std::string::npos && d < s)) return {};
    return lower(n.substr(d + 1));
}
std::string base_of(const std::string& n) { size_t s = n.find_last_of("/\\"); return s == std::string::npos ? n : n.substr(s + 1); }
std::string parent_of(const std::string& n) { size_t s = n.find_last_of('/'); return s == std::string::npos ? std::string() : n.substr(0, s + 1); }
std::string u8(const fs::path& p) { auto s = p.u8string(); return std::string(s.begin(), s.end()); }
bool any_of_ext(const std::string& e, std::initializer_list<const char*> l) { for (auto* x : l) if (e == x) return true; return false; }
bool has_word(const std::string& ln, std::initializer_list<const char*> l) { for (auto* x : l) if (ln.find(x) != std::string::npos) return true; return false; }
constexpr uint32_t k32 = 0xFFFFFFFFu;
} // namespace

// ---------------------------------------------------------------------------
// reader
// ---------------------------------------------------------------------------
bool Zip::read_at(uint64_t off, size_t n, std::vector<uint8_t>& out) const {
    out.clear();
    if (off > size_ || n > size_ - off) return false;
    if (in_mem_) { out.assign(mem_.begin() + (ptrdiff_t)off, mem_.begin() + (ptrdiff_t)(off + n)); return true; }
    std::ifstream f(upath(path_), std::ios::binary);
    if (!f) return false;
    f.seekg((std::streamoff)off);
    out.resize(n);
    f.read((char*)out.data(), (std::streamsize)n);
    return (size_t)f.gcount() == n;
}

bool Zip::open_file(const std::string& path, std::string& err) {
    path_ = path; in_mem_ = false; mem_.clear(); items_.clear(); zip64_ = false;
    std::error_code ec;
    size_ = fs::file_size(upath(path), ec);
    if (ec) { err = "cannot open " + path; return false; }
    return parse(err);
}
bool Zip::open_memory(std::vector<uint8_t> bytes, std::string& err) {
    path_.clear(); in_mem_ = true; mem_ = std::move(bytes); size_ = mem_.size(); items_.clear(); zip64_ = false;
    return parse(err);
}

bool Zip::parse(std::string& err) {
    if (size_ < 22) { err = "zip: too small"; return false; }
    const uint64_t tail_n = std::min<uint64_t>(size_, 22 + 65535 + 20);
    std::vector<uint8_t> t;
    if (!read_at(size_ - tail_n, (size_t)tail_n, t)) { err = "zip: read error"; return false; }
    size_t e = std::string::npos;
    for (size_t i = t.size() - 22 + 1; i-- > 0;)
        if (t[i] == 'P' && t[i + 1] == 'K' && t[i + 2] == 5 && t[i + 3] == 6) { e = i; break; }
    if (e == std::string::npos) { err = "zip: end of central directory not found"; return false; }
    uint64_t count = rd16(&t[e + 10]), cd_size = rd32(&t[e + 12]), cd_off = rd32(&t[e + 16]);
    if (e >= 20 && rd32(&t[e - 20]) == 0x07064b50u) {   // Zip64 locator
        const uint64_t z64 = rd64(&t[e - 20 + 8]);
        std::vector<uint8_t> r;
        if (!read_at(z64, 56, r) || rd32(r.data()) != 0x06064b50u) { err = "zip: bad Zip64 end record"; return false; }
        count = rd64(&r[32]); cd_size = rd64(&r[40]); cd_off = rd64(&r[48]);
        zip64_ = true;
    }
    if (cd_off > size_ || cd_size > size_ - cd_off || count > 2000000) { err = "zip: bad central directory"; return false; }
    std::vector<uint8_t> cd;
    if (!read_at(cd_off, (size_t)cd_size, cd)) { err = "zip: read error"; return false; }
    size_t q = 0;
    for (uint64_t k = 0; k < count; ++k) {
        if (q + 46 > cd.size() || rd32(&cd[q]) != 0x02014b50u) { err = "zip: bad central header"; return false; }
        const uint8_t* h = &cd[q];
        Item it;
        it.flags = rd16(h + 8); it.method = rd16(h + 10); it.mtime = rd16(h + 12); it.mdate = rd16(h + 14);
        it.crc = rd32(h + 16); it.csize = rd32(h + 20); it.usize = rd32(h + 24);
        const uint16_t nl = rd16(h + 28), xl = rd16(h + 30), cl = rd16(h + 32);
        it.lho = rd32(h + 42);
        if (q + 46 + size_t(nl) + xl + cl > cd.size()) { err = "zip: truncated central header"; return false; }
        it.name.assign((const char*)h + 46, nl);
        for (auto& c : it.name) if (c == '\\') c = '/';
        size_t x = q + 46 + nl, xe = x + xl;
        while (x + 4 <= xe) {
            const uint16_t id = rd16(&cd[x]), sz = rd16(&cd[x + 2]);
            if (x + 4 + sz > xe) break;
            if (id == 1) {
                size_t z = x + 4, ze = z + sz;
                if (it.usize == k32 && z + 8 <= ze) { it.usize = rd64(&cd[z]); z += 8; }
                if (it.csize == k32 && z + 8 <= ze) { it.csize = rd64(&cd[z]); z += 8; }
                if (it.lho == k32 && z + 8 <= ze) { it.lho = rd64(&cd[z]); z += 8; }
                zip64_ = true;
            }
            x += 4 + sz;
        }
        q += 46 + size_t(nl) + xl + cl;
        if (!archive_name_safe(it.name)) { err = "zip: unsafe entry path rejected: " + it.name.substr(0, 200); return false; }
        items_.push_back(std::move(it));
    }
    return true;
}

int Zip::find(const std::string& name) const {
    for (size_t i = 0; i < items_.size(); ++i) if (items_[i].name == name) return (int)i;
    const std::string l = lower(name);
    for (size_t i = 0; i < items_.size(); ++i) if (lower(items_[i].name) == l) return (int)i;
    return -1;
}

bool Zip::read_raw(const Item& it, std::vector<uint8_t>& raw, std::string& err) const {
    std::vector<uint8_t> lh;
    if (!read_at(it.lho, 30, lh) || rd32(lh.data()) != 0x04034b50u) { err = "zip: bad local header for " + it.name; return false; }
    const uint64_t data = it.lho + 30 + rd16(&lh[26]) + rd16(&lh[28]);
    if (it.csize > (uint64_t(1) << 32) * 4) { err = "zip: entry too large"; return false; }
    if (!read_at(data, (size_t)it.csize, raw)) { err = "zip: truncated data in " + it.name; return false; }
    return true;
}

bool Zip::read(const Item& it, std::vector<uint8_t>& out, std::string& err) const {
    out.clear();
    if (it.dir()) return true;
    if (it.flags & 1) { err = "zip: encrypted entry " + it.name; return false; }
    std::vector<uint8_t> raw;
    if (!read_raw(it, raw, err)) return false;
    if (it.method == 0) out = std::move(raw);
    else if (it.method == 8) {
        if (it.usize > (uint64_t(1) << 31)) { err = "zip: entry too large to open in memory"; return false; }
        if (!inflate_raw(raw.data(), raw.size(), out, (size_t)it.usize, err, nullptr, (size_t)it.usize + 1)) { err = "zip: " + it.name + ": " + err; return false; }
    } else { err = "zip: unsupported method " + std::to_string(it.method) + " (" + it.name + ")"; return false; }
    if (out.size() != it.usize || crc32_ieee(out.data(), out.size()) != it.crc) { err = "zip: CRC/size mismatch in " + it.name; return false; }
    return true;
}
bool Zip::read(const std::string& name, std::vector<uint8_t>& out, std::string& err) const {
    int i = find(name);
    if (i < 0) { err = "not in archive: " + name; return false; }
    return read(items_[(size_t)i], out, err);
}

// ---------------------------------------------------------------------------
// writer
// ---------------------------------------------------------------------------
bool should_store(const std::string& name) {
    return any_of_ext(ext_of(name), {"zip", "7z", "rar", "gz", "tgz", "bz2", "xz", "zst", "jar", "png", "jpg", "jpeg", "gif", "webp",
                                     "heic", "pdf", "docx", "xlsx", "xlsm", "pptx", "odt", "ods", "odp", "mp4", "mov", "avi", "mkv",
                                     "mp3", "m4a", "ogg", "flac", "cab", "3mf"});
}

namespace {
struct Out {
    std::ofstream f;
    uint64_t pos = 0;
    bool put(const std::vector<uint8_t>& b) { f.write((const char*)b.data(), (std::streamsize)b.size()); pos += b.size(); return (bool)f; }
};
struct Written { Item it; };
void dos_now(uint16_t& t, uint16_t& d) {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    t = uint16_t(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
    d = uint16_t((std::max(1980, tm.tm_year + 1900) - 1980) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
}
bool non_ascii(const std::string& s) { for (unsigned char c : s) if (c >= 0x80) return true; return false; }

bool write_entry(Out& o, Item it, const std::vector<uint8_t>& payload, bool force64, std::vector<Item>& done) {
    it.lho = o.pos;
    it.flags &= ~uint16_t(8);                       // sizes are known: no data descriptor
    if (non_ascii(it.name)) it.flags |= 0x800;      // UTF-8 names
    const bool z64 = force64 || it.usize >= k32 || it.csize >= k32;
    std::vector<uint8_t> h;
    w32(h, 0x04034b50u); w16(h, z64 ? 45 : 20); w16(h, it.flags); w16(h, it.method); w16(h, it.mtime); w16(h, it.mdate);
    w32(h, it.crc); w32(h, z64 ? k32 : uint32_t(it.csize)); w32(h, z64 ? k32 : uint32_t(it.usize));
    w16(h, (uint32_t)it.name.size()); w16(h, z64 ? 20 : 0);
    h.insert(h.end(), it.name.begin(), it.name.end());
    if (z64) { w16(h, 1); w16(h, 16); w64(h, it.usize); w64(h, it.csize); }
    if (!o.put(h) || !o.put(payload)) return false;
    done.push_back(it);
    return true;
}

bool finish(Out& o, const std::vector<Item>& items, bool force64) {
    const uint64_t cd_off = o.pos;
    std::vector<uint8_t> cd;
    bool any64 = force64;
    for (auto& it : items) {
        const bool u64 = force64 || it.usize >= k32, c64 = force64 || it.csize >= k32, l64 = force64 || it.lho >= k32;
        std::vector<uint8_t> x;
        if (u64 || c64 || l64) {
            any64 = true;
            w16(x, 1); w16(x, uint32_t((u64 + c64 + l64) * 8));
            if (u64) w64(x, it.usize);
            if (c64) w64(x, it.csize);
            if (l64) w64(x, it.lho);
        }
        w32(cd, 0x02014b50u); w16(cd, 0x033F /* made by: unix-ish, 6.3 */); w16(cd, x.empty() ? 20 : 45);
        w16(cd, it.flags); w16(cd, it.method); w16(cd, it.mtime); w16(cd, it.mdate); w32(cd, it.crc);
        w32(cd, c64 ? k32 : uint32_t(it.csize)); w32(cd, u64 ? k32 : uint32_t(it.usize));
        w16(cd, (uint32_t)it.name.size()); w16(cd, (uint32_t)x.size()); w16(cd, 0); w16(cd, 0); w16(cd, 0);
        w32(cd, it.dir() ? 0x41ED0010u : 0x81A40000u);   // external attrs: unix mode + DOS dir bit
        w32(cd, l64 ? k32 : uint32_t(it.lho));
        cd.insert(cd.end(), it.name.begin(), it.name.end());
        cd.insert(cd.end(), x.begin(), x.end());
    }
    if (!o.put(cd)) return false;
    const uint64_t cd_size = cd.size(), n = items.size();
    if (any64 || n >= 0xFFFF || cd_off >= k32 || cd_size >= k32) any64 = true;
    std::vector<uint8_t> t;
    if (any64) {
        const uint64_t z64 = o.pos;
        w32(t, 0x06064b50u); w64(t, 44); w16(t, 45); w16(t, 45); w32(t, 0); w32(t, 0);
        w64(t, n); w64(t, n); w64(t, cd_size); w64(t, cd_off);
        w32(t, 0x07064b50u); w32(t, 0); w64(t, z64); w32(t, 1);
    }
    w32(t, 0x06054b50u); w16(t, 0); w16(t, 0);
    w16(t, any64 ? 0xFFFF : uint32_t(n)); w16(t, any64 ? 0xFFFF : uint32_t(n));
    w32(t, any64 ? k32 : uint32_t(cd_size)); w32(t, any64 ? k32 : uint32_t(cd_off)); w16(t, 0);
    return o.put(t);
}

// new name of `name` under the rename map (exact or folder prefix); "" = unchanged
std::string renamed(const std::string& name, const std::map<std::string, std::string>& rn) {
    auto it = rn.find(name);
    if (it != rn.end()) return it->second;
    for (auto& [a, b] : rn) if (!a.empty() && a.back() == '/' && name.size() > a.size() && name.compare(0, a.size(), a) == 0) return b + name.substr(a.size());
    return {};
}
bool removed(const std::string& name, const std::set<std::string>& rm) {
    if (rm.count(name)) return true;
    for (auto& a : rm) if (!a.empty() && a.back() == '/' && name.compare(0, a.size(), a) == 0) return true;
    return false;
}
} // namespace

bool rewrite(const std::string& zip_path, const Edit& e, std::string& err) {
    Zip old;
    std::error_code ec;
    const bool exists = fs::exists(upath(zip_path), ec);
    if (exists && !old.open_file(zip_path, err)) return false;
    for (auto& a : e.add) if (!archive_name_safe(a.name)) { err = "unsafe name: " + a.name; return false; }
    for (auto& [a, b] : e.rename) if (!archive_name_safe(b)) { err = "unsafe name: " + b; return false; }
    const std::string tmp = zip_path + ".tmp";
    std::vector<Item> done;
    std::set<std::string> names;
    {
        Out o;
        o.f.open(upath(tmp), std::ios::binary | std::ios::trunc);
        if (!o.f) { err = "cannot write " + tmp; return false; }
        std::set<std::string> replaced;
        for (auto& a : e.add) replaced.insert(lower(a.name));
        for (auto& it0 : old.items()) {
            if (removed(it0.name, e.remove)) continue;
            Item it = it0;
            std::string nn = renamed(it.name, e.rename);
            if (!nn.empty()) it.name = nn;
            if (replaced.count(lower(it.name)) || names.count(lower(it.name))) continue;
            std::vector<uint8_t> raw;
            if (!it.dir() && !old.read_raw(it0, raw, err)) { o.f.close(); fs::remove(upath(tmp), ec); return false; }
            if (it.dir()) it.csize = it.usize = 0;
            if (!write_entry(o, it, raw, e.force_zip64, done)) { err = "write error"; o.f.close(); fs::remove(upath(tmp), ec); return false; }
            names.insert(lower(it.name));
        }
        uint16_t t, d; dos_now(t, d);
        for (auto& dn : e.add_dirs) {
            std::string n = dn; if (n.empty()) continue; if (n.back() != '/') n += '/';
            if (names.count(lower(n))) continue;
            Item it; it.name = n; it.mtime = t; it.mdate = d;
            if (!write_entry(o, it, {}, e.force_zip64, done)) { err = "write error"; return false; }
            names.insert(lower(n));
        }
        for (auto& a : e.add) {
            std::vector<uint8_t> data;
            const std::vector<uint8_t>* src = &a.data;
            if (!a.disk.empty()) {
                std::ifstream f(upath(a.disk), std::ios::binary);
                if (!f) { err = "cannot read " + a.disk; o.f.close(); fs::remove(upath(tmp), ec); return false; }
                data.assign(std::istreambuf_iterator<char>(f), {});
                src = &data;
            }
            Item it; it.name = a.name; it.mtime = t; it.mdate = d;
            it.usize = src->size(); it.crc = crc32_ieee(src->data(), src->size());
            std::vector<uint8_t> comp;
            if (!should_store(a.name) && !src->empty()) comp = deflate_raw(src->data(), src->size(), e.level);
            const bool def = !comp.empty() && comp.size() < src->size();
            it.method = def ? 8 : 0; it.csize = def ? comp.size() : src->size();
            if (!write_entry(o, it, def ? comp : *src, e.force_zip64, done)) { err = "write error"; o.f.close(); fs::remove(upath(tmp), ec); return false; }
            names.insert(lower(it.name));
        }
        if (!finish(o, done, e.force_zip64)) { err = "write error"; o.f.close(); fs::remove(upath(tmp), ec); return false; }
        o.f.close();
        if (!o.f) { err = "write error (disk full?)"; fs::remove(upath(tmp), ec); return false; }
    }
    {   // verify what we wrote before replacing anything
        Zip chk; std::string e2;
        if (!chk.open_file(tmp, e2) || chk.items().size() != done.size()) { err = "verification failed: " + e2; fs::remove(upath(tmp), ec); return false; }
    }
    if (exists) {
        fs::copy_file(upath(zip_path), upath(zip_path + ".bak"), fs::copy_options::overwrite_existing, ec);
        if (ec) { err = "cannot write backup " + zip_path + ".bak: " + ec.message(); fs::remove(upath(tmp), ec); return false; }
    }
    fs::rename(upath(tmp), upath(zip_path), ec);   // replaces the target (MoveFileEx REPLACE_EXISTING on Windows)
    if (ec) { err = "cannot replace " + zip_path + ": " + ec.message(); fs::remove(upath(tmp), ec); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// sniffing
// ---------------------------------------------------------------------------
bool is_zip_name(const std::string& n) { return ext_of(n) == "zip"; }
bool is_image_name(const std::string& n) { return any_of_ext(ext_of(n), {"png", "jpg", "jpeg", "gif", "bmp", "tif", "tiff", "webp"}); }

namespace {
struct Census { int gerber = 0, drill = 0, cad = 0, bom = 0, centroid = 0; };
void census_one(const std::string& name, const std::vector<uint8_t>& b, Census& c, int depth);
void census_zip(const std::vector<uint8_t>& bytes, Census& c, int depth) {
    if (depth > 2) return;
    Zip z; std::string err;
    if (!z.open_memory(bytes, err)) return;
    int n = 0;
    for (auto& it : z.items()) {
        if (it.dir() || ++n > 400) continue;
        if (it.usize > (64u << 20)) continue;
        std::vector<uint8_t> d;
        if (z.read(it, d, err)) census_one(it.name, d, c, depth + 1);
    }
}
void census_one(const std::string& name, const std::vector<uint8_t>& b, Census& c, int depth) {
    if (is_zip_name(name) || (b.size() > 4 && b[0] == 'P' && b[1] == 'K' && b[2] == 3 && b[3] == 4 && !any_of_ext(ext_of(name), {"xlsx", "xlsm", "docx", "pptx", "ods", "odt"}))) {
        census_zip(b, c, depth); return;
    }
    int score = 0; std::string imp;
    const std::string head((const char*)b.data(), std::min<size_t>(b.size(), 65536));
    switch (detect::classify(name, head, score, imp)) {
    case detect::Kind::Gerber: c.gerber++; break;
    case detect::Kind::Drill: c.drill++; break;
    case detect::Kind::Odb: case detect::Kind::Ipc2581: case detect::Kind::Cad: case detect::Kind::LegacyCpf: case detect::Kind::ViewMateJob: c.cad++; break;
    case detect::Kind::Bom: c.bom++; break;
    case detect::Kind::Centroid: c.centroid++; break;
    case detect::Kind::Spreadsheet: c.bom++; break;
    default: break;
    }
}
bool looks_email(const std::string& e, const std::string& head) {
    if (e == "eml" || e == "msg") return true;
    const std::string h = head.substr(0, 4096);
    int hits = 0;
    for (const char* k : {"\nFrom: ", "\nTo: ", "\nSubject: ", "\nMessage-ID: ", "\nReceived: "}) if (h.find(k) != std::string::npos || h.rfind(k + 1, 0) == 0) hits++;
    return hits >= 3;
}
bool looks_image(const std::vector<uint8_t>& b) {
    if (b.size() >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') return true;
    if (b.size() >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return true;
    if (b.size() >= 6 && (std::memcmp(b.data(), "GIF87a", 6) == 0 || std::memcmp(b.data(), "GIF89a", 6) == 0)) return true;
    if (b.size() >= 2 && b[0] == 'B' && b[1] == 'M' && b.size() > 54) return true;
    return false;
}
} // namespace

// ---------------------------------------------------------------------------
// R22 categories
// ---------------------------------------------------------------------------
namespace {
const char* const kCatLabel[kCategoryCount] = {"Original Circuit", "Panel Gerbers", "Stencil", "CAD", "BOM", "Machine Programs",
                                               "Schematic", "Drawing", "Image", "Email / Correspondence", "Notes", "Other"};
const char* const kCatFolder[kCategoryCount] = {"Original Circuit", "Panel Gerbers", "Stencil", "CAD", "BOM", "Machine Programs",
                                                "Schematic", "Drawing", "Image", "Email", "Notes", "Other"};
int ci(Category c) { const int i = (int)c; return i >= 0 && i < kCategoryCount ? i : kCategoryCount - 1; }
} // namespace

const char* category_label(Category c) { return kCatLabel[ci(c)]; }
const char* category_folder(Category c) { return kCatFolder[ci(c)]; }

Category category_from_name(const std::string& s0, bool* known) {
    const std::string s = lower(s0);
    if (known) *known = true;
    for (int i = 0; i < kCategoryCount; ++i)
        if (s == lower(kCatLabel[i]) || s == lower(kCatFolder[i])) return (Category)i;
    for (auto& l : kLegacyFolders) if (s == lower(l.name)) return l.to;
    if (s == "email" || s == "correspondence" || s == "emails") return Category::Email;
    if (s == "bill of materials" || s == "boms") return Category::Bom;
    if (s == "programs" || s == "machine program" || s == "machine") return Category::Programs;
    if (known) *known = false;
    return Category::Other;
}

const LegacyFolder* legacy_folder(const std::string& top) {
    for (auto& l : kLegacyFolders) if (top == l.name && std::strcmp(l.name, "Other") != 0) return &l;   // "Other" is current too
    return nullptr;
}

std::string top_folder(const std::string& path) {
    const size_t sl = path.find('/');
    return sl == std::string::npos ? std::string() : path.substr(0, sl);
}

Category category_of_path(const std::string& path, bool* known) {
    const std::string top = top_folder(path);
    if (known) *known = false;
    if (top.empty()) return Category::Other;
    for (int i = 0; i < kCategoryCount; ++i) if (top == kCatFolder[i]) { if (known) *known = true; return (Category)i; }
    if (const LegacyFolder* l = legacy_folder(top)) { if (known) *known = true; return l->to; }
    return Category::Other;
}

std::string path_in_category(const std::string& path, Category c) {
    bool known = false;
    category_of_path(path, &known);
    const std::string dst = category_folder(c);
    if (known) return dst + path.substr(top_folder(path).size());
    std::string p = path;
    const bool dir = !p.empty() && p.back() == '/';
    if (dir) p.pop_back();
    return dst + "/" + base_of(p) + (dir ? "/" : "");
}

std::vector<std::string> legacy_folders_in(const Zip& z) {
    std::vector<std::string> v;
    for (auto& it : z.items()) {
        const std::string top = top_folder(it.name);
        if (!top.empty() && legacy_folder(top) && std::find(v.begin(), v.end(), top) == v.end()) v.push_back(top);
    }
    return v;
}

bool plan_folder_migration(const Zip& z, Edit& e) {
    std::set<std::string> taken;   // lower-case final names
    std::vector<const Item*> moving;
    for (auto& it : z.items()) {
        const std::string top = top_folder(it.name);
        if (!top.empty() && legacy_folder(top)) moving.push_back(&it);
        else taken.insert(lower(it.name));
    }
    if (moving.empty()) return false;
    for (const Item* it : moving) {
        const LegacyFolder* l = legacy_folder(top_folder(it->name));
        std::string to = category_folder(l->to) + it->name.substr(std::strlen(l->name));
        // A folder entry whose new name exists already is renamed anyway: rewrite() keeps the first entry of a name,
        // and both are folder entries. (Never Edit::remove a folder: that removes everything below it.)
        if (!it->dir() && taken.count(lower(to))) {
            const std::string par = to.substr(0, to.find_last_of('/'));
            to = unique_name(nullptr, par, base_of(to), taken);
        }
        taken.insert(lower(to));
        e.rename[it->name] = to;
    }
    return true;
}

// ---------------------------------------------------------------------------
// R22 sniffing
// ---------------------------------------------------------------------------
int count_outline_loops(const std::string& text, int* repeated) {
    if (repeated) *repeated = 0;
    gerber::Layer L; gerber::ParseOptions o; std::string err;
    if (!gerber::parse(text, o, L, err)) return 0;
    // chain Draw / Arc segments by shared end points (union-find on quantised points, 1 um), regions are loops
    std::map<std::pair<long long, long long>, int> pid;
    std::vector<int> par;
    auto id = [&](Vec2 p) {
        const auto k = std::make_pair((long long)std::llround(p.x * 1000), (long long)std::llround(p.y * 1000));
        auto it = pid.find(k);
        if (it != pid.end()) return it->second;
        const int n = (int)par.size(); par.push_back(n); pid[k] = n; return n;
    };
    auto root = [&](int i) { while (par[(size_t)i] != i) { par[(size_t)i] = par[(size_t)par[(size_t)i]]; i = par[(size_t)i]; } return i; };
    struct Seg { int a, b; BBox bb; };
    std::vector<Seg> segs;
    std::vector<BBox> loops;
    for (auto& ob : L.objs) {
        if (ob.clear) continue;
        if (ob.kind == gerber::Object::Region) { BBox bb; for (auto& p : ob.poly) bb.add(p); if (!bb.empty()) loops.push_back(bb); continue; }
        if (ob.kind != gerber::Object::Draw && ob.kind != gerber::Object::Arc) continue;
        BBox bb; bb.add(ob.a); bb.add(ob.b);
        if (ob.kind == gerber::Object::Arc) for (auto& p : gerber::arc_points(ob, 36)) bb.add(p);
        const int a = id(ob.a), b = id(ob.b);
        segs.push_back({a, b, bb});
        if (ob.kind == gerber::Object::Arc && a == b) continue;   // full circle: its own loop
        par[(size_t)root(a)] = root(b);
    }
    std::map<int, BBox> comp;
    std::map<int, int> deg_odd;   // open chains are not loops: count end points used an odd number of times
    std::map<int, int> deg;
    for (auto& sg : segs) { deg[sg.a]++; deg[sg.b]++; }
    for (auto& sg : segs) { BBox& c = comp[root(sg.a)]; c.add(Vec2{sg.bb.x0, sg.bb.y0}); c.add(Vec2{sg.bb.x1, sg.bb.y1}); }
    for (auto& [p, d] : deg) if (d % 2) deg_odd[root(p)]++;
    for (auto& [r, bb] : comp) if (!deg_odd.count(r)) loops.push_back(bb);
    BBox all; for (auto& b : loops) { all.add(Vec2{b.x0, b.y0}); all.add(Vec2{b.x1, b.y1}); }
    if (all.empty()) return 0;
    const double big = std::max(all.w(), all.h());
    std::vector<BBox> keep;
    for (auto& b : loops) if (std::max(b.w(), b.h()) >= 0.10 * big && std::min(b.w(), b.h()) > 0.5) keep.push_back(b);
    int best = 0;
    for (size_t i = 0; i < keep.size(); ++i) {
        int n = 0;
        for (size_t j = 0; j < keep.size(); ++j) {
            const double tol = std::max(0.05, 0.005 * std::max(keep[i].w(), keep[i].h()));
            auto eq = [&](double a, double b) { return std::fabs(a - b) <= tol; };
            if ((eq(keep[i].w(), keep[j].w()) && eq(keep[i].h(), keep[j].h())) || (eq(keep[i].w(), keep[j].h()) && eq(keep[i].h(), keep[j].w()))) ++n;
        }
        best = std::max(best, n);
    }
    if (repeated) *repeated = best >= 2 ? best : 0;
    return (int)keep.size();
}

namespace {
bool name_panel(const std::string& ln) { return has_word(ln, {"panel", "array", "pnl", "panelized", "panelised", "palette", "nutzen"}); }
bool name_stencil(const std::string& ln) { return has_word(ln, {"stencil", "sten", "schablone"}); }
bool name_schematic(const std::string& ln) { return has_word(ln, {"schematic", "schem", "sch_", "_sch", "-sch", " sch", "sch.", "circuit diagram", "wiring"}) || ln.rfind("sch", 0) == 0; }
bool name_drawing(const std::string& ln) { return has_word(ln, {"drawing", "dwg", "assy", "assembly", "fab", "print", "mech", "outline", "panel", "paste", "drill", "layout"}); }
bool name_datasheet(const std::string& ln) { return has_word(ln, {"datasheet", "data sheet", "_ds", "-ds", " ds."}); }
// JUKI programs: HLC master .h8h / .h7h / .h5h / .h4h and machine splits .h81..; ISS .iss and splits .i01..; per-machine .e41..
bool machine_program_name(const std::string& name) {
    const std::string e = ext_of(name);
    if (e == "iss" || e == "h8h" || e == "h7h" || e == "h5h" || e == "h4h") return true;
    auto digits = [&](size_t from) { for (size_t i = from; i < e.size(); ++i) if (!std::isdigit((unsigned char)e[i])) return false; return e.size() > from; };
    return e.size() == 3 && ((e[0] == 'h' && digits(1)) || (e[0] == 'i' && digits(1)) || (e[0] == 'e' && digits(1)));
}
bool name_notes(const std::string& ln) { return has_word(ln, {"note", "readme", "instruction", "procedure", "todo", "changelog"}); }

// one Gerber-set member: layer roles + outline contours
void gerber_member(const std::string& name, const std::vector<uint8_t>& b, GerberTraits& g, int depth) {
    const std::string e = ext_of(name);
    if ((is_zip_name(name) || (b.size() > 4 && b[0] == 'P' && b[1] == 'K' && b[2] == 3 && b[3] == 4)) && !any_of_ext(e, {"xlsx", "xlsm", "docx", "pptx", "ods", "odt"})) {
        if (depth > 2) return;
        Zip z; std::string err;
        if (!z.open_memory(b, err)) return;
        int n = 0;
        for (auto& it : z.items()) {
            if (it.dir() || ++n > 400 || it.usize > (64u << 20)) continue;
            std::vector<uint8_t> d;
            if (z.read(it, d, err)) gerber_member(it.name, d, g, depth + 1);
        }
        return;
    }
    int score = 0; std::string imp;
    const std::string head((const char*)b.data(), std::min<size_t>(b.size(), 65536));
    const detect::Kind k = detect::classify(name, head, score, imp);
    if (k == detect::Kind::Drill) { g.drill++; return; }
    if (k != detect::Kind::Gerber) return;
    g.layers++;
    const std::string ln = lower(base_of(name));
    if (name_stencil(ln)) g.name_stencil = true;
    const gerber::LayerGuess lg = gerber::guess_layer_function(name, head);
    switch (lg.role) {
    case 1: g.copper++; break;
    case 2: g.paste++; break;
    case 3: g.mask++; break;
    case 4: g.silk++; break;
    case 6: g.drill++; g.layers--; break;
    case 7: {
        g.outline++;
        int rep = 0;
        const std::string txt(b.begin(), b.end());
        g.outline_loops += count_outline_loops(txt, &rep);
        g.repeated_outlines = std::max(g.repeated_outlines, rep);
    } break;
    default: g.other++; break;
    }
}

// decide among Original / Panel / Stencil for a Gerber set
void gerber_decide(Proposal& p, const std::string& set_name) {
    const std::string ln = lower(set_name);
    p.gerber_set = true;
    p.g.name_panel |= name_panel(ln);
    p.g.name_stencil |= name_stencil(ln);
    const GerberTraits& g = p.g;
    const int art = g.copper + g.mask + g.silk;
    char b[160];
    if (g.name_stencil || (g.paste > 0 && art == 0)) {
        p.cat = Category::Stencil;
        p.why = g.name_stencil ? "Gerber set, name says stencil" : "Gerber set with paste layers only: stencil";
    } else if (g.name_panel || g.repeated_outlines >= 2) {
        p.cat = Category::Panel;
        if (g.repeated_outlines >= 2) { std::snprintf(b, sizeof b, "Gerber set, outline repeats %d circuits: panel", g.repeated_outlines); p.why = b; }
        else p.why = "Gerber set, name says panel / array";
    } else {
        p.cat = Category::Original;
        std::snprintf(b, sizeof b, "Gerber set (%d layer%s%s), one circuit outline", g.layers, g.layers == 1 ? "" : "s", g.drill ? " + drill" : "");
        p.why = b;
    }
}
} // namespace

Proposal propose_category(const std::string& name, const std::vector<uint8_t>& bytes) {
    Proposal p;
    const std::string e = ext_of(name), ln = lower(base_of(name));
    const std::string head((const char*)bytes.data(), std::min<size_t>(bytes.size(), 65536));
    if (looks_email(e, head)) { p.cat = Category::Email; p.why = e == "eml" || e == "msg" ? "saved e-mail" : "e-mail headers"; return p; }
    if (is_image_name(name) || looks_image(bytes)) { p.cat = Category::Image; p.why = "image"; return p; }
    if (e == "pdf" || head.rfind("%PDF-", 0) == 0) {
        if (name_schematic(ln)) { p.cat = Category::Schematic; p.why = "PDF, name says schematic"; return p; }
        if (name_datasheet(ln)) { p.cat = Category::Other; p.tags = {"Datasheet"}; p.why = "component datasheet"; return p; }
        if (name_drawing(ln)) { p.cat = Category::Drawing; p.why = "PDF, name says drawing"; return p; }
        if (lower(head).find("schematic") != std::string::npos) { p.cat = Category::Schematic; p.why = "PDF titled schematic"; return p; }
        p.cat = Category::Drawing; p.why = "PDF document (fabrication / assembly drawing)"; return p;
    }
    if (e == "md" || e == "markdown") { p.cat = Category::Notes; p.why = "Markdown note"; return p; }   // .txt: census first (NC drill / centroid exports)
    if (any_of_ext(e, {"dxf", "dwg", "step", "stp", "igs", "iges"})) { p.cat = Category::Drawing; p.why = "mechanical drawing / model"; return p; }
    if (any_of_ext(e, {"docx", "doc", "rtf", "odt"})) {
        if (name_schematic(ln)) { p.cat = Category::Schematic; p.why = "document, name says schematic"; return p; }
        if (has_word(ln, {"bom", "bill of mat"})) { p.cat = Category::Bom; p.why = "bill of materials"; return p; }
        if (name_drawing(ln) && !name_notes(ln)) { p.cat = Category::Drawing; p.why = "document, name says drawing"; return p; }
        p.cat = Category::Notes; p.why = "document"; return p;
    }
    if (machine_program_name(name)) { p.cat = Category::Programs; p.why = "machine program"; return p; }
    Census c;
    census_one(name, bytes, c, 0);
    if (c.gerber + c.drill >= 1) {
        gerber_member(name, bytes, p.g, 0);
        gerber_decide(p, base_of(name));
        if (is_zip_name(name)) p.why = "zip: " + p.why;
        return p;
    }
    if (c.cad || c.centroid || any_of_ext(e, {"tgz", "cpf", "cpj", "brd", "pcbdoc", "kicad_pcb"})) {
        p.cat = Category::Cad;
        p.why = c.cad ? "CAD data" : c.centroid ? "placement (centroid) data" : "CAD file (." + e + ")";
        if (e == "tgz" && !c.cad) p.why = "ODB++ / CAD archive (.tgz)";
        return p;
    }
    if (c.bom || has_word(ln, {"bom", "bill of mat"})) { p.cat = Category::Bom; p.why = "bill of materials"; return p; }
    if (any_of_ext(e, {"txt", "text", "log"}) || (e.empty() && !head.empty() && head.find('\0') == std::string::npos && name_notes(ln))) {
        p.cat = Category::Notes; p.why = "text note"; return p;
    }
    if (is_zip_name(name)) { p.cat = Category::Other; p.why = "zip without Gerber data"; return p; }
    if (any_of_ext(e, {"xlsx", "xls", "xlsm", "ods", "csv"})) { p.cat = Category::Other; p.why = "spreadsheet"; return p; }
    p.cat = Category::Other; p.why = e.empty() ? "unknown type" : "unrecognised ." + e + " file";
    return p;
}

Proposal propose_category_set(const std::string& folder_name, const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
    Proposal p;
    int gerbers = 0;
    std::map<int, int> votes;
    std::map<std::string, int> tagv;
    size_t n = 0;
    for (auto& [rel, b] : files) {
        Census c; census_one(rel, b, c, 0);
        if (c.gerber + c.drill) { ++gerbers; gerber_member(rel, b, p.g, 0); continue; }
        if (++n > 64) continue;
        Proposal q = propose_category(rel, b);
        votes[(int)q.cat]++;
        for (auto& t : q.tags) tagv[t]++;
    }
    if (gerbers) {
        gerber_decide(p, folder_name);
        char b[64]; std::snprintf(b, sizeof b, "folder (%d files): ", (int)files.size());
        p.why = b + p.why;
        return p;
    }
    int best = -1;
    for (auto& [cat, v] : votes) if (v > best) { best = v; p.cat = (Category)cat; }
    for (auto& [t, v] : tagv) if (v * 2 > (int)std::max<size_t>(1, std::min<size_t>(n, 64))) p.tags.push_back(t);
    if (best < 0) { p.cat = Category::Other; p.why = "empty folder"; return p; }
    char b[96]; std::snprintf(b, sizeof b, "folder (%d files), mostly %s", (int)files.size(), category_label(p.cat));
    p.why = b;
    return p;
}

std::string sniff_folder(const std::string& name, const std::vector<uint8_t>& bytes) { return category_folder(propose_category(name, bytes).cat); }

std::vector<uint8_t> sniff_head(const std::string& path) {
    std::vector<uint8_t> b;
    std::ifstream f(upath(path), std::ios::binary);
    if (!f) return b;
    std::error_code ec;
    const uint64_t n = fs::file_size(upath(path), ec);
    if (ec) return b;
    const bool whole = is_zip_name(path) && n < (256u << 20);
    b.resize(whole ? (size_t)n : (size_t)std::min<uint64_t>(n, 65536));
    f.read((char*)b.data(), (std::streamsize)b.size());
    b.resize((size_t)f.gcount());
    return b;
}

// ---------------------------------------------------------------------------
// R22 drop confirm sheet
// ---------------------------------------------------------------------------
int DropSheet::included_files() const { int n = 0; for (auto& r : rows) if (r.include) n += r.files(); return n; }
void DropSheet::set_all(Category c) { for (auto& r : rows) r.cat = c; }

bool DropSheet::build(const Zip* z, Edit& e, std::map<std::string, std::vector<std::string>>* tags, std::map<int, int>* per_cat,
                      std::vector<std::string>* skipped) const {
    std::set<std::string> taken;
    // one file at its archive name: a new name; an existing one with other content -> versioned; same content -> skipped
    auto place = [&](const std::string& want, const std::string& folder, const std::string& disk, const DropRow& r) {
        int k = -1;
        if (keep_versions && z && !taken.count(lower(want))) k = z->find(want);
        std::string n;
        if (k >= 0) {
            const Item& it = z->items()[(size_t)k];
            uint64_t sz = 0;
            uint32_t c = 0;
            if (!it.dir() && file_crc(disk, sz, c) && sz == it.usize && c == it.crc) {
                if (skipped) skipped->push_back(it.name);
                return false;
            }
            const std::string vp = version_path(it, z, taken);
            e.rename[it.name] = vp;
            taken.insert(lower(vp));
            n = it.name;
        } else if (keep_versions && (!z || z->find(want) < 0) && !taken.count(lower(want))) {
            n = want;
        } else {
            n = unique_name(z, folder, base_of(want), taken);
        }
        taken.insert(lower(n));
        e.add.push_back({n, {}, disk});
        if (tags && !r.p.tags.empty()) (*tags)[n] = r.p.tags;
        return true;
    };
    for (auto& r : rows) {
        if (!r.include) continue;
        const std::string top = category_folder(r.cat);
        int put = 0;
        if (r.folder) {
            // versions: the folder merges into an existing one of the same name; else a new unique folder
            const std::string pre = keep_versions ? top + "/" + sanitize_file_name(r.name) : unique_name(z, top, sanitize_file_name(r.name), taken);
            for (auto& [disk, rel] : r.members) {
                std::string rr = rel; for (auto& ch : rr) if (ch == '\\') ch = '/';
                const std::string n = pre + "/" + rr;
                const size_t sl = n.find_last_of('/');
                if (keep_versions) put += place(n, n.substr(0, sl), disk, r);
                else { taken.insert(lower(n)); e.add.push_back({n, {}, disk}); if (tags && !r.p.tags.empty()) (*tags)[n] = r.p.tags; ++put; }
            }
            taken.insert(lower(pre + "/"));
            if (r.members.empty() && (!z || z->find(pre + "/") < 0)) e.add_dirs.push_back(pre + "/");
        } else {
            put += place(top + "/" + sanitize_file_name(r.name), top, r.disk, r);
        }
        if (per_cat && put) (*per_cat)[(int)r.cat] += put;
    }
    return !e.add.empty() || !e.add_dirs.empty();
}

// --- R25 versions ---------------------------------------------------------------------------------------------
bool is_version_path(const std::string& path) { return path.rfind(kVersionsFolder, 0) == 0; }

std::string version_path(const Item& old, const Zip* z, const std::set<std::string>& taken) {
    const std::string dir = parent_of(old.name), fn = base_of(old.name);
    const size_t d = fn.find_last_of('.');
    const std::string stem = d == std::string::npos || d == 0 ? fn : fn.substr(0, d), ext = d == std::string::npos || d == 0 ? "" : fn.substr(d);
    char st[32] = "undated";
    if (old.mdate)
        std::snprintf(st, sizeof st, "%04d-%02d-%02d %02d%02d", 1980 + (old.mdate >> 9), (old.mdate >> 5) & 15, old.mdate & 31, old.mtime >> 11,
                      (old.mtime >> 5) & 63);
    const std::string base = std::string(kVersionsFolder) + dir + stem + " @" + st;
    for (int i = 1; i < 10000; ++i) {
        const std::string n = base + (i == 1 ? std::string() : " (" + std::to_string(i) + ")") + ext;
        if ((!z || z->find(n) < 0) && !taken.count(lower(n))) return n;
    }
    return base + ext;
}

std::string version_of(const std::string& path) {
    if (!is_version_path(path)) return std::string();
    const std::string rest = path.substr(std::strlen(kVersionsFolder));
    const std::string dir = parent_of(rest), fn = base_of(rest);
    const size_t at = fn.rfind(" @");
    if (at == std::string::npos) return std::string();
    const size_t d = fn.find_last_of('.');
    const std::string ext = d == std::string::npos || d < at ? "" : fn.substr(d);
    return dir + fn.substr(0, at) + ext;
}

std::vector<const Item*> versions_of(const Zip& z, const std::string& path) {
    std::vector<const Item*> v;
    for (auto& it : z.items()) if (!it.dir() && version_of(it.name) == path) v.push_back(&it);
    std::sort(v.begin(), v.end(), [](const Item* a, const Item* b) {
        if (a->mdate != b->mdate) return a->mdate > b->mdate;
        if (a->mtime != b->mtime) return a->mtime > b->mtime;
        return a->name > b->name;
    });
    return v;
}

bool plan_restore(const Zip& z, const std::string& version, Edit& e) {
    const std::string cur = version_of(version);
    if (cur.empty() || z.find(version) < 0) return false;
    const int k = z.find(cur);
    std::set<std::string> taken = {lower(version)};
    if (k >= 0) e.rename[z.items()[(size_t)k].name] = version_path(z.items()[(size_t)k], &z, taken);
    e.rename[version] = cur;
    return true;
}

void carry_versions(const Zip& z, Edit& e) {
    const std::string V = kVersionsFolder;
    std::set<std::string> rm;
    for (auto& r : e.remove) {
        if (is_version_path(r)) continue;
        const bool folder = !r.empty() && r.back() == '/';
        for (auto& it : z.items()) {
            if (!is_version_path(it.name)) continue;
            const std::string cur = version_of(it.name);
            if (folder ? (it.name.rfind(V + r, 0) == 0) : cur == r) rm.insert(it.name);
        }
    }
    std::map<std::string, std::string> mv;
    for (auto& [from, to] : e.rename) {
        if (is_version_path(from) || is_version_path(to)) continue;
        const bool folder = !from.empty() && from.back() == '/';
        for (auto& it : z.items()) {
            if (!is_version_path(it.name) || e.rename.count(it.name) || rm.count(it.name)) continue;
            if (folder) {
                if (it.name.rfind(V + from, 0) == 0) mv[it.name] = V + to + it.name.substr(V.size() + from.size());
            } else if (version_of(it.name) == from) {
                // keep the version's own stamp: ".versions/<new dir><new stem> @<stamp><ext>"
                const std::string fn = base_of(it.name);
                const size_t at = fn.rfind(" @");
                const std::string tfn = base_of(to);
                const size_t d = tfn.find_last_of('.');
                const std::string stem = d == std::string::npos || d == 0 ? tfn : tfn.substr(0, d), ext = d == std::string::npos || d == 0 ? "" : tfn.substr(d);
                const size_t fd = fn.find_last_of('.');
                const std::string stamp = fn.substr(at, (fd == std::string::npos || fd < at ? fn.size() : fd) - at);
                mv[it.name] = V + parent_of(to) + stem + stamp + ext;
            }
        }
    }
    for (auto& x : rm) e.remove.insert(x);
    for (auto& [a, b] : mv) e.rename[a] = b;
}

bool file_crc(const std::string& disk, uint64_t& size, uint32_t& crc) {
    std::ifstream f(upath(disk), std::ios::binary);
    if (!f) return false;
    std::vector<char> b(1 << 16);
    size = 0;
    crc = 0;
    while (f) {
        f.read(b.data(), (std::streamsize)b.size());
        const std::streamsize n = f.gcount();
        if (n > 0) { crc = crc32_ieee((const uint8_t*)b.data(), (size_t)n, crc); size += (uint64_t)n; }
    }
    return !f.bad();
}

// --- R25 pack the project folder -----------------------------------------------------------------------------
namespace {
bool junk_name(const std::string& fn) {
    const std::string l = lower(fn);
    return l == "thumbs.db" || l == "desktop.ini" || l == ".ds_store" || l.rfind("~$", 0) == 0 || l.rfind(".~lock.", 0) == 0;
}
bool project_own(const std::string& fn, const std::string& archive_name) {
    const std::string l = lower(fn), e = ext_of(fn);
    if (e == "pcbp" || l.find(".pcbp.") != std::string::npos) return true;          // project, autosave, backup
    if (!archive_name.empty() && (l == lower(archive_name) || l == lower(archive_name) + ".bak" || l.rfind(lower(archive_name) + ".tmp", 0) == 0)) return true;
    return e == "tmp";
}
}  // namespace

PackPlan plan_pack(const std::string& dir, const std::vector<std::string>& projects, const std::string& archive_name, const Zip* z) {
    PackPlan plan;
    std::error_code ec;
    std::set<std::pair<uint64_t, uint32_t>> have;   // content the archive holds
    if (z) for (auto& it : z->items()) if (!it.dir()) have.insert({it.usize, it.crc});
    auto in_archive = [&](const std::string& disk) {
        uint64_t sz = 0;
        uint32_t c = 0;
        return !have.empty() && file_crc(disk, sz, c) && have.count({sz, c});
    };
    std::vector<std::pair<std::string, std::string>> top;   // (name, disk), sorted
    for (auto it = fs::directory_iterator(upath(dir), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        auto fn = it->path().filename().u8string(), d = it->path().u8string();
        top.push_back({std::string(fn.begin(), fn.end()), std::string(d.begin(), d.end())});
    }
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return lower(a.first) < lower(b.first); });
    std::vector<std::string> paths;
    for (auto& [fn, disk] : top) {
        if (fs::is_directory(upath(disk), ec)) {
            if (fn.empty() || fn[0] == '.' || lower(fn) == lower(kPackedFolder)) continue;
            paths.push_back(disk);
            continue;
        }
        if (!fs::is_regular_file(upath(disk), ec) || junk_name(fn) || project_own(fn, archive_name)) continue;
        if (machine_program_name(fn)) { plan.stays.push_back({disk, fn, "machine program: the machines read it from the folder"}); continue; }
        SideFile sf;
        if (classify_side(fn, projects, archive_name, sf)) {
            static const std::set<std::string> side_ext = {"cpf", "cmp", "osl", "mmd", "err", "tbl"};
            const char* why = sf.kind == SideKind::WorkOrder ? "M1 work order / traveller (TrayTagPrinter reads it)"
                            : sf.kind == SideKind::AoiImage ? "AOI board picture (TrayTagPrinter's board viewer)"
                            : side_ext.count(ext_of(fn)) ? "machine side file of the project" : nullptr;
            if (why) { plan.stays.push_back({disk, fn, why}); continue; }
        }
        if (in_archive(disk)) { plan.already.push_back({disk, fn, "the archive has the same file"}); continue; }
        paths.push_back(disk);
    }
    plan.sheet = make_drop_sheet(paths, -1);
    // folders: leave out temp / lock files and members the archive already has
    std::vector<DropRow> rows;
    for (auto& r : plan.sheet.rows) {
        if (r.folder) {
            std::vector<std::pair<std::string, std::string>> keep;
            for (auto& m : r.members) {
                if (junk_name(base_of(m.second))) continue;
                if (in_archive(m.first)) { plan.already.push_back({m.first, r.name + "/" + m.second, "the archive has the same file"}); continue; }
                keep.push_back(m);
            }
            if (keep.empty()) continue;
            r.members = std::move(keep);
        }
        rows.push_back(std::move(r));
    }
    plan.sheet.rows = std::move(rows);
    return plan;
}

bool verify_packed(const Zip& z, const Edit& e, std::string& err) {
    for (auto& a : e.add) {
        if (a.disk.empty()) continue;
        const int k = z.find(a.name);
        uint64_t sz = 0;
        uint32_t c = 0;
        if (k < 0) { err = a.name + ": not in the archive"; return false; }
        if (!file_crc(a.disk, sz, c)) { err = a.disk + ": cannot be read"; return false; }
        const Item& it = z.items()[(size_t)k];
        if (it.usize != sz || it.crc != c) { err = a.name + ": differs from " + a.disk; return false; }
        std::vector<uint8_t> d;   // the stored data decodes to that CRC
        if (!z.read(it, d, err)) { err = a.name + ": " + err; return false; }
    }
    return true;
}

int move_packed(const std::string& dir, const std::vector<std::string>& disk_paths, std::vector<std::string>& failed) {
    std::error_code ec;
    const fs::path root = upath(dir), dst = root / upath(kPackedFolder);
    int n = 0;
    std::set<fs::path> parents;
    for (auto& d : disk_paths) {
        const fs::path src = upath(d);
        fs::path rel = fs::relative(src, root, ec);
        if (ec || rel.empty() || rel.native()[0] == '.') { failed.push_back(d); ec.clear(); continue; }
        fs::path to = dst / rel;
        fs::create_directories(to.parent_path(), ec);
        for (int i = 2; fs::exists(to, ec) && i < 10000; ++i)
            to = dst / rel.parent_path() / upath(u8(rel.stem()) + " (" + std::to_string(i) + ")" + u8(rel.extension()));
        fs::rename(src, to, ec);
        if (ec) { failed.push_back(d); ec.clear(); continue; }
        ++n;
        for (fs::path p = src.parent_path(); p != root && p.has_parent_path() && p.string().size() > root.string().size(); p = p.parent_path()) parents.insert(p);
    }
    // empty source folders (deepest first)
    std::vector<fs::path> ps(parents.begin(), parents.end());
    std::sort(ps.begin(), ps.end(), [](const fs::path& a, const fs::path& b) { return a.string().size() > b.string().size(); });
    for (auto& p : ps) if (fs::is_directory(p, ec) && fs::is_empty(p, ec)) fs::remove(p, ec);
    return n;
}

DropSheet make_drop_sheet(const std::vector<std::string>& paths, int target) {
    DropSheet sh;
    sh.target = target;
    for (auto& p : paths) {
        std::error_code ec;
        DropRow r;
        r.disk = p;
        { auto fn = upath(p).filename().u8string(); r.name.assign(fn.begin(), fn.end()); }
        if (r.name.empty()) { auto fn = upath(p).parent_path().filename().u8string(); r.name.assign(fn.begin(), fn.end()); }
        if (fs::is_directory(upath(p), ec)) {
            r.folder = true;
            std::vector<std::pair<std::string, std::vector<uint8_t>>> heads;
            for (auto it = fs::recursive_directory_iterator(upath(p), ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                auto d = it->path().u8string(), rel = fs::relative(it->path(), upath(p), ec).u8string();
                std::string ds(d.begin(), d.end()), rs(rel.begin(), rel.end());
                for (auto& ch : rs) if (ch == '\\') ch = '/';
                r.members.push_back({ds, rs});
                if (heads.size() < 200) heads.push_back({rs, sniff_head(ds)});
            }
            r.p = propose_category_set(r.name, heads);
        } else if (fs::is_regular_file(upath(p), ec)) {
            r.p = propose_category(r.name, sniff_head(p));
        } else continue;
        r.cat = target >= 0 && target < kCategoryCount ? (Category)target : r.p.cat;
        sh.rows.push_back(std::move(r));
    }
    return sh;
}
int import_kinds(const std::string& name, const std::vector<uint8_t>& bytes) {
    Census c;
    census_one(name, bytes, c, 0);
    int k = 0;
    if (c.gerber + c.drill) k |= ImpGerber;
    if (c.cad) k |= ImpCad;
    if (c.bom) k |= ImpBom;
    if (c.centroid) k |= ImpCentroid;
    const std::string e = ext_of(name);
    if (any_of_ext(e, {"xlsx", "xls", "xlsm", "ods", "csv", "tsv"})) k |= ImpBom | ImpCentroid;
    return k;
}

std::string sanitize_file_name(const std::string& s) {
    std::string o;
    for (unsigned char c : s) o += (c < 0x20 || std::strchr("<>:\"/\\|?*", c)) ? '_' : (char)c;
    while (!o.empty() && (o.back() == ' ' || o.back() == '.')) o.pop_back();
    while (!o.empty() && o.front() == ' ') o.erase(0, 1);
    return o;
}

std::string archive_path_for(const std::string& pcbp_path, const std::string& project_name) {
    if (pcbp_path.empty()) return {};
    fs::path p = upath(pcbp_path);
    std::string name = sanitize_file_name(project_name);
    if (name.empty()) { auto st = p.stem().u8string(); name.assign(st.begin(), st.end()); }
    auto u = (p.parent_path() / upath(name + ".zip")).u8string();
    return std::string(u.begin(), u.end());
}

std::string unique_name(const Zip* z, const std::string& folder, const std::string& file_name, const std::set<std::string>& taken) {
    std::string pre = folder.empty() ? std::string() : (folder.back() == '/' ? folder : folder + "/");
    const std::string fn = sanitize_file_name(base_of(file_name));
    size_t d = fn.find_last_of('.');
    const std::string stem = d == std::string::npos || d == 0 ? fn : fn.substr(0, d), ext = d == std::string::npos || d == 0 ? "" : fn.substr(d);
    for (int i = 1; i < 10000; ++i) {
        std::string n = pre + (i == 1 ? fn : stem + " (" + std::to_string(i) + ")" + ext);
        if ((!z || z->find(n) < 0) && !taken.count(lower(n))) return n;
    }
    return pre + fn;
}

DocumentRef* index_find(std::vector<DocumentRef>& idx, const std::string& path) {
    for (auto& r : idx) if (r.path == path) return &r;
    return nullptr;
}

void sync_index(std::vector<DocumentRef>& idx, const Zip& z, const std::map<std::string, std::string>& renames) {
    auto add_tag = [](DocumentRef& r, const char* t) { if (t && std::find(r.tags.begin(), r.tags.end(), t) == r.tags.end()) r.tags.push_back(t); };
    std::map<std::string, DocumentRef> old;
    for (auto& r : idx) {
        DocumentRef c = r;
        // R22 index migration: legacy category names -> new labels; BOM / Datasheet folders leave a tag behind
        if (const LegacyFolder* l = legacy_folder(top_folder(r.path))) add_tag(c, l->tag);
        if (const LegacyFolder* l = legacy_folder(c.category)) add_tag(c, l->tag);
        std::string n = renamed(r.path, renames);
        if (!n.empty()) c.path = n;
        old[c.path] = c;
    }
    std::vector<DocumentRef> out;
    for (auto& it : z.items()) {
        if (it.dir() || is_version_path(it.name)) continue;   // R25: versions belong to their file, not to the index
        DocumentRef r;
        auto o = old.find(it.name);
        if (o != old.end()) r = o->second;
        if (o != old.end() && (o->second.size != it.usize || o->second.crc != it.crc)) r.sha256.clear();   // content changed
        r.path = it.name; r.size = it.usize; r.crc = it.crc;
        if (const LegacyFolder* l = legacy_folder(top_folder(it.name))) add_tag(r, l->tag);
        bool known = false;
        const Category c = category_of_path(it.name, &known);
        if (known) r.category = category_label(c);
        else r.category = r.category.empty() ? category_label(Category::Other) : category_label(category_from_name(r.category));
        out.push_back(std::move(r));
    }
    idx = std::move(out);
}

// --- R20 explorer helpers ---------------------------------------------------
FileClass file_class(const std::string& name) {
    std::string n = name;
    if (!n.empty() && n.back() == '/') n.pop_back();
    const size_t sl = n.find_last_of('/');
    std::string base = sl == std::string::npos ? n : n.substr(sl + 1);
    for (auto& ch : base) ch = (char)std::tolower((unsigned char)ch);
    const size_t d = base.find_last_of('.');
    const std::string e = d == std::string::npos ? std::string() : base.substr(d + 1);
    auto any = [&](std::initializer_list<const char*> l) { for (auto* x : l) if (e == x) return true; return false; };
    if (e == "pdf") return FileClass::Pdf;
    if (any({"zip", "7z", "rar", "tgz", "gz", "tar"})) return FileClass::Zip;
    if (any({"eml", "msg"})) return FileClass::Email;
    if (any({"png", "jpg", "jpeg", "bmp", "gif", "tif", "tiff", "webp", "ico"})) return FileClass::Image;
    if (e == "bom" || (base.find("bom") != std::string::npos && any({"csv", "tsv", "txt", "xlsx", "xls", "xlsm", "ods", "prn"}))) return FileClass::Bom;
    if (any({"xlsx", "xlsm", "xls", "ods"})) return FileClass::Sheet;
    if (any({"csv", "tsv"})) return FileClass::Csv;
    if (any({"docx", "doc", "dotx", "rtf", "odt", "htm", "html"})) return FileClass::Doc;
    if (any({"drl", "xln", "exc", "drd", "nc", "tap", "dri"})) return FileClass::Drill;
    if (any({"gbr", "ger", "pho", "art", "274", "gtl", "gbl", "gto", "gbo", "gts", "gbs", "gtp", "gbp", "gko", "gml", "gm1", "gm2", "gpt", "gpb", "cmp", "sol", "stc", "sts", "plc", "pls", "crc", "crs", "g1", "g2", "g3", "g4", "gp1", "gp2", "out"})) return FileClass::Gerber;
    if (any({"pcbp", "cpf", "brd", "kicad_pcb", "sch", "cad", "asc", "ipc", "cvg", "net", "odb", "tgz", "gen", "dsn", "pcb", "lay", "h8h", "h7h", "h5h", "h4h"})) return FileClass::Cad;
    if (any({"txt", "log", "rep", "ini", "xml", "json", "md", "pos", "xy", "mnt", "csv"})) return FileClass::Text;
    return FileClass::Generic;
}
const char* file_class_label(FileClass c) {
    switch (c) {
    case FileClass::Pdf: return "PDF document";
    case FileClass::Doc: return "Document";
    case FileClass::Sheet: return "Spreadsheet";
    case FileClass::Csv: return "CSV file";
    case FileClass::Zip: return "Zip archive";
    case FileClass::Gerber: return "Gerber layer";
    case FileClass::Drill: return "Drill file";
    case FileClass::Image: return "Image";
    case FileClass::Email: return "E-mail";
    case FileClass::Cad: return "CAD / program data";
    case FileClass::Bom: return "Bill of materials";
    case FileClass::Text: return "Text file";
    default: return "File";
    }
}
std::string dos_datetime_text(uint16_t date, uint16_t time) {
    if (!date) return {};
    char b[32];
    std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d", 1980 + (date >> 9), (date >> 5) & 15, date & 31, time >> 11, (time >> 5) & 63);
    return b;
}


// --- R23 job folder sidecars ---------------------------------------------------
namespace {
char side_norm(char c) {
    c = (char)std::tolower((unsigned char)c);
    return c == '.' || c == '_' ? '-' : c;
}
std::string side_lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
bool side_sep(char c) { return c == ' ' || c == '.' || c == '-' || c == '_' || c == '[' || c == '(' || c == '\t'; }
std::string side_trim(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    return s;
}
std::string split_ext(const std::string& name, std::string* stem) {
    size_t d = name.find_last_of('.');
    if (d == std::string::npos || d == 0) { if (stem) *stem = name; return {}; }
    if (stem) *stem = name.substr(0, d);
    return side_lower(name.substr(d + 1));
}
} // namespace

const char* side_kind_label(SideKind k) {
    switch (k) {
    case SideKind::HlcMaster: return "HLC program";
    case SideKind::HlcSplit:  return "HLC machine split";
    case SideKind::IssMaster: return "ISS program";
    case SideKind::IssSplit:  return "ISS machine split";
    case SideKind::AoiImage:  return "AOI board image";
    case SideKind::WorkOrder: return "Work order";
    case SideKind::Archive:   return "Job archive";
    default:                  return "Machine / side file";
    }
}
const char* side_group_label(SideGroup g) {
    static const char* n[] = {"Programs", "AOI image", "Work orders", "Machine / other files", "Job archive"};
    return n[(int)g];
}
SideGroup side_group(SideKind k) {
    switch (k) {
    case SideKind::HlcMaster: case SideKind::HlcSplit: case SideKind::IssMaster: case SideKind::IssSplit: return SideGroup::Programs;
    case SideKind::AoiImage: return SideGroup::AoiImage;
    case SideKind::WorkOrder: return SideGroup::WorkOrders;
    case SideKind::Archive: return SideGroup::Archive;
    default: return SideGroup::Machine;
    }
}

bool side_name_match(const std::string& file_name, const std::vector<std::string>& projects, std::string* rest) {
    size_t best = 0;
    for (const auto& p0 : projects) {
        const std::string p = side_trim(p0);
        if (p.empty() || p.size() > file_name.size() || p.size() <= best) continue;
        bool eq = true;
        for (size_t i = 0; i < p.size() && eq; ++i) eq = side_norm(p[i]) == side_norm(file_name[i]);
        if (!eq) continue;
        if (p.size() < file_name.size() && !side_sep(file_name[p.size()])) continue;
        best = p.size();
    }
    if (!best) return false;
    if (rest) *rest = file_name.substr(best);
    return true;
}

bool parse_work_order(const std::string& file_name, const std::vector<std::string>& projects, std::string& job_id, int& qty) {
    std::string stem;
    const std::string e = split_ext(file_name, &stem);
    if (e != "xlsx" && e != "xls" && e != "xlsm" && e != "csv") return false;
    std::string rest;
    if (!side_name_match(stem, projects, &rest)) return false;
    rest = side_trim(rest);
    // "<job id> x<qty>"
    size_t sp = rest.find_last_of(" \t");
    if (sp == std::string::npos) return false;
    std::string q = side_trim(rest.substr(sp + 1)), id = side_trim(rest.substr(0, sp));
    if (q.size() < 2 || (q[0] != 'x' && q[0] != 'X') || id.empty() || id.find_first_of(" \t") != std::string::npos) return false;
    for (size_t i = 1; i < q.size(); ++i) if (!std::isdigit((unsigned char)q[i])) return false;
    if (q.size() > 10) return false;
    job_id = id;
    qty = std::atoi(q.c_str() + 1);
    return true;
}

bool classify_side(const std::string& file_name, const std::vector<std::string>& projects, const std::string& archive_name,
                   SideFile& out) {
    std::string stem, rest;
    const std::string e = split_ext(file_name, &stem);
    if (e == "pcbp" || e == "bak" || e == "tmp" || e == "autosave" || file_name.rfind("~$", 0) == 0) return false;
    if (file_name.find(".pcbp.") != std::string::npos) return false;   // "x.pcbp.autosave", "x.pcbp.bak"
    const bool is_archive = !archive_name.empty() && side_lower(file_name) == side_lower(archive_name);
    if (!is_archive && !side_name_match(file_name, projects, &rest)) return false;
    out.name = file_name;
    out.stem = stem;
    out.side.clear();
    {   // "[SIDE1]" / "(Side1)" right after the project name
        std::string r = side_trim(rest);
        if (!r.empty() && (r[0] == '[' || r[0] == '(')) {
            size_t c = r.find(r[0] == '[' ? ']' : ')');
            if (c != std::string::npos) out.side = r.substr(1, c - 1);
        }
    }
    out.kind = SideKind::Other;
    out.master = -1;
    if (is_archive) { out.kind = SideKind::Archive; return true; }
    if (e == "h8h" || e == "h7h" || e == "h5h" || e == "h4h") out.kind = SideKind::HlcMaster;
    else if (e.size() == 3 && e[0] == 'h' && std::isdigit((unsigned char)e[1]) && e[2] != 'h') out.kind = SideKind::HlcSplit;   // H81, H82, H8x
    else if (e == "iss") out.kind = SideKind::IssMaster;
    else if (e.size() == 3 && e[0] == 'i' && std::isdigit((unsigned char)e[1]) && std::isdigit((unsigned char)e[2])) out.kind = SideKind::IssSplit;   // i01..
    else if (e == "jpg" || e == "jpeg" || e == "png" || e == "bmp") out.kind = SideKind::AoiImage;
    else if (parse_work_order(file_name, projects, out.job_id, out.qty)) out.kind = SideKind::WorkOrder;
    return true;
}

std::vector<SideFile> build_job_folder(std::vector<SideFile> listing, const std::vector<std::string>& projects,
                                       const std::string& archive_name) {
    std::vector<SideFile> in;
    for (auto& f : listing) {
        SideFile c = f;
        if (classify_side(f.name, projects, archive_name, c)) in.push_back(std::move(c));
    }
    auto key = [](const SideFile& f) { return side_lower(f.name); };
    std::stable_sort(in.begin(), in.end(), [&](const SideFile& a, const SideFile& b) {
        if (side_group(a.kind) != side_group(b.kind)) return side_group(a.kind) < side_group(b.kind);
        return key(a) < key(b);
    });
    auto is_master = [](SideKind k) { return k == SideKind::HlcMaster || k == SideKind::IssMaster; };
    auto master_of = [](SideKind k) { return k == SideKind::HlcSplit ? SideKind::HlcMaster : SideKind::IssMaster; };
    std::vector<SideFile> out;
    std::vector<bool> used(in.size(), false);
    for (size_t i = 0; i < in.size(); ++i) {
        if (used[i]) continue;
        const bool split = in[i].kind == SideKind::HlcSplit || in[i].kind == SideKind::IssSplit;
        if (split) {   // a split whose master exists is emitted under it
            bool has = false;
            for (size_t j = 0; j < in.size(); ++j) has |= in[j].kind == master_of(in[i].kind) && side_lower(in[j].stem) == side_lower(in[i].stem);
            if (has) continue;
        }
        used[i] = true;
        out.push_back(in[i]);
        if (is_master(in[i].kind)) {
            const int mi = (int)out.size() - 1;
            const SideKind sk = in[i].kind == SideKind::HlcMaster ? SideKind::HlcSplit : SideKind::IssSplit;
            for (size_t j = 0; j < in.size(); ++j)
                if (!used[j] && in[j].kind == sk && side_lower(in[j].stem) == side_lower(in[i].stem)) {
                    used[j] = true;
                    out.push_back(in[j]);
                    out.back().master = mi;
                }
        }
    }
    return out;
}

std::vector<SideFile> scan_job_folder(const std::string& dir, const std::vector<std::string>& projects,
                                      const std::string& archive_name) {
    namespace fs = std::filesystem;
    std::vector<SideFile> l;
    if (dir.empty()) return l;
    std::error_code ec;
    const fs::path d(std::u8string(dir.begin(), dir.end()));
    for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        SideFile f;
        auto u = it->path().filename().u8string(); f.name.assign(u.begin(), u.end());
        auto pu = it->path().u8string(); f.path.assign(pu.begin(), pu.end());
        f.size = it->file_size(e2);
        const auto t = it->last_write_time(e2);
        if (!e2) f.mtime = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::clock_cast<std::chrono::system_clock>(t).time_since_epoch()).count();
        l.push_back(std::move(f));
    }
    return build_job_folder(std::move(l), projects, archive_name);
}

int aoi_for(const std::vector<SideFile>& files, int program) {
    if (program < 0 || program >= (int)files.size()) return -1;
    const SideFile& p = files[(size_t)program];
    const std::string stem = side_lower(p.master >= 0 ? files[(size_t)p.master].stem : p.stem);
    int same_side = -1, n = 0, any = -1;
    for (int i = 0; i < (int)files.size(); ++i) {
        if (files[(size_t)i].kind != SideKind::AoiImage) continue;
        if (side_lower(files[(size_t)i].stem) == stem) return i;
        if (!p.side.empty() && side_lower(files[(size_t)i].side) == side_lower(p.side) && same_side < 0) same_side = i;
        ++n; any = i;
    }
    return same_side >= 0 ? same_side : n == 1 ? any : -1;
}

// --- R25 hashes --------------------------------------------------------------------------------------------------
std::map<std::string, std::string> hash_added(const Edit& e) {
    std::map<std::string, std::string> h;
    for (auto& a : e.add) {
        const std::string x = a.disk.empty() ? sha256_hex(a.data) : sha256_file(a.disk);
        if (!x.empty()) h[a.name] = x;
    }
    return h;
}

std::string entry_sha256(const Zip& z, const Item& it) {
    std::vector<uint8_t> d;
    std::string err;
    return z.read(it, d, err) ? sha256_hex(d) : std::string();
}

int fill_hashes(std::vector<DocumentRef>& idx, const Zip& z, const std::map<std::string, std::string>& known, std::vector<std::string>* bad) {
    int n = 0;
    for (auto& r : idx) {
        auto k = known.find(r.path);
        if (k != known.end()) { r.sha256 = k->second; continue; }
        const int i = z.find(r.path);
        if (i < 0) continue;
        const std::string h = entry_sha256(z, z.items()[(size_t)i]);
        if (h.empty()) { if (bad) bad->push_back(r.path); continue; }
        if (!r.sha256.empty() && r.sha256 != h && bad) bad->push_back(r.path);   // stored hash no longer matches
        if (r.sha256.empty()) ++n;
        r.sha256 = h;
    }
    return n;
}

} // namespace pcbp::io::jobzip
