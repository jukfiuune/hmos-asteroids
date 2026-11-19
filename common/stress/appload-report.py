#!/usr/bin/env python3
"""Summarises appload.sh's data per marked segment: GPU busy and clock, CPU policy clocks, frame pacing from the
composer, and the processes that used the most CPU (100% = one core).

    appload-report.py <dir with clocks.txt, procs.txt, marks.txt, metrics.txt>
"""
import collections
import re
import statistics
import sys

d = sys.argv[1]


def lines(name):
    try:
        return open(f"{d}/{name}").read().splitlines()
    except FileNotFoundError:
        return []


marks = [(float(l.split()[0]), l.split(None, 1)[1]) for l in lines("marks.txt") if len(l.split()) > 1]
clocks = []
for l in lines("clocks.txt"):
    f = l.split()
    if len(f) >= 4:
        try:
            clocks.append((float(f[0]), int(f[1]) // 1000000, int(f[2]), [int(x) // 1000 for x in f[3:]]))
        except ValueError:
            pass
# procs: {time: {pid: (ticks, name)}}
snaps = []
cur = None
for l in lines("procs.txt"):
    if l.startswith("T "):
        cur = (float(l.split()[1]), {})
        snaps.append(cur)
    elif cur is not None:
        f = l.split(None, 2)
        if len(f) == 3:
            cur[1][f[0]] = (int(f[1]), f[2])
frames = []  # (t, counts per bin)
for l in lines("metrics.txt"):
    m = re.match(r"t_ns=(\d+) FRAME_GAPS bins_ms=\S+ counts=([\d,]+)", l)
    if m:
        frames.append((int(m.group(1)) / 1e9, [int(x) for x in m.group(2).split(",")]))

end = max([c[0] for c in clocks] + [s[0] for s in snaps] + [0])
segs = [(t, marks[i + 1][0] if i + 1 < len(marks) else end, name) for i, (t, name) in enumerate(marks)]
BINS = ["<=8", "<=12", "<=17", "<=25", "<=34", "<=50", "<=100", ">100"]

for t0, t1, name in segs:
    print(f"== {name} ({t1 - t0:.0f} s)")
    c = [x for x in clocks if t0 <= x[0] < t1]
    if c:
        busy = [x[2] for x in c]
        clk = collections.Counter(x[1] for x in c)
        top = ", ".join(f"{k} MHz {v * 100 // len(c)}%" for k, v in sorted(clk.items()))
        print(f"  GPU busy avg {statistics.mean(busy):.0f}%, p90 {sorted(busy)[len(busy) * 9 // 10]}%; clocks: {top}")
        for i, pol in enumerate(zip(*[x[3] for x in c])):
            print(f"  CPU policy {i}: avg {statistics.mean(pol):.0f} MHz, max {max(pol)} MHz")
    f = [x for x in frames if t0 <= x[0] < t1]
    if f:
        tot = [sum(col) for col in zip(*[x[1] for x in f])]
        n = sum(tot[:-1])  # >100 ms gaps are idle, not frames
        if n:
            late = sum(tot[3:7])
            print(f"  frames {n}: " + ", ".join(f"{b} {v}" for b, v in zip(BINS, tot)) +
                  f"; >17 ms {late * 100 / n:.1f}%")
    s = [x for x in snaps if t0 <= x[0] <= t1]
    if len(s) >= 2:
        a, b = s[0], s[-1]
        dt = b[0] - a[0]
        use = []
        for pid, (ticks, nm) in b[1].items():
            before = a[1].get(pid, (0, nm))[0]
            if ticks - before > 0:
                use.append(((ticks - before) / dt, nm, pid))
        use.sort(reverse=True)
        total = sum(u[0] for u in use)
        print(f"  CPU total {total:.0f}% of one core; top: " +
              ", ".join(f"{nm}({pid}) {u:.0f}%" for u, nm, pid in use[:8]))
