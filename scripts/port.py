#!/usr/bin/env python3
"""The port's workflow from the laptop: the repo is the only place files are edited; the build PC builds from a copy
of it, and the phone gets the results through M1's USB network shell (m0net.py).

The build machine is BUILD_HOST in port.conf (port.conf.example): user@host over ssh, or "local".

    port sync                         repo -> build machine: the whole repo to BUILD_ROOT/repo, which the builds read
    port build <target>...            sync, then build on the PC from that copy: boot (build/assemble-boot.sh),
                                      asteroids (build/build-asteroids.sh), android
                                      (platform/qcom/android/build-android-helpers.sh)
    port deploy <part>...             puts built images on the phone: asteroids, android and the donor's system,
                                      vendor, sys_prod, chip_prod, preload and cust go into super
                                      (make-super.sh, write-super.sh), boot, init_boot and vendor_boot into their
                                      _b partitions; then reboots and waits for the desktop (--no-reboot to skip)
    port reboot                       reboots, waits for the desktop, prints M1's and the helpers' log lines
    port log                          the hmos-* kernel log lines
    port sh "<cmd>" [timeout]         a command in M1's root shell
    port put <local> <phone> | get <phone> <local>
    port screens [N] [out.png]        the newest panel captures, side by side
"""
import os
import subprocess
import sys
import time

R = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(R, "scripts"))
import m0net  # noqa: E402



def conf():
    """BUILD_HOST and BUILD_ROOT from port.conf, overridden by the environment."""
    c = {"BUILD_HOST": "local", "BUILD_ROOT": "harmony-build"}
    p = os.path.join(R, "port.conf")
    if os.path.exists(p):
        for line in open(p):
            k, _, v = line.strip().partition("=")
            if k in c and v:
                c[k] = v.strip().strip('"')
    return {k: os.environ.get(k) or v for k, v in c.items()}


CONF = conf()
PC, B = CONF["BUILD_HOST"], CONF["BUILD_ROOT"]
LOCAL = PC == "local"
OUT = os.path.join(R, "out")
SUPER = ["system", "vendor", "sys_prod", "chip_prod", "preload", "cust", "asteroids", "android"]
# Built image -> path on the PC.
IMAGES = {"asteroids": "m2/asteroids.img", "android": "android-b/android.img",
          "boot": "src/out/images-m1/boot.img", "init_boot": "src/out/images-m1/init_boot.img",
          "vendor_boot": "src/out/images-m1/vendor_boot.img"}
# The donor's partitions, unmodified (build/unpack-donor.sh); written once, on the first install.
DONOR = "donor/pura70proplus-7.0.0.107"
IMAGES.update({p: f"{DONOR}/images/{p}.img" for p in ("system", "vendor", "sys_prod", "chip_prod", "cust")})
IMAGES["preload"] = f"{DONOR}/preload-pkg/x/preload.img"
BUILDS = {"boot": "bash repo/build/assemble-boot.sh", "asteroids": "bash repo/build/build-asteroids.sh",
          "android": "bash repo/platform/qcom/android/build-android-helpers.sh"}


def run(*cmd, **kw):
    print("+", " ".join(cmd) if len(" ".join(cmd)) < 160 else " ".join(cmd)[:157] + "...", flush=True)
    return subprocess.run(cmd, check=True, **kw)


def on_pc(cmd, check=True, **kw):
    """Runs a shell command on the build machine in BUILD_ROOT, with HARMONY_BUILD set to it."""
    cmd = f'cd {B} && export HARMONY_BUILD="$PWD" && {cmd}'
    if LOCAL:
        return subprocess.run(["bash", "-c", cmd], cwd=os.path.expanduser("~"), check=check, **kw)
    return subprocess.run(["ssh", PC, cmd], check=check, **kw)


def phone(cmd, timeout=30):
    return m0net.sh(cmd, timeout)


def sync():
    files = subprocess.run(["git", "-C", R, "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                           capture_output=True, check=True).stdout
    names = [f for f in files.decode().split("\0") if f and os.path.isfile(os.path.join(R, f))]
    tar = subprocess.Popen(["tar", "-C", R, "--null", "-T", "-", "-cz"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    tar.stdin.write("\0".join(names).encode())
    tar.stdin.close()
    on_pc("rm -rf repo.new && mkdir -p repo.new && tar -xz -C repo.new && rm -rf repo && mv repo.new repo",
          stdin=tar.stdout)
    tar.wait()
    print(f"{len(names)} files in {B}/repo")


def build(targets):
    sync()
    for t in targets:
        if t not in BUILDS:
            sys.exit(f"unknown target {t} (one of {', '.join(BUILDS)})")
        log = f"/tmp/port-build-{t}.log"
        r = on_pc(f"({BUILDS[t]}) > {log} 2>&1; rc=$?; tail -5 {log}; exit $rc", check=False)
        if r.returncode:
            sys.exit(f"build {t} failed (log on the build machine: {log})")


def fetch(part):
    os.makedirs(os.path.join(OUT, "super"), exist_ok=True)
    local = os.path.join(OUT, "super", f"{part}.img")
    if LOCAL:
        run("cp", os.path.join(os.path.expanduser("~"), B, IMAGES[part]), local)
    else:
        run("scp", "-q", f"{PC}:{B}/{IMAGES[part]}", local)
    return local


def wait_desktop(since):
    """Waits for after-boot.sh's 'desktop up' from a boot newer than `since` (phone uptime, s)."""
    deadline = time.time() + 300
    while time.time() < deadline:
        time.sleep(5)
        try:
            o = phone("cut -d' ' -f1 /proc/uptime; dmesg | grep -c 'hmos-after-boot: desktop up'", 8).split()
        except OSError:
            continue
        if len(o) == 2 and float(o[0]) < since + 600 and o[1] != "0":
            return float(o[0])
    sys.exit("no 'desktop up' within 5 minutes")


def reboot():
    phone("sync; chroot /hmos-root /system/bin/param set ohos.startup.powerctrl reboot", 8)
    time.sleep(15)
    up = wait_desktop(0)
    print(f"desktop up (uptime {up:.0f} s)")
    log()


def log():
    print(phone("dmesg | grep 'hmos-m1:' | grep -v 'modprobe .* failed'; cat /data/local/tmp/asteroids-boot.log", 15), end="")


def deploy(parts, reboot_after=True):
    boot = [p for p in parts if p in ("boot", "init_boot", "vendor_boot")]
    sup = [p for p in parts if p in SUPER]
    if len(boot) + len(sup) != len(parts):
        sys.exit(f"parts: {', '.join(list(IMAGES))}")
    if sup:
        run(os.path.join(R, "common/m1/super/make-super.sh"))
        d = "/data/local/tmp/super"
        phone(f"rm -rf {d}; mkdir -p {d}")
        for f in ("out/super/super-head.img", "out/super/layout.txt", "common/m1/super/write-super.sh"):
            m0net.put(os.path.join(R, f), f"{d}/{os.path.basename(f)}")
        for p in sup:
            m0net.put(fetch(p), f"{d}/{p}.img")
        o = phone(f"sh {d}/write-super.sh {d} {' '.join(sup)}; echo rc=$?; rm -rf {d}", 900)
        print(o, end="")
        if "rc=0" not in o:
            sys.exit("write-super.sh failed")
    for p in boot:
        local = fetch(p)
        m0net.put(local, f"/data/local/tmp/{p}.img")
        o = phone(f"""set -e; n={p}_b; dev=
            for u in /sys/class/block/*/uevent; do grep -q "^PARTNAME=$n$" $u && dev=/dev/$(basename $(dirname $u)); done
            f=/data/local/tmp/{p}.img; dd if=$f of=$dev bs=1M conv=fsync 2>/dev/null; sync
            [ "$(head -c $(stat -c %s $f) $dev | sha256sum | cut -c1-64)" = "$(sha256sum < $f | cut -c1-64)" ] &&
                echo "$n ($dev) written and checked"; rm $f""", 120)
        print(o, end="")
        if "checked" not in o:
            sys.exit(f"writing {p} failed")
    if reboot_after:
        reboot()


def screens(n="6", out="/tmp/screens.png"):
    run(os.path.join(R, "scripts/screens.sh"), n, out)


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    c, a = a[0], a[1:]
    if c == "sync":
        sync()
    elif c == "build":
        build(a)
    elif c == "deploy":
        deploy([p for p in a if p != "--no-reboot"], "--no-reboot" not in a)
    elif c == "reboot":
        reboot()
    elif c == "log":
        log()
    elif c == "sh":
        print(phone(a[0], int(a[1]) if len(a) > 1 else 30), end="")
    elif c == "put":
        m0net.put(a[0], a[1])
    elif c == "get":
        m0net.get(a[0], a[1])
    elif c == "screens":
        screens(*a)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
