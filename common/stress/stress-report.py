#!/usr/bin/env python3
"""Summarises a stress.sh run fetched from the phone (/data/asteroids/stress):

    stress-report.py <dir>

samples.txt: per service, whether its pid changed (a restart) and how its fds and RSS moved from first to last
sample; MemAvailable's range; GPU busy (kgsl gpubusy is busy/total time since the last read) and clock; GPU
temperature. events.txt: what the run did. hilog.txt: counts of watchdog kills, freezes, crashes and the errors
seen before (allocator refusals, Vulkan import failures). exits.txt: non-zero process exits from dmesg."""
import collections
import re
import sys

d = sys.argv[1]
samples = []
for line in open(f"{d}/samples.txt"):
    fields = dict(f.split("=", 1) for f in line.split() if "=" in f)
    samples.append(fields)
print(f"samples: {len(samples)} over {float(samples[-1]['t']) - float(samples[0]['t']):.0f} s")

services = [k for k in samples[0] if "/" in samples[0][k] and k not in ("gpubusy",)]
print("\nservice                       restarts  fds first->last (max)   RSS MB first->last (max)")
for s in services:
    pids, fds, rss = [], [], []
    for x in samples:
        p, f, r = x[s].split("/")
        pids.append(p)
        fds.append(int(f[:-2] or 0))
        rss.append(int(r[:-2] or 0) / 1024)
    restarts = sum(1 for a, b in zip(pids, pids[1:]) if a != b)
    print(f"{s:30s}{restarts:>8d}  {fds[0]:>5d}->{fds[-1]:<5d} ({max(fds):>5d})   "
          f"{rss[0]:>6.0f}->{rss[-1]:<6.0f} ({max(rss):>6.0f})")

mem = [int(x["memavail_kb"]) / 1024 for x in samples]
print(f"\nMemAvailable MB: first {mem[0]:.0f}, last {mem[-1]:.0f}, min {min(mem):.0f}")
busy = []
for x in samples:
    parts = [v for v in (x.get("gpubusy") or "").split("/") if v]  # gpubusy is space-padded
    b, t = (parts + ["0", "0"])[:2]
    if int(t):
        busy.append(100 * int(b) / int(t))
clk = [int(x["gpuclk"]) / 1e6 for x in samples if x.get("gpuclk")]
temp = [int(x["gputemp"]) / 1000 for x in samples if x.get("gputemp", "").lstrip("-").isdigit()]
if busy:
    busy.sort()
    print(f"GPU busy %: median {busy[len(busy) // 2]:.0f}, p90 {busy[int(len(busy) * 0.9)]:.0f}, max {busy[-1]:.0f}")
if clk:
    print(f"GPU clock MHz: min {min(clk):.0f}, max {max(clk):.0f}, " +
          ", ".join(f"{c:.0f}:{n}" for c, n in sorted(collections.Counter(clk).items())))
if temp:
    print(f"GPU temperature C: min {min(temp):.1f}, max {max(temp):.1f}")
off = sum(1 for x in samples if x.get("bl") != "0")
print(f"samples with the panel off: {off}")

events = open(f"{d}/events.txt").read().split("\n")
print(f"\nevents: {sum('cycle' in e for e in events)} cycles, "
      f"{sum(' open ' in e for e in events)} app opens, {sum('rotation' in e for e in events)} rotations")

LINE = re.compile(r"^\S+ \S+\s+\d+\s+\d+ \w \w+/([^/\s]+)/")  # date time pid tid level domain/process/tag
PATTERNS = {
    "watchdog (XCollie) blocked": r"XCollie.*blocked|SERVICE_BLOCK|THREAD_BLOCK",
    "app freeze": r"APP_FREEZE|APP_INPUT_BLOCK|appfreeze|LIFECYCLE_TIMEOUT",
    "native crash": r"cppcrash|Fatal signal|SIGSEGV|SIGABRT",
    "JS crash": r"jscrash|JsError|Uncaught",
    "allocator refused": r"AllocMem unsupported",
    "Vulkan import failed": r"vkGetNativeBufferPropertiesOHOS Failed",
    "RS RequestBuffer failed": r"RequestBuffer failed",
    "render_service frame drop/jank": r"JANK|jank_frame|SkipFrame",
    "EGL error": r"eglSwapBuffers.*fail|EGL_BAD|egl.*error",
    "binder transaction failed": r"transaction failed|DEAD_REPLY",
}
try:
    log = open(f"{d}/hilog.txt", errors="replace").read().split("\n")
    print(f"\nhilog: {len(log)} lines")
    for name, pattern in PATTERNS.items():
        hits = [l for l in log if re.search(pattern, l, re.I)]
        if hits:
            procs = collections.Counter(m.group(1) if (m := LINE.match(l)) else "?" for l in hits)
            print(f"  {name}: {len(hits)}  " + ", ".join(f"{p}:{n}" for p, n in procs.most_common(4)))
except FileNotFoundError:
    print("\nno hilog.txt")
try:
    exits = open(f"{d}/exits.txt").read().strip().split("\n")
    print(f"\nnon-zero exits in dmesg: {len([e for e in exits if 'exit with' in e])}")
    for e in exits[-12:]:
        print("  " + e[e.find("]") + 1:].strip()[:150])
except FileNotFoundError:
    pass
