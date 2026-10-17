# HobbyOS Gaming Program — Plan (living document)

Status: **IN EXECUTION (started 2026-10-17, Coordinator on `swarm-coordinator` .233).**
M0 board/fleet automation landed (GB-002/003/004); M1 ABI-core is the critical path.
Owner: Sarah (architect), assisted by the HobbyOS agent swarm.
Ticketing: **Forgejo v17 live at http://192.168.0.234:3000**, repo `sarah/gaming-program`;
`GAMING-BOARD` board scaffolding = tickets GB-002/GB-003.
Swarm: 5 VMs on Proxmox `c2` (192.168.0.19) — inventory in `swarm-fleet.md`, credentials in the
scratch file (see §11.1). This document is the single source of truth. The **Coordinator** keeps
it in sync with the board; agents never fork it. If a fact here drifts from the repo, update §3
first, then the board.

---

## 0. Executive summary

**Vision (success criteria):** launch real games "like Dyson Sphere Program, RimWorld, and
Total Annihilation" from HobbyOS, running under QEMU (no real-GPU path this program).

**Program spine (what makes that vision reachable):** give HobbyOS **Linux ELF binary load &
run capability** — a Linux x86_64 syscall-ABI compatibility layer plus an ELF loader — so
real Linux binaries can execute inside HobbyOS. Alongside it, a **native game catalog** (ports
on the HobbyOS ABI) guarantees playable results this month no matter how far the compat layer
gets.

**Honest scope contract (read §1 before anything else):** the three named games *as shipped*
cannot run in HobbyOS this month — and two of them need a Mono/CLR and a Vulkan driver,
respectively, which are not one-month builds for any swarm. The plan therefore stages them as
**vision-level epics on a long arc**, and defines this program's real acceptance criteria as a
**weight-class ladder** that makes measured progress toward exactly those games. By the end of
week 4 we expect: **a playable game delivered through each of the two tracks** (native port +
unmodified Linux static-ELF binary), plus a game launcher, plus full multiplan testing.

**Autonomy model:** one **Coordinator** agent (the only agent allowed to call the deep model)
runs the board, the plan, and a **hourly/daily re-plan loop** that invokes **DeepSeek v4.1
Flash** to re-assess and mint new tickets. Every other agent runs almost entirely on the **much
faster DeepSeek V4 Flash** workhorse.

---

## 1. Success criteria and the honest scope contract

### 1.1 What the three target games actually are

| Game | Engine / runtime | Ships as | What "run it" requires | Verdict for this program |
|---|---|---|---|---|
| **RimWorld** | C# on **Mono** (.NET), OpenTK/OpenGL | x86_64 Linux bundle w/ bundled Mono | A **CLR (Mono) port** + OpenGL + near-complete Linux userspace | **Not this month.** Track as Program-2 epic (`MONO`). |
| **Dyson Sphere Program** | **Unity** (IL2CPP) | x86_64 Linux ELF, Vulkan/GL | **Vulkan driver** + full glibc/dynamic-loader + huge proprietary binary | **Not this month.** Track as Program-2/3 epic (`VULKAN`). |
| **Total Annihilation** | Cavedog C++ (Windows PE) / **Spring RTS** (open-source engine) | Windows binary, or Spring (Linux ELF, SDL2+OpenGL2) | Spring-class C++/OpenGL2 engine under compat | **Not this month in full form.** Most tractable of the three; Program-2 epic (`RTS`). |

No swarm, no amount of parallelism, changes that these require a CLR, a Vulkan/driver stack, or
a Spring-class engine **plus** the full Linux userspace they drag in. Any proposal that says
"we'll run the DSP Linux binary this month" is not engineering. What we *can* do is build the
layer those games all need — **Linux ELF run capability** — and prove it on a ladder of
heavier and heavier real binaries.

### 1.2 The weight-class ladder (this is the real success metric)

Every step is a **ticket with a Definition of Done** and a differential test (same binary on a
real Linux box vs. HobbyOS):

| Class | Binary example | Enables | Target |
|---|---|---|---|
| W0 | static-musl **`-static-pie`** `hello`, `cat`, shell builtins | ABI skeleton proven | **Week 1 gate (M1)** |
| W1 | static-musl GNU coreutils / BusyBox | Real-world syscall coverage | Week 2 |
| W2 | static-musl **ncurses** app (`ninvaders`, a roguelike) | termios, ioctl, TTY | Week 2 |
| W3 | static-musl **SDL2** app (fbcon/fbdev path) | fbdev+evdev+threads+futex+TLS | **Week 3 gate (M2/M3)** |
| W4 | **static-musl Doom** (doomgeneric) — *playable* | Full 2D game under ELF-compat | **Week 4 (S1)** |
| W5 | dynamic glibc binaries / windowed SDL over X11-ish | glibc dynamic loader, X protocol | Program-2 |
| W6 | Mono (RimWorld) / Vulkan (DSP) / Spring RTS | CLR + driver stacks | Program-2/3 (epics) |

### 1.3 This program's acceptance criteria (S0/S1)

- **S0 — Native track (definite):** ≥5 open-source games launch and play from the HobbyOS
  desktop under QEMU on **both** `ARCH=arm` and `ARCH=intel` — including a full-screen 3D-ish
  title (DOOM-class raycaster) and a text roguelike — with persistent input, windowed/launcher
  integration, and a scripted **playthrough test** (QMP input + screendump assertions).
- **S1 — ELF-compat track (the moonshot):** HobbyOS boots **unmodified x86-64 Linux static-ELF
  binaries** through the compat layer, culminating in a **playable static-musl SDL game
  (class W4)** on `ARCH=intel`, differential-tested for syscall/errno parity.
- **S2 — Launcher + evidence:** a desktop **game launcher app** that starts both native `.BIN`
  games and `ELF-compat` games; per-game evidence (serial log, screenshots, replay script);
  RimWorld/DSP/TA feasibility re-derivation landed as tickets on the Program-2 arc.

---

## 2. Architecture

### 2.1 Two tracks, one OS

```
HobbyOS (QEMU: -M virt aarch64 / -M q35 x86_64)
├─ Native track (HobbyOS ABI)  → desktop, xlib, gfx libs, native .BIN games     [S0]
└─ ELF-compat track (Linux ABI, ARCH=intel)                                     [S1]
   ├─ ELF loader (ET_EXEC/ET_DYN-static) in the kernel loader path
   └─ Linux x86_64 syscall translation → HobbyOS kernel (Linuxulator-style)
        + compat device nodes: /dev/fb0 (framebuffer), /dev/input/event* (evdev),
          /dev/urandom, /dev/null; TLS via arch_prctl/fs; real signal delivery
```

- The ELF-compat track is **x86_64-only** (Linux binaries are x86_64; musl cross targets
  `x86_64-linux-musl`). It does **not** touch the ARM primary target — ARM stays the home of
  the native catalog and must never regress (two-arch parity rule, §6).
- **Corpus flavour (GB-013 decision, 2026-10-17 — `docs/elf-loader.md`):** the compat corpus is
  built **`-static-pie`** (`ET_DYN`, no `PT_INTERP`), not fixed-address `ET_EXEC`. A musl `ET_EXEC`
  is linked at 0x400000, which HobbyOS cannot map: `vm_region_insert()` rejects any region outside
  the 64 GiB user window, and low VAs are the shared kernel identity map in every address space
  (x64 0-8 GiB). Static-PIE self-relocates, so the loader may place it in the window. `ET_EXEC`
  still parses but is refused at load time with a clear reason.
- "Required support" for ELF run = the kernel ABI surface below, not a userspace emulator.

### 2.2 The compat layer (what gets built)

1. **ELF static loader.** Parse **static-PIE `ET_DYN`** (`ET_EXEC` accepted-but-refused, see §2.1),
   map each PT_LOAD with its real `p_flags` protections, zero p->bss, set the entry to
   `base + e_entry`. Loader lives alongside the existing flat-binary loader
   (`src/kernel/program_loader.c`) and dispatches on magic bytes; design + integration points in
   `docs/elf-loader.md`. `PT_INTERP` (dynamic) is a Program-2 ticket — static-PIE builds don't need it.
2. **Syscall translation table.** Linux x86_64 syscall numbers and HobbyOS's internal ABI are
   different. Implement a per-syscall shim (Linux args → HobbyOS call → Linux negative-errno —
   errno *numbers* must be Linux's, see §2.3). Core set first (W0/W1), then ioctl/futex/etc. as
   classes demand. Dispatch stays a table so the existing per-arch parity rule holds.
3. **ABI ground truth (the contract).** `gaming.md` §2.3 + `docs/linux-abi.md` define: syscall
   numbers, errno values, and **struct layouts** (`struct stat`, `sigaction`, `ucontext`,
   `epoll_event`, `termios`) exactly matching Linux x86_64. musl is compiled by a normal
   `x86_64-linux-musl` cross — musl itself never runs on HobbyOS; HobbyOS must satisfy what
   musl-generated code calls. This is the highest-risk correctness surface → differential tests.
4. **Kernel prerequisites** (proven on the selfhost fork; must be merged/gated first, ticket
   GB-012): `brk`, anon `mmap`/`munmap`, full `fork`/`execve`/`waitpid` + exit status, threads
   via `clone`+`futex`, **TLS** (`arch_prctl(ARCH_SET_FS)` on x86_64 → swap `%fs` base),
   **real signal delivery** (frame + `sigreturn`), `poll`/`select`, `gettimeofday`/`nanosleep`,
   `uname` (report a Linux-ish name for SDL/games), minimal `/proc/self` and `/sys` stubs.
5. **Compat device nodes** for the game experience, all backed by QEMU virtio:
   - `/dev/fb0` — mmap-backed HobbyOS framebuffer (+ `FBIOGET_VSCREENINFO` ioctls) so SDL's
     `fbcon` driver works untouched.
   - `/dev/input/event*` — evdev ABI fed from virtio-input, so SDL's evdev driver works.
   - `/dev/urandom`, `/dev/null`, `/dev/dsp` (audio if the virtio-sound epic lands).
   - These are added in the compat shim, not the native ABI.
6. **SDL2 static-musl** built against `x86_64-linux-musl` with the fbcon+evdev drivers — this is
   the dependency that makes "SDL game under compat" real.

### 2.3 Ground rules that never regress

- `-mgeneral-regs-only` kernel rule, no unaligned access, byte-wise parsing, Normal-NC DMA — the
  existing kernel constraints continue to apply to all new code.
- **Two-arch parity:** every native change must stay green on ARM **and** intel
  (`make host_tests`, `./run_unit_tests.sh`, `./run_unit_tests_intel.sh`, `make test`,
  `make test_intel`). The ELF-compat work lives behind existing arch guards and adds its own
  intel-only test tier (differential).
- **Tests are part of every ticket** (see §6) and every DoD names its test.

---

## 3. Environment & hardware

### 3.1 Inference servers — model-tier policy (how we use the fast model as much as possible)

| Tier | Model | Endpoint | Used by | Policy |
|---|---|---|---|---|
| **FLASH (workhorse)** | DeepSeek **V4 Flash** | `http://llm.localdomain:8000/v1` (vLLM, RTX 6000 pair box) | **every** worker / reviewer / tester agent | **Default for ~all tickets.** Target >90% of fleet tokens. |
| **DEEP (planner)** | DeepSeek **v4.1 Flash** (8× DGX Spark) | `http://dgx1.localdomain:8000/v1` (vLLM; alias `dgx-main`) | **Coordinator only** | Planning, re-planning, ABI/architecture design, blocked-root-cause after 1 FLASH retry, milestone reviews. |

Measures behind us (Sept/Oct 2026 community benchmarks for exactly this class of hardware):
~156–241 tok/s aggregate on the 8-Spark v4.1 box; ~47 tok/s single-stream per RTX 6000 card;
cold prefill ~840–2,100 tok/s depending on context. Consequences coded into the plan:

- **Agent count ≈ 8–12 full-speed agents** (fleet ceiling ~250–335 tok/s sustained). Configure
  10–14 agents; expect ~8 actively generating at any time. More agents ≠ faster; deeper/batched
  tickets get more throughput, not more agents.
- **Long-lived sessions, never churn.** Restarting a 300K-context agent costs minutes of
  prefill. Agents stay on their subsystem; new work = new ticket, not new session.
- **Deterministic work stays in tools** (grep/sed/test/CI), never in model tokens.
- Endpoints recorded and verified 2026-10-17 (GB-004), see §11.3 for the probe evidence. Mapping the
  tiers to the built infra: **FLASH = DeepSeek V4 on the RTX-6000 pair box** (workhorse),
  **DEEP = DeepSeek v4.1 on the 8× DGX Spark fleet** (planner/coordinator only).
- **Measured 2026-10-17 (single stream, GB-004, `max_tokens=200`, temp 0):** FLASH
  `llm.localdomain:8000` **57.7 tok/s** (196 completion tokens); DEEP `dgx1.localdomain:8000`
  **40.5 tok/s** (119 tokens). Both serve `max_model_len: 1048576`. These are *single-stream*
  numbers on a shared box — §3.1's 94–150 tok/s is an aggregate/community figure, so cap active
  workers from measurement, not from the table.
- **Model budget (validated 2026-10-17):** split lands ~90% FLASH / ~10% DEEP tokens; DEEP duty
  ~3% (it is mostly parked — that is intended). The binding constraint is FLASH throughput:
  the RTX pair measures ~94–150 tok/s, which feeds only **~3–5 agents** at full clip (the
  8–11 named in §5 is optimistic). Before scaling agents past ~4–6, benchmark both endpoints
  at GB-004 and cap concurrency at what FLASH sustains; keep DEEP reserved for planning.

### 3.2 QEMU focus (scope boundary)

All game execution this program is under QEMU. No real-GPU, no Proxmox-GPU/RDMA passthrough —
that workstream (`proxmox-gpu`) is explicitly **out of scope** here. Targets:

| Target | Invocation | Used for |
|---|---|---|
| `ARCH=arm` | `qemu-system-aarch64 -M virt -cpu cortex-a53 -smp 4` | Native catalog, primary target |
| `ARCH=intel` | `qemu-system-x86_64 -M q35 -smp 8` | ELF-compat track, native ports |
| Nested KVM | enable if the Proxmox host CPU permits (x86_64 farm VMs) | Speeds up intel boot/test several× |

### 3.3 Proxmox 192.168.0.19 — the built fleet (Oct 2026)

Five VMs created on Proxmox `c2`; disks on `c2flash` (NAS 192.168.0.56); **no VM has a
physical GPU**. All guests Ubuntu 26.04.1; dev VMs run the Kubuntu desktop (like VM 125).

| VMID | Name | IP | vCPU/RAM/Disk | Role |
|---|---|---|---|---|
| 140 | `swarm-farm-1` | 192.168.0.230 | 32 / 64 GiB / 128 G | QEMU build/test farm + worker agents |
| 141 | `swarm-farm-2` | 192.168.0.231 | 32 / 64 GiB / 128 G | same |
| 142 | `swarm-farm-3` | 192.168.0.232 | 32 / 64 GiB / 128 G | same |
| 143 | `swarm-coordinator` | 192.168.0.233 | 32 / 64 GiB / 128 G | coordinator Hermes (DEEP), re-plan cron |
| 144 | `swarm-ticketing` | 192.168.0.234 | 8 / 16 GiB / 64 G | Forgejo (git + issues + projects) |

Every dev VM already has: clang/lld/llvm, `qemu-system-arm`+`qemu-system-x86`, `qemu-efi-aarch64`
+`ovmf` firmware symlinked to the Makefile's `~/.local/share/...` paths, `mkfs.fat`/`mtools`,
python3, ccache/sccache, **Hermes installed** (`sarah:~/.hermes`), **HobbyOS repo cloned incl. the
WebKit port** (`third_party/webkit-hobbyos`), and `qemu-guest-agent` enabled. **LLVM 21.1.8
dist-staging is built and cached on farm-1** — rsync `third_party/llvm-21.1.8/dist-staging` to the
other VMs to skip that one-time build.

Notes: agents SSH passwordlessly between VMs; shared home/artifacts go via NAS (`/mnt/fast` on
ubuntu62 or the c2flash share). **Inference boxes and the DGX Sparks are off-limits for build
load** — never run QEMU tests on them (measured TTFT is concurrency-sensitive). Full inventory +
known-good verification commands: `swarm-fleet.md`.

---

## 4. Ticketing system

**Choice: Forgejo v17 on `swarm-ticketing` (192.168.0.234:3000 — built Oct 2026).** Rationale:
self-hosted, free, OSS, full REST API + webhooks (agents live off the API), git + issues + kanban
boards in one, milestones and labels, light enough for a VM.

### 4.1 Structure

- **Project:** `GAMING-BOARD` (kanban).
- Lanes: `📋 Backlog` → `🟦 Ready` → `🟨 In Progress` → `🟧 In Review` → `🚦 Merge Gate` → `✅ Done`; plus `🧊 Icebox` (Program-2 vision epics).
- Labels: `infra`, `abi`, `loader`, `libc`, `elf-compat`, `graphics`, `input`, `audio`, `native-port`, `game`, `tests`, `perf`, `docs`, `decision` (needs Sarah's sign-off), `deep` (DEEP-model required), `blocked`, `parity-ar` (ARM), `parity-x64`.
- Milestones: `M0·infra` `M1·abi-core` `M2·devices-sdl` `M3·games` `M4·acceptance`.
- Ticket skeleton: ID, workstream, weight S/M/L, depends-on, DoD + test, evidence link.
- Bot user: `swarm-bot` (API token) — the Coordinator and workers move/annotate tickets through it. Webhook on the board posts to the Coordinator.

**LANE LIMITATION (GB-002, found 2026-10-17).** The live Forgejo is
**v17.0.0+gitea-1.22.0** and its REST API has **no `/projects` endpoint**
(`/api/v1/repos/sarah/gaming-program/projects` → `404 page not found`), so the
`GAMING-BOARD` kanban project cannot be created or driven over the API that every
agent works through. **The lanes are therefore labels** whose names are the lane
names above (one lane label per issue, colours matching the lane), stacked on the
**native milestones** M0–M4; agents move tickets with
`tools/board/lane-move.sh GB-nnn "<lane>"` instead of dragging cards. When a Forgejo
build with `/projects` lands, `tools/board/board.py` grows a project backend and the
label fallback retires. Weight is carried as a `wt-S|wt-M|wt-L` label, the ticket
skeleton as an issue header block (see `tools/board/README.md`).

### 4.2 Ticket workflow (agents own it end-to-end)

1. Worker claims a `Ready` ticket → sets `In Progress` → creates git branch `ticket/<id>-<slug>` (worktree mode, one per agent).
2. Worker codes **and tests** (§6); runs the local gate; opens a merge request against `main`.
3. Reviewer agent reviews (diff + DoD + tests) → `In Review`.
4. Merge-gate CI (all test tiers + differential + both arches) must be green → `Merge Gate`.
5. Coordinator merges (or escalates `decision` to Sarah) → closes ticket **with evidence link** → lane `Done`.

### 4.3 The re-plan loop (autonomy — this is how the swarm steers itself)

Triggers (whichever first): **every 24h** (cron on `swarm-coordinator`), every milestone gate, `>3 tickets blocked`, `>30% red on latest gate`, or acceptance evidence landed.

The loop:
1. Coordinator snapshots: board state, open MRs, gate results, evidence dir tree, `gaming.md`.
2. Coordinator runs a **`deep` `PLAN` ticket** on **DeepSeek v4.1 Flash** only: re-assess plan vs reality, re-scope acceptance if reality demands, **mint new tickets** on the board, re-prioritize lanes, update `gaming.md` if facts changed.
3. Coordinator posts the re-plan summary as a comment on the `PLAN` ticket and closes it.
4. New tickets are `Ready` for workers by the next loop. The swarm therefore never waits for a human to discover what to do next — **autonomy is key**.

---

## 5. Swarm operating model

### 5.1 Roles

| Role | Count | Model | Where | Responsibility |
|---|---|---|---|---|
| **Coordinator** | 1 | DEEP | `swarm-coordinator` (.233) | plan + board + re-plan loop + merges + escalation; the only DEEP consumer |
| **Worker** | 3–8 | FLASH | `swarm-farm-1..3` | claim tickets, branch, code+tests, MR — one long-lived session per subsystem |
| **Reviewer** | 1 | FLASH | coordinator/farms | contract/diff/DoD review; flags `decision` |
| **Tester** | 1 | FLASH | `swarm-farm-1..3` | playthrough + stress harnesses, flaky triage |

### 5.2 Agent mechanics (existing Hermes capabilities; no custom build)

- One **profile per role** (`swarm-coordinator`, `swarm-worker`), each with a distinct
  `HERMES_HOME`; workers use `hermes -w` (worktree mode) so parallel agents never collide on
  files. Sessions under `tmux` on their VM; transcripts/evidence land on the shared NFS store so
  the Coordinator/planner can read them.
- Re-plan cadence via `hermes cronjob` (daily) on `swarm-coordinator`.
- `gaming.md`, `docs/linux-abi.md`, and this board are the only cross-agent interface points —
  no agent improvises ABI/architecture (that path is `decision`+user or the `deep` planner).

### 5.3 Autonomy guardrails (freedom within rails)

- Merge only through the gate; evidence-before-close; `decision` labels pause for Sarah.
- Real machines: **inference servers and DGX Sparks are never touched** (no repurposing, no
  test load). All execution is QEMU on the farm VMs.
- The 30-second deadlock rule from `hobbyos-run-tests` stays law: hang = defect, investigate,
  don't re-run.
- The two-arch parity rule stays law — no feature lands grey on either arch.

---

## 6. Testing strategy (three native tiers + two compat tiers)

Reuse the proven discipline; add the two new tiers for the compat track:

| Tier | What | How | Catches |
|---|---|---|---|
| T1 Host golden | pure userland logic on macOS, seconds | `make host_tests` (+ new per-game/function tests) | logic, refactor regressions |
| T2 Kernel unit | in-EL1, pre-scheduler | `./run_unit_tests.sh`, `./run_unit_tests_intel.sh` | kernel subsystems |
| T3 Integration | userland syscalls/SMP/IPC under booted kernel | `make test`, `make test_intel`; parallel `tools/run_parallel.sh`/`run_waves.sh` | ABI, scheduling, drivers |
| **T4 Differential (new)** | same static-musl binary on the Linux build VM **and** on HobbyOS-intel; compare stdout/exit/errno | `run_differential.sh` (harness from GB-007/GB-015) | syscall/errno/struct-layout parity — the compat layer's core risk |
| **T5 Playthrough (new)** | scripted QMP input + screendump assertions per game, long-duration cycles | extends `run_desktop_test.py` / `test_input.py` patterns | "launches and plays" at the acceptance level |

Every ticket's DoD names its tier(s). New game/port logic first appears as a T1 host test; the
compat layer's numbers are T4-locked before any T3/T5 work depends on them.

---

## 7. Timeline & milestones (4 weeks)

| Milestone | Target | Gate (all must be green + evidence) |
|---|---|---|
| **M0 — Infra (d1–3)** | VMs up, Forgejo live, Hermes fleet configured, branching+CI wired, baseline green | GB-001→GB-012 done; baseline suites green on `main` |
| **M1 — ABI core (wk1)** | static-musl W0 binary boots on `ARCH=intel`, differential parity | GB-013→GB-024; T4 green for W0 corpus |
| **M2 — Devices + SDL (wk2)** | `/dev/fb0`, evdev, TLS, threads/futex under compat; static-musl **SDL app** draws+inputs | GB-025→GB-033; T4+T3 green |
| **M3 — Games (wk3)** | native catalog ≥5 playable; launcher; **static-musl Doom (W4) playable under compat** | GB-034→GB-042 majority with T1/T3/T5 evidence |
| **M4 — Acceptance (wk4)** | S0+S1 evidence pack; long-duration playthroughs pass; re-plan → Program-2 backlog | GB-043→GB-055; both arches; evidence archive on NAS |

---

## 8. Seed backlog (first wave of tickets)

~55 tickets, `GB-001+`. Weights S/M/L; `[DEEP]` = v4.1-only (all others are FLASH). The
re-plan loop (GB-048) will grow/re-prioritize this continuously — this is the seed, not the
final list.

### Milestone M0 — Infrastructure (GB-001 … GB-012)

| ID | Ticket | Wt | Depends | DoD / test |
|---|---|---|---|---|
| GB-001 | Verify built fleet (§3.3: `swarm-farm-1..3` .230-.232, `swarm-coordinator` .233, `swarm-ticketing` .234): SSH trust + passwordless sudo, shared store (NAS `/mnt/fast` or c2flash) mounted, host_tests green | M | — | all VMs answer ssh; shared store mounted; `make host_tests` PASSES on ≥2 VMs |
| GB-002 | Forgejo already deployed (§11.2, `swarm-ticketing` .234:3000, repo `sarah/gaming-program`, admin `sarah`, `swarm-bot` token) — finish: `GAMING-BOARD` project, labels, milestones, repo mirrors | M | GB-001 | board visible; bot token works via API |
| GB-003 | Board automation: webhook board→Coordinator; `swarm-bot` lane-move + comment scripts | S | GB-002 | moving a ticket posts to coordinator |
| GB-004 | Hermes fleet: profile per role, endpoints (`FLASH` default, `DEEP` coordinator-only), tmux supervisors, session-store on NFS | M | GB-001 | every worker `GET /models` shows FLASH; coordinator shows DEEP |
| GB-005 | Git workflow + merge-gate CI: branch-per-ticket, MR template, gate = T1+T2+T3 both arches + parity | M | GB-001/002 | a dummy branch fails→passes the gate |
| GB-006 | Shared build cache: sccache/ccache + the prebuilt clang 21.1.8 cross toolchain; seed from farm-1 (LLVM dist-staging already built there → rsync `third_party/llvm-21.1.8/dist-staging` to farms 2/3 + coordinator) | M | GB-001 | cold vs cached build time measured |
| GB-007 | Differential harness skeleton: builder image (x86_64-musl cross on Linux) + run-on-HobbyOS-intel + stdout/exit/errno compare | M | GB-005 | `hello` diverges/lie detector works |
| GB-008 | `gaming.md`↔board sync script (tickets must match plan §1/§7) | S | GB-002 | lint passes |
| GB-009 | Baseline capture: record current green state of all suites on `main`; CI badge | S | — | baseline archived |
| GB-010 | **[DEEP]** Write `docs/linux-abi.md` v1: syscall numbers, errno values, struct layouts (stat/sigaction/ucontext/termios/epoll), TLS ABI, uname string | L | — | doc + first T4 assertions against it |
| GB-011 | **[DEEP]** Re-derive RimWorld / DSP / TA feasibility: per-game 20K-word assessment → staging plan → `MONO`/`VULKAN`/`RTS` icebox epics | M | GB-010 | three epics on board with staged tickets |
| GB-012 | Merge selfhost kernel core (threads/futex, vm v2+memfd+MAP_SHARED, fork/exec/signals) to `main` behind arch guards + tests | L | GB-010 | T2/T3 both arches green post-merge |

### Milestone M1 — ABI core / ELF loader (GB-013 … GB-024)

| ID | Ticket | Wt | Depends | DoD |
|---|---|---|---|---|
| GB-013 | **[DEEP design, FLASH exec]** ELF static loader (static-PIE `ET_DYN`; `ET_EXEC` parsed-but-refused) in loader path; coexist w/ flat loader; magic dispatch | L | GB-012 | static-PIE binary maps+boot, entry set right (`docs/elf-loader.md`) |
| GB-014 | Linux x86_64 syscall translation table: core set (open/read/write/close/lseek/stat/fstat/brk/mmap/munmap/fork/execve/waitpit/exit_group/writev… ) | L | GB-013, GB-010 | T4 W0 corpus pass |
| GB-015 | errno + struct-layout parity locks (T4 assertion suite from GB-010) | M | GB-014 | T4 fails on any mismatch |
| GB-016 | `uname` shim reporting Linux-ish identity (SDL/games sniff it) | S | GB-014 | T4 matches |
| GB-017 | `/dev/null`, `/dev/urandom`, `getrandom` shims | S | GB-014 | T4 |
| GB-018 | `poll`/`select`/`pselect6` under compat | M | GB-014 | T4 socket+pipe cases |
| GB-019 | `ioctl` subset: `TIOCGWINSZ`, `F_GETFL/SETFL`, `TCGETS` | M | GB-014 | T4 |
| GB-020 | `getdents64` + `/proc/self` minimal stubs | M | GB-014 | T4 |
| GB-021 | Threads under compat: `clone` → HobbyOS threads; `futex` wait/wake parity | L | GB-012, GB-014 | T4 multithread test |
| GB-022 | TLS ABI: `arch_prctl(ARCH_SET_FS)` (x86) / `TPIDR_EL0` (arm) on context switch | L | GB-021 | T4 `errno` per-thread + `__thread` vars |
| GB-023 | Real signal delivery: frame build, `sigreturn`, `rt_sigprocmask` | L | GB-021 | T4 signal test |
| GB-024 | **M1 gate:** W0 corpus (hello, cat, sh, coreutils-static) boots on intel + writes expected output, T4 green | M | GB-013..23 | gate evidence |

### Milestone M2 — Compat device nodes + SDL (GB-025 … GB-033)

| ID | Ticket | Wt | Depends | DoD |
|---|---|---|---|---|
| GB-025 | `/dev/fb0`: mmap → HobbyOS framebuffer (virtio-gpu 2D path) | M | GB-024 | SDL fbcon probes, rect draws |
| GB-026 | `FBIOGET_VSCREENINFO`/`FBIOGET_FSCREENINFO` ioctls | S | GB-025 | T4 parity |
| GB-027 | `/dev/input/event*` evdev ABI fed from virtio-input | M | GB-025 | SDL evdev sees moves/clicks |
| GB-028 | `nanosleep`/`clock_gettime`/`gettimeofday` parity + timer precision | S | GB-024 | T4 timings within tolerance |
| GB-029 | Audio: survey virtio-sound; stub `/dev/dsp`; SDL dummy/ALSA decision | L | GB-027 | recorded decision + silent-safe SDL |
| GB-030 | `memfd_create` + shared-mem for SDL surfaces | M | GB-025 | T4 |
| GB-031 | **[DEEP]** SDL2 static-musl build profile: fbcon + evdev drivers, no X11; freeze exact flag set | L | GB-027 | build reproducibly |
| GB-032 | SDL test app (`surface`, 2D blit, input echo) under compat | M | GB-031 | draws + echoes input |
| GB-033 | **M2 gate:** static-musl SDL app on intel shows window, responds to input, quits clean | M | GB-032 | gate evidence |

### Milestone M3 — Games (GB-034 … GB-042, native catalog = the parallel swarm)

| ID | Ticket | Wt | Depends | DoD |
|---|---|---|---|---|
| GB-034 | **DOOM native port** (doomgeneric, GPL): full-screen fb raycaster; *shareware `doom1.wad`* (license note: distribute free shareware WAD only) | L | — | plays + T5 playthrough |
| GB-035 | NES emulator (compact single-source) + homebrew/test ROM (license-clean) | M | — | boots → plays a ROM |
| GB-036 | Roguelike on existing curses port (nano's curses tree) | M | — | T1 logic + T3 |
| GB-037 | Second X11-era arcade port via `src/user/x11` xlib | S | — | T3 |
| GB-038 | Original arcade game by swarm + host golden tests subject | M | — | T1 + T5 |
| GB-039 | **Game launcher** desktop app: scans `/GAMES` (`.BIN` + ELF), launches either kind, escape-to-desktop | M | turtles | launches both tracks |
| GB-040 | Per-game evidence rig: serial log + screendump + replay scripts | S | — | assets land in evidence dir |
| GB-041 | **[DEEP]** **Doom as static-musl ELF under compat** (W4) — resolve interop gotchas (mmap, ioctl, threads) | L | GB-033, GB-034 | **playable on intel** — the S1 moonshot |
| GB-042 | Gamepad/joystick via evdev for arcade titles (optional) | M | GB-027 | axis/buttons register |

### Milestone M4 — Integration, testing, acceptance (GB-043 … GB-055)

| ID | Ticket | Wt | Depends | DoD |
|---|---|---|---|---|
| GB-043 | Long-duration **playthrough harness** per game (QMP input scripting + screendump assert; ≤30 s rule; auto-fail) | M | GB-040 | each game has ≥1 replay script |
| GB-044 | Two-arch parity CI per farm VM on every merge | M | GB-005 | run window < best |
| GB-045 | Performance pass: `gfxbench` baseline vs post; boot-time + framerate budget | M | — | numbers + report |
| GB-046 | Memory/leak pass under 30-min game cycling (freeze watchdogs) | M | GB-043 | no unbounded growth |
| GB-047 | Acceptance evidence pack for S0/S1: logs, screenshots, replays, gate history | M | GB-044/45/46 | archive on NAS |
| GB-048 | Re-plan cadence implementation: cron 24h + the three event triggers → coordinator → `deep` `PLAN` ticket | M | GB-003/004 | loop runs end-to-end once |
| GB-049 | Backlog grooming bot: close/await/block moves; blockers lint (deadlock rule) | S | GB-048 | board lint green |
| GB-050 | Flaky-test triage workflow + quarantine lane for timeouts (≥30 s = defect) | S | GB-044 | documented workflow |
| GB-051 | Audio acceptance (if GB-029 lands) | S | GB-029 | T5 with audio assert |
| GB-052 | Docs: per-game READMEs + user guide + `gaming.md` finalization | S | — | docs pass human read |
| GB-053 | Final acceptance run, both arches, full evidence archive | M | GB-047 | S0+S1 signed off |
| GB-054 | **[DEEP]** Program-2 roadmap ticket: Mono (RimWorld) staging, Spring RTS (TA) evaluation, Vulkan-virgl evaluation, dynamic-loader/glibc plan | M | GB-053 | epics + staged tickets |
| GB-055 | Program retrospective + rules update (what to teach `gaming.md`/skills) | S | GB-054 | lessons merged into skills |

---

## 9. Risks & open decisions

| Risk | Mitigation |
|---|---|
| ELF-compat syscall/errno parity churn | T4 differential locked before anything depends on it; Linux-ABI doc frozen by `[decision]` |
| Compat layer steals the whole month → no games | Native catalog (S0) is on its own agent track with zero dependency on the compat spine; S0 delivers no matter what |
| Token ceiling (8–12 agents) | Batch small tickets; long sessions; deterministic work offloaded to tools; FLASH >90% |
| Deadline pressure on syscall breadth | Weight-class ladder: each class shippable + demonstrable alone; W5+ deferred deliberately |
| Long-prefill agent restarts | Lifecycle discipline: agents persist per subsystem; evidence written incrementally |
| Threads/TLS/signals under compat are subtle | `[deep]` planning on the ABI semantics first; T4 multithread tests; small muscle car |
| Licensing (DOOM shareware WAD, game ROMs, TA content) | Ship only freely-distributable assets; document provenance per title |
| Legacy `-mgeneral-regs-only`/parity regressions | two-arch gate; kernel constraint checklist in review |

**Open (Sarah or `decision` tickets):** final endpoint/IP mapping for the two model tiers;
whether audio is in-scope this month; `ARCH=intel` promotion stakes for ARM primary target
(compat is intel-only by design — no ARM ABI change); optional nested-KVM enablement.

---

## 10. Quickstart (for the Coordinator on day 1)

Infrastructure is **already built** (VMs, Forgejo, repos, Hermes, LLVM cache — §§3.3, 11.7).
The Coordinator therefore starts mid-M0:

1. Read `gaming.md` §0–§7, then the board (Forgejo on `.234`).
2. Execute GB-002 (board rollout: `GAMING-BOARD`, labels, milestones — repo + `swarm-bot` exist),
   GB-003 (webhook + lane-move scripts), GB-004 (endpoints + profiles + **throughput benchmark**).
3. GB-008 (plan↔board sync), GB-009 (baseline capture on `main`), then open the M0 gate.
4. M1 ABI core is the critical path (**one owner**); native catalog tickets fan out immediately —
   do not stall S0 waiting for GB-013.
5. Hand the loop over at M3: GB-048 makes the swarm self-steering. Human touchpoints = `decision`
   tickets and milestone gates only.

---

## 11. Operating the swarm (runbook)

Companion to §5 — the concrete "how to run it" given the built fleet (§3.3) and Forgejo (§4).

### 11.1 Access & secrets
- Fleet inventory + known-good verification: **`swarm-fleet.md`** (this repo).
- SSH from the Mac: `ssh -i ~/.ssh/id_proxmox sarah@192.168.0.23x` — passwordless sudo on each.
- Secrets (VM bootstrap pw, Forgejo admin pw, `swarm-bot` API token): kept in
  `~/.hermes/cache/scratch/swarm-vm-bootstrap-pw.txt` (chmod 600) on the Mac. **Never commit
  secrets to this repo (it is on GitHub).** On swarm VMs export the token as an env var rather
  than writing it into tracked scripts:
  `export FORGEJO_TOKEN=$(cat ~/.hermes/cache/scratch/swarm-vm-bootstrap-pw.txt | grep swarm-bot | cut -d' ' -f4)`.
- Admin password rotates in the Forgejo UI anytime; the swarm uses the token, so rotation is safe.

### 11.2 Ticketing (Forgejo) — agent-facing
- Web UI: http://192.168.0.234:3000 — admin user `sarah`.
- REST API: base `http://192.168.0.234:3000/api/v1`, auth header `Authorization: token $FORGEJO_TOKEN`.
- Examples:
  - List open tickets: `curl -s -H "Authorization: token $FORGEJO_TOKEN" "http://192.168.0.234:3000/api/v1/repos/sarah/gaming-program/issues?state=open"`
  - Create a ticket: `curl -s -X POST -H "Authorization: token $FORGEJO_TOKEN" -H "Content-Type: application/json" "http://192.168.0.234:3000/api/v1/repos/sarah/gaming-program/issues" -d '{"title":"...","body":"...","labels":["abi"]}'`
  - Milestones/labels/projects (kanban) have their own namespaces; reuse the GB-003 lane-move
    scripts instead of ad-hoc curls.
- `swarm-bot` account (all-scopes token) already exists — created 2026-10-16.
- **Board tooling (GB-002/GB-003, in-repo):** `tools/board/` — `board.py` (claim/move/comment/status/lint),
  `import_seed.py` (idempotent §8 → board import), `{claim,lane-move,comment}.sh` (workflow wrappers),
  `hook_server.py` (webhook receiver), `poll.py` (60 s fallback). See `tools/board/README.md`.

### 11.2a Board → Coordinator wiring (built 2026-10-17)

- Webhook `coordinator-board-hook` (repo hooks API) → `http://192.168.0.233:8765/board-hook`,
  served by systemd unit **`hobbyos-board-hook.service`** on `swarm-coordinator`; events land in
  `/var/log/hobbyos/board-events.jsonl` (`wake: true` for lane moves into In Review/Merge Gate,
  `decision` labels, closes).
- **`hobbyos-board-poll.service`** runs the 60 s fallback poll (`poll.py`) into the same inbox.
- Forgejo needed `ALLOWED_HOST_LIST = private` under a new `[webhook]` section in
  `/etc/forgejo/app.ini` on `swarm-ticketing` — without it deliveries to a LAN address are refused
  ("webhook can only call allowed HTTP servers"). The only change is that appended section
  (the file had no `[webhook]` block before); removing it restores the original exactly.

### 11.2b Fleet preconditions discovered on first contact (fix these before any x86 run)

1. **x86 firmware must be writable.** The Makefile passes `-pflash $(EDK2_X86_64)`, and `-pflash` opens
   the image read-write; the cloud-init symlink to `/usr/share/OVMF/OVMF_CODE_4M.fd` makes every x86 QEMU
   run fail with `Could not open ...: Permission denied`. Fix per VM:
   `rm -f ~/.local/share/OVMF/OVMF_CODE_4M.fd && cp /usr/share/OVMF/OVMF_CODE_4M.fd ~/.local/share/OVMF/OVMF_CODE_4M.fd && chmod u+w …`
   (ARM uses `-bios` and was never affected). Applied on .233 + farms 1-3.
2. **`musl-tools`** must be installed wherever the T4 harness runs (`musl-gcc -static` is leg A); without
   it the harness honestly SKIPs and the differential gate is hollow. Installed on .233.
3. **The staging cache is not on every VM, and inter-VM SSH is not actually trusted.** §11.5 says to rsync
   `third_party/llvm-21.1.8/dist-staging` (227 MB, already built on `swarm-farm-1`) instead of paying the
   cold build; it was absent on farms 2 and 3, so `wgames` was blocked on hours of cold staging for its T5
   acceptance runs. Landed on farms 2-3 from `swarm-coordinator` (the coordinator has host keys for every
   VM; **farm-1 -> farm-2/3 rsync fails with `Host key verification failed`** because the cloud-init keys are
   only seeded for sarah's own key pairs — either seed them or route copies through .233).
4. **Workers run `hermes -w`.** A git worktree does **not** contain gitignored trees, so the vendored
   `third_party/*/src` extractions are missing there and vendored lanes (C++/RTTI, WebKit) go red inside a
   worktree while the same tree is green in the main checkout. Seed them (symlink is cheapest) before
   believing a worker's "main is red" report — tracked as GB-006.

### 11.3 Model endpoints & Hermes profiles
- Two vLLM OpenAI-compatible endpoints, **recorded + verified 2026-10-17 (GB-004)**:
  - **FLASH — DeepSeek V4 Flash** = `http://llm.localdomain:8000/v1`, `GET /v1/models` →
    `["deepseek-v4-flash"]` (root `/mnt/fast/deepseek-v4-flash-0731`), `max_model_len` 1048576.
    Default for every role; all routine work. Measured **57.7 tok/s** single-stream.
  - **DEEP — DeepSeek v4.1 Flash** = `http://dgx1.localdomain:8000/v1`, `GET /v1/models` →
    `["deepseek-v4.1-flash", "dgx-main"]` (root `/models/deepseek-v4.1-flash`), `max_model_len`
    1048576. Coordinator only: planning/re-plan/review. Measured **40.5 tok/s** single-stream.
- Verification (GB-004): `swarm-coordinator` resolved `model.default=deepseek-v4.1-flash`,
  `model.provider=dgx` → `hermes chat -q` answered (DEEP-LINK-OK); `swarm-farm-1..3` resolved
  `model.default=deepseek-v4-flash`, `model.provider=flash`, `model.base_url=http://llm.localdomain:8000/v1`
  → answered (FLASH-LINK-OK) with only `llm.localdomain:8000` in the logs. Role provisioning is
  scripted: `tools/swarm/provision-hermes-role.sh` (`ROLE=flash|deep`); it uses `hermes config set`
  only (never hand-edits `config.yaml`) and writes `FORGEJO_TOKEN` to `~/.hermes/.env` (chmod 600),
  never to a tracked file.
- Budget (validated 2026-10-17): ~90% FLASH / ~10% DEEP tokens. FLASH sustains ~94–150 tok/s →
  cap active workers at ~3–5 until GB-004 benchmarks say otherwise. Never put build/test load on
  the inference boxes.
- Per-role Hermes config: one profile per role (`swarm-coordinator`→DEEP, `swarm-worker`→FLASH),
  distinct `HERMES_HOME`; workers run `hermes -w` (worktree) so parallel agents never collide on
  files. Hermes is installed at `sarah:~/.hermes` on every VM.

### 11.4 Standing the swarm up (order matters)
1. **Coordinator first** on `swarm-coordinator` (.233): start in a tmux session with the
   `swarm-coordinator` profile and this doc as its charter. It runs GB-002→GB-004 in order
   (board rollout → endpoint verification → model-split check), then GB-008/009, then opens M0.
   `tmux new -s coord` → run hermes inside so it survives disconnect.
2. **Reviewer + Tester** (FLASH) on .233 / farms — start alongside step 1.
3. **Workers** on the farms: one long-lived `hermes -w` session per subsystem
   (abi/loader/libc, graphics/input/audio, native-ports, tests); each claims `Ready` tickets.
4. **Re-plan cron** (GB-048): `hermes cronjob` daily on the coordinator → `deep` `PLAN` ticket.
5. Evidence/artifacts land on the shared store (NAS `/mnt/fast` or c2flash) under `evidence/` —
   the planner reads them at re-plan time.

### 11.5 Day-to-day operation / monitoring
- Watch the board (UI or API). Every merge passes the full gate (T1–T3 both arches, + T4/T5 where
  the ticket demands). Evidence-before-close; `decision` tickets pause for Sarah.
- **30-second deadlock rule is law:** a hang is a defect — investigate, never just re-run.
- Two-arch parity is law — nothing lands grey on either arch.
- First `make run`/`make test` on farms 2/3 & coordinator triggers the one-time staging build
  unless you first rsync `third_party/llvm-21.1.8/dist-staging` from farm-1 (already built).
- Quiet farm VM? SSH in, check tmux/top/qemu, restart the agent session under tmux.

### 11.6 Stop / cleanup
- Quit agents: `tmux kill-session -t <name>` on each VM.
- Kill stray QEMUs inside a VM: `pkill -f qemu-system`.
- Leave VMs running for resumption, or power off from Proxmox (`qm stop <vmid>` on 192.168.0.19).
- Nothing in this swarm touches the inference boxes or DGX Sparks — stop means stop, no harm.

### 11.7 Known-good verification (done 2026-10-16/17)
- `make host_tests` → **ALL APPS SUITE TESTS PASSED** (farm-1, coordinator).
- `make MODE=unit_tests hobbyos.elf` → 2.1 MB aarch64 ELF linked by `ld.lld` (farm-2).
- `qemu-system-aarch64 -M virt -smp 4 -m 2048 -bios ~/.local/share/AAVMF/AAVMF_CODE.fd`
  boots UEFI to the shell, no crash (farm-2).
- LLVM 21.1.8 dist-staging cached on farm-1.

### 11.8 Execution log (newest first, maintained by the Coordinator)

**2026-10-17 — kickoff (Coordinator on .233; workers `wabi`@farm-1, `wgfx`@farm-2, `wgames`@farm-3 in
long-lived `hermes -w` tmux sessions).**

| what | state |
|---|---|
| Board (GB-002) | 55 seed tickets + PLAN-001; lanes are labels (no Projects REST in this Forgejo build, §4.1) |
| Board automation (GB-003) | webhook `coordinator-board-hook` + 60 s poll, both systemd units on .233 (§11.2a) |
| Model split (GB-004) | DEEP `dgx1:8000` on the coordinator, FLASH `llm:8000` on farms 1-3 (§11.3) |
| Plan sync (GB-008) | `tools/board/sync_gaming_md.py` — IN SYNC, drift detection proven |
| Baseline (GB-009) | T1 green (coordinator + farm-1) and **T2 green on ARM** (`run_unit_tests.sh`); Intel T2 launching, **T3 still to capture** |
| ABI contract (GB-010) | `docs/linux-abi.md` v1 (measured; `gen_abi_doc.py --check` guards drift) — In Review |
| ELF loader (GB-013) | parser + segment planner landed and host-tested (53 checks), linked into both kernel builds; **mapper half open** |
| Syscall table (GB-014) | 114 measured numbers + errno locked by goldens (T1); **shims + trap routing open** |
| First worker MR | **GB-026 merged** (fb ABI surface, worker `wgfx`, branch `ticket/26-fb-ioctls`); gate = T1 + both kernel links |
| Second worker MR | **GB-007 harness merged, In Review** (T4 differential host leg, worker `wabi`); device leg gated on GB-013 |
| Fleet pitfall | workers run in `hermes -w` worktrees, which do **not** carry gitignored `third_party/*/src` — vendored lanes (C++) go red in a worktree while `main` is green; seeded as GB-006 |
| Toolchain | `musl-tools` installed on the coordinator so the T4 harness leg A is real (`musl-gcc -static`); belongs in the fleet provisioning (GB-006) |
| Gate discipline (first incident) | a worker closed **GB-034 + GB-038 from the Ready lane** with the T5 playthrough unrun; both were **reopened to In Review** by the coordinator, the blocker (missing staging cache) was fixed, and the T5 evidence is now the gate. Recorded as the reason `tools/board/board.py` never closes from Done alone: `move` to Done closes, everything else is a lane swap |
| Fleet fix (blocker) | every VM's `~/.local/share/OVMF/OVMF_CODE_4M.fd` was a **symlink to the read-only system file**, and the Makefile passes it to `-pflash` — which opens read-write, so **every x86 QEMU run died with `Permission denied`** (ARM was fine: `-bios`). Replaced with a real, user-writable copy on .233 and farms 1-3 (`cp /usr/share/OVMF/OVMF_CODE_4M.fd …` + `chmod u+w`, sha256 identical to the system file). This blocked the whole Intel test path — i.e. the compat track's T2/T3/T4/T5 — until fixed |

Merge path for workers (no GitHub token on the farms): push `ticket/<n>-<slug>` to `origin`, post an
`EVIDENCE` comment on the ticket, and the Coordinator merges from `origin` after running the gate.
The automated merge-gate CI is GB-005 — until it exists the Coordinator runs the gate by hand and
records the output on the ticket.
