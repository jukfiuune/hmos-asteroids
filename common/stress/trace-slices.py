#!/usr/bin/env python3
"""Slices from a raw ftrace with HarmonyOS trace points (common/stress ftrace capture of jank.sh):

    trace-slices.py <trace.txt> <thread-name regex> [slice regex]

Pairs B/E tracing_mark_write events per thread. Without a slice regex: the slice names (numbers stripped) by total
time on the matching threads. With one: every matching slice's duration in ms, in order, with its start time, so
long frames can be lined up against other threads."""
import collections
import re
import sys

LINE = re.compile(r"^\s*(.+?)-(\d+)\s+\[\d+\]\s+\S+\s+([\d.]+): tracing_mark_write: ([BE])\|(\d+)(?:\|H:([^|]*))?")
path, thread = sys.argv[1], re.compile(sys.argv[2])
wanted = re.compile(sys.argv[3]) if len(sys.argv) > 3 else None
stacks = collections.defaultdict(list)
totals = collections.Counter()
counts = collections.Counter()
for line in open(path, errors="replace"):
    m = LINE.match(line)
    if not m or not thread.search(m.group(1)):
        continue
    tid, t, kind, name = int(m.group(2)), float(m.group(3)), m.group(4), m.group(6)
    if kind == "B":
        stacks[tid].append((name or "?", t))
    elif stacks[tid]:
        name, start = stacks[tid].pop()
        if wanted:
            if wanted.search(name):
                print(f"{start:.4f} {1000 * (t - start):7.2f} ms  {m.group(1)}-{tid}  {name[:100]}")
        else:
            key = re.sub(r"\d+", "#", name)[:90]
            totals[key] += t - start
            counts[key] += 1
if not wanted:
    for key, total in totals.most_common(40):
        print(f"{1000 * total:9.1f} ms  {counts[key]:6d}x  avg {1000 * total / counts[key]:6.2f}  {key}")
