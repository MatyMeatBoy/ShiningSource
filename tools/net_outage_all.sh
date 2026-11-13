#!/bin/sh
# net_outage_all.sh PREFIX RUNS [NAME=value ...]: every case of tools/net_outage_test.sh, RUNS times each, and
# the rates. These sessions depend on timing, so one run of a case says little.
#
# The cases (CASES, "players:who:seconds ..."): two and three players; the last player or the host (1) cut off;
# for 3, 10 and 30 seconds (below and above the 15 seconds after which a silent player is given up).
# PARALLEL sessions run side by side (default 6), each with its own relay on UDP ports PORT, PORT+1, ...
# (default from 47970). The runs are kept in build/outage/PREFIX_<players>_<who>_<seconds>_<run>; the table is
# printed and written to build/outage/PREFIX.txt. NAME=value settings are passed on (SS_EXE=..., AFTER=...).
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PREFIX=$1 RUNS=$2
shift 2
CASES=${CASES:-2:2:3 2:2:10 2:2:30 2:1:3 2:1:10 2:1:30 3:3:3 3:3:10 3:3:30 3:1:3 3:1:10 3:1:30}
PARALLEL=${PARALLEL:-6} PORT=${PORT:-47970}
mkdir -p "$ROOT/build/outage"
running=0 slot=0 folders=
run=1
while [ $run -le "$RUNS" ]; do
    for case in $CASES; do
        players=${case%%:*} rest=${case#*:}
        who=${rest%%:*} seconds=${rest#*:}
        tag=${PREFIX}_${players}_${who}_${seconds}_$run
        folders="$folders $ROOT/build/outage/$tag"
        sh "$ROOT/tools/net_outage_test.sh" "$tag" $((PORT + slot)) "$players" "$who" "$seconds" "$@" > /dev/null 2>&1 &
        slot=$(( (slot + 1) % PARALLEL ))
        running=$((running + 1))
        if [ $running -ge "$PARALLEL" ]; then
            wait
            running=0
        fi
    done
    run=$((run + 1))
done
wait
for PYTHON in python3 python /c/msys64/ucrt64/bin/python3; do
    "$PYTHON" -c "" > /dev/null 2>&1 && break
done
"$PYTHON" "$ROOT/tools/net_outage_report.py" $folders | tee "$ROOT/build/outage/$PREFIX.txt"
