# tools/board — HobbyOS Gaming Program board automation (GB-002/GB-003)

The ticketing system is **Forgejo 17.0.0 (gitea-1.22) on `swarm-ticketing`
(192.168.0.234:3000)**, repo **`sarah/gaming-program`** — gaming.md §4.
All agents talk to it through `board.py`; never ad-hoc curl (§4.1, §11.2).

## The board (lanes) — GB-002 limitation

§4.1 asks for a kanban project `GAMING-BOARD` with lanes
`Backlog → Ready → In Progress → In Review → Merge Gate → Done` + `Icebox`.

**This Forgejo build has no Projects REST API**: `/api/v1/repos/{owner}/{repo}/projects`
returns `404 page not found` (checked 2026-10-17, both repo-scoped and `/admin/projects`),
so a project cannot be created or driven from the API — which is how every agent works.

Fallback (documented, in force until a Forgejo version with `/projects` lands):

| §4.1 concept | implemented as |
|---|---|
| milestone | Forgejo **milestone** `M0-infra … M4-acceptance` (native) |
| lane | **label** with the lane's name and colour (§4.1 names kept verbatim) |
| ticket skeleton | **issue header block** (ID / workstream / weight / depends / tier) |
| claim | only from `Ready`: assignee + `In Progress` label + a claim comment (`board.py claim` refuses every other lane) |
| gate | `Merge Gate` / `In Review` labels + a mandatory `EVIDENCE` comment |

Exactly one lane label per issue; `board.py move` swaps it atomically.
The Forgejo web UI groups issues by label or milestone, which reproduces the
kanban view closely enough for the swarm.

## Scripts

```bash
export FORGEJO_TOKEN=$(cat ~/.hermes/cache/scratch/swarm-vm-bootstrap-pw.txt | grep swarm-bot | cut -d' ' -f4)
export FORGEJO_URL=http://192.168.0.234:3000        # default
export BOARD_REPO=sarah/gaming-program              # default

tools/board/claim.sh GB-025 swarm-worker-1          # -> In Progress + claim comment
tools/board/lane-move.sh GB-025 "In Review"         # swap lane (Done also closes the issue)
tools/board/comment.sh GB-025 -m "EVIDENCE: ./run_unit_tests_intel.sh -> 412/412 pass"
echo "long text" | tools/board/comment.sh GB-025

python3 tools/board/board.py status                 # lanes + per-milestone rollup
python3 tools/board/board.py list --lane Ready      # who can work right now
python3 tools/board/board.py show GB-013            # header, DoD, comments
python3 tools/board/board.py lint                   # lane hygiene / evidence gate
python3 tools/board/import_seed.py                  # (re)import gaming.md §8 -> board
python3 tools/board/sync_gaming_md.py               # GB-008 plan<->board drift lint
```

`import_seed.py` is idempotent: it matches existing issues by their `GB-nnn` title
prefix and updates them in place — re-run it after any §8 edit.

## Board → Coordinator

Two paths, one inbox (`$BOARD_EVENT_LOG`, default `~/.hermes/board-events.jsonl`):

1. **Webhook (primary)** — `hook_server.py` listens on `:8765/board-hook`; the
   Forgejo webhook `coordinator-board-hook` (repo hooks API) posts `issues` +
   `issue_comment` events. Installed as systemd unit
   `hobbyos-board-hook.service` on `swarm-coordinator` (.233).
2. **60 s poll (fallback)** — `poll.py` diffs lanes/comments/state and appends the
   same records; run it when the webhook is down or on a host Forgejo cannot reach.

Event records carry `wake: true` for the things that need the Coordinator's
attention: a lane move into `In Review`/`Merge Gate`, a `decision` label, or a close.
