#!/usr/bin/env python3
"""Makes our sys_prod/etc/camera/CameraProfiles.json from the donor's (build/build-etc.sh).

Huawei's Camera app takes its photo, preview and video sizes, its modes and its zoom steps from this file (through
the config policy, CameraProfilesJsonUtilV2), not from the camera's abilities: the donor's is the Pura 70's
(4096x3072 YUV photos for Huawei's own JPEG pipeline, three back lenses, a dozen modes). With it the app asked for a
4096x3072 YUV photo, which this camera doesn't offer, and never started the preview. Ours, per camera position
(cameraType 1 back, 2 front), has what camera_host offers (device/asteroids/camera/camera_host.cpp): JPEG photos at
the HAL's sizes, NV21 previews up to 2048 wide, 1080p and 720p video; the photo and video modes only; zoom
steps across the back lenses; and the features that need Huawei's own processing (XMAGE, HDR Vivid, live photos, HEIF, ...) off.

    make-camera-profiles.py <donor CameraProfiles.json> <out>
"""
import json
import sys

# Sizes from the HAL's stream configurations (camtool list): the largest JPEG per aspect ratio, a preview the bridge
# copies at 30 fps.
PHOTO = {
    1: {"4:3": (4080, 3072), "1:1": (3072, 3072), "full": (4080, 1840), "16:9": (4080, 2296)},
    2: {"4:3": (3280, 2464), "1:1": (2464, 2464), "full": (3280, 1476), "16:9": (3280, 1846)},
}
PREVIEW = {"4:3": (1440, 1080), "1:1": (1080, 1080), "full": (2040, 928), "16:9": (1920, 1080)}
VIDEO = [("16:9 1080p", 1920, 1080), ("16:9 720p", 1280, 720)]
# The zoom bar's points: the back camera's zoom (camserver's lens group) goes from the ultrawide lens (0.66 by the
# lenses' fields of view) through the main one (1) to the tele (1.98) and on digitally to 10.
ZOOM = {1: {"quick": [0.66, 1, 2, 4], "optical": [0.66, 1, 2, 4, 10]}, 2: {"quick": [1], "optical": [1]}}


def ratio_key(ratio):
    for k in ("4:3", "1:1", "full", "16:9"):
        if ratio.startswith(k):
            return k
    return "16:9"


def profiles(position):
    photo = [{"ratio": r, "profileWidth": w, "profileHeight": h, "format": ["JPEG"]}
             for r, (w, h) in PHOTO[position].items() if r != "16:9"]
    preview = [{"ratio": r, "profileWidth": w, "profileHeight": h, "format": ["YUV_420_SP"]}
               for r, (w, h) in PREVIEW.items() if r != "16:9"]
    video = [{"ratio": r, "profileWidth": w, "profileHeight": h, "format": ["YUV_420_SP"], "frameRateArray": [30]}
             for r, w, h in VIDEO]
    video_preview = [{"ratio": r, "profileWidth": w, "profileHeight": h, "format": ["YUV_420_SP"]}
                     for r, w, h in VIDEO]
    return {"photoProfiles": photo, "photoPreviewProfiles": preview, "videoProfiles": video,
            "videoPreviewProfiles": video_preview}


def main(src, out):
    d = json.load(open(src))
    for cam in d["cameraCapability"]:
        position = cam["cameraType"]
        for state in cam["cameraTypeData"]:
            state["foldStateData"] = []  # no mode-specific profiles: every mode we keep uses the defaults
            state["defaultData"] = profiles(position)
    for cam in d["zoomParameter"]:
        steps = ZOOM.get(cam["cameraType"], ZOOM[2])
        cam["cameraTypeData"] = []
        cam["defaultData"] = [{"modeName": m, "quickZoomArray": steps["quick"], "opticalZoomArray": steps["optical"]}
                              for m in ("PHOTO", "VIDEO")]
    d["supportedMode"] = {"modeBarList": ["PHOTO", "VIDEO"], "moreModeList": []}
    for k, v in d["metadata"].items():
        if k.startswith(("isSupport", "isHeif")) and v is True:
            d["metadata"][k] = False
    json.dump(d, open(out, "w"), indent=1)


if __name__ == "__main__":
    main(*sys.argv[1:3])
