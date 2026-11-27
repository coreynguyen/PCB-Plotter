// juki_autopart - machine data for H8H parts that are not in the component database.
//
// Two ways, both producing the same records as Database -> Research (io/juki_research):
//   * template: a package profile of a FlexLine package library (.flx) taught for the part (flexdb::teach_part)
//     with the part's own body, carrier, pitch and nozzle put over it, as applying the profile to a component DB
//     and then the part's machine data would (template_rows);
//   * built-in rules (no database at all): generic laser-centring data from the package family and the body size
//     (builtin_rows). Nozzle, carrier, pitch and component class come from the part's machine data
//     (core/machine_assign) when set; speeds, vacuum codes, laser height and mechanical centring are ours, by family
//     and size band. Vision recognition data is never generated: large ICs and BGAs are laser centred.
#pragma once
#include "core/machine_job.h"
#include "io/juki_builtin.h"
#include "io/juki_research.h"

#include <string>
#include <vector>

namespace pcbp::juki {

// The built-in rules' input for a project part (class from its machine data or package family).
BuiltinInput builtin_input(const JobPart& p);

// Built-in rules, researched: the part's records as research would write them for a DB part with these rows.
bool builtin_research(const JobPart& p, ResearchResult& out, std::string& why);

// The columns teaching puts over a package-library profile for this part: Compo body (Width the long edge, Length
// the short edge), height, description, data-ok; Feeder carrier and pitch; Laser / LaserFx3 nozzle; Expansion auto-teach
// for paper. 0 / empty arguments leave the profile's value. l / w / h in mm.
PartRows template_rows(const std::string& description, double l, double w, double h, int tape_type, int pitch, int nozzle);

}  // namespace pcbp::juki
