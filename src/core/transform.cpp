// transform.cpp — see transform.h (R16c, REVIEW_V15 §9)
#include "transform.h"
#include <cmath>

namespace pcbp {

Xform transform_xform(const TransformSpec& s) {
    const Vec2 p = s.pivot;
    Xform S, M, R, D;
    if (s.scale) {
        const double fx = s.sx != 0 ? s.sx : 1, fy = s.uniform ? fx : (s.sy != 0 ? s.sy : 1);
        S = Xform::scale(fx, fy);
    }
    if (s.mirror) M = s.mirror_axis == 0 ? Xform::scale(-1, 1) : Xform::scale(1, -1);
    if (s.rotate) R = Xform::rotation(s.angle);
    if (s.translate) D = Xform::translate(s.dx, s.dy);
    return D * Xform::translate(p.x, p.y) * R * M * S * Xform::translate(-p.x, -p.y);
}

double transformed_angle(const Xform& T, double rot) {
    const double r = deg2rad(rot);
    const double x = T.a * std::cos(r) + T.c * std::sin(r), y = T.b * std::cos(r) + T.d * std::sin(r);
    double a = rad2deg(std::atan2(y, x));
    a = std::round(a * 1e9) / 1e9;
    return norm_deg(a);
}

void transform_component(Component& c, const Xform& T, bool flip_side) {
    Xform L = T; L.tx = L.ty = 0;
    c.pos = T.apply(c.pos);
    c.label_off = L.apply(c.label_off);
    if (T.det() < 0 && flip_side) {
        // board flip: R(drot) S with S = scale(-1,1)  ->  rot' = drot - rot (bottom-view convention, as Align)
        const double k = std::sqrt(std::fabs(T.det()));
        const double a = -T.a / k, b = -T.b / k;
        const double drot = rad2deg(std::atan2(b, a));
        c.rot = norm_deg(std::round((drot - c.rot) * 1e9) / 1e9);
        c.label_rot = norm_deg(std::round((drot - c.label_rot) * 1e9) / 1e9);
        c.side = c.side == Side::Top ? Side::Bottom : Side::Top;
    } else {
        c.rot = transformed_angle(L, c.rot);
        c.label_rot = transformed_angle(L, c.label_rot);
    }
}

int apply_transform(Project& p, const TransformSpec& s) {
    if (!s.any()) return 0;
    const Xform T = transform_xform(s);
    int n = 0;
    for (auto& c : p.comps) {
        if (s.sides == 1 && c.side != Side::Top) continue;
        if (s.sides == 2 && c.side != Side::Bottom) continue;
        transform_component(c, T, s.mirror && s.flip_side);
        ++n;
    }
    if (s.board_outline) for (auto& pl : p.panel_frame) for (auto& q : pl) q = T.apply(q);
    if (s.circuit_outline) {
        for (auto& pl : p.board_frame) for (auto& q : pl) q = T.apply(q);
        // panel instances: conjugate so every circuit copy follows (mirror cancels; uniform scale scales the offset)
        const Xform Ti = T.inverse();
        for (size_t i = 1; i < p.images.size(); ++i) {
            PanelImage& im = p.images[i];
            const Xform J = T * Xform::translate(im.offset.x, im.offset.y) * Xform::rotation(im.rot) * Ti;
            im.offset = {J.tx, J.ty};
            const double k = std::sqrt(std::fabs(J.det()));
            im.rot = k > 0 ? norm_deg(std::round(rad2deg(std::atan2(J.b / k, J.a / k)) * 1e9) / 1e9) : im.rot;
        }
    }
    if (s.move_origin) p.virtual_origin = T.apply(p.virtual_origin);
    p.dirty = true;
    return n;
}

} // namespace pcbp
