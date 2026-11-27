// flexdb.cpp — JUKI FlexLine component database integration (see flexdb.h).
// The classifier is a faithful port of FlexAssign.hta (classify / buildPriors / nearestByDims ...).
#include "flexdb.h"
#include "../core/fileio.h"
#include "machine_history.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <regex>
#include <cctype>

namespace pcbp::flexdb {

const char* const kCopyTables[] = {
    "Mech", "Feeder", "Vision", "Visiondata", "Inspection", "Expansion", "Laser", "LaserFX1", "LaserFx2", "LaserFx3",
    "LaserCX", "LaserGx5", "LaserGx6", "LightControl", "LightControlCX", "LightControlGx5", "LightControlGx6",
    "VCSControlCX", "VCSControlGx5", "VCSControlGx6", "VisionCX", "VisionGx5", "VisionGx6", "Gx4Vision", "Gx4ExtendVision"};
const int kCopyTableCount = (int)(sizeof(kCopyTables) / sizeof(kCopyTables[0]));

// Compo scalars that belong to the package profile (FlexAssign COMPO_SCALARS)
static const char* const kCompoScalars[] = {"PkgClassID", "MoldlineWid", "MoldlineLen", "ConHeight", "UseMech",
    "UseVision", "UseLaser", "CompOrient", "MoldLineOffsetX", "MoldLineOffsetY", "SubNozzle", "GX4PriorityLV"};

namespace {

std::string upper(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }
bool ieq(const std::string& a, const std::string& b) { return upper(a) == upper(b); }
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
// JS: s.toUpperCase().replace(/[^A-Z0-9]+/g, rep)
std::string norm(const std::string& s, const char* rep = " ") {
    std::string u = upper(s), out;
    bool in = false;
    for (char c : u) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (ok) { out += c; in = false; }
        else if (!in) { out += rep; in = true; }
    }
    return out;
}
std::string fmt2(double v) { char b[32]; std::snprintf(b, sizeof b, "%.2f", v); return b; }
std::string fmtg(double v) { char b[32]; std::snprintf(b, sizeof b, "%g", std::round(v * 1000) / 1000); return b; }

using RX = std::regex;
const auto ICASE = std::regex::ECMAScript | std::regex::icase;
bool rs(const RX& r, const std::string& s) { return std::regex_search(s, r); }

// ---- tables from FlexAssign ------------------------------------------------
const int TAPE_WIDTHS[] = {8, 12, 16, 24, 32, 44, 56};
const int TAPE_PITCHES[] = {2, 4, 8, 12, 16, 20, 24, 32, 40};
struct NozStep { double max; const char* nz; };
const NozStep NOZZLE_LADDER[] = {{0.35, "501"}, {1.40, "500"}, {2.00, "504"}, {3.80, "505"}, {7.00, "506"}, {11.00, "507"}, {1e9, "508"}};

struct ChipSz { const char* code; double l, w; };
const ChipSz CHIP[] = {{"01005", 0.40, 0.20}, {"0201", 0.60, 0.30}, {"0402", 1.00, 0.50}, {"0603", 1.60, 0.80},
    {"0805", 2.00, 1.25}, {"1206", 3.20, 1.60}, {"1210", 3.20, 2.50}, {"1806", 4.50, 1.60}, {"1808", 4.50, 2.00},
    {"1812", 4.50, 3.20}, {"1825", 4.50, 6.40}, {"2010", 5.00, 2.50}, {"2220", 5.70, 5.00}, {"2225", 5.70, 6.40},
    {"2512", 6.40, 3.20}, {"0306", 0.80, 1.60}, {"0508", 1.25, 2.00}, {"0612", 1.50, 3.20}, {"0504", 1.25, 1.00}};
// JS object key order for CHIP (integer-like keys first ascending, then insertion order): "0201".. are
// strings with a leading zero, so they keep insertion order; all keys keep insertion order here.

struct TantSz { const char* code; double l, w; };
const TantSz TANT_DIM[] = {{"1608", 1.60, 0.80}, {"2012", 2.00, 1.25}, {"3216", 3.20, 1.60}, {"3528", 3.50, 2.80},
                           {"6032", 6.00, 3.20}, {"7260", 7.20, 6.00}, {"7343", 7.30, 4.30}};
const char* tant_letter(char c) {
    switch (c) {
    case 'A': return "3216"; case 'B': return "3528"; case 'C': return "6032"; case 'D': return "7343"; case 'E': return "7260";
    case 'R': return "2012"; case 'S': return "3216"; case 'T': return "3528"; case 'U': return "6032"; case 'V': return "7343";
    case 'X': return "7343"; case 'Y': return "7343"; case 'J': return "1608"; default: return nullptr;
    }
}
const char* tant_imp(const std::string& c) {
    static const char* m[][2] = {{"0603", "1608"}, {"0805", "2012"}, {"1206", "3216"}, {"1411", "3528"},
                                 {"2312", "6032"}, {"2917", "7343"}, {"2924", "7361"}, {"3024", "7660"}};
    for (auto& e : m) if (c == e[0]) return e[1];
    return nullptr;
}
const char* met2imp(const std::string& c) {
    static const char* m[][2] = {{"0603", "0201"}, {"1005", "0402"}, {"1608", "0603"}, {"2012", "0805"}, {"3216", "1206"},
        {"3225", "1210"}, {"4516", "1806"}, {"4520", "1808"}, {"4532", "1812"}, {"4564", "1825"}, {"5025", "2010"},
        {"5650", "2220"}, {"5664", "2225"}, {"6332", "2512"}};
    for (auto& e : m) if (c == e[0]) return e[1];
    return nullptr;
}

const char* const LEAD_FAMS[] = {"HTSSOP", "HVSSOP", "TVSOP", "TSSOP", "VSSOP", "SSOP", "QSOP", "MSOP", "VSOP", "HSOP",
    "TSOP", "PSOP", "SOIC", "SOP", "SOL", "SOJ", "LQFP", "TQFP", "BQFP", "QFP", "X2QFN", "WQFN", "VQFN", "UQFN", "HQFN",
    "QFN", "PLCC", "FBGA", "BBGA", "VFBGA", "BGA", "WLCSP", "CSP", "WSON", "VSON", "UDFN", "VDFN", "WDFN", "DFN", "SON",
    "LLP", "LGA"};
std::string fam_base(const std::string& f) {
    static const char* m[][2] = {{"HTSSOP", "TSSOP"}, {"HVSSOP", "VSSOP"}, {"TVSOP", "TSSOP"}, {"X2QFN", "QFN"},
        {"WQFN", "QFN"}, {"VQFN", "QFN"}, {"UQFN", "QFN"}, {"HQFN", "QFN"}, {"WSON", "SON"}, {"VSON", "SON"},
        {"UDFN", "DFN"}, {"VDFN", "DFN"}, {"WDFN", "DFN"}, {"BBGA", "BGA"}, {"VFBGA", "FBGA"}, {"LGA", "QFN"}};
    for (auto& e : m) if (f == e[0]) return e[1];
    return f;
}
const char* const DISCRETE[] = {"SOT23-8", "SOT23-6", "SOT23-5", "SOT223", "SOT323", "SOT325", "SOT326", "SOT343",
    "SOT353", "SOT363", "SOT523", "SOT563", "SOT663", "SOT665", "SOT666", "SOT143", "SOT89", "SOT23", "SOD923", "SOD723",
    "SOD523", "SOD323", "SOD123", "SOD106", "SOD80", "SC70", "SC79", "SC88", "SC89", "SC90", "D2PAK", "DPAK", "TO252",
    "TO263", "SMA", "SMB", "SMC", "LL34", "LL41", "DO214AA", "DO214AB", "DO214AC", "DO213AA", "DO213AB", "MELF"};

struct PkgE { RX re; double L, W, H; int cls; const char* label; };
const std::vector<PkgE>& pkgdb() {
    static const std::vector<PkgE> db = [] {
        std::vector<PkgE> v;
        auto a = [&](const char* re, double L, double W, double H, int cls, const char* lbl) { v.push_back({RX(re, ICASE), L, W, H, cls, lbl}); };
        a(R"(\bX2QFN[- ]?10\b|\b10[- ]?X2QFN\b)", 2.00, 1.50, 0.40, ClsQfn, "X2QFN-10");
        a(R"(\bWSON[- ]?8\b|\b8[- ]?WSON\b)", 2.00, 2.00, 0.80, ClsQfn, "WSON-8 2x2");
        a(R"(\bVSON[- ]?10\b|\b10[- ]?VSON\b)", 3.00, 3.00, 0.95, ClsQfn, "VSON-10 3x3");
        a(R"(\bVSON[- ]?8\b|\b8[- ]?VSON\b)", 6.00, 5.00, 1.00, ClsQfn, "VSON-8 CLIP 5x6");
        a(R"(\bVQFN[- ]?14\b|\b14[- ]?VQFN\b)", 3.50, 3.50, 0.88, ClsQfn, "VQFN-14");
        a(R"(\bWQFN[- ]?32\b|\b32[- ]?WQFN\b)", 5.00, 5.00, 0.90, ClsQfn, "WQFN-32 5x5");
        a(R"(\bQFN[- ]?20\b|\b20[- ]?QFN\b)", 4.00, 4.00, 0.75, ClsQfn, "QFN-20 4x4");
        a(R"(\bQFN[- ]?32\b|\b32[- ]?QFN\b)", 5.00, 5.00, 0.75, ClsQfn, "QFN-32 5x5");
        a(R"(\bDFN[- ]?6\b|\b6[- ]?DFN\b)", 3.00, 2.00, 0.75, ClsQfn, "DFN-6 3x2");
        a(R"(\bDFN[- ]?8\b|\b8[- ]?DFN\b)", 6.00, 5.00, 0.80, ClsQfn, "DFN-8 6x5");
        a(R"(\bDFN[- ]?12\b|\b12[- ]?DFN\b)", 4.00, 3.00, 0.75, ClsQfn, "DFN-12 4x3");
        a(R"(\bDFN[- ]?14\b|\b14[- ]?DFN\b)", 4.00, 3.00, 0.75, ClsQfn, "DFN-14 4x3");
        a(R"(\bTDSON\b|\bTDSON[- ]?8\b|SUPERSO8)", 5.90, 5.15, 1.00, ClsQfn, "TDSON-8-6 SuperSO8");
        a(R"(\bWHSON\b|PQFN ?5X6|\bIQD\d)", 6.00, 5.00, 0.70, ClsQfn, "PG-WHSON-8 5x6");
        a(R"(LFPAK ?5?6?|\bSOT[- ]?669\b|POWER[- ]?SO8)", 6.00, 5.00, 1.10, ClsSot, "LFPAK56 / Power-SO8");
        a(R"(\bHSOG\b|\bMTOLG\b|\bIPM\d{3}N)", 8.00, 8.00, 1.60, ClsSot, "PG-HSOG-4 mTOLG");
        a(R"(\bSLP1006P2\b)", 1.00, 0.60, 0.50, ClsChip, "SLP1006P2 (0402)");
        a(R"(\bDO[- ]?219AD\b|MICROSMP)", 1.60, 0.80, 0.65, ClsChip, "DO-219AD MicroSMP");
        a(R"(\bWSK[- ]?2512\b)", 6.35, 3.18, 0.89, ClsChip, "WSK-2512 Kelvin");
        a(R"(VLS ?201612)", 2.00, 1.60, 1.20, ClsChip, "VLS201612");
        a(R"(XAL ?6060)", 6.56, 6.36, 6.10, ClsChip, "XAL6060");
        a(R"(XGL ?6060)", 6.71, 6.51, 6.10, ClsChip, "XGL6060");
        a(R"(IHLP ?3232)", 13.20, 12.90, 3.50, ClsChip, "IHLP-3232DZ");
        a(R"(CKG57|\b5750 ?METRIC\b)", 6.00, 5.00, 5.00, ClsChip, "CKG57 stacked MLCC 2220");
        a(R"(\b3024\b.*TANT|TANT.*\b3024\b|\b7660 ?METRIC\b)", 7.60, 6.00, 6.45, ClsChip, "Tant 3024 / 7660 Z case");
        a(R"(TEST ?POINT ?MINIATURE)", 2.67, 1.02, 1.52, ClsChip, "Keystone 5015 test point");
        a(R"(\bSOT[- ]?563\b)", 1.60, 1.60, 0.60, ClsSot, "SOT-563");
        a(R"(\bSOD[- ]?323\b|\bSC[- ]?76\b)", 1.70, 1.25, 0.95, ClsSot, "SOD-323 / SC-76");
        a(R"(\bDO[- ]?216AA\b|\bSTMITE\b)", 2.05, 2.05, 1.15, ClsSot, "DO-216AA STmite");
        a(R"(\bTSOT[- ]?23[- ]?8\b)", 3.00, 3.00, 1.00, ClsSot, "TSOT-23-8");
        a(R"(\bASDMB\b|MEMS ?OSC|OSC ?MEMS)", 2.50, 2.00, 0.90, ClsSot, "MEMS osc 2520");
        a(R"(\bVSSOP[- ]?8\b|\b8[- ]?VSSOP\b)", 2.30, 2.00, 0.80, ClsSop, "VSSOP-8");
        a(R"(\bHTSSOP[- ]?14\b|\b14[- ]?HTSSOP\b)", 5.00, 4.40, 1.05, ClsSop, "HTSSOP-14");
        a(R"(\bHTSSOP[- ]?44\b|\b44[- ]?HTSSOP\b)", 11.00, 6.20, 1.05, ClsSop, "HTSSOP-44");
        a(R"(\bSSOP[- ]?4\b|\b4[- ]?SSOP\b)", 4.40, 4.40, 2.10, ClsSop, "SSOP-4 mini-flat");
        a(R"(\bSOIC[- ]?8\b|\b8[- ]?SOIC\b)", 4.90, 3.90, 1.75, ClsSop, "SOIC-8");
        a(R"(\bTQFP[- ]?64\b|\b64[- ]?TQFP\b)", 10.00, 10.00, 1.00, ClsQfp, "TQFP-64 10x10");
        a(R"(\bBBGA\b.*\b32\b|\b32[- ]?BBGA\b)", 15.00, 11.25, 2.80, ClsBga, "32-BBGA module");
        a(R"(\b121 ?BGA\b|\bBGA[- ]?121\b)", 15.00, 15.00, 4.92, ClsBga, "BGA-121 module");
        a(R"(\b48[- ]?BGA\b|\bBGA[- ]?48\b)", 9.00, 6.25, 3.30, ClsBga, "48-BGA module");
        a(R"(\bLGA[- ]?25\b|\b25[- ]?LGA\b)", 6.25, 6.25, 1.82, ClsFbga, "LGA-25 module");
        a(R"(CAP ?ELEC.*10 ?X ?10|PCR1H)", 10.00, 10.00, 10.00, ClsECap, "Radial can 10x10");
        a(R"(\bLCCI[- ]?50\b|\bP0351NL\b)", 19.56, 17.02, 9.91, ClsOutline, "LCCI-50 choke");
        a(R"(\bHCCI[- ]?80\b|\bPG1265NL\b)", 31.00, 25.40, 12.70, ClsOutline, "HCCI-80 choke");
        a(R"(\bP0502NL\b)", 30.99, 25.40, 12.70, ClsOutline, "4-pad CMC");
        a(R"(\bSMTSO[- ]?440\b)", 5.56, 5.56, 4.09, ClsOutline, "PEM SMTSO-440 standoff");
        a(R"(\bINA241A2)", 4.90, 3.90, 1.75, ClsSop, "SOIC-8");
        a(R"(\bLTM4623)", 6.25, 6.25, 1.82, ClsFbga, "LGA-25 6.25x6.25");
        a(R"(\bMC78113\b)", 10.00, 10.00, 1.00, ClsQfp, "TQFP-64 10x10");
        a(R"(\bDRV8962)", 11.00, 6.20, 1.05, ClsSop, "HTSSOP-44-EP");
        a(R"(\bSN74LVC126A)", 3.50, 3.50, 0.88, ClsQfn, "VQFN-14 3.5x3.5");
        a(R"(\bLTM8073)", 9.00, 6.25, 3.30, ClsBga, "BGA-48 9x6.25");
        a(R"(\bQTH[- ]?030\b)", 21.90, 5.80, 7.26, ClsConn2, "Samtec QTH-030 2x30");
        a(R"(\bSHF[- ]?105\b)", 8.00, 5.00, 4.50, ClsConn2, "Samtec SHF-105 2x5");
        return v;
    }();
    return db;
}
const PkgE* lookup_package(const std::string& txt) {
    std::string u = " " + norm(txt) + " ";
    for (auto& e : pkgdb())
        if (rs(e.re, u) || rs(e.re, txt)) return &e;
    return nullptr;
}

struct Size3 { bool ok = false; double a = 0, b = 0, c = 0; std::string note; };

Size3 size_from_text(const std::string& txt) {
    static const RX re(R"((\d+(?:\.\d+)?)\s*(?:mm)?\s*(?:x|\*|\xC3\x97)\s*(\d+(?:\.\d+)?)\s*(?:mm)?(?:\s*(?:x|\*|\xC3\x97)\s*(\d+(?:\.\d+)?)\s*(?:mm)?)?)", ICASE);
    std::smatch m;
    Size3 s;
    if (!std::regex_search(txt, m, re)) return s;
    double a = std::atof(m[1].str().c_str()), b = std::atof(m[2].str().c_str()), c = m[3].matched ? std::atof(m[3].str().c_str()) : 0;
    if (!(a > 0.3 && b > 0.15 && a < 80 && b < 80)) return s;
    s.ok = true; s.a = std::max(a, b); s.b = std::min(a, b); s.c = c;
    s.note = "body " + fmtg(a) + "x" + fmtg(b) + "mm from text";
    return s;
}

Size3 size_from_partno(const std::string& txt) {
    std::string u;
    for (char c : upper(txt)) u += ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : ' ';
    std::smatch m;
    Size3 s;
    static const RX r1(R"((?:^| )(?:VLS|DFE|LQH|MLZ)\s?(\d{2})(\d{2})(\d{2})(?![0-9]))");
    if (std::regex_search(u, m, r1)) {
        s.ok = true; s.a = std::stoi(m[1]) / 10.0; s.b = std::stoi(m[2]) / 10.0;
        s.note = trim(m[0].str()) + " = " + fmtg(s.a) + "x" + fmtg(s.b) + "mm";
        return s;
    }
    static const RX r2(R"((?:^| )IHLP\s?(\d{2})(\d{2})(?![0-9]))");
    if (std::regex_search(u, m, r2)) {
        std::string code = m[1].str() + m[2].str();
        static const std::map<std::string, std::array<double, 3>> IH = {{"2525", {6.86, 6.47, 3.00}}, {"3232", {13.20, 12.90, 3.50}},
            {"5050", {13.20, 12.90, 3.50}}, {"6767", {17.15, 17.15, 7.00}}, {"4040", {10.80, 10.20, 4.00}}, {"1616", {4.70, 4.70, 2.00}}};
        auto it = IH.find(code);
        s.ok = true;
        if (it != IH.end()) { s.a = it->second[0]; s.b = it->second[1]; s.note = "IHLP" + code + " = " + fmtg(s.a) + "x" + fmtg(s.b) + "mm (datasheet)"; }
        else { s.a = s.b = std::stoi(m[1]) * 0.254; s.note = "IHLP" + code + " = ~" + fmt2(s.a) + "mm (estimated)"; }
        return s;
    }
    static const RX r3(R"((?:^| )(XAL|XGL|XFL|XEL|SRN|SRR|SRP|NR|IHSR)\s?(\d{2})(\d{2})(?![0-9]))");
    if (std::regex_search(u, m, r3)) {
        std::string code = m[1].str() + m[2].str() + m[3].str();
        static const std::map<std::string, std::array<double, 3>> CD = {{"XFL3012", {3.0, 3.0, 1.3}}, {"XAL4020", {4.0, 4.0, 2.1}},
            {"XAL4030", {4.0, 4.0, 3.1}}, {"XAL5020", {5.2, 5.2, 2.1}}, {"XAL5030", {5.2, 5.2, 3.1}}, {"XAL6030", {6.56, 6.36, 3.1}},
            {"XAL6060", {6.56, 6.36, 6.1}}, {"XAL7030", {7.50, 7.50, 3.1}}, {"XAL7050", {7.50, 7.50, 5.1}}, {"XGL4020", {4.0, 4.0, 2.1}},
            {"XGL4030", {4.0, 4.0, 3.1}}, {"XGL6030", {6.71, 6.51, 3.1}}, {"XGL6060", {6.71, 6.51, 6.1}}};
        auto it = CD.find(code);
        s.ok = true;
        if (it != CD.end()) { s.a = it->second[0]; s.b = it->second[1]; s.note = code + " = " + fmtg(s.a) + "x" + fmtg(s.b) + "mm (datasheet)"; }
        else { s.a = s.b = std::stoi(m[2]) / 10.0; s.note = trim(m[0].str()) + " = " + fmtg(s.a) + "mm square (estimated)"; }
        return s;
    }
    static const RX r4(R"((?:^| )MSS\s?(\d{2})(\d{2})(?![0-9]))");
    if (std::regex_search(u, m, r4)) {
        std::string code = m[1].str() + m[2].str();
        static const std::map<std::string, std::array<double, 3>> MD = {{"1278", {12.0, 12.0, 7.8}}, {"1048", {10.5, 10.5, 4.8}}, {"7341", {7.3, 7.3, 4.1}}};
        auto it = MD.find(code);
        s.ok = true;
        if (it != MD.end()) { s.a = it->second[0]; s.b = it->second[1]; s.note = "MSS" + code + " = " + fmtg(s.a) + "x" + fmtg(s.b) + "mm (datasheet)"; }
        else { s.a = s.b = std::stoi(m[1]); s.note = "MSS" + code + " = about " + fmtg(s.a) + "mm square (estimated)"; }
        return s;
    }
    return s;
}

std::vector<std::pair<std::string, double>> dim_sizes(double w, double l) {
    std::vector<std::pair<std::string, double>> out;
    if (!w || !l) return out;
    double a = std::max(w, l), b = std::min(w, l);
    for (auto& c : CHIP) {
        double nl = std::max(c.l, c.w), nw = std::min(c.l, c.w);
        double dx = (a - nl) / nl, dy = (b - nw) / nw, e = std::sqrt(dx * dx + dy * dy);
        if (e <= 0.22) out.push_back({c.code, e});
    }
    std::stable_sort(out.begin(), out.end(), [](auto& x, auto& y) { return x.second < y.second; });
    return out;
}

struct LeadPkg { bool ok = false; std::string base; int pins = 0; std::string fam; };
LeadPkg lead_pkg(const std::string& s) {
    std::string u = norm(s);
    LeadPkg best;
    static std::vector<RX> res = [] {
        std::vector<RX> v;
        for (auto f : LEAD_FAMS) v.emplace_back(std::string("(?:(\\d{1,3})\\s*") + f + "|" + f + "\\s*(\\d{1,3}))(?![0-9])");
        return v;
    }();
    int k = 0;
    for (auto f : LEAD_FAMS) {
        const RX& re = res[(size_t)k++];
        for (auto it = std::sregex_iterator(u.begin(), u.end(), re); it != std::sregex_iterator(); ++it) {
            int n = std::atoi(((*it)[1].matched ? (*it)[1] : (*it)[2]).str().c_str());
            if (n >= 3 && n <= 304)
                if (!best.ok || std::strlen(f) > best.base.size()) { best.ok = true; best.base = fam_base(f); best.pins = n; best.fam = f; }
        }
    }
    if (best.ok) return best;
    static std::vector<RX> bare = [] {
        std::vector<RX> v;
        for (auto f : LEAD_FAMS) v.emplace_back(std::string("(^|[^A-Z0-9])") + f + "([^A-Z0-9]|$)");
        return v;
    }();
    k = 0;
    for (auto f : LEAD_FAMS) {
        if (rs(bare[(size_t)k++], u)) { best.ok = true; best.base = fam_base(f); best.pins = 0; best.fam = f; return best; }
    }
    return best;
}

std::string discrete_pkg(const std::string& s) {
    std::string u;
    for (char c : upper(s)) if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) u += c;
    for (auto d : DISCRETE) {
        std::string k;
        for (const char* p = d; *p; ++p) if (*p != '-') k += *p;
        if (u.find(k) != std::string::npos) return d;
    }
    return {};
}

std::string package_word(const std::string& txt) {
    std::string u = norm(txt);
    static const RX r1(R"((?:^| )(\d{1,3}[- ]?(?:X2QFN|WQFN|VQFN|QFN|WSON|VSON|UDFN|DFN|SON|HTSSOP|TSSOP|VSSOP|SSOP|QSOP|MSOP|SOIC|SOP|TSOP|LQFP|TQFP|QFP|PLCC|BGA|LGA|CSP)|(?:X2QFN|WQFN|VQFN|QFN|WSON|VSON|UDFN|DFN|SON|HTSSOP|TSSOP|VSSOP|SSOP|QSOP|MSOP|SOIC|SOP|TSOP|LQFP|TQFP|QFP|PLCC|BGA|LGA|CSP)[- ]?\d{1,3})(?: |$))");
    static const RX r2(R"((?:^| )(SOT[- ]?\d{2,3}(?:[- ]\d)?|SOD[- ]?\d{2,3}|SC[- ]?\d{2}|D?O?[- ]?21\d[A-Z]{0,2}|LFPAK\d*|DPAK|D2PAK)(?: |$))");
    std::smatch m;
    auto fix = [](std::string w) { w = trim(w); std::string o; bool sp = false; for (char c : w) { if (c == ' ') { if (!sp) o += '-'; sp = true; } else { o += c; sp = false; } } return o; };
    if (std::regex_search(u, m, r1)) return fix(m[1].str());
    if (std::regex_search(u, m, r2)) return fix(m[1].str());
    return {};
}

int class_from_text(const std::string& txt) {
    std::string u = " " + norm(txt) + " ";
    static const RX a(R"( (CONN|CONNECTOR|HEADER|HDR|RECEPTACLE|SOCKET|RJ45|USB|HDMI|SIM) )"), b(R"( (LED|LEDS) )"),
        c("(ELECTROLYTIC|CAP ELEC|ALUM|RADIAL CAN)"), d("(FBGA|WLCSP|CSP\\d|LGA)"), e("(BGA)"), f("(LQFP|TQFP|BQFP|PQFP|QFP)"),
        g("(PLCC|QFJ)"), h("(QFN|DFN|SON|MLF|MLP|VQFN|WQFN|X2QFN|WSON|VSON|UDFN|TDSON|WHSON|HSON)"), i("(TSOP)"),
        j("(HTSSOP|TSSOP|VSSOP|SSOP|QSOP|MSOP|SOIC|SOP|SOL|SO\\d)"), k("(SOJ)"), l("(MELF|LL34|LL41|SOD80)"),
        m("(SOT|SOD|SC7\\d|SC8\\d|SC9\\d|DPAK|D2PAK|TO2\\d\\d|DO2\\d\\d|SMA|SMB|SMC|LFPAK|POWERDI|TRANS|MOSFET|DIODE|ZENER|SCHOTTKY|TVS|OPTO)");
    if (rs(a, u)) return ClsConn2;
    if (rs(b, u)) return ClsLed;
    if (rs(c, u)) return ClsECap;
    if (rs(d, u)) return ClsFbga;
    if (rs(e, u)) return ClsBga;
    if (rs(f, u)) return ClsQfp;
    if (rs(g, u)) return ClsPlcc;
    if (rs(h, u)) return ClsQfn;
    if (rs(i, u)) return ClsTsop;
    if (rs(j, u)) return ClsSop;
    if (rs(k, u)) return ClsSoj;
    if (rs(l, u)) return ClsMelf;
    if (rs(m, u)) return ClsSot;
    return ClsChip;
}

double guess_height(double L, double W, int c) {
    double mn = std::min(L, W);
    if (c == 120) return 0.90;
    if (c == 11) return std::min(1.20, 0.45 * mn + 0.35);
    if (c == 101 || c == 104) return 1.20;
    if (c == 102 || c == 103) return 1.40;
    if (c == 107 || c == 110) return std::max(1.20, 0.20 * mn);
    if (c == 6) return mn;
    return std::max(0.40, 0.30 * mn);
}

const RX* fam_namepat(const char* fam) {
    if (!fam) return nullptr;
    static const RX ind("IND|COIL|SLF|CDRH|LCI|DT\\d|CHOKE|BEAD", ICASE), bead("BEAD|IND|COIL", ICASE),
        cap("^CAP|AL ?CAP|ELEC", ICASE), res("^RES", ICASE), tant("^TANT", ICASE), led("LED", ICASE),
        xtal("XTAL|OSC|CRYSTAL|RESONATOR|CSTC|MC-?\\d", ICASE);
    std::string f = fam;
    if (f == "IND") return &ind;
    if (f == "BEAD") return &bead;
    if (f == "CAP") return &cap;
    if (f == "RES") return &res;
    if (f == "TANT") return &tant;
    if (f == "LED") return &led;
    if (f == "XTAL") return &xtal;
    return nullptr;
}
const char* class_fam(int cls) { return cls == 6 ? "CAP" : cls == 14 ? "LED" : cls == 3 ? "TANT" : nullptr; }

std::pair<int, int> top_of(const std::map<int, int>& c) {   // (key, count); total via sum
    int bk = 0, bn = 0;
    bool have = false;
    for (auto& kv : c) {
        if (kv.second > bn || (kv.second == bn && have && kv.first < bk)) { bn = kv.second; bk = kv.first; have = true; }
    }
    return {bk, bn};
}
int total_of(const std::map<int, int>& c) { int t = 0; for (auto& kv : c) t += kv.second; return t; }
bool solid(const std::map<int, int>& c) {
    auto t = top_of(c);
    int tot = total_of(c);
    return t.first && t.second >= 3 && tot && (double)t.second / tot >= 0.5;
}

} // namespace

// ---------------------------------------------------------------------------
// public text helpers
// ---------------------------------------------------------------------------
std::vector<std::string> text_chip_sizes(const std::string& s) {
    std::string u = upper(s);
    std::vector<std::string> out;
    static const RX r1(R"((^|[^0-9A-Z.])(01005|0201|0402|0603|0805|1206|1210|1806|1808|1812|1825|2010|2220|2225|2512|0306|0508|0612|0504)(?![0-9A-Z]))");
    static const RX r2(R"((^|[^0-9A-Z.])(1005|1608|2012|3216|3225|4516|4520|4532|4564|5025|5650|5664|6332)(?![0-9A-Z]))");
    static const RX r3(R"((^|[^0-9A-Z.])(805|603|402|206|210)(?![0-9A-Z]))");
    for (auto it = std::sregex_iterator(u.begin(), u.end(), r1); it != std::sregex_iterator(); ++it) out.push_back((*it)[2]);
    for (auto it = std::sregex_iterator(u.begin(), u.end(), r2); it != std::sregex_iterator(); ++it) out.push_back(met2imp((*it)[2]));
    for (auto it = std::sregex_iterator(u.begin(), u.end(), r3); it != std::sregex_iterator(); ++it) {
        std::string c = (*it)[2];
        out.push_back(c == "805" ? "0805" : c == "603" ? "0603" : c == "402" ? "0402" : c == "206" ? "1206" : "1210");
    }
    return out;
}

bool chip_dims(const std::string& code, double& l, double& w) {
    for (auto& c : CHIP) if (code == c.code) { l = c.l; w = c.w; return true; }
    return false;
}

std::string family_of(const std::string& s) {
    std::string u = " " + norm(s) + " ";
    static const RX tant(" (TANT|TANTALUM) "), alcap(" (ELECT|ELECTROLYTIC|ALUM|ALUMINUM) "), bead(" (FERRITE|BEAD) "),
        ind(" (IND|INDUCTOR|CHOKE|COIL) "), indv("\\d ?[NUM]H "), led(" (LED|LEDS) "), cap(" (CAP|CAPACITOR|CAPACITORS|MLCC) "),
        capv("\\d ?[NUMP]F "), res(" (RES|RESISTOR|RESISTORS) "), ohm(" OHMS? "), fuse(" (FUSE|FUSES) "),
        xtal(" (XTAL|CRYSTAL|RESONATOR|OSC|OSCILLATOR|MEMS) ");
    if (rs(tant, u)) return "TANT";
    if (rs(alcap, u)) return "ALCAP";
    if (rs(bead, u)) return "BEAD";
    if (rs(ind, u) || rs(indv, u)) return "IND";
    if (rs(led, u)) return "LED";
    if (rs(cap, u) || rs(capv, u)) return "CAP";
    if (rs(res, u) || rs(ohm, u)) return "RES";
    if (rs(fuse, u)) return "FUSE";
    if (rs(xtal, u)) return "XTAL";
    return {};
}

std::string tape_type_name(int t) {
    switch (t) {
    case 1: return "8mm paper"; case 2: return "8mm emboss"; case 3: return "12mm emboss"; case 4: return "16mm emboss";
    case 5: return "24mm emboss"; case 6: return "32mm paper"; case 7: return "32mm emboss"; case 8: return "44mm emboss";
    case 9: return "56mm emboss"; default: return t ? "type " + std::to_string(t) : "";
    }
}

std::string nozzle_for_short_edge(double b) {
    for (auto& s : NOZZLE_LADDER) if (b <= s.max) return s.nz;
    return "508";
}

FeederFit predict_feeder(double x, double y) {
    FeederFit f;
    double a = std::max(x, y), b = std::min(x, y);
    if (a <= 0 || b <= 0) return f;
    auto step = [](double v, const int* opts, int n) { for (int i = 0; i < n; ++i) if (v <= opts[i]) return opts[i]; return opts[n - 1]; };
    f.tape_mm = step(1.4 * b + 4, TAPE_WIDTHS, 7);
    f.pitch_mm = step(a + 0.6, TAPE_PITCHES, 9);
    f.nozzle = nozzle_for_short_edge(b);
    f.long_edge = a; f.short_edge = b;
    return f;
}

// ---------------------------------------------------------------------------
// Library
// ---------------------------------------------------------------------------
static int iget(const jet::ETable& t, size_t r, const char* c) { return (int)t.get_i(r, t.col(c)); }

bool Library::open(const std::string& p, std::string& err) {
    *this = Library{};
    jet::Editor ed;
    if (!ed.open(p, err)) return false;
    path = p;
    std::vector<std::string> want{"Compo", "PkgClass", "Side", "GripNozzle"};
    for (int i = 0; i < kCopyTableCount; ++i) want.push_back(kCopyTables[i]);
    for (auto& w : want) {
        if (!ed.has_table(w)) continue;
        jet::ETable t;
        if (!ed.load(w, t, err)) return false;
        std::string key = upper(w);
        int cc = t.col("CompoID");
        if (cc >= 0)
            for (size_t r = 0; r < t.rows.size(); ++r) by_compo[key].insert({(int)t.get_i(r, cc), r});
        tables[key] = std::move(t);
    }
    if (!tables.count("COMPO")) { err = "not a FlexLine database (no Compo table)"; return false; }
    if (tables.count("PKGCLASS")) {
        auto& pc = tables["PKGCLASS"];
        for (size_t r = 0; r < pc.rows.size(); ++r) class_names[iget(pc, r, "PkgClassID")] = pc.get_s(r, pc.col("EngClass"));
    }
    auto& co = tables["COMPO"];
    for (size_t r = 0; r < co.rows.size(); ++r) {
        Profile pf;
        pf.id = iget(co, r, "CompoID");
        pf.name = co.get_s(r, co.col("CompoName"));
        pf.cls = iget(co, r, "PkgClassID");
        pf.w = co.get_i(r, co.col("Width")) / 10000.0;
        pf.l = co.get_i(r, co.col("Length")) / 10000.0;
        pf.h = co.get_i(r, co.col("Height")) / 10000.0;
        profiles.push_back(pf);
    }
    std::sort(profiles.begin(), profiles.end(), [](const Profile& a, const Profile& b) { return a.name < b.name; });
    for (size_t i = 0; i < profiles.size(); ++i) index_[profiles[i].id] = i;
    auto first_row = [&](const char* tn, int id) -> std::pair<const jet::ETable*, size_t> {
        auto bt = by_compo.find(upper(tn));
        if (bt == by_compo.end()) return {nullptr, 0};
        auto it = bt->second.find(id);
        if (it == bt->second.end()) return {nullptr, 0};
        return {&tables[upper(tn)], it->second};
    };
    std::map<int, int> side_count;
    if (tables.count("SIDE")) {
        auto& sd = tables["SIDE"];
        for (size_t r = 0; r < sd.rows.size(); ++r) side_count[iget(sd, r, "SideID")] = iget(sd, r, "Count");
    }
    for (auto& pf : profiles) {
        if (auto f = first_row("Feeder", pf.id); f.first) {
            pf.tape_type = iget(*f.first, f.second, "TapeTypeID");
            pf.feeder_type = iget(*f.first, f.second, "FdrTypeID");
            pf.pitch = iget(*f.first, f.second, "Pitch");
        }
        if (auto f = first_row("Laser", pf.id); f.first) pf.nozzle = iget(*f.first, f.second, "GX4NozzleNo");
        if (auto f = first_row("Expansion", pf.id); f.first) pf.auto_teach = iget(*f.first, f.second, "AutoTeach");
        if (auto f = first_row("Visiondata", pf.id); f.first) {
            const auto& t = *f.first;
            pf.lead_pitch = t.get_i(f.second, t.col("LeadPitch")) / 10000.0;
            pf.lead_len = t.get_i(f.second, t.col("LeadLength")) / 10000.0;
            pf.lead_w = t.get_i(f.second, t.col("LeadWidth")) / 10000.0;
            const char* ids[4] = {"BottomID", "RightID", "TopID", "LeftID"};
            for (int k = 0; k < 4; ++k) {
                auto it = side_count.find(iget(t, f.second, ids[k]));
                pf.side_count[k] = it == side_count.end() ? 0 : it->second;
            }
        }
        // paper / emboss from the name (FlexAssign buildSiblings), else from the tape type
        auto ends = [&](const char* suf) { size_t n = std::strlen(suf); return pf.name.size() >= n && pf.name.compare(pf.name.size() - n, n, suf) == 0; };
        if (ends(" Pp") || ends("Paper")) pf.tape_kind = 'P';
        else if (ends(" Em") || ends("Emboss")) pf.tape_kind = 'E';
        else if (pf.tape_type == 1 || pf.tape_type == 6) pf.tape_kind = 'P';
        else if (pf.tape_type >= 2) pf.tape_kind = 'E';
    }
    std::map<std::string, int> by_name;
    for (auto& pf : profiles) by_name[pf.name] = pf.id;
    for (auto& pf : profiles) {
        std::string alt;
        auto rep = [&](const std::string& suf, const std::string& to) {
            if (pf.name.size() >= suf.size() && pf.name.compare(pf.name.size() - suf.size(), suf.size(), suf) == 0)
                alt = pf.name.substr(0, pf.name.size() - suf.size()) + to;
        };
        rep(" Pp", " Em");
        if (alt.empty()) rep(" Em", " Pp");
        if (alt.empty()) rep("Paper", "Emboss");
        if (alt.empty()) rep("Emboss", "Paper");
        auto it = by_name.find(alt);
        if (!alt.empty() && it != by_name.end()) pf.sibling = it->second;
    }
    return true;
}

const Profile* Library::find(int id) const {
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : &profiles[it->second];
}
const Profile* Library::find_name(const std::string& n) const {
    for (auto& p : profiles) if (ieq(p.name, n)) return &p;
    return nullptr;
}
std::string Library::class_name(int cls) const {
    auto it = class_names.find(cls);
    return it != class_names.end() ? it->second : (cls ? "class " + std::to_string(cls) : std::string());
}

// ---------------------------------------------------------------------------
// ComponentDB
// ---------------------------------------------------------------------------
bool ComponentDB::open(const std::string& p, std::string& err) {
    t_.clear(); touched_.clear(); lookups_done_ = false; open_ = false; defaults_.clear(); defaults_loaded_ = false;
    if (!ed_.open(p, err)) return false;
    if (!ed_.has_table("Compo")) { err = "not a FlexLine component database (no Compo table)"; return false; }
    path_ = p;
    open_ = true;
    return true;
}

bool ComponentDB::create_from_template(const std::string& tpl, std::string& err) {
    if (!open(tpl, err)) return false;
    path_.clear();
    jet::ETable* c = table("Compo", &err);
    if (!c) return false;
    if (!c->rows.empty()) { err = "template is not empty (" + std::to_string(c->rows.size()) + " components)"; open_ = false; return false; }
    touched_.insert("COMPO");   // a new database is unsaved
    return true;
}

bool ComponentDB::create_empty_from(const std::string& src, std::string& err) {
    if (!open(src, err)) return false;
    path_.clear();
    static const char* keep_all[] = {"FEEDERTYPE", "FLEXDBINFO", "LOOKUP", "PKGCLASS", "VACUUMDFLT", "WRITELOCKINFO"};
    for (const std::string& name : ed_.tables()) {
        const std::string k = upper(name);
        bool keep = false;
        for (const char* x : keep_all) keep |= k == x;
        if (keep) continue;
        jet::ETable* t = table(name, &err);
        if (!t) return false;
        std::vector<jet::ERow> kept;
        if (k == "SIDE") {                       // default side row
            int c = t->col("SideID");
            for (size_t r = 0; r < t->rows.size(); ++r) if (t->get_i(r, c) == 1) kept.push_back(t->rows[r]);
        } else if (k == "COPLAGX4") {            // default coplanarity row
            int c = t->col("IsDefault"), i = t->col("CoplaID");
            for (size_t r = 0; r < t->rows.size(); ++r) if (t->get_i(r, c) != 0 || (kept.empty() && t->get_i(r, i) == 1)) { kept.push_back(t->rows[r]); break; }
        }
        if (kept.size() == t->rows.size()) continue;
        t->rows = std::move(kept);
        t->reset_autonumber = true;   // a new database numbers its components from 1
        touched_.insert(k);
    }
    jet::ETable* c = table("Compo", &err);
    if (!c) { open_ = false; return false; }
    touched_.insert("COMPO");   // a new database is unsaved
    return true;
}

jet::ETable* ComponentDB::table(const std::string& name, std::string* err) {
    std::string k = upper(name);
    auto it = t_.find(k);
    if (it != t_.end()) return &it->second;
    if (!ed_.has_table(name)) { if (err) *err = "no table " + name; return nullptr; }
    jet::ETable t;
    std::string e;
    if (!ed_.load(name, t, e)) { if (err) *err = e; return nullptr; }
    return &(t_[k] = std::move(t));
}

bool ComponentDB::save(const std::string& p, std::string& err) {
    if (!open_) { err = "no database open"; return false; }
    for (auto& k : touched_) {
        auto it = t_.find(k);
        if (it == t_.end()) continue;
        if (!ed_.store(it->second, err)) return false;
    }
    ed_.claim_owned_pages();   // page 1 must not list any page a table owns as free (older exports did)
    if (!ed_.save(p, err)) return false;
    touched_.clear();
    path_ = p;
    return true;
}

std::vector<Part> ComponentDB::parts() {
    std::vector<Part> out;
    jet::ETable* c = table("Compo");
    if (!c) return out;
    std::map<int, int> tape, at;
    if (jet::ETable* f = table("Feeder")) for (size_t r = 0; r < f->rows.size(); ++r) tape[iget(*f, r, "CompoID")] = iget(*f, r, "TapeTypeID");
    if (jet::ETable* x = table("Expansion")) for (size_t r = 0; r < x->rows.size(); ++r) at[iget(*x, r, "CompoID")] = iget(*x, r, "AutoTeach");
    int cid = c->col("CompoID"), cn = c->col("CompoName"), cd = c->col("Description"), ce = c->col("ExtCompoName"),
        cp = c->col("PkgTypeID"), cpt = c->col("PkgType"), cc = c->col("PkgClassID"), cw = c->col("Width"),
        cl = c->col("Length"), ch = c->col("Height"), cok = c->col("DataOk");
    for (size_t r = 0; r < c->rows.size(); ++r) {
        Part p;
        p.id = (int)c->get_i(r, cid);
        p.name = c->get_s(r, cn);
        p.desc = c->get_s(r, cd);
        p.ext_name = c->get_s(r, ce);
        p.pkg_id = (int)c->get_i(r, cp);
        p.pkg_name = c->get_s(r, cpt);
        p.cls = (int)c->get_i(r, cc);
        p.w = c->get_i(r, cw) / 10000.0; p.l = c->get_i(r, cl) / 10000.0; p.h = c->get_i(r, ch) / 10000.0;
        p.data_ok = (int)c->get_i(r, cok);
        auto t = tape.find(p.id); if (t != tape.end()) p.tape_type = t->second;
        auto a = at.find(p.id); if (a != at.end()) p.auto_teach = a->second;
        out.push_back(std::move(p));
    }
    return out;
}

int ComponentDB::find(const std::string& name) {
    jet::ETable* c = table("Compo");
    if (!c) return 0;
    int cn = c->col("CompoName"), cid = c->col("CompoID");
    std::string u = upper(trim(name));
    for (size_t r = 0; r < c->rows.size(); ++r)
        if (upper(trim(c->get_s(r, cn))) == u) return (int)c->get_i(r, cid);
    return 0;
}

static bool set_named(jet::ETable& t, size_t r, const char* col, const jet::Value& v, std::string& err) {
    int c = t.col(col);
    if (c < 0) return true;   // column absent in this schema version: ignore
    return t.set(r, c, v, &err);
}
static jet::Value V(int64_t i) { jet::Value v; v.null = false; v.i = i; v.d = (double)i; return v; }
static jet::Value VD(double d) { jet::Value v; v.null = false; v.d = d; v.i = (int64_t)d; return v; }
static jet::Value VS(const std::string& s) { jet::Value v; v.null = false; v.s = s; return v; }
static jet::Value VN() { return jet::Value{}; }

int ComponentDB::add_part(const std::string& name, const std::string& desc, const std::string& ext, std::string& err) {
    jet::ETable* c = table("Compo", &err);
    if (!c) return 0;
    if (name.empty() || name.size() > (size_t)kMachineNameMax) { err = "machine name must be 1.." + std::to_string(kMachineNameMax) + " characters"; return 0; }
    std::vector<uint8_t> k;
    if (!jet::jet4_text_key(name, k)) { err = "unsupported character in component name '" + name + "'"; return 0; }
    if (find(name)) { err = "component '" + name + "' already exists"; return 0; }
    int maxid = c->reset_autonumber ? 0 : (int)ed_.autonumber("Compo");
    int cid = c->col("CompoID");
    for (size_t r = 0; r < c->rows.size(); ++r) maxid = std::max(maxid, (int)c->get_i(r, cid));
    int id = maxid + 1;
    size_t r = c->add_row();
    apply_defaults(*c, r);
    std::string d = desc.size() > 255 ? desc.substr(0, 255) : desc;
    bool ok = set_named(*c, r, "CompoID", V(id), err) && set_named(*c, r, "DataOk", V(0), err) &&
              set_named(*c, r, "CompoName", VS(name), err) && set_named(*c, r, "Description", d.empty() ? VN() : VS(d), err) &&
              set_named(*c, r, "ConHeight", V(0), err) && set_named(*c, r, "UseMech", V(0), err) &&
              set_named(*c, r, "UseVision", V(0), err) && set_named(*c, r, "UseLaser", V(0), err) &&
              set_named(*c, r, "AdheTest", V(0), err) && set_named(*c, r, "GX4PriorityLV", V(0), err) &&
              set_named(*c, r, "CompOrient", V(0), err) && set_named(*c, r, "MoldLineOffsetX", V(0), err) &&
              set_named(*c, r, "MoldLineOffsetY", V(0), err) && set_named(*c, r, "SubNozzle", V(0), err) &&
              set_named(*c, r, "ExtCompoName", ext.empty() ? VN() : VS(ext.substr(0, 60)), err);
    if (!ok) { c->rows.pop_back(); return 0; }
    touched_.insert("COMPO");
    // FlexLine creates an Inspection, Expansion and Fluxer row with defaults for every imported name.
    struct KV { const char* k; int64_t v; bool null; };
    static const KV insp[] = {{"DataOk", 1, 0}, {"ChipStandCheck", 0, 0}, {"Vacuum5", 0, 0}, {"HChkX", 0, 0}, {"HChkY", 0, 0},
        {"HChkZ", 0, 0}, {"CoplaChk", 0, 0}, {"CoplaTolerance", 2000, 0}, {"CoplaOffset", 0, 0}, {"VerifyChk", 0, 0},
        {"VerifyType", 0, 0}, {"VerifyUnit", 0, 0}, {"Standard", 0, 0}, {"Max", 0, 0}, {"Min", 0, 0}, {"DimChkChk", 0, 0},
        {"DimChkWid", 0, 0}, {"DimChkLen", 0, 0}, {"MaxRangeX", 30, 0}, {"MinRangeX", 30, 0}, {"MaxRangeY", 30, 0},
        {"MinRangeY", 30, 0}, {"RatioMin", 0, 0}, {"RatioMax", 0, 0}, {"SOT_Dir_Insp", 0, 0}, {"Polarity_Dir_Insp", 0, 0},
        {"PitchChk", 0, 0}, {"PitchErrLvl", 0, 0}, {"PwbHeightMeasure", 0, 0}, {"PH_Point", 1, 0}, {"CmpHeightCheck", 0, 0}};
    static const KV expn[] = {{"DataOk", 1, 0}, {"Retry", 1, 0}, {"Test", 0, 0}, {"Protect", 0, 0}, {"OverPlace", 5000, 0},
        {"OverPick", 0, 0}, {"CompoCheck", 0, 0}, {"PickPosAdjust", 0, 0}, {"ScrapType", 0, 0}, {"CompoLayer", 4, 0},
        {"AutoTeach", 0, 0}, {"GripNozID", 1, 0}, {"PickOffsetZ", 0, 0}, {"OverPick2K", 0, 0}, {"OverPlace2K", 5000, 0},
        {"PickOffsetX", 0, 0}, {"PickOffsetY", 0, 0}, {"OverPickGx5", 0, 0}, {"OverPlaceGx5", 5000, 0},
        {"PickVacuumTiming", 0, 0}, {"OverPickFx1", 0, 0}, {"OverPlaceFx1", 0, 0}, {"TableAccel", 0, 0},
        {"TableAccelFx1R", 0, 0}, {"OverPickCX", 0, 0}, {"OverPlaceCX", 0, 0}, {"FCType", 0, 0}, {"FCOverPick", 0, 0},
        {"FCOverPlace", 0, 0}, {"PlaceVacOffset", 0, 0}, {"OCCPickPosAdjust", 0, 0}, {"AdheTest", 0, 0}, {"WeakBlow", 0, 0},
        {"BlowStartCX", 0, 0}, {"BlowTimeCX", 0, 0}, {"PumpVacuumTiming", 0, 0}, {"FCTypeGx6", 0, 0}, {"OverPickGx6", 0, 0},
        {"OverPlaceGx6", 0, 0}, {"FCOverPickGx6", 0, 0}, {"FCOverPlaceGx6", 0, 0}, {"ScrapTypeGx6", 0, 0},
        {"OverPickFx3", 0, 0}, {"OverPlaceFx3", 5000, 0}, {"PickVacuumTimingFx3", 0, 0}, {"PickVacOnFx3", 0, 0},
        {"PickVacCorrFx3", 0, 0}, {"PickHoldFx3", 0, 0}, {"PickVacuumTimingFx3_2", 0, 0}, {"PickVacOnFx3_2", 0, 0},
        {"PickVacCorrFx3_2", 0, 0}, {"PickHoldFx3_2", 0, 0}, {"PlaceVacuumTimingFx3", 0, 0}, {"PlaceVacOffFx3", 0, 0},
        {"PlaceVacCorrFx3", 0, 0}, {"PlaceBlowOnFx3", 0, 0}, {"PlaceBlowTimeFx3", 0, 0}, {"PlaceHoldFx3", 0, 0},
        {"PlaceVacuumTimingFx3_2", 0, 0}, {"PlaceVacOffFx3_2", 0, 0}, {"PlaceVacCorrFx3_2", 0, 0},
        {"PlaceBlowOnFx3_2", 0, 0}, {"PlaceBlowTimeFx3_2", 0, 0}, {"PlaceHoldFx3_2", 0, 0}, {"BeaChip", 0, 0},
        {"DirectPlace", 0, 0}, {"OverPickFx2", 0, 0}, {"OverPlaceFx2", 2000, 0}, {"TableAccelFx2", 50, 0}};
    static const KV flux[] = {{"FluxerUse", 0, 0}, {"FluxerMode", 0, 0}, {"FluxerLoad", 0, 0}, {"FluxerStroke", 0, 0},
        {"FluxerBottomDeadCenter", 0, 0}, {"FluxerSpeedUpZ", 0, 0}, {"FluxerSpeedDownZ", 0, 0}, {"FluxerTransPos", 0, 0}};
    auto add_child = [&](const char* tn, const KV* kv, size_t n) -> bool {
        jet::ETable* t = table(tn);
        if (!t) return true;
        size_t rr = t->add_row();
        apply_defaults(*t, rr);
        if (!set_named(*t, rr, "CompoID", V(id), err)) return false;
        for (size_t i = 0; i < n; ++i)
            if (!set_named(*t, rr, kv[i].k, kv[i].null ? VN() : (t->cols[(size_t)std::max(0, t->col(kv[i].k))].type == jet::ColType::Double ? VD((double)kv[i].v) : V(kv[i].v)), err)) return false;
        touched_.insert(upper(tn));
        return true;
    };
    if (!add_child("Inspection", insp, sizeof insp / sizeof *insp) || !add_child("Expansion", expn, sizeof expn / sizeof *expn) ||
        !add_child("Fluxer", flux, sizeof flux / sizeof *flux))
        return 0;
    return id;
}

bool ComponentDB::set_description(int id, const std::string& desc, std::string& err) {
    jet::ETable* c = table("Compo", &err);
    if (!c) return false;
    int cid = c->col("CompoID");
    for (size_t r = 0; r < c->rows.size(); ++r)
        if ((int)c->get_i(r, cid) == id) {
            touched_.insert("COMPO");
            return set_named(*c, r, "Description", VS(desc.substr(0, 255)), err);
        }
    err = "no component " + std::to_string(id);
    return false;
}

bool ComponentDB::ensure_lookups(const Library& lib, std::string& err) {
    if (lookups_done_) return true;
    // FlexAssign ensureLookups: copy every Side / GripNozzle row of the library the target lacks.
    for (const char* tn : {"Side", "GripNozzle"}) {
        auto st = lib.tables.find(upper(tn));
        jet::ETable* t = table(tn);
        if (st == lib.tables.end() || !t) continue;
        const jet::ETable& s = st->second;
        const std::string key = s.cols.empty() ? std::string() : s.cols[0].name;   // SideID / GripNozID
        int sk = s.col(key), tk = t->col(key);
        if (sk < 0 || tk < 0) continue;
        std::set<int64_t> have;
        for (size_t r = 0; r < t->rows.size(); ++r) have.insert(t->get_i(r, tk));
        bool added = false;
        for (size_t r = 0; r < s.rows.size(); ++r) {
            if (have.count(s.get_i(r, sk))) continue;
            size_t nr = t->add_row();
            apply_defaults(*t, nr);
            for (size_t c = 0; c < t->cols.size(); ++c) {
                int sc = s.col(t->cols[c].name);
                if (sc < 0) continue;
                if (s.cols[(size_t)sc].type == t->cols[c].type) { t->rows[nr].c[c] = s.rows[r].c[(size_t)sc]; }
                else if (!t->set(nr, (int)c, s.get(r, sc), &err)) return false;
            }
            added = true;
        }
        if (added) touched_.insert(upper(tn));
    }
    lookups_done_ = true;
    return true;
}

bool ComponentDB::apply_profile(int id, const Library& lib, int pid, int cls_override, bool copy_dims, std::string& err) {
    const Profile* pf = lib.find(pid);
    if (!pf) { err = "profile " + std::to_string(pid) + " not in the library"; return false; }
    jet::ETable* c = table("Compo", &err);
    if (!c) return false;
    int cid = c->col("CompoID");
    size_t row = (size_t)-1;
    for (size_t r = 0; r < c->rows.size(); ++r) if ((int)c->get_i(r, cid) == id) row = r;
    if (row == (size_t)-1) { err = "no component " + std::to_string(id); return false; }
    if (!ensure_lookups(lib, err)) return false;
    // Compo stamps
    const jet::ETable& lc = lib.tables.at("COMPO");
    auto lit = lib.by_compo.at("COMPO").find(pid);
    if (lit == lib.by_compo.at("COMPO").end()) { err = "profile row missing"; return false; }
    size_t lr = lit->second;
    std::vector<std::string> pull(std::begin(kCompoScalars), std::end(kCompoScalars));
    if (copy_dims) { pull.push_back("Width"); pull.push_back("Length"); pull.push_back("Height"); }
    if (!set_named(*c, row, "PkgTypeID", V(pid), err) || !set_named(*c, row, "PkgType", VS(pf->name.substr(0, 50)), err)) return false;
    for (auto& col : pull) {
        int tc = c->col(col), sc = lc.col(col);
        if (tc < 0 || sc < 0) continue;
        jet::Value v = lc.get(lr, sc);
        if (col == "PkgClassID" && cls_override > 0) v = V(cls_override);
        if (!c->set(row, tc, v, &err)) return false;
    }
    if (!set_named(*c, row, "DataOk", V(1), err)) return false;
    touched_.insert("COMPO");
    // parameter tables: wipe the component's rows, then copy the profile's rows
    for (int i = 0; i < kCopyTableCount; ++i) {
        std::string tn = kCopyTables[i], key = upper(tn);
        auto st = lib.tables.find(key);
        if (st == lib.tables.end() || !ed_.has_table(tn)) continue;
        jet::ETable* t = table(tn, &err);
        if (!t) return false;
        int tk = t->col("CompoID");
        if (tk < 0 || st->second.col("CompoID") < 0) continue;
        auto& rows = t->rows;
        rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const jet::ERow& rr) {
                       const jet::Cell& cc = rr.c[(size_t)tk];
                       return cc.present && cc.b.size() == 4 && (int)(cc.b[0] | cc.b[1] << 8 | cc.b[2] << 16 | cc.b[3] << 24) == id;
                   }), rows.end());
        const jet::ETable& s = st->second;
        auto range = lib.by_compo.at(key).equal_range(pid);
        for (auto it = range.first; it != range.second; ++it) {
            size_t nr = t->add_row();
            apply_defaults(*t, nr);
            for (size_t col = 0; col < t->cols.size(); ++col) {
                int sc = s.col(t->cols[col].name);
                if (sc < 0) continue;
                if (s.cols[(size_t)sc].type == t->cols[col].type && s.cols[(size_t)sc].size == t->cols[col].size)
                    t->rows[nr].c[col] = s.rows[it->second].c[(size_t)sc];
                else if (!t->set(nr, (int)col, s.get(it->second, sc), &err)) return false;
            }
            t->rows[nr].dirty = true;
            if (!t->set_i(nr, tk, id)) return false;
        }
        touched_.insert(key);
    }
    return true;
}

bool ComponentDB::set_tape(int id, bool paper, std::string& err) {
    jet::ETable* f = table("Feeder", &err);
    if (!f) return false;
    int fk = f->col("CompoID"), ft = f->col("TapeTypeID");
    bool found = false;
    for (size_t r = 0; r < f->rows.size(); ++r) {
        if ((int)f->get_i(r, fk) != id) continue;
        found = true;
        int t = (int)f->get_i(r, ft), nt = t;
        if (t == 1 || t == 2) nt = paper ? 1 : 2;
        else if (t == 6 || t == 7) nt = paper ? 6 : 7;
        else if (paper) { err = tape_type_name(t) + ": no paper carrier at this tape width"; return false; }
        if (nt != t) { f->set_i(r, ft, nt); touched_.insert("FEEDER"); }
    }
    if (!found) { err = "component has no feeder data (apply a profile first)"; return false; }
    if (jet::ETable* x = table("Expansion")) {
        int xk = x->col("CompoID"), xa = x->col("AutoTeach");
        for (size_t r = 0; r < x->rows.size(); ++r)
            if ((int)x->get_i(r, xk) == id && xa >= 0) { x->set_i(r, xa, paper ? 1 : 0); touched_.insert("EXPANSION"); }
    }
    return true;
}

// ---------------------------------------------------------------------------
// column defaults (Jet applies DefaultValue on INSERT for omitted columns; so do we)
// ---------------------------------------------------------------------------
std::map<std::string, std::string> parse_lvprop_defaults(const std::vector<uint8_t>& b) {
    std::map<std::string, std::string> out;
    auto rd16 = [&](size_t o) { return (uint32_t)(b[o] | b[o + 1] << 8); };
    auto rd32 = [&](size_t o) { return (uint32_t)(b[o] | b[o + 1] << 8 | b[o + 2] << 16 | (uint32_t)b[o + 3] << 24); };
    auto ucs = [&](size_t o, size_t n) { std::string s; for (size_t i = 0; i + 1 < n; i += 2) utf8_append(s, rd16(o + i)); return s; };
    if (b.size() < 4 || std::memcmp(b.data(), "MR2", 3) != 0) return out;
    std::vector<std::string> names;
    size_t pos = 4;
    while (pos + 6 <= b.size()) {
        uint32_t len = rd32(pos), type = rd16(pos + 4);
        if (len < 6 || pos + len > b.size()) break;
        size_t end = pos + len, p = pos + 6;
        if (type == 0x80) {
            while (p + 2 <= end) { uint32_t n = rd16(p); if (p + 2 + n > end) break; names.push_back(ucs(p + 2, n)); p += 2 + n; }
        } else if (type == 0x00 || type == 0x01) {
            if (p + 6 > end) break;
            uint32_t sub = rd32(p);
            uint32_t nl = rd16(p + 4);
            std::string col = p + 6 + nl <= end ? ucs(p + 6, nl) : std::string();
            p += std::max<uint32_t>(sub, 6 + nl);
            while (p + 8 <= end) {
                uint32_t el = rd16(p), dt = b[p + 3], ni = rd16(p + 4), dl = rd16(p + 6);
                if (el < 8 || p + el > end) break;
                if (ni < names.size() && names[ni] == "DefaultValue" && !col.empty() && p + 8 + dl <= end) {
                    std::string v = (dt == 10 || dt == 12) ? ucs(p + 8, dl) : std::string();
                    if (!v.empty()) out[upper(col)] = v;
                }
                p += el;
            }
        }
        pos = end;
    }
    return out;
}

void ComponentDB::load_defaults() {
    if (defaults_loaded_) return;
    defaults_loaded_ = true;
    jet::Database db;
    std::string err;
    if (!db.open_memory(ed_.image(), err)) return;
    jet::Table so;
    if (!db.read_table("MSysObjects", so, err)) return;
    int cn = so.col("Name"), cl = so.col("LvProp"), ct = so.col("Type");
    if (cn < 0 || cl < 0) return;
    for (auto& r : so.rows) {
        if (ct >= 0 && (r[(size_t)ct].as_int() & 0x7FFF) != 1) continue;
        auto d = parse_lvprop_defaults(r[(size_t)cl].bin);
        auto& m = defaults_[upper(r[(size_t)cn].s)];
        for (auto& kv : d) {
            std::string v = trim(kv.second);
            jet::Value x;
            std::string uv = upper(v);
            if (uv == "YES" || uv == "TRUE" || uv == "ON") x = V(1);
            else if (uv == "NO" || uv == "FALSE" || uv == "OFF") x = V(0);
            else if (v.size() >= 2 && v.front() == '"' && v.back() == '"') x = VS(v.substr(1, v.size() - 2));
            else {
                char* e = nullptr;
                double dv = std::strtod(v.c_str(), &e);
                if (!e || *e || v.empty()) continue;   // expressions (=Now() ...) are not evaluated
                x = VD(dv);
                x.i = (int64_t)std::llround(dv);
            }
            m[kv.first] = x;
        }
    }
}

void ComponentDB::apply_defaults(jet::ETable& t, size_t row) {
    load_defaults();
    auto it = defaults_.find(upper(t.name));
    if (it == defaults_.end()) return;
    std::string e;
    for (size_t c = 0; c < t.cols.size(); ++c) {
        auto d = it->second.find(upper(t.cols[c].name));
        if (d != it->second.end()) t.set(row, (int)c, d->second, &e);
    }
}

// ---------------------------------------------------------------------------
// machine names
// ---------------------------------------------------------------------------
std::string sanitize_machine_chars(const std::string& s) {
    std::string out;
    for (char ch : s) {
        unsigned char c = (unsigned char)ch;
        std::string one(1, ch);
        std::vector<uint8_t> k;
        if (c >= 0x80 || (c != '-' && c != '\'' && !jet::jet4_text_key(std::string("A") + one, k))) out += '_';
        else out += ch;
    }
    return out;
}

std::string machine_name(const std::string& pn, const std::function<bool(const std::string&)>& taken) {
    std::string s = sanitize_machine_chars(trim(pn));
    if (s.empty()) s = "PART";
    std::string base = s.substr(0, (size_t)kMachineNameMax);
    while (!base.empty() && base.back() == ' ') base.pop_back();
    if (!taken(base)) return base;
    for (int n = 1; n < 1000; ++n) {
        std::string suf = "~" + std::to_string(n);
        std::string cand = s.substr(0, (size_t)kMachineNameMax - suf.size()) + suf;
        if (!taken(cand)) return cand;
    }
    return base;
}

// ---------------------------------------------------------------------------
// Classifier
// ---------------------------------------------------------------------------
void Classifier::set_library(const Library& lib) { lib_ = &lib; }

std::string Classifier::base_size(const std::string& nm) {
    static const RX r(R"(^(?:(?:Res|Cap|IND)(\d{4,5})|Tant([A-Z])))");
    std::smatch m;
    if (std::regex_search(nm, m, r)) return m[1].matched ? m[1].str() : m[2].str();
    return nm;
}

void Classifier::learn(const std::vector<Row>& rows) {
    chip.clear(); lead.clear(); disc.clear(); used.clear();
    if (!lib_) return;
    static const RX rp(R"(^(?:(Res|Cap|IND)(\d{4,5})\(|Tant([A-Z]):))");
    static const RX rt(R"(:\s*(\d{4}))");
    for (auto& r : rows) {
        const Profile* pf = lib_->find(r.pid);
        if (!pf) continue;
        ++used[r.pid];
        std::smatch m;
        if (std::regex_search(pf->name, m, rp)) {
            if (m[3].matched) {
                std::smatch tm;
                std::string key;
                if (std::regex_search(pf->name, tm, rt)) key = tm[1];
                else { const char* tl = tant_letter(m[3].str()[0]); key = tl ? tl : m[3].str(); }
                ++chip["TANT|" + key][r.pid];
            } else {
                std::string f = m[1].str() == "Res" ? "RES" : m[1].str() == "Cap" ? "CAP" : "IND";
                ++chip[f + "|" + m[2].str()][r.pid];
            }
            continue;
        }
        std::string txt = r.desc + " | " + r.name;
        LeadPkg lp = lead_pkg(txt);
        if (lp.ok && lp.pins) ++lead[lp.base + "|" + std::to_string(lp.pins)][r.pid];
        std::string dp = discrete_pkg(txt);
        if (!dp.empty()) ++disc[dp][r.pid];
    }
}

std::string Classifier::alt_text(const std::map<int, int>& c, int chosen, std::vector<int>* ids) const {
    std::vector<std::pair<int, int>> arr;
    for (auto& kv : c) if (kv.first != chosen) arr.push_back(kv);
    std::stable_sort(arr.begin(), arr.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::string out;
    for (size_t i = 0; i < arr.size() && i < 2; ++i) {
        const Profile* p = lib_ ? lib_->find(arr[i].first) : nullptr;
        if (!out.empty()) out += ", ";
        out += std::to_string(arr[i].second) + "x " + (p ? p->name : std::to_string(arr[i].first));
    }
    if (ids) for (auto& a : arr) ids->push_back(a.first);
    return out;
}

bool Classifier::nearest(double len, double wid, int want_cls, const char* fam, double tol, NB& out) const {
    if (!lib_ || !len || !wid) return false;
    const RX* pat = fam_namepat(fam);
    double a = std::max(len, wid), b = std::min(len, wid);
    int best = 0;
    double be = 1e9;
    const Profile* bp = nullptr;
    for (auto& p : lib_->profiles) {
        if (pat && !rs(*pat, p.name)) continue;
        if (want_cls && p.cls != want_cls) continue;
        double W = std::trunc(p.w * 10000 + 0.5) / 10000, L = std::trunc(p.l * 10000 + 0.5) / 10000;
        if (!W || !L) continue;
        double pa = std::max(W, L), pb = std::min(W, L);
        double dx = (a - pa) / pa, dy = (b - pb) / pb, e = std::sqrt(dx * dx + dy * dy);
        if (e < be) { be = e; best = p.id; bp = &p; }
    }
    if (!best || be > tol) return false;
    out.id = best; out.err = be;
    out.note = fmt2(bp->l) + "x" + fmt2(bp->w) + "mm, " + std::to_string((int)std::lround(be * 100)) + "% off";
    return true;
}

Guess Classifier::by_package(int cls, double L, double W, double H, const std::string& label, const std::string& note) const {
    Guess g;
    NB nb;
    std::string cl = lib_ ? lib_->class_name(cls) : std::to_string(cls);
    bool ok = nearest(L, W, cls, class_fam(cls), 0.08, nb) || nearest(L, W, cls, nullptr, 0.08, nb);
    if (ok) {
        g.pid = nb.id; g.conf = 0.80; g.variant = 1; g.cls_override = cls;
        g.why = label + " " + fmtg(L) + "x" + fmtg(W) + "mm, class " + cl + (note.empty() ? "" : " (" + note + ")") + " -> " + nb.note;
        return g;
    }
    g.gap = true; g.gap_l = L; g.gap_w = W; g.gap_h = H; g.gap_cls = cls; g.gap_label = label;
    g.why = "BUILD: " + label + " " + fmtg(L) + "x" + fmtg(W) + "x" + fmtg(H) + "mm, class " + cl + (note.empty() ? "" : " (" + note + ")");
    return g;
}

int Classifier::profile_named(const std::string& fam, int pins, std::vector<int>& hits) const {
    hits.clear();
    if (!lib_) return 0;
    RX re("(^|[^A-Z0-9])" + fam + "\\s*\\(?" + std::to_string(pins) + "(?![0-9])", ICASE);
    for (auto& p : lib_->profiles) if (rs(re, p.name)) hits.push_back(p.id);
    if (hits.empty()) return 0;
    int best = 0, bu = -1;
    for (int h : hits) {
        auto it = used.find(h);
        int u = it == used.end() ? 0 : it->second;
        if (u > bu || (u == bu && best && h < best)) { bu = u; best = h; }
    }
    return best;
}

Guess Classifier::classify(const std::string& name, const std::string& desc_in, double w, double l) const {
    // "CAP0402", "RES0603K" -> "CAP 0402": size codes glued to the family word
    static const RX glued(R"(([A-Za-z])(0201|0402|0603|0805|1206|1210|1812|2010|2512)(?![0-9]))");
    const std::string desc = std::regex_replace(desc_in, glued, "$1 $2");
    Guess g;
    if (!lib_) { g.why = "no library"; return g; }
    std::string txt = desc + " | " + name;
    std::string fam = family_of(desc);
    if (fam.empty()) fam = family_of(name);
    auto mk = [&](int pid, double cf, const std::string& why, double vr, const std::map<int, int>* c, int cls = 0) {
        Guess r; r.pid = pid; r.conf = cf; r.why = why; r.variant = vr; r.cls_override = cls;
        if (c) r.alt = alt_text(*c, pid, &r.alt_ids);
        return r;
    };

    if (fam == "TANT") {
        std::string U;
        for (char c : upper(txt)) U += ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ') ? c : ' ';
        std::string metric, route;
        std::smatch m;
        static const RX mm(R"((?:^| )(1608|2012|3216|3528|6032|7260|7343)(?: |$))"), mi(R"((?:^| )(0603|0805|1206|1411|2312|2917|2924|3024)(?: |$))"),
            mc(R"(\bTANT\w*\b.*?\b(?:CASE\s*)?([ABCDERSTUVXYJ])\b)"), many(R"((?:^| )(\d{4})(?: |$))");
        if (std::regex_search(U, m, mm)) { metric = m[1]; route = "metric case " + metric + " in text"; }
        if (metric.empty() && std::regex_search(U, m, mi)) { const char* t = tant_imp(m[1]); metric = t ? t : ""; route = "EIA case " + m[1].str() + " = " + metric; }
        if (metric.empty() && std::regex_search(U, m, mc)) { const char* t = tant_letter(m[1].str()[0]); metric = t ? t : ""; route = "case letter " + m[1].str() + " = " + metric; }
        double cf2 = metric.empty() ? 0 : 0.90;
        if (metric.empty() && w && l) {
            double a = std::max(w, l), b = std::min(w, l), be = 1e9;
            std::string bk;
            for (auto& t : TANT_DIM) {
                double dx = a - std::max(t.l, t.w), dy = b - std::min(t.l, t.w), e = std::sqrt(dx * dx + dy * dy);
                if (e < be) { be = e; bk = t.code; }
            }
            if (be < 0.6) { metric = bk; route = "dims -> " + bk; cf2 = 0.80; }
        }
        if (!metric.empty()) {
            auto it = chip.find("TANT|" + metric);
            if (it != chip.end()) {
                auto t = top_of(it->second);
                return mk(t.first, cf2, "TANT " + route, (double)t.second / total_of(it->second), &it->second);
            }
        }
        if (std::regex_search(U, m, many)) {
            std::string code = m[1];
            double L = std::stoi(code.substr(0, 2)) * 2.54, W = std::stoi(code.substr(2, 2)) * 2.54;
            if (L >= 1 && W >= 0.5 && L <= 20 && W <= 20) {
                NB nb;
                if (nearest(L, W, 1, "TANT", 0.18, nb))
                    return mk(nb.id, 0.70, "TANT case " + code + " = " + fmt2(L) + "x" + fmt2(W) + "mm -> " + nb.note, 1, nullptr);
            }
        }
        if (w && l) {
            NB nb;
            if (nearest(w, l, 1, "TANT", 0.18, nb)) return mk(nb.id, 0.70, "TANT by measured dims, nearest chip profile " + nb.note, 1, nullptr);
        }
        if (const PkgE* pk = lookup_package(txt)) return by_package(pk->cls, pk->L, pk->W, pk->H, pk->label, "");
        g.why = "TANT: no case size" + (metric.empty() ? std::string(" found") : " profile for " + metric);
        return g;
    }

    std::string bead_note;
    int force_cls = 0;
    if (fam == "BEAD") { fam = "CAP"; bead_note = "bead as chip; "; }
    if (fam == "LED") {
        std::string lsz;
        auto ts = text_chip_sizes(txt);
        auto ds = dim_sizes(w, l);
        if (!ts.empty()) lsz = ts[0]; else if (!ds.empty()) lsz = ds[0].first;
        if (!lsz.empty()) {
            auto it = chip.find("LED|" + lsz);
            if (it == chip.end()) it = chip.find("CAP|" + lsz);
            if (it != chip.end()) {
                auto t = top_of(it->second);
                return mk(t.first, 0.90, "LED " + lsz + " -> class Chip(LED)", (double)t.second / total_of(it->second), &it->second, 14);
            }
        }
        fam = "CAP"; bead_note = "LED as chip; "; force_cls = 14;
    }

    if (fam == "RES" || fam == "CAP" || fam == "IND") {
        auto dim = dim_sizes(w, l);
        auto tsz = text_chip_sizes(txt);
        std::vector<std::string> uniq;
        for (auto& s : tsz) if (std::find(uniq.begin(), uniq.end(), s) == uniq.end()) uniq.push_back(s);
        auto usedc = [&](const std::string& code) { auto it = chip.find(fam + "|" + code); return it == chip.end() ? 0 : total_of(it->second); };
        std::stable_sort(dim.begin(), dim.end(), [&](auto& x, auto& y) {
            long ex = std::lround(x.second * 100), ey = std::lround(y.second * 100);
            if (ex != ey) return ex < ey;
            return usedc(y.first) < usedc(x.first);
        });
        std::vector<std::string> codes;
        for (auto& d : dim) codes.push_back(d.first);
        std::vector<std::string> order;
        double cf = 0;
        std::string why;
        bool have = true;
        if (!codes.empty() && !uniq.empty()) {
            std::string ag;
            for (auto& u : uniq) { if (std::find(codes.begin(), codes.end(), u) != codes.end()) { ag = u; break; } }
            if (!ag.empty()) { order = {ag}; cf = 0.95; why = "text+dims agree"; }
            else {
                order = codes; cf = 0.60;
                why = "CONFLICT dims=" + codes[0] + " text=";
                for (size_t i = 0; i < uniq.size(); ++i) why += (i ? "/" : "") + uniq[i];
            }
        } else if (!codes.empty()) { order = codes; cf = 0.85; why = "dims only"; }
        else if (uniq.size() == 1) { order = uniq; cf = 0.90; why = "text only, single size token"; }
        else if (!uniq.empty()) {
            order = uniq; cf = 0.50; why = "text ambiguous ";
            for (size_t i = 0; i < uniq.size(); ++i) why += (i ? "/" : "") + uniq[i];
        } else have = false;
        if (have) {
            for (auto& o : order) {
                auto it = chip.find(fam + "|" + o);
                if (it == chip.end()) continue;
                auto t = top_of(it->second);
                return mk(t.first, cf, bead_note + why + " -> " + fam + o, (double)t.second / total_of(it->second), &it->second, force_cls);
            }
        }
        Size3 pnc = size_from_partno(txt);
        if (!pnc.ok && w && l) { pnc.ok = true; pnc.a = w; pnc.b = l; pnc.note = "measured dims"; }
        if (pnc.ok) {
            NB nb;
            if (nearest(pnc.a, pnc.b, 1, fam.c_str(), 0.18, nb))
                return mk(nb.id, 0.70, fam + ": " + pnc.note + " -> nearest " + fam + " profile " + nb.note, 1, nullptr, force_cls);
            if (nearest(pnc.a, pnc.b, 0, fam.c_str(), 0.18, nb))
                return mk(nb.id, 0.65, fam + ": " + pnc.note + " -> nearest profile " + nb.note, 1, nullptr, force_cls);
        }
        if (const PkgE* pk = lookup_package(txt)) return by_package(pk->cls, pk->L, pk->W, pk->H, pk->label, "");
        g.why = fam + ": no size found";
        return g;
    }

    LeadPkg lp = lead_pkg(txt);
    if (lp.ok && lp.pins) {
        auto it = lead.find(lp.base + "|" + std::to_string(lp.pins));
        if (it != lead.end() && solid(it->second)) {
            auto t = top_of(it->second);
            std::string why = lp.fam + std::to_string(lp.pins) + " in text" + (lp.fam != lp.base ? " (read as " + lp.base + ")" : "");
            return mk(t.first, 0.90, why, (double)t.second / total_of(it->second), &it->second);
        }
        std::vector<int> hits;
        int byname = profile_named(lp.fam, lp.pins, hits);
        if (!byname) byname = profile_named(lp.base, lp.pins, hits);
        if (byname) {
            std::map<int, int> cc;
            int tot = 0;
            for (int h : hits) { auto u = used.find(h); int uu = (u == used.end() ? 0 : u->second) + 1; cc[h] = uu; tot += uu; }
            Guess r = mk(byname, 0.85, lp.fam + std::to_string(lp.pins) + " matches the profile name", (double)cc[byname] / std::max(1, tot), &cc);
            return r;
        }
    }
    std::string dp = discrete_pkg(txt);
    if (!dp.empty()) {
        auto it = disc.find(dp);
        if (it != disc.end() && solid(it->second)) {
            auto t = top_of(it->second);
            return mk(t.first, 0.90, dp + " in text", (double)t.second / total_of(it->second), &it->second);
        }
    }
    if (const PkgE* pk = lookup_package(txt)) return by_package(pk->cls, pk->L, pk->W, pk->H, pk->label, "");
    Size3 d1 = size_from_text(desc), d2 = size_from_text(name);
    Size3 ds = d1.ok ? d1 : d2.ok ? d2 : size_from_partno(txt);
    std::string pw = package_word(txt);
    if (ds.ok && (!pw.empty() || d1.ok || d2.ok)) {
        int gc = class_from_text(txt);
        std::string lbl = (pw.empty() ? (lib_->class_name(gc)) : pw) + " " + fmt2(ds.a) + "x" + fmt2(ds.b);
        double H = (d1.ok || d2.ok) && ds.c ? ds.c : guess_height(ds.a, ds.b, gc);
        return by_package(gc, ds.a, ds.b, H, lbl, ds.note);
    }
    g.why = "no rule matched";
    return g;
}

// ---------------------------------------------------------------------------
// Footprint evidence
// ---------------------------------------------------------------------------
Footprint measure_footprint(const Package& pkg) {
    Footprint f;
    if (pkg.pads.empty()) return f;
    f.valid = true;
    f.pads = (int)pkg.pads.size();
    f.smt = pkg.smt;
    for (auto& p : pkg.pads) if (p.drill > 0) f.smt = false;
    BBox e;
    struct P { double x, y, w, h; };
    std::vector<P> ps;
    for (auto& p : pkg.pads) {
        double r = std::fmod(std::fabs(p.rot), 180.0);
        bool sw = r > 45 && r < 135;
        double pw = sw ? p.h : p.w, ph = sw ? p.w : p.h;
        ps.push_back({p.pos.x, p.pos.y, pw, ph});
        e.add({p.pos.x - pw / 2, p.pos.y - ph / 2});
        e.add({p.pos.x + pw / 2, p.pos.y + ph / 2});
    }
    f.ext_l = std::max(e.w(), e.h());
    f.ext_w = std::min(e.w(), e.h());
    // rows: pads sharing an x (column) or y (row) coordinate
    auto cluster = [&](bool byx) {
        std::vector<double> v;
        for (auto& p : ps) v.push_back(byx ? p.x : p.y);
        std::sort(v.begin(), v.end());
        std::vector<std::pair<double, int>> c;
        for (double x : v) {
            if (!c.empty() && std::fabs(x - c.back().first) < 0.05) ++c.back().second;
            else c.push_back({x, 1});
        }
        return c;
    };
    auto cx = cluster(true), cy = cluster(false);
    double minpitch = 1e9;
    for (size_t i = 0; i < ps.size(); ++i)
        for (size_t j = i + 1; j < ps.size(); ++j) {
            bool same_row = std::fabs(ps[i].y - ps[j].y) < 0.05, same_col = std::fabs(ps[i].x - ps[j].x) < 0.05;
            if (!same_row && !same_col) continue;
            double d = same_row ? std::fabs(ps[i].x - ps[j].x) : std::fabs(ps[i].y - ps[j].y);
            if (d > 0.05) minpitch = std::min(minpitch, d);
        }
    if (minpitch < 1e8 && f.pads > 2) f.pitch = minpitch;
    // body from artwork when present
    if (!pkg.body.empty()) {
        BBox bb = gprims_bbox(pkg.body);
        // CircuitCAM renderings are often an outline drawn AROUND the pads (chips, discretes):
        // only trust the artwork as the component body when it does not enclose the pad field.
        bool encloses = bb.w() >= e.w() - 0.05 && bb.h() >= e.h() - 0.05;
        if (!bb.empty() && bb.w() > 0.2 && bb.h() > 0.2 && !encloses) {
            f.body_l = std::max(bb.w(), bb.h()); f.body_w = std::min(bb.w(), bb.h()); f.body_from_art = true; f.body_known = true;
        }
    }
    double est_l = f.ext_l, est_w = f.ext_w;
    if (f.pads == 2) {
        const P& a = ps[0]; const P& b = ps[1];
        double dx = std::fabs(a.x - b.x), dy = std::fabs(a.y - b.y);
        bool horiz = dx >= dy;
        double cc = horiz ? dx : dy;
        double across = horiz ? std::min(a.h, b.h) : std::min(a.w, b.w);
        double padlen = horiz ? std::max(a.w, b.w) : std::max(a.h, b.h);
        // land patterns put more toe than heel outside the body: body ~ 0.92 x pad centre distance,
        // but never longer than the gap plus both pads
        est_l = std::min(cc * 0.92, cc + padlen); est_w = across * 0.9;
        f.shape = "2-pad";
        f.sides = 2;
        f.body_known = true;
    } else if (cx.size() == 2 && cx[0].second >= 2 && cx[1].second >= 2 && cx[0].second + cx[1].second == f.pads) {
        double padlen = 0; for (auto& p : ps) padlen = std::max(padlen, p.w);
        double inner = std::fabs(cx[1].first - cx[0].first) - padlen;
        int per = std::max(cx[0].second, cx[1].second);
        double along = (per - 1) * (f.pitch > 0 ? f.pitch : 1.27) + (f.pitch > 0 ? f.pitch : 1.27) * 0.85;
        est_l = std::max(along, inner); est_w = std::min(along, inner);
        f.sides = 2; f.shape = "dual row " + std::to_string(f.pads); f.body_known = true;
    } else if (cy.size() == 2 && cy[0].second >= 2 && cy[1].second >= 2 && cy[0].second + cy[1].second == f.pads) {
        double padlen = 0; for (auto& p : ps) padlen = std::max(padlen, p.h);
        double inner = std::fabs(cy[1].first - cy[0].first) - padlen;
        int per = std::max(cy[0].second, cy[1].second);
        double along = (per - 1) * (f.pitch > 0 ? f.pitch : 1.27) + (f.pitch > 0 ? f.pitch : 1.27) * 0.85;
        est_l = std::max(along, inner); est_w = std::min(along, inner);
        f.sides = 2; f.shape = "dual row " + std::to_string(f.pads); f.body_known = true;
    } else if (f.pads >= 8) {
        // quad: pads on the outer columns/rows on all four sides -> body = inner span
        int edge = 0;
        for (auto& p : ps)
            if (std::fabs(p.x - cx.front().first) < 0.05 || std::fabs(p.x - cx.back().first) < 0.05 ||
                std::fabs(p.y - cy.front().first) < 0.05 || std::fabs(p.y - cy.back().first) < 0.05) ++edge;
        if (edge >= f.pads - 1 && cx.size() > 2 && cy.size() > 2) {
            double plx = 0, ply = 0;
            for (auto& p : ps) { if (std::fabs(p.x - cx.front().first) < 0.05) plx = std::max(plx, p.w); if (std::fabs(p.y - cy.front().first) < 0.05) ply = std::max(ply, p.h); }
            double ix = std::fabs(cx.back().first - cx.front().first) - plx, iy = std::fabs(cy.back().first - cy.front().first) - ply;
            est_l = std::max(ix, iy); est_w = std::min(ix, iy);
            f.sides = 4; f.shape = "quad " + std::to_string(f.pads); f.body_known = true;
            if (edge == f.pads - 1) f.shape += " + thermal pad";
        } else {
            f.shape = "array " + std::to_string(f.pads);
        }
    } else {
        f.shape = std::to_string(f.pads) + "-pad";
        if (f.pads == 3) f.shape = "SOT (3 pads)";
    }
    if (!f.smt) f.shape += ", through-hole";
    if (!f.body_from_art) { f.body_l = std::max(est_l, est_w); f.body_w = std::min(est_l, est_w); }
    if (f.pads == 2 && f.body_l > 0) {
        auto ds = dim_sizes(f.body_l, f.body_w);
        if (!ds.empty() && ds[0].second <= 0.22) { f.chip_code = ds[0].first; f.chip_err = ds[0].second; }
    }
    return f;
}

const char* status_name(Status s) {
    switch (s) { case Status::Taught: return "taught"; case Status::Auto: return "auto"; case Status::Conflict: return "conflict"; default: return "untaught"; }
}

static double geo_error(const Profile& p, const Footprint& f) {
    if (!f.valid || p.w <= 0 || p.l <= 0) return -1;
    double pa = std::max(p.w, p.l), pb = std::min(p.w, p.l);
    auto err = [&](double a, double b) {
        if (a <= 0 || b <= 0) return 1e9;
        double dx = (a - pa) / pa, dy = (b - pb) / pb;
        return std::sqrt(dx * dx + dy * dy);
    };
    double e = std::min(err(f.body_l, f.body_w), err(f.ext_l, f.ext_w));
    if (f.pads > 2 || !f.body_known) {
        // leaded parts: FlexDB nominal sizes are sometimes the plastic body only (SOT 2.9x1.3) and the
        // CircuitCAM outline includes the leads -> accept a profile that fits inside the pad field
        // with its long side matching the span
        double span = std::max(f.ext_l, f.body_l), across = std::max(f.ext_w, f.body_w);
        if (pb <= across * 1.15 + 0.2) e = std::min(e, std::fabs(pa - span) / pa);
    }
    return e;
}

static int pins_in_name(const std::string& nm) {
    LeadPkg lp = lead_pkg(nm);
    if (lp.ok && lp.pins) return lp.pins;
    std::string d = discrete_pkg(nm);
    if (d == "SOT23") return 3;
    if (d == "SOT23-5") return 5;
    if (d == "SOT23-6" || d == "SOT363") return 6;
    if (d == "SOT23-8") return 8;
    if (d == "SOT223" || d == "SOT89") return 4;
    return 0;
}

Evaluation evaluate(const Classifier& cl, const std::string& name, const std::string& desc, const Package* pkg,
                    double meas_w, double meas_l) {
    Evaluation ev;
    const Library* lib = cl.lib();
    ev.text = cl.classify(name, desc, meas_w, meas_l);
    if (pkg) ev.fp = measure_footprint(*pkg);
    const Footprint& fp = ev.fp;
    if (!lib) return ev;

    struct Cand { double base = 0; std::string why; };
    std::set<int> geo_only;
    std::map<int, Cand> cand;
    auto add = [&](int pid, double base, const std::string& why) {
        if (!pid || !lib->find(pid)) return;
        auto& c = cand[pid];
        if (base > c.base) { c.base = base; c.why = why; }
    };
    add(ev.text.pid, ev.text.conf, ev.text.why);
    for (size_t i = 0; i < ev.text.alt_ids.size() && i < 3; ++i) add(ev.text.alt_ids[i], ev.text.conf * 0.5, "house alternative for the same key");
    if (const Profile* p = lib->find(ev.text.pid)) if (p->sibling) add(p->sibling, ev.text.conf * 0.45, "same size, other carrier tape");

    std::string fam = family_of(desc);
    if (fam.empty()) fam = family_of(name);
    bool chip_family = fam == "RES" || fam == "CAP" || fam == "IND" || fam == "BEAD" || fam == "LED" || fam == "TANT";
    if (fp.valid && fp.body_l > 0) {
        // 1) FlexAssign's own logic with the footprint body standing in for machine-measured dims
        if (fp.pads == 2 && chip_family && !(meas_w && meas_l)) {
            Guess g2 = cl.classify(name, desc, fp.body_w, fp.body_l);
            if (g2.pid) add(g2.pid, g2.conf, g2.why + " [footprint body " + fmt2(fp.body_l) + "x" + fmt2(fp.body_w) + "]");
        }
        // 2) nearest library bodies of a compatible class
        int want = 0;
        if (fp.pads == 2) want = 0;   // chip / MELF / LED / tantalum all possible
        std::vector<std::pair<double, int>> near;
        for (auto& p : lib->profiles) {
            double e = geo_error(p, fp);
            if (e < 0 || e > 0.25) continue;
            if (want && p.cls != want) continue;
            if (fp.pads == 2 && !(p.cls == 1 || p.cls == 3 || p.cls == 14 || p.cls == 6)) continue;
            if (fp.pads > 2 && (p.cls == 1 || p.cls == 3 || p.cls == 14)) continue;
            int pn = pins_in_name(p.name);
            if (pn && fp.pads > 2 && pn != fp.pads && pn + 1 != fp.pads) continue;
            near.push_back({e, p.id});
        }
        std::sort(near.begin(), near.end());
        const char* tfam = fam.empty() ? nullptr : (fam == "BEAD" ? "CAP" : fam == "ALCAP" ? "CAP" : fam.c_str());
        const std::regex* pat = fam_namepat(tfam);
        size_t taken = 0;
        for (size_t i = 0; i < near.size() && taken < 5; ++i) {
            const Profile* p = lib->find(near[i].second);
            bool fam_ok = !pat || rs(*pat, p->name);
            if (!fam_ok && chip_family) continue;          // geometry alone never crosses part families
            auto u = cl.used.find(p->id);
            double house = u == cl.used.end() ? 0 : std::min(1.0, u->second / 50.0);
            ++taken;
            geo_only.insert(p->id);
            add(p->id, 0.30 + 0.25 * (1 - near[i].first / 0.25) + 0.10 * house,
                "footprint body " + fmt2(fp.body_l) + "x" + fmt2(fp.body_w) + " matches " + fmt2(p->l) + "x" + fmt2(p->w) +
                    " (" + std::to_string((int)std::lround(near[i].first * 100)) + "% off)");
        }
    }
    // score
    for (auto& kv : cand) {
        const Profile* p = lib->find(kv.first);
        Suggestion s;
        s.pid = kv.first;
        s.conf = kv.second.base;
        s.why = kv.second.why;
        s.geo_err = geo_error(*p, fp);
        if (fp.valid) {
            bool from_geo = geo_only.count(s.pid) && kv.second.why.rfind("footprint body", 0) == 0;
            if (s.geo_err >= 0 && s.geo_err <= 0.15 && !from_geo) s.conf += (1 - s.conf) * 0.5;
            else if (s.geo_err > 0.5) { s.conf *= 0.5; s.why += "; body size disagrees with footprint"; }
            else if (s.geo_err > 0.3) { s.conf *= 0.85; s.why += "; body size only roughly matches the footprint"; }
            if (from_geo && ev.text.pid && ev.text.conf >= 0.8) s.conf = std::min(s.conf, ev.text.conf * 0.7);
            int pn = pins_in_name(p->name);
            if (pn && fp.pads > 2 && pn != fp.pads && pn + 1 != fp.pads) { s.conf *= 0.4; s.why += "; pin count " + std::to_string(pn) + " vs " + std::to_string(fp.pads) + " pads"; }
            if (fp.pads == 2 && pn > 2) s.conf *= 0.3;
        }
        s.conf = std::min(0.99, s.conf);
        ev.sugg.push_back(s);
    }
    std::stable_sort(ev.sugg.begin(), ev.sugg.end(), [](const Suggestion& a, const Suggestion& b) { return a.conf > b.conf; });
    if (ev.sugg.size() > 8) ev.sugg.resize(8);

    // warnings
    if (fp.valid) {
        auto ts = text_chip_sizes(desc + " | " + name);
        if (!fp.chip_code.empty() && !ts.empty() && std::find(ts.begin(), ts.end(), fp.chip_code) == ts.end()) {
            ev.warnings.push_back("description says " + ts[0] + " but the footprint body measures " + fmt2(fp.body_l) + "x" +
                                  fmt2(fp.body_w) + " mm (" + fp.chip_code + ")");
            ev.conflict = true;
        }
        if (!fp.smt) ev.warnings.push_back("through-hole footprint (drilled pads) - check that this part is machine placed");
        if (!ev.sugg.empty()) {
            const Profile* b = lib->find(ev.sugg[0].pid);
            if (ev.sugg[0].geo_err > 0.5) {
                ev.warnings.push_back("best profile " + b->name + " is " + fmt2(b->l) + "x" + fmt2(b->w) + " mm, footprint body is " +
                                      fmt2(fp.body_l) + "x" + fmt2(fp.body_w) + " mm");
                ev.conflict = true;
            }
            int pn = pins_in_name(b->name);
            if (pn && fp.pads > 2 && pn != fp.pads && pn + 1 != fp.pads) {
                ev.warnings.push_back(b->name + " has " + std::to_string(pn) + " leads, footprint has " + std::to_string(fp.pads) + " pads");
                ev.conflict = true;
            }
        }
        if (ev.text.pid) {
            const Profile* t = lib->find(ev.text.pid);
            double ge = t ? geo_error(*t, fp) : -1;
            if (t && ge > 0.5 && (ev.sugg.empty() || ev.sugg[0].pid != ev.text.pid))
                ev.warnings.push_back("description classifier picked " + t->name + " but its body does not fit the footprint");
        }
    }
    if (ev.text.gap) ev.warnings.push_back("no library profile for " + ev.text.gap_label + " - build one (" + ev.text.why + ")");
    // feeder implications
    double fa = 0, fb = 0;
    if (fp.valid && fp.body_l > 0) { fa = fp.body_l; fb = fp.body_w; }
    else if (!ev.sugg.empty()) { const Profile* b = lib->find(ev.sugg[0].pid); fa = b->l; fb = b->w; }
    ev.feeder = predict_feeder(fa, fb);
    if (!ev.sugg.empty() && ev.feeder.tape_mm) {
        const Profile* b = lib->find(ev.sugg[0].pid);
        if (b->nozzle >= 500 && b->nozzle <= 508 && std::to_string(b->nozzle) != ev.feeder.nozzle) {
            int d = std::abs(std::stoi(ev.feeder.nozzle) - b->nozzle);
            if (d >= 2 || (fp.valid && d >= 1 && fb > 1.4))
                ev.warnings.push_back("profile nozzle " + std::to_string(b->nozzle) + ", body short edge " + fmt2(fb) +
                                      " mm suggests " + ev.feeder.nozzle);
        }
    }
    return ev;
}

// ------------------------------------------------------------------------------------------------
namespace {
std::string lower_s(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
struct FamRule { const char* rx; std::vector<int> cls; std::vector<const char*> words; };
const std::vector<FamRule>& fam_rules() {
    static const std::vector<FamRule> r = {
        {R"(\b(RES|RESISTOR|OHMS?)\b)", {1, 3}, {"res"}},
        {R"(\b(CAP|CAPACITOR)\b)", {1, 3, 6}, {"cap", "tant"}},
        {R"(\b(IND|INDUCTORS?|FERRITE|BEAD|CMC|CHOKES?|COIL)\b)", {1, 3, 6, 251, 999}, {"ind", "coil", "bead", "shield", "smc", "choke"}},
        {R"(\bLED\b)", {14, 1}, {"led"}},
        {R"(\b(OSC|XTAL|CRYSTAL|RESONATOR)\b)", {1, 251, 999}, {"osc", "xtal", "crys"}},
        {R"(\bSOD-?\d+)", {11, 3, 1}, {"sod"}},
        {R"(\bSOT-?\d+|\bSC-?70|\bSC-?88)", {11}, {"sot"}},
        {R"(\b(QFN|DFN|VQFN|WQFN|SON|LFCSP|\d+LFCSP)\b)", {120, 101, 102, 104}, {"qfn", "dfn", "vqfn", "son"}},
        {R"(\b(LQFP|TQFP|QFP)\b)", {102, 106}, {"qfp", "lqfp", "tqfp"}},
        {R"(TSSOP)", {101, 104}, {"tssop"}},
        {R"(SSOP|MSOP)", {101, 104}, {"ssop", "msop"}},
        {R"(\bSOIC|\bSO-?\d+\b|\bSOP\b)", {101, 109}, {"so", "soic", "sol", "sop"}},
        {R"(\bBGA\b)", {107, 110}, {"bga"}},
        {R"(\b(CONN|HEADER|SOCKET|TERMINAL|RCPT|PLUG)\b)", {201, 202, 203, 251, 999}, {"conn", "header", "hdr"}},
        {R"(\b(DIODE|TVS|ZENER|SCHOTTKY|RECTIFIER)\b)", {11, 3, 1}, {"sod", "sm", "do", "melf"}},
    };
    return r;
}
}  // namespace

BodyPick pick_by_body(const Library& lib, const std::string& desc, double l, double w, double h, int tape, int nozzle, const std::set<int>* owned,
                      int pins) {
    BodyPick best;
    if (l <= 0 || w <= 0) { best.why = "no body size"; return best; }
    std::string D = upper(desc);
    std::vector<int> cls;
    std::vector<const char*> words;
    for (auto& r : fam_rules()) {
        static std::map<const char*, std::regex> cache;
        auto it = cache.find(r.rx);
        if (it == cache.end()) it = cache.emplace(r.rx, std::regex(r.rx)).first;
        if (std::regex_search(D, it->second)) {
            if (cls.empty()) cls = r.cls;          // first matching family decides the class set
            words.insert(words.end(), r.words.begin(), r.words.end());
        }
    }
    const std::string chip = history::chip_code_in(D);
    char nz[8] = "";
    if (nozzle) std::snprintf(nz, sizeof nz, "%d", nozzle);
    for (const Profile& p : lib.profiles) {
        double pl = std::max(p.w, p.l), pw = std::min(p.w, p.l);
        if (pl <= 0 || pw <= 0) continue;
        double c = std::fabs(std::log(l / pl)) + std::fabs(std::log(w / pw));
        if (h > 0 && p.h > 0) c += 0.3 * std::fabs(std::log(std::max(h, 0.1) / std::max(p.h, 0.1)));
        if (tape && p.tape_type && p.tape_type != tape) c += 0.6;
        if (!cls.empty() && std::find(cls.begin(), cls.end(), p.cls) == cls.end()) c += 1.0;
        std::string nm = lower_s(p.name);
        if (!words.empty()) {
            bool hit = false;
            for (const char* t : words) {
                size_t k = nm.find(t);
                if (k != std::string::npos && (k == 0 || !std::isalpha((unsigned char)nm[k - 1]))) { hit = true; break; }
            }
            if (!hit) {
                // IC families are named in the library (QFN24, SOIC8 ...): a miss there is a different package
                static const char* ic[] = {"qfn", "dfn", "qfp", "lqfp", "tqfp", "soic", "so", "sop", "ssop", "tssop", "msop", "bga", "sot", "sod"};
                bool icf = false;
                for (const char* t : words) for (const char* q : ic) if (!std::strcmp(t, q)) icf = true;
                c += icf ? 1.0 : 0.5;
            }
        }
        if (!chip.empty() && !cls.empty() && (cls[0] == 1)) c += nm.find(chip) != std::string::npos ? 0 : 0.8;
        if (nozzle) {
            static const std::regex any50x(R"((^|[^0-9])50[0-9]([^0-9]|$))");
            bool has = false;
            for (size_t k = nm.find(nz); k != std::string::npos; k = nm.find(nz, k + 1)) {
                bool lb = k == 0 || !std::isdigit((unsigned char)nm[k - 1]), rb = k + 3 >= nm.size() || !std::isdigit((unsigned char)nm[k + 3]);
                if (lb && rb) { has = true; break; }
            }
            if (!has && std::regex_search(nm, any50x)) c += 0.15;
        }
        if (owned && !owned->empty() && p.nozzle && !owned->count(p.nozzle)) c += 1.0;   // needs a nozzle the shop does not have
        // a profile named for another part family ("Res4527" for a choke, "QFP48" for an inductor)
        if (!words.empty()) {
            static const char* fams[] = {"res", "cap", "ind", "coil", "led", "qfp", "lqfp", "tqfp", "qfn", "dfn", "sot", "sod", "soic", "sop",
                                         "ssop", "tssop", "bga", "tant", "melf", "xtal", "osc", "header", "conn"};
            size_t a = 0;
            while (a < nm.size() && !std::isalpha((unsigned char)nm[a])) ++a;
            size_t b = a;
            while (b < nm.size() && std::isalpha((unsigned char)nm[b])) ++b;
            const std::string tok = nm.substr(a, b - a);
            bool other = false, mine = false;
            for (const char* f : fams) if (tok == f) other = true;
            for (const char* t : words) if (tok == t || tok.rfind(t, 0) == 0) mine = true;
            if (other && !mine) c += 0.8;
        }
        if (pins > 2) {
            int pn = pins_in_name(p.name);
            if (pn) c += std::min(0.8, 0.15 * std::min(std::abs(pn - pins), std::abs(pn - (pins - 1))));   // pins - 1: exposed pad
        }
        if (c < best.cost) { best.cost = c; best.pid = p.id; best.why = p.name; }
    }
    return best;
}

int profile_for_code(const Library& lib, const std::vector<std::string>& aliases, const std::set<int>* owned, const std::map<int, int>* used) {
    int best = 0;
    double bs = -1e9;
    for (const Profile& p : lib.profiles) {
        const std::string n = history::norm_code(p.name);
        int rank = -1;
        for (size_t i = 0; i < aliases.size() && rank < 0; ++i) {
            const std::string& a = aliases[i];
            if (n.compare(0, a.size(), a) != 0) continue;
            char nx = n.size() > a.size() ? n[a.size()] : ' ';
            if (std::isdigit((unsigned char)nx)) continue;                  // SOD123 must not take SOD1234
            if (std::isalpha((unsigned char)nx) && nx != 'F') continue;      // ... nor SOT23 take SOT23A
            rank = (int)i;
        }
        if (rank < 0) continue;
        double sc = -rank * 0.5;                                           // canonical alias first
        if (owned && !owned->empty() && p.nozzle) sc += owned->count(p.nozzle) ? 2.0 : -4.0;
        if (used) { auto u = used->find(p.id); if (u != used->end()) sc += std::min(1.5, 0.1 * u->second); }
        sc -= 0.02 * (double)p.name.size();                                // plain "SOD123" over "SOD123 special"
        if (sc > bs) { bs = sc; best = p.id; }
    }
    return best;
}

int owned_twin(const Library& lib, int pid, const std::set<int>& owned) {
    const Profile* p = lib.find(pid);
    if (!p || owned.empty() || !p->nozzle || owned.count(p->nozzle)) return pid;
    const double pl = std::max(p->l, p->w), pw = std::min(p->l, p->w);
    int best = 0;
    double bc = 1e9;
    for (const Profile& q : lib.profiles) {
        if (!q.nozzle || !owned.count(q.nozzle) || q.cls != p->cls) continue;
        double ql = std::max(q.l, q.w), qw = std::min(q.l, q.w);
        if (ql <= 0 || qw <= 0 || pl <= 0 || pw <= 0) continue;
        double c = std::fabs(std::log(ql / pl)) + std::fabs(std::log(qw / pw));
        if (c > 0.15) continue;
        if (q.tape_kind != p->tape_kind) c += 0.3;
        if (Classifier::base_size(q.name) != Classifier::base_size(p->name)) c += 0.2;
        // same family word ("Cap" stays "Cap")
        std::string a = p->name.substr(0, 3), b = q.name.substr(0, 3);
        for (auto& ch : a) ch = (char)std::tolower((unsigned char)ch);
        for (auto& ch : b) ch = (char)std::tolower((unsigned char)ch);
        if (a != b) c += 0.1;
        if (c < bc) { bc = c; best = q.id; }
    }
    return best;
}

Teach teach_part(const Classifier& cls, const Library& lib, const history::History& hist, const std::string& name, const std::string& desc,
                 double ext_l, double ext_w, bool use_hist, const std::string& pkg_name, double fp_l, double fp_w) {
    Teach t;
    const std::set<int>& owned = hist.owned_nozzles();
    // package evidence: a code in the description wins over the CAD package name; IPC names give a body
    history::PkgBody pc, ipc;
    const bool code_in_desc = history::package_code(desc, pc);
    bool has_code = code_in_desc || history::package_code(pkg_name, pc);
    bool has_ipc = history::ipc_body(pkg_name, ipc);
    if (!has_code && has_ipc && !ipc.code.empty()) { pc = ipc; has_code = true; }
    // family word implied by the land-pattern name, for the body pick ("INDM5552X300N" -> inductor)
    std::string fdesc = desc;
    {
        std::string u = pkg_name;
        for (auto& c : u) c = (char)std::toupper((unsigned char)c);
        static const std::pair<const char*, const char*> pre[] = {
            {"INDM", "INDUCTOR"}, {"INDC", "INDUCTOR"}, {"CAPC", "CAP"}, {"CAPM", "CAP"}, {"CAPT", "CAP"}, {"CAPAE", "CAP"}, {"RESC", "RES"}, {"RESM", "RES"},
            {"LEDC", "LED"}, {"DIOM", "DIODE"}, {"DIOC", "DIODE"}, {"SOD", "DIODE"}, {"VQFN", "QFN"}, {"WQFN", "QFN"}, {"QFN", "QFN"}, {"DFN", "DFN"},
            {"SON", "DFN"}, {"SOIC", "SOIC"}, {"SOP", "SOIC"}, {"TSSOP", "TSSOP"}, {"SSOP", "SSOP"}, {"MSOP", "MSOP"}, {"LQFP", "LQFP"},
            {"TQFP", "TQFP"}, {"QFP", "QFP"}, {"SOT", "SOT"}, {"BGA", "BGA"}};
        for (auto& pr : pre) if (u.rfind(pr.first, 0) == 0) { fdesc += std::string(" ") + pr.second; break; }
    }
    const int pins = has_ipc ? ipc.pins : has_code ? pc.pins : 0;
    history::BodyHint hint;
    if (has_code) { hint.l = pc.l; hint.w = pc.w; hint.h = pc.h; hint.src = pc.src; }
    else if (has_ipc) { hint.l = ipc.l; hint.w = ipc.w; hint.h = ipc.h; hint.src = ipc.src; }
    else if (fp_l > 0 && fp_w > 0) { hint.l = fp_l; hint.w = fp_w; hint.src = "footprint body"; }
    t.machine = hist.propose(name, desc, ext_l, ext_w, 0, use_hist, hint.l > 0 ? &hint : nullptr);
    const history::Proposal& m = t.machine;

    auto take = [&](int pid, double conf, const std::string& route) {
        int tw = owned_twin(lib, pid, owned);
        if (tw && tw != pid) {
            const Profile* a = lib.find(pid);
            const Profile* b = lib.find(tw);
            t.pid = tw;
            t.profile_route = route + "; nozzle " + std::to_string(a ? a->nozzle : 0) + " not owned -> " + (b ? b->name : std::string("?"));
            t.conf = conf * 0.95;
            return true;
        }
        t.pid = pid; t.conf = conf; t.profile_route = route;
        if (!tw) {   // no same-size profile with an owned nozzle: keep it, flag it
            const Profile* a = lib.find(pid);
            t.profile_route += "; needs nozzle " + std::to_string(a ? a->nozzle : 0) + " (not in the owned list, no alternative)";
            t.conf = conf * 0.7;
        }
        return true;
    };
    Guess g = cls.classify(name, desc);
    if (g.pid && g.conf >= 0.9) {
        char b[32]; std::snprintf(b, sizeof b, "text %.2f: ", g.conf);
        if (take(g.pid, std::min(0.97, g.conf), b + g.why)) return t;
    }
    if (has_code)
        if (int pid = profile_for_code(lib, pc.aliases, &owned, &cls.used))
            if (take(pid, 0.9, "package " + pc.code + " in " + (code_in_desc ? "description" : "package name"))) return t;
    if (m.l > 0) {
        BodyPick bp = pick_by_body(lib, fdesc, m.l, m.w, m.h, m.tape, m.nozzle, &owned, pins);
        if (bp.pid) {
            double c = m.from_history ? 0.8 : hint.l > 0 || m.why.find("CAD") != std::string::npos ? 0.72 : 0.6;
            if (bp.cost > 1.5) c *= 0.7;
            take(bp.pid, c, "body: closest library profile " + bp.why + " (" + m.why.substr(0, m.why.find(';')) + ")");
            return t;
        }
    }
    if (g.pid) take(g.pid, std::max(0.3, g.conf * 0.8), "text (low confidence): " + g.why);
    return t;
}

bool ComponentDB::apply_machine(int cid, const history::Proposal& p, std::string& err) {
    auto put = [&](const char* table, const char* col, int64_t v) -> bool {
        if (!v) return true;
        jet::ETable* t = this->table(table, &err);
        if (!t) return true;                      // table absent in this DB generation: nothing to set
        int k = t->col("CompoID"), c = t->col(col);
        if (k < 0 || c < 0) return true;
        for (size_t r = 0; r < t->rows.size(); ++r)
            if (t->get_i(r, k) == cid && !t->set_i(r, c, v)) { err = std::string("cannot set ") + table + "." + col; return false; }
        touched_.insert(upper(table));
        return true;
    };
    if (p.l > 0 && p.w > 0) {
        if (!put("Compo", "Width", std::llround(std::max(p.l, p.w) * 1e4)) || !put("Compo", "Length", std::llround(std::min(p.l, p.w) * 1e4))) return false;
        if (p.h > 0 && !put("Compo", "Height", std::llround(p.h * 1e4))) return false;
    }
    return put("Feeder", "TapeTypeID", p.tape) && put("Feeder", "Pitch", p.pitch) && put("Laser", "GX4NozzleNo", p.nozzle) &&
           put("LaserFx3", "NozzleNo", p.nozzle);
}

} // namespace pcbp::flexdb
