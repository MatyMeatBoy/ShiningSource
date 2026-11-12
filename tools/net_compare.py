#!/usr/bin/env python3
"""Compare the SS_NET_LOG files of several players of one session.

Each line is `label players hash`: the timeline frame, the set of players whose blocks were delivered, and a
hash of those blocks. Two players that were given the same set at the same label must have the same hash.
"""
import sys
from collections import Counter


def main():
    logs = []
    for path in sys.argv[1:]:
        frames = {}
        for line in open(path):
            parts = line.split()
            if len(parts) == 3:
                frames[int(parts[0])] = (parts[1], parts[2])
        logs.append(frames)
    labels = set().union(*logs) if logs else set()
    shared = same = different = 0
    sets = Counter()
    first_bad = None
    for label in sorted(labels):
        seen = [log[label] for log in logs if label in log]
        if len(seen) < 2:
            continue
        shared += 1
        by_set = {}
        for players, digest in seen:
            by_set.setdefault(players, set()).add(digest)
        sets[max(by_set, key=lambda p: bin(int(p, 16)).count("1"))] += 1
        if any(len(d) > 1 for d in by_set.values()):
            different += 1
            first_bad = first_bad or label
        else:
            same += 1
    print("%d labels delivered to two or more players: %d consistent, %d different%s"
          % (shared, same, different, "" if first_bad is None else " (first at %d)" % first_bad))
    print("largest player set per label:", ", ".join("%s x%d" % kv for kv in sorted(sets.items())))
    return 1 if different else 0


if __name__ == "__main__":
    sys.exit(main())
