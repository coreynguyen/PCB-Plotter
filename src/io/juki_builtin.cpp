#include "juki_builtin.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

// Built-in machine data rules (shared by PCB Plotter and Juki Tool; standard library only). See juki_builtin.h.
namespace pcbp::juki {

namespace {

// ---- families -----------------------------------------------------------------------------------------------
// Speed / acceleration tiers of the generic data: small two-terminal parts, small leaded parts, ICs, large ICs.
enum Tier { TChip, TSot, TIc, TBig };

struct Family {
    const char* name;
    Tier tier;
    double laser_ratio;   // laser height = -ratio x body height (where the laser beam crosses the body)
    bool mech;            // mechanical centring as well (small parts)
    double dflt_h;        // assumed height when the part has none (mm); 0 = from the body (chips)
};

const Family& family_of_class(int cls) {
    static const Family chip{"chip", TChip, 0.50, true, 0};
    static const Family ecap{"electrolytic capacitor", TChip, 0.95, true, 0};
    static const Family melf{"MELF", TChip, 0.50, true, 0};
    static const Family sot{"SOT", TSot, 0.25, true, 1.10};
    static const Family soj{"SOJ", TIc, 0.65, true, 3.50};
    static const Family sop{"SOP", TIc, 0.70, true, 1.75};
    static const Family qfp{"QFP", TBig, 0.70, false, 1.60};
    static const Family plcc{"PLCC", TBig, 0.65, false, 4.40};
    static const Family bga{"BGA", TBig, 0.85, false, 1.40};
    static const Family qfn{"QFN", TBig, 0.50, false, 0.90};
    static const Family conn{"connector", TBig, 0.50, false, 5.00};
    switch (cls) {
        case 1: case 2: case 8: case 14: return chip;
        case 3: return melf;
        case 6: return ecap;
        case 11: return sot;
        case 12: return soj;
        case 101: case 104: case 105: case 109: return sop;
        case 102: case 106: return qfp;
        case 103: return plcc;
        case 107: case 108: case 110: return bga;
        case 120: return qfn;
        case 201: case 202: case 203: return conn;
        default: return sop;
    }
}

// ---- nozzles ------------------------------------------------------------------------------------------------
// GX-4 nozzle -> the older laser head's nozzle and its vacuum level code; the mechanical-centring nozzle and its
// vacuum code (two-terminal parts / leaded parts)
struct Nozzle { int gx4, laser, vacuum, mech_chip, mech_ic, mech_vac_chip, mech_vac_ic; };
constexpr Nozzle kNozzles[] = {
    {500, 101, 227, 1, 1, 220, 220}, {501, 149, 227, 0, 0, 0, 0},       {502, 101, 227, 1, 1, 220, 220},
    {503, 101, 227, 1, 1, 220, 220}, {504, 102, 207, 2, 3, 218, 220},   {505, 103, 184, 3, 3, 220, 220},
    {506, 104, 222, 4, 4, 213, 213}, {507, 105, 236, 4, 4, 213, 213},   {508, 106, 238, 4, 4, 213, 213},
};
const Nozzle& nozzle_row(int gx4) {
    for (const auto& n : kNozzles) if (n.gx4 == gx4) return n;
    return kNozzles[8];
}

// DB tape type of a tape width / carrier (8 mm paper 1, 32 mm paper 6, embossed 8..56 mm 2,3,4,5,7,8,9)
int tape_type(int w, bool paper) {
    if (paper && w == 8) return 1;
    if (paper && w == 32) return 6;
    switch (w) {
        case 8: return 2; case 12: return 3; case 16: return 4; case 24: return 5; case 32: return 7; case 44: return 8;
        default: return w > 44 ? 9 : 3;
    }
}

long angle_code(double deg) {   // 1/80 degree, 0 .. 360 deg
    long a = std::lround(std::fmod(std::fmod(deg, 360.0) + 360.0, 360.0) * 80.0);
    return a >= 28800 ? 0 : a;
}

// a JEDEC matrix tray (322.6 x 135.9 mm, 7.62 mm high): pocket pitch = body + 4 mm, as many pockets as fit
void tray_rows(PartRows& r, double x, double y, double angle) {
    const double px = std::ceil((x + 4.0) * 10.0) / 10.0, py = std::ceil((y + 4.0) * 10.0) / 10.0;
    const int cols = std::clamp((int)std::floor(315.0 / px), 1, 40), rows = std::clamp((int)std::floor(128.0 / py), 1, 40);
    r.set("Feeder", "FdrTypeID", 4);
    r.set("Feeder", "TrayTypeID", 1);   // a tray on a tray holder (not a tray magazine)
    r.set("Feeder", "TrayAngle", (double)angle_code(angle));
    r.set("Feeder", "PilotPosX", std::lround(((322.6 - cols * px) / 2 + px / 2) * 1e4));
    r.set("Feeder", "PilotPosY", std::lround(((135.9 - rows * py) / 2 + py / 2) * 1e4));
    r.set("Feeder", "PitchX", std::lround(px * 1e4));
    r.set("Feeder", "PitchY", std::lround(py * 1e4));
    r.set("Feeder", "ColNo", cols);
    r.set("Feeder", "RowNo", rows);
    r.set("Feeder", "TrayHeight", 76200);
}

void speeds(PartRows& r, const char* t, Tier tier, bool fx1r) {
    // XY, Z down at pick, Z up at pick, Z down at place, Z up at place, theta, theta during laser
    struct S { int xy, pdz, puz, ldz, luz, th, thla; };
    static const S s[] = {{4, 4, 4, 4, 4, 0, 0}, {4, 4, 4, 4, 1, 0, 0}, {4, 4, 4, 1, 2, 1, 2}, {4, 4, 2, 1, 1, 1, 2}};
    const S& v = s[tier];
    r.set(t, "SpdXY", v.xy);
    r.set(t, "SpdZ", 0);
    r.set(t, "SpdPickDownZ", v.pdz);
    r.set(t, "SpdPickUpZ", v.puz);
    r.set(t, "SpdPlaceDownZ", v.ldz);
    r.set(t, "SpdPlaceUpZ", v.luz);
    r.set(t, "SpdTheta", v.th);
    r.set(t, "SpdThetaInLA", v.thla);
    if (fx1r) { r.set(t, "SpdPickDownZFX1R", v.pdz); r.set(t, "SpdPickUpZFX1R", v.puz); }
}

void fx3_speeds(PartRows& r, Tier tier) {
    struct S { int pdz, puz, lxy, lth, lthla, ldz, luz; };
    static const S s[] = {{10, 10, 10, 10, 10, 10, 10}, {10, 10, 10, 10, 10, 10, 15}, {10, 10, 10, 15, 17, 15, 16}, {10, 16, 10, 15, 17, 15, 15}};
    const S& v = s[tier];
    const char* t = "LaserFx3";
    for (const char* c : {"PickSpdXY", "PickSpdTheta", "PickSpdThetaInLA", "PickSpdXY_2", "PickSpdTheta_2", "PickSpdUpZ_2",
                          "PickSpdDownZ_2", "PickSpdThetaInLA_2", "PickZ2StepUse", "PickZ2StepUse_2", "PlaceSpdXY_2",
                          "PlaceSpdTheta_2", "PlaceSpdUpZ_2", "PlaceSpdDownZ_2", "PlaceSpdThetaInLA_2", "PlaceZ2StepUse",
                          "PlaceZ2StepUse_2"})
        r.set(t, c, 0);
    for (const char* c : {"PickZ2StepUp", "PickZ2StepDown", "PlaceZ2StepUp", "PlaceZ2StepDown"}) r.set(t, c, 20000);
    for (const char* c : {"PickZ2StepUp_2", "PickZ2StepDown_2", "PlaceZ2StepUp_2", "PlaceZ2StepDown_2"}) r.set(t, c, 4);
    r.set(t, "PickSpdDownZ", v.pdz);
    r.set(t, "PickSpdUpZ", v.puz);
    r.set(t, "PlaceSpdXY", v.lxy);
    r.set(t, "PlaceSpdTheta", v.lth);
    r.set(t, "PlaceSpdThetaInLA", v.lthla);
    r.set(t, "PlaceSpdDownZ", v.ldz);
    r.set(t, "PlaceSpdUpZ", v.luz);
}

std::string mm(double v) { char b[32]; std::snprintf(b, sizeof b, "%.2f", v); return b; }

// house nozzles (500, 504..508) and the nozzle for a body's short edge
int house_nozzle(int n) {
    if (n == 500 || (n >= 504 && n <= 508)) return n;
    if (n == 501) return 500;
    if (n == 502 || n == 503) return 504;
    if (n >= 509 && n <= 520) return 508;
    if (n > 0 && n < 500) return 500;
    return 0;
}
// GX-4 nozzle by the body's short edge, per package group (step tables: up to `max` mm -> nozzle)
struct NozStep { double max; int nz; };
const NozStep kNozChip[] = {{1.45, 500}, {2.4, 504}, {4.0, 505}, {7.0, 506}, {10.5, 507}, {1e9, 508}};
const NozStep kNozSot[] = {{1.2, 500}, {2.2, 504}, {3.75, 505}, {6.75, 506}, {9.5, 507}, {1e9, 508}};
const NozStep kNozSop[] = {{2.6, 500}, {3.4, 504}, {5.0, 505}, {8.0, 506}, {11.0, 507}, {1e9, 508}};
const NozStep kNozQfp[] = {{5.5, 504}, {6.5, 505}, {9.8, 506}, {14.0, 507}, {1e9, 508}};
const NozStep kNozQfn[] = {{1.45, 500}, {2.45, 504}, {4.0, 505}, {7.0, 506}, {9.9, 507}, {1e9, 508}};
const NozStep kNozBga[] = {{5.3, 504}, {6.75, 505}, {10.0, 506}, {13.5, 507}, {1e9, 508}};
const NozStep kNozConn[] = {{1.5, 500}, {3.0, 504}, {4.0, 505}, {7.0, 506}, {10.6, 507}, {1e9, 508}};
template <size_t N> int step(const NozStep (&t)[N], double s) { for (auto& e : t) if (s <= e.max) return e.nz; return 508; }
int nozzle_for(double s, int cls = 1) {
    switch (cls) {
        case 11: case 12: case 13: return step(kNozSot, s);
        case 101: case 104: case 105: case 109: return step(kNozSop, s);
        case 102: case 103: case 106: return step(kNozQfp, s);
        case 120: return step(kNozQfn, s);
        case 107: case 108: case 110: return step(kNozBga, s);
        case 201: case 202: case 203: return step(kNozConn, s);
        default: return step(kNozChip, s);
    }
}
// tape width (1.4 x short edge + 4) and feed pitch (long edge + 0.6) on the standard steps
void feeder_for(double x, double y, int& tw, int& pitch) {
    static const int W[] = {8, 12, 16, 24, 32, 44, 56}, P[] = {2, 4, 8, 12, 16, 20, 24, 32, 40};
    const double a = std::max(x, y), b = std::min(x, y);
    tw = 0; pitch = 0;
    if (a <= 0 || b <= 0) return;
    tw = 56; for (int w : W) if (1.4 * b + 4 <= w) { tw = w; break; }
    pitch = 40; for (int q : P) if (a + 0.6 <= q) { pitch = q; break; }
}
}  // namespace

bool builtin_rows(const BuiltinInput& p, AutoPart& out, std::string& why) {
    out = AutoPart();
    if (!(p.x > 0) || !(p.y > 0)) { why = "no body size"; return false; }
    const int cls = p.cls > 0 ? p.cls : 101;
    const Family& F = family_of_class(cls);
    out.cls = cls;
    out.family = F.name;
    const double x = p.x, y = p.y, s = std::min(x, y), l = std::max(x, y);
    double h = p.h;
    if (!(h > 0)) {
        h = F.dflt_h > 0 ? F.dflt_h : std::clamp(std::round(s * 0.55 * 20) / 20, 0.2, 2.5);
        out.notes.push_back("height " + mm(h) + " mm assumed (" + std::string(F.name) + ")");
    }
    // nozzle: the part's machine data, else by the body's short edge (house nozzles only)
    int gx4 = p.nozzle ? house_nozzle(p.nozzle) : 0;
    if (!gx4) gx4 = nozzle_for(s, cls);
    if (!gx4) gx4 = 508;
    const Nozzle& N = nozzle_row(gx4);
    const bool leaded = cls >= 11 && cls != 14;
    const bool mech = F.mech && N.mech_chip > 0;
    if (l > 33.5) out.notes.push_back("body " + mm(l) + " mm exceeds the laser range: teach vision centring in the editor");

    PartRows& r = out.rows;
    // Compo
    r.set("Compo", "CompoID", 1);
    r.set("Compo", "DataOk", 1);
    r.set_text("Compo", "Description", p.description);
    r.set("Compo", "PkgClassID", cls);
    r.set("Compo", "Width", std::lround(x * 1e4));
    r.set("Compo", "Length", std::lround(y * 1e4));
    r.set("Compo", "Height", std::lround(h * 1e4));
    for (const char* c : {"ConHeight", "AdheTest", "GX4PriorityLV", "CompOrient", "MoldLineOffsetX", "MoldLineOffsetY", "SubNozzle",
                          "UseVision"})
        r.set("Compo", c, 0);
    r.set("Compo", "UseMech", mech ? 1 : 0);
    r.set("Compo", "UseLaser", 1);

    // Feeder: the part's packaging (tape when unknown; QFP / BGA types on tray, as the machine-data rules)
    int kind = p.feeder_kind;
    if (!kind) kind = (F.tier == TBig && cls != 120 && cls < 200) ? 3 : 1;
    r.set("Feeder", "CompoID", 1);
    r.set("Feeder", "DataOk", 1);
    for (const char* c : {"Electric", "EtfProfile", "UseEtfProfile", "MTCPadPickUp", "MTCPadShuttle", "MTCType", "MTCSpeed", "MTSspeed",
                          "DTSSpeed", "MTSMarkRecog", "PickUpSelect", "TapeCylSpeed", "StickCylSpeed", "BulkCylSpeed", "TrayStartX",
                          "TrayStartY", "TrayPitchX", "TrayPitchY"})
        r.set("Feeder", c, 0);
    r.set("Feeder", "SupplyCond", 1);
    r.set("Feeder", "StickFeedTime", 100);
    r.set("Feeder", "BulkFeedTime", 100);
    r.set("Feeder", "TrayColNo", 1);
    r.set("Feeder", "TrayRowNo", 1);
    bool paper = false;
    if (kind == 3) tray_rows(r, x, y, p.supply_angle);
    else if (kind == 2) {
        r.set("Feeder", "FdrTypeID", 2);
        r.set("Feeder", "StickTypeID", 9);
        r.set("Feeder", "StickAngle", (double)angle_code(p.supply_angle ? p.supply_angle : 270));
        out.notes.push_back("stick type 9 assumed: check the stick feeder");
    } else {
        if (kind == 4) out.notes.push_back("bulk packaging written as tape");
        int tw = p.tape_w, pitch = p.pitch, ftw = 0, fpitch = 0;
        const std::string carrier = p.carrier;
        feeder_for(x, y, ftw, fpitch);
        if (!tw) tw = ftw ? ftw : 8;
        if (!pitch) pitch = fpitch ? fpitch : 4;
        // feeder limits: 8 mm tape feeds 2 / 4 mm, 24 mm and wider tape 8 mm and more
        if (tw == 8 && pitch > 4) { out.notes.push_back("8 mm tape cannot feed " + std::to_string(pitch) + " mm: 12 mm tape"); tw = 12; }
        if (tw >= 24 && pitch < 8) pitch = 8;
        // paper carriers: 8 mm tape at 2 / 4 mm pitch for flat two-terminal parts; anything else embossed
        const bool flat = (cls == 1 || cls == 2 || cls == 8 || cls == 14) && h <= 1.1;
        paper = carrier.empty() ? tw == 8 && pitch <= 4 && flat : carrier == "Paper";
        if (paper && (tw != 8 || pitch > 4) && tw != 32) {
            paper = false;
            out.notes.push_back(std::to_string(tw) + " mm tape at " + std::to_string(pitch) + " mm pitch written as embossed");
        }
        r.set("Feeder", "FdrTypeID", 1);
        r.set("Feeder", "TapeTypeID", tape_type(tw, paper));
        r.set("Feeder", "Pitch", pitch);
        r.set("Feeder", "TapeAngle", (double)angle_code(p.supply_angle));
    }

    // Expansion (FlexLine's import defaults, accelerations by tier)
    r.set("Expansion", "CompoID", 1);
    r.set("Expansion", "DataOk", 1);
    for (const char* c : {"Test", "Protect", "CompoCheck", "PickPosAdjust", "ScrapType", "PickOffsetZ", "PickOffsetX", "PickOffsetY",
                          "PickVacuumTiming", "FCType", "FCOverPick", "FCOverPlace", "PlaceVacOffset", "OCCPickPosAdjust", "AdheTest",
                          "WeakBlow", "BlowStartCX", "BlowTimeCX", "PumpVacuumTiming", "FCTypeGx6", "FCOverPickGx6", "FCOverPlaceGx6",
                          "ScrapTypeGx6", "PickVacuumTimingFx3", "PickVacOnFx3", "PickVacCorrFx3", "PickHoldFx3", "PlaceVacuumTimingFx3",
                          "PlaceVacOffFx3", "PlaceVacCorrFx3", "PlaceBlowOnFx3", "PlaceBlowTimeFx3", "PlaceHoldFx3", "BeaChip",
                          "DirectPlace", "Epv"})
        r.set("Expansion", c, 0);
    r.set("Expansion", "CompoLayer", 4);
    r.set("Expansion", "Retry", F.tier == TBig ? 2 : 1);
    r.set("Expansion", "GripNozID", F.tier == TBig ? 2 : 1);
    r.set("Expansion", "AutoTeach", paper ? 1 : 0);
    for (const char* m : {"", "2K", "Gx5", "Fx1", "CX", "Gx6", "Fx3", "Fx2"}) {
        r.set("Expansion", std::string("OverPick") + m, 2000);
        r.set("Expansion", std::string("OverPlace") + m, 5000);
    }
    static const int accel[4][3] = {{100, 156, 100}, {90, 156, 100}, {90, 100, 100}, {80, 80, 80}};
    r.set("Expansion", "TableAccel", accel[F.tier][0]);
    r.set("Expansion", "TableAccelFx1R", accel[F.tier][1]);
    r.set("Expansion", "TableAccelFx2", accel[F.tier][2]);

    // Mech (small parts)
    if (mech) {
        r.set("Mech", "CompoID", 1);
        r.set("Mech", "DataOk", 1);
        r.set("Mech", "NozzleNo", leaded ? N.mech_ic : N.mech_chip);
        r.set("Mech", "Vacuum", leaded ? N.mech_vac_ic : N.mech_vac_chip);
        r.set("Mech", "CtrLvl", F.tier == TChip ? 2 : F.tier == TSot ? 3 : 4);
        r.set("Mech", "WithLead", leaded ? 1 : 0);
        r.set("Mech", "PreRotate", F.tier == TSot ? 1 : 0);
        r.set("Mech", "SpeedID", F.tier == TChip ? 0 : F.tier == TSot ? 128 : 64);
        for (const char* c : {"StrokeCtrl", "PreCtr", "HgtChkMtd"}) r.set("Mech", c, 0);
    }

    // Laser and the machine-specific laser tables
    const long lh = -std::lround(F.laser_ratio * h * 1e4);
    static const long speed_id[4][2] = {{0, 0}, {64, 64}, {37008, 4240}, {38992, 6224}};
    r.set("Laser", "CompoID", 1);
    r.set("Laser", "DataOk", 1);
    r.set("Laser", "NozzleNo", N.laser);
    r.set("Laser", "Vacuum", N.vacuum);
    r.set("Laser", "LaserAlgo", F.tier <= TSot ? 1 : 2);
    for (const char* c : {"LaserHeight", "LaserHeightLnc60", "Fx3LaserHeightLnc60", "Fx2LaserHeightLnc60"}) r.set("Laser", c, (double)lh);
    for (const char* c : {"OffsetX", "OffsetY", "MaxChipTolerance", "MinChipTolerance", "GX4OffsetRot"}) r.set("Laser", c, 0);
    r.set("Laser", "SpeedID", (double)speed_id[F.tier][0]);
    r.set("Laser", "SpeedID700", (double)speed_id[F.tier][1]);
    r.set("Laser", "LA", 1);
    r.set("Laser", "GX4LA", 1);
    r.set("Laser", "GX4NozzleNo", gx4);
    r.set("Laser", "GX4Vacuum", 217);
    r.set("Laser", "GX4PreRotate", 2400);
    const int shape = F.tier <= TSot ? 2 : 4;
    struct M { const char* t; const char* la; bool pump, vac, shape, fx1r; };
    for (const M& m : {M{"LaserCX", "CXLA", false, true, false, false}, M{"LaserFX1", "FX1LA", false, true, false, true},
                       M{"LaserFx2", "FX2LA", true, false, true, false}, M{"LaserGx5", "Gx5LA", false, true, false, false},
                       M{"LaserGx6", "Gx6LA", true, true, true, false}}) {
        r.set(m.t, "CompoID", 1);
        r.set(m.t, m.la, 1);
        r.set(m.t, "NozzleNo", gx4);
        if (m.vac) r.set(m.t, "Vacuum", 217);
        if (m.pump) r.set(m.t, "PumpVacuum", 217);
        if (m.shape) r.set(m.t, "CompoShape", shape);
        speeds(r, m.t, F.tier, m.fx1r);
    }
    r.set("LaserGx6", "DetailFlag", 0);
    r.set("LaserGx6", "LaserSelect", 0);
    r.set("LaserFx3", "CompoID", 1);
    r.set("LaserFx3", "Fx3LA", 1);
    r.set("LaserFx3", "NozzleNo", gx4);
    r.set("LaserFx3", "PumpVacuum", 217);
    r.set("LaserFx3", "CompoShape", shape);
    fx3_speeds(r, F.tier);

    // Inspection (FlexLine's import defaults) and Fluxer (off)
    r.set("Inspection", "CompoID", 1);
    for (const char* c : {"ChipStandCheck", "Vacuum5", "HChkX", "HChkY", "HChkZ", "CoplaChk", "CoplaOffset", "VerifyChk", "VerifyType",
                          "VerifyUnit", "Standard", "Max", "Min", "DimChkChk", "DimChkWid", "DimChkLen", "RatioMin", "RatioMax",
                          "SOT_Dir_Insp", "Polarity_Dir_Insp", "PitchChk", "PitchErrLvl", "PwbHeightMeasure", "CmpHeightCheck"})
        r.set("Inspection", c, 0);
    r.set("Inspection", "DataOk", 1);
    r.set("Inspection", "CoplaTolerance", 2000);
    for (const char* c : {"MaxRangeX", "MinRangeX", "MaxRangeY", "MinRangeY"}) r.set("Inspection", c, 30);
    r.set("Inspection", "PH_Point", 1);
    r.set("Fluxer", "CompoID", 1);
    r.set("Fluxer", "FluxerUse", 0);
    return true;
}

bool builtin_research(const BuiltinInput& p, ResearchResult& out, std::string& why, AutoPart* rows) {
    AutoPart a;
    if (!builtin_rows(p, a, why)) return false;
    if (!research_record(a.rows, out)) { why = "research failed"; return false; }
    why = a.family;
    for (const auto& n : a.notes) why += "; " + n;
    out.why = why;
    if (rows) *rows = std::move(a);
    return true;
}

int builtin_nozzle(double short_edge, int cls) { return nozzle_for(short_edge, cls); }
void builtin_feeder(double x, double y, int& tape_w, int& pitch) { feeder_for(x, y, tape_w, pitch); }
const char* builtin_family(int cls) { return family_of_class(cls).name; }

}  // namespace pcbp::juki
