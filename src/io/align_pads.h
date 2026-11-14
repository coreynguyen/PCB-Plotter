#pragma once
// ============================================================================
// align_pads.h — R17a: pad centres of a Gerber layer inside a selection, for Align Plot to Gerber
// (core/plot_align.h). The Gerber tab and the tests call the same function.
// A pad = flash, region, or zero-length draw; clear (LPC) objects are skipped. Centres are returned
// in Gerber world coordinates (layer transform applied), de-duplicated within 1 um (stacked flashes).
// ============================================================================
#include "gerber.h"
#include "digitize.h"
#include <functional>
#include <vector>

namespace pcbp::io {
// world_quad: selection polygon in world coordinates (a box = 4 corners). visible(obj) filters objects
// (crop / hidden D-codes); empty = all.
std::vector<Vec2> gerber_pad_centres(const gerber::Layer& L, const Xform& layer_to_world, const std::vector<Vec2>& world_quad,
                                     const std::function<bool(int)>& visible = {});
std::vector<Vec2> box_quad(const BBox& b);
// Package acquisition from Gerber (REVIEW_V16 §2): pads of a digitize PadSet (layer coordinates) whose centre is
// inside world_quad, as board-coordinate Pads (Pad::pos world mm, w/h along the pad's own axes, Pad::rot world
// deg) ready for package_from_board_pads(). layer_to_world = the Gerber layer transform.
std::vector<Pad> padset_board_pads(const digitize::PadSet& ps, const Xform& layer_to_world, const std::vector<Vec2>& world_quad);
} // namespace pcbp::io
