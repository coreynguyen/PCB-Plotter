// ============================================================================
// autosave.cpp — crash-safe recovery snapshots + session locks (see autosave.h).
// ============================================================================
#include "autosave.h"
#include "fileio.h"
#include "json.h"
#include "project_io.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <mutex>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pcbp::recovery {
namespace {
std::mutex g_mx;
#ifdef _WIN32
HANDLE g_lock = INVALID_HANDLE_VALUE;
std::wstring wide(const std::string& s) { return utf8_to_wide(s); }
bool mkdirs(const std::string& p) {
    std::wstring w = wide(p);
    for (size_t i = 3; i <= w.size(); ++i)
        if (i == w.size() || w[i] == L'\\' || w[i] == L'/') { std::wstring s = w.substr(0, i); CreateDirectoryW(s.c_str(), nullptr); }
    DWORD a = GetFileAttributesW(w.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
std::vector<std::string> list_dir(const std::string& dir, bool dirs) {
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wide(dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == dirs) out.push_back(wide_to_utf8(n));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}
void rm_file(const std::string& p) { DeleteFileW(wide(p).c_str()); }
void rm_dir(const std::string& p) { RemoveDirectoryW(wide(p).c_str()); }
bool exists(const std::string& p) { return GetFileAttributesW(wide(p).c_str()) != INVALID_FILE_ATTRIBUTES; }
int my_pid() { return (int)GetCurrentProcessId(); }
const char kSep = '\\';
#else
int g_lock = -1;
bool mkdirs(const std::string& p) {
    for (size_t i = 1; i <= p.size(); ++i) if (i == p.size() || p[i] == '/') ::mkdir(p.substr(0, i).c_str(), 0755);
    struct stat st; return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
std::vector<std::string> list_dir(const std::string& dir, bool dirs) {
    std::vector<std::string> out;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return out;
    while (dirent* e = ::readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        struct stat st;
        if (::stat((dir + "/" + n).c_str(), &st) == 0 && S_ISDIR(st.st_mode) == dirs) out.push_back(n);
    }
    ::closedir(d);
    return out;
}
void rm_file(const std::string& p) { ::unlink(p.c_str()); }
void rm_dir(const std::string& p) { ::rmdir(p.c_str()); }
bool exists(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0; }
int my_pid() { return (int)getpid(); }
const char kSep = '/';
#endif

std::string join(const std::string& a, const std::string& b) { return a.empty() ? b : a + kSep + b; }

// A session is alive when its lock file is held open by its process (no sharing / flock).
bool session_alive(const std::string& sid) {
    if (sid == session_id()) return true;
    const std::string lp = join(join(root(), "sessions"), sid + ".lock");
    if (!exists(lp)) return false;
#ifdef _WIN32
    HANDLE h = CreateFileW(wide(lp).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION;
    CloseHandle(h);
    return false;
#else
    int fd = ::open(lp.c_str(), O_RDONLY);
    if (fd < 0) return false;
    bool alive = ::flock(fd, LOCK_EX | LOCK_NB) != 0;
    ::close(fd);
    return alive;
#endif
}

bool read_meta(const std::string& dir, Entry& e) {
    std::string text, err;
    if (!read_text_file(join(dir, "meta.json"), text, err)) return false;
    json::Value v;
    if (!json::Value::parse(text, v, err)) return false;
    e.original_path = v["original"].str();
    e.display_name = v["name"].str();
    e.session = v["session"].str();
    e.clean = v["clean"].str() == "1";
    e.closed = v["closed"].str();
    for (auto& s : v["snaps"].items()) {
        Snapshot sn{s["file"].str(), s["time"].str()};
        if (!sn.file.empty() && exists(join(dir, sn.file))) e.snaps.push_back(sn);
    }
    return true;
}
bool write_meta(const std::string& dir, const Entry& e, std::string& err) {
    json::Value v = json::Value::object();
    v.set("format", "pcbp-recovery-1");
    v.set("original", e.original_path);
    v.set("name", e.display_name);
    v.set("session", e.session);
    if (e.clean) { v.set("clean", "1"); v.set("closed", e.closed); }
    json::Value a = json::Value::array();
    for (auto& s : e.snaps) { json::Value o = json::Value::object(); o.set("file", s.file); o.set("time", s.time); a.push(o); }
    v.set("snaps", a);
    std::string t = v.dump(1);
    return atomic_write_file(join(dir, "meta.json"), t.data(), t.size(), err);
}
} // namespace

std::string now_iso() {
    char b[32];
    std::time_t t = std::time(nullptr);
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return b;
}

std::string root() {
    static std::string r;
    if (!r.empty()) return r;
    if (const char* e = std::getenv("PCBP_RECOVERY_DIR"); e && *e) r = e;
    else {
#ifdef _WIN32
        wchar_t* p = nullptr;
        if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p) == S_OK && p) { r = wide_to_utf8(p) + "\\PCBPlotter\\Recovery"; }
        if (p) CoTaskMemFree(p);
        if (r.empty()) { wchar_t t[MAX_PATH]; GetTempPathW(MAX_PATH, t); r = wide_to_utf8(t) + "PCBPlotter\\Recovery"; }
#else
        const char* h = std::getenv("HOME");
        r = std::string(h ? h : "/tmp") + "/.local/share/PCBPlotter/Recovery";
#endif
    }
    mkdirs(r);
    return r;
}
std::string crash_dir() { std::string d = join(root(), "crash"); mkdirs(d); return d; }

const std::string& session_id() {
    static std::string s;
    if (s.empty()) {
        char b[48];
        std::time_t t = std::time(nullptr);
        char ts[20]; std::strftime(ts, sizeof ts, "%Y%m%d%H%M%S", std::localtime(&t));
        std::snprintf(b, sizeof b, "%d-%s", my_pid(), ts);
        s = b;
    }
    return s;
}

bool begin_session(std::string& err) {
    const std::string sd = join(root(), "sessions");
    if (!mkdirs(sd)) { err = "Cannot create " + sd; return false; }
    const std::string lp = join(sd, session_id() + ".lock");
    std::string body = "pid=" + std::to_string(my_pid()) + "\nstarted=" + now_iso() + "\n";
#ifdef _WIN32
    g_lock = CreateFileW(wide(lp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_lock == INVALID_HANDLE_VALUE) { err = "Cannot create the session lock " + lp; return false; }
    DWORD w = 0; WriteFile(g_lock, body.data(), (DWORD)body.size(), &w, nullptr); FlushFileBuffers(g_lock);
#else
    g_lock = ::open(lp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (g_lock < 0) { err = "Cannot create the session lock " + lp; return false; }
    ::flock(g_lock, LOCK_EX);
    if (::write(g_lock, body.data(), body.size()) < 0) {}
#endif
    return true;
}

void end_session() {
    const std::string lp = join(join(root(), "sessions"), session_id() + ".lock");
#ifdef _WIN32
    if (g_lock != INVALID_HANDLE_VALUE) { CloseHandle(g_lock); g_lock = INVALID_HANDLE_VALUE; }
#else
    if (g_lock >= 0) { ::close(g_lock); g_lock = -1; }
#endif
    rm_file(lp);
}

std::string project_id(const std::string& original_path, int untitled_serial) {
    if (original_path.empty()) return "untitled-" + session_id() + "-" + std::to_string(untitled_serial);
    uint64_t h = 1469598103934665603ull;   // FNV-1a over the lower-cased path
    for (char c : original_path) { h ^= (uint8_t)(c >= 'A' && c <= 'Z' ? c + 32 : c == '/' ? '\\' : c); h *= 1099511628211ull; }
    std::string stem = file_stem(original_path);
    for (auto& c : stem) if (!(isalnum((unsigned char)c) || c == '-' || c == '_')) c = '_';
    if (stem.size() > 40) stem.resize(40);
    char b[24]; std::snprintf(b, sizeof b, "%016llx", (unsigned long long)h);
    return stem + "-" + b;
}

bool write_snapshot(const std::string& id, const std::string& original_path, const std::string& display_name,
                    const std::string& js, std::string& err) {
    std::lock_guard<std::mutex> lk(g_mx);
    const std::string dir = join(root(), id);
    if (!mkdirs(dir)) { err = "Cannot create " + dir; return false; }
    Entry e;
    read_meta(dir, e);
    // next slot: the oldest of kKeepSnapshots files (never overwrites the newest valid snapshot)
    int slot = 0;
    if (!e.snaps.empty()) {
        const std::string& last = e.snaps.back().file;   // "snap<k>.pcbp"
        int k = last.size() > 4 ? std::atoi(last.c_str() + 4) : 0;
        slot = (k + 1) % kKeepSnapshots;
    }
    const std::string fname = "snap" + std::to_string(slot) + ".pcbp";
    if (!atomic_write_file(join(dir, fname), js.data(), js.size(), err)) return false;
    e.original_path = original_path;
    e.display_name = display_name;
    e.session = session_id();
    e.clean = false; e.closed.clear();
    e.snaps.erase(std::remove_if(e.snaps.begin(), e.snaps.end(), [&](const Snapshot& s) { return s.file == fname; }), e.snaps.end());
    e.snaps.push_back({fname, now_iso()});
    while ((int)e.snaps.size() > kKeepSnapshots) e.snaps.erase(e.snaps.begin());
    return write_meta(dir, e, err);
}

bool adopt(const std::string& id, std::string& err) {
    std::lock_guard<std::mutex> lk(g_mx);
    const std::string dir = join(root(), id);
    Entry e;
    if (!read_meta(dir, e)) { err = "No recovery data for " + id; return false; }
    e.session = session_id();
    return write_meta(dir, e, err);
}

void remove_project(const std::string& id) {
    if (id.empty()) return;
    std::lock_guard<std::mutex> lk(g_mx);
    const std::string dir = join(root(), id);
    for (auto& f : list_dir(dir, false)) rm_file(join(dir, f));
    rm_dir(dir);
}

std::vector<Entry> list_entries() {
    std::vector<Entry> out;
    const std::string r = root();
    for (auto& d : list_dir(r, true)) {
        if (d == "sessions" || d == "crash") continue;
        Entry e;
        e.id = d; e.dir = join(r, d);
        if (!read_meta(e.dir, e)) continue;
        e.alive = session_alive(e.session);
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<Entry> find_unfinished() {
    std::vector<Entry> out;
    for (auto& e : list_entries()) if (!e.alive && !e.clean && !e.snaps.empty()) out.push_back(std::move(e));
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.snaps.back().time > b.snaps.back().time; });
    return out;
}

void cleanup_stale_sessions() {
    const std::string sd = join(root(), "sessions");
    std::map<std::string, int> used;
    for (auto& e : list_entries()) used[e.session]++;
    for (auto& f : list_dir(sd, false)) {
        if (f.size() < 6 || f.substr(f.size() - 5) != ".lock") continue;
        std::string sid = f.substr(0, f.size() - 5);
        if (sid == session_id() || used.count(sid) || session_alive(sid)) continue;
        rm_file(join(sd, f));
    }
}

void close_clean(const std::string& id) {
    if (id.empty()) return;
    std::lock_guard<std::mutex> lk(g_mx);
    const std::string dir = join(root(), id);
    Entry e;
    if (!read_meta(dir, e)) return;
    if (e.snaps.empty()) { for (auto& f : list_dir(dir, false)) rm_file(join(dir, f)); rm_dir(dir); return; }
    e.clean = true; e.closed = now_iso();
    std::string err;
    write_meta(dir, e, err);
}

int prune_clean(int days) {
    char b[32];
    std::time_t t = std::time(nullptr) - (std::time_t)days * 86400;
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    const std::string cutoff = b;
    int n = 0;
    for (auto& e : list_entries()) {
        if (e.alive || !e.clean) continue;
        const std::string when = !e.closed.empty() ? e.closed : (e.snaps.empty() ? std::string() : e.snaps.back().time);
        if (when.empty() || when < cutoff) { remove_project(e.id); ++n; }
    }
    return n;
}

bool delete_snapshot(const std::string& id, const std::string& file) {
    std::lock_guard<std::mutex> lk(g_mx);
    const std::string dir = join(root(), id);
    Entry e;
    if (!read_meta(dir, e)) return false;
    rm_file(join(dir, file));
    e.snaps.erase(std::remove_if(e.snaps.begin(), e.snaps.end(), [&](const Snapshot& s) { return s.file == file; }), e.snaps.end());
    if (e.snaps.empty()) { for (auto& f : list_dir(dir, false)) rm_file(join(dir, f)); rm_dir(dir); return true; }
    std::string err;
    return write_meta(dir, e, err);
}

uint64_t snapshot_bytes(const Entry& e, const Snapshot& s) {
    std::string text, err;
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(wide(join(e.dir, s.file)).c_str(), GetFileExInfoStandard, &fa)) return ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    return 0;
#else
    if (!read_text_file(join(e.dir, s.file), text, err)) return 0;
    return text.size();
#endif
}

void remove_all() {
    for (auto& e : list_entries()) if (!e.alive || e.session == session_id()) remove_project(e.id);
}

bool take_crash_marker(std::string& text) {
    const std::string m = join(crash_dir(), "last_crash.txt");
    std::string err;
    if (!read_text_file(m, text, err)) return false;
    const std::string seen = join(crash_dir(), "last_crash.seen.txt");
    rm_file(seen);
#ifdef _WIN32
    MoveFileExW(wide(m).c_str(), wide(seen).c_str(), MOVEFILE_REPLACE_EXISTING);
#else
    ::rename(m.c_str(), seen.c_str());
#endif
    return true;
}

} // namespace pcbp::recovery
