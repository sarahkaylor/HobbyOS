# R1 boot: `ec5bcd11` is the fourth measured negative — and the transport is measured in the same boot

Card `t_9bf9bc0c` (RPC link: first round trip against a live GSP-RM), 2026-11-27.

**Boots spent: 1** (`r1newa`, the one authorized boot). No second boot, no synthetic
reply, no claim of R1 and no claim of a round trip.

## What was done before the boot

| step | result |
|---|---|
| merge `nv/wp-rpc-final` (`c58b1295`) into `nv/wp-r1-rig` (`5b64dc0d`) | branch `nv/wp-r1-merged`, merge `e8feac31`, docs `4e40feae` — see §8 of `SURVEY-R1.md` for the two conflicted files and why both resolve to the rig's side |
| build | `make ARCH=intel MODE=test LO_ON_DISK= TOOLS_SKIP=1 -j8 disk.img`, exit 0, no warning in `nv_gsp.c`/`nv_rpc.c` |
| unit tier | `TOOLS_SKIP=1 SKIP_KVM=1 ./run_unit_tests_intel.sh` → **600 run / 0 failed**, `UNIT TESTS PASSED` |
| boot image rebuilt after the tier (the tier build is `MODE=unit_tests`) | exit 0; `hobbyos.elf` sha256 `f6146d47…` |
| staging preflight, `count=0` | booter `sha256=ec5bcd117e18166c… size=58488`, 4 firmware files staged, `VM 300: stopped`, `VM 312: stopped`, `qemu 0` |

The candidate was staged **by name and hash**, and the check is repeated on the image the
VM actually booted (extracted back out of `disk.img` after the run):

```
$ mcopy -n -i disk.img ::/NVIDIA/GSP/BOOTER.BIN staged-booter.bin
ec5bcd117e18166c1f5e0336b62239457d9048e68b7e8d8a93ea628394724bc5  staged-booter.bin   (58488 B)
```

so the image under test is not the 535 control (which would parse as 55 928 B and
`patch_site=0x7710`).

## The SEC2 answer: `0x002B0009`, at +4.176 ms, `INTR=0x10`

The guest's own parse of the staged container (58488 = 0xE478 bytes, the candidate's
geometry) and the S launch path, verbatim from `evidence/nv/r1newa.log`:

```
[NV-SEC2] image data_off=0x0000000000000378 imem@file=0x0000000000000478 len=0x0000000000008000 dmem@file=0x0000000000008478 len=0x0000000000006000 bootvec=0x0000000000000000 ends@=0x000000000000E478 file_size=0x000000000000E478 verdict=TILES-THE-FILE (IMEM runs into the DMEM image and the image ends exactly at the end of the file)
[NV-SEC2] fuse SEC2 ucode block @0x824140: 0x0000000000000001 … 0x000000000000007F 0x0000000000007FFF 0x0000000000007FFF (one dword per ucode id 1..16)
[NV-SEC2] fuse SEC2 ucode=3 word=0x0000000000000001 -> reg_fuse_version=1 | NVIDIA slot rule idx=num_sigs-1-reg=0 (valid)
[NV-SEC2] sig block@0x000000000000003C slots=2 bytes=0x0000000000000180 fuse@0x00000000008241C8=0x0000000000000001 patchsite=0x0000000000008110 -> dmem+0x0x0000000000000010 fuse_rule_slot=1
[NV-SEC2T] S reload-dma imem=0x0000000000008000 dmem=0x0000000000006000 dmem_img@0x0x0000000000008000 rc=0 dmem_transport=DMA transport=DMA (256-byte-aligned staging buffer)
[NV-SEC2T] S tl dt_ns=0x00000000003B7920 ctl=0x0000000000000000 intr=0x0000000000000000 st4c=0x0000000000000011 sec240=0x0000000000003002 u94=0x0000000000000001 d0=0x0000000000900000 dc=0x00000000008E0437
[NV-SEC2T] S tl dt_ns=0x00000000003C3C00 ctl=0x0000000000000000 intr=0x0000000000000000 st4c=0x0000000001000001 sec240=0x0000000000003002 u94=0x00000000003900FF d0=0x0000000000900000 dc=0x0000000000CE040F
[NV-SEC2T] S tl dt_ns=0x00000000003FB400 ctl=0x0000000000000000 intr=0x0000000000000000 st4c=0x0000000000000001 sec240=0x0000000000003002 u94=0x00000000002B0009 d0=0x0000000000900000 dc=0x00000000008E043F
[NV-SEC2T] S tl dt_ns=0x0000000000417A80 ctl=0x0000000000000010 intr=0x0000000000000010 st4c=0x0000000000000000 sec240=0x0000000000003002 u94=0x00000000002B0009 d0=0x0000000000900000 dc=0x00000000008E043F
[NV-SEC2T] S tl-summary polls=0x0000000000001770 rows=0x0000000000000007 first_ns=0x00000000003B7920 last_ns=0x00000000023CAE60
[NV-SEC2T] S verdict-wait ms=0x0000000000000259 verified=0x000000005EC2A0A0 ctl=0x0000000000000010 halted=1 pc=0x00000000BADF5620 alias130=0x00000000BADF5620 mb0=0x00000000094EA000 mb1=0x0000000000000002 polls=0x000000000438E501 verdict=NOWRITE-HALTED (the core is STOPPED and the metadata still holds the sentinel: a rejection, not a slow boot)
[NV-GSP] stage5 result=-1 (no success reported; the [NV-SEC2] rows are the evidence)
```

| run | blob | `0x840094` | first seen | `INTR` |
|---|---|---|---|---|
| `v1a` | `7d4ef325` (535.113.01) | `0x002B0009` | +4.208 ms | `0x10` EXIT |
| `v2a` | `8b293e19` (570.86.15 → 615.71.10) | `0x002B0009` | +4.163 ms | `0x10` EXIT |
| `v3a` | `3d8a9f22` (555.58 → 565.77) | `0x002B0009` | +4.176 ms | `0x10` EXIT |
| **`r1newa`** | **`ec5bcd11`** (580.196.07 → 620.73.04) | **`0x002B0009`** | **+4.176 ms** | **`0x10` EXIT** |

The candidate is a *new container* (58 488 B, +768 B of app code, its own PROD signature
and payload, patch site 0x8110 vs 0x7710) and the boot ROM's answer is the same word at the
same point of the same path: the AD102 production signature verifies and the comparison
after it fails, exactly as for the three earlier generations. That closes the last untested
artifact on R1's list; it does not move the wall.

### The one caveat, stated rather than buried

The S path patches slot **1** and its own row prints the reference rule's answer, slot 0:
`fuse_rule_slot=1` in the row above, `idx=num_sigs-1-reg=0 (valid)` in the row before it.
So this is a **slot-1** measurement. It is apples-to-apples with the three prior negatives
— `bv1a`/`fz1a`/`eg1a` (the A12 evidence, same tree before this merge) show
`S sig-patched slot=1` with `patchsite=0x7710`: same path, same slot, only the image
differs. Slot 0 was reached in this boot only on the A path, which never starts
(`START-NEVER-VISIBLE`, `u94=0`). A slot-0 S measurement needs a `SEC2_AUTH_BISECT` build
and one more boot; it is the cheapest remaining lever and is **not** funded here. Round 11
did measure both slots on the 5xx images and both returned `0x002B0009`, so nothing
suggests the answer changes — but this card does not claim it.

## The transport half, measured in the same boot

This is what merging `nv/wp-rpc-final` into the rig bought: the 129-page block is
published, the pre-boot 72/73 are queued before the launch, and the peer's status header
is still untouched, so the send path refuses and nothing is sent.

```
[NV-RPC] stage=block bus=0x00000002094EB000 phys=0x00000002094EB000 pages=129 pte0=0x00000002094EB000 pte128=0x000000020956B000 identity=1
[NV-ARG] stage=args bus=0x000000020956C000 phys=0x000000020956C000 magic=0x0000000020505347 size=0x00000000000000C8
[NV-ARG] stage=args mq sharedMemPhysAddr=0x00000002094EB000 pte0=0x00000002094EB000 equal=1
[NV-ARG] stage=args mq cmd=0x0000000000001000 stat=0x0000000000041000 hdr=0x0000000000000030 min=0x0000000000001000 max=0x0000000000010000 halign=0x0000000000000004 ealign=0x000000000000000C pte_count=129
[NV-ARG] stage=args crosscheck=OK (the page in memory describes the block this boot built)
[NV-ARG] stage=libos[3] id=RMARGS.. pa=0x000000020956C000 size=0x0000000000001000 kind=1 loc=1
[NV-ARG] stage=status bus=0x00000002094EB000 off=0x0000000000041000 zero=8/8 w0=0x0000000000000000 w1=0x0000000000000000 w2=0x0000000000000000 w3=0x0000000000000000 w4=0x0000000000000000 w5=0x0000000000000000 w6=0x0000000000000000 w7=0x0000000000000000 (publishing did not counterfeit the peer's first write)
[NV-PRE] stage=system_info fn=72 len=0x2E0 ec=1 wp_after=1 sum=OK
[NV-PRE] stage=registry fn=73 len=0x28 ec=1 wp_after=2 sum=OK
[NV-PRE] stage=doorbell wr=2 pw=0 doorbells=1
[NV-GSP] stage4b (rpc pre-boot 72/73) result=0 (two messages queued, one doorbell)
[NV-RPC] stage=link bus=0x00000002094EB000 linked=0 swapped=1 wr=2 rd=0 prd=0 pw=0 wr_off=0x0000000000001010 pw_off=0x0000000000041010 txseq=2 rxseq=0
[NV-RPC] statq off=0x0000000000041000 nonzero=0/8 w0=0x0000000000000000 w1=0x0000000000000000 w2=0x0000000000000000 w3=0x0000000000000000 w4=0x0000000000000000 w5=0x0000000000000000 w6=0x0000000000000000 w7=0x0000000000000000 (nothing has written the peer header)
[NV-RPC] peer=none (read from block+0x0000000000041000, not from the cached peer struct)
[NV-RPC] stage=canary own_off=0x0000000000001010 before=2 wrote=49 restored=2 peer_off=0x0000000000041010 peer_before=0 peer_during=0 verdict=SEPARATE-WORDS (the peer's word did not follow a store to ours, and is still the zero we built)
[NV-RPC] stage=init skipped: linked=0 (no peer header to answer, so nothing was sent from here and none of the doorbells=1 so far came from here)
```

## Acceptance, item by item

| requirement | state | evidence |
|---|---|---|
| nv_rpc's 129-page block is **published** | **MET** | `pages=129 … identity=1`; args page `magic=0x20505347 size=0xC8`, `sharedMemPhysAddr==pte0 equal=1`, `crosscheck=OK`, `RMARGS..`→args page, MAILBOX0; pre-boot `wr=2, doorbells=1` |
| the **peer's status header moves** (not our own write pointer) | **NOT MET** | `statq nonzero=0/8`; `peer=none` re-read from `block+0x41000`; canary `before=2 wrote=49 restored=2 peer_before=0 peer_during=0 → SEPARATE-WORDS` |
| **one message is answered** — the first round trip | **NOT MET** | `linked=0`, `stage=init skipped`; the send path refuses before any store or doorbell, so no RPC stage rang a doorbell |

There is no peer because SEC2's boot ROM refuses every AD102 booter this project can reach
— now four of them, same word, same timing. That is a capability wall, not a transport
defect, and no synthetic reply was used as evidence.

## Housekeeping and provenance

- `[sec2-lane] r1newa KEPT (trace rows present)`; log `evidence/nv/r1newa.log`
  (3 952 lines, `sec2t_rows=299`, sweep rows present).
- Closing lines: `VM 300: status: stopped`, `VM 312: status: stopped`,
  `qemu processes on r1: 0`.
- Local image sha256 after staging: `856ee2a3…` (1 GiB); `hobbyos.elf` `f6146d47…`;
  branch tip at boot time `4e40feae` on `nv/wp-r1-merged`.
- The boot log is the A12 lineage's plain `serial.log` capture (this lane predates the
  `SEC2-RECORD` provenance receipt): the log was truncated before `qm start`, the rows in
  it are this run's (the staged booter hash above is read from the same `disk.img` the lane
  uploaded), and no other lane booted — `qm status 300` was `stopped` at preflight and
  again at the end.
