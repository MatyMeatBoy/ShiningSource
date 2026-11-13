#!/bin/sh
# net_town_test.sh TAG PORT ROUTE1 ROUTE2 [ROUTE3 [ROUTE4]] [NAME=value ...]: the shops of the town, online.
#
# Checks what players reported as "a crash when opening a shop in multiplayer": as many players as routes
# are named play one session on this machine through a relay of their own on UDP port PORT
# (tools/net_outage_test.sh with nobody cut off: headless, real time, direct transport, message channel).
# In the first field the host hands over the game's own "everybody changes map" (SS_NET_TEST_MSG), which
# takes every player to the square of the town; 200 frames later player N starts ROUTE N of
# tools/tests/town_routes.txt: into the hall of the shops, to a counter, trade, leave. So one player can
# have a shop open while another trades at the next counter, opens the game's menu, or leaves the hall and
# comes back (routes item_stay, weapon, menus, come_go, ...).
# tools/town_report.py then says for each player whether its route held (room, bag, money, held, free, no
# crash, no stub reached); the report of net_outage_test.sh says whether the session itself stayed whole.
# Everything is written to build/outage/TAG (pN/run.log, pN/where.log, ...). Settings as for
# net_outage_test.sh, e.g. SS_EXE=.../ss2_win64.exe, SS_TEST_RENDER=427 SS_VIEW=427 (every frame drawn in
# a wide view), N@SS_DUMP=frame (player N's picture of that frame). Never the public relay.
# Returns 0 when every player's route holds.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TAG=$1 PORT=$2
shift 2
ROUTES=$ROOT/tools/tests/town_routes.txt
F=${FIELD:-23400}
change=$((F + 300)) start=$((F + 500))
players=0 longest=0 settings=
for word in "$@"; do
    case $word in
        *=*) settings="$settings $word" ;;
        *)  players=$((players + 1))
            line=$(awk -v name="$word" '!/^#/ && $1 == name' "$ROUTES")
            [ -n "$line" ] || { echo "no route $word in $ROUTES" >&2; exit 2; }
            last=$(echo "$line" | tr ' ' '\n' | grep ":" | tail -1 | cut -d: -f1)
            [ "$last" -gt "$longest" ] && longest=$last
            eval "route$players=\$word"
            eval "checks$players=\$(echo \"\$line\" | tr ' ' '\\n' | grep '=' | tr '\\n' ' ')" ;;
    esac
done
[ "$players" -ge 2 ] || { echo "name a route for each of 2 to 4 players" >&2; exit 2; }
after=$(( (200 + longest + 300) / 60 + 1 ))
frames=$(( F + (3 + after) * 60 ))
# each player: stand still before the change of map, its route, and no more walking until the end
i=1
while [ $i -le "$players" ]; do
    eval "name=\$route$i"
    then=$(awk -v name="$name" '!/^#/ && $1 == name' "$ROUTES" | tr ' ' '\n' | grep ":" |
        awk -F: -v start="$start" -v first="$((F + 250))" -v end="$frames" \
            'BEGIN { printf "%d:0000", first } { printf " %d:%s", $1 + start, $2 } END { printf " %d:0000", end }')
    eval "then$i=\$then"
    i=$((i + 1))
done
set --
i=2
while [ $i -le "$players" ]; do
    eval "set -- \"\$@\" \"THEN_$i=\$then$i\""
    i=$((i + 1))
done
# shellcheck disable=SC2086
sh "$ROOT/tools/net_outage_test.sh" "$TAG" "$PORT" "$players" 0 0 BEFORE=3 AFTER=$after THEN="$then1" "$@" \
    SS_WHERE_LOG=where.log "1@SS_NET_TEST_MSG=$change:9F010100000000000000" $settings | grep -v "^  place"
for PYTHON in python3 python /c/msys64/ucrt64/bin/python3; do
    "$PYTHON" -c "" > /dev/null 2>&1 && break
done
failed=0
i=1
while [ $i -le "$players" ]; do
    eval "name=\$route$i checks=\$checks$i"
    # shellcheck disable=SC2086
    "$PYTHON" "$ROOT/tools/town_report.py" "$TAG player $i $name" "$ROOT/build/outage/$TAG/p$i" "$frames" $checks || failed=1
    i=$((i + 1))
done
exit $failed
