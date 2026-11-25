#!/bin/sh
# ShiningSource GA36 - Medir FPS: about 6 minutes, automatic, no buttons needed. Measures the frame rate in Flat
# (320x240), stage by stage, into shiningsourcega36/diagnostico-NNN/. The saves are not touched.
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
DIAG_SET=fps exec /bin/sh "$here/ShiningSource GA36 - Diagnostico.sh" "$@"
