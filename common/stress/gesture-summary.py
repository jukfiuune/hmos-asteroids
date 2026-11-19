#!/usr/bin/env python3
"""Summary of one gesture-trace.sh run (the /data/asteroids/gest/<gesture> directory, copied to the laptop):

    gesture-summary.py <dir>

Frame pacing from the composer (FRAME_GAPS), render_service's RenderFrame times, GPU and CPU clocks, and the slowest
frames with what took their time on the render thread (nested trace slices), plus slow slices on other threads in
those frames (the app's UI thread, render_service's main thread) and every pipeline creation."""
import collections
import re
import statistics
import sys

d = sys.argv[1]
LINE = re.compile(r"^\s*(.+?)-(\d+)\s+\[\d+\]\s+\S+\s+([\d.]+): tracing_mark_write: ([BE])\|(\d+)(?:\|H:([^|]*))?")
NUM = re.compile(r"\d+")

# Slices per thread: (thread name, tid, start, end, depth, name)
slices = []
stacks = collections.defaultdict(list)
for line in open(f"{d}/trace.txt", errors="replace"):
    m = LINE.match(line)
    if not m:
        continue
    comm, tid, t, kind, name = m.group(1), int(m.group(2)), float(m.group(3)), m.group(4), m.group(6)
    if kind == "B":
        stacks[tid].append((name or "?", t))
    elif stacks[tid]:
        name, start = stacks[tid].pop()
        slices.append((comm, tid, start, t, len(stacks[tid]), name))

gaps = [0] * 8
for line in open(f"{d}/metrics.txt", errors="replace"):
    m = re.search(r"FRAME_GAPS bins_ms=\S+ counts=([\d,]+)", line)
    if m:
        gaps = [a + int(b) for a, b in zip(gaps, m.group(1).split(","))]
n = sum(gaps[:7])
if n:
    print("composer frame gaps (<=8,<=12,<=17,<=25,<=34,<=50,<=100,>100 ms):", gaps,
          f"over 12 ms {100 * sum(gaps[2:7]) / n:.1f}%, over 17 ms {100 * sum(gaps[3:7]) / n:.1f}%")

clocks = [l.split() for l in open(f"{d}/clocks.txt") if len(l.split()) >= 4]
if clocks:
    gpu = collections.Counter(int(c[1]) // 1000000 for c in clocks)
    busy = [int(c[2]) for c in clocks]
    print("GPU MHz share:", ", ".join(f"{k}:{v * 100 // len(clocks)}%" for k, v in sorted(gpu.items())),
          f"| busy avg {statistics.mean(busy):.0f}%")
    print("CPU MHz avg per policy:", [round(statistics.mean(int(c[i]) for c in clocks) / 1000) for i in range(3, len(clocks[0]))])

rt = [s for s in slices if s[0].startswith("RSUniRenderThre")]
frames = sorted((s for s in rt if s[5].startswith("RenderFrame")), key=lambda s: s[2])
if frames:
    ms = sorted(1000 * (s[3] - s[2]) for s in frames)
    print(f"RenderFrame n={len(ms)} p50 {ms[len(ms) // 2]:.1f} p90 {ms[len(ms) * 9 // 10]:.1f} max {ms[-1]:.1f} ms; "
          f"over 8.3 ms {100 * sum(x > 8.3 for x in ms) / len(ms):.0f}%, over 11.1 ms {100 * sum(x > 11.1 for x in ms) / len(ms):.0f}%")
    print("slowest frames:")
    for f in sorted(frames, key=lambda s: s[2] - s[3])[:8]:
        inner = collections.Counter()
        for s in rt:
            if s[1] == f[1] and s[4] == f[4] + 1 and f[2] <= s[2] and s[3] <= f[3]:
                inner[NUM.sub("#", s[5])[:60]] += 1000 * (s[3] - s[2])
        print(f"  {f[2]:.3f} {1000 * (f[3] - f[2]):6.1f} ms: " + "; ".join(f"{k} {v:.1f}" for k, v in inner.most_common(4)))
        others = [s for s in slices if s[1] != f[1] and s[3] - s[2] > 0.004 and s[2] < f[3] and s[3] > f[2]
                  and s[4] <= 1]
        for s in sorted(others, key=lambda s: s[2] - s[3])[:3]:
            print(f"      {s[0]}: {NUM.sub('#', s[5])[:70]} {1000 * (s[3] - s[2]):.1f} ms")
pipes = [s for s in slices if "Pipeline" in s[5] and s[3] - s[2] > 0.002]
if pipes:
    print(f"pipeline creations over 2 ms: {len(pipes)}, total {sum(1000 * (s[3] - s[2]) for s in pipes):.0f} ms, "
          f"max {max(1000 * (s[3] - s[2]) for s in pipes):.0f} ms ({collections.Counter(s[0] for s in pipes).most_common(3)})")
