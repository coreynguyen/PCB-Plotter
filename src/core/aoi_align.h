#pragma once
// ============================================================================
// aoi_align.h (core, portable) — align an AOI board picture to placement coordinates (R23).
//
// Port of Corey's TrayTagPrinter fiducial_align.cpp (itself from his jpv.ms MaxScript Coordinate Tool), with
// GDI+ replaced by a plain BGRA buffer:
//   1. base = stretch the image over the board rectangle (mm -> px, Y flipped: image row 0 = board top);
//   2. per fiducial: window of max(3 x diameter, 4 mm) around its expected pixel, threshold red >= 180
//      (retry on a histogram-equalised window), 4-connected blobs, best = count x circularity, centroid;
//   3. two found fiducials -> similarity correction C (px space) taking the expected pixels to the measured
//      ones; final = C o base.
// The same correction is used when the user clicks the two fiducials on the image by hand.
// ============================================================================
#include <cstdint>
#include <vector>

namespace pcbp::aoi {

struct Affine {   // px = m00*x + m01*y + m02 ; py = m10*x + m11*y + m12   (x, y in mm)
    double m00 = 1, m01 = 0, m02 = 0, m10 = 0, m11 = 1, m12 = 0;
    void apply(double x, double y, double& ox, double& oy) const { ox = m00 * x + m01 * y + m02; oy = m10 * x + m11 * y + m12; }
    bool inverse(Affine& out) const;
};
Affine compose(const Affine& a, const Affine& b);   // a o b (b first)

// Image of w x h pixels stretched over the board rectangle [x0,x1] x [y0,y1] mm.
Affine stretch_to_bbox(int w, int h, double x0, double y0, double x1, double y1);
// base corrected so that fiducial mm1 lands on pixel px1 and mm2 on px2 (similarity in pixel space).
// false (out = base) when the points coincide.
bool align_pairs(const Affine& base, double mm1x, double mm1y, double mm2x, double mm2y,
                 double px1x, double px1y, double px2x, double px2y, Affine& out);

struct FidHit {
    double mm_x = 0, mm_y = 0, dia_mm = 1.35;
    bool found = false, histogram = false;
    double px_x = 0, px_y = 0;
    int pixels = 0;
    double circularity = 0;
};
// Detect one fiducial (bgra: 4 bytes per pixel, R at +2).
FidHit detect_fid(const uint8_t* bgra, int w, int h, int stride, const Affine& base, double mm_x, double mm_y, double dia_mm);
// Auto alignment: returns true when 2+ fiducials were found (out = corrected), else out = base.
bool auto_align(const uint8_t* bgra, int w, int h, int stride, double x0, double y0, double x1, double y1,
                std::vector<FidHit>& fids, Affine& out);

} // namespace pcbp::aoi
