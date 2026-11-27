#include "juki_research_rules.h"

#include <filesystem>
#include <fstream>
#include <iterator>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <cstring>

namespace pcbp::juki::rules {
namespace {

void w8(uint8_t* b, int o, long v) { b[o] = (uint8_t)v; }
void w16(uint8_t* b, int o, long v) { b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8); }
void w32(uint8_t* b, int o, long v) { const int32_t x = (int32_t)v; std::memcpy(b + o, &x, 4); }
uint16_t r16(const uint8_t* b, int o) { return (uint16_t)(b[o] | b[o + 1] << 8); }
void set16(uint8_t* b, int o, uint16_t m) { w16(b, o, r16(b, o) | m); }
void clr16(uint8_t* b, int o, uint16_t m) { w16(b, o, r16(b, o) & ~m); }

// Columns of the tables the read takes through bound record fields: a NULL there reads as the field's "pseudo
// NULL" value (byte 255, short 0x7EE4, long 0x4A4D4120, double -9.123e19), not as 0.
struct Kind { const char *table, *column; char k; };
const Kind kKinds[] = {
{"Compo", "CompoID", 'L'},
{"Compo", "DataOk", 'B'},
{"Compo", "MnftrID", 'L'},
{"Compo", "PkgClassID", 'S'},
{"Compo", "PkgTypeID", 'L'},
{"Compo", "Width", 'L'},
{"Compo", "Length", 'L'},
{"Compo", "Height", 'L'},
{"Compo", "ConHeight", 'L'},
{"Compo", "MoldlineWid", 'L'},
{"Compo", "MoldlineLen", 'L'},
{"Compo", "AdheID", 'L'},
{"Compo", "UseMech", 'B'},
{"Compo", "UseVision", 'B'},
{"Compo", "UseLaser", 'B'},
{"Compo", "AdheTest", 'B'},
{"Compo", "AdheOk", 'B'},
{"Compo", "AdheOk775", 'B'},
{"Compo", "GX4PriorityLV", 'B'},
{"Compo", "AdheOk2077", 'B'},
{"Compo", "CompOrient", 'S'},
{"Compo", "MoldLineOffsetX", 'L'},
{"Compo", "MoldLineOffsetY", 'L'},
{"Compo", "SubNozzle", 'L'},
{"Feeder", "CompoID", 'L'},
{"Feeder", "FdrTypeID", 'B'},
{"Feeder", "DataOk", 'B'},
{"Feeder", "TapeTypeID", 'B'},
{"Feeder", "Pitch", 'B'},
{"Feeder", "TapeAngle", 'S'},
{"Feeder", "StickTypeID", 'B'},
{"Feeder", "StickFeedTime", 'B'},
{"Feeder", "StickAngle", 'S'},
{"Feeder", "StickChangerType", 'B'},
{"Feeder", "BulkTypeID", 'B'},
{"Feeder", "BulkFeedTime", 'B'},
{"Feeder", "BulkAngle", 'S'},
{"Feeder", "TrayTypeID", 'B'},
{"Feeder", "TrayAngle", 'S'},
{"Feeder", "PilotPosX", 'L'},
{"Feeder", "PilotPosY", 'L'},
{"Feeder", "PitchX", 'L'},
{"Feeder", "PitchY", 'L'},
{"Feeder", "RowNo", 'B'},
{"Feeder", "ColNo", 'B'},
{"Feeder", "TrayHeight", 'L'},
{"Feeder", "MTCPadPickUp", 'B'},
{"Feeder", "MTCPadShuttle", 'B'},
{"Feeder", "MTCType", 'B'},
{"Feeder", "MTCSpeed", 'B'},
{"Feeder", "MTSspeed", 'B'},
{"Feeder", "SupplyCond", 'B'},
{"Feeder", "TrayColNo", 'B'},
{"Feeder", "TrayPitchX", 'L'},
{"Feeder", "TrayPitchY", 'L'},
{"Feeder", "TrayRowNo", 'B'},
{"Feeder", "TrayStartX", 'L'},
{"Feeder", "TrayStartY", 'L'},
{"Feeder", "DTSSpeed", 'B'},
{"Feeder", "MTSMarkRecog", 'B'},
{"Feeder", "PickUpSelect", 'B'},
{"Feeder", "FctfTypeID", 'B'},
{"Feeder", "FctfAngle", 'S'},
{"Feeder", "Electric", 'L'},
{"Feeder", "UseEtfProfile", 'B'},
{"Feeder", "EtfProfile", 'B'},
{"Mech", "CompoID", 'L'},
{"Mech", "DataOk", 'B'},
{"Mech", "NozzleNo", 'S'},
{"Mech", "Vacuum", 'B'},
{"Mech", "CtrLvl", 'B'},
{"Mech", "WithLead", 'B'},
{"Mech", "PreRotate", 'B'},
{"Mech", "SpeedID", 'L'},
{"Mech", "HgtChkMtd", 'B'},
{"Mech", "PreCtr", 'B'},
{"Mech", "StrokeCtrl", 'B'},
{"Vision", "CompoID", 'L'},
{"Vision", "DataOk", 'B'},
{"Vision", "LAICNozzleNo", 'S'},
{"Vision", "LAIC", 'B'},
{"Vision", "IC", 'B'},
{"Vision", "LAICVacuum", 'B'},
{"Vision", "ICVacuum", 'B'},
{"Vision", "OutlineWid", 'L'},
{"Vision", "OutlineLen", 'L'},
{"Vision", "CenterOffsetX", 'L'},
{"Vision", "CenterOffsetY", 'L'},
{"Vision", "OffsetX", 'L'},
{"Vision", "OffsetY", 'L'},
{"Vision", "SpeedID", 'L'},
{"Vision", "FMLA", 'B'},
{"Vision", "FMLACenterOffsetX", 'L'},
{"Vision", "FMLACenterOffsetY", 'L'},
{"Vision", "FMLANozzleNo", 'S'},
{"Vision", "FMLAOffsetX", 'L'},
{"Vision", "FMLAOffsetY", 'L'},
{"Vision", "FMLAOffsetRot", 'S'},
{"Vision", "FMLAOutlineX", 'L'},
{"Vision", "FMLAOutlineY", 'L'},
{"Vision", "FMLAVacuum", 'B'},
{"Vision", "SpeedID700", 'L'},
{"Laser", "CompoID", 'L'},
{"Laser", "DataOk", 'B'},
{"Laser", "NozzleNo", 'S'},
{"Laser", "Vacuum", 'B'},
{"Laser", "LaserAlgo", 'B'},
{"Laser", "LaserHeight", 'L'},
{"Laser", "OffsetX", 'L'},
{"Laser", "OffsetY", 'L'},
{"Laser", "MaxChipTolerance", 'S'},
{"Laser", "MinChipTolerance", 'S'},
{"Laser", "SpeedID", 'L'},
{"Laser", "GX4LA", 'B'},
{"Laser", "GX4Laser", 'L'},
{"Laser", "GX4LaserAlgo", 'B'},
{"Laser", "GX4NozzleNo", 'S'},
{"Laser", "GX4OffsetX", 'L'},
{"Laser", "GX4OffsetY", 'L'},
{"Laser", "GX4OffsetRot", 'S'},
{"Laser", "GX4PreRotate", 'S'},
{"Laser", "GX4Vacuum", 'B'},
{"Laser", "LA", 'B'},
{"Laser", "SpeedID700", 'L'},
{"Laser", "LaserHeightLnc60", 'L'},
{"Laser", "Fx3LaserHeightLnc60", 'L'},
{"Laser", "Fx2LaserHeightLnc60", 'L'},
{"Inspection", "CompoID", 'L'},
{"Inspection", "DataOk", 'B'},
{"Inspection", "ChipStandCheck", 'B'},
{"Inspection", "Vacuum5", 'B'},
{"Inspection", "HChkX", 'L'},
{"Inspection", "HChkY", 'L'},
{"Inspection", "HChkZ", 'L'},
{"Inspection", "CoplaChk", 'B'},
{"Inspection", "CoplaTolerance", 'L'},
{"Inspection", "CoplaOffset", 'L'},
{"Inspection", "VerifyChk", 'B'},
{"Inspection", "VerifyType", 'B'},
{"Inspection", "VerifyUnit", 'B'},
{"Inspection", "Standard", 'D'},
{"Inspection", "Max", 'S'},
{"Inspection", "Min", 'S'},
{"Inspection", "DimChkChk", 'B'},
{"Inspection", "DimChkWid", 'L'},
{"Inspection", "DimChkLen", 'L'},
{"Inspection", "MaxRangeX", 'S'},
{"Inspection", "MinRangeX", 'S'},
{"Inspection", "MaxRangeY", 'S'},
{"Inspection", "RatioMin", 'S'},
{"Inspection", "RatioMax", 'S'},
{"Inspection", "MinRangeY", 'S'},
{"Inspection", "SOT_Dir_Insp", 'B'},
{"Inspection", "Polarity_Dir_Insp", 'B'},
{"Inspection", "PitchChk", 'B'},
{"Inspection", "PitchErrLvl", 'L'},
{"Inspection", "PwbHeightMeasure", 'B'},
{"Inspection", "PH_Point", 'B'},
{"Inspection", "PH_Level", 'L'},
{"Inspection", "PH_OffsetX", 'L'},
{"Inspection", "PH_OffsetY", 'L'},
{"Inspection", "CmpHeightCheck", 'B'},
{"Inspection", "CH_Point", 'B'},
{"Inspection", "CH_Level", 'L'},
{"Inspection", "CH_Offset1_X", 'L'},
{"Inspection", "CH_Offset1_Y", 'L'},
{"Inspection", "CH_Offset2_X", 'L'},
{"Inspection", "CH_Offset2_Y", 'L'},
{"Inspection", "CH_Offset3_X", 'L'},
{"Inspection", "CH_Offset3_Y", 'L'},
{"Inspection", "CH_Offset4_X", 'L'},
{"Inspection", "CH_Offset4_Y", 'L'},
{"Expansion", "CompoID", 'L'},
{"Expansion", "DataOk", 'B'},
{"Expansion", "Retry", 'B'},
{"Expansion", "Test", 'B'},
{"Expansion", "OverPlace", 'L'},
{"Expansion", "OverPick", 'L'},
{"Expansion", "CompoCheck", 'B'},
{"Expansion", "PickPosAdjust", 'B'},
{"Expansion", "ScrapType", 'B'},
{"Expansion", "Protect", 'B'},
{"Expansion", "CompoLayer", 'B'},
{"Expansion", "AutoTeach", 'B'},
{"Expansion", "GripNozID", 'L'},
{"Expansion", "OverPick2K", 'L'},
{"Expansion", "OverPlace2K", 'L'},
{"Expansion", "PickOffsetZ", 'L'},
{"VisionData", "CompoID", 'L'},
{"VisionData", "DataOk", 'B'},
{"VisionData", "Contrast", 'B'},
{"VisionData", "LeadPitch", 'L'},
{"VisionData", "LeadLength", 'L'},
{"VisionData", "LeadWidth", 'L'},
{"VisionData", "BottomID", 'L'},
{"VisionData", "RightID", 'L'},
{"VisionData", "TopID", 'L'},
{"VisionData", "LeftID", 'L'},
{"VisionData", "BendLevel", 'B'},
{"VisionData", "ViewingField", 'B'},
{"VisionData", "PatternID", 'L'},
{"VisionData", "SubClass", 'B'},
{"VisionData", "Algorithm", 'B'},
{"VisionData", "NonInterfere", 'B'},
{"VisionData", "Channel", 'B'},
{"VisionData", "LightType", 'L'},
{"VisionData", "LightVol", 'B'},
{"VisionData", "BottomLeftLead", 'B'},
{"VisionData", "BottomRightLead", 'B'},
{"VisionData", "TopLeftLead", 'B'},
{"VisionData", "TopRightLead", 'B'},
{"VisionData", "DefCheck", 'B'},
{"VisionData", "DefChkLvl", 'B'},
{"VisionData", "DiaCheck", 'B'},
{"VisionData", "DiaChkLvl", 'B'},
{"VisionData", "LightCorrect", 'S'},
{"VisionData", "LeadPosFlg", 'B'},
{"VisionData", "LeadREdgePosX", 'L'},
{"VisionData", "LeadREdgePosY", 'L'},
{"VisionData", "CoplaID", 'L'},
{"Side", "SideID", 'L'},
{"Side", "DataOk", 'B'},
{"Side", "Count", 'L'},
{"Side", "LackedStart1", 'L'},
{"Side", "LackedCount1", 'B'},
{"Side", "LackedStart2", 'L'},
{"Side", "LackedCount2", 'B'},
{"Side", "LackedStart3", 'L'},
{"Side", "LackedCount3", 'B'},
{"Side", "LeadLength", 'L'},
{"Side", "LeadWidth", 'L'},
{"Side", "LeadPitch", 'L'},
{"Side", "Profile", 'B'},
{"BGA", "PatternID", 'L'},
{"BGA", "ColNum", 'B'},
{"BGA", "LineNum", 'B'},
{"BGA", "Peripheral", 'B'},
{"BGA", "BallNum", 'L'},
{"BGA", "PatternType", 'B'},
{"BGA", "BaseStyle", 'B'}
};
char kind_of(const char* t, const char* c) {
    auto ieq = [](const char* a, const char* b) {
        for (; *a && *b; ++a, ++b) if (std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b)) return false;
        return *a == *b;
    };
    for (const Kind& k : kKinds) if (ieq(k.table, t) && ieq(k.column, c)) return k.k;
    return 0;
}
long pseudo_null(char k) { return k == 'B' ? 0xFF : k == 'S' ? 0x7EE4 : k == 'L' ? 0x4A4D4120L : 0; }
// value as the read sees it: the column, or its pseudo NULL (0 for columns read by name)
long raw(const Get& get, const char* t, const char* c) {
    long v = 0;
    return get(t, c, v) ? v : pseudo_null(kind_of(t, c));
}
long raw_ref(const Lookup& lookup, const char* t, const char* kc, long key, const char* c) {
    long v = 0;
    return lookup(t, kc, key, c, v) ? v : pseudo_null(kind_of(t, c));
}

// database text (UTF-8 here) as the editor's ANSI code page (1252) sees it; unmappable characters become '?'
std::string ansi(const std::string& s) {
    static const uint16_t hi[32] = {0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                    0x2039, 0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013,
                                    0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};
    std::string o;
    for (size_t i = 0; i < s.size();) {
        const uint8_t b = (uint8_t)s[i];
        uint32_t c = b;
        size_t n = 1;
        if (b >= 0xC0) {
            n = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : 2;
            c = b & (0x7F >> n);
            for (size_t k = 1; k < n && i + k < s.size(); ++k) c = c << 6 | ((uint8_t)s[i + k] & 0x3F);
        }
        i += n;
        if (c < 0x80 || (c >= 0xA0 && c < 0x100)) { o += (char)c; continue; }
        char m = '?';
        for (int k = 0; k < 32; ++k) if (hi[k] && hi[k] == c) m = (char)(0x80 + k);
        o += m;
    }
    return o;
}

// machine feed code of a tape (tape width/type code x pitch code); pitch codes above 200 are the paper/emboss
// variants of the plain pitches
uint8_t tape_feed_code(uint8_t tape, uint8_t pitch) {
    const int p = pitch;
    auto in = [&](std::initializer_list<int> l) { for (int x : l) if (x == p) return true; return false; };
    switch (tape) {
        case 1: case 2: case 11: return p == 1 ? 22 : 0;
        case 3: return in({8, 12}) ? 1 : in({201, 202}) ? 20 : 0;
        case 4: return in({4, 8, 12}) ? 1 : in({16, 201, 202}) ? 20 : 0;
        case 5: return in({8, 204}) ? 2 : in({12, 16, 20, 24}) ? 3 : 0;
        case 6: return 14;
        case 7:
            if (in({8, 204, 206})) return 2;
            if (in({12, 16, 20, 203, 205, 207})) return 3;
            return p == 24 ? 4 : p == 28 ? 5 : p == 32 ? 6 : 0;
        case 8: return in({8, 12, 20, 206, 208}) ? 16 : in({24, 28, 32, 36, 40, 44}) ? 15 : 0;
        case 9: case 10:
            if (in({12, 20, 206, 208})) return 16;
            if (in({24, 28, 32, 36, 40, 44, 203})) return 17;
            if (p == 48) return 18;
            if (in({52, 56}) || (tape == 9 && p == 209)) return 19;
            return 0;
        default: return 0;
    }
}

// machine code of a stick feeder type
uint8_t stick_code(uint8_t t) {
    if (t && t <= 7) return (uint8_t)(t + 6);
    switch (t) {
        case 8: return 9;
        case 9: return 11;
        case 10: return 13;
        case 12: case 13: return 1;
        case 14: case 15: return 9;
        case 20: case 21: return 1;
        default: return 0;
    }
}

// pitches a tape type is fed at without a pitch check (pitch code -> tape types)
bool pitch_without_check(uint8_t tape, uint8_t pitch) {
    auto in = [&](std::initializer_list<int> l) { for (int x : l) if (x == tape) return true; return false; };
    switch (pitch) {
        case 1: return true;
        case 4: return in({5, 7, 8, 9});
        case 8: return in({1, 2, 9});
        case 203: return in({4, 5});
        case 205: return in({4});
        case 206: return in({3});
        case 207: return in({4, 5});
        case 208: return in({4});
        default: return false;
    }
}

void tray(uint8_t* b, const Get& get) {
    long v = 0;
    auto col = [&](const char* c) { return raw(get, "Feeder", c); };
    auto has = [&](const char* c) { return get("Feeder", c, v); };
    w8(b, 80, 3);
    w16(b, 224, 0x0F0F);
    w16(b, 1135, (r16(b, 1135) & 0xFFCF) | 16 * (col("DTSSpeed") & 3));
    long tt = 0;
    const bool tt_set = get("Feeder", "TrayTypeID", tt);
    if (tt_set && tt == 3) {
        b[224] |= 0x40;
        w8(b, 176, 2);
        w8(b, 1130, 16 * col("MTCPadPickUp") + col("MTCPadShuttle"));
        w16(b, 1135, (r16(b, 1135) & 0xFFFC) | (col("MTCSpeed") & 3));
        w16(b, 1135, (r16(b, 1135) & 0xFFF3) | 4 * (col("MTSSpeed") & 3));
        w8(b, 202, col("SupplyCond"));
        w16(b, 1135, (r16(b, 1135) & 0xFFBF) | (col("MTSMarkRecog") & 1) << 6);
        w16(b, 1135, (r16(b, 1135) & 0xFF7F) | (col("PickupSelect") & 1) << 7);
    } else if (tt_set && tt == 4) {
        b[224] |= 0x40;
        w8(b, 176, 4);
    } else if (tt_set) {
        b[224] |= 0x40 | 0x80;
        w8(b, 176, 1);
        w8(b, 177, tt);
    }
    if (tt_set && tt == 5) {
        w8(b, 177, 0);   // wafer feeder: its own table (not read here)
        return;
    }
    if (has("TrayAngle")) { b[225] |= 1; w16(b, 178, v); } else clr16(b, 224, 0x100);
    if (has("PilotPosX")) w32(b, 180, v); else clr16(b, 224, 1);
    if (has("PilotPosY")) w32(b, 184, v); else clr16(b, 224, 2);
    if (has("PitchX")) w32(b, 188, v); else clr16(b, 224, 4);
    if (has("PitchY")) w32(b, 192, v); else clr16(b, 224, 8);
    if (has("RowNo")) { b[224] |= 0x10; w8(b, 196, v); } else clr16(b, 224, 0x10);
    if (has("ColNo")) { b[224] |= 0x20; w8(b, 197, v); } else clr16(b, 224, 0x20);
    if (has("TrayHeight")) w32(b, 198, v);
    if (has("TrayStartX")) w32(b, 206, v);
    if (has("TrayStartY")) w32(b, 210, v);
    if (has("TrayPitchX")) w32(b, 214, v);
    if (has("TrayPitchY")) w32(b, 218, v);
    if (has("TrayColNo")) w8(b, 222, v);
    if (has("TrayRowNo")) w8(b, 223, v);
}

}  // namespace

void compo(uint8_t* b, const Get& get, const GetText& text) {
    long v = 0;
    if (!get("Compo", "CompoID", v)) return;
    auto has = [&](const char* c) { return get("Compo", c, v); };
    auto col = [&](const char* c) { return raw(get, "Compo", c); };
    std::string d;
    text("Compo", "Description", d);
    d = ansi(d);
    std::memset(b + 47, 0, 31);
    std::memcpy(b + 47, d.data(), std::min<size_t>(d.size(), 31));
    b[77] = 0;
    w16(b, 78, has("PkgClassID") ? v : 0);
    w16(b, 113, 0);
    if (has("Width")) { b[113] |= 2; w32(b, 81, v); }
    if (has("Length")) { b[113] |= 4; w32(b, 85, v); }
    if (has("Height")) { b[113] |= 8; w32(b, 89, v); }
    w32(b, 93, has("ConHeight") ? v : 0);
    w16(b, 43, r16(b, 78) == 201 ? col("CompOrient") : 0);
    if (has("MoldlineWid")) w32(b, 97, v);
    if (has("MoldlineLen")) w32(b, 101, v);
    w32(b, 168, col("MoldLineOffsetX"));
    w32(b, 172, col("MoldLineOffsetY"));
    w8(b, 226, col("UseMech") != 0);
    w8(b, 392, col("UseLaser") != 0);
    w8(b, 246, col("UseVision") != 0);
    w8(b, 696, col("GX4PriorityLV"));
    w8(b, 1124, col("AdheTest") != 0);
    w8(b, 1333, col("SubNozzle") == 1);
    w8(b, 117, col("DataOk") + 1);
}

void feeder(uint8_t* b, const Get& get) {
    long v = 0;
    if (!get("Feeder", "CompoID", v)) return;   // no feeder row
    auto has = [&](const char* c) { return get("Feeder", c, v); };
    auto col = [&](const char* c) { return raw(get, "Feeder", c); };
    b[113] |= 1;
    switch (col("FdrTypeID")) {
        case 1: {   // tape
            w8(b, 80, 1);
            if (has("TapeTypeID")) { b[183] |= 1; w8(b, 176, v); } else clr16(b, 183, 1);
            if (has("TapeAngle")) { set16(b, 183, 2); w16(b, 180, v); } else clr16(b, 183, 2);
            if (has("Pitch")) w8(b, 178, v);
            const long tape = col("TapeTypeID");
            w8(b, 177, tape != 1 && tape != 6);
            w8(b, 179, tape_feed_code(b[176], b[178]));
            if (pitch_without_check(b[176], b[178])) { w8(b, 178, 0); w8(b, 179, 0); w8(b, 117, 1); }
            w8(b, 187, col("UseETFProfile"));
            w8(b, 188, col("ETFProfile"));
            if (has("Electric")) w8(b, 182, v == 1);
            break;
        }
        case 2:     // stick
            w8(b, 80, 2);
            if (has("StickTypeID")) { b[182] |= 1; w8(b, 176, v); } else clr16(b, 182, 1);
            w8(b, 177, col("StickFeedTime"));
            if (has("StickAngle")) { set16(b, 182, 2); w16(b, 178, v); } else clr16(b, 182, 2);
            if (has("StickChangerType")) w8(b, 181, v);
            w8(b, 180, stick_code(b[176]));
            break;
        case 4:     // tray
            tray(b, get);
            break;
        case 8:     // bulk
            w8(b, 80, 4);
            if (has("BulkTypeID")) { b[186] |= 1; w8(b, 176, v); } else clr16(b, 186, 1);
            w8(b, 177, col("BulkFeedTime"));
            if (has("BulkAngle")) { set16(b, 186, 2); w16(b, 178, v); } else clr16(b, 186, 2);
            w8(b, 180, 0);
            break;
        default:    // no feeder kind (the transfer-feeder kind needs the machine option)
            clr16(b, 113, 1);
            break;
    }
}

void mech(uint8_t* b, const Get& get) {
    long v = 0;
    if (!get("Mech", "CompoID", v)) return;
    auto has = [&](const char* c) { return get("Mech", c, v); };
    auto col = [&](const char* c) { return raw(get, "Mech", c); };
    w8(b, 227, 1);
    w16(b, 244, 0x707);
    if (has("NozzleNo")) { w16(b, 228, v + 1); set16(b, 244, 1); } else clr16(b, 244, 1);
    if (has("Vacuum")) { w8(b, 230, v); b[244] |= 2; } else clr16(b, 244, 2);
    b[244] |= 4;
    w8(b, 231, col("WithLead") + 1);
    w8(b, 232, col("CtrLvl"));
    if (has("PreRotate")) w8(b, 233, v);
    if (has("StrokeCtrl")) w8(b, 234, v);
    if (has("PreCtr")) w8(b, 235, v);
    w8(b, 236, col("HgtChkMtd"));
    w16(b, 242, col("SpeedID"));
    w16(b, 242, (r16(b, 242) & 0xFFCF) | ((b[242] >> 2) & 0x30));
}

void vision(uint8_t* b, const Get& get) {
    long v = 0;
    if (!get("Vision", "CompoID", v)) return;
    auto has = [&](const char* c) { return get("Vision", c, v); };
    auto col = [&](const char* c) { return raw(get, "Vision", c); };
    w8(b, 247, 1);
    if (has("LAICNozzleNo")) { b[382] |= 1; w16(b, 250, v + 1); } else clr16(b, 382, 1);
    if (has("LAICVacuum") && v >= 0x33) { w8(b, 252, v); b[382] |= 2; } else clr16(b, 382, 2);
    w8(b, 248, col("LAIC") != 0);
    w8(b, 249, col("IC") != 0);
    if (has("ICVacuum") && v >= 0x33) { w8(b, 253, v); b[382] |= 4; } else clr16(b, 382, 4);
    w16(b, 356, col("SpeedID"));
    w16(b, 354, col("SpeedID700"));
    w32(b, 262, col("CenterOffsetX"));
    w32(b, 266, col("CenterOffsetY"));
    b[382] |= 0x20 | 0x40;
    const long fmla = col("FMLA");
    w8(b, 278, fmla);
    if (!(fmla & 0xFF)) return;
    if (has("FMLANozzleNo")) { b[384] |= 1; w16(b, 279, v + 1); } else clr16(b, 384, 1);
    if (has("FMLAVacuum") && v >= 0x33) { b[384] |= 2; w8(b, 281, v); } else clr16(b, 384, 2);
    if (has("FMLAOutlineX")) { b[384] |= 4; w32(b, 282, v); } else clr16(b, 384, 4);
    if (has("FMLAOutlineY")) { b[384] |= 8; w32(b, 286, v); } else clr16(b, 384, 8);
    std::memcpy(b + 290, b + 262, 4);
    std::memcpy(b + 294, b + 266, 4);
    w16(b, 384, (r16(b, 384) & 0xFFEF) | ((b[382] >> 1) & 0x10));
    w16(b, 384, (r16(b, 384) & 0xFFDF) | ((b[382] >> 1) & 0x20));
    if (has("FMLAOffsetX")) { b[384] |= 0x40; w32(b, 298, v); } else clr16(b, 384, 0x40);
    if (has("FMLAOffsetY")) { b[384] |= 0x80; w32(b, 302, v); } else clr16(b, 384, 0x80);
    if (has("FMLAOffsetRot")) { b[385] |= 1; w16(b, 306, v); } else clr16(b, 384, 0x100);
}

void laser(uint8_t* b, const Get& get) {
    long v = 0;
    if (!get("Laser", "CompoID", v)) return;
    auto has = [&](const char* c) { return get("Laser", c, v); };
    auto col = [&](const char* c) { return raw(get, "Laser", c); };
    w8(b, 393, 1);
    w16(b, 603, 0);
    if (has("NozzleNo")) w16(b, 394, v + 1);
    if (has("Vacuum") && v >= 0x33) { w8(b, 396, v); b[604] |= 8; } else clr16(b, 603, 0x800);
    if (has("LaserAlgo")) w8(b, 397, (v & 0xFF) == 3 ? 101 : v);
    if (has("LaserHeight")) { b[603] |= 0x20; w32(b, 402, v); } else clr16(b, 603, 0x20);
    if (has("OffsetX")) { b[604] |= 2; w32(b, 410, v); } else clr16(b, 603, 0x200);
    if (has("OffsetY")) { b[604] |= 4; w32(b, 414, v); } else clr16(b, 603, 0x400);
    w32(b, 466, has("LaserHeightLnc60") ? v : 0);
    if (has("Fx3LaserHeightLnc60")) { w32(b, 478, v); b[652] |= 0x40; } else { w32(b, 478, 0); clr16(b, 652, 0x40); }
    if (has("Fx2LaserHeightLnc60")) { w32(b, 488, v); b[654] |= 0x08; } else { w32(b, 488, 0); clr16(b, 654, 0x08); }
    w16(b, 505, col("SpeedID"));
    w16(b, 603, (r16(b, 603) & 0xFFE7) | 0x10);
    w16(b, 503, col("SpeedID700"));
    w8(b, 422, has("LA") ? v : 0);
    w8(b, 423, has("GX4LA") ? v : 0);
    w16(b, 605, (r16(b, 605) & 0xFFFC) | ((b[603] >> 3) & 3));
    w16(b, 605, (r16(b, 605) & 0xFFFB) | ((b[603] >> 3) & 4));
    std::memcpy(b + 428, b + 402, 4);
    b[427] = b[397];
    w16(b, 424, has("GX4NozzleNo") ? v + 1 : 0);
    w16(b, 605, (r16(b, 605) & 0xFFF7) | ((r16(b, 603) >> 6) & 8));
    w16(b, 605, (r16(b, 605) & 0xFFEF) | ((r16(b, 603) >> 6) & 0x10));
    std::memcpy(b + 432, b + 410, 4);
    std::memcpy(b + 436, b + 414, 4);
    if (has("GX4OffsetRot")) { b[605] |= 0x20; w16(b, 440, v); } else clr16(b, 605, 0x20);
    w16(b, 442, has("GX4PreRotate") ? v : 0);
    if (has("GX4Vacuum") && v >= 0x33) { b[605] |= 0x40; w8(b, 426, v); } else clr16(b, 605, 0x40);
}

namespace {
std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    s.erase(0, i);
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) s = s.substr(1, s.size() - 2);
    return s;
}
std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
// a plain decimal number; anything else (comments, units) reads as 0 like the machine software
double ini_number(const std::string& v) {
    if (v.empty() || v.find_first_not_of("0123456789.+-") != std::string::npos) return 0;
    return std::strtod(v.c_str(), nullptr);
}
// leading integer (GetPrivateProfileInt); none -> missing
bool ini_int(const std::string& v, int& out) {
    size_t i = 0;
    bool neg = false;
    if (i < v.size() && (v[i] == '-' || v[i] == '+')) neg = v[i++] == '-';
    if (i >= v.size() || !std::isdigit((unsigned char)v[i])) return false;
    long x = 0;
    while (i < v.size() && std::isdigit((unsigned char)v[i])) x = x * 10 + (v[i++] - '0');
    out = (int)(neg ? -x : x);
    return true;
}
// component class as the SubCompo tables name it (0 = not a table class)
int subcompo_class(int c) {
    switch (c) {
        case 1: case 2: case 3: case 6: case 8: case 11: case 12: case 13: case 14: case 101: case 102: case 103: case 104:
        case 105: case 106: case 107: case 109: case 120: case 201: case 202: case 203: case 301: case 302: case 303:
        case 999: return c;
        default: return 0;
    }
}
}  // namespace

bool SubCompo::load(const std::string& path) {
    entries.clear();
#if (defined(_MSVC_LANG) && _MSVC_LANG >= 202002L) || __cplusplus >= 202002L
    std::ifstream f(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);   // UTF-8 path
#else
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);   // UTF-8 path (C++17)
#endif
    if (!f) return false;
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::map<std::string, std::map<std::string, std::string>> ini;
    std::string sec, line;
    auto flush = [&]() {
        std::string l = trim(line);
        line.clear();
        if (l.empty() || l[0] == ';') return;
        if (l[0] == '[') { const size_t e = l.find(']'); sec = lower(l.substr(1, e == std::string::npos ? std::string::npos : e - 1)); return; }
        const size_t eq = l.find('=');
        if (eq == std::string::npos) return;
        ini[sec][lower(trim(l.substr(0, eq)))] = trim(l.substr(eq + 1));
    };
    for (char ch : text) { if (ch == '\n') flush(); else line += ch; }
    flush();
    int count = 0;
    if (!ini_int(ini["config"]["subcompocount"], count) || count <= 0) return false;
    count = std::min(count, 999);
    for (int i = 0; i < count; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "compotype_%03d", i);
        auto it = ini.find(name);
        if (it == ini.end()) break;
        auto& k = it->second;
        Entry e;
        if (!ini_int(k["compotype"], e.type) || !k.count("volumemax") || !k.count("volumemin") || !k.count("thicknessmax") ||
            !k.count("thicknessmin") || !k.count("subcomponame") || !ini_int(k["tableaccel"], e.accel))
            break;
        e.vmax = ini_number(k["volumemax"]);
        e.vmin = ini_number(k["volumemin"]);
        e.tmax = ini_number(k["thicknessmax"]);
        e.tmin = ini_number(k["thicknessmin"]);
        entries.push_back(e);
    }
    return !entries.empty();
}

int SubCompo::accel(const uint8_t* b) const {
    if (entries.empty()) return 0;
    int32_t w, l, h;
    std::memcpy(&w, b + 81, 4);
    std::memcpy(&l, b + 85, 4);
    std::memcpy(&h, b + 89, 4);
    const int cls = r16(b, 78);
    const double vol = w * 1e-4 * (l * 1e-4) * (h * 1e-4), th = h * 1e-4;
    for (size_t i = 1; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        if (subcompo_class(e.type) != cls) continue;
        if (vol != 0 && e.vmin <= vol && vol <= e.vmax && e.tmin <= th && th <= e.tmax) return e.accel;
    }
    return entries[0].accel;
}

bool expansion(uint8_t* b, const Get& get, const Lookup& lookup, const SubCompo* const accel[3]) {
    long v = 0;
    if (!get("Expansion", "CompoID", v)) return false;
    auto has = [&](const char* c) { return get("Expansion", c, v); };
    auto col = [&](const char* c) { return raw(get, "Expansion", c); };
    w8(b, 711, col("CompoCheck") != 0);
    w8(b, 712, col("PickPosAdjust") != 0);
    w8(b, 713, col("ScrapType"));
    w8(b, 706, col("Protect"));
    w8(b, 704, col("Retry"));
    w8(b, 705, col("Test") != 0);
    w16(b, 709, col("OverPick"));
    w16(b, 707, col("OverPlace"));
    w8(b, 715, col("CompoLayer"));
    w8(b, 714, col("AutoTeach"));
    if (has("GripNozID")) {
        const long id = v;
        long g = 0;
        if (lookup("GripNozzle", "GripNozID", id, "GripNozID", g)) {
            auto gl = [&](const char* c, long dflt) { long x = 0; return lookup("GripNozzle", "GripNozID", id, c, x) ? x : dflt; };
            w32(b, 25, gl("GripPosX", 0x7FFFFFFF));
            w32(b, 29, gl("GripPosY", 0x7FFFFFFF));
            w32(b, 33, gl("HorizClearance", 0x7FFFFFFF));
            w32(b, 37, gl("NozDirAtPick", 0x7FFFFFFF));
            w16(b, 41, gl("HighAdjust", 0x7FFF));
        }
    }
    if (has("OverPick2K")) w32(b, 721, v);
    if (has("OverPlace2K")) w32(b, 717, v);
    w32(b, 789, col("PickOffsetZ"));
    w32(b, 781, col("PickOffsetX"));
    w32(b, 785, col("PickOffsetY"));
    w32(b, 725, col("OverPlaceGx5"));
    w32(b, 729, col("OverPickGx5"));
    w8(b, 793, col("PickVacuumTiming"));
    w32(b, 733, col("OverPlaceFx1"));
    w32(b, 737, col("OverPickFx1"));
    const uint16_t a0 = r16(b, 1139), a1 = r16(b, 1141), a2 = r16(b, 1340);
    w16(b, 1139, col("TableAccel"));
    w16(b, 1141, col("TableAccelFx1R"));
    w32(b, 741, col("OverPlaceCX"));
    w32(b, 745, col("OverPickCX"));
    if ((col("FCType") & 0xFF) == 1) b[800] |= 1; else clr16(b, 800, 1);
    if (b[800] & 1) { w16(b, 802, col("FCOverPick")); w16(b, 804, col("FCOverPlace")); }
    w16(b, 806, col("PlaceVacOffset"));
    w8(b, 832, col("OCCPickPosAdjust") != 0);
    if (b[832] == 1 && !has("OCCPickPosAdjustID")) w8(b, 832, 0);
    if (col("WeakBlow")) b[896] |= 1; else clr16(b, 896, 1);
    if (col("BeaChip")) b[896] |= 2; else clr16(b, 896, 2);
    w16(b, 808, col("BlowStartCX"));
    w16(b, 810, col("BlowTimeCX"));
    w8(b, 794, (col("PumpVacuumTiming") & 0xFF) == 1);
    w32(b, 749, col("OverPlaceGx6"));
    w32(b, 753, col("OverPickGx6"));
    if ((col("FCTypeGx6") & 0xFF) == 1) b[816] |= 1; else clr16(b, 816, 1);
    if (b[816] & 1) { w16(b, 818, col("FCOverPickGx6")); w16(b, 820, col("FCOverPlaceGx6")); }
    w8(b, 795, col("ScrapTypeGx6"));
    {
        int32_t w, l;
        std::memcpy(&w, b + 81, 4);
        std::memcpy(&l, b + 85, 4);
        if (std::min(w, l) > 740000 && !b[795]) w8(b, 795, 3);
    }
    const bool two = b[1333] != 0;
    const char* sfx = two ? "_2" : "";
    auto c2 = [&](const char* base) { return col((std::string(base) + sfx).c_str()); };
    w32(b, two ? 761 : 757, c2("OverPickFx3"));
    w32(b, two ? 769 : 765, c2("OverPlaceFx3"));
    const int pk = two ? 906 : 898, pl = two ? 926 : 914;
    w8(b, pk, (c2("PickVacuumTimingFx3") & 0xFF) == 1);
    w16(b, pk + 2, c2("PickVacOnFx3"));
    w16(b, pk + 4, c2("PickVacCorrFx3"));
    w16(b, pk + 6, c2("PickHoldFx3"));
    w8(b, pl, (c2("PlaceVacuumTimingFx3") & 0xFF) == 1);
    w16(b, pl + 2, c2("PlaceVacOffFx3"));
    w16(b, pl + 4, c2("PlaceVacCorrFx3"));
    w16(b, pl + 6, c2("PlaceBlowOnFx3"));
    w16(b, pl + 8, c2("PlaceBlowTimeFx3"));
    w16(b, pl + 10, c2("PlaceHoldFx3"));
    w32(b, 777, col("OverPickFx2"));
    w32(b, 773, col("OverPlaceFx2"));
    w16(b, 1340, col("TableAccelFx2"));
    // unset accelerations: the machine software's default for the outline, else as they were
    const int at[3] = {1139, 1141, 1340};
    const uint16_t was[3] = {a0, a1, a2};
    for (int k = 0; k < 3; ++k) {
        if (r16(b, at[k])) continue;
        const int d = accel && accel[k] ? accel[k]->accel(b) : 0;
        w16(b, at[k], d ? (uint16_t)d : was[k]);
    }
    return true;
}

namespace {
// One machine-specific table: an enable byte, then (when it is 1) nozzle / vacuum entries with set flags and plain
// byte columns. A NULL plain byte reads as 0xFF.
enum class K { Nozzle, Vacuum, Byte, Long, Var8, Var16, Var32, VarBool };
struct Item { const char* col; K kind; int dest; int flag_word; uint16_t flag_bit; };
struct SubTable {
    const char* table;
    const char* enable;
    int enable_at;
    std::initializer_list<Item> items;
};
const SubTable kVisionTables[] = {
    {"VisionGx5", "Gx5FMLA", 308, {{"NozzleNo", K::Nozzle, 309, 386, 1}, {"Vacuum", K::Vacuum, 311, 386, 2},
        {"SpdXY", K::Byte, 358}, {"SpdTheta", K::Byte, 359}, {"SpdPlaceUpZ", K::Byte, 360}, {"SpdPickDownZ", K::Byte, 361},
        {"SpdPickUpZ", K::Byte, 362}, {"SpdPlaceDownZ", K::Byte, 363}}},
    {"VisionCX", "CXVision", 312, {{"NozzleNo", K::Nozzle, 313, 388, 1}, {"Vacuum", K::Vacuum, 315, 388, 2},
        {"SpdXY", K::Byte, 366}, {"SpdTheta", K::Byte, 367}, {"SpdPlaceUpZ", K::Byte, 368}, {"SpdPickDownZ", K::Byte, 369},
        {"SpdPickUpZ", K::Byte, 370}, {"SpdPlaceDownZ", K::Byte, 371}}},
    {"VisionGx6", "Gx6Vision", 316, {{"NozzleNo", K::Nozzle, 317, 390, 1}, {"Vacuum", K::Vacuum, 323, 390, 4},
        {"PumpVacuum", K::Vacuum, 324, 390, 8}, {"SpdXY", K::Byte, 374}, {"SpdTheta", K::Byte, 375},
        {"SpdPlaceUpZ", K::Byte, 376}, {"SpdPickDownZ", K::Byte, 377}, {"SpdPickUpZ", K::Byte, 378},
        {"SpdPlaceDownZ", K::Byte, 379}, {"CdsHight", K::Var32, 325}}},
};
const SubTable kLaserTables[] = {
    {"LaserGx5", "Gx5LA", 444, {{"NozzleNo", K::Nozzle, 445, 607, 1}, {"Vacuum", K::Vacuum, 447, 607, 2},
        {"SpdXY", K::Byte, 507}, {"SpdThetaInLA", K::Byte, 513}, {"SpdTheta", K::Byte, 508}, {"SpdPlaceUpZ", K::Byte, 509},
        {"SpdPickDownZ", K::Byte, 510}, {"SpdPickUpZ", K::Byte, 511}, {"SpdPlaceDownZ", K::Byte, 512}}},
    {"LaserFX1", "FX1LA", 465, {{"NozzleNo", K::Nozzle, 613, 624, 1}, {"Vacuum", K::Vacuum, 615, 624, 2},
        {"SpdXY", K::Byte, 616}, {"SpdThetaInLA", K::Byte, 622}, {"SpdTheta", K::Byte, 617}, {"SpdPlaceUpZ", K::Byte, 618},
        {"SpdPickDownZ", K::Byte, 619}, {"SpdPickUpZ", K::Byte, 620}, {"SpdPlaceDownZ", K::Byte, 621},
        {"SpdPickDownZFX1R", K::Byte, 632}, {"SpdPickUpZFX1R", K::Byte, 633}}},
    {"LaserCX", "CXLA", 448, {{"NozzleNo", K::Nozzle, 449, 609, 1}, {"Vacuum", K::Vacuum, 451, 609, 2},
        {"SpdXY", K::Byte, 515}, {"SpdThetaInLA", K::Byte, 521}, {"SpdTheta", K::Byte, 516}, {"SpdPlaceUpZ", K::Byte, 517},
        {"SpdPickDownZ", K::Byte, 518}, {"SpdPickUpZ", K::Byte, 519}, {"SpdPlaceDownZ", K::Byte, 520}}},
    {"LaserGx6", "Gx6LA", 452, {{"NozzleNo", K::Nozzle, 453, 611, 1}, {"Vacuum", K::Vacuum, 459, 611, 4},
        {"PumpVacuum", K::Vacuum, 460, 611, 8}, {"CompoShape", K::Byte, 461}, {"SpdXY", K::Byte, 523},
        {"SpdThetaInLA", K::Byte, 529}, {"SpdTheta", K::Byte, 524}, {"SpdPlaceUpZ", K::Byte, 525},
        {"SpdPickDownZ", K::Byte, 526}, {"SpdPickUpZ", K::Byte, 527}, {"SpdPlaceDownZ", K::Byte, 528},
        {"DetailFlag", K::Byte, 462}, {"LaserSelect", K::Byte, 463}}},
    {"LaserFx3", "Fx3LA", 470, {{"NozzleNo", K::Nozzle, 471, 652, 1}, {"NozzleNo_2", K::Nozzle, 473, 652, 2},
        {"PumpVacuum", K::Vacuum, 475, 652, 4}, {"PumpVacuum_2", K::Vacuum, 476, 652, 8}, {"CompoShape", K::Byte, 477}}},
    {"LaserFx2", "FX2LA", 482, {{"NozzleNo", K::Nozzle, 483, 654, 1}, {"Vacuum", K::Vacuum, 485, 654, 2},
        {"PumpVacuum", K::Vacuum, 486, 654, 4}, {"CompoShape", K::Byte, 487}, {"SpdXY", K::Byte, 595},
        {"SpdThetaInLA", K::Byte, 601}, {"SpdTheta", K::Byte, 596}, {"SpdPlaceUpZ", K::Byte, 597},
        {"SpdPickDownZ", K::Byte, 598}, {"SpdPickUpZ", K::Byte, 599}, {"SpdPlaceDownZ", K::Byte, 600}}},
};
// LaserFx3 speed sets: the second set is used with the sub nozzle (+1333)
const Item kFx3Speeds[2][13] = {
    {{"PickSpdUpZ", K::Var8, 533}, {"PickSpdDownZ", K::Var8, 534}, {"PickZ2StepUse", K::VarBool, 543},
     {"PickZ2StepUp", K::Var32, 545}, {"PickZ2StepDown", K::Var32, 549}, {"PlaceSpdXY", K::Var8, 563},
     {"PlaceSpdTheta", K::Var8, 564}, {"PlaceSpdUpZ", K::Var8, 565}, {"PlaceSpdDownZ", K::Var8, 566},
     {"PlaceSpdThetaInLA", K::Var8, 567}, {"PlaceZ2StepUse", K::VarBool, 575}, {"PlaceZ2StepUp", K::Var32, 577},
     {"PlaceZ2StepDown", K::Var32, 581}},
    {{"PickSpdUpZ_2", K::Var8, 539}, {"PickSpdDownZ_2", K::Var8, 540}, {"PickZ2StepUse_2", K::VarBool, 553},
     {"PickZ2StepUp_2", K::Var32, 555}, {"PickZ2StepDown_2", K::Var32, 559}, {"PlaceSpdXY_2", K::Var8, 569},
     {"PlaceSpdTheta_2", K::Var8, 570}, {"PlaceSpdUpZ_2", K::Var8, 571}, {"PlaceSpdDownZ_2", K::Var8, 572},
     {"PlaceSpdThetaInLA_2", K::Var8, 573}, {"PlaceZ2StepUse_2", K::VarBool, 585}, {"PlaceZ2StepUp_2", K::Var32, 587},
     {"PlaceZ2StepDown_2", K::Var32, 591}},
};

int item_size(K k) { return k == K::Nozzle || k == K::Var16 ? 2 : k == K::Long || k == K::Var32 ? 4 : 1; }

void put_item(uint8_t* b, const Item& it, const char* table, const Get& get) {
    long v = 0;
    const bool set = get(table, it.col, v);
    switch (it.kind) {
        case K::Nozzle:
            if (set) { w16(b, it.dest, v + 1); set16(b, it.flag_word, it.flag_bit); } else clr16(b, it.flag_word, it.flag_bit);
            break;
        case K::Vacuum:
            if (set && (v & 0xFF) >= 0x33) { w8(b, it.dest, v); set16(b, it.flag_word, it.flag_bit); }
            else clr16(b, it.flag_word, it.flag_bit);
            break;
        case K::Byte: w8(b, it.dest, set ? v : 0xFF); break;
        case K::Long: w32(b, it.dest, set ? v : 0x7FFFFFFF); break;
        case K::Var8: w8(b, it.dest, set ? v : 0); break;
        case K::Var16: w16(b, it.dest, set ? v : 0); break;
        case K::Var32: w32(b, it.dest, set ? v : 0); break;
        case K::VarBool: w8(b, it.dest, set && (v & 0xFF) == 1); break;
    }
}

void clear_item(uint8_t* b, const Item& it) {
    std::memset(b + it.dest, 0, (size_t)item_size(it.kind));
    if (it.kind == K::Nozzle || it.kind == K::Vacuum) clr16(b, it.flag_word, it.flag_bit);
}

void sub_table(uint8_t* b, const SubTable& t, const Get& get, uint8_t sub_nozzle) {
    long v = 0;
    if (!get(t.table, "CompoID", v)) return;
    const bool en = get(t.table, t.enable, v) && (v & 0xFF) == 1;
    w8(b, t.enable_at, en ? 1 : 0);
    if (!en) return;
    for (const Item& it : t.items) put_item(b, it, t.table, get);
    if (!std::strcmp(t.table, "LaserFx3"))
        for (const Item& it : kFx3Speeds[sub_nozzle ? 1 : 0]) put_item(b, it, t.table, get);
}
}  // namespace

void machine_tables(uint8_t* b, const Get& get) {
    for (const SubTable& t : kVisionTables) { b[t.enable_at] = 0; for (const Item& it : t.items) clear_item(b, it); }
    for (const SubTable& t : kLaserTables) { b[t.enable_at] = 0; for (const Item& it : t.items) clear_item(b, it); }
    for (const auto& set : kFx3Speeds) for (const Item& it : set) clear_item(b, it);
    std::memset(b + 880, 0, 13);
    if (b[246]) for (const SubTable& t : kVisionTables) sub_table(b, t, get, b[1333]);
    if (b[392]) for (const SubTable& t : kLaserTables) sub_table(b, t, get, b[1333]);
    // fluxer
    long v = 0;
    if (get("Fluxer", "CompoID", v)) {
        if (get("Fluxer", "FluxerUse", v) && v) {
            w8(b, 880, 1);
            auto col = [&](const char* c, long dflt) { long x = 0; return get("Fluxer", c, x) ? x : dflt; };
            w8(b, 881, col("FluxerMode", 0xFF));
            w16(b, 882, col("FluxerLoad", 0x7FFF));
            w32(b, 884, col("FluxerStroke", 0x7FFFFFFF));
            w16(b, 888, col("FluxerBottomDeadCenter", 0));
            w8(b, 890, col("FluxerSpeedUpZ", 0xFF));
            w8(b, 891, col("FluxerSpeedDownZ", 0xFF));
            w8(b, 892, col("FluxerTransPos", 0xFF));
        } else w8(b, 880, 0);
    }
}

namespace {
uint8_t reverse_bits(uint8_t v, int n) {
    uint8_t r = 0;
    for (int i = 0; i < n; ++i) if (v & (1 << i)) r |= (uint8_t)(1 << (n - 1 - i));
    return r;
}
}  // namespace

long vision_control(uint8_t* c, const uint8_t* b4, const Get& get, const Lookup& lookup) {
    long v = 0;
    w8(c, 0, 1);
    if (!get("VisionData", "CompoID", v)) return 0;
    auto has = [&](const char* k) { return get("VisionData", k, v); };
    auto col = [&](const char* k) { return raw(get, "VisionData", k); };
    w8(c, 0, col("DataOk") + 1);
    if (has("Contrast")) w8(c, 1, v);
    // the four lead sides (bottom, right, top, left); a connector with an orientation uses the bottom set on top
    const uint16_t cls = r16(b4, 78);
    const bool oriented = cls == 201 && (int16_t)r16(b4, 43) > 0;
    const long ids[4] = {oriented ? 0 : col("BottomID"), col("RightID"), oriented ? col("BottomID") : col("TopID"), col("LeftID")};
    for (int s = 0; s < 4; ++s) {
        if (!ids[s]) continue;
        long x = 0;
        if (!lookup("Side", "SideID", ids[s], "SideID", x)) continue;
        uint8_t* d = c + 2 + 27 * s;
        auto sv = [&](const char* k, long& out) { return lookup("Side", "SideID", ids[s], k, out); };
        if (sv("LeadLength", x)) w32(d, 4, x); else w32(d, 4, 0);
        if (sv("LeadPitch", x)) w32(d, 0, x); else w32(d, 4, 0);
        if (sv("LeadWidth", x)) w32(d, 8, x); else w32(d, 8, 0);
        auto rawv = [&](const char* k) { return raw_ref(lookup, "Side", "SideID", ids[s], k); };
        w16(d, 12, rawv("Count"));
        w16(d, 15, rawv("LackedStart1"));
        w16(d, 19, rawv("LackedStart2"));
        w16(d, 23, rawv("LackedStart3"));
        w16(d, 17, rawv("LackedCount1") & 0xFF);
        w16(d, 21, rawv("LackedCount2") & 0xFF);
        w16(d, 25, rawv("LackedCount3") & 0xFF);
        if (sv("Profile", x)) w8(d, 14, x); else w8(d, 14, 0);
    }
    if (has("LeadPitch")) w32(c, 2, v);
    if (has("BendLevel")) w8(c, 110, v);
    if (has("ViewingField")) w8(c, 112, v);
    if (has("LightCorrect")) w8(c, 174, v);
    const long pattern = col("PatternID");
    if (has("SubClass")) w8(c, 137, reverse_bits((uint8_t)v, cls == 251 ? 6 : 8));
    if (has("DiaCheck")) w8(c, 127, v);
    if (has("DiaChkLvl")) w8(c, 128, v);
    if (has("DefCheck")) w8(c, 129, v);
    if (has("DefChkLvl")) w8(c, 130, v);
    if (has("TopLeftLead")) w8(c, 175, v);
    w8(c, 175, c[175] * 16);
    if (has("TopRightLead")) w8(c, 175, c[175] + v);
    if (cls == 202 || cls == 203) {
        if (has("BottomLeftLead")) w8(c, 176, v);
        w8(c, 176, c[176] * 16);
        if (has("BottomRightLead")) w8(c, 176, c[176] + v);
    }
    if (has("LeadPosFlg")) w8(c, 171, v);
    if (has("LeadREdgePosX")) w32(c, 155, v);
    if (has("LeadREdgePosY")) w32(c, 159, v);
    // coplanarity set
    if (has("CoplaID")) {
        const long id = v;
        long x = 0;
        if (lookup("CoplaGX4", "CoplaID", id, "CoplaID", x)) {
            auto cg = [&](const char* k, long dflt) { long y = 0; return lookup("CoplaGX4", "CoplaID", id, k, y) ? y : dflt; };
            uint8_t* d = c + 609;
            w8(d, 0, cg("Check", 0xFF));
            if (d[0]) {
                w8(d, 1, cg("TermCoat", 0xFF));
                w8(d, 2, cg("Power", 0xFF));
                w8(d, 4, cg("ScanMode", 0xFF));
                w32(d, 32, cg("CoplaOffset", 0x7FFFFFFF));
                w32(d, 16, cg("CoplaTolerance", 0x7FFFFFFF));
                w32(d, 28, cg("MeasHgtOffset", 0x7FFFFFFF));
                w32(d, 20, cg("TermSizeX", 0x7FFFFFFF));
                w32(d, 24, cg("TermSizeY", 0x7FFFFFFF));
                w16(d, 8, cg("DarkThreshLvl", 0x7FFF));
                w8(d, 3, cg("AGCLevel", 0xFF));
            }
        }
    }
    w8(c, 657, col("QFNBaseStyle"));
    return pattern;
}

namespace {
struct VisionSet { const char *vcs, *ctrl, *pattern, *div; int at7, at18; bool gx4; };
const VisionSet kVisionSets[4] = {
    {"GX4Vision", "LightControl", "LightPattern", "LightDivAsgn", 179, 0, true},
    {"VCSControlGx5", "LightControlGx5", "LightPatternGx5", "LightDivAsgnGx5", 293, 1296, false},
    {"VCSControlCX", "LightControlCX", "LightPatternCX", "LightDivAsgnCX", 393, 2592, false},
    {"VCSControlGx6", "LightControlGx6", "LightPatternGx6", "LightDivAsgnGx6", 493, 3888, false},
};
// GX4 light patterns kept per light type / style (other machines keep every pattern)
bool gx4_pattern_used(uint8_t type, uint8_t style, uint8_t device) {
    switch (type) {
        case 0: return (device >= 6 && device <= 10) || device == 16;
        case 1: return device == 1 || device == 3 || device == 5;
        case 2:
            if (device == 7 || device == 8) return style != 48 && style != 49;
            if (device == 11 || device == 12) return style != 32 && style != 33;
            return device == 16;
        default: return false;
    }
}
}  // namespace

void vision_sets(uint8_t* b7, uint8_t* b18, const Get& get, const RowCount& count, const GetRow& row) {
    for (const VisionSet& vs : kVisionSets) {
        long v = 0;
        if (!get(vs.vcs, "CompoID", v)) continue;
        uint8_t local[1052] = {0};
        uint8_t* a = vs.gx4 ? b7 + vs.at7 : local + 179;
        auto byte = [&](const char* c, int o) { long x = 0; w8(a, o, get(vs.vcs, c, x) ? x : 0xFF); };
        auto lng = [&](const char* c, int o) { long x = 0; w32(a, o, get(vs.vcs, c, x) ? x : 0x7FFFFFFF); };
        auto sht = [&](const char* c, int o) { long x = 0; w16(a, o, get(vs.vcs, c, x) ? x : 0x7FFF); };
        byte("DataOk", 0);
        w8(a, 0, a[0] + 1);
        w8(a, 4, 1);
        byte("DPXDivNum", 5); byte("DPYDivNum", 6); byte("DPZDivNum", 7);
        lng("DPXPitch", 8); lng("DPYPitch", 12); lng("DPZPitch", 16);
        byte("VCSAuto", 20); byte("VCSSelect", 21);
        byte("VCSAble1", 22); byte("VCSAble2", 23); byte("VCSAble3", 24); byte("VCSAble4", 25);
        lng("CentOffsetX", 36); lng("CentOffsetY", 40); lng("VCSFcsHgtOffset", 44);
        lng("PlaceOffsetX", 48); lng("PlaceOffsetY", 52); lng("PlaceOffsetZ", 56); lng("PlaceOffsetTh", 60);
        byte("MtnPreRotateUse", 80); byte("MtnPreRotateSpeed", 81); sht("MtnPreRotateAngle", 82); lng("MtnPreRotateZPos", 84);
        byte("MtnRecogUse", 88); byte("MtnRecogXYSpeed", 89); byte("MtnRecogThSpeed", 90); byte("MtnRecogZSpeed", 91);
        sht("MtnRecogRotAngle", 92); lng("MtnRecogZPos", 94);
        if (vs.gx4) { byte("HgtInspCheck", 98); lng("HgtInspCoplTol", 100); lng("HgtInspCopOffset", 104); }
        // light control + its rows
        uint8_t l18[1296] = {0};
        uint8_t* d = vs.gx4 ? b18 + vs.at18 : l18;
        if (get(vs.ctrl, "CompoID", v)) {
            auto lb = [&](const char* c, int o) { long x = 0; w8(d, o, get(vs.ctrl, c, x) ? x : 0xFF); };
            lb("DataOk", 0);
            w8(d, 0, d[0] + 1);
            lb("LightDivideNo", 4);
            lb("LightTypeId", 264);
            lb("LightStyleId", 265);
            lb("ControlAC", 266);
            const size_t nd = count(vs.div);
            if (nd) {
                int used = 0;
                for (size_t r = 0; r < nd; ++r) {
                    long s = 0;
                    if (!row(vs.div, r, "SequenceNo", s) || (s & 0xFF) > 31) continue;
                    ++used;
                    const int o = 8 * (int)(s & 0xFF);
                    auto rb = [&](const char* c, int off) { long x = 0; w8(d, off, row(vs.div, r, c, x) ? x : 0xFF); };
                    rb("ByUse", o + 8); rb("XDivPhaseNo", o + 9); rb("YDivPhaseNo", o + 10); rb("ZDivPhaseNo", o + 11);
                    rb("LightPatternLink", o + 12);
                }
                w8(d, 4, used);
            }
            const size_t np = count(vs.pattern);
            for (size_t r = 0; r < np; ++r) {
                long p = 0, dev = 0;
                row(vs.pattern, r, "PattnPhaseNum", p);
                if ((p & 0xFF) >= 4) continue;
                row(vs.pattern, r, "CtrlDeviceNum", dev);
                if ((dev & 0xFF) >= 32) continue;
                if (vs.gx4 && !gx4_pattern_used(d[264], d[265], (uint8_t)dev)) continue;
                const int o = 272 + 8 * (int)((dev & 0xFF) + 32 * (p & 0xFF));
                w8(d, o, 1);
                auto rb = [&](const char* c, int off) { long x = 0; w8(d, off, row(vs.pattern, r, c, x) ? x : 0xFF); };
                rb("OnOff", o + 1); rb("Volume", o + 2); rb("DeviceIndexNo", o + 3);
            }
        }
        if (!vs.gx4) {
            std::memcpy(b7 + vs.at7, local + 179, 100);
            std::memcpy(b18 + vs.at18, l18, 1296);
        }
    }
}

void bga(uint8_t* c, std::vector<uint8_t>& out, const uint8_t* b4, long pid, const Lookup& lookup, const LookupText& text) {
    out.clear();
    const uint16_t cls = r16(b4, 78);
    if (!(cls == 107 || cls == 108 || cls == 110) || pid <= 0) return;
    long v = 0;
    if (!lookup("BGA", "PatternID", pid, "PatternID", v)) return;
    auto col = [&](const char* k) { return raw_ref(lookup, "BGA", "PatternID", pid, k); };
    w8(c, 121, col("PatternType"));
    w8(c, 122, col("ColNum"));
    w8(c, 123, col("LineNum"));
    w8(c, 124, col("Peripheral"));
    w16(c, 125, col("BallNum"));
    w8(c, 136, col("BaseStyle"));
    if (c[121] != 0xFF) return;
    // custom pattern: the ball grid
    out.assign(12830, 0);
    uint8_t* g = out.data();
    const int cols = col("ColNum") & 0xFF, rows = col("LineNum") & 0xFF;
    std::string name, grid;
    text("BGA", "PatternID", pid, "PattName", name);
    text("BGA", "PatternID", pid, "Array", grid);
    w8(g, 1, cols);
    w8(g, 2, rows);
    // the name as the database read leaves it: the ANSI text over its own UTF-16 copy (the bytes after the NUL keep
    // the wide characters)
    {
        const std::string a = ansi(name);
        std::vector<uint8_t> buf(2 * a.size() + 2, 0);
        for (size_t q = 0; q < a.size(); ++q) buf[2 * q] = (uint8_t)a[q];
        std::memcpy(buf.data(), a.c_str(), a.size() + 1);
        std::memcpy(g + 4, buf.data(), std::min<size_t>(buf.size(), 26));
    }
    const int wc = std::min(cols, 160), wr = std::min(rows, 160);
    size_t at = 0;
    int balls = 0;
    for (int r = 0; r < wr; ++r)
        for (int k = 0; k < wc; ++k) {
            const uint8_t ch = at < grid.size() ? (uint8_t)grid[at] : 0;
            const int d = (std::isdigit(ch) ? ch - '0' : ch - 87) & 0xF;
            const int o = 30 + 2 * (40 * r + k / 4);
            const int sh = 4 * (k % 4);
            w16(g, o, (r16(g, o) & ~(0xF << sh)) | (d << sh));
            ++at;
            if (d == 1) ++balls;
        }
    w8(g, 0, at != 0);
    w16(c, 125, balls);
}

namespace {
// Lead-side fields that apply to a component class: per field group (pitch, length, width, count, profile, missing
// leads) one flag per side (bottom, right, top, left). Fields of a side whose flag is 0 are cleared.
struct SideUse { uint16_t cls; uint8_t f[24]; };
const SideUse kSideUse[] = {
    {1, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {3, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {6, {0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {2, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {8, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {11, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {12, {1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0}},
    {13, {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {14, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {101, {1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0}},
    {102, {1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1}},
    {103, {1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1}},
    {104, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1}},
    {105, {1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0}},
    {106, {1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1}},
    {107, {1, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1}},
    {108, {1, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1}},
    {109, {1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0}},
    {110, {1, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1}},
    {120, {1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1}},
    {201, {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0}},
    {202, {1, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0}},
    {203, {1, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0}},
    {204, {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0}},
    {251, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1}},
    {301, {1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1}},
    {302, {1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1}},
    {303, {1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1}},
    {501, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
};
}  // namespace

void vision_finish(uint8_t* b4, uint8_t* c, uint8_t* l) {
    const uint16_t cls = r16(b4, 78);
    if (cls == 1 || cls == 2 || cls == 3 || cls == 8 || cls == 11 || cls == 14 || cls == 999) {
        if (!c[0]) return;
        std::memset(c, 0, 1052);
        std::memset(l, 0, 5240);
    }
    if (!c[0]) return;
    for (const SideUse& u : kSideUse) {
        if (u.cls != cls) continue;
        for (int sd = 0; sd < 4; ++sd) {
            uint8_t* d = c + 2 + 27 * sd;
            if (!u.f[0 + sd]) w32(d, 0, 0);
            if (!u.f[4 + sd]) w32(d, 4, 0);
            if (!u.f[8 + sd]) w32(d, 8, 0);
            if (!u.f[12 + sd]) w16(d, 12, 0);
            if (!u.f[16 + sd]) w8(d, 14, 0);
            if (!u.f[20 + sd]) for (int k = 0; k < 3; ++k) { w16(d, 15 + 4 * k, 0); w16(d, 17 + 4 * k, 0); }
        }
        break;
    }
    auto s32 = [&](const uint8_t* p, int o) { int32_t x; std::memcpy(&x, p + o, 4); return x; };
    auto clamp = [&](int o, int lim, int b4_at, int b4_w) {
        const int32_t x = s32(c, o);
        if (x < -lim || x > lim) { w32(c, o, 0); if (b4_at) (b4_w == 4 ? w32(b4, b4_at, 0) : w16(b4, b4_at, 0)); }
    };
    clamp(227, 990000, 298, 4);
    clamp(231, 990000, 302, 4);
    clamp(235, 100000, 0, 0);
    clamp(239, 30000, 306, 2);
    clamp(341, 990000, 0, 0);
    clamp(345, 990000, 0, 0);
    clamp(349, 100000, 0, 0);
    clamp(353, 30000, 0, 0);
    if (!c[0]) return;
    auto drop = [&](int o) { if (l[o]) { l[o] = l[o + 1] = l[o + 2] = 0; } };
    // GX4 set: patterns the light type does not use
    for (int i = 0; i < 1024; i += 256) {
        const uint8_t t = l[264], st = l[265];
        if (t == 0) { drop(i + 360); drop(i + 368); }
        else if (t == 1) { drop(i + 328); drop(i + 336); drop(i + 360); drop(i + 368); }
        else if (t == 2) {
            if (st == 48 || st == 49) { drop(i + 328); drop(i + 336); }
            if (st == 32 || st == 33) { drop(i + 360); drop(i + 368); }
        }
    }
    for (int e = 0; e < 128; ++e) l[275 + 8 * e] = l[272 + 8 * e] ? (uint8_t)(e % 32) : 0;
    if (c[183]) {
        const uint8_t n = (uint8_t)(c[184] * c[185]);
        if (l[4] != n && n <= 4) l[4] = n;
        // default division assignments for a 1..4 division layout
        static const uint8_t def[4][4][3] = {
            {{1, 1, 1}}, {{1, 1, 1}, {1, 2, 1}}, {{1, 1, 1}, {1, 2, 1}, {1, 3, 1}}, {{2, 1, 1}, {2, 2, 1}, {1, 2, 1}, {1, 1, 1}}};
        if (n >= 1 && n <= 4)
            for (int k = 0; k < n; ++k) {
                uint8_t* d = l + 8 + 8 * k;
                if (!d[1] && !d[2] && !d[3]) { d[0] = 1; d[1] = def[n - 1][k][0]; d[2] = def[n - 1][k][1]; d[3] = def[n - 1][k][2]; }
            }
    }
    // GX5 set
    uint8_t* m = l + 1296;
    for (int p = 0; p < 4; ++p)
        for (int j = 0; j < 32; ++j) {
            const uint8_t t = m[264], st = m[265];
            bool keep;
            if (t == 0) keep = j >= 6 && j <= 9;
            else if (t == 1) keep = j >= 17 && j <= 20;
            else if (t == 2) {
                if (j == 7 || j == 8) keep = st == 32 || st == 33;
                else if (j == 11 || j == 12) keep = st == 48 || st == 49;
                else keep = false;
            } else keep = false;
            if (!keep) { uint8_t* e = m + 272 + 8 * (32 * p + j); if (e[0]) e[0] = e[1] = e[2] = 0; }
        }
    for (int e = 0; e < 128; ++e) m[275 + 8 * e] = m[272 + 8 * e] ? (uint8_t)(e % 32) : 0;
}

namespace {
// one row as the named-field reader sees it: NULL byte 0xFF, short 0x7FFF, long 0x7FFFFFFF, variant 0
struct RowReader {
    std::function<bool(const char*, long&)> val;
    void byte(const char* c, uint8_t* p) const { long v; *p = (uint8_t)(val(c, v) ? v : 0xFF); }
    void shrt(const char* c, uint8_t* p) const { long v; w16(p, 0, val(c, v) ? v : 0x7FFF); }
    void lng(const char* c, uint8_t* p) const { long v; w32(p, 0, val(c, v) ? v : 0x7FFFFFFF); }
    long var(const char* c) const { long v; return val(c, v) ? v : 0; }
};

// Element rows of one owner (ElementGroupID / ExtendArrayID = key) appended to elems; returns {first, count}
std::pair<uint16_t, uint16_t> read_elements(std::vector<uint8_t>& elems, const char* key_column, long key, const ElemDb& db) {
    const std::vector<size_t> rows = db.ref_rows("Element", key_column, key);
    if (rows.empty()) return {0xFFFF, 0};
    const uint16_t first = (uint16_t)(elems.size() / 112);
    uint16_t prev = 0xFFFF;
    for (size_t r : rows) {
        if (elems.size() / 112 >= 4096) break;
        const uint16_t at = (uint16_t)(elems.size() / 112);
        elems.resize(elems.size() + 112, 0);
        uint8_t* e = &elems[(size_t)at * 112];
        RowReader rd{[&](const char* c, long& v) { return db.ref_val("Element", r, c, v); }};
        e[1] = 1;
        rd.byte("DataOk", e);
        ++e[0];
        rd.byte("AnchorPos", e + 3);
        rd.byte("Polarity", e + 4);
        rd.byte("Algorithm", e + 5);
        rd.byte("RecognizeLayer", e + 6);
        const char* longs[12] = {"ElemOffsetX", "ElemOffsetY", "ElemOffsetZ", "ElemOffsetTheta", "DistElemOffsetX", "DistElemOffsetY",
                                 "DistElemOffsetZ", "DistElemOffsetTheta", "ElemSizeX", "ElemSizeY", "DistElemSizeX", "DistElemSizeY"};
        for (int k = 0; k < 12; ++k) rd.lng(longs[k], e + 24 + 4 * k);
        rd.byte("ShapeType", e + 2);
        const long shape = rd.var("ShapeID");
        struct S { int type; const char *table, *key; };
        static const S kShapes[] = {{1, "ShapeLead", "ShapeLeadID"}, {2, "ShapeLead", "ShapeLeadID"}, {3, "ShapeBall", "ShapeBallID"},
                                    {4, "ShapeLand", "ShapeLandID"}, {5, "ShapeMark", "ShapeMarkID"}, {6, "ShapeHole", "ShapeHoleID"},
                                    {7, "ShapeNonIF", "ShapeNonIFID"}, {8, "ShapeColumn", "ShapeColumnID"}, {9, "ShapeSide", "ShapeSideID"},
                                    {10, "ShapeCorner", "ShapeCornerID"}};
        for (const S& sh : kShapes) {
            if (sh.type != e[2]) continue;
            const std::vector<size_t> sr = db.ref_rows(sh.table, sh.key, shape);
            if (sr.empty()) break;
            RowReader sd{[&](const char* c, long& v) { return db.ref_val(sh.table, sr[0], c, v); }};
            auto insp = [&](int at0) {
                const char* n[8] = {"InspLenUse", "InspLenThresh", "InspSizeUse", "InspSizeThresh", "InspExistUse", "InspExistThresh",
                                    "InspLenBarUse", "InspLenBarThresh"};
                for (int k = 0; k < 8; ++k) sd.byte(n[k], e + at0 + k);
            };
            switch (sh.type) {
                case 1: case 2:
                    sd.byte("Profile", e + 72); sd.byte("Cutting", e + 73); sd.byte("Coating", e + 74);
                    sd.lng("CuttingLength", e + 80); sd.lng("CuttingWidth", e + 84);
                    sd.lng("FootSizeX", e + 96); sd.lng("FootSizeY", e + 100); sd.lng("DistFootSizeX", e + 104); sd.lng("DistFootSizeY", e + 108);
                    break;
                case 3: insp(80); break;
                case 4: sd.byte("MarkID", e + 72); insp(80); break;
                case 5:
                    sd.byte("MarkID", e + 72); sd.byte("InspCorUse", e + 80); sd.shrt("InspCorThresh", e + 84);
                    if (e[72] != 10) w32(e, 60, 0x7FFFFFFF);   // a mark other than type 10 has no Y size
                    break;
                case 6: sd.byte("MarkID", e + 72); break;
                case 8: sd.byte("MarkID", e + 72); sd.lng("Height", e + 80); break;
                case 10: sd.lng("IR", e + 80); break;
                default: break;
            }
            break;
        }
        w16(e, 12, prev);
        w16(e, 14, 0xFFFF);
        if (prev != 0xFFFF) w16(&elems[(size_t)prev * 112], 14, at);
        prev = at;
    }
    return {first, (uint16_t)(elems.size() / 112 - first)};
}
}  // namespace

bool elements(uint8_t* b19, std::vector<uint8_t>& groups, std::vector<uint8_t>& elems, std::vector<uint8_t>& arrays,
              const Get& get, const ElemDb& db) {
    long v = 0;
    if (!get("Gx4ExtendVision", "CompoID", v)) return false;
    RowReader hd{[&](const char* c, long& x) { return get("Gx4ExtendVision", c, x); }};
    b19[1] = 1;
    hd.byte("DataOk", b19);
    ++b19[0];
    hd.byte("ElemType", b19 + 2);
    hd.byte("ElemGroupUse", b19 + 4);
    hd.byte("ExtendArrayUse", b19 + 5);
    hd.byte("ExternalFileUse", b19 + 3);
    // element groups
    w16(b19, 20, 0xFFFF);
    const size_t ng = db.part_rows("ElementGroup");
    uint16_t prev = 0xFFFF;
    for (size_t r = 0; r < ng && r < 1024; ++r) {
        const uint16_t at = (uint16_t)(groups.size() / 170);
        groups.resize(groups.size() + 170, 0);
        uint8_t* g = &groups[(size_t)at * 170];
        RowReader rd{[&](const char* c, long& x) { return db.part_val("ElementGroup", r, c, x); }};
        g[1] = 1;
        rd.byte("DataOk", g);
        ++g[0];
        rd.byte("ElemGrpDim", g + 2);
        rd.shrt("NumInRow", g + 16);
        rd.shrt("NumInCol", g + 18);
        std::string nm;
        db.part_text("ElementGroup", r, "ElemDefName", nm);
        nm = ansi(nm);
        std::memcpy(g + 24, nm.c_str(), std::min<size_t>(nm.size() + 1, 146));
        const char* longs[12] = {"LocationX", "LocationY", "LocationZ", "LocationTheta", "DistLocationX", "DistLocationY",
                                 "DistLocationZ", "DistLocationTheta", "PitchX", "PitchY", "DistPitchX", "DistPitchY"};
        for (int k = 0; k < 12; ++k) rd.lng(longs[k], g + 56 + 4 * k);
        rd.byte("InspLocUse", g + 168);
        rd.byte("InspLocThresh", g + 169);
        const long gid = rd.var("ElementGroupID");
        const std::vector<size_t> mp = db.ref_rows("MissPattern", "ElementGroupID", gid);
        for (size_t k = 0; k < mp.size() && k < 4; ++k) {
            uint8_t* m = g + 104 + 16 * k;
            RowReader md{[&](const char* c, long& x) { return db.ref_val("MissPattern", mp[k], c, x); }};
            m[0] = 1;
            md.byte("RowUse", m + 1);
            if (m[1]) { md.shrt("RowStart", m + 8); md.shrt("RowCount", m + 10); }
            m[2] = (uint8_t)md.var("ColumnUse");
            if (m[2]) { md.shrt("ColumnStart", m + 12); md.shrt("ColumnCount", m + 14); }
        }
        const auto [first, count] = read_elements(elems, "ElementGroupID", gid, db);
        g = &groups[(size_t)at * 170];
        w16(g, 12, first);
        w16(g, 14, count);
        w16(g, 8, prev);
        w16(g, 10, 0xFFFF);
        if (prev != 0xFFFF) w16(&groups[(size_t)prev * 170], 10, at);
        else w16(b19, 20, at);
        prev = at;
    }
    w16(b19, 22, (uint16_t)(groups.size() / 170));
    // extended arrays
    w16(b19, 24, 0xFFFF);
    const size_t na = db.part_rows("ExtendArray");
    prev = 0xFFFF;
    for (size_t r = 0; r < na && r < 256; ++r) {
        const uint16_t at = (uint16_t)(arrays.size() / 3142);
        arrays.resize(arrays.size() + 3142, 0);
        uint8_t* a = &arrays[(size_t)at * 3142];
        RowReader rd{[&](const char* c, long& x) { return db.part_val("ExtendArray", r, c, x); }};
        a[1] = 1;
        rd.byte("DataOk", a);
        ++a[0];
        std::string nm;
        db.part_text("ExtendArray", r, "ElemDefName", nm);
        nm = ansi(nm);
        std::memcpy(a + 24, nm.c_str(), std::min<size_t>(nm.size() + 1, 31));
        rd.byte("InspBallLocUse", a + 56);
        rd.lng("InspBallLocXDist", a + 60);
        rd.lng("InspBallLocYDist", a + 64);
        const long aid = rd.var("ExtendArrayID");
        const std::vector<size_t> loc = db.ref_rows("ElemLoc", "ExtendArrayID", aid);
        size_t k = 0;
        for (; k < loc.size() && k < 256; ++k) {
            uint8_t* p = a + 70 + 12 * k;
            RowReader ld{[&](const char* c, long& x) { return db.ref_val("ElemLoc", loc[k], c, x); }};
            p[0] = 1;
            ld.lng("X", p + 4);
            ld.lng("Y", p + 8);
        }
        w16(a, 68, (uint16_t)k);   // the ball count is the number of locations
        const auto [first, count] = read_elements(elems, "ExtendArrayID", aid, db);
        a = &arrays[(size_t)at * 3142];
        w16(a, 12, first);
        w16(a, 14, count);
        w16(a, 8, prev);
        w16(a, 10, 0xFFFF);
        if (prev != 0xFFFF) w16(&arrays[(size_t)prev * 3142], 10, at);
        else w16(b19, 24, at);
        prev = at;
    }
    w16(b19, 26, (uint16_t)(arrays.size() / 3142));
    return true;
}

void vision_state(uint8_t* b4, uint8_t* c, bool elements_done) {
    const uint8_t was = c[0];
    // per side (bottom, right, top, left): count, pitch, length, width present
    bool cnt[4], pit[4], len[4], wid[4], extra = false;
    for (int i = 0; i < 4; ++i) {
        const uint8_t* d = c + 27 * i;
        auto nz32 = [&](int o) { return d[o] || d[o + 1] || d[o + 2] || d[o + 3]; };
        cnt[i] = d[14] || d[15];
        pit[i] = nz32(2);
        len[i] = nz32(6);
        wid[i] = nz32(10);
    }
    // fields the class does not need count as present: {count, pitch, length, width} side masks (bit i = side i)
    struct Need { std::initializer_list<int> cls; int cnt, pit, len, wid; };
    static const Need kNeed[] = {
        {{101, 109, 12, 105}, 0b1010, 0b1110, 0b1110, 0b1111},
        {{102, 106}, 0, 0b1110, 0b1110, 0b1111},
        {{103, 302, 303, 301}, 0, 0b1110, 0b1110, 0b1111},
        {{104}, 0b0101, 0b1110, 0b1101, 0b1111},
        {{201}, 0b1011, 0b1110, 0b1011, 0b1111},
        {{202, 203}, 0b1010, 0b1110, 0b1010, 0b1111},
        {{251, 501}, 0b1111, 0b1111, 0b1111, 0b1111},
        {{107, 108, 110}, 0, 0b1100, 0b1111, 0b1110},
        {{6}, 0b1111, 0b1111, 0b1110, 0b1110},
        {{120}, 0b1100, 0b1100, 0b1100, 0b1100},
        {{13}, 0b1111, 0b1111, 0, 0},
        {{204}, 0b1011, 0b1110, 0b1011, 0b1011},
    };
    const uint16_t cls = r16(b4, 78);
    for (const Need& n : kNeed) {
        bool hit = false;
        for (int x : n.cls) hit |= x == cls;
        if (!hit) continue;
        for (int i = 0; i < 4; ++i) {
            if (n.cnt >> i & 1) cnt[i] = true;
            if (n.pit >> i & 1) pit[i] = true;
            if (n.len >> i & 1) len[i] = true;
            if (n.wid >> i & 1) wid[i] = true;
        }
        extra = true;
    }
    const uint8_t kind = c[1];
    const bool pattern_ok = (kind == 2 || kind == 3 || kind == 4) ? c[121] != 0 : true;
    bool any = elements_done || pattern_ok || extra, all = pattern_ok && extra;
    for (int i = 0; i < 4; ++i) { any |= cnt[i] || pit[i] || len[i] || wid[i]; all &= cnt[i] && pit[i] && len[i] && wid[i]; }
    if (any) c[0] = 1;
    if (all && (cls != 501 || elements_done)) c[0] = 2;
    if (c[0] != was) b4[247] = c[0];
}

void inspection(uint8_t* b, const Get& get, const GetReal& real) {
    long v = 0;
    if (!get("Inspection", "CompoID", v)) return;
    auto has = [&](const char* c) { return get("Inspection", c, v); };
    auto col = [&](const char* c) { return raw(get, "Inspection", c); };
    const bool stand = has("ChipStandCheck");
    w8(b, 944, stand ? v : 0);
    if (b[944]) {
        if (b[244] & 2) {
            const long d = 5L * b[230] - 944;
            w8(b, 945, d < 0 ? -d : d);
            b[962] |= 1;
        }
        w32(b, 946, col("HChkX"));
        w32(b, 950, col("HChkY"));
        w32(b, 954, col("HChkZ"));
    }
    if (has("CoplaChk")) {
        w8(b, 964, v != 0);
        w32(b, 966, col("CoplaTolerance"));
        w32(b, 970, col("CoplaOffset"));
    } else w8(b, 964, 0);
    if (stand) {
        w8(b, 978, col("VerifyChk") != 0);
        w8(b, 980, col("VerifyType"));
        w8(b, 981, col("VerifyUnit"));
        double st = 0;
        if (!real("Inspection", "Standard", st)) st = -9.123e19;
        std::memcpy(b + 982, &st, 8);
        w16(b, 990, col("Max"));
        w16(b, 992, -col("Min"));
        w8(b, 994, col("Polarity_Dir_Insp"));
    } else w8(b, 978, 0);
    const bool dim = has("DimChkChk") && (r16(b, 78) == 251 ? b[246] == 1 : b[392] == 1);
    if (dim) {
        w8(b, 998, v);
        w32(b, 1000, col("DimChkWid"));
        w32(b, 1004, col("DimChkLen"));
        w16(b, 1008, col("MaxRangeX"));
        w16(b, 1010, col("MinRangeX"));
        w16(b, 1012, col("MaxRangeY"));
        w16(b, 1014, col("MinRangeY"));
    } else w8(b, 998, 0);
    w8(b, 1024, col("SOT_Dir_Insp"));
    w8(b, 1040, col("PitchChk"));
    if (has("PitchErrLvl")) w32(b, 1048, v);
    if (has("PwbHeightMeasure")) {
        w8(b, 1056, v != 0);
        if (b[1056] == 1) {
            if (has("PH_Point")) w8(b, 1057, v);
            if (has("PH_Level")) w32(b, 1058, v);
            if (has("PH_OffsetX")) w32(b, 1062, v);
            if (has("PH_OffsetY")) w32(b, 1066, v);
        }
    } else w8(b, 1056, 0);
    if (has("CmpHeightCheck")) {
        w8(b, 1070, v != 0);
        if (b[1070] == 1) {
            if (has("CH_Point")) w8(b, 1071, v);
            if (has("CH_Level")) w32(b, 1072, v);
            for (int i = 0; i < b[1071] && i < 4; ++i) {
                const std::string x = "CH_Offset" + std::to_string(i + 1) + "_X", y = "CH_Offset" + std::to_string(i + 1) + "_Y";
                if (has(x.c_str())) w32(b, 1076 + 8 * i, v);
                if (has(y.c_str())) w32(b, 1080 + 8 * i, v);
            }
        }
    } else w8(b, 1070, 0);
}

namespace {
int16_t s16(const uint8_t* b, int o) { return (int16_t)r16(b, o); }
int32_t s32(const uint8_t* b, int o) { int32_t x; std::memcpy(&x, b + o, 4); return x; }
bool bit(const uint8_t* b, int o, int n) { return (r16(b, o) >> n) & 1; }

bool check_recognition(uint8_t* b, bool fx2) {
    if (b[226]) b[227] = bit(b, 244, 0) && bit(b, 244, 1) && bit(b, 244, 2) ? 2 : 1;
    if (b[246]) {
        int n = b[249] ? bit(b, 382, 2) : 0;
        if (b[248] && bit(b, 382, 0) && bit(b, 382, 1)) ++n;
        if (b[278] && bit(b, 384, 0) && bit(b, 384, 1)) ++n;
        if (b[308] && bit(b, 386, 0) && bit(b, 386, 1)) ++n;
        if (b[312] && bit(b, 388, 0) && bit(b, 388, 1)) ++n;
        if (b[316] && bit(b, 390, 0) && bit(b, 390, 2) && bit(b, 390, 3)) ++n;
        if (n) {
            b[247] = 2;
            if (b[248] && !(bit(b, 382, 0) && bit(b, 382, 1))) b[247] = 1;
        } else b[247] = 1;
    }
    if (b[392]) {
        int ok = 0, used = 0;
        // (enable byte, nozzle word, flag word, flag bits that must be set)
        struct L { int on, noz, flags; uint16_t need; };
        const L ls[] = {{422, 394, 603, 0x800}, {423, 424, 605, 0x40}, {444, 445, 607, 0x2}, {465, 613, 624, 0x2},
                        {448, 449, 609, 0x2}, {452, 453, 611, 0xC}, {470, 471, 652, 0x4}};
        for (const L& l : ls) {
            if (!b[l.on]) continue;
            ++used;
            if (r16(b, l.noz) && (r16(b, l.flags) & l.need) == l.need) ++ok;
        }
        if (fx2 && b[482]) {
            ++used;
            if (r16(b, 483) && (r16(b, 654) & 0x6)) ++ok;
        }
        b[393] = ok && ok == used ? 2 : 1;
    }
    return (!b[226] || b[227] != 1) && (!b[246] || b[247] != 1) && (!b[392] || b[393] != 1) && (b[226] || b[246] || b[392]);
}

bool check_feeder(const uint8_t* b) {
    if (!bit(b, 113, 0)) return false;
    switch (b[80]) {
        case 1: return bit(b, 183, 0) && bit(b, 183, 1) && b[178];
        case 2: return bit(b, 182, 0) && bit(b, 182, 1) && b[177];
        case 3: {
            const uint16_t f = r16(b, 224);
            if ((f & 0x7F) != 0x7F || !(f & 0x100)) return false;
            return (b[176] == 1 && (f & 0x80)) || b[176] == 2 || b[176] == 4;
        }
        case 4: return bit(b, 186, 0) && bit(b, 186, 1);
        case 5: return bit(b, 184, 1);
        default: return false;
    }
}

bool check_options(const uint8_t* b) {
    int ok = 1;
    if (b[978]) {
        if (b[980]) {
            ok = b[981] && (unsigned)s16(b, 990) <= 100 && s16(b, 992) <= 0 && s16(b, 992) >= -100 ? 1 : -1;
            if (b[980] == 1) ok = 1;
        } else ok = -1;
    }
    if (ok == 1 && b[1040]) ok = s32(b, 1048) == -1 ? -1 : 1;
    if (ok == 1 && b[964]) ok = s32(b, 966) ? 1 : -1;
    if (ok == 1 && b[998]) {
        auto in = [](int v, int lo, int hi) { return v >= lo && v <= hi; };
        ok = in(s32(b, 1000), 100, 1500000) && in(s32(b, 1004), 100, 1500000) && in(s16(b, 1008), 1, 100) &&
             in(s16(b, 1010), 1, 100) && in(s16(b, 1012), 1, 100) && in(s16(b, 1014), 1, 100) ? 1 : -1;
    }
    if (ok == 1) ok = b[1056] == 1 || !b[1056] ? 1 : -1;
    if (ok == 1) ok = b[1070] == 1 || !b[1070] ? 1 : -1;
    return ok == 1;
}
}  // namespace

void check(uint8_t* b, bool fx2) {
    const bool a = check_recognition(b, fx2);
    b[117] = a && check_feeder(b) && check_options(b) ? 2 : 1;
}

}  // namespace pcbp::juki::rules
