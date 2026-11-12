#!/bin/sh
# net_drop_test.sh: three players; the third leaves early (clean exit), comes back, and is then killed.
# The other two must keep running to the end: first told at once, then after the silence timeout.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
EXE=${SS_EXE:-$ROOT/build/dev32/ss2/ss2_dev32.exe}
ROM=${SS_ROM_PATH:-D:/01Claude/GBA/ss2-nov03-w6/baserom.gba}
PORT=47956
"$ROOT/build/relay/ss_relay.exe" $PORT > "$ROOT/build/relay/relay.log" 2>&1 &
RELAY=$!
sleep 1
export SS_NET=127.0.0.1:$PORT SS_NET_ROOM=drop$$ SS_NET_DELAY=4 SS_INPUT="420:0001,430:0000" SS_NET_TIMEOUT=3000
player() {      # player NUMBER FRAMES PACE
    mkdir -p "$ROOT/build/net$1"
    ( cd "$ROOT/build/net$1" && SS_NET_LOG=net$3.log SS_FRAMES=$2 SS_PACE=$4 exec "$EXE" "$ROM" > run$3.log 2>&1 ) &
}
player 1 3000 a 1; P1=$!
sleep 1
player 2 3000 a 1; P2=$!
sleep 1
player 3 900 a 1; P3=$!
wait $P3
sleep 3
player 3 100000 b 1; P3=$!
sleep 12
kill -9 $P3 2>/dev/null
wait $P1 $P2
kill $RELAY 2>/dev/null
for i in 1 2; do
    echo "--- player $i: $(wc -l < "$ROOT/build/net$i/neta.log") frames delivered"
    grep "^net:" "$ROOT/build/net$i/runa.log"
done
cat "$ROOT/build/relay/relay.log"
