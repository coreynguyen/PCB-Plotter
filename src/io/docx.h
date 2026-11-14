#pragma once
// ============================================================================
// docx.h — WordprocessingML (.docx) reader -> RichDoc (pcbp::io, portable).
// Read-only: paragraphs, runs (bold / italic / underline / strike / size /
// colour / monospace fonts), headings (Heading1..6 / Title styles), lists
// (numPr; numbering.xml decimal vs bullet), hyperlinks (relationship targets,
// shown only), tables (nested tables flattened), inline images (word/media).
// Macros (vbaProject.bin), OLE objects and fields are ignored, never run.
// ============================================================================
#include "richdoc.h"
#include <string>
#include <vector>

namespace pcbp::io {

bool is_docx_bytes(const std::vector<uint8_t>& data);
bool docx_read(const std::vector<uint8_t>& data, RichDoc& out, std::string& err);

} // namespace pcbp::io
