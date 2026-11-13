#!/bin/sh
# net_exit_test.sh TAG PORT PLAYERS CASE [NAME=value ...]: a dungeon level online, its fights and its exit.
#
# Checks what the scripted sessions of the first field never reach: enemies that fight back, items that drop
# and are picked up, a level's exit zone that asks for every player, and what a player that is gone or cut off
# does to all of that. PLAYERS (2 or 3) play one session on this machine through a relay of their own on UDP
# port PORT, headless, with the direct transport and the message channel:
#   - tools/tests/net_outage_input.txt takes them from the title through the lobby into the first field
#     (frame 23400), where each chooses a weapon (L three times);
#   - the host hands over the game's own "everybody changes map" (SS_NET_TEST_MSG), which takes all to the
#     castle gate; the script walks them down onto the world map, moves the hand to the first dungeon and
#     chooses it (as tools/net_outage_map.sh, but later: a player that is not the host counts its frames up
#     to 350 ahead of the host, and every step has to come after that player's own arrival at the gate).
#     They arrive in map 1 area 0 at about frame 24700 of the host;
#   - there the opening dialogue is answered and the players hunt (SS_INPUT_HUNT) round the walls
#     (SS_INPUT_PATH, runtime/core/frame.c): the enemies near the entrance and those that appear after them;
#   - from frame EXIT_AT on, whenever no enemy lives, they walk to the level's exit (SS_INPUT_EXIT). The exit
#     takes them on when every player stands in its zone and has confirmed; then the same goes on in the next
#     level (map 1 area 1, ...).
# CASE says what happens on the way:
#   none             nothing (the reference)
#   kill:WHO:AT      player WHO's program is ended from outside at about frame AT, as one that crashes. The
#                    others give it up after SS_NET_TIMEOUT (3000 here unless given) and keep its character.
#   quit:WHO:AT      player WHO's program ends by itself at frame AT and says goodbye.
#   idle:WHO:AT      player WHO stops pressing buttons at frame AT (its character stands and is struck): with
#                    IDLE_ALL=1 every player does, which is the party's defeat.
#   outage:WHO:SECONDS:BEFORE   the relay cuts player WHO off for SECONDS (SS_RELAY_OUTAGE), BEFORE seconds
#                    after frame REAL. Give REAL=24700 with it: from that frame the session runs in real time,
#                    so seconds are the game's seconds.
# WHO = 1 is the host: the others' session ends then, by design. AT about 24900: during the opening dialogue;
# 25700: in the first fight (a player's own frames; see above).
# KILL=WHO:AT besides any CASE ends player WHO's program as kill:WHO:AT does (a defeat or an outage with a
# player gone).
# tools/net_exit_report.py then says what became of it: the fights (messages by kind), every change of map
# of each player, whether those left passed the exit together, the enemies of the players held against each
# other at the end, and who was struck by what.
#
# The run goes at FAST times the speed (8) up to frame REAL (default: to the end). Everything is written to
# build/exits/TAG: relay.log, case.txt and per program pN/run.log, msg.log, net.log, state.log, enemy.log,
# scene.log. Several of these can run side by side with their own TAG and PORT. Never the public relay.
#
# Settings (environment, or NAME=value after CASE; those also reach the programs):
#   SS_EXE   the program (default: the dev32 build)     FRAMES   frames the players run (30000)
#   FAST     times the speed (8)                        REAL     frame from which the speed is the game's own
#   EXIT_AT  frame from which the players go for the exit (24800)
#   HUNT_AT  frame from which they hunt (24700)         LOSS LAG JITTER  a bad network besides
#   STAY=N   player N alone never goes for the exit (it stands where the last enemy fell)
#   N@NAME=value   a setting for player N alone
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ROM=${SS_ROM_PATH:-D:/01Claude/GBA/ss2-nov03-w6/baserom.gba}
TAG=$1 PORT=$2 PLAYERS=$3 CASE=$4
shift 4
ONLY=
for setting in "$@"; do
    case $setting in
        [1-4]@*) ONLY="$ONLY $setting" ;;
        *) export "$setting" ;;
    esac
done
EXE=${SS_EXE:-$ROOT/build/dev32/ss2/ss2_dev32.exe}
RELAY_EXE=${SS_RELAY_EXE:-$ROOT/build/relay/ss_relay.exe}
FRAMES=${FRAMES:-30000} FAST=${FAST:-8} FIELD=23400
REAL=${REAL:-999999999} EXIT_AT=${EXIT_AT:-24800} HUNT_AT=${HUNT_AT:-24700} STAY=${STAY:-0}
HOW=${CASE%%:*} WHO=0 AT=0 OUTAGE=
KILL=${KILL:-0:0}
KILL_WHO=${KILL%%:*} KILL_AT=${KILL#*:}
rest=${CASE#*:}
case $HOW in
    none) ;;
    kill) KILL_WHO=${rest%%:*} KILL_AT=${rest#*:} WHO=$KILL_WHO AT=$KILL_AT ;;
    quit|idle) WHO=${rest%%:*} AT=${rest#*:} ;;
    outage)
        WHO=${rest%%:*} rest=${rest#*:}
        length=${rest%%:*} before=${rest#*:}
        # the relay counts from the moment the seat was taken, about frame 500 of that player
        OUTAGE=$WHO:$(( (REAL - 500) * 1000 / (60 * FAST) + before * 1000 )):$((length * 1000))
        AT=$((REAL + before * 60))
        ;;
    *) echo "CASE is none, kill:WHO:AT, quit:WHO:AT, idle:WHO:AT or outage:WHO:SECONDS:BEFORE" >&2; exit 2 ;;
esac
OUT=$ROOT/build/exits/$TAG
rm -rf "$OUT"; mkdir -p "$OUT"
SS_RELAY_OUTAGE=$OUTAGE SS_RELAY_LOSS=${LOSS:-0} SS_RELAY_LAG=${LAG:-0} SS_RELAY_JITTER=${JITTER:-0} "$RELAY_EXE" "$PORT" > "$OUT/relay.log" 2>&1 &
RELAY=$!
PIDS=
trap 'kill $RELAY $PIDS 2>/dev/null' EXIT INT TERM
sleep 1
export SS_PACE=1 SS_PACE_FAST=$REAL:$FAST SS_SKIP=0
export SS_NET=127.0.0.1:$PORT SS_NET_ROOM=exit$TAG SS_NET_DELAY=0 SS_NET_TIMEOUT=${SS_NET_TIMEOUT:-3000} SS_NET_PLAYERS=$PLAYERS
export SS_NET_MSG_LOG=msg.log SS_NET_MSG_DUMP=1 SS_NET_LOG=net.log SS_STATE_LOG=state.log SS_ENEMY_LOG=enemy.log SS_SCENE_LOG=scene.log
export SS_INPUT_PATH=1
# The buttons: the way into the field; L three times chooses the weapon; stand still, walk down from the gate
# (the world map opens), move the hand to the dungeon, choose it; then A every 40 frames (a dialogue that no
# enemy is alive for). $1: the frame from which nothing is pressed any more (idle), 0 for never.
script() {
    tr ',' '\n' < "$ROOT/tools/tests/net_outage_input.txt" | awk -F: -v field="$FIELD" '$1 + 0 < field'
    awk -v from="$FIELD" -v to="$FRAMES" -v idle="$1" 'BEGIN {
        print from ":0000"; print from + 10 ":0200"; print from + 16 ":0000"; print from + 40 ":0200"; print from + 46 ":0000"
        print from + 70 ":0200"; print from + 76 ":0000"
        print from + 900 ":0080"; print from + 932 ":0000"; print from + 1100 ":0090"; print from + 1120 ":0000"
        print from + 1200 ":0001"; print from + 1210 ":0000"
        if (idle + 0 == 0) idle = to
        for (f = from + 1600; f < idle; f += 40) { print f ":0001"; print f + 6 ":0000" }
        print idle ":0000" }'
}
script 0 | tr '\n' ',' | sed 's/,$//' > "$OUT/input.txt"
script "$AT" | tr '\n' ',' | sed 's/,$//' > "$OUT/input_idle.txt"
echo "tag $TAG players $PLAYERS how $HOW who $WHO at $AT frames $FRAMES fast $FAST real $REAL stay $STAY kill $KILL_WHO idle_all ${IDLE_ALL:-0}" > "$OUT/case.txt"
i=1
while [ $i -le "$PLAYERS" ]; do
    mkdir -p "$OUT/p$i"
    frames=$FRAMES input=../input.txt hunt=$HUNT_AT exit_at=$EXIT_AT
    [ "$HOW" = quit ] && [ "$i" = "$WHO" ] && frames=$AT
    if [ "$HOW" = idle ] && { [ "$i" = "$WHO" ] || [ "${IDLE_ALL:-0}" = 1 ]; }; then
        input=../input_idle.txt hunt=0 exit_at=0
    fi
    [ "$i" = "$STAY" ] && exit_at=0
    ( cd "$OUT/p$i" || exit 1
      [ "$i" = 1 ] && export SS_NET_TEST_MSG=$((FIELD + 300)):9F010100000000000000
      [ "$hunt" != 0 ] && export SS_INPUT_HUNT=$hunt
      [ "$exit_at" != 0 ] && export SS_INPUT_EXIT=$exit_at
      # (an idle player hunts up to the frame it goes idle at: the hunt has no end of its own)
      [ "$hunt" = 0 ] && export SS_INPUT_HUNT=$HUNT_AT SS_INPUT_HUNT_UNTIL=$AT
      for setting in $ONLY; do [ "${setting%%@*}" = "$i" ] && export "${setting#*@}"; done
      SS_FRAMES=$frames SS_INPUT_FILE=$input exec "$EXE" "$ROM" > run.log 2>&1 ) &
    eval "PID$i=$!"
    [ "$i" = "$KILL_WHO" ] || PIDS="$PIDS $!"
    sleep "$(awk -v fast="$FAST" 'BEGIN { print 1 / fast }')"
    i=$((i + 1))
done
if [ "$KILL_WHO" != 0 ]; then
    # ended without a goodbye, as a program that crashes: when its own state log says it is at frame KILL_AT
    while kill -0 "$(eval "echo \$PID$KILL_WHO")" 2>/dev/null; do
        at=$(tail -n 1 "$OUT/p$KILL_WHO/state.log" 2>/dev/null | awk '{ print $2 + 0 }')
        [ "${at:-0}" -ge "$KILL_AT" ] && break
        sleep 0.2
    done
    eval "kill \$PID$KILL_WHO" 2>/dev/null
fi
wait $PIDS
kill $RELAY 2>/dev/null
trap - EXIT INT TERM
# (on Windows "python3" may be a stub that only points to the store: take the first one that really runs)
for PYTHON in python3 python /c/msys64/ucrt64/bin/python3; do
    "$PYTHON" -c "" > /dev/null 2>&1 && break
done
"$PYTHON" "$ROOT/tools/net_exit_report.py" "$OUT" | tee "$OUT/report.txt"
