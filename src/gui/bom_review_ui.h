#pragma once
// bom_review_ui.h — BOM wizard Review step (R14d): filterable / sortable review
// table with an issue column and filter chips, plus a resizable, selectable,
// grouped Summary pane with Copy / Copy all / Copy as report and Export report.
#include "../io/bom_review.h"
#include <set>
#include <string>

namespace pcbp::gui {

struct BomReviewUi {
    int view = 0;                       // 0 component, 1 part number
    char filter[128] = {};
    bool issues_only = false;
    uint32_t chip_mask = 0;             // 1 << io::ReviewCat (0 = no chip filter)
    float split = 0.62f;                // table share of the height
    bool summary_max = false;
    std::set<std::string> sel;          // selected summary lines ("g:i")
    io::BomReview review;               // rebuilt every frame from the preview
};

// source: file name / "M1 job ..."; job: Job ID / BOM number ("" none).
void draw_bom_review(BomReviewUi& st, const io::BomPreview& prev, const std::set<std::string>& board_refs,
                     const std::string& source, const std::string& job, float w, float h);

} // namespace pcbp::gui
