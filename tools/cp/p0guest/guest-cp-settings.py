#!/usr/bin/env python3
"""cp/p0guest (runs IN the guest): set the P0 measurement settings in the
GUEST prefix's UserSettings.json (never the user's prefix).

    guest-cp-settings.py 1280x720|1920x1080 [--show]

Applies: QuickPresets Low, ray tracing and path tracing off, dynamic
resolution and frame generation off, VSync off, frame cap off, fullscreen at
the given resolution. Option names are the game's own (UserSettings.json
version 152 on the installed 2.x build).

STATUS: UNVERIFIED. The game only writes this file after the user agreement
has been accepted, which this lane may not do (docs/cp-p0-guest.md §6).
Whether the game re-derives the individual "Low" values from QuickPresets at
load, or only when the preset is picked in the menu, is not known; the
follow-up run must confirm the effective settings on the in-game Graphics
page (screenshot + OCR) before trusting an fps row.
"""
import json
import os
import sys

F = os.path.expanduser("~/compat/1091500/pfx/drive_c/users/steamuser/AppData/Local/"
                       "CD Projekt Red/Cyberpunk 2077/UserSettings.json")
WANT = {
    ("/graphics/presets", "QuickPresets"): "Low",
    ("/graphics/presets", "DynamicResolutionScaling"): False,
    ("/graphics/presets", "FSR3_FrameGeneration"): False,
    ("/graphics/presets", "FSR4_FrameGeneration"): False,
    ("/graphics/presets", "XESS_FrameGeneration"): False,
    ("/graphics/presets", "DLSSFrameGen"): False,
    ("/graphics/raytracing", "RayTracing"): False,
    ("/graphics/raytracing", "RayTracedReflections"): False,
    ("/graphics/raytracing", "RayTracedSunShadows"): False,
    ("/graphics/raytracing", "RayTracedLocalShadows"): False,
    ("/graphics/raytracing", "RayTracedPathTracing"): False,
    ("/video/display", "VSync"): "UI-Settings-Video-QualitySetting-Off",
    ("/video/display", "MaximumFPS_OnOff"): False,
    ("/video/display", "WindowMode"): "Fullscreen",
}


def main():
    res = sys.argv[1]
    WANT[("/video/display", "Resolution")] = res
    d = json.load(open(F))
    seen = set()
    for g in d["data"]:
        for o in g["options"]:
            k = (g["group_name"], o["name"])
            if k in WANT:
                if "--show" not in sys.argv:
                    o["value"] = WANT[k]
                seen.add(k)
                print(f"{k[0]} {k[1]} = {o['value']!r}")
    for k in sorted(set(WANT) - seen):
        print(f"MISSING {k[0]} {k[1]} (not in the game-written file; left to the game)")
    if "--show" not in sys.argv:
        json.dump(d, open(F, "w"), indent=4)


if __name__ == "__main__":
    main()
