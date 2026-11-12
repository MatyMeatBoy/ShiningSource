#!/bin/sh
# net_test.sh PLAYERS FRAMES [DELAY] [INPUT] [DUMP_FRAME] [LOSS LAG JITTER] [win64|dev32] [pace]: headless online test on this machine.
# Starts the relay, runs PLAYERS copies of the dev32 build in one room and compares what each was given.
# Every copy logs one line per delivered frame (label, players, hash); frames delivered to several copies with
# the same players must have the same hash.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
EXE=$ROOT/build/dev32/ss2/ss2_dev32.exe
[ "${10:-}" = pace ] && export SS_PACE=1
[ "${9:-}" = win64 ] && EXE=$ROOT/build/win64/ss2/ss2_win64.exe
ROM=${SS_ROM_PATH:-D:/01Claude/GBA/ss2-nov03-w6/baserom.gba}
PLAYERS=$1
FRAMES=$2
PORT=47955
SS_RELAY_LOSS=${6:-0} SS_RELAY_LAG=${7:-0} SS_RELAY_JITTER=${8:-0} "$ROOT/build/relay/ss_relay.exe" $PORT > "$ROOT/build/relay/relay.log" 2>&1 &
RELAY=$!
sleep 1
export SS_NET=127.0.0.1:$PORT SS_NET_ROOM=test$$ SS_NET_DELAY=${3:-4} SS_FRAMES=$FRAMES SS_INPUT=${4:-} SS_NET_PLAYERS=$PLAYERS SS_DUMP=${5:-0}
i=1
while [ $i -le "$PLAYERS" ]; do
    mkdir -p "$ROOT/build/net$i"
    ( cd "$ROOT/build/net$i" && SS_NET_LOG=net.log "$EXE" "$ROM" > run.log 2>&1 ) &
    eval "PID$i=$!"
    sleep ${SS_STAGGER:-1}
    i=$((i + 1))
done
i=1
while [ $i -le "$PLAYERS" ]; do
    eval "wait \$PID$i"
    i=$((i + 1))
done
kill $RELAY 2>/dev/null
i=1
while [ $i -le "$PLAYERS" ]; do
    echo "--- player $i: $(wc -l < "$ROOT/build/net$i/net.log") frames delivered"
    grep "^net:" "$ROOT/build/net$i/run.log" | head -12
    tail -1 "$ROOT/build/net$i/run.log" | cut -c1-90
    i=$((i + 1))
done
python3 "$ROOT/tools/net_compare.py" "$ROOT"/build/net[1-$PLAYERS]/net.log
