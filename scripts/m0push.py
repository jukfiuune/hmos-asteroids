#!/usr/bin/env python3
# m0push.py <local file> <phone path>: copies a small file to the phone over the M0/M1 serial shell (base64 in a
# heredoc, 1000 characters a line because the tty cuts lines at 4 KiB) and checks its sha256 on the phone.
import base64, hashlib, os, subprocess, sys
src, dst = sys.argv[1], sys.argv[2]
data = open(src, "rb").read()
b64 = base64.b64encode(data).decode()
m0sh = os.path.join(os.path.dirname(os.path.abspath(__file__)), "m0sh.py")
sh = lambda cmd, t="60": subprocess.run([sys.executable, m0sh, cmd, t], check=True, capture_output=True, text=True).stdout
sh("rm -f %s.b64" % dst)
step = 64 * 1000
for i in range(0, len(b64), step):
    lines = "\n".join(b64[j:j + 1000] for j in range(i, min(i + step, len(b64)), 1000))
    sh("cat >> %s.b64 <<'M0PUSH'\n%s\nM0PUSH\ntrue" % (dst, lines))
sh("base64 -d %s.b64 > %s && rm %s.b64" % (dst, dst, dst))
want = hashlib.sha256(data).hexdigest()
got = sh("sha256sum %s" % dst).split()[0]
if got != want:
    sys.exit("m0push: sha256 mismatch on the phone: %s, expected %s" % (got, want))
print("%s -> %s (%d bytes, sha256 ok)" % (src, dst, len(data)))
