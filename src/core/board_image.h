#pragma once
// ============================================================================
// board_image.h — board photo / AOI image underlays and their registration
// (pcbp, portable; LEAD_BRIEF §14).
//
// An image is mapped to board coordinates (mm, +Y up, root image) by a 3x3
// homography H:  [X Y W]^T = H [u v 1]^T,  board = (X/W, Y/W); (u, v) are
// image pixel coordinates (origin top-left, v down, pixel centres at +0.5).
//
// Calibration from user picked point pairs (image px -> board mm):
//   1 pair    translation only (keeps current scale/rotation)
//   2 pairs   similarity: scale + rotation + translation (no shear; mirror
//             only if `mirrored` is set, e.g. bottom-side scans)
//   3 pairs   affine
//   >=4       perspective homography (normalised DLT, least squares for >4)
// rms() reports the residual in mm at the picked points.
//
// Lens distortion is NOT modelled (see specs/ODB_RE.md §B "Later"): a
// Brown-Conrady k1/k2 pre-warp would be applied to (u, v) before H.
// ============================================================================
#include "geom.h"
#include <cstdint>
#include <string>
#include <vector>

namespace pcbp {

struct ImagePointPair { Vec2 img; Vec2 board; };

struct BoardImage {
    std::string name;
    std::string path;               // original file (reference)
    std::vector<uint8_t> data;      // embedded encoded file (png/jpg/bmp/ppm); empty = use path
    bool embed = true;
    int px_w = 0, px_h = 0;         // image size in pixels
    double H[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    int calib = 0;                  // pairs used by the last calibration (0 = default fit)
    std::vector<ImagePointPair> pairs;
    bool mirrored = false;          // 2-point solve with X mirror (image of the bottom side, unflipped)
    float opacity = 0.6f;
    bool visible = true;
    bool locked = false;
    int side = 0;                   // 0 top, 1 bottom: shown when that side is viewed (2 = always)
    double rms_mm = 0;
    uint32_t revision = 0;          // bumped on pixel changes (GPU cache key)
};

// homography helpers (row-major 3x3)
Vec2 h_apply(const double H[9], Vec2 p);
bool h_invert(const double H[9], double out[9]);
void h_identity(double H[9]);

// Solvers; return false when degenerate (coincident / collinear points).
bool solve_translation(const BoardImage& cur, const ImagePointPair& p, double H[9]);
bool solve_similarity(const ImagePointPair& a, const ImagePointPair& b, bool mirror, double H[9]);
bool solve_affine(const ImagePointPair* p, double H[9]);                 // 3 pairs
bool solve_homography(const ImagePointPair* p, size_t n, double H[9]);   // n >= 4

// Uses img.pairs (see table above); updates H, calib, rms_mm. err explains failures.
bool calibrate(BoardImage& img, std::string& err);
double rms(const BoardImage& img);

// Default placement: fit the image into `board` (mm) keeping aspect, Y up.
void fit_to_box(BoardImage& img, const BBox& board);
// Board-space corners of the image (TL, TR, BR, BL).
void image_corners(const BoardImage& img, Vec2 out[4]);

// Tiny portable decoder for tests / Linux: binary PPM (P6) and PGM (P5) -> RGBA.
bool decode_pnm(const uint8_t* p, size_t n, int& w, int& h, std::vector<uint8_t>& rgba, std::string& err);
// Header-only size probe for PNG / JPEG / BMP / PNM (no decoding).
bool probe_image_size(const uint8_t* p, size_t n, int& w, int& h);

std::string base64_encode(const std::vector<uint8_t>& d);
bool base64_decode(const std::string& s, std::vector<uint8_t>& out);

} // namespace pcbp
