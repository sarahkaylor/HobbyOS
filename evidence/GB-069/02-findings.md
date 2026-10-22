# GB-069 — measured state, and an instrument correction I owe

Revision: `83ff46b` family worktree (`hermes-289f3b84`), swarm-farm-2, intel T3 wave with disk I/O
(NVMe BAR0 physically `0x81000000`, mapped at VA `0x1FC000000`; FAT-16 volume mounted; 60 programs).

## 1. The fault does not reproduce on this wave

```
kernel-mode FATAL faults: 0
vector=14 in the BAR window: 0
```

The run exercised the NVMe path (`Found NVMe controller on PCI bus!` → `NVMe driver initialized
successfully!`) and the FAT-16 disk path, and never took a `#PF` at `cr2 ∈ [0x1FC000000, 0x1FDFFFFF)`.
So the *negative* half of the DoD holds on this configuration — one run, one configuration, no claim
beyond that.

## 2. Instrument correction — my first reading was wrong, and I caught it before publishing

I added a check at the NVMe doorbell write comparing `cpu_pd7[480]` with the value installed at init
(`0x8100009B`). It fired twice:

```
[GB-069-PD7] slot480=0x00000000810000FB init=0x000000008100009B
```

I nearly reported that as "another mapper rewrote the hardcoded PD slot" — mechanism (1) confirmed.
**It is not.** `0xFB ^ 0x9B = 0x60`, i.e. bits 5 and 6 only: the **Accessed** and **Dirty** bits the CPU
sets itself on first use. Base (`0x81000000`), PS, P and RW are identical. The entry was never rewritten.

That is the same instrument trap the program has recorded before: a test whose failure mode is the
hardware's normal bookkeeping, read as a finding. The instrument now masks `0x60` before comparing, and
a strict equality check is explicitly called out in the code comment so the next reader does not repeat it.

## 3. What that leaves

- Mechanism (1) — "the PD entry is replaced by another mapper" — is **not** observed in this run: the
  slot's identity bits survived every doorbell write.
- Mechanism (2) — wrong CR3 — and (3) — protection — cannot be tested without a fault site, and the
  fault does not fire here.
- The DoD's second clause ("the mechanism named with the fault-site evidence") therefore remains
  **unmet**, not because the analysis is incomplete but because there is no fault to read: `err=`,
  `cr3=`, `pd480=` are already printed by the kernel-mode FATAL path (`trap.c:2695-2712`), so the
  instrument is in place and simply has nothing to report on this wave.

## 4. Honest status

The ticket is **not** closable by this pass. To finish it, a reproducer is needed — the historical
capture was load-dependent, so the next attempt should vary the load (a wave that writes to disk while
another subsystem maps the low VA range), not the driver. Evidence in this worktree:
`evidence/GB-069/01-t3-intel.log` (tier output) and `obj/t3_intel.log` (serial).
