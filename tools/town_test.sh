#!/bin/sh
# town_test.sh [dev32|win64] [WIDTH] [ROUTE ...]: one player walks to the counters of the town and uses them.
#
# Checks that the shops and the game's menus open, trade and close in a single-player game: the item shop
# (it crashed until 0.4 whenever it was opened), the weapon and armour shops, the blacksmith, the appraiser,
# the storage, and the menu behind Start. Each route of tools/tests/town_routes.txt (all of them, or the ones
# named) is one headless run:
#   - tools/tests/town_input.txt takes a new character from the title into the first field (frame 8960);
#   - SS_TEST_WARP changes map to the town at frame 9000 (the square behind the gate), with the game's own
#     steps for a change of map, so the field need not be played to its end;
#   - from frame 9200 the route's buttons walk into the hall of the shops, to a counter, and trade;
#   - tools/town_report.py reads SS_WHERE_LOG and the program's output: the room, the bag, the money, whether
#     the player was held and is free again, no crash, no stub reached.
# WIDTH (240 the console's picture, 427 a wide view; default 240) is the width every frame is drawn at
# (SS_TEST_RENDER), so the picture's code runs too although no window is open.
# Everything is written to build/town/BUILD-WIDTH/ROUTE. ROM: SS_ROM_PATH. SS_EXE: another program.
# DUMP=frame (counted from the route's frame 0) also writes that frame's picture (ss_frame.raw, never kept in
# the repository). Returns 0 when every route holds.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ROM=${SS_ROM_PATH:-D:/01Claude/GBA/ss2-nov03-w6/baserom.gba}
BUILD=dev32 WIDTH=240
case ${1:-} in dev32|win64) BUILD=$1; shift ;; esac
case ${1:-} in [0-9]*) WIDTH=$1; shift ;; esac
EXE=${SS_EXE:-$ROOT/build/$BUILD/ss2/ss2_$BUILD.exe}
ROUTES=$ROOT/tools/tests/town_routes.txt
WARP=9000 START=9200
for PYTHON in python3 python /c/msys64/ucrt64/bin/python3; do
    "$PYTHON" -c "" > /dev/null 2>&1 && break
done
[ $# -gt 0 ] || set -- $(awk '!/^#/ && NF { print $1 }' "$ROUTES")
failed=0
for route in "$@"; do
    line=$(awk -v name="$route" '!/^#/ && $1 == name' "$ROUTES")
    if [ -z "$line" ]; then
        echo "no route $route in $ROUTES" >&2
        failed=1
        continue
    fi
    out=$ROOT/build/town/$BUILD-$WIDTH/$route
    rm -rf "$out"; mkdir -p "$out"
    checks=$(echo "$line" | tr ' ' '\n' | grep "=" | tr '\n' ' ')
    last=$(echo "$line" | tr ' ' '\n' | grep ":" | tail -1 | cut -d: -f1)
    frames=$((START + last + 300))
    { cat "$ROOT/tools/tests/town_input.txt"
      echo "$line" | tr ' ' '\n' | grep ":" | awk -F: -v start="$START" '{ printf ",%d:%s", $1 + start, $2 }'
    } > "$out/input.txt"
    ( cd "$out" || exit 1
      [ -n "${DUMP:-}" ] && export SS_DUMP=$((START + DUMP))
      SS_SKIP=0 SS_FRAMES=$frames SS_INPUT_FILE=input.txt SS_TEST_WARP=$WARP:0:0 SS_WHERE_LOG=where.log \
          SS_TEST_RENDER=$WIDTH SS_VIEW=$WIDTH "$EXE" "$ROM" > run.log 2>&1 )
    # shellcheck disable=SC2086
    "$PYTHON" "$ROOT/tools/town_report.py" "$BUILD $WIDTH $route" "$out" "$frames" $checks || failed=1
done
exit $failed
