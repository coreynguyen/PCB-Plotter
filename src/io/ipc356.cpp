// ============================================================================
// ipc356.cpp — IPC-D-356(A) netlist import. See ipc356.h.
// ============================================================================
#include "ipc356.h"
#include "io_util.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <sstream>
#include <cstdlib>
#include <iterator>

namespace pcbp::io {

namespace {

std::string col(const std::string& s, size_t a1, size_t b1) {   // 1-based inclusive
    if (s.size() < a1) return "";
    return trim(s.substr(a1 - 1, b1 - a1 + 1));
}

// read [+-]digits at p; returns chars consumed
size_t read_int(const std::string& s, size_t p, long& v, bool allow_sign) {
    size_t i = p;
    bool neg = false;
    if (allow_sign && i < s.size() && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; ++i; }
    while (i < s.size() && s[i] == ' ') ++i;            // some writers pad after the sign
    size_t d0 = i;
    long r = 0;
    while (i < s.size() && std::isdigit((unsigned char)s[i])) { r = r * 10 + (s[i] - '0'); ++i; }
    if (i == d0) return 0;
    v = neg ? -r : r;
    return i - p;
}

bool is_pin1(const std::string& p) { return p == "1" || ieq(p, "A1") || p == "01" || p == "001" || p == "0001"; }

} // namespace

bool parse_ipc356_text(const std::string& text, const Ipc356Options& opt, Ipc356Result& res, std::string& err) {
    res = Ipc356Result{};
    double k = 0.0001 * kMmPerInch;     // default CUST 0
    std::map<std::string, Ipc356Net> nets;
    std::vector<std::string> net_order;
    std::map<std::string, std::string> net_alias;    // 356A "P  NNAME1  long name" aliases
    bool any = false;
    for (auto& raw : split_lines(text)) {
        if (raw.empty()) continue;
        if (raw[0] == 'C') continue;
        if (raw[0] == 'P') {
            auto t = split_ws_quoted(raw.substr(1));
            if (t.empty()) continue;
            std::string key = to_upper(t[0]);
            if (key == "UNITS") {
                std::string u = t.size() > 1 ? to_upper(t[1]) : "";
                std::string n = t.size() > 2 ? t[2] : "0";
                res.units = u + (t.size() > 2 ? " " + n : "");
                if (u == "SI") k = 0.001;
                else if (u == "CUST" && n == "1") k = 0.001;
                else k = 0.0001 * kMmPerInch;
            } else if (key == "JOB") {
                res.job = t.size() > 1 ? t[1] : "";
            } else if (key.rfind("NNAME", 0) == 0 && t.size() > 1) {
                std::string full;
                for (size_t i = 1; i < t.size(); ++i) full += (i > 1 ? " " : "") + t[i];
                net_alias[t[0]] = full;
            }
            continue;
        }
        if (raw.size() >= 3 && raw.compare(0, 3, "999") == 0) break;
        if (raw.size() < 32) { ++res.skipped; continue; }
        const std::string rt = raw.substr(0, 3);
        if (!((rt[0] == '3' || rt[0] == '0') && (rt[1] == '1' || rt[1] == '2') && rt[2] == '7')) {
            ++res.skipped;      // 367 tooling, 378 conductor, 389 outline, etc.
            continue;
        }
        any = true;
        ++res.records;
        Ipc356Pin p;
        p.smt = rt[1] == '2';
        p.net = col(raw, 4, 17);
        if (auto it = net_alias.find(p.net); it != net_alias.end()) p.net = it->second;
        p.ref = col(raw, 21, 26);
        p.pin = col(raw, 28, 31);
        p.midpoint = raw.size() >= 32 && raw[31] == 'M';
        // tolerant scan of the remainder
        size_t i = 32;
        int xy_seen = 0;
        while (i < raw.size()) {
            char c = raw[i];
            long v = 0;
            size_t n;
            if (c == 'D' && (n = read_int(raw, i + 1, v, false))) {
                p.drill = v * k;
                i += 1 + n;
                if (i < raw.size() && (raw[i] == 'P' || raw[i] == 'U')) { p.plated = raw[i] == 'P'; ++i; }
            } else if (c == 'A' && (n = read_int(raw, i + 1, v, false))) {
                p.access = (int)v; i += 1 + n;
            } else if (c == 'X' && xy_seen == 0 && (n = read_int(raw, i + 1, v, true))) {
                p.pos.x = v * k; i += 1 + n;
                if (i < raw.size() && raw[i] == 'Y' && (n = read_int(raw, i + 1, v, true))) { p.pos.y = v * k; i += 1 + n; }
                xy_seen = 1;
            } else if (c == 'Y' && xy_seen == 0 && (n = read_int(raw, i + 1, v, true))) {
                p.pos.y = v * k; i += 1 + n; xy_seen = 1;
            } else if (c == 'X' && xy_seen == 1 && (n = read_int(raw, i + 1, v, false))) {
                p.w = v * k; i += 1 + n; xy_seen = 2;
                if (i < raw.size() && raw[i] == 'Y' && (n = read_int(raw, i + 1, v, false))) { p.h = v * k; i += 1 + n; }
            } else if (c == 'Y' && xy_seen >= 1 && (n = read_int(raw, i + 1, v, false))) {
                p.h = v * k; i += 1 + n; xy_seen = 2;
            } else if (c == 'R' && (n = read_int(raw, i + 1, v, false))) {
                p.rot = (double)v; i += 1 + n;
            } else if (c == 'S' && (n = read_int(raw, i + 1, v, false))) {
                i += 1 + n;
            } else ++i;
        }
        if (p.w <= 0 && p.h <= 0) {
            double d = p.drill > 0 ? p.drill * 1.6 : opt.default_pad_mm;
            p.w = p.h = d;
        } else if (p.h <= 0) p.h = p.w;
        else if (p.w <= 0) p.w = p.h;
        if (p.ref.empty() || ieq(p.ref, "VIA")) {
            ++res.vias;
            if (!opt.include_vias) { if (!p.net.empty()) { /* still a net node */ } continue; }
            p.ref = "VIA";
        }
        ++res.pins;
        if (!p.net.empty() && !ieq(p.net, "N/C")) {
            if (!nets.count(p.net)) { net_order.push_back(p.net); nets[p.net].name = p.net; }
            nets[p.net].nodes.push_back(p.ref + "-" + p.pin);
        }
        res.pin_list.push_back(p);
    }
    if (!any) { err = "no IPC-D-356 feature records (317/327/017/027) found"; return false; }
    for (auto& n : net_order) res.nets.push_back(nets[n]);
    return true;
}

bool import_ipc356_text(const std::string& text, Project& prj, const Ipc356Options& opt,
                        Ipc356Result& res, std::string& err) {
    if (!parse_ipc356_text(text, opt, res, err)) return false;
    if (!opt.create_components) return true;
    if (opt.replace) {
        prj.comps.clear();
        for (auto it = prj.packages.begin(); it != prj.packages.end();)
            it = it->first.rfind("IPC356_", 0) == 0 ? prj.packages.erase(it) : std::next(it);
    }
    // group pins by refdes (first-seen order)
    std::vector<std::string> order;
    std::map<std::string, std::vector<const Ipc356Pin*>> groups;
    for (auto& p : res.pin_list) {
        std::string key = to_upper(p.ref);
        if (!groups.count(key)) order.push_back(key);
        groups[key].push_back(&p);
    }
    std::map<std::string, std::string> sig2pkg;
    int pkg_seq = 0;
    for (auto& key : order) {
        auto& g = groups[key];
        // side: any TH pin -> top (TH comps are top-mounted by default);
        // all-SMT with access >= 2 -> bottom
        bool th = false, all_bottom = true;
        for (auto* p : g) {
            if (!p->smt || p->drill > 0) th = true;
            if (p->access < 2) all_bottom = false;
        }
        Side side = (!th && all_bottom) ? Side::Bottom : Side::Top;
        BBox bb;
        for (auto* p : g) {
            const double r = deg2rad(p->rot), cs = std::fabs(std::cos(r)), sn = std::fabs(std::sin(r));
            Vec2 hs{p->w * 0.5 * cs + p->h * 0.5 * sn, p->w * 0.5 * sn + p->h * 0.5 * cs};
            bb.add(p->pos - hs);
            bb.add(p->pos + hs);
        }
        Vec2 c = bb.center();
        Package pk;
        pk.smt = !th;
        int pin1 = -1, ord = 0;
        // order pads by pin name naturally
        std::vector<const Ipc356Pin*> sorted = g;
        std::stable_sort(sorted.begin(), sorted.end(), [](auto* a, auto* b) { return ref_less(a->pin, b->pin); });
        for (auto* p : sorted) {
            Pad pd;
            Vec2 rel = p->pos - c;
            if (side == Side::Bottom) rel.x = -rel.x;     // comp_xform mirrors bottom footprints
            pd.pos = {std::round(rel.x * 1e4) / 1e4, std::round(rel.y * 1e4) / 1e4};
            pd.w = p->w; pd.h = p->h;
            pd.rot = norm_deg(side == Side::Bottom ? -p->rot : p->rot);
            pd.drill = p->drill;
            pd.shape = p->drill > 0 && std::fabs(p->w - p->h) < 1e-6 ? PadShape::Round : PadShape::Rect;
            ++ord;
            bool numeric = !p->pin.empty() && std::all_of(p->pin.begin(), p->pin.end(), [](char ch) { return std::isdigit((unsigned char)ch); });
            pd.pin = numeric ? std::atoi(p->pin.c_str()) : ord;
            if (pin1 < 0 && is_pin1(p->pin)) pin1 = (int)pk.pads.size();
            pk.pads.push_back(pd);
        }
        pk.leads = (int)pk.pads.size();
        pk.auto_body();
        // signature for dedupe
        std::ostringstream sig;
        sig << pk.smt;
        for (auto& pd : pk.pads)
            sig << "|" << pd.pin << ":" << fmt_fixed(pd.pos.x, 3) << "," << fmt_fixed(pd.pos.y, 3) << ","
                << fmt_fixed(pd.w, 3) << "," << fmt_fixed(pd.h, 3) << "," << fmt_fixed(pd.rot, 1) << "," << fmt_fixed(pd.drill, 3);
        std::string name;
        if (opt.dedupe_packages && sig2pkg.count(sig.str())) name = sig2pkg[sig.str()];
        else {
            name = "IPC356_" + std::to_string(pk.pads.size()) + "P_" + std::to_string(++pkg_seq);
            pk.name = name;
            pk.description = "Built from IPC-D-356 pads of " + g[0]->ref;
            prj.packages[name] = pk;
            sig2pkg[sig.str()] = name;
            ++res.packages;
        }
        Component cm;
        cm.ref = g[0]->ref;
        cm.pos = c;
        cm.rot = 0;
        cm.side = side;
        cm.package = name;
        if (pin1 >= 0) { cm.pin1_set = true; cm.pin1_index = pin1; }
        cm.cad_pos = cm.pos;
        prj.add_component(cm);
        ++res.components;
    }
    prj.dirty = true;
    return true;
}

bool import_ipc356_file(const std::string& path, Project& prj, const Ipc356Options& opt,
                        Ipc356Result& res, std::string& err) {
    std::string text;
    if (!read_file(path, text, err)) return false;
    return import_ipc356_text(text, prj, opt, res, err);
}

std::string Ipc356Result::describe() const {
    std::ostringstream s;
    s << "IPC-D-356 job \"" << job << "\" units " << (units.empty() ? "CUST 0 (default)" : units)
      << ": " << records << " feature records, " << pins << " component pins, " << vias << " vias, "
      << skipped << " other records skipped; " << components << " components, " << packages
      << " packages, " << nets.size() << " nets";
    for (auto& w : warnings) s << "\nwarning: " << w;
    return s.str();
}

} // namespace pcbp::io
