#!/usr/bin/env python3
"""tools/board/import_seed.py — import/refresh the gaming.md §8 seed backlog as issues.

Parses `gaming.md` §8 (GB-001…GB-055) and UPSERTS each ticket into Forgejo
`sarah/gaming-program`: title / milestone / body (DoD + test + evidence slot) /
labels (`deep`, workstream, weight `wt-S|M|L`).  Existing demo issues are
updated **in place** (matched by their `GB-nnn` title prefix), never duplicated.

Weight, workstream and model-tier metadata that gaming.md keeps only in prose is
carried in WORKSTREAM / TIER below (single source of truth for the importer).

  import_seed.py [--dry-run] [--only GB-013,GB-014] [--no-title] [--quiet]
"""
import argparse
import re
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import board  # noqa: E402

PLAN = "gaming.md"

# ---- metadata not carried by the §8 table (workstream labels per §4.1) ----
WORKSTREAM = {
    "GB-001": ["infra"], "GB-002": ["infra"], "GB-003": ["infra"], "GB-004": ["infra"],
    "GB-005": ["infra", "tests"], "GB-006": ["infra", "perf"], "GB-007": ["tests", "elf-compat"],
    "GB-008": ["docs", "infra"], "GB-009": ["tests"], "GB-010": ["abi", "docs"],
    "GB-011": ["docs", "game"], "GB-012": ["abi", "tests"],
    "GB-013": ["loader", "elf-compat"], "GB-014": ["abi", "elf-compat"], "GB-015": ["abi", "elf-compat", "tests"],
    "GB-016": ["abi", "elf-compat"], "GB-017": ["abi", "elf-compat"], "GB-018": ["abi", "elf-compat"],
    "GB-019": ["abi", "elf-compat"], "GB-020": ["abi", "elf-compat"], "GB-021": ["abi", "elf-compat"],
    "GB-022": ["abi", "elf-compat"], "GB-023": ["abi", "elf-compat"], "GB-024": ["tests", "elf-compat"],
    "GB-025": ["graphics", "elf-compat"], "GB-026": ["graphics", "elf-compat"], "GB-027": ["input", "elf-compat"],
    "GB-028": ["abi", "elf-compat"], "GB-029": ["audio"], "GB-030": ["abi", "elf-compat"],
    "GB-031": ["graphics", "elf-compat", "libc"], "GB-032": ["graphics", "elf-compat", "tests"],
    "GB-033": ["graphics", "elf-compat", "tests"],
    "GB-034": ["native-port", "game"], "GB-035": ["native-port", "game"], "GB-036": ["native-port", "game"],
    "GB-037": ["native-port", "game"], "GB-038": ["native-port", "game"], "GB-039": ["native-port", "game"],
    "GB-040": ["tests"], "GB-041": ["elf-compat", "game"], "GB-042": ["input", "game"],
    "GB-043": ["tests"], "GB-044": ["tests", "infra"], "GB-045": ["perf"], "GB-046": ["perf", "tests"],
    "GB-047": ["docs", "tests"], "GB-048": ["infra"], "GB-049": ["infra"], "GB-050": ["tests"],
    "GB-051": ["audio", "tests"], "GB-052": ["docs"], "GB-053": ["tests"], "GB-054": ["docs"], "GB-055": ["docs"],
    "GB-062": ["tests"],
}
DEEP_IDS = {"GB-010", "GB-011", "GB-013", "GB-031", "GB-041", "GB-054"}

MS_KEY = {"M0": "M0-infra", "M1": "M1-abi-core", "M2": "M2-devices-sdl", "M3": "M3-games", "M4": "M4-acceptance"}
MS_TIERS = {
    "M0-infra": "T1 host golden + T2 kernel unit + T3 integration (both arches, §2.3)",
    "M1-abi-core": "T4 differential (Linux box vs HobbyOS-intel) on the W0 corpus",
    "M2-devices-sdl": "T4 differential + T3 integration",
    "M3-games": "T1 host golden + T3 integration + T5 playthrough (QMP input + screendump assert)",
    "M4-acceptance": "T5 playthrough (long duration) + full gate T1-T3 both arches",
}
LANE_DEFAULT = "Backlog"


def parse_plan(path=PLAN):
    """-> {GID: {title, weight, depends, dod, ms}} in plan order."""
    out, ms, order = {}, None, []
    for line in open(path, encoding="utf-8"):
        m = re.match(r"^### Milestone (M\d)\b", line)
        if m:
            ms = MS_KEY[m.group(1)]
            continue
        if line.startswith("#"):
            ms = None          # any non-milestone heading ends the §8 milestone section
            continue
        if not ms:
            continue
        c = [x.strip() for x in line.strip().strip("|").split("|")] if line.startswith("|") else None
        if not c or len(c) < 5 or not re.match(r"^GB-\d{3}$", c[0]):
            continue
        gid = c[0]
        if gid in out:
            continue
        title = re.sub(r"\*\*|`", "", c[1]).replace("  ", " ").strip()
        out[gid] = {"title": title, "weight": c[2], "depends": c[3], "dod": c[4], "ms": ms}
        order.append(gid)
    return out, order


def build_body(gid, t, lane=None):
    ws = WORKSTREAM.get(gid, [])
    deep = gid in DEEP_IDS
    tiers = MS_TIERS[t["ms"]]
    found = sorted(set(re.findall(r"\bT[1-5]\b", t["dod"])))
    tier_line = f"**{'/'.join(found)}** (as named in the DoD)" if found else f"**{tiers}**"
    return f"""<!-- seeded from gaming.md §8 by tools/board/import_seed.py — edit the plan, then re-run; do not hand-edit this header block -->

| field | value |
|---|---|
| ID | {gid} |
| Workstream | {', '.join(ws) or '—'} |
| Weight | {t['weight']} |
| Milestone | {t['ms']} |
| Depends on | {t['depends']} |
| Model tier | {'DEEP (design/review) + FLASH (exec)' if deep else 'FLASH (workhorse)'} |
| Lane | see kanban lane label (Forgejo 17 has no Projects REST → lanes are labels; GB-002) |

### Definition of Done
{t['dod']}

### Test
Required tier(s): {tier_line}
Per gaming.md §6 — derived from the milestone; confirm/extend at claim time. The DoD cell above is the
table text verbatim (it names the test, where the plan spells one out).

### Evidence
_pending_ — attach the command + its real output (serial log / screendump / gate run / diff) as a
comment **before** moving past Merge Gate; evidence-before-close is law (§4.2, §5.3).

### References
gaming.md §8 (seed backlog) · §7 (milestone gate) · §6 (test tiers) · §2 (architecture)
"""


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--only", help="comma-separated GB ids")
    p.add_argument("--no-title", action="store_true", help="don't touch existing titles")
    p.add_argument("--quiet", action="store_true")
    a = p.parse_args()

    tickets, order = parse_plan()
    only = set(x.strip().upper() for x in a.only.split(",")) if a.only else None

    # lanes + weight labels
    if not a.dry_run:
        board.ensure_lanes(create=True)
        for w, col in board.WEIGHT_LABELS.items():
            board.ensure_label(f"wt-{w}", col, f"weight {w} (§4.1 ticket skeleton)")

    ms_ids = {}
    if not a.dry_run:
        for key, title in MS_KEY.items():
            mid, mt = board.milestone_id(title)
            ms_ids[title] = mid

    existing = {}
    if not a.dry_run:
        for it in board.all_issues("all"):
            m = re.match(r"^(GB-\d{3})\b", it["title"])
            if m:
                existing[m.group(1)] = it

    created = updated = 0
    for gid in order:
        if only and gid not in only:
            continue
        t = tickets[gid]
        title = f"{gid} {t['title']}"
        body = build_body(gid, t)
        ws = WORKSTREAM.get(gid, [])
        labs = ws + ([f"wt-{t['weight']}"] if t["weight"] in "SML" else [])
        if gid in DEEP_IDS:
            labs.append("deep")
        cur = existing.get(gid)
        if cur:
            labs.append(board.lane_of(cur) or LANE_DEFAULT)
        else:
            labs.append(LANE_DEFAULT)
        if a.dry_run:
            print(f"[dry] {gid} ms={t['ms']} wt={t['weight']} labels={labs}\n      {title}")
            continue
        if cur:
            patch = {"body": body, "milestone": ms_ids[t["ms"]]}
            if not a.no_title:
                patch["title"] = title
            board.api_patch(f"/repos/{board.REPO}/issues/{cur['number']}", patch)
            board.api_put_labels(cur["number"], sorted(set(labs)))
            updated += 1
            if not a.quiet:
                print(f"updated {gid} #{cur['number']}")
        else:
            it = board.api_post(f"/repos/{board.REPO}/issues", {
                "title": title, "body": body, "milestone": ms_ids[t["ms"]],
                "labels": board.label_ids(sorted(set(labs))),
            })
            created += 1
            if not a.quiet:
                print(f"created {gid} #{it['number']}")
    if not a.dry_run:
        print(f"\nseed import: {created} created, {updated} updated ({len(order)} in plan, {len(tickets)} parsed)")


if __name__ == "__main__":
    main()
