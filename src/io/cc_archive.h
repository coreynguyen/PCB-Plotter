#pragma once
// ============================================================================
// cc_archive.h — decoder for Aegis CircuitCAM graphics blobs (portable).
//
// CircuitCAM stores vector graphics (package renderings, artworks, visual-aid
// annotations, board / image outlines, pictures) in OLE columns as:
//
//   [optional "CC_COMPRESS" 32-byte header + raw DEFLATE]  ->  MFC CArchive
//
// The archive root object is serialised with obj->Serialize(ar) (no class tag);
// children use CArchive::WriteObject (0xFFFF new-class tag, 0x8000|n class refs)
// for archive versions < "1.8", or a private (type-id + body) scheme from "1.8".
// Coordinates are integer (< "2.3") or double (>= "2.3") logical units of
// 1 mil (0.001 inch), +Y up.  See specs/CPF_FORMAT.md for the full layout.
//
// Clean-room: written from independent format notes.
// ============================================================================
#include "../core/project.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pcbp::cc {

constexpr double kMmPerUnit = 0.0254;   // 1 CircuitCAM logical unit = 1 mil

// GRTYPE ids (IGrComp::GetType; also the type tag of the >= 1.8 child scheme)
enum class GrType : int32_t {
    Comp = 1, Group = 2, Polygon = 3, Rect = 4, Line = 5, PolyLine = 6, Ellipse = 7,
    Bitmap = 8, Arc = 9, Text = 10, Handles = 11, Selection = 12, BomBlock = 13,
    OleItem = 14, PolyArcLine = 15, EnhMetaFile = 16,
    BomCol = 100, BomEntry = 101, BomOb = 102,      // internal (CObject-only classes)
    Unknown = -1
};

// IGrLabels long[10] indices with known meaning (package renderings)
enum : int { kLabelPin = 4, kLabelRole = 5 };
// values of labels[kLabelRole] inside a package rendering
enum : int32_t { kRoleHighlight = 1, kRolePads = 2, kRoleArtwork = 3 };

// CGrComp attribute block (22 x int32 in stream order; COLORREF = 0x00BBGGRR)
struct GrAttr {
    int32_t pen_size = 2, pen_color = 0, pen_style = 0;
    int32_t brush_color = 0xFFFFFF, brush_style = 1, hatch_style = 3;
    int32_t bk_mode = 1, bk_color = 0xFFFFFF;
    int32_t flag96 = 1;          // unknown (ctor default 1)
    int32_t own_pen = 1;         // 1 = select own pen when drawing, 0 = inherit DC pen
    int32_t own_brush = 1;       // 1 = select own brush, 0 = inherit
    int32_t printable = 1, visible = 1, fixed_aspect = 0, scalable_pen = 1;
    int32_t selectable = 1, moveable = 1, sizeable = 1;
    int32_t select_width = 2, handle_size = 80, select_style = 2, select_color = 0;
};

struct GrLogFont {
    int32_t size = 0;            // only present for archive version >= 1.9
    int32_t height = 0, width = 0, escapement = 0, orientation = 0, weight = 0;
    uint8_t italic = 0, underline = 0, strikeout = 0, charset = 0;
    uint8_t out_prec = 0, clip_prec = 0, quality = 0, pitch_family = 0;
    std::string face;
};

struct GrBomOb {                 // CGrBomOb: one BOM cell / field descriptor
    std::string name, value;
    int32_t a = 0, b = 0, c = 0, d = 0;
};

// One decoded archive object (union of all per-class fields).
struct GrObject {
    GrType type = GrType::Unknown;
    std::string cls;             // MFC class name, e.g. "CGrRect"
    std::string ver;             // per-object archive version, e.g. "1.6"
    // --- CGrComp base ---
    std::array<int32_t, 10> labels{};
    std::vector<std::string> strs;      // 3 (ver < 1.3) or 4 user strings
    GrAttr attr;
    Vec2 org;                    // reference point (NOT applied to pts)
    Vec2 scale{1, 1};
    double rot = 0;              // degrees
    std::vector<Vec2> pts;       // geometry, logical units (mils), final coordinates
    // --- CGrGroup ---
    std::vector<std::unique_ptr<GrObject>> children;
    int32_t group_tail = 0;
    // --- CGrPolygon / Rect / Line / PolyLine ---
    int32_t closed = 0, arrow1 = 0, arrow0 = 0;
    // --- CGrPolyArcLine ---
    std::vector<uint8_t> seg_types;
    // --- CGrText ---
    std::string text, url;
    std::array<int32_t, 7> text_i{};    // stream order: +340,+344,+352,+348,+556,+568,+560
    int32_t text_h552 = 0;
    double text_d576 = 0;
    GrLogFont font;
    // --- CGrBitmap / CGrEnhMetaFile ---
    int32_t bmp_mode = 0;
    std::vector<uint8_t> data, data2;
    // --- CGrHandles ---
    int32_t handles_tail = 0;
    // --- BOM block family ---
    std::vector<GrLogFont> fonts;
    std::vector<std::string> bom_strs;
    std::vector<int32_t> bom_ints;
    std::vector<GrBomOb> bom_obs;
    std::vector<uint32_t> bom_dwords;
    int32_t ref_index = 0;       // >0: this is a back-reference to archive object #n
};

// Strip the CC_COMPRESS wrapper (if present) and inflate.  Unwrapped blobs are
// returned verbatim.
bool unwrap_blob(const uint8_t* p, size_t n, std::vector<uint8_t>& payload, std::string& err);

// Parse an archive payload.  `root_class` = MFC name of the root object; when
// null, the common roots are tried and the first that consumes the payload
// exactly is kept.  Trailing bytes are an error.
bool parse_archive(const uint8_t* p, size_t n, GrObject& root, std::string& err,
                   const char* root_class = nullptr);

// unwrap_blob + parse_archive
bool decode_blob(const uint8_t* p, size_t n, GrObject& root, std::string& err,
                 const char* root_class = nullptr);

// Convert a decoded object tree into PCB Plotter primitives (mm, +Y up).
// Invisible objects, bitmaps, metafiles and OLE items are skipped.
void to_gprims(const GrObject& o, std::vector<GPrim>& out);

// Decode any graphics blob straight to GPrims (root group's children).
bool decode_graphics(const uint8_t* p, size_t n, std::vector<GPrim>& out, std::string& err);

// Split view of a package rendering.
struct PackageRendering {
    std::vector<Pad> pads;               // one per pad sub-group, sorted by pin
    std::vector<GPrim> pad_shapes;       // the raw pad graphics (mm)
    std::vector<GPrim> artwork;          // role 3: body outline / notch / marks
    std::vector<GPrim> highlight;        // role 1: selection highlight extent
    std::vector<GPrim> other;            // anything without a known role
    int pin_one_index = -1;              // index into pads of pin 1 (-1 = none)
};

bool decode_package_rendering(const uint8_t* p, size_t n, PackageRendering& r, std::string& err);

// Fills pkg.pads (pin numbers from labels[4]) and pkg.body (artwork).  Other
// Package fields (flags, leads ...) come from the Packages table columns.
bool decode_package_rendering(const uint8_t* p, size_t n, Package& pkg, std::string& err);

// COLORREF (0x00BBGGRR) -> 0xAARRGGBB (opaque)
inline uint32_t colorref_to_argb(int32_t c) {
    const uint32_t u = uint32_t(c);
    return 0xFF000000u | ((u & 0xFF) << 16) | (u & 0xFF00) | ((u >> 16) & 0xFF);
}

} // namespace pcbp::cc
