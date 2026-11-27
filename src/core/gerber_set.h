#pragma once
// ============================================================================
// gerber_set.h — load a set of Gerber / Excellon files into Project layers without UI
// (LLM_API §3A.4 G1). Files, folders and archives (zip / tar / tgz, nested) are expanded;
// each file is classified (RS-274X, RS-274D, Excellon, aperture list, netlist, other),
// 274D files get the aperture list with the matching stem (or the only one), and a
// guessed coordinate format; layer type and side come from file-name conventions and
// X2 attributes (gerber::guess_layer_function). Empty / unparsable files are skipped
// with a reason, never silently dropped.
//
// The Load Gerber Layers dialog (gui/gerber_workspace.cpp) and app::Doc::parse_layer
// use the same helpers (role colours, format strings, layer parse, stack order).
// ============================================================================
#include "project.h"
#include "../io/gerber.h"
#include <string>
#include <vector>

namespace pcbp::gset {

// ---- per-layer helpers -------------------------------------------------------------------
uint32_t role_color(LayerRole r, int n);                 // fixed colour per role; others cycle by n
void guess_role(GerberLayerRef& r, int n, std::string* why = nullptr);   // role, side, colour
// "in 2.3 L" / "mm 3.4 T" <-> gerber::Format (GerberLayerRef::format)
bool parse_format_string(const std::string& s, gerber::Format& f);
std::string format_string(const gerber::Format& f);
bool is_rs274d(const GerberLayerRef& l);                 // neither RS-274X nor Excellon
gerber::ListUnits list_units(const std::string& units_hint);   // "mil" / "inch" / "mm" / "" (auto)
// Parse a layer as app::Doc::parse_layer does: aperture list, stored or guessed 274D format
// (a guessed format is written back to ref.format). false = hard parse failure.
bool parse_layer_ref(GerberLayerRef& ref, gerber::Layer& out, std::string& err);
// Aperture-list stem key: lower-case stem without "_apertures" / ".apt" / "_rep" ... suffixes.
std::string aperture_stem_key(const std::string& path);
// Physical stack order seen from the top (bottom assy..copper, inner deepest first, top copper..assy,
// other, outline, drill last). Lower = drawn first.
int stack_rank(const GerberLayerRef& l);

// ---- set loading -------------------------------------------------------------------------
struct Skip { std::string path, reason, detail; };
struct Loaded {
    GerberLayerRef ref;
    std::string why;                    // role/side guess reason ("ext .GTL", "X2 FileFunction", ...)
    int objects = 0, flashes = 0, draws = 0, regions = 0, apertures = 0;
    double w = 0, h = 0;                // extent (mm)
    bool excellon = false;
    std::vector<std::string> warnings;  // first parser warnings (capped)
};
struct SetResult {
    std::vector<Loaded> layers;         // stack order
    std::vector<Skip> skipped;
    std::vector<std::string> notes;     // e.g. "RS-274D without aperture list"
};
struct SetOptions {
    int max_depth = 4;                  // nested folders / archives
    bool keep_empty = false;            // keep valid files with no objects (hidden)
    size_t max_files = 2000;
};
// paths: files, folders and archives (UTF-8). false only when nothing could be read at all.
bool load_gerber_set(const std::vector<std::string>& paths, SetResult& out, std::string& err, const SetOptions& opt = {});

// Append to the project. Names are made unique (" (2)"...); name_prefix (e.g. "Panel ") marks a
// pallet set. When no layer of the project is the digitize layer, the first top paste / copper /
// mask layer becomes it. Returns the index of the first added layer (-1 when none).
int add_layers(Project& p, const SetResult& r, const std::string& name_prefix = "");

} // namespace pcbp::gset
