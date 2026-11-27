#include "juki_record.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

// The research of one part from its rows, in the editor's database-read order (shared by PCB Plotter and Juki Tool;
// no database, file or UI dependencies).
namespace pcbp::juki {

namespace {
std::string up(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }
}  // namespace

bool ResearchRecord::has_vision() const { return std::any_of(b7.begin(), b7.end(), [](uint8_t b) { return b != 0; }); }

// ------------------------------------------------------------------------------------------------ PartRows
void PartRows::set(const std::string& t, const std::string& c, double v) { num[up(t)][up(c)] = v; }
void PartRows::set_null(const std::string& t, const std::string& c) { num[up(t)][up(c)] = std::nan(""); }
void PartRows::set_text(const std::string& t, const std::string& c, const std::string& v) { text[up(t)][up(c)] = v; }
bool PartRows::has_table(const std::string& t) const { return num.count(up(t)) || text.count(up(t)); }
bool PartRows::get(const std::string& t, const std::string& c, double& v, bool& null) const {
    auto a = num.find(up(t));
    if (a == num.end()) return false;
    auto b = a->second.find(up(c));
    if (b == a->second.end()) return false;
    null = std::isnan(b->second);
    v = null ? 0 : b->second;
    return true;
}
bool PartRows::get_text(const std::string& t, const std::string& c, std::string& v) const {
    auto a = text.find(up(t));
    if (a == text.end()) return false;
    auto b = a->second.find(up(c));
    if (b == a->second.end()) return false;
    v = b->second;
    return true;
}


void research_set_index(ResearchRecord& r, int index) {
    auto put16 = [](std::vector<uint8_t>& v, size_t o, int x) { if (v.size() >= o + 2) { v[o] = (uint8_t)x; v[o + 1] = (uint8_t)(x >> 8); } };
    if (!r.has_vision()) { put16(r.b4, 338, 0); return; }
    put16(r.b4, 338, index);
    for (size_t o : {291u, 391u, 491u, 591u})
        if (r.b7.size() >= o + 2) put16(r.b7, o, index);
}

namespace {
uint16_t g16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
void s16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

// The editor's merge of the read's element lists into the program, here into lists of their own starting at 0 (the
// writer adds the program offsets): groups only when the part uses element groups (+4), arrays only when it uses
// extended arrays (+5); records renumbered in list order, each owner's elements right after it.
void merge_elements(const std::vector<uint8_t>& h, ResearchRecord& r) {
    std::vector<uint8_t> groups, arrays, elems;
    r.b19 = h;
    auto take = [&](const std::vector<uint8_t>& src, size_t rs, int use_at, int first_at, std::vector<uint8_t>& dst) {
        s16(&r.b19[(size_t)first_at], 0xFFFF);
        s16(&r.b19[(size_t)first_at + 2], 0);
        if (!h[(size_t)use_at]) return;
        uint16_t idx = g16(&h[(size_t)first_at]), prev = 0xFFFF;
        const size_t n = src.size() / rs;
        while (idx != 0xFFFF && idx < n) {
            const uint16_t at = (uint16_t)(dst.size() / rs);
            dst.insert(dst.end(), src.begin() + (std::ptrdiff_t)(rs * idx), src.begin() + (std::ptrdiff_t)(rs * (idx + 1)));
            uint8_t* o = &dst[rs * at];
            s16(o + 8, prev);
            o[1] = 1;
            if (g16(o + 14)) {
                uint16_t e = g16(o + 12), ep = 0xFFFF, cnt = 0;
                const uint16_t first = (uint16_t)(elems.size() / 112);
                while (e != 0xFFFF && (size_t)e < r.elements.size() / 112) {
                    const uint16_t en = (uint16_t)(elems.size() / 112);
                    elems.insert(elems.end(), r.elements.begin() + 112 * e, r.elements.begin() + 112 * (e + 1));
                    uint8_t* q = &elems[(size_t)en * 112];
                    const uint16_t next = g16(q + 14);
                    s16(q + 12, ep);
                    q[1] = 1;
                    s16(q + 14, 0xFFFF);
                    if (ep != 0xFFFF) s16(&elems[(size_t)ep * 112 + 14], en);
                    ep = en;
                    ++cnt;
                    e = next;
                }
                o = &dst[rs * at];
                s16(o + 12, first);
                s16(o + 14, cnt);
            }
            const uint16_t next = g16(&src[rs * idx + 10]);
            s16(o + 10, 0xFFFF);
            if (prev != 0xFFFF) s16(&dst[rs * prev + 10], at);
            else s16(&r.b19[(size_t)first_at], at);
            s16(&r.b19[(size_t)first_at + 2], (uint16_t)(g16(&r.b19[(size_t)first_at + 2]) + 1));
            prev = at;
            idx = next;
        }
    };
    take(r.groups, 170, 4, 20, groups);
    take(r.arrays, 3142, 5, 24, arrays);
    r.groups = std::move(groups);
    r.arrays = std::move(arrays);
    r.elements = std::move(elems);
    r.b19[4] = r.b19[4] && !r.groups.empty() ? 1 : 0;
    r.b19[5] = r.b19[5] && !r.arrays.empty() ? 1 : 0;
}
}  // namespace

bool research_with(const RecordSource& s, ResearchResult& out) {
    out = ResearchResult();
    const rules::Get& get = s.get;
    const rules::GetReal& real = s.real;
    const rules::GetText& text = s.text;
    const rules::Lookup& lookup = s.lookup;
    const rules::LookupText& ltext = s.ltext;
    const rules::RowCount& count = s.count;
    const rules::GetRow& row = s.row;
    // the database read starts from an empty record; every byte comes from the part's own rows, in the read's order
    ResearchRecord& r = out.rec;
    r.b4.assign(1822, 0);
    uint8_t* b = r.b4.data();
    rules::compo(b, get, text);
    rules::feeder(b, get);
    const rules::SubCompo* acc[3] = {s.accel ? &s.accel[0] : nullptr, s.accel ? &s.accel[1] : nullptr, s.accel ? &s.accel[2] : nullptr};
    rules::expansion(b, get, lookup, acc);
    if (b[226]) rules::mech(b, get);
    if (b[246]) rules::vision(b, get);
    if (b[392]) rules::laser(b, get);
    rules::machine_tables(b, get);
    rules::inspection(b, get, real);
    // a tray part's tray height is at least the part height
    if (b[80] == 3) {
        int32_t th, h;
        std::memcpy(&th, b + 198, 4);
        std::memcpy(&h, b + 89, 4);
        if (th < h) std::memcpy(b + 198, &h, 4);
    }
    // vision control (block 7), recognition sets and lights (block 18), BGA pattern (block 16)
    if (b[246] == 1) {
        r.b7.assign(1052, 0);
        r.b18.assign(5240, 0);
        const long pattern = rules::vision_control(r.b7.data(), b, get, lookup);
        rules::vision_sets(r.b7.data(), r.b18.data(), get, count, row);
        rules::bga(r.b7.data(), r.bga, b, pattern, lookup, ltext);
        // a BGA read as a fine-pitch BGA (contrast 1 / 4)
        if ((b[78] | b[79] << 8) == 107 && r.b7[0] && (r.b7[1] == 1 || r.b7[1] == 4)) { b[78] = 108; b[79] = 0; }
        rules::vision_finish(b, r.b7.data(), r.b18.data());
    }
    rules::check(b);
    if (b[246] == 1) rules::vision_state(b, r.b7.data(), false);
    // the editor writes a fixed record into block 19 for every part; element-group parts carry their element header
    r.b19.assign(36, 0);
    r.b19[20] = r.b19[21] = r.b19[24] = r.b19[25] = 0xFF;
    if ((b[78] | b[79] << 8) == 501) {
        rules::ElemDb edb = s.edb;
        if (!edb.part_rows) edb.part_rows = count;
        if (!edb.part_val) edb.part_val = row;
        if (!edb.part_text) edb.part_text = [](const char*, size_t, const char*, std::string&) { return false; };
        if (!edb.ref_rows) edb.ref_rows = [](const char*, const char*, long) { return std::vector<size_t>(); };
        if (!edb.ref_val) edb.ref_val = [](const char*, size_t, const char*, long&) { return false; };
        if (!edb.ref_text) edb.ref_text = [](const char*, size_t, const char*, std::string&) { return false; };
        std::vector<uint8_t> h(36, 0);
        if (rules::elements(h.data(), r.groups, r.elements, r.arrays, get, edb)) {
            merge_elements(h, r);
            // the vision check again, now with the part's element state
            if (b[246] == 1) rules::vision_state(b, r.b7.data(), r.b19[0] == 2);
        }
    }
    out.why = std::string(b[117] == 2 ? "complete" : "incomplete") + " record" + (b[246] == 1 ? ", vision data" : "") +
              (r.bga.empty() ? "" : ", BGA ball pattern");
    return true;
}

bool research_record(const PartRows& p, ResearchResult& out, const rules::SubCompo* accel) {
    RecordSource s;
    auto num = [&](const char* t, const char* c, double& v) {
        bool null = false;
        return p.has_table(t) && p.get(t, c, v, null) && !null;
    };
    s.get = [&](const char* t, const char* c, long& v) {
        double d;
        if (!num(t, c, d)) return false;
        v = std::lround(d);
        return true;
    };
    s.real = [&](const char* t, const char* c, double& v) { return num(t, c, v); };
    s.text = [&](const char* t, const char* c, std::string& v) { return p.has_table(t) && p.get_text(t, c, v); };
    s.lookup = [](const char*, const char*, long, const char*, long&) { return false; };
    s.ltext = [](const char*, const char*, long, const char*, std::string&) { return false; };
    s.count = [](const char*) { return (size_t)0; };
    s.row = [](const char*, size_t, const char*, long&) { return false; };
    s.accel = accel;
    return research_with(s, out);
}

}  // namespace pcbp::juki
