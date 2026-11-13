#!/usr/bin/env python3
"""enemy_compare.py RUN_FOLDER... : do the players of an online test session have the same enemies?

Each RUN_FOLDER holds one folder per player (p1, p2, ...) with the files a test run writes:
  enemy.log  (SS_ENEMY_LOG, runtime/core/frame.c)   the live enemies, four samples a second, taken at the same
             moments of the clock on every machine of the computer
  msg.log    (SS_NET_MSG_LOG, runtime/core/netlink.c) every message handed over (S) and dispatched (R)

Reported per run:
  - samples: moments at which every player had enemy records and at least one of them a live enemy
  - same enemies: samples at which every player had the same live enemies, by the identifier their spawn gave
    them and their kind; "same records" also asks for the same record (slot) for each
  - the longest stretch of samples in a row that disagreed (a spawn or a death reaches the players a few frames
    apart, so single samples may disagree; a stretch of seconds is a session that drifted apart)
  - the largest distance between two players' positions of one enemy, and how many samples had one further
    apart than 48 pixels
  - dispatch order: whether every player dispatched the messages in the same order (sender and tag, own
    messages included), and where the first difference is if not

Exit status 1 if any run disagrees for two seconds or longer, or its dispatch order differs."""
import glob, os, sys

FAR = 48            # pixels: further apart than latency explains
DRIFT = 8           # samples in a row (two seconds)


def enemies(path):
    samples = {}
    for line in open(path):
        parts = line.strip().split(" | ")
        head = parts[0].split()
        if len(head) < 4:
            continue
        found = {}
        for part in parts[1:]:
            f = part.split()
            if len(f) >= 7:
                slot, ident, kind, x, y, health, state = (int(v) for v in f[:7])
                found.setdefault((ident, kind), []).append((slot, x, y, health, state))
        samples[int(head[0])] = found
    return samples


def dispatched(path):
    order = []
    for line in open(path):
        f = line.split()
        if len(f) >= 5 and f[2] == "R":
            order.append((int(f[3]), f[4]))
    return order


def main():
    bad = 0
    for run in sys.argv[1:]:
        players = sorted(glob.glob(os.path.join(run, "p*")))
        logs = [enemies(os.path.join(p, "enemy.log")) for p in players if os.path.exists(os.path.join(p, "enemy.log"))]
        print(os.path.basename(os.path.normpath(run)) + ": %d players" % len(players))
        if len(logs) == len(players) and logs:
            common = sorted(set.intersection(*(set(l) for l in logs)))
            common = [q for q in common if any(l[q] for l in logs)]
            same = same_records = far = 0
            stretch = longest = 0
            longest_at = None
            largest = (0, None, None)
            kinds = set()
            lives = set()
            for q in common:
                views = [l[q] for l in logs]
                kinds.update(k for v in views for k in v)
                agree = all(set(v) == set(views[0]) and all(len(v[k]) == len(views[0][k]) for k in v) for v in views)
                if agree:
                    same += 1
                    stretch = 0
                    if all(sorted(e[0] for e in v[k]) == sorted(e[0] for e in views[0][k]) for v in views for k in v):
                        same_records += 1
                    worst = 0
                    for key in views[0]:
                        if any(len(v[key]) != 1 for v in views):
                            continue
                        xs = [v[key][0][1] for v in views]
                        ys = [v[key][0][2] for v in views]
                        distance = max(max(xs) - min(xs), max(ys) - min(ys))
                        worst = max(worst, distance)
                        if distance > largest[0]:
                            largest = (distance, key, q)
                    far += worst > FAR
                else:
                    stretch += 1
                    if stretch > longest:
                        longest, longest_at = stretch, q
            # how many enemies lived: a new one each time an identifier appears on player 1 after being absent
            before = set()
            born = 0
            for q in common:
                now = set(logs[0][q])
                born += len(now - before)
                before = now
            print("  samples %d (%.0f s), enemies born on player 1: %d, kinds seen: %d" % (len(common), len(common) / 4.0, born, len({k[1] for k in kinds})))
            if common:
                print("  same enemies on every player: %d of %d (%.1f%%); same records too: %d" % (
                    same, len(common), 100.0 * same / len(common), same_records))
                print("  longest disagreement: %d samples in a row (%.2f s)%s" % (
                    longest, longest / 4.0, " ending at sample %d" % longest_at if longest_at else ""))
                print("  largest distance between two players' positions of one enemy: %d px%s; samples with one over %d px: %d" % (
                    largest[0], " (enemy %s at sample %s)" % (largest[1], largest[2]) if largest[1] else "", FAR, far))
                if longest >= DRIFT:
                    bad = 1
        else:
            print("  no enemy.log for every player")
        orders = [dispatched(os.path.join(p, "msg.log")) for p in players if os.path.exists(os.path.join(p, "msg.log"))]
        if len(orders) == len(players) and orders:
            shortest = min(len(o) for o in orders)
            first = next((i for i in range(shortest) if any(o[i] != orders[0][i] for o in orders)), None)
            per_sender = all([m for m in o[:shortest] if m[0] == s] [:min(len([m for m in p[:shortest] if m[0] == s]) for p in orders)]
                             == [m for m in orders[0][:shortest] if m[0] == s][:min(len([m for m in p[:shortest] if m[0] == s]) for p in orders)]
                             for o in orders for s in range(4))
            if first is None:
                print("  dispatch order: identical on every player (%d messages compared; dispatched: %s)" % (
                    shortest, ", ".join(str(len(o)) for o in orders)))
            else:
                print("  dispatch order: DIFFERENT from message %d of %d on (%s); per sender the same: %s" % (
                    first + 1, shortest, " / ".join("p%d %d:%s" % (i + 1, o[first][0], o[first][1]) for i, o in enumerate(orders)),
                    "yes" if per_sender else "NO"))
                bad = 1
        else:
            print("  no msg.log for every player")
    return bad


if __name__ == "__main__":
    sys.exit(main())
