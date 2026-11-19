# Flashing the port

The phone is on the laptop. Everything that boots is in the boot partitions and super; userdata is HarmonyOS's
`/data` only, and M1 formats it when it is blank, so wiping it is a factory reset ([docs/storage.md](storage.md)).

- Boot images: `images/{boot,vendor_boot,init_boot,dtbo}.img` (check with `sha256sum -c SHA256SUMS`). Rebuild on
  the build PC with `~/harmony-build/assemble-m1.sh` (output `~/harmony-build/src/out/images-m1/`).
- boot.img (kernel only) carries the one-way binder pid change since 2026-11-13 (`common/kernel/binder-oneway-pid.py`,
  applied uncommitted in the PC's kernel tree so the release string and module vermagic stay the same);
  `boot-pre-oneway.img` is the kernel before it. Its command line is `arm64.nopauth` since 2026-11-13 (pointer
  authentication off, for apps' Cangjie runtimes; see build/assemble-boot.sh); `boot-pre-nopauth.img` is the image
  before it.
- From M1's root shell boot_b and init_boot_b can be written in place: `dd if=boot.img of=/dev/sde40 bs=1M
  conv=fsync` (sde40 is boot_b, sde55 init_boot_b, by PARTNAME), then reboot. Earlier init_boot images are on the
  build PC in `~/harmony-build/m1-backup/` (`.pre-usbshell`, `.pre-m1link`, `.pre-super`, `.pre-asteroids`).
- super's images, on the build PC: the donor's (`~/harmony-build/donor/pura70proplus-7.0.0.107/images/*.img`,
  `preload-pkg/x/preload.img`), `~/harmony-build/m2/asteroids.img` (`build/build-asteroids.sh`) and
  `~/harmony-build/android-b/android.img` (`platform/qcom/android/build-android-helpers.sh`).
- Android restore set: `~/Downloads/flash_20261104_124306` (Nothing Archive's Asteroids_C5.0-261023-2146: stock
  images, including `recovery.img` and `flash_all.sh`) and `tools/kernelsu/out/init_boot_ksu.img`.
- Helpers: `scripts/m0net.py` (the USB network shell), `scripts/m0sh.py "<command>"` (the serial shell),
  `scripts/m0push.py` (copy a small file over serial).

The partition table is stock (the 64 GiB userdata split was undone; back up your own with `sgdisk --backup` before changing it).

## Things the bootloader and fastbootd do

- **The bootloader (ABL) silently drops every part of a sparse image past 14 GiB** and reports OKAY. super (9.66 GB)
  is under that; userdata isn't, so never flash a userdata image from the bootloader (none is needed now).
- **fastbootd** (Android's userspace fastboot, in the stock recovery) writes large sparse images correctly, but it only
  opens partitions its SELinux policy allows: `userdata` and `metadata` yes; a new partition, `persist` or `rawdump` no.
  It needs the stock boot chain on the slot (`boot`, `vendor_boot`, `recovery`).
- M1's busybox can't reboot into the bootloader. Use Vol- and Power (a hard reset: it clears pstore). `reboot -f`
  from the serial shell is a warm reboot back into M1.

## 1. Build

On the build PC: the boot images (`assemble-m1.sh`), `build/build-asteroids.sh` (asteroids.img) and
`platform/qcom/android/build-android-helpers.sh` (android.img; its inputs are Android's slot b system, system_ext, vendor and odm,
checked against `device/asteroids/android/inputs.sha256`). erofs-utils first, once: `build/build-erofs-utils.sh`. Then on the
laptop:

```bash
common/m1/super/make-super.sh        # metadata for the image sizes on the PC: out/super/{super-head.img,layout.txt}
```

## 2. Flash the boot chain to slot b

From the bootloader (Vol- and Power from M1):

```bash
cd ~/Documents/harmony/m1/images && sha256sum -c SHA256SUMS
fastboot flash boot_b boot.img
fastboot flash vendor_boot_b vendor_boot.img
fastboot flash init_boot_b init_boot.img
fastboot flash dtbo_b dtbo.img
fastboot --set-active=b
```

## 2b. Write super

**On a phone already running M1** (how it is done; the system may be running, M1 only reads super):

```bash
./port build asteroids android      # on the PC, after syncing the repo
./port deploy asteroids android     # make-super.sh, copies the images, write-super.sh, reboot
```

`port deploy` runs make-super.sh, copies the metadata, write-super.sh and each image to `/data/local/tmp/super` and
runs `write-super.sh /data/local/tmp/super <partitions>`; partitions not named must keep their extents (write-super.sh
checks). It writes each image into its extent (zeroing the rest of it) and the metadata over the first MiB, then checks the
metadata on disk against layout.txt and each written extent against its image. The new contents are used from the
next boot.

**On a blank phone** (not tried): make the whole of super as one sparse image on the PC and flash it from the
bootloader (it is under ABL's 14 GiB limit), or from fastbootd as Nothing's `flash_all.sh` does:

```bash
# on the PC, with the eight images in one directory and layout.txt's sizes:
lpmake --metadata-size 65536 --metadata-slots 3 --device-size 9663676416 --super-name super --virtual-ab \
    --group qti_dynamic_partitions_a:9659482112 --group qti_dynamic_partitions_b:9659482112 \
    --partition system_b:readonly:<size>:qti_dynamic_partitions_b --image system_b=system.img ... --sparse \
    --output super.img
fastboot flash super super.img
```

## 2c. Userdata

Nothing to flash. To start from a factory state, `fastboot erase userdata` (or `fastboot -w`); M1 formats a blank
userdata ext4 at the next boot (an f2fs one from `fastboot -w` is used as it is) and HarmonyOS starts with its setup
wizard.

`fastboot reboot`. M1 maps super, mounts userdata at `/data`, assembles `/hmos-root`, runs prep.sh and hands over
by itself. To hold at `root ready` instead (to inspect a root HarmonyOS init chokes on), create
`/data/asteroids/hold`; the steps below are then the manual handoff.

## 3. Hand over to HarmonyOS init

```bash
python3 ~/Documents/harmony/scripts/m0sh.py 'dmesg | grep hmos-m1'
python3 ~/Documents/harmony/scripts/m0sh.py 'touch /tmp/m1-go'
python3 ~/Documents/harmony/scripts/m0sh.py 'sleep 20; dmesg | grep -E "SAMGR|ServiceStart"' 40
```

The serial shell stays outside the HarmonyOS root and survives the handoff. To look inside, use
`chroot /hmos-root /system/bin/<tool>`: `hilog -x`, `param get`, `begetctl start_service <name>`. To change which
services start, rebuild asteroids.img (its `common/m1/etc-init`) and write it to super. If `/data/asteroids/blackbox`
exists, M1 starts `/asteroids/boot/blackbox.sh` before the handoff; it writes the kernel log, processes and mounts to
`/data/asteroids/blackbox/` every 2 seconds.

What M1's init does for HarmonyOS init, and why:

- `enforcing=0` in HarmonyOS's `/proc/cmdline`: its policy loader takes the SELinux mode from there. Without it the
  policy loads enforcing and every unlabeled M1 process (the serial shell included) is denied everything.
- The root is bound onto `/` as well as chrooted into: init's sandbox code uses `setns`, which resets its root to the
  namespace root. With only a chroot it ends up in M1's ramdisk and no service binary can be found.
- The binderfs nodes are chmod 0666: they are created 0600 root, and samgr (uid 5555) must open `/dev/binder`.

## 4. Restore Android on slot b

super holds HarmonyOS. To bring Android back, flash the stock set with its `flash_all.sh` (it writes super's logical
partitions from fastbootd, after flashing the stock boot chain and recovery) or as in its steps by hand, then
`fastboot -w` and `fastboot reboot`. Not tried since super changed.
