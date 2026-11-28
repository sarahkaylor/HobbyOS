#!/usr/bin/env python3
"""check_tiera.py -- the no-boot, no-GPU gate for the six Tier A rows.

Card t_9630728b.  docs/gb202-vendor-diff.md section 5 Tier A names the vendor
file:line, ours, the change and the unit proof for each row.  This script is
that proof made executable: every assertion is against source text in this
tree, and every row also names the unit case that exercises the helper.

Run from the repo root (tools/nv/check_tiera.py) or from a scratch dir that
holds the worktree as ./tree.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.join(HERE, "tree")
if not os.path.isdir(os.path.join(TREE, "src")):
    TREE = os.path.abspath(os.path.join(HERE, "..", ".."))
GSP = open(os.path.join(TREE, "src/kernel/nv_gsp.c")).read()
COT = open(os.path.join(TREE, "src/kernel/nv_fsp_cot.c")).read()
TEST = open(os.path.join(TREE, "src/kernel/nv_gsp_glue_test.c")).read()
GLUE = open(os.path.join(TREE, "src/include/nv_gsp_glue.h")).read()

fails = []
n = [0]


def check(ok, what):
    n[0] += 1
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        fails.append(what)


# ---- A3: FALCON_OS after lockdown release ----------------------------------
check("#define NV_GSP_FALCON_OS              0x080u" in GSP,
      "A3 FALCON_OS is the falcon word at +0x080")
check("nv_wr32(d, NV_FALCON_GSP_BASE + NV_GSP_FALCON_OS, app_version);" in GSP,
      "A3 the writer stores appVersion into FALCON_OS")
check("nv_rd32(d, NV_FALCON_GSP_BASE + NV_GSP_FALCON_OS)" in GSP,
      "A3 the same word is read back")
check("nv_gsp_write_falcon_os(d, NV_GSP_GB202_BOOT_APP_VERSION)" in COT,
      "A3 the FSP path calls it at lockdown release")
check("#define NV_GSP_GB202_BOOT_APP_VERSION 0u" in GLUE and
      "580.178.04+0x1C" in GSP,
      "A3 the value is 580.178.04's descriptor appVersion, with provenance")

# ---- A4: the one-shot status-queue link ------------------------------------
check("int rx_linked;" in GLUE, "A4 the link latch is per transport")
check("if (r->rx_linked) {" in GSP and
      GSP.index("if (r->rx_linked) {") <
      GSP.index("nv_rpc_link_apply(&r->q", GSP.index("if (r->rx_linked) {")),
      "A4 a second call returns before it can rewrite the read pointer")
check("nv_gsp_rpc_peer_link(d)" in COT,
      "A4 the FSP path links before it waits for RM")
check("nv_gsp_rpc_peer_link_into(&g_gsp_rpc, d)" in GSP,
      "A4 the boot-path entry point is the same function the test drives")

# ---- A5: GPU time before the COT -------------------------------------------
check("#define NV_PGC6_SCI_SEC_TIMER_TIME_0    0x00118F54u" in GSP and
      "#define NV_PGC6_SCI_SEC_TIMER_TIME_1    0x00118F58u" in GSP,
      "A5 secure-timer pair is the vendor's GC6 offsets")
check("#define NV_PGC6_SCI_SYS_TIMER_OFFSET_0  0x00118DF4u" in GSP and
      "#define NV_PGC6_SCI_SYS_TIMER_OFFSET_1  0x00118DF8u" in GSP,
      "A5 system-timer-offset pair is the vendor's GC6 offsets")
check("nv_wr32(d, NV_PGC6_SCI_SYS_TIMER_OFFSET_1, (uint32_t)(off >> 32));" in GSP and
      "NV_PGC6_SYS_TIMER_OFFSET_UPDATE" in GSP,
      "A5 offset is written hi then lo|UPDATE, as tmrSetCurrentTime_GH100 does")
check("ucode >= RM_FSP_BUG_3957833_FIX_VERSION" in GSP,
      "A5 the write is gated on the FSP bug-3957833 fix (0x44C), as the vendor gates it")
check(COT.index("nv_gsp_set_gpu_time(d)") <
      COT.rindex('cot_roundtrip(d, base, "cot"'),
      "A5 GPU time is set before the COT is sent")

# ---- A7: the two pre-boot RPCs ---------------------------------------------
check("NV_RPC_FN_SET_SYSTEM_INFO" in GSP and "NV_RPC_FN_SET_REGISTRY" in GSP,
      "A7 functions 72 and 73 are both queued")
check("#define RM_SYSTEM_INFO_BYTES        888u" in GSP,
      "A7 SET_SYSTEM_INFO payload is the vendor's sizeof(GspSystemInfo) = 888")
check("nv_rpc_kick" not in GSP[GSP.index("int nv_gsp_rpc_queue_async_init"):
                              GSP.index("int nv_gsp_rpc_async_init")],
      "A7 the two messages are committed WITHOUT a doorbell")
check(COT.index("nv_gsp_rpc_async_init(d)") <
      COT.rindex('cot_roundtrip(d, base, "cot"'),
      "A7 the queue is filled before the COT, i.e. before GSP-RM starts")

# ---- A9: the pre-COT mailbox write is gone ---------------------------------
# Scoped to nv_fsp_rm_boot(), the path D13 cites (the older /COT.CFG rung is a
# different instrument and is not this row); nv_gsp.c's SEC2 stage writes the
# pair for its own handshake and is not the FSP path either.
rm_boot = COT[COT.index("int nv_fsp_rm_boot(struct nv_dev *d) {"):
              COT.index("#else /* the COT probe is x86-64 hardware code")]
check("nv_wr32(d, NV_GSP_BASE + NV_GSP_MAILBOX0" not in rm_boot and
      "nv_wr32(d, NV_GSP_BASE + NV_GSP_MAILBOX1" not in rm_boot,
      "A9 no pre-COT write to GSP MAILBOX0/1 on the FSP RM path")
check('p("gsp mailbox pre-cot args=");' in COT,
      "A9 the read-back row is kept")
check('pkv(" m0", (uint64_t)m0);' in COT and 'pkv(" m1", (uint64_t)m1);' in COT and
      'pki(" wrote", 0);' in COT,
      "A9 the row says both what the pair holds and that this host wrote nothing")

# ---- A10: the second FRTS alias write is gone ------------------------------
check("nv_wr32(d, NV_PBUS_ZB_FRTS_ADDR_LO32" not in COT and
      "nv_wr32(d, NV_PBUS_ZB_FRTS_ADDR_HI32" not in COT and
      "nv_wr32(d, NV_PBUS_ZB_FRTS_CONFIG" not in COT,
      "A10 only 0x14F4/8/C is programmed; the 0x4F4/8/C alias is not written")
check('pkv(" gb202_rb_lo", nv_rd32(d, NV_PBUS_ZB_FRTS_ADDR_LO32));' in COT and
      'pkv(" gb202_rb_hi", nv_rd32(d, NV_PBUS_ZB_FRTS_ADDR_HI32));' in COT and
      'pkv(" gb202_rb_cfg", nv_rd32(d, NV_PBUS_ZB_FRTS_CONFIG));' in COT,
      "A10 the alias read-backs are kept")
check('pki(" wrote_zb", 0);' in COT,
      "A10 the row says the alias was not written")

# ---- the unit cases that exercise each helper ------------------------------
for case in ("test_nv_gsp_falcon_os_write_read_back",
             "test_nv_gsp_peer_link_is_one_shot",
             "test_nv_gsp_time_offset_conversion",
             "test_nv_gsp_time_write_gated_on_fsp_version",
             "test_nv_gsp_async_init_queues_two_rpcs"):
    check(("TEST(%s)" % case) in TEST,
          "unit case %s exists" % case)
check(all(("  %s();" % c) in TEST for c in
          ("test_nv_gsp_falcon_os_write_read_back",
           "test_nv_gsp_peer_link_is_one_shot",
           "test_nv_gsp_time_offset_conversion",
           "test_nv_gsp_time_write_gated_on_fsp_version",
           "test_nv_gsp_async_init_queues_two_rpcs")),
      "all five Tier A cases are registered in the suite")

print()
print("assertions %d failures %d" % (n[0], len(fails)))
for f in fails:
    print("  FAILED: " + f)
sys.exit(1 if fails else 0)
