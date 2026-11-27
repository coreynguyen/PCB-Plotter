#pragma once
// ============================================================================
// gerber_gpu.h — GPU (D3D11) Gerber layer renderer for the Gerber workspace.
//
// Each parsed layer is tessellated ONCE into a LayerMesh:
//   * flashes  : one triangle template per aperture (macro clear parts get a
//                second template drawn with clear polarity) + one float2
//                instance per flash  -> DrawInstanced per (run, tile, D-code)
//   * draws/arcs: capsule instances (a, b, half size, round/rect pen); arcs
//                are flattened adaptively; the VS expands a quad, the PS
//                evaluates the pen SDF (exact at every zoom, anti-aliased)
//   * regions  : ear-clipped triangles (holes are cut-ins / clear polarity);
//                huge or degenerate contours use stencil-then-cover
// Objects are sorted by polarity run (file order preserved across runs),
// then by a 16x16 tile grid (frustum culling), then by D-code.
//
// Rendering: every layer is drawn into an R8 coverage target (4x MSAA when
// available): dark = union blend, clear = subtract blend. The resolved
// coverage is composited onto the colour target with the layer colour
// (solid / outline-by-edge-detect / sketch), optionally negative. Layer
// compare composites two coverages (XOR highlighted). LOD: flashes smaller
// than ~1.5 px are drawn as min-1px quads.
//
// If D3D11 setup fails, `Renderer::ok()` is false and the caller falls back
// to Direct2D (D2DLayerGeo below).
// ============================================================================
#include "../core/geom.h"
#include "../io/gerber.h"
#include "gfx.h"
#include <memory>
#include <vector>

namespace pcbp::gui::ggpu {

struct MeshStats {
    size_t flashes = 0, capsules = 0, region_tris = 0, big_regions = 0, batches = 0, runs = 0, templates = 0;
    double build_ms = 0;
};

class LayerMesh;   // opaque (CPU + GPU buffers)

// Build a mesh from a parsed layer. hidden = blanked D-codes, crop = per-object hide mask (may be null).
std::shared_ptr<LayerMesh> build_mesh(const gerber::Layer& L, const std::vector<int>& hidden, const std::vector<char>* crop);
const MeshStats& mesh_stats(const LayerMesh& m);
BBox mesh_extent(const LayerMesh& m);   // layer coordinates

enum class FillMode { Solid, Outline, Sketch };

struct View {
    int w = 0, h = 0;            // target size in px
    double scale = 1;            // px per mm
    double cx = 0, cy = 0;       // world centre
    bool mirror = false;         // bottom view (X flipped)
    bool mirror_y = false;       // flipped about the X axis (Y flipped)
    double rot = 0;              // display-only view rotation (deg, CCW), applied about (cx,cy) after mirroring
};

class Renderer {
public:
    Renderer();
    ~Renderer();
    bool init(std::string& err);          // compiles shaders, creates states
    bool ok() const;
    // frame
    bool begin(const View& v, uint32_t bg_argb);   // (re)creates targets, clears colour
    // draw one layer: xf = layer -> world. alpha 0..1.
    // screen: "Screen" blend (1 - (1 - dst)(1 - src)): where two screened layers overlap their colours add up, so
    // complementary colours (magenta + green, red + cyan) turn white on a dark background (ViewMate's compare look)
    void draw_layer(LayerMesh& m, const Xform& xf, uint32_t argb, float alpha, FillMode mode, bool negative, bool screen = false);
    // a picture (board scan / photo) mapped onto the world: grid = (n+1) x (n+1) world points of the image's
    // (u, v) = (i / n, j / n) positions, row-major from the top-left (perspective-correct when n is large enough).
    // threshold < 0: the picture's own colours; else luminance >= threshold (0..1) becomes `tint`, the rest black
    // (invert swaps them): the black-and-white board for a stencil compare. srv: ID3D11ShaderResourceView* (BGRA).
    void draw_image(void* srv, const Vec2* grid, int n, uint32_t tint, float alpha, float threshold, bool invert, bool screen);
    // XOR compare of two layers: differences in col_a / col_b, common area dimmed
    void draw_compare(LayerMesh& a, const Xform& xa, LayerMesh& b, const Xform& xb, uint32_t col_a, uint32_t col_b, uint32_t col_common);
    // highlight all objects of one D-code (polarity ignored)
    void draw_highlight(LayerMesh& m, const Xform& xf, int dcode, uint32_t argb, float alpha);
    void end();
    void* texture() const;               // ImTextureID
    int width() const;
    int height() const;
    size_t last_batches() const;         // drawn batches in last frame (after culling)
    size_t last_instances() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace pcbp::gui::ggpu
