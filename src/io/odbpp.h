#pragma once
// ============================================================================
// odbpp.h — ODB++ job import (pcbp::io), portable. See specs/ODB_RE.md.
//
// Input: a job directory, the matrix/matrix file inside one, or an archive
// (.tgz/.tar.gz/.tar/.zip/.tar.Z, nested archives one level deep). The job
// root is the shortest path prefix of a "matrix/matrix" member.
//
// Read per step:
//   steps/<s>/stephdr             STEP-REPEAT { NAME X Y DX DY NX NY ANGLE MIRROR }
//                                 X_DATUM / Y_DATUM
//   steps/<s>/profile             board outline surface (OB/OS/OC/OE)
//   steps/<s>/eda/data            PKG / PIN + pin shapes (RC, CR, SQ, CT)
//   steps/<s>/layers/<comp>/components   CMP / PRP / TOP (comp_+_top / comp_+_bot)
//   steps/<s>/layers/<drill>/features    round drill pads -> pin drills
//   steps/<s>/layers/<outer>/features    copper / silk / mask / paste ->
//                                 RS-274X layers (geometry fallback)
// Every member may be stored plain, ".Z" (Unix compress LZW) or ".gz".
//
// Units: "UNITS=MM|INCH" or legacy "U MM|INCH" per file (default INCH).
// Symbol sizes (r12, rect20x40, ...) are mil in INCH files, micron in MM.
// Rotation: CMP rot is clockwise (ODB++ v7/v8); our model is CCW, so
// rot_ccw = -rot. Bottom ('M' mirror flag or bottom component layer):
// footprint is mirrored in X then rotated — the same order as
// Project::comp_xform, so rot_ccw = -rot holds on both sides. The convention
// is verified per job against TOP toeprints when eda/data exists; if the
// opposite sign fits, it is used and a warning reports it.
//
// CadImportOptions::step syntax:  "<step>[|images][|flat][|layers][|nolayers]"
//   images  (default for a step whose own components are empty but that has
//           STEP-REPEAT children) import the leaf board once + panel images
//   flat    expand every step-repeat instance as separate components (_n refs)
//   layers  also add copper/silk/mask/paste/drill as Gerber layers (default:
//           only when no components were found)
// ============================================================================
#include "cad_common.h"
#include <string>
#include <functional>
#include <vector>

namespace pcbp::io {

struct OdbRepeat {
    std::string child;
    double x = 0, y = 0, dx = 0, dy = 0, angle_cw = 0;   // mm / deg
    int nx = 1, ny = 1;
    bool mirror = false;
};

struct OdbStepInfo {
    std::string name;
    std::string kind;               // "pcb" | "panel" | "array" | "coupon" | "unknown"
    double w = 0, h = 0;            // profile extent (mm), 0 when no profile
    bool has_profile = false, has_eda = false;
    int comp_top = 0, comp_bot = 0; // CMP records in this step's own component layers
    int leaf_comps = 0;             // components reachable through step-repeat (per instance sum)
    int layers_with_features = 0;
    int instances = 0;              // total step-repeat instances (all children)
    std::vector<OdbRepeat> repeats;
    std::vector<std::string> notes; // missing members, compressed members, ...
    // units (REVIEW_V9 §6C): what each file of this step declares
    std::vector<std::string> unit_evidence;   // "profile: UNITS=MM", "eda/data: (none -> inch default)" ...
    double units_k = 0;             // mm per unit if all declaring files agree, else 0
    bool units_conflict = false;    // files declare different units
    bool units_declared = false;    // at least one file declares units
    BBox profile_box, comp_box;     // mm, as declared (comp_box: CMP centroids)
    int comps_outside_profile = 0;  // CMP centroids outside the profile box (+1 mm)
    int pkg_outlines = 0, pkg_extents = 0, pkgs = 0;   // eda/data PKG records with outline / extent
};

struct OdbMatrixLayer {
    std::string name, type, context, polarity;
    int row = 0;
    std::string side;               // "top" | "bottom" | "inner" | ""
};

struct OdbJobInfo {
    std::string root;               // job root inside the tree ("" or "job/")
    std::string container;          // "directory" | "tgz" | "zip" | "tar.Z" | "nested: x.tgz" ...
    std::vector<OdbStepInfo> steps;
    std::vector<OdbMatrixLayer> layers;
    int recommended = -1;           // index into steps
    std::string recommend_reason;
    bool recommend_images = false;  // recommended step should use step-repeat images
    int compressed_members = 0;
    std::vector<std::string> warnings;
    std::string units;              // of the recommended step's eda/data or components
    std::string job_units;          // misc/info UNITS= ("" when absent)
};

// Suspicious-range checks for a board of w x h mm (after units/scale) with
// placement box `comps`: board > 1 m or < 5 mm, placements outside the
// profile. Empty = nothing suspicious.
std::vector<std::string> odb_unit_warnings(double w, double h, int comps_outside, int comps_total);

int sniff_odbpp(const std::string& path, const std::string& head);

// Inventory without importing (for the import dialog / diagnostics).
bool odb_scan(const std::string& path, OdbJobInfo& info, std::string& err);
// Human readable multi-line summary of the job (steps, recommendation).
std::string odb_describe(const OdbJobInfo& info);

bool import_odbpp(const std::string& path, Project& prj, const CadImportOptions& opt,
                  CadImportResult& res, std::string& err);

// Convert one ODB++ features file to RS-274X text (mm, 4.6). Returns false
// when nothing drawable. `notes` receives counts of unsupported symbols.
bool odb_features_to_gerber(const std::string& features, std::string& gerber, std::string& notes);
// R12: with user-defined symbol support. lookup(name, features) returns the features text of
// <job>/symbols/<name>/features (also by the original Gerber aperture name stored as ".source_name" in
// the symbol's attrlist, e.g. "i274x.rotatedrectd154.d154"); user symbols may nest (depth <= 4).
using OdbSymbolLookup = std::function<bool(const std::string& name, std::string& features)>;
bool odb_features_to_gerber(const std::string& features, std::string& gerber, std::string& notes, const OdbSymbolLookup* lookup);
bool odb_features_to_excellon(const std::string& features, std::string& drill, int& holes);

} // namespace pcbp::io
