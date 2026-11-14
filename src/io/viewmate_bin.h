#pragma once
// ============================================================================
// viewmate_bin.h — ViewMate (Pro) job database reader (*.bin), portable.
//
// Clean-room implementation of the layout documented in specs/VIEWMATE_BIN.md
// (worked out from the shipped Samples/Sample1up.bin plus static analysis of
// the job load/save routines; no vendor code is used). A job is
// self-contained: geometry, D-code table, tool table, layer table (names,
// colour index, type, board-layer number, visible/negative flags) and a
// colour palette are all embedded. Original Gerber files are NOT referenced.
//
// to_gerber() renders one job layer as an RS-274X file in the database's own
// integer grid (1 uin / 0.1 uin, or 10 nm / 1 nm), so coordinates pass
// through without rounding.
//
// write_job() is the inverse used by the tests to build synthetic jobs (arcs,
// polygons, thermals ...) — it writes only the records the reader needs.
// ============================================================================
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace pcbp::io::vm {

// Aperture shape codes (database byte, letter = ViewMate D-code table letter).
enum Shape : uint8_t {
    ShRound = 0,        // C  w = diameter
    ShSquare = 1,       // S  w = side
    ShRect = 2,         // R  w x h
    ShObround = 3,      // O  w x h
    ShDrawnObround = 4, // X  w x h
    ShThermal = 5,      // H  w = outer, h = gap; extra: inner(u32), spokes(u32), angle(double)
    ShTarget = 6,       // T
    ShDiamond = 8,      // D  w x h
    ShBlank = 9,        // B
    ShPolyOutline = 11, // F
    ShUndefined = 12,   // ?
    ShDrawnTarget = 13, // Y
    ShCustom = 14,      // M  extra = custom aperture blob (not decoded)
    ShTool = 15,        // N  NC tool: w = diameter, h = tool number
    ShOctagon = 16,     // E
    ShRoundRect = 17,   // Q
};
const char* shape_letter(int shape);

struct Aperture {
    int dcode = 0;
    int shape = ShUndefined;
    int shape2 = 12;            // second shape byte (0x0c default, 0x0f for tools); kept, unused
    int32_t p0 = 0;             // first dword (extra-data length for H / M, else a shape parameter)
    int32_t w = 0, h = 0;       // database units
    std::vector<uint8_t> extra; // H / M only
    // thermal (decoded from extra)
    int32_t th_inner = 0; int th_spokes = 0; double th_angle = 0;
};

struct Layer {
    int index = 0;              // 1-based database layer number (element headers refer to it)
    std::string name;           // usually the imported file name
    int color = 0;              // palette index 1..15
    int state = 0;              // bit0 visible, bit1 negative, bit3 scratch (others kept)
    int type = 33;              // layer type code (see type_code())
    int bnum = 0;               // board layer number
    std::string raw;            // attribute string as stored ("name=..\1color=..\1cfg=..")
    bool visible() const { return state & 1; }
    bool negative() const { return state & 2; }
};

struct Contour {
    // ints exactly as stored: vertex = x,y  or  arc vertex = 0x7FFFFFFF,cx,cy,r,flags,x,y
    // (the arc describes the segment FROM this vertex TO the next one; flags bit1 = CCW)
    std::vector<int32_t> words;
    uint32_t hdr1 = 0, flags = 0;
};

struct Element {
    enum Kind : uint8_t { Pad, Trace, Arc, Polygon, Route } kind = Pad;
    int layer = 0;              // database layer number
    int dcode = 0;
    bool clear = false;         // scratch (clear) polarity
    int32_t v[6] = {0, 0, 0, 0, 0, 0};
    // Pad: x,y   Trace: x1,y1,x2,y2   Arc: sx,sy,ex,ey (relative to centre), cx,cy — clockwise from s to e
    std::vector<Contour> contours;   // Polygon (first = outline, rest = holes) / Route (one path)
    int tool = 0;               // Route
    uint32_t attr = 0xFFFFFFC0u; // per-element u32 when the stream has it (tag 101)
    int group = 0;              // 0 = top level, else index+1 into Job::groups
};

struct Group {                  // text / step-repeat containers (children are flattened into elements)
    enum Kind : uint8_t { Text, StepRepeat } kind = Text;
    int32_t raw[10] = {};
    std::string s1, s2;         // Text: font name, text
    int parent = 0;             // enclosing group (index+1), 0 = top level
};

struct Job {
    int version = 0;            // "JOB V<n>"
    int units = 0;              // record 3 byte 0: 0/1 imperial base, >= 2 metric base
    int resolution = 100;       // record 34: 100 or 1000
    uint32_t flags = 0;         // record 22 (bit0 = high-resolution coordinates)
    double unit_mm = 0.0000254; // size of one database unit in mm (after low-res scaling)
    bool metric() const { return units >= 2; }
    int32_t bbox[4] = {0, 0, 0, 0};          // record 3 extent (x0,y0,x1,y1), database units
    std::vector<Layer> layers;               // in record order
    std::map<int, Aperture> apertures;       // by D-code
    std::vector<int32_t> tool_sizes;         // record 27: 256 diameters (index = tool number)
    uint32_t palette[16] = {};               // 0xAARRGGBB, index 0..15
    bool has_palette = false;
    std::vector<Element> elements;           // file order
    std::vector<Group> groups;
    std::map<int, int> unknown_records;      // tag -> count (skipped by length)
    std::vector<std::string> warnings;
    const Layer* layer(int index) const;
    // per-layer element counts
    int count(int layer, Element::Kind k) const;
};

// Signature check on the first bytes ("Lavenir Technology V2001 JOB V<n>\r\n").
bool is_job(const uint8_t* p, size_t n, int* version = nullptr);

bool read_job(const uint8_t* p, size_t n, Job& job, std::string& err);
bool read_job_file(const std::string& path, Job& job, std::string& err);

// Layer type code -> mnemonic ("CPU", "SKL", "DRI", "P01", ...) and a friendly name.
std::string type_code(int type);
std::string type_name(int type);

// RS-274X text for one database layer. `stats` (optional) receives warnings.
std::string to_gerber(const Job& job, int layer_index, std::vector<std::string>* warnings = nullptr);

// Synthetic writer (tests / sample construction). Writes version 12, tag 101 stream.
std::vector<uint8_t> write_job(const Job& job);

// Default ViewMate palette (used when the job has no record 28).
uint32_t default_palette(int index);

} // namespace pcbp::io::vm
