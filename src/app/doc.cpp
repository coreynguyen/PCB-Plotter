// ============================================================================
// doc.cpp — document implementation.
// ============================================================================
#include "doc.h"
#include "../core/package_resolve.h"
#include "core/package_edit.h"
#include <algorithm>
#include <cstdio>

namespace pcbp::app {

std::wstring Doc::display_name() const {
    if (!title_override.empty()) return title_override;   // "Recovered: name" (review copy)
    std::wstring n;
    if (!prj.path.empty()) n = ui::W(file_stem(prj.path) + file_ext_lower(prj.path));
    else if (!prj.info.name.empty()) n = ui::W(prj.info.name);
    else n = L"Untitled";
    return n;
}

std::string Doc::resolve_format(size_t i, gerber::Format& fmt) const {
    const GerberLayerRef& l = prj.layers[i];
    if (!l.format.empty()) {
        char u[8] = {}, z = 'L';
        int a = 2, b = 4;
        if (std::sscanf(l.format.c_str(), "%7s %d.%d %c", u, &a, &b, &z) >= 3) {
            fmt.inch = std::string(u) != "mm";
            fmt.int_digits = a;
            fmt.frac_digits = b;
            fmt.lead_supp = z != 'T';
            return fmt.describe();
        }
    }
    return {};
}

bool Doc::parse_layer(size_t i, std::string& err) {
    if (i >= prj.layers.size()) return false;
    if (glayers.size() < prj.layers.size()) glayers.resize(prj.layers.size());
    GerberLayerRef& ref = prj.layers[i];
    gerber::ParseOptions opt;
    if (!ref.aperture_list.empty()) {
        gerber::ListUnits u = ref.units_hint == "mil" ? gerber::ListUnits::Mil : ref.units_hint == "inch" ? gerber::ListUnits::Inch
                            : ref.units_hint == "mm" ? gerber::ListUnits::MM : gerber::ListUnits::Auto;
        opt.apertures = gerber::parse_aperture_list(ref.aperture_list, u).aps;
    }
    gerber::Format f;
    if (!resolve_format(i, f).empty()) { opt.fmt = f; opt.force_format = true; }
    else if (!gerber::looks_like_rs274x(ref.content) && !gerber::looks_like_excellon(ref.content)) {
        auto cands = gerber::guess_format(ref.content, opt.apertures);
        if (!cands.empty()) {
            opt.fmt = cands.front().fmt;
            opt.force_format = true;
            ref.format = std::string(opt.fmt.inch ? "in " : "mm ") + std::to_string(opt.fmt.int_digits) + "." +
                         std::to_string(opt.fmt.frac_digits) + (opt.fmt.lead_supp ? " L" : " T");
        }
    }
    auto L = std::make_shared<gerber::Layer>();
    if (!gerber::parse(ref.content, opt, *L, err)) { glayers[i] = L; return false; }
    L->name = ref.name;
    glayers[i] = L;
    return true;
}

void Doc::parse_all_layers() {
    glayers.assign(prj.layers.size(), nullptr);
    for (size_t i = 0; i < prj.layers.size(); ++i) { std::string e; parse_layer(i, e); }
}

UndoSnap Doc::snap(const std::string& label, bool with_layers) const {
    UndoSnap s;
    s.label = label;
    if (with_layers) { s.is_import = true; s.layers = prj.layers; s.sources = prj.sources; s.board_images = prj.board_images; }
    s.comps = prj.comps; s.packages = prj.packages; s.parts = prj.parts; s.images = prj.images;
    s.board_frame = prj.board_frame; s.panel_frame = prj.panel_frame; s.model = prj.model;
    s.board_outline_src = prj.board_outline_src; s.panel_outline_src = prj.panel_outline_src;
    s.outline_ack_box = prj.outline_ack_box; s.outline_warn_off = prj.outline_warn_off;   // R17b
    s.origin = prj.virtual_origin; s.info = prj.info;
    return s;
}

void Doc::restore(const UndoSnap& s) {
    prj.comps = s.comps; prj.packages = s.packages; prj.parts = s.parts; prj.images = s.images;
    prj.board_frame = s.board_frame; prj.panel_frame = s.panel_frame; prj.model = s.model;
    prj.board_outline_src = s.board_outline_src; prj.panel_outline_src = s.panel_outline_src;
    prj.outline_ack_box = s.outline_ack_box; prj.outline_warn_off = s.outline_warn_off;   // R17b
    prj.virtual_origin = s.origin; prj.info = s.info;
    for (auto& c : prj.comps) if (c.id >= prj.next_id) prj.next_id = c.id + 1;
    if (s.is_import) {
        prj.layers = s.layers; prj.sources = s.sources; prj.board_images = s.board_images;
        parse_all_layers();
    }
}

void Doc::checkpoint_import(const std::string& label) {
    ++significant;   // R12: imports trigger an autosave right after they land
    undo_.push_back(snap(label, true));
    if (undo_.size() > 60) undo_.erase(undo_.begin());
    redo_.clear();
}

void Doc::checkpoint(const std::string& label) {
    if (++edits % 25 == 0) ++significant;   // R12: autosave after every 25 undoable edits, besides the timer
    undo_.push_back(snap(label));
    if (undo_.size() > 60) undo_.erase(undo_.begin());
    redo_.clear();
}

bool Doc::undo() {
    if (undo_.empty()) return false;
    redo_.push_back(snap(undo_.back().label, undo_.back().is_import));
    restore(undo_.back());
    undo_.pop_back();
    changed(ui::CH_ALL);
    return true;
}

bool Doc::redo() {
    if (redo_.empty()) return false;
    undo_.push_back(snap(redo_.back().label, redo_.back().is_import));
    restore(redo_.back());
    redo_.pop_back();
    changed(ui::CH_ALL);
    return true;
}

void Doc::attach(HWND h) { if (std::find(views_.begin(), views_.end(), h) == views_.end()) views_.push_back(h); }
void Doc::detach(HWND h) { views_.erase(std::remove(views_.begin(), views_.end(), h), views_.end()); }

void Doc::changed(unsigned flags) {
    ++gen;
    if (flags & ~ui::CH_SELECTION) { prj.dirty = true; ++data_gen; prj_apply_polarity_defaults(prj); }   // R15: new packages get their polarity default once
    if (flags & ui::CH_GEOMETRY) ensure_package_entries(prj);   // R17a (REVIEW_V16 §2): every package a placement names is listed (dropdowns, Packages tab)
    for (HWND h : views_) if (IsWindow(h)) PostMessageW(h, ui::WM_APP_DOCCHANGED, flags, 0);
    HWND top = views_.empty() ? nullptr : GetAncestor(views_.front(), GA_ROOT);
    if (top) PostMessageW(top, ui::WM_APP_DOCCHANGED, flags, 0);
}

int Doc::selected_count() const {
    int n = 0;
    for (auto& c : prj.comps) if (c.selected) ++n;
    return n;
}
void Doc::select_none() { for (auto& c : prj.comps) c.selected = false; }
std::vector<Component*> Doc::selection() {
    std::vector<Component*> r;
    for (auto& c : prj.comps) if (c.selected) r.push_back(&c);
    return r;
}

} // namespace pcbp::app
