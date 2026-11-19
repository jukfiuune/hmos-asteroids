#!/usr/bin/env python3
"""Which HarmonyOS system symbols (HM Symbol, $r('sys.symbol.*')) an app uses, read from its compiled modules.abc.

    symbol-usage.py <sysResource.js> <modules.abc>

$r('sys.symbol.x') compiles to the symbol's numeric resource ID, so each ID from the SDK's table
(sdk/default/openharmony/ets/build-tools/ets-loader/sysResource.js) is looked for as an i32 or f64 literal. Prints
"<hits> <name>" per symbol found. A short random byte match can add a stray name; the icons an app really uses show up
with several hits. Used to match Petrelgram's icons to the system Messages app's (/system/app/Mms/Mms.hap)."""
import re
import struct
import sys

table = open(sys.argv[1]).read()
block = table[table.index("symbol: {"):]
block = block[:block.index("}")]
ids = {int(v): k for k, v in re.findall(r"(\w+): (\d+)", block)}
data = open(sys.argv[2], "rb").read()
found = {}
for rid, name in ids.items():
    n = data.count(struct.pack("<i", rid)) + data.count(struct.pack("<d", float(rid)))
    if n:
        found[name] = n
for name in sorted(found):
    print(found[name], name)
print("%d of %d symbols" % (len(found), len(ids)), file=sys.stderr)
