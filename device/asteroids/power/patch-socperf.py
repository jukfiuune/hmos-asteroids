#!/usr/bin/env python3
"""The donor's SocPerf boosts, moved to this phone's clusters and GPU.

    patch-socperf.py <socperf_resource_config.xml> <socperf_boost_config.xml> <out resources> <out boosts>

resource_schedule_service's SocPerf raises frequency floors for what the UI does: app start, click, slide, list
fling, page push and pop, the status bar and Control Center (status_bar), animations (animation_boost),
render_service's heavy frames (render_service_compose_*), blur. The donor's resource file (sys_prod) is for a Kirin:
policy0/1/2, /sys/class/devfreq/gpufreq, schedutil and EAS knobs, cpu.boost in the cpu cgroups. None of that exists
here, so every boost wrote nowhere and the gold and prime cores stayed at their 634 MHz floor through animations
(Control Center frames of 20-40 ms on a gold core at 634 MHz). SocPerf only takes boost values that are in the
resource's frequency list, so both files are rewritten:
  - resources: the same ids and names for the cluster floors and ceilings (lit = policy0, mid = policy4,
    big = policy7) and the GPU's (KGSL's devfreq), with this phone's frequency tables; the rest (DDR, L3, IPA, the
    governor, EAS and cgroup knobs) is left out
  - boosts: the same commands and durations, each frequency moved to the same place in this phone's range (rounded up
    to a real step, at most cpu-tuning.sh's sched_fmax_cap) and every other resource dropped
GPU floors stop at 362 MHz: the boosts' gain came from the CPU (Control Center's frames wait on the render thread;
the GPU ran ~16% busy), and a 644 MHz animation floor only spent power. KGSL's governor still raises the clock on
real load.
"""
import bisect
import re
import sys
import xml.etree.ElementTree as ET

res_src, boost_src, res_out, boost_out = sys.argv[1:5]

# Frequency steps (scaling_available_frequencies, KGSL's devfreq available_frequencies); floors and caps as
# cpu-tuning.sh leaves them.
CPU = {
    "lit": ("policy0", [441600, 595200, 787200, 902400, 1017600, 1113600, 1228800, 1344000, 1497600, 1593600,
                        1708800, 1804800], 595200, 1708800),
    "mid": ("policy4", [480000, 633600, 787200, 940800, 1056000, 1190400, 1286400, 1401600, 1497600, 1612800,
                        1708800, 1824000, 1920000, 2016000, 2073600, 2208000, 2400000], 633600, 2208000),
    "big": ("policy7", [480000, 633600, 787200, 960000, 1094400, 1209600, 1324800, 1459200, 1651200, 1766400,
                        1900800, 2208000, 2304000, 2496000], 633600, 2496000),
}
GPU = ([264000000, 362000000, 510000000, 644000000, 688000000, 763000000, 895000000, 960000000, 1050000000],
       264000000, 362000000)
GPU_PATH = "/sys/class/kgsl/kgsl-3d0/devfreq"

# The donor's ranges, from its resource file.
donor = {}
for res in ET.parse(res_src).getroot().iter("res"):
    node = res.find("node")
    if node is not None and node.text and re.fullmatch(r"[\d ]+", node.text.strip()):
        donor[res.get("name")] = [int(v) for v in node.text.split()]


def move(name, value):
    """A donor frequency for resource <name> at the same place in our range, rounded up to one of our steps."""
    kind = name.split("_")[0]
    steps, lo, cap = (CPU[kind][1:] if kind in CPU else GPU)
    d = donor[name]
    pos = min(max((value - d[0]) / (d[-1] - d[0]), 0), 1)
    want = lo + pos * (cap - lo)
    usable = [s for s in steps if lo <= s <= cap]
    return usable[min(bisect.bisect_left(usable, want), len(usable) - 1)]


ids = {}
lines = ['<?xml version="1.0" encoding="utf-8"?>', "<!-- patch-socperf.py: this phone's clusters and GPU -->",
         "<Configs>", "  <Resource>"]


def resource(rid, name, pair, path, steps, default, ceiling):
    ids[name] = rid
    mode = ' mode="1"' if ceiling else ""
    lines.extend([f'    <res id="{rid}" name="{name}" pair="{pair}"{mode} switch="0">',
                  f"      <default>{default}</default>", f"      <path>{path}</path>",
                  f"      <node>{' '.join(str(s) for s in steps)}</node>", "    </res>"])


for i, (kind, (policy, steps, lo, cap)) in enumerate(CPU.items()):
    path = f"/sys/devices/system/cpu/cpufreq/{policy}/scaling_"
    resource(1000 + 2 * i, f"{kind}_cpu_min_freq", 1001 + 2 * i, path + "min_freq", steps, lo, False)
    resource(1001 + 2 * i, f"{kind}_cpu_max_freq", 1000 + 2 * i, path + "max_freq", steps, steps[-1], True)
resource(1020, "gpu_min_freq", 1021, GPU_PATH + "/min_freq", GPU[0], GPU[1], False)
resource(1021, "gpu_max_freq", 1020, GPU_PATH + "/max_freq", GPU[0], GPU[0][-1], True)
lines.extend(["  </Resource>", "</Configs>", ""])
open(res_out, "w").write("\n".join(lines))

# The boosts: each Action keeps its duration and the resources above, moved.
root = ET.parse(boost_src).getroot()
kept = dropped = 0
for cmd in root.iter("cmd"):
    for action in cmd.findall("Action"):
        for item in list(action):
            if item.tag == "duration":
                continue
            if item.tag in ids:
                item.text = str(move(item.tag, int(item.text)))
                kept += 1
            else:
                action.remove(item)
                dropped += 1
        if len(action) == 1 and cmd.findall("Action").index(action) > 0:
            cmd.remove(action)  # nothing left but the duration
ET.indent(root, "  ")
open(boost_out, "wb").write(b'<?xml version="1.0" encoding="utf-8"?>\n' + ET.tostring(root) + b"\n")
print(f"{len(list(root.iter('cmd')))} boosts: {kept} frequency settings moved, {dropped} other settings dropped")
