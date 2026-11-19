#!/usr/bin/env python3
"""Copies an allowlist of HarmonyOS service configs from the donor's /system/etc/init for the M1 root, with each
service's "critical" setting removed: a critical service that dies makes HarmonyOS init reboot the phone, which
turns every bring-up crash into a boot loop.

    make-m1-services.py <donor system/etc/init dir> <out dir> <cfg>...

Each <cfg> is a file name in the init dir, or a path relative to it (for configs in chip_prod or vendor), optionally
followed by ":name,name" to keep only those services from it (jobs are kept). Output files are named after the
config's file name.
"""
import json
import os
import sys

src, out, names = sys.argv[1], sys.argv[2], sys.argv[3:]
os.makedirs(out, exist_ok=True)
for arg in names:
    path, _, keep = arg.partition(":")
    name = os.path.basename(path)
    cfg = json.load(open(os.path.join(src, path)))
    if keep:
        wanted = keep.split(",")
        cfg["services"] = [svc for svc in cfg.get("services", []) if svc["name"] in wanted]
        missing = set(wanted) - {svc["name"] for svc in cfg["services"]}
        if missing:
            sys.exit("%s: no service %s" % (name, ", ".join(sorted(missing))))
    for svc in cfg.get("services", []):
        if svc.pop("critical", None) is not None:
            print("%s: %s no longer critical" % (name, svc["name"]))
    with open(os.path.join(out, name), "w") as f:
        json.dump(cfg, f, indent=4)
        f.write("\n")
    print("%s: %d services, %d jobs" % (name, len(cfg.get("services", [])), len(cfg.get("jobs", []))))
