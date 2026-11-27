// juki_resize - a size change on an H8H component record (block 4, 1822 bytes), the way the HLC editor reacts when a
// part's width / length / height is typed in: the outline changes and the nozzle, vacuum and laser height that depend
// on it are worked out again. Shared by PCB Plotter and Juki Tool; standard library only.
//
// Which bytes are "nozzle / vacuum / laser height" is not hand-listed: it is measured from the research rules
// themselves (the bytes and bits that change when only those database columns change), so it stays in step with
// juki_research_rules. The new values come from the built-in rules (juki_builtin) for the record's class and the new
// size. Everything else in the record (feeder slots, speeds, vision data, inspection, ...) is kept.
// Laser fields are only touched on laser parts (+392), mechanical-centring fields only on mech parts (+226). Vision
// parts keep their nozzle (the editor's vision nozzle rules are not derived).
//
// The feeder is not changed by a resize; the suggested tape width / pitch are returned and apply_feeder() writes them.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pcbp::juki {

struct ComponentInfo {               // read from a record
    int cls = 0;                     // +78
    double x = 0, y = 0, h = 0;      // mm (+81 / +85 / +89)
    bool laser = false, mech = false, vision = false;
    int gx4_nozzle = 0;              // GX-4 head nozzle (500..), 0 = none
    int laser_nozzle = 0;            // older laser head nozzle (101..), 0 = none
    int mech_nozzle = 0;             // mechanical centring nozzle, 0 = none
    int vacuum = 0, gx4_vacuum = 0;  // vacuum level codes (0 = not set)
    int laser_height = 0;            // 1e-4 mm (+402)
    int feeder_kind = 0;             // +80: 1 tape, 2 stick, 3 tray, 4 bulk
    int tape_w = 0, pitch = 0;       // tape parts: tape width / pitch mm (0 = unknown)
    bool paper = false;
};
ComponentInfo component_info(const uint8_t* b4);
std::string tape_label(int tape_w, bool paper);    // "8 mm paper", "12 mm emboss"

struct ResizeResult {
    ComponentInfo before, after;
    int suggest_tape_w = 0, suggest_pitch = 0;      // the feeder the new size wants (tape parts)
    bool suggest_paper = false;
    bool feeder_differs = false;                    // the record's tape / pitch differs from the suggestion at all
    // worth a warning: the tape width is two or more standard widths off (a 56 mm feeder on a 2 mm part), or a laser
    // part has no nozzle / a house nozzle two or more sizes off (special nozzles, 501..503 and 509+, are left alone)
    bool feeder_off = false, nozzle_off = false;
    std::vector<std::string> changes;               // "nozzle 506 -> 504", ...
    std::string why;                                // rules used / why nothing changed
};

// x / y / h in mm (X / Y at 0 deg, height). false (+ why) for a size out of range.
bool resize_component(uint8_t* b4, double x, double y, double h, ResizeResult& r);
// Suggested nozzle etc. for a size without changing the record.
bool preview_resize(const uint8_t* b4, double x, double y, double h, ResizeResult& r);
// Tape feeder data (tape width / pitch mm, carrier) into a tape part's record. false when it is not a tape part.
bool apply_feeder(uint8_t* b4, int tape_w, int pitch, bool paper, std::string* why = nullptr);

}  // namespace pcbp::juki
