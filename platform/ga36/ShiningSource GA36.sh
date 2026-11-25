#!/bin/sh
# ShiningSource Ports entry for the GA36-MB (Allwinner A33, ARMv7 hard-float).
ports=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P) || exit 1
game="$ports/shiningsourcega36"

if ! mkdir -p "$game/user" || ! cd "$game/user"; then
    printf 'ShiningSource GA36: cannot write to %s/user.\n' "$game" >&2
    exit 1
fi
if [ -f last-run.log ]; then mv -f last-run.log last-run.1.log; fi
: >last-run.log

if [ ! -r "$game/ga36-env.sh" ]; then
    printf 'ShiningSource GA36: %s/ga36-env.sh is missing. Extract the whole package again.\n' "$game" \
        | tee -a last-run.log >&2
    exit 1
fi
. "$game/ga36-env.sh"

{
    printf 'ShiningSource GA36 %s\n' "$(date 2>/dev/null)"
    printf 'kernel: %s\n' "$(uname -a 2>/dev/null)"
    printf 'rom: %s\n' "${rom:-none}"
    printf 'video: %s fb: %s audio: %s\n' "$SDL_VIDEO_DRIVER" "${SS_FBDEV:-none}" "$SDL_AUDIO_DRIVER"
} >>last-run.log

if [ -n "$ss_error" ]; then
    printf 'ShiningSource GA36: %s\n' "$ss_error" | tee -a last-run.log >&2
    exit 5
fi

ss_run "$@" >>last-run.log 2>&1
status=$?
printf 'exit status: %s\n' "$status" >>last-run.log
exit "$status"
