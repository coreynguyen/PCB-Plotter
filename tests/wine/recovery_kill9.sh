#!/bin/bash
# R12 acceptance (REVIEW_V11 §4): unsaved placement edits survive kill -9.
#  1) open <project>, make 5 unsaved placement edits (PCBP_TEST_EDITS), wait for the autosave, kill -9
#  2) relaunch: screenshot of the recovery dialog (Restore / Review / Discard, snapshot times)
#  3) relaunch with auto-OK (= Restore): screenshot of the restored, still-unsaved project
# usage: tests/wine/recovery_kill9.sh <PCBPlotter.exe> <project.pcbp> [outdir]
set -u
EXE=$(readlink -f "$1"); PRJ=$(readlink -f "$2"); OUT=${3:-$(mktemp -d /tmp/pcbp_rec.XXXX)}
mkdir -p "$OUT"; cp "$PRJ" "$OUT/board.pcbp"; ORIG_SUM=$(md5sum < "$OUT/board.pcbp")
export WINEDEBUG=-all WINEPREFIX=${WINEPREFIX:-$OUT/pfx}
REC="$OUT/rec"; mkdir -p "$REC"; export PCBP_RECOVERY_DIR="Z:${REC//\//\\}"
DN=$((100 + RANDOM % 800)); Xvfb :$DN -screen 0 1600x900x24 >/dev/null 2>&1 & XP=$!
sleep 2; export DISPLAY=:$DN
# kill -9 only the PCBPlotter.exe of THIS wine prefix (other sessions may run their own)
kill9_mine() { for p in $(pgrep -x 'PCBPlotter.exe'); do tr '\0' '\n' < /proc/$p/environ 2>/dev/null | grep -qx "WINEPREFIX=$WINEPREFIX" && kill -9 $p; done; }
winpath() { echo "Z:${1//\//\\}"; }
PCBP_TEST_EDITS=5 timeout 120 wine "$EXE" "$(winpath "$OUT/board.pcbp")" > "$OUT/run1.log" 2>&1 &
sleep ${WAIT1:-30}
import -window root "$OUT/1_edited.png"
kill9_mine; sleep 2
echo "recovery files after kill -9:"; find "$REC" -type f | sed "s|$REC/||"
timeout 60 wine "$EXE" > "$OUT/run2.log" 2>&1 &
sleep ${WAIT2:-20}
import -window root "$OUT/2_recovery_dialog.png"
kill9_mine; sleep 2
PCBP_TEST_FORM_AUTOOK=4 timeout 60 wine "$EXE" > "$OUT/run3.log" 2>&1 &
sleep ${WAIT3:-25}
import -window root "$OUT/3_restored.png"
kill9_mine; WINEPREFIX=$WINEPREFIX wineserver -k 2>/dev/null; kill $XP 2>/dev/null; wait 2>/dev/null
RC=0
[ "$(md5sum < "$OUT/board.pcbp")" = "$ORIG_SUM" ] && echo "original project untouched: yes" || { echo "FAIL: original modified"; RC=1; }
ls "$REC"/*/snap*.pcbp >/dev/null 2>&1 && echo "snapshots: yes" || { echo "FAIL: no snapshot"; RC=1; }
echo "screenshots in $OUT"; exit $RC
