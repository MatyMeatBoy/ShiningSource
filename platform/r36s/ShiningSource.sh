#!/bin/bash

ports=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
gamedir="$ports/shiningsource"
rom="$gamedir/roms/rom.gba"
program="$gamedir/ss2_linux_arm64"

for controls in "$ports/PortMaster" /opt/system/Tools/PortMaster /opt/tools/PortMaster \
    "${XDG_DATA_HOME:-$HOME/.local/share}/PortMaster" /roms/ports/PortMaster /roms2/ports/PortMaster; do
    [ -r "$controls/control.txt" ] || continue
    controlfolder=$controls
    . "$controlfolder/control.txt"
    if command -v get_controls >/dev/null; then
        get_controls
        if [ -z "${SDL_GAMECONTROLLERCONFIG:-}" ] && [ -n "${sdl_controllerconfig:-}" ]; then
            export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
        fi
    fi
    break
done

finish() {
    result=$?
    if command -v pm_finish >/dev/null; then pm_finish; fi
    exit "$result"
}
trap finish EXIT

mkdir -p "$gamedir/user" || exit 1
cd "$gamedir/user" || exit 1
exec >last-run.log 2>&1
message=
if [ "$(uname -m)" != aarch64 ]; then
    message='ShiningSource requires a 64-bit Linux ARM firmware.'
elif [ ! -r "$rom" ]; then
    message='Place your European Shining Soul II cartridge dump in shiningsource/roms/rom.gba.'
fi
if [ -n "${message:-}" ]; then
    printf '%s\n' "$message" >&2
    if command -v pm_message >/dev/null; then pm_message "$message"; fi
    exit 5
fi

if command -v pm_platform_helper >/dev/null; then pm_platform_helper "$program"; fi
export SDL_VIDEO_DRIVER="${SDL_VIDEO_DRIVER:-${SDL_VIDEODRIVER:-kmsdrm}}"
export SDL_AUDIO_DRIVER="${SDL_AUDIO_DRIVER:-${SDL_AUDIODRIVER:-alsa}}"
"$program" --game "$rom"
