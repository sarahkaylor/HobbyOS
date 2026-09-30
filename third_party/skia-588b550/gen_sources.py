#!/usr/bin/env python3
"""gen_sources.py -- derive the CPU-raster source lists from WebKit's own
Source/ThirdParty/skia/CMakeLists.txt (vendored verbatim in webkit/), so the
recipe cannot silently drift from the WPE 2.54.0 file.

Model
-----
* The `add_library(Skia STATIC ...)` block is the always-built set ("MAIN").
* Every `target_sources(Skia PRIVATE ...)` block is tagged with the
  condition path it sits under (if/else tracked as a stack, e.g.
  "ELSE(WIN32) > ELSE(ANDROID)" = the non-Android POSIX+fontconfig block).
* Selection for this host spike (CPU raster, no GL/EGL):
    - MAIN minus src/gpu/** (GPU/Ganesh/Graphite + GL/EGL interfaces)
    - ELSE(WIN32)                    -> SKIA_POSIX_SOURCES      (kept)
    - ELSE(WIN32) > ELSE(ANDROID)    -> SKIA_FONTCONFIG_SOURCES (kept when
                                                                SKIA_FONTMGR=fontconfig)
    - USE_SKIA_ENCODERS              -> SKIA_ENC_SOURCES        (kept when SKIA_ENCODERS=ON)
    - ELSE(USE_SKIA_ENCODERS)        -> SKIA_ENC_STUB_SOURCES   (fallback)
    - USE_SKIA_OPENTYPE_SVG          -> SKIA_SVG_SOURCES        (kept when SKIA_OPENTYPE_SVG=ON)
    - USE_LIBEPOXY / ELSE(...)       -> SKIA_EGL_SOURCES        (audit only; NOT built)
    - WIN32 / ANDROID                -> audit only; NOT built
* Every referenced file must exist in --source-root (hard gate).
* The expected group set is asserted: if the WebKit file changes shape the
  script fails loudly instead of silently building something different.

Outputs (into --out-dir):
  sources.cmake          CMake list variables consumed by the root CMakeLists.txt
  sources-report.json    counts + sha256 of the WebKit file + missing-file check
  excluded-gpu.txt       the src/gpu/** entries removed from MAIN (audit)

Usage:
  gen_sources.py --cmake <webkit/CMakeLists.txt> --source-root <src/> --out-dir <dir>
"""
import argparse
import hashlib
import json
import os
import re
import sys

EXPECTED_GROUPS = {
    "MAIN",
    "WIN32",
    "ELSE(WIN32)",
    "ELSE(WIN32) > ANDROID",
    "ELSE(WIN32) > ELSE(ANDROID)",
    "USE_LIBEPOXY",
    "ELSE(USE_LIBEPOXY)",
    "USE_SKIA_ENCODERS",
    "ELSE(USE_SKIA_ENCODERS)",
    "USE_SKIA_OPENTYPE_SVG",
}

# Emitted under SKIA_FONTMGR=empty -- NOT from the WebKit CMakeLists (WebKit
# always uses fontconfig).  File list from upstream Skia's gn/ports.gni:
# skia_ports_fontmgr_custom_sources (:52-54) + skia_ports_fontmgr_empty_sources (:96).
CUSTOM_FONTMGR_SOURCES = [
    "src/ports/SkFontMgr_custom.cpp",
    "src/ports/SkFontMgr_custom_empty.cpp",
]

# Recipe delta D7 -- files upstream Skia's own GN build compiles but WebKit
# 2.54.0's CMakeLists.txt omits, which a plain static link cannot do without:
#   * src/core/SkStrikeRef.cpp (upstream gn/core.gni:572) -- SkFont.cpp:240
#     (SkFont::makeStrikeRef) references SkStrikeRef's ctor; without this TU
#     the smoke executable fails to link ("undefined reference to
#     SkStrikeRef::SkStrikeRef").  WebKit's final link may hide the gap via
#     dead-stripping; our recipe must not depend on that.  WK-3: candidate
#     upstream CMake fix (README.WebKit itself warns to re-check the CMake
#     file on Skia bumps).
EXTRA_MAIN_SOURCES = [
    "src/core/SkStrikeRef.cpp",
]


def parse_groups(cmake_path):
    lines = open(cmake_path, encoding="utf-8").read().splitlines()
    groups = {}
    stack = []  # [cond, in_else]

    def group_key():
        if not stack:
            return "MAIN"
        return " > ".join(("ELSE(%s)" % c) if e else c for c, e in stack)

    def read_block(start_idx, first_rest):
        """Return (entries, next_index) for an add_library/target_sources body."""
        entries = []
        rest = first_rest
        j = start_idx
        while True:
            t = rest.strip()
            if t.startswith(")"):
                t2 = t[1:].strip()
                if t[1:].strip():
                    raise SystemExit(f"unexpected tokens after ')' on line {j+1}: {rest!r}")
                break
            if t.endswith(")"):
                tok = t[:-1].strip()
                if tok:
                    entries.append(tok)
                break
            if t and not t.startswith("#"):
                entries.append(t)
            j += 1
            if j >= len(lines):
                raise SystemExit(f"unterminated block starting at line {start_idx+1}")
            rest = lines[j]
        return entries, j + 1

    i = 0
    while i < len(lines):
        s = lines[i].strip()
        if s.startswith("if (") or s.startswith("if("):
            cond = s[3:].strip()
            if cond.startswith("("):
                cond = cond[1:].strip()
            if cond.endswith(")"):
                cond = cond[:-1].strip()
            stack.append([cond, False])
            i += 1
            continue
        if s == "else" or s.startswith("else ") or s.startswith("else("):
            if not stack:
                raise SystemExit(f"stray else at line {i+1}")
            stack[-1][1] = True
            i += 1
            continue
        if s.startswith("endif"):
            if not stack:
                raise SystemExit(f"stray endif at line {i+1}")
            stack.pop()
            i += 1
            continue
        m = re.match(r"(add_library\(Skia STATIC|target_sources\(Skia PRIVATE)(.*)$", s)
        if m:
            entries, nxt = read_block(i, m.group(2))
            entries = [e for e in entries if e and e != ")"]
            groups.setdefault(group_key(), []).extend(entries)
            i = nxt
            continue
        i += 1
    return groups


def emit_list(fh, name, entries, source_root, comment=None):
    fh.write(f"\n# {comment}\n" if comment else "\n")
    fh.write(f"set({name}\n")
    for e in entries:
        fh.write(f'  "${{SKIA_SOURCE_ROOT}}/{e}"\n')
    fh.write(")\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmake", required=True)
    ap.add_argument("--source-root", required=True)
    ap.add_argument("--out-dir", required=True)
    args = ap.parse_args()

    groups = parse_groups(args.cmake)
    missing_keys = EXPECTED_GROUPS - set(groups)
    extra_keys = set(groups) - EXPECTED_GROUPS
    if missing_keys or extra_keys:
        raise SystemExit(
            "WebKit CMakeLists shape changed -- refusing to guess.\n"
            f"  groups missing: {sorted(missing_keys)}\n"
            f"  groups new:     {sorted(extra_keys)}\n"
            f"  all groups seen: {sorted(groups)}"
        )

    main_all = groups["MAIN"]
    main_cpu = [e for e in main_all if not e.startswith("src/gpu/")] + EXTRA_MAIN_SOURCES
    excluded_gpu = [e for e in main_all if e.startswith("src/gpu/")]

    selection = {
        "SKIA_MAIN_SOURCES_CPU": (main_cpu,
            "MAIN add_library list minus src/gpu/** (CPU raster; SK_GL/SK_GANESH not "
            "defined) plus the D7 extras WebKit's list omits (see gen_sources.py)"),
        "SKIA_POSIX_SOURCES": (groups["ELSE(WIN32)"],
            "non-Windows ports: FreeType font host + posix OSFile + WebKit's typeface proxy (kept)"),
        "SKIA_FONTCONFIG_SOURCES": (groups["ELSE(WIN32) > ELSE(ANDROID)"],
            "non-Android POSIX font manager (fontconfig; kept when SKIA_FONTMGR=fontconfig)"),
        "SKIA_ENC_SOURCES": (groups["USE_SKIA_ENCODERS"],
            "image encoders (USE_SKIA_ENCODERS=ON; needs WebP::mux)"),
        "SKIA_ENC_STUB_SOURCES": (groups["ELSE(USE_SKIA_ENCODERS)"],
            "encoder stubs (USE_SKIA_ENCODERS=OFF)"),
        "SKIA_SVG_SOURCES": (groups["USE_SKIA_OPENTYPE_SVG"],
            "SVG module + shaper + XML (USE_SKIA_OPENTYPE_SVG=ON; needs EXPAT + HarfBuzz)"),
        "SKIA_EGL_SOURCES": (groups["USE_LIBEPOXY"] + groups["ELSE(USE_LIBEPOXY)"],
            "GL/EGL interface files -- AUDIT ONLY, never built by this recipe (CPU raster)"),
        "SKIA_WIN32_SOURCES": (groups["WIN32"],
            "Windows ports -- AUDIT ONLY"),
    }

    # hard gate: every referenced file must exist in the fetched tree
    missing = []
    for name, (entries, _) in selection.items():
        for e in entries:
            if not os.path.exists(os.path.join(args.source_root, e)):
                missing.append(e)
    custom_ok = all(os.path.exists(os.path.join(args.source_root, e)) for e in CUSTOM_FONTMGR_SOURCES)

    cmake_sha = hashlib.sha256(open(args.cmake, "rb").read()).hexdigest()
    os.makedirs(args.out_dir, exist_ok=True)

    report = {
        "webkit_cmakelists_sha256": cmake_sha,
        "groups": {k: len(v) for k, v in sorted(groups.items())},
        "emitted": {k: len(v[0]) for k, v in sorted(selection.items())},
        "total_main": len(main_all),
        "total_main_cpu": len(main_cpu),
        "excluded_gpu": len(excluded_gpu),
        "recipe_extras_D7": EXTRA_MAIN_SOURCES,
        "custom_fontmgr_alternate": CUSTOM_FONTMGR_SOURCES,
        "custom_fontmgr_alternate_files_present": custom_ok,
        "missing_files": missing,
    }
    with open(os.path.join(args.out_dir, "sources-report.json"), "w") as fh:
        json.dump(report, fh, indent=2, sort_keys=True)
        fh.write("\n")

    with open(os.path.join(args.out_dir, "excluded-gpu.txt"), "w") as fh:
        fh.write("# src/gpu/** entries removed from the MAIN list (CPU-raster build)\n")
        for e in excluded_gpu:
            fh.write(e + "\n")

    with open(os.path.join(args.out_dir, "sources.cmake"), "w") as fh:
        fh.write("# GENERATED by gen_sources.py from webkit/CMakeLists.txt\n")
        fh.write(f"#   source file sha256: {cmake_sha}\n")
        fh.write("# Do not edit: regenerate via build.sh (the generator asserts the\n"
                 "# WebKit file's shape and fails on drift).\n")
        for name, (entries, comment) in selection.items():
            emit_list(fh, name, entries, args.source_root, comment)
        emit_list(fh, "SKIA_CUSTOM_FONTMGR_SOURCES", CUSTOM_FONTMGR_SOURCES, args.source_root,
                  "alternate font manager for SKIA_FONTMGR=empty (from Skia gn/ports.gni, "
                  "not WebKit's file); HobbyOS target leg uses this shape")

    if missing:
        raise SystemExit("missing source files in the fetched tree:\n  " + "\n  ".join(missing))
    if not custom_ok:
        print("WARNING: custom-fontmgr alternate files not present", file=sys.stderr)

    print(f"gen_sources: MAIN {len(main_all)} -> {len(main_cpu)} CPU "
          f"(-{len(excluded_gpu)} src/gpu); posix={len(selection['SKIA_POSIX_SOURCES'][0])}, "
          f"fontconfig={len(selection['SKIA_FONTCONFIG_SOURCES'][0])}, "
          f"enc={len(selection['SKIA_ENC_SOURCES'][0])}, svg={len(selection['SKIA_SVG_SOURCES'][0])}; "
          f"all files present; report -> {args.out_dir}/sources-report.json")


if __name__ == "__main__":
    main()
