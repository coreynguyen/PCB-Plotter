// ============================================================================
// gerber_gpu.cpp — tessellate-once GPU renderer for Gerber layers (D3D11).
// See gerber_gpu.h for the design.
// ============================================================================
#include "gerber_gpu.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace pcbp::gui::ggpu {
namespace gb = pcbp::gerber;

namespace {

struct F2 { float x, y; };
struct Caps { float ax, ay, bx, by, hx, hy, kind, dcode; };   // kind 0 round pen, 1 rect pen
struct Tmpl { int dcode = 0; uint32_t first = 0, count = 0; float size = 0; bool clear_part = false; };
enum : uint8_t { T_FLASH, T_CAPS, T_TRIS, T_BIG };
struct Batch {
    uint8_t type = 0, clear = 0;
    int tmpl = -1;
    uint32_t start = 0, count = 0;
    float bb[4] = {1e30f, 1e30f, -1e30f, -1e30f};   // local (relative to origin)
};

constexpr size_t kEarClipMax = 300;    // larger contours use stencil-then-cover

double cross(Vec2 o, Vec2 a, Vec2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

double poly_area(const std::vector<Vec2>& p) {
    double a = 0;
    for (size_t i = 0, n = p.size(); i < n; ++i) { const Vec2& u = p[i]; const Vec2& v = p[(i + 1) % n]; a += u.x * v.y - v.x * u.y; }
    return a * 0.5;
}

void clean_poly(const std::vector<Vec2>& in, std::vector<Vec2>& p) {
    p.clear();
    const double eps = 1e-9;
    for (const Vec2& v : in)
        if (p.empty() || std::fabs(v.x - p.back().x) > eps || std::fabs(v.y - p.back().y) > eps) p.push_back(v);
    while (p.size() > 1 && std::fabs(p.front().x - p.back().x) <= eps && std::fabs(p.front().y - p.back().y) <= eps) p.pop_back();
}

// Ear clipping (CCW after normalisation). Handles collinear / duplicate vertices and
// cut-in bridges. Returns false when no ear can be found (caller falls back).
bool earclip(const std::vector<Vec2>& in, std::vector<Vec2>& tris) {
    std::vector<Vec2> p;
    clean_poly(in, p);
    const size_t n = p.size();
    if (n < 3) return true;
    if (poly_area(p) < 0) std::reverse(p.begin(), p.end());
    if (n == 3) { tris.insert(tris.end(), p.begin(), p.end()); return true; }
    BBox bb;
    for (auto& v : p) bb.add(v);
    const double eps = std::max(bb.w(), bb.h()) * std::max(bb.w(), bb.h()) * 1e-12;
    std::vector<int> prv(n), nxt(n);
    for (size_t i = 0; i < n; ++i) { prv[i] = (int)((i + n - 1) % n); nxt[i] = (int)((i + 1) % n); }
    size_t count = n;
    int i = 0;
    size_t guard = 0;
    const size_t base = tris.size();
    auto same = [](Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; };
    while (count > 3) {
        const int a = prv[i], b = i, c = nxt[i];
        const double cr = cross(p[a], p[b], p[c]);
        bool ear = false, drop = false;
        if (std::fabs(cr) <= eps) {
            drop = true;   // collinear / spike: remove without a triangle
        } else if (cr > 0) {
            ear = true;
            for (int k = nxt[c]; k != a; k = nxt[k]) {
                const Vec2& q = p[k];
                if (same(q, p[a]) || same(q, p[b]) || same(q, p[c])) continue;
                if (cross(p[a], p[b], q) >= 0 && cross(p[b], p[c], q) >= 0 && cross(p[c], p[a], q) >= 0) { ear = false; break; }
            }
        }
        if (ear || drop) {
            if (ear) { tris.push_back(p[a]); tris.push_back(p[b]); tris.push_back(p[c]); }
            nxt[a] = c; prv[c] = a;
            --count;
            i = a;
            guard = 0;
            continue;
        }
        i = nxt[i];
        if (++guard > count + 2) { tris.resize(base); return false; }
    }
    if (std::fabs(cross(p[prv[i]], p[i], p[nxt[i]])) > eps) {
        tris.push_back(p[prv[i]]); tris.push_back(p[i]); tris.push_back(p[nxt[i]]);
    }
    return true;
}

// outer polygon + one hole -> single polygon via a bridge (outer convex-ish shapes)
std::vector<Vec2> bridge_hole(std::vector<Vec2> outer, std::vector<Vec2> hole) {
    if (poly_area(outer) < 0) std::reverse(outer.begin(), outer.end());
    if (poly_area(hole) > 0) std::reverse(hole.begin(), hole.end());
    size_t hm = 0;
    for (size_t i = 1; i < hole.size(); ++i) if (hole[i].x > hole[hm].x) hm = i;
    size_t om = 0;
    double best = 1e300;
    for (size_t i = 0; i < outer.size(); ++i) {
        double d = (outer[i].x - hole[hm].x) * (outer[i].x - hole[hm].x) + (outer[i].y - hole[hm].y) * (outer[i].y - hole[hm].y);
        if (outer[i].x >= hole[hm].x && d < best) { best = d; om = i; }
    }
    std::vector<Vec2> r;
    for (size_t i = 0; i <= om; ++i) r.push_back(outer[i]);
    for (size_t k = 0; k <= hole.size(); ++k) r.push_back(hole[(hm + k) % hole.size()]);
    for (size_t i = om; i < outer.size(); ++i) r.push_back(outer[i]);
    return r;
}

// triangulate an aperture (relative to its centre). dark/clear = macro exposure parts
void aperture_tris(const gb::Aperture* ap, std::vector<Vec2>& dark, std::vector<Vec2>& clear, float& size) {
    if (!ap) {
        std::vector<Vec2> c;
        for (int i = 0; i < 12; ++i) c.push_back({0.05 * std::cos(i * kPi / 6), 0.05 * std::sin(i * kPi / 6)});
        earclip(c, dark);
        size = 0.1f;
        return;
    }
    const double big = std::max(ap->w, ap->h);
    const int segs = big > 3 ? 96 : big > 0.8 ? 64 : 40;
    std::vector<bool> clr;
    auto polys = gb::flash_polys(*ap, {0, 0}, segs, &clr);
    BBox bb;
    if (ap->shape == gb::Aperture::Circle && polys.size() >= 1) {
        // fan / ring directly (exact, no ear clipping needed)
        const double r = ap->w / 2, h = ap->hole > 0 ? ap->hole / 2 : 0;
        for (int i = 0; i < segs; ++i) {
            double a0 = 2 * kPi * i / segs, a1 = 2 * kPi * (i + 1) / segs;
            Vec2 o0{r * std::cos(a0), r * std::sin(a0)}, o1{r * std::cos(a1), r * std::sin(a1)};
            if (h > 0) {
                Vec2 i0{h * std::cos(a0), h * std::sin(a0)}, i1{h * std::cos(a1), h * std::sin(a1)};
                dark.insert(dark.end(), {i0, o0, o1, i0, o1, i1});
            } else {
                dark.insert(dark.end(), {Vec2{0, 0}, o0, o1});
            }
        }
        size = (float)ap->w;
        return;
    }
    if (ap->shape != gb::Aperture::Macro) {
        std::vector<Vec2> outer = polys.empty() ? std::vector<Vec2>{} : polys[0];
        for (auto& v : outer) bb.add(v);
        if (polys.size() > 1 && clr.size() > 1 && clr[1]) outer = bridge_hole(outer, polys[1]);
        if (!earclip(outer, dark)) earclip(polys[0], dark);
    } else {
        for (size_t i = 0; i < polys.size(); ++i) {
            if (polys[i].size() < 3) continue;
            bool c = i < clr.size() && clr[i];
            if (!c) for (auto& v : polys[i]) bb.add(v);
            if (!earclip(polys[i], c ? clear : dark)) {   // fan fallback
                for (size_t k = 1; k + 1 < polys[i].size(); ++k) (c ? clear : dark).insert((c ? clear : dark).end(), {polys[i][0], polys[i][k], polys[i][k + 1]});
            }
        }
    }
    size = bb.empty() ? (float)big : (float)std::max(bb.w(), bb.h());
}

void pen_of(const gb::Aperture* ap, float& hx, float& hy, float& kind) {
    kind = 0; hx = hy = 0;
    if (!ap) return;
    switch (ap->shape) {
    case gb::Aperture::Circle: hx = hy = (float)(ap->w / 2); break;
    case gb::Aperture::Rect: kind = 1; hx = (float)(ap->w / 2); hy = (float)(ap->h / 2); break;
    default: hx = hy = (float)(std::min(ap->w, ap->h) / 2); break;
    }
}

typedef HRESULT(WINAPI* fnD3DCompile)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
bool compile(const char* src, const char* target, Com<ID3DBlob>& out, std::string& err) {
    static fnD3DCompile cmp = nullptr;
    if (!cmp) {
        for (const wchar_t* n : {L"d3dcompiler_47.dll", L"d3dcompiler_46.dll", L"d3dcompiler_43.dll"})
            if (HMODULE m = LoadLibraryW(n)) { cmp = (fnD3DCompile)GetProcAddress(m, "D3DCompile"); if (cmp) break; }
        if (!cmp) { err = "d3dcompiler not found"; return false; }
    }
    Com<ID3DBlob> errs;
    HRESULT hr = cmp(src, std::strlen(src), nullptr, nullptr, nullptr, "main", target, 0, 0, out.put(), errs.put());
    if (FAILED(hr)) { err = errs ? std::string((const char*)errs->GetBufferPointer(), errs->GetBufferSize()) : "compile failed"; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// shaders
// ---------------------------------------------------------------------------
const char* kCB = R"(
cbuffer P : register(b0) { float4 m0; float4 m1; float4 prm; float4 prm2; };
// m0 = (a, c, tx, 0)  m1 = (b, d, ty, 0): local -> NDC
// prm = (px_local, template_scale, sketch, filter_dcode)  prm2 = (min_half_px, 0, 0, 0)
float4 to_ndc(float2 p) { return float4(m0.x * p.x + m0.y * p.y + m0.z, m1.x * p.x + m1.y * p.y + m1.z, 0, 1); }
)";
const char* kVSFlash = R"(
struct VI { float2 pos : POSITION; float2 inst : TEXCOORD0; };
float4 main(VI i) : SV_Position { return to_ndc(i.inst + i.pos * prm.y); }
)";
const char* kPSOne = R"(
float4 main(float4 p : SV_Position) : SV_Target { return float4(1, 1, 1, 1); }
)";
const char* kVSCaps = R"(
struct VI { float2 c : POSITION; float4 ab : TEXCOORD0; float4 hk : TEXCOORD1; };
struct VO { float4 pos : SV_Position; float2 p : TEXCOORD0; nointerpolation float4 ab : TEXCOORD1; nointerpolation float4 hk : TEXCOORD2; };
VO main(VI i) {
    VO o;
    float px = prm.x;
    float2 a = i.ab.xy, b = i.ab.zw;
    float2 h = i.hk.xy;
    float kind = i.hk.z;
    if (prm.z > 0.5) { h = float2(0.6 * px, 0.6 * px); kind = 0; }
    h = max(h, float2(prm2.x * px, prm2.x * px));
    float e = 1.5 * px;
    float2 p;
    if (kind < 0.5) {
        float2 d = b - a; float l = length(d);
        float2 u = l > 1e-9 ? d / l : float2(1, 0);
        float2 n = float2(-u.y, u.x);
        float r = h.x + e;
        p = (i.c.x < 0 ? a - u * r : b + u * r) + n * (i.c.y * r);
    } else {
        float2 lo = min(a, b) - h - e, hi = max(a, b) + h + e;
        p = float2(i.c.x < 0 ? lo.x : hi.x, i.c.y < 0 ? lo.y : hi.y);
    }
    o.pos = to_ndc(p);
    if (prm.w >= 0 && abs(i.hk.w - prm.w) > 0.5) o.pos = float4(-9, -9, 0, 1);
    o.p = p; o.ab = float4(a, b); o.hk = float4(h, kind, 0);
    return o;
}
)";
const char* kPSCaps = R"(
struct VO { float4 pos : SV_Position; float2 p : TEXCOORD0; nointerpolation float4 ab : TEXCOORD1; nointerpolation float4 hk : TEXCOORD2; };
float4 main(VO i) : SV_Target {
    float2 a = i.ab.xy, b = i.ab.zw, d = b - a;
    float dd = dot(d, d);
    float t = dd > 1e-18 ? saturate(dot(i.p - a, d) / dd) : 0;
    float2 c = a + d * t;
    float sd;
    if (i.hk.z < 0.5) sd = length(i.p - c) - i.hk.x;
    else { float2 q = abs(i.p - c) - i.hk.xy; sd = length(max(q, 0)) + min(max(q.x, q.y), 0); }
    float al = saturate(0.5 - sd / prm.x);
    if (al <= 0) discard;
    return float4(al, al, al, al);
}
)";
const char* kVSFull = R"(
float4 main(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
)";
const char* kPSComp = R"(
Texture2D cov0 : register(t0);
Texture2D cov1 : register(t1);
cbuffer C : register(b0) { float4 col; float4 mode; float4 col2; float4 col3; };
float L0(int2 p) { return cov0.Load(int3(p, 0)).r; }
float4 main(float4 pos : SV_Position) : SV_Target {
    int2 ip = int2(pos.xy);
    float c = L0(ip);
    if (mode.x > 1.5) {
        float d = cov1.Load(int3(ip, 0)).r;
        return col * saturate(c - d) + col2 * saturate(d - c) + col3 * min(c, d);
    }
    if (mode.x > 0.5) {
        float e = max(max(abs(c - L0(ip + int2(1, 0))), abs(c - L0(ip - int2(1, 0)))),
                      max(abs(c - L0(ip + int2(0, 1))), abs(c - L0(ip - int2(0, 1)))));
        float f = (mode.y > 0.5 ? 1 - c : c);
        return col * saturate(e * 1.6) + col * (f * mode.z);
    }
    if (mode.y > 0.5) c = 1 - c;
    return col * c;
}
)";

} // namespace

// ===========================================================================
// LayerMesh
// ===========================================================================
class LayerMesh {
public:
    Vec2 origin;
    BBox extent;
    MeshStats st;
    std::vector<Tmpl> tmpls;
    std::vector<F2> tverts;          // templates
    std::vector<F2> finst;           // flash instances (+1 zero instance at the end)
    std::vector<Caps> caps;
    std::vector<F2> tris;            // region triangles
    std::vector<F2> big;             // stencil regions: fan tris + 6 cover verts each
    std::vector<Batch> batches;
    // GPU
    Com<ID3D11Buffer> vb_t, vb_f, vb_c, vb_r, vb_b;
    bool gpu_tried = false;
    bool upload();
};

namespace {
Com<ID3D11Buffer> make_vb(const void* data, size_t bytes) {
    Com<ID3D11Buffer> b;
    ID3D11Device* dev = gfx_device();
    if (!dev || !bytes) return b;
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = (UINT)bytes;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA s{data, 0, 0};
    dev->CreateBuffer(&d, &s, b.put());
    return b;
}
}

bool LayerMesh::upload() {
    if (gpu_tried) return (bool)vb_f;
    gpu_tried = true;
    vb_t = make_vb(tverts.data(), tverts.size() * sizeof(F2));
    vb_f = make_vb(finst.data(), finst.size() * sizeof(F2));
    vb_c = make_vb(caps.data(), caps.size() * sizeof(Caps));
    vb_r = make_vb(tris.data(), tris.size() * sizeof(F2));
    vb_b = make_vb(big.data(), big.size() * sizeof(F2));
    return (bool)vb_f;
}

const MeshStats& mesh_stats(const LayerMesh& m) { return m.st; }
BBox mesh_extent(const LayerMesh& m) { return m.extent; }

std::shared_ptr<LayerMesh> build_mesh(const gb::Layer& L, const std::vector<int>& hidden, const std::vector<char>* crop) {
    auto t0 = std::chrono::steady_clock::now();
    auto m = std::make_shared<LayerMesh>();
    std::set<int> hid(hidden.begin(), hidden.end());
    BBox ext = L.extent;
    if (ext.empty()) for (auto& o : L.objs) ext.add(gb::object_bbox(L, o));
    if (ext.empty()) ext = BBox(0, 0, 1, 1);
    m->extent = ext;
    m->origin = ext.center();
    const Vec2 O = m->origin;
    // tile grid for frustum culling: ~3000 objects per tile, at most 16x16
    const int kTiles = std::clamp((int)std::sqrt(L.objs.size() / 3000.0), 1, 16);
    const double tw = std::max(ext.w(), 1e-6) / kTiles, th = std::max(ext.h(), 1e-6) / kTiles;
    auto tile_of = [&](Vec2 c) {
        int tx = std::clamp((int)((c.x - ext.x0) / tw), 0, kTiles - 1), ty = std::clamp((int)((c.y - ext.y0) / th), 0, kTiles - 1);
        return ty * kTiles + tx;
    };

    // templates per D-code (dark part, clear part)
    std::map<int, std::pair<int, int>> tmap;
    auto tmpl_for = [&](int dcode) -> std::pair<int, int> {
        auto it = tmap.find(dcode);
        if (it != tmap.end()) return it->second;
        std::vector<Vec2> dk, cl;
        float size = 0;
        aperture_tris(gb::find_ap(L, dcode), dk, cl, size);
        auto add = [&](const std::vector<Vec2>& v, bool c) {
            if (v.empty()) return -1;
            Tmpl t;
            t.dcode = dcode; t.first = (uint32_t)m->tverts.size(); t.count = (uint32_t)v.size(); t.size = size; t.clear_part = c;
            for (auto& p : v) m->tverts.push_back({(float)p.x, (float)p.y});
            m->tmpls.push_back(t);
            return (int)m->tmpls.size() - 1;
        };
        std::pair<int, int> r{add(dk, false), add(cl, true)};
        tmap[dcode] = r;
        return r;
    };

    // records
    struct Rec { uint64_t key; uint32_t payload; uint32_t pcount; float bb[4]; };
    std::vector<Rec> recs;
    recs.reserve(L.objs.size() + 16);
    std::vector<F2> fpos;          // temp flash positions (payload index)
    std::vector<Caps> tcaps;
    std::vector<F2> ttris;
    std::vector<std::pair<uint32_t, uint32_t>> tri_ranges;   // payload -> (start,count) in ttris
    std::vector<std::vector<Vec2>> bigs;
    uint32_t run = 0;
    int last_clear = -1;
    auto key = [](uint32_t runkey, uint8_t type, int tile, int tmpl) {
        return ((uint64_t)runkey << 40) | ((uint64_t)type << 36) | ((uint64_t)(tile & 0xFFF) << 24) | (uint64_t)((tmpl + 1) & 0xFFFFFF);
    };
    auto setbb = [&](Rec& r, const BBox& b) { r.bb[0] = (float)(b.x0 - O.x); r.bb[1] = (float)(b.y0 - O.y); r.bb[2] = (float)(b.x1 - O.x); r.bb[3] = (float)(b.y1 - O.y); };

    auto add_flash = [&](int dcode, Vec2 at, bool clear, const BBox& ob) {
        auto [td, tc] = tmpl_for(dcode);
        uint32_t pi = (uint32_t)fpos.size();
        fpos.push_back({(float)(at.x - O.x), (float)(at.y - O.y)});
        int tile = tile_of(at);
        if (td >= 0) { Rec r{key(run * 2, T_FLASH, tile, td), pi, 1, {}}; setbb(r, ob); recs.push_back(r); }
        if (tc >= 0 && !clear) { Rec r{key(run * 2 + 1, T_FLASH, tile, tc), pi, 1, {}}; setbb(r, ob); recs.push_back(r); }
    };
    auto add_cap = [&](Vec2 a, Vec2 b, const gb::Aperture* ap, int dcode) {
        Caps c{};
        c.ax = (float)(a.x - O.x); c.ay = (float)(a.y - O.y); c.bx = (float)(b.x - O.x); c.by = (float)(b.y - O.y);
        pen_of(ap, c.hx, c.hy, c.kind);
        c.dcode = (float)dcode;
        BBox bb; bb.add(a); bb.add(b); bb.inflate(std::max(c.hx, c.hy) + 1e-3);
        Rec r{key(run * 2, T_CAPS, tile_of((a + b) * 0.5), 0), (uint32_t)tcaps.size(), 1, {}};
        setbb(r, bb);
        recs.push_back(r);
        tcaps.push_back(c);
    };

    for (size_t i = 0; i < L.objs.size(); ++i) {
        const gb::Object& o = L.objs[i];
        if (crop && i < crop->size() && (*crop)[i]) continue;
        if (o.kind != gb::Object::Region && hid.count(o.dcode)) continue;
        if (last_clear >= 0 && (int)o.clear != last_clear) ++run;
        last_clear = o.clear;
        const gb::Aperture* ap = o.kind == gb::Object::Region ? nullptr : gb::find_ap(L, o.dcode);
        switch (o.kind) {
        case gb::Object::Flash: add_flash(o.dcode, o.a, o.clear, gb::object_bbox(L, o)); ++m->st.flashes; break;
        case gb::Object::Draw:
            if (dist(o.a, o.b) < 1e-7) { add_flash(o.dcode, o.a, o.clear, gb::object_bbox(L, o)); ++m->st.flashes; }
            else { add_cap(o.a, o.b, ap, o.dcode); ++m->st.capsules; }
            break;
        case gb::Object::Arc: {
            const double r = dist(o.a, o.center);
            const double sw = gb::arc_sweep_deg(o), a0 = gb::arc_start_deg(o);
            const double tol = std::max(0.0015, r * 1e-4);
            double step = r > tol ? 2 * std::acos(std::max(-1.0, 1 - tol / r)) : kPi / 2;
            int n = std::clamp((int)std::ceil(deg2rad(std::fabs(sw)) / std::max(step, 1e-4)), 2, 720);
            Vec2 prev = o.a;
            for (int k = 1; k <= n; ++k) {
                double a = deg2rad(a0 + sw * k / n);
                Vec2 q = k == n ? o.b : Vec2{o.center.x + r * std::cos(a), o.center.y + r * std::sin(a)};
                add_cap(prev, q, ap, o.dcode);
                ++m->st.capsules;
                prev = q;
            }
            break;
        }
        case gb::Object::Region: {
            if (o.poly.size() < 3) break;
            BBox bb;
            for (auto& v : o.poly) bb.add(v);
            const uint32_t start = (uint32_t)ttris.size();
            std::vector<Vec2> tr;
            bool ok = o.poly.size() <= kEarClipMax && earclip(o.poly, tr);
            if (ok) {
                for (auto& v : tr) ttris.push_back({(float)(v.x - O.x), (float)(v.y - O.y)});
                Rec r{key(run * 2, T_TRIS, tile_of(bb.center()), 0), (uint32_t)tri_ranges.size(), 0, {}};
                setbb(r, bb);
                recs.push_back(r);
                tri_ranges.push_back({start, (uint32_t)tr.size()});
                m->st.region_tris += tr.size() / 3;
            } else {
                Rec r{key(run * 2, T_BIG, 0, 0), (uint32_t)bigs.size(), 0, {}};
                setbb(r, bb);
                recs.push_back(r);
                bigs.push_back(o.poly);
                ++m->st.big_regions;
            }
            break;
        }
        }
    }
    // order: run, type, tile, template; payload keeps file order inside a bucket
    std::stable_sort(recs.begin(), recs.end(), [](const Rec& a, const Rec& b) { return a.key < b.key; });
    auto unpack_run = [](uint64_t k) { return (uint32_t)(k >> 40); };
    auto unpack_type = [](uint64_t k) { return (uint8_t)((k >> 36) & 15); };
    uint64_t cur = ~0ull;
    // polarity of a runkey: run parity from the original objects
    std::vector<uint8_t> run_clear(run + 1, 0);
    {
        uint32_t r = 0; int lc = -1;
        for (size_t i = 0; i < L.objs.size(); ++i) {
            const gb::Object& o = L.objs[i];
            if (crop && i < crop->size() && (*crop)[i]) continue;
            if (o.kind != gb::Object::Region && hid.count(o.dcode)) continue;
            if (lc >= 0 && (int)o.clear != lc) ++r;
            lc = o.clear;
            if (r <= run) run_clear[r] = o.clear;
        }
    }
    for (const Rec& r : recs) {
        const uint8_t type = unpack_type(r.key);
        const uint32_t rk = unpack_run(r.key);
        if (r.key != cur || type == T_BIG) {
            Batch b;
            b.type = type;
            b.clear = (rk & 1) ? 1 : run_clear[std::min<uint32_t>(rk / 2, run)];
            b.tmpl = type == T_FLASH ? (int)(r.key & 0xFFFFFF) - 1 : -1;
            b.start = type == T_FLASH ? (uint32_t)m->finst.size() : type == T_CAPS ? (uint32_t)m->caps.size()
                    : type == T_TRIS ? (uint32_t)m->tris.size() : (uint32_t)m->big.size();
            m->batches.push_back(b);
            cur = r.key;
        }
        Batch& b = m->batches.back();
        b.bb[0] = std::min(b.bb[0], r.bb[0]); b.bb[1] = std::min(b.bb[1], r.bb[1]);
        b.bb[2] = std::max(b.bb[2], r.bb[2]); b.bb[3] = std::max(b.bb[3], r.bb[3]);
        switch (type) {
        case T_FLASH: m->finst.push_back(fpos[r.payload]); ++b.count; break;
        case T_CAPS: m->caps.push_back(tcaps[r.payload]); ++b.count; break;
        case T_TRIS: {
            auto [s, c] = tri_ranges[r.payload];
            m->tris.insert(m->tris.end(), ttris.begin() + s, ttris.begin() + s + c);
            b.count += c;
            break;
        }
        case T_BIG: {
            const auto& poly = bigs[r.payload];
            for (size_t k = 1; k + 1 < poly.size(); ++k)
                for (const Vec2& v : {poly[0], poly[k], poly[k + 1]}) m->big.push_back({(float)(v.x - O.x), (float)(v.y - O.y)});
            b.count = (uint32_t)(m->big.size() - b.start);
            const float x0 = r.bb[0], y0 = r.bb[1], x1 = r.bb[2], y1 = r.bb[3];
            for (F2 v : {F2{x0, y0}, F2{x1, y0}, F2{x0, y1}, F2{x0, y1}, F2{x1, y0}, F2{x1, y1}}) m->big.push_back(v);
            break;
        }
        }
    }
    m->finst.push_back({0, 0});   // zero instance for region triangles
    m->st.batches = m->batches.size();
    m->st.runs = run + 1;
    m->st.templates = m->tmpls.size();
    m->st.build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return m;
}

// ===========================================================================
// Renderer
// ===========================================================================
struct Renderer::Impl {
    bool ok = false;
    UINT msaa = 1;
    Com<ID3D11VertexShader> vs_flash, vs_caps, vs_full;
    Com<ID3D11PixelShader> ps_one, ps_caps, ps_comp;
    Com<ID3D11InputLayout> il_flash, il_caps;
    Com<ID3D11BlendState> bl_dark, bl_clear, bl_none, bl_comp;
    Com<ID3D11DepthStencilState> ds_off, ds_inv, ds_cover;
    Com<ID3D11RasterizerState> rs;
    Com<ID3D11Buffer> cb, cbc, vb_corner, vb_quad;
    // targets
    int w = 0, h = 0;
    Com<ID3D11Texture2D> color_tex, cov_ms_tex, ds_tex, cov_tex[2];
    Com<ID3D11RenderTargetView> color_rtv, cov_ms_rtv, cov_rtv[2];
    Com<ID3D11ShaderResourceView> color_srv, cov_srv[2];
    Com<ID3D11DepthStencilView> dsv;
    View view;
    size_t batches = 0, instances = 0;
    // saved state
    ID3D11RenderTargetView* save_rtv = nullptr;
    ID3D11DepthStencilView* save_dsv = nullptr;
    D3D11_VIEWPORT save_vp{};
    UINT save_nvp = 0;

    bool targets(int W, int H);
    void cb_set(const Xform& M, float px, float tscale, float sketch, float filter);
    void coverage(LayerMesh& m, const Xform& xf, int target, int filter, bool sketch);
    void composite(int mode, bool neg, const float* c1, const float* c2, const float* c3, float fill_alpha);
};

namespace {
void premul(uint32_t argb, float alpha, float* out) {
    float a = ((argb >> 24) & 255) / 255.f * alpha;
    if (((argb >> 24) & 255) == 0) a = alpha;
    out[0] = ((argb >> 16) & 255) / 255.f * a;
    out[1] = ((argb >> 8) & 255) / 255.f * a;
    out[2] = (argb & 255) / 255.f * a;
    out[3] = a;
}
}

Renderer::Renderer() : p_(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;
bool Renderer::ok() const { return p_->ok; }
void* Renderer::texture() const { return (void*)p_->color_srv.get(); }
int Renderer::width() const { return p_->w; }
int Renderer::height() const { return p_->h; }
size_t Renderer::last_batches() const { return p_->batches; }
size_t Renderer::last_instances() const { return p_->instances; }

bool Renderer::init(std::string& err) {
    Impl& I = *p_;
    ID3D11Device* dev = gfx_device();
    if (!dev) { err = "no D3D11 device"; return false; }
    Com<ID3DBlob> b_vsf, b_vsc, b_vsfull, b_one, b_psc, b_comp;
    std::string cb = kCB;
    if (!compile((cb + kVSFlash).c_str(), "vs_4_0", b_vsf, err) || !compile((cb + kVSCaps).c_str(), "vs_4_0", b_vsc, err) ||
        !compile(kVSFull, "vs_4_0", b_vsfull, err) || !compile(kPSOne, "ps_4_0", b_one, err) ||
        !compile((cb + kPSCaps).c_str(), "ps_4_0", b_psc, err) || !compile(kPSComp, "ps_4_0", b_comp, err))
        return false;
    auto okh = [&](HRESULT hr, const char* what) { if (FAILED(hr)) { err = what; return false; } return true; };
    if (!okh(dev->CreateVertexShader(b_vsf->GetBufferPointer(), b_vsf->GetBufferSize(), nullptr, I.vs_flash.put()), "vs_flash") ||
        !okh(dev->CreateVertexShader(b_vsc->GetBufferPointer(), b_vsc->GetBufferSize(), nullptr, I.vs_caps.put()), "vs_caps") ||
        !okh(dev->CreateVertexShader(b_vsfull->GetBufferPointer(), b_vsfull->GetBufferSize(), nullptr, I.vs_full.put()), "vs_full") ||
        !okh(dev->CreatePixelShader(b_one->GetBufferPointer(), b_one->GetBufferSize(), nullptr, I.ps_one.put()), "ps_one") ||
        !okh(dev->CreatePixelShader(b_psc->GetBufferPointer(), b_psc->GetBufferSize(), nullptr, I.ps_caps.put()), "ps_caps") ||
        !okh(dev->CreatePixelShader(b_comp->GetBufferPointer(), b_comp->GetBufferSize(), nullptr, I.ps_comp.put()), "ps_comp"))
        return false;
    D3D11_INPUT_ELEMENT_DESC ilf[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };
    D3D11_INPUT_ELEMENT_DESC ilc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };
    if (!okh(dev->CreateInputLayout(ilf, 2, b_vsf->GetBufferPointer(), b_vsf->GetBufferSize(), I.il_flash.put()), "il_flash") ||
        !okh(dev->CreateInputLayout(ilc, 3, b_vsc->GetBufferPointer(), b_vsc->GetBufferSize(), I.il_caps.put()), "il_caps"))
        return false;
    auto blend = [&](D3D11_BLEND src, D3D11_BLEND dst, D3D11_BLEND srca, D3D11_BLEND dsta, UINT8 mask, Com<ID3D11BlendState>& out) {
        D3D11_BLEND_DESC bd{};
        auto& r = bd.RenderTarget[0];
        r.BlendEnable = TRUE; r.SrcBlend = src; r.DestBlend = dst; r.BlendOp = D3D11_BLEND_OP_ADD;
        r.SrcBlendAlpha = srca; r.DestBlendAlpha = dsta; r.BlendOpAlpha = D3D11_BLEND_OP_ADD; r.RenderTargetWriteMask = mask;
        dev->CreateBlendState(&bd, out.put());
    };
    blend(D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_COLOR, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_COLOR_WRITE_ENABLE_ALL, I.bl_dark);
    blend(D3D11_BLEND_ZERO, D3D11_BLEND_INV_SRC_COLOR, D3D11_BLEND_ZERO, D3D11_BLEND_INV_SRC_ALPHA, D3D11_COLOR_WRITE_ENABLE_ALL, I.bl_clear);
    blend(D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, 0, I.bl_none);
    blend(D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_COLOR_WRITE_ENABLE_ALL, I.bl_comp);
    D3D11_DEPTH_STENCIL_DESC dd{};
    dev->CreateDepthStencilState(&dd, I.ds_off.put());
    dd.StencilEnable = TRUE; dd.StencilReadMask = 0xFF; dd.StencilWriteMask = 0xFF;
    dd.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_INVERT, D3D11_COMPARISON_ALWAYS};
    dd.BackFace = dd.FrontFace;
    dev->CreateDepthStencilState(&dd, I.ds_inv.put());
    dd.FrontFace = {D3D11_STENCIL_OP_ZERO, D3D11_STENCIL_OP_ZERO, D3D11_STENCIL_OP_ZERO, D3D11_COMPARISON_NOT_EQUAL};
    dd.BackFace = dd.FrontFace;
    dev->CreateDepthStencilState(&dd, I.ds_cover.put());
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE; rd.MultisampleEnable = TRUE;
    dev->CreateRasterizerState(&rd, I.rs.put());
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 64; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev->CreateBuffer(&bd, nullptr, I.cb.put());
    dev->CreateBuffer(&bd, nullptr, I.cbc.put());
    const F2 corner[6] = {{-1, -1}, {1, -1}, {-1, 1}, {-1, 1}, {1, -1}, {1, 1}};
    const F2 quad[6] = {{-.5f, -.5f}, {.5f, -.5f}, {-.5f, .5f}, {-.5f, .5f}, {.5f, -.5f}, {.5f, .5f}};
    I.vb_corner = make_vb(corner, sizeof corner);
    I.vb_quad = make_vb(quad, sizeof quad);
    UINT q = 0;
    if (SUCCEEDED(dev->CheckMultisampleQualityLevels(DXGI_FORMAT_R8_UNORM, 4, &q)) && q > 0 &&
        SUCCEEDED(dev->CheckMultisampleQualityLevels(DXGI_FORMAT_D24_UNORM_S8_UINT, 4, &q)) && q > 0)
        I.msaa = 4;
    I.ok = I.cb && I.vb_corner && I.vb_quad;
    if (!I.ok) err = "buffers";
    return I.ok;
}

bool Renderer::Impl::targets(int W, int H) {
    if (W == w && H == h && color_rtv) return true;
    ID3D11Device* dev = gfx_device();
    color_tex.reset(); color_rtv.reset(); color_srv.reset(); cov_ms_tex.reset(); cov_ms_rtv.reset(); ds_tex.reset(); dsv.reset();
    for (int k = 0; k < 2; ++k) { cov_tex[k].reset(); cov_rtv[k].reset(); cov_srv[k].reset(); }
    w = h = 0;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)W; td.Height = (UINT)H; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, color_tex.put()))) return false;
    dev->CreateRenderTargetView(color_tex.get(), nullptr, color_rtv.put());
    dev->CreateShaderResourceView(color_tex.get(), nullptr, color_srv.put());
    td.Format = DXGI_FORMAT_R8_UNORM;
    for (int k = 0; k < 2; ++k) {
        if (FAILED(dev->CreateTexture2D(&td, nullptr, cov_tex[k].put()))) return false;
        dev->CreateRenderTargetView(cov_tex[k].get(), nullptr, cov_rtv[k].put());
        dev->CreateShaderResourceView(cov_tex[k].get(), nullptr, cov_srv[k].put());
    }
    td.SampleDesc.Count = msaa;
    if (msaa > 1) {
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, cov_ms_tex.put()))) { msaa = 1; td.SampleDesc.Count = 1; }
        else dev->CreateRenderTargetView(cov_ms_tex.get(), nullptr, cov_ms_rtv.put());
    }
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, ds_tex.put()))) return false;
    dev->CreateDepthStencilView(ds_tex.get(), nullptr, dsv.put());
    w = W; h = H;
    return color_rtv && cov_rtv[0] && cov_rtv[1] && dsv;
}

bool Renderer::begin(const View& v, uint32_t bg) {
    Impl& I = *p_;
    if (!I.ok) return false;
    ID3D11DeviceContext* ctx = gfx_context();
    if (!I.targets(std::max(8, v.w), std::max(8, v.h))) { I.ok = false; return false; }
    I.view = v;
    I.batches = I.instances = 0;
    ctx->OMGetRenderTargets(1, &I.save_rtv, &I.save_dsv);
    I.save_nvp = 1;
    ctx->RSGetViewports(&I.save_nvp, &I.save_vp);
    float c[4] = {((bg >> 16) & 255) / 255.f, ((bg >> 8) & 255) / 255.f, (bg & 255) / 255.f, 1};
    ctx->ClearRenderTargetView(I.color_rtv.get(), c);
    D3D11_VIEWPORT vp{0, 0, (float)I.w, (float)I.h, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(I.rs.get());
    ctx->GSSetShader(nullptr, nullptr, 0);
    ID3D11Buffer* cbs[1] = {I.cb.get()};
    ctx->VSSetConstantBuffers(0, 1, cbs);
    ctx->PSSetConstantBuffers(0, 1, cbs);
    return true;
}

void Renderer::end() {
    Impl& I = *p_;
    ID3D11DeviceContext* ctx = gfx_context();
    ID3D11ShaderResourceView* nul[2] = {nullptr, nullptr};
    ctx->PSSetShaderResources(0, 2, nul);
    ctx->OMSetRenderTargets(1, &I.save_rtv, I.save_dsv);
    if (I.save_nvp) ctx->RSSetViewports(1, &I.save_vp);
    if (I.save_rtv) I.save_rtv->Release();
    if (I.save_dsv) I.save_dsv->Release();
    I.save_rtv = nullptr; I.save_dsv = nullptr;
    const float bf[4] = {0, 0, 0, 0};
    ctx->OMSetDepthStencilState(I.ds_off.get(), 0);
    ctx->OMSetBlendState(nullptr, bf, 0xffffffff);
}

void Renderer::Impl::cb_set(const Xform& M, float px, float tscale, float sketch, float filter) {
    ID3D11DeviceContext* ctx = gfx_context();
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(ctx->Map(cb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
    float d[16] = {(float)M.a, (float)M.c, (float)M.tx, 0, (float)M.b, (float)M.d, (float)M.ty, 0, px, tscale, sketch, filter, 0.5f, 0, 0, 0};
    std::memcpy(ms.pData, d, sizeof d);
    ctx->Unmap(cb.get(), 0);
}

void Renderer::Impl::coverage(LayerMesh& m, const Xform& xf, int target, int filter, bool sketch) {
    ID3D11DeviceContext* ctx = gfx_context();
    if (!m.upload()) return;
    // local (relative to origin) -> world -> NDC
    const double sx = view.mirror ? -view.scale : view.scale;
    const double sy = view.mirror_y ? -view.scale : view.scale;
    // world -> (centre, rotate, mirror+scale) -> NDC
    const Xform V = Xform::scale(2.0 / w, 2.0 / h) * Xform::scale(sx, sy) * Xform::rotation(view.rot) * Xform::translate(-view.cx, -view.cy);
    const Xform LW = xf * Xform::translate(m.origin.x, m.origin.y);
    const Xform M = V * LW;
    const float px = (float)(1.0 / view.scale);
    // visible box in local coordinates
    const Xform inv = M.inverse();
    BBox vis;
    for (Vec2 c : {Vec2{-1, -1}, Vec2{1, -1}, Vec2{-1, 1}, Vec2{1, 1}}) vis.add(inv.apply(c));
    ID3D11RenderTargetView* rtv = msaa > 1 ? cov_ms_rtv.get() : cov_rtv[target].get();
    ctx->OMSetRenderTargets(1, &rtv, dsv.get());
    const float zero[4] = {0, 0, 0, 0};
    ctx->ClearRenderTargetView(rtv, zero);
    ctx->ClearDepthStencilView(dsv.get(), D3D11_CLEAR_STENCIL, 1.f, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const float bf[4] = {0, 0, 0, 0};
    int cur_shader = -1, cur_blend = -1;
    float cur_ts = -1;
    const float filt = (float)filter;
    cb_set(M, px, 1, sketch ? 1.f : 0.f, filter >= 0 ? filt : -1.f);
    cur_ts = 1;
    ctx->OMSetDepthStencilState(ds_off.get(), 0);
    const UINT zero_inst = (UINT)(m.finst.size() - 1);
    for (const Batch& b : m.batches) {
        if (b.count == 0) continue;
        if (b.bb[0] > vis.x1 || b.bb[2] < vis.x0 || b.bb[1] > vis.y1 || b.bb[3] < vis.y0) continue;
        const Tmpl* t = b.type == T_FLASH && b.tmpl >= 0 && b.tmpl < (int)m.tmpls.size() ? &m.tmpls[b.tmpl] : nullptr;
        if (filter >= 0) {
            if (b.type == T_TRIS || b.type == T_BIG) continue;
            if (b.type == T_FLASH && (!t || t->dcode != filter || t->clear_part)) continue;
        }
        const int blend = filter >= 0 ? 0 : b.clear ? 1 : 0;
        if (blend != cur_blend) { ctx->OMSetBlendState(blend ? bl_clear.get() : bl_dark.get(), bf, 0xffffffff); cur_blend = blend; }
        ++batches;
        if (b.type == T_CAPS) {
            if (cur_shader != 1) {
                ctx->IASetInputLayout(il_caps.get());
                ctx->VSSetShader(vs_caps.get(), nullptr, 0);
                ctx->PSSetShader(ps_caps.get(), nullptr, 0);
                ID3D11Buffer* vbs[2] = {vb_corner.get(), m.vb_c.get()};
                UINT st[2] = {sizeof(F2), sizeof(Caps)}, of[2] = {0, 0};
                ctx->IASetVertexBuffers(0, 2, vbs, st, of);
                cur_shader = 1;
            }
            ctx->DrawInstanced(6, b.count, 0, b.start);
            instances += b.count;
            continue;
        }
        if (cur_shader != 0) {
            ctx->IASetInputLayout(il_flash.get());
            ctx->VSSetShader(vs_flash.get(), nullptr, 0);
            ctx->PSSetShader(ps_one.get(), nullptr, 0);
            cur_shader = 0;
        }
        UINT st[2] = {sizeof(F2), sizeof(F2)}, of[2] = {0, 0};
        if (b.type == T_FLASH && t) {
            const double spx = t->size * view.scale;
            const bool lod = spx < 1.5;
            const float ts = lod ? (float)std::max<double>(t->size, 1.0 / view.scale) : 1.f;
            if (ts != cur_ts) { cb_set(M, px, ts, sketch ? 1.f : 0.f, filter >= 0 ? filt : -1.f); cur_ts = ts; }
            ID3D11Buffer* vbs[2] = {lod ? vb_quad.get() : m.vb_t.get(), m.vb_f.get()};
            ctx->IASetVertexBuffers(0, 2, vbs, st, of);
            if (lod) ctx->DrawInstanced(6, b.count, 0, b.start);
            else ctx->DrawInstanced(t->count, b.count, t->first, b.start);
            instances += b.count;
        } else if (b.type == T_TRIS) {
            if (cur_ts != 1) { cb_set(M, px, 1, sketch ? 1.f : 0.f, -1.f); cur_ts = 1; }
            ID3D11Buffer* vbs[2] = {m.vb_r.get(), m.vb_f.get()};
            ctx->IASetVertexBuffers(0, 2, vbs, st, of);
            ctx->DrawInstanced(b.count, 1, b.start, zero_inst);
            instances += b.count / 3;
        } else if (b.type == T_BIG) {
            if (cur_ts != 1) { cb_set(M, px, 1, sketch ? 1.f : 0.f, -1.f); cur_ts = 1; }
            ID3D11Buffer* vbs[2] = {m.vb_b.get(), m.vb_f.get()};
            ctx->IASetVertexBuffers(0, 2, vbs, st, of);
            ctx->OMSetBlendState(bl_none.get(), bf, 0xffffffff);
            ctx->OMSetDepthStencilState(ds_inv.get(), 0);
            ctx->DrawInstanced(b.count, 1, b.start, zero_inst);
            ctx->OMSetBlendState(b.clear ? bl_clear.get() : bl_dark.get(), bf, 0xffffffff);
            ctx->OMSetDepthStencilState(ds_cover.get(), 0);
            ctx->DrawInstanced(6, 1, b.start + b.count, zero_inst);
            ctx->OMSetDepthStencilState(ds_off.get(), 0);
            instances += 1;
        }
    }
    if (msaa > 1) ctx->ResolveSubresource(cov_tex[target].get(), 0, cov_ms_tex.get(), 0, DXGI_FORMAT_R8_UNORM);
}

void Renderer::Impl::composite(int mode, bool neg, const float* c1, const float* c2, const float* c3, float fill_alpha) {
    ID3D11DeviceContext* ctx = gfx_context();
    ID3D11RenderTargetView* rtv = color_rtv.get();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    const float bf[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(bl_comp.get(), bf, 0xffffffff);
    ctx->OMSetDepthStencilState(ds_off.get(), 0);
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(ctx->Map(cbc.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        float d[16] = {c1[0], c1[1], c1[2], c1[3], (float)mode, neg ? 1.f : 0.f, fill_alpha, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        if (c2) std::memcpy(d + 8, c2, 16);
        if (c3) std::memcpy(d + 12, c3, 16);
        std::memcpy(ms.pData, d, sizeof d);
        ctx->Unmap(cbc.get(), 0);
    }
    ID3D11Buffer* cbs[1] = {cbc.get()};
    ctx->PSSetConstantBuffers(0, 1, cbs);
    ID3D11ShaderResourceView* srv[2] = {cov_srv[0].get(), cov_srv[1].get()};
    ctx->PSSetShaderResources(0, 2, srv);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(vs_full.get(), nullptr, 0);
    ctx->PSSetShader(ps_comp.get(), nullptr, 0);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nul[2] = {nullptr, nullptr};
    ctx->PSSetShaderResources(0, 2, nul);
    cbs[0] = cb.get();
    ctx->PSSetConstantBuffers(0, 1, cbs);
}

void Renderer::draw_layer(LayerMesh& m, const Xform& xf, uint32_t argb, float alpha, FillMode mode, bool negative) {
    Impl& I = *p_;
    if (!I.ok) return;
    I.coverage(m, xf, 0, -1, mode == FillMode::Sketch);
    float c[4];
    premul(argb, alpha, c);
    I.composite(mode == FillMode::Solid ? 0 : 1, negative, c, nullptr, nullptr, mode == FillMode::Solid ? 1.f : 0.12f);
}

void Renderer::draw_compare(LayerMesh& a, const Xform& xa, LayerMesh& b, const Xform& xb, uint32_t ca, uint32_t cbb, uint32_t cc) {
    Impl& I = *p_;
    if (!I.ok) return;
    I.coverage(a, xa, 0, -1, false);
    I.coverage(b, xb, 1, -1, false);
    float c1[4], c2[4], c3[4];
    premul(ca, 1, c1); premul(cbb, 1, c2); premul(cc, 1, c3);
    I.composite(2, false, c1, c2, c3, 1);
}

void Renderer::draw_highlight(LayerMesh& m, const Xform& xf, int dcode, uint32_t argb, float alpha) {
    Impl& I = *p_;
    if (!I.ok) return;
    I.coverage(m, xf, 0, dcode, false);
    float c[4];
    premul(argb, alpha, c);
    I.composite(0, false, c, nullptr, nullptr, 1);
}

} // namespace pcbp::gui::ggpu
