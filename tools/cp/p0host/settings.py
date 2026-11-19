#!/usr/bin/env python3
"""tools/cp/p0host/settings.py — edit Cyberpunk's UserSettings.json in OUR prefix copy.

usage: settings.py <UserSettings.json> show [substr...]
       settings.py <UserSettings.json> set group/Name=value ...      (value parsed as JSON if possible)
       settings.py <UserSettings.json> diff <other.json>

Refuses to touch anything under /mnt/fast (the user's prefix and the Steam library).
"""
import json, sys, os

def load(p):
    with open(p, encoding="utf-8") as f:
        return json.load(f)

def flat(d):
    out = {}
    for g in d["data"]:
        for o in g["options"]:
            out[g["group_name"] + "/" + o["name"]] = o
    return out

def main():
    path, cmd = sys.argv[1], sys.argv[2]
    d = load(path)
    if cmd == "show":
        subs = sys.argv[3:]
        for k, o in flat(d).items():
            if not subs or any(s.lower() in k.lower() for s in subs):
                print(k, "=", json.dumps(o.get("value")), "values=" + json.dumps(o["values"]) if "values" in o else "")
    elif cmd == "diff":
        a, b = flat(d), flat(load(sys.argv[3]))
        for k in sorted(set(a) | set(b)):
            va, vb = a.get(k, {}).get("value"), b.get(k, {}).get("value")
            if va != vb:
                print(f"{k}: {json.dumps(va)} -> {json.dumps(vb)}")
    elif cmd == "set":
        real = os.path.realpath(path)
        if real.startswith("/mnt/fast"):
            sys.exit("refusing to edit a file under /mnt/fast: " + real)
        f = flat(d)
        for a in sys.argv[3:]:
            k, v = a.split("=", 1)
            try:
                v = json.loads(v)
            except ValueError:
                pass
            if k not in f:
                sys.exit("no such option: " + k)
            o = f[k]
            o["value"] = v
            if "values" in o and v in o["values"]:
                o["index"] = o["values"].index(v)
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(d, fh, indent=2)
    else:
        sys.exit(__doc__)

if __name__ == "__main__":
    main()
