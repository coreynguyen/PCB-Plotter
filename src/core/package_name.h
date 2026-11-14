#pragma once
// ============================================================================
// package_name.h — package-name equivalence for BOM <-> board comparison
// (header-only, portable; see specs/BOM_PACKAGES.md).
//
// A BOM's package text and the project's package names come from different
// tools: "0402", "C0402", "CAP_0402", "CAPC1005X55N" (IPC-7351, metric body
// code) all describe the same chip body. package_key() reduces a name to a
// comparison key; packages_equivalent() compares keys. Only used to decide
// whether a BOM-supplied package DISAGREES — names are never rewritten.
// ============================================================================
#include <cctype>
#include <string>

namespace pcbp {

// metric body code (IPC-7351 "CAPC1005") -> imperial chip size
inline const char* pkg_metric_to_imperial(const std::string& m) {
    static const char* tab[][2] = {{"0402", "01005"}, {"0603", "0201"}, {"1005", "0402"}, {"1608", "0603"}, {"2012", "0805"},
                                   {"3216", "1206"}, {"3225", "1210"}, {"4532", "1812"}, {"5025", "2010"}, {"6332", "2512"}};
    for (auto& r : tab) if (m == r[0]) return r[1];
    return nullptr;
}

inline std::string package_key(const std::string& in) {
    std::string u;
    for (unsigned char ch : in) {
        if (std::isspace(ch) || ch == '-' || ch == '_' || ch == '.') continue;
        u += (char)std::toupper(ch);
    }
    if (u.empty()) return u;
    auto digits_at = [&](size_t pos, size_t n) {
        if (pos + n > u.size()) return false;
        for (size_t k = 0; k < n; ++k) if (!std::isdigit((unsigned char)u[pos + k])) return false;
        return true;
    };
    // IPC-7351 chip names: CAPC1005X55N, RESC1608X55N, INDC2012..., CAPCP3216..., LEDC1608, DIOC..., FUSC...
    static const char* ipc[] = {"CAPCP", "CAPC", "RESC", "INDC", "LEDC", "DIOC", "FUSC", "CAPM", "RESM", "INDM"};
    for (const char* pre : ipc) {
        const size_t n = std::char_traits<char>::length(pre);
        if (u.compare(0, n, pre) == 0 && digits_at(n, 4) && (u.size() == n + 4 || u[n + 4] == 'X')) {
            if (const char* imp = pkg_metric_to_imperial(u.substr(n, 4))) return imp;
        }
    }
    // generic chip names: 0402, C0402, R0603, CAP0402, RES0805, SMD0603, 0603M / 1608METRIC (metric)
    static const char* gen[] = {"CAP", "RES", "IND", "SMD", "CHIP", "LED", "C", "R", "L", "D", ""};
    for (const char* pre : gen) {
        const size_t n = std::char_traits<char>::length(pre);
        if (u.compare(0, n, pre) != 0) continue;
        size_t dn = 0;
        while (n + dn < u.size() && std::isdigit((unsigned char)u[n + dn])) ++dn;
        if (dn != 4 && dn != 5) continue;
        std::string code = u.substr(n, dn), rest = u.substr(n + dn);
        if (rest.empty()) return code;
        if (rest == "M" || rest == "METRIC") { if (const char* imp = pkg_metric_to_imperial(code)) return imp; return code + "M"; }
        if (rest == "N" || rest == "L") return code;   // IPC density suffix
    }
    return u;
}

inline bool packages_equivalent(const std::string& a, const std::string& b) {
    return package_key(a) == package_key(b);
}

} // namespace pcbp
