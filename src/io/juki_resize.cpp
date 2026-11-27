#include "juki_resize.h"

#include "juki_builtin.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

namespace pcbp::juki {

namespace {
constexpr size_t kRec = 1822;
using Mask = std::array<uint8_t, kRec>;
int r16(const uint8_t* b, size_t o) { return b[o] | b[o + 1] << 8; }
int32_t r32(const uint8_t* b, size_t o) { int32_t v; std::memcpy(&v, b + o, 4); return v; }

// columns whose record bytes a size change rewrites
struct Col { const char* t; const char* c; };
const Col kNozzle[] = {{"Laser", "NozzleNo"}, {"Laser", "Vacuum"}, {"Laser", "GX4NozzleNo"}, {"Laser", "GX4Vacuum"},
                       {"LaserCX", "NozzleNo"}, {"LaserCX", "Vacuum"}, {"LaserFX1", "NozzleNo"}, {"LaserFX1", "Vacuum"},
                       {"LaserGx5", "NozzleNo"}, {"LaserGx5", "Vacuum"}, {"LaserFx2", "NozzleNo"}, {"LaserFx2", "PumpVacuum"},
                       {"LaserGx6", "NozzleNo"}, {"LaserGx6", "Vacuum"}, {"LaserGx6", "PumpVacuum"}, {"LaserFx3", "NozzleNo"},
                       {"LaserFx3", "PumpVacuum"}};
const Col kHeight[] = {{"Laser", "LaserHeight"}, {"Laser", "LaserHeightLnc60"}, {"Laser", "Fx3LaserHeightLnc60"}, {"Laser", "Fx2LaserHeightLnc60"}};
const Col kMech[] = {{"Mech", "NozzleNo"}, {"Mech", "Vacuum"}};
const Col kDims[] = {{"Compo", "Width"}, {"Compo", "Length"}, {"Compo", "Height"}};
const Col kFeeder[] = {{"Feeder", "TapeTypeID"}, {"Feeder", "Pitch"}};

struct Masks { Mask nozzle{}, height{}, mech{}, dims{}, feeder{}; bool ok = false; };

// bytes / bits a set of columns writes: research a reference part, change one column at a time, keep what moved
template <size_t N>
void measure(const AutoPart& base, const std::vector<uint8_t>& b0, const Col (&cols)[N], Mask& m) {
    for (const Col& c : cols) {
        double v = 0;
        bool null = false;
        if (!base.rows.get(c.t, c.c, v, null) || null) continue;
        // every bit of the value (the field's whole width), and 0x10 / 0: vacuum codes below 0x33 are "not set", so
        // their flag bits are found too
        std::vector<double> tries = {0x10, 0};
        const long iv = std::lround(v);
        for (int k = 0; k < 31; ++k) tries.push_back((double)(iv ^ (1L << k)));
        for (double t : tries) {
            AutoPart p = base;
            p.rows.set(c.t, c.c, t);
            ResearchResult r;
            if (!research_record(p.rows, r) || r.rec.b4.size() != kRec) continue;
            for (size_t o = 0; o < kRec; ++o) m[o] |= (uint8_t)(r.rec.b4[o] ^ b0[o]);
        }
    }
    // record state bytes belong to the editor's checks, not to the column
    for (size_t o : {117u, 227u, 247u, 393u}) m[o] = 0;
}

const Masks& masks() {
    static Masks M;
    static std::once_flag once;
    std::call_once(once, [] {
        // a chip (laser + mechanical centring) on 8 mm tape: every measured table has a row
        BuiltinInput in;
        in.cls = 1; in.x = 2.0; in.y = 1.25; in.h = 0.6;
        AutoPart base;
        std::string why;
        ResearchResult r0;
        if (!builtin_rows(in, base, why) || !research_record(base.rows, r0) || r0.rec.b4.size() != kRec) return;
        measure(base, r0.rec.b4, kNozzle, M.nozzle);
        measure(base, r0.rec.b4, kHeight, M.height);
        measure(base, r0.rec.b4, kMech, M.mech);
        measure(base, r0.rec.b4, kDims, M.dims);
        measure(base, r0.rec.b4, kFeeder, M.feeder);
        M.ok = true;
    });
    return M;
}

void patch(uint8_t* dst, const uint8_t* src, const Mask& m) {
    for (size_t o = 0; o < kRec; ++o)
        if (m[o]) dst[o] = (uint8_t)((dst[o] & ~m[o]) | (src[o] & m[o]));
}

int tape_width_of(int id, bool* paper) {
    static const int w[] = {0, 8, 8, 12, 16, 24, 32, 32, 44, 56};
    if (paper) *paper = id == 1 || id == 6;
    return id >= 1 && id <= 9 ? w[id] : 0;
}
int tape_id_of(int w, bool paper) {
    if (paper && w == 8) return 1;
    if (paper && w == 32) return 6;
    switch (w) { case 8: return 2; case 12: return 3; case 16: return 4; case 24: return 5; case 32: return 7; case 44: return 8; case 56: return 9; }
    return 0;
}

// the built-in part the new size wants (the record's class, packaging and tape kept as inputs)
bool wanted(const ComponentInfo& ci, double x, double y, double h, ResearchResult& out, std::string& why, AutoPart* rows = nullptr) {
    BuiltinInput in;
    in.cls = ci.cls;
    in.x = x; in.y = y; in.h = h;
    in.feeder_kind = ci.feeder_kind ? ci.feeder_kind : 1;
    return builtin_research(in, out, why, rows);
}

void suggest_feeder(const ComponentInfo& ci, double x, double y, double h, ResizeResult& r) {
    if (ci.feeder_kind != 1 && ci.feeder_kind != 0) return;
    int tw = 0, p = 0;
    builtin_feeder(x, y, tw, p);
    if (tw == 8 && p > 4) tw = 12;
    if (tw >= 24 && p < 8) p = 8;
    const bool flat = (ci.cls == 1 || ci.cls == 2 || ci.cls == 8 || ci.cls == 14) && h <= 1.1;
    r.suggest_tape_w = tw;
    r.suggest_pitch = p;
    r.suggest_paper = tw == 8 && p <= 4 && flat;
    r.feeder_differs = ci.feeder_kind == 1 && (ci.tape_w != tw || ci.pitch != p || ci.paper != r.suggest_paper);
    static const int W[] = {8, 12, 16, 24, 32, 44, 56};
    auto rank = [](int w) { for (int i = 0; i < 7; ++i) if (W[i] == w) return i; return -1; };
    const int a = rank(ci.tape_w), b = rank(tw);
    r.feeder_off = ci.feeder_kind == 1 && a >= 0 && b >= 0 && std::abs(a - b) >= 2;
}
int nozzle_rank(int n) {   // house nozzles by size; -1 for special ones
    switch (n) { case 500: return 0; case 504: return 1; case 505: return 2; case 506: return 3; case 507: return 4; case 508: return 5; }
    return -1;
}

bool run(uint8_t* b, double x, double y, double h, ResizeResult& r) {
    r = ResizeResult();
    if (!(x > 0 && y > 0) || x > 200 || y > 200 || h < 0 || h > 100) { r.why = "size out of range"; return false; }
    const Masks& M = masks();
    if (!M.ok) { r.why = "rules unavailable"; return false; }
    r.before = component_info(b);
    ResearchResult w;
    std::string why;
    if (!wanted(r.before, x, y, h, w, why)) { r.why = why; return false; }
    const uint8_t* n = w.rec.b4.data();
    patch(b, n, M.dims);
    if (r.before.laser) {
        patch(b, n, M.nozzle);
        patch(b, n, M.height);
    }
    if (r.before.mech && n[226]) patch(b, n, M.mech);
    r.after = component_info(b);
    r.why = why;
    if (r.before.vision && !r.before.laser) r.why += "; vision part: nozzle kept (teach it in the editor)";
    auto ch = [&](const char* what, int a, int c) {
        if (a != c) r.changes.push_back(std::string(what) + " " + std::to_string(a) + " -> " + std::to_string(c));
    };
    ch("nozzle", r.before.gx4_nozzle, r.after.gx4_nozzle);
    ch("laser nozzle", r.before.laser_nozzle, r.after.laser_nozzle);
    ch("mech nozzle", r.before.mech_nozzle, r.after.mech_nozzle);
    ch("vacuum", r.before.vacuum, r.after.vacuum);
    ch("laser height", r.before.laser_height, r.after.laser_height);
    suggest_feeder(r.after, x, y, h, r);
    if (r.before.laser) {
        // what the size wants against what the record had (run() rewrote the copy: compare with `before`)
        const int a = nozzle_rank(r.before.gx4_nozzle), b = nozzle_rank(r.after.gx4_nozzle);
        r.nozzle_off = r.before.gx4_nozzle == 0 ? r.after.gx4_nozzle != 0 : a >= 0 && b >= 0 && std::abs(a - b) >= 2;
    }
    return true;
}
}  // namespace

ComponentInfo component_info(const uint8_t* b) {
    ComponentInfo c;
    c.cls = r16(b, 78);
    c.x = r32(b, 81) / 1e4;
    c.y = r32(b, 85) / 1e4;
    c.h = r32(b, 89) / 1e4;
    c.laser = b[392] != 0;
    c.mech = b[226] != 0;
    c.vision = b[246] != 0;
    c.gx4_nozzle = r16(b, 424) ? r16(b, 424) - 1 : 0;
    c.laser_nozzle = r16(b, 394) ? r16(b, 394) - 1 : 0;
    c.mech_nozzle = r16(b, 228) ? r16(b, 228) - 1 : 0;
    c.vacuum = (b[604] & 8) ? b[396] : 0;
    c.gx4_vacuum = (b[605] & 0x40) ? b[426] : 0;
    c.laser_height = r32(b, 402);
    c.feeder_kind = b[80];
    if (c.feeder_kind == 1) {
        c.tape_w = tape_width_of(b[176], &c.paper);
        c.pitch = b[178];
    }
    return c;
}

std::string tape_label(int w, bool paper) {
    if (!w) return std::string();
    return std::to_string(w) + " mm " + (paper ? "paper" : "emboss");
}

bool resize_component(uint8_t* b4, double x, double y, double h, ResizeResult& r) { return run(b4, x, y, h, r); }

bool preview_resize(const uint8_t* b4, double x, double y, double h, ResizeResult& r) {
    std::vector<uint8_t> c(b4, b4 + kRec);
    return run(c.data(), x, y, h, r);
}

bool apply_feeder(uint8_t* b, int tape_w, int pitch, bool paper, std::string* why) {
    const ComponentInfo ci = component_info(b);
    if (ci.feeder_kind != 1) { if (why) *why = "not a tape part"; return false; }
    const int id = tape_id_of(tape_w, paper);
    if (!id || pitch <= 0 || pitch > 255) { if (why) *why = "no such tape"; return false; }
    if (tape_w == 8 && pitch > 4) { if (why) *why = "8 mm tape feeds 2 or 4 mm only"; return false; }
    const Masks& M = masks();
    if (!M.ok) { if (why) *why = "rules unavailable"; return false; }
    AutoPart rows;
    ResearchResult w;
    std::string wy;
    if (!wanted(ci, ci.x > 0 ? ci.x : 1, ci.y > 0 ? ci.y : 1, ci.h, w, wy, &rows)) { if (why) *why = wy; return false; }
    rows.rows.set("Feeder", "FdrTypeID", 1);
    rows.rows.set("Feeder", "TapeTypeID", id);
    rows.rows.set("Feeder", "Pitch", pitch);
    if (!research_record(rows.rows, w)) { if (why) *why = "research failed"; return false; }
    patch(b, w.rec.b4.data(), M.feeder);
    return true;
}

}  // namespace pcbp::juki
