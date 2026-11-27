# SWARM-REPORT-WP-RPCPUB — publishing the RPC block (publication half)

Branch `nv/wp-rpcpub`, cut from `nv/integ-swarm` @ `2fc2d17c`. Card
`t_f5bfc288`: make the nv_rpc transport's 129-page block real and peer-visible.
This report covers **publication only** — no message is queued and no reply is
claimed.

Files: `src/include/nv_rpc.h`, `src/kernel/nv_rpc.c`, `src/kernel/nv_rpc_test.c`,
`src/include/nv_gsp_glue.h`, `src/kernel/nv_gsp.c`,
`src/kernel/nv_gsp_glue_test.c`, `src/include/nv_dma.h`, `src/kernel/nv_dma.c`,
`src/kernel/nv_dma_test.c`, `docs/nv-gsp-rpc-contract.md`.

## What was wired

The 129-page block existed and its PTEs were built from `nv_dma_bus()`, but
firmware was never told where the block was: no `GSP_ARGUMENTS_CACHED` page, no
libos-args table, no mailbox programming. A running RM would have read a zero
status header forever.

1. **`GSP_ARGUMENTS_CACHED`** (`nv_rpc_args_build`/`nv_rpc_args_read`): magic
   `0x20505347`, size `0xC8`, `mq.sharedMemPhysAddr` = the block's
   `nv_dma_bus()`, cmd `0x1000`, stat `0x41000`, element geometry,
   `pageTableEntryCount = 129`. Every offset is pinned by T-25 against the
   contract's table.
2. **libos-args table** (`nv_rpc_libos_build`): LOGINIT, LOGINTR, LOGRM (one
   real 4 KiB page each) then RMARGS -> the argument page, with
   `id8`/pa/size/kind/loc. T-26 pins it.
3. **`MAILBOX0/1`** = the table's `nv_dma_bus()`, low:high. Exactly two
   register writes; the status header is only ever read.
4. **One `[NV-RPC] stage=publish` row** (below) carrying the block base, the
   page count, and the address the peer will actually walk, read back out of
   the argument page's own bytes. `match` is an assertion: a wrong address
   prints the row and then fails the publish with `-EIO`, so a green log
   cannot coexist with a block nobody can walk.
5. **`nv_dma_bus_of_run()`**: the same accessor for frames the caller already
   owns (firmware blobs from `frame_alloc_contig`). It returns a value, not a
   map, because `nv_dma_free()` must never release borrowed frames. Used for
   the radix3 tree's image address, the tree's own root, the metadata block's
   bootloader/signature pointers, and the SEC2 metadata mailbox.
6. **SEC2 metadata**: `struct sec2_img.meta_phys` -> `meta_bus`; the function
   now takes the `struct nv_dma_map *`, so the CPU write uses `.phys` and the
   mailbox write uses `nv_dma_bus()`. The old code passed a bus address into a
   parameter named `phys` and did `phys_to_virt()` on it.

Verbatim row, from the unit tier (`evidence/nv/rpcpub-unit-green.log`):

```
[NV-RPC] stage=publish base=0x0000000209480000 pages=129 programmed=0x0000000209480000 args=0x0000000209501000 libos=0x0000000209502000 mailbox_read=0x0000000000000000 match=1
```

`mailbox_read` is 0 because the unit tier's fake device returns 0 on read; the
contract's MEASURED rows say the real words read 0 by design, so nothing gates
on it. `programmed` is the `mq.sharedMemPhysAddr` read out of the page.

## Gates (exact lines, on the committed tree)

```
$ SKIP_KVM=1 TOOLS_SKIP=1 BROWSER_SKIP=1 UNIT_TESTS_TIMEOUT=1800 ./run_unit_tests_intel.sh
Tests run:    610
Tests failed: 0
UNIT TESTS PASSED
```

- The nv_rpc suite banner is `[NV-RPC] unit suite (spec section 7: T-01..T-24,
  contract T-25/26)`, and all 26 cases print `Running test_t…` — the 24
  synthetic-queue cases are unchanged and green.

## A test that can fail — and was watched failing

Mutation 1 — delete the translation refusal in `nv_dma_bus_of_run()` (return
`phys`): `ASSERTION FAILED: nv_dma_bus_of_run(phys, NV_DMA_PAGE) ==
NV_DMA_BUS_INVALID at src/kernel/nv_dma_test.c:376`; 4 failed
(`evidence/nv/rpcpub-mut1-dma-bus.log`). The helper cannot be a
`return phys` in disguise.

Mutation 2 — make `nv_rpc_args_build()` store `block_bus + 0x1000` (and its own
read-back agree with it): `ASSERTION FAILED: get64(arg_page + 0x18) == TEST_BUS
at src/kernel/nv_rpc_test.c:1295` and `EXPECT_EQ FAILED: rc == -EAGAIN at
src/kernel/nv_gsp_glue_test.c:377`; 2 failed
(`evidence/nv/rpcpub-mut2-args-address.log`). The published address is checked
by reading the page, not by trusting the variable that wrote it.

## The address rule — what the greps show

```
$ grep -nE 'nv_wr32\([^;]*(\bphys\b|_phys)' src/kernel/nv_gsp.c | grep -v phys_to_virt
   (no matches: every address register is programmed with a *_bus value)
$ grep -nE 'sysmem_addr_[a-z0-9_]+ *= *[^;]*(\.phys|_phys)' src/kernel/nv_gsp.c
   (no matches; the one remaining mention is a comment)
$ grep -nE '\b(src_phys|dst_phys|meta_phys|img_phys)\b' src/kernel/nv_gsp.c
   (no matches)
$ grep -n 'nv_radix3_build(' src/kernel/nv_gsp.c
5262:    trc = nv_radix3_build(img_bus, elf.fwimage.size, &root, &tpages);
```

`phys_to_virt()` remains everywhere it should: those are CPU accesses to the
frames, not device addresses. The falcon XFER helpers take
`falcon_dma_block(…, fwsec_bus|base_bus, …)`, `falcon_dma_store_block(…,
scratch_bus, …)` and `nv_store_block(…, landing_bus, …)`; a refusal skips the
DMA (on the SEC2 booter path it falls back to PIO, which needs no address)
instead of programming `0xFFFF…`.

**Replaced, and noted here because the commit message requires it:**
`loader.phys + 0x6C` and `img_phys + sig_off` (metadata descriptors), the
radix3 tree's image address and its own root, `SEC2 MAILBOX0/1 <- meta_phys`,
the four `GSP_ARGUMENTS_CACHED`/libos/mailbox addresses, and every falcon XFER
address register in the FWSEC/DMEM probe machinery. Follow-up card
`t_96ac9709` was created for that last group before it was folded into this
branch; it is superseded and carries a comment saying so.

## What is NOT verified

- **No GPU on this host.** The unit tier proves image construction, address
  provenance and the refusal paths; it cannot prove that GSP-RM walks the
  table. The row above is from the synthetic device, not a live card.
- **No message is queued and no reply is expected.** Pre-queuing 72/73 and the
  round trip belong to the sibling cards.
- **The mailbox handshake is not acknowledged by firmware** (the words read 0
  by design); nothing gates on them.
- The three log regions are allocated and named but never written by us.
- The peer status header is untouched by design; `zero=8/8` after publishing is
  the evidence that publishing did not counterfeit RM's first write.

## Next step

On the live path: flash this branch and look for the `[NV-RPC] stage=publish`
row; then watch for the peer's status header to become valid (`stage=link
linked=1`). If the header stays zero after RM is running, the argument page's
`magic`/mailbox address is the first thing to compare against this row.
