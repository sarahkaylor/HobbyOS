# HobbyOS nv swarm — VM 201 `nv-hermes`

Proxmox c4, VMID 201, `192.168.0.251`, Ubuntu 26.04, 48 vCPU / 192 GiB / 512 GiB,
`/dev/kvm` (nested on the host, `-cpu host` in the guest). Provisioned
2026-11-23 as the home of the Hermes swarm for `PLAN.md`.

## What runs here

| Piece | Where / how |
|---|---|
| Hermes Agent | `~/.hermes/hermes-agent`, launcher `~/.local/bin/hermes` (git install, Python 3.14.9) |
| Model | provider `llm` → `http://llm.localdomain:8000/v1`, `deepseek-v4.1-flash`, `agent.reasoning_effort: high` |
| Web dashboard | `http://192.168.0.251:9119` — systemd user-facing unit `hermes-dashboard.service` (system), bound `0.0.0.0` |
| Dashboard login | user `admin`; password and signing secret in `~/.hermes/.env` as `HERMES_DASHBOARD_BASIC_AUTH_{USERNAME,PASSWORD,SECRET}` (chmod 600) |
| Gateway + kanban dispatcher | `hermes-gateway.service` (systemd **user** unit, linger enabled) — one gateway per host serves every profile |
| Board | `hobbyos-nv` — `hermes kanban boards list`; DB at `~/.hermes/kanban/boards/hobbyos-nv/kanban.db` |
| Worker profiles | `nv-card`, `nv-shim`, `nv-plat`, `nv-verify` (cloned from `default`, same model) |
| Repo | `~/Documents/GitHub/HobbyOS` @ `nv/integ-swarm`, `third_party/` + `obj/` pre-warmed from the Mac build box |
| Cron | `nv nightly soak` — 03:15 daily, runs the three-times-tier soak and comments on the card |
| Skills | `~/.hermes/skills/hobbyos/*` copied from the Mac (nv-swarm, kernel-constraints, run-tests, build-and-run, proxmox-gpu, gui-test, screenshot) |

## Reading the dashboard password

```bash
ssh sarah@192.168.0.251 'grep ^HERMES_DASHBOARD_BASIC_AUTH_PASSWORD ~/.hermes/.env'
```

Do not paste it into tickets, commits, or chat. `HERMES_DASHBOARD_BASIC_AUTH_SECRET`
must stay stable — changing it ends every dashboard session.

## Build/test on this box

Builds happen **here**, not on the Mac and not by rsync to VM 200:

```bash
cd ~/Documents/GitHub/HobbyOS
make -j24 TOOLS_SKIP=1 BROWSER_SKIP=1 ARCH=intel MODE=unit_tests LO_ON_DISK= hobbyos.elf disk.img
TOOLS_SKIP=1 BROWSER_SKIP=1 ./run_unit_tests_intel.sh      # intel tier
TOOLS_SKIP=1 BROWSER_SKIP=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests.sh   # ARM tier (TCG)
```

In a fresh **worktree** (`hermes kanban` gives each worker one), `third_party/` is
not there — symlink the warm copy instead of fetching 3.6 GB again:

```bash
ln -s ~/Documents/GitHub/HobbyOS/third_party third_party
```

`TOOLS_SKIP=1 BROWSER_SKIP=1` are mandatory: the in-OS LLVM dist-staging build dies
with `undefined symbol: pthread_setname_np`, and the kernel tier needs neither it
nor the WebKit staging.

## Board mechanics that matter

- Cards live in `triage` until promoted; the dispatcher only spawns **ready** tasks.
  `hermes kanban promote <id>` moves one; `hermes kanban dispatch --dry-run` shows
  what would spawn without spawning.
- Assignee = profile name. A worker gets its own workspace (worktree) and the
  `kanban_*` toolset, reads the whole comment thread on spawn, and must call
  `kanban_complete` / `kanban_block`.
- `--goal` cards run a Ralph-style loop with a judge against the card body — write
  bodies as explicit acceptance criteria.
- Never `kanban link` a support card as a child of the card it exists to unblock:
  it gates behind its own parent. Reference the id in the body instead.

## Card lane map

| Assignee | Lane |
|---|---|
| `nv-card` | the one RTX 4090 (r1 VM 300): SEC2 unfence, booter, GSP-RM, BAR1 reads |
| `nv-shim` | `/dev/nvshim`: NVIF, PRIME, mmap semantics, DRM ABI |
| `nv-plat` | platform 3D: nvk build/enumeration, RimWorld audio, Cyberpunk recipe |
| `nv-verify` | merge gate, nightly soak, docs-in-step |

## What this VM does NOT own

- The 4090 and Proxmox r1 (`root@192.168.10.174`) — that is `nv-card`'s lane only,
  one deploy at a time, and the VM must be left stopped.
- The Mac control node (`sarah@192.168.0.123`'s build box) and VM 200 `nv-swarm`
  (`192.168.0.250`), which stay as the off-host build/test fallback.
