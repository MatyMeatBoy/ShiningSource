#!/usr/bin/env python3
"""net_gone_report.py RUN_FOLDER... : did the others go on after a player was gone?

RUN_FOLDER is what tools/net_gone_test.sh wrote (build/gone/TAG): case.txt and per program pN/run.log and
pN/state.log (SS_STATE_LOG, runtime/core/frame.c: clock, frame, mode, scene flags, map, area, seats, players).
tools/net_outage_map.sh's folders (build/outage/TAG, scene.log instead of state.log) are not read here.

Per program: the maps and areas its game was in, in order, with the frame of each change, and where it ended.
Of the programs that were to run to the end (all but player `who` of case.txt):
  survived   each ran to its last frame
  went on    each made at least WANT changes of map after the player was gone (WANT: the changes the route has
             left at that point; the environment's WANT, else every change the reference route makes after
             frame `at`: 12/0 -> 12/1 -> 12/2 -> 0/0 -> 0/2)
  together   they made the same changes, and ended in the same map and area
A run in which all three hold is WENT ON; with the host gone (who 1) the others' session ends by design and
the line says ENDED. One line per run starts with RESULT.

Exit status 1 if any run with a player gone (not the host) did not go on."""
import os, re, sys

ROUTE = [(12, 0), (12, 1), (12, 2), (0, 0), (0, 2)]


def places(path):
    """[(frame, map, area)] at every change"""
    found = []
    if os.path.exists(path):
        for line in open(path, errors="replace"):
            f = line.split()
            if len(f) >= 8 and f[7] == "|":
                here = (int(f[4]), int(f[5]))
                if not found or found[-1][1:] != here:
                    found.append((int(f[1]),) + here)
    return found


def last_frame(path):
    frame = 0
    if os.path.exists(path):
        for line in open(path, errors="replace"):
            m = re.match(r"frame (\d+)\s+hash", line)
            if m:
                frame = int(m.group(1))
    return frame


def report(folder):
    case = dict(zip(*[iter(open(os.path.join(folder, "case.txt")).read().split())] * 2))
    players, who, at, frames = int(case["players"]), int(case["who"]), int(case["at"]), int(case["frames"])
    how = case["how"]
    print("%s: %d players, player %d %s at frame %d" % (os.path.basename(folder), players, who, how, at))
    ways, ok_survived, said = {}, True, []
    for i in range(1, players + 1):
        log = os.path.join(folder, "p%d" % i)
        way = places(os.path.join(log, "state.log"))
        ended = last_frame(os.path.join(log, "run.log"))
        text = open(os.path.join(log, "run.log"), errors="replace").read() if os.path.exists(os.path.join(log, "run.log")) else ""
        said += ["  p%d says: %s" % (i, line[:150]) for line in text.splitlines() if line.startswith("net: player") or "went on" in line]
        print("  p%d: %s; last frame %d%s" % (i, " -> ".join("%d/%d@%d" % (m, a, f) for f, m, a in way) or "no state", ended,
                                              " (the player that was gone)" if i == who and how != "none" else ""))
        if i == who and how != "none":
            continue
        ways[i] = way
        if ended < frames:
            ok_survived = False
    for line in said:
        print(line)
    # the changes the route has left after frame `at`: by the place the first that stayed was in then
    # (the title is map 0 too: the route starts at its first map 12)
    stayed = sorted(ways)
    for i in stayed:
        first = next((k for k, p in enumerate(ways[i]) if p[1] == 12), 0)
        ways[i] = ways[i][first:]
    if how == "none":
        at = 0
    before = [(m, a) for f, m, a in ways[stayed[0]] if f <= at]
    start = ROUTE.index(before[-1]) if before and before[-1] in ROUTE else 0
    want = int(os.environ.get("WANT", len(ROUTE) - 1 - start))
    made = {i: len([p for p in ways[i] if p[0] > at]) for i in stayed}
    ways = {i: [(m, a) for f, m, a in ways[i]] for i in stayed}
    went_on = all(n >= want for n in made.values())
    together = len({tuple(w) for w in ways.values()}) == 1
    print("  survived %s; went on %s (changes of map after it was gone: %s, wanted %d); together %s" % (
        "yes" if ok_survived else "NO", "yes" if went_on else "NO",
        ", ".join("p%d %d" % (i, made[i]) for i in stayed), want, "yes" if together else "NO"))
    if who == 1 and how != "none":
        verdict = "ENDED (the host was gone: the session ends by design)"
        good = True
    else:
        good = ok_survived and went_on and together
        verdict = "WENT ON" if good else "STUCK" if not went_on else "NOT WHOLE"
    print("RESULT %s %s" % (os.path.basename(folder), verdict))
    return good


if __name__ == "__main__":
    results = [report(folder) for folder in sys.argv[1:]]
    sys.exit(0 if all(results) else 1)
