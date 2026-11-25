#!/bin/sh
# ShiningSource GA36 - Diagnostico: one automatic run, no buttons needed. Writes shiningsourcega36/diagnostico-NNN/.
# POSIX sh, busybox-safe. Every case runs in a fresh work folder (tmpfs when possible) under a watchdog
# (TERM, then KILL). The user's save is never written: it is only copied into the save test's scratch folder.
# Child mode (internal): "$0" --child sdl|headless  runs one case in the current folder.
self=$0
if [ "${1:-}" = --child ]; then
    # ga36-env.sh needs `game` (the shiningsourcega36 folder): the parent exports it as DIAG_GAME
    game=${DIAG_GAME:-}
    [ -n "$game" ] && [ -d "$game" ] || { echo "diagnostico: DIAG_GAME is not the game folder: '$game'" >&2; exit 121; }
    . "$DIAG_ENVSH" || exit 120
    # the case's own variables win over ga36-env.sh
    if [ -r ./case.env ]; then
        while IFS= read -r line; do [ -n "$line" ] && export "$line"; done < ./case.env
    fi
    if [ "$2" = headless ] && [ -x "$game/ss2_ga36_headless" ]; then
        # through the bundled loader, as ss_run: the binary's own interpreter path is relative to the game folder
        unset LD_PRELOAD
        exec "$game/libarmhf/ld-linux-armhf.so.3" --library-path "$game/libarmhf" "$game/ss2_ga36_headless" --game "$rom"
    fi
    ss_run
    exit $?
fi

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
self="$here/$(basename -- "$0")"   # absolute: each case runs from its own folder
game=${DIAG_GAME:-}
for g in "$here/shiningsourcega36" "$here" "$here/../shiningsourcega36" "$here/.."; do
    [ -z "$game" ] && [ -r "$g/ga36-env.sh" ] && game=$(CDPATH= cd -- "$g" && pwd)
done
[ -n "$game" ] || game="$here/shiningsourcega36"
envsh=${DIAG_ENVSH:-$game/ga36-env.sh}
export DIAG_ENVSH="$envsh"
export DIAG_GAME="$game"   # each case re-runs this script with --child: it needs the folder too
n=1
while [ -e "$game/diagnostico-$(printf %03d $n)" ]; do n=$((n + 1)); done
out="$game/diagnostico-$(printf %03d $n)"
mkdir -p "$out" || exit 1
status="$out/status.txt"
: > "$status"
log() { printf '%s\n' "$*" >> "$status"; }
up() { cut -d' ' -f1 /proc/uptime 2>/dev/null || echo 0; }
have() { command -v "$1" >/dev/null 2>&1; }
TOTAL=25
say() { ( printf '\rDiagnostico %s/%s %s          \n' "$1" "$TOTAL" "$2" > /dev/tty0 ) 2>/dev/null & }
log "START ShiningSource GA36 diagnostico v1 uptime $(up)"
log "GAME $game"
[ -r "$envsh" ] || log "MISSING ga36-env.sh"

findfile() { for d in "$here/diagnostic" "$game/diagnostic" "$here" "$game"; do [ -r "$d/$1" ] && { echo "$d/$1"; return; }; done; }
play=$(findfile play_input.txt); town=$(findfile town_input.txt)
[ -n "$play" ] || log "MISSING play_input.txt"
headless=no; [ -x "$game/ss2_ga36_headless" ] && headless=yes
log "HEADLESS $headless"

# work folders: tmpfs when there is room
ram=
for d in /dev/shm /tmp /run; do
    [ -w "$d" ] || continue
    awk -v d="$d" '$2 == d && $3 == "tmpfs" { ok = 1 } END { exit !ok }' /proc/mounts 2>/dev/null || continue
    free=$(df -k "$d" 2>/dev/null | awk 'NR == 2 { print $4 }')
    [ "${free:-0}" -gt 65536 ] && { ram="$d/ssdiag.$$"; break; }
done
[ -n "$ram" ] || ram="$out/work"
mkdir -p "$ram" || ram="$out/work"
log "WORK $ram"

governor_saved=
cleanup() {
    for p in $capture_pids; do kill "$p" 2>/dev/null; done
    if [ -n "$governor_saved" ]; then
        for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo "$governor_saved" > "$g" 2>/dev/null; done
    fi
    [ -f "$out/fb0-before.raw" ] && cat "$out/fb0-before.raw" > /dev/fb0 2>/dev/null
    case "$ram" in */ssdiag.*) rm -rf "$ram" ;; esac
    sync
}
trap 'log "INTERRUPTED"; cleanup; exit 1' INT TERM HUP

# ---- phase 0: census ----
say 0 sistema
c="$out/census"; mkdir -p "$c/input"
{
    echo "== uname"; uname -a
    echo "== uptime"; cat /proc/uptime
    echo "== date (board clock, may be wrong)"; date
    for f in /proc/cpuinfo /proc/meminfo /proc/version /proc/cmdline /proc/loadavg /proc/fb /proc/asound/cards \
             /proc/asound/pcm /proc/asound/devices /proc/asound/version /proc/bus/input/devices /etc/os-release \
             /etc/issue /etc/version /etc/hostname /proc/mounts; do
        echo "== $f"; cat "$f" 2>&1
    done
    echo "== cpu"
    for p in /sys/devices/system/cpu/cpu[0-9]*; do
        echo "$(basename "$p") online=$(cat "$p/online" 2>/dev/null) gov=$(cat "$p/cpufreq/scaling_governor" 2>/dev/null)" \
             "min=$(cat "$p/cpufreq/scaling_min_freq" 2>/dev/null) max=$(cat "$p/cpufreq/scaling_max_freq" 2>/dev/null)" \
             "cur=$(cat "$p/cpufreq/scaling_cur_freq" 2>/dev/null) avail=$(cat "$p/cpufreq/scaling_available_frequencies" 2>/dev/null)" \
             "govs=$(cat "$p/cpufreq/scaling_available_governors" 2>/dev/null)"
    done
    echo "== thermal"
    for t in /sys/class/thermal/thermal_zone*; do echo "$(basename "$t") $(cat "$t/type" 2>/dev/null) $(cat "$t/temp" 2>/dev/null)"; done
    echo "== fb0"
    for f in /sys/class/graphics/fb0/*; do [ -f "$f" ] && echo "$(basename "$f"): $(cat "$f" 2>/dev/null | head -5 | tr '\n' ' ')"; done
    have fbset && { echo "== fbset"; fbset 2>&1; }
    have amixer && { echo "== amixer"; amixer 2>&1 | head -80; }
    have aplay && { echo "== aplay -l"; aplay -l 2>&1; }
    echo "== ls"; ls -l /dev/input /dev/fb* /dev/snd /dev/tty* 2>&1 | head -150
    echo "== mount"; mount 2>&1
    echo "== df"; df -k 2>&1
    echo "== free"; free 2>&1
    echo "== ps"; ps 2>&1 | head -100
    echo "== env"; env 2>&1 | sort
    echo "== ulimit"; ulimit -a 2>&1
    echo "== busybox"; busybox --list 2>&1 | tr '\n' ' '; echo
    for b in taskset timeout setsid nice renice hexdump od dd awk ip ifconfig iw fbset amixer aplay evtest; do
        printf '%s=%s\n' "$b" "$(command -v $b 2>/dev/null)"
    done
    echo "== network (presence only)"
    have ip && ip addr 2>&1; have ifconfig && ifconfig -a 2>&1; cat /proc/net/dev /proc/net/wireless 2>&1
    echo "== tmpfs $ram"
} > "$out/system.txt" 2>&1
cat /proc/cpuinfo > "$c/cpuinfo.txt" 2>&1; cat /proc/meminfo > "$c/meminfo.txt" 2>&1
cat /proc/bus/input/devices > "$c/input-devices.txt" 2>&1; cat /proc/asound/cards > "$c/asound-cards.txt" 2>&1
for e in /sys/class/input/event*; do
    [ -d "$e" ] || continue
    echo "$(basename "$e") name=$(cat "$e/device/name" 2>/dev/null) ev=$(cat "$e/device/capabilities/ev" 2>/dev/null) key=$(cat "$e/device/capabilities/key" 2>/dev/null) abs=$(cat "$e/device/capabilities/abs" 2>/dev/null)"
done > "$c/input-capabilities.txt" 2>&1
dd if=/dev/fb0 of="$out/fb0-before.raw" bs=65536 2>/dev/null   # the whole panel, 16 or 32 bpp
# write speed: card and tmpfs
for target in "$out" "$ram"; do
    t0=$(up); dd if=/dev/zero of="$target/ddtest.bin" bs=65536 count=64 conv=fsync 2>> "$c/dd.txt"; t1=$(up)
    echo "dd 4MiB to $target: $(awk -v a="$t0" -v b="$t1" 'BEGIN { print b - a }') s" >> "$c/dd.txt"
    rm -f "$target/ddtest.bin"
done
# raw input for the whole run (anything the user presses is recorded); 16-byte events on 32-bit ARM
capture_pids=
for d in /dev/input/event* /dev/input/js*; do
    [ -c "$d" ] || continue
    case "$d" in *js*) bs=8 ;; *) bs=16 ;; esac
    dd if="$d" of="$c/input/$(basename "$d").raw" bs=$bs count=20000 2>/dev/null &
    capture_pids="$capture_pids $!"
done
sync
log "END census 0 $(up)"

# ---- cases ----
start=$(up)
DEADLINE=${DIAG_DEADLINE:-870}  # seconds after the census: no new case starts later
index=0
desc() {  # every descendant of $1
    for s in /proc/[0-9]*/stat; do
        { read -r line < "$s"; } 2>/dev/null || continue
        rest=${line##*) }; set -- $rest
        [ "$2" = "$want" ] && echo "${s%/stat}" | cut -d/ -f3
    done
}
tree() { want=$1; for k in $(desc); do tree "$k"; echo "$k"; done; }
killtree() { for p in $(tree "$2") "$2"; do kill -"$1" "$p" 2>/dev/null; done; }
sample() {  # time rss_kb temp cpu0..3 freq
    rss=0
    for p in $(tree "$1"); do
        r=$(awk '/VmRSS/ { print $2 }' "/proc/$p/status" 2>/dev/null)
        [ "${r:-0}" -gt "$rss" ] && rss=$r
    done
    temp=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null)
    freq=$(for k in 0 1 2 3; do cat /sys/devices/system/cpu/cpu$k/cpufreq/scaling_cur_freq 2>/dev/null || echo -; done | tr '\n' ' ')
    echo "$(up) $rss ${temp:--} $freq $(cut -d' ' -f1-3 /proc/loadavg)"
}

# run NAME BUDGET KIND [VAR=VALUE ...]   KIND: sdl | headless | sdl-pinned
# Rehearsals in an emulator only: DIAG_BUDGET_MUL=N gives every case N times its seconds, DIAG_CASES=N runs the
# first N cases alone.
run() {
    name=$1 budget=$(($2 * ${DIAG_BUDGET_MUL:-1})) kind=$3; shift 3
    index=$((index + 1))
    if [ -n "${DIAG_CASES:-}" ] && [ "$index" -gt "$DIAG_CASES" ]; then
        log "SKIP $name DIAG_CASES"; return
    fi
    now=$(up)
    if awk -v a="$start" -v b="$now" -v d="$DEADLINE" 'BEGIN { exit !(b - a > d) }'; then
        log "SKIP $name deadline"; return
    fi
    say "$index" "$name"
    dir="$out/$(printf %02d $index)-$name"; work="$ram/$name"
    mkdir -p "$dir" "$work"
    : > "$work/case.env"
    for v in SS_SKIP=0 SS_NET= "SS_FRAMELOG=$work/frames.csv" "$@"; do echo "$v" >> "$work/case.env"; done
    [ -n "${PROF_CLOCK:-}" ] && echo "SS_PROF_CLOCK=$PROF_CLOCK" >> "$work/case.env"
    cp "$work/case.env" "$dir/env.txt"
    echo "kind=$kind headless=$headless" >> "$dir/env.txt"
    log "BEGIN $name"
    t0=$(up)
    child=headless; [ "$kind" != headless ] && child=sdl
    if [ "$kind" = sdl-pinned ] && have taskset; then
        ( cd "$work" && exec taskset 1 sh "$self" --child "$child" ) > "$dir/stdout.txt" 2> "$dir/stderr.txt" &
    else
        ( cd "$work" && exec sh "$self" --child "$child" ) > "$dir/stdout.txt" 2> "$dir/stderr.txt" &
    fi
    pid=$!
    waited=0 timeout=0
    while kill -0 "$pid" 2>/dev/null; do
        [ $((waited % 5)) = 0 ] && sample "$pid" >> "$dir/monitor.txt"
        if [ "$waited" -ge "$budget" ]; then
            timeout=1; log "TIMEOUT $name"
            killtree TERM "$pid"; sleep 5; killtree KILL "$pid"
            break
        fi
        sleep 1; waited=$((waited + 1))
    done
    wait "$pid"; code=$?
    t1=$(up)
    secs=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.1f", b - a }')
    echo "$code" > "$dir/exit.txt"
    echo "start $t0 end $t1 seconds $secs timeout $timeout" > "$dir/time.txt"
    for f in "$work"/*; do
        [ -f "$f" ] || continue
        case "$(basename "$f")" in case.env|shiningsource.sav*) ;; *) cp "$f" "$dir/" 2>/dev/null ;; esac
    done
    ls -la "$work" > "$dir/workdir.txt" 2>&1
    [ "$code" != 0 ] && dmesg 2>/dev/null | tail -n 60 > "$dir/dmesg-tail.txt"
    rm -rf "$work"
    log "END $name $code $secs"
    sync
}

P="SS_INPUT_FILE=$play"
# 1 the precise clock, alone (it may end with an illegal instruction)
run clock-probe 25 headless SS_FRAMES=180 SS_PROF=1 SS_PROF_CLOCK=counter
f="$out/01-clock-probe/frames.csv"
if [ "$(cat "$out/01-clock-probe/exit.txt" 2>/dev/null)" = 0 ] && grep -q "clock cntvct32" "$f" 2>/dev/null &&
   awk -F, '/^[0-9]/ { s += $2 } /^# end/ { ms = $6 } END { exit !(s > 0 && ms > 0 && s / 1000 / ms > 0.8 && s / 1000 / ms < 1.2) }' "$f"; then
    PROF_CLOCK=counter; log "CLOCK counter"
else
    log "CLOCK ms"
fi
run boot 45 sdl SS_QUIT=600 SS_INPUT_LOG=1 SS_STATS=1 SS_FB_SHOT=300
if [ "$(cat "$out/02-boot/exit.txt" 2>/dev/null)" != 0 ]; then
    log "ABORT the game did not start: see 02-boot"
    say "-" "ABORTADO: el juego no arranco"
    DIAG_SET=aborted
fi
if [ "${DIAG_SET:-}" = fps ]; then
    # ---- frame rate in Flat (320x240), stage by stage: what the 80-frames-a-second goal needs ----
    TOTAL=8
    F=SS_CANVAS=320x240
    run fps-logic 150 sdl "$F" SS_PICTURE=0 SS_QUIT=3000 SS_PROF=1 SS_STATS=1 "$P"
    # the opening (title, save, character creation) and, from frame 3900 on, the first field
    run fps-render 260 sdl "$F" SS_PICTURE=2 SS_QUIT=6000 SS_PROF=1 "$P"
    run fps-show 200 sdl "$F" SS_PICTURE=0:3900 SS_TEST_SLOW=17 SS_FRAMESKIP=0 SS_QUIT=5400 SS_PROF=3900:5400 SS_STATS=1 "$P"
    run fps-play 160 sdl "$F" SS_QUIT=6000 SS_PROF=1 SS_STATS=1 SS_FB_SHOT=5000 "$P"
    run fps-render-cpu 260 headless SS_FRAMES=6000 SS_TEST_RENDER=320 SS_VIEW=320 SS_VIEW_H=240 SS_PROF=1 "$P"
    [ -n "$town" ] && run fps-town 150 sdl "$F" "SS_INPUT_FILE=$town" SS_PICTURE=0:9000 SS_TEST_WARP=9000:0:0 SS_QUIT=9900 SS_PROF=9000:9900 SS_STATS=1
elif [ "${DIAG_SET:-}" != aborted ]; then
run play-paced-320 80 sdl SS_FB_VIEW=320 SS_QUIT=2400 SS_FB_SHOT=1500 SS_STATS=1 SS_PROF=1 "$P"
run hash-headless 100 headless SS_FRAMES=3000 SS_HASH_ALL=1 "$P"
run cap-headless 100 headless SS_FRAMES=4000 SS_PROF=1 "$P"
run cap-render-320 130 headless SS_FRAMES=4000 SS_TEST_RENDER=320 SS_VIEW=320 SS_PROF=1 "$P"
run play-paced-240 80 sdl SS_FB_VIEW=240 SS_QUIT=2400 SS_FB_SHOT=1500 SS_STATS=1 SS_PROF=1 "$P"
run cap-render-240 130 headless SS_FRAMES=4000 SS_TEST_RENDER=240 SS_VIEW=240 SS_PROF=1 "$P"
run cap-nopic-alsa 100 sdl SS_FB_VIEW=320 SS_PICTURE=0 SS_QUIT=4000 SS_PROF=1 SS_STATS=1 "$P"
run cap-nopic-dummy 100 sdl SS_FB_VIEW=320 SS_PICTURE=0 SS_QUIT=4000 SS_PROF=1 SDL_AUDIO_DRIVER=dummy "$P"
run cap-noshow-320 130 sdl SS_FB_VIEW=320 SS_PICTURE=2 SS_QUIT=4000 SS_PROF=1 "$P"
run cap-show-320 90 sdl SS_FB_VIEW=320 SS_TEST_SLOW=17 SS_FRAMESKIP=0 SS_QUIT=1200 SS_PROF=1 SS_STATS=1 "$P"
run hash-sdl 100 sdl SS_FRAMES=3000 SS_HASH_ALL=1 "$P"
run stability-320 260 sdl SS_PICTURE=0:8900 SS_QUIT=16100 SS_STATS=1 SS_PROF=1 SS_FB_SHOT=6000 "$P"
run audio-disk 40 sdl SS_FB_VIEW=320 SS_QUIT=600 SS_AUDIO_FILE=1 SDL_AUDIO_DRIVER=disk SDL_DISK_AUDIO_FILE=sdl_audio.raw "$P"
# save tests: tmpfs and the card, with a copy of the user's save when there is one
for where in tmpfs card; do
    if [ $where = card ]; then oldram=$ram; ram="$out/savework"; mkdir -p "$ram"; fi
    mkdir -p "$ram/save-$where"
    for s in "$game/shiningsource.sav" "$game/user/shiningsource.sav" "$game/saves/shiningsource.sav"; do
        [ -f "$s" ] && { cp "$s" "$ram/save-$where/" && log "SAVECOPY $s"; break; }
    done
    run save-$where 60 headless SS_FRAMES=3100 SS_TEST_SAVE=3000 "$P"
    if [ $where = card ]; then rm -rf "$ram"; ram=$oldram; fi
done
run cores-pinned-320 100 sdl-pinned SS_FB_VIEW=320 SS_PICTURE=2 SS_QUIT=2000 SS_PROF=1 "$P"
g0=/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
if [ -w "$g0" ]; then
    governor_saved=$(cat "$g0")
    for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > "$g" 2>/dev/null; done
    log "GOVERNOR $governor_saved -> $(cat "$g0")"
    run gov-performance-320 100 sdl SS_FB_VIEW=320 SS_PICTURE=2 SS_QUIT=2000 SS_PROF=1 "$P"
    for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo "$governor_saved" > "$g" 2>/dev/null; done
    log "GOVERNOR restored $(cat "$g0")"; governor_saved=
else
    log "SKIP gov-performance-320 not-writable"
fi
run cap-show-240 90 sdl SS_FB_VIEW=240 SS_TEST_SLOW=17 SS_FRAMESKIP=0 SS_QUIT=1200 SS_PROF=1 SS_STATS=1 "$P"
run cap-render-480 130 headless SS_FRAMES=4000 SS_TEST_RENDER=480 SS_VIEW=480 SS_PROF=1 "$P"
run play-paced-320-noskip 60 sdl SS_FB_VIEW=320 SS_FRAMESKIP=0 SS_QUIT=1800 SS_STATS=1 "$P"
[ -n "$town" ] && run town-320 150 sdl SS_FB_VIEW=320 "SS_INPUT_FILE=$town" SS_PICTURE=0:9000 SS_TEST_WARP=9000:0:0 SS_QUIT=9900 SS_PROF=9000:9900 SS_STATS=1
fi

# ---- end ----
dmesg 2>/dev/null | tail -n 200 > "$out/dmesg-end.txt"
{
    echo "caso salida segundos fotogramas fps_media"
    for d in "$out"/[0-9][0-9]-*; do
        [ -d "$d" ] || continue
        fr=$(awk -F, '/^[0-9]/ { n++; s += $2 } END { if (n && s) printf "%d %.1f", n, 1e6 * n / s; else print "0 -" }' "$d/frames.csv" 2>/dev/null)
        echo "$(basename "$d") $(cat "$d/exit.txt" 2>/dev/null) $(cut -d' ' -f6 "$d/time.txt" 2>/dev/null) ${fr:-0 -}"
    done
} > "$out/summary.txt" 2>&1
cleanup
log "END 0"
sync
exit 0
