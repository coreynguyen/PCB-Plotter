#pragma once
// ============================================================================
// panelize.h — CircuitCAM-style panelization (Board > Panelize, dialog #202
// "Panelization" and #321 "Step And Repeat Parameters"), portable.
//
// Model: a grid of cells (XIndex, YIndex), each optionally holding an image
// with a rotation 0/90/180/270 relative to the root image. One cell is the
// Root (yellow). For every rotation group the dialog asks for offsets
// measured between the *reference location* (a component centroid) of two
// images:
//   * 0 deg group:  "Yellow To Red" X  = distance root -> red image along X,
//                   "Yellow To Blue" Y = distance root -> blue image along Y.
//                   The pitch is that distance divided by the index distance.
//   * 90/180/270:   "Yellow To Red" X,Y  = root ref location -> ref location of
//                   the red image of that group, "Yellow To Blue" X,Y = root ->
//                   blue image. X pitch = (blueX - redX) / (blue.x - red.x),
//                   Y pitch = (blueY - redY) / (blue.y - red.y).
//                   Image offset = (col - red.x) * pitchX + base.x, same for Y,
//                   where base = ref - R(rot) * ref + YellowToRed so that the
//                   red image's reference component lands exactly YellowToRed
//                   away from the root's.
// Red / blue cells are chosen exactly like the original: cells are scanned
// X index outer, Y index inner; red = first non-root image of the group (for
// 0 deg it must be in another column than the root); blue = first further
// image in another row AND column than red (0 deg: another row than root),
// else any other image of the group. An offset group's edit fields are only
// enabled when the group has images (0 deg: a non-root 0 deg image).
// Image transform (matches Project::image_xform): p' = R(rot) * p + offset.
//
// Step And Repeat All Images: replicates the current image block nX * nY
// times; block (ix,iy) gets offset + (ix*dX, iy*dY) and indices
// (ix*W + x, iy*H + y) with W,H = block size in cells.
// ============================================================================
#include "project.h"
#include <string>
#include <vector>

namespace pcbp {

struct PanelCell {
    int x = 0, y = 0;           // grid indices (XIndex / YIndex)
    int rot = 0;                // 0, 90, 180, 270 (relative to root)
    bool root = false;
    bool assigned = false;      // panel coordinates computed ("grey background")
    bool x_out = false;
    Vec2 offset;                // valid when assigned (root: 0,0)
};

// Offsets for one rotation group, in PanelSetup::units.
struct RotGroupOffsets {
    double red_x = 0, red_y = 0;    // "Yellow To Red"  X / Y   (0 deg: only X used)
    double blue_x = 0, blue_y = 0;  // "Yellow To Blue" X / Y   (0 deg: only Y used)
};

struct PanelMarkers {
    int root_x = -1, root_y = -1;           // yellow
    int red_x[4]{-1, -1, -1, -1}, red_y[4]{-1, -1, -1, -1};
    int blue_x[4]{-1, -1, -1, -1}, blue_y[4]{-1, -1, -1, -1};
};

struct PanelSetup {
    std::vector<PanelCell> cells;
    RotGroupOffsets groups[4];              // index = rot / 90
    Units units = Units::MM;                // "Offset Units"
    std::string ref_location;               // "Reference Location" (component ref)

    // grid editing (the ActiveX grid + buttons)
    void reset();                                       // only a root image at (0,0)
    PanelCell* at(int x, int y);
    const PanelCell* at(int x, int y) const;
    PanelCell& set(int x, int y, int rot);              // add / change rotation
    bool remove(int x, int y);                          // "Delete..." (root cannot be deleted)
    bool assign_root(int x, int y);                     // "Assign Root"
    int max_x() const;
    int max_y() const;
    int count() const { return (int)cells.size(); }

    // load / store from project images (x_index / y_index / offset / rot)
    static PanelSetup from_images(const std::vector<PanelImage>& v);
};

PanelMarkers panel_markers(const PanelSetup& s);
// Which "Image Creation" rows are enabled (CircuitCAM greys out unused groups).
bool panel_group_enabled(const PanelSetup& s, int rot_index);
// Human readable list of what the user must enter (dialog guidance text).
std::vector<std::string> panel_requirements(const PanelSetup& s);

// "Panelize..." button: computes offsets for every non-root cell.
// ref_pos: centroid of the reference location component (mm); pass
// panel_ref_pos() for the project lookup.
bool panelize(PanelSetup& s, Vec2 ref_pos, std::string& err);
bool panel_ref_pos(const Project& prj, const std::string& ref, Vec2& out);

// "Step And Repeat All Images..." (#321). nx = Number in X, ny = Number in Y,
// dx/dy in s.units. Validation mirrors the dialog messages.
bool step_and_repeat_all(PanelSetup& s, int nx, int ny, double dx, double dy, std::string& err);

// Convert to project images: root first (name "1"), others in scan order.
std::vector<PanelImage> panel_images(const PanelSetup& s);
// Write the instances (images) into the project. R22: never creates or changes an outline (the retired
// panel_frame_outline / make_frame generator fabricated a board outline from circuit extents + margin).
void apply_panel(Project& prj, const PanelSetup& s);
// R22: fills the "Yellow To Red / Blue" entries (in s.units) from the cells' existing offsets (e.g. a setup made by
// from_images), so that panelize() reproduces the current array instead of collapsing it to zero offsets.
void panel_backfill_offsets(PanelSetup& s, Vec2 ref_pos);
// R22: PanelSetup <-> persisted dialog parameters (Project::array_params, "OK saves without applying").
void panel_setup_to_params(const PanelSetup& s, Project::ArrayParams& a);
bool panel_setup_from_params(const Project::ArrayParams& a, PanelSetup& s);   // false when nothing saved

} // namespace pcbp
