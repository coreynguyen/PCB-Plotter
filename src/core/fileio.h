#pragma once
// ============================================================================
// fileio.h — portable whole-file I/O and UTF-8 path helpers for PCB Plotter.
//
// Paths are UTF-8 std::string everywhere in core. On _WIN32 the path is
// converted to UTF-16 and opened with _wfopen; elsewhere plain fopen is used.
// write_file() writes to "<path>.tmp" and renames over the target so a crash
// never leaves a half-written file. No exceptions; errors via bool + err.
// ============================================================================
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pcbp {

bool read_file(const std::string& utf8_path, std::vector<uint8_t>& out, std::string& err);
bool write_file(const std::string& utf8_path, const void* data, size_t n, std::string& err);
// R21 (AUDIT_V20 P1): write_file() with verification. The temp file is flushed, read back and compared
// byte for byte BEFORE it replaces the destination (a mismatch keeps the previous file); after the
// replace the destination is read back again. Returns false (err set, `stage` = "write" / "verify-temp" /
// "replace" / "verify") on any failure. On Windows the replace is MoveFileExW(REPLACE_EXISTING |
// WRITE_THROUGH): the old destination stays in place if the replace fails.
bool write_file_verified(const std::string& utf8_path, const void* data, size_t n, std::string& err,
                         std::string* stage = nullptr);
bool read_text_file(const std::string& utf8_path, std::string& out, std::string& err);

// Manual UTF-8 <-> UTF-16/32 (wchar_t width) conversion. Invalid input
// sequences are replaced with U+FFFD; never throws.
std::wstring utf8_to_wide(const std::string& s);
std::string wide_to_utf8(const std::wstring& w);

// Append one code point to a UTF-8 string (invalid -> U+FFFD).
void utf8_append(std::string& out, uint32_t cp);

// Path helpers (accept both '/' and '\\' separators).
std::string file_ext_lower(const std::string& path);  // ".mdb" (with dot), "" if none
std::string file_stem(const std::string& path);       // "cclib" for "C:\\x\\cclib.mdb"
std::string file_dir(const std::string& path);        // "C:\\x" (no trailing sep), "" if none

} // namespace pcbp
