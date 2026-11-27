#pragma once
// ============================================================================
// import_extras.h — ODB++ step chooser (gui/odb_import_dialog.cpp) and board
// photo / AOI image underlays (gui/board_image.cpp).
// ============================================================================
#include "../app/app.h"
#include "../plugins/plugin.h"
#include <string>

namespace pcbp::app {

// Scans the ODB++ job, lists steps (type guess, size, top/bottom component
// counts, layers, step-repeat children) with the recommended one preselected,
// and fills o["step"] ("<step>[|images|flat][|layers]") plus the common CAD
// options. false = cancelled. A job that cannot be read shows the reason and
// returns false.
bool dlg_odb_import(HWND owner, const std::wstring& path, Options& o);
// Board > Rescale Coordinates (REVIEW_V9 §6C): unit fix (mm<->inch<->mil ...)
// or arbitrary factor applied to placements, frames, packages, panel images and
// Gerber/Excellon layers about (0,0); one undo step.
bool dlg_rescale_coordinates(HWND owner, Doc& d);
// R11 P1-03: a project created by a CAD/ODB++ import starts with the full default factory
// model (Top SMT, Bottom SMT + six legacy process points) -> many unwanted export layers.
// Keeps only the placement export layers that are used or needed: "Top SMT" when there are top
// components, "Bottom SMT" when there are bottom components, plus any point components are
// already assigned to. Remaps Component::assigned_point. Returns the number removed.
int prune_import_export_layers(Project& prj);

// File > Import > Board Image: loads png/jpg/bmp/tif (WIC) or ppm, fits it to
// the board and opens the calibration dialog.
bool cmd_import_board_image(HWND owner, Doc& d);
// Board image manager: visibility, opacity, lock, side, embed; 1/2/3/4+ point
// registration against pads / fiducials / frame corners.
bool dlg_board_images(HWND owner, Doc& d, int select = -1);
// Decoded pixels of a board image (premultiplied BGRA, downscaled to max_dim on the long side; cached).
// w / h: stored size. false (+ err) when it cannot be decoded.
bool board_image_pixels(const BoardImage& bi, int max_dim, int& w, int& h, const std::vector<uint8_t>*& bgra, std::string& err);

} // namespace pcbp::app
