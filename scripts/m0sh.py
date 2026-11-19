#!/usr/bin/env python3
# m0sh.py "<command>" [timeout]: runs one shell command on the phone's M0/M1 ACM console and prints exactly its
# output (no prompt, no echoed command), so it can be piped, e.g. into base64 -d.
import os, sys, termios, time, select, glob
# macOS names the ACM port after the gadget serial; Linux uses ttyACM*. M0SH_DEV overrides both.
dev = os.environ.get("M0SH_DEV") or sorted(glob.glob("/dev/cu.usbmodemasteroids*") + glob.glob("/dev/ttyACM*"))[0]
fd = os.open(dev, os.O_RDWR | os.O_NOCTTY)
a = termios.tcgetattr(fd); a[0] = 0; a[1] = 0; a[3] = 0; a[6][termios.VMIN] = 0; a[6][termios.VTIME] = 0
termios.tcsetattr(fd, termios.TCSANOW, a)
# A fresh shell's line editor asks the terminal for the cursor position and garbles the first line it reads, so
# send an empty line first and throw away whatever comes back.
os.write(fd, b"\n"); time.sleep(0.5)
while select.select([fd], [], [], 0.2)[0] and os.read(fd, 65536): pass
tag = "%d" % os.getpid()
# The markers are split with "" in the command, so only the shell's output of them matches, never the echo.
begin, end = "__M0_BEGIN_%s__\n" % tag, "__M0_END_%s__\n" % tag
os.write(fd, ("stty -echo 2>/dev/null; echo __M0_BEG\"\"IN_%s__; %s; echo __M0_E\"\"ND_%s__\n"
              % (tag, sys.argv[1], tag)).encode())
buf, deadline = b"", time.time() + float(sys.argv[2] if len(sys.argv) > 2 else 20)
while time.time() < deadline and end.encode() not in buf.replace(b"\r", b""):
    r, _, _ = select.select([fd], [], [], 0.5)
    if r:
        buf += os.read(fd, 65536)
out = buf.replace(b"\r", b"").decode(errors="replace")
if begin in out and end in out:
    sys.stdout.write(out.split(begin, 1)[1].split(end, 1)[0])
else:
    sys.stderr.write("m0sh: no complete reply within the timeout\n" + out)
    sys.exit(1)
