#!/usr/bin/env python3
"""Adds our developer signing identity to the donor's HAP trust config, so apps we sign with the private key in
tools/asteroids-signing/ install on this port. HarmonyOS's app verifier (base/security/appverify) reads its trusted
roots from /system/etc/security/trusted_root_ca.json and its trusted signing sources from trusted_apps_sources.json;
this port builds both. We add our self-signed root CA (asteroids-root.cer) as a trusted root and a source entry naming
our app/profile signing certs. Signature verification stays on; the device simply also trusts our developer cert, as
provisioning a device for development does.

    merge-trust.py <donor security dir> <out dir> <our root .cer>
"""
import json
import os
import sys

donor, out, root_cer = sys.argv[1], sys.argv[2], sys.argv[3]
O = "C=CN, O=Asteroids, OU=Asteroids Port"
root_pem = open(root_cer).read()
if not root_pem.endswith("\n"):
    root_pem += "\n"

roots = json.load(open(os.path.join(donor, "trusted_root_ca.json")))
roots[f"{O}, CN=Asteroids Root CA"] = root_pem

sources = json.load(open(os.path.join(donor, "trusted_apps_sources.json")))
names = {s["name"] for s in sources["trust-app-source"]}
if "asteroids dev" not in names:
    sources["trust-app-source"].append({
        "name": "asteroids dev",
        "app-signing-cert": f"{O}, CN=Asteroids Application Release",
        "profile-signing-certificate": f"{O}, CN=Asteroids Profile Release",
        "issuer-ca": f"{O}, CN=Asteroids Signing CA",
        "root-ca": f"{O}, CN=Asteroids Root CA",
        "max-certs-path": 3,
        "critialcal-cert-extension": [],  # the field name is misspelled in the donor; keep it to match the reader
    })

os.makedirs(out, exist_ok=True)
json.dump(roots, open(os.path.join(out, "trusted_root_ca.json"), "w"), indent=4, ensure_ascii=False)
json.dump(sources, open(os.path.join(out, "trusted_apps_sources.json"), "w"), indent=4, ensure_ascii=False)
print("trust: %d roots, %d sources" % (len(roots), len(sources["trust-app-source"])))
