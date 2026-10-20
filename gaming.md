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
| **DEEP (planner)** | DeepSeek **v4.1 Flash** (8× DGX Spark) | `http://dgx1.localdomain:8000/v1` (vLLM; alias `dgx-main`) | **Coordinator + farm Verifiers** | Planning, re-planning, ABI/architecture design, blocked-root-cause after 1 FLASH retry, milestone reviews, **and verification/gate verdicts (`verify` tickets)**. |

Measures behind us (Sept/Oct 2026 community benchmarks for exactly this class of hardware):
~156–241 tok/s aggregate on the 8-Spark v4.1 box; ~47 tok/s single-stream per RTX 6000 card;
cold prefill ~840–2,100 tok/s depending on context. Consequences coded into the plan:

- **Agent count ≈ 8–12 full-speed agents** (fleet ceiling ~250–335 tok/s sustained). Configure
  10–14 agents; expect ~8 actively generating at any time. More agents ≠ faster; deeper/batched
  tickets get more throughput, not more agents.
- **Long-lived sessions, never churn.** Restarting a 300K-context agent costs minutes of
  prefill. Agents stay on their subsystem; new work = new ticket, not new session.
- **Deterministic work stays in tools** (grep/sed/test/CI), never in model tokens.
- **Verification is DEEP-class work.** Every `verify`-labeled gate ticket (T3 QEMU intel+arm, merge-gate,
  device-tier) is run by a farm **Verifier** session on **DeepSeek v4.1 Flash (`dgx1`)** via
  `--provider dgx -m deepseek-v4.1-flash --reasoning high` — never on the FLASH workhorse. A wrong verdict
  is the costliest mistake a swarm can make, so gate judgment gets the stronger model; gates stay
  scripted/deterministic, so DEEP stays ~10% of tokens even with verification parallelized.
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
  at GB-004 and cap concurrency at what FLASH sustains; keep DEEP reserved for planning + verification verdicts.

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
4. **Verification is its own ticket** (`verify` + `deep` labels). When an implementation lands `In Review`
   with evidence but no gate proof, the Coordinator mints `GB-nnn-V` (ticket skeleton per §4.1; DoD = the
   exact gate command(s) + pass criteria per §6 + two-arch parity). Minting happens at every re-plan for the
   `In Review` backlog, so gates run **in parallel** with new production tickets instead of queueing behind
   a single verifier.
5. A farm **Verifier** (DEEP session, §5.1) claims `GB-nnn-V` → `In Progress` → runs the gate on its own VM →
   posts evidence + a **RED/GREEN** verdict on both the verification and the implementation ticket.
   GREEN → the implementation ticket to `Merge Gate`; RED → back to `In Progress` with the failing output
   quoted verbatim. **This propagation is machine-checked:** `python3 tools/board/check_verdicts.py`
   (and `--self-test`, which proves the check can fail) compares every posted verdict against its
   parent's current lane and exits non-zero on a mismatch. It exists because the propagation was
   missed twice in one loop — GB-010 and GB-070 both sat `In Review` with a RED already posted
   against them, reading as "awaiting review" when a gate had been run and failed.
6. Merge-gate CI (all test tiers + differential + both arches) must be green → `Merge Gate`.
7. Coordinator merges (or escalates `decision` to Sarah) → closes ticket **with evidence link** → lane `Done`.

- **Verifier honesty is law.** A Verifier never verifies its own just-finished implementation without the
  Coordinator's sign-off (independence); no GREEN ever lands without a gate that can **provably fail**
  (GB-066/GB-070 discipline) — a skipped, self-served, or "looks green" gate is not a verdict.

### 4.3 The re-plan loop (autonomy — this is how the swarm steers itself)

Triggers (whichever first): **every 24h** (cron on `swarm-coordinator`), every milestone gate, `>3 tickets blocked`, `>30% red on latest gate`, or acceptance evidence landed.

The loop:
1. Coordinator snapshots: board state, open MRs, gate results, evidence dir tree, `gaming.md`.
2. Coordinator runs a **`deep` `PLAN` ticket** on **DeepSeek v4.1 Flash** only: re-assess plan vs reality, re-scope acceptance if reality demands, **mint new tickets** on the board — including a `verify`+`deep` **verification ticket `GB-nnn-V` for every `In Review` ticket that lacks a gate verdict** — re-prioritize lanes, update `gaming.md` if facts changed.
3. Coordinator posts the re-plan summary as a comment on the `PLAN` ticket and closes it.
4. New tickets are `Ready` for workers by the next loop. The swarm therefore never waits for a human to discover what to do next — **autonomy is key**.

---

## 5. Swarm operating model

### 5.1 Roles

| Role | Count | Model | Where | Responsibility |
|---|---|---|---|---|
| **Coordinator** | 1 | DEEP | `swarm-coordinator` (.233) | plan + board + re-plan loop + merges + escalation; primary DEEP consumer |
| **Worker** | 3–8 | FLASH | `swarm-farm-1..3` | claim tickets, branch, code+tests, MR — one long-lived session per subsystem |
| **Verifier** | 3 (one per farm) | DEEP | `swarm-farm-1..3` | claim `verify`+`deep` tickets, run §6 gates on own VM, post RED/GREEN verdicts; gate-running is its own role so gates parallelize |
| **Reviewer** | 1 | FLASH | coordinator/farms | contract/diff/DoD review; flags `decision` |
| **Tester** | 1 | FLASH | `swarm-farm-1..3` | playthrough + stress harnesses, flaky triage |

### 5.2 Agent mechanics (existing Hermes capabilities; no custom build)

- One **profile per role** (`swarm-coordinator`, `swarm-worker`), each with a distinct
  `HERMES_HOME`; workers use `hermes -w` (worktree mode) so parallel agents never collide on
  files. Sessions under `tmux` on their VM; transcripts/evidence land on the shared NFS store so
  the Coordinator/planner can read them.
- **Verifier session** = a second headless session on each farm running at the DEEP provider
  (`--provider dgx -m deepseek-v4.1-flash --reasoning high`), same `~/.hermes`, its own worktree,
  kept alive by a `swarm-verify@<agent>` systemd unit (clone of `swarm-driver@` carrying the DEEP
  provider flags) — see §11.4/§11.5.
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
| **M1 — ABI core (wk1)** | static-musl W0 binary boots on `ARCH=intel`, differential parity | GB-013→GB-024; T4 green for W0 corpus — **T4 means leg A vs a *HobbyOS guest*, not vs host `ho_*` mocks** (2026-10-19: the host-mock leg is a proxy that has never compared HobbyOS against Linux; the device leg is GB-066 and it does not exist yet) |
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
| GB-064 | **Self-registering host test suite.** Every ticket PR edits the *same* `host_tests:` prerequisite line and recipe body (PRs #25–#31 all collided on it on the night of 2026-10-17/18; #27's branch even *replaced* #29's two recipe lines), so N open PRs guarantee N−1 conflicts and every merge is a hand-resolved line. Discover `src/host/*_test.c` (and the python/parity tests) by `$(wildcard …)`/`$(patsubst …)` instead of enumerating them | M | — | adding a host test needs no edit to a shared Makefile line; two branches that each add a test merge with **no** conflict; `make host_tests` still builds *and executes* every existing test (verified by grepping the run's own PASS lines, never by the build alone) |

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
| GB-057 | **ARCH=intel T3 prints 222 failing checks (sed/grep/cmp/tsort) yet exits 0** — the intel integration tier is not a gate (minted from the GB-009 baseline run) | L | — | `make test_intel` with 0 FAIL lines + a deliberately-broken check fails the tier; `make test` (ARM) stays green. **Split 2026-10-19:** **a** (no halt at end of wave, 7/8) is **fixed** — `8995009`, waker-liveness predicate + `[ORPHAN-WAIT]`, two consecutive tiers `halt=2 orphan=1` (`evidence/gb-057/t3intel_probe56_wave_halts_orphan_named.log`); **b** (premature halt, 1/8, probe54) and **c** (LAPIC fault, ≤1/7) remain **open**, and b is the one that can still make a green tier a lie — `T3_MIN_LINES` caught it once, which is the only reason it was not read as a pass |
| GB-058 | **ARCH=arm T3 carries one real failure the exit-status-only tier called green: `SHELLTEST WATCHDOG: protocol stalled 25 s; FAILING TEST`** (`evidence/gb-009-baseline/t3_integration.log:5329`, surfaced by `tools/t3_tier.sh police`) | M | GB-056 | the stall is explained and fixed (the watchdog process-table dump is the evidence); `make test` (policed, ARM) green. **2026-10-19 — do this before writing any code:** GB-063's whole hypothesis was that this stall and the intel never-halt were *one* predicate defect, and that predicate changed on `main` (`8995009`, `4cefd7d`). Re-run the policed ARM tier on current `main` first; if it is green the ticket closes as a consequence, if it still stalls the dump now names the live set and the work is scoped by that |
| GB-060 | **x64 context-switch durability: a `proc_lock` critical section must not be able to strand the lock.** Measured on GB-057: a CPU that stops inside the scheduler's IRQs-off section holds `proc_lock` forever and every other CPU waits behind it — 9 `[HOLDER] cpu=2 sched_stage=5 picked=45` reports, exactly one claimant, no lock cycle, no exception (`evidence/gb-057/t3intel_probe3_*`). Whatever the immediate cause, the scheduler's critical sections need an owner + a failure path that releases (or a watchdog-bounded break) instead of a silent machine-wide freeze. **Update 2026-10-18: two shapes, and they must be separated.** The *stopped* holder is fixed and proven — `spinlock_t.owner`, `spinlock_release_if_held_by_me()` on both fatal halt paths, and a halt-keyed break that frees a dead owner's lock and re-acquires it normally (T1: `src/host/locksteal_test.c`, 14 checks, running in-suite); it was observed working in the wild (probe16) and declining correctly in probe19. The *wedged-alive* holder is measured and **open**: probe19 showed a halted CPU release `proc_lock`, a live CPU re-acquire it and then loop while holding it, with 46 `LOCKFOREVER` and no `[STOLEN]` — the break declined because the owner is running, not stopped (`evidence/gb-057/t3intel_probe19_steal_declined_live_owner.log`). Remaining work = a break keyed on **per-CPU progress heartbeat** (not elapsed time, which would free a live-but-slow holder's lock), with the threshold stated and printed on any break. **Second shape landed 2026-10-18 (`24a7f8a`):** `lock_stallbreak_ok()` breaks a lock whose recorded owner has stopped taking timer interrupts for `LOSTWAKE_DEAD_OWNER_MS` — the lost-owner reaper's own constant, reused so the two cannot drift — freeing it and re-contending exactly like the halt case, announced as `[STALLBREAK]` with the stale age; **T1 proves the boundary exactly** (`locksteal_test_host`, 27 checks: one ms inside the grace period protected, at the boundary broken, fresh heartbeat never broken, halt rule wins immediately). Wild (T3) run in flight as probe20 | M | GB-057 | **both** shapes recover: a deliberately-stopped holder *and* a deliberately-wedged holder do not freeze the machine, provable in T1/T3; the holder is named and the lock recovered in each |
| GB-061 | **x64 resume-path exception: `general_interrupt_handler -> enter_user_space -> restore_user_sp_helper` faults (CPU 6, pid 42)** — resolved stack from `evidence/gb-057/t3intel_probe2_exception_not_wedge.log`; a fault in the interrupt-driven return-to-user path is a defect with or without the GB-057 wedge, and it has a reproducible register/stack dump to work from | M | — | the stack is explained and the path survives a stress run: no exception dump in a clean T3 intel run |
| GB-063 | **Scheduler liveness accounting: a finished wave must reach `System halt`.** `schedule()`'s halt/`any_alive` branch is never reached because a slot parked in a state that is neither READY nor FREE/EXITED/ALLOCATED/THREAD_DONE is counted *alive*, so the machine idles forever instead of halting — measured all-idle-no-halt in `evidence/gb-057/t3intel_probe17_false_pass_loader_gp.log` (every CPU in `safe_wfi`, one slot parked), and consistent with GB-058's `SHELLTEST WATCHDOG: protocol stalled 25 s` (a test waiting on a process that is never scheduled again, machine otherwise healthy). **Update 2026-10-18 (Coordinator):** the *measurement* half is landed — when no READY work exists and the predicate still counts live work, the scheduler prints every live slot (`slot/pid/state/parent/claimed_by/name`), once per idle stretch, annotated in place for the two unwakeable shapes (RUNNING with no CPU on it = stale claim; a parked WAIT_SPAWN/WAIT_CHILD with no parent). probe20 is why: 30,179 console lines, `proc_lock` free, no strand, no kernel fault, and still no `System halt`. The predicate itself is **deliberately untouched** until that report has been read — `BLOCKED` on a timer is legitimately alive and a wrong predicate halts healthy waves, which is strictly worse than a wave that fails to halt. Second increment = the **owner-side bounded wait**: give a parked `WAIT_SPAWN` caller a deadline and resume it with `-EAGAIN` past it, which covers worker death, worker starvation and slot reuse with one rule and never touches another context's state. Ruled out by reading, so nobody repeats it: both spawn paths *do* release the caller when no worker slot is free, and both workers *do* unpark on completion — so the reachable leak is a worker that dies before unparking (the loader `#GP`, GB-013). Also ruled out by measurement: a scheduler-side forced reap (my own attempt) is wrong, because the live spawn path re-parks the slot underneath it — the same slot "reaped" 11 times in a row | M | — | the counter's state set is enumerated; a parked/idle slot is not counted live (or is reaped); a T3 run on the affected arch prints `System halt` where it previously idled forever, and each state it used to count is named. **CLOSED 2026-10-19 — both halves landed on `main`:** the counting half is `8995009` (a `BLOCKED` slot is alive only while it has a possible waker; 60 s budget; `[ORPHAN-WAIT]` names slot/pid/image/parent and states the slot is **not** reaped) and the owner-side half is `4cefd7d` (bounded parked `WAIT_SPAWN`, `-EAGAIN` past it, set at all four park sites on both arches). Proof of the DoD sentence itself: two consecutive intel tiers reach `System halt` with the orphan named (`evidence/gb-057/t3intel_probe56_wave_halts_orphan_named.log`, tail `[ORPHAN-WAIT] slot=13 … / System halt`), and the states it used to count are `BLOCKED`-on-a-dead-waker and `WAIT_SPAWN`-with-a-dead-worker |
| GB-062 | **intel FAT big-file test is inconsistent across runs: `[FATBIG] FAIL tail checksum clean (got=0x0000000000000800 want=0x0000000000000000)` in one run and `[FATBIG] FAIL short` in another, while it passes in two other intel runs of the same night and passed on ARM** — evidence `evidence/gb-057/t3intel_probe16_gb060_release_fired.log` and `t3intel_probe14_idle_stuck_no_halt.log` versus `t3intel_probe15_waitspawn_blocker.log` and `t3intel_probe17_false_pass_loader_gp.log`. Two different sub-checks failing in different runs is the signature of a timing/ordering defect, not a wrong constant: count it as flaky-on-intel (2 of 4) rather than deterministically broken | M | — | FATBIG passes on intel in repeated consecutive T3 intel runs, and the cause is named with evidence. **DoD amended 2026-10-18:** the original second clause ("the ordering the FAT write path depends on is named") assumed a storage-ordering cause; the verified cause is a signed-`char` comparison in `fatbig_test.c:95` (half the tail has bit 7 set, sign-extended against an int), reproduced on a host three ways and confirmed by consecutive intel runs, with the wrong NVMe conclusion corrected in `docs/fatbig-intel-delta.md` |
| GB-065 | **Device-tier blocker (2026-10-18): the only FAIL lines left in a complete intel T3 wave** — **`graphics_draw_line`'s pixel assertions fail on intel: GRAPHICS.BIN reports `FAIL: line startpoint`, `line midpoint`, `line endpoint`** — surfaced when GB-057's step bound let the graphics test complete for the first time; before the bound the process hung *inside* this function, so these assertions were never reached. **Attribution closed 2026-10-18 (worker `wgfx`, verified by me):** not the bound (the failing diagonal `(10,600)→(110,700)` needs 201 iterations and uses 101, so the bound cannot fire), not the line maths (32/32 host tests, 217 checks, exact in-OS call reproduced on a coherent buffer, `c654013`). Root cause = **`mmu_map_user_framebuffer()` mapped two 2 MB huge pages (4 MB) for an 8 MB 1920×1080×4 framebuffer**, so every pixel at y ≥ 546 was unmapped; the diagonal lives at y = 600..700 and the rect check at y = 100, which is exactly the observed asymmetry. **The fix is written and NOT on `main`** (`src/kernel/arch/x64/mmu.c`, map `ceil(fb_bytes/2 MB) = 4` pages; commit `3a86967` exists only on `ticket/65-line-assert` because PR #45 was merged at `c654013`, *before* this commit existed) | S | — | The three line assertions pass on intel with GRAPHICS.BIN completing. **Next action, stated so it is not re-derived:** rebase the single `3a86967` hunk onto `main`, build intel, rerun `tools/t3_tier.sh intel`, and attach the run where `FAIL=` is empty — that is the first intel tier that can be cited as green |
| GB-066 | **T4's device leg does not exist — the M1 gate has never been measured against HobbyOS.** `src/host/t4_parity.sh` calls leg A (static-musl, real Linux) and leg B *the same probe compiled for the compat surface and run on the host against glibc `ho_*` mocks*, deferring the in-guest leg to "once the ELF loader lands"; `--device intel` **SKIPs**, and a SKIP is not a pass. So every "T4 green" so far compares Linux against a *model* of HobbyOS, and GB-024's gate sentence cannot be satisfied by any kernel fix. Deliverable: a probe in `src/user/` built **both** ways (Linux static-musl for leg A; HobbyOS intel for leg B), staged into the guest through `T3_STAGE` (`tools/t3_tier.sh`), its stdout captured from the serial console, and a `t4_device.sh` that diffs **return value / errno / stdout** for the W0 row set and **exits non-zero when leg B is absent** instead of skipping. The tier must assert the probe's *arrival in the guest* (the guest's own dispatch/output line), never an image listing — probe57's `mdir` output grepped for `pie.bin` could only ever print 0 (`mdir` renders `PIE      BIN`), and that cost two runs | L | GB-013, GB-014, GB-024 | leg B runs **inside HobbyOS-intel** for the W0 corpus and the diff passes; a deliberately-wrong shim makes the same harness FAIL (it must be able to report both); a missing/absent probe fails the tier rather than skipping it; the row set is the one GB-015 locked (numbers, errno, struct layout), not a new one |
| GB-067 | **Host-staged files are read as ABSENT by the FAT16 driver and then corrupted in place.** Measured on intel, current `main`: a file copied onto `disk.img` after the build (`mcopy -o -i disk.img /tmp/pie_hello ::PIE.BIN`, confirmed by `mdir` and by `od` at offset 284,352 = **sector 555, entry 182 of a 512-entry root**) is **not visible to the guest**. The driver's own dump proves its mount arithmetic is identical to `od`'s (`fat_sector=32 root_dir_sector=544 root_dir_sectors=32 root_entries=512`), yet a raw sweep of all 32 root sectors finds **no entry beginning with the query's first three bytes** — so the read of that sector returns empty. The driver then takes its create-on-miss path and stamps a name-only entry (`attr=0 cluster=0 size=0`) over the real slot, so the image is **damaged** and the next boot inherits it. The 167 files staged *by the build* read correctly every boot, so the correlation is with entries late in the directory; correlation is not cause, which is why the ticket is written this way. Two defects are visible and either is independently wrong: (a) a stale/zeroed sector read, (b) an entry-initialisation bug that leaves `attr=0`. **This is the reason GB-013's DoD is unmet and GB-066's device leg cannot run**, and it silently masquerades as "the fixture was not there". Deliverable: a host-staged file is visible to the guest, a create-on-miss never emits a partially initialised entry, and a regression test that stages a file post-build and asserts the guest can read byte-exact content | S | — | post-build stage of a 4 KiB file, read back in-guest byte-exact; `attr != 0` on any entry the driver creates; evidence in the ticket |
| GB-068 | **M1 BLOCKER, highest priority (§4.3, 2026-10-19): 48 distinct processes are SIGSEGV-killed every intel wave, 31 at the identical address `0x7559DBC8` = `<stack base> + 0xDBC8`.** **Every intel T3 wave carries 44–55 user-mode memory faults and still reports "0 FAIL lines".** Measured across seven runs on `main` (`dist_run1` 54, `live_run1` 55, `final1` 48, `final2` 44, `quiet` 44, `badelf` 54, `fat3` 43) — **including runs with no ELF process whatsoever**, so this predates the GB-013 work and is not caused by it. The lines are `[KERNEL] pid=N (IMAGE) memory fault VA=0x755xDBC8 PC=0x100000…` in `SUBPRB.BIN`, `SHTEST.BIN`, `SHTEST2.BIN`, `SHTEST3.BIN`, `BLKTEST.BIN`, `FATBIG_T.BIN` — a stack-looking address in processes that otherwise "pass". The tier's verdict counted only `FAIL` lines meanwhile, and its fatal-fault check looks for `FATAL kernel-mode fault`, a different string, so these were invisible for the whole session. **The tier now prints `N user memory faults` on every verdict line** (2026-10-19), which is the measure this ticket is judged by. Deliverable: the cause named, and the count driven to 0 on a full intel wave — or an explicit, evidenced statement of why a non-zero baseline is legitimate, written into the plan | M | — | `T3_intel` verdict prints `0 user memory faults` on a full wave; the fault's mechanism is named in the ticket with evidence; a regression guard fails the tier if it regresses |
| GB-069 | **Kernel `#PF` at `cr2=0x1FC001008` (NVMe BAR window) kills the wave — and the mapping is NOT too small: `cpu_pd7[480]` installs a **2 MiB page** (`0x9B` = P\|RW\|PWT\|PCD\|PS) covering `0x1FC000000–0x1FDFFFFF`, so the faulting offset `+0x1008` is inside it.** Measured 2026-10-19: a run ended at 453 lines, `halted=0`, 4 FATAL kernel-mode faults, `vector=14 rip=0x7002A8E5 cr2=0x1FC001008`. Three surviving mechanisms, all different to fix: (1) **PD entry reuse** — `cpu_pd7[480]` is a hardcoded slot at a hardcoded VA in the kernel low range, so any other 4 KiB mapper inside that 2 MiB window rewrites it as a PTE-table pointer and the BAR page disappears; (2) **wrong CR3** — the mapping lives in the kernel's `cpu_pd7`, and a doorbell write from process context uses a CR3 whose entry 480 may be empty; (3) protection. Load-dependent, which is why it read as flakiness. Deliverable: an intel T3 run performs disk I/O and halts with 0 FATAL kernel-mode faults, and the ticket names which of the three it was | S | — | no `vector=14` with `cr2 ∈ [0x1FC000000, 0x1FDFFFFF)` on a run that does disk I/O; the mechanism named with the fault-site evidence (`error_code` P/W/US bits, current CR3, and `cpu_pd7[480]` raw value vs the value written at init) |
| GB-070 | **The M1 gate names a corpus the harness never runs: the boot wave loads 70 programs and only `HELLO.BIN` is from the W0 corpus.** Measured 2026-10-19 on a complete intel wave (63 programs exited, halt, `slot=63 MMTEST.BIN` reached): exit lines for `HELLO.BIN` = 2; **`CAT.BIN`, `SH.BIN`, `LS.BIN`, `GREP.BIN`, `WC.BIN`, `SED.BIN`, `CMP.BIN`, `CUT.BIN`, `TR.BIN`, `SORT.BIN` = 0 each.** `src/kernel/main.c`'s `test_wave_loader` lists 70 programs and every one is a `*TEST`/`_T` binary plus `HELLO.BIN`; `cat`, `sh` and the coreutils are on the image and are **never executed by the wave**. So GB-024's first clause — *"W0 corpus (hello, cat, sh, coreutils-static) boots on intel + writes expected output"* — has never been measurable, and no amount of kernel fixing would satisfy it while the harness does not run the corpus. Deliverable: a corpus run in the intel wave (or a dedicated `MODE=corpus` tier) that boots each corpus member as a program, captures its stdout, and compares it byte-exactly against the same binary run on the Linux box (the T4 style GB-066 established), with a per-member PASS/FAIL line | M | — | every W0 corpus member appears in an intel wave's exited-programs set and its output matches the Linux leg byte-for-byte; a deliberately-broken member fails the run |
| GB-071 | **Family B delivery: a user process holds a kernel-range pointer inside its own CPU's kernel scratch frame — located, delivery unknown.** Minted by PLAN-006 (2026-10-20) when GB-068's family B was split from family A. Measured: the fault address lies in `[target_sp-296, target_sp)`, `target_sp = &__stack_top - cpu*0x10000 - 4096` (`process.c:1557/3141`), `region=NONE` and CR3-equal are *correct* readings; the displacement moved from `-0x28` to `-0xA8` when `__stack_top` was read from a stale build, so it is **not** a constant. **Retired by measurement:** the iretq RSP slot / `enter_user_space → restore_user_sp_helper` (`IN_SCRATCH=1` on 0 of 14 resumes, 45 faults in the run), plus the TLS/`.bss`/v1-base/CR3 list on GB-068. Remaining delivery candidates: a kernel address handed back by a syscall, or a struct field written from a kernel pointer. Deliverable: the delivery named with pid/instruction/register evidence, the fix, and a regression guard | M | — | full intel T3 wave prints `0 user memory faults` of the family-B shape (parsed/raw counts quoted); the delivery is named in the ticket; the guard fails if a kernel address reaches a user process through the fixed path again |

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
| GB-059 | Port the four hand-rolled T5 harnesses (`run_doom_test.py`, `run_stardust_test.py`, `run_xnibbler_test.py`, `run_xcrypt_test.py`) onto `tools/evidence_rig.py`: locate the title by name from the parsed `[MENU]` dump (never a magic slot index — four PRs each asserted their own on the same growing `pinned_games[]` line) and take the repo root from the environment so it works from a `hermes -w` worktree | M | GB-040 | both a native port and a compat title run through the one rig from a worktree; the four hand-rolled copies are deleted |

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
| GB-056 | T3 PROCTEST `WEXITSTATUS` mismatch (got=2 for want=1 and want=42) — suites exit 0 anyway (minted by the Coordinator from the GB-009 baseline run; §10/§11.8) | S | — | T3 suite's PROCTEST passes and a printed FAIL fails the suite |

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
- **GitHub API access (PR open/merge):** push is SSH (seeded key, no creds). For GitHub REST API calls
  (open a PR, merge a PR, status), `GITHUB_TOKEN` is stored in `~/.hermes/.env` on every swarm VM
  (chmod 600). Any shell needing it must load it first — sessions started before it was added do NOT
  have it in env:
  `set -a; source ~/.hermes/.env; set +a` (or `export GITHUB_TOKEN=$(grep '^GITHUB_TOKEN=' ~/.hermes/.env | cut -d= -f2-)`).
  `GIT_TERMINAL_PROMPT=0` is also there so git never hangs on a credential prompt. The Coordinator owns
  all merges (mount a merge = PATCH `/repos/sarahkaylor/HobbyOS/pulls/{n}/merge` with the token).

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
   believing a worker's "main is red" report — tracked as GB-006. Landed: `tools/swarm/seed-worktree-vendored.sh`
   (plain `ln -s` only — `ln -sfn` silently type-changes tracked files, which blocks the next rebase).
5. **A non-login/background shell may not have `/usr/sbin` on PATH**, and `mkfs.fat` lives there, so
   `make disk.img` dies with `make: mkfs.fat: No such file or directory` / `Error 127` *after* the whole
   build has succeeded — which reads like a code failure and is not. If a backgrounded build fails only at
   the disk step, re-run it with `export PATH="/usr/sbin:/sbin:$PATH"` (or re-run `make disk.img` in an
   interactive shell; the objects are already built). `tools/run_waves.sh` already carries its own PATH
   handling for the same reason.
6. **`/dev/kvm` exists on the fleet VMs but is NOT usable as provisioned.** `ls -l /dev/kvm` is
   `crw-rw---- root:kvm`, and `sarah` is not in the `kvm` group, so an accelerated run dies with
   `Could not access KVM kernel module: Permission denied` (the device's presence is not the capability —
   that is the trap). The host CPU exposes nested SVM, so the one-line fix is provisioning:
   `sudo usermod -aG kvm sarah` (then a fresh login), or a targeted device ACL. Until that is done, **every
   intel QEMU run is TCG** — tens of minutes per wave, which is why an A/B measurement on `ARCH=intel` is
   still expensive. Tracked as part of GB-006's provisioning checklist. `tools/t3_tier.sh` takes
   `T3_QEMU_ARGS` so the fix is a one-liner away: `T3_QEMU_ARGS="-enable-kvm" make test_intel`.

### 11.3 Model endpoints & Hermes profiles
- Two vLLM OpenAI-compatible endpoints, **recorded + verified 2026-10-17 (GB-004)**:
  - **FLASH — DeepSeek V4 Flash** = `http://llm.localdomain:8000/v1`, `GET /v1/models` →
    `["deepseek-v4-flash"]` (root `/mnt/fast/deepseek-v4-flash-0731`), `max_model_len` 1048576.
    Default for every role; all routine work. Measured **57.7 tok/s** single-stream.
  - **DEEP — DeepSeek v4.1 Flash** = `http://dgx1.localdomain:8000/v1`, `GET /v1/models` →
    `["deepseek-v4.1-flash", "dgx-main"]` (root `/models/deepseek-v4.1-flash`), `max_model_len`
    1048576. Coordinator + farm Verifiers: planning/re-plan/review/verification. Measured **40.5 tok/s** single-stream.
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

**2026-10-20 — §4.3 re-plan (PLAN-006): 18 of 18 In Review tickets had no gate verdict, so every one now
has a `GB-nnn-V`.** Board 79 → 99 tickets (Backlog 2 · Ready 3→22 · In Progress 19 · In Review 18 ·
Done 34→35 · Icebox 3 · Merge Gate 0), `lint` clean, `sync_gaming_md.py` IN SYNC. Pass started on
`origin/main` = `d341b8a`; it ended on `dd391d4` (the board moved under the pass — see the last row).

| what | state |
|---|---|
| **The finding** | Every In Review card was un-gated: **15** had no verdict of any kind, **GB-034** had a Coordinator RED from 2026-10-17 (pre-fix, so it is a re-verification), **GB-044** had a Coordinator `8/8 PASS` on the PR head (not a Verifier, not the merged tree), **GB-066** had a row-level claim. Minted 18 × `GB-nnn-V` (#80–#97), `verify`+`deep`, Ready, each naming its exact gate, pass criteria, and what a RED does *not* mean; each implementation ticket now carries a comment naming its `-V` |
| **T1 measured, not assumed** | `make host_tests` on `d341b8a`: exit 0, **897 checks, 0 failed**, 58 `PASSED` lines. Of 9 `FAIL` string matches, 8 are the new gates' own negative-case proofs and 1 is a skip note — the first pass that can say that, because `t4_device.sh --self-test` and `t4_w0.sh --self-test` now run **inside** the suite |
| **Two instruments disagree (folded into GB-066-V)** | `src/host/t4_parity.sh --device intel` still prints `SKIP — device leg not wired yet`, while `tools/gate/t4_device.sh` is a real per-row in-guest differential. A reader of the first line concludes the M1 device leg does not exist; a reader of the second concludes it does. One leg, one verdict — GB-066-V settles which script is authoritative |
| **Tooling defect the new ticket kind created** | `issue_by_id()` matched with `^GB-016\b`; a hyphen is a word boundary, so `GB-016-V` matched `GB-016` — and `all_issues()` is **newest-first**, so `move GB-016 Done` would have laned and closed the *verification* ticket. `import_seed.py` had the same `\b` in its upsert matcher. Both now `^(GB-\d{3})(?![\w-])`, verified by assertion after minting (`GB-016` → #17, `GB-016-V` → #84) |
| **GB-068 split by fix site — and one of my own comments was refuted 4 minutes before I posted it** | Family A (the one-byte `.bss` NUL overrun at `align_up(_end)`) is GB-068. Family B was *located* (fault address inside the running CPU's kernel scratch frame `[target_sp-296, target_sp)`), but its first delivery hypothesis is **measured dead**: `enter_user_space → restore_user_sp_helper` shows `IN_SCRATCH=1` on **0 of 14** resumes across a 45-fault run, so GB-061 is *not* the source, and the `-0x28` displacement was measured against a stale `__stack_top` (the running kernel gives `-0xA8`, so it is not a constant). I posted the `-0x28` / "GB-061 owns family B" version from the plan row minutes after the refutation landed on the ticket, without re-reading the ticket — so a correction is posted on GB-068, GB-061 and PLAN-006. **The rule, again: a re-plan reads the ticket, not the plan's summary of the ticket.** |
| **GB-071 minted (the one production ticket)** | Family-B **delivery**, split out because it is a different job from family A with a different instrument and it had no owner: locate what hands a user process a kernel-range pointer (candidates: a syscall return value, or a struct field written from a kernel pointer), name it with pid/instruction evidence, fix it, add a regression guard. `deep`, M1, Ready. Do not re-walk the iretq slot there either |
| **GB-049 given the verify-lane lint** | `unverified-in-review`, `stale-verify`, `orphan-verify`. 18/18 un-gated cards were found by a hand-written grep; that detection belongs in `board.py lint`, not in a re-plan |
| **GB-001 escalated** | `decision` + `blocked`: ssh/sudo/`host_tests` clauses met; the NAS shared-store clause needs a credential on .56 (SMB-only, NFS closed). Cost of the pause is **zero lanes parked**; fallback (local evidence home + `git add -f`, split the mount into its own ticket) stated with a time box |
| **One production ticket minted, and only one** | **GB-071** (family-B delivery). Everything else in M0–M4 is behind the same premise (a user program that runs to completion on intel), so minting it would fill Ready with rows that cannot start |
| **`origin/main` moved during the pass** | `d341b8a` → **`dd391d4`** while this pass ran: `e47acd6` (the family-B correction above) and `dd391d4`, which contains **the same `GB-nnn-V` prefix fix** this pass made in `board.py`/`import_seed.py` — same defect, same fix, landed concurrently. Recorded as `dd391d4`'s work, not claimed here |
| **Milestone read** | M1's gate is now **measurable and red**: both instruments exist and fail honestly. Corrected critical path (see the GB-068 row): **GB-068 family A + GB-071 family B delivery** → faults to zero → GB-070 harness half → GB-066-V real device diff → the six shim `-V`s get their first verdicts. GB-061 keeps its own DoD and is no longer on this path. Milestone % is not comparable across passes now (the `-V`s sit in the same milestones but are gate work): M0 6/20 · M1 5/33 · M2 5/9 · M3 5/12 · M4 6/18 |

**2026-10-19 — Policy (caretaker): verification is its own ticket (`verify`+`deep`), run by per-farm
Verifier sessions on DeepSeek v4.1 Flash (dgx1); workers claim `verify` tickets directly; the re-plan mints
`GB-nnn-V` for every un-gated `In Review` ticket, so gate work parallelizes with production.**

**2026-10-19 — GB-068 characterised: two instruction families, and three instrument defects that hid them.**

| what | state |
|---|---|
| **Fault table, read as rows** | 44 serial-log lines carry `memory fault`; a strict line parse read **25**. The SMP console interleaves writes from 8 CPUs, so 43% of rows are shredded mid-line and silently dropped. Every fault count derived from the log is a lower bound until fault lines are atomic. |
| **Family A** | `movb $0x0,(%r8,%rdx,1)` — a **NUL terminator** — at `align_up(_end)`, exact **4-for-4** against each binary's `_end`, with `rw=w in=HOLE region=NONE`: a static buffer at the top of `.bss` overrunning by one byte. |
| **Family B** | `cmpb $0x0,0x1(%rdi,%rsi,1)` with `rsi=-1` — a **byte scan** — at `<64 KiB-aligned base> + 0xDBC8`, base varying per process, `rw=r in=PROT region=NONE`, CR3 correct: a string helper handed a kernel-range pointer. |
| **Family B — located, delivery still open (corrected same day)** | every family-B fault sits **inside the running CPU's kernel scratch frame region** `[target_sp-296, target_sp)`, with `target_sp = &__stack_top - cpu*0x10000 - 4096` (`process.c:1557/3141`) — so a user process is running with a stack pointer inside **its own CPU's kernel stack**, which is why `region=NONE` and CR3-matches are both *correct* measurements rather than contradictions. **Corrections to what I first posted:** the displacement is `-0xA8` against the running kernel, not `-0x28` (I measured `__stack_top` on a stale `hobbyos.elf` 0x80 lower), so it is not a universal constant; and the **delivery is not the iretq RSP slot** — a probe in `restore_user_sp_helper` comparing the value about to become the user RSP against the scratch region reported **`IN_SCRATCH=1` on 0 of 14 samples across a run with 45 faults**, with legitimate user SPs (`0x23FFF000`, `0x17FFFFFFF8`). So GB-061 is *not* the source through that slot. Next candidates: a kernel address returned to user space by a syscall, or a struct field written from a kernel pointer |
| **Dead mechanism — resume-RSP plant** | `enter_user_space`'s user branch fills the iretq RSP from `gs:[24]` (`user_sp_temp`, bridged from the per-CPU `user_sp[]` by `restore_user_sp_helper`); measured clean on every sampled resume. Do not re-walk |
| **Family C (unparsed)** | one shredded row shows a fault at `0x40000000`, the v1 user base, in `MEMTEST.BIN` — the v1-pointer-into-a-v2-process shape. Not characterisable from one line, but not dismissible either. |
| **Dead mechanisms — do not re-walk** | TLS self-pointer (probe printed `block=0x1000005b90 image=0x10 tls=0x1000005ba0`, all correct), unsigned `tls_size` underflow, `.bss` filesz-vs-memsz, v1-overlay base, wrong CR3 (26/26 equal), ELF-induced damage, and "the heap region is never created" (`sbrk` returns `USER_HEAP_BASE_V2`; `malloc`'s v2 branch uses the same constant). |
| **Instrument defect — symbol identity** | `ho_tls_setup_initial` is genuinely **1,903 bytes**: several `static` helpers in one libc translation unit share a section and one symbol, so every PC in the blob resolves to that name. That is how "15+ unrelated programs fault inside TLS setup" became a plausible finding. `-ffunction-sections -Wl,--gc-sections` makes it answerable by `nm`. |
| **Instrument defect — parse coverage** | my table read 25 of 44 rows and I reported the split as a census. The tier should print `N parsed of M raw`, so a shrinking parse can never look like a clean wave. |
| **Frame-of-reference defect — this checkout** | the main worktree was on the worker branch `gb057-recursion-probe`, so `grep tools/t3_tier.sh` read a stale file and "the tier has no fault count" looked true. It is not: `15c1733` and `59f16a8` add it on `main` (`grep -c` = 5 there). Checkout is now `coordinator-main` tracking `origin/main`; the uncommitted plan edits were stale duplicates of content already on main. |

Rule recorded: **before trusting a tool's silence or its labels, make it say something falsifiable — and count what you parsed against what exists.** Two of today's three instrument failures were tools answering a narrower question than the one asked, read as complete.

**2026-10-19 — §4.3 re-plan (PLAN-005): the gate had no instrument, and the compat track has one wall.**

| what | state |
|---|---|
| **Board snapshot** | 73 tickets: Backlog 5 · Ready 2 · **In Progress 20** · In Review 16 · Merge Gate 0 · Done 30. M0 6/16 · M1 3/19 · M2 5/9 · M3 5/10 · M4 6/14. `sync_gaming_md.py` IN SYNC 65/65 before the pass. Repo: `origin/main` = `26325ac`, 5 open PRs (#43, #31, #30, #26, #1), all `mergeable=null` (GitHub recomputes lazily — nothing merged, nothing green by inspection) |
| **Finding 1 — the M1 gate is not measurable, and that is a plan defect, not a kernel defect** | `src/host/t4_parity.sh` leg B is the probe compiled for the compat surface and run **on the host against glibc `ho_*` mocks**; its own header defers the in-guest leg to "once the ELF loader lands", and `--device intel` SKIPs. So M1's sentence ("T4 green for the W0 corpus") has been evaluated against a *model* of HobbyOS for the entire program. No amount of GB-057/GB-065 work can satisfy it. **Minted GB-066** (M1, L, → Ready `wabi`) = the device leg: a probe built both ways, staged through `T3_STAGE`, stdout captured from serial, diffed on rc/errno/stdout, and *failing* (not skipping) when leg B is absent. §7's M1 gate cell now says what T4 means |
| **Finding 2 — zero ELF binaries have ever executed** | probe57 (committed) put the first static-PIE on the image and produced no `[ELF-DISPATCH]` line; the follow-up showed why: `t3_tier.sh` rebuilds `disk.img`, so the staged fixture was wiped before QEMU — a harness bug, now fixed with `T3_STAGE` (stage after build, before QEMU, fatal on failure). GB-013's mapper is good code with **no caller that has reached it in a guest**; that, not the loader's maths, is why M2's SDL leg and M3's W4 Doom leg are unreachable. Also retired: the `mdir \| grep pie.bin` check I published could only ever print `0` (`mdir` renders `PIE      BIN`) — a zero that could not have been otherwise is not evidence |
| **Finding 3 — the board is holding claims, not progressing** | 20 In Progress vs 2 Ready, and the In Progress set is mostly *correct* — those tickets each hold a runtime leg that one wall blocks. The lanes therefore read as healthy while the track is serialized on a single binary execution. Two tickets were Done-in-fact and still In Progress (**GB-063** → Done: `8995009` counting half + `4cefd7d` owner-side bound, both on `main`, `[ORPHAN-WAIT]` then `System halt` in probe56). One fix was Done-in-fact and **not on `main`** (**GB-065**: `3a86967`, the 4 MB→8 MB framebuffer mapping, stranded on a branch whose PR had already merged at `c654013`) |
| **Adjusted, not minted (deliberately)** | **GB-057** split into a/b/c in the plan: a fixed and proven, b (premature halt, 1/8) and c (LAPIC, ≤1/7) open — b matters because it is the one shape that can turn a green tier into a lie. **GB-058** told to *run the policed ARM tier on current `main` before writing code*, because GB-063's hypothesis was that ARM's stall and intel's never-halt were one predicate, and that predicate changed. **GB-065** given its exact next action (rebase one hunk, build intel, rerun T3) so the last red line in the intel tier is not re-derived by the next reader |
| **Why only one ticket was minted** | The temptation was one ticket per stalled lane (M2 SDL, M3 Doom, M4 evidence). Those are not parallel problems: they are the *same* missing premise — an ELF that runs — plus, for M1, an instrument that can see it. Minting five tickets that all block on GB-013/GB-066 would put five honest-looking rows in Backlog and produce no work. The native track (S0) stays the one place capacity converts to playable games this week |
| **Lane moves** | GB-063 → Done (2 commits named). GB-066 created (#74) → Ready `wabi`. EPICs #66/#67/#68 → Icebox (§4.1: Program-2 vision epics live there, and three cards in Backlog were being counted as backlog). After the pass: Backlog 4 · Ready 3 · In Progress 19 · In Review 16 · Merge Gate 0 · Done 32 · Icebox 3 |
| **Tooling defect found and fixed** | `board.py issue_by_id()` only resolved `GB-nnn` titles, so `comment PLAN-005` and `move EPIC-66` both returned **`no issue for …`** — the tool could not address the two ticket kinds the re-plan loop itself creates. That is the mechanism behind the PLAN-003 phantom (a plan ticket that posted its summary and was never closed or laned) recorded on 2026-10-18. It now accepts `GB-`/`PLAN-`/`EPIC-` prefixes and a bare issue number; `import_seed.py`'s WORKSTREAM map also gained GB-063/064/065/066, which existed only in an uncommitted local copy while the board already carried those labels |
| **Summary** | Posted on **PLAN-005 (#75)**, closed |

**2026-10-18 — §4.3 re-plan (PLAN-004) + two tickets minted.**

| what | state |
|---|---|
| **Re-plan pass** | Board snapshot 70 → 71 tickets, `lint` clean, `sync_gaming_md.py` IN SYNC 64/64. Lanes: Backlog 11→9 / Ready 4→6 / In Progress 16 / In Review 14 / Merge Gate 1 / Done 23→25 / no-lane 1→0. Milestones M0 5/16 · M1 2/18 · M2 5/9 · M3 4/10 · M4 4/14. Summary posted on **PLAN-004 (#71)**, closed. |
| **The re-shape reality forced** | The T4 differential-lock layer is the one M1 lane *not* behind the intel wall (it runs on the host), and it is now essentially complete — GB-015/016/017/018/019/020/022/023 merged or on clean PRs. So M1's remaining gap is no longer "measure the contract"; it is "boot a W0 corpus on intel", which is single-threaded on x64 stability: **GB-057** (instance) / **GB-060** (lock durability — *both* shapes landed, `24a7f8a`, T1 boundary proof 27 checks) / **GB-061** (resume fault) / GB-013 loader `#GP` (`program_loader.c:611`) / new **GB-063**. Both T3 tiers remain uncitable; every green so far is T1/T2/T4-host + ARM T5. |
| **Minted GB-063** (M1, M, → Ready `wabi`) | *Scheduler liveness accounting: a finished wave must reach `System halt`.* probe17's intel wave idled forever instead of halting (every CPU in `safe_wfi`, one slot parked) — the same shape as GB-058's ARM `SHELLTEST WATCHDOG: stalled 25 s`. Likely the **shared** root cause of both tier failures: `any_alive` counting a slot in a state the scheduler does not know how to drain. Provable on ARM ⇒ not behind the intel wall |
| **Minted GB-064** (M0, M, → Ready `wgfx`) | *Self-registering host test suite.* PRs #25–#31 all collided on the same `host_tests:` prerequisite line and recipe body in one night (#27 even replaced #29's recipe lines). The resolver exists; the *cause* is one line every ticket edits. Wildcard/patsubst discovery ends the conflict class and unblocks GB-005/GB-044 |
| **PLAN-003 (#64) tidied** | Its loop posted a complete re-plan but never closed the ticket and never gave it a lane — a phantom open plan ticket on the board. Moved to Done, superseded by PLAN-004 |
| **No drift this pass** | Unlike the last two loops, the In Review set is honest — each holds a genuine runtime/leg it cannot yet evidence (e.g. GB-021's PR #35 is held by a real ABI disagreement: `futex(FUTEX_WAKE)` returns `rc=1` for us vs `rc=0` on Linux, i.e. our shim reports a waiter woken when none was). Nothing corrected |

**2026-10-18 — lane fan-out + board-drift correction (Coordinator, continuation).**

| what | state |
|---|---|
| **Ready lane refilled (it was empty)** | Promoted five genuinely unblocked tickets, each with its deliverable, gate, and an explicit note about what is *not* gated: **wabi** ← GB-017 (dev nodes + `getrandom`), GB-020 (`getdents64` + `/proc/self`), GB-019 (ioctl subset) — the core of each is independent of GB-014's syscall routing, which is only their wiring tail; **wgames** ← GB-043 (long-duration playthrough harness, dep GB-040 Done) and GB-045 (gfxbench baseline, no unmet deps). **wgfx got nothing, deliberately**: every remaining graphics/input/audio Backlog ticket is gated by wgfx's own in-flight work (GB-032 SDL, GB-029 audio), so that lane is full rather than idle — manufacturing a promotion there would have been theatre |
| **Board drift corrected (5 tickets)** | GB-025, GB-027, GB-028, GB-030, GB-042 were sitting in In Review / Merge Gate with their work already merged on `main`. Each was **verified present at `origin/main` before** being moved to Done, naming the PR that landed it. A merged ticket in a review lane tells the next reader that work is pending when it is finished — the same class of lie as an instrument that reports the wrong state |
| **Merge queue** | PR #4 closed as superseded (its work landed via PR #10) with a pointer comment so nobody rebases it. PR #1 (DOOM / GB-034) and PR #2 (STARDUST / GB-038) stay red on their T5 legs; PR #17 (GB-059 rig) stays changes-requested. Nothing in the queue had green gates, so nothing was merged |

**2026-10-18 (later) — worker wave landed into the queue.**

| what | state |
|---|---|
| **Second fan-out wave** | wabi ← GB-016 (`uname` shim) and GB-018 (`poll`/`select`/`pselect6`). GB-017/GB-019 went In Review and GB-020 In Progress within the hour of the first wave, so the lane is genuinely fed rather than nominally |
| **GB-016: I demoted a claimed ticket, then corrected it** | I moved GB-016 In Progress → Ready as part of the wave, reading a lane listing taken a minute earlier — `swarm-farm-1` had claimed it in between. Restored to In Progress with an honest note. **Lesson: re-read the ticket's current lane at the instant of the move** (the board is live and claims arrive in seconds), which is the same stale-view error as reading an instrument once and believing it later |
| **PR #25 merged (GB-017 + GB-019 + GB-020)** | One branch, three tickets, +150 lines of T4 differential probe: `main` now races 31 probe rows, static-musl vs the HobbyOS host compat surface. Reviewed rather than rubber-stamped — the probe deliberately does not diff *random payloads* leg-to-leg (only return counts and errno are contractual, which is the correct call: diffing random bytes would have produced a flaky test that "proves" failure) — and I ran the host leg by hand to confirm the rows are real (`getrandom_16 rc=16`, `urandom_open rc=3`, `urandom_read16 rc=16`, `null_write3 rc=3`, `null_read1 rc=0`). Gate: T1=0, parity PASS |
| **PR #26 (GB-035 NES revision) returned to the worker** | Dirty against `main`: 10 content hunks across 5 files, because it revises files `main` already has from the earlier GB-035 merge (`nes.c` 383→411, `nes_test.c` 130→230). I did **not** resolve it — with 10 hunks inside emulator content, choosing a side is a judgement about the worker's own code. Told them precisely what conflicts and why, plus the two mechanical hints (the `host_tests:`-line resolution rule, and that a wholesale replacement must be *stated* in its EVIDENCE comment) |
| **GB-060 scope amended after probe19** | The stale-owner break did **not** fire in the wild, and the log says why: the halted CPU released `proc_lock` (the earlier fix working), a **live** CPU re-acquired it and then wedged *alive* — a shape the halt-keyed break correctly declines (46 `LOCKFOREVER`, no `[STOLEN]`). So the class has two shapes and only one is closed. Plan row updated: remaining work = a break keyed on a **per-CPU progress heartbeat**, never on elapsed time, with the threshold printed on any break |

**2026-10-17 — merge-queue pass (Coordinator, continuation).**

| what | state |
|---|---|
| **Merge queue drained to the M2 ABI-lock layer** | Four increments landed and gated on the integrated tip (`092edf5`, +461 lines): PR #13 `fb_pixel` B8G8R8A8 packing lock (a2eebdb, credited to GB-025), PR #12 gamepad codes in `tools/linux-abi/probe_abi.c` (ad14334, GB-042 governance), PR #11 GB-042 gamepad/joystick ABI lock (49d523f), PR #6 GB-028 clock/timer ABI lock (092edf5). Gate: `make host_tests` T1=0 with `fb_pixel_test_host` 12, `joypad_test_host` 11 and `clockabi_test_host` 13 checks all executing **inside** the suite, plus `MODE=unit_tests hobbyos.elf` links ARM + intel T2A/T2B=0. All four tickets stay In Review: their runtime legs are not citable while the device tier is red (GB-057/GB-058) |
| **Merge-conflict tooling (and a self-inflicted incident)** | Every native/ABI PR edits the same `host_tests:` prerequisite line, so conflicts are guaranteed. Added `resolve_makefile_additions.py` (prerequisite line → token union; recipe lines → keep both; anything else refused) and the rule that **`make -n host_tests` must parse after any resolution**. I first resolved `#6` with the older line-union script, which glued a recipe line onto the prerequisite line, and then committed the merge **with the markers still in the file** — caught immediately by the parse check and the grep, amended to `092edf5`. Exactly the incident recorded for GB-027 earlier; the *check* is what catches it, not the intent |
| **PR hygiene** | Three of four branches were based on stale mains (the conflict work above), and one PR (#13) carried no ticket id in the title and no board comment — merged against GB-025 because the diff named it, with a note on the ticket asking for the id next time. `git fetch && git rebase origin/main` before opening a PR is now stated on the affected tickets |

**2026-10-17 — continuation session (Coordinator on .233; `wabi`@farm-1, `wgfx`@farm-2,
`wgames`@farm-3 alive and dispatched through the board).**

| what | state |
|---|---|
| **GB-057 root cause #1 (the 222 FAIL lines)** | `src/kernel/arch/x64/trap.c: sys_exit` never remapped the exit code out of rdi into the canonical `regs[0]` slot the shared `process_exit()` reads — on x64 `regs[0]` is **rax**, which at `syscall` entry holds the *syscall number*, so `SYS_EXIT == 2` became every intel exit status (`status=512`, `WEXITSTATUS == 2` for **every** child, whatever it returned). That is why only the status-reading suites failed (SEDTEST/GREPTEST/CMPTEST/TSORTTEST/…). Fixed; ARM was already correct (x0 carries the code there) |
| **GB-057 root cause #2 (early exits / ECHILD)** | `sys_spawn`/`sys_spawn_ex` (both arches) retried the spawn-worker slot for `18000 × 100 ms` — a 30-minute **hot spin** by a caller that is *already parked in WAIT_SPAWN holding a PCB slot*, while its worker needs a free slot from the same table: a circular wait that waiting harder cannot break. Watchdog proof captured (7 of 8 CPUs, `age_ms≈350000`, `syscall=8`, console silent, loader starved). Bounded to ~1 s + the documented `-EAGAIN` path |
| **GB-057 blocker #3 (found, NOT fixed)** | With #2 bounded, the same intel wave runs much further and then **hard-deadlocks on `proc_lock`**: `[LOCKFOREVER] lock=0x74530c70 (= proc_lock)` from 7 CPUs, all in `spinlock_acquire_irqsave+0xe` called from `schedule()`, `process_create_internal_ver()`, `process_thread_create()`; after that the console is fully dead (IRQs off, watchdog silent). Suspect: `process_fork()` holds `proc_lock` across `vm_as_clone_into()` + the 32 MiB `kmemcpy`. Next owner: whoever takes GB-057's remainder |
| **T3 tier is now a gate (GB-056/GB-057 DoD)** | `tools/t3_tier.sh` + `make test` / `make test_intel` / `test_arm` / `test_intel` now police their own console: non-zero exit on any `FAIL` line, on a missing `System halt`, or on the wall-clock deadline (hang = defect). Raw boots stay available as `make test_run_raw` |
| **GB-006 worktree false-reds** | `tools/swarm/seed-worktree-vendored.sh` links every gitignored `third_party/…` tree from the main checkout into a `hermes -w` worktree (idempotent; never clobbers a tracked file — `ln -sfn` does, and silently type-changes tracked files, so never use `-f`). Verified: 13 vendored paths linked; `make host_tests` green in a seeded worktree |
| **GB-001 fleet** | all 6 VMs answer ssh + passwordless sudo; `host_tests` green on coordinator + farm-1. The "shared store mounted" clause is **not met**: 192.168.0.56 is SMB-only (NFS closed) and no NAS credentials exist on .233 → `decision` label for Sarah. Not closed |
| **M0 gate** | opened with two named exceptions (see the gate comment on PLAN/board) — GB-008 sync IN SYNC 58/58 (GB-058 minted), GB-009 baseline archived (T1/T2 green both arches, T3 ARM carries 1 real FAIL (GB-058), T3 intel red → GB-057) |
| **M1 gate corrected** (2026-10-18) | I labelled GB-065 the M1 blocker and that was wrong: `gaming.md:372` makes the M1 gate *"W0 corpus boots on intel + writes expected output, T4 green"*, and GB-065's failures are in `GRAPHICS.BIN` — a T3 member, not the W0 corpus, not T4. Worse, the T4 leg that matters does not exist: `t4_parity.sh` compares musl-Linux against **host glibc `ho_*` mocks** and its own header defers the in-QEMU leg to "once the ELF loader lands"; there is no `src/user/` probe binary, so `--device intel` SKIPs. **T4 green has never meant HobbyOS agrees with Linux.** The loader itself is on `main` (`elf_image.c` ET_DYN/`is_pie`, `elf_loader.c`, `program_loader.c:757`) and GB-013 sits In Review. Next: verify GB-013's DoD by execution (static-PIE maps+boots on intel) and build the in-guest T4 probe |
| **GB-057-a FIXED — the wave halts** (`8995009`) | The wave-end predicate was a negative test, so `PROC_STATE_BLOCKED` counted as live work and a parentless blocked `SPAWN.BIN` kept `any_alive` true forever: `!any_alive` never fired, `scheduler_finished()` never ran, the wave never halted (7 of 8 runs; the trace was already in probe42 as `slot=12 st=5 parent=-1`). Fix: a **waker-liveness** predicate — the scheduler lazily stamps the first pass that sees a slot `BLOCKED` and excludes it only after a 60 s budget, printing `[ORPHAN-WAIT] slot/pid/name/parent/wake_ms/waited_ms` and stating the slot is **not reaped**. Not a state exemption, because a wave whose children wait legitimately on a pipe must not truncate. Proven in two consecutive intel tiers (`halt=2 orphan=1` both, slots 12 and 13, same tail test `MMTEST.BIN`). Line count fell 3,689 -> 1,296 because **2,315 of the old run's lines were `[IDLESTUCK]` spin chatter** — so line count was partly a mark of a *sick* wave, retiring a number I had been quoting as progress. Also landed: `[ALIVE-SET]`/`[IDLECLAIM]` print `<torn:a/b>` instead of quoting a racy pid (probe54 printed `pid=62831` for slot 1). Remaining on GB-057: **b** (premature halt, 1/8, probe54) and **c** (LAPIC fault, <=1/7, the widened spawn storm failed to provoke it twice) |
| **PR #1 (DOOM) / PR #2 (STARDUST)** | rebased onto `origin/main` (both were based on stale mains: 8101128 / e3039f1). PR #1 T1 gate green on the rebased tree (incl. ELF-image/ABI/fb_ioctl/evdev/t4_parity inside `make host_tests`). T5 playthrough still unrun → both stay In Review. Review note: both harnesses hardcode `ROOT=$HOME/Documents/GitHub/HobbyOS` and assert a magic launcher slot (12) that neither PR pins in `desktop.c`'s `pinned_games[]` |
| **PR #1 gate verdict (intel T5)** | **NOT merged.** T1 green; launcher pin fixed by the coordinator (DOOM at idx 18, structurally asserted) and the harness's IWAD false-positive fixed (its own `[disk] IWAD …` build echo was being matched). T5 still **red and real**: `nonzero=1024000, colors=6` over the whole centre region = a flat fill, and the serial shows only `DESKTOP.BIN` was ever loaded → **DOOM never launched**; the screendumps are the desktop background. Handed back with the launch-path + stdin-input (desktop apps get keys on **stdin**, not `get_events()`) work named. Patch: `~/.hermes/cache/scratch/gb057/pr1_coordinator_review_fixes.patch` |
| **Worker lane movement (continuation)** | `wgames` fixed two real STARDUST defects (unpinned title; no stdin input path) and posted **green ARM T5** evidence → GB-038 at Merge Gate (intel T5 + harness de-magic-index still required). `wgfx` posted ABI-lock increments for GB-027 (evdev) and GB-030 (memfd) — accepted as first increments, not as the tickets; both missing the runtime node / PR hygiene. `wabi` pushed a parallel tier-gating branch (`ticket/57-intel-t3-gate`) that duplicates the landed `tools/t3_tier.sh` → redirected to GB-057's remaining blocker (the `proc_lock` deadlock) |
| **Re-plan (PLAN-002)** | M0 gate opened with named exceptions + §4.3 re-plan recorded on the board (ticket #60, closed). Gate rule now in force: no intel tier citable until GB-057 closes, no ARM T3 citable until GB-058 closes |


**2026-10-17 — kickoff (Coordinator on .233; workers `wabi`@farm-1, `wgfx`@farm-2, `wgames`@farm-3 in
long-lived `hermes -w` tmux sessions).**

| what | state |
|---|---|
| Board (GB-002) | 55 seed tickets + PLAN-001; lanes are labels (no Projects REST in this Forgejo build, §4.1) |
| Board automation (GB-003) | webhook `coordinator-board-hook` + 60 s poll, both systemd units on .233 (§11.2a) |
| Model split (GB-004) | DEEP `dgx1:8000` on the coordinator, FLASH `llm:8000` on farms 1-3 (§11.3) |
| Plan sync (GB-008) | `tools/board/sync_gaming_md.py` — IN SYNC, drift detection proven |
| Baseline (GB-009) | T1 green (coordinator + farm-1); **T2 green arm + intel** (344/0 intel); **T3 ARM green (0 FAIL lines), T3 intel red-but-exit-0** (222 FAIL lines) → GB-057. The baseline is **not** complete on intel and no intel gate may cite T3 until it is |
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
