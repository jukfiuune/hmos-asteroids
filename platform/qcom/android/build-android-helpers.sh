#!/bin/bash
# Builds android.img on the build PC: the android partition in super, the Android root that the audio, sensor,
# Bluetooth and vibration helpers run in (boot/svc/android-root.sh mounts it at /android), and that the camera root is
# made from (boot/svc/camera.sh). Android's slot b partitions
# hold 5.7 GB, far more than super has left, and the helpers use a few hundred files of them, so it takes:
#   - vendor: etc, without the voice assistant's models (etc/whisper); the camera's: lib64/camera (CamX's sensor,
#     tuning and pipeline modules, dlopened by path; of its feature components only those seen in use), the ICP firmware, CamX's override settings (ours, below), and the
#     closure of camera-observed.txt (what camserver, the service managers, the allocator and the perf HAL had mapped);
#     lib/rfsa (the DSP modules audioadsprpcd and sscrpcd hand the ADSP); the three daemons the helpers start; and
#     from lib64 the libraries that can be loaded: the dependency closure (DT_NEEDED, and any vendor library whose
#     name is written in a vendor binary: dlopen by name) of roots.txt and of our preloaded libraries, of every
#     library a vendor or odm config file names, and of observed.txt (the files these processes had read on
#     2026-11-15, by page-cache residency)
#   - system: lib64 by the same closure, and etc; system_ext's VINTF manifests (hwservicemanager checks them)
#   - odm: all of it but overlayfs_origin (a copy of system_ext)
#   - apex: the runtime APEX (Android's linker and bionic) and VNDK 34, whole, unpacked from system and system_ext
#   - empty directories where the helpers bind or mount things (camera: where camera.sh builds the camera root;
#     vendor/lib64/asteroids: where it binds our camera libraries, inside the vendor linker namespace's reach)
# Input: $HARMONY_BUILD/android-b/{system_b,system_ext_b,vendor_b,odm_b}.img: Nothing OS Asteroids_C5.0-261023-2146's
# system, system_ext, vendor and odm, as they were on slot b of super before HarmonyOS took it (copied off the phone;
# the same files as Nothing Archive's image-logical set for that build, github.com/spike0en/nothing_archive; checked
# against device/asteroids/android/inputs.sha256). Our preloaded libraries from
# $WORK/out-{audio,bluetooth,vibrator}. Output: $HARMONY_BUILD/android-b/android.img (EROFS, lz4hc).
# Needs erofs-utils (build/build-erofs-utils.sh), debugfs and readelf.
set -e
HB=${HARMONY_BUILD:-$HOME/harmony-build}
B=$HB/android-b
. "$(dirname "$0")/../../../build/env.sh"
E=$HB/tools/erofs-utils/bin
H=$(cd "$(dirname "$0")" && pwd)
X=$B/x
(cd $B && sha256sum -c --quiet $DEVICE/android/inputs.sha256)
rm -rf $X $B/root
mkdir -p $X
for p in system_b system_ext_b vendor_b odm_b; do
    $E/fsck.erofs --extract=$X/$p $B/$p.img >/dev/null
done
for a in runtime:system_b/system/apex/com.android.runtime.apex vndk34:system_ext_b/apex/com.android.vndk.v34.apex; do
    unzip -p $X/${a#*:} apex_payload.img > $X/apex-${a%%:*}.img
    mkdir -p $X/apex-${a%%:*}
    debugfs -R "rdump / $X/apex-${a%%:*}" $X/apex-${a%%:*}.img 2>/dev/null
done

python3 - $X $B/root $H/roots.txt $DEVICE/android/observed.txt $DEVICE/android/camera-observed.txt \
    $WORK/out-camera/libcamserver.so $WORK/out-camera/libpropd.so \
    $WORK/out-audio/libsheng_pal_server.so $WORK/out-audio/libsheng_pal_lifecycle.so $WORK/out-audio/libpal_no_hwbinder.so \
    $WORK/out-audio/pal_tone $WORK/out-bluetooth/libsheng_bt_bridge.so $WORK/out-vibrator/libasteroids_richtap.so <<'PY'
import os, re, shutil, subprocess, sys
X, root, roots_txt, observed_txt, camera_txt, *ours = sys.argv[1:]
# Where each top-level directory of the Android root comes from.
src = {"system": f"{X}/system_b/system", "system_ext": f"{X}/system_ext_b", "vendor": f"{X}/vendor_b", "odm": f"{X}/odm_b",
       "apex/com.android.runtime": f"{X}/apex-runtime", "apex/com.android.vndk.v34": f"{X}/apex-vndk34"}
def host(p):
    for k, v in src.items():
        if p == k or p.startswith(k + "/"):
            return v + p[len(k):]
    return None
# The helpers' LD_LIBRARY_PATH (start-audio.sh), in order.
libpath = ["vendor/lib64", "system/lib64", "apex/com.android.vndk.v34/lib64", "apex/com.android.runtime/lib64",
           "apex/com.android.runtime/lib64/bionic"]
keep = set()
def add(p):
    """Keeps p (a path in the Android root), the targets of its symlinks and the directories above it."""
    while p and p not in keep:
        h = host(p)
        if h is None or not os.path.lexists(h):
            return
        keep.add(p)
        if os.path.islink(h):
            t = os.readlink(h)
            add(os.path.normpath(t.lstrip("/") if t.startswith("/") else os.path.join(os.path.dirname(p), t)))
        p = os.path.dirname(p)
def needed(f):
    out = subprocess.run(["readelf", "-d", f], capture_output=True, text=True).stdout
    return re.findall(r"Shared library: \[(.*)\]", out)
# vendor/lib64's libraries by name (camera's own directory left out), for names found inside vendor binaries.
vl = host("vendor/lib64")
byname = {}
for dp, dns, fns in os.walk(vl):
    dns[:] = [d for d in dns if d != "camera"]
    for n in fns:
        byname.setdefault(n.encode(), "vendor/lib64" + dp[len(vl):] + "/" + n)
seen = set()
def closure(f, by_name=True):
    if f in seen:
        return
    seen.add(f)
    # Libraries a vendor binary (or ours) loads by a name written in it, as PAL does libaudiofeaturestats.so and CamX
    # libipebpsstriping.so and libhme.so (only while it configures streams).
    if by_name and (not f.startswith(X) or f.startswith(host("vendor"))):
        for t in set(re.split(rb"[^A-Za-z0-9_.@+-]+", open(f, "rb").read())):
            if t in byname:
                add(byname[t])
                closure(os.path.realpath(host(byname[t])))
    for n in needed(f):
        for d in libpath:
            h = host(f"{d}/{n}")
            if h and os.path.exists(h):
                add(f"{d}/{n}")
                closure(os.path.realpath(h), by_name)
                break
        else:
            print(f"not found: {n} (for {f})")
def tree(top, skip=()):
    for dp, dns, fns in os.walk(host(top)):
        rel = top + dp[len(host(top)):]
        dns[:] = [d for d in dns if f"{rel}/{d}" not in skip]
        for n in fns:
            add(f"{rel}/{n}")
        add(rel)

tree("vendor/etc", skip={"vendor/etc/whisper"})
# CamX's sensor, tuning and pipeline modules and its components (some loaded only while streams are configured); of
# Nothing's feature nodes (node/: ArcSoft's effects, hundreds of MB with their libraries) none.
tree("vendor/lib64/camera", skip={"vendor/lib64/camera/node"})
# And CamX's support libraries, some of which it loads by paths it puts together at run time (libhme.so,
# libcamxswprocessalgo.so, libipebpsstriping.so) while it configures streams.
for n in sorted(os.listdir(host("vendor/lib64"))):
    if re.match(r"(libcamx|libchi|libcom\.qti|com\.qti|libhme|lib.*striping)", n):
        add(f"vendor/lib64/{n}")
tree("system_ext/etc/vintf")
for n in sorted(os.listdir(host("vendor/firmware"))):
    if n.startswith("CAMERA_ICP"):
        add(f"vendor/firmware/{n}")
# The camera root's propd loads these; its linker namespaces find bionic through system/lib64's links into the
# runtime APEX, as Android's do.
for p in ("system/build.prop", "vendor/build.prop", "system/bin/linker64", "vendor/bin/sleep",
          "system/lib64/libc.so", "system/lib64/libdl.so", "system/lib64/libm.so", "system/lib64/libdl_android.so"):
    add(p)
tree("vendor/lib/rfsa")
tree("system/etc")
tree("odm", skip={"odm/overlayfs_origin"})
tree("apex/com.android.runtime")
tree("apex/com.android.vndk.v34")
add("vendor/dsp")
roots = [l.strip() for f in (roots_txt, observed_txt) for l in open(f) if l.strip() and not l.startswith("#")]
camera_roots = [l.strip() for l in open(camera_txt) if l.strip() and not l.startswith("#")]
camera_roots += ["vendor/lib64/hw/camera.qcom.so"] + sorted(p for p in keep if p.startswith("vendor/lib64/camera/") or
    re.match(r"vendor/lib64/(libcamx|libchi|libcom\.qti|com\.qti|libhme|lib.*striping)", p))
# Libraries named in vendor and odm configs (dlopened by name: PAL's and AGM's plugins, effects, sensor drivers).
named = set()
for p in list(keep):
    if p.startswith(("vendor/etc/", "odm/")) and not p.startswith("vendor/etc/camera/") and os.path.isfile(host(p)) and \
            os.path.getsize(host(p)) < 4 << 20:
        named.update(t for t in re.split(rb"[^A-Za-z0-9_.@+-]+", open(host(p), "rb").read()) if t.endswith(b".so"))
roots += [byname[n] for n in named if n in byname]
for r in roots + camera_roots:
    add(r)
    h = host(r)
    if h and os.path.isfile(h) and open(h, "rb").read(4) == b"\x7fELF":
        closure(os.path.realpath(h))
for f in ours:
    if os.path.exists(f):  # the camera's are built against this image's bionic: missing on a first build
        closure(f, "camera" not in f)

os.makedirs(root)
size = 0
for p in sorted(keep):
    h, d = host(p), f"{root}/{p}"
    if os.path.islink(h):
        os.symlink(os.readlink(h), d)
    elif os.path.isdir(h):
        os.makedirs(d, exist_ok=True)
        shutil.copymode(h, d)
    else:
        os.makedirs(os.path.dirname(d), exist_ok=True)
        shutil.copy2(h, d)
        size += os.path.getsize(h)
for d in ("dev", "proc", "sys", "data", "linkerconfig", "m2", "etc-rw", "bt_firmware", "mnt/vendor/persist",
          "hmos-socket", "m2bt", "m2vib", "vendor/dsp", "system/bin", "camera", "vendor/lib64/asteroids"):
    os.makedirs(f"{root}/{d}", exist_ok=True)
os.symlink("system/bin", f"{root}/bin")
# CamX's overrides: CHI's own stream buffer manager off (it asks for buffers on top of the request's and keeps one per
# frame), and MFNR off (its JPEG path wants a queue of earlier full-size frames). The non-camera sensor service (NCS:
# the gyro for EIS) stays on: its FastRPC channel to the sensor PD blocked only while nothing served that PD, and
# sscrpcd (platform/qcom/boot/svc/sscrpcd.sh) does now. platform/qcom/camera/test/README.md has the story.
with open(f"{root}/vendor/etc/camera/camxoverridesettings.txt", "w") as f:
    f.write("halBufferMgrMode=0\noverrideEnableMFNR=FALSE\n")
print(f"{len(keep)} paths, {size / 1e6:.1f} MB of files")
PY
$E/mkfs.erofs -zlz4hc --all-root -T0 --quiet $B/android.img.new $B/root
mv $B/android.img.new $B/android.img
# Android's bionic and libc++, which our libraries for this root (audio, Bluetooth, vibration) link against.
mkdir -p $WORK/bionic-link
cp $B/root/apex/com.android.runtime/lib64/bionic/libc.so $B/root/apex/com.android.runtime/lib64/bionic/libdl.so \
   $B/root/apex/com.android.runtime/lib64/bionic/libm.so $B/root/system/lib64/libc++.so $WORK/bionic-link/
rm -rf $X
ls -l $B/android.img
