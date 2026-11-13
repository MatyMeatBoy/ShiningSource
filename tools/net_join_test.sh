#!/bin/sh
# net_join_test.sh TAG PORT PLAYERS SECONDS [NAME=value ...]: a program joins a game in progress (SS_NET_JOIN=1).
#
# PLAYERS (2 or 3; the game's lobby does not open for one) start one session on this machine through a relay of
# their own on UDP port PORT, headless, with the direct transport and the message channel. SECONDS later one
# more program starts in the same room: the newcomer. The scripted input takes the players from the title
# through the game's lobby into the first field (tools/tests/net_outage_input.txt, to frame 23400); then they
# choose a weapon, kill the enemy there (SS_INPUT_HUNT), answer the dialogue, and walk to the way out
# (SS_INPUT_GOTO). That takes them through three changes of map: map 12 area 0 -> 12/1 (about frame 25600) ->
# 12/2 (32100) -> the town, map 0 (33800, 34850). At the end they are given directions only (walk half a
# second, stand three quarters of one, right, down, left, up in turn). The newcomer presses the same buttons:
# the script's start for its own way through title and lobby, then the others' script at the frame they are
# at (SS_INPUT_SHIFT), so it answers the same dialogues and walks when they walk. It is let in at the first
# set-up of a map after its game is ready (which takes it about 2800 frames). At eight times the speed, with
# SECONDS up to about 27 that is the set-up of the session's first map (12/0, before anybody plays), from
# about 28 to 46 the change to 12/1, from about 47 to 60 the change to 12/2; later nobody is let in (the
# session only changes map inside the town after that; docs/JOIN_IN_PROGRESS.md).
# tools/net_join_report.py then says what became of it: let in, in the same map as the others, the same
# messages in the same order, seen moving by the others and seeing them move.
#
# The whole run goes at FAST times the speed (8; the game counts its waits in frames, the relay is on this
# machine). Everything is written to build/join/TAG: relay.log, case.txt and per program pN/run.log, msg.log,
# net.log, state.log, enemy.log. Several of these can run side by side with their own TAG and PORT. Never the
# public relay.
#
# Settings (environment, or NAME=value after SECONDS; those also reach the programs):
#   SS_EXE     the program (default: the dev32 build)       FRAMES  frames the players run (42000)
#   FAST       times the speed (8)                          JOIN_SETTING  the newcomer's and the players' SS_NET_JOIN (1)
#   LOSS LAG JITTER  a bad network besides (relay/ss_relay.c)
#   DUMP       a frame of the players at which every program writes its picture (pN/ss_frame.raw; the newcomer
#              at its own frame of about that moment); tools/raw_to_png.py makes a PNG of it
#   LEAVE=N:S  player N's program (2 or 3, not the host) is ended S seconds after the start, without a goodbye.
#              The others keep its character standing; the relay frees its seat after 30 seconds of silence,
#              and a newcomer that starts after that is given that seat: it takes the place of the player that
#              left instead of a seat of its own. (With SS_NET_TIMEOUT=3000 the others give it up after 3 s.)
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ROM=${SS_ROM_PATH:-D:/01Claude/GBA/ss2-nov03-w6/baserom.gba}
TAG=$1 PORT=$2 PLAYERS=$3 JOIN=$4
shift 4
for setting in "$@"; do export "$setting"; done
EXE=${SS_EXE:-$ROOT/build/dev32/ss2/ss2_dev32.exe}
RELAY_EXE=${SS_RELAY_EXE:-$ROOT/build/relay/ss_relay.exe}
FRAMES=${FRAMES:-42000} FAST=${FAST:-8} FIELD=23400 TOWN=36000
OUT=$ROOT/build/join/$TAG
rm -rf "$OUT"; mkdir -p "$OUT"
# how many frames the players are ahead of the newcomer, and how long the newcomer runs: to the players' end
OFFSET=$(awk -v s="$JOIN" -v fast="$FAST" 'BEGIN { printf "%d", s * 60 * fast }')
JOIN_FRAMES=$((FRAMES - OFFSET - 60 * FAST))
SS_RELAY_LOSS=${LOSS:-0} SS_RELAY_LAG=${LAG:-0} SS_RELAY_JITTER=${JITTER:-0} "$RELAY_EXE" "$PORT" > "$OUT/relay.log" 2>&1 &
RELAY=$!
PIDS=
trap 'kill $RELAY $PIDS 2>/dev/null' EXIT INT TERM
sleep 1
export SS_PACE=1 SS_PACE_FAST=999999999:$FAST SS_SKIP=0
export SS_NET=127.0.0.1:$PORT SS_NET_ROOM=join$TAG SS_NET_DELAY=0 SS_NET_JOIN=${JOIN_SETTING:-1}
export SS_NET_MSG_LOG=msg.log SS_ENEMY_LOG=enemy.log SS_NET_LOG=net.log SS_STATE_LOG=state.log
# The buttons (see above). L three times chooses the weapon; A every 40 frames answers the dialogues until the
# town; there, directions only.
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
LEAVE=${LEAVE:-0}
echo "tag $TAG players $PLAYERS join $JOIN frames $FRAMES jframes $JOIN_FRAMES fast $FAST leave ${LEAVE%%:*}" > "$OUT/case.txt"
i=1
while [ $i -le "$PLAYERS" ]; do
    mkdir -p "$OUT/p$i"
    ( cd "$OUT/p$i" && SS_FRAMES=$FRAMES SS_NET_PLAYERS=$PLAYERS SS_INPUT_HUNT=$((FIELD + 100)) SS_DUMP=${DUMP:-0} \
        SS_INPUT_GOTO=25200:120:100:34000 exec "$EXE" "$ROM" > run.log 2>&1 ) &
    PIDS="$PIDS $!"
    eval "PID$i=$!"
    sleep "$(awk -v fast="$FAST" 'BEGIN { print 2 / fast }')"
    i=$((i + 1))
done
if [ "$LEAVE" != 0 ]; then
    # one of the players is ended without a goodbye, as a program that crashes: the others keep its character
    sleep "${LEAVE#*:}"
    eval "kill \$PID${LEAVE%%:*}" 2>/dev/null
    sleep "$(awk -v join="$JOIN" -v left="${LEAVE#*:}" 'BEGIN { print (join > left ? join - left : 0) }')"
else
    sleep "$JOIN"
fi
i=$((PLAYERS + 1))
mkdir -p "$OUT/p$i"
( cd "$OUT/p$i" && SS_FRAMES=$JOIN_FRAMES SS_INPUT_SHIFT=3000:$OFFSET SS_INPUT_HUNT=3000 SS_DUMP=$((${DUMP:-0} > OFFSET ? ${DUMP:-0} - OFFSET : 0)) \
    SS_INPUT_GOTO=$((25200 > OFFSET + 3000 ? 25200 - OFFSET : 3000)):120:100:$((34000 - OFFSET)) exec "$EXE" "$ROM" > run.log 2>&1 ) &
PIDS="$PIDS $!"
wait $PIDS
kill $RELAY 2>/dev/null
trap - EXIT INT TERM
# (on Windows "python3" may be a stub that only points to the store: take the first one that really runs)
for PYTHON in python3 python /c/msys64/ucrt64/bin/python3; do
    "$PYTHON" -c "" > /dev/null 2>&1 && break
done
"$PYTHON" "$ROOT/tools/net_join_report.py" "$OUT" | tee "$OUT/report.txt"
