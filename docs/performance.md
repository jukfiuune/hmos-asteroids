# Performance: CPU, scheduling, frame pacing

## CPU frequency (2026-11-13)

The boot command line sets `cpufreq.default_governor=performance`; Android's post-boot script
(`init.kernel.post_boot-volcano_default_4_3_1.sh`, clusters 4+3+1) switches to WALT, and HarmonyOS never ran it.
`device/asteroids/boot/svc/cpu-tuning.sh` (booted.sh, at desktop up) applies its kernel part: WALT with its limits, core control, task
placement, the 120 ms touch boost, bus DCVS, LPM. Idle ~716/633/633 MHz. Checked 2026-11-14: all three policies walt.

## Scheduling cgroups (2026-11-14)

Every task sat in the root cpuset and cpu group. HarmonyOS's init.sched.cfg mounts /dev/cpuset and /dev/cpuctl in
pre-init, but here those mounts stay in M1's namespace and the groups became plain directories. `common/boot/svc/cgroups.sh`
(init service asteroids_cgroups, on bootevent.samgr.ready) mounts both hierarchies in both namespaces, replays init.sched.cfg (106 commands, 18
cpuset groups; background 0-3 is the silver cluster, system-default 0-6 leaves out prime) and restarts
resource_schedule_service while it still sees boot complete. Checked: apps go to top-app on launch and to background
(cores 0-3) ~30 s after going home; render_service in graphic-high. Commands inside HarmonyOS's namespaces need
`/system/bin` (M1's PATH isn't there).

## SocPerf interaction boosts (2026-11-14)

SocPerf (in resource_schedule_service) raises frequency floors for clicks, flings, window switches and animations.
The donor's resource files are for its Kirin, every resource reports to Huawei's perf .so (`switch="1"`) and the
executor plugin wasn't loaded (`libpathexe`). `device/asteroids/power/patch-socperf.py` rewrites both files for this SoC's clusters
(policy0/4/7) and KGSL's devfreq, `switch="0"`, boosts moved to the same place in this phone's ranges (capped at
sched_fmax_cap); DDR, L3, governor and cgroup boosts dropped; GPU floors capped at 362 MHz. A tap lifts gold to
1.29-1.40 GHz. Control Center, 6 opens: render frame p50/p90 9.1/17.2 → 8.1/12.3 ms, frames over 11.1 ms 35% → 13%.
Possible next: the cgroup boosts as `cpu.uclamp.min` (measure power first).

## Frame pacing: what's known

- Before the composer changes: 58 fps home swipes; after direct scanout and non-blocking commits ~110 fps on 120 Hz.
- Jank (`common/stress/jank.sh`, 16 flings in Settings at 90 Hz): ~15% of frames miss one refresh with either governor;
  not the CPU clock. Traced (`common/stress/jank-ftrace.sh`, `trace-slices.py`, `renderframe-summary.sh`):
  render_service's RenderFrame p50 ~8.2 ms, p90 ~13 ms against 11.1 ms; on-CPU and memory-bound (CPI 2.6-3.0; Skia
  40%, render_service nodes ~17%, Turnip command recording 10%). Pipeline compiles only right after an app opens.
- Control Center: each open makes a full-screen texture (fresh 48 MB VMA block) that's freed on hide; KGSL's
  allocation costs ~19 ms with boosts ([gpu.md](gpu.md)).
- GPU-bound apps (Telegram/Petrelgram flings): render_service plays back ~260 nodes per frame and redraws four
  background blurs with no valid cache; list-row cache surfaces are remade per fling; the render thread spends a third
  of its time on silver cores (SCHED_FIFO, no uclamp) while prime idles.
- Tracing: prep.sh mounts tracefs with `trace_marker` 0222 before the handoff (HitraceMeter writes there). The hitrace
  tool still refuses (errorCode 1); `debug.hitrace.tags.enableflags` plus raw ftrace works. hiperf works with
  `const.security.developermode.state=true` and `const.debuggable=1` (`asteroids.para`, only in the PC's hmos-stage).
  M1's busybox has no taskset; use `/system/bin/taskset`.

## Stress runs

`common/stress/stress.sh` (phone), `stress-report.py` (laptop), data in `recordings/` (git-ignored).

| Run | Result |
|---|---|
| 1 (2026-11-12, 12 min) | no service restarts; SceneBoard crash-loop panic before the run ([display.md](display.md)); render_service RSS 249 → 294 MB |
| 3 (2026-11-13, 30 min) | no restarts; render_service 205 → 390 MB, mostly KGSL mappings; GPU median 57% busy, max 46 °C |
| 4 (2026-11-13, 20 min) | render_service memory is a pool that grows and shrinks, not a leak |

Also: `common/stress/appload.sh` / `appload-report.py` (app-load sampler), `gesture-trace.sh` / `gesture-summary.py`,
`jank-summary.sh`.

## Open

- Render-thread work per frame (blurs, row caches), or 60 Hz for ~2x headroom.
- HarmonyOS's own frame-aware boosting needs the qos_ctrl/RTG kernel driver.
- com.ohos.callui spun on display-change callbacks (~440/s) in the background (2026-11-12; not rechecked).
