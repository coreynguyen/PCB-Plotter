#pragma once
// ============================================================================
// program_view.h — R23 read-only view model of a machine program (Files tab program viewer).
//
// HLC (.h8h / .h7h / .H81.. splits) and ISS (.iss / .i01.. splits, XML) are parsed with the existing
// importer (io/juki_import: import_hlc / import_iss) into a scratch Project, then flattened into placements +
// fiducials + board rectangle. ISS additionally gets the feeder slot of every component from
// <pickData>/<feederPosition>/<feeder> (index, componentName, position stationId / bankPos / holeNo), ported
// from the author's earlier TrayTagPrinter ISS reader. specs/ISS_FORMAT.md.
// ============================================================================
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace pcbp::io::progview {

struct Place { std::string ref, part, package, feeder; double x = 0, y = 0, rot = 0; bool skip = false; int image = 0; };
struct Fid { std::string ref; double x = 0, y = 0, dia = 0; };
struct Program {
    std::string format;                 // "H8H", "ISS", ...
    std::string name;                   // program / board id
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // board rectangle (mm); x1 <= x0 when unknown
    std::vector<Place> places;
    std::vector<Fid> fids;
    std::vector<std::string> warnings;
};
bool load(const std::vector<uint8_t>& bytes, Program& out, std::string& err);
// ISS feeder slots by component name ("St1 Bank2 #15"). Empty for non-ISS text.
std::map<std::string, std::string> iss_feeders(const std::string& xml);

} // namespace pcbp::io::progview
