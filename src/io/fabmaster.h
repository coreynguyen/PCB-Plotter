#pragma once
// ============================================================================
// fabmaster.h — FABmaster FATF (.fab / .fatf) import (pcbp::io), portable.
//
// Sections start with ':' ("…;" terminated records, may span lines, "{ }"
// list wrappers ignored):
//   :FABMASTER FATF REV n;     header (sniffing)
//   :UNITS = 1/1000 INCH;      also "1/10000 INCH", "MM", "1/100 MM", "MILS"
//   :PADS    pad_id,name,shape(ROUND|RECT|SQUARE|OBLONG|OCTAGON),w,h[,drill];
//   :PARTS   part_id,ref,part_number,package,x,y,rot,T|B[,value];
//   :PINS    part_id,pin_name,x,y,pad_id;   (absolute pin centres)
//   :BOARD_OUTLINE  LINE,x1,y1,x2,y2;  ARC,xs,ys,xe,ye,xc,yc,CW|CCW;
// Unknown sections are skipped. Pads are rebuilt in package space from the
// part placement (rotation CCW, B = bottom/mirrored).
// ============================================================================
#include "cad_common.h"
#include <string>

namespace pcbp::io {

int sniff_fabmaster(const std::string& path, const std::string& head);
bool import_fabmaster_text(const std::string& text, Project& prj, const CadImportOptions& opt,
                           CadImportResult& res, std::string& err);
bool import_fabmaster_file(const std::string& path, Project& prj, const CadImportOptions& opt,
                           CadImportResult& res, std::string& err);

} // namespace pcbp::io
