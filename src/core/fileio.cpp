// ============================================================================
// fileio.cpp — see fileio.h.
// ============================================================================
#include "fileio.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <stdio.h>
#include <algorithm>
#endif

namespace pcbp {

void utf8_append(std::string& out, uint32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::wstring utf8_to_wide(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    size_t i = 0, n = s.size();
    while (i < n) {
        uint8_t c = static_cast<uint8_t>(s[i]);
        uint32_t cp;
        int extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { w.push_back(static_cast<wchar_t>(0xFFFD)); ++i; continue; }
        bool ok = i + static_cast<size_t>(extra) < n;  // whole sequence in range
        for (int k = 1; ok && k <= extra; ++k) {
            uint8_t cc = static_cast<uint8_t>(s[i + k]);
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        static const uint32_t kMin[4] = {0, 0x80, 0x800, 0x10000};
        if (!ok || cp < kMin[extra] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            w.push_back(static_cast<wchar_t>(0xFFFD));
            ++i;
            continue;
        }
        i += static_cast<size_t>(extra) + 1;
        if (sizeof(wchar_t) == 2 && cp >= 0x10000) {
            cp -= 0x10000;
            w.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            w.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            w.push_back(static_cast<wchar_t>(cp));
        }
    }
    return w;
}

std::string wide_to_utf8(const std::wstring& w) {
    std::string out;
    out.reserve(w.size());
    for (size_t i = 0; i < w.size(); ++i) {
        uint32_t cp = static_cast<uint32_t>(w[i]);
        if (sizeof(wchar_t) == 2) {
            cp &= 0xFFFF;
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < w.size()) {
                uint32_t lo = static_cast<uint32_t>(w[i + 1]) & 0xFFFF;
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    ++i;
                }
            }
        }
        utf8_append(out, cp);
    }
    return out;
}

static FILE* open_utf8(const std::string& path, const char* mode) {
#ifdef _WIN32
    std::wstring wp = utf8_to_wide(path);
    std::wstring wm = utf8_to_wide(mode);
    return _wfopen(wp.c_str(), wm.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

bool read_file(const std::string& utf8_path, std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    FILE* f = open_utf8(utf8_path, "rb");
    if (!f) {
        err = "cannot open '" + utf8_path + "': " + std::strerror(errno);
        return false;
    }
    uint8_t buf[65536];
    for (;;) {
        size_t got = std::fread(buf, 1, sizeof(buf), f);
        if (got) out.insert(out.end(), buf, buf + got);
        if (got < sizeof(buf)) break;
    }
    bool bad = std::ferror(f) != 0;
    std::fclose(f);
    if (bad) {
        err = "read error on '" + utf8_path + "'";
        out.clear();
        return false;
    }
    return true;
}

bool read_text_file(const std::string& utf8_path, std::string& out, std::string& err) {
    std::vector<uint8_t> raw;
    if (!read_file(utf8_path, raw, err)) return false;
    size_t skip = (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) ? 3 : 0;
    out.assign(reinterpret_cast<const char*>(raw.data()) + skip, raw.size() - skip);
    return true;
}

static void remove_utf8(const std::string& p) {
#ifdef _WIN32
    _wremove(utf8_to_wide(p).c_str());
#else
    std::remove(p.c_str());
#endif
}

// temp file, flushed to disk
static bool write_tmp(const std::string& tmp, const void* data, size_t n, std::string& err) {
    FILE* f = open_utf8(tmp, "wb");
    if (!f) {
        err = "cannot create '" + tmp + "': " + std::strerror(errno);
        return false;
    }
    size_t put = n ? std::fwrite(data, 1, n, f) : 0;
    bool ok = (put == n) && std::fflush(f) == 0;
#ifdef _WIN32
    if (ok) ok = _commit(_fileno(f)) == 0;
#endif
    if (std::fclose(f) != 0) ok = false;
    if (!ok) {
        err = "write error on '" + tmp + "'";
        remove_utf8(tmp);
        return false;
    }
    return true;
}

// R21 (AUDIT_V20 P1): replace the destination in one step. Windows: MoveFileExW(REPLACE_EXISTING |
// WRITE_THROUGH) -- the old file is NOT deleted first, so a failed replace keeps it.
static bool replace_with_tmp(const std::string& tmp, const std::string& dst, std::string& err) {
#ifdef _WIN32
    const std::wstring wdst = utf8_to_wide(dst), wtmp = utf8_to_wide(tmp);
    if (!MoveFileExW(wtmp.c_str(), wdst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const unsigned long e = GetLastError();
        err = "cannot replace '" + dst + "' (Windows error " + std::to_string(e) + "; is it open in another program?). The previous file is unchanged.";
        _wremove(wtmp.c_str());
        return false;
    }
#else
    if (std::rename(tmp.c_str(), dst.c_str()) != 0) {
        err = "cannot rename '" + tmp + "' to '" + dst + "': " + std::strerror(errno) + ". The previous file is unchanged.";
        std::remove(tmp.c_str());
        return false;
    }
#endif
    return true;
}

bool write_file(const std::string& utf8_path, const void* data, size_t n, std::string& err) {
    const std::string tmp = utf8_path + ".tmp";
    if (!write_tmp(tmp, data, n, err)) return false;
    return replace_with_tmp(tmp, utf8_path, err);
}

static bool same_bytes(const std::vector<uint8_t>& a, const void* data, size_t n) {
    return a.size() == n && (n == 0 || std::memcmp(a.data(), data, n) == 0);
}

bool write_file_verified(const std::string& utf8_path, const void* data, size_t n, std::string& err, std::string* stage) {
    auto fail = [&](const char* st) { if (stage) *stage = st; return false; };
    const std::string tmp = utf8_path + ".tmp";
    if (!write_tmp(tmp, data, n, err)) return fail("write");
    {
        std::vector<uint8_t> back; std::string re;
        if (!read_file(tmp, back, re) || !same_bytes(back, data, n)) {
            err = "read-back of the temporary file does not match the generated program" + (re.empty() ? std::string() : " (" + re + ")") +
                  ". The previous file is unchanged.";
            remove_utf8(tmp);
            return fail("verify-temp");
        }
    }
    if (!replace_with_tmp(tmp, utf8_path, err)) return fail("replace");
    std::vector<uint8_t> back; std::string re;
    if (!read_file(utf8_path, back, re) || !same_bytes(back, data, n)) {
        err = "the written file does not read back identical to the generated program" + (re.empty() ? std::string() : " (" + re + ")") + ".";
        return fail("verify");
    }
    if (stage) stage->clear();
    return true;
}

static size_t last_sep(const std::string& p) {
    size_t a = p.find_last_of("/\\");
    return a;
}

std::string file_ext_lower(const std::string& path) {
    size_t s = last_sep(path);
    size_t d = path.find_last_of('.');
    if (d == std::string::npos || (s != std::string::npos && d < s)) return "";
    std::string e = path.substr(d);
    for (char& c : e)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return e;
}

std::string file_stem(const std::string& path) {
    size_t s = last_sep(path);
    std::string name = (s == std::string::npos) ? path : path.substr(s + 1);
    size_t d = name.find_last_of('.');
    if (d != std::string::npos && d > 0) name.resize(d);
    return name;
}

std::string file_dir(const std::string& path) {
    size_t s = last_sep(path);
    if (s == std::string::npos) return "";
    if (s == 0) return path.substr(0, 1);
    return path.substr(0, s);
}

} // namespace pcbp
