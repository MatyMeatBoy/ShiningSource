#!/bin/bash
# ShiningSource Ports entry for the R36S (Rockchip RK3326, 64-bit ArkOS / dArkOS / ROCKNIX-style firmwares).
ports=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P) || exit 1
game="$ports/shiningsource"

# PortMaster, when installed, gives the firmware's SDL controller mapping.
for controls in /opt/system/Tools/PortMaster /opt/tools/PortMaster "${XDG_DATA_HOME:-$HOME/.local/share}/PortMaster" \
    "$ports/PortMaster" /roms/ports/PortMaster /roms2/ports/PortMaster; do
    [ -r "$controls/control.txt" ] || continue
    controlfolder=$controls
    . "$controlfolder/control.txt"
    if command -v get_controls >/dev/null 2>&1; then
        get_controls
        if [ -z "${SDL_GAMECONTROLLERCONFIG:-}" ] && [ -n "${sdl_controllerconfig:-}" ]; then
            export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
        fi
    fi
    break
done

finish() {
    status=$?
    if command -v pm_finish >/dev/null 2>&1; then pm_finish; fi
    exit "$status"
}
trap finish EXIT

if ! mkdir -p "$game/user" || ! cd "$game/user"; then
    printf 'ShiningSource: cannot write to %s/user.\n' "$game" >&2
    exit 1
fi
if [ -f last-run.log ]; then mv -f last-run.log last-run.1.log; fi
: >last-run.log

if [ ! -r "$game/r36s-env.sh" ]; then
    printf 'ShiningSource: %s/r36s-env.sh is missing. Extract the whole package again.\n' "$game" | tee -a last-run.log >&2
    exit 1
fi
. "$game/r36s-env.sh"

{
    printf 'ShiningSource R36S %s\n' "$(date 2>/dev/null)"
    printf 'kernel: %s\n' "$(uname -a 2>/dev/null)"
    printf 'model: %s\n' "$(tr -d '\000' </proc/device-tree/model 2>/dev/null)"
    printf 'system: %s, glibc %s\n' "$(. /etc/os-release 2>/dev/null; printf '%s' "${PRETTY_NAME:-?}")" \
        "$(getconf GNU_LIBC_VERSION 2>/dev/null)"
    printf 'cpu: %s cores, governor %s, %s kHz max\n' "$(getconf _NPROCESSORS_ONLN 2>/dev/null)" \
        "$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)" \
        "$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq 2>/dev/null)"
    printf 'display: %s\n' "$(cat /sys/class/drm/card*-*/modes 2>/dev/null | tr '\n' ' ')"
    printf 'rom: %s\n' "${rom:-none}"
    printf 'video: %s canvas: %s audio: %s\n' "${SS_FBDEV:-SDL}" "${SS_CANVAS:-none}" "$SDL_AUDIO_DRIVER"
} >>last-run.log

if [ -n "$ss_error" ]; then
    printf 'ShiningSource: %s\n' "$ss_error" | tee -a last-run.log >&2
    if command -v pm_message >/dev/null 2>&1; then pm_message "$ss_error"; fi
    exit 5
fi

if command -v pm_platform_helper >/dev/null 2>&1; then pm_platform_helper "$game/ss2_r36s"; fi
ss_run >>last-run.log 2>&1
status=$?
printf 'exit status: %s\n' "$status" >>last-run.log
exit "$status"
