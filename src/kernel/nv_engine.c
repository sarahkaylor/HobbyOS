/* ============================================================================
 *  nv_engine: is the GR engine window addressable at all, and does it carry a
 *  topology?  Read-only, one row per stage, no writes -- ever.
 *
 *  Why this file exists (docs/nv-no-rm-path.md, row 2).  On Ada the host-side
 *  GR bring-up in the reference stack is gated twice: ga102_gr_new() returns
 *  -ENODEV while GSP-RM is present, and the ucode load it would run
 *  (ga102_gr_load -> nvkm_acr_lsfw_load_bl_sig_net for FECS/GPCCS) is an ACR
 *  operation whose bootstrap owner is SEC2.  Neither fact says whether the
 *  PGRAPH *window* is host-addressable on this part, and that is the first
 *  question any no-RM path has to answer.  It is also the question the plan's
 *  DECLARED topology (11 GPC / 64 TPC, nv-plan.md section 2.7) has been
 *  waiting on: the same two reads that prove addressability also measure the
 *  topology, if the window answers.
 *
 *  Addresses are taken from the reference driver's own GR code (SOURCE), not
 *  from a guess:
 *    0x409604                  gf100_gr_oneinit: gpc_nr = rd32 & 0x1f
 *    0x500000+i*0x8000+0x2608  gf100_gr_oneinit: tpc_nr[i], via GPC_UNIT(i,0x2608)
 *                              (GPC_UNIT(t,r) = 0x500000 + t*0x8000 + r)
 *    0x400154                  ga102_gr_oneinit_intr (interrupt vector)
 *    0x400160                  ga102_gr_nonstall
 *    0x409614 / 0x41a614       ga102_gr_fecs_reset / ga102_gr_gpccs_reset
 *    0x41bcb4                  ga102_gr_zbc_clear_color
 *
 *  What this probe can and cannot conclude, stated because both failure modes
 *  already cost this project a round (nv-plan.md section 6, rule 1):
 *    - a word that is not a sentinel is *reachability*.  It is not permission to
 *      write, and it is not proof the engine can run: the engine needs FECS
 *      executing, which needs the ACR, which needs SEC2.
 *    - a topology word that reads 0 while its block's control registers answer
 *      data is "zero GPCs reported", not "zero GPCs exist".  The probe refuses
 *      to publish a topology unless a control register in the same block
 *      answered data AND the decoded count is in 1..32.
 * ========================================================================== */
#ifdef __x86_64__

#include "nv.h"
#include "nv_regs.h"
#include "errno.h"

extern void uart_puts(const char *s);
extern void uart_print_hex(uint64_t v);
extern void print_int(int v);

/* The named registers, exported so the unit tier's fake register file can
 * answer exactly these addresses instead of accidentally reading zeros. */
const struct nv_eng_reg nv_eng_probe_regs[NV_ENG_PROBE_REGS] = {
    {"gr_window_first", 0x400000u},  /* window liveness; no nvkm reader */
    {"gr_intr_vector", 0x400154u},   /* ga102_gr_oneinit_intr */
    {"gr_nonstall", 0x400160u},      /* ga102_gr_nonstall */
    {"gr_topology", 0x409604u},      /* gf100_gr_oneinit: gpc_nr */
    {"fecs_reset", 0x409614u},       /* ga102_gr_fecs_reset */
    {"gpccs_reset", 0x41a614u},      /* ga102_gr_gpccs_reset */
    {"zbc_clear_color", 0x41bcb4u},  /* ga102_gr_zbc_clear_color */
};

/* GPC_UNIT(t,r) from the reference GR code.  Bounded to AD102's maximum GPC
 * count (12 on the full die) so a bogus topology word cannot walk the BAR. */
uint32_t nv_eng_gpc_unit(int gpc, uint32_t reg) {
  if (gpc < 0 || gpc >= NV_ENG_MAX_GPC) return 0;
  return 0x500000u + (uint32_t)gpc * 0x8000u + reg;
}

/* Classify one word.  The three sentinels have three meanings and one of them
 * (0xBADF5040) is a *normal* answer on a healthy card, so it is named, not
 * lumped in with "poison". */
static void nv_eng_count(struct nv_eng_report *r, uint32_t v) {
  r->n_regs++;
  if (v == 0xffffffffu) {
    r->n_absent++;
  } else if (v == NV_REG_FENCED) {
    r->n_fenced++;
  } else if (nv_is_badf(v)) {
    r->n_unreadable++;
  } else {
    r->n_data++;
  }
}

static const char *nv_eng_word_kind(uint32_t v) {
  if (v == 0xffffffffu) return "absent";
  if (v == NV_REG_FENCED) return "fenced";
  if (nv_is_badf(v)) return "unreadable";
  return "data";
}

int nv_engine_probe(struct nv_dev *d) {
  if (!d) return -EINVAL;
  struct nv_eng_report *r = &d->eng;
  uint8_t *p = (uint8_t *)r;
  for (unsigned i = 0; i < sizeof *r; i++) p[i] = 0;
  r->gpc_nr = -1;
  r->topo_verdict = NV_TOPO_UNMEASURED;

  /* ---- named registers -------------------------------------------------- */
  uint32_t val[NV_ENG_PROBE_REGS];
  for (int i = 0; i < NV_ENG_PROBE_REGS; i++) {
    val[i] = nv_rd32(d, nv_eng_probe_regs[i].addr);
    nv_eng_count(r, val[i]);
    uart_puts("[NV-ENG] ");
    uart_puts(nv_eng_probe_regs[i].name);
    uart_puts(" 0x");
    uart_print_hex(nv_eng_probe_regs[i].addr);
    uart_puts("=");
    uart_print_hex(val[i]);
    uart_puts(" ");
    uart_puts(nv_eng_word_kind(val[i]));
    uart_puts("\n");
  }

  /* Verdict on the window.  "Addressable" needs a data-answering word; one
   * fenced word and nothing else is a fenced window, which on this part is a
   * privilege answer, not absence (docs/nv-gsp-reference.md section 3.1). */
  if (r->n_data > 0) {
    r->window = NV_ENG_ADDRESSABLE;
  } else if (r->n_fenced > 0) {
    r->window = NV_ENG_FENCED;
  } else if (r->n_unreadable > 0) {
    r->window = NV_ENG_UNREADABLE;
  } else {
    r->window = NV_ENG_ABSENT;
  }
  const char *wname = r->window == NV_ENG_ADDRESSABLE ? "ADDRESSABLE"
                       : r->window == NV_ENG_FENCED    ? "FENCED"
                       : r->window == NV_ENG_UNREADABLE ? "UNREADABLE"
                                                        : "ABSENT";
  uart_puts("[NV-ENG] pgraph window 0x400000..0x5FFFFF regs=");
  print_int(r->n_regs);
  uart_puts(" data=");
  print_int(r->n_data);
  uart_puts(" fenced=");
  print_int(r->n_fenced);
  uart_puts(" unreadable=");
  print_int(r->n_unreadable);
  uart_puts(" absent=");
  print_int(r->n_absent);
  uart_puts(" verdict=");
  uart_puts(wname);
  uart_puts(" note=read-only (reachability is not permission to write)\n");
  if (r->window != NV_ENG_ADDRESSABLE) return 0; /* nothing further to measure */

  /* ---- topology ---------------------------------------------------------
   * gjpc_nr comes from 0x409604's low 5 bits (the reference driver's own
   * decode).  The check that makes the number evidence rather than a guess:
   * a control register in the same block must have answered data, and the
   * count must be in 1..32.  Otherwise "zero GPCs" and "unreadable" are the
   * same row, and this project has already paid for that confusion once. */
  const uint32_t topo = val[3];
  const int control_ok = !nv_is_badf(val[1]) && val[1] != 0xffffffffu;
  int gpc = (int)(topo & 0x1fu);
  uart_puts("[NV-ENG] gr topology 0x409604=");
  uart_print_hex(topo);
  uart_puts(" gpc_nr=");
  print_int(gpc);
  uart_puts(" fbp_or_ltc=");
  print_int((int)((topo >> 16) & 0x1fu));
  uart_puts(" control_ok=");
  print_int(control_ok);
  uart_puts("\n");
  if (topo == NV_REG_FENCED || nv_is_badf(topo) || topo == 0xffffffffu) {
    r->topo_verdict = NV_TOPO_UNMEASURED;
    uart_puts("[NV-ENG] topology verdict=UNMEASURED (the topology word is not "
              "data; the DECLARED model stays declared, nv-plan.md 2.7)\n");
    return 0;
  }
  if (!control_ok || gpc < 1 || gpc > NV_ENG_MAX_GPC) {
    r->topo_verdict = NV_TOPO_IMPLAUSIBLE;
    uart_puts("[NV-ENG] topology verdict=IMPLAUSIBLE (word answered data but no "
              "control register in the block did, or the count is out of range: "
              "NOT a measurement)\n");
    return 0;
  }

  r->gpc_nr = gpc;
  r->topo = topo;
  uart_puts("[NV-ENG] gpc tpc masks (GPC_UNIT(i,0x2608)):");
  for (int i = 0; i < gpc && i < NV_ENG_MAX_GPC; i++) {
    uint32_t m = nv_rd32(d, nv_eng_gpc_unit(i, 0x2608u));
    r->tpc[i] = m;
    if (!nv_is_badf(m) && m != 0xffffffffu) r->tpc_ok |= 1u << i;
    uart_puts(" ");
    uart_print_hex(m);
  }
  uart_puts("\n");
  /* TPC total: popcount of each answered mask, which is how the reference
   * driver sums it (Mesa then derives SMs as tpc * mp_per_tpc). */
  int tpc_total = 0, answered = 0;
  for (int i = 0; i < gpc && i < NV_ENG_MAX_GPC; i++) {
    if (!(r->tpc_ok & (1u << i))) continue;
    uint32_t m = r->tpc[i];
    while (m) {
      tpc_total += (int)(m & 1u);
      m >>= 1;
    }
    answered++;
  }
  r->tpc_nr = tpc_total;
  if (answered == gpc && tpc_total > 0) {
    r->topo_verdict = NV_TOPO_MEASURED;
    uart_puts("[NV-ENG] topology verdict=MEASURED (this replaces the declared "
              "11/64 row for gpc/tpc on the next nvk boot)\n");
  } else {
    r->topo_verdict = NV_TOPO_PARTIAL;
    uart_puts("[NV-ENG] topology verdict=PARTIAL (gpc answered, tpc masks did "
              "not; DECLARED model stays in force)\n");
  }
  uart_puts("[NV-ENG] measured gpc=");
  print_int(r->gpc_nr);
  uart_puts(" tpc=");
  print_int(r->tpc_nr);
  uart_puts(" src=register\n");
  return 0;
}

#endif /* __x86_64__ */
