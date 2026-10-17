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
- Remaining swarm config (per gaming.md): Hermes endpoints → FLASH workhorse / DEEP planner,
  tmux workers + worktrees, Forgejo webhook → coordinator, re-plan cadence (GB-003/004/048).
