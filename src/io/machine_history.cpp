#include "io/machine_history.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <regex>

namespace pcbp::history {

static std::string upper(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
static double num(const std::string& s) { char* e = nullptr; double v = std::strtod(s.c_str(), &e); return e == s.c_str() ? 0 : v; }

int tape_from_code(const std::string& t) {
    static const std::pair<const char*, int> kinds[] = {{"8MMPAPER", 1}, {"8MMEMBOSS", 2}, {"12MM", 3}, {"16MM", 4}, {"24MM", 5},
                                                        {"32PAPER", 6}, {"32EMBOSS", 7}, {"44MM", 8}, {"56MM", 9}};
    const std::string u = upper(trim(t));
    for (auto& k : kinds) if (u == k.first) return k.second;
    if (u.size() > 2 && u[0] == '0' && u[1] == 'X') {
        int v = (int)(std::strtol(u.c_str() + 2, nullptr, 16) & 0xff);   // HLC: low byte = TapeTypeID
        return v >= 1 && v <= 9 ? v : 0;
    }
    return 0;
}

int pitch_from_code(const std::string& p) {
    const std::string u = upper(trim(p));
    if (u.size() > 2 && u[0] == '0' && u[1] == 'X') {
        int v = (int)std::strtol(u.c_str() + 2, nullptr, 16);
        if (v == 0x02CC) return 204;     // 2 mm special feed (FlexLine code)
        if (v == 0x02CE) return 206;
        return v & 0xff;
    }
    int v = (int)num(u);
    return v > 0 && v <= 206 ? v : 0;
}

int nozzle_from_field(const std::string& ext, const std::string& f) {
    // h8h: "KE:a GX:b FX3:c" -> GX value; others: first non-zero number
    int v = 0;
    if (ext == ".h8h" || ext == ".h7h") {
        size_t p = f.find("GX:");
        if (p != std::string::npos) v = std::atoi(f.c_str() + p + 3);
    } else {
        const char* s = f.c_str();
        while (*s) {
            while (*s && !std::isdigit((unsigned char)*s)) ++s;
            if (!*s) break;
            int x = std::atoi(s);
            while (std::isdigit((unsigned char)*s)) ++s;
            if (x) { v = x; break; }
        }
    }
    if (!v) return 0;
    if (v >= 7000) v -= 7000;            // RS-1: 75xx
    else if (v >= 5000) v -= 5000;
    // machine split files and the HLC master store FlexLine number + 1; ISS stores it as-is
    if (ext == ".e47" || ext == ".e48" || ext == ".x52" || ext == ".x01" || ext == ".s01x" || ext == ".h8h" || ext == ".h7h") --v;
    return v >= 500 && v <= 520 ? v : 0;
}

// ---------------------------------------------------------------------------------------- CSV
static bool next_row(const std::string& s, size_t& i, std::vector<std::string>& out) {
    out.clear();
    if (i >= s.size()) return false;
    std::string cell;
    bool q = false;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (q) {
            if (c == '"') { if (i + 1 < s.size() && s[i + 1] == '"') { cell += '"'; ++i; } else q = false; }
            else cell += c;
        } else if (c == '"') q = true;
        else if (c == ',') { out.push_back(cell); cell.clear(); }
        else if (c == '\n') { ++i; break; }
        else if (c != '\r') cell += c;
    }
    out.push_back(cell);
    return true;
}

size_t History::load_csv(const std::string& path, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open " + path; return 0; }
    std::string s((std::istreambuf_iterator<char>(in)), {});
    size_t i = 0;
    std::vector<std::string> row, head;
    if (!next_row(s, i, head)) { err = "empty file"; return 0; }
    auto col = [&](const char* n) { for (size_t k = 0; k < head.size(); ++k) if (head[k] == n) return (int)k; return -1; };
    const int cExt = col("Ext"), cName = col("Name"), cW = col("W"), cL = col("L"), cH = col("H"), cT = col("TapeCode"), cP = col("PitchCode"),
              cN = col("Nozzle");
    if (cExt < 0 || cName < 0 || cW < 0 || cL < 0) { err = "not a pnp_harvest components CSV"; return 0; }
    size_t n = 0;
    auto at = [&](int c) { return c >= 0 && c < (int)row.size() ? row[c] : std::string(); };
    while (next_row(s, i, row)) {
        Record r;
        r.name = upper(trim(at(cName)));
        if (r.name.empty()) continue;
        r.ext = at(cExt);
        double a = num(at(cW)), b = num(at(cL));
        r.l = std::max(a, b); r.w = std::min(a, b); r.h = num(at(cH));
        r.tape = tape_from_code(at(cT));
        r.pitch = pitch_from_code(at(cP));
        r.nozzle = nozzle_from_field(r.ext, at(cN));
        r.measured = r.ext == ".e47" || r.ext == ".e48" || r.ext == ".x52" || r.ext == ".x01" || r.ext == ".s01x";
        add(r);
        ++n;
    }
    return n;
}

size_t History::load_cpf_parts(const std::string& path, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open " + path; return 0; }
    std::string s((std::istreambuf_iterator<char>(in)), {});
    size_t i = 0, n = 0;
    std::vector<std::string> head, row;
    if (!next_row(s, i, head)) { err = "empty file"; return 0; }
    auto col = [&](const char* k) { for (size_t j = 0; j < head.size(); ++j) if (head[j] == k) return (int)j; return -1; };
    const int cP = col("PartNumber"), cL = col("BodyL"), cW = col("BodyW"), cH = col("Height"), cEL = col("PadExtL"), cEW = col("PadExtW");
    if (cP < 0 || cL < 0 || cW < 0) { err = "not a cpf_harvest parts CSV"; return 0; }
    while (next_row(s, i, row)) {
        if ((int)row.size() <= std::max(cL, cW)) continue;
        double a = num(row[cL]), b = num(row[cW]);
        if (a <= 0 || b <= 0) continue;
        double ea = cEL >= 0 && cEL < (int)row.size() ? num(row[cEL]) : 0, eb = cEW >= 0 && cEW < (int)row.size() ? num(row[cEW]) : 0;
        cad_[upper(trim(row[cP]))] = {std::max(a, b), std::min(a, b), cH >= 0 && cH < (int)row.size() ? num(row[cH]) : 0, std::max(ea, eb), std::min(ea, eb)};
        ++n;
    }
    return n;
}

bool History::cad_body(const std::string& name, double& l, double& w, double& h) const {
    auto it = cad_.find(upper(trim(name)));
    if (it == cad_.end()) return false;
    l = it->second.l; w = it->second.w; h = it->second.h;
    return true;
}

void History::add(const Record& r) { pending_.push_back(r); ++n_records_; }

static Pick majority(const std::map<int, int>& m) {
    Pick p;
    int tot = 0;
    for (auto& kv : m) { tot += kv.second; if (kv.second > p.votes) { p.votes = kv.second; p.value = kv.first; } }
    p.share = tot ? (double)p.votes / tot : 0;
    return p;
}
static double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
static long long fine_key(double l, double w, double h) {
    long long a = std::lround(std::log(std::max(l, 0.1)) * 6), b = std::lround(std::log(std::max(w, 0.1)) * 6);
    long long c = h > 0 ? std::min<long long>((long long)(h * 2), 12) : -1;
    return ((a + 100) << 20) | ((b + 100) << 8) | (c + 1);
}
static long long coarse_key(double l, double w) {
    long long a = std::lround(std::log(std::max(l, 0.1)) * 4), b = std::lround(std::log(std::max(w, 0.1)) * 4);
    return ((a + 100) << 12) | (b + 100);
}

void History::build() {
    std::map<std::string, std::vector<const Record*>> by;
    for (auto& r : pending_) by[r.name].push_back(&r);
    for (auto& kv : by) {
        std::vector<double> L, W, H;
        bool meas = false;
        for (auto* r : kv.second) meas |= r->measured && r->l > 0;
        std::map<int, int> t, p, n;
        for (auto* r : kv.second) {
            if (r->l > 0 && (!meas || r->measured)) { L.push_back(r->l); W.push_back(r->w); H.push_back(r->h); }
            if (r->tape) t[r->tape]++;
            if (r->pitch) p[r->pitch]++;
            if (r->nozzle) n[r->nozzle]++;
        }
        if (L.empty()) continue;
        PartSummary s;
        s.l = median(L); s.w = median(W); s.h = median(H);
        s.measured = meas;
        s.programs = (int)kv.second.size();
        s.tape = majority(t); s.pitch = majority(p); s.nozzle = majority(n);
        parts_[kv.first] = s;
        if (s.nozzle.value) nozzle_use_[s.nozzle.value]++;
        const int vals[3] = {s.tape.value, s.pitch.value, s.nozzle.value};
        for (int f = 0; f < 3; ++f) {
            if (!vals[f]) continue;
            fine_[f][fine_key(s.l, s.w, s.h)][vals[f]]++;
            coarse_[f][coarse_key(s.l, s.w)][vals[f]]++;
        }
    }
    // extent-keyed tables: parts that ran AND have a CAD land pattern
    for (auto& kv : parts_) {
        auto c = cad_.find(kv.first);
        if (c == cad_.end() || c->second.el <= 0 || c->second.ew <= 0) continue;
        const PartSummary& s = kv.second;
        const int vals[3] = {s.tape.value, s.pitch.value, s.nozzle.value};
        for (int f = 0; f < 3; ++f) if (vals[f]) ext_[f][coarse_key(c->second.el, c->second.ew)][vals[f]]++;
        if (s.measured) {
            auto& r = ratio_[coarse_key(c->second.el, c->second.ew)];
            r.first.push_back(s.l / c->second.el); r.second.push_back(s.w / c->second.ew);
        }
    }
    // owned nozzles: the majority nozzle of at least 1% of the parts (rare entries are old programs, typos or
    // borrowed nozzles); the user's list in the Components view overrides this
    owned_auto_.clear();
    int ntot = 0;
    for (auto& kv : nozzle_use_) ntot += kv.second;
    for (auto& kv : nozzle_use_) if (kv.second >= std::max(3, ntot / 100)) owned_auto_.insert(kv.first);
    for (auto& kv : ratio_) if (kv.second.first.size() >= 3) ratio_med_[kv.first] = {median(kv.second.first), median(kv.second.second)};
    ratio_.clear();
    pending_.clear();
    pending_.shrink_to_fit();
}

const PartSummary* History::find(const std::string& name) const {
    auto it = parts_.find(upper(trim(name)));
    return it == parts_.end() ? nullptr : &it->second;
}

Pick History::by_size(double l, double w, double h, int f, int exclude) const {
    if (f < 0 || f > 2 || l <= 0) return {};
    auto filt = [&](std::map<int, int> m) {
        if (exclude) { auto e = m.find(exclude); if (e != m.end() && --e->second <= 0) m.erase(e); }
        return m;
    };
    for (int pass = 0; pass < 2; ++pass) {
        const auto& tab = pass ? coarse_[f] : fine_[f];
        auto it = tab.find(pass ? coarse_key(l, w) : fine_key(l, w, h));
        if (it == tab.end()) continue;
        std::map<int, int> m = filt(it->second);
        if (m.empty()) continue;
        return majority(m);
    }
    // nothing of this exact size ran: nearest populated coarse bucket (within ~1.7x on each side)
    const long long ka = std::lround(std::log(std::max(l, 0.1)) * 4), kb = std::lround(std::log(std::max(w, 0.1)) * 4);
    double bestd = 1e9;
    Pick best;
    for (auto& kv : coarse_[f]) {
        long long a = (kv.first >> 12) - 100, b = (kv.first & 0xfff) - 100;
        double d = std::abs(a - ka) + std::abs(b - kb);
        if (d > 4 || d >= bestd) continue;
        std::map<int, int> m = filt(kv.second);
        if (m.empty()) continue;
        bestd = d;
        best = majority(m);
        best.share *= 0.7;   // borrowed from a neighbouring size
    }
    return best;
}

Pick History::by_extent(double el, double ew, int f) const {
    if (f < 0 || f > 2 || el <= 0) return {};
    auto it = ext_[f].find(coarse_key(std::max(el, ew), std::min(el, ew)));
    return it == ext_[f].end() ? Pick{} : majority(it->second);
}

// ---------------------------------------------------------------------------------------- text
std::string chip_code_in(const std::string& desc) {
    static const std::regex rx(R"((?:^|[^0-9.])(0201|0402|0603|0805|1206|1210|1812|2010|2512)(?![0-9.]))");
    std::smatch m;
    return std::regex_search(desc, m, rx) ? m[1].str() : std::string();
}
bool chip_dims(const std::string& c, double& l, double& w) {
    static const struct { const char* c; double l, w; } t[] = {{"0201", 0.6, 0.3}, {"0402", 1.0, 0.5}, {"0603", 1.6, 0.8}, {"0805", 2.0, 1.25},
                                                           {"1206", 3.2, 1.6}, {"1210", 3.2, 2.5}, {"1812", 4.5, 3.2}, {"2010", 5.0, 2.5},
                                                           {"2512", 6.4, 3.2}};
    for (auto& x : t) if (c == x.c) { l = x.l; w = x.w; return true; }
    return false;
}
bool is_passive(const std::string& desc) {
    static const std::regex rx(R"(\b(RES|RESISTOR|CAP|CAPACITOR|OHMS?|IND|INDUCTOR|FERRITE|BEAD)\b)", std::regex::icase);
    return std::regex_search(desc, rx);
}
static bool is_rc(const std::string& desc) {
    static const std::regex rx(R"(\b(RES|RESISTOR|CAP|CAPACITOR|OHMS?)\b)", std::regex::icase);
    return std::regex_search(desc, rx);
}

Proposal History::propose(const std::string& name, const std::string& desc, double bl, double bw, double bh, bool use_hist,
                          const BodyHint* hint) const {
    Proposal p;
    const PartSummary* s = use_hist ? find(name) : nullptr;
    std::string code = is_passive(desc) ? chip_code_in(upper(desc)) : std::string();
    if (s) {
        p.l = s->l; p.w = s->w; p.h = s->h; p.from_history = true;
        p.why = s->measured ? "body: machine-measured" : "body: program";
    } else if (double cl0, cw0, ch0; cad_body(name, cl0, cw0, ch0)) {
        p.l = cl0; p.w = cw0; p.h = ch0 > 0 ? ch0 : bh;
        p.why = "body: CAD pads (.cpf)";
    } else if (hint && hint->l > 0) {
        p.l = std::max(hint->l, hint->w); p.w = std::min(hint->l, hint->w); p.h = hint->h > 0 ? hint->h : bh;
        p.why = "body: " + hint->src;
    } else if (bl > 0) {
        double el = std::max(bl, bw), ew = std::min(bl, bw);
        auto r = ratio_med_.find(coarse_key(el, ew));
        double rl = r != ratio_med_.end() ? r->second.first : 0.8, rw = r != ratio_med_.end() ? r->second.second : 0.8;
        p.l = el * rl; p.w = ew * rw; p.h = bh;
        if (p.w > p.l) std::swap(p.l, p.w);
        p.why = r != ratio_med_.end() ? "body: land pattern x learned body ratio" : "body: land pattern x 0.8";
    }
    double cl = 0, cw = 0;
    if (!code.empty() && chip_dims(code, cl, cw)) {
        if (p.l <= 0) { p.l = cl; p.w = cw; p.h = bh > 0 ? bh : 0.5; p.why = "body: size " + code + " in description"; }
        else if (std::fabs(std::log(p.l / cl)) > 0.25) { p.l = cl; p.w = cw; p.why += "; body: description size " + code + " overrides"; }
    }
    int* dst[3] = {&p.tape, &p.pitch, &p.nozzle};
    double* conf[3] = {&p.conf_tape, &p.conf_pitch, &p.conf_nozzle};
    const char* nm[3] = {"tape", "pitch", "nozzle"};
    for (int f = 0; f < 3; ++f) {
        const Pick* hp = s ? (f == 0 ? &s->tape : f == 1 ? &s->pitch : &s->nozzle) : nullptr;
        if (hp && hp->value) { *dst[f] = hp->value; *conf[f] = hp->share; p.why += std::string("; ") + nm[f] + ": history (" + std::to_string(s->programs) + " programs)"; }
        else if (Pick e = by_extent(bl, bw, f); e.value && e.votes >= 2 && !p.from_history && code.empty() && p.why.find("CAD pads") == std::string::npos) {
            *dst[f] = e.value; *conf[f] = e.share; p.why += std::string("; ") + nm[f] + ": land-pattern table";
        } else if (p.l > 0) {
            Pick b = by_size(p.l, p.w, p.h, f);
            if (b.value) { *dst[f] = b.value; *conf[f] = b.share; p.why += std::string("; ") + nm[f] + ": size table"; }
        }
    }
    // small R/C chips run on 8 mm paper at 2 / 4 mm pitch unless they are thick
    if ((code == "0201" || code == "0402" || code == "0603" || code == "0805") && p.h <= 0.95 && is_rc(desc)) {
        if (p.tape != 1) { p.tape = 1; p.why += "; tape: small R/C chip -> 8mm paper"; }
        const int want = (code == "0201" || code == "0402") ? 2 : 4;    // 8 mm paper: 2 mm pockets up to 0402, 4 mm above
        if (p.pitch != want) { p.pitch = want; p.why += "; pitch: " + code + " on paper -> " + std::to_string(want) + " mm"; }
    }
    // nothing voted: carrier from the body (pocket = outer bounds), JUKI nozzle ladder by short edge
    if (!p.tape && p.l > 0) {
        const double L = p.l;
        p.tape = (p.h <= 0.95 && L <= 2.2 && is_rc(desc)) ? 1 : L <= 4.2 ? 2 : L <= 8.5 ? 3 : L <= 12.5 ? 4 : L <= 20 ? 5 : L <= 28 ? 7 : 8;
        p.conf_tape = 0.4;
        p.why += "; tape: from body size";
    }
    if (!p.nozzle && p.w > 0) {
        static const struct { double max; int nz; } lad[] = {{0.45, 501}, {0.9, 500}, {1.4, 503}, {2.2, 504}, {3.6, 505}, {6.5, 506}, {10, 507}, {1e9, 508}};
        const std::set<int>& own = owned_nozzles();
        for (size_t i = 0; i < sizeof lad / sizeof lad[0] && !p.nozzle; ++i)
            if (p.w <= lad[i].max)
                for (size_t j = i; j < sizeof lad / sizeof lad[0]; ++j)   // smallest owned nozzle that can carry it
                    if (own.empty() || own.count(lad[j].nz)) { p.nozzle = lad[j].nz; break; }
        if (!p.nozzle && !own.empty()) p.nozzle = *own.rbegin();   // nothing big enough owned: the largest one
        p.conf_nozzle = 0.4;
        p.why += "; nozzle: from body short edge";
    }
    // nothing measured the height: plausible value so the profile pick and the DB row are complete
    if (p.l > 0 && p.h <= 0) { p.h = fake_height(p.l, p.w, is_passive(desc)); p.why += "; height: estimated " + std::to_string(p.h).substr(0, 4) + " mm"; }
    if (p.tape && !p.pitch) {
        if (p.tape == 1) p.pitch = p.w < 0.6 ? 2 : 4;
        else { const double L = std::max(p.l, p.w); p.pitch = L <= 3.2 ? 4 : L <= 6.5 ? 8 : L <= 10.5 ? 12 : L <= 14 ? 16 : 24; }
    }
    if (!p.why.empty() && p.why[0] == ';') p.why.erase(0, 2);
    return p;
}

// --------------------------------------------------------------------------------- package text
std::string norm_code(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = (char)std::toupper((unsigned char)s[i]);
        if ((c == '-' || c == '_') && i > 0 && i + 1 < s.size() && std::isalnum((unsigned char)s[i - 1]) && std::isalnum((unsigned char)s[i + 1])) continue;
        o += c;
    }
    return o;
}

double fake_height(double l, double w, bool passive) {
    if (passive && l <= 6.5) return std::max(0.3, std::min(2.5, w * 0.9));
    if (l <= 2.2) return 0.6;
    if (l <= 3.5) return 1.0;
    if (l <= 8) return 1.4;
    return 2.0;
}

namespace {
struct CodeRow { const char* code; std::vector<const char*> aliases; double l, w, h; int pins; };
const std::vector<CodeRow>& code_table() {
    static const std::vector<CodeRow> t = {
        {"SOD923", {"SOD923"}, 1.0, 0.6, 0.4, 2},
        {"SOD723", {"SOD723"}, 1.0, 0.6, 0.5, 2},
        {"SOD523", {"SOD523", "SC79"}, 1.6, 0.8, 0.6, 2},
        {"SOD323", {"SOD323F", "SOD323", "SC76", "SC90"}, 1.7, 1.25, 0.95, 2},
        {"SOD123", {"SOD123FL", "SOD123F", "SOD123"}, 2.7, 1.6, 1.1, 2},
        {"SOD80", {"MINIMELF", "SOD80C", "SOD80", "LL34", "DO213AA"}, 3.5, 1.5, 1.5, 2},
        {"SMA", {"DO214AC", "SMAJ", "SMA"}, 4.3, 2.6, 2.2, 2},
        {"SMB", {"DO214AA", "SMBJ", "SMB"}, 4.4, 3.6, 2.3, 2},
        {"SMC", {"DO214AB", "SMCJ", "SMC"}, 6.9, 5.9, 2.3, 2},
        {"SOT23-5", {"SOT235", "SOT23-5", "SOT25", "SOT753", "SC74A", "TSOT235", "TSOT23-5"}, 2.9, 1.6, 1.1, 5},
        {"SOT23-6", {"SOT236", "SOT23-6", "SOT26", "SOT457", "SC74", "TSOT236", "TSOT23-6"}, 2.9, 1.6, 1.1, 6},
        {"SOT23-8", {"SOT238", "SOT23-8", "TSOT238", "TSOT23-8"}, 2.9, 1.6, 1.1, 8},
        {"SOT23", {"SOT233", "SOT23", "TO236AB", "TO236", "SC59"}, 2.9, 1.3, 1.0, 3},
        {"SOT143", {"SOT143", "TO253"}, 2.9, 1.3, 1.0, 4},
        {"SOT363", {"SOT363", "SC88", "SC706", "SC70-6"}, 2.0, 1.25, 0.95, 6},
        {"SOT353", {"SOT353", "SC88A", "SC705", "SC70-5"}, 2.0, 1.25, 0.95, 5},
        {"SOT323", {"SOT323", "SC70"}, 2.0, 1.25, 0.95, 3},
        {"SOT523", {"SOT523", "SC75A", "SC75", "SC89"}, 1.6, 0.85, 0.75, 3},
        {"SOT563", {"SOT563", "SOT666", "SC89-6"}, 1.6, 1.2, 0.55, 6},
        {"SOT89", {"SOT89", "SC62", "TO243"}, 4.5, 2.5, 1.5, 3},
        {"SOT223", {"SOT223", "SC73", "TO261"}, 6.5, 3.5, 1.6, 4},
        {"DPAK", {"DPAK", "TO252"}, 6.6, 6.1, 2.3, 3},
        {"D2PAK", {"D2PAK", "TO263"}, 10.2, 9.2, 4.5, 3},
    };
    return t;
}
bool code_at(const std::string& u, const std::string& a, size_t k) {
    if (k > 0 && std::isalnum((unsigned char)u[k - 1])) return false;
    size_t e = k + a.size();
    if (e >= u.size()) return true;
    char n = u[e];
    if (std::isdigit((unsigned char)n)) return false;
    if (std::isalpha((unsigned char)n)) {
        // SOD123F / SMAJ5.0 style suffixes only
        if (n == 'F' && (a.rfind("SOD", 0) == 0 || a.rfind("SOT", 0) == 0)) return true;
        if ((n == 'J' || n == 'G') && a.size() == 3 && a.rfind("SM", 0) == 0) return e + 1 >= u.size() || !std::isalpha((unsigned char)u[e + 1]);
        return false;
    }
    return true;
}
}  // namespace

bool package_code(const std::string& text, PkgBody& out) {
    const std::string u = norm_code(text);
    size_t best_k = std::string::npos;
    const CodeRow* best = nullptr;
    size_t best_len = 0;
    for (auto& r : code_table())
        for (const char* a0 : r.aliases) {
            std::string a = norm_code(a0);
            for (size_t k = u.find(a); k != std::string::npos; k = u.find(a, k + 1))
                if (code_at(u, a, k) && (a.size() > best_len || (a.size() == best_len && k < best_k))) { best = &r; best_k = k; best_len = a.size(); }
        }
    if (!best) return false;
    out = PkgBody{};
    out.code = best->code;
    for (const char* a : best->aliases) out.aliases.push_back(norm_code(a));
    out.l = best->l; out.w = best->w; out.h = best->h; out.pins = best->pins;
    out.src = std::string("package ") + best->code;
    return true;
}

bool ipc_body(const std::string& name, PkgBody& out) {
    const std::string u = upper(trim(name));
    std::smatch m;
    auto d = [](const std::string& s) { return std::atof(s.c_str()); };
    // chips / molded bodies: CAPC1005X56N, RESC2012X65, INDM5552X300N, CAPC1005N
    static const std::regex chip(R"(^(CAPC|RESC|INDC|LEDC|DIOC|FUSC|BEADC|CAPM|RESM|INDM|DIOM|LEDM|CAPMP|CAPT|CAPAE|OSCC)(\d{4}|\d{6}|\d{8})(?:X(\d{2,4}))?)");
    if (std::regex_search(u, m, chip)) {
        std::string D = m[2].str();
        size_t half = D.size() / 2;
        double div = D.size() == 4 ? 10 : 100;
        out = PkgBody{};
        out.l = d(D.substr(0, half)) / div; out.w = d(D.substr(half)) / div;
        out.h = m[3].matched ? d(m[3].str()) / 100 : 0;
        if (out.w > out.l) std::swap(out.l, out.w);
        out.pins = 2;
        out.src = "IPC name " + m[0].str();
        return out.l > 0 && out.w > 0;
    }
    // SOD: lead span + body width x height (SOD3717X135N = SOD123)
    static const std::regex sod(R"(^SOD(\d{2})(\d{2})X(\d{2,3}))");
    if (std::regex_search(u, m, sod)) {
        out = PkgBody{};
        double span = d(m[1].str()) / 10, bw = d(m[2].str()) / 10;
        out.l = span * 0.75; out.w = bw; out.h = d(m[3].str()) / 100;
        if (out.w > out.l) std::swap(out.l, out.w);
        out.pins = 2;
        out.src = "IPC name " + m[0].str();
        return true;
    }
    // gull-wing / no-lead: SOT95P280X145-5N, SOIC127P600X175-8N, QFN50P400X400X100-21N, LQFP50P900X900X160-64N
    static const std::regex lead(R"(^(SOT|SOIC|SOP|SSOP|TSSOP|MSOP|SOJ|SON|DFN|QFN|VQFN|WQFN|PQFN|QFP|LQFP|TQFP|PQFP|SOTFL)(\d+)P(\d+)X(\d+)(?:X(\d+))?-(\d+))");
    if (std::regex_search(u, m, lead)) {
        out = PkgBody{};
        const std::string fam = m[1].str();
        double pitch = d(m[2].str()) / 100, a = d(m[3].str()) / 100, b = d(m[4].str()) / 100;
        int pins = std::atoi(m[6].str().c_str());
        out.pins = pins;
        out.src = "IPC name " + m[0].str();
        const bool quad = fam.find("QF") != std::string::npos;
        if (m[5].matched) {                     // L x W x H given
            out.l = a; out.w = b; out.h = d(m[5].str()) / 100;
            if (quad && fam.find("QFN") == std::string::npos) { out.l = std::max(0.5, a - 2.0); out.w = std::max(0.5, b - 2.0); }   // QFP: span -> body
        } else {                                // lead span x height
            out.h = b;
            const bool sot = fam.rfind("SOT", 0) == 0;
            int pos = sot && pins <= 6 ? 3 : (pins + 1) / 2;
            out.l = (pos - 1) * pitch + (sot ? 1.0 : 1.1);
            out.w = sot ? a * 0.55 : fam.rfind("SO", 0) == 0 || fam.find("SOP") != std::string::npos ? a * 0.62 : a * 0.6;
        }
        if (out.w > out.l) std::swap(out.l, out.w);
        if (fam == "SOT" && !m[5].matched) {    // name the common SOT outlines so the library profile can be found by name
            const int pi = (int)std::lround(pitch * 100);
            std::string code;
            if (pi == 95) code = pins <= 3 ? "SOT23" : pins == 5 ? "SOT23-5" : pins == 6 ? "SOT23-6" : pins == 8 ? "SOT23-8" : "";
            else if (pi == 65) code = pins <= 3 ? "SOT323" : pins == 5 ? "SOT353" : pins == 6 ? "SOT363" : "";
            if (!code.empty()) { PkgBody c; if (package_code(code, c)) { out.code = c.code; out.aliases = c.aliases; } }
        }
        return out.l > 0 && out.w > 0;
    }
    return false;
}

}  // namespace pcbp::history
