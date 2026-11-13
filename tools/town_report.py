#!/usr/bin/env python3
"""town_report.py NAME DIR FRAMES [CHECK ...]: what became of one player's walk through the town.

DIR holds what the program wrote: run.log (what it said) and where.log (SS_WHERE_LOG, runtime/core/frame.c:
every tenth frame the map, the room, where the character stands, its flags, the scene's flags, the bag).
FRAMES is the frame the run was to reach. The checks are those of tools/tests/town_routes.txt:

  room=N    the town's room at the end           bag=N    records of the bag in use at the end
  money=N   the money in the bag at the end      held=1   an event held the player at some time
  free=1    the player has the control at the end (no event holds it, no menu is open)

Whatever the checks say, the run fails if the program ended before FRAMES, reported a crash, or reached a
function the port only stubs. Prints one line and returns 0 when everything holds.
"""
import re, sys
from pathlib import Path

HELD = 0x800 | 0x400            # scene flags: an event holds the player, the game's menu is open


def main():
    name, folder, frames = sys.argv[1], Path(sys.argv[2]), int(sys.argv[3])
    checks = dict(c.split("=") for c in sys.argv[4:])
    said = (folder / "run.log").read_text(errors="replace") if (folder / "run.log").exists() else ""
    bad = []
    reached = [int(m) for m in re.findall(r"^frame\s+(\d+)", said, re.M)]
    if not reached or reached[-1] < frames:
        bad.append("ended at frame %d of %d" % (reached[-1] if reached else 0, frames))
    crash = re.search(r"^.*(?:crash|exception).*$", said, re.M | re.I)
    if crash:
        bad.append(crash.group(0).strip()[:120])
    stubs = [int(m) for m in re.findall(r"stubs reached (\d+)", said)]
    if stubs and stubs[-1]:
        bad.append("stubs reached %d (%s)" % (stubs[-1], ", ".join(sorted(set(re.findall(r"^\[stub\] *(\S+)", said, re.M))))))
    rows = []
    where = folder / "where.log"
    if where.exists():
        for line in where.read_text().splitlines():
            part = line.split()
            if len(part) == 10:
                rows.append(part)
    town = [r for r in rows if r[1] == "0"]
    if not town:
        bad.append("never in the town")
        last = None
    else:
        last = town[-1]
        seen = {"room": last[2], "bag": last[8], "money": last[9],
                "held": "1" if any(int(r[7], 16) & HELD for r in town) else "0",
                "free": "0" if int(last[7], 16) & HELD or int(last[6], 16) & 1 else "1"}
        for key, want in checks.items():
            if seen.get(key) != want:
                bad.append("%s=%s, wanted %s" % (key, seen.get(key), want))
    place = "room %s at %s,%s, bag %s, money %s" % (last[2], last[4], last[5], last[8], last[9]) if last else "-"
    print("RESULT %s: %s  [%s]%s" % (name, "FAILED" if bad else "ok", place, "  " + "; ".join(bad) if bad else ""))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
