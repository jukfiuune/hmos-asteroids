# Storage and partitions

## Layout (UFS, LUNs sda-sdf)

| Partition | Use |
|---|---|
| super (sda6, 9.66 GB) | everything that boots: the donor's images, asteroids, android (below) |
| userdata (sda13, 225 GB) | HarmonyOS's `/data` and nothing else; a blank one is formatted at boot |
| boot_b, init_boot_b, vendor_boot_b, dtbo_b | our boot chain ([boot.md](boot.md)) |
| modem_b (vfat) | modem, WPSS, ADSP firmware; mounted read-only by M1 |
| dsp_b | the DSP's dynamic modules, for audioadsprpcd ([audio.md](audio.md)) |
| modemst1, modemst2, fsg, fsc | the modem's EFS, served by rmtfs ([modem.md](modem.md)) |
| persist | read-only; the sensors' registry gets a writable copy ([sensors.md](sensors.md)) |

Never written: xbl, abl, tz, modem. The partition table is stock: the 64 GiB userdata split was undone (original GPT
in `device/asteroids/gpt/`). LUN 0's GPT has 32 entries but Qualcomm reserves 16 KiB, so the backup entries start at N-5. LUN 4's
GPT attribute bytes are written for the slot-success bit ([boot.md](boot.md)).

## super

Logical partitions on slot b, group qti_dynamic_partitions_b, LP metadata 10.2 as stock (64 KiB, 3 slots, virtual
A/B flag, groups qti_dynamic_partitions_a/_b of 9,659,482,112 bytes, 1 MiB-aligned extents). All read-only EROFS:

| Partition | Size | Contents, built by |
|---|---|---|
| system_b, vendor_b, sys_prod_b, chip_prod_b, preload_b, cust_b | 8.5 GB | the donor's images, unmodified (PC `~/harmony-build/donor/pura70proplus-7.0.0.107/images`, `preload-pkg/x/preload.img`) |
| asteroids_b | 256 MiB (53 MB used) | ours: the M2 bundle, the service set, M1's busybox for scripts that outlive the ramdisk, the USB DHCP server; `build/build-asteroids.sh` |
| android_b | the rest | the Android files the audio, sensor, Bluetooth and vibration helpers use, cut down from Android's slot b system, system_ext, vendor and odm; `platform/qcom/android/build-android-helpers.sh` |

`common/m1/super/make-super.sh` (laptop) makes the metadata from the image sizes on the build PC; `common/m1/super/write-super.sh`
(phone, M1's shell) writes the images it is given and the metadata, refuses if a partition it doesn't write would
move, and checks the metadata and every written image afterwards. Partitions are laid out in that order, so updating
asteroids (the one that changes) rewrites only its extent. M1 maps them with `lpmap` (common/m1/lpmap.c, device-mapper,
read-only) and mounts `/dev/mapper/<name>_b`: the donor's under `/hmos`, asteroids at `/asteroids`; prep.sh mounts
preload, the asteroids_android service android at `/android`.

Android's own partitions are gone from super (since 2026-11-14). Their slot b images (system_b, system_ext_b,
vendor_b, odm_b, as they were in super) are kept on the build PC in `~/harmony-build/android-b/` (SHA256SUMS there),
the input of build-android-helpers.sh. The whole of Android's super is not kept.

## userdata

HarmonyOS's `/data`, nothing else: M1 mounts it at `/data` and binds it into the root. Our own state lives in
`/data/asteroids/`: `android/` (the Android helpers' `/data`: the Bluetooth address, the sensor registry copy, audio),
`modem/` (the modem's read-write files, tqftpserv), `clock-offset`, and the switches `hold` (M1 waits for
`/tmp/m1-go` before the handoff), `blackbox` (start common/boot/blackbox.sh, logs in `blackbox/`), `no-capture` (no panel
captures) and `m2-test.sh` (sourced by prep.sh). All of it is made again when missing.

A blank userdata (its first 8 KiB zeros and no filesystem found, as after `fastboot erase userdata`) is formatted
ext4 by M1 with the donor's `mke2fs`; an f2fs one (`fastboot -w` may make one) is mounted as it is. HarmonyOS then
starts as on a first boot: setup wizard, preload apps. Anything else that doesn't mount holds the boot rather than
being formatted.

Before 2026-11-15 userdata was "hmosdata": HarmonyOS's data in `data/`, beside the donor's image files, copies of
Android's images, the bundle (unpacked on every boot) and backups. `common/m1/migrate-userdata.sh` (run by M1 when it finds
that layout) moved HarmonyOS's data to the top and our state into `/data/asteroids`, and the rest into
`/data/asteroids/old-hmosdata`.

## Encryption and user storage

- `/data` is unencrypted: the kernel has no fscrypt, and init.cfg's synchronous `sdc filecrypt init_main_user` times
  out (`init_main_user failed`) after holding init ~20 s at M1 time.
- User 100's storage needs hmdfs, which this kernel lacks (ENODEV). As sheng (apply-storage-local-v1.sh):
  `const.distributed_file_property.enabled=false` (prep.sh binds a copy of distributed_file.para), so storage_daemon
  mounts user storage locally; the account is then verified and appspawn stops building `*_preunlock` sandboxes
  ("App data is being decrypted").
- Files showed only the gallery folder: apps' `/storage/Users` is a sandbox bind of `/mnt/user/100/sharefs/docs`, and
  the kernel has no sharefs either. svc/booted.sh waits for storage_daemon's mount and binds the real Docs over it in
  init's namespace (nsenter). Every app sees all of Docs (sharefs would have limited each to its share). These mounts
  live in init's namespace: look through `/proc/1/root` or `/proc/1/mountinfo`, not M1's `/hmos-root/mnt`.

## Backups

- `device/asteroids/efs-backup/{modemst1,modemst2,fsg,fsc}.img` on the laptop (git-ignored: IMEI and RF calibration).
- Android's slot b images: PC `~/harmony-build/android-b/`. Earlier boot images: `images/` and PC
  `~/harmony-build/m1-backup/`.
- Android restore set: `~/Downloads/flash_20261104_124306` (stock images) and `tools/kernelsu/out/init_boot_ksu.img`.
