#!/usr/bin/env python3
"""net_outage_report.py RUN_FOLDER... : what became of a session in which a player's connection was gone.

RUN_FOLDER is what tools/net_outage_test.sh wrote (build/outage/TAG): case.txt, relay.log and per player pN/run.log,
pN/msg.log (SS_NET_MSG_LOG) and pN/net.log (SS_NET_LOG with the direct transport, runtime/core/netlink.c).

Asked of every run, over the time from five seconds after the connection came back to the end:
  survived   every program ran to its last frame and was part of the session until then (none was reset by
             the game, ended early, or was told that the session is over)
  order      every player dispatched the same messages in the same order (sender, kind and contents), as far as
             the shortest list goes
  once       of every sender, every player dispatched exactly what that sender handed over, in that order: no
             message twice, none left out in the middle
  complete   every player that survived had dispatched everything handed over up to two seconds before the end
  state      what each player's program is given of each other player moves again and is the same as that
             player has of itself: not frozen (the other player changed, this view of it did not), and equal
             whenever the other player stood still from a quarter of a second before to one after
  gone       no player counts another as departed at the end
  place      every player's game is on the same map at the end (pN/scene.log, SS_SCENE_LOG; a run without
             these logs passes)
  tokens     no token of the game's barrier is left over at the end: one that stays in a ring makes a later
             barrier count its sender as ready before it is
A run in which all eight hold is HEALED. One line per run starts with RESULT; with several folders a table of
rates per case (players, who was cut off, for how long) follows.

Exit status 1 if any run is not healed."""
import glob, os, re, sys
from collections import defaultdict


def read_messages(path):
    sent, taken = [], []
    if os.path.exists(path):
        for line in open(path, errors="replace"):
            f = line.split()
            if len(f) < 7:
                continue
            entry = (int(f[0]), int(f[3]), f[4], f[6])       # clock, sender, kind, hash of the contents
            (sent if f[2] == "S" else taken).append(entry)
    return sent, taken


def read_states(path):
    """quarter -> (gone mask, [state of seat 0..3 or None])"""
    found = {}
    if os.path.exists(path):
        for line in open(path, errors="replace"):
            f = line.split()
            if len(f) == 8 and f[0] == "Q":
                found[int(f[1])] = (int(f[3], 16), [None if v == "-" else v for v in f[4:8]])
    return found


def read_scenes(path):
    """[(quarter, (map set, area, choosing on the world map), tokens held of each seat)]"""
    found = []
    if os.path.exists(path):
        for line in open(path, errors="replace"):
            f = line.split()
            if len(f) == 10:
                found.append((int(f[0]), (int(f[2]), int(f[3]), int(f[5])), [int(v.split(":")[0]) for v in f[6:10]]))
    return found


def report(run):
    case = {}
    words = open(os.path.join(run, "case.txt")).read().split()
    for name, value in zip(words[0::2], words[1::2]):
        case[name] = value
    players, who, seconds, frames = int(case["players"]), int(case["who"]), int(case["seconds"]), int(case["frames"])
    cut = back = None
    # (with several outages: the first cut and the last return)
    for line in open(os.path.join(run, "relay.log"), errors="replace"):
        m = re.match(r"outage: seat \d+ (cut off|back) at \d+ ms \(clock (\d+)\)", line)
        if m and m.group(1) == "cut off":
            cut = cut or int(m.group(2))
        elif m:
            back = int(m.group(2))
    lines = []
    said, last_frame, reset = [], [], []
    for p in range(1, players + 1):
        text = open(os.path.join(run, "p%d" % p, "run.log"), errors="replace").read()
        numbers = re.findall(r"^frame +(\d+) +hash", text, re.M)
        last_frame.append(int(numbers[-1]) if numbers else 0)
        reset.append("SoftReset" in text)
        said.append([l[5:] for l in text.splitlines() if l.startswith("net: ")])
    messages = [read_messages(os.path.join(run, "p%d" % p, "msg.log")) for p in range(1, players + 1)]
    states = [read_states(os.path.join(run, "p%d" % p, "net.log")) for p in range(1, players + 1)]
    # A program whose game was told that the session is over (the host is lost) is given no blocks any more:
    # its state log stops there, though the program may run on (the game then shows its link error).
    last_state = [max(s) * 250 if s else 0 for s in states]
    out_early = [last_state[i] < max(last_state) - 5000 for i in range(players)]
    survived = [last_frame[i] >= frames and not reset[i] and not out_early[i] for i in range(players)]
    ends = [last_state[i] for i in range(players) if survived[i]]
    end = (min(ends) if ends else 0) - 1500         # the programs do not end in the same moment
    # (what a player says after the first program ended is the end of the test, not of the session)
    for i in range(players):
        if survived[i]:
            fate = "ran to frame %d" % last_frame[i]
        elif out_early[i] and last_frame[i] >= frames and not reset[i]:
            fate = "OUT of the session %d s before the end (the program ran on)" % ((max(last_state) - last_state[i]) // 1000)
        else:
            fate = "ENDED at frame %d of %d%s" % (last_frame[i], frames, " (the game reset itself)" if reset[i] else "")
        lines.append("  player %d: %s; said: %s" % (i + 1, fate, " | ".join(said[i][2:]) or "-"))

    # order: the dispatched lists agree as far as the shortest goes
    taken = [[e[1:] for e in m[1] if e[0] <= end or not ends] for m in messages]
    shortest = min(len(t) for t in taken) if taken else 0
    first = next((i for i in range(shortest) if any(t[i] != taken[0][i] for t in taken)), None)
    order = first is None and shortest > 0
    lines.append("  order: %s (%d compared; dispatched: %s)" % (
        "the same on every player" if order else "DIFFERENT from message %s" % (None if first is None else first + 1),
        shortest, ", ".join(str(len(t)) for t in taken)))

    # once and complete: each sender's handed-over list against what every player dispatched of that sender
    once = complete = True
    for sender in range(players):
        handed = [e[1:] for e in messages[sender][0]]
        due = len([e for e in messages[sender][0] if e[0] <= end - 2000])
        for viewer in range(players):
            got = [e for e in taken[viewer] if e[0] == sender]
            if got != handed[:len(got)]:
                once = False
                at = next((i for i in range(min(len(got), len(handed))) if got[i] != handed[i]), min(len(got), len(handed)))
                lines.append("  once: player %d dispatched of player %d something else than it handed over, from its message %d (%d dispatched, %d handed over)"
                             % (viewer + 1, sender + 1, at + 1, len(got), len(handed)))
            elif survived[viewer] and len(got) < due:
                complete = False
                lines.append("  complete: player %d lacks %d of the %d messages player %d handed over in time"
                             % (viewer + 1, due - len(got), due, sender + 1))
    if not all(survived):
        complete = False
    # Were the others disturbed by the return? How long a message of a player that was never cut off took to
    # be dispatched by another such player (their clocks are one clock here), before the cut and in the ten
    # seconds after the return, when the returning player is given all it missed.
    if cut and back and who not in (1, 9):
        before, after = [], []
        for sender in range(players):
            for viewer in range(players):
                if sender + 1 == who or viewer + 1 == who:
                    continue
                arrived = [e[0] for e in messages[viewer][1] if e[1] == sender]
                for handed, taken_at in zip(messages[sender][0], arrived):
                    if handed[0] < cut:
                        before.append(taken_at - handed[0])
                    elif back <= handed[0] < back + 10000:
                        after.append(taken_at - handed[0])
        if before and after:
            middle = lambda times: sorted(times)[len(times) // 2]
            lines.append("  the others' messages among themselves took %d ms (median; longest %d) before the cut, %d ms (longest %d) in the 10 s after the return"
                         % (middle(before), max(before), middle(after), max(after)))

    # state: every view of another player against that player's own, after the connection is back
    start = (back if back else (min(min(s) for s in states if s) * 250 if any(states) else 0)) + 5000
    frozen, differ, rest_total, rest_same = [], [], 0, 0
    for viewer in range(players):
        for other in range(players):
            if viewer == other or not survived[viewer] or not survived[other]:
                continue
            own, seen = states[other], states[viewer]
            quarters = [q for q in sorted(own) if start <= q * 250 <= end and q in seen and q - 1 in own and q + 1 in own]
            own_values = {own[q][1][other] for q in quarters}
            seen_values = {seen[q][1][other] for q in quarters}
            if len(own_values) >= 3 and len(seen_values) <= 1:
                frozen.append("%d's view of %d" % (viewer + 1, other + 1))
            total = same = 0
            for q in quarters:
                # at rest a quarter of a second before and after: the two programs take their samples a few
                # frames apart, and what one has of the other is a few frames old
                if own[q][1][other] == own[q - 1][1][other] == own[q + 1][1][other]:
                    total += 1
                    same += seen[q][1][other] == own[q][1][other]
            rest_total += total
            rest_same += same
            if total and same < total * 0.95:
                differ.append("%d's view of %d (%d of %d)" % (viewer + 1, other + 1, same, total))
    state = not frozen and not differ and rest_total > 0
    lines.append("  state after the return: %s; equal at rest %d of %d samples%s" % (
        "FROZEN: " + ", ".join(frozen) if frozen else "nothing frozen", rest_same, rest_total,
        "; DIFFERENT: " + ", ".join(differ) if differ else ""))
    gone_end = []
    for i, s in enumerate(states):
        quarters = [q for q in s if q * 250 <= end]
        gone_end.append(s[max(quarters)][0] if quarters and survived[i] else 0)
    gone = not any(gone_end)
    if not gone:
        lines.append("  gone at the end: " + ", ".join("player %d counts %s as departed" % (
            i + 1, "+".join(str(b + 1) for b in range(4) if g >> b & 1)) for i, g in enumerate(gone_end) if g))
    # place and tokens: the last sample of every player's scene log before the end
    scenes = [read_scenes(os.path.join(run, "p%d" % p, "scene.log")) for p in range(1, players + 1)]
    last = [next((s for s in reversed(log) if s[0] * 250 <= end), None) if survived[i] else None for i, log in enumerate(scenes)]
    places = {s[1] for s in last if s}
    place = len(places) <= 1
    tokens = not any(sum(s[2]) for s in last if s)
    if any(scenes):
        name = lambda where: "the world map" if where[2] else "map %d area %d" % where[:2]
        lines.append("  place at the end: " + ", ".join("player %d %s" % (i + 1, name(s[1]) if s else "-") for i, s in enumerate(last))
                     + ("" if place else "  DIFFERENT"))
        if not tokens:
            lines.append("  tokens LEFT OVER: " + ", ".join("player %d holds %s" % (
                i + 1, " and ".join("%d of player %d" % (n, b + 1) for b, n in enumerate(s[2]) if n)) for i, s in enumerate(last) if s and sum(s[2])))
    checks = dict(survived=all(survived), order=order, once=once, complete=complete, state=state, gone=gone, place=place, tokens=tokens)
    healed = all(checks.values())
    print("RESULT %s: %d players, %s cut off for %d s: %s  [%s]" % (
        os.path.basename(os.path.normpath(run)), players,
        "nobody" if not who else "everybody" if who == 9 else "player %d" % who, seconds, "HEALED" if healed else "NOT HEALED",
        " ".join("%s=%s" % (k, "yes" if v else "NO") for k, v in checks.items())))
    print("\n".join(lines))
    return (players, who, seconds), healed, checks


def main():
    table = defaultdict(list)
    for run in sys.argv[1:]:
        if not os.path.exists(os.path.join(run, "case.txt")):
            continue
        key, healed, checks = report(run)
        table[key].append((healed, checks))
    if len(sys.argv) > 2:
        print()
        print("players  cut off      runs  healed  survived  order  once  complete  state  gone  place  tokens")
        for key in sorted(table):
            runs = table[key]
            count = lambda name: sum(1 for _, c in runs if c[name])
            print("%7d  %-11s  %4d  %6d  %8d  %5d  %4d  %8d  %5d  %4d  %5d  %6d" % (
                key[0], "nobody" if not key[1] else "%s %2d s" % ("host" if key[1] == 1 else "everybody" if key[1] == 9 else "player %d" % key[1], key[2]),
                len(runs), sum(1 for h, _ in runs if h), count("survived"), count("order"), count("once"),
                count("complete"), count("state"), count("gone"), count("place"), count("tokens")))
    return 0 if all(h for runs in table.values() for h, _ in runs) else 1


if __name__ == "__main__":
    sys.exit(main())
