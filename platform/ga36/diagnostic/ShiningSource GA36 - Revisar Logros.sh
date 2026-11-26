#!/bin/sh
# ShiningSource GA36 - Revisar Logros: about 1 minute, automatic. Checks what the RetroAchievements link needs
# (clock, network, DNS, OpenSSL, the account files by size only) and makes one short start on copies of the files.
# Writes shiningsourcega36/diagnostico-logros.txt and diagnostico-NNN/. The saves and the real queue are not touched.
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
DIAG_SET=logros exec /bin/sh "$here/ShiningSource GA36 - Diagnostico.sh" "$@"
