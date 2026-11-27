// ============================================================================
// migrate.cpp — batch migration of legacy projects (see migrate.h).
// ============================================================================
#include "migrate.h"
#include "fileio.h"
#include "project.h"
#include "project_io.h"
#include "../io/archive.h"
#include "../io/deflate.h"
#include "../io/inflate.h"
#include "../io/viewmate_bin.h"
#include "../plugins/plugin.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>
#include <sys/stat.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <cctype>
#endif

namespace pcbp::migrate {

namespace fs = std::filesystem;

namespace {

fs::path P(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string S(const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }
std::string G(const fs::path& p) { auto u = p.generic_u8string(); return std::string(u.begin(), u.end()); }

std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

FILE* open_u8(const std::string& path, const char* mode) {
#ifdef _WIN32
    std::wstring m(mode, mode + std::strlen(mode));
    return _wfopen(utf8_to_wide(path).c_str(), m.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

int64_t file_mtime(const std::string& path) {
#ifdef _WIN32
    struct _stat64 st;
    if (_wstat64(utf8_to_wide(path).c_str(), &st) != 0) return 0;
    return st.st_mtime;
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return st.st_mtime;
#endif
}

bool is_vm_job(const std::string& path) {
    FILE* f = open_u8(path, "rb");
    if (!f) return false;
    uint8_t b[64];
    size_t n = std::fread(b, 1, sizeof b, f);
    std::fclose(f);
    return io::vm::is_job(b, n);
}

bool wanted(const fs::path& p, const Options& o, bool explicit_file) {
    const std::string ext = lower(S(p.extension()));
    if (ext == ".cpf") return o.include_cpf || explicit_file;
    if (ext == ".bin") return (o.include_bin || explicit_file) && is_vm_job(S(p));
    return false;
}

struct Counts {
    size_t comps = 0, packages = 0, parts = 0, layers = 0, images = 0, points = 0, layer_bytes = 0;
    bool operator==(const Counts&) const = default;
};
Counts counts_of(const Project& p) {
    Counts c;
    c.comps = p.comps.size(); c.packages = p.packages.size(); c.parts = p.parts.size(); c.layers = p.layers.size();
    c.images = p.images.size(); c.points = p.model.points.size();
    for (auto& l : p.layers) c.layer_bytes += l.content.size() + l.aperture_list.size();
    return c;
}
std::string describe(const Counts& c) {
    char b[200];
    std::snprintf(b, sizeof b, "%zu comps, %zu packages, %zu parts, %zu layers, %zu images, %zu points", c.comps, c.packages,
                  c.parts, c.layers, c.images, c.points);
    return b;
}

std::string unique_name(const std::string& out, const std::set<std::string>& claimed) {
    std::error_code ec;
    auto taken = [&](const std::string& p) { return claimed.count(lower(p)) || fs::exists(P(p), ec); };
    if (!taken(out)) return out;
    const fs::path base = P(out);
    const std::string dir = S(base.parent_path()), stem = S(base.stem()), ext = S(base.extension());
    for (int n = 2; n < 10000; ++n) {
        std::string cand = S(P(dir) / P(stem + " (" + std::to_string(n) + ")" + ext));
        if (!taken(cand)) return cand;
    }
    return out;
}

// ---- zip (PKWARE APPNOTE 6.3: local headers, central directory, EOCD; no ZIP64) ----
void put16(std::vector<uint8_t>& o, uint32_t v) { o.push_back(uint8_t(v)); o.push_back(uint8_t(v >> 8)); }
void put32(std::vector<uint8_t>& o, uint32_t v) { put16(o, v & 0xFFFF); put16(o, v >> 16); }
uint32_t rd16(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
uint32_t rd32(const uint8_t* p) { return rd16(p) | rd16(p + 2) << 16; }

void dos_time(int64_t t, uint16_t& time, uint16_t& date) {
    time = 0; date = (1 << 5) | 1;
    if (t <= 0) return;
    std::time_t tt = (std::time_t)t;
    std::tm tmv{};
#ifdef _WIN32
    if (localtime_s(&tmv, &tt) != 0) return;
#else
    if (!localtime_r(&tt, &tmv)) return;
#endif
    if (tmv.tm_year < 80) return;
    time = uint16_t(tmv.tm_hour << 11 | tmv.tm_min << 5 | tmv.tm_sec / 2);
    date = uint16_t((tmv.tm_year - 80) << 9 | (tmv.tm_mon + 1) << 5 | tmv.tm_mday);
}

bool read_all(const std::string& path, std::vector<uint8_t>& d, std::string& err) { return read_file(path, d, err); }

std::string csv(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string r = "\"";
    for (char c : s) { if (c == '"') r += '"'; r += c; }
    return r + "\"";
}

} // namespace

// ---------------------------------------------------------------------------
std::string status_name(Status s) {
    switch (s) {
    case Status::Pending: return "Pending";
    case Status::Converted: return "Converted";
    case Status::DryRun: return "Dry run";
    case Status::Skipped: return "Skipped";
    case Status::Failed: return "Failed";
    case Status::Cancelled: return "Cancelled";
    }
    return "?";
}

std::string Summary::describe() const {
    std::string s = std::to_string(total) + " file(s): " + std::to_string(converted) + " converted";
    if (dry) s += ", " + std::to_string(dry) + " checked (dry run)";
    if (skipped) s += ", " + std::to_string(skipped) + " skipped";
    if (failed) s += ", " + std::to_string(failed) + " failed";
    if (cancelled) s += ", " + std::to_string(cancelled) + " cancelled";
    if (archive_attempted) s += archive_ok ? "; " + std::to_string(archived) + " archived (verified)" : "; archive FAILED: " + archive_error;
    if (deleted || delete_failed) s += "; " + std::to_string(deleted) + " original(s) deleted";
    if (delete_failed) s += ", " + std::to_string(delete_failed) + " could not be deleted";
    return s;
}

std::string planned_output(const Item& it, const Options& o) {
    fs::path rel = P(it.rel);
    rel.replace_extension(".pcbp");
    rel.make_preferred();
    if (o.output == OutputMode::MirrorTree && !o.out_dir.empty()) return S(P(o.out_dir) / rel);
    fs::path src = P(it.src);
    return S(src.parent_path() / P(S(src.stem()) + ".pcbp"));
}

std::vector<Item> collect(const std::vector<std::string>& inputs, const Options& o, std::vector<std::string>& errors) {
    std::vector<Item> items;
    std::set<std::string> seen;
    std::error_code ec;
    auto add = [&](const fs::path& file, const fs::path& root) {
        fs::path abs = fs::absolute(file, ec);
        fs::path canon = fs::weakly_canonical(abs, ec);
        if (ec) { canon = abs; ec.clear(); }
        const std::string key = lower(G(canon));
        if (!seen.insert(key).second) return;
        Item it;
        it.src = S(canon);
        if (root.empty()) { it.rel = S(canon.filename()); }
        else {
            it.root = S(root);
            fs::path r = fs::relative(canon, root, ec);
            if (ec || r.empty()) { r = canon.filename(); ec.clear(); }
            it.rel = G(r);
        }
        it.size = (uint64_t)fs::file_size(canon, ec);
        if (ec) { it.size = 0; ec.clear(); }
        items.push_back(std::move(it));
    };
    for (auto& in : inputs) {
        fs::path p = P(in);
        if (fs::is_directory(p, ec)) {
            fs::path root = fs::weakly_canonical(fs::absolute(p, ec), ec);
            if (ec) { root = fs::absolute(p); ec.clear(); }
            std::vector<fs::path> files;
            if (o.recursive) {
                for (auto i = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
                     !ec && i != fs::recursive_directory_iterator(); i.increment(ec))
                    if (i->is_regular_file(ec) && wanted(i->path(), o, false)) files.push_back(i->path());
            } else {
                for (auto i = fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
                     !ec && i != fs::directory_iterator(); i.increment(ec))
                    if (i->is_regular_file(ec) && wanted(i->path(), o, false)) files.push_back(i->path());
            }
            if (ec) { errors.push_back("cannot fully list " + in + ": " + ec.message()); ec.clear(); }
            std::sort(files.begin(), files.end());
            for (auto& f : files) add(f, root);
        } else if (fs::is_regular_file(p, ec)) {
            if (wanted(p, o, true)) add(p, {});
            else errors.push_back(in + ": not a legacy project (.cpf) or ViewMate job (.bin)");
        } else {
            errors.push_back(in + ": not found");
        }
        ec.clear();
    }
    for (auto& it : items) it.out = planned_output(it, o);
    return items;
}

bool convert_item(Item& it, const Options& o) {
    const auto t0 = std::chrono::steady_clock::now();
    auto finish = [&](Status s, const std::string& e = {}) {
        it.status = s;
        if (!e.empty()) it.error = e;
        it.secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return s == Status::Converted || s == Status::DryRun;
    };
    std::error_code ec;
    if (it.out.empty()) it.out = planned_output(it, o);
    const bool exists = fs::exists(P(it.out), ec);
    if (exists && o.collision == Collision::Skip) return finish(Status::Skipped, "output exists: " + it.out);

    Importer* im = PluginRegistry::get().detect(it.src);
    if (!im || im->info().import_mode != ImportMode::Project) return finish(Status::Failed, "no project importer recognises this file");
    it.importer = im->info().id;
    Project prj;
    ImportReport rep;
    std::string err;
    pcbp::Options opts = default_options(im->info());
    bool ok = false;
    try { ok = im->run(it.src, opts, prj, rep, err); }
    catch (const std::exception& x) { ok = false; err = std::string("exception: ") + x.what(); }
    catch (...) { ok = false; err = "unknown exception"; }
    if (!ok) return finish(Status::Failed, err.empty() ? "import failed" : err);
    const Counts c = counts_of(prj);
    it.comps = (int)c.comps; it.packages = (int)c.packages; it.parts = (int)c.parts; it.layers = (int)c.layers; it.points = (int)c.points;
    for (size_t i = 0; i < rep.messages.size() && i < 40; ++i) it.warnings.push_back(rep.messages[i]);
    if (rep.messages.size() > 40) it.warnings.push_back("... " + std::to_string(rep.messages.size() - 40) + " more messages");
    if (c.comps == 0 && c.layers == 0 && c.packages == 0 && c.parts == 0) return finish(Status::Failed, "nothing was imported");
    if (o.dry_run) return finish(Status::DryRun);

    fs::path out = P(it.out);
    fs::create_directories(out.parent_path(), ec);
    if (ec) return finish(Status::Failed, "cannot create folder " + S(out.parent_path()) + ": " + ec.message());
    if (prj.info.original_filename.empty()) prj.info.original_filename = it.src;
    {
        char ts[32];
        std::time_t now = std::time(nullptr);
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &now);
#else
        localtime_r(&now, &tmv);
#endif
        std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tmv);
        if (prj.info.created.empty()) prj.info.created = ts;
        prj.info.modified = ts;
    }
    prj.path = it.out;
    const std::string tmp = it.out + ".migrating";
    if (!save_project(prj, tmp, err)) { fs::remove(P(tmp), ec); return finish(Status::Failed, "save failed: " + err); }
    Project chk;
    if (!load_project(tmp, chk, err)) { fs::remove(P(tmp), ec); return finish(Status::Failed, "verification reload failed: " + err); }
    const Counts c2 = counts_of(chk);
    if (!(c2 == c)) {
        fs::remove(P(tmp), ec);
        return finish(Status::Failed, "verification mismatch: imported " + describe(c) + ", reloaded " + describe(c2));
    }
    if (fs::exists(out, ec)) {
        if (o.collision != Collision::Overwrite) { fs::remove(P(tmp), ec); return finish(Status::Failed, "output appeared during conversion: " + it.out); }
        fs::remove(out, ec);
        if (ec) { fs::remove(P(tmp), ec); return finish(Status::Failed, "cannot replace " + it.out + ": " + ec.message()); }
    }
    fs::rename(P(tmp), out, ec);
    if (ec) { std::error_code e2; fs::remove(P(tmp), e2); return finish(Status::Failed, "cannot rename to " + it.out + ": " + ec.message()); }
    return finish(Status::Converted);
}

// ---------------------------------------------------------------------------
bool write_archive(const std::vector<Item*>& items, const std::string& zip_path, std::string& err, const ProgressFn& progress) {
    std::error_code ec;
    fs::create_directories(P(zip_path).parent_path(), ec);
    const std::string tmp = zip_path + ".partial";
    FILE* f = open_u8(tmp, "wb");
    if (!f) { err = "cannot create " + zip_path; return false; }
    std::vector<uint8_t> cd;
    std::set<std::string> names;
    uint64_t off = 0;
    bool ok = true;
    int idx = 0;
    for (Item* it : items) {
        ++idx;
        if (progress && !progress(idx, (int)items.size(), "Archiving " + it->rel, float(idx - 1) / std::max<size_t>(1, items.size()))) {
            err = "cancelled"; ok = false; break;
        }
        std::vector<uint8_t> data;
        if (!read_all(it->src, data, err)) { ok = false; break; }
        if (data.size() >= 0xFFFFFFFFull) { err = it->src + ": larger than 4 GB (ZIP64 not supported)"; ok = false; break; }
        std::string name = it->rel;
        for (auto& ch : name) if (ch == '\\') ch = '/';
        while (!name.empty() && (name[0] == '/' || name.rfind("../", 0) == 0)) name.erase(0, name[0] == '/' ? 1 : 3);
        if (name.empty()) name = S(P(it->src).filename());
        { std::string base = name; int n = 2; while (!names.insert(lower(name)).second) name = base + " (" + std::to_string(n++) + ")"; }
        const uint32_t crc = io::crc32_ieee(data.data(), data.size());
        std::vector<uint8_t> comp = deflate_raw(data.data(), data.size(), 6);
        const bool deflated = comp.size() < data.size();
        const uint8_t* body = deflated ? comp.data() : data.data();
        const uint32_t csize = deflated ? (uint32_t)comp.size() : (uint32_t)data.size();
        uint16_t t, d; dos_time(file_mtime(it->src), t, d);
        if (off + 30 + name.size() + csize >= 0xFFFFFFFFull) { err = "archive would exceed 4 GB (ZIP64 not supported)"; ok = false; break; }
        std::vector<uint8_t> h;
        put32(h, 0x04034b50); put16(h, 20); put16(h, 0x0800); put16(h, deflated ? 8 : 0); put16(h, t); put16(h, d);
        put32(h, crc); put32(h, csize); put32(h, (uint32_t)data.size()); put16(h, (uint32_t)name.size()); put16(h, 0);
        h.insert(h.end(), name.begin(), name.end());
        if (std::fwrite(h.data(), 1, h.size(), f) != h.size() || std::fwrite(body, 1, csize, f) != csize) { err = "write error (disk full?)"; ok = false; break; }
        put32(cd, 0x02014b50); put16(cd, 20); put16(cd, 20); put16(cd, 0x0800); put16(cd, deflated ? 8 : 0); put16(cd, t); put16(cd, d);
        put32(cd, crc); put32(cd, csize); put32(cd, (uint32_t)data.size()); put16(cd, (uint32_t)name.size()); put16(cd, 0); put16(cd, 0);
        put16(cd, 0); put16(cd, 0); put32(cd, 0); put32(cd, (uint32_t)off);
        cd.insert(cd.end(), name.begin(), name.end());
        off += h.size() + csize;
        it->zip_name = name;
    }
    if (ok) {
        if (items.size() > 0xFFFF) { err = "too many files for one zip (65535 max without ZIP64)"; ok = false; }
        else {
            std::vector<uint8_t> e;
            put32(e, 0x06054b50); put16(e, 0); put16(e, 0); put16(e, (uint32_t)items.size()); put16(e, (uint32_t)items.size());
            put32(e, (uint32_t)cd.size()); put32(e, (uint32_t)off); put16(e, 0);
            if (std::fwrite(cd.data(), 1, cd.size(), f) != cd.size() || std::fwrite(e.data(), 1, e.size(), f) != e.size()) { err = "write error"; ok = false; }
        }
    }
    if (std::fclose(f) != 0 && ok) { err = "close failed (disk full?)"; ok = false; }
    if (!ok) { fs::remove(P(tmp), ec); return false; }
    if (fs::exists(P(zip_path), ec)) fs::remove(P(zip_path), ec);
    fs::rename(P(tmp), P(zip_path), ec);
    if (ec) { err = "cannot rename archive: " + ec.message(); fs::remove(P(tmp), ec); return false; }
    return true;
}

bool verify_archive(const std::string& zip_path, const std::vector<Item*>& items, std::string& err) {
    std::vector<uint8_t> z;
    if (!read_all(zip_path, z, err)) return false;
    if (z.size() < 22) { err = "archive too small"; return false; }
    size_t eocd = std::string::npos;
    for (size_t i = z.size() - 22 + 1; i-- > 0 && z.size() - i <= 22 + 65535;) if (rd32(&z[i]) == 0x06054b50) { eocd = i; break; }
    if (eocd == std::string::npos) { err = "end of central directory not found"; return false; }
    const uint32_t n = rd16(&z[eocd + 10]), cdsize = rd32(&z[eocd + 12]), cdoff = rd32(&z[eocd + 16]);
    if ((uint64_t)cdoff + cdsize > eocd) { err = "central directory out of range"; return false; }
    struct Ent { uint32_t crc, csize, usize, off; int method; };
    std::map<std::string, Ent> ents;
    size_t p = cdoff;
    for (uint32_t i = 0; i < n; ++i) {
        if (p + 46 > z.size() || rd32(&z[p]) != 0x02014b50) { err = "bad central directory entry"; return false; }
        Ent e{rd32(&z[p + 16]), rd32(&z[p + 20]), rd32(&z[p + 24]), rd32(&z[p + 42]), (int)rd16(&z[p + 10])};
        const uint32_t nl = rd16(&z[p + 28]), xl = rd16(&z[p + 30]), cl = rd16(&z[p + 32]);
        if (p + 46 + nl > z.size()) { err = "bad central directory name"; return false; }
        ents[std::string((const char*)&z[p + 46], nl)] = e;
        p += 46 + nl + xl + cl;
    }
    for (Item* it : items) {
        const std::string& name = it->zip_name;
        auto f = ents.find(name);
        if (name.empty() || f == ents.end()) { err = it->rel + ": missing from the archive"; return false; }
        const Ent& e = f->second;
        if ((uint64_t)e.off + 30 > z.size() || rd32(&z[e.off]) != 0x04034b50) { err = name + ": bad local header"; return false; }
        const size_t data = e.off + 30 + rd16(&z[e.off + 26]) + rd16(&z[e.off + 28]);
        if ((uint64_t)data + e.csize > z.size()) { err = name + ": data out of range"; return false; }
        std::vector<uint8_t> out;
        if (e.method == 8) {
            std::string e2;
            if (!inflate_raw(&z[data], e.csize, out, e.usize, e2, nullptr, size_t(e.usize) + 16)) { err = name + ": inflate failed: " + e2; return false; }
        } else if (e.method == 0) out.assign(z.begin() + (long)data, z.begin() + (long)(data + e.csize));
        else { err = name + ": unsupported method"; return false; }
        if (out.size() != e.usize || io::crc32_ieee(out.data(), out.size()) != e.crc) { err = name + ": CRC / size mismatch in the archive"; return false; }
        std::vector<uint8_t> src;
        std::string e3;
        if (!read_all(it->src, src, e3)) { err = it->src + ": cannot re-read source: " + e3; return false; }
        if (src.size() != out.size() || io::crc32_ieee(src.data(), src.size()) != e.crc) { err = name + ": archive copy differs from the source file"; return false; }
    }
    return true;
}

bool remove_file(const std::string& path, bool recycle, std::string& err) {
    std::error_code ec;
#ifdef _WIN32
    if (recycle) {
        std::wstring w = utf8_to_wide(S(fs::absolute(P(path), ec)));
        for (auto& c : w) if (c == L'/') c = L'\\';
        w.push_back(L'\0');   // double NUL terminated list
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = w.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
        const int r = SHFileOperationW(&op);
        if (r != 0 || op.fAnyOperationsAborted) { err = "Recycle Bin delete failed (code " + std::to_string(r) + ")"; return false; }
        if (fs::exists(P(path), ec)) { err = "file still exists after delete"; return false; }
        return true;
    }
#else
    (void)recycle;
#endif
    if (!fs::remove(P(path), ec) || ec) { err = ec ? ec.message() : "not removed"; return false; }
    return true;
}

Summary run(std::vector<Item>& items, const Options& o, const ProgressFn& progress) {
    Summary s;
    s.total = (int)items.size();
    std::set<std::string> claimed;   // outputs planned in this batch (case-insensitive)
    bool cancelled = false;
    const int n = (int)items.size();
    for (int i = 0; i < n; ++i) {
        Item& it = items[i];
        if (cancelled) { it.status = Status::Cancelled; ++s.cancelled; continue; }
        if (progress && !progress(i, n, "Converting " + it.rel, float(i) / std::max(1, n))) { cancelled = true; it.status = Status::Cancelled; ++s.cancelled; continue; }
        it.out = planned_output(it, o);
        if (o.collision == Collision::Rename) it.out = unique_name(it.out, claimed);
        else if (claimed.count(lower(it.out)) && o.collision == Collision::Skip) { it.status = Status::Skipped; it.error = "another input maps to " + it.out; ++s.skipped; continue; }
        claimed.insert(lower(it.out));
        convert_item(it, o);
        switch (it.status) {
        case Status::Converted: ++s.converted; break;
        case Status::DryRun: ++s.dry; break;
        case Status::Skipped: ++s.skipped; break;
        default: ++s.failed; break;
        }
    }
    if (o.dry_run || cancelled) return s;
    std::vector<Item*> done;
    for (auto& it : items) if (it.status == Status::Converted) done.push_back(&it);
    if (o.archive && !done.empty()) {
        s.archive_attempted = true;
        std::string err;
        if (o.archive_path.empty()) err = "no archive path";
        else if (write_archive(done, o.archive_path, err, progress) && verify_archive(o.archive_path, done, err)) {
            s.archive_ok = true;
            for (auto* it : done) it->archived = true;
            s.archived = (int)done.size();
        }
        if (!s.archive_ok) s.archive_error = err;
    }
    if (o.delete_originals && (!o.archive || s.archive_ok)) {
        int k = 0;
        for (auto* it : done) {
            ++k;
            if (progress && !progress(k, (int)done.size(), "Removing " + it->rel, float(k) / std::max<size_t>(1, done.size()))) break;
            if (o.archive && !it->archived) continue;
            std::string err;
            if (remove_file(it->src, o.recycle, err)) { it->deleted = true; ++s.deleted; }
            else { it->warnings.push_back("original not deleted: " + err); ++s.delete_failed; }
        }
    }
    return s;
}

std::string results_csv(const std::vector<Item>& items) {
    std::string s = "Source,Status,Importer,Components,Part numbers,Packages,Layers,Model points,Output,Archived,Deleted,Seconds,Error,Messages\r\n";
    for (auto& it : items) {
        std::string msgs;
        for (auto& w : it.warnings) { if (!msgs.empty()) msgs += " | "; msgs += w; }
        char secs[32]; std::snprintf(secs, sizeof secs, "%.2f", it.secs);
        s += csv(it.src) + "," + csv(status_name(it.status)) + "," + csv(it.importer) + "," + std::to_string(it.comps) + "," +
             std::to_string(it.parts) + "," + std::to_string(it.packages) + "," + std::to_string(it.layers) + "," +
             std::to_string(it.points) + "," + csv(it.status == Status::Converted || it.status == Status::DryRun ? it.out : "") + "," +
             (it.archived ? "yes" : "no") + "," + (it.deleted ? "yes" : "no") + "," + secs + "," + csv(it.error) + "," + csv(msgs) + "\r\n";
    }
    return s;
}

// ---------------------------------------------------------------------------
std::string cli_usage() {
    return "PCBPlotter.exe --migrate <folder|file>... [options]\n"
           "  Converts legacy CAM projects (*.cpf) and optionally ViewMate jobs (*.bin) to .pcbp.\n"
           "  --no-recursive          only the given folders, not their subfolders\n"
           "  --bin                   also convert ViewMate *.bin jobs     --no-cpf   skip *.cpf\n"
           "  --out <folder>          mirror the source tree into <folder> (default: next to each source)\n"
           "  --collision skip|overwrite|rename   existing .pcbp outputs (default rename)\n"
           "  --archive <file.zip>    archive converted originals into one zip (verified)\n"
           "  --delete --yes          delete converted originals (Recycle Bin; after verified archive)\n"
           "  --no-recycle            delete permanently instead of the Recycle Bin\n"
           "  --dry-run               import and report only; write nothing\n"
           "  --log <file.csv>        write the results table as CSV\n";
}

int cli_main(const std::vector<std::string>& args, std::string& out) {
    Options o;
    std::vector<std::string> inputs;
    std::string log;
    bool del = false, yes = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next = [&](std::string& v) { if (i + 1 >= args.size()) return false; v = args[++i]; return true; };
        if (a == "--migrate") continue;
        if (a == "--help" || a == "-h" || a == "/?") { out += cli_usage(); return 0; }
        else if (a == "--recursive") o.recursive = true;
        else if (a == "--no-recursive") o.recursive = false;
        else if (a == "--bin") o.include_bin = true;
        else if (a == "--no-cpf") o.include_cpf = false;
        else if (a == "--out") { if (!next(o.out_dir)) { out += "--out needs a folder\n"; return 2; } o.output = OutputMode::MirrorTree; }
        else if (a == "--collision") {
            std::string v; if (!next(v)) { out += "--collision needs a value\n"; return 2; }
            v = lower(v);
            if (v == "skip") o.collision = Collision::Skip; else if (v == "overwrite") o.collision = Collision::Overwrite;
            else if (v == "rename") o.collision = Collision::Rename; else { out += "unknown collision mode " + v + "\n"; return 2; }
        }
        else if (a == "--archive") { if (!next(o.archive_path)) { out += "--archive needs a .zip path\n"; return 2; } o.archive = true; }
        else if (a == "--delete") del = true;
        else if (a == "--yes" || a == "-y") yes = true;
        else if (a == "--no-recycle") o.recycle = false;
        else if (a == "--dry-run") o.dry_run = true;
        else if (a == "--log") { if (!next(log)) { out += "--log needs a path\n"; return 2; } }
        else if (!a.empty() && a[0] == '-') { out += "unknown option " + a + "\n" + cli_usage(); return 2; }
        else inputs.push_back(a);
    }
    if (inputs.empty()) { out += cli_usage(); return 2; }
    if (del && !yes) { out += "--delete requires --yes (originals are only removed after verified conversion";
                       out += o.archive ? " and verified archive)\n" : ")\n"; return 2; }
    o.delete_originals = del && yes;
    std::vector<std::string> errors;
    auto items = collect(inputs, o, errors);
    for (auto& e : errors) out += "warning: " + e + "\n";
    if (items.empty()) { out += "no matching files\n"; return errors.empty() ? 0 : 1; }
    Summary s = run(items, o, [&](int i, int n, const std::string& m, float) {
        (void)i; (void)n; (void)m; return true;
    });
    for (auto& it : items) {
        out += "[" + status_name(it.status) + "] " + it.src;
        if (it.status == Status::Converted || it.status == Status::DryRun) {
            out += " -> " + it.out + "  (" + std::to_string(it.comps) + " comps, " + std::to_string(it.parts) + " parts, " +
                   std::to_string(it.packages) + " packages, " + std::to_string(it.layers) + " layers)";
        }
        if (!it.error.empty()) out += "  ERROR: " + it.error;
        if (it.archived) out += "  [archived]";
        if (it.deleted) out += "  [deleted]";
        out += "\n";
    }
    out += s.describe() + "\n";
    if (!log.empty()) {
        std::string csvs = results_csv(items), err;
        if (!write_file(log, csvs.data(), csvs.size(), err)) out += "could not write log: " + err + "\n";
        else out += "log written to " + log + "\n";
    }
    return (s.failed || (s.archive_attempted && !s.archive_ok) || s.delete_failed) ? 1 : 0;
}

} // namespace pcbp::migrate
