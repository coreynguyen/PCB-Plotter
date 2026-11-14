// align_pads.cpp — see align_pads.h
#include "align_pads.h"
#include <cmath>

namespace pcbp::io {

static bool in_poly(Vec2 p, const std::vector<Vec2>& q) {
    bool in = false;
    for (size_t i = 0, j = q.size() - 1; i < q.size(); j = i++)
        if (((q[i].y > p.y) != (q[j].y > p.y)) && (p.x < (q[j].x - q[i].x) * (p.y - q[i].y) / (q[j].y - q[i].y) + q[i].x)) in = !in;
    return in;
}

std::vector<Vec2> box_quad(const BBox& b) { return {{b.x0, b.y0}, {b.x1, b.y0}, {b.x1, b.y1}, {b.x0, b.y1}}; }

std::vector<Vec2> gerber_pad_centres(const gerber::Layer& L, const Xform& t, const std::vector<Vec2>& quad,
                                     const std::function<bool(int)>& visible) {
    std::vector<Vec2> out;
    if (quad.size() < 3) return out;
    for (int oi = 0; oi < (int)L.objs.size(); ++oi) {
        const gerber::Object& o = L.objs[oi];
        if (o.clear || (visible && !visible(oi))) continue;
        Vec2 c;
        if (o.kind == gerber::Object::Flash) c = o.a;
        else if (o.kind == gerber::Object::Region) c = gerber::object_bbox(L, o).center();
        else if (o.kind == gerber::Object::Draw && std::hypot(o.a.x - o.b.x, o.a.y - o.b.y) < 1e-7) c = o.a;
        else continue;
        c = t.apply(c);
        if (!in_poly(c, quad)) continue;
        bool dup = false;
        for (auto& q : out) if (std::fabs(q.x - c.x) < 1e-3 && std::fabs(q.y - c.y) < 1e-3) { dup = true; break; }
        if (!dup) out.push_back(c);
    }
    return out;
}

std::vector<Pad> padset_board_pads(const digitize::PadSet& ps, const Xform& lt, const std::vector<Vec2>& quad) {
    std::vector<Pad> out;
    if (quad.size() < 3) return out;
    const Xform ti = lt.inverse();
    std::vector<Vec2> lq;
    for (auto& q : quad) lq.push_back(ti.apply(q));
    BBox qb;
    for (auto& q : lq) qb.add(q);
    qb.inflate(8);
    const bool lmir = lt.det() < 0;
    const double lrot = std::atan2(lmir ? -lt.b : lt.b, lmir ? -lt.a : lt.a) * 57.29577951308232;
    for (int j : ps.query(qb)) {
        const digitize::PadFeature& f = ps.pads[j];
        if (!in_poly(f.c, lq)) continue;
        Pad pd;
        pd.pos = lt.apply(f.c);
        const double base = f.oriented ? f.oang : 0.0;
        pd.w = f.oriented ? f.ow : f.w;
        pd.h = f.oriented ? f.oh : f.h;
        pd.rot = norm_deg(lrot + (lmir ? -base : base));
        pd.shape = f.shape;
        pd.drill = f.hole > 0 ? f.hole : 0;
        bool dup = false;
        for (auto& q : out) if (dist(q.pos, pd.pos) < 0.01) { dup = true; break; }
        if (!dup) out.push_back(pd);
    }
    return out;
}

} // namespace pcbp::io
