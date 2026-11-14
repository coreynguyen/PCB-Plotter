#pragma once
// ============================================================================
// io_util.h — small string / number helpers shared by the pcbp::io importers.
// Portable (no platform headers). All helpers are locale-independent.
// ============================================================================
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pcbp::io {

std::string trim(std::string_view s);
std::string to_lower(std::string_view s);
std::string to_upper(std::string_view s);
bool ieq(std::string_view a, std::string_view b);
bool istarts_with(std::string_view s, std::string_view prefix);
bool icontains(std::string_view hay, std::string_view needle);

// Lower-case and keep only [a-z0-9] ("Center-X(mm)" -> "centerxmm").
std::string norm_key(std::string_view s);

// Strict number parse: whole (trimmed) string must be a number.
// Accepts leading '+', and ',' as decimal point when no '.' is present.
std::optional<double> parse_num(std::string_view s);

// Number followed by an optional unit suffix ("12.5mm", "1234mil", "0.5in",
// "90deg"). suffix receives the lower-cased suffix (may be empty).
std::optional<double> parse_num_suffix(std::string_view s, std::string* suffix);

bool looks_numeric(std::string_view s);         // parse_num_suffix succeeds
bool looks_like_refdes(std::string_view s);     // letters followed by digits ("R12", "U3A", "TP1")

// Split on ASCII whitespace; double-quoted tokens are kept together (quotes removed).
std::vector<std::string> split_ws_quoted(std::string_view line);

// Split text into lines (handles \r\n, \n, \r). A UTF-8 BOM is stripped.
std::vector<std::string> split_lines(std::string_view text);

bool read_file(const std::string& path, std::string& out, std::string& err);

// Format a double with fixed decimals using '.' regardless of locale.
std::string fmt_fixed(double v, int decimals);

} // namespace pcbp::io
