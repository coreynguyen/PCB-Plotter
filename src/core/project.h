#pragma once
// ============================================================================
// project.h — PCB Plotter document model (portable).
//
// Mirrors the CircuitCAM project concepts (from the user manual and the
// .cpf table layout): Assembly info, Locations (components / fiducials),
// Packages (shape library), Numbers (BOM part numbers), Images (panel),
// Factory model (process points -> model points) and per-model-point
// machine setups. Everything is in millimetres / degrees CCW.
// ============================================================================
#include "geom.h"
#include "selection_sets.h"
#include "board_image.h"
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pcbp {

// ---------------------------------------------------------------------------
// Vector graphics (package renderings, board outlines, artwork)
// ---------------------------------------------------------------------------
enum class GKind : uint8_t { Line, Polyline, Polygon, Rect, Circle, Arc, Text, Group };

struct GPrim {
    GKind kind = GKind::Line;
    std::vector<Vec2> pts;      // Line: 2, Polyline/Polygon: n, Rect: 2 corners,
                                // Circle/Arc: pts[0] = centre, Text: pts[0] = anchor
    double width = 0;           // pen width (mm); 0 = hairline
    bool filled = false;
    uint32_t color = 0;         // 0xAARRGGBB; 0 = "use context colour"
    double radius = 0;          // Circle/Arc
    double a0 = 0, a1 = 0;      // Arc start / sweep (deg, CCW)
    double rot = 0;             // Rect/Text rotation (deg)
    double text_h = 0;          // Text height (mm)
    std::string text;
    std::vector<GPrim> children; // Group
};

BBox gprim_bbox(const GPrim& g);
BBox gprims_bbox(const std::vector<GPrim>& v);
struct Package;
// R24a (user v23 #2): the package body box for machine component data (JUKI laser / vision checks the body, legs
// ignored). A real body outline (CAD outline, Gerber / library / user drawn) gives its box and *src = "body"; a body
// that is only a pad-derived rectangle ("pads") or the CAD package extent box ("cad_extent", which includes the
// pins) is not a body: the pad / lead extent is returned and *src = "pads" (approximate, includes legs).
BBox pkg_body_box(const Package& p, std::string* src = nullptr);
// R25 (262648-01): where the machine has to put the part, in package coordinates. A CAD footprint's origin is not
// always its centre (chip footprints drawn from pad 1: origin on the pad, body from -0.38 to +1.40 mm), and a centroid
// file made from such footprints carries the origin. The placement point is the centre of the pad (land) extent; the
// centre of pkg_body_box() when the package has no pads; under 0.02 mm it is taken as centred (0, 0).
Vec2 pkg_place_offset(const Package& p);

// ---------------------------------------------------------------------------
// Packages (shape library)
// ---------------------------------------------------------------------------
enum class PadShape : uint8_t { Round, Square, Rect, Oblong, Octagon };

struct Pad {
    Vec2 pos;                   // relative to package centroid, 0 deg
    double w = 0.5, h = 0.5;    // mm
    double rot = 0;
    PadShape shape = PadShape::Rect;
    double drill = 0;           // >0 => through-hole
    int pin = 0;                // 1-based pin number (0 = unknown)
};

struct Package {
    std::string name;
    std::string description;
    bool smt = true;
    bool j_lead = false, radial = false, notched = false, pin_marked = false;
    bool polarized = false;
    bool shape_confirmed = false;
    int leads = 0;
    double height = 0;          // mm
    // R24a: provenance of `height`: "cad" (read from the CAD package / component data), "user" (package editor),
    // "" = unknown / legacy / not given. Never fabricated: 0 stays 0 (machine height field left unset).
    std::string height_src;
    std::string default_process;
    std::vector<Pad> pads;
    std::vector<GPrim> body;    // artwork outline; empty => auto from pads
    // Provenance of `body` (REVIEW_V9 §6A): "cad" = outline read from CAD data,
    // "cad_extent" = CAD-declared package extent box, "pads" = derived from pads
    // (approximate), "" = drawn / edited by the user or unknown (legacy files).
    std::string body_source;
    BBox extent() const;        // pads + body
    void auto_body();           // build rectangular body around pads
    // ---- orientation / pin-one marker (package template; see specs/PACKAGES_RE.md, core/package_edit.h)
    // Package 0 deg orientation reference = the part as presented in cut tape.
    uint8_t orient_class = 0;   // OrientClass: 0 Unset (derive), 1 Polarized, 2 Ambiguous, 3 Symmetric
    uint8_t marker_kind = 0;    // MarkerKind: 0 None, 1 Dot, 2 Arrow, 3 Custom (marker_art)
    uint32_t marker_color = 0xFFE03131;
    bool marker_user_pos = false;   // false: auto at pin 1
    Vec2 marker_pos;            // package coords (when marker_user_pos)
    double marker_size = 0;     // mm; 0 = auto from pin-1 pad
    std::vector<GPrim> marker_art;  // Custom marker artwork (package coords)
    // R15 polarity marker (specs/POLARITY.md): marker_kind != None <=> the part has polarity / orientation.
    double marker_rot = 0;      // Arrow direction (deg, package coords, 0 = +X)
    bool polarity_set = false;  // defaults applied / migrated / user edited: never auto-changed again
    std::vector<std::string> keep_separate;   // R12 §8: packages the user marked "keep separate" (never auto-merged)
    // R16b (specs/ANGLES.md §10, core/package_resolve.h)
    // tape_rot: R16b feeder / tape orientation. R18a: NO LONGER APPLIED anywhere (the package graphic is drawn
    // as the part sits in cut tape; the supply angle lives on the part number). Kept only so old files
    // round-trip; a non-zero value triggers a one-time notice on load (tape_rot_notice()).
    int tape_rot = 0;
    // How the graphics were obtained ("" = CAD / user / unknown; "template:<id>", "library", "gerber").
    std::string resolved_by;
    // R24b machine component data (core/machine_assign.h): USER OVERRIDES only, field by field (0 / "" = use the
    // suggestion computed from package code + body). `mach_assigned` = the user ran / accepted "Assign machine data".
    struct MachineOverride {
        int nozzle = 0, tape_w = 0, pitch = 0, type_code = 0;
        std::string packaging, carrier, centring;
        double supply_angle = 0;
        bool supply_angle_set = false;
        bool empty() const { return !nozzle && !tape_w && !pitch && !type_code && packaging.empty() && carrier.empty() &&
                                    centring.empty() && !supply_angle_set; }
    } mach;
    bool mach_assigned = false;
};

// ---------------------------------------------------------------------------
// Part numbers (BOM "Numbers")
// ---------------------------------------------------------------------------
struct PartNumber {
    std::string number;
    std::string description;
    std::string package;
    bool polarized = false;
    bool socket = false;
    std::string cust_pn, vend_pn, manf_pn;
    std::string manufacturer;       // MPN maker (BOM 'Manufacturer'); informational, never written to ERP
    std::string part_rev, soft_ver, stock_code, bin, line_item;
    std::string default_process;    // process point name (BOM "Process" column)
    std::string pop_type;
    std::string part_marking;
    double height = 0;
    double cost = 0;
    // R18a supplied packaging + machine supply angle (specs/ANGLES.md §10). Per part number, not per package
    // (a 0603 cap and a 0603 tantalum share a package). packaging: "" (unknown), Tape, Stick, Tray, MTC, Bulk.
    // supply_angle: deg CCW the machine reads from the component data (H8H +180 tape / +178 other);
    // when !supply_angle_set the default for the packaging applies (supply_angle_default()).
    // Never changes placement angles.
    std::string packaging;
    double supply_angle = 0;
    bool supply_angle_set = false;
    // R24b: component machine codes read from an imported machine program (H8H +78 class, +176 tape, +178 pitch),
    // re-written verbatim while the package's machine data is not assigned / overridden. 0 = none.
    int prog_type = 0, prog_tape = 0, prog_pitch = 0;
};
// R18a: Tape 0, Stick 270, Tray 180, MTC 0, Bulk / unknown 0.
double supply_angle_default(const std::string& packaging);
double effective_supply_angle(const PartNumber& pn);
// JUKI component feeder kind (+80): 1 tape, 2 stick, 3 tray (Tray and MTC), 4 bulk, 0 unknown; and back.
int packaging_feeder_kind(const std::string& packaging);
std::string packaging_from_feeder_kind(int kind);

// ---------------------------------------------------------------------------
// Locations (components, fiducials, free-floating items)
// ---------------------------------------------------------------------------
enum class Side : uint8_t { Top = 0, Bottom = 1 };
enum class Feature : uint8_t { Component = 0, Fiducial = 1, BadMark = 2, Hole = 3 };
// Fiducial scope (CircuitCAM Locations.FiducialType: 1 Master -> Global,
// 2 Image -> Image, 3 Local -> Local, 4 Zero-Point -> Global):
//   Global : panel / board level, defined in panel coordinates, NEVER instanced
//            per image (BOC marks of the whole panel).
//   Image  : circuit fiducial, defined once relative to the root circuit and
//            instanced on every panel image (circuit BOC).
//   Local  : placement-specific, bound to local_fid_owner; instanced with it.
enum class FiducialScope : uint8_t { Global = 0, Local = 1, Image = 2 };
const char* fid_scope_name(FiducialScope s);
FiducialScope fid_scope_from_cpf(int fiducial_type);   // CPF FiducialType -> scope
int fid_scope_to_cpf(FiducialScope s);

// Population / assembly process. Auto = derive from package (smt flag / drills).
enum class Process : uint8_t { Auto = 0, SMT = 1, TH = 2, Hardware = 3, Other = 4 };
const char* process_name(Process p);
Process process_from_name(const std::string& s);

// ---- BOM reconciliation state + data provenance (R10; REVIEW_V9 §11E/F/G, §19) ----
// BomState is written by the BOM import only. NotChecked = no BOM imported yet.
enum class BomState : uint8_t { NotChecked = 0, Listed = 1, Dnp = 2, Orphan = 3 };
// Where a component's part number / package came from.
enum class DataSrc : uint8_t { Unknown = 0, Cad = 1, Bom = 2, Inferred = 3, User = 4, Library = 5 };
inline const char* data_src_name(DataSrc s) {
    switch (s) { case DataSrc::Cad: return "CAD"; case DataSrc::Bom: return "BOM"; case DataSrc::Inferred: return "inferred";
                 case DataSrc::User: return "user"; case DataSrc::Library: return "part library"; default: return "unknown"; }
}
// Display state: Dnp (explicit in BOM) / Orphan (on board, not in BOM) / Unassigned (no P/N yet) / Placed.
enum class CompState : uint8_t { Placed, Dnp, Orphan, Unassigned };
inline const char* comp_state_name(CompState s) {
    switch (s) { case CompState::Dnp: return "DNP"; case CompState::Orphan: return "Not in BOM"; case CompState::Unassigned: return "Unassigned"; default: return "Placed"; }
}

struct Component {
    uint32_t id = 0;            // stable id within project
    std::string ref;
    Vec2 pos;                   // centroid (board coordinates, root image)
    // ---- rotation (specs/ANGLES.md) -------------------------------------
    // rot: THE placement rotation. deg CCW, normalised [0,360), fractional
    // precision, never snapped. What Board View shows and what every exporter
    // writes (through the explicit per-machine convention in machine_job).
    double rot = 0;
    // rot_imported: value from the CAD/CPF import (provenance, "Reset to imported").
    double rot_imported = 0;
    // pre_rotation: optional package/feeder supply correction, deg CLOCKWISE
    // (JUKI convention). Metadata only: never added to rot; exported only into
    // machine fields that exist for it.
    double pre_rotation = 0;
    // Legacy CircuitCAM per-point machine angle (ComponentAssignments.MachRotation /
    // old native "mach_rot"). Imported for comparison only; never exported.
    double legacy_mach_rot = 0;
    bool has_legacy_mach_rot = false;
    Side side = Side::Top;
    std::string number;         // part number (key into Project::parts)
    std::string package;        // package name (key into Project::packages)
    Feature feature = Feature::Component;
    FiducialScope fid_scope = FiducialScope::Global;
    int fid_number = 0;
    int local_fid_owner = 0;    // component id for local fiducials
    bool dnp = false;
    bool pin1_set = false;
    int pin1_index = 0;         // pad index chosen as pin one
    // label (reference designator text) placement relative to centroid
    Vec2 label_off;
    double label_rot = 0;
    bool label_moved = false;
    // CAD source values kept for "Use CAD coordinates" + ECN merge
    Vec2 cad_pos;
    std::string url;
    int assigned_point = -1;    // index into factory model points; -1 = none
    Process process = Process::Auto;   // population / assembly process (selection)
    std::string group_label;           // free user group label (selection / reports)
    bool selected = false;      // UI state (not persisted)
    // ---- R10 BOM state / provenance (persisted: "bom_state", "pn_src", "pkg_src", "bom_pkg") ----
    BomState bom_state = BomState::NotChecked;
    DataSrc pn_src = DataSrc::Unknown;
    DataSrc pkg_src = DataSrc::Unknown;
    std::string bom_package;    // package text the BOM line gave for this ref ("" = the BOM gave none)
    bool from_gerber = false;   // R14: built from Gerber data (Gerber tab); only these show over the Gerbers by default
    bool review_ok = false;     // R16c: duplicate reference / double location reviewed and accepted (Data tab; persisted "review_ok")
};
inline CompState comp_state(const Component& c) {
    if (c.bom_state == BomState::Dnp || (c.dnp && c.bom_state != BomState::Orphan)) return CompState::Dnp;
    if (c.bom_state == BomState::Orphan) return CompState::Orphan;
    if (c.number.empty()) return CompState::Unassigned;
    return CompState::Placed;
}

// ---------------------------------------------------------------------------
// Panel images (step & repeat)
// ---------------------------------------------------------------------------
struct PanelImage {
    std::string name = "1";
    Vec2 offset;                // image origin relative to root
    double rot = 0;
    bool root = true;
    bool x_out = false;         // skipped / bad board
    int x_index = 0, y_index = 0;
};

// ---------------------------------------------------------------------------
// Gerber layers (attached to project; geometry parsed by io/gerber)
// ---------------------------------------------------------------------------
enum class LayerRole : uint8_t { Other, Copper, Paste, Mask, Silk, Assembly, Drill, Outline };

struct GerberLayerRef {
    std::string name;
    std::string path;           // original file path
    std::string content;        // raw file content (embedded, like CircuitCAM)
    std::string aperture_list;  // raw aperture list text (274D files)
    LayerRole role = LayerRole::Other;
    Side side = Side::Top;
    uint32_t color = 0xFFC08040;
    bool visible = true;
    bool digitize = false;      // the active digitizable layer
    Vec2 offset;
    double rot = 0;
    bool mirror = false;
    std::vector<int> hidden_dcodes;
    std::string format;         // "" = auto, else "in|mm I.F L|T" e.g. "in 2.3 L"
    std::string units_hint;     // aperture list units ""=auto, "mil", "inch", "mm"
};

// ---------------------------------------------------------------------------
// Factory model
// ---------------------------------------------------------------------------
enum class PointKind : uint8_t {
    Placement,      // machine placement (SMT / thru-hole insertion)
    Manual,         // hand placement / slide line
    Inspection,     // AOI / visual inspection
    Process,        // reflow, wash, wave etc. (no components)
    Test
};

struct MachineSetup {
    std::string exporter_id;            // plugin id, e.g. "juki.h8h"
    std::string target_dir;
    std::string target_file;
    int orientation = 0;                // 0/90/180/270 board rotation
    bool flipped = false;
    Side side = Side::Top;
    Vec2 hard_offset;
    // Legacy CircuitCAM flags: kept so .cpf/old projects round-trip; they do NOT
    // change the exported angle or position any more (specs/ANGLES.md §5).
    bool force_angle_calc = false;
    bool single_board = false;          // treat all images as a single board
    bool use_cad_coords = false;
    // R25: false (default) = placements are exported at the package's placement point (pkg_place_offset(): the centre
    // of its lands); true = at the footprint origin as imported (what every export before R25 wrote).
    bool place_at_origin = false;
    // Bottom-side angle rule when flipped. false (default) = keep the displayed
    // angle (CircuitCAM-validated JUKI behaviour); true = mirror (angle -> -angle).
    bool mirror_angle_when_flipped = false;
    bool has_origin = false;            // machine-specific virtual origin
    Vec2 origin;
    // R21: "No marks (intentional)": this layer is programmed without fiducial marks on purpose.
    // Suppresses the zero-marks Error in export validation; never makes the builder borrow marks.
    bool no_marks = false;
    std::map<std::string, std::string> options;   // exporter-specific
};

struct ModelPoint {
    std::string name;
    std::string process_type;           // "SMT", "AUTO", "SLIDE", "HAND", ...
    PointKind kind = PointKind::Placement;
    uint32_t color = 0xFF3C8CFF;        // colour coding
    Side side = Side::Top;
    std::string line;                   // line / process group name
    bool wip = false, repair = false, inspection = false;
    // filters (auto-assignment): empty = no filter
    std::vector<std::string> filter_packages, filter_numbers, filter_process;
    bool has_machine = false;
    MachineSetup machine;
    std::string notes;                  // Notes tab (plain/RTF text)
    std::string visual_aid_title;
    int link = 0;                       // R18d export-layer link ID (0 = none); two layers with one ID = a two-sided SMT job, upper layer = Side 1
};

struct FactoryModel {
    std::string name = "Default";
    std::string description;
    std::vector<ModelPoint> points;
    // R19a: one project-level output folder for every machine program (file names stay per layer:
    // "<Project Name> <layer name>.<ext>"). Empty = ask on Generate. MachineSetup::target_dir is legacy.
    std::string export_dir;
    void migrate_export_dir();                  // legacy per-layer folders -> export_dir (first layer's folder wins)
    static FactoryModel make_default();         // new projects: Top SMT + Bottom SMT export layers
    static FactoryModel make_legacy_default();  // pre-R10 default (single "SMT Placement"), used for legacy loads
};

// ---------------------------------------------------------------------------
// Assembly information
// ---------------------------------------------------------------------------
struct AssemblyInfo {
    std::string name, number, description, originator;
    std::string bom_number, assembly_rev, bom_rev, manf_rev;
    std::string standard, customer, url;
    std::string pcb_id;                 // physical PCB / PWB part number (machine programs)
    std::string custom[4];
    std::string created, modified;      // ISO-8601
    std::string original_filename;
};

enum class Units : uint8_t { MM, Inch, Mil };

// ---------------------------------------------------------------------------
// Manufacturing sides (production sequence; see core/sides.h, LEAD_BRIEF 11).
// Side 1 / Side 2 = run ORDER on the line, chosen by the user; never inferred
// from physical Top/Bottom. The export profile of a side (machine type,
// origin, board rotation, mirroring, folder, options) is the MachineSetup of
// the ModelPoint it maps to -- no second profile structure.
// ---------------------------------------------------------------------------
struct MfgSide {
    int seq = 1;                        // 1 = run first
    Side physical = Side::Bottom;
    std::string point;                  // ModelPoint name (export profile)
    std::string suffix;                 // filename suffix; "" = "[Side N]"
};
struct MfgSides {
    bool configured = false;            // user confirmed the order (never set automatically)
    std::vector<MfgSide> sides;         // sorted by seq
    std::string last_validation;        // summary of the last pre-export validation (persisted)
};

// ---------------------------------------------------------------------------
// Project
// ---------------------------------------------------------------------------
// R10 §16: automatic project revision history. Entries are added on export (export checkpoint: links a
// generated machine program to the project state) and by an explicit "Checkpoint". Never on Save.
struct RevisionEntry {
    std::string time;                   // ISO-8601 local time
    std::string kind;                   // "export" | "checkpoint"
    std::string rev;                    // revision identifier ("R1", "R2", ...)
    std::string summary;                // automatic summary (counts, layer, format)
    std::string comment;                // optional user comment
    std::string program_file;           // export: written file name
    std::string program_hash;           // export: FNV-1a 64 of the written bytes (hex)
    std::string author;
    // R17d (REVIEW_V16 §12): restorable checkpoint = compact project JSON (project_to_json without Gerber
    // layer content and without the history itself). Empty for old entries / pruned ones (only the newest
    // kKeepCheckpointSnapshots keep it). Restore brings back exactly the undo-step fields (see output_views.cpp).
    std::string snapshot;
};
constexpr int kKeepCheckpointSnapshots = 10;

// R19b (specs/DOCUMENTS.md): light index of one job-archive entry ("<Project Name>.zip" next to the .pcbp).
// The bytes live only in the zip; the project (and its checkpoints) store just this.
struct DocumentRef {
    std::string path;                   // name inside the zip ('/')
    uint64_t size = 0;
    uint32_t crc = 0;
    std::string category;               // top folder / sniffed category
    std::vector<std::string> tags;
    std::string note;
    std::string sha256;                 // R25 content hash (hex), "" until computed; kept while size + CRC match
};

enum class OutlineSource : uint8_t { Inferred = 0, Confirmed = 1, Imported = 2 };
inline const char* outline_source_name(OutlineSource s) {
    switch (s) { case OutlineSource::Inferred: return "Inferred"; case OutlineSource::Imported: return "Imported"; default: return "Confirmed"; }
}

struct Project {
    AssemblyInfo info;
    std::string project_notes;          // Project Info notes (project-wide; process notes stay per point)
    std::vector<RevisionEntry> history; // R10 §16
    Units display_units = Units::Inch;
    Vec2 virtual_origin;                // global virtual origin
    // Circuit (image) frame: CircuitCAM Images.ImageOutline, in root-image
    // coordinates; drawn / checked once per panel image. Single boards: the
    // board outline. Exported as the HLC circuit object (board block +70..+87).
    std::vector<std::vector<Vec2>> board_frame;
    // Panel frame: CircuitCAM Assembly.Outline (panel coordinates, never instanced).
    std::vector<std::vector<Vec2>> panel_frame;
    bool board_frame_from_gerber = false;   // R14: frames committed from Gerber graphics (provenance)
    bool panel_frame_from_gerber = false;
    // R16 (REVIEW_V15 §3, specs/OUTLINES.md): outline provenance. Inferred = placement extents (never
    // authoritative, drawn dashed, blocks machine export); Confirmed = user-defined / user-confirmed;
    // Imported = from CAD / ODB++ / Gerber / machine program. Confirmed and Imported never auto-resize.
    // An empty board_frame is always Inferred (board_bbox() falls back to the live placement extent).
    OutlineSource board_outline_src = OutlineSource::Confirmed;
    OutlineSource panel_outline_src = OutlineSource::Confirmed;
    // R17b (REVIEW_V16 §4): an Inferred board outline is a Warning, acknowledgeable without the editor.
    // outline_ack_box = "Use as-is" for exactly these dimensions (a later re-inference re-warns);
    // outline_warn_off = "Don't warn for this project". Both persisted; neither changes the outline.
    BBox outline_ack_box;
    bool outline_warn_off = false;
    bool tape_rot_notice_ack = false;   // R18a: the "tape orientation no longer applied" notice was shown (persisted)
    uint32_t frame_color = 0xFF2F9E44;
    std::vector<Component> comps;
    std::map<std::string, Package> packages;
    std::map<std::string, PartNumber> parts;
    std::vector<PanelImage> images{PanelImage{}};
    std::vector<GerberLayerRef> layers;
    FactoryModel model = FactoryModel::make_default();
    std::vector<std::string> revision_notes;
    std::vector<SelectionSet> selection_sets;   // named selection sets (UI, saved)
    std::vector<DocumentRef> documents;         // R19b job-archive index (Files tab)
    std::string documents_archive;              // R19b: re-linked archive path (empty = "<Project Name>.zip" next to the .pcbp)
    MfgSides mfg;                       // production side order + per-side profile mapping
    std::string path;                   // file this project was loaded from / saved to
    uint32_t next_id = 1;
    bool dirty = false;

    // helpers
    Component& add_component(const Component& c);
    Component* find_ref(const std::string& ref);
    const Component* find_ref(const std::string& ref) const;
    const Package* package_of(const Component& c) const;
    const PartNumber* part_of(const Component& c) const;
    BBox board_bbox() const;            // frame or components extent
    BBox comp_bbox(const Component& c) const;
    Process process_of(const Component& c) const;   // resolves Process::Auto  // rotated body/pads extent
    Xform comp_xform(const Component& c, int image = 0) const;
    Xform image_xform(int image) const;
    // --- user coordinates: EVERY displayed / edited X,Y goes through these.
    // Stored positions are root-image board coordinates; the user sees them
    // relative to virtual_origin (which is also a root-image point).
    Vec2 to_user(Vec2 root) const { return {root.x - virtual_origin.x, root.y - virtual_origin.y}; }
    Vec2 from_user(Vec2 u) const { return {u.x + virtual_origin.x, u.y + virtual_origin.y}; }
    Vec2 user_pos(const Component& c) const { return to_user(c.pos); }
    // make component c's centroid the virtual origin (its user position becomes exactly 0,0)
    void set_origin_at(const Component& c) { virtual_origin = c.pos; }
    void link_bom();                    // copy Number.package into comps lacking a package
    int point_index(const std::string& name) const;
    int auto_assign();                  // apply model-point filters; returns count assigned
    void make_all_like_root();

    // --- fiducial scope / instancing ---
    // false for global (panel) fiducials: they exist once in panel coordinates.
    bool instanced(const Component& c) const;
    // panel images this component appears on (1 for global fiducials)
    int instance_count(const Component& c) const { return instanced(c) ? (int)images.size() : 1; }
    // transform for instance `image` (global fiducials ignore the image index)
    Xform instance_xform(const Component& c, int image) const;

    // --- frames (API for the UI) ---
    bool has_circuit_frame() const { return !board_frame.empty(); }
    bool is_panel() const { return images.size() > 1; }
    BBox circuit_frame_bbox() const;    // root image; empty when no frame
    BBox panel_frame_bbox() const;      // panel frame, else union of all image frames
    void set_circuit_frame_rect(const BBox& b);   // replaces board_frame with one closed rect
    void set_panel_frame_rect(const BBox& b);
    void offset_circuit_frame(Vec2 d);
    // circuit frame polylines placed on panel image `image`
    std::vector<std::vector<Vec2>> circuit_frame_at(int image) const;
    // frame the component (root coordinates) must lie in: panel frame for
    // global fiducials on a panel, else the circuit frame. nullptr = none.
    const std::vector<std::vector<Vec2>>* frame_for(const Component& c) const;
    // --- R16 outline provenance (specs/OUTLINES.md) ---
    OutlineSource circuit_outline_source() const { return board_frame.empty() ? OutlineSource::Inferred : board_outline_src; }
    // the outline that sets the machine board size: panel frame when present, else the circuit frame
    OutlineSource board_outline_source() const { return !panel_frame.empty() ? panel_outline_src : circuit_outline_source(); }
    bool board_outline_confirmed() const { return board_outline_source() != OutlineSource::Inferred; }
    BBox placement_extents(double margin = 2.0) const;   // components extent + margin (empty without components)
    bool circuit_frame_is_placement_extent() const;     // one axis-aligned rect == placement extents (+0 or +2 mm)
    // freeze/refresh an Inferred circuit outline from the placement extents (no-op for Confirmed/Imported)
    void infer_circuit_outline();
    void confirm_outlines();                             // explicit user confirmation (Inferred -> Confirmed)
    bool inferred_outline_acknowledged() const;          // R17b: Use as-is (same dims) or Don't warn
    void acknowledge_inferred_outline(bool never_warn);  // R17b: Use as-is / Don't warn for this project

    // --- import provenance (LEAD_BRIEF §17A/§17G): one entry per import run ---
    struct SourceRef {
        std::string path;               // file / directory imported
        std::string importer;           // plugin id ("odbpp", "gerber", ...)
        std::string imported_at;        // ISO-8601 local time
        std::map<std::string, std::string> settings;   // importer options used
        std::string summary;            // result line
        std::string result;             // "Complete" | "Artwork-only" | "Partial" | "" (REVIEW_V9 §6D)
        std::vector<std::string> warnings;
    };
    std::vector<SourceRef> sources;
    // --- board photo / AOI image underlays (LEAD_BRIEF §14, core/board_image.h) ---
    std::vector<BoardImage> board_images;
    // --- R16b: Gerber-assisted package acquisition wizard (core/package_resolve.h), persisted "pkg_wizard" ---
    struct PkgWizardState {
        std::vector<std::string> done;      // packages built / assigned through the wizard
        std::vector<std::string> skipped;   // skipped for now (revisited at the end of the queue)
        std::string current;                // package the wizard was on (resume point)
        int placement = 0;                  // n-th placement of `current` shown (cycled by "Next placement")
        bool started = false;
    } pkg_wizard;
    bool plot_aligned = false;          // R16b: placements confirmed aligned to the Gerbers (Align Plot to Gerber / user)
    // R22 (REVIEW_V21 §1): Board > Panelize dialog parameters, persisted "array_params". OK in either dialog saves
    // these without touching the array; Apply commits. They never change geometry by themselves.
    struct ArrayParams {
        struct Cell { int x = 0, y = 0, rot = 0; bool root = false, x_out = false; };
        bool nl_saved = false;                  // nonlinear (rotation-group) dialog
        std::vector<Cell> nl_cells;
        double nl_groups[4][4] = {};            // per 0/90/180/270 group: red_x, red_y, blue_x, blue_y (in nl_units)
        Units nl_units = Units::MM;
        std::string nl_ref;                     // reference location (component ref)
        bool lin_saved = false;                 // linear (step & repeat) dialog
        int lin_nx = 2, lin_ny = 1, lin_root_col = 0, lin_root_row = 0;
        double lin_pitch_x = 0, lin_pitch_y = 0;    // mm
        Units lin_units = Units::MM;
    } array_params;
    std::vector<std::string> migration_notes;   // R22: what loading changed in an older project (not persisted; shown once)
};

// Chain open segments (e.g. CircuitCAM outlines stored as 4 CGrLine objects)
// into polylines; chains whose ends meet within tol are closed (last == first).
std::vector<std::vector<Vec2>> join_frame_segments(const std::vector<std::vector<Vec2>>& in, double tol = 0.05);
// even-odd point in polygon test over closed polylines (open ones ignored)
bool point_in_frame(const std::vector<std::vector<Vec2>>& frame, Vec2 p);

// Expand "R1-R5, C3,C7-9" style reference lists (BOM consolidation).
std::vector<std::string> expand_ref_list(const std::string& s);

// Natural compare for refdes sorting (R2 < R10).
bool ref_less(const std::string& a, const std::string& b);

std::string units_suffix(Units u);
double to_display(double mm, Units u);
double from_display(double v, Units u);

} // namespace pcbp
