// machine_history - what actually ran on the JUKI lines, harvested by tools/pnp_harvest
// (h8h / iss / e47 / e48 / x52 / x01 / s01x), normalised to FlexLine component-DB codes.
//
// Two uses:
//   1. per part name: the machine-measured body (laser auto-measure) and the tape / pitch / nozzle
//      that the operators settled on (majority over every program the part appears in)
//   2. size tables learned from all parts: body L x W (x H) -> tape type, feed pitch, nozzle,
//      for parts that have never run.
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

namespace pcbp::history {

// one harvested record, already normalised
struct Record {
    std::string name;          // upper-case component name
    std::string ext;           // ".e48" ...
    double l = 0, w = 0, h = 0;   // mm, l >= w
    int tape = 0;              // Feeder.TapeTypeID (1 = 8mm paper, 2 = 8mm emboss, 3 = 12mm ...), 0 unknown
    int pitch = 0;             // Feeder.Pitch (mm; 204/206 = 2/4 mm special codes kept as-is)
    int nozzle = 0;            // Laser.GX4NozzleNo / LaserFx3.NozzleNo (500..520), 0 unknown
    bool measured = false;     // machine split file (laser-measured dims) vs master program
};

// decoders, exposed for tests
int tape_from_code(const std::string& tape_code);              // "0x0101" / "8MMPAPER" -> 1
int pitch_from_code(const std::string& pitch_code);            // "0x0004" / "4" -> 4
int nozzle_from_field(const std::string& ext, const std::string& nozzle_field);   // machine -> FlexLine code

struct Pick { int value = 0; double share = 0; int votes = 0; };

struct PartSummary {
    double l = 0, w = 0, h = 0;
    bool measured = false;
    int programs = 0;
    Pick tape, pitch, nozzle;
};

struct Proposal {
    double l = 0, w = 0, h = 0;
    int tape = 0, pitch = 0, nozzle = 0;
    double conf_tape = 0, conf_pitch = 0, conf_nozzle = 0;
    bool from_history = false;
    std::string why;
};

// body known from the package itself (IPC-7351 land-pattern name, package code in the description)
struct BodyHint { double l = 0, w = 0, h = 0; std::string src; };

class History {
public:
    // load one pnp_harvest *_components.csv; may be called repeatedly. Returns records read.
    size_t load_csv(const std::string& path, std::string& err);
    // load a cpf_harvest *_parts.csv: CAD body estimate per part number (used when the part never ran)
    size_t load_cpf_parts(const std::string& path, std::string& err);
    bool cad_body(const std::string& name, double& l, double& w, double& h) const;
    void add(const Record& r);
    void build();                                  // summaries + size tables; call after loading
    size_t records() const { return n_records_; }
    size_t parts() const { return parts_.size(); }

    const PartSummary* find(const std::string& name) const;
    // size-table vote; exclude: remove one vote of this value (leave-one-out evaluation)
    Pick by_size(double l, double w, double h, int field, int exclude = 0) const;   // field 0 tape, 1 pitch, 2 nozzle

    // vote keyed on the CAD land-pattern extent (pads incl. toes) instead of the body
    Pick by_extent(double el, double ew, int field) const;
    // full proposal. ext_l / ext_w = CAD land-pattern extent of the part in the job being taught (mm, 0 = unknown),
    // body_h = known height (0 = unknown).
    // use_part_history = false evaluates the size tables alone.
    Proposal propose(const std::string& name, const std::string& desc, double ext_l, double ext_w, double body_h,
                     bool use_part_history = true, const BodyHint* hint = nullptr) const;

    // nozzles the shop owns: everything that ran in the harvested programs (FlexLine codes), or the user's list
    const std::set<int>& owned_nozzles() const { return owned_user_.empty() ? owned_auto_ : owned_user_; }
    const std::set<int>& owned_from_history() const { return owned_auto_; }
    const std::map<int, int>& nozzle_use() const { return nozzle_use_; }
    void set_owned_nozzles(const std::set<int>& s) { owned_user_ = s; }

private:
    std::vector<Record> pending_;
    std::map<std::string, PartSummary> parts_;
    struct Body { double l, w, h, el, ew; };
    std::map<std::string, Body> cad_;
    std::map<long long, std::map<int, int>> ext_[3];
    std::map<long long, std::pair<std::vector<double>, std::vector<double>>> ratio_;   // extent bucket -> body/extent l, w
    std::map<long long, std::pair<double, double>> ratio_med_;
    std::map<long long, std::map<int, int>> fine_[3], coarse_[3];
    size_t n_records_ = 0;
    std::map<int, int> nozzle_use_;       // nozzle -> parts whose majority nozzle it is
    std::set<int> owned_auto_, owned_user_;
};

// Package evidence from text: package code in a description ("SOD123", "SOT-23-5", "SMA") or an
// IPC-7351 land-pattern name ("SOD3717X135N", "CAPC1005X56N", "SOT95P280X145-5N", "QFN50P400X400X100-21N").
struct PkgBody {
    std::string code;          // canonical package code ("SOD123"), "" for a generic IPC name
    std::vector<std::string> aliases;   // names the library may use for the code
    double l = 0, w = 0, h = 0;          // body, mm, l >= w (h 0 = unknown)
    int pins = 0;
    std::string src;
};
bool package_code(const std::string& text, PkgBody& out);       // code table
bool ipc_body(const std::string& pkg_name, PkgBody& out);        // IPC-7351 name parser
std::string norm_code(const std::string& s);                     // upper, '-'/'_' between alnum dropped
// plausible height when nothing measured it (chip ~ its width, ICs by size)
double fake_height(double l, double w, bool passive);

// chip size code from a description ("0402" ...), "" if none; dims of a size code (mm, l >= w)
std::string chip_code_in(const std::string& desc);
bool chip_dims(const std::string& code, double& l, double& w);
bool is_passive(const std::string& desc);   // resistor / capacitor wording

}  // namespace pcbp::history
