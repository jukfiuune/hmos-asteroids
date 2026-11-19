#!/bin/bash
# Builds the Adreno anco_spec for the anco (DroiTong) Android container: the directory the container binds at the
# guest's /vendor_spec, which the guest's init overlays onto its /vendor. On Huawei phones the host's
# /vendor/anco_spec/vendor carries the guest's native GPU driver (Kirin: libGLES_v200, vulkan.hvgr_v200) and its gralloc
# (Huawei's allocator service and mapper); here those are Qualcomm's, from Nothing's own Android vendor: Adreno
# EGL/GLES/Vulkan, the QTI allocator service and the QTI mapper 4.0, with every library they load that the guest's
# Android 16 system and vendor don't already provide. Everything else (NPU, camera, codecs, sensors, TEE, the vendor
# SELinux files) stays as BRE-AL80 ships it, the same build as the guest image.
# Deploy (test hook for now): untar on the phone, chcon each path per labels.txt, bind vendor/ over the host's
# /vendor/anco_spec/vendor, and pass /dev/kgsl-3d0 and the dma-buf heaps to the guest in the override hostconfig.
#
# Nothing's vendor is 64-bit only, so the guest's 32-bit processes get no GLES/Vulkan.
#
# Inputs (build PC): fw/bre-al80/vendor_x/anco_spec/vendor (BRE-AL80's, from build-anco-preset.sh's OTA), the guest
# image extracted at fw/bre-al80/anco_x, Nothing's vendor at android-b/xv (platform/qcom/android/build-android-helpers.sh).
# Output: fw/anco-adreno/vendor/ (the tree), fw/anco-adreno/labels.txt (path -> SELinux label, applied on the phone;
# these are the host's aoco_* types, copied from the Kirin files they replace) and fw/anco-adreno/anco-adreno.tar.
set -e
HB=${HARMONY_BUILD:-$HOME/harmony-build}
B=$HB
BASE=$B/fw/bre-al80/vendor_x/anco_spec/vendor
G=$B/fw/bre-al80/anco_x
NV=$B/android-b/xv
O=$B/fw/anco-adreno
rm -rf $O && mkdir -p $O
W=$O/vendor
cp -a $BASE $W

# Out: the Kirin GPU driver (Mali GLES, vulkan.burbank) and Huawei's gralloc module, which ro.hardware.* pointed at
# (egl=mali, vulkan=burbank, gralloc=burbank_anco). Qualcomm's AIDL allocator and mapper 4.0 take precedence over the
# guest vendor's own HIDL allocator 2.0, which stays (on gralloc.default).
rm -f $W/lib*/egl/libGLES_mali.so $W/lib*/hw/vulkan.burbank.so $W/lib*/hw/gralloc.sdfnkos_rgm.so
sed -i -E '/^ro\.hardware\.(egl|vulkan|gralloc)=/d' $W/build.prop
printf '%s\n' ro.hardware.egl=adreno ro.hardware.vulkan=adreno ro.hardware.gralloc=default >> $W/build.prop
# Linear buffers only: the guest's window buffers go to the host compositor over the AncoHwc bridge, and the host's
# buffer import (platform/qcom/display/buffer_vdi.cpp) takes linear layouts only, not Qualcomm's UBWC compression.
printf '%s\n' vendor.gralloc.disable_ubwc=1 debug.gralloc.enable_fb_ubwc=0 >> $W/build.prop
# Out: Huawei's keymaster module, which keeps its keys in iTrustee (Kirin's TEE, absent here: every key failed with
# TEEC_ERROR_GENERIC). Without a keystore_anco module, the guest's keymaster 3.0 uses AOSP's software keymaster,
# provided ro.tee.anco.hwkeymaster.enbale is false (prep.d/65-anco.sh turns off the host parameter it comes from).
rm -f $W/lib*/hw/keystore_anco.sdfnkos_rgm.so

# The guest's cgroup root (lxc's cgroup2 mount, its own namespace root) belongs to root; stock Android chowns it when it
# mounts cgroup2, Huawei's guest skips that mount ("ANCO not need mount cgroup2"). system_server moves webview and app
# zygote children into their cgroups itself, which takes write access to the root's cgroup.procs: without it, the
# first WebView sandbox (the app store's) asserts system_server down ("Unable to create process group").
cat > $W/etc/init/init.anco_qcom.rc <<'RC'
# asteroids (build-anco-adreno.sh): system_server places webview/app zygote children in cgroups itself.
on early-init
    chown system system /sys/fs/cgroup/cgroup.procs
    chown system system /sys/fs/cgroup/cgroup.threads
    chmod 0664 /sys/fs/cgroup/cgroup.procs
    chmod 0664 /sys/fs/cgroup/cgroup.threads
RC

# Out for now: Huawei's sensor HALs. Their implementation reads Kirin's sensor hub (/dev/sensorhub_lake), and while
# the VINTF manifest declares them, system_server waits forever for them (Watchdog). Undeclared, the guest boots
# without sensors until a bridge to the port's own sensor stack exists.
rm -f $W/etc/vintf/manifest/vendor.huawei.hardware.sensors@1.3.xml $W/etc/vintf/manifest/android.hardware.sensors@2.1-multihal.xml \
      $W/etc/init/vendor.huawei.hardware.sensors@1.3-service_anco.rc $W/etc/init/android.hardware.sensors@2.1-service-multihal.rc

# In: Nothing's GPU and gralloc entry points.
# All of lib64/egl: libEGL_adreno dlopens egl/eglSubDriverAndroid.so in eglGetDisplay (no display without it).
ROOTS="$(cd $NV && ls lib64/egl/*.so) lib64/hw/vulkan.adreno.so
lib64/hw/android.hardware.graphics.mapper@4.0-impl-qti-display.so bin/hw/vendor.qti.hardware.display.allocator-service"
for f in $ROOTS etc/init/vendor.qti.hardware.display.allocator-service.rc \
         etc/vintf/manifest/android.hardware.graphics.mapper-impl-qti-display.xml \
         etc/vintf/manifest/vendor.qti.hardware.display.allocator-service.xml; do
    install -D -m 644 $NV/$f $W/$f
done
chmod 755 $W/bin/hw/vendor.qti.hardware.display.allocator-service

# Their libraries: DT_NEEDED, plus any Nothing vendor library whose name one of them writes out (the GPU driver
# dlopens libgsl, libllvm-glnext and friends by name, and libllvm-glnext in turn dlopens the core shader compiler,
# libllvm-qgl: without it every hwui app aborts on its first shader, "Failed to load core compiler"). A name the
# guest has (system, system_ext, vendor, or the runtime APEX's bionic) uses the guest's; otherwise Nothing's lib64
# copy comes along.
python3 -I - "$NV" "$G" "$W" $ROOTS <<'EOF'
import os, re, subprocess, sys
nv, g, w, roots = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
have = {"libc.so", "libm.so", "libdl.so", "libdl_android.so"}
for d in ("system/lib64", "system_ext/lib64", "vendor/lib64", "vendor/lib64/hw"):
    p = os.path.join(g, d)
    if os.path.isdir(p):
        have |= set(os.listdir(p))
for d in ("lib64", "lib64/hw", "lib64/egl"):
    p = os.path.join(w, d)
    if os.path.isdir(p):
        have |= set(os.listdir(p))
nvlib = set(os.listdir(os.path.join(nv, "lib64")))
def needed(path):
    out = subprocess.run(["readelf", "-dW", path], capture_output=True, text=True).stdout
    return re.findall(r"\(NEEDED\).*\[(.+?)\]", out)
def dlopened(path):
    data = open(path, "rb").read()
    return {n.decode() for n in re.findall(rb"(lib[A-Za-z0-9_+.\-]+\.so)\x00", data)}
todo, taken, missing = list(roots), [], set()
seen = set()
while todo:
    rel = todo.pop()
    if rel in seen:
        continue
    seen.add(rel)
    src = os.path.join(nv, rel)
    if rel not in roots:
        dst = os.path.join(w, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        subprocess.run(["cp", "-a", src, dst], check=True)
        taken.append(rel)
    for n in needed(src):
        if n in have:
            continue
        if n in nvlib:
            todo.append("lib64/" + n)
            have.add(n)
        else:
            missing.add(n)
    if rel.endswith(".so"):
        for n in dlopened(src):
            if n in nvlib and n not in have:
                todo.append("lib64/" + n)
                have.add(n)
print("taken from Nothing's vendor: %d libraries" % len(taken))
for t in sorted(taken):
    print("  " + t)
if missing:
    print("UNRESOLVED: " + " ".join(sorted(missing)))
EOF

# AOSP's libdmabufheap over the guest vendor's own. Huawei's build hands the caller's ION-style flags to
# DMA_HEAP_IOCTL_ALLOC as heap_flags (Kirin's kernel takes them); Linux returns EINVAL for any nonzero heap_flags, so
# every allocation through QTI gralloc failed ("Unable to allocate from DMA-BUF heap: qcom,system: Invalid argument"),
# apps got no window buffers and hwui looped on "Surface lost". AOSP's passes 0. Nothing in the guest vendor imports
# the two symbols only Huawei's has (DmabufSetName, the four-argument DmabufAlloc). 64-bit only: buffers are
# allocated in the (64-bit) allocator service.
install -D -m 644 $B/android-b/root/apex/com.android.vndk.v34/lib64/libdmabufheap.so $W/lib64/libdmabufheap.so

# Labels: the host policy's own file_contexts entries for /vendor/anco_spec/vendor (BRE-AL80's vendor and the donor's
# system policy; the last matching entry wins, as in file_contexts), which give the guest's services their exec types
# (init won't start a service whose binary has no domain transition). Ours on top: the GPU driver and gralloc
# libraries load into app processes (aoco_same_process_hal_file), the QTI allocator runs as the graphics allocator.
FC="$B/fw/bre-al80/vendor_x/etc/selinux/targeted/contexts/file_contexts
$B/donor/pura70proplus-7.0.0.107/x/system/system/etc/selinux/targeted/contexts/file_contexts"
python3 -I - "$O" "$BASE" $FC <<'EOF' > $O/labels.txt
import os, re, sys
o, base, fcs = sys.argv[1], sys.argv[2], sys.argv[3:]
rules = []
for fc in fcs:
    for line in open(fc):
        f = line.split()
        if len(f) >= 2 and f[0].startswith("/vendor/anco_spec/vendor"):
            rules.append((re.compile(f[0] + "$"), f[-1]))
paths = []
for root, dirs, files in os.walk(os.path.join(o, "vendor")):
    paths += [root] + [os.path.join(root, x) for x in files]
for full in paths:
    p = os.path.relpath(full, o)
    rel = os.path.relpath(full, os.path.join(o, "vendor"))
    label = "u:object_r:aoco_vendor_file:s0"
    for rx, l in rules:
        if rx.match("/vendor/anco_spec/" + p):
            label = l
    if rel == "bin/hw/vendor.qti.hardware.display.allocator-service":
        label = "u:object_r:aoco_hal_graphics_allocator_default_exec:s0"
    elif rel.endswith(".so") and not os.path.exists(os.path.join(base, rel)):
        label = "u:object_r:aoco_same_process_hal_file:s0"
    print(label, p)
EOF

# The guest's libgui with token-mode delegation off (the boot hook binds it over the guest's /system/lib64/libgui.so).
# DelegatorController::SendAncoGraphicVersionToBroker probes the host broker with SetAlphaWithWindowId(-999, 115):
# 0, 5 or 8 means an old broker, anything else is taken as the broker's version, and bit 1 of it turns on the newer
# "token" protocol (request 101 to the host's ProducerDelegator). Our donor's broker answers that probe with an error,
# 243, which the guest reads as a token-capable version 243; the donor's libdelegator then rejects every frame
# ("recv unkonw request(101)"). One instruction: the bit-1 test (tbz w20, #1) becomes an unconditional branch past
# setting the token flag. Checked against the expected instruction before writing.
mkdir -p $O/system/lib64
python3 -I - $G/system/lib64/libgui.so $O/system/lib64/libgui.so <<'EOF'
import subprocess, sys
src, dst = sys.argv[1:3]
d = bytearray(open(src, "rb").read())
nm = subprocess.run(["llvm-nm", "-D", "--defined-only", src], capture_output=True, text=True).stdout
fn = [int(l.split()[0], 16) for l in nm.splitlines() if "SendAncoGraphicVersionToBroker" in l][0]
# text is mapped at its file offset in this library (first LOAD segment at 0); the tbz sits 0x80 into the function
off = fn + 0x80
ins = int.from_bytes(d[off:off + 4], "little")
assert ins >> 24 == 0x36 and (ins >> 19) & 0x1f == 1 and ins & 0x1f == 20, "unexpected instruction %08x" % ins
imm14 = (ins >> 5) & 0x3fff
d[off:off + 4] = (0x14000000 | imm14).to_bytes(4, "little")   # b <same target>
open(dst, "wb").write(d)
print("libgui: token probe patched at 0x%x (%08x -> %08x)" % (off, ins, 0x14000000 | imm14))
EOF
echo "u:object_r:aoco_system_lib_file:s0 system/lib64/libgui.so" >> $O/labels.txt
grep -c . $O/labels.txt
(cd $O && tar -cf anco-adreno.tar vendor system labels.txt)
echo "=== $O/anco-adreno.tar ($(du -sh $W | cut -f1)) ==="
