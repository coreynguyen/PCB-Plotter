// ============================================================================
// board_image.cpp — image registration math + tiny decoders (see board_image.h)
// ============================================================================
#include "board_image.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace pcbp {

void h_identity(double H[9]) { const double I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; std::memcpy(H, I, sizeof I); }

Vec2 h_apply(const double H[9], Vec2 p) {
    const double X = H[0] * p.x + H[1] * p.y + H[2];
    const double Y = H[3] * p.x + H[4] * p.y + H[5];
    const double W = H[6] * p.x + H[7] * p.y + H[8];
    if (std::fabs(W) < 1e-15) return {0, 0};
    return {X / W, Y / W};
}

bool h_invert(const double m[9], double o[9]) {
    const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const double det = a * A + b * B + c * C;
    if (std::fabs(det) < 1e-300) return false;
    const double r = 1.0 / det;
    o[0] = A * r; o[1] = -(b * i - c * h) * r; o[2] = (b * f - c * e) * r;
    o[3] = B * r; o[4] = (a * i - c * g) * r;  o[5] = -(a * f - c * d) * r;
    o[6] = C * r; o[7] = -(a * h - b * g) * r; o[8] = (a * e - b * d) * r;
    return true;
}

bool solve_translation(const BoardImage& cur, const ImagePointPair& p, double H[9]) {
    std::memcpy(H, cur.H, sizeof cur.H);
    Vec2 now = h_apply(H, p.img);
    // shift in board space: H' = T(d) * H
    const double dx = p.board.x - now.x, dy = p.board.y - now.y;
    for (int k = 0; k < 3; ++k) { H[k] += dx * H[6 + k]; H[3 + k] += dy * H[6 + k]; }
    return true;
}

bool solve_similarity(const ImagePointPair& a, const ImagePointPair& b, bool mirror, double H[9]) {
    // image v points down; board y up. Work in "flipped" image coords (u, -v), optionally mirrored.
    auto f = [&](Vec2 p) { return Vec2{mirror ? -p.x : p.x, -p.y}; };
    const Vec2 ia = f(a.img), ib = f(b.img);
    const double ux = ib.x - ia.x, uy = ib.y - ia.y, bx = b.board.x - a.board.x, by = b.board.y - a.board.y;
    const double l2 = ux * ux + uy * uy;
    if (l2 < 1e-12 || bx * bx + by * by < 1e-12) return false;
    // complex ratio s*e^{i th} = (bx+i by)/(ux+i uy)
    const double c = (bx * ux + by * uy) / l2, s = (by * ux - bx * uy) / l2;
    // board = R*f(img) + t with R = [[c,-s],[s,c]]
    const double tx = a.board.x - (c * ia.x - s * ia.y), ty = a.board.y - (s * ia.x + c * ia.y);
    const double fx = mirror ? -1 : 1;
    H[0] = c * fx; H[1] = s;  H[2] = tx;     // -s * (-v) = s v
    H[3] = s * fx; H[4] = -c; H[5] = ty;
    H[6] = 0; H[7] = 0; H[8] = 1;
    return true;
}

namespace {
// Gaussian elimination with partial pivoting, n <= 9
bool solve_lin(std::vector<double>& A, std::vector<double>& b, int n) {
    for (int c = 0; c < n; ++c) {
        int piv = c;
        for (int r = c + 1; r < n; ++r) if (std::fabs(A[r * n + c]) > std::fabs(A[piv * n + c])) piv = r;
        if (std::fabs(A[piv * n + c]) < 1e-12) return false;
        if (piv != c) { for (int k = 0; k < n; ++k) std::swap(A[c * n + k], A[piv * n + k]); std::swap(b[c], b[piv]); }
        for (int r = 0; r < n; ++r) {
            if (r == c) continue;
            const double m = A[r * n + c] / A[c * n + c];
            if (m == 0) continue;
            for (int k = c; k < n; ++k) A[r * n + k] -= m * A[c * n + k];
            b[r] -= m * b[c];
        }
    }
    for (int c = 0; c < n; ++c) b[c] /= A[c * n + c];
    return true;
}
// Hartley normalisation: centroid at 0, mean distance sqrt(2)
void norm_xf(const std::vector<Vec2>& pts, double T[9]) {
    double cx = 0, cy = 0;
    for (auto& p : pts) { cx += p.x; cy += p.y; }
    cx /= pts.size(); cy /= pts.size();
    double md = 0;
    for (auto& p : pts) md += std::hypot(p.x - cx, p.y - cy);
    md /= pts.size();
    const double s = md > 1e-12 ? std::sqrt(2.0) / md : 1;
    const double t[9] = {s, 0, -s * cx, 0, s, -s * cy, 0, 0, 1};
    std::memcpy(T, t, sizeof t);
}
void mul(const double A[9], const double B[9], double O[9]) {
    double r[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i * 3 + j] = A[i * 3] * B[j] + A[i * 3 + 1] * B[3 + j] + A[i * 3 + 2] * B[6 + j];
    std::memcpy(O, r, sizeof r);
}
double cross(Vec2 a, Vec2 b, Vec2 c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }
} // namespace

bool solve_affine(const ImagePointPair* p, double H[9]) {
    if (std::fabs(cross(p[0].img, p[1].img, p[2].img)) < 1e-9 || std::fabs(cross(p[0].board, p[1].board, p[2].board)) < 1e-9) return false;
    std::vector<double> A(9), bx(3), by(3);
    for (int i = 0; i < 3; ++i) {
        A[i * 3] = p[i].img.x; A[i * 3 + 1] = p[i].img.y; A[i * 3 + 2] = 1;
        bx[i] = p[i].board.x; by[i] = p[i].board.y;
    }
    std::vector<double> A2 = A;
    if (!solve_lin(A, bx, 3) || !solve_lin(A2, by, 3)) return false;
    H[0] = bx[0]; H[1] = bx[1]; H[2] = bx[2];
    H[3] = by[0]; H[4] = by[1]; H[5] = by[2];
    H[6] = 0; H[7] = 0; H[8] = 1;
    return true;
}

bool solve_homography(const ImagePointPair* p, size_t n, double H[9]) {
    if (n < 4) return false;
    std::vector<Vec2> si, sb;
    for (size_t i = 0; i < n; ++i) { si.push_back(p[i].img); sb.push_back(p[i].board); }
    // degeneracy: any 3 of the first 4 collinear
    for (int a = 0; a < 4; ++a) {
        Vec2 q[3]; int k = 0;
        for (int b = 0; b < 4; ++b) if (b != a) q[k++] = si[b];
        double scale = 0;
        for (auto& v : si) scale = std::max(scale, std::hypot(v.x - si[0].x, v.y - si[0].y));
        if (std::fabs(cross(q[0], q[1], q[2])) < 1e-6 * scale * scale) return false;
    }
    double Ti[9], Tb[9];
    norm_xf(si, Ti); norm_xf(sb, Tb);
    // h8 = 1 normal equations of the 2n x 8 system
    std::vector<double> AtA(64, 0), Atb(8, 0);
    for (size_t i = 0; i < n; ++i) {
        Vec2 u = h_apply(Ti, si[i]), X = h_apply(Tb, sb[i]);
        const double r1[8] = {u.x, u.y, 1, 0, 0, 0, -u.x * X.x, -u.y * X.x};
        const double r2[8] = {0, 0, 0, u.x, u.y, 1, -u.x * X.y, -u.y * X.y};
        for (int a = 0; a < 8; ++a) {
            for (int b = 0; b < 8; ++b) AtA[a * 8 + b] += r1[a] * r1[b] + r2[a] * r2[b];
            Atb[a] += r1[a] * X.x + r2[a] * X.y;
        }
    }
    if (!solve_lin(AtA, Atb, 8)) return false;
    double Hn[9] = {Atb[0], Atb[1], Atb[2], Atb[3], Atb[4], Atb[5], Atb[6], Atb[7], 1};
    double Tbi[9];
    if (!h_invert(Tb, Tbi)) return false;
    double t[9];
    mul(Tbi, Hn, t);
    mul(t, Ti, H);
    if (std::fabs(H[8]) > 1e-15) for (int k = 0; k < 9; ++k) H[k] /= H[8] == 0 ? 1 : H[8];
    // must not flip W sign over the image region (point behind the camera)
    return true;
}

double rms(const BoardImage& img) {
    if (img.pairs.empty()) return 0;
    double s = 0;
    for (auto& p : img.pairs) { Vec2 q = h_apply(img.H, p.img); s += (q.x - p.board.x) * (q.x - p.board.x) + (q.y - p.board.y) * (q.y - p.board.y); }
    return std::sqrt(s / img.pairs.size());
}

bool calibrate(BoardImage& img, std::string& err) {
    const size_t n = img.pairs.size();
    double H[9];
    bool ok = false;
    if (n == 0) { err = "pick at least one image point and its board point"; return false; }
    if (n == 1) ok = solve_translation(img, img.pairs[0], H);
    else if (n == 2) ok = solve_similarity(img.pairs[0], img.pairs[1], img.mirrored, H);
    else if (n == 3) ok = solve_affine(img.pairs.data(), H);
    else ok = solve_homography(img.pairs.data(), n, H);
    if (!ok) { err = n >= 3 ? "points are collinear or coincide: spread them out (use the four board corners or fiducials)" : "the two points coincide"; return false; }
    std::memcpy(img.H, H, sizeof H);
    img.calib = (int)n;
    img.rms_mm = rms(img);
    return true;
}

void fit_to_box(BoardImage& img, const BBox& b0) {
    BBox b = b0;
    if (b.empty() || b.w() <= 0 || b.h() <= 0) { b = BBox{}; b.add({0, 0}); b.add({100, 80}); }
    const double w = std::max(1, img.px_w), h = std::max(1, img.px_h);
    const double s = std::min(b.w() / w, b.h() / h);
    const double ox = b.x0 + (b.w() - w * s) / 2, oy = b.y0 + (b.h() - h * s) / 2;
    // board = (ox + u s, oy + (h - v) s)
    const double H[9] = {s, 0, ox, 0, -s, oy + h * s, 0, 0, 1};
    std::memcpy(img.H, H, sizeof H);
    img.calib = 0;
}

void image_corners(const BoardImage& img, Vec2 out[4]) {
    const double w = img.px_w, h = img.px_h;
    out[0] = h_apply(img.H, {0, 0}); out[1] = h_apply(img.H, {w, 0});
    out[2] = h_apply(img.H, {w, h}); out[3] = h_apply(img.H, {0, h});
}

// ---------------------------------------------------------------------------
static bool pnm_header(const uint8_t* p, size_t n, int vals[3], size_t& i, std::string& err) {
    if (n < 3 || p[0] != 'P' || (p[1] != '6' && p[1] != '5')) { err = "not a binary PPM/PGM"; return false; }
    i = 2;
    for (int k = 0; k < 3; ++k) {
        while (i < n && (std::isspace(p[i]) || p[i] == '#')) {
            if (p[i] == '#') while (i < n && p[i] != '\n') ++i;
            else ++i;
        }
        int v = 0, digits = 0;
        while (i < n && p[i] >= '0' && p[i] <= '9') { v = v * 10 + (p[i] - '0'); ++i; ++digits; if (v > 1 << 20) break; }
        if (!digits) { err = "bad PNM header"; return false; }
        vals[k] = v;
    }
    return true;
}

bool decode_pnm(const uint8_t* p, size_t n, int& w, int& h, std::vector<uint8_t>& rgba, std::string& err) {
    int vals[3] = {0, 0, 0};
    size_t i = 0;
    if (!pnm_header(p, n, vals, i, err)) return false;
    const bool gray = p[1] == '5';
    ++i;   // single whitespace
    w = vals[0]; h = vals[1];
    if (w <= 0 || h <= 0 || vals[2] != 255) { err = "unsupported PNM (need maxval 255)"; return false; }
    const size_t need = (size_t)w * h * (gray ? 1 : 3);
    if (i + need > n) { err = "truncated PNM"; return false; }
    rgba.resize((size_t)w * h * 4);
    for (size_t k = 0; k < (size_t)w * h; ++k) {
        const uint8_t* s = p + i + k * (gray ? 1 : 3);
        rgba[k * 4] = s[0]; rgba[k * 4 + 1] = gray ? s[0] : s[1]; rgba[k * 4 + 2] = gray ? s[0] : s[2]; rgba[k * 4 + 3] = 255;
    }
    return true;
}

bool probe_image_size(const uint8_t* p, size_t n, int& w, int& h) {
    auto be32 = [&](size_t o) { return (int)((uint32_t)p[o] << 24 | (uint32_t)p[o + 1] << 16 | (uint32_t)p[o + 2] << 8 | p[o + 3]); };
    auto le32 = [&](size_t o) { return (int32_t)((uint32_t)p[o] | (uint32_t)p[o + 1] << 8 | (uint32_t)p[o + 2] << 16 | (uint32_t)p[o + 3] << 24); };
    if (n >= 24 && !std::memcmp(p, "\x89PNG", 4)) { w = be32(16); h = be32(20); return w > 0 && h > 0; }
    if (n >= 26 && p[0] == 'B' && p[1] == 'M') { w = le32(18); h = std::abs(le32(22)); return w > 0 && h > 0; }
    if (n >= 4 && p[0] == 0xFF && p[1] == 0xD8) {
        size_t i = 2;
        while (i + 9 < n) {
            if (p[i] != 0xFF) { ++i; continue; }
            const uint8_t m = p[i + 1];
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
            const size_t len = (size_t)p[i + 2] << 8 | p[i + 3];
            if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                h = p[i + 5] << 8 | p[i + 6]; w = p[i + 7] << 8 | p[i + 8];
                return w > 0 && h > 0;
            }
            i += 2 + len;
        }
        return false;
    }
    int vals[3];
    size_t i = 0;
    std::string e;
    if (pnm_header(p, n, vals, i, e) && vals[0] > 0 && vals[1] > 0) { w = vals[0]; h = vals[1]; return true; }
    return false;
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::vector<uint8_t>& d) {
    std::string s;
    s.reserve((d.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < d.size(); i += 3) {
        uint32_t v = d[i] << 16 | d[i + 1] << 8 | d[i + 2];
        s += kB64[v >> 18]; s += kB64[(v >> 12) & 63]; s += kB64[(v >> 6) & 63]; s += kB64[v & 63];
    }
    if (i < d.size()) {
        uint32_t v = d[i] << 16 | (i + 1 < d.size() ? d[i + 1] << 8 : 0);
        s += kB64[v >> 18]; s += kB64[(v >> 12) & 63];
        s += i + 1 < d.size() ? kB64[(v >> 6) & 63] : '=';
        s += '=';
    }
    return s;
}

bool base64_decode(const std::string& s, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(s.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        else return false;
        acc = acc << 6 | v; bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((uint8_t)(acc >> bits)); }
    }
    return true;
}

} // namespace pcbp
