#!/bin/sh
# ShiningSource for Linux x64: the game with its own SDL3 and the RetroAchievements set in this folder.
# usage: ./shiningsource.sh /path/to/your.gba   (your own unmodified European Shining Soul II dump)
# Achievements are evaluated here (notices, local record). After ra_vincular_cuenta.py has signed this folder in,
# unlocks are sent to the account (softcore) over the system OpenSSL; SS_RA_SUBMIT=0 keeps them on this PC.
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
export LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export SS_RA_DIR="${SS_RA_DIR-$here}"
export SS_RA_SUBMIT="${SS_RA_SUBMIT-1}"
exec "$here/ss2_linux64" "$@"
