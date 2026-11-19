#!/usr/bin/env python3
"""The donor's refresh-rate policy, adapted to our composer as sheng's tools/patch_refresh_policy.py does for his.

    patch-hgm-policy.py <hgm_policy_config.xml> <out>

Settings' refresh-rate choices are refresh_rate_4settings: auto (-1), 60 (1) and 120 (2). The donor's 120 setting
runs strategy 8 (60-120, dynamic) with LTPO on, which this panel and composer don't do (no LTPO control, just the
DSI modes 60/90/120). As sheng, only the high-refresh setting changes: strategy 12 (fixed 120), LTPO off. Auto and 60
stay as they are.
"""
import re
import sys
import xml.etree.ElementTree as ET

src, out = sys.argv[1:3]
data = open(src, "rb").read()
root = ET.fromstring(data)
fixed = root.find('./Params[@name="refreshRate_strategy_config"]/Strategy[@name="12"]')
if fixed is None or fixed.get("min") != "120" or fixed.get("max") != "120":
    sys.exit("strategy 12 is not fixed 120 Hz (a different donor?)")
screens = list(re.finditer(rb'(<Params name="screen_config" type="LTPO-DEFAULT">)(.*?)(</Params>)', data, re.S))
if len(screens) != 1:
    sys.exit("expected one primary screen_config")
screen = screens[0]
settings = list(re.finditer(rb'<Setting id="2" strategy="8">.*?\n        </Setting>', screen[2], re.S))
if len(settings) != 1:
    sys.exit("expected the 120 Hz setting (id 2, strategy 8)")
setting = settings[0]
updated = setting[0].replace(b'<Setting id="2" strategy="8">', b'<Setting id="2" strategy="12">', 1)
before = b'<Ltpo name="switch" value="1"/>'
if updated.count(before) != 1:
    sys.exit("unexpected LTPO switch in the 120 Hz setting")
updated = updated.replace(before, b'<Ltpo name="switch" value="0"/>', 1)
start = screen.start(2) + setting.start()
end = screen.start(2) + setting.end()
result = data[:start] + updated + data[end:]
ET.fromstring(result)
open(out, "wb").write(result)
print("120 Hz setting: strategy 8 -> 12, LTPO off")
