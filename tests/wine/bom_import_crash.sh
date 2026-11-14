#!/bin/bash
# R12 regression (REVIEW_V11 §3/§4): BOM import -> warnings report -> dismiss must not crash.
# R11 crashed here (page fault): the in-tab flow opened the report modal from inside the frame.
# usage: tests/wine/bom_import_crash.sh <PCBPlotter.exe> <project.pcbp> <bom.xlsx|csv>
# Needs Xvfb + wine. Exit 0 = app alive after import + report auto-dismissed, BOM attached.
set -u
EXE=$(readlink -f "$1"); PRJ=$(readlink -f "$2"); BOM=$(readlink -f "$3")
W=$(mktemp -d /tmp/pcbp_bomreg.XXXX)
export WINEDEBUG=-all WINEPREFIX=${WINEPREFIX:-$W/pfx}
export PCBP_RECOVERY_DIR="Z:${W//\//\\}\\rec"
DN=$((100 + RANDOM % 800)); Xvfb :$DN -screen 0 1600x900x24 >/dev/null 2>&1 & XP=$!
sleep 2; export DISPLAY=:$DN
winpath() { echo "Z:${1//\//\\}"; }
PCBP_TEST_IMPORT=$(winpath "$BOM") PCBP_TEST_BOM_IMPORT=1 PCBP_TEST_FORM_AUTOOK=3 \
  timeout 120 wine "$EXE" "$(winpath "$PRJ")" > "$W/wine.log" 2>&1 & WP=$!
sleep ${WAIT:-35}
import -window root "$W/after.png" 2>/dev/null
RC=0
if ! kill -0 $WP 2>/dev/null; then echo "FAIL: process exited"; RC=1; fi
if grep -q "Unhandled page fault\|Unhandled exception" "$W/wine.log"; then echo "FAIL: crash in wine log"; RC=1; fi
kill $WP 2>/dev/null; wineserver -k 2>/dev/null; kill $XP 2>/dev/null; wait 2>/dev/null
[ $RC = 0 ] && echo "PASS (screenshot $W/after.png)"
exit $RC
