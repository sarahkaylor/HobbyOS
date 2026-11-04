# Program-2 roadmap — the staging order after the M4 acceptance

*GB-054. This is the re-plan the M4 acceptance triggers (§4.3): Program-1's measured
capability, the three Program-2 epics, and the smallest first ticket for each. It does not
re-argue feasibility — `feasibility-rimworld-dsp-ta.md` (GB-011) did that — and every
dependency is cited to a landed or ticketed card.*

## 1. What Program-1 actually proved (measured, not claimed)

| capability | evidence | state |
|---|---|---|
| Static-PIE x86-64 Linux ELF runs, syscalls translated | GB-137-V2, GB-183, GB-184; W4 DOOM gate | **landed** |
| Unmodified `linuxdoom-1.10` runs through the compat loader and XHOBBY | `docs/doom-elf/REPORT.md` (GB-091) | **landed** |
| Native DOOM (doomgeneric) renders and consumes input | GB-034-V GREEN at `138a6874` | **landed** |
| Desktop launcher starts .BIN and static-PIE ELF by name | GB-039-V GREEN | **landed** |
| Native SDL-shaped app on `/dev/fb0` + `/dev/input/event0` | GB-032, GB-033-V2, GB-194 | **landed** |
| Five pinned titles, both arches, default 8 CPUs, 0 FATAL | GB-053-V2 / S0-S1-ACCEPTANCE.md | **landed** |
| **Native** dynamic linking (`PT_INTERP /LDHOBBY.SO`, `LIBC.SO`, `dlopen` shape) | `docs/shared-libs.md`; HDYN.BIN both arches | **landed** |
| libSDL itself built for fbcon (not a hand-rolled shim) | GB-031 | **decision, open** |
| **Linux** dynamic linking: glibc `ld-linux` + its `DT_NEEDED` closure | `docs/linux-abi.md` §W5 | **absent — Program-2** |
| GPU 3D (OpenGL 2.x / Vulkan) | `docs/graphics-accel.md` | **absent — Program-2** |

The boundary is sharp and specific: **static** Linux binaries run, and **native** HobbyOS
binaries can be dynamic (LDHOBBY.SO), but a **glibc dynamic** Linux binary is refused at
`PT_INTERP`, and there is no 3D surface. Every Program-2 target needs the Linux-dl route and
most need the 3D surface, which is why the order below is a ladder, not three projects.

## 2. The epics and their entry criteria

| epic | board | entry criteria (all required) | first blocker |
|---|---|---|---|
| **MONO** — RimWorld (C#/Mono) | #66 | Linux-dl loader + threads/futex (GB-021) + audio (GB-029) + a CLR on the compat surface | W5 ladder rung 1 |
| **RTS** — Spring / Total Annihilation | #68 | Linux-dl loader + SDL2 + **OpenGL 2.1 surface** + threads | GL surface + W5 |
| **VULKAN** — DSP (Unity/IL2CPP) | #67 | Vulkan driver (or a virgl-backed GL path) + Linux-dl + huge binary | virgl answer + W5 |

None is a one-card change; each is a milestone checkpoint with child cards (§4). The epics
stay in `🧊 Icebox` until their entry criteria are met — promoting them early would
manufacture false Ready work (WORK POLICY rule 5).

## 3. The ladder

### P2-0 — the Linux-dl route (`docs/linux-abi.md` §W5)

The native route (`LDHOBBY.SO`) is landed; it is deliberately not the Linux route: the
compat track parses without `ELF_PARSE_ALLOW_INTERP`, so a glibc binary is refused with
`ELF_ERR_INTERP` and an empty path (the message says "is not the native /LDHOBBY.SO").
The first rung is to make that refusal a **named fact**: capture the interpreter path on the
compat track, resolve `ld-linux-x86-64.so.2` from the image, map its image, and print its
`DT_NEEDED` closure — or name the first dependency that is missing. Running glibc is later.

- **GB-196** (staged, Ready) — compat-track `PT_INTERP` capture + `ld-linux` image map +
  `DT_NEEDED` report. Artifact: `src/kernel/elf_loader.c` (`el_admit_exe`/`elf_loader_load`)
  and the compat flag path. Falsifier: a glibc dynamic hello prints
  `PT_INTERP /lib64/ld-linux-x86-64.so.2` and the loader's report names the first missing
  DSO; the native `LDHOBBY.SO` path still runs HDYN.BIN unchanged (regression clause).
  One gate run.
- Next rungs (not minted; each needs GB-196's output): map the closure; enter `ld.so`'s
  entry; a glibc hello prints. The ladder is the milestone, not one wt-L.

### P2-1 — the 3D surface (RTS/DSP)

The feasibility doc names a **to-open** GL gap ticket; before writing a shim, answer whether
the graded VM can expose a 3D device at all. On the coordinator host
`qemu-system-x86_64 -device virtio-gpu-gl-pci,help` reports the device absent, so the answer
may be "no virgl here" — which is itself the measured result that gates the epics.

- **GB-197** (staged, Ready) — 3D-device availability + GL surface gap: a probe script that
  boots the two device models (`virtio-gpu-device`, `virtio-gpu-gl-pci`) and records which
  one the host/QEMU supports and what capset the guest sees; if no 3D device exists, the
  card writes `docs/gl2-surface.md`'s "blocked on device" verdict instead of a shim.
  Artifact: `tools/t4/virgl_probe.py` + `docs/gl2-surface.md`. Falsifier: the device line
  on this host (`not found`) versus a host that has it; a probe that reports "supported"
  without the QEMU line is RED. One gate run, both outcomes are results.

### P2-2 — Mono (RimWorld)

Only after P2-0: Mono's runtime is a glibc dynamic consumer (`libc`, `libdl`, `libpthread`).
The staging is a milestone checkpoint with three child cards:
1. Mono runtime's dynamic dependency audit against the P2-0 loader (a script + the list).
2. An interpreter-only CLR running a trivial assembly calling `printf` (Mono `--interp`).
3. The RimWorld assembly load + its OpenTK/GL calls (needs P2-1).
No Mono card is minted before P2-0 lands; the dependency audit is the first falsifiable step.

## 4. What this roadmap does NOT claim

- It does not claim RimWorld/DSP/TA run this month. The feasibility doc's verdict stands.
- It does not stage the epics into Ready. They stay in Icebox until their entry criteria are
  met; GB-196/GB-197 are the two concrete tickets this plan mints now.
- It does not assume virgl works or that Mesa can be ported in one card. GB-197 exists to
  make that answer measured, including the answer "no 3D device on this VM".

## 5. Ordered backlog this plan creates

| card | epic | weight | first measurable result |
|---|---|---|---|
| GB-196 compat `PT_INTERP` + `ld-linux` image map + `DT_NEEDED` report | MONO/RTS/VULKAN | M | a glibc binary names its interpreter and the first missing DSO instead of a generic refusal |
| GB-197 3D-device availability + GL surface gap | VULKAN/RTS | S | a 3D capset is present or absent, with the QEMU device line quoted |

Both are Ready; both name a code artifact and a falsifier. The next three rungs (closure
mapping, GL surface, Mono dependency audit) are staged here and will be minted only after
their predecessor's measured result, per §4.3.
