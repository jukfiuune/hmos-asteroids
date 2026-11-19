#!/usr/bin/env python3
"""Talks to M1 over its USB network (CDC NCM) instead of the serial port: much faster, and it doesn't share a tty.

    m0net.py sh "<command>" [timeout]     runs one command in a fresh root shell, prints its output
    m0net.py put <local> <phone path>     copies a file to the phone (checked with sha256)
    m0net.py get <phone path> <local>     copies a file from the phone (checked with sha256)

The phone is fe80::a5:7eff:fe00:1 on whichever interface has the gadget's host MAC 02:a5:7e:00:00:02; its shell
listens on TCP 2323 (nc -lk ... -e sh, outside the HarmonyOS chroot), bound to that address so it is not reachable over
Wi-Fi. Transfers use a one-shot listener on 2324, bound the same way.
"""
import glob
import hashlib
import os
import socket
import sys
import time

PHONE = "fe80::a5:7eff:fe00:1"
BIND = "-s %s%%m1link " % PHONE
HOST_MAC = "02:a5:7e:00:00:02"


def iface():
    for p in glob.glob("/sys/class/net/*/address"):
        if open(p).read().strip() == HOST_MAC:
            return p.split("/")[4]
    sys.exit("m0net: no interface with MAC %s (is M1 up with its network function?)" % HOST_MAC)


def connect(port, timeout):
    s = socket.create_connection((PHONE + "%" + iface(), port), timeout=timeout)
    s.settimeout(timeout)
    return s


def sh(cmd, timeout=30):
    s = connect(2323, timeout)
    s.sendall((cmd + "\nexit\n").encode())
    out = b""
    try:
        while True:
            d = s.recv(65536)
            if not d:
                break
            out += d
    except socket.timeout:
        sys.stderr.write("m0net: timed out\n")
    return out.decode(errors="replace")


def put(local, remote):
    data = open(local, "rb").read()
    sh("rm -f '%s'; (nc -l %s-p 2324 > '%s') </dev/null >/dev/null 2>&1 &" % (remote, BIND, remote), 10)
    time.sleep(0.5)
    s = connect(2324, 60)
    s.sendall(data)
    s.shutdown(socket.SHUT_WR)
    s.recv(1)
    s.close()
    time.sleep(0.3)
    want = hashlib.sha256(data).hexdigest()
    # Synced before the check: HarmonyOS's reboot doesn't flush, and an unsynced bundle came back empty after one.
    got = sh("sync; sha256sum '%s'" % remote).split(" ")[0].strip()
    if got != want:
        sys.exit("m0net: sha256 mismatch on the phone: %s, expected %s" % (got, want))
    print("%s -> %s (%d bytes, sha256 ok)" % (local, remote, len(data)))


def get(remote, local):
    want = sh("sha256sum '%s'" % remote).split(" ")[0].strip()
    sh("(nc -l %s-p 2324 < '%s') </dev/null >/dev/null 2>&1 &" % (BIND, remote), 10)
    time.sleep(0.5)
    s = connect(2324, 60)
    s.shutdown(socket.SHUT_WR)
    data = b""
    while True:
        d = s.recv(1 << 20)
        if not d:
            break
        data += d
    if hashlib.sha256(data).hexdigest() != want:
        sys.exit("m0net: sha256 mismatch for %s" % remote)
    open(local, "wb").write(data)
    print("%s -> %s (%d bytes, sha256 ok)" % (remote, local, len(data)))


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[0] == "sh":
        sys.stdout.write(sh(a[1], float(a[2]) if len(a) > 2 else 30))
    elif a[0] == "put":
        put(a[1], a[2])
    elif a[0] == "get":
        get(a[1], a[2])
