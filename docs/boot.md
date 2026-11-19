# Boot chain

Runbook with commands: [flashing.md](flashing.md). Partitions: [storage.md](storage.md).

## Slots and the bootloader

- Everything runs on slot `_b`. Slot `_a` crashes ABL during load and is marked unbootable after one try, even with
  all-stock firmware, boot images and vbmeta on it (checked by hash), so the cause is below what we can flash.
- Bootloader unlocked (`orange`). ABL tolerates modified images; vbmeta_b stays stock.
- ABL accepts our 6.6 `Image.gz` (`fastboot boot` works), but on header v4 it still takes vendor_boot (DTB and
  modules), init_boot and dtbo from flash, so the whole chain is built and flashed as one set.
- ABL passes the vendor_boot and init_boot ramdisks plus bootconfig as the initrd, never boot.img's own ramdisk.
- The init_boot header needs `--os_version 17.0.0 --os_patch_level 2026-11` (from stock).
- **ABL silently drops every part of a sparse image past 14 GiB** and reports OKAY (a 64 GiB ext4 lost all 32
  metadata chunks past 14 GiB, journal superblock included). Big images go through fastbootd, which only opens
  partitions its SELinux policy allows (`userdata`, `metadata`). Boot images are small and fine from ABL.
- M1's busybox can't reboot into the bootloader: Vol- and Power (a hard reset, clears pstore). `reboot -f` from M1's
  shell is a warm reboot back into M1.
- Boot success: `platform/qcom/services/slot_success.c` sets the running slot's "successful" bit (ABL's flags in byte 6 of the
  `boot_<slot>` GPT entry's attributes) in both tables of LUN 4, checking every CRC first and reading back after.
  `common/boot/svc/booted.sh` runs it once the desktop has been up a minute with SceneBoard unchanged. With the bit set ABL
  stops spending retries and no longer falls into fastboot. LUN 4's tables before the first write: `device/asteroids/gpt/lun4/`.

## Images

`build/assemble-boot.sh` (build PC, `~/harmony-build/assemble-m1.sh`, output `~/harmony-build/src/out/images-m1/`):

| Image | Contents |
|---|---|
| boot (v4) | kernel only; command line `arm64.nopauth` ([kernel.md](kernel.md)) |
| init_boot (8 MiB) | busybox, `common/m1/init`, `lpmap` (common/m1/lpmap.c), `common/m1/migrate-userdata.sh`, the M1 service set |
| vendor_boot (96 MiB) | merged volcano DTB, the vendor_boot, recovery and vendor_dlkm module lists with dependencies (342 modules: kstate.ko added before the WLAN driver; the patched icnss2 and hapticdrv), firmware the early drivers ask for (tfa98xx.cnt, haptic config, WLAN ini), device cmdline and bootconfig |
| dtbo | from M0's build |

boot_b (sde40) and init_boot_b (sde55) can be rewritten in place from M1's shell with `dd ... conv=fsync`. Previous
images are kept on the build PC in `~/harmony-build/m1-backup/` (`init_boot_b.pre-usbshell.img`, `.pre-m1link.img`,
`.pre-super.img`, `.pre-asteroids.img`).

## M0 and M1

- M0 (2026-11-11): our kernel and busybox init, recovery modules, modem firmware, ADSP, root shell on a USB ACM gadget.
  USB needs the recovery module list plus `peripheral` in `/sys/bus/platform/devices/a600000.ssusb/mode`.
- M1 (`common/m1/init`, PID 1 until the handoff):
  1. Loads the vendor_boot, recovery and vendor_dlkm lists: no deferred probes, 59 thermal zones. Only `cnss2` fails
     ("No such device"; this phone's Wi-Fi is icnss2).
  2. Mounts the modem partition's firmware at `/vendor/firmware_mnt` and requests the ADSP boot.
  3. USB gadget: ACM serial shell (ttyGS0) and CDC NCM network. The link is renamed `m1link` so HarmonyOS's Ethernet
     manager (which adopts every `(eth|usb)\d`) leaves it alone; the network shell (TCP 2323) binds to
     `fe80::a5:7eff:fe00:1%m1link` only, not Wi-Fi.
  4. Maps super's partitions (lpmap) and mounts them read-only: the donor's under `/hmos`, asteroids (the M2 bundle)
     at `/asteroids` ([storage.md](storage.md)). udhcpd (Alpine's, in asteroids) serves 192.168.77.0/24 on the USB
     link so the laptop's NetworkManager keeps it.
  5. Mounts userdata at `/data`, formatting it first if it is blank; moves an old "hmosdata" layout over once.
  6. Builds `/hmos-root`: system as an overlay over a tmpfs upper, the other partitions bound in, `/data` bound in,
     tmpfs for metadata/log/mnt/storage/tmp, `/dev`, `/proc`, `/sys` rbound, binderfs.
  7. Hides every partition's `etc/init` and installs the service set (`/asteroids/init`: make-m1-services.py's plus ours).
  8. Writes HarmonyOS's `/proc/cmdline` and `/proc/bootconfig` (bound over the real ones): `ohos.boot.hardware`,
     `ohos.boot.selinux=permissive`, `enforcing=0`.
  9. Runs `/asteroids/boot/prep.sh` (its steps in `prep.d/`, [framework.md](framework.md)), which also binds
     `/asteroids` into the root for the init services in `/asteroids/init/asteroids-helpers.cfg`.
  10. Waits for `/tmp/m1-go` if `/data/asteroids/hold` exists (else goes straight on), then binds the root
      onto `/` and `exec chroot /hmos-root /system/bin/init --second-stage`.
- What HarmonyOS init needs from M1: `enforcing=0` in its cmdline (libload_policy reads the SELinux mode there;
  without it everything unlabeled is denied), the root bound onto `/` and not only chrooted (init's sandbox `setns`
  resets the root to the namespace root), the binderfs nodes at 0666 (samgr runs as uid 5555).
- `common/boot/blackbox.sh`, started before the handoff when `/data/asteroids/blackbox` exists, writes the kernel log,
  processes and mounts to `/data/asteroids/blackbox/` every 2 s. The kernel ring buffer (1 MiB) wraps within the first minute (ueventd), so early
  `hmos-m1` lines are only in the live log right after boot.
- (Before the helpers were init services) a boot held at `/tmp/m1-go` let the audio script run ahead of HarmonyOS: its parameter sets were
  lost and audio, Bluetooth and the sensors stayed off for that boot.

## The asteroids partition

Ours, read-only at `/asteroids` (built by `build/build-asteroids.sh`, `./port build asteroids`):

| Directory | Contents |
|---|---|
| `bin/` | busybox with its applet links (for everything that outlives M1's ramdisk) and our static tools: pd-mapper, rmtfs, tqftpserv, asteroids_uim, asteroids_gnss, wowlan, slot_success, tap, ssc_probe, asteroids_vibrate |
| `boot/` | `common/boot/`: `lib.sh` (log, part, rproc, until_, respawn, android), `device.conf` (this phone's facts), `prep.sh` and `prep.d/`, `svc/` (the init services' scripts), `wake.sh`, `blackbox.sh`, `shell.sh` |
| `init/` | the service set M1 installs: make-m1-services.py's, `asteroids-desktop.cfg`, `asteroids-helpers.cfg` |
| `etc/` | configs bound over the donor's (`build/build-etc.sh` and ours), app trust, `bootpic.zip` |
| `vendor/`, `system/` | overlaid on the donor's vendor; copied over the root's system |
| `android/` | what the Android helpers preload: `audio/` (PAL server), `bluetooth/` (bridge, vendor library), `vibrator/` (RichTap server) |
| `dhcp/`, `stress/` | Alpine's udhcpd for the USB network; the UI stress test |

prep.sh's steps (M1's root, before the handoff): `10-clock` (RTC plus the saved offset), `20-overlays` (/vendor, /system,
/asteroids and /android in the root), `30-configs` (binds, Base/Pro name, chip name), `40-data` (log files and
directories services expect), `50-devices` (GPU and backlight nodes, tracefs, preload), `60-wifi` (WPSS boot),
`70-audio-dsp` (pd-mapper, sound card), `80-modem` (rmtfs, tqftpserv, MPSS).

HarmonyOS init services (`asteroids-helpers.cfg`: root, no sandbox, so in init's mount namespace with all
capabilities):

| Service | Started on | Does |
|---|---|---|
| asteroids_route | post-init | direct scanout off for the boot animation |
| asteroids_clock | post-init | saves (now - RTC) every 10 min |
| asteroids_android | post-fs-data | mounts the Android root, sets asteroids.android.ready |
| asteroids_adsprpcd, _pal, _sscrpcd, _bt_bridge, _richtap | asteroids.android.ready=true | the Android daemons and sheng's servers; they set asteroids.audio/sensors/bluetooth.start for asteroids-desktop.cfg |
| asteroids_cgroups | bootevent.samgr.ready=true | the scheduling cgroups ([performance.md](performance.md)) |
| asteroids_booted | bootevent.boot.completed=true | CPU tuning, Wi-Fi wake, video nodes, user Docs, display wake, capture loop, slot success |
| asteroids_capture | started by booted.sh | panel captures |

Their log lines go to the kernel log and `/data/local/tmp/asteroids-boot.log`; prep.sh also saves the kernel log at the
handoff in `/data/local/tmp/dmesg-handoff.txt`.

### Keeping the helpers up

Until 2026-11-16 every helper ran once: one that died (the Bluetooth bridge exits on a failed chip init, by design)
stayed dead until a reboot. Now:

- The scripts of asteroids_adsprpcd, _pal, _sscrpcd, _bt_bridge and _richtap start their daemon in a function that
  lib.sh's `respawn` runs again whenever it returns, with a pause of 2 s doubling to 60 s (back to 2 s after a run of
  10 minutes). Each exit is logged as `<tag>: exited (<status>) after <n>s`.
- Their clients reconnect: the audio VDI on its next write ([audio.md](audio.md)); HarmonyOS's Bluetooth stack doesn't
  notice its bridge going, so bt-bridge.sh restarts asteroids_bluetooth_hdi and bluetooth_service after a new bridge
  is up (they power the chip on again if Bluetooth was on).
- audio_server and audio_host are no longer `once`: init restarts them.
- Tested: the PAL server killed (back in 2 s, the next playback went through), the Bluetooth bridge killed (back,
  reconnected and the chip initialised within 5 s).

`scripts/health.sh` shows in one go whether the DSPs, the helpers and their HarmonyOS services are up, the restarts
since boot, failed audio writes and lost audio links, native crashes since boot and test roots left up.

## Timeline (2026-11-15, everything from super, helpers as init services)

| t (s) | |
|---|---|
| 0.2 | M1 starts |
| 4.6 | modules loaded |
| 7.7 | USB shells up, super mapped and mounted |
| 8.2 | userdata mounted, prep starts |
| 10.5 | prep done, handoff |
| 15-35 | samgr ready, Android root, helpers (varies from boot to boot, below) |
| 28-45 | `bootevent.boot.completed` |

Open: the time from the handoff to samgr varies a lot between boots: samgr ready (`ohos.boot.time.samgr.ready`) at
15.4 s on one boot and 31.8-35.0 s on the next three, with the same images. Before the move to super,
boot.completed was seen at 32.8 s; after it, 45-51 s (the old after-boot.sh polled every 5 s).

## Clock across power-offs

The PMIC RTC (rtc-pm8xxx) can't be set from Linux and reads ~1970. `common/boot/svc/clock.sh` does what Android's time_daemon does:
the asteroids_clock service keeps (now - RTC) in `/data/asteroids/clock-offset`, prep.sh adds it back before the handoff (checked:
the root's first mounts carry the right time 9 s into boot). Network time then syncs over Wi-Fi; the zone is the
donor's Asia/Shanghai.
