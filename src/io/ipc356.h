#pragma once
// ============================================================================
// ipc356.h — IPC-D-356 / IPC-D-356A netlist import (pcbp::io).
//
// Reads the 80-column test records:
//   317 / 017  through-hole feature      327 / 027  surface-mount feature
//   (also 367 tooling holes, skipped)    P  JOB/UNITS/... parameters
//   C  comments                          999 end of file
// Columns (1-based): 4-17 net name, 21-26 refdes, 27 '-', 28-31 pin,
// 32 'M' mid-point; then (scanned tolerant of misalignment) D####[P|U]
// drill, A## access side (00 both, 01 top, >=02 bottom/inner), X±######
// Y±###### position, X#### Y#### R### pad size/rotation, S# solder mask.
// UNITS: CUST 0 / CUST 2 = 0.0001 inch, CUST 1 / SI = 0.001 mm.
//
// Builds a Package per pin group (centroid = pad bbox centre, rotation 0,
// pin1 = pin "1" or "A1"), dedupes identical packages, creates Components.
// ============================================================================
#include "../core/project.h"
#include <string>
#include <vector>

namespace pcbp::io {

struct Ipc356Options {
    bool replace = true;                // clear comps first (packages named IPC356_* are replaced)
    bool create_components = true;
    bool include_vias = false;          // "VIA" refdes rows become pads of a VIA pseudo component
    bool dedupe_packages = true;
    double default_pad_mm = 0.5;        // when no size field
};

struct Ipc356Pin {
    std::string net, ref, pin;
    Vec2 pos;                           // mm
    double w = 0, h = 0, rot = 0, drill = 0;
    bool smt = false, plated = true, midpoint = false;
    int access = 0;                     // 0 both, 1 top, >=2 bottom (2-layer assumption)
    int line = 0;
};

struct Ipc356Net { std::string name; std::vector<std::string> nodes; };   // "U1-3"

struct Ipc356Result {
    std::string job, units;
    int records = 0, pins = 0, vias = 0, skipped = 0, components = 0, packages = 0;
    std::vector<Ipc356Pin> pin_list;
    std::vector<Ipc356Net> nets;
    std::vector<std::string> warnings;
    std::string describe() const;
};

bool parse_ipc356_text(const std::string& text, const Ipc356Options& opt, Ipc356Result& res, std::string& err);
bool import_ipc356_text(const std::string& text, Project& prj, const Ipc356Options& opt,
                        Ipc356Result& res, std::string& err);
bool import_ipc356_file(const std::string& path, Project& prj, const Ipc356Options& opt,
                        Ipc356Result& res, std::string& err);

} // namespace pcbp::io
