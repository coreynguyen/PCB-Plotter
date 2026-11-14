#pragma once
// ============================================================================
// thumb_render.h — portable board-preview rasterizer for Explorer thumbnails
// (pcbp_thumb.dll) and tests. No GDI / D2D: polygons are scan-converted at
// 2-3x resolution and box-filtered down, output is premultiplied BGRA.
// ============================================================================
#include "../../core/project.h"
#include <cstdint>
#include <string>
#include <vector>

namespace pcbp::thumb {

struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> px;   // premultiplied 0xAARRGGBB (== BGRA bytes little-endian), top-down
};

// Longest side = cx (aspect preserved). Returns false when the project has nothing to draw.
bool render(const Project& prj, int cx, Image& out);

// Parse a .pcbp (JSON) or legacy .cpf (Jet database; needs the built-in importers) from bytes.
// `cpf_loader` loads a legacy project from a temporary file path (nullptr = .pcbp only).
bool load_bytes(const std::string& bytes, Project& prj, std::string& err,
                bool (*cpf_loader)(const std::string& bytes, Project& prj, std::string& err) = nullptr);

} // namespace pcbp::thumb
