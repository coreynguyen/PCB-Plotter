#pragma once
// ============================================================================
// flexdb.h — JUKI FlexLine component database (FlexDB) integration.
//
//   * Library      : read-only package-profile library (".flx", Jet4) — e.g.
//                    "package LM.flx". Profiles are Compo rows + per-CompoID rows
//                    in the parameter tables (kCopyTables).
//   * ComponentDB  : an editable FlexLine component database (".mdb"): a new one
//                    made from FlexLine's empty template, or an existing one
//                    (e.g. Parts.mdb). Add parts (20-char machine names), apply profiles
//                    (verbatim per-CompoID row copy + Compo stamps), set paper /
//                    emboss, save (Jet4 writer, see jet_write.h).
//   * Classifier   : C++ port of FlexAssign's description heuristics, with house
//                    conventions learned from an assigned database.
//   * Footprint / evaluate(): new evidence from the PCB Plotter package (pad
//                    geometry, body artwork) -> ranked profile suggestions,
//                    tape/nozzle implications and mismatch warnings.
//
// Units: FlexDB stores lengths in 1/10000 mm; everything here is millimetres.
// Link: ComponentDB Compo.PkgTypeID == Library Compo.CompoID.
// ============================================================================
#include "machine_history.h"
#include "jet_write.h"
#include "../core/project.h"
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace pcbp::flexdb {

constexpr int kMachineNameMax = 20;   // JUKI machine limit for component names

// Per-component parameter tables copied verbatim when a profile is applied.
extern const char* const kCopyTables[];
extern const int kCopyTableCount;

enum PkgClass { ClsChip = 1, ClsMelf = 3, ClsECap = 6, ClsSot = 11, ClsSoj = 12, ClsLed = 14, ClsSop = 101,
                ClsQfp = 102, ClsPlcc = 103, ClsTsop = 104, ClsBga = 107, ClsFbga = 110, ClsQfn = 120,
                ClsConn = 201, ClsConn2 = 202, ClsOutline = 251 };

struct Profile {
    int id = 0;
    std::string name;
    int cls = 0;                       // PkgClassID
    double w = 0, l = 0, h = 0;        // nominal Compo.Width / Length / Height (mm)
    int tape_type = 0;                 // Feeder.TapeTypeID (1 = 8mm paper, 2 = 8mm emboss, ...)
    int feeder_type = 0;               // Feeder.FdrTypeID (1 tape, 2 stick, 4 tray, 8 bulk)
    int pitch = 0;                     // Feeder.Pitch (mm)
    int nozzle = 0;                    // Laser.GX4NozzleNo (500..508)
    int auto_teach = -1;               // Expansion.AutoTeach
    double lead_pitch = 0, lead_len = 0, lead_w = 0;   // Visiondata
    int side_count[4] = {0, 0, 0, 0};  // leads per side (bottom, right, top, left) from Side rows
    int sibling = 0;                   // paper <-> emboss twin profile id (0 = none)
    char tape_kind = 0;                // 'P' paper, 'E' emboss (from name), 0 unknown
};

std::string tape_type_name(int tape_type_id);
std::string nozzle_for_short_edge(double mm);

struct FeederFit { int tape_mm = 0, pitch_mm = 0; std::string nozzle; double long_edge = 0, short_edge = 0; };
FeederFit predict_feeder(double a_mm, double b_mm);

// ---------------------------------------------------------------------------
class Library {
public:
    bool open(const std::string& path, std::string& err);
    bool is_open() const { return !profiles.empty(); }
    const Profile* find(int id) const;
    const Profile* find_name(const std::string& name) const;   // case-insensitive
    std::string class_name(int cls) const;
    std::string path;
    std::vector<Profile> profiles;           // sorted by name
    std::map<int, std::string> class_names;  // PkgClass.EngClass
    // raw rows for applying (table -> loaded table); Compo included
    std::map<std::string, jet::ETable> tables;
    std::map<std::string, std::multimap<int, size_t>> by_compo;   // table -> CompoID -> row index
private:
    std::map<int, size_t> index_;
};

struct Part {
    int id = 0;                    // CompoID
    std::string name, desc, ext_name;
    int pkg_id = 0;                // PkgTypeID (0 = untaught)
    std::string pkg_name;          // PkgType (cached text)
    int cls = 0;
    double w = 0, l = 0, h = 0;    // mm
    int data_ok = 0;
    int tape_type = 0;             // Feeder.TapeTypeID (0 = no feeder row)
    int auto_teach = -1;
};

class ComponentDB {
public:
    bool open(const std::string& path, std::string& err);
    bool create_from_template(const std::string& template_path, std::string& err);
    // New, empty component database from ANY FlexLine database (template not needed): every component table is
    // emptied; reference data (FeederType, Lookup, PkgClass, VacuumDflt, FlexDBInfo, WriteLockInfo) and the default
    // rows (Side 1, CoplaGX4 default) are kept. The source file is only read. Unsaved until save().
    bool create_empty_from(const std::string& any_flexline_db, std::string& err);
    bool save(const std::string& path, std::string& err);   // stores touched tables, writes file
    bool is_open() const { return open_; }
    const std::string& path() const { return path_; }
    bool dirty() const { return !touched_.empty(); }

    std::vector<Part> parts();
    int find(const std::string& machine_name);              // CompoID, 0 if missing (case-insensitive)
    // add an untaught part (FlexLine import defaults). name must already obey the machine rules.
    int add_part(const std::string& machine_name, const std::string& desc, const std::string& ext_name, std::string& err);
    bool set_description(int compo_id, const std::string& desc, std::string& err);
    bool apply_profile(int compo_id, const Library& lib, int profile_id, int cls_override, bool copy_dims, std::string& err);
    bool set_tape(int compo_id, bool paper, std::string& err);
    // write a machine-history proposal over an applied profile: Compo Width/Length/Height (body),
    // Feeder TapeTypeID/Pitch, Laser.GX4NozzleNo + LaserFx3.NozzleNo. Fields that are 0 are left alone.
    bool apply_machine(int compo_id, const history::Proposal& p, std::string& err);   // Feeder.TapeTypeID 1/2 + Expansion.AutoTeach 1/0
    jet::ETable* table(const std::string& name, std::string* err = nullptr);   // lazy load
    jet::Editor& editor() { return ed_; }

private:
    bool ensure_lookups(const Library& lib, std::string& err);
    void load_defaults();
    void apply_defaults(jet::ETable& t, size_t row);              // Jet DefaultValue of every column
    std::map<std::string, std::map<std::string, jet::Value>> defaults_;   // TABLE -> COLUMN -> default
    bool defaults_loaded_ = false;
    jet::Editor ed_;
    bool open_ = false, lookups_done_ = false;
    std::string path_;
    std::map<std::string, jet::ETable> t_;
    std::set<std::string> touched_;
};

// 20-character machine name for a part number. `taken(name)` must return true when the
// candidate is already used by a DIFFERENT part. Unsupported characters are replaced so the
// name is indexable; collisions get a "~n" suffix inside the 20 characters.
std::string machine_name(const std::string& pn, const std::function<bool(const std::string&)>& taken);
std::string sanitize_machine_chars(const std::string& s);

// Column DefaultValue properties from a table's MSysObjects.LvProp blob ("MR2" format).
std::map<std::string, std::string> parse_lvprop_defaults(const std::vector<uint8_t>& blob);

// ---------------------------------------------------------------------------
// Classifier (FlexAssign heuristics)
// ---------------------------------------------------------------------------
struct Guess {
    int pid = 0;                   // proposed profile (0 = none)
    double conf = 0;
    std::string why;
    double variant = 0;            // share of the house convention for this key
    std::string alt;               // "64x Cap0805(2012) 500 Em, ..."
    std::vector<int> alt_ids;      // runner-up profiles for the same key
    int cls_override = 0;
    bool gap = false;              // known package, no profile in the library -> build one
    double gap_l = 0, gap_w = 0, gap_h = 0;
    int gap_cls = 0;
    std::string gap_label;
};

// Profile chosen from the body (l >= w, mm), carrier and nozzle, scored against every library profile:
// log-size distance + height, tape-type mismatch, package-class / family-word / chip-size / nozzle-in-name agreement.
struct BodyPick { int pid = 0; double cost = 1e9; std::string why; };
// owned: nozzles the shop has (empty = any); profiles that need another nozzle are pushed far down
BodyPick pick_by_body(const Library& lib, const std::string& desc, double l, double w, double h, int tape_type, int nozzle,
                      const std::set<int>* owned = nullptr, int pins = 0);
// library profile named after a package code ("SOD123", "SOD-123", "SOT23-5"); owned nozzle and house use break ties
int profile_for_code(const Library& lib, const std::vector<std::string>& aliases, const std::set<int>* owned, const std::map<int, int>* used);
// same body / class / carrier as pid but with a nozzle the shop owns (pid itself when it is fine, 0 if none)
int owned_twin(const Library& lib, int pid, const std::set<int>& owned);

class Classifier {
public:
    void set_library(const Library& lib);
    // learn house conventions from assigned rows (name, description, PkgTypeID)
    struct Row { std::string name, desc; int pid = 0; };
    void learn(const std::vector<Row>& rows);
    // w/l: machine-measured body (mm, 0 = unknown)
    Guess classify(const std::string& name, const std::string& desc, double w = 0, double l = 0) const;
    const Library* lib() const { return lib_; }
    // size code of a chip profile name ("0603" for "Res0603(1608) N:500"), else the name
    static std::string base_size(const std::string& profile_name);
    std::map<std::string, std::map<int, int>> chip, lead, disc;
    std::map<int, int> used;
private:
    struct NB { int id = 0; double err = 0; std::string note; };
    bool nearest(double len, double wid, int want_cls, const char* fam, double tol, NB& out) const;
    Guess by_package(int cls, double L, double W, double H, const std::string& label, const std::string& note) const;
    int profile_named(const std::string& fam, int pins, std::vector<int>& hits) const;
    std::string alt_text(const std::map<int, int>& c, int chosen, std::vector<int>* ids) const;
    const Library* lib_ = nullptr;
};

// text helpers (exposed for tests)
std::vector<std::string> text_chip_sizes(const std::string& s);    // "0603", ...
bool chip_dims(const std::string& code, double& l, double& w);
std::string family_of(const std::string& s);                         // RES CAP IND TANT LED BEAD ...

// ---------------------------------------------------------------------------
// Footprint evidence
// ---------------------------------------------------------------------------
struct Footprint {
    bool valid = false;
    bool smt = true;
    int pads = 0;
    double ext_l = 0, ext_w = 0;       // pad extent (long, short), mm
    double body_l = 0, body_w = 0;     // estimated component body (long, short), mm
    bool body_from_art = false;
    bool body_known = false;           // body from artwork or a recognised land pattern (2-pad, dual, quad)
    double pitch = 0;                  // smallest pad-to-pad pitch in a row (multi-pad parts)
    int sides = 0;                     // pad rows: 2 = dual, 4 = quad, 1 = single row, 0 = n/a
    std::string chip_code;             // nearest chip size from the body ("0805"), "" if not chip-like
    double chip_err = 1;
    std::string shape;                 // "2-pad chip", "SOT (3 pads)", "dual row 8", ...
};
Footprint measure_footprint(const Package& pkg);

struct Suggestion {
    int pid = 0;
    double conf = 0;
    double geo_err = -1;               // relative body-size error vs footprint (-1 = n/a)
    std::string why;
};

enum class Status { Untaught, Taught, Auto, Conflict };
const char* status_name(Status s);

struct Evaluation {
    Guess text;                        // description classifier
    Footprint fp;
    std::vector<Suggestion> sugg;      // ranked, best first
    std::vector<std::string> warnings;
    bool conflict = false;
    FeederFit feeder;                  // implied by footprint body (or best profile)
};
Evaluation evaluate(const Classifier& cls, const std::string& name, const std::string& desc, const Package* pkg,
                    double meas_w = 0, double meas_l = 0);

// Tooling readiness of a set of part numbers (Export page summary, filled by the Components view).
struct Readiness {
    bool library = false;              // a profile library is loaded (statuses are meaningful)
    bool db = false;                   // a target FlexLine DB is open
    std::string library_name;
    int parts = 0;                     // distinct part numbers considered
    int taught = 0, autoa = 0, untaught = 0, conflict = 0;
    int renamed = 0;                   // machine name differs from the part number (20-char rule)
    int pending = 0;                   // assignments not yet written to the FlexLine DB
    std::map<std::string, int> tape;   // "8 mm paper" -> parts
    std::map<std::string, int> nozzle; // "502" -> parts
    std::vector<std::string> attention;   // untaught / conflict part numbers (first few)
    int open() const { return untaught + conflict; }
};

// One-shot teaching for a part: FlexLine profile + body + carrier + nozzle.
// Profile: house-trained text classifier when it is sure (conf >= 0.9), otherwise the body pick.
// Profile: house-trained text classifier when sure, else a library profile named after the package code
// (SOD123 ...), else the closest body; every pick is moved onto a nozzle the shop owns. Body comes from the
// part's machine history, its .cpf body, the package code / IPC land-pattern name, the footprint, or the
// land-pattern extent; height is estimated when nothing measured it. Always returns a pick when any size is known.
struct Teach {
    int pid = 0;
    double conf = 0;               // for optional review only
    std::string profile_route;     // "text 0.95: ..." / "package SOD123" / "body: ..."
    history::Proposal machine;
};
Teach teach_part(const Classifier& cls, const Library& lib, const history::History& hist, const std::string& name, const std::string& desc,
                 double ext_l, double ext_w, bool use_part_history = true, const std::string& pkg_name = std::string(),
                 double fp_body_l = 0, double fp_body_w = 0);

} // namespace pcbp::flexdb
