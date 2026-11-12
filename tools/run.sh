#!/bin/sh
# run.sh FRAMES [INPUT] : headless run of the dev32 build with a crash translated to function names.
# ROM path: SS_ROM_PATH. Writes build/run.log and, with a third argument, build/shot.png of that frame.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TC=${SHININGSOURCE_LLVM_MINGW:-D:/01Claude/toolchain/llvm-mingw-20261006-ucrt-x86_64}
EXE=$ROOT/build/dev32/ss2/ss2_dev32.exe
cd "$ROOT/build" || exit 1
SS_FRAMES=$1 SS_INPUT=${2:-} SS_DUMP=${3:-0} "$EXE" "${SS_ROM_PATH:-D:/01Claude/GBA/ss2-nov03-w6/baserom.gba}" > run.log 2>&1
grep -v "^frame" run.log | grep -v "^\[stub\|code pointers\|memory mapped" | head -4 | cut -c1-200
tail -1 run.log | cut -c1-100
for a in $(grep -oE "eip [0-9A-F]{8}" run.log | head -1 | cut -d' ' -f2) $(grep "stack words" run.log | head -1 | grep -oE " 01[89A-F][0-9A-F]{5}" | head -7); do
    "$TC/bin/llvm-addr2line.exe" -f -e "$EXE" 0x$a 2>/dev/null | head -2 | tr '\n' ' ' | sed "s#.*ShiningSource/##; s#D:/01Claude/GBA/##"; echo
done
[ "${3:-0}" != "0" ] && python3 "$ROOT/tools/raw_to_png.py" ss_frame.raw shot.png
exit 0
