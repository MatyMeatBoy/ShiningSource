# ShiningSource GA36 environment. POSIX sh (busybox ash, dash), sourced, never run.
# The caller sets `game` to the shiningsourcega36 folder. Afterwards `rom` holds the
# cartridge dump to use, `ss_error` a message when the game cannot start, and
# `ss_run [arguments]` starts the game in the current directory (saves, settings, logs).

ss_error=
rom=

case "$(uname -m)" in
    armv7*|armv8*|aarch64) ;;
    *) ss_error="this package needs an ARMv7 hard-float Linux (uname -m: $(uname -m))." ;;
esac

# The ROM is only read in place, never copied.
if [ -r "$game/roms/rom.gba" ]; then
    rom="$game/roms/rom.gba"
else
    for ss_file in "$game"/roms/*.gba "$game"/roms/*.GBA; do
        if [ -f "$ss_file" ] && [ -r "$ss_file" ]; then rom=$ss_file; break; fi
    done
fi
if [ -z "$rom" ]; then
    for ss_dir in "$game/../../roms/gba" "$game/../../gba"; do
        [ -d "$ss_dir" ] || continue
        for ss_file in "$ss_dir"/*[Ss][Hh][Ii][Nn][Ii][Nn][Gg]*[Ss][Oo][Uu][Ll]*[Ii][Ii]*.[Gg][Bb][Aa] \
                       "$ss_dir"/*[Ss][Hh][Ii][Nn][Ii][Nn][Gg]*[Ss][Oo][Uu][Ll]*2*.[Gg][Bb][Aa]; do
            if [ -f "$ss_file" ] && [ -r "$ss_file" ]; then rom=$ss_file; break 2; fi
        done
    done
fi
unset ss_file ss_dir
if [ -z "$ss_error" ] && [ -z "$rom" ]; then
    ss_error="no ROM found. Copy your own unmodified European Shining Soul II dump (AU2P) to $game/roms/rom.gba."
fi

export SS_FBDEV="${SS_FBDEV-/dev/fb0}"
if [ -z "$ss_error" ] && [ -n "$SS_FBDEV" ] && { [ ! -r "$SS_FBDEV" ] || [ ! -w "$SS_FBDEV" ]; }; then
    ss_error="framebuffer $SS_FBDEV is not readable and writable."
fi
# The 4:3 canvas: 320x240 game pixels at 2x fill the 640x480 panel. The field is
# shown in every direction; menus and cut-scenes keep the console's 240x160
# picture (480x320 on the panel) with the port's decor around it.
# SS_CANVAS=240x160 shows the console's picture alone, 480x320 in the middle;
# an empty SS_CANVAS gives the earlier wide picture (SS_FB_VIEW, 320x160).
export SS_CANVAS="${SS_CANVAS-320x240}"
# The art beside the picture (assets/launcher): the program runs through the loader, from user/.
export SS_ASSETS="${SS_ASSETS-$game/}"
# GA36 face buttons: the bottom one is the GBA's A and the right one its B (SDL buttons, in KEYINPUT order:
# A B Select Start Right Left Up Down R L). A binding the player saves in the options still wins.
export SS_PAD="${SS_PAD-0,1,4,6,14,13,11,12,10,9}"
export SDL_VIDEO_DRIVER="${SDL_VIDEO_DRIVER:-offscreen}"
export SDL_AUDIO_DRIVER="${SDL_AUDIO_DRIVER:-alsa}"

if [ -z "${SDL_GAMECONTROLLERCONFIG_FILE:-}" ] && [ -r "$game/gamecontrollerdb.txt" ]; then
    export SDL_GAMECONTROLLERCONFIG_FILE="$game/gamecontrollerdb.txt"
fi

# The bundled libasound reads its configuration from /usr/share/alsa. Use the copy
# shipped with the package only on firmwares that have none.
if [ -z "${ALSA_CONFIG_DIR:-}" ] && [ ! -r /usr/share/alsa/alsa.conf ] && [ -r "$game/alsa/alsa.conf" ]; then
    export ALSA_CONFIG_DIR="$game/alsa"
fi

# The explicit loader makes the bundled glibc independent of the current directory
# and of the firmware's own library paths.
ss_run() {
    (
        unset LD_PRELOAD
        exec "$game/libarmhf/ld-linux-armhf.so.3" --library-path "$game/libarmhf" \
            "$game/ss2_ga36" --game "$rom" "$@"
    )
}
