#!/usr/bin/env python3
"""cp/p0guest (runs IN the guest): set the P0 measurement settings in the
GUEST prefix's UserSettings.json (never the user's prefix).

    guest-cp-settings.py 1280x720|1920x1080 [--show]

Sets only the resolution; prints the settings that define the P0 rows
(preset, scaling, RT/PT, VSync, frame cap, window mode) for the record. The
Low preset itself was picked on the game's Graphics page (docs/cp-p0-guest.md
§5.6), which is also where each run is verified by screenshot.
"""
import json
import os
import sys

F = os.path.expanduser("~/compat/1091500/pfx/drive_c/users/steamuser/AppData/Local/"
                       "CD Projekt Red/Cyberpunk 2077/UserSettings.json")
WANT = {
    # The preset itself is chosen in the game's own Graphics page (Quick
    # Preset "Low"), which the game writes here; this script only switches the
    # resolution and checks the rest.
    ("/video/display", "Resolution"): None,
}
CHECK = [("/graphics/presets", "QuickPresets"), ("/graphics/presets", "ResolutionScaling"),
         ("/graphics/presets", "FSR2"), ("/graphics/presets", "DynamicResolutionScaling"),
         ("/graphics/presets", "TextureQuality"), ("/graphics/raytracing", "RayTracing"),
         ("/graphics/raytracing", "RayTracedPathTracing"), ("/video/display", "VSync"),
         ("/video/display", "MaximumFPS_OnOff"), ("/video/display", "WindowMode"),
         ("/video/display", "Resolution")]


def main():
    res = sys.argv[1]
    d = json.load(open(F))
    for g in d["data"]:
        for o in g["options"]:
            k = (g["group_name"], o["name"])
            if k in WANT and "--show" not in sys.argv:
                o["value"] = res
    for g in d["data"]:
        for o in g["options"]:
            k = (g["group_name"], o["name"])
            if k in CHECK:
                print(f"{k[0]} {k[1]} = {o['value']!r}")
    if "--show" not in sys.argv:
        json.dump(d, open(F, "w"), indent=4)


if __name__ == "__main__":
    main()
