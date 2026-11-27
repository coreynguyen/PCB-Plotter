// juki_builtin - generic machine data for a part without a database row: laser centring (plus mechanical centring for
// small parts), nozzle / vacuum / laser height / speeds / feeder from the component class and the body size. Shared by
// PCB Plotter (parts not in the DB, io/juki_autopart) and Juki Tool (size edits, juki_resize). Standard library only.
//   * nozzle: given (house nozzles 500, 504..508), else by the body's short edge on a step table per package group
//     (thresholds fitted to the house component DB: 95 % agreement for chips)
//   * laser height: a fraction of the body height by family; mechanical centring for chip / SOT / SOP families
//   * speeds, accelerations, vacuum codes: four tiers (chip, SOT, IC, large IC)
//   * feeder: given packaging / tape / pitch, else tape (QFP / BGA / PLCC types on a JEDEC tray); 8 mm tape feeds
//     2 / 4 mm only, paper for flat chips only
// Vision recognition data is never generated.
#pragma once
#include "juki_record.h"

#include <string>
#include <vector>

namespace pcbp::juki {

struct BuiltinInput {
    int cls = 0;                     // component class code (H8H +78); 0 -> SOP rules
    double x = 0, y = 0, h = 0;      // body mm: X / Y at 0 deg (Compo Width / Length), height (0 = assumed by family)
    std::string description;
    int feeder_kind = 0;             // H8H +80: 1 tape, 2 stick, 3 tray, 4 bulk; 0 = by family
    int tape_w = 0, pitch = 0;       // tape width / feed pitch mm (0 = from the body)
    std::string carrier;             // "Paper" / "Embossed" / "" (from the tape and the part)
    int nozzle = 0;                  // GX-4 nozzle (0 = from the body)
    double supply_angle = 0;         // deg
};

// A part's machine data, generated without a component DB.
struct AutoPart {
    PartRows rows;
    int cls = 0;                     // component class code (Compo.PkgClassID)
    std::string family;              // "chip", "SOT", "SOP", "QFP", "QFN", "BGA", "connector", ...
    std::vector<std::string> notes;  // what was assumed (height, stick type, oversize, ...)
};

// The part's rows. false (+ why) without a body size.
bool builtin_rows(const BuiltinInput& p, AutoPart& out, std::string& why);
// ... researched: the records research writes for a DB part with these rows. rows (optional): the rows used.
bool builtin_research(const BuiltinInput& p, ResearchResult& out, std::string& why, AutoPart* rows = nullptr);

int builtin_nozzle(double short_edge, int cls = 1);            // GX-4 nozzle for a body short edge (mm) and class
void builtin_feeder(double x, double y, int& tape_w, int& pitch);   // tape width / pitch mm for a body
const char* builtin_family(int cls);                            // family name the rules use for a class

}  // namespace pcbp::juki
