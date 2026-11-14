// aoi_align.cpp — see aoi_align.h. Ported from TrayTagPrinter src/fiducial_align.cpp (Corey's own code).
#include "aoi_align.h"
#include <algorithm>
#include <cmath>

namespace pcbp::aoi {
namespace {
constexpr double kWindowMm = 4.0;
constexpr int kMinBlobPixels = 8;
constexpr int kRedThresh = 180;

struct Blob { bool found = false; double cx = 0, cy = 0; int count = 0; double circ = 0; };

Blob best_blob(const std::vector<uint8_t>& bin, int w, int h) {
    Blob best;
    std::vector<uint8_t> vis(bin.size(), 0);
    std::vector<int> st;
    double best_score = -1;
    for (int sy = 0; sy < h; ++sy)
        for (int sx = 0; sx < w; ++sx) {
            const int i0 = sy * w + sx;
            if (vis[(size_t)i0]) continue;
            vis[(size_t)i0] = 1;
            if (!bin[(size_t)i0]) continue;
            st.assign(1, i0);
            double sumx = 0, sumy = 0; int count = 0, perim = 0;
            while (!st.empty()) {
                const int i = st.back(); st.pop_back();
                const int x = i % w, y = i / w;
                sumx += x; sumy += y; ++count;
                bool edge = false;
                const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
                for (int k = 0; k < 4; ++k) {
                    if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h || !bin[(size_t)(ny[k] * w + nx[k])]) { edge = true; continue; }
                    const int j = ny[k] * w + nx[k];
                    if (!vis[(size_t)j]) { vis[(size_t)j] = 1; st.push_back(j); }
                }
                if (edge) ++perim;
            }
            if (count < kMinBlobPixels) continue;
            const double circ = perim > 0 ? 4.0 * 3.14159265358979 * count / ((double)perim * perim) : 0;
            const double score = count * circ;
            if (score > best_score) { best_score = score; best = {true, sumx / count, sumy / count, count, circ}; }
        }
    return best;
}
} // namespace

bool Affine::inverse(Affine& o) const {
    const double det = m00 * m11 - m01 * m10;
    if (std::fabs(det) < 1e-15) return false;
    o.m00 = m11 / det; o.m01 = -m01 / det; o.m10 = -m10 / det; o.m11 = m00 / det;
    o.m02 = -(o.m00 * m02 + o.m01 * m12);
    o.m12 = -(o.m10 * m02 + o.m11 * m12);
    return true;
}
Affine compose(const Affine& a, const Affine& b) {
    Affine t;
    t.m00 = a.m00 * b.m00 + a.m01 * b.m10; t.m01 = a.m00 * b.m01 + a.m01 * b.m11; t.m02 = a.m00 * b.m02 + a.m01 * b.m12 + a.m02;
    t.m10 = a.m10 * b.m00 + a.m11 * b.m10; t.m11 = a.m10 * b.m01 + a.m11 * b.m11; t.m12 = a.m10 * b.m02 + a.m11 * b.m12 + a.m12;
    return t;
}

Affine stretch_to_bbox(int w, int h, double x0, double y0, double x1, double y1) {
    Affine t;
    if (w <= 0 || h <= 0 || x1 <= x0 || y1 <= y0) return t;
    const double sx = w / (x1 - x0), sy = h / (y1 - y0);
    t.m00 = sx; t.m01 = 0; t.m02 = -sx * x0;
    t.m10 = 0; t.m11 = -sy; t.m12 = sy * y1;
    return t;
}

bool align_pairs(const Affine& base, double mm1x, double mm1y, double mm2x, double mm2y,
                 double px1x, double px1y, double px2x, double px2y, Affine& out) {
    out = base;
    double e1x, e1y, e2x, e2y;
    base.apply(mm1x, mm1y, e1x, e1y);
    base.apply(mm2x, mm2y, e2x, e2y);
    const double ecx = (e1x + e2x) * 0.5, ecy = (e1y + e2y) * 0.5, mcx = (px1x + px2x) * 0.5, mcy = (px1y + px2y) * 0.5;
    const double edx = e1x - e2x, edy = e1y - e2y, mdx = px1x - px2x, mdy = px1y - px2y;
    const double ed = std::hypot(edx, edy), md = std::hypot(mdx, mdy);
    if (md < 0.001 || ed < 0.001) return false;
    const double s = md / ed, th = std::atan2(mdy, mdx) - std::atan2(edy, edx);
    Affine c;
    c.m00 = std::cos(th) * s; c.m01 = -std::sin(th) * s;
    c.m10 = std::sin(th) * s; c.m11 = std::cos(th) * s;
    c.m02 = mcx - (c.m00 * ecx + c.m01 * ecy);
    c.m12 = mcy - (c.m10 * ecx + c.m11 * ecy);
    out = compose(c, base);
    return true;
}

FidHit detect_fid(const uint8_t* px, int w, int h, int stride, const Affine& base, double mm_x, double mm_y, double dia) {
    FidHit r; r.mm_x = mm_x; r.mm_y = mm_y; r.dia_mm = dia > 0.01 ? dia : 1.35;
    if (!px || w <= 0 || h <= 0) return r;
    double cx, cy; base.apply(mm_x, mm_y, cx, cy);
    const double ppm_x = std::hypot(base.m00, base.m10), ppm_y = std::hypot(base.m01, base.m11);
    const double win = std::max(r.dia_mm * 3.0, kWindowMm);
    int ww = (int)(win * ppm_x), wh = (int)(win * ppm_y);
    if (ww < 4 || wh < 4) return r;
    int x0 = std::max(0, (int)(cx - ww / 2.0)), y0 = std::max(0, (int)(cy - wh / 2.0));
    if (x0 + ww >= w) ww = w - x0 - 1;
    if (y0 + wh >= h) wh = h - y0 - 1;
    if (ww < 4 || wh < 4) return r;
    auto red = [&](int x, int y) { return (int)px[(size_t)y * stride + (size_t)x * 4 + 2]; };
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<uint8_t> bin((size_t)ww * wh);
        if (pass == 0) {
            for (int y = 0; y < wh; ++y) for (int x = 0; x < ww; ++x) bin[(size_t)(y * ww + x)] = red(x0 + x, y0 + y) >= kRedThresh;
        } else {   // histogram equalisation of the window (TTP equalize_window)
            int hist[256] = {}, cdf[256];
            for (int y = 0; y < wh; ++y) for (int x = 0; x < ww; ++x) hist[red(x0 + x, y0 + y)]++;
            cdf[0] = hist[0]; for (int i = 1; i < 256; ++i) cdf[i] = cdf[i - 1] + hist[i];
            int cmin = 0; for (int i = 0; i < 256; ++i) if (cdf[i]) { cmin = cdf[i]; break; }
            const int total = ww * wh;
            if (total == cmin) break;
            for (int y = 0; y < wh; ++y) for (int x = 0; x < ww; ++x) {
                const double v = (cdf[red(x0 + x, y0 + y)] - cmin) / (double)(total - cmin) * 255.0;
                bin[(size_t)(y * ww + x)] = std::clamp(v, 0.0, 255.0) >= kRedThresh;
            }
        }
        Blob b = best_blob(bin, ww, wh);
        if (b.found) {
            r.found = true; r.histogram = pass == 1;
            r.px_x = x0 + b.cx; r.px_y = y0 + b.cy; r.pixels = b.count; r.circularity = b.circ;
            return r;
        }
    }
    return r;
}

bool auto_align(const uint8_t* px, int w, int h, int stride, double x0, double y0, double x1, double y1,
                std::vector<FidHit>& fids, Affine& out) {
    const Affine base = stretch_to_bbox(w, h, x0, y0, x1, y1);
    out = base;
    std::vector<const FidHit*> got;
    for (auto& f : fids) {
        f = detect_fid(px, w, h, stride, base, f.mm_x, f.mm_y, f.dia_mm);
        if (f.found) got.push_back(&f);
    }
    if (got.size() < 2) return false;
    // TTP uses the first two found; we take the two farthest apart (more leverage, same math).
    size_t a = 0, b = 1; double best = -1;
    for (size_t i = 0; i < got.size(); ++i) for (size_t j = i + 1; j < got.size(); ++j) {
        const double d = std::hypot(got[i]->mm_x - got[j]->mm_x, got[i]->mm_y - got[j]->mm_y);
        if (d > best) { best = d; a = i; b = j; }
    }
    return align_pairs(base, got[a]->mm_x, got[a]->mm_y, got[b]->mm_x, got[b]->mm_y, got[a]->px_x, got[a]->px_y, got[b]->px_x, got[b]->px_y, out);
}

} // namespace pcbp::aoi
