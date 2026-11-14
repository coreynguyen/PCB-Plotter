// program_view.cpp — see program_view.h
#include "program_view.h"
#include "juki_import.h"
#include "core/project.h"
#include <algorithm>

namespace pcbp::io::progview {
namespace {
// tiny tag scanner (the feeder list is flat and attribute based)
size_t find_open(const std::string& s, const std::string& tag, size_t from, size_t to) {
    for (size_t p = s.find("<" + tag, from); p != std::string::npos && p < to; p = s.find("<" + tag, p + 1)) {
        const char c = p + 1 + tag.size() < s.size() ? s[p + 1 + tag.size()] : '>';
        if (c == ' ' || c == '>' || c == '/' || c == '\t' || c == '\r' || c == '\n') return p;
    }
    return std::string::npos;
}
size_t find_close(const std::string& s, const std::string& tag, size_t open, size_t to) {
    const size_t gt = s.find('>', open);
    if (gt == std::string::npos) return to;
    if (gt > 0 && s[gt - 1] == '/') return gt + 1;
    const size_t e = s.find("</" + tag + ">", gt);
    return e == std::string::npos || e > to ? to : e + tag.size() + 3;
}
std::string attr(const std::string& s, size_t open, const std::string& name) {
    const size_t gt = s.find('>', open);
    const std::string head = s.substr(open, gt == std::string::npos ? std::string::npos : gt - open);
    for (size_t p = head.find(name + "="); p != std::string::npos; p = head.find(name + "=", p + 1)) {
        if (p > 0 && head[p - 1] != ' ' && head[p - 1] != '\t' && head[p - 1] != '\n') continue;
        const size_t q = p + name.size() + 1;
        if (q >= head.size()) break;
        const char quote = head[q];
        const size_t e = head.find(quote, q + 1);
        if (e == std::string::npos) break;
        return head.substr(q + 1, e - q - 1);
    }
    return {};
}
std::string text_of(const std::string& s, const std::string& tag, size_t from, size_t to) {
    const size_t o = find_open(s, tag, from, to);
    if (o == std::string::npos) return {};
    const size_t gt = s.find('>', o);
    if (gt == std::string::npos || s[gt - 1] == '/') return {};
    const size_t e = s.find("</" + tag + ">", gt);
    if (e == std::string::npos || e > to) return {};
    std::string v = s.substr(gt + 1, e - gt - 1);
    // minimal entity decode
    for (auto [a, b] : {std::pair<const char*, const char*>{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}})
        for (size_t p = v.find(a); p != std::string::npos; p = v.find(a, p + 1)) v.replace(p, std::char_traits<char>::length(a), b);
    return v;
}
} // namespace

std::map<std::string, std::string> iss_feeders(const std::string& x) {
    std::map<std::string, std::string> m;
    for (size_t fp = find_open(x, "feederPosition", 0, x.size()); fp != std::string::npos; fp = find_open(x, "feederPosition", fp + 1, x.size())) {
        const size_t fe = find_close(x, "feederPosition", fp, x.size());
        for (size_t f = find_open(x, "feeder", fp + 1, fe); f != std::string::npos && f < fe;) {
            const size_t e = find_close(x, "feeder", f, fe);
            const std::string cn = text_of(x, "componentName", f, e);
            std::string lab;
            const size_t po = find_open(x, "position", f, e);
            if (po != std::string::npos) {
                const std::string st = attr(x, po, "stationId"), bk = attr(x, po, "bankPos"), ho = attr(x, po, "holeNo");
                if (!st.empty()) lab += "St" + st;
                if (!bk.empty()) lab += (lab.empty() ? "" : " ") + std::string("Bank") + bk;
                if (!ho.empty()) lab += (lab.empty() ? "" : " ") + std::string("#") + ho;
            }
            if (lab.empty()) { const std::string ix = attr(x, f, "index"); if (!ix.empty()) lab = "F" + ix; }
            if (!cn.empty() && !lab.empty()) { auto& v = m[cn]; v += (v.empty() ? "" : ", ") + lab; }
            f = find_open(x, "feeder", e, fe);
        }
        fp = fe - 1;
    }
    return m;
}

bool load(const std::vector<uint8_t>& bytes, Program& out, std::string& err) {
    out = Program{};
    Project prj;
    juki::ImportResult res;
    if (!juki::import_juki(bytes, prj, {}, res, err)) return false;
    out.format = res.format;
    out.name = prj.info.number.empty() ? prj.info.name : prj.info.number;
    out.warnings = res.warnings;
    std::map<std::string, std::string> feeders;
    if (res.format == "ISS") {
        size_t i = 0; while (i < bytes.size() && bytes[i] != '<') ++i;
        feeders = iss_feeders(std::string(bytes.begin() + (long)i, bytes.end()));
    }
    const BBox bb = prj.board_bbox();
    if (!bb.empty()) { out.x0 = bb.x0; out.y0 = bb.y0; out.x1 = bb.x1; out.y1 = bb.y1; }
    for (const auto& c : prj.comps) {
        if (c.feature == Feature::Fiducial) { out.fids.push_back({c.ref, c.pos.x, c.pos.y, 0}); continue; }
        if (c.feature != Feature::Component) continue;
        Place p;
        p.ref = c.ref; p.part = c.number; p.package = c.package;
        p.x = c.pos.x; p.y = c.pos.y; p.rot = c.rot; p.skip = c.dnp;
        auto it = feeders.find(c.number);
        if (it != feeders.end()) p.feeder = it->second;
        out.places.push_back(std::move(p));
    }
    // step & repeat images: show every circuit's copy (offsets from the panel images)
    if (prj.images.size() > 1) {
        const size_t n = out.places.size();
        for (size_t im = 1; im < prj.images.size(); ++im)
            for (size_t i = 0; i < n; ++i) {
                Place q = out.places[i];
                q.x += prj.images[im].offset.x; q.y += prj.images[im].offset.y; q.image = (int)im;
                out.places.push_back(q);
            }
    }
    return true;
}

} // namespace pcbp::io::progview
