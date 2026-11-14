#pragma once
// ============================================================================
// bom_review.h — complete, grouped BOM review (R14d): every finding of a BOM
// preview against the board, by category, nothing truncated. Drives the
// wizard's Review > Summary pane, the review-table issue column / filter
// chips, "Copy report" and "Export report..." (.txt / .csv).
// Portable (unit-tested in tests/test_bom_review.cpp).
// ============================================================================
#include "bom_import2.h"
#include <set>
#include <string>
#include <vector>

namespace pcbp::io {

enum class ReviewCat : uint8_t {
    PnConflict,     // reference listed on several lines with different parts      (Error)
    RefError,       // reference range could not be expanded                        (Error)
    QtyMismatch,    // Quantity column differs from the expanded reference count    (Warning)
    NotOnBoard,     // BOM reference with no board component                        (Warning)
    Unmatched,      // board component (placement) not in the BOM                   (Warning)
    NoPartNumber,   // BOM line without part number                                 (Warning)
    Duplicate,      // reference listed twice with the same part                    (Warning)
    OtherWarning,   // any other warning from the preview                           (Warning)
    Dnp,            // DNP references                                               (Info)
    Info,           // bare-board line, engine notes ...                            (Info)
    Count_
};
const char* review_cat_title(ReviewCat c);      // "Part assignment conflicts"
const char* review_cat_key(ReviewCat c);        // "pn_conflict" (CSV)
IssueSev review_cat_sev(ReviewCat c);

struct ReviewItem {
    std::string ref;                    // reference (empty for line-level items)
    int line = 0;                       // 1-based source line (0 = none)
    std::string text;                   // full, untruncated text
};
struct ReviewGroup {
    ReviewCat cat = ReviewCat::Info;
    std::vector<ReviewItem> items;
};
struct BomReview {
    std::string source, job, date;      // header
    int bom_lines = 0, bom_refs = 0, part_numbers = 0, board_placements = 0, matched = 0;
    std::vector<ReviewGroup> groups;    // only non-empty categories, in ReviewCat order
    const ReviewGroup* group(ReviewCat c) const;
    int count(ReviewCat c) const { const ReviewGroup* g = group(c); return g ? (int)g->items.size() : 0; }
    // per reference (upper-case) -> categories it is in (review-table issue column / chips)
    std::vector<std::pair<std::string, uint32_t>> ref_flags;   // sorted by ref
    uint32_t flags_of(const std::string& ref_upper) const;
};

// board_refs: upper-case references of the board components (placements).
// source: file name or "M1 job 123"; job: Job ID / BOM number ("" = none); date "" = now (local).
BomReview bom_review(const BomPreview& p, const std::set<std::string>& board_refs,
                     const std::string& source, const std::string& job = {}, const std::string& date = {});

// Plain-text report for e-mail: header (source, job, date, counts) + every category in full.
std::string bom_review_text(const BomReview& r);
// CSV: severity,category,reference,line,detail (RFC 4180 quoting), header row first.
std::string bom_review_csv(const BomReview& r);
// One category as text (for "Copy" of a group).
std::string bom_review_group_text(const ReviewGroup& g);

} // namespace pcbp::io
