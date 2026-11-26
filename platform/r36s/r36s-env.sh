# ShiningSource R36S environment. POSIX sh (bash, dash, busybox ash), sourced, never run.
# The caller sets `game` to the shiningsource folder. Afterwards `rom` holds the cartridge dump to use,
# `ss_error` a message when the game cannot start, and `ss_run` starts the game in the current
# directory (saves, settings, logs).

ss_error=
rom=

case "$(uname -m)" in
    aarch64|arm64) ;;
    *) ss_error="this package needs a 64-bit ARM Linux firmware (uname -m: $(uname -m))." ;;
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
    for ss_dir in "$game/../../gba" "$game/../../roms/gba" /roms/gba /roms2/gba; do
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

# The picture goes to the panel through DRM/KMS: three buffers, a page flip at the start of each refresh (never
# torn), the panel's measured rate paces the game when it is near the console's 59.73 Hz and the sound follows
# it (runtime/plat/fbdev_linux.c, as on the GA36). SDL then runs offscreen for the controller and the sound.
# SS_FBDEV names any display device that exists: /dev/fb0 is the fallback when KMS cannot be driven.
# SS_FBDEV= (empty) leaves the screen to SDL3's own KMSDRM driver (OpenGL ES on the Mali GPU) instead.
if [ -z "${SS_FBDEV+set}" ]; then
    SS_FBDEV=
    for ss_file in /dev/fb0 /dev/dri/card0 /dev/dri/card1; do
        if [ -e "$ss_file" ]; then SS_FBDEV=$ss_file; break; fi
    done
    unset ss_file
fi
export SS_FBDEV
# (a video driver named by the firmware would make SDL open the panel too, beside the program's own KMS)
if [ -n "$SS_FBDEV" ]; then unset SDL_VIDEO_DRIVER SDL_VIDEODRIVER; fi
# Flat, the 4:3 canvas: 320x240 game pixels at exactly 2x fill the 640x480 panel. The field is shown in every
# direction, the status display at the top edge, the message window at the bottom edge; menus and cut-scenes keep
# the console's 240x160 picture (480x320 in the middle) with the port's art or black around it.
# SS_CANVAS=240x160 shows the console's picture alone, 480x320 in the middle.
export SS_CANVAS="${SS_CANVAS-320x240}"
# The art beside the picture (assets/launcher): the program runs from user/.
export SS_ASSETS="${SS_ASSETS-$game/}"
# Without the framebuffer path SDL3 opens a window: full screen at the panel's mode.
export SS_WINDOW_MODE="${SS_WINDOW_MODE-borderless}"
export SDL_AUDIO_DRIVER="${SDL_AUDIO_DRIVER:-${SDL_AUDIODRIVER:-alsa}}"

# RetroAchievements (runtime/achievements): the set ships in this folder (ra_set_*.json, achievements/badges) and is
# evaluated on the console. The card is linked to an account from a PC (copy retroachievements.auth and .ini
# of a linked card folder here); without that, the sign-in of ReKHCoM on the same card is used.
# Unlocks go to the account (softcore) when the console has internet, else wait in retroachievements_pending.txt
# for the next start online; retroachievements_estado.txt says how it stands. A Rebel run (cheats) sends nothing.
# SS_RA_SUBMIT=0 in user/shiningsource.env keeps every unlock on the console.
export SS_RA_DIR="${SS_RA_DIR-$game}"
export SS_RA_SUBMIT="${SS_RA_SUBMIT-1}"
# Notices and retroachievements_estado.txt in the language the game was set to (its first-start choice).
if [ -z "${SS_LANG+set}" ] && [ -f "$game/user/shiningsource-language.txt" ]; then
    case "$(head -c 2 "$game/user/shiningsource-language.txt")" in es) export SS_LANG=es ;; esac
fi
if [ -z "${SS_RA_SHARED_AUTH+set}" ]; then
    SS_RA_SHARED_AUTH=
    for ss_file in "$game/../rekhcomr36s/retroachievements.auth" "$game/../rekhcomga36/retroachievements.auth"; do
        if [ -f "$ss_file" ]; then SS_RA_SHARED_AUTH=$ss_file; break; fi
    done
    unset ss_file
fi
export SS_RA_SHARED_AUTH
# Frame times, rate and stage costs at the end of last-run.log (cheap: a clock reading per stage).
export SS_STATS="${SS_STATS-1}"
export SS_PROF="${SS_PROF-1}"

if [ -z "${SDL_GAMECONTROLLERCONFIG_FILE:-}" ] && [ -r "$game/gamecontrollerdb.txt" ]; then
    export SDL_GAMECONTROLLERCONFIG_FILE="$game/gamecontrollerdb.txt"
fi

ss_run() {
    (
        unset LD_PRELOAD
        exec "$game/ss2_r36s" --game "$rom"
    )
}
