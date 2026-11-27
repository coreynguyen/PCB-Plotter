#include "juki_autopart.h"

#include "core/machine_assign.h"
#include "io/juki_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pcbp::juki {

namespace {
// component class from the package family (core/machine_assign) when the part has no class code
int class_of(const JobPart& p) {
    if (p.type_code > 0) return p.type_code;
    const mach::PkgId id = mach::identify(p.package, p.description);
    switch (id.fam) {
        case mach::Fam::Chip: case mach::Fam::Tant: case mach::Fam::Inductor: case mach::Fam::Xtal: return 1;
        case mach::Fam::Melf: return 3;
        case mach::Fam::ECap: return 6;
        case mach::Fam::Led: return 14;
        case mach::Fam::Sod: case mach::Fam::Sot: case mach::Fam::Dpak: return 11;
        case mach::Fam::Sop: return 101;
        case mach::Fam::Qfn: return 120;
        case mach::Fam::Qfp: return 102;
        case mach::Fam::Bga: return 107;
        case mach::Fam::Plcc: return 103;
        case mach::Fam::Conn: return 201;
        default: break;
    }
    // by shape: two / three terminals and small -> chip / SOT, else an SO-type IC, four-sided when square
    const double s = std::min(p.length, p.width), l = std::max(p.length, p.width);
    if (p.leads > 0 && p.leads <= 2 && l <= 7.5) return 1;
    if (p.leads > 0 && p.leads <= 6 && l <= 7.5) return 11;
    if (p.leads >= 16 && s > 0 && l / s < 1.25) return 102;
    return 101;
}

}  // namespace

BuiltinInput builtin_input(const JobPart& p) {
    BuiltinInput in;
    in.cls = class_of(p);
    in.x = p.length;
    in.y = p.width;
    in.h = p.height;
    in.description = p.description;
    in.feeder_kind = p.feeder_kind;
    if (p.tape_code > 0) mach::h8h_decode_tape(p.tape_code, in.tape_w, in.carrier);
    if (p.pitch_code > 0) in.pitch = mach::h8h_decode_pitch(p.pitch_code);
    in.nozzle = p.nozzle;
    in.supply_angle = p.supply_angle;
    return in;
}

bool builtin_research(const JobPart& p, ResearchResult& out, std::string& why) { return builtin_research(builtin_input(p), out, why); }

PartRows template_rows(const std::string& description, double l, double w, double h, int tape_type_id, int pitch, int nozzle) {
    PartRows r;
    r.set("Compo", "DataOk", 1);
    if (!description.empty()) r.set_text("Compo", "Description", description);
    if (l > 0 && w > 0) {   // as teaching writes the machine body: Width the long edge
        r.set("Compo", "Width", std::lround(std::max(l, w) * 1e4));
        r.set("Compo", "Length", std::lround(std::min(l, w) * 1e4));
    }
    if (h > 0) r.set("Compo", "Height", std::lround(h * 1e4));
    if (tape_type_id > 0) {
        r.set("Feeder", "TapeTypeID", tape_type_id);
        r.set("Expansion", "AutoTeach", tape_type_id == 1 || tape_type_id == 6 ? 1 : 0);
    }
    if (pitch > 0) r.set("Feeder", "Pitch", pitch);
    if (nozzle > 0) { r.set("Laser", "GX4NozzleNo", nozzle); r.set("LaserFx3", "NozzleNo", nozzle); }
    return r;
}

// the writer's built-in rules for H8H parts no hook filled
namespace {
struct RegisterAutopart {
    RegisterAutopart() {
        set_h8h_autopart([](JobPart& p) {
            ResearchResult rr;
            std::string why;
            if (!builtin_research(p, rr, why)) return false;
            p.h8h_b4 = rr.rec.b4; p.h8h_b7 = rr.rec.b7; p.h8h_b18 = rr.rec.b18; p.h8h_b19 = rr.rec.b19;
            p.h8h_bga = rr.rec.bga; p.h8h_groups = rr.rec.groups; p.h8h_elements = rr.rec.elements; p.h8h_arrays = rr.rec.arrays;
            p.h8h_note = why;
            return true;
        });
    }
} g_register_autopart;
}  // namespace

}  // namespace pcbp::juki
