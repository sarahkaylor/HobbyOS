# Swarm Fleet — live inventory (Proxmox c2 / 192.168.0.19)

Created 2026-10-16/17 for the HobbyOS Gaming Program (`gaming.md`). Disks on `c2flash`
(NAS 192.168.0.56), raw, thin on the share. **No VM has a physical GPU allocated.**
All guests: Ubuntu 26.04.1 (resolute) — dev VMs run the Kubuntu desktop (installed via
`kubuntu-desktop`), matching the pre-existing `kubuntu-dev` VM 125.

| VMID | Name | IP | CPU | RAM | Disk | Role |
|---|---|---|---|---|---|---|
| 140 | swarm-farm-1 | 192.168.0.230 | 32 vCPU | 64 GiB | 128 G | dev/test farm, swarm worker host |
| 141 | swarm-farm-2 | 192.168.0.231 | 32 vCPU | 64 GiB | 128 G | dev/test farm, swarm worker host |
| 142 | swarm-farm-3 | 192.168.0.232 | 32 vCPU | 64 GiB | 128 G | dev/test farm, swarm worker host |
| 143 | swarm-coordinator | 192.168.0.233 | 32 vCPU | 64 GiB | 128 G | swarm coordinator (Hermes, planner, cron) |
| 144 | swarm-ticketing | 192.168.0.234 | 8 vCPU | 16 GiB | 64 G | Forgejo issue tracker |
| 145 | swarm-dashboard | 192.168.0.235 | 4 vCPU | 8 GiB | 64 G | Swarm dashboard (dgxmon-mirror stack) — see `swarm-dashboard.md` |

Access: `sarah@192.168.0.23x` — passwordless sudo; SSH pubkeys: sarah's `sarahpc-dgx-admin`
key + the agent's `id_proxmox` key (both seeded via cloud-init). VM SSH keys + bootstrap
passwords are in `~/.hermes/cache/scratch/swarm-vm-bootstrap-pw.txt` (chmod 600). Sarah's
GitHub SSH key was copied from VM 125 so `git@github.com:sarahkaylor/HobbyOS.git` works.

## Installed on every dev VM (140-143)

- Toolchain: clang/lld/llvm, build-essential, qemu-system-arm, qemu-system-x86,
  qemu-efi-aarch64, ovmf, dosfstools, mtools, python3, tmux, rsync, ccache, sccache, ninja, cmake.
- EDK2 firmware symlinks at `~/.local/share/OVMF/OVMF_CODE_4M.fd` and
  `~/.local/share/AAVMF/AAVMF_CODE.fd` (what the Makefile's Linux branch expects).
- `qemu-guest-agent` enabled (Proxmox-side guest control works).
- Hermes: installed for user `sarah` (`~/.hermes`).
- Repos: `~/Documents/GitHub/HobbyOS` cloned (HTTPS, with GitHub key installed), including the
  WebKit port scaffolding (`third_party/webkit-hobbyos`, `browser*` build scripts).

## Verified

- `make host_tests` → `ALL APPS SUITE TESTS PASSED` (farm-1, coordinator).
- Cross kernel build: `make MODE=unit_tests hobbyos.elf` → 2.1 MB `aarch64` ELF linked by
  `ld.lld` (farm-2).
- QEMU+UEFI: `qemu-system-aarch64 -bios AAVMF_CODE.fd` boots firmwared to the UEFI prompt,
  no crash (farm-2).
- LLVM 21.1.8 dist-staging built & cached on **farm-1**
  (`third_party/llvm-21.1.8/dist-staging/usr/local/bin/clang-21`).

## Ticketing VM 144

- Forgejo **v17.0.0** (gitea-1.22) at http://192.168.0.234:3000 — systemd service, sqlite.
- Admin user `sarah` (admin), `swarm-bot` API token with `all` scopes.
- Repo `sarah/gaming-program` created (auto-init, `main`). Board/project scaffolding,
  labels, milestones = gaming.md tickets GB-002/GB-003 (swarm step).

## Notes / caveats

- Host c2 is heavily CPU-oversubscribed (~500 configured vCPU on 128 physical). Fine for
  bursty build/test work; expect contention with the other running VMs (vllm, head-node, …).
- `local-lvm` thin pool shows the pre-existing "sum exceeds pool" warning (cloud-init CDs land
  there, ~4 MB each) — not caused by this fleet; disks themselves are on c2flash.
- First `make run`/`make test` on the other VMs will trigger the one-time staging build
  (`tools-staged` = LLVM dist + browser/WebKit) — cache the resulting
  `third_party/llvm-21.1.8/dist-staging` (e.g. rsync from farm-1) to skip it.
## RUNNING STATE (initiated 2026-10-17 ~05:00 UTC)

- **One model, one endpoint (MODEL POLICY 2026-10-31):** **every** agent — workers, coordinator, verifiers, reviewer — runs **DeepSeek V4.1 Flash** at `http://llm.localdomain:8000/v1` (provider `llm`). There is no model split. `deepseek-v4.1-flash` is canonical and `deepseek-v4-flash` is a legacy alias for **the same weights** — never reason about the alias as a weaker model. **`dgx1.localdomain` is retired for swarm work:** no lane calls it, no config carries a `providers.dgx` block, no charter or prompt offers it as a fallback, and `--provider dgx` must never be proposed. `agent.reasoning_effort: high` is still required on v4.1 (it rejects other values). `deep` is a **work classification** (planning / design / root-cause / gate verdicts), *not* a model tier — a `deep` ticket runs on exactly the same v4.1 Flash endpoint as every other ticket.
- **Verification lane (added 2026-10-20, gaming.md §3.1/§4.2/§5.1):** every farm runs a second driver loop `swarm-verify@<agent>.service` (e.g. `wabi` → `swarm-verify@wabi`, `AGENT=%i-verify` for its own flock) that claims `verify`+`deep`-labeled tickets. **It runs on the same `llm` endpoint and the same v4.1 Flash weights** — `deep` here selects *what the session works on*, not which model answers. The `verify` Forgejo label (id 29) exists; the re-plan mints `GB-nnn-V` tickets for every un-gated In Review item.
- **Keep-alive is headless drivers (not tmux):** `swarm-driver@<agent>.service` on each agent VM (`.230` `wabi`, `.231` `wgfx`, `.232` `wgames`, `.233` `coord`) runs `hermes chat --resume <sid> --query-file /opt/swarm-charters/<a>.continue.txt --oneshot --worktree` when the session is idle >10 min, gated by a ~25 min cooldown against no-op spin (first-ever start uses `--query-file /opt/swarm-charters/<a>.txt`). Session id + last-launch in `~/.swarm/<a>.json`; drivers are `enabled` with `Restart=always`, so a VM reboot resumes the agent automatically (~first resume within ~25 min unless the cooldown is stale'd). Manual restart: `systemctl restart swarm-driver@<a>`. The tmux + `swarm-nudge@` poking approach is deprecated.
- **2026-10-18:** rebooted the 4 agent VMs (farm-1..3 + coord) and added **sarah to the `kvm` group** on each (intel-QEMU accel — the item the replan had been flagging for a human). Verified post-boot: `/dev/kvm` RW-accessible from `sarah`, drivers auto-restarted and resumed sessions, board + both model endpoints reachable.
- **Re-plan cron** on `.233`: daily 03:00 → one-shot coordinator re-plan per gaming.md §4.3 (log `~/replan.log`).
- **Board:** 18 labels + GAMING-BOARD scaffolding live; coordinator imports GB-001..055 from gaming.md §8 (Forgejo Projects API 404s on v17 — before/kanban is milestone+label driven).
- **Dashboard live view reads `~/.hermes/state.db`** (SQLite, via exporter) — real streaming transcripts/models/tokens, not just completed sessions.
- **GitHub auth on agent VMs:** origin rewritten to `git@github.com:sarahkaylor/HobbyOS.git`; global
  `url."git@github.com:".insteadOf https://github.com/` set — **pushes** work over the seeded SSH key
  (verified `Hi sarahkaylor!`). **PR creation** uses the GitHub REST API via `GITHUB_TOKEN`, stored in
  `~/.hermes/.env` on each VM (chmod 600, value never printed/logged). Sessions that started before
  the token was added must source it in-shell first:
  `set -a; source ~/.hermes/.env; set +a`
  `GIT_TERMINAL_PROMPT=0` is also in `.env` so git never hangs on a credential prompt.
  First fruit: PR #1 (DOOM native port) opened 2026-10-17 by games-worker.

## Remaining swarm config (per gaming.md): Forgejo webhook → coordinator
  (board automation), re-plan cadence tuning (GB-003/004/048), kanban project via UI/other API.
