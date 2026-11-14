#!/bin/bash
# R11 P1-01: Dear ImGui ID-conflict walk under Wine. Fails (exit 1) when any "visible items with conflicting ID"
# is reported while the app walks Home, every view of a project, every top-level menu and the common dialogs
# (+ the BOM import controls when a BOM file is given). The probe needs no hovering: every interactive item id
# of every frame is checked (patched ImGui ItemHoverable hook, see third_party/imgui/VENDORED.txt).
# usage: tests/idwalk.sh <PCBPlotter.exe> <project.pcbp> [bom.csv]      env: WINEPREFIX (private), DISPLAY or Xvfb
set -u
EXE=$(readlink -f "$1"); PRJ=$(readlink -f "$2"); BOM=${3:+$(readlink -f "$3")}
W=$(mktemp -d); LOG=$W/idwalk.log
export WINEDEBUG=-all PCBP_IDCHECK="Z:${LOG//\//\\}" PCBP_IDWALK=1
[ -n "$BOM" ] && export PCBP_TEST_IMPORT="Z:${BOM//\//\\}"
if [ -z "${DISPLAY:-}" ]; then DN=$((100 + RANDOM % 800)); Xvfb :$DN -screen 0 1600x900x24 >/dev/null 2>&1 & XP=$!; export DISPLAY=:$DN; sleep 2; fi
cp "$EXE" $W/PCBPlotter.exe
(cd $W && timeout 300 wine PCBPlotter.exe "Z:${PRJ//\//\\}"); rc=$?
[ -n "${XP:-}" ] && kill $XP
cat $LOG 2>/dev/null
grep -q "walk done" $LOG 2>/dev/null || { echo "idwalk: walk did not finish (rc=$rc)"; exit 2; }
[ $rc -eq 0 ] && { echo "idwalk: PASS (no ID conflicts)"; exit 0; }
echo "idwalk: FAIL ($rc conflict(s))"; exit 1
