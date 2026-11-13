#!/usr/bin/env python3
"""net_join_report.py RUN_FOLDER... : what became of a program that joined a game in progress.

RUN_FOLDER is what tools/net_join_test.sh wrote (build/join/TAG): case.txt, relay.log and per program pN/run.log,
pN/msg.log (SS_NET_MSG_LOG), pN/net.log (SS_NET_LOG with the direct transport) and pN/state.log (SS_STATE_LOG,
runtime/core/frame.c). Programs 1..PLAYERS started the session; the last one is the newcomer.

Asked of every run, from two seconds after the newcomer was let in to a second and a half before the first
program ended:
  survived   every program ran to its last frame and none was reset by the game
  let in     the host let the newcomer in, every other program gave it a seat, and the newcomer's game entered
             the map it was told
  map        the newcomer's game is in the same map and area as the host's (state.log, sampled at the same
             moments of the clock) nearly always and at the end, and every program counts the same seats
  order      what the newcomer dispatched is, message for message (sender, kind, contents), a run of what the
             host dispatched from the newcomer's first message on; and the others' lists agree with the host's
  once       of every sender, every program dispatched exactly what that sender handed over, in that order
             (the newcomer: from the point of the order where it was let in)
  seen       what the others are given of the newcomer is the newcomer's own state, moves when it moves and is
             equal whenever it stands still, and the same for what the newcomer is given of each other player
  gone       no program counts another as departed at the end
A run in which all seven hold is JOINED. One line per run starts with RESULT.

Exit status 1 if any run is not JOINED."""
import os, re, sys


def read_messages(path):
    sent, taken, own = [], [], []
    if os.path.exists(path):
        for line in open(path, errors="replace"):
            f = line.split()
            if len(f) >= 7:
                entry = (int(f[0]), int(f[3]), f[4], f[6])      # clock, sender, kind, hash of the contents
                (sent if f[2] == "S" else taken).append(entry)
            elif len(f) >= 5 and f[2] in "JP":
                own.append((int(f[0]), f[2], int(f[3]), f[4]))   # this layer's own entries; tokens given again
    return sent, taken, own


def read_blocks(path):
    """quarter -> (gone mask, [state bytes of seat 0..3 or None])"""
    found = {}
    if os.path.exists(path):
        for line in open(path, errors="replace"):
            f = line.split()
            if len(f) == 8 and f[0] == "Q":
                found[int(f[1])] = (int(f[3], 16), [None if v == "-" else v for v in f[4:8]])
    return found


def read_places(path):
    """[(quarter, frame, map, area, seats mask)]"""
    found = []
    if os.path.exists(path):
        for line in open(path, errors="replace"):
            f = line.split()
            if len(f) >= 8 and f[7] == "|":
                found.append((int(f[0]), int(f[1]), int(f[4]), int(f[5]), int(f[6], 16)))
    return found


def run_of(part, whole):
    """index in `whole` at which `part` (as far as `whole` reaches) is a contiguous run, or None"""
    if not part:
        return 0
    for start in range(len(whole)):
        if whole[start] == part[0]:
            length = min(len(part), len(whole) - start)
            if whole[start:start + length] == part[:length]:
                return start
    return None


def report(run):
    case = {}
    words = open(os.path.join(run, "case.txt")).read().split()
    for name, value in zip(words[0::2], words[1::2]):
        case[name] = value
    players, frames, join_frames = int(case["players"]), int(case["frames"]), int(case["jframes"])
    left = int(case.get("leave", 0)) - 1    # index of a player whose program was ended during the run, -1 if none
    programs = players + 1
    newcomer = players                      # index of the newcomer's program
    lines, said, last_frame, reset = [], [], [], []
    for p in range(1, programs + 1):
        text = open(os.path.join(run, "p%d" % p, "run.log"), errors="replace").read()
        numbers = re.findall(r"^frame +(\d+) +hash", text, re.M)
        last_frame.append(int(numbers[-1]) if numbers else 0)
        reset.append("SoftReset" in text)
        said.append([l[5:] for l in text.splitlines() if l.startswith("net: ")])
    messages = [read_messages(os.path.join(run, "p%d" % p, "msg.log")) for p in range(1, programs + 1)]
    blocks = [read_blocks(os.path.join(run, "p%d" % p, "net.log")) for p in range(1, programs + 1)]
    places = [read_places(os.path.join(run, "p%d" % p, "state.log")) for p in range(1, programs + 1)]
    want = [frames] * players + [join_frames]
    survived = [i == left or (last_frame[i] >= want[i] and not reset[i]) for i in range(programs)]
    last_block = [max(b) * 250 if b else 0 for b in blocks]
    end = min(t for i, t in enumerate(last_block) if i != left) - 1500
    for i in range(programs):
        if i == left:
            lines.append("  player %d: its program was ended at frame %d (as the test asked); said: %s" % (
                i + 1, last_frame[i], " | ".join(said[i][2:]) or "-"))
            continue
        fate = "ran to frame %d" % last_frame[i] if survived[i] else "ENDED at frame %d of %d%s" % (
            last_frame[i], want[i], " (the game reset itself)" if reset[i] else "")
        lines.append("  %s %d: %s; said: %s" % ("newcomer" if i == newcomer else "player", i + 1, fate, " | ".join(said[i][2:]) or "-"))

    # let in
    seat = None
    for l in said[newcomer]:
        m = re.match(r"seat (\d+) of room", l)
        if m:
            seat = int(m.group(1)) - 1
    entered = next((l for l in said[newcomer] if l.startswith("this player enters map")), None)
    host_let = any("is let in at this change of map" in l for l in said[0])
    gave = [i == left or any("joined the game in progress" in l for l in said[i]) for i in range(players)]
    let_in = bool(entered) and host_let and all(gave) and seat is not None
    admitted = next((e[0] for e in messages[newcomer][2] if e[1] == "J" and e[3] == "F5"), None)
    # (the host put the newcomer into the order a little earlier than the newcomer learnt of it)
    let_at = max([e[0] for e in messages[0][2] if e[1] == "J" and e[3] == "F2" and e[0] <= (admitted or 0)] or [admitted or 0])
    began = min(blocks[newcomer]) * 250 if blocks[newcomer] else None
    parked = None
    for line in open(os.path.join(run, "p%d" % programs, "msg.log"), errors="replace"):
        f = line.split()
        if len(f) >= 7 and f[2] == "S":
            parked = int(f[0])              # its game's first messages: it is in the session's start
            break
    lines.append("  let in: %s%s" % (
        ("seat %d; %s" % (seat + 1, entered)) if let_in else "NO (host: %s, the others: %s, the newcomer: %s)" % (
            host_let, gave, entered),
        "" if not (admitted and began) else "; %.1f s after its link opened (its game had started its session after %.1f s)" % (
            (admitted - began) / 1000.0, ((parked or admitted) - began) / 1000.0)))
    start = (admitted or 0) + 2000

    # map: the newcomer's place against the host's at the same moments of the clock
    def place_at(samples, quarter):
        best = None
        for s in samples:
            if s[0] > quarter:
                break
            best = s
        return best
    total = same = changing = 0
    visited, final = [], None
    for sample in places[newcomer]:
        if sample[0] * 250 < start or sample[0] * 250 > end:
            continue
        host = place_at(places[0], sample[0])
        if not host:
            continue
        total += 1
        final = (sample[2:4], host[2:4])
        if sample[2:4] == host[2:4]:
            same += 1
            if not visited or visited[-1] != sample[2:4]:
                visited.append(sample[2:4])
        else:
            # the programs do not change map in the same frame: a second around the host's change does not count
            before, after = place_at(places[0], sample[0] - 4), place_at(places[0], sample[0] + 4)
            changing += bool(before and after and before[2:4] != after[2:4])
    seats = set()
    for i in range(programs):
        if i == left:
            continue
        at_end = place_at(places[i], end // 250)
        seats.add(at_end[4] if at_end else None)
    same_map = total > 0 and same + changing == total and final is not None and final[0] == final[1] and len(seats) == 1
    lines.append("  map: the newcomer and the host in the same map and area in %d of %d samples (%d more within a second of a change of map); together in %s; at the end %s; seats counted: %s" % (
        same, total, changing, ", ".join("%d/%d" % v for v in visited) or "-",
        "-" if not final else ("both in %d/%d" % final[0] if final[0] == final[1] else "newcomer %d/%d, host %d/%d" % (final[0] + final[1])),
        ", ".join("%X" % s if s is not None else "?" for s in sorted(seats, key=lambda v: -1 if v is None else v))))

    # order
    taken = [[e[1:] for e in m[1] if e[0] <= end] for m in messages]
    at = run_of(taken[newcomer], taken[0])
    others_first = None
    for i in range(1, players):
        if i == left:
            continue
        shortest = min(len(taken[0]), len(taken[i]))
        bad = next((n for n in range(shortest) if taken[0][n] != taken[i][n]), None)
        if bad is not None:
            others_first = (i + 1, bad + 1)
    order = at is not None and len(taken[newcomer]) > 0 and others_first is None
    lines.append("  order: the newcomer's %d dispatched messages %s; the players that started the session %s (dispatched: %s)" % (
        len(taken[newcomer]),
        "are the host's from its message %d on" % (at + 1) if at is not None else "are NOT a run of the host's",
        "agree" if others_first is None else "DIFFER (player %d from message %d)" % others_first,
        ", ".join(str(len(t)) for t in taken)))

    # once and complete
    once = complete = True
    for sender in range(programs):
        if sender == left:
            continue                        # (what it handed over last may never have reached the host)
        sender_seat = seat if sender == newcomer else sender
        handed = [e[1:] for e in messages[sender][0]]
        due = len([e for e in messages[sender][0] if e[0] <= end - 2000])
        for viewer in range(programs):
            if viewer == left:
                continue
            got = [e for e in taken[viewer] if e[0] == sender_seat]
            if sender == newcomer:
                # (if the newcomer took the seat of a player that left, what that one handed over came before)
                got = [e[1:] for e in messages[viewer][1] if e[1] == sender_seat and let_at <= e[0] <= end]
            if viewer == newcomer and sender != newcomer:
                where = run_of(got, handed)
                if where is None:
                    once = False
                    lines.append("  once: the newcomer dispatched of player %d something that is not a run of what it handed over" % (sender + 1))
                elif len(got) + where < due:
                    complete = False
                    lines.append("  complete: the newcomer lacks %d messages of player %d" % (due - len(got) - where, sender + 1))
                continue
            if got != handed[:len(got)]:
                once = False
                bad = next((n for n in range(min(len(got), len(handed))) if got[n] != handed[n]), min(len(got), len(handed)))
                lines.append("  once: program %d dispatched of program %d something else than it handed over, from its message %d (%d dispatched, %d handed over)"
                             % (viewer + 1, sender + 1, bad + 1, len(got), len(handed)))
            elif survived[viewer] and len(got) < due:
                complete = False
                lines.append("  complete: program %d lacks %d of the %d messages program %d handed over in time"
                             % (viewer + 1, due - len(got), due, sender + 1))
    once = once and complete

    # seen: every view of another player against that player's own, after the newcomer was let in
    frozen, differ, notes = [], [], []
    rest_total = rest_same = 0
    seat_of = list(range(players)) + [seat if seat is not None else players]
    for viewer in range(programs):
        for other in range(programs):
            if viewer == other or newcomer not in (viewer, other) or left in (viewer, other):
                continue
            column = seat_of[other]
            own, seen = blocks[other], blocks[viewer]
            quarters = [q for q in sorted(own) if start + 3000 <= q * 250 <= end and q in seen and q - 1 in own and q + 1 in own]
            own_values = {own[q][1][column] for q in quarters}
            seen_values = {seen[q][1][column] for q in quarters}
            if len(own_values) >= 3 and len(seen_values) <= 1:
                frozen.append("%d's view of %d" % (viewer + 1, other + 1))
            total = same = 0
            for q in quarters:
                if own[q][1][column] == own[q - 1][1][column] == own[q + 1][1][column]:
                    total += 1
                    same += seen[q][1][column] == own[q][1][column]
            rest_total += total
            rest_same += same
            if total and same < total * 0.95:
                differ.append("%d's view of %d (%d of %d)" % (viewer + 1, other + 1, same, total))
            notes.append("%d sees %d in %d states (%d of its own)" % (viewer + 1, other + 1, len(seen_values - {None}), len(own_values - {None})))
    moved = all(int(n.split(" in ")[1].split()[0]) >= 3 for n in notes) if notes else False
    seen_ok = not frozen and not differ and rest_total > 0 and moved
    lines.append("  seen: %s; equal at rest %d of %d samples%s; %s" % (
        "FROZEN: " + ", ".join(frozen) if frozen else "nothing frozen", rest_same, rest_total,
        "; DIFFERENT: " + ", ".join(differ) if differ else "", "; ".join(notes)))

    gone_end = []
    for i, b in enumerate(blocks):
        quarters = [q for q in b if q * 250 <= end]
        gone_end.append(b[max(quarters)][0] if quarters and survived[i] and i != left else 0)
    gone = not any(gone_end)
    if not gone:
        lines.append("  gone at the end: " + ", ".join("program %d counts %s as departed" % (
            i + 1, "+".join(str(b + 1) for b in range(4) if g >> b & 1)) for i, g in enumerate(gone_end) if g))
    checks = dict(survived=all(survived), let_in=let_in, map=same_map, order=order, once=once, seen=seen_ok, gone=gone)
    joined = all(checks.values())
    print("RESULT %s: %d players and a newcomer %s s later: %s  [%s]" % (
        os.path.basename(os.path.normpath(run)), players, case.get("join", "?"), "JOINED" if joined else "NOT JOINED",
        " ".join("%s=%s" % (k, "yes" if v else "NO") for k, v in checks.items())))
    print("\n".join(lines))
    return joined


def main():
    results = [report(run) for run in sys.argv[1:] if os.path.exists(os.path.join(run, "case.txt"))]
    if len(results) > 1:
        print("\n%d of %d runs JOINED" % (sum(results), len(results)))
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
