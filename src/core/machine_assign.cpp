// machine_assign.cpp — see machine_assign.h (R24b).
#include "machine_assign.h"
#include "project.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace pcbp::mach {

static const char* const kFamNames[] = {"Unknown", "Chip", "MELF", "Tantalum", "LED", "ECap", "Inductor", "SOD", "SOT",
                                        "DPAK", "SOP", "QFN", "QFP", "BGA", "PLCC", "Connector", "Crystal", "Other"};

const char* fam_name(Fam f) {
    int i = (int)f;
    return i >= 0 && i < (int)Fam::Count ? kFamNames[i] : "Unknown";
}
Fam fam_from_name(const std::string& s) {
    for (int i = 0; i < (int)Fam::Count; ++i) if (s == kFamNames[i]) return (Fam)i;
    return Fam::Unknown;
}
bool fam_is_tray(Fam f) { return f == Fam::Qfp || f == Fam::Bga; }

// ---------------------------------------------------------------------------
// text identification
// ---------------------------------------------------------------------------
namespace {

std::string upper_norm(const std::string& s) {
    std::string u;
    u.reserve(s.size() + 2);
    u += ' ';
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '.') u += (char)std::toupper(c);
        else u += ' ';
    }
    u += ' ';
    return u;
}
bool isd(char c) { return c >= '0' && c <= '9'; }

// Imperial chip code -> nominal body L x W (EIA-198 / IEC 60115-8 nominals).
struct ChipSz { const char* imp; const char* met; double l, w, h; };
const ChipSz kChip[] = {
    {"01005", "0402", 0.40, 0.20, 0.13}, {"0201", "0603", 0.60, 0.30, 0.25}, {"0402", "1005", 1.00, 0.50, 0.35},
    {"0603", "1608", 1.60, 0.80, 0.45}, {"0805", "2012", 2.00, 1.25, 0.50}, {"1206", "3216", 3.20, 1.60, 0.55},
    {"1210", "3225", 3.20, 2.50, 0.55}, {"1812", "4532", 4.50, 3.20, 0.60}, {"2010", "5025", 5.00, 2.50, 0.55},
    {"2512", "6332", 6.30, 3.20, 0.55}, {"1008", "2520", 2.50, 2.00, 1.00}, {"0612", "1632", 1.60, 3.20, 0.55},
    {"1806", "4516", 4.50, 1.60, 0.80}, {"2220", "5750", 5.70, 5.00, 1.50}, {"2225", "5763", 5.70, 6.30, 1.50},
    {"0302", "0805M", 0.0, 0, 0}};

// Tantalum / moulded case codes (EIA-535).
struct CaseSz { const char* code; double l, w, h; };
const CaseSz kTant[] = {{"A", 3.2, 1.6, 1.6}, {"B", 3.5, 2.8, 1.9}, {"C", 6.0, 3.2, 2.5}, {"D", 7.3, 4.3, 2.8},
                        {"E", 7.3, 4.3, 4.0}, {"P", 2.0, 1.25, 1.2}, {"R", 2.0, 1.25, 1.2}, {"T", 3.5, 2.8, 1.2},
                        {"V", 7.3, 6.1, 3.5}, {"X", 7.3, 4.3, 4.0}};
const CaseSz kTantMet[] = {{"3216", 3.2, 1.6, 1.6}, {"3528", 3.5, 2.8, 1.9}, {"6032", 6.0, 3.2, 2.5},
                           {"7343", 7.3, 4.3, 2.8}, {"2012", 2.0, 1.25, 1.2}};

struct NamedSz { const char* key; Fam fam; double l, w, h; int pins; };
// Common JEDEC / EIA discrete outlines (nominal body, leads excluded).
const NamedSz kNamed[] = {
    {"SOT23", Fam::Sot, 2.9, 1.3, 1.0, 3}, {"SOT233", Fam::Sot, 2.9, 1.3, 1.0, 3}, {"SOT235", Fam::Sot, 2.9, 1.6, 1.1, 5},
    {"SOT236", Fam::Sot, 2.9, 1.6, 1.1, 6}, {"SOT238", Fam::Sot, 2.9, 1.6, 1.1, 8}, {"SOT323", Fam::Sot, 2.0, 1.25, 0.9, 3},
    {"SOT353", Fam::Sot, 2.0, 1.25, 0.9, 5}, {"SOT363", Fam::Sot, 2.0, 1.25, 0.9, 6}, {"SOT523", Fam::Sot, 1.6, 0.8, 0.7, 3},
    {"SOT563", Fam::Sot, 1.6, 1.2, 0.55, 6}, {"SOT723", Fam::Sot, 1.2, 0.8, 0.5, 3}, {"SOT89", Fam::Sot, 4.5, 2.5, 1.5, 3},
    {"SOT223", Fam::Sot, 6.5, 3.5, 1.6, 4}, {"SOT143", Fam::Sot, 2.9, 1.3, 1.0, 4}, {"TSOT23", Fam::Sot, 2.9, 1.6, 0.9, 5},
    {"SC70", Fam::Sot, 2.0, 1.25, 0.9, 5}, {"SC88", Fam::Sot, 2.0, 1.25, 0.9, 6}, {"SC59", Fam::Sot, 2.9, 1.5, 1.1, 3},
    {"SOD123", Fam::Sod, 2.7, 1.6, 1.1, 2}, {"SOD123F", Fam::Sod, 2.7, 1.6, 1.0, 2}, {"SOD323", Fam::Sod, 1.7, 1.25, 0.9, 2},
    {"SOD523", Fam::Sod, 1.2, 0.8, 0.6, 2}, {"SOD923", Fam::Sod, 0.8, 0.6, 0.4, 2}, {"SOD128", Fam::Sod, 3.8, 2.5, 1.0, 2},
    {"SMA", Fam::Sod, 4.3, 2.6, 2.1, 2}, {"SMB", Fam::Sod, 4.3, 3.6, 2.2, 2}, {"SMC", Fam::Sod, 6.9, 5.9, 2.2, 2},
    {"DO214AC", Fam::Sod, 4.3, 2.6, 2.1, 2}, {"DO214AA", Fam::Sod, 4.3, 3.6, 2.2, 2}, {"DO214AB", Fam::Sod, 6.9, 5.9, 2.2, 2},
    {"POWERDI123", Fam::Sod, 2.7, 1.6, 1.0, 2}, {"MINIMELF", Fam::Melf, 3.5, 1.5, 1.5, 2}, {"MICROMELF", Fam::Melf, 2.0, 1.25, 1.25, 2},
    {"MELF", Fam::Melf, 5.8, 2.2, 2.2, 2}, {"LL34", Fam::Melf, 3.5, 1.5, 1.5, 2}, {"SOD80", Fam::Melf, 3.5, 1.5, 1.5, 2},
    {"DPAK", Fam::Dpak, 6.6, 6.1, 2.3, 3}, {"TO252", Fam::Dpak, 6.6, 6.1, 2.3, 3}, {"D2PAK", Fam::Dpak, 10.2, 9.2, 4.5, 3},
    {"TO263", Fam::Dpak, 10.2, 9.2, 4.5, 3}, {"PLCC2", Fam::Led, 3.5, 2.8, 1.9, 2}, {"PLCC4", Fam::Led, 3.5, 2.8, 1.9, 4},
    {"PLCC6", Fam::Led, 3.5, 2.8, 1.9, 6}};

// LQFP/TQFP body by pin count (JEDEC MS-026 common bodies).
int qfp_body(int pins) {
    switch (pins) {
        case 32: return 7; case 44: return 10; case 48: return 7; case 52: return 10; case 64: return 10;
        case 80: return 12; case 100: return 14; case 120: return 14; case 128: return 14; case 144: return 20;
        case 160: return 24; case 176: return 24; case 208: return 28; case 216: return 24; case 256: return 28;
    }
    return 0;
}

// number right after position (skipping one separator space)
int num_after(const std::string& u, size_t p) {
    while (p < u.size() && u[p] == ' ') ++p;
    int v = 0, n = 0;
    while (p < u.size() && isd(u[p]) && n < 4) { v = v * 10 + (u[p] - '0'); ++p; ++n; }
    return n ? v : 0;
}
// number immediately before position (e.g. "8SOIC", "16 TSSOP")
int num_before(const std::string& u, size_t p) {
    size_t e = p;
    while (e > 0 && u[e - 1] == ' ') --e;
    size_t b = e;
    while (b > 0 && isd(u[b - 1])) --b;
    if (b == e || e - b > 3) return 0;
    if (b > 0 && (std::isalpha((unsigned char)u[b - 1]) || u[b - 1] == '.')) return 0;
    return std::atoi(u.substr(b, e - b).c_str());
}
// "5X5", "5.0X6.0", "3 X 3" -> a, b
bool dims_token(const std::string& u, double& a, double& b, size_t from = 0) {
    for (size_t i = from; i + 2 < u.size(); ++i) {
        if (u[i] != 'X') continue;
        size_t l = i;
        while (l > 0 && u[l - 1] == ' ') --l;
        size_t ls = l;
        while (ls > 0 && (isd(u[ls - 1]) || u[ls - 1] == '.')) --ls;
        size_t r = i + 1;
        while (r < u.size() && u[r] == ' ') ++r;
        size_t re = r;
        while (re < u.size() && (isd(u[re]) || u[re] == '.')) ++re;
        if (ls == l || re == r) continue;
        a = std::atof(u.substr(ls, l - ls).c_str());
        b = std::atof(u.substr(r, re - r).c_str());
        if (a > 0.3 && b > 0.3 && a < 60 && b < 60) return true;
    }
    return false;
}
bool word(const std::string& u, const char* w) {   // whole word (u is space padded)
    std::string k = std::string(" ") + w + " ";
    return u.find(k) != std::string::npos;
}
size_t find_prefix(const std::string& u, const char* w) {   // word start
    std::string k = std::string(" ") + w;
    size_t p = u.find(k);
    return p == std::string::npos ? p : p + 1;
}
std::string squash(const std::string& u, std::vector<bool>* wstart = nullptr) {
    std::string s;
    bool sp = true;
    for (char c : u) {
        if (c == ' ' || c == '.') { sp = true; continue; }
        s += c;
        if (wstart) wstart->push_back(sp);
        sp = false;
    }
    return s;
}

bool chip_code(const std::string& u, PkgId& id) {
    // tokens of 4/5 digits, optionally prefixed by C/R/L/CAP/RES/IND/... ; metric IPC names CAPC1608X..
    for (size_t i = 1; i < u.size(); ++i) {
        if (u[i - 1] != ' ' && !std::isalpha((unsigned char)u[i - 1])) continue;
        if (!isd(u[i]) || (i > 0 && isd(u[i - 1]))) continue;
        size_t e = i;
        while (e < u.size() && isd(u[e])) ++e;
        const std::string d = u.substr(i, e - i);
        if (d.size() != 4 && d.size() != 5) continue;
        // prefix letters
        size_t b = i;
        while (b > 0 && std::isalpha((unsigned char)u[b - 1])) --b;
        const std::string pre = u.substr(b, i - b);
        const std::string post = e < u.size() ? u.substr(e, std::min<size_t>(6, u.size() - e)) : "";
        const bool metric_hint = pre == "CAPC" || pre == "RESC" || pre == "INDC" || pre == "LEDC" || pre == "DIOC" ||
                                 pre == "FUSC" || post.rfind("M ", 0) == 0 || post.rfind("METRIC", 0) == 0;
        const bool ok_pre = pre.empty() || pre == "C" || pre == "R" || pre == "L" || pre == "D" || pre == "CAP" ||
                            pre == "RES" || pre == "IND" || pre == "SMD" || pre == "CHIP" || pre == "LED" || metric_hint ||
                            pre == "CC" || pre == "RC" || pre == "CR" || pre == "FB";
        if (!ok_pre) continue;
        if (!post.empty() && std::isalpha((unsigned char)post[0]) && post[0] != 'M' && post[0] != 'X' && post[0] != 'N' &&
            post[0] != 'L')
            continue;
        for (const auto& c : kChip) {
            if (!c.l) continue;
            if ((metric_hint && d == c.met) || (!metric_hint && d == c.imp)) {
                id.fam = Fam::Chip; id.token = c.imp; id.l = c.l; id.w = c.w; id.h = c.h; id.pins = 2;
                return true;
            }
        }
    }
    return false;
}

} // namespace

PkgId identify(const std::string& name, const std::string& desc) {
    PkgId id;
    for (int pass = 0; pass < 2; ++pass) {
        const std::string& src = pass == 0 ? name : desc;
        if (src.empty()) continue;
        const std::string u = upper_norm(src);
        std::vector<bool> ws;
        const std::string s = squash(u, &ws);
        auto pins_near = [&](size_t p, size_t klen) {
            int n = num_after(u, p + klen);
            if (!n) n = num_before(u, p);
            return n;
        };
        // ---- area-array / quad families first (most specific)
        static const char* bga[] = {"LFBGA", "TFBGA", "VFBGA", "WFBGA", "FBGA", "UBGA", "CSPBGA", "PBGA", "CBGA", "BGA", "WLCSP", "CSP"};
        for (const char* k : bga) {
            size_t p = find_prefix(u, k);
            if (p == std::string::npos) p = u.find(k);
            if (p != std::string::npos) {
                id.fam = Fam::Bga; id.token = k; id.pins = pins_near(p, std::char_traits<char>::length(k));
                double a, b; if (dims_token(u, a, b)) { id.l = std::max(a, b); id.w = std::min(a, b); }
                return id;
            }
        }
        static const char* qfp[] = {"LQFP", "TQFP", "PQFP", "BQFP", "VQFP", "HQFP", "MQFP", "QFP"};
        for (const char* k : qfp) {
            size_t p = u.find(k);
            if (p != std::string::npos) {
                id.fam = Fam::Qfp; id.token = k; id.pins = pins_near(p, std::char_traits<char>::length(k));
                double a, b;
                if (dims_token(u, a, b)) { id.l = std::max(a, b); id.w = std::min(a, b); }
                else if (int bd = qfp_body(id.pins)) { id.l = id.w = bd; }
                id.h = 1.4;
                return id;
            }
        }
        static const char* qfn[] = {"VQFN", "WQFN", "UQFN", "TQFN", "QFN", "DFN", "UDFN", "WDFN", "TDFN", "VSON", "WSON", "USON", "SON", "MLF", "LGA"};
        for (const char* k : qfn) {
            size_t p = find_prefix(u, k);
            if (p == std::string::npos) { size_t q = u.find(k); if (q != std::string::npos && (k[0] != 'S' || q == 0 || !std::isalpha((unsigned char)u[q - 1]))) p = q; }
            if (p != std::string::npos) {
                id.fam = Fam::Qfn; id.token = k; id.pins = pins_near(p, std::char_traits<char>::length(k));
                double a, b; if (dims_token(u, a, b)) { id.l = std::max(a, b); id.w = std::min(a, b); }
                id.h = 0.9;
                return id;
            }
        }
        if (u.find("PLCC") != std::string::npos && u.find("PLCC2") == std::string::npos && u.find("PLCC4") == std::string::npos &&
            u.find("PLCC6") == std::string::npos) {
            id.fam = Fam::Plcc; id.token = "PLCC"; id.pins = pins_near(u.find("PLCC"), 4); return id;
        }
        // ---- named discrete outlines (squashed so "SOT-23-5" == "SOT235")
        {
            const NamedSz* best = nullptr; size_t bl = 0;
            for (const auto& n : kNamed) {
                const size_t kl = std::char_traits<char>::length(n.key);
                size_t p = s.find(n.key);
                if (p == std::string::npos || kl <= bl) continue;
                if (p > 0 && !ws[p] && std::isalpha((unsigned char)s[p - 1]) && n.key[0] != 'T') continue;   // word boundary ("TSOT23" own key)
                const char nx = p + kl < s.size() ? s[p + kl] : 0;
                if (n.key == std::string("SMA") || n.key == std::string("SMB") || n.key == std::string("SMC")) {
                    if (!word(u, n.key) && !(nx == 'J' || nx == 'F')) continue;
                }
                if (isd(nx) && std::string(n.key).back() >= '0' && std::string(n.key).back() <= '9' &&
                    n.fam == Fam::Sot && std::string(n.key).size() <= 5 && nx != '3' && nx != '5' && nx != '6' && nx != '8')
                    continue;
                best = &n; bl = kl;
            }
            if (best) {
                id.fam = best->fam; id.token = best->key; id.l = best->l; id.w = best->w; id.h = best->h; id.pins = best->pins;
                return id;
            }
        }
        // ---- SO families
        static const char* sop[] = {"TSSOP", "HTSSOP", "SSOP", "QSOP", "MSOP", "VSSOP", "TSOP", "HSOP", "SOIC", "SOP", "SOJ", "SOW", "SOI", "SO"};
        for (const char* k : sop) {
            size_t p = find_prefix(u, k);
            const size_t kl = std::char_traits<char>::length(k);
            if (p == std::string::npos) {
                // "8SOIC", "16-SOIC" (number glued in front)
                size_t q = u.find(k);
                if (q != std::string::npos && q > 0 && isd(u[q - 1])) p = q;
            }
            if (p == std::string::npos) continue;
            if ((std::string(k) == "SO" || std::string(k) == "SOI") && p + kl < u.size() && std::isalpha((unsigned char)u[p + kl])) continue;
            id.fam = Fam::Sop; id.token = k; id.pins = pins_near(p, kl);
            const std::string ks = k;
            const double pitch = (ks == "TSSOP" || ks == "HTSSOP" || ks == "MSOP" || ks == "VSSOP") ? 0.65
                               : (ks == "SSOP" || ks == "QSOP") ? 0.635 : 1.27;
            const double bw = (ks == "TSSOP" || ks == "HTSSOP") ? 4.4 : (ks == "MSOP" || ks == "VSSOP") ? 3.0
                            : (ks == "SSOP") ? 5.3 : (ks == "QSOP") ? 3.9 : (u.find("WIDE") != std::string::npos || ks == "SOW") ? 7.5 : 3.9;
            const int n = id.pins > 0 ? id.pins : 8;
            id.w = bw; id.l = std::max(bw, n / 2 * pitch + (pitch > 1 ? 0.0 : 0.4)); id.h = pitch > 1 ? 1.5 : 1.0;
            if (id.l < id.w) std::swap(id.l, id.w);
            return id;
        }
        // ---- connectors / crystals
        if (word(u, "CONN") || u.find("CONNECTOR") != std::string::npos || u.find("HEADER") != std::string::npos ||
            word(u, "HDR") || u.find("RECEPTACLE") != std::string::npos || word(u, "USB") || u.find("TERMINAL") != std::string::npos) {
            id.fam = Fam::Conn; id.token = "CONN"; return id;
        }
        if (word(u, "XTAL") || u.find("CRYSTAL") != std::string::npos || word(u, "OSC") || u.find("OSCILLATOR") != std::string::npos ||
            u.find("RESONATOR") != std::string::npos) {
            id.fam = Fam::Xtal; id.token = "XTAL";
            double a, b; if (dims_token(u, a, b)) { id.l = std::max(a, b); id.w = std::min(a, b); }
            return id;
        }
        // ---- electrolytic / polymer cans
        if (u.find("ELECT") != std::string::npos || u.find("ALUM") != std::string::npos || word(u, "ECAP") ||
            u.find("CAPAE") != std::string::npos || u.find("POLYMER") != std::string::npos) {
            id.fam = Fam::ECap; id.token = "ECAP";
            double a, b; if (dims_token(u, a, b)) { id.l = id.w = std::min(a, b) + 0.3; id.h = std::max(a, b); }
            return id;
        }
        // ---- tantalum (case letter or EIA metric)
        if (u.find("TANT") != std::string::npos || u.find("CAPPM") != std::string::npos || u.find("CAPMP") != std::string::npos) {
            id.fam = Fam::Tant;
            for (const auto& c : kTantMet) if (u.find(c.code) != std::string::npos) { id.token = c.code; id.l = c.l; id.w = c.w; id.h = c.h; return id; }
            for (const auto& c : kTant) {
                if (word(u, (std::string("CASE ") + c.code).c_str()) || word(u, c.code) ||
                    word(u, (std::string(c.code) + "CASE").c_str())) { id.token = std::string(c.code) + " case"; id.l = c.l; id.w = c.w; id.h = c.h; return id; }
            }
            id.token = "TANT"; return id;
        }
        // ---- chip sizes
        if (chip_code(u, id)) {
            if (u.find("LED") != std::string::npos) id.fam = Fam::Led;
            else if (word(u, "IND") || u.find("INDUCTOR") != std::string::npos || word(u, "FERRITE") || word(u, "BEAD") ||
                     u.find("INDC") != std::string::npos) id.fam = Fam::Chip;
            return id;
        }
        if (u.find("LED") != std::string::npos) {
            id.fam = Fam::Led; id.token = "LED";
            double a, b; if (dims_token(u, a, b)) { id.l = std::max(a, b); id.w = std::min(a, b); }
            if (pass == 0) return id;
        }
        if (word(u, "IND") || u.find("INDUCTOR") != std::string::npos || u.find("CHOKE") != std::string::npos) {
            id.fam = Fam::Inductor; id.token = "IND";
            double a, b; if (dims_token(u, a, b)) { id.l = std::max(a, b); id.w = std::min(a, b); }
            return id;
        }
    }
    return id;
}

// ---------------------------------------------------------------------------
// tables
// ---------------------------------------------------------------------------
int Step::eval(double s, double l, double* pur) const {
    if (vals.empty()) return 0;
    const double x = feature == 1 ? l : s;
    for (size_t i = 0; i < vals.size(); ++i) {
        if (i + 1 == vals.size() || x <= bounds[i]) {
            if (pur) *pur = i < purity.size() ? purity[i] : 0.5;
            return vals[i];
        }
    }
    return vals.back();
}

// Format:  step <key> <short|long> <n> <bound>:<val>:<purity> ... *:<val>:<purity>
//          const <key> <int>
//          source <text>
bool parse_tables(const std::string& text, Tables& out, std::string& err) {
    out = Tables{};
    std::istringstream in(text);
    std::string line;
    int ln = 0;
    while (std::getline(in, line)) {
        ++ln;
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string kind, key;
        ls >> kind;
        if (kind == "source") { std::getline(ls, out.source); if (!out.source.empty() && out.source[0] == ' ') out.source.erase(0, 1); continue; }
        ls >> key;
        if (kind == "const") { int v = 0; ls >> v; out.consts[key] = v; continue; }
        if (kind != "step") { err = "line " + std::to_string(ln) + ": unknown '" + kind + "'"; return false; }
        Step st;
        std::string feat; ls >> feat >> st.n;
        st.feature = feat == "long" ? 1 : 0;
        std::string tok;
        while (ls >> tok) {
            const size_t a = tok.find(':'), b = tok.rfind(':');
            if (a == std::string::npos || a == b) { err = "line " + std::to_string(ln) + ": bad band '" + tok + "'"; return false; }
            const std::string bs = tok.substr(0, a);
            st.bounds.push_back(bs == "*" ? 1e9 : std::atof(bs.c_str()));
            st.vals.push_back(std::atoi(tok.substr(a + 1, b - a - 1).c_str()));
            st.purity.push_back(std::atof(tok.substr(b + 1).c_str()));
        }
        if (st.vals.empty()) { err = "line " + std::to_string(ln) + ": empty step"; return false; }
        out.steps[key] = st;
    }
    return true;
}

std::string format_tables(const Tables& t) {
    std::string o;
    char b[96];
    if (!t.source.empty()) o += "source " + t.source + "\n";
    for (const auto& kv : t.steps) {
        const Step& s = kv.second;
        std::snprintf(b, sizeof b, "step %s %s %d", kv.first.c_str(), s.feature ? "long" : "short", s.n);
        o += b;
        for (size_t i = 0; i < s.vals.size(); ++i) {
            if (i + 1 == s.vals.size()) std::snprintf(b, sizeof b, " *:%d:%.3f", s.vals[i], s.purity[i]);
            else std::snprintf(b, sizeof b, " %.3f:%d:%.3f", s.bounds[i], s.vals[i], s.purity[i]);
            o += b;
        }
        o += "\n";
    }
    for (const auto& kv : t.consts) o += "const " + kv.first + " " + std::to_string(kv.second) + "\n";
    return o;
}

static const char* const kEmbedded =
#include "machine_tables.inc"
    ;

const Tables& default_tables() {
    static Tables t = [] {
        Tables r; std::string err;
        parse_tables(kEmbedded, r, err);
        return r;
    }();
    return t;
}

// ---------------------------------------------------------------------------
// house rules + encodings
// ---------------------------------------------------------------------------
bool nozzle_allowed(int n) { return n == 500 || (n >= 504 && n <= 508); }
int remap_nozzle(int n) {
    if (nozzle_allowed(n)) return n;
    if (n == 501) return 500;             // no 501: next allowed by size
    if (n == 502 || n == 503) return 504; // no 503
    if (n >= 509 && n <= 520) return 508;
    if (n > 0 && n < 500) return 500;
    return 0;
}
double supply_angle_for(const std::string& p) {
    if (p == "Tray") return 180;
    if (p == "Stick") return 270;
    return 0;
}
int h8h_tape_code(int w, const std::string& carrier) {
    const bool paper = carrier == "Paper";
    switch (w) {
        case 8: return paper ? 1 : 0x102;
        case 12: return 0x103;
        case 16: return 0x104;
        case 24: return 0x105;
        case 32: return paper ? 6 : 0x107;
        case 44: return 0x108;
        case 56: return 0x109;
        case 72: return 0x10A;
    }
    return 0;
}
int h8h_pitch_code(int p) {
    switch (p) {
        case 2: return 0x0002; case 4: return 0x0004; case 8: return 0x0108; case 12: return 0x010C;
        case 16: return 0x0310; case 20: return 0x0314; case 24: return 0x1118; case 32: return 0x1120;
    }
    return 0;
}
void h8h_decode_tape(int code, int& w, std::string& carrier) {
    w = 0; carrier.clear();
    static const int ws[] = {0, 8, 8, 12, 16, 24, 32, 32, 44, 56, 72};
    const int lo = code & 0xFF;
    if (lo < 1 || lo > 10) return;
    w = ws[lo];
    carrier = (lo == 1 || lo == 6) ? "Paper" : "Embossed";
}
int h8h_decode_pitch(int code) {
    const int lo = code & 0xFF;
    if (lo < 200) return lo;
    switch (lo) { case 0xC9: return 8; case 0xCA: case 0xCC: return 12; case 0xCB: return 28; case 0xCD: return 24;
                  case 0xCE: return 16; case 0xCF: return 32; case 0xD0: return 20; }
    return 0;
}

// ---------------------------------------------------------------------------
// suggestion
// ---------------------------------------------------------------------------
static std::string fmt1(double v) { char b[32]; std::snprintf(b, sizeof b, "%.2f", v); return b; }

Suggestion suggest(const Input& in, const Tables& t) {
    Suggestion sg;
    const PkgId id = identify(in.name, in.desc);
    sg.fam = id.fam; sg.token = id.token;
    double idc = 0;
    std::string why;
    if (id.fam != Fam::Unknown) { why = std::string(fam_name(id.fam)) + (id.token.empty() ? "" : " (" + id.token + ")"); idc = 0.85; }
    else why = "package not recognised";
    // body: CAD body dims win (laser measures the body), else nominal from the package code
    if (in.body_x > 0 && in.body_y > 0) {
        sg.body_s = std::min(in.body_x, in.body_y); sg.body_l = std::max(in.body_x, in.body_y); sg.dims_src = "cad";
        idc = id.fam != Fam::Unknown ? 0.95 : 0.6;
        // sanity: text nominal disagrees strongly with CAD -> lower confidence
        if (id.l > 0 && (std::fabs(id.l - sg.body_l) > 0.35 * id.l + 0.2 || std::fabs(id.w - sg.body_s) > 0.35 * id.w + 0.2)) {
            idc = 0.6; why += "; CAD body differs from nominal " + fmt1(id.l) + "x" + fmt1(id.w);
        }
    } else if (id.l > 0) {
        sg.body_s = std::min(id.l, id.w); sg.body_l = std::max(id.l, id.w); sg.dims_src = "text";
    } else {
        if (id.fam != Fam::Unknown) idc = 0.3;
    }
    if (sg.body_l > 0) why += "; body " + fmt1(sg.body_l) + "x" + fmt1(sg.body_s) + " (" + sg.dims_src + ")";
    const std::string fk = fam_name(id.fam);
    auto step = [&](const std::string& pre, const std::string& key, double* pur, std::string* used) -> int {
        auto it = t.steps.find(pre + key);
        if (it == t.steps.end() || it->second.n < 8) { it = t.steps.find(pre + "ALL"); if (used) *used = "ALL"; }
        else if (used) *used = key;
        if (it == t.steps.end()) return 0;
        return it->second.eval(sg.body_s, sg.body_l, pur);
    };
    double conf = idc;
    // ---- packaging (house rule)
    std::string pk = in.pn_packaging;
    std::string pk_why;
    if (pk == "MTC") { pk = "Tray"; pk_why = "MTC not used -> Tray"; }
    if (fam_is_tray(id.fam) && pk != "Tray") { pk = "Tray"; pk_why = std::string(fam_name(id.fam)) + " -> tray"; }
    else if (pk.empty()) { pk = "Tape"; pk_why = "cut tape (default)"; }
    else if (pk_why.empty()) pk_why = "part packaging " + pk;
    sg.f.packaging = pk;
    sg.f.supply_angle = supply_angle_for(pk);
    why += "; " + pk_why + ", angle " + std::to_string((int)sg.f.supply_angle);
    // ---- nozzle
    if (sg.body_l > 0) {
        double pur = 0; std::string used;
        int n = step("noz.", fk, &pur, &used);
        const int r = remap_nozzle(n);
        if (r) {
            sg.f.nozzle = r;
            why += "; nozzle " + std::to_string(r) + " (band noz." + used + (r != n ? ", remapped from " + std::to_string(n) : "") + ")";
            conf *= 0.5 + 0.5 * pur;
        }
    } else conf *= 0.5;
    // ---- tape
    if (pk == "Tape" && sg.body_l > 0) {
        double pur = 0; std::string used;
        int w = step("tw.", fk, &pur, &used);
        if (w > 0) {
            sg.f.tape_w = w;
            sg.f.carrier = w == 8 ? "Paper" : "Embossed";
            double pp = 0;
            auto it = t.steps.find("pitch." + std::to_string(w));
            int p = it != t.steps.end() ? it->second.eval(sg.body_s, sg.body_l, &pp) : 0;
            if (w == 8 && p > 4) p = 4;      // 8 mm tape: 2 or 4 mm pockets (EIA-481)
            if (p <= 0) p = w == 8 ? 4 : w == 12 ? 8 : 12;
            sg.f.pitch = p;
            why += "; tape " + std::to_string(w) + " mm " + sg.f.carrier + " pitch " + std::to_string(p) + " (tw." + used + ")";
            conf *= 0.7 + 0.3 * std::min(pur, pp > 0 ? pp : pur);
        }
    }
    // ---- class code + centring
    {
        auto it = t.consts.find("type." + fk);
        if (it != t.consts.end()) sg.f.type_code = it->second;
        sg.f.centring = (id.fam == Fam::Bga || (id.fam == Fam::Qfp && sg.body_l > 20) || id.fam == Fam::Conn) ? "Vision" : "Laser";
    }
    sg.conf = std::clamp(conf, 0.0, 1.0);
    sg.rule = why;
    return sg;
}

Fields merge(const Fields& s, const Fields& o) {
    Fields r = s;
    if (o.nozzle) r.nozzle = o.nozzle;
    if (!o.packaging.empty()) {
        r.packaging = o.packaging == "MTC" ? "Tray" : o.packaging;
        if (!o.supply_angle_set) r.supply_angle = supply_angle_for(r.packaging);
        if (r.packaging != "Tape") { r.tape_w = 0; r.pitch = 0; r.carrier.clear(); }
    }
    if (o.tape_w) { r.tape_w = o.tape_w; if (o.carrier.empty()) r.carrier = o.tape_w == 8 ? "Paper" : "Embossed"; }
    if (r.tape_w == 8) r.carrier = "Paper";       // house rule: 8 mm is always paper
    if (o.pitch) r.pitch = o.pitch;
    if (!o.carrier.empty() && r.tape_w != 8) r.carrier = o.carrier;
    if (o.supply_angle_set) { r.supply_angle = o.supply_angle; r.supply_angle_set = true; }
    if (!o.centring.empty()) r.centring = o.centring;
    if (o.type_code) r.type_code = o.type_code;
    if (!nozzle_allowed(r.nozzle)) r.nozzle = remap_nozzle(r.nozzle);
    return r;
}

} // namespace pcbp::mach

namespace pcbp::mach {

bool package_body(const Package& p, double& bx, double& by) {
    bx = by = 0;
    if (p.body.empty() || p.body_source == "pads") return false;
    const BBox b = gprims_bbox(p.body);
    if (!(b.w() > 0.05 && b.h() > 0.05)) return false;
    bx = b.w(); by = b.h();
    return true;
}

Input input_for(const Package& pk, const PartNumber* pn) {
    Input in;
    in.name = pk.name;
    in.desc = pn ? pn->description : pk.description;
    if (pn) in.pn_packaging = pn->packaging;
    package_body(pk, in.body_x, in.body_y);
    in.body_h = pn && pn->height > 0 ? pn->height : pk.height;
    in.pins = pk.leads;
    return in;
}

Fields override_fields(const Package& pk) {
    Fields f;
    const auto& m = pk.mach;
    f.nozzle = m.nozzle; f.tape_w = m.tape_w; f.pitch = m.pitch; f.type_code = m.type_code;
    f.packaging = m.packaging; f.carrier = m.carrier; f.centring = m.centring;
    f.supply_angle = m.supply_angle; f.supply_angle_set = m.supply_angle_set;
    return f;
}

Fields effective_for(const Package& pk, const PartNumber* pn, Suggestion* out) {
    Suggestion s = suggest(input_for(pk, pn));
    if (out) *out = s;
    return merge(s.f, override_fields(pk));
}

const PartNumber* part_for_package(const Project& prj, const std::string& pkg) {
    for (const auto& c : prj.comps) {
        if (c.package != pkg || c.number.empty()) continue;
        auto it = prj.parts.find(c.number);
        if (it != prj.parts.end()) return &it->second;
    }
    for (const auto& kv : prj.parts) if (kv.second.package == pkg) return &kv.second;
    return nullptr;
}

int assign_all(Project& prj, int which, const std::vector<std::string>& sel) {
    int n = 0;
    for (auto& kv : prj.packages) {
        if (which == 1 && std::find(sel.begin(), sel.end(), kv.first) == sel.end()) continue;
        if (which == 2 && kv.second.mach_assigned) continue;
        kv.second.mach_assigned = true;
        ++n;
    }
    return n;
}

} // namespace pcbp::mach
