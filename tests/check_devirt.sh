#!/bin/bash
# R23a: guards the v22 crash class. GCC devirtualizes calls on abstract interfaces with internal linkage
# (hand-declared COM/WinRT ABI structs inside `namespace {}`) to __cxa_pure_virtual, which is only a WEAK
# undefined reference under -static and resolves to 0: the exe then contains `call 0x100000000`
# (ImageBase-0x40000000 at run time) and dies with an execute-AV on real Windows (Wine hides it when the
# COM object is unavailable). Fails if any object or the linked exe still has such a call.
cd "$(dirname "$0")/.."
OD=x86_64-w64-mingw32-objdump
command -v $OD >/dev/null || { echo "check_devirt: SKIP (no mingw objdump)"; exit 0; }
bad=0
for o in build/mingw/obj/*.o; do
  [ -f "$o" ] || continue
  if $OD -dr "$o" | grep -q "REL32	__cxa_pure_virtual"; then echo "FAIL: $o calls __cxa_pure_virtual directly (devirtualized abstract call; give the interface external linkage)"; bad=1; fi
done
if [ -f build/mingw/PCBPlotter.exe ]; then
  n=$($OD -d build/mingw/PCBPlotter.exe | grep -cE "call +100000000 ")
  [ "$n" != 0 ] && { echo "FAIL: PCBPlotter.exe has $n call(s) to address 0 (unresolved weak symbol)"; bad=1; }
fi
[ $bad = 0 ] && echo "check_devirt: PASS"
exit $bad
