#pragma once
// ============================================================================
// doc.h — an open project: Project data + parsed Gerber cache + undo + views.
// ============================================================================
#include "../core/project.h"
#include "../io/gerber.h"
#include "../ui/window.h"
#include <memory>

namespace pcbp::app {

struct UndoSnap {
    std::string label;
    std::vector<Component> comps;
    std::map<std::string, Package> packages;
    std::map<std::string, PartNumber> parts;
    std::vector<PanelImage> images;
    std::vector<std::vector<Vec2>> board_frame, panel_frame;
    OutlineSource board_outline_src = OutlineSource::Confirmed, panel_outline_src = OutlineSource::Confirmed;   // R16
    BBox outline_ack_box; bool outline_warn_off = false;   // R17b
    FactoryModel model;
    Vec2 origin;
    AssemblyInfo info;
    // Import steps (REVIEW_V9 §19): one atomic step that also restores the
    // Gerber layers, board images and import provenance; undo asks first.
    bool is_import = false;
    std::vector<GerberLayerRef> layers;
    std::vector<Project::SourceRef> sources;
    std::vector<BoardImage> board_images;
};

class Doc {
public:
    Project prj;
    std::vector<std::shared_ptr<gerber::Layer>> glayers;   // parallel to prj.layers
    int active_point = 0;                                  // factory model selection
    // view filters shared by all views of this doc
    bool show_top = true, show_bottom = false, all_images = true,   // selectable sides (see apply_side_view)
    show_frame = true, show_labels = true;
    bool highlight_unassigned = true, highlight_noref = false, verify_angles = false;   // v17: unassigned highlight on by default; drawn only while assignment is partial (plotter_view)
    bool second_side = true;      // draw far side semi-transparent (false = hide it)
    // --- board view (LEAD_BRIEF 10): orientation and side visibility are separate.
    //   view_from_bottom : physical orientation (mirrored canvas = board turned over)
    //   side_display     : Normal (active side only) / Ghost (far side transparent,
    //                      never selectable) / Xray (both drawn; far side selectable
    //                      only with back_selectable)
    // Active side = the side facing the viewer. show_top/show_bottom are DERIVED
    // by apply_side_view() and mean "selectable / editable" for every tool.
    enum class SideDisplay : uint8_t { Normal = 0, Ghost = 1, Xray = 2 };
    bool view_from_bottom = false;
    SideDisplay side_display = SideDisplay::Ghost;
    bool back_selectable = false;  // default off: no accidental far-side edits
    Side active_side() const { return view_from_bottom ? Side::Bottom : Side::Top; }
    bool far_drawn() const { return side_display != SideDisplay::Normal; }
    bool selectable(Side s) const { return s == Side::Top ? show_top : show_bottom; }
    void apply_side_view() {
        const bool far_sel = side_display == SideDisplay::Xray && back_selectable;
        show_top = !view_from_bottom || far_sel;
        show_bottom = view_from_bottom || far_sel;
        second_side = far_drawn();
    }
    bool move_mode = false;       // placements are locked unless the Move tool is on
    uint64_t gen = 1;             // bumped by changed(); views re-render when it moves
    uint64_t data_gen = 1;        // bumped by changed() for non-selection changes (re-run checks on this)
    std::vector<std::string> issues_cache;
    // R12 recovery (REVIEW_V11 §4)
    uint64_t significant = 0;     // bumped by imports / every 25 edits: autosave soon
    uint64_t edits = 0;
    std::wstring title_override;  // "Recovered: board.pcbp" for a review copy (never saved over the original)

    std::wstring display_name() const;
    // gerber
    bool parse_layer(size_t i, std::string& err);
    void parse_all_layers();
    std::string resolve_format(size_t i, gerber::Format& fmt) const;  // returns description
    // undo
    void checkpoint(const std::string& label);
    // Atomic import step: label like "Import ODB++ <name>"; also captures layers / sources.
    void checkpoint_import(const std::string& label);
    bool undo_is_import() const { return !undo_.empty() && undo_.back().is_import; }
    std::string redo_label() const { return redo_.empty() ? std::string() : redo_.back().label; }
    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
    std::string undo_label() const { return undo_.empty() ? std::string() : undo_.back().label; }
    bool undo();
    bool redo();
    // views
    void attach(HWND h);
    void detach(HWND h);
    const std::vector<HWND>& views() const { return views_; }
    void changed(unsigned flags);                // marks dirty (unless selection only) and notifies
    // selection helpers
    int selected_count() const;
    void select_none();
    std::vector<Component*> selection();

private:
    UndoSnap snap(const std::string& label, bool with_layers = false) const;
    void restore(const UndoSnap& s);
    std::vector<UndoSnap> undo_, redo_;
    std::vector<HWND> views_;
};

using DocPtr = std::shared_ptr<Doc>;

} // namespace pcbp::app
