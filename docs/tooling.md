# Working on the port

## Machines

- Laptop: this repo, the phone on USB.
- Build machine: `BUILD_HOST` in `port.conf` (`port.conf.example`; `local` builds on the laptop itself), tree
  `BUILD_ROOT` (default `~/harmony-build`, `$HARMONY_BUILD` in the build scripts): `repo/` (this repo, from
  `./port sync`; the only sources builds read), `m2/` (the builds' work tree: outputs, the Mesa trees and build
  directories, sysroots, link stubs, the stage; `build/env.sh`), src/ (kernel and modules), ohos/ (SDK and
  OpenHarmony sources), donor/ (with x/, the extracted images: build/extract-donor.sh), android-b/ (Android's slot b
  images and the helper partition), fw/ (the Android container's inputs), tools/. The README lists how each is
  filled. Heavy builds run there, at most -j10.
- Secrets live only on the build machine: Telegram api_id/api_hash (Harlgram's `config/telegram.local.json`,
  Petrelgram's build), the app-signing keystore and the phone's UDID (`$HARMONY_BUILD/tools/asteroids-signing/`,
  made by `common/signing/gen-pki.sh`).

## The port tool

`./port` (scripts/port.py) on the laptop:

| Command | Does |
|---|---|
| `port sync` | the repo to the build machine's `BUILD_ROOT/repo` |
| `port build boot\|asteroids\|android...` | sync, then the image builds on the PC (assemble-m1.sh, build/build-asteroids.sh, platform/qcom/android/build-android-helpers.sh); logs in the PC's `/tmp/port-build-<target>.log` |
| `port deploy asteroids\|android\|boot\|init_boot\|vendor_boot...` | super's partitions through make-super.sh and write-super.sh, boot partitions with dd and a check; then reboot and wait for the desktop (`--no-reboot`) |
| `port reboot`, `port log` | reboot and wait; M1's lines and the boot scripts' log |
| `port sh "<cmd>" [timeout]`, `port put`, `port get` | M1's shell (m0net.py) |
| `port screens [N] [out.png]` | the newest panel captures |

## Talking to the phone

- `scripts/m0net.py sh "<cmd>" [timeout] | put <local> <phone> | get <phone> <local>`: M1's root shell over the USB
  network (fe80::a5:7eff:fe00:1%m1link, TCP 2323), outside HarmonyOS. Transfers are checked with SHA-256 and synced.
- `scripts/m0sh.py "<cmd>"` and `scripts/m0push.py`: the same over the serial port (slower; for when the network is down).
- Inside HarmonyOS: `chroot /hmos-root /system/bin/<tool>` (hilog, param, begetctl) or
  `nsenter -t 1 -m -- /system/bin/<tool>` for init's namespace (the real `/mnt`, `/dev/cpuset`).
- `asteroids_dump` (init service with DUMP permission) runs `/data/local/tmp/asteroids-dump.sh`, for hidumper.

## UI automation

`common/services/tap.c` (`/asteroids/bin/tap`): touches, swipes and keys through the evdev nodes; `common/boot/wake.sh`
(`/asteroids/boot/wake.sh`): wake and unlock (only without a PIN); panel captures by svc/capture.sh into `/data/local/tmp/cap` (ring
of 60), `scripts/screens.sh` to fetch. Single-shot key writes only (press and release together): an injected power key
left pressed for 10 s forces a restart. Text injection through uinput gets only the touchscreen's gesture letters
through; type with the soft keyboard. powermgr refuses screen-timeout changes from a root shell (no token).

## Diagnostics

- `scripts/health.sh`: the DSPs, helpers and their HarmonyOS services, restarts, audio link trouble, crashes since
  boot, test roots left up ([boot.md](boot.md)).
- `scripts/missing-services.sh` (samgr misses), `scripts/suspend-report.sh`, `common/boot/blackbox.sh`.
- `common/stress/`: stress, jank, gesture and app-load scripts with their laptop-side reports ([performance.md](performance.md)).
- Kernel log: pstore after a warm reset (`/sys/fs/pstore`); M1 logs to `/dev/kmsg` and pmsg.

## Rules kept on this port

- Never flash xbl, abl, tz or modem; never write firmware partitions.
- No audible test sounds on the phone without asking; verify audio silently (PCM summaries).
- Don't reinstall apps the user removed (the bundle installer only takes listed directories).
- Commit locally; nothing is pushed.
- Every step that changes the phone lives in the repo as a script or patch, so the port can be rebuilt from scratch.

## Other scripts

`history/assemble-m0.sh`, `assemble-m1.sh` (boot images), `build-kernel.sh`, `build-ext-modules.sh`,
`build-erofs-utils.sh` (mkfs.erofs and fsck.erofs on the PC, for asteroids.img and android.img), `common/m1/super/*` (super),
`apply-ohos-ipc.sh`, `make-m1-services.py` (M1's service set), `simgdelta.py` (sparse-image delta),
`gpt_split.py` (the undone userdata split), `adb-keepalive.sh` (Android era), `restore-b.sh` (stock boot chain from
fastboot, build PC). Switches for M1 and the boot scripts are files in `/data/asteroids/` ([storage.md](storage.md)).
