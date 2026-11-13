#!/bin/sh
# net_outage_map.sh TAG PORT PLAYERS WHO SECONDS WHEN [NAME=value ...]: a change of map while a player is away.
#
# tools/net_outage_test.sh with a session that leaves the first field: the host hands over the game's own
# "everybody changes map" (SS_NET_TEST_MSG, runtime/core/netlink.c), which takes every player to the castle
# gate; the script walks them down onto the world map, moves each player's hand to the first dungeon and
# chooses it. The game starts the dungeon when every player has chosen it, behind its barrier (every player
# sends a token and waits until it holds everybody's). There enemies appear, so with SS_INPUT_HUNT=frame the
# players fight (the hunt presses A, which also moves the opening dialogue on).
#
# WHO (1 = the host) is cut off for SECONDS, and WHEN says where in that:
#   before   three seconds into the field, before the change of map. The others go to the world map, choose,
#            and wait there for the missing player, as the game has it. When it is back it is given the change
#            of map, walks down and chooses too (its buttons come five seconds after its return).
#   chosen   on the world map, after WHO has chosen the dungeon and before the others have. With their choice
#            the barrier starts while WHO is away: the others wait for its token until it is back or given up.
#   dungeon  ten seconds after everybody has arrived in the dungeon (a plain outage, on another map).
#   none     nobody is cut off (WHO and SECONDS are not used).
# The report (tools/net_outage_report.py) then also says whether all players ended on the same map and
# whether a barrier token was left over. Settings as for net_outage_test.sh (AFTER, SS_EXE, MORE, ...).
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TAG=$1 PORT=$2 PLAYERS=$3 WHO=$4 SECONDS_OUT=$5 WHEN=$6
shift 6
F=${FIELD:-23400}
change=$((F + 300))
# stand still, walk down from the gate (the world map opens), move the hand to the dungeon, choose it
steps() {       # $1: frame of the walk down, $2: frame of the choice
    echo "$((F + 250)):0000 $1:0080 $(($1 + 32)):0000 $(($1 + 200)):0090 $(($1 + 220)):0000 $2:0001 $(($2 + 10)):0000"
}
down=$((F + 500))
all=$(steps $down $((down + 300)))
case $WHEN in
    before)
        before=3
        late=$((F + (before + SECONDS_OUT + 5) * 60))
        own=$(steps $late $((late + 300)))
        [ "$WHO" = 1 ] && all=$own      # the host's own message waits with it: everybody changes map on its return
        ;;
    chosen)
        before=16
        own=$all
        all=$(steps $down $((down + 600)))
        ;;
    dungeon)
        before=30
        own=$all
        ;;
    none)
        before=3 WHO=0 SECONDS_OUT=0
        own=$all
        ;;
    *)
        echo "WHEN is before, chosen, dungeon or none" >&2
        exit 2
        ;;
esac
# ROUTE="frame:buttons ...": more buttons for everybody after that (a way through the dungeon)
all="$all ${ROUTE:-}" own="$own ${ROUTE:-}"
if [ "$WHO" = 0 ] || [ "$WHO" = 9 ]; then
    sh "$ROOT/tools/net_outage_test.sh" "$TAG" "$PORT" "$PLAYERS" "$WHO" "$SECONDS_OUT" BEFORE=$before THEN="$all" \
        "1@SS_NET_TEST_MSG=$change:9F010100000000000000" "$@"
else
    sh "$ROOT/tools/net_outage_test.sh" "$TAG" "$PORT" "$PLAYERS" "$WHO" "$SECONDS_OUT" BEFORE=$before THEN="$all" \
        "THEN_$WHO=$own" "1@SS_NET_TEST_MSG=$change:9F010100000000000000" "$@"
fi
