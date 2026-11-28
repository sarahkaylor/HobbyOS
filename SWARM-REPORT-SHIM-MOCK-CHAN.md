# SWARM-REPORT: the channel object from the mock RM — card t_17ca89e5

Card: **t_17ca89e5**, "Boot GB202 and allocate a channel object from GSP-RM".
Re-scoped by the owner on 2026-11-28: **the mock RM is acceptance for this
object; the firmware version of the claim stays parked on the GB202 ACR wall**
(`t_a769beaa`, revive condition R2). **Zero boots**: VM 300 / 313 / 314 / 315
were not started.

Tag on every claim: **MEASURED** = read out of a log or a live machine in this
run, with the file and line; **SOURCE** = read out of this tree's source, with
`file:line`; **INFERRED** = a conclusion drawn from those two, labelled as
such.

---

## 0. The hypothesis, answered

Original hypothesis: *"GSP-RM on GB202 allocates a channel object for our
driver, and the handle comes from firmware, not the mock RM."*

**Falsified as written, and deliberately not attempted.** No boot was run
(owner instruction, 2026-11-28), and the GB202 corpus has no GSP-RM peer to
ask: the wall card's own ladder ends at `MAILBOX0 = 0x0B` with `linked=0`,
`initdone=0`, `statq_written=0` in every boot (docs/gb202-rm-objects.md §0,
nine boots, unchanged by the later Tier A boots `t_97b8a05c`).

What this card delivers instead is a **mock** object: a channel handle minted
by `nv_rm_mock` (SOURCE: `src/kernel/nv_rm_mock.c:455-493`) through the
`/dev/nvshim` ABI (SOURCE: `src/kernel/nv_shim.c:1316-1362`). It is **not**,
and must not be quoted as, "GSP-RM allocated", a firmware handle, or L3
evidence. Every row quoted below carries `mock=1` in its own text.

---

## 1. Part identity — read-only, nothing booted

MEASURED on r1, 2026-11-28 05:49:14Z (read-only `qm config` / `lspci`,
no VM started):

```
cores: 8
hostpci0: 0000:41:00,pcie=1
memory: 16384
41:00.0 VGA compatible controller [0300]: NVIDIA Corporation GB202 [GeForce RTX 5090] [10de:2b85] (rev a1)
```

VM 300, for contrast, still passes through `0000:01:00` (the AD102):
`hostpci0: 0000:01:00,pcie=1`. **The mock run below never touched either
card** — it ran on the unit-tier guest (x86-64 TCG, `QEMU_ARGS:` empty, no PCI
passthrough), which is why the handle is a mock object and not a GB202 one.

---

## 2. What the shim could not express, and the extension it got

Before this card the shim printed one row per channel allocation:

```
[NVSHIM] channel ch=1 engine=1 pushbuf=0x209586000
```

It named the handle but **not the RM behind it**, and it did not separate the
request from the reply. A reader could not tell a mock handle from a firmware
one, and there was no row to quote as "the request" at all — the card's own
acceptance shape (request row, reply row, handle) was inexpressible. So the
shim was extended (SOURCE, `src/kernel/nv_shim.c`):

* `channel req` row before `rm()->channel_create()` (SOURCE: `nv_shim.c:1316`):
  `mock=1`, the `ioc=` that asked, the VM id, the push-buffer bus address and
  its size — the four arguments the RM is handed.
* `channel rsp` row after it (SOURCE: `nv_shim.c:1358`): `mock=1`, the `ch=`
  handle and `engine=` the RM returned. A failed `channel_create` prints the
  same row with `err=` instead of silence (SOURCE: `nv_shim.c:1342`).
* The DRM face gets the same pair (`drm channel req` / `drm channel rsp`,
  SOURCE: `nv_shim.c:3291`, `3303`, `3343`) and its `drm channel_free` row now
  says `mock=1` too (SOURCE: `nv_shim.c:3378`).
* Row budget `r_ch` 8 -> 24 (SOURCE: `nv_shim.c:406`): the tier makes four
  N-face allocations and each now prints two rows, so 8 was exactly full and
  the next suite's rows would have been suppressed silently.

No behaviour change: same RM calls, same handles, same errnos. The change is
48 insertions / 10 deletions in one file.

---

## 3. The rows, quoted verbatim

Both tiers of this checkout ran the change; the quoted pair is from the intel
tier's console log, and the ARM tier printed the same rows in the same order
(its lines are cited for each quote).

### 3.1 The channel request row

`evidence/shim-mock-chan/unit-tier-intel-chan.log:2350` (MEASURED), in
`test_nvshim_probe`, immediately after the device row that already says
`mock=1`:

```
[NVSHIM] channel req mock=1 ioc=channel_alloc vm=1 pushbuf=0x209686000 size=0x10000
```

ARM, same row shape: `unit-tier-arm-chan.log:1637`
`[NVSHIM] channel req mock=1 ioc=channel_alloc vm=1 pushbuf=0x8ef46000 size=0x10000`.

### 3.2 The channel response row — the object handle

`unit-tier-intel-chan.log:2351` (MEASURED):

```
[NVSHIM] channel rsp mock=1 ch=1 engine=1
```

ARM, `unit-tier-arm-chan.log:1638`: `[NVSHIM] channel rsp mock=1 ch=1 engine=1`.

A later suite allocates a second channel in the same boot and the mock's
monotonic counter names it `2` — `unit-tier-intel-chan.log:2391-2392`
(MEASURED):

```
[NVSHIM] channel req mock=1 ioc=channel_alloc vm=1 pushbuf=0x209685000 size=0x10000
[NVSHIM] channel rsp mock=1 ch=2 engine=1
```

### 3.3 The DRM face mints the same kind of object

`unit-tier-intel-chan.log:2426-2427` and `:2440` (MEASURED) — the
`DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC` path allocates its own push buffer and gets
a mock channel back, then frees it:

```
[NVSHIM] drm channel req mock=1 ioc=drm_channel_alloc vm=1 pushbuf=0x209696000 size=0x10000
[NVSHIM] drm channel rsp mock=1 ch=1 pushbuf=0x209696000 size=0x10000
[NVSHIM] drm channel_free mock=1 ch=1
```

ARM, the same path: `unit-tier-arm-chan.log:1713-1714` (MEASURED):

```
[NVSHIM] drm channel req mock=1 ioc=drm_channel_alloc vm=1 pushbuf=0x8ef56000 size=0x10000
[NVSHIM] drm channel rsp mock=1 ch=1 pushbuf=0x8ef56000 size=0x10000
```

### 3.4 The provenance row that makes the `mock=1` mean something

`unit-tier-intel-chan.log:2341` (MEASURED):

```
[NVSHIM] device dev=1 chipset=0x192 pci=0x2684 mock=1 feats=0x1ff
```

`chipset=0x192 pci=0x2684` is the mock's **declared** AD102 identity, not a
card it read — the same suite prints
`[NVSHIM] getparam refused param=14 reason=mock_has_no_card` on the next line
(`:2342`). A `mock=1` device is the only thing this node can hand out.

---

## 4. The ids used — quoted as they appear

There is **no GSP-RM message id** on this path, and saying otherwise would be
the fabricated reply the card forbids: `NV_VGPU_MSG_*` appears exactly once in
this tree, in a comment (`src/include/nv_rm.h:70`), and nothing sends one
(SOURCE). The mock has no message protocol at all — no queue, no packet, no
console (SOURCE: `src/kernel/nv_rm_mock.c`, no output call in the file).

The ids that *are* on the path, quoted from the headers as written:

* N-face: `src/include/nv_shim.h:277`
  `#define NVSHIM_IOC_CHANNEL_ALLOC NVSHIM_IOWR(NVSHIM_IOC_MAGIC, 8, struct nvshim_channel)`
  with `#define NVSHIM_IOC_MAGIC 'N'` (`nv_shim.h:95`). This is the request the
  rows above name as `ioc=channel_alloc`.
* D face: `src/include/nv_drm_uapi.h:545`
  `#define NVSHIM_DRM_NOUVEAU_CHANNEL_ALLOC     0x02`, used by
  `DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC` (`nv_drm_uapi.h:574`), named in the rows as
  `ioc=drm_channel_alloc`.
* The object id itself: the `ch=` field of the reply row, which the RM
  allocated — not the client (SOURCE: `nv_rm_mock.c:484`,
  `c->id = (uint32_t)++g_mock.next_ch;`).

---

## 5. Where the handle comes from

SOURCE, `src/kernel/nv_rm_mock.c:455-493`: `mock_channel_create()` validates
the (dev, vm, pushbuf, size) tuple against the mock's own tables, takes the
first free channel slot, and stamps `c->id = (uint32_t)++g_mock.next_ch`
(`:484`); the engine it reports is `1` = `NVSHIM_ENGINE_GR`
(`:489`, `src/include/nv_shim.h:229`). The interface is named `"mock"`
(`nv_rm_mock.c:708`), and `nv_shim.c:504` binds the whole node to it
(`static const struct nv_rm_if *rm(void) { return nv_rm_mock_if(); }`). So the
`ch=1` in §3.2 is a **mock** handle: an index in `g_mock.ch[]`, not a GSP-RM
object.

---

## 6. The tiers that produced the rows

Tested tree: branch `nv/shim-mock-chan2` at `fd69e681`, on `nv/integ-swarm`
`f7c2a714` (the fold parent, which carries the owner's §5.1 reference-note
commit), one source file changed.

* intel (x86-64, TCG): `TOOLS_SKIP=1 SKIP_KVM=1 ./run_unit_tests_intel.sh`
  — banner `Running unit tests with QEMU_ARGS: `,
  **`Tests run: 621` / `Tests failed: 0` / `UNIT TESTS PASSED`**
  (`unit-tier-intel-chan.log:3205-3207`, MEASURED).
* ARM: `TOOLS_SKIP=1 ./run_unit_tests.sh` —
  **`Tests run: 486` / `Tests failed: 0` / `UNIT TESTS PASSED`**
  (`unit-tier-arm-chan.log:2463-2465`, MEASURED).

Both logs are the scripts' own console output, kept verbatim in
`evidence/shim-mock-chan/`. The channel rows appear in both, so the row change
is not an x86-only path (`nv_shim.c` has no `__x86_64__` guard by design).
The branch tip differs from the tested `fd69e681` by this report file only
(`git diff --stat fd69e681 HEAD` = `SWARM-REPORT-SHIM-MOCK-CHAN.md`).

---

## 7. Closing state — and the proof that nothing booted

MEASURED on r1 at 05:49:14Z and again after the tiers, read-only:

```
status: stopped          # qm status 313
status: stopped          # qm status 300
0                        # pgrep -c qemu-system-x86
```

`pgrep -af qemu-system-x86` lists only the ssh helper's own `bash -c` line
containing the pattern; no QEMU process exists. The guests' files were not
touched by this run — their mtimes are still the previous card's boot:

```
/root/hobbyos5090/serial.log 2026-11-27 23:39:01.536392179 -0600 74995
/root/hobbyos5090/disk.img   2026-11-27 23:39:01.541392178 -0600 1073741824
```

i.e. 2026-11-28 05:39:01Z, the `rm68` boot of card `t_97b8a05c` (started
05:38:44Z). Nothing in this card wrote them, so **zero boots** is a
measurement, not a claim.

---

## 8. What this does and does not prove

**Does:** the `/dev/nvshim` ABI can mint a channel object against the mock RM,
the shim's own rows now say so (`mock=1`) on both faces, the handle comes from
the RM's allocator and not the client, both tiers are green, and the rows are
byte-quoted from the logs.

**Does not:** it says nothing about GB202, GSP-RM, firmware, WPR2 or L3. The
firmware claim stays exactly where the wall card left it — `MAILBOX0 = 0x0B`,
no peer — and this card is **not** evidence for it. The next card in this
serialised spine (VA space, then engine class) builds on the same shim rows and
inherits the same rule: every row and every sentence says `mock`.
