#!/usr/bin/env python3
"""net_exit_report.py RUN_FOLDER... : what became of a session in a dungeon level (tools/net_exit_test.sh).

RUN_FOLDER (build/exits/TAG) holds case.txt and per program pN/run.log, pN/state.log (SS_STATE_LOG), pN/msg.log
(SS_NET_MSG_LOG with SS_NET_MSG_DUMP), pN/enemy.log (SS_ENEMY_LOG), all of runtime/core.

Per program: the maps and areas its game was in, with the frame of each change; how its player's character
fared (struck, defeated); what its program said about the session. For the session:
  fights     the messages every player dispatched, by kind (the game's link messages, games/ss2/decomp
             docs/LINK_MESSAGES.md): enemies that appeared (0x86), damage to an enemy or a player (0x8A), enemies
             that fell (0x89), items asked for (0x95) and made (0x98), the three steps of picking one up (0x99,
             0x9A, 0x9B), a player's flags, which tell its defeat (0x8C), the host's change of map (0x8D)
  survived   every program that was to run to the end did
  exit       every such program left the first level (map 1 area 0) for the next one after the event of the
             case (a player gone, the start of an outage), within two seconds of the clock of the others
             (the players count their frames from different starts, so frames are not compared)
  order      they dispatched the same messages in the same order, as far as the shortest list goes
  enemies    at the last quarter of a second all of them sampled, they had the same live enemies (identifier
             and kind)
  hunted     for a player that is gone, from the host's logs after it gave the player up: of the samples (four
             a second) with a live enemy, at how many an enemy stood within 28 pixels of the standing
             character, and at how many the standing character was the player nearest to some enemy. Enemies
             that go for it gather there and stay (reported, not judged; the game's blocks carry each
             player's state and place: pN/net.log)
A run in which survived, exit and order hold is PASSED; one line per run starts with RESULT. With `how idle`
and every player idle (the party's defeat) the question is instead whether every program left the level for
the town: DEFEATED.

Exit status 1 if any run did not pass."""
import os, re, sys
from collections import Counter

NAMES = {"86": "enemy appears", "8A": "damage", "89": "enemy falls", "95": "item drop asked", "98": "item appears",
         "99": "pick-up asked", "9A": "pick-up answered", "9B": "item removed", "8C": "player's flags",
         "93": "exit opens", "8D": "change of map", "9F": "everybody changes map"}


def places(path):
    """[(frame, map, area)] at every change"""
    found = []
    if os.path.exists(path):
        for line in open(path, errors="replace"):
            f = line.split()
            if len(f) >= 8 and f[7] == "|":
                place = (int(f[4]), int(f[5]))
                if not found or found[-1][1:3] != place:
                    found.append((int(f[1]),) + place + (int(f[0]),))
    return found


def fate(path, seat):
    """(times struck, frame of the defeat or None, last frame) of that seat's character in this program's log"""
    struck, defeat, last, before = 0, None, 0, None
    if os.path.exists(path):
        for line in open(path, errors="replace"):
            f = line.split()
            if len(f) < 8 + 4 * (seat + 1) or f[7] != "|":
                continue
            state, flags = int(f[8 + 4 * seat]), int(f[9 + 4 * seat], 16)
            last = int(f[1])
            if state == 10 and before != 10:
                struck += 1
            if flags & 0x2000 and defeat is None:
                defeat = last
            before = state
    return struck, defeat, last


def messages(path):
    """dispatched: [(sender, kind, hash)]"""
    taken = []
    if os.path.exists(path):
        for line in open(path, errors="replace"):
            f = line.split()
            if len(f) >= 7 and f[2] == "R" and f[4] != "FE":
                taken.append((f[3], f[4], f[6], f[7] if len(f) > 7 else "", int(f[0])))
    return taken


def last_enemies(paths):
    """the last quarter every log has: per log the set of (identifier, kind) alive"""
    logs = []
    for path in paths:
        found = {}
        if os.path.exists(path):
            for line in open(path, errors="replace"):
                parts = line.strip().split(" | ")
                head = parts[0].split()
                if len(head) >= 4:
                    found[int(head[0])] = frozenset(tuple(part.split()[1:3]) for part in parts[1:])
        logs.append(found)
    common = set(logs[0]) if logs else set()
    for found in logs[1:]:
        common &= set(found)
    if not common:
        return None, []
    quarter = max(common)
    return quarter, [found[quarter] for found in logs]


def report(run):
    words = open(os.path.join(run, "case.txt")).read().split()
    case = dict(zip(words[0::2], words[1::2]))
    players, how, who, at = int(case["players"]), case["how"], int(case["who"]), int(case["at"])
    frames = int(case["frames"])
    tag = case["tag"]
    killed = int(case.get("kill", "0"))
    stays = [i for i in range(1, players + 1) if not (how == "quit" and i == who) and i != killed]
    lines, ok = [], {}
    went, ends, lists = {}, {}, {}
    for i in range(1, players + 1):
        folder = os.path.join(run, "p%d" % i)
        text = open(os.path.join(folder, "run.log"), errors="replace").read() if os.path.exists(os.path.join(folder, "run.log")) else ""
        frame = [int(v) for v in re.findall(r"^frame (\d+)", text, re.M)]
        ends[i] = frame[-1] if frame else 0
        said = [s.strip() for s in re.findall(r"^net: (.*)$", text, re.M)]
        went[i] = places(os.path.join(folder, "state.log"))
        lists[i] = messages(os.path.join(folder, "msg.log"))
        struck, defeat, _ = fate(os.path.join(folder, "state.log"), i - 1)
        lines.append("  player %d: ran to frame %d; maps %s; struck %d times%s; said: %s" % (
            i, ends[i], " ".join("%d/%d@%d" % (m, a, f) for f, m, a, _ in went[i]) or "-", struck,
            ", defeated at frame %d" % defeat if defeat else "", " | ".join(said[-6:]) or "-"))
    ok["survived"] = all(ends[i] >= frames for i in stays)
    # the exit of the first level
    left = {}
    for i in stays:
        inside = False
        for f, m, a, clock in went[i]:
            if (m, a) == (1, 0):
                inside = True
            elif inside:
                left[i] = (f, m, a, clock)
                break
    if left and len(left) == len(stays) and all(v[1] == 0 for v in left.values()):
        verdict_exit = "every player left the level for the town (defeat) at frames %s" % ", ".join(str(left[i][0]) for i in stays)
        ok["exit"] = True
    elif len(left) == len(stays) and left:
        first, last = min(v[3] for v in left.values()), max(v[3] for v in left.values())
        same = len(set(v[1:3] for v in left.values())) == 1
        early = who in left and left[who][0] < at
        ok["exit"] = same and last - first <= 8 and not early
        verdict_exit = "left map 1 area 0 for %s at frames %s (%.2f s apart)%s" % (
            " and ".join("%d/%d" % v for v in sorted(set(v[1:3] for v in left.values()))),
            ", ".join(str(left[i][0]) for i in stays), (last - first) / 4.0, " (before the event at %d)" % at if early else "")
    else:
        ok["exit"] = False
        verdict_exit = "still in map 1 area 0 at the end: %s" % ", ".join("player %d" % i for i in stays if i not in left) \
            if any(any((m, a) == (1, 0) for f, m, a, _ in went[i]) for i in stays) else "the level was never reached"
    # the order of the messages
    shortest = min((len(lists[i]) for i in stays), default=0)
    differ = next((n for n in range(shortest) if len(set(lists[i][n][:3] for i in stays)) > 1), None)
    ok["order"] = differ is None and shortest > 0
    kinds = Counter(entry[1] for entry in lists[stays[0]][:shortest]) if stays else Counter()
    fights = ", ".join("%s %d (%s)" % (kind, kinds[kind], NAMES[kind]) for kind in sorted(NAMES) if kinds.get(kind))
    quarter, alive = last_enemies([os.path.join(run, "p%d" % i, "enemy.log") for i in stays])
    ok["enemies"] = quarter is not None and len(set(alive)) == 1
    if case.get("idle_all") == "1":
        result = "DEFEATED" if ok["survived"] and ok["order"] and ok["exit"] and "defeat" in verdict_exit else "FAILED"
    else:
        result = "PASSED" if ok["survived"] and ok["exit"] and ok["order"] else "FAILED"
    print("RESULT %s: %d players, %s%s: %s  [%s]" % (
        tag, players, how, (" player %d at frame %d" % (who, at) if who else "")
        + (", player %d killed" % killed if killed and how != "kill" else ""), result,
        " ".join("%s=%s" % (k, "yes" if v else "no") for k, v in ok.items())))
    for line in lines:
        print(line)
    print("  fights (dispatched by all, %d messages compared%s): %s" % (
        shortest, "" if differ is None else "; FIRST DIFFERENCE at message %d" % differ, fights or "none"))
    print("  exit: %s" % verdict_exit)
    # the enemies and the standing character of a player that is gone, as the host has them
    for gone in sorted(set([killed] + ([who] if how == "quit" else [])) - {0}):
        blocks, since = {}, None
        log = os.path.join(run, "p1", "net.log")
        if os.path.exists(log):
            for line in open(log, errors="replace"):
                f = line.split()
                if len(f) == 8 and f[0] == "Q":
                    if since is None and int(f[3], 16) >> (gone - 1) & 1:
                        since = int(f[1])
                    where = []
                    for text in f[4:8]:
                        if text == "-":
                            where.append(None)
                        else:
                            word = int.from_bytes(bytes.fromhex(text[:8]), "little")
                            where.append((word >> 4 & 0xFFF, word >> 16 & 0xFFF, word & 15))
                    blocks[int(f[1])] = where
        if since is None:
            print("  hunted: the host never counted player %d as gone" % gone)
            continue
        samples = beside = nearest = 0
        log = os.path.join(run, "p1", "enemy.log")
        for line in open(log, errors="replace") if os.path.exists(log) else []:
            parts = line.strip().split(" | ")
            head = parts[0].split()
            if len(head) < 4 or int(head[0]) < since or int(head[0]) not in blocks or len(parts) < 2:
                continue
            where = blocks[int(head[0])]
            if where[gone - 1] is None:
                continue
            samples += 1
            is_beside = is_nearest = False
            for part in parts[1:]:
                f = part.split()
                x, y = int(f[3]), int(f[4])
                far = [abs(x - w[0]) + abs(y - w[1]) if w else 1 << 30 for w in where]
                is_beside = is_beside or far[gone - 1] <= 28
                is_nearest = is_nearest or far[gone - 1] == min(far)
            beside += is_beside
            nearest += is_nearest
        print("  hunted: of %d samples with enemies after player %d was gone, an enemy stood beside its character at %d, "
              "and it was the nearest player to an enemy at %d; the game is given state %s for it" % (
                  samples, gone, beside, nearest, blocks[max(blocks)][gone - 1][2] if blocks and blocks[max(blocks)][gone - 1] else "-"))
    if quarter is not None:
        print("  enemies at the last common sample: %s" % ("the same on every player (%d alive)" % len(alive[0]) if ok["enemies"]
              else "DIFFERENT: " + " / ".join(",".join(sorted("%s:%s" % e for e in s)) or "none" for s in alive)))
    return result != "FAILED"


def main():
    good = True
    for run in sys.argv[1:]:
        if not os.path.exists(os.path.join(run, "case.txt")):
            print("%s: no case.txt" % run)
            good = False
            continue
        good = report(run) and good
    sys.exit(0 if good else 1)


main()
