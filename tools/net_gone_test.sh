#!/bin/sh
# net_gone_test.sh TAG PORT PLAYERS WHO AT HOW [NAME=value ...]: the others go on after a player is gone.
#
# The session of tools/net_join_test.sh without a newcomer and without SS_NET_JOIN: PLAYERS (2 or 3) go from
# the title through the lobby into the first field (frame 23400), kill the enemy there (SS_INPUT_HUNT), answer
# the dialogue and walk to the way out (SS_INPUT_GOTO): map 12 area 0 -> 12/1 (about frame 25600) -> 12/2
# (32100) -> the town, map 0 (33800, 34850). Player WHO's program ends on the way, at about frame AT:
#   HOW=kill   the program is ended from outside, as one that crashes: no goodbye. The others give it up after
#              SS_NET_TIMEOUT (3000 here unless given: at eight times the speed that is 1440 frames) and keep
#              its character standing; for them it may still return.
#   HOW=quit   the program ends by itself at frame AT and says goodbye: it left for good.
#   HOW=none   nobody ends (the reference).
# WHO = 1 is the host: the others' session ends then, by design.
# AT about 24400: in the first field; 28000: in 12/1; 33000: in 12/2.
# tools/net_gone_report.py then says how far the others got: every change of map each of them made, whether
# they made the same ones, and where they ended.
#
# The whole run goes at FAST times the speed (8). Everything is written to build/gone/TAG: relay.log, case.txt
# and per program pN/run.log, msg.log, net.log, state.log. Several of these can run side by side with their own
# TAG and PORT. Never the public relay.
#
# Settings (environment, or NAME=value after HOW; those also reach the programs):
#   SS_EXE   the program (default: the dev32 build)     FRAMES  frames the players run (42000)
#   FAST     times the speed (8)                        LOSS LAG JITTER  a bad network besides
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ROM=${SS_ROM_PATH:-D:/01Claude/GBA/ss2-nov03-w6/baserom.gba}
TAG=$1 PORT=$2 PLAYERS=$3 WHO=$4 AT=$5 HOW=$6
shift 6
for setting in "$@"; do export "$setting"; done
EXE=${SS_EXE:-$ROOT/build/dev32/ss2/ss2_dev32.exe}
RELAY_EXE=${SS_RELAY_EXE:-$ROOT/build/relay/ss_relay.exe}
FRAMES=${FRAMES:-42000} FAST=${FAST:-8} FIELD=23400 TOWN=36000
OUT=$ROOT/build/gone/$TAG
rm -rf "$OUT"; mkdir -p "$OUT"
SS_RELAY_LOSS=${LOSS:-0} SS_RELAY_LAG=${LAG:-0} SS_RELAY_JITTER=${JITTER:-0} "$RELAY_EXE" "$PORT" > "$OUT/relay.log" 2>&1 &
RELAY=$!
PIDS=
trap 'kill $RELAY $PIDS 2>/dev/null' EXIT INT TERM
sleep 1
export SS_PACE=1 SS_PACE_FAST=999999999:$FAST SS_SKIP=0
export SS_NET=127.0.0.1:$PORT SS_NET_ROOM=gone$TAG SS_NET_DELAY=0 SS_NET_TIMEOUT=${SS_NET_TIMEOUT:-3000}
export SS_NET_MSG_LOG=msg.log SS_NET_LOG=net.log SS_STATE_LOG=state.log
# The buttons, as in tools/net_join_test.sh.
{
    tr ',' '\n' < "$ROOT/tools/tests/net_outage_input.txt" | awk -F: -v field="$FIELD" '$1 + 0 < field'
    awk -v from="$FIELD" -v town="$TOWN" -v to="$FRAMES" 'BEGIN {
        print from ":0000"; print from + 10 ":0200"; print from + 16 ":0000"; print from + 40 ":0200"; print from + 46 ":0000"
        print from + 70 ":0200"; print from + 76 ":0000"
        for (f = from + 100; f < town; f += 40) { print f ":0001"; print f + 6 ":0000" }
        split("0010 0080 0020 0040", way, " ")
        for (f = town; f < to; f += 75) { print f ":" way[1 + (n++ % 4)]; print f + 30 ":0000" } }'
} | tr '\n' ',' | sed 's/,$//' > "$OUT/input.txt"
export SS_INPUT_FILE=$(cygpath -m "$OUT/input.txt" 2>/dev/null || echo "$OUT/input.txt")
echo "tag $TAG players $PLAYERS who $WHO at $AT how $HOW frames $FRAMES fast $FAST" > "$OUT/case.txt"
i=1
while [ $i -le "$PLAYERS" ]; do
    mkdir -p "$OUT/p$i"
    frames=$FRAMES
    [ "$HOW" = quit ] && [ "$i" = "$WHO" ] && frames=$AT
    ( cd "$OUT/p$i" && SS_FRAMES=$frames SS_NET_PLAYERS=$PLAYERS SS_INPUT_HUNT=$((FIELD + 100)) \
        SS_INPUT_GOTO=25200:120:100:34000 exec "$EXE" "$ROM" > run.log 2>&1 ) &
    eval "PID$i=$!"
    [ "$HOW" = kill ] && [ "$i" = "$WHO" ] || PIDS="$PIDS $!"
    sleep "$(awk -v fast="$FAST" 'BEGIN { print 2 / fast }')"
    i=$((i + 1))
done
if [ "$HOW" = kill ]; then
    # ended without a goodbye, as a program that crashes: when its own state log says it is at frame AT
    while kill -0 "$(eval "echo \$PID$WHO")" 2>/dev/null; do
        at=$(tail -n 1 "$OUT/p$WHO/state.log" 2>/dev/null | awk '{ print $2 + 0 }')
        [ "${at:-0}" -ge "$AT" ] && break
        sleep 0.2
    done
    eval "kill \$PID$WHO" 2>/dev/null
fi
wait $PIDS
kill $RELAY 2>/dev/null
trap - EXIT INT TERM
# (on Windows "python3" may be a stub that only points to the store: take the first one that really runs)
for PYTHON in python3 python /c/msys64/ucrt64/bin/python3; do
    "$PYTHON" -c "" > /dev/null 2>&1 && break
done
"$PYTHON" "$ROOT/tools/net_gone_report.py" "$OUT" | tee "$OUT/report.txt"
