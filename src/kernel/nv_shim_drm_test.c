/* ============================================================================
 *  nv_shim_drm_test: kernel unit-tier coverage for /dev/nvshim's DRM face
 *  (WP-F round 2: nouveau's real DRM_IOCTL_NOUVEAU_* surface over the mock RM).
 *
 *  Five layers, deliberately separate:
 *
 *  1. The contract.  Struct sizes, a handful of field offsets, and every
 *     request number asserted against a LITERAL 32-bit value computed by hand
 *     from the _IOC encoding -- not against the same macro the header uses, so
 *     an error in the encoding macros cannot hide itself.
 *
 *  2. Size validation.  A known number carrying the wrong struct width is
 *     -EINVAL; an unknown number is -ENOTTY; a known-but-refused one is
 *     -ENOSYS with a name.  Three mistakes, three answers.
 *
 *  3. The probe -- the gate.  A real process, a real address space and a real
 *     file_open("/dev/nvshim") run the sequence a stock nouveau winsys runs:
 *     VERSION, GET_CAP, GETPARAM, GEM_NEW, VM_INIT (with Mesa's exact
 *     {1<<39, 1<<39} arguments), a two-op VM_BIND, CHANNEL_ALLOC, EXEC into a
 *     mapping, and the completion observed through a binary syncobj.
 *
 *  4. Both directions of the fence.  With the mock's kick held, EXEC enqueues
 *     and SYNCOBJ_WAIT must return -ETIME (poll and a millisecond deadline);
 *     after the hold is released, the NEXT EXEC's kick retires both fences and
 *     the same wait returns 0.  A test that only ever saw "signalled" would
 *     pass against a syncobj that is true by construction (PLAN.md §6.5).
 *
 *  5. Ownership and limits: one client cannot touch another's channel, handle
 *     or syncobj; VM_INIT after a BO is -ENOSYS (the uAPI's own rule); a batch
 *     VM_BIND validates every op before applying any.
 *
 *  What passing proves: the ABI's layout, size checking, bookkeeping,
 *  ownership and completion state machine, with no card.  What it does NOT
 *  prove: that a stock nvk gets all the way through -- it also opens with
 *  DRM_IOCTL_VERSION/GET_CAP (served) and then wants NVIF, GEM_PUSHBUF and
 *  PRIME (refused by name).  See SWARM-REPORT-WP-F2.md §5.
 * ========================================================================== */

#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"

#include "errno.h"
#include "file_obj.h"
#include "fs.h"
#include "nv_drm_uapi.h"
#include "nv_rm.h"
#include "nv_shim.h"
#include "process.h"
#include "timer.h"
#include "vm.h"

extern int process_kill(int pid); /* fs_test precedent: not in process.h */

#define OFF(st, fld) ((int)__builtin_offsetof(st, fld))

/* ---- 1. the contract --------------------------------------------------- */

static void test_nvshim_drm_layout(void) {
  uart_puts("  Running test_nvshim_drm_layout...\n");
  tests_run++;

  /* uAPI struct widths, byte for byte.  Each number is the one the uAPI's own
   * layout produces on a 64-bit target; the _IOC encoding in the macros below
   * is what the kernel validates against, so a wrong size here is a winsys-
   * visible break, not a cosmetic one. */
  EXPECT_EQ((int)sizeof(struct nvshim_drm_version), 64);
  EXPECT_EQ((int)sizeof(struct nvshim_drm_cap), 16);
  EXPECT_EQ((int)sizeof(struct nvshim_drm_gem_close), 8);
  EXPECT_EQ((int)sizeof(struct nvshim_drm_syncobj_create), 8);
  EXPECT_EQ((int)sizeof(struct nvshim_drm_syncobj_destroy), 8);
  EXPECT_EQ((int)sizeof(struct nvshim_drm_syncobj_array), 16);
  EXPECT_EQ((int)sizeof(struct nvshim_drm_syncobj_wait), 40);
  EXPECT_EQ((int)sizeof(struct nvshim_drm_getparam), 16);
  EXPECT_EQ((int)sizeof(struct nvshim_drm_gem_info), 40);
  EXPECT_EQ((int)sizeof(struct nvshim_drm_gem_new), 48);
  EXPECT_EQ((int)sizeof(struct nvshim_drm_vm_init), 16);
  EXPECT_EQ((int)sizeof(struct nvshim_drm_vm_bind_op), 40);
  EXPECT_EQ((int)sizeof(struct nvshim_drm_vm_bind), 40);
  EXPECT_EQ((int)sizeof(struct nvshim_drm_sync), 16);
  EXPECT_EQ((int)sizeof(struct nvshim_drm_channel_alloc), 88);
  EXPECT_EQ((int)sizeof(struct nvshim_drm_channel_free), 4);
  EXPECT_EQ((int)sizeof(struct nvshim_drm_exec_push), 16);
  EXPECT_EQ((int)sizeof(struct nvshim_drm_exec), 40);

  /* The offsets a client's generated code depends on (uAPI field order). */
  EXPECT_EQ(OFF(struct nvshim_drm_channel_alloc, subchan), 20);
  EXPECT_EQ(OFF(struct nvshim_drm_channel_alloc, nr_subchan), 84);
  EXPECT_EQ(OFF(struct nvshim_drm_channel_alloc, channel), 8);
  EXPECT_EQ(OFF(struct nvshim_drm_gem_info, offset), 16);
  EXPECT_EQ(OFF(struct nvshim_drm_gem_info, map_handle), 24);
  EXPECT_EQ(OFF(struct nvshim_drm_gem_new, channel_hint), 40);
  EXPECT_EQ(OFF(struct nvshim_drm_vm_bind_op, addr), 16);
  EXPECT_EQ(OFF(struct nvshim_drm_vm_bind_op, range), 32);
  EXPECT_EQ(OFF(struct nvshim_drm_vm_bind, op_ptr), 32);
  EXPECT_EQ(OFF(struct nvshim_drm_sync, timeline_value), 8);
  EXPECT_EQ(OFF(struct nvshim_drm_exec, push_ptr), 32);
  EXPECT_EQ(OFF(struct nvshim_drm_syncobj_wait, first_signaled), 24);
  EXPECT_EQ(OFF(struct nvshim_drm_syncobj_wait, deadline_nsec), 32);
  EXPECT_EQ(OFF(struct nvshim_drm_version, name_len), 16);
  EXPECT_EQ(OFF(struct nvshim_drm_version, desc), 56);

  /* Request numbers, literal.  _IOWR('d', nr, size) = 0xC0000000 |
   * (size << 16) | ('d' << 8) | nr  -- computed here by hand, which is the
   * point: if the encoding macros were wrong, these would not match. */
  EXPECT_EQ((int)NVSHIM_DRM_IOC_VERSION, (int)0xC0406400u);      /* nr 00, 64 */
  EXPECT_EQ((int)NVSHIM_DRM_IOC_GET_CAP, (int)0xC010640Cu);      /* nr 0c, 16 */
  EXPECT_EQ((int)NVSHIM_DRM_IOC_GEM_CLOSE, (int)0x40086409u);    /* nr 09,  8 */
  EXPECT_EQ((int)NVSHIM_DRM_IOC_GETPARAM, (int)0xC0106440u);     /* nr 40, 16 */
  EXPECT_EQ((int)NVSHIM_DRM_IOC_CHANNEL_ALLOC, (int)0xC0586442u);/* nr 42, 88 */
  EXPECT_EQ((int)NVSHIM_DRM_IOC_CHANNEL_FREE, (int)0x40046443u); /* nr 43,  4 */
  EXPECT_EQ((int)NVSHIM_DRM_IOC_VM_INIT, (int)0xC0106450u);      /* nr 50, 16 */
  EXPECT_EQ((int)NVSHIM_DRM_IOC_VM_BIND, (int)0xC0286451u);      /* nr 51, 40 */
  EXPECT_EQ((int)NVSHIM_DRM_IOC_EXEC, (int)0xC0286452u);         /* nr 52, 40 */
  EXPECT_EQ((int)NVSHIM_DRM_IOC_GEM_NEW, (int)0xC0306480u);      /* nr 80, 48 */
  EXPECT_EQ((int)NVSHIM_DRM_IOC_GEM_INFO, (int)0xC0286484u);     /* nr 84, 40 */
  EXPECT_EQ((int)NVSHIM_DRM_IOC_SYNCOBJ_CREATE, (int)0xC00864BFu);
  EXPECT_EQ((int)NVSHIM_DRM_IOC_SYNCOBJ_DESTROY, (int)0xC00864C0u);
  EXPECT_EQ((int)NVSHIM_DRM_IOC_SYNCOBJ_WAIT, (int)0xC02864C3u);
  EXPECT_EQ((int)NVSHIM_DRM_IOC_SYNCOBJ_RESET, (int)0xC01064C4u);
  EXPECT_EQ((int)NVSHIM_DRM_IOC_SYNCOBJ_SIGNAL, (int)0xC01064C5u);

  /* Field decomposition: magic 'd', the uAPI sub-number, the struct size. */
  EXPECT_EQ((int)NVSHIM_DRM_TYPE(NVSHIM_DRM_IOC_EXEC), 0x64);
  EXPECT_EQ((int)NVSHIM_DRM_NR(NVSHIM_DRM_IOC_EXEC), 0x40 + 0x12);
  EXPECT_EQ((int)NVSHIM_DRM_SIZE(NVSHIM_DRM_IOC_EXEC), 40);
  EXPECT_EQ((int)NVSHIM_DRM_DIR(NVSHIM_DRM_IOC_EXEC), 3);
  EXPECT_EQ((int)NVSHIM_DRM_DIR(NVSHIM_DRM_IOC_CHANNEL_FREE), 1);

  /* The route table: implemented carries its size, refused carries 0, absent
   * carries -1.  A refusal is keyed on the NUMBER, so any encoded size hits
   * it -- which is what makes the NVIF probe below honest. */
  EXPECT_EQ(nv_shim_drm_req_size(NVSHIM_DRM_IOC_GETPARAM), 16);
  EXPECT_EQ(nv_shim_drm_req_size(NVSHIM_DRM_IOC_CHANNEL_ALLOC), 88);
  EXPECT_EQ(nv_shim_drm_req_size(NVSHIM_DRM_IOC_SYNCOBJ_WAIT), 40);
  /* NVIF is served since WP-NVIF, and it is the ONE row whose encoded size is
   * a buffer length: the table states the bare header as its minimum, and the
   * handler applies each type's own shape rule.  So any encoded size >= 24
   * resolves to the same "known, served" answer here -- 8 bytes does too,
   * because the number is known; the handler then refuses the short call. */
  EXPECT_EQ(nv_shim_drm_req_size(NVSHIM_DRM_IOC_NVIF), (int)NVSHIM_NVIF_MIN_SIZE);
  EXPECT_EQ(nv_shim_drm_req_size(NVSHIM_DRM_IOC_NVIF_LEN(160)), (int)NVSHIM_NVIF_MIN_SIZE);
  EXPECT_EQ(nv_shim_drm_req_size(NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_NVIF, 8)), (int)NVSHIM_NVIF_MIN_SIZE);
  EXPECT_EQ(nv_shim_drm_req_size(NVSHIM_DRM_IOC_GEM_PUSHBUF), 0);
  EXPECT_EQ(nv_shim_drm_req_size(NVSHIM_DRM_IOC_ANY(0x7e, 8)), -1);
  EXPECT_EQ(nv_shim_drm_req_size(NVSHIM_DRM_IOC_ANY(0x50, 8)), 16); /* same nr, size checked later */

  /* Refusal names exist for every refused row, and for nothing served. */
  EXPECT_EQ((int)(nv_shim_drm_refusal_name(NVSHIM_DRM_IOC_NVIF) != 0), 0);
  EXPECT_EQ((int)(nv_shim_drm_refusal_name(NVSHIM_DRM_IOC_GEM_PUSHBUF) != 0), 1);
  EXPECT_EQ((int)(nv_shim_drm_refusal_name(NVSHIM_DRM_IOC_SYNCOBJ_TIMELINE_WAIT) != 0), 1);
  EXPECT_EQ((int)(nv_shim_drm_refusal_name(NVSHIM_DRM_IOC_EXEC) != 0), 0);
  EXPECT_EQ((int)(nv_shim_drm_refusal_name(NVSHIM_DRM_IOC_GEM_NEW) != 0), 0);
  EXPECT_EQ((int)(nv_shim_drm_refusal_name(NVSHIM_DRM_IOC_ANY(0x7e, 8)) != 0), 0);

  /* The two surfaces do not shadow each other. */
  EXPECT_EQ(nv_shim_req_size(NVSHIM_IOC_SUBMIT), (int)sizeof(struct nvshim_submit));
  EXPECT_EQ(nv_shim_req_size(NVSHIM_DRM_IOC_EXEC), -1);
  EXPECT_EQ(nv_shim_drm_req_size(NVSHIM_IOC_SUBMIT), -1);
  EXPECT_EQ((int)NVSHIM_IOC_TYPE(NVSHIM_IOC_SUBMIT), 'N');
  EXPECT_EQ((int)(NVSHIM_IOC_TYPE(NVSHIM_IOC_SUBMIT) != NVSHIM_DRM_MAGIC), 1);
}

/* ---- 2. size validation and named refusals ----------------------------- */

static void test_nvshim_drm_refusals(void) {
  uart_puts("  Running test_nvshim_drm_refusals...\n");
  tests_run++;

  /* Every refused row: -ENOSYS by name, with no process and a garbage arg --
   * the refusal is about the request, not about the call. */
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_SETPARAM, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_GROBJ_ALLOC, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_NOTIFIEROBJ_ALLOC, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_GPUOBJ_FREE, 0), -ENOSYS);
  /* NVIF is SERVED since WP-NVIF, so it is no longer in this list; with no
   * process there is nothing to attribute an object to, and the answer is the
   * same -EINVAL every other served row gives.  Its own suites follow. */
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_NVIF, 0), -EINVAL);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_SVM_INIT, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_SVM_BIND, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_GET_ZCULL_INFO, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_GEM_PUSHBUF, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_GEM_CPU_PREP, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_GEM_CPU_FINI, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_GEM_FLINK, 0), -ENOSYS);
  /* WP-PRIME: 0x2d / 0x2e are SERVED now -- with no process they answer the
   * same -EINVAL every other served row does, and they are no longer in the
   * named-refusal table.  test_nvshim_drm_prime_* drives both for real. */
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_PRIME_HANDLE_TO_FD, 0), -EINVAL);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_PRIME_FD_TO_HANDLE, 0), -EINVAL);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_SYNCOBJ_HANDLE_TO_FD, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_SYNCOBJ_FD_TO_HANDLE, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_SYNCOBJ_TIMELINE_WAIT, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_SYNCOBJ_QUERY, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_SYNCOBJ_TRANSFER, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_SYNCOBJ_TIMELINE_SIGNAL, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_SYNCOBJ_EVENTFD, 0), -ENOSYS);

  /* A number nobody implements is -ENOTTY (not a silent 0, not a refusal). */
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_ANY(0x7e, 8), 0), -ENOTTY);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_ANY(0x60, 16), 0), -ENOTTY);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_ANY(0x70, 8), 0), -ENOTTY);

  /* A known number with the wrong struct width is -EINVAL: the request exists,
   * the argument's shape is wrong.  With no process the answer would be
   * -EINVAL anyway, so these only prove the request is not executed; the
   * DISCRIMINATING size check is in test_nvshim_drm_probe, where the same call
   * with a live client would otherwise succeed. */
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_ANY(0x50, 8), 0), -EINVAL);   /* VM_INIT short */
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_ANY(0x50, 24), 0), -EINVAL);  /* VM_INIT long */
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_ANY(0x42, 80), 0), -EINVAL);  /* CHANNEL_ALLOC */
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_ANY(0x80, 40), 0), -EINVAL);  /* GEM_NEW as INFO */
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_ANY(0x00, 40), 0), -EINVAL);  /* VERSION short */

  /* The 'N' face keys on the WHOLE request, not on number+size: the same
   * number with a different width is -ENOTTY there (round 1's contract,
   * unchanged), while the 'd' face above answers -EINVAL.  Both differences are
   * asserted so neither can drift silently. */
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_IOC(NVSHIM_IOC_READ | NVSHIM_IOC_WRITE,
                                            NVSHIM_IOC_MAGIC, 6, 8), 0), -ENOTTY);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_IOC(NVSHIM_IOC_READ, NVSHIM_IOC_MAGIC, 99, 8), 0),
            -ENOTTY);

  /* An implemented row with no process is -EINVAL, exactly as before. */
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_EXEC, 0), -EINVAL);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_GETPARAM, 0), -EINVAL);
}

/* ---- the probe harness (same shape as nv_shim_test.c) ------------------ */

struct drm_probe {
  int pid;
  struct process *p;
  struct addr_space *as;
  int fd;
  uint64_t base;
  int old_pid;
  int up;
};

#define DREQ_BASE USER_MMAP_BASE_V2
#define DREQ_OPS (DREQ_BASE + 0x200)    /* 64 * 40 = 2560 */
#define DREQ_PUSH (DREQ_BASE + 0xC00)   /* 64 * 16 = 1024 */
#define DREQ_HANDLES (DREQ_BASE + 0x1000)
#define DREQ_STR (DREQ_BASE + 0x1200)
#define DREQ_REGION 0x4000

static int drm_probe_up(struct drm_probe *pr, uint64_t tgid) {
  for (unsigned i = 0; i < sizeof(*pr); i++) ((unsigned char *)pr)[i] = 0;
  pr->fd = -1;
  pr->pid = process_create();
  if (pr->pid < 0) return -1;
  pr->p = process_get_pcb(pr->pid);
  if (!pr->p) return -1;
  pr->up = 1;
  pr->p->is_kernel_process = 1;
  pr->old_pid = cpu_current_pids[0];
  set_current_process_pid(0, pr->pid);
  pr->as = vm_as_create(tgid);
  if (!pr->as) return -1;
  pr->p->as = pr->as;
  pr->base = DREQ_BASE;
  if (vm_region_insert(pr->as, pr->base, DREQ_REGION,
                       VM_PROT_READ | VM_PROT_WRITE, VMK_ANON, VM_MAP_PRIVATE,
                       0, 0) != 0)
    return -1;
  pr->fd = file_open(pr->p, "/dev/nvshim", 2 /* O_RDWR */);
  if (pr->fd < 0) return -1;
  return 0;
}

static void drm_probe_down(struct drm_probe *pr) {
  if (!pr->up) return;
  if (pr->fd >= 0) (void)file_close(pr->p, pr->fd);
  pr->fd = -1;
  pr->p->as = 0;
  if (pr->as) vm_as_teardown(pr->as);
  pr->as = 0;
  set_current_process_pid(0, pr->old_pid);
  process_kill(pr->pid);
  pr->up = 0;
}

/* Place a request struct at `where`, run the ioctl through the ROUTE (so the
 * magic-'d' dispatch is exercised), copy the result back.  Out fields are
 * prefilled by the caller. */
static int drm_ioc_at(struct drm_probe *pr, uint64_t where, unsigned long req, void *st,
                      int len) {
  if (vm_kwrite(pr->p, where, st, len) != 0) return -1000;
  int rc = nv_shim_ioctl(pr->p, pr->fd, req, where);
  if (vm_user_read(pr->p, where, st, len) != 0) return -1001;
  return rc;
}

static int drm_ioc(struct drm_probe *pr, unsigned long req, void *st, int len) {
  return drm_ioc_at(pr, pr->base, req, st, len);
}

/* ---- 3 + 4. the probe and both directions of the fence ----------------- */

static void test_nvshim_drm_probe(void) {
  uart_puts("  Running test_nvshim_drm_probe...\n");
  tests_run++;

  struct drm_probe pr;
  EXPECT_EQ(drm_probe_up(&pr, 91), 0);
  if (!pr.up) return;

  /* --- DRM_IOCTL_VERSION, the way libdrm calls it: query, then fill --- */
  struct nvshim_drm_version v;
  for (unsigned i = 0; i < sizeof(v); i++) ((unsigned char *)&v)[i] = 0;
  v.name_len = v.date_len = v.desc_len = 0;
  v.name = v.date = v.desc = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VERSION, &v, (int)sizeof v), 0);
  EXPECT_EQ((int)v.major, 1);
  EXPECT_EQ((int)v.minor, 3);
  EXPECT_EQ((int)v.patchlevel, 1); /* Mesa's nouveau winsys needs >= 1.3.1 */
  EXPECT_EQ((int)v.name_len, 8);   /* "nouveau" + NUL */
  EXPECT_EQ((v.date_len > 1), 1);
  EXPECT_EQ((v.desc_len > 1), 1);

  v.name = DREQ_STR;
  v.date = DREQ_STR + 32;
  v.desc = DREQ_STR + 64;
  v.name_len = 32;
  v.date_len = 32;
  v.desc_len = 32;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VERSION, &v, (int)sizeof v), 0);
  char str[16];
  EXPECT_EQ(vm_user_read(pr.p, DREQ_STR, str, 8), 0);
  EXPECT_EQ((int)str[0], 'n');
  EXPECT_EQ((int)str[1], 'o');
  EXPECT_EQ((int)str[6], 'u');
  EXPECT_EQ((int)str[7], 0);
  /* A buffer one byte too small is -EINVAL, DRM's own rule. */
  v.name_len = 7;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VERSION, &v, (int)sizeof v), -EINVAL);

  /* --- DRM_IOCTL_GET_CAP --- */
  struct nvshim_drm_cap cap;
  cap.capability = NVSHIM_DRM_CAP_SYNCOBJ;
  cap.value = 0xFFFFFFFFFFFFFFFFull;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GET_CAP, &cap, (int)sizeof cap), 0);
  EXPECT_EQ((int)cap.value, 1);
  cap.capability = NVSHIM_DRM_CAP_SYNCOBJ_TIMELINE;
  cap.value = 1;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GET_CAP, &cap, (int)sizeof cap), 0);
  EXPECT_EQ((int)cap.value, 0);
  cap.capability = 0x7777;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GET_CAP, &cap, (int)sizeof cap), -EINVAL);

  /* --- DRM_IOCTL_NOUVEAU_GETPARAM: the identity a winsys keys on --- */
  struct nvshim_drm_getparam q;
  unsigned prefill[][2] = {{NVSHIM_PARAM_CHIPSET_ID, 0x192},
                           {NVSHIM_PARAM_PCI_DEVICE, 0x2684},
                           {NVSHIM_PARAM_PCI_VENDOR, 0x10de},
                           {NVSHIM_DRM_PARAM_EXEC_PUSH_MAX, NVSHIM_DRM_MAX_PUSH}};
  for (unsigned i = 0; i < sizeof prefill / sizeof prefill[0]; i++) {
    q.param = prefill[i][0];
    q.value = 0xFFFFFFFFFFFFFFFFull;
    EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GETPARAM, &q, (int)sizeof q), 0);
    EXPECT_EQ((int)q.value, (int)prefill[i][1]);
  }
  q.param = NVSHIM_PARAM_FB_SIZE;
  q.value = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GETPARAM, &q, (int)sizeof q), 0);
  EXPECT_EQ((q.value >= (1ull << 30)), 1);
  q.param = NVSHIM_PARAM_PTIMER_TIME;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GETPARAM, &q, (int)sizeof q), -ENOSYS);
  q.param = 0x1234;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GETPARAM, &q, (int)sizeof q), -EINVAL);

  /* --- size validation with a live client ---
   * The same 16-byte struct, sent with a request whose encoded size lies about
   * it (8 instead of 16), must be refused.  This is the assertion a mutation
   * that drops the size comparison cannot survive: without the check the op
   * reads the full struct (it always copies sizeof), succeeds, and creates the
   * VM -- which the very next line then finds already taken. */
  struct nvshim_drm_vm_init vmi;
  vmi.kernel_managed_addr = 1ull << 39;
  vmi.kernel_managed_size = 1ull << 39;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_ANY(0x50, 8), &vmi, (int)sizeof vmi), -EINVAL);
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_ANY(0x50, 24), &vmi, (int)sizeof vmi), -EINVAL);

  /* --- VM_INIT with Mesa's exact arguments: TOP = 1<<40, KERN = 1<<39 --- */
  vmi.kernel_managed_addr = 1ull << 39;
  vmi.kernel_managed_size = 1ull << 39;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_INIT, &vmi, (int)sizeof vmi), 0);

  /* --- GEM_NEW: a 64 KiB object, VRAM --- */
  struct nvshim_drm_gem_new gn;
  for (unsigned i = 0; i < sizeof(gn); i++) ((unsigned char *)&gn)[i] = 0;
  gn.info.domain = NVSHIM_DOMAIN_VRAM;
  gn.info.size = 65536;
  gn.info.handle = 0xFFFFFFFFu;
  gn.align = 0;
  gn.channel_hint = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_NEW, &gn, (int)sizeof gn), 0);
  EXPECT_EQ((gn.info.handle != 0 && gn.info.handle != 0xFFFFFFFFu), 1);
  EXPECT_EQ((int)gn.info.size, 65536);
  EXPECT_EQ((gn.info.offset != 0), 1);
  EXPECT_EQ((int)gn.info.map_handle, 0); /* no mmap in this subset */
  uint32_t bo_handle = gn.info.handle;
  uint64_t bo_offset = gn.info.offset;

  /* --- GEM_INFO reports what GEM_NEW granted --- */
  struct nvshim_drm_gem_info gi;
  for (unsigned i = 0; i < sizeof(gi); i++) ((unsigned char *)&gi)[i] = 0xFF;
  gi.handle = bo_handle;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_INFO, &gi, (int)sizeof gi), 0);
  EXPECT_EQ(gi.offset, bo_offset);
  EXPECT_EQ(gi.size, 65536ull);
  EXPECT_EQ((int)gi.domain, NVSHIM_DOMAIN_VRAM);
  gi.handle = bo_handle + 1000;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_INFO, &gi, (int)sizeof gi), -EINVAL);

  /* --- VM_BIND: two ops in ONE call, so a driver that only applied op[0]
   * would pass the first assertion and fail the second --- */
  struct nvshim_drm_vm_bind_op ops[2];
  for (unsigned i = 0; i < sizeof(ops); i++) ((unsigned char *)ops)[i] = 0;
  ops[0].op = NVSHIM_DRM_VM_BIND_OP_MAP;
  ops[0].handle = bo_handle;
  ops[0].addr = 0x10000;
  ops[0].range = 65536;
  ops[1].op = NVSHIM_DRM_VM_BIND_OP_MAP;
  ops[1].handle = bo_handle;
  ops[1].addr = 0x20000;
  ops[1].range = 4096;
  ops[1].bo_offset = 0x8000;
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_OPS, ops, (int)sizeof ops), 0);

  struct nvshim_drm_vm_bind vb;
  for (unsigned i = 0; i < sizeof(vb); i++) ((unsigned char *)&vb)[i] = 0;
  vb.op_count = 2;
  vb.op_ptr = DREQ_OPS;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_BIND, &vb, (int)sizeof vb), 0);

  /* op[1] really landed: mapping it again is an overlap, and unmapping it
   * succeeds while an unmapped range does not. */
  struct nvshim_drm_vm_bind one;
  one.op_count = 1;
  one.flags = 0;
  one.wait_count = 0;
  one.sig_count = 0;
  one.wait_ptr = 0;
  one.sig_ptr = 0;
  one.op_ptr = DREQ_OPS;
  struct nvshim_drm_vm_bind_op single;
  for (unsigned i = 0; i < sizeof(single); i++) ((unsigned char *)&single)[i] = 0;
  single.op = NVSHIM_DRM_VM_BIND_OP_MAP;
  single.handle = bo_handle;
  single.addr = 0x20000;
  single.range = 4096;
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_OPS, &single, (int)sizeof single), 0);
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_BIND, &one, (int)sizeof one), -EINVAL);

  single.op = NVSHIM_DRM_VM_BIND_OP_UNMAP;
  single.handle = 0;
  single.bo_offset = 0;
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_OPS, &single, (int)sizeof single), 0);
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_BIND, &one, (int)sizeof one), 0);
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_BIND, &one, (int)sizeof one), -EINVAL);

  /* --- CHANNEL_ALLOC: the kernel allocates the push buffer --- */
  struct nvshim_drm_channel_alloc ca;
  for (unsigned i = 0; i < sizeof(ca); i++) ((unsigned char *)&ca)[i] = 0xFF;
  ca.fb_ctxdma_handle = 0;
  ca.tt_ctxdma_handle = 0;
  ca.notifier_handle = 0;
  ca.pushbuf_domains = 0;
  ca.nr_subchan = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_CHANNEL_ALLOC, &ca, (int)sizeof ca), 0);
  EXPECT_EQ((ca.channel > 0), 1);
  EXPECT_EQ((int)ca.pushbuf_domains, NVSHIM_DOMAIN_VRAM);
  EXPECT_EQ((int)ca.nr_subchan, 0);
  int32_t channel = ca.channel;

  /* --- syncobjs, then EXEC signalling one --- */
  struct nvshim_drm_syncobj_create sc;
  sc.flags = 0;
  sc.handle = 0xFFFFFFFFu;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_SYNCOBJ_CREATE, &sc, (int)sizeof sc), 0);
  EXPECT_EQ((sc.handle != 0 && sc.handle != 0xFFFFFFFFu), 1);
  uint32_t sobj = sc.handle;
  struct nvshim_drm_syncobj_create sc2;
  sc2.flags = 0;
  sc2.handle = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_SYNCOBJ_CREATE, &sc2, (int)sizeof sc2), 0);
  EXPECT_EQ((sc2.handle != sobj), 1);

  struct nvshim_drm_exec_push push;
  push.va = 0x10000;
  push.va_len = 64;
  push.flags = 0;
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_PUSH, &push, (int)sizeof push), 0);

  struct nvshim_drm_sync sig;
  sig.flags = NVSHIM_DRM_SYNC_SYNCOBJ;
  sig.handle = sobj;
  sig.timeline_value = 0;
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_STR, &sig, (int)sizeof sig), 0);

  struct nvshim_drm_exec ex;
  for (unsigned i = 0; i < sizeof(ex); i++) ((unsigned char *)&ex)[i] = 0;
  ex.channel = (uint32_t)channel;
  ex.push_count = 1;
  ex.sig_count = 1;
  ex.sig_ptr = DREQ_STR;
  ex.push_ptr = DREQ_PUSH;

  uint32_t handles1[1];
  handles1[0] = sobj;
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_HANDLES, handles1, (int)sizeof handles1), 0);

  struct nvshim_drm_syncobj_wait w;
  for (unsigned i = 0; i < sizeof(w); i++) ((unsigned char *)&w)[i] = 0;
  w.handles = DREQ_HANDLES;
  w.count_handles = 1;
  w.first_signaled = 0xFFFFFFFFu;

  /* Hold the mock's kick: EXEC enqueues, nothing retires. */
  nv_rm_mock_hold(1);
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_EXEC, &ex, (int)sizeof ex), 0);
  EXPECT_EQ(nv_rm_mock_pending(), 1);

  w.timeout_nsec = 0; /* poll */
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_SYNCOBJ_WAIT, &w, (int)sizeof w), -ETIME);
  /* A 1 ms absolute deadline in the future, still not retired. */
  w.timeout_nsec = (int64_t)((timer_get_ms() + 1) * 1000000ull);
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_SYNCOBJ_WAIT, &w, (int)sizeof w), -ETIME);
  EXPECT_EQ(nv_rm_mock_pending(), 1);

  /* An EXEC into a VA that is not mapped is refused by the RM, not counted. */
  push.va = 0x40000;
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_PUSH, &push, (int)sizeof push), 0);
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_EXEC, &ex, (int)sizeof ex), -EINVAL);
  /* ...and a push that starts mapped but runs off the end of the mapping. */
  push.va = 0x10000 + 0x10000 - 0x800; /* starts mapped, ends past 0x20000 */
  push.va_len = 0x1000;
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_PUSH, &push, (int)sizeof push), 0);
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_EXEC, &ex, (int)sizeof ex), -EINVAL);

  /* Release the hold and submit again: that kick retires the first fence too,
   * and the syncobj created for it becomes signalled.  Nothing about the
   * first wait was faked -- same syncobj, same call, different engine state. */
  nv_rm_mock_hold(0);
  push.va = 0x10000;
  push.va_len = 64;
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_PUSH, &push, (int)sizeof push), 0);
  ex.sig_count = 0;
  ex.sig_ptr = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_EXEC, &ex, (int)sizeof ex), 0);
  EXPECT_EQ(nv_rm_mock_pending(), 0);

  ex.sig_count = 1;
  ex.sig_ptr = DREQ_STR;
  w.timeout_nsec = 0;
  w.first_signaled = 0xFFFFFFFFu;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_SYNCOBJ_WAIT, &w, (int)sizeof w), 0);
  EXPECT_EQ((int)w.first_signaled, 0);

  /* A handle this client never created is -EINVAL, not a silent pass. */
  handles1[0] = sobj + 1000;
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_HANDLES, handles1, (int)sizeof handles1), 0);
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_SYNCOBJ_WAIT, &w, (int)sizeof w), -EINVAL);

  /* SIGNAL / RESET are the client-forced forms; both directions observed so a
   * syncobj that is signalled by construction cannot pass. */
  handles1[0] = sc2.handle;
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_HANDLES, handles1, (int)sizeof handles1), 0);
  struct nvshim_drm_syncobj_array sa;
  sa.handles = DREQ_HANDLES;
  sa.count_handles = 1;
  sa.pad = 0;
  w.first_signaled = 0xFFFFFFFFu;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_SYNCOBJ_WAIT, &w, (int)sizeof w), -ETIME);
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_SYNCOBJ_SIGNAL, &sa, (int)sizeof sa), 0);
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_SYNCOBJ_WAIT, &w, (int)sizeof w), 0);
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_SYNCOBJ_RESET, &sa, (int)sizeof sa), 0);
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_SYNCOBJ_WAIT, &w, (int)sizeof w), -ETIME);
  struct nvshim_drm_syncobj_destroy sd;
  sd.handle = sc2.handle;
  sd.pad = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_SYNCOBJ_DESTROY, &sd, (int)sizeof sd), 0);
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_SYNCOBJ_DESTROY, &sd, (int)sizeof sd), -EINVAL);

  /* --- teardown --- */
  struct nvshim_drm_channel_free cf;
  cf.channel = channel;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_CHANNEL_FREE, &cf, (int)sizeof cf), 0);
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_CHANNEL_FREE, &cf, (int)sizeof cf), -EINVAL);

  struct nvshim_drm_gem_close gc;
  gc.handle = bo_handle;
  gc.pad = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_CLOSE, &gc, (int)sizeof gc), 0);
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_CLOSE, &gc, (int)sizeof gc), -EINVAL);

  uart_puts("  [NVSHIM] drm probe version+cap+getparam+gem+vm+bind+channel+exec+syncobj: PASS\n");
  drm_probe_down(&pr);
  EXPECT_EQ(nv_rm_mock_live(), 0); /* close released the VM too */
}

/* ---- 5a. ownership ----------------------------------------------------- */

static void test_nvshim_drm_ownership(void) {
  uart_puts("  Running test_nvshim_drm_ownership...\n");
  tests_run++;

  struct drm_probe pr;
  EXPECT_EQ(drm_probe_up(&pr, 92), 0);
  if (!pr.up) return;

  int fd_b = file_open(pr.p, "/dev/nvshim", 2);
  EXPECT_EQ((fd_b >= 0), 1);
  if (fd_b < 0) {
    drm_probe_down(&pr);
    return;
  }

  /* Client A: device (implicit), VM, BO, channel, syncobj. */
  struct nvshim_drm_getparam q;
  q.param = NVSHIM_PARAM_CHIPSET_ID;
  q.value = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GETPARAM, &q, (int)sizeof q), 0);

  struct nvshim_drm_vm_init vmi;
  vmi.kernel_managed_addr = 1ull << 39;
  vmi.kernel_managed_size = 1ull << 39;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_INIT, &vmi, (int)sizeof vmi), 0);

  struct nvshim_drm_gem_new gn;
  for (unsigned i = 0; i < sizeof(gn); i++) ((unsigned char *)&gn)[i] = 0;
  gn.info.domain = NVSHIM_DOMAIN_VRAM;
  gn.info.size = 65536;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_NEW, &gn, (int)sizeof gn), 0);

  struct nvshim_drm_vm_bind_op op;
  for (unsigned i = 0; i < sizeof(op); i++) ((unsigned char *)&op)[i] = 0;
  op.op = NVSHIM_DRM_VM_BIND_OP_MAP;
  op.handle = gn.info.handle;
  op.addr = 0x10000;
  op.range = 65536;
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_OPS, &op, (int)sizeof op), 0);
  struct nvshim_drm_vm_bind vb;
  for (unsigned i = 0; i < sizeof(vb); i++) ((unsigned char *)&vb)[i] = 0;
  vb.op_count = 1;
  vb.op_ptr = DREQ_OPS;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_BIND, &vb, (int)sizeof vb), 0);

  struct nvshim_drm_channel_alloc ca;
  for (unsigned i = 0; i < sizeof(ca); i++) ((unsigned char *)&ca)[i] = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_CHANNEL_ALLOC, &ca, (int)sizeof ca), 0);

  struct nvshim_drm_syncobj_create sc;
  sc.flags = 0;
  sc.handle = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_SYNCOBJ_CREATE, &sc, (int)sizeof sc), 0);

  /* Client B is the same process on another fd: a second device. */
  int save_fd = pr.fd;
  pr.fd = fd_b;
  struct nvshim_drm_getparam qb;
  qb.param = NVSHIM_PARAM_CHIPSET_ID;
  qb.value = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GETPARAM, &qb, (int)sizeof qb), 0);

  /* Every one of A's objects is refused on B's fd. */
  struct nvshim_drm_gem_info gi;
  for (unsigned i = 0; i < sizeof(gi); i++) ((unsigned char *)&gi)[i] = 0xFF;
  gi.handle = gn.info.handle;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_INFO, &gi, (int)sizeof gi), -EINVAL);

  struct nvshim_drm_gem_close gc;
  gc.handle = gn.info.handle;
  gc.pad = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_CLOSE, &gc, (int)sizeof gc), -EINVAL);

  struct nvshim_drm_exec_push push;
  push.va = 0x10000;
  push.va_len = 64;
  push.flags = 0;
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_PUSH, &push, (int)sizeof push), 0);
  struct nvshim_drm_exec ex;
  for (unsigned i = 0; i < sizeof(ex); i++) ((unsigned char *)&ex)[i] = 0;
  ex.channel = (uint32_t)ca.channel;
  ex.push_count = 1;
  ex.push_ptr = DREQ_PUSH;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_EXEC, &ex, (int)sizeof ex), -EINVAL);

  struct nvshim_drm_channel_free cf;
  cf.channel = ca.channel;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_CHANNEL_FREE, &cf, (int)sizeof cf), -EINVAL);

  uint32_t h[1];
  h[0] = sc.handle;
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_HANDLES, h, (int)sizeof h), 0);
  struct nvshim_drm_syncobj_wait w;
  for (unsigned i = 0; i < sizeof(w); i++) ((unsigned char *)&w)[i] = 0;
  w.handles = DREQ_HANDLES;
  w.count_handles = 1;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_SYNCOBJ_WAIT, &w, (int)sizeof w), -EINVAL);

  /* B never created a VM, so a VM_BIND there is refused too. */
  struct nvshim_drm_vm_bind vb2;
  for (unsigned i = 0; i < sizeof(vb2); i++) ((unsigned char *)&vb2)[i] = 0;
  vb2.op_count = 1;
  vb2.op_ptr = DREQ_OPS;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_BIND, &vb2, (int)sizeof vb2), -EINVAL);

  pr.fd = save_fd;
  EXPECT_EQ(file_close(pr.p, fd_b), 0);
  drm_probe_down(&pr);
  EXPECT_EQ(nv_rm_mock_live(), 0);
}

/* ---- 5b. argument and ordering limits ---------------------------------- */

static void test_nvshim_drm_limits(void) {
  uart_puts("  Running test_nvshim_drm_limits...\n");
  tests_run++;

  struct drm_probe pr;
  EXPECT_EQ(drm_probe_up(&pr, 93), 0);
  if (!pr.up) return;

  /* VM_INIT ordering: the uAPI says a BO created first makes it -ENOSYS. */
  struct nvshim_drm_gem_new gn;
  for (unsigned i = 0; i < sizeof(gn); i++) ((unsigned char *)&gn)[i] = 0;
  gn.info.domain = NVSHIM_DOMAIN_VRAM;
  gn.info.size = 4096;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_NEW, &gn, (int)sizeof gn), 0);

  struct nvshim_drm_vm_init vmi;
  vmi.kernel_managed_addr = 1ull << 39;
  vmi.kernel_managed_size = 1ull << 39;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_INIT, &vmi, (int)sizeof vmi), -ENOSYS);

  /* GEM_NEW geometry: zero size, a CPU domain (WP-F3: legal, it is system
   * memory without a CPU mapping), a coherence promise with no mapping, a
   * non-arena alignment, a foreign channel hint, over the per-client cap. */
  struct nvshim_drm_gem_new bad;
  for (unsigned i = 0; i < sizeof(bad); i++) ((unsigned char *)&bad)[i] = 0;
  bad.info.domain = NVSHIM_DOMAIN_VRAM;
  bad.info.size = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_NEW, &bad, (int)sizeof bad), -EINVAL);
  bad.info.size = 4096;
  /* Round 2 refused every CPU bit.  WP-F3 serves CPU memory -- but only a
   * domain that ALSO says MAPPABLE may be mapped into the client, so plain CPU
   * is an object with memory and map_handle == 0. */
  bad.info.domain = NVSHIM_DOMAIN_CPU;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_NEW, &bad, (int)sizeof bad), 0);
  EXPECT_EQ((int)bad.info.map_handle, 0);
  bad.info.domain = NVSHIM_DOMAIN_VRAM | NVSHIM_DOMAIN_COHERENT;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_NEW, &bad, (int)sizeof bad), -EINVAL);
  bad.info.domain = NVSHIM_DOMAIN_VRAM;
  bad.align = 8192;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_NEW, &bad, (int)sizeof bad), -EINVAL);
  bad.align = 0;
  bad.channel_hint = 1;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_NEW, &bad, (int)sizeof bad), -EINVAL);
  bad.channel_hint = 0;
  bad.info.size = 513ull * 1024 * 1024;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_NEW, &bad, (int)sizeof bad), -ENOMEM);
  bad.info.size = 4096;
  bad.info.domain = NVSHIM_DOMAIN_GART;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_NEW, &bad, (int)sizeof bad), 0);

  /* A fresh client for the VM rows: this one already has two BOs. */
  drm_probe_down(&pr);
  EXPECT_EQ(nv_rm_mock_live(), 0);
  EXPECT_EQ(drm_probe_up(&pr, 94), 0);
  if (!pr.up) return;

  /* VM_INIT geometry. */
  struct nvshim_drm_vm_init v;
  v.kernel_managed_addr = 0;
  v.kernel_managed_size = 1ull << 30;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_INIT, &v, (int)sizeof v), -EINVAL);
  v.kernel_managed_addr = 0x1234;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_INIT, &v, (int)sizeof v), -EINVAL);
  v.kernel_managed_addr = 1ull << 39;
  v.kernel_managed_size = 0x800;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_INIT, &v, (int)sizeof v), -EINVAL);
  v.kernel_managed_addr = 1ull << 39;
  v.kernel_managed_size = (1ull << 40); /* sum over 1 TiB */
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_INIT, &v, (int)sizeof v), -EINVAL);
  v.kernel_managed_size = 1ull << 39;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_INIT, &v, (int)sizeof v), 0);
  /* One VA space per file: a second VM_INIT is -EBUSY (this row is ours; the
   * uAPI has no second VM to name). */
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_INIT, &v, (int)sizeof v), -EBUSY);

  /* A channel needs the VM and a 4 KiB-aligned push buffer domain. */
  struct nvshim_drm_channel_alloc ca;
  for (unsigned i = 0; i < sizeof(ca); i++) ((unsigned char *)&ca)[i] = 0;
  ca.fb_ctxdma_handle = 1; /* pre-nvif context DMA */
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_CHANNEL_ALLOC, &ca, (int)sizeof ca), -ENOSYS);
  ca.fb_ctxdma_handle = 0;
  ca.nr_subchan = 2;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_CHANNEL_ALLOC, &ca, (int)sizeof ca), -ENOSYS);
  ca.nr_subchan = 0;
  ca.pushbuf_domains = NVSHIM_DOMAIN_CPU;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_CHANNEL_ALLOC, &ca, (int)sizeof ca), -EINVAL);
  ca.pushbuf_domains = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_CHANNEL_ALLOC, &ca, (int)sizeof ca), 0);
  int32_t channel = ca.channel;

  /* VM_BIND shapes: no ops, too many ops, an unaligned addr, a zero range,
   * async and sync flavours we do not implement. */
  struct nvshim_drm_vm_bind vb;
  for (unsigned i = 0; i < sizeof(vb); i++) ((unsigned char *)&vb)[i] = 0;
  vb.op_count = 0;
  vb.op_ptr = DREQ_OPS;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_BIND, &vb, (int)sizeof vb), -EINVAL);
  vb.op_count = NVSHIM_DRM_MAX_VM_BIND_OPS + 1;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_BIND, &vb, (int)sizeof vb), -EINVAL);
  vb.op_count = 1;
  vb.op_ptr = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_BIND, &vb, (int)sizeof vb), -EINVAL);
  vb.op_ptr = DREQ_OPS;

  struct nvshim_drm_vm_bind_op one;
  for (unsigned i = 0; i < sizeof(one); i++) ((unsigned char *)&one)[i] = 0;
  one.op = NVSHIM_DRM_VM_BIND_OP_MAP;
  one.handle = 1;
  one.addr = 0x1001; /* unaligned */
  one.range = 4096;
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_OPS, &one, (int)sizeof one), 0);
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_BIND, &vb, (int)sizeof vb), -EINVAL);
  one.addr = 0x1000;
  one.range = 0;
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_OPS, &one, (int)sizeof one), 0);
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_BIND, &vb, (int)sizeof vb), -EINVAL);
  one.range = 4096;
  one.op = 7; /* not MAP/UNMAP */
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_OPS, &one, (int)sizeof one), 0);
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_BIND, &vb, (int)sizeof vb), -EINVAL);
  one.op = NVSHIM_DRM_VM_BIND_OP_MAP;
  one.flags = NVSHIM_DRM_VM_BIND_SPARSE;
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_OPS, &one, (int)sizeof one), 0);
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_BIND, &vb, (int)sizeof vb), -ENOSYS);
  one.flags = 0;
  one.handle = 1; /* a handle this client never received */
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_OPS, &one, (int)sizeof one), 0);
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_BIND, &vb, (int)sizeof vb), -EINVAL);
  vb.flags = NVSHIM_DRM_VM_BIND_RUN_ASYNC;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_BIND, &vb, (int)sizeof vb), -ENOSYS);
  vb.flags = 0;
  vb.wait_count = 1;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_BIND, &vb, (int)sizeof vb), -ENOSYS);
  vb.wait_count = 0;
  vb.sig_count = 1;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_BIND, &vb, (int)sizeof vb), -ENOSYS);

  /* EXEC shapes. */
  struct nvshim_drm_exec ex;
  for (unsigned i = 0; i < sizeof(ex); i++) ((unsigned char *)&ex)[i] = 0;
  ex.channel = (uint32_t)channel;
  ex.push_count = 0;
  ex.push_ptr = DREQ_PUSH;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_EXEC, &ex, (int)sizeof ex), -EINVAL);
  ex.push_count = NVSHIM_DRM_MAX_PUSH + 1;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_EXEC, &ex, (int)sizeof ex), -EINVAL);
  ex.push_count = 1;
  struct nvshim_drm_exec_push push;
  push.va = 0x2000;
  push.va_len = 6; /* not a dword multiple */
  push.flags = 0;
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_PUSH, &push, (int)sizeof push), 0);
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_EXEC, &ex, (int)sizeof ex), -EINVAL);
  push.va = 0x2002; /* unaligned push buffer */
  push.va_len = 4;
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_PUSH, &push, (int)sizeof push), 0);
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_EXEC, &ex, (int)sizeof ex), -EINVAL);
  push.va = 0x2000;
  push.flags = 0x80; /* undefined push flag */
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_PUSH, &push, (int)sizeof push), 0);
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_EXEC, &ex, (int)sizeof ex), -EINVAL);
  push.flags = 0;
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_PUSH, &push, (int)sizeof push), 0);
  ex.wait_count = 1; /* a pre-submit wait: refused by name */
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_EXEC, &ex, (int)sizeof ex), -ENOSYS);
  ex.wait_count = 0;

  /* A timeline sync in the sig array is refused (binary syncobjs only). */
  struct nvshim_drm_sync ts;
  ts.flags = NVSHIM_DRM_SYNC_TIMELINE_SYNCOBJ;
  ts.handle = 1;
  ts.timeline_value = 5;
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_STR, &ts, (int)sizeof ts), 0);
  ex.sig_count = 1;
  ex.sig_ptr = DREQ_STR;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_EXEC, &ex, (int)sizeof ex), -ENOSYS);
  ts.flags = NVSHIM_DRM_SYNC_SYNCOBJ;
  ts.handle = 999; /* a syncobj this client does not own */
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_STR, &ts, (int)sizeof ts), 0);
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_EXEC, &ex, (int)sizeof ex), -EINVAL);
  ex.sig_count = 0;
  ex.sig_ptr = 0;

  /* syncobj rows: unknown create flags, a wait with no handles, a wait flag
   * that needs fence registration, a reset of a foreign handle. */
  struct nvshim_drm_syncobj_create sc;
  sc.flags = 2;
  sc.handle = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_SYNCOBJ_CREATE, &sc, (int)sizeof sc), -EINVAL);
  sc.flags = NVSHIM_DRM_SYNCOBJ_CREATE_SIGNALED;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_SYNCOBJ_CREATE, &sc, (int)sizeof sc), 0);
  EXPECT_EQ((sc.handle != 0), 1);

  struct nvshim_drm_syncobj_wait w;
  for (unsigned i = 0; i < sizeof(w); i++) ((unsigned char *)&w)[i] = 0;
  w.handles = DREQ_HANDLES;
  w.count_handles = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_SYNCOBJ_WAIT, &w, (int)sizeof w), -EINVAL);
  uint32_t h[1];
  h[0] = sc.handle;
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_HANDLES, h, (int)sizeof h), 0);
  w.count_handles = 1;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_SYNCOBJ_WAIT, &w, (int)sizeof w), 0); /* created signalled */
  w.flags = NVSHIM_DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_SYNCOBJ_WAIT, &w, (int)sizeof w), -ENOSYS);
  w.flags = 0x80; /* not a wait flag */
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_SYNCOBJ_WAIT, &w, (int)sizeof w), -EINVAL);
  w.flags = 0;
  w.handles = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_SYNCOBJ_WAIT, &w, (int)sizeof w), -EINVAL);

  struct nvshim_drm_syncobj_array sa;
  sa.handles = DREQ_HANDLES;
  sa.count_handles = 1;
  sa.pad = 0;
  h[0] = 4242;
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_HANDLES, h, (int)sizeof h), 0);
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_SYNCOBJ_RESET, &sa, (int)sizeof sa), -EINVAL);

  /* An unmapped user range is -EFAULT, not a fault. */
  EXPECT_EQ(nv_shim_ioctl(pr.p, pr.fd, NVSHIM_DRM_IOC_GEM_NEW, 0x1000), -EFAULT);
  EXPECT_EQ(nv_shim_ioctl(pr.p, pr.fd, NVSHIM_DRM_IOC_GEM_NEW, 0), -EINVAL);

  drm_probe_down(&pr);
  EXPECT_EQ(nv_rm_mock_live(), 0);
}

/* ---- 6. CPU-mapped BOs (WP-F3) ----------------------------------------- */

/* The mock has one device (MOCK_DEV_ID, nv_rm_mock.c), so the seams that take
 * a dev_id can name it.  It is not assumed: GEM_NEW below only succeeds if a
 * device is open, and it is the only device this RM ever hands out. */
#define DDEV 1u

/* The gate.  A BO is created, mmap'd, written through the mapping, and then read
 * back through the KERNEL's view of the object -- not through the client's
 * pointer -- and submitted to EXEC, which digests the bytes it fetched.
 *
 * Three things must all hold, and each rules out a different fake:
 *   - nv_rm_mock_read_bo() (the RM's own frame address) sees the bytes: the
 *     mapping is the object, not a copy of it;
 *   - no leaf exists before the first store and one exists after: the ordinary
 *     fault path is what populated it (an eager map would show present/present);
 *   - the EXEC digest equals the pattern, and changes when the pattern at a
 *     different object offset changes: the submit path read the VA it was given.
 */
static void test_nvshim_drm_cpu_map(void) {
  uart_puts("  Running test_nvshim_drm_cpu_map...\n");
  tests_run++;

  /* The three spellings of the domain bits are one contract (nv_rm.h,
   * nv_shim.h, nouveau's uAPI).  Drift here would silently change which
   * objects are mappable. */
  EXPECT_EQ((int)NV_RM_DOM_CPU, (int)NVSHIM_DOMAIN_CPU);
  EXPECT_EQ((int)NV_RM_DOM_VRAM, (int)NVSHIM_DOMAIN_VRAM);
  EXPECT_EQ((int)NV_RM_DOM_GART, (int)NVSHIM_DOMAIN_GART);
  EXPECT_EQ((int)NV_RM_DOM_MAPPABLE, (int)NVSHIM_DOMAIN_MAPPABLE);
  EXPECT_EQ((int)NV_RM_DOM_COHERENT, (int)NVSHIM_DOMAIN_COHERENT);
  EXPECT_EQ((int)NV_RM_DOM_NO_SHARE, (int)NVSHIM_DOMAIN_NO_SHARE);

  struct drm_probe pr;
  EXPECT_EQ(drm_probe_up(&pr, 95), 0);
  if (!pr.up) return;

  /* No EXEC yet: the digest seam must say so.  A digest that answered 0 here
   * would be true by construction and worthless below. */
  uint32_t sum = 0, dlen = 0;
  EXPECT_EQ(nv_shim_test_last_exec(pr.p, pr.fd, &sum, &dlen), -ENOENT);

  /* VM_INIT first: the uAPI refuses it once a BO exists. */
  struct nvshim_drm_vm_init vmi;
  vmi.kernel_managed_addr = 1ull << 39;
  vmi.kernel_managed_size = 1ull << 39;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_INIT, &vmi, (int)sizeof vmi), 0);

  /* Two pages, so a mapping that shifted by a page cannot pass unnoticed. */
  struct nvshim_drm_gem_new gn;
  for (unsigned i = 0; i < sizeof(gn); i++) ((unsigned char *)&gn)[i] = 0;
  gn.info.domain = NVSHIM_DOMAIN_CPU | NVSHIM_DOMAIN_MAPPABLE;
  gn.info.size = 8192;
  gn.info.handle = 0xFFFFFFFFu;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_NEW, &gn, (int)sizeof gn), 0);
  uint32_t h = gn.info.handle;
  uint64_t moff = gn.info.map_handle;
  EXPECT_EQ((h != 0 && h != 0xFFFFFFFFu), 1);
  EXPECT_EQ((int)gn.info.size, 8192);
  EXPECT_EQ((int)(gn.info.offset != 0), 1);  /* the device address exists */
  /* map_handle is DRM's fake offset, handle << PAGE_SHIFT, and never 0 for an
   * object that offered a CPU mapping. */
  EXPECT_EQ((int)(moff == ((uint64_t)h << 12)), 1);
  EXPECT_EQ((int)(moff != 0), 1);

  struct nvshim_drm_gem_info gi;
  for (unsigned i = 0; i < sizeof(gi); i++) ((unsigned char *)&gi)[i] = 0xFF;
  gi.handle = h;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_INFO, &gi, (int)sizeof gi), 0);
  EXPECT_EQ(gi.map_handle, moff);
  EXPECT_EQ(gi.offset, gn.info.offset);

  /* Binding and a channel: EXEC names a GPU VA, so the object needs a VA and a
   * channel before the submit below means anything. */
  struct nvshim_drm_vm_bind_op op;
  for (unsigned i = 0; i < sizeof(op); i++) ((unsigned char *)&op)[i] = 0;
  op.op = NVSHIM_DRM_VM_BIND_OP_MAP;
  op.handle = h;
  op.addr = 0x10000;
  op.range = 8192;
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_OPS, &op, (int)sizeof op), 0);
  struct nvshim_drm_vm_bind vb;
  for (unsigned i = 0; i < sizeof(vb); i++) ((unsigned char *)&vb)[i] = 0;
  vb.op_count = 1;
  vb.op_ptr = DREQ_OPS;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_BIND, &vb, (int)sizeof vb), 0);

  struct nvshim_drm_channel_alloc ca;
  for (unsigned i = 0; i < sizeof(ca); i++) ((unsigned char *)&ca)[i] = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_CHANNEL_ALLOC, &ca, (int)sizeof ca), 0);
  int32_t ch = ca.channel;
  EXPECT_EQ((ch > 0), 1);

  /* --- the mapping --- */
  int64_t mva = vm_mmap(pr.p, 0, 8192, VM_PROT_READ | VM_PROT_WRITE,
                        VM_MAP_SHARED, pr.fd, moff);
  EXPECT_EQ((mva > 0), 1);
  if (mva <= 0) {
    drm_probe_down(&pr);
    return;
  }
  EXPECT_EQ((int)((uint64_t)mva >= USER_MMAP_BASE_V2 &&
                  (uint64_t)mva < USER_MMAP_LIMIT_V2), 1);

  struct vm_region *r = vm_region_find(pr.as, (uint64_t)mva);
  EXPECT_EQ((int)(r != 0), 1);
  EXPECT_EQ((int)(r && r->kind == VMK_SHARED), 1);
  EXPECT_EQ((int)(r && r->flags == VM_MAP_SHARED), 1);
  EXPECT_EQ((int)(r && r->obj != 0), 1); /* the page list is an object */
  EXPECT_EQ((int)(r && r->len == 8192), 1);

  /* Demand-paged: absent before the first touch. */
  uint64_t leaf = 0;
  EXPECT_EQ((int)(vm_arch_walk(pr.as, (uint64_t)mva, &leaf) != 0), 1);

  uint8_t pat[256], got[256];
  for (int i = 0; i < 256; i++) pat[i] = (uint8_t)(0xA5 ^ ((i * 7 + 1) & 0xFF));
  EXPECT_EQ(vm_kwrite(pr.p, (uint64_t)mva, pat, 256), 0);
  /* ... and present after it, installed by the fault path. */
  EXPECT_EQ((int)(vm_arch_walk(pr.as, (uint64_t)mva, &leaf) == 0), 1);

  /* THE GATE: the same bytes read through the kernel's view of the OBJECT. */
  for (int i = 0; i < 256; i++) got[i] = 0;
  EXPECT_EQ(nv_rm_mock_read_bo(DDEV, h, 0, got, 256), 0);
  int diff = 0;
  for (int i = 0; i < 256; i++)
    if (got[i] != pat[i]) diff++;
  EXPECT_EQ(diff, 0);

  /* The mapping's own read path agrees (same frames, not a coincidence). */
  for (int i = 0; i < 256; i++) got[i] = 0;
  EXPECT_EQ(vm_user_read(pr.p, (uint64_t)mva, got, 256), 0);
  diff = 0;
  for (int i = 0; i < 256; i++)
    if (got[i] != pat[i]) diff++;
  EXPECT_EQ(diff, 0);

  /* The object was zeroed when it was allocated and only 256 bytes were
   * written.  Prefilled with 0xFF first: "still zero" is then an observation,
   * not an initialisation. */
  for (int i = 0; i < 64; i++) got[i] = 0xFF;
  EXPECT_EQ(nv_rm_mock_read_bo(DDEV, h, 256, got, 64), 0);
  int nz = 0;
  for (int i = 0; i < 64; i++)
    if (got[i]) nz++;
  EXPECT_EQ(nz, 0);

  /* --- EXEC sees the bytes --- */
  struct nvshim_drm_exec_push push;
  push.va = 0x10000;
  push.va_len = 64;
  push.flags = 0;
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_PUSH, &push, (int)sizeof push), 0);
  struct nvshim_drm_exec ex;
  for (unsigned i = 0; i < sizeof(ex); i++) ((unsigned char *)&ex)[i] = 0;
  ex.channel = (uint32_t)ch;
  ex.push_count = 1;
  ex.push_ptr = DREQ_PUSH;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_EXEC, &ex, (int)sizeof ex), 0);
  EXPECT_EQ(nv_shim_test_last_exec(pr.p, pr.fd, &sum, &dlen), 0);
  EXPECT_EQ((int)dlen, 64);
  EXPECT_EQ((int)sum, (int)nv_shim_test_sum(pat, 64));

  /* A second pattern at object offset 64, written through the SAME mapping, and
   * a push that names that VA.  The digest must become the second pattern: a
   * submit path that always read offset 0, or that cached the first stream,
   * cannot produce both values. */
  uint8_t pat2[64];
  for (int i = 0; i < 64; i++) pat2[i] = (uint8_t)(0x3C + i);
  EXPECT_EQ(vm_kwrite(pr.p, (uint64_t)mva + 64, pat2, 64), 0);
  EXPECT_EQ((int)(nv_shim_test_sum(pat2, 64) != nv_shim_test_sum(pat, 64)), 1);
  push.va = 0x10000 + 64;
  push.va_len = 64;
  EXPECT_EQ(vm_kwrite(pr.p, DREQ_PUSH, &push, (int)sizeof push), 0);
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_EXEC, &ex, (int)sizeof ex), 0);
  EXPECT_EQ(nv_shim_test_last_exec(pr.p, pr.fd, &sum, &dlen), 0);
  EXPECT_EQ((int)dlen, 64);
  EXPECT_EQ((int)sum, (int)nv_shim_test_sum(pat2, 64));
  for (int i = 0; i < 64; i++) got[i] = 0;
  EXPECT_EQ(nv_rm_mock_read_bo(DDEV, h, 64, got, 64), 0);
  diff = 0;
  for (int i = 0; i < 64; i++)
    if (got[i] != pat2[i]) diff++;
  EXPECT_EQ(diff, 0);

  /* A second, shorter mapping of the same object is the same memory: a store
   * through it must be visible through the first mapping and in the object. */
  int64_t mva2 = vm_mmap(pr.p, 0, 4096, VM_PROT_READ | VM_PROT_WRITE,
                         VM_MAP_SHARED, pr.fd, moff);
  EXPECT_EQ((mva2 > 0), 1);
  if (mva2 > 0) {
    EXPECT_EQ((int)(mva2 != mva), 1);
    const uint8_t one[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    EXPECT_EQ(vm_kwrite(pr.p, (uint64_t)mva2, one, 8), 0);
    for (int i = 0; i < 8; i++) got[i] = 0;
    EXPECT_EQ(nv_rm_mock_read_bo(DDEV, h, 0, got, 8), 0);
    diff = 0;
    for (int i = 0; i < 8; i++)
      if (got[i] != one[i]) diff++;
    EXPECT_EQ(diff, 0);
    for (int i = 0; i < 8; i++) got[i] = 0;
    EXPECT_EQ(vm_user_read(pr.p, (uint64_t)mva, got, 8), 0);
    diff = 0;
    for (int i = 0; i < 8; i++)
      if (got[i] != one[i]) diff++;
    EXPECT_EQ(diff, 0);
  }

  /* --- teardown: the mapping must not outlive the object --- */
  struct nvshim_drm_channel_free cf;
  cf.channel = ch;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_CHANNEL_FREE, &cf, (int)sizeof cf), 0);
  struct nvshim_drm_gem_close gc;
  gc.handle = h;
  gc.pad = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_CLOSE, &gc, (int)sizeof gc), 0);
  /* GEM_CLOSE took both mappings down before the frames went back to the pool;
   * either VA now answers -EFAULT instead of reading a recycled frame. */
  EXPECT_EQ(vm_user_read(pr.p, (uint64_t)mva, got, 8), -1);
  EXPECT_EQ((int)(vm_region_find(pr.as, (uint64_t)mva) == 0), 1);
  if (mva2 > 0) {
    EXPECT_EQ(vm_user_read(pr.p, (uint64_t)mva2, got, 8), -1);
    EXPECT_EQ((int)(vm_region_find(pr.as, (uint64_t)mva2) == 0), 1);
  }

  uart_puts("  [NVSHIM] cpu-mapped BO: fault+write+kernel-view+exec digest: PASS\n");
  drm_probe_down(&pr);
  EXPECT_EQ(nv_rm_mock_live(), 0);
}

/* Every shape a client can get wrong, answered by name.  The point of the
 * MAPPABLE-without-CPU and NO_SHARE cases is that they are refused rather than
 * silently dropped: a domain is a promise, and this node only makes the ones it
 * keeps. */
static void test_nvshim_drm_cpu_map_refusals(void) {
  uart_puts("  Running test_nvshim_drm_cpu_map_refusals...\n");
  tests_run++;

  struct drm_probe pr;
  EXPECT_EQ(drm_probe_up(&pr, 96), 0);
  if (!pr.up) return;

  struct nvshim_drm_vm_init vmi;
  vmi.kernel_managed_addr = 1ull << 39;
  vmi.kernel_managed_size = 1ull << 39;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_INIT, &vmi, (int)sizeof vmi), 0);

  struct nvshim_drm_gem_new gn;
  for (unsigned i = 0; i < sizeof(gn); i++) ((unsigned char *)&gn)[i] = 0;
  gn.info.size = 4096;

  gn.info.domain = NVSHIM_DOMAIN_VRAM | NVSHIM_DOMAIN_MAPPABLE;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_NEW, &gn, (int)sizeof gn), -EINVAL);
  /* WP-PRIME: NO_SHARE is ACCEPTED at allocation now and refused at EXPORT
   * (test_nvshim_drm_prime_refusals).  Before PRIME existed there was no
   * export to refuse, so the bit was refused here; the hint now has somewhere
   * to be honoured, and refusing the allocation would be the wrong refusal. */
  gn.info.domain = NVSHIM_DOMAIN_NO_SHARE | NVSHIM_DOMAIN_CPU | NVSHIM_DOMAIN_MAPPABLE;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_NEW, &gn, (int)sizeof gn), 0);
  uint32_t nsh = gn.info.handle;
  EXPECT_EQ((int)(nsh != 0 && nsh != 0xFFFFFFFFu), 1);
  gn.info.domain = NVSHIM_DOMAIN_COHERENT; /* a coherence promise, no mapping */
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_NEW, &gn, (int)sizeof gn), -EINVAL);
  gn.info.domain = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_NEW, &gn, (int)sizeof gn), -EINVAL);

  /* The accepted shape, COHERENT included: pool memory the device already sees,
   * so "coherent" is a fact here rather than a flag to honour. */
  gn.info.domain = NVSHIM_DOMAIN_CPU | NVSHIM_DOMAIN_MAPPABLE | NVSHIM_DOMAIN_COHERENT;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_NEW, &gn, (int)sizeof gn), 0);
  uint32_t mh = gn.info.handle;
  uint64_t mo = gn.info.map_handle;
  EXPECT_EQ((int)(mo == ((uint64_t)mh << 12)), 1);

  /* A plain VRAM object: real memory (EXEC can read it), no CPU mapping. */
  gn.info.domain = NVSHIM_DOMAIN_VRAM;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_NEW, &gn, (int)sizeof gn), 0);
  uint32_t vh = gn.info.handle;
  EXPECT_EQ((int)gn.info.map_handle, 0);
  uint64_t vphys = 0, vlen = 0;
  EXPECT_EQ(nv_rm_mock_if()->mem_phys(DDEV, vh, &vphys, &vlen), -ENOTSUP);
  EXPECT_EQ((int)(vphys == 0), 1); /* nothing filled in on a refusal */

  /* mmap shapes. */
  EXPECT_EQ(vm_mmap(pr.p, 0, 4096, VM_PROT_READ | VM_PROT_WRITE, VM_MAP_SHARED,
                    pr.fd, 0), -EINVAL);                   /* offset 0: no object */
  EXPECT_EQ(vm_mmap(pr.p, 0, 4096, VM_PROT_READ | VM_PROT_WRITE, VM_MAP_SHARED,
                    pr.fd, mo + 1), -EINVAL);              /* unaligned offset */
  EXPECT_EQ(vm_mmap(pr.p, 0, 4096, VM_PROT_READ | VM_PROT_WRITE, VM_MAP_PRIVATE,
                    pr.fd, mo), -EINVAL);                  /* a copy is not a mapping */
  EXPECT_EQ(vm_mmap(pr.p, 0, 4096, 0, VM_MAP_SHARED, pr.fd, mo), -EINVAL);
  EXPECT_EQ(vm_mmap(pr.p, 0, 4096, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC,
                    VM_MAP_SHARED, pr.fd, mo), -EINVAL);
  EXPECT_EQ(vm_mmap(pr.p, 0, 8192, VM_PROT_READ | VM_PROT_WRITE, VM_MAP_SHARED,
                    pr.fd, mo), -EINVAL);                  /* past the object */
  EXPECT_EQ(vm_mmap(pr.p, 0, 4096, VM_PROT_READ | VM_PROT_WRITE, VM_MAP_SHARED,
                    pr.fd, (uint64_t)vh << 12), -EINVAL);  /* domain said no */
  EXPECT_EQ(vm_mmap(pr.p, 0, 4096, VM_PROT_READ | VM_PROT_WRITE, VM_MAP_SHARED,
                    pr.fd, (uint64_t)(mh + 777) << 12), -EINVAL); /* foreign */

  /* A read-only mapping is legal; the protection is the mapping's, so a store
   * through it fails and a read works. */
  int64_t ro = vm_mmap(pr.p, 0, 4096, VM_PROT_READ, VM_MAP_SHARED, pr.fd, mo);
  EXPECT_EQ((ro > 0), 1);
  if (ro > 0) {
    const uint8_t eight[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t got[8];
    EXPECT_EQ(vm_kwrite(pr.p, (uint64_t)ro, eight, 8), -1);
    for (int i = 0; i < 8; i++) got[i] = 0x77;
    EXPECT_EQ(vm_user_read(pr.p, (uint64_t)ro, got, 8), 0);
    int nz = 0;
    for (int i = 0; i < 8; i++)
      if (got[i]) nz++;
    EXPECT_EQ(nz, 0); /* untouched by the refused store */
  }

  struct nvshim_drm_gem_close gc;
  gc.handle = mh;
  gc.pad = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_CLOSE, &gc, (int)sizeof gc), 0);
  gc.handle = nsh; /* the NO_SHARE object from above */
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_CLOSE, &gc, (int)sizeof gc), 0);
  gc.handle = vh;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_CLOSE, &gc, (int)sizeof gc), 0);

  drm_probe_down(&pr);
  EXPECT_EQ(nv_rm_mock_live(), 0);
}

/* ============================================================================
 *  6b. PRIME / dma-buf (WP-PRIME): a GEM object <-> a real fd, same pages.
 *
 *  What these suites are FOR.  A compositor or a second process takes a buffer
 *  by mmap'ing a dma-buf fd, and the ONLY thing that makes that useful is that
 *  the pages are the exporting object's pages -- not a copy, not a snapshot.
 *  So every direction below is checked twice: once through the other mapping,
 *  and once through nv_rm_mock_read_bo(), which is the RM's own view of the
 *  object and therefore cannot be fooled by a mapping that lies.
 *
 *  What passing does NOT mean: nothing here says the driver can RENDER.  This
 *  is the ABI rung that lets a winsys present when there is something to
 *  present; the engine-class wall (need=gsp_rm) is untouched by it.
 * ========================================================================== */

/* Both PRIME directions through the real route, with the struct in the probe's
 * own user pages (so the copy-in/copy-out path is exercised, not bypassed). */
static int prime_export(struct drm_probe *pr, uint32_t handle, uint32_t flags,
                        int *fdp) {
  struct nvshim_drm_prime_handle ph;
  ph.handle = handle;
  ph.flags = flags;
  ph.fd = -1;
  int rc = drm_ioc(pr, NVSHIM_DRM_IOC_PRIME_HANDLE_TO_FD, &ph, (int)sizeof ph);
  if (rc == 0) *fdp = ph.fd;
  return rc;
}

static int prime_import(struct drm_probe *pr, int ufd, uint32_t flags,
                        uint32_t *handlep) {
  struct nvshim_drm_prime_handle ph;
  ph.handle = 0xFFFFFFFFu;
  ph.flags = flags;
  ph.fd = ufd;
  int rc = drm_ioc(pr, NVSHIM_DRM_IOC_PRIME_FD_TO_HANDLE, &ph, (int)sizeof ph);
  if (rc == 0) *handlep = ph.handle;
  return rc;
}

/* Compare a PRIME refusal's name (it is copied into a buffer, nv_shim.h). */
static int prime_name_is(int why, const char *want) {
  char b[NVSHIM_PRIME_REASON_MAX];
  nv_shim_prime_refusal_name(why, b, (int)sizeof b);
  int i = 0;
  while (want[i] && b[i] == want[i]) i++;
  return want[i] == 0 && b[i] == 0;
}

/* A CPU|MAPPABLE object of `size` bytes; returns its handle and map_handle. */
static int prime_make_bo(struct drm_probe *pr, uint64_t size, uint32_t domain,
                         uint32_t *hp, uint64_t *moffp) {
  struct nvshim_drm_gem_new gn;
  for (unsigned i = 0; i < sizeof(gn); i++) ((unsigned char *)&gn)[i] = 0;
  gn.info.domain = domain;
  gn.info.size = size;
  gn.info.handle = 0xFFFFFFFFu;
  int rc = drm_ioc(pr, NVSHIM_DRM_IOC_GEM_NEW, &gn, (int)sizeof gn);
  if (rc != 0) return rc;
  if (hp) *hp = gn.info.handle;
  if (moffp) *moffp = gn.info.map_handle;
  return 0;
}

#define PRIME_PAT_A 0x5Au
#define PRIME_PAT_B 0xC3u

static void test_nvshim_drm_prime_export(void) {
  uart_puts("  Running test_nvshim_drm_prime_export...\n");
  tests_run++;

  /* The struct is the uAPI's own 12 bytes and the encoded sizes are the two
   * DRM_IOWR() numbers a libdrm client sends.  Asserted here as literals so a
   * header edit that shifts them fails loudly. */
  EXPECT_EQ((int)sizeof(struct nvshim_drm_prime_handle), 12);
  EXPECT_EQ((int)NVSHIM_DRM_IOC_PRIME_HANDLE_TO_FD, (int)0xC00C642Du);
  EXPECT_EQ((int)NVSHIM_DRM_IOC_PRIME_FD_TO_HANDLE, (int)0xC00C642Eu);
  EXPECT_EQ(nv_shim_drm_req_size(NVSHIM_DRM_IOC_PRIME_HANDLE_TO_FD), 12);
  EXPECT_EQ(nv_shim_drm_req_size(NVSHIM_DRM_IOC_PRIME_FD_TO_HANDLE), 12);
  /* ... and they are SERVED now, so the named-refusal table has no row for
   * them.  (They were the last two -ENOSYS rows of the old WP-F contract.) */
  EXPECT_EQ((int)(nv_shim_drm_refusal_name(NVSHIM_DRM_IOC_PRIME_HANDLE_TO_FD) == 0), 1);
  EXPECT_EQ((int)(nv_shim_drm_refusal_name(NVSHIM_DRM_IOC_PRIME_FD_TO_HANDLE) == 0), 1);
  /* The 'N' face's DEVICE_ALLOC advertises what the node can do: the PRIME
   * bit must be set, or a client asking first would be told otherwise. */
  EXPECT_EQ((nv_shim_features() & NVSHIM_FEAT_PRIME) != 0, 1);

  struct drm_probe pr;
  EXPECT_EQ(drm_probe_up(&pr, 97), 0);
  if (!pr.up) return;

  struct nvshim_drm_vm_init vmi;
  vmi.kernel_managed_addr = 1ull << 39;
  vmi.kernel_managed_size = 1ull << 39;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_INIT, &vmi, (int)sizeof vmi), 0);

  uint32_t h = 0;
  uint64_t moff = 0;
  EXPECT_EQ(prime_make_bo(&pr, 8192, NVSHIM_DOMAIN_CPU | NVSHIM_DOMAIN_MAPPABLE,
                          &h, &moff), 0);
  EXPECT_EQ((int)(h != 0 && h != 0xFFFFFFFFu), 1);

  /* --- export: a REAL fd in this process's table --- */
  int ufd = -1;
  EXPECT_EQ(prime_export(&pr, h, 0, &ufd), 0);
  EXPECT_EQ((int)(ufd >= 0), 1);
  EXPECT_EQ((int)(ufd != pr.fd), 1);
  if (ufd < 0) {
    drm_probe_down(&pr);
    return;
  }
  /* "Real" is measured, not assumed: the fd answers a file query, it is in the
   * fd table as a dma-buf, and its object resolves back to the export. */
  uint32_t ofl = 0;
  EXPECT_EQ(file_open_flags(pr.p, ufd, &ofl), 0);
  EXPECT_EQ((int)((ofl & 3u) == 2u), 1); /* O_RDWR, what DRM_RDWR produces */
  EXPECT_EQ(file_is_dmabuf(pr.p, ufd), 1);
  void *obj = file_dmabuf_obj(pr.p, ufd);
  EXPECT_EQ((int)(obj != 0), 1);
  /* One reference for the GEM handle entry, one for the export. */
  EXPECT_EQ(nv_shim_test_prime_refs(obj), 2);

  /* --- the two mappings --- */
  int64_t va_h = vm_mmap(pr.p, 0, 8192, VM_PROT_READ | VM_PROT_WRITE,
                         VM_MAP_SHARED, pr.fd, moff);
  EXPECT_EQ((va_h > 0), 1);
  int64_t va_fd = vm_mmap(pr.p, 0, 8192, VM_PROT_READ | VM_PROT_WRITE,
                          VM_MAP_SHARED, ufd, 0);
  EXPECT_EQ((va_fd > 0), 1);
  if (va_h <= 0 || va_fd <= 0) {
    drm_probe_down(&pr);
    return;
  }
  EXPECT_EQ((int)(va_h != va_fd), 1);
  /* Both are the ordinary shared region over an object: same region kind, an
   * object page list, and (below) the same frames. */
  struct vm_region *rh = vm_region_find(pr.as, (uint64_t)va_h);
  struct vm_region *rf = vm_region_find(pr.as, (uint64_t)va_fd);
  EXPECT_EQ((int)(rh && rf && rh->kind == VMK_SHARED && rf->kind == VMK_SHARED), 1);
  EXPECT_EQ((int)(rh && rf && rh->obj != 0 && rf->obj != 0), 1);

  uint8_t pat_a[256], got[256];
  for (int i = 0; i < 256; i++) pat_a[i] = (uint8_t)(PRIME_PAT_A ^ ((i * 11 + 3) & 0xFF));

  /* DIRECTION 1: store through the FD's mapping, read through the HANDLE's. */
  EXPECT_EQ(vm_kwrite(pr.p, (uint64_t)va_fd, pat_a, 256), 0);
  for (int i = 0; i < 256; i++) got[i] = 0;
  EXPECT_EQ(vm_user_read(pr.p, (uint64_t)va_h, got, 256), 0);
  int diff = 0;
  for (int i = 0; i < 256; i++)
    if (got[i] != pat_a[i]) diff++;
  EXPECT_EQ(diff, 0);
  /* ... and through the RM's own view of the object: the mapping is the
   * object, not a copy of it. */
  for (int i = 0; i < 256; i++) got[i] = 0;
  EXPECT_EQ(nv_rm_mock_read_bo(DDEV, h, 0, got, 256), 0);
  diff = 0;
  for (int i = 0; i < 256; i++)
    if (got[i] != pat_a[i]) diff++;
  EXPECT_EQ(diff, 0);

  /* DIRECTION 2: store through the HANDLE's mapping at a non-zero offset, read
   * through the FD's.  An offset-shifted copy cannot pass both directions. */
  uint8_t pat_b[64];
  for (int i = 0; i < 64; i++) pat_b[i] = (uint8_t)(PRIME_PAT_B + i);
  EXPECT_EQ(vm_kwrite(pr.p, (uint64_t)va_h + 64, pat_b, 64), 0);
  for (int i = 0; i < 64; i++) got[i] = 0;
  EXPECT_EQ(vm_user_read(pr.p, (uint64_t)va_fd + 64, got, 64), 0);
  diff = 0;
  for (int i = 0; i < 64; i++)
    if (got[i] != pat_b[i]) diff++;
  EXPECT_EQ(diff, 0);
  for (int i = 0; i < 64; i++) got[i] = 0;
  EXPECT_EQ(nv_rm_mock_read_bo(DDEV, h, 64, got, 64), 0);
  diff = 0;
  for (int i = 0; i < 64; i++)
    if (got[i] != pat_b[i]) diff++;
  EXPECT_EQ(diff, 0);

  /* The bytes NOT written are still the object's zeroes: the two mappings are
   * one allocation, not two buffers that happened to be filled. */
  for (int i = 0; i < 64; i++) got[i] = 0xFF;
  EXPECT_EQ(nv_rm_mock_read_bo(DDEV, h, 4096, got, 64), 0);
  int nz = 0;
  for (int i = 0; i < 64; i++)
    if (got[i]) nz++;
  EXPECT_EQ(nz, 0);

  /* A second export of the same handle is its own fd and its own reference. */
  int ufd2 = -1;
  EXPECT_EQ(prime_export(&pr, h, NVSHIM_DRM_PRIME_CLOEXEC, &ufd2), 0);
  EXPECT_EQ((int)(ufd2 >= 0 && ufd2 != ufd), 1);
  EXPECT_EQ(nv_shim_test_prime_refs(obj), 3);
  if (ufd2 >= 0) EXPECT_EQ(file_close(pr.p, ufd2), 0);
  EXPECT_EQ(nv_shim_test_prime_refs(obj), 2);

  /* --- teardown: the fd is a file, so closing it is the release --- */
  EXPECT_EQ(file_close(pr.p, ufd), 0);
  EXPECT_EQ(file_is_dmabuf(pr.p, ufd), 0);
  /* The handle still holds the object, so the FD's mapping is gone but the
   * object's pages are not: the handle's mapping still reads the pattern. */
  EXPECT_EQ(vm_user_read(pr.p, (uint64_t)va_fd, got, 256), -1);
  for (int i = 0; i < 256; i++) got[i] = 0;
  EXPECT_EQ(vm_user_read(pr.p, (uint64_t)va_h, got, 256), 0);
  /* The expected content is pat_a with direction 2's pat_b laid over
   * [64,128): both writes went through mappings of this one object. */
  diff = 0;
  for (int i = 0; i < 256; i++) {
    uint8_t want = (i >= 64 && i < 128) ? pat_b[i - 64] : pat_a[i];
    if (got[i] != want) diff++;
  }
  EXPECT_EQ(diff, 0);

  struct nvshim_drm_gem_close gc;
  gc.handle = h;
  gc.pad = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_CLOSE, &gc, (int)sizeof gc), 0);
  EXPECT_EQ(vm_user_read(pr.p, (uint64_t)va_h, got, 8), -1);

  drm_probe_down(&pr);
  EXPECT_EQ(nv_rm_mock_live(), 0);
}

static void test_nvshim_drm_prime_import(void) {
  uart_puts("  Running test_nvshim_drm_prime_import...\n");
  tests_run++;

  struct drm_probe pr;
  EXPECT_EQ(drm_probe_up(&pr, 98), 0);
  if (!pr.up) return;
  struct drm_probe pr2;
  /* A DIFFERENT tgid: the AS pool is indexed by it (GB124-AS collision), and
   * two probes here are two real clients of the same node.  Must stay below
   * MAX_PROCESSES (192 on ARM). */
  EXPECT_EQ(drm_probe_up(&pr2, 100), 0);
  if (!pr2.up) {
    drm_probe_down(&pr);
    return;
  }

  struct nvshim_drm_vm_init vmi;
  vmi.kernel_managed_addr = 1ull << 39;
  vmi.kernel_managed_size = 1ull << 39;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_INIT, &vmi, (int)sizeof vmi), 0);
  EXPECT_EQ(drm_ioc(&pr2, NVSHIM_DRM_IOC_VM_INIT, &vmi, (int)sizeof vmi), 0);

  uint32_t h = 0;
  uint64_t moff = 0;
  EXPECT_EQ(prime_make_bo(&pr, 8192, NVSHIM_DOMAIN_CPU | NVSHIM_DOMAIN_MAPPABLE,
                          &h, &moff), 0);
  int ufd = -1;
  EXPECT_EQ(prime_export(&pr, h, 0, &ufd), 0);
  if (ufd < 0) {
    drm_probe_down(&pr2);
    drm_probe_down(&pr);
    return;
  }
  void *obj = file_dmabuf_obj(pr.p, ufd);
  EXPECT_EQ((int)(obj != 0), 1);
  EXPECT_EQ(nv_shim_test_prime_refs(obj), 2);

  /* --- import in the SAME client --- */
  uint32_t h2 = 0;
  EXPECT_EQ(prime_import(&pr, ufd, 0, &h2), 0);
  /* Handles are RM-global on this node: the import returns the object's own
   * handle number (nv_drm_uapi.h, "PRIME contract", documents the divergence
   * from Linux's per-file namespace). */
  EXPECT_EQ((int)h2, (int)h);
  EXPECT_EQ(nv_shim_test_prime_refs(obj), 3);
  struct nvshim_drm_gem_info gi;
  for (unsigned i = 0; i < sizeof(gi); i++) ((unsigned char *)&gi)[i] = 0xFF;
  gi.handle = h2;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_INFO, &gi, (int)sizeof gi), 0);
  EXPECT_EQ((int)gi.size, 8192);
  EXPECT_EQ(gi.map_handle, moff);
  EXPECT_EQ(gi.domain, NVSHIM_DOMAIN_CPU | NVSHIM_DOMAIN_MAPPABLE);

  /* The imported handle maps the same pages... */
  int64_t va_h2 = vm_mmap(pr.p, 0, 8192, VM_PROT_READ | VM_PROT_WRITE,
                          VM_MAP_SHARED, pr.fd, moff);
  EXPECT_EQ((va_h2 > 0), 1);
  int64_t va_fd = vm_mmap(pr.p, 0, 8192, VM_PROT_READ | VM_PROT_WRITE,
                          VM_MAP_SHARED, ufd, 0);
  EXPECT_EQ((va_fd > 0), 1);
  uint8_t pat[128], got[128];
  for (int i = 0; i < 128; i++) pat[i] = (uint8_t)(0x21 + i);
  if (va_h2 > 0 && va_fd > 0) {
    EXPECT_EQ(vm_kwrite(pr.p, (uint64_t)va_h2, pat, 128), 0);
    for (int i = 0; i < 128; i++) got[i] = 0;
    EXPECT_EQ(vm_user_read(pr.p, (uint64_t)va_fd, got, 128), 0);
    int diff = 0;
    for (int i = 0; i < 128; i++)
      if (got[i] != pat[i]) diff++;
    EXPECT_EQ(diff, 0);
    for (int i = 0; i < 128; i++) got[i] = 0;
    EXPECT_EQ(nv_rm_mock_read_bo(DDEV, h, 0, got, 128), 0);
    diff = 0;
    for (int i = 0; i < 128; i++)
      if (got[i] != pat[i]) diff++;
    EXPECT_EQ(diff, 0);
  }

  /* --- SCM_RIGHTS: the fd machine hands the SAME file description to a
   * SECOND process, and the import there is a real import of the same pages.
   * These two calls are exactly what unix.c's sendmsg/recvmsg use. --- */
  int gfd = pr.p->open_fds[ufd];
  EXPECT_EQ((int)(gfd >= 0), 1);
  EXPECT_EQ(fs_msg_ref_gfd(gfd), 0);
  int fd2 = fs_msg_install_gfd(pr2.p, gfd);
  EXPECT_EQ((fd2 >= 0), 1);
  if (fd2 >= 0) {
    EXPECT_EQ(file_is_dmabuf(pr2.p, fd2), 1);
    /* The receiver's fd names the same object: no copy was made by passing it. */
    EXPECT_EQ((int)(file_dmabuf_obj(pr2.p, fd2) == obj), 1);

    uint32_t h3 = 0;
    EXPECT_EQ(prime_import(&pr2, fd2, 0, &h3), 0);
    EXPECT_EQ((int)h3, (int)h);
    EXPECT_EQ(nv_shim_test_prime_refs(obj), 4);

    int64_t va2 = vm_mmap(pr2.p, 0, 8192, VM_PROT_READ | VM_PROT_WRITE,
                          VM_MAP_SHARED, fd2, 0);
    EXPECT_EQ((va2 > 0), 1);
    if (va2 > 0 && va_fd > 0) {
      uint8_t pat2[64];
      for (int i = 0; i < 64; i++) pat2[i] = (uint8_t)(0x90 + i);
      /* Write in process 2, read in process 1 through ITS mapping. */
      EXPECT_EQ(vm_kwrite(pr2.p, (uint64_t)va2, pat2, 64), 0);
      for (int i = 0; i < 64; i++) got[i] = 0;
      EXPECT_EQ(vm_user_read(pr.p, (uint64_t)va_fd, got, 64), 0);
      int diff = 0;
      for (int i = 0; i < 64; i++)
        if (got[i] != pat2[i]) diff++;
      EXPECT_EQ(diff, 0);
      for (int i = 0; i < 64; i++) got[i] = 0;
      EXPECT_EQ(nv_rm_mock_read_bo(DDEV, h, 0, got, 64), 0);
      diff = 0;
      for (int i = 0; i < 64; i++)
        if (got[i] != pat2[i]) diff++;
      EXPECT_EQ(diff, 0);
    }

    /* CLOSE ORDER 1 (importer first).  An SCM_RIGHTS pass shares the FILE
     * DESCRIPTION: the receiver's fd is the same description, so closing it
     * drops a reference to the description, not the buffer.  The dma-buf is
     * released when the LAST holder of the description closes -- here, the
     * exporter.  That is why the refcount stays 3 after the receiver's close,
     * and why the receiver's MAPPING survives until the object frees (the
     * mapping is not itself a reference, so it dies with the object). */
    struct nvshim_drm_gem_close gc;
    gc.pad = 0;
    gc.handle = h3; /* the importer's handle */
    EXPECT_EQ(drm_ioc(&pr2, NVSHIM_DRM_IOC_GEM_CLOSE, &gc, (int)sizeof gc), 0);
    EXPECT_EQ(nv_shim_test_prime_refs(obj), 3);
    /* The object is still readable through the RM after the import closed. */
    for (int i = 0; i < 64; i++) got[i] = 0;
    EXPECT_EQ(nv_rm_mock_read_bo(DDEV, h, 0, got, 64), 0);
    /* The receiver drops its fd: the description is still open in the
     * exporter, so nothing is freed and the receiver's mapping still reads. */
    EXPECT_EQ(file_close(pr2.p, fd2), 0);
    EXPECT_EQ(nv_shim_test_prime_refs(obj), 3);
    if (va2 > 0) {
      for (int i = 0; i < 64; i++) got[i] = 0;
      EXPECT_EQ(vm_user_read(pr2.p, (uint64_t)va2, got, 64), 0);
    }

    /* Import in process 1 closed, then its handle: the export fd keeps it. */
    gc.handle = h2;
    EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_CLOSE, &gc, (int)sizeof gc), 0);
    EXPECT_EQ(nv_shim_test_prime_refs(obj), 2);
    gc.handle = h;
    EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_CLOSE, &gc, (int)sizeof gc), 0);
    EXPECT_EQ(nv_shim_test_prime_refs(obj), 1);
    /* ... the object is STILL there: one exported fd is open. */
    for (int i = 0; i < 64; i++) got[i] = 0;
    EXPECT_EQ(nv_rm_mock_read_bo(DDEV, h, 0, got, 64), 0);
    if (va_fd > 0) {
      for (int i = 0; i < 64; i++) got[i] = 0;
      EXPECT_EQ(vm_user_read(pr.p, (uint64_t)va_fd, got, 64), 0);
    }
    /* ... and the last close of the DESCRIPTION frees the frames.  Every
     * mapping of the object goes with it, including the one the receiver made
     * in its own address space (that is the drop_object sweep resolving the
     * group from the stored tgid). */
    EXPECT_EQ(file_close(pr.p, ufd), 0);
    EXPECT_EQ(vm_user_read(pr.p, (uint64_t)va_fd, got, 8), -1);
    if (va2 > 0) EXPECT_EQ(vm_user_read(pr2.p, (uint64_t)va2, got, 8), -1);
    EXPECT_EQ(nv_rm_mock_read_bo(DDEV, h, 0, got, 8), -EINVAL);
  }

  /* CLOSE ORDER 2 (exporter's fd first): a fresh object, exported and mapped,
   * the fd closed FIRST.  The handle keeps the pages, the handle's mapping
   * still works, and the frames go when the handle goes. */
  uint32_t h4 = 0;
  uint64_t mo4 = 0;
  EXPECT_EQ(prime_make_bo(&pr, 4096, NVSHIM_DOMAIN_CPU | NVSHIM_DOMAIN_MAPPABLE,
                          &h4, &mo4), 0);
  int ufd4 = -1;
  EXPECT_EQ(prime_export(&pr, h4, 0, &ufd4), 0);
  EXPECT_EQ(nv_shim_test_prime_refs(file_dmabuf_obj(pr.p, ufd4)), 2);
  int64_t va4 = vm_mmap(pr.p, 0, 4096, VM_PROT_READ | VM_PROT_WRITE,
                        VM_MAP_SHARED, ufd4, 0);
  EXPECT_EQ((va4 > 0), 1);
  uint8_t b4[16];
  for (int i = 0; i < 16; i++) b4[i] = (uint8_t)(0x40 + i);
  if (va4 > 0) EXPECT_EQ(vm_kwrite(pr.p, (uint64_t)va4, b4, 16), 0);
  EXPECT_EQ(file_close(pr.p, ufd4), 0); /* fd first */
  if (va4 > 0) EXPECT_EQ(vm_user_read(pr.p, (uint64_t)va4, got, 16), -1);
  for (int i = 0; i < 16; i++) got[i] = 0;
  EXPECT_EQ(nv_rm_mock_read_bo(DDEV, h4, 0, got, 16), 0);
  int diff4 = 0;
  for (int i = 0; i < 16; i++)
    if (got[i] != b4[i]) diff4++;
  EXPECT_EQ(diff4, 0);
  int64_t va4h = vm_mmap(pr.p, 0, 4096, VM_PROT_READ | VM_PROT_WRITE,
                         VM_MAP_SHARED, pr.fd, mo4);
  EXPECT_EQ((va4h > 0), 1);
  struct nvshim_drm_gem_close gc4;
  gc4.handle = h4;
  gc4.pad = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_CLOSE, &gc4, (int)sizeof gc4), 0);
  if (va4h > 0) EXPECT_EQ(vm_user_read(pr.p, (uint64_t)va4h, got, 8), -1);
  EXPECT_EQ(nv_rm_mock_read_bo(DDEV, h4, 0, got, 8), -EINVAL);

  /* CLOSE ORDER 3 (client teardown with an fd out): the node fd closes first,
   * which destroys the client and every handle it held -- and the exported fd
   * still holds the pages, so they must NOT be freed.  This is the case that
   * proves the reference is on the OBJECT and not on the client. */
  uint32_t h5 = 0;
  EXPECT_EQ(prime_make_bo(&pr, 4096, NVSHIM_DOMAIN_CPU | NVSHIM_DOMAIN_MAPPABLE,
                          &h5, 0), 0);
  int ufd5 = -1;
  EXPECT_EQ(prime_export(&pr, h5, 0, &ufd5), 0);
  void *obj5 = file_dmabuf_obj(pr.p, ufd5);
  EXPECT_EQ(nv_shim_test_prime_refs(obj5), 2);
  EXPECT_EQ(file_close(pr.p, pr.fd), 0); /* the node fd: teardown */
  pr.fd = -1;
  EXPECT_EQ(nv_shim_test_prime_refs(obj5), 1);
  for (int i = 0; i < 16; i++) got[i] = 0;
  EXPECT_EQ(nv_rm_mock_read_bo(DDEV, h5, 0, got, 16), 0); /* pages alive */
  EXPECT_EQ(file_close(pr.p, ufd5), 0);
  EXPECT_EQ(nv_rm_mock_read_bo(DDEV, h5, 0, got, 16), -EINVAL); /* freed */

  drm_probe_down(&pr2);
  drm_probe_down(&pr);
  EXPECT_EQ(nv_rm_mock_live(), 0);
}

static void test_nvshim_drm_prime_refusals(void) {
  uart_puts("  Running test_nvshim_drm_prime_refusals...\n");
  tests_run++;

  /* The refusal names are part of the contract: a client in a log has to be able
   * to read WHY.  The name is copied (see nv_shim.h), so this compares it. */
  EXPECT_EQ(prime_name_is(NVSHIM_PRIME_NO_SHARE, "no_share"), 1);
  EXPECT_EQ(prime_name_is(NVSHIM_PRIME_CROSS_DEVICE, "cross_device"), 1);
  EXPECT_EQ(prime_name_is(NVSHIM_PRIME_NOT_DMABUF, "not_a_dmabuf"), 1);
  EXPECT_EQ(prime_name_is(NVSHIM_PRIME_NOT_MAPPABLE,
                          "domain_never_offered_a_cpu_mapping"), 1);
  EXPECT_EQ(prime_name_is(NVSHIM_PRIME_BAD_FLAGS, "flags"), 1);
  EXPECT_EQ(prime_name_is(77, "unknown"), 1);

  struct drm_probe pr;
  EXPECT_EQ(drm_probe_up(&pr, 99), 0);
  if (!pr.up) return;

  struct nvshim_drm_vm_init vmi;
  vmi.kernel_managed_addr = 1ull << 39;
  vmi.kernel_managed_size = 1ull << 39;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_INIT, &vmi, (int)sizeof vmi), 0);

  /* --- NO_SHARE: the object is MADE, maps locally, and cannot be exported ---
   * (Before WP-PRIME the bit was refused at GEM_NEW because there was no
   * export to refuse; this is the row that changed, and it changed because the
   * hint now has somewhere to be honoured.) */
  uint32_t nh = 0;
  uint64_t nmoff = 0;
  EXPECT_EQ(prime_make_bo(&pr, 4096,
                          NVSHIM_DOMAIN_CPU | NVSHIM_DOMAIN_MAPPABLE |
                              NVSHIM_DOMAIN_NO_SHARE,
                          &nh, &nmoff), 0);
  EXPECT_EQ((int)(nmoff == ((uint64_t)nh << 12)), 1);
  int64_t nva = vm_mmap(pr.p, 0, 4096, VM_PROT_READ | VM_PROT_WRITE,
                        VM_MAP_SHARED, pr.fd, nmoff);
  EXPECT_EQ((nva > 0), 1); /* a private object still maps for its owner */
  int nfd = -1;
  EXPECT_EQ(prime_export(&pr, nh, 0, &nfd), -EINVAL);
  EXPECT_EQ(nfd, -1);
  struct nvshim_drm_gem_close gc;
  gc.pad = 0;
  gc.handle = nh;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_CLOSE, &gc, (int)sizeof gc), 0);

  /* --- a domain with no CPU mapping: EXPORT is fine, MMAP of the fd is named
   * (a device-visible buffer is exactly what PRIME shares) --- */
  uint32_t vh = 0;
  EXPECT_EQ(prime_make_bo(&pr, 4096, NVSHIM_DOMAIN_VRAM, &vh, 0), 0);
  int vfd = -1;
  EXPECT_EQ(prime_export(&pr, vh, 0, &vfd), 0);
  EXPECT_EQ((vfd >= 0), 1);
  if (vfd >= 0) {
    void *vobj = file_dmabuf_obj(pr.p, vfd);
    EXPECT_EQ((int)(vobj != 0), 1);
    EXPECT_EQ(vm_mmap(pr.p, 0, 4096, VM_PROT_READ | VM_PROT_WRITE, VM_MAP_SHARED,
                      vfd, 0), -EINVAL);
    /* ... and memory_offset 0 is the only offset a dma-buf fd accepts. */
    EXPECT_EQ(vm_mmap(pr.p, 0, 4096, VM_PROT_READ | VM_PROT_WRITE, VM_MAP_SHARED,
                      vfd, 4096), -EINVAL);
    /* Import works without a CPU mapping: a device-visible buffer can be bound
     * by the importer, which is the whole point. */
    uint32_t vh2 = 0;
    EXPECT_EQ(prime_import(&pr, vfd, 0, &vh2), 0);
    EXPECT_EQ((int)vh2, (int)vh);
    struct nvshim_drm_gem_close gv;
    gv.handle = vh2;
    gv.pad = 0;
    EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_CLOSE, &gv, (int)sizeof gv), 0);
    EXPECT_EQ(file_close(pr.p, vfd), 0);
  }
  gc.handle = vh;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_CLOSE, &gc, (int)sizeof gc), 0);

  /* --- the shapes that are refused before any work is done --- */
  uint32_t ch = 0;
  EXPECT_EQ(prime_make_bo(&pr, 4096, NVSHIM_DOMAIN_CPU | NVSHIM_DOMAIN_MAPPABLE,
                          &ch, 0), 0);
  int bad = -1;
  EXPECT_EQ(prime_export(&pr, ch, 0x80u, &bad), -EINVAL);   /* unknown flag */
  EXPECT_EQ(prime_export(&pr, ch + 777u, 0, &bad), -EINVAL); /* foreign handle */
  EXPECT_EQ(prime_export(&pr, 0, 0, &bad), -EINVAL);         /* no such handle */
  uint32_t hh = 0;
  EXPECT_EQ(prime_import(&pr, -1, 0, &hh), -EINVAL);
  EXPECT_EQ(prime_import(&pr, pr.fd, 0, &hh), -EINVAL); /* the node is not a buf */
  EXPECT_EQ(prime_import(&pr, 999, 0, &hh), -EINVAL);   /* no such fd */

  int cfd = -1;
  EXPECT_EQ(prime_export(&pr, ch, 0, &cfd), 0);
  if (cfd >= 0) {
    EXPECT_EQ(prime_import(&pr, cfd, NVSHIM_DRM_PRIME_CLOEXEC, &hh), -EINVAL);
    /* --- cross-device: the one branch userspace cannot reach on this node.
     * The mock has a single device, so the seam lies about the exporter's dev;
     * the import must answer the NAMED refusal instead of importing memory the
     * RM could not attach.  The dev is restored before the close, because the
     * release path frees the object through the device it recorded. --- */
    void *cobj = file_dmabuf_obj(pr.p, cfd);
    EXPECT_EQ((int)(cobj != 0), 1);
    int refs_before = nv_shim_test_prime_refs(cobj);
    EXPECT_EQ(nv_shim_test_prime_set_dev(cobj, 99), 0);
    EXPECT_EQ(prime_import(&pr, cfd, 0, &hh), -EINVAL);
    EXPECT_EQ(prime_import(&pr, cfd, 0, &hh), -EINVAL); /* still refused */
    /* The refs seam resolves the object through the buffer's recorded device,
     * so it is read after restoring it: a refusal must not have taken a
     * reference, and with the real device back the count is what it was. */
    EXPECT_EQ(nv_shim_test_prime_set_dev(cobj, DDEV), 0);
    EXPECT_EQ(nv_shim_test_prime_refs(cobj), refs_before); /* no ref leaked */
    /* ... and with its real device it imports, so the refusal was about the
     * device and nothing else. */
    EXPECT_EQ(prime_import(&pr, cfd, 0, &hh), 0);
    struct nvshim_drm_gem_close gh;
    gh.handle = hh;
    gh.pad = 0;
    EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_CLOSE, &gh, (int)sizeof gh), 0);
    EXPECT_EQ(file_close(pr.p, cfd), 0);
  }
  gc.handle = ch;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_CLOSE, &gc, (int)sizeof gc), 0);

  /* --- every OTHER named refusal keeps its row: the table did not lose
   * anything when PRIME left it --- */
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_GEM_FLINK, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_SYNCOBJ_HANDLE_TO_FD, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_GEM_PUSHBUF, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_SETPARAM, 0), -ENOSYS);
  EXPECT_EQ((int)(nv_shim_drm_refusal_name(NVSHIM_DRM_IOC_GEM_FLINK) != 0), 1);
  EXPECT_EQ((int)(nv_shim_drm_refusal_name(NVSHIM_DRM_IOC_SYNCOBJ_TIMELINE_WAIT) != 0), 1);
  /* ... and the two PRIME numbers are served, not refused-by-size: a wrong
   * encoded size is -EINVAL (the number exists), never -ENOSYS. */
  EXPECT_EQ(nv_shim_ioctl(pr.p, pr.fd, NVSHIM_DRM_IOC_ANY(0x2d, 16), pr.base), -EINVAL);
  EXPECT_EQ(nv_shim_drm_req_size(NVSHIM_DRM_IOC_ANY(0x2d, 16)), 12);

  drm_probe_down(&pr);
  EXPECT_EQ(nv_rm_mock_live(), 0);
}

/* ============================================================================
 *  8-11. NVIF (WP-NVIF): the object model, driven through the real route.
 *
 *  These suites call nv_shim_ioctl() exactly as the compat ioctl route does,
 *  with every struct in the probe process's own user pages and every encoded
 *  size built by NVSHIM_DRM_IOC_NVIF_LEN() -- so the variable-length rule, the
 *  per-type shape rules and the object table are all exercised, not just the
 *  handlers' bookkeeping.
 *
 *  What passing does NOT mean: no engine object was created (every one is
 *  refused), so nothing here says a GR context exists.  The refusal IS the
 *  result: -ENOSYS with a row naming the class id is the wall a stock winsys
 *  reports, and these suites pin the wall's position.
 * ========================================================================== */

#define NVIF_BUF (DREQ_BASE + 0x2000) /* 4 KiB of the probe region */
#define NVIF_HDR 24                   /* sizeof(struct nvshim_nvif_hdr) */

static int nvif_hdr_put(struct drm_probe *pr, uint32_t type, uint8_t route,
                        uint64_t token, uint64_t object, uint8_t version) {
  struct nvshim_nvif_hdr h;
  for (unsigned i = 0; i < sizeof h; i++) ((unsigned char *)&h)[i] = 0;
  h.version = version;
  h.type = (uint8_t)type;
  h.owner = NVSHIM_NVIF_OWNER_ANY;
  h.route = route;
  h.token = token;
  h.object = object;
  return vm_kwrite(pr->p, NVIF_BUF, &h, (int)sizeof h);
}

static int nvif_call(struct drm_probe *pr, uint32_t len) {
  return nv_shim_ioctl(pr->p, pr->fd, NVSHIM_DRM_IOC_NVIF_LEN(len), NVIF_BUF);
}

/* The exact sequence a winsys runs for its device object: NEW(route 0, class
 * NV_DEVICE, cookie, one nv_device_v0 of class data).  72 bytes. */
static int nvif_new_device(struct drm_probe *pr, uint64_t cookie) {
  struct nvshim_nvif_new nw;
  struct nvshim_device_v0 dv;
  for (unsigned i = 0; i < sizeof nw; i++) ((unsigned char *)&nw)[i] = 0;
  for (unsigned i = 0; i < sizeof dv; i++) ((unsigned char *)&dv)[i] = 0;
  nw.object = cookie;
  nw.oclass = NVSHIM_NVIF_CLASS_DEVICE;
  nw.route = NVSHIM_NVIF_ROUTE_NVIF;
  nw.token = cookie;
  nw.handle = 0;
  dv.device = ~0ull;
  if (nvif_hdr_put(pr, NVSHIM_NVIF_NEW, NVSHIM_NVIF_ROUTE_NVIF, 0, 0, 0) != 0)
    return -1000;
  if (vm_kwrite(pr->p, NVIF_BUF + NVIF_HDR, &nw, (int)sizeof nw) != 0) return -1001;
  if (vm_kwrite(pr->p, NVIF_BUF + NVIF_HDR + (int)sizeof nw, &dv, (int)sizeof dv) != 0)
    return -1002;
  return nvif_call(pr, NVIF_HDR + (uint32_t)sizeof nw + (uint32_t)sizeof dv);
}

/* A child object under a channel: route 0xff, token = channel. */
static int nvif_new_child(struct drm_probe *pr, uint32_t ch, int32_t oclass,
                          uint64_t cookie, uint32_t len) {
  struct nvshim_nvif_new nw;
  for (unsigned i = 0; i < sizeof nw; i++) ((unsigned char *)&nw)[i] = 0;
  nw.object = cookie;
  nw.oclass = oclass;
  nw.route = NVSHIM_NVIF_ROUTE_NVIF;
  nw.token = cookie;
  if (nvif_hdr_put(pr, NVSHIM_NVIF_NEW, NVSHIM_NVIF_ROUTE_HIDDEN, ch, 0, 0) != 0)
    return -1000;
  if (vm_kwrite(pr->p, NVIF_BUF + NVIF_HDR, &nw, (int)sizeof nw) != 0) return -1001;
  return nvif_call(pr, len);
}

/* MTHD on an object cookie.  The payload is `len - 32` bytes at +32. */
static int nvif_mthd(struct drm_probe *pr, uint64_t cookie, uint8_t method,
                     uint32_t len, uint8_t info_version) {
  struct nvshim_nvif_mthd m;
  struct nvshim_device_info_v0 info;
  for (unsigned i = 0; i < sizeof m; i++) ((unsigned char *)&m)[i] = 0;
  for (unsigned i = 0; i < sizeof info; i++) ((unsigned char *)&info)[i] = 0xa5;
  m.method = method;
  info.version = info_version;
  if (nvif_hdr_put(pr, NVSHIM_NVIF_MTHD, NVSHIM_NVIF_ROUTE_NVIF, 0, cookie, 0) != 0)
    return -1000;
  if (vm_kwrite(pr->p, NVIF_BUF + NVIF_HDR, &m, (int)sizeof m) != 0) return -1001;
  if (len > NVIF_HDR + (uint32_t)sizeof m &&
      vm_kwrite(pr->p, NVIF_BUF + NVIF_HDR + (int)sizeof m, &info,
                (int)sizeof info) != 0)
    return -1002;
  return nvif_call(pr, len);
}

static void test_nvshim_nvif_layout(void) {
  uart_puts("  Running test_nvshim_nvif_layout...\n");
  tests_run++;

  /* nvif/ioctl.h: the header is 24 bytes and these are its field offsets. */
  EXPECT_EQ((int)sizeof(struct nvshim_nvif_hdr), 24);
  EXPECT_EQ(OFF(struct nvshim_nvif_hdr, type), 1);
  EXPECT_EQ(OFF(struct nvshim_nvif_hdr, owner), 6);
  EXPECT_EQ(OFF(struct nvshim_nvif_hdr, route), 7);
  EXPECT_EQ(OFF(struct nvshim_nvif_hdr, token), 8);
  EXPECT_EQ(OFF(struct nvshim_nvif_hdr, object), 16);
  EXPECT_EQ((int)sizeof(struct nvshim_nvif_sclass), 8);
  EXPECT_EQ((int)sizeof(struct nvshim_nvif_sclass_oclass), 8);
  EXPECT_EQ((int)sizeof(struct nvshim_nvif_new), 32);
  EXPECT_EQ(OFF(struct nvshim_nvif_new, route), 7);
  EXPECT_EQ(OFF(struct nvshim_nvif_new, token), 8);
  EXPECT_EQ(OFF(struct nvshim_nvif_new, object), 16);
  EXPECT_EQ(OFF(struct nvshim_nvif_new, handle), 24);
  EXPECT_EQ(OFF(struct nvshim_nvif_new, oclass), 28);
  EXPECT_EQ((int)sizeof(struct nvshim_nvif_mthd), 8);
  EXPECT_EQ(OFF(struct nvshim_nvif_mthd, method), 1);

  /* cl0080.h */
  EXPECT_EQ((int)sizeof(struct nvshim_device_v0), 16);
  EXPECT_EQ((int)sizeof(struct nvshim_device_info_v0), 104);
  EXPECT_EQ(OFF(struct nvshim_device_info_v0, platform), 1);
  EXPECT_EQ(OFF(struct nvshim_device_info_v0, chipset), 2);
  EXPECT_EQ(OFF(struct nvshim_device_info_v0, revision), 4);
  EXPECT_EQ(OFF(struct nvshim_device_info_v0, family), 5);
  EXPECT_EQ(OFF(struct nvshim_device_info_v0, ram_size), 8);
  EXPECT_EQ(OFF(struct nvshim_device_info_v0, ram_user), 16);
  EXPECT_EQ(OFF(struct nvshim_device_info_v0, chip), 24);
  EXPECT_EQ(OFF(struct nvshim_device_info_v0, name), 40);
  EXPECT_EQ((int)sizeof(struct nvshim_device_time_v0), 16);

  /* Type numbers and the route/owner constants, as literals. */
  EXPECT_EQ((int)NVSHIM_NVIF_SCLASS, 0x01);
  EXPECT_EQ((int)NVSHIM_NVIF_NEW, 0x02);
  EXPECT_EQ((int)NVSHIM_NVIF_DEL, 0x03);
  EXPECT_EQ((int)NVSHIM_NVIF_MTHD, 0x04);
  EXPECT_EQ((int)NVSHIM_NVIF_MAP, 0x07);
  EXPECT_EQ((int)NVSHIM_NVIF_UNMAP, 0x08);
  EXPECT_EQ((int)NVSHIM_NVIF_ROUTE_NVIF, 0x00);
  EXPECT_EQ((int)NVSHIM_NVIF_ROUTE_HIDDEN, 0xff);
  EXPECT_EQ((int)NVSHIM_NVIF_OWNER_ANY, 0xff);
  EXPECT_EQ((int)NVSHIM_DEVICE_V0_INFO, 0x00);
  EXPECT_EQ((int)NVSHIM_DEVICE_V0_TIME, 0x01);
  EXPECT_EQ((int)NVSHIM_DEVICE_V0_GCX_READY, 0x02);
  EXPECT_EQ((int)NVSHIM_DEVICE_INFO_V0_PCIE, 0x03);
  EXPECT_EQ((int)NVSHIM_DEVICE_INFO_V0_ADA, 0x0e);

  /* class.h -- the class ids this node reports and refuses. */
  EXPECT_EQ((int)NVSHIM_NVIF_CLASS_DEVICE, 0x00000080);
  EXPECT_EQ((int)NVSHIM_NVIF_CLASS_FERMI_TWOD_A, 0x0000902d);
  EXPECT_EQ((int)NVSHIM_NVIF_CLASS_KEPLER_INLINE_TO_MEM_B, 0x0000a140);
  EXPECT_EQ((int)NVSHIM_NVIF_CLASS_AMPERE_B, 0x0000c797);
  EXPECT_EQ((int)NVSHIM_NVIF_CLASS_AMPERE_COMPUTE_B, 0x0000c7c0);
  EXPECT_EQ((int)NVSHIM_NVIF_CLASS_AMPERE_DMA_COPY_A, 0x0000c6b5);
  EXPECT_EQ((int)NVSHIM_NVIF_CLASS_AMPERE_DMA_COPY_B, 0x0000c7b5);

  /* The request encodes the BUFFER LENGTH, magic 'd', nr 0x47. */
  EXPECT_EQ((int)NVSHIM_NVIF_MIN_SIZE, 24);
  EXPECT_EQ((int)NVSHIM_DRM_SIZE(NVSHIM_DRM_IOC_NVIF_LEN(136)), 136);
  EXPECT_EQ((int)NVSHIM_DRM_NR(NVSHIM_DRM_IOC_NVIF_LEN(136)), 0x47);
  EXPECT_EQ((int)NVSHIM_DRM_TYPE(NVSHIM_DRM_IOC_NVIF_LEN(136)), 0x64);
  EXPECT_EQ((int)NVSHIM_DRM_DIR(NVSHIM_DRM_IOC_NVIF_LEN(136)), 3);
  /* ... and it is not in the refusal table any more. */
  EXPECT_EQ((int)(nv_shim_drm_refusal_name(NVSHIM_DRM_IOC_NVIF_LEN(136)) != 0), 0);
}

static void test_nvshim_nvif_device(void) {
  uart_puts("  Running test_nvshim_nvif_device...\n");
  tests_run++;

  struct drm_probe pr;
  EXPECT_EQ(drm_probe_up(&pr, 121), 0);
  if (!pr.up) return;

  const uint64_t cookie = 0x1000;
  /* A device object, exactly as nouveau_ws_device_alloc sends it. */
  EXPECT_EQ(nvif_new_device(&pr, cookie), 0);
  /* The same cookie twice is nouveau's -EEXIST, not a second object. */
  EXPECT_EQ(nvif_new_device(&pr, cookie), -EEXIST);
  /* A different cookie is a different object. */
  EXPECT_EQ(nvif_new_device(&pr, cookie + 8), 0);

  /* MTHD NV_DEVICE_V0_INFO: 136 bytes, and the payload says who the card is. */
  EXPECT_EQ(nvif_mthd(&pr, cookie, NVSHIM_DEVICE_V0_INFO,
                      NVIF_HDR + 8 + (uint32_t)sizeof(struct nvshim_device_info_v0), 0),
            0);
  struct nvshim_device_info_v0 info;
  EXPECT_EQ(vm_user_read(pr.p, NVIF_BUF + NVIF_HDR + 8, &info, (int)sizeof info), 0);
  EXPECT_EQ((int)info.version, 0);
  EXPECT_EQ((int)info.platform, NVSHIM_DEVICE_INFO_V0_PCIE);
  EXPECT_EQ((int)info.chipset, 0x192);
  EXPECT_EQ((int)info.revision, 0xa1);
  EXPECT_EQ((int)info.family, NVSHIM_DEVICE_INFO_V0_ADA);
  EXPECT_EQ((info.ram_size == info.ram_user), 1);
  EXPECT_EQ((int)(info.ram_user >> 32), 1); /* 4 GiB: the mock's arena */
  EXPECT_EQ((int)info.chip[0], 'A');
  EXPECT_EQ((int)info.chip[1], 'D');
  EXPECT_EQ((int)info.chip[3], '0');
  EXPECT_EQ((int)info.chip[4], '2'); /* "AD102" */
  EXPECT_EQ((int)info.chip[5], 0);
  EXPECT_EQ((int)info.chip[6], 0);
  EXPECT_EQ((int)info.name[0], 'N');

  /* The payload's own version must be 0, and its length must be exactly one
   * nv_device_info_v0 -- 135 and 137 are both -EINVAL (nouveau's rule). */
  EXPECT_EQ(nvif_mthd(&pr, cookie, NVSHIM_DEVICE_V0_INFO, 136, 1), -EINVAL);
  EXPECT_EQ(nvif_mthd(&pr, cookie, NVSHIM_DEVICE_V0_INFO, 135, 0), -EINVAL);
  EXPECT_EQ(nvif_mthd(&pr, cookie, NVSHIM_DEVICE_V0_INFO, 137, 0), -EINVAL);

  /* TIME needs the card's PTIMER; GCX_READY needs the GSP.  Both named. */
  EXPECT_EQ(nvif_mthd(&pr, cookie, NVSHIM_DEVICE_V0_TIME, NVIF_HDR + 8 + 16, 0),
            -ENOSYS);
  EXPECT_EQ(nvif_mthd(&pr, cookie, NVSHIM_DEVICE_V0_GCX_READY, NVIF_HDR + 8 + 16, 0),
            -ENOSYS);
  /* An unknown method is -EINVAL, and so is a method on an object that was
   * never created (the lookup is the gate, not the method number). */
  EXPECT_EQ(nvif_mthd(&pr, cookie, 9, NVIF_HDR + 8 + 16, 0), -EINVAL);
  EXPECT_EQ(nvif_mthd(&pr, cookie + 0x40, NVSHIM_DEVICE_V0_INFO, 136, 0), -EINVAL);

  /* NEW shapes nouveau rejects: a parent named on route 0, an engine class on
   * route 0, a zero cookie, a channel that does not exist. */
  struct nvshim_nvif_new nw;
  struct nvshim_device_v0 dv;
  for (unsigned i = 0; i < sizeof nw; i++) ((unsigned char *)&nw)[i] = 0;
  for (unsigned i = 0; i < sizeof dv; i++) ((unsigned char *)&dv)[i] = 0;
  nw.object = 0x2000;
  nw.oclass = NVSHIM_NVIF_CLASS_DEVICE;
  EXPECT_EQ(nvif_hdr_put(&pr, NVSHIM_NVIF_NEW, NVSHIM_NVIF_ROUTE_NVIF, 0, 7, 0), 0);
  EXPECT_EQ(vm_kwrite(pr.p, NVIF_BUF + NVIF_HDR, &nw, (int)sizeof nw), 0);
  EXPECT_EQ(vm_kwrite(pr.p, NVIF_BUF + NVIF_HDR + (int)sizeof nw, &dv, (int)sizeof dv), 0);
  EXPECT_EQ(nvif_call(&pr, 72), -EINVAL); /* ioctl->object != 0 */

  nw.oclass = NVSHIM_NVIF_CLASS_AMPERE_B;
  EXPECT_EQ(nvif_hdr_put(&pr, NVSHIM_NVIF_NEW, NVSHIM_NVIF_ROUTE_NVIF, 0, 0, 0), 0);
  EXPECT_EQ(vm_kwrite(pr.p, NVIF_BUF + NVIF_HDR, &nw, (int)sizeof nw), 0);
  EXPECT_EQ(nvif_call(&pr, 56), -EINVAL); /* a GR class is not a root object */

  nw.oclass = NVSHIM_NVIF_CLASS_DEVICE;
  nw.object = 0;
  EXPECT_EQ(vm_kwrite(pr.p, NVIF_BUF + NVIF_HDR, &nw, (int)sizeof nw), 0);
  EXPECT_EQ(nvif_call(&pr, 56), -EINVAL); /* zero cookie */

  EXPECT_EQ(nvif_new_child(&pr, 1, NVSHIM_NVIF_CLASS_AMPERE_B, 0x3000, 56), -EINVAL);

  /* Header-level rules: version, route, type, and a buffer shorter than the
   * header itself.  Four shapes, four refusals, all -EINVAL. */
  EXPECT_EQ(nvif_hdr_put(&pr, NVSHIM_NVIF_DEL, NVSHIM_NVIF_ROUTE_NVIF, 0, cookie, 1), 0);
  EXPECT_EQ(nvif_call(&pr, 24), -EINVAL); /* header version != 0 */
  EXPECT_EQ(nvif_hdr_put(&pr, NVSHIM_NVIF_DEL, 0x55, 0, cookie, 0), 0);
  EXPECT_EQ(nvif_call(&pr, 24), -EINVAL); /* route not 0x00/0xff */
  EXPECT_EQ(nvif_hdr_put(&pr, 0x09, NVSHIM_NVIF_ROUTE_NVIF, 0, cookie, 0), 0);
  EXPECT_EQ(nvif_call(&pr, 24), -EINVAL); /* no such type */
  EXPECT_EQ(nvif_hdr_put(&pr, NVSHIM_NVIF_NEW, NVSHIM_NVIF_ROUTE_NVIF, 0, 0, 0), 0);
  EXPECT_EQ(nvif_call(&pr, 8), -EINVAL); /* shorter than the header */

  /* DEL: 24 bytes exactly, route 0; an unknown cookie is NOT an error. */
  EXPECT_EQ(nvif_hdr_put(&pr, NVSHIM_NVIF_DEL, NVSHIM_NVIF_ROUTE_NVIF, 0, cookie, 0), 0);
  EXPECT_EQ(nvif_call(&pr, 32), -EINVAL); /* payload where none is allowed */
  EXPECT_EQ(nvif_call(&pr, 24), 0);
  /* ... and the object is gone: the same MTHD is now -EINVAL. */
  EXPECT_EQ(nvif_mthd(&pr, cookie, NVSHIM_DEVICE_V0_INFO, 136, 0), -EINVAL);
  EXPECT_EQ(nvif_hdr_put(&pr, NVSHIM_NVIF_DEL, NVSHIM_NVIF_ROUTE_NVIF, 0, cookie, 0), 0);
  EXPECT_EQ(nvif_call(&pr, 24), 0); /* second DEL of the same cookie */

  drm_probe_down(&pr);
  EXPECT_EQ(nv_rm_mock_live(), 0);
}

static void test_nvshim_nvif_sclass(void) {
  uart_puts("  Running test_nvshim_nvif_sclass...\n");
  tests_run++;

  struct drm_probe pr;
  EXPECT_EQ(drm_probe_up(&pr, 122), 0);
  if (!pr.up) return;

  /* No channel yet: SCLASS names a parent that does not exist. */
  EXPECT_EQ(nvif_hdr_put(&pr, NVSHIM_NVIF_SCLASS, NVSHIM_NVIF_ROUTE_HIDDEN, 1, 0, 0),
            0);
  struct nvshim_nvif_sclass sc;
  for (unsigned i = 0; i < sizeof sc; i++) ((unsigned char *)&sc)[i] = 0;
  sc.count = 16;
  EXPECT_EQ(vm_kwrite(pr.p, NVIF_BUF + NVIF_HDR, &sc, (int)sizeof sc), 0);
  EXPECT_EQ(nvif_call(&pr, 160), -EINVAL);

  /* route 0x00 is not a valid route for SCLASS (nouveau: `if (!route)`). */
  EXPECT_EQ(nvif_hdr_put(&pr, NVSHIM_NVIF_SCLASS, NVSHIM_NVIF_ROUTE_NVIF, 1, 0, 0), 0);
  EXPECT_EQ(nvif_call(&pr, 160), -EINVAL);

  /* A channel, in the exact shape a stock winsys asks for: {~0, GR} -- the
   * legacy context-DMA fields used as the engine selector, normalised to 0. */
  struct nvshim_drm_vm_init vmi;
  vmi.kernel_managed_addr = 1ull << 39;
  vmi.kernel_managed_size = 1ull << 39;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_INIT, &vmi, (int)sizeof vmi), 0);

  struct nvshim_drm_channel_alloc ca;
  for (unsigned i = 0; i < sizeof ca; i++) ((unsigned char *)&ca)[i] = 0;
  ca.fb_ctxdma_handle = 0xFFFFFFFFu;      /* "no legacy ctxdma" */
  ca.tt_ctxdma_handle = NVSHIM_ENGINE_GR; /* ... use this as the selector  */
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_CHANNEL_ALLOC, &ca, (int)sizeof ca), 0);
  EXPECT_EQ((int)ca.fb_ctxdma_handle, 0);
  EXPECT_EQ((int)ca.tt_ctxdma_handle, 0);
  EXPECT_EQ((ca.channel > 0), 1);
  uint32_t ch = (uint32_t)ca.channel;

  /* The class list: count is what the channel HAS, entries are class.h's. */
  EXPECT_EQ(nvif_hdr_put(&pr, NVSHIM_NVIF_SCLASS, NVSHIM_NVIF_ROUTE_HIDDEN, ch, 0, 0),
            0);
  sc.count = 16;
  EXPECT_EQ(vm_kwrite(pr.p, NVIF_BUF + NVIF_HDR, &sc, (int)sizeof sc), 0);
  EXPECT_EQ(nvif_call(&pr, 160), 0);
  EXPECT_EQ(vm_user_read(pr.p, NVIF_BUF + NVIF_HDR, &sc, (int)sizeof sc), 0);
  EXPECT_EQ((int)sc.count, 6);
  struct nvshim_nvif_sclass_oclass list[NVSHIM_NVIF_MAX_SCLASS];
  EXPECT_EQ(vm_user_read(pr.p, NVIF_BUF + NVIF_HDR + 8, list, (int)sizeof list), 0);
  EXPECT_EQ((int)list[0].oclass, NVSHIM_NVIF_CLASS_FERMI_TWOD_A);
  EXPECT_EQ((int)list[1].oclass, NVSHIM_NVIF_CLASS_KEPLER_INLINE_TO_MEM_B);
  EXPECT_EQ((int)list[2].oclass, NVSHIM_NVIF_CLASS_AMPERE_B);
  EXPECT_EQ((int)list[3].oclass, NVSHIM_NVIF_CLASS_AMPERE_COMPUTE_B);
  EXPECT_EQ((int)list[4].oclass, NVSHIM_NVIF_CLASS_AMPERE_DMA_COPY_A);
  EXPECT_EQ((int)list[5].oclass, NVSHIM_NVIF_CLASS_AMPERE_DMA_COPY_B);
  EXPECT_EQ((int)list[2].minver, -1);
  EXPECT_EQ((int)list[2].maxver, -1);
  /* The winsys's own match: the highest class whose low byte is 0x97. */
  EXPECT_EQ((int)(list[2].oclass & 0xff), 0x97);

  /* A caller with room for two entries gets two entries and the true count
   * (nouveau reports the real number and truncates the array). */
  EXPECT_EQ(nvif_hdr_put(&pr, NVSHIM_NVIF_SCLASS, NVSHIM_NVIF_ROUTE_HIDDEN, ch, 0, 0),
            0);
  sc.count = 2;
  EXPECT_EQ(vm_kwrite(pr.p, NVIF_BUF + NVIF_HDR, &sc, (int)sizeof sc), 0);
  struct nvshim_nvif_sclass_oclass two[2];
  two[0].oclass = 0x7e7e;
  two[1].oclass = 0x7e7e;
  EXPECT_EQ(vm_kwrite(pr.p, NVIF_BUF + NVIF_HDR + 8, two, (int)sizeof two), 0);
  EXPECT_EQ(nvif_call(&pr, NVIF_HDR + 8 + (uint32_t)sizeof two), 0);
  EXPECT_EQ(vm_user_read(pr.p, NVIF_BUF + NVIF_HDR + 8, two, (int)sizeof two), 0);
  EXPECT_EQ((int)two[0].oclass, NVSHIM_NVIF_CLASS_FERMI_TWOD_A);
  EXPECT_EQ((int)two[1].oclass, NVSHIM_NVIF_CLASS_KEPLER_INLINE_TO_MEM_B);
  EXPECT_EQ(vm_user_read(pr.p, NVIF_BUF + NVIF_HDR, &sc, (int)sizeof sc), 0);
  EXPECT_EQ((int)sc.count, 6);

  /* Shape rules: count must match the buffer, and must fit in 16 entries. */
  EXPECT_EQ(nvif_hdr_put(&pr, NVSHIM_NVIF_SCLASS, NVSHIM_NVIF_ROUTE_HIDDEN, ch, 0, 0),
            0);
  sc.count = 16;
  EXPECT_EQ(vm_kwrite(pr.p, NVIF_BUF + NVIF_HDR, &sc, (int)sizeof sc), 0);
  EXPECT_EQ(nvif_call(&pr, NVIF_HDR + 8 + 8), -EINVAL); /* 16 claimed, 1 present */
  sc.count = 17;
  EXPECT_EQ(vm_kwrite(pr.p, NVIF_BUF + NVIF_HDR, &sc, (int)sizeof sc), 0);
  EXPECT_EQ(nvif_call(&pr, NVIF_HDR + 8 + 17 * 8), -EINVAL);
  sc.count = 0;
  EXPECT_EQ(vm_kwrite(pr.p, NVIF_BUF + NVIF_HDR, &sc, (int)sizeof sc), 0);
  EXPECT_EQ(nvif_call(&pr, NVIF_HDR + 8), 0); /* asks nothing, learns the count */

  /* A channel this client does not own. */
  EXPECT_EQ(nvif_hdr_put(&pr, NVSHIM_NVIF_SCLASS, NVSHIM_NVIF_ROUTE_HIDDEN, ch + 99, 0, 0),
            0);
  sc.count = 0;
  EXPECT_EQ(vm_kwrite(pr.p, NVIF_BUF + NVIF_HDR, &sc, (int)sizeof sc), 0);
  EXPECT_EQ(nvif_call(&pr, NVIF_HDR + 8), -EINVAL);

  /* After CHANNEL_FREE the parent is gone and so is its class list. */
  struct nvshim_drm_channel_free cf;
  cf.channel = (int32_t)ch;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_CHANNEL_FREE, &cf, (int)sizeof cf), 0);
  EXPECT_EQ(nvif_hdr_put(&pr, NVSHIM_NVIF_SCLASS, NVSHIM_NVIF_ROUTE_HIDDEN, ch, 0, 0),
            0);
  sc.count = 0;
  EXPECT_EQ(vm_kwrite(pr.p, NVIF_BUF + NVIF_HDR, &sc, (int)sizeof sc), 0);
  EXPECT_EQ(nvif_call(&pr, NVIF_HDR + 8), -EINVAL);

  drm_probe_down(&pr);
  EXPECT_EQ(nv_rm_mock_live(), 0);
}

static void test_nvshim_nvif_engine(void) {
  uart_puts("  Running test_nvshim_nvif_engine...\n");
  tests_run++;

  struct drm_probe pr;
  EXPECT_EQ(drm_probe_up(&pr, 123), 0);
  if (!pr.up) return;

  struct nvshim_drm_vm_init vmi;
  vmi.kernel_managed_addr = 1ull << 39;
  vmi.kernel_managed_size = 1ull << 39;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_VM_INIT, &vmi, (int)sizeof vmi), 0);

  /* Engine selectors the mock has no channel for are refused by name; only
   * {~0, GR} is a channel this node can make. */
  struct nvshim_drm_channel_alloc ca;
  for (unsigned i = 0; i < sizeof ca; i++) ((unsigned char *)&ca)[i] = 0;
  ca.fb_ctxdma_handle = 0xFFFFFFFFu;
  ca.tt_ctxdma_handle = 0x30; /* NOUVEAU_FIFO_ENGINE_CE */
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_CHANNEL_ALLOC, &ca, (int)sizeof ca), -ENOSYS);
  ca.tt_ctxdma_handle = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_CHANNEL_ALLOC, &ca, (int)sizeof ca), 0);
  uint32_t ch = (uint32_t)ca.channel;
  /* The notifier object is an RM object too. */
  ca.notifier_handle = 1;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_CHANNEL_ALLOC, &ca, (int)sizeof ca), -ENOSYS);
  ca.notifier_handle = 0;

  EXPECT_EQ(nvif_new_device(&pr, 0x5000), 0);

  /* Every class SCLASS reports is refused with -ENOSYS -- the wall.  A real
   * GR/CE context object is what a real RM would allocate here. */
  const int32_t classes[] = {
      NVSHIM_NVIF_CLASS_FERMI_TWOD_A,      NVSHIM_NVIF_CLASS_KEPLER_INLINE_TO_MEM_B,
      NVSHIM_NVIF_CLASS_AMPERE_B,          NVSHIM_NVIF_CLASS_AMPERE_COMPUTE_B,
      NVSHIM_NVIF_CLASS_AMPERE_DMA_COPY_A, NVSHIM_NVIF_CLASS_AMPERE_DMA_COPY_B,
  };
  for (unsigned i = 0; i < sizeof classes / sizeof classes[0]; i++)
    EXPECT_EQ(nvif_new_child(&pr, ch, classes[i], 0x6000 + i, 56), -ENOSYS);

  /* A class the chip does not have is -EINVAL, not a refusal: the class check
   * is a lookup, so it can come out negative. */
  EXPECT_EQ(nvif_new_child(&pr, ch, 0x00001234, 0x7000, 56), -EINVAL);
  /* And a DEVICE object cannot be made under a channel either. */
  EXPECT_EQ(nvif_new_child(&pr, ch, NVSHIM_NVIF_CLASS_DEVICE, 0x7008, 56), -EINVAL);

  /* The engine class object is NOT created: DEL of its cookie finds nothing,
   * and the shim's own table never grew. */
  EXPECT_EQ(nvif_hdr_put(&pr, NVSHIM_NVIF_DEL, NVSHIM_NVIF_ROUTE_NVIF, 0, 0x6002, 0), 0);
  EXPECT_EQ(nvif_call(&pr, 24), 0);

  /* MAP/UNMAP are for objects that have RM memory behind them. */
  struct nvshim_nvif_hdr h;
  for (unsigned i = 0; i < sizeof h; i++) ((unsigned char *)&h)[i] = 0;
  h.owner = NVSHIM_NVIF_OWNER_ANY;
  h.type = NVSHIM_NVIF_MAP;
  h.object = 0x5000;
  EXPECT_EQ(vm_kwrite(pr.p, NVIF_BUF, &h, (int)sizeof h), 0);
  EXPECT_EQ(nvif_call(&pr, 24), -ENOSYS);

  drm_probe_down(&pr);
  EXPECT_EQ(nv_rm_mock_live(), 0);
}

/* ---- 6. GETPARAM GRAPH_UNITS and the engine-class wall ------------------
 *
 * The winsys asks GRAPH_UNITS on the device-create path and treats failure as
 * fatal (Mesa nouveau_device.c: `if (nouveau_ws_param(fd, ...)) goto out_err`),
 * which is why it -- not NVIF -- is the first thing that stopped a stock nvk.
 * The tier holds the ANSWER to a value and the ABSENCE to a name:
 *   - the packing is the Ada/GSP-RM one, (tpc_total << 8) | gpc_nr, and it
 *     stays inside 24 bits (r535_gr_units has no ROP field);
 *   - the numbers are the declared RTX 4090 model (11 GPC / 64 TPC), which is
 *     nvk's 128 SMs once mp_per_tpc = 2 is applied -- the declared model is
 *     the falsifiable kind, not a hedge;
 *   - both faces answer identically: one table, two ioctls;
 *   - an RM with NO topology refuses -ENOSYS instead of answering, and its
 *     reason is mock_has_no_topo rather than the generic mock_has_no_card;
 *   - VRAM_BAR_SIZE is an aperture size, not the amount of VRAM: 0 here, and
 *     never equal to FB_SIZE (bar == vram is nvk's full-BAR fast path).
 * It also pins the engine-class wall's naming: class -> (name, engine, RM
 * allocation), the same table the refusal row prints.
 */
static int str_eq_(const char *a, const char *b) {
  if (!a || !b) return 0;
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static void test_nvshim_getparam_graph_units(void) {
  uart_puts("  Running test_nvshim_getparam_graph_units...\n");
  tests_run++;

  /* The packing, as a pure function: 11 GPC / 64 TPC -> 0x400b, and the ROP
   * count is NOT in the word (the Ada path never sets those bits). */
  struct nv_rm_gpu_topo topo;
  topo.gpc_nr = 11;
  topo.tpc_total = 64;
  topo.rop_nr = 176;
  topo.source = NV_RM_TOPO_DECLARED;
  EXPECT_EQ((int)nv_shim_gr_units_value(&topo), 0x400b);
  EXPECT_EQ((int)(nv_shim_gr_units_value(&topo) >> 32), 0);
  /* Field edges: 8 bits of GPC and 16 bits of TPC, no carry into bit 24. */
  topo.gpc_nr = 0xff;
  topo.tpc_total = 0xffff;
  EXPECT_EQ((int)nv_shim_gr_units_value(&topo), 0xffffffff & 0xffffff);
  /* No model, no number -- even if the fields happen to hold values. */
  topo.gpc_nr = 11;
  topo.tpc_total = 64;
  topo.source = NV_RM_TOPO_NONE;
  EXPECT_EQ((int)nv_shim_gr_units_value(&topo), 0);
  EXPECT_EQ((int)nv_shim_gr_units_value(0), 0);

  /* The wall's named requirement, from the same table the row prints. */
  const char *name = 0, *engine = 0, *alloc = 0;
  EXPECT_EQ(nv_shim_nvif_engine_class(NVSHIM_NVIF_CLASS_AMPERE_B, &name, &engine, &alloc), 0);
  EXPECT_EQ(str_eq_(name, "ampere_b_gr"), 1);
  EXPECT_EQ(str_eq_(engine, "eng3d"), 1);
  EXPECT_EQ(str_eq_(alloc, "rm_alloc"), 1);
  EXPECT_EQ(nv_shim_nvif_engine_class(NVSHIM_NVIF_CLASS_AMPERE_DMA_COPY_B, &name, &engine,
                                      &alloc), 0);
  EXPECT_EQ(str_eq_(alloc, "ce_alloc"), 1);
  EXPECT_EQ(str_eq_(engine, "copy1"), 1);
  EXPECT_EQ(nv_shim_nvif_engine_class(NVSHIM_NVIF_CLASS_KEPLER_INLINE_TO_MEM_B, &name,
                                      &engine, &alloc), 0);
  EXPECT_EQ(str_eq_(engine, "m2mf"), 1);
  EXPECT_EQ(nv_shim_nvif_engine_class(0x00001234, &name, &engine, &alloc), -1);

  /* The refusal REASON is part of the answer: an RM with no topology and a node
   * with no card are different absences, and a client reading the console
   * should not have to guess which one it met. */
  EXPECT_EQ(str_eq_(nv_shim_param_refusal_reason(NVSHIM_PARAM_GRAPH_UNITS),
                    "mock_has_no_topo"), 1);
  EXPECT_EQ(str_eq_(nv_shim_param_refusal_reason(NVSHIM_PARAM_PTIMER_TIME),
                    "mock_has_no_card"), 1);
  EXPECT_EQ(str_eq_(nv_shim_param_refusal_reason(NVSHIM_PARAM_BUS_TYPE),
                    "mock_has_no_card"), 1);

  /* A model that cannot be packed the way nouveau packs it is rejected at the
   * seam, not silently truncated. */
  EXPECT_EQ(nv_rm_mock_set_topo(NV_RM_TOPO_DECLARED, 0, 64, 0), -EINVAL);
  EXPECT_EQ(nv_rm_mock_set_topo(NV_RM_TOPO_DECLARED, 11, 0, 0), -EINVAL);
  EXPECT_EQ(nv_rm_mock_set_topo(7, 11, 64, 0), -EINVAL);
  EXPECT_EQ(nv_rm_mock_set_topo(NV_RM_TOPO_DECLARED, 11, 64, 176), 0);

  struct drm_probe pr;
  EXPECT_EQ(drm_probe_up(&pr, 124), 0);
  if (!pr.up) return;

  /* The declared answer, on the winsys face. */
  struct nvshim_drm_getparam q;
  q.param = NVSHIM_PARAM_GRAPH_UNITS;
  q.value = 0xFFFFFFFFFFFFFFFFull;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GETPARAM, &q, (int)sizeof q), 0);
  EXPECT_EQ((int)q.value, 0x400b);
  EXPECT_EQ((int)(q.value & 0xff), 11);          /* gpc_count */
  EXPECT_EQ((int)((q.value >> 8) & 0xffff), 64); /* tpc_count */
  EXPECT_EQ((int)(q.value >> 24), 0);            /* no ROP field, as on Ada */
  /* nvk's own derivation of shaderSMCount = tpc_count * mp_per_tpc, and
   * mp_per_tpc == 2 for anything >= 0x140 (Mesa nouveau_device.c). */
  EXPECT_EQ((int)(((q.value >> 8) & 0xffff) * 2), 128);

  /* VRAM_BAR_SIZE is an aperture, and this node has none.  FB_SIZE is the
   * arena a client sizes heaps from, and the two must not be conflated. */
  struct nvshim_drm_getparam b;
  b.param = NVSHIM_PARAM_VRAM_BAR_SIZE;
  b.value = 0xFFFFFFFFFFFFFFFFull;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GETPARAM, &b, (int)sizeof b), 0);
  EXPECT_EQ((int)b.value, 0);
  struct nvshim_drm_getparam f;
  f.param = NVSHIM_PARAM_FB_SIZE;
  f.value = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GETPARAM, &f, (int)sizeof f), 0);
  EXPECT_EQ((f.value >= (1ull << 30)), 1);
  EXPECT_EQ((b.value != f.value), 1);

  /* The deprecated 'N' face reads the SAME table: one param number, one
   * answer, whichever magic the client used.  It needs the 'N' request number
   * (magic 'N', not 'd'), and the struct still goes through the probe's user
   * window -- the handler copies from user memory either way. */
  struct nvshim_param nq;
  nq.param = NVSHIM_PARAM_GRAPH_UNITS;
  nq.pad = 0;
  nq.value = 0xFFFFFFFFFFFFFFFFull;
  EXPECT_EQ(drm_ioc_at(&pr, DREQ_STR, NVSHIM_IOC_GET_PARAM, &nq, (int)sizeof nq), 0);
  EXPECT_EQ((int)nq.value, 0x400b);
  nq.param = NVSHIM_PARAM_VRAM_BAR_SIZE;
  nq.value = 0xFFFFFFFFFFFFFFFFull;
  EXPECT_EQ(drm_ioc_at(&pr, DREQ_STR, NVSHIM_IOC_GET_PARAM, &nq, (int)sizeof nq), 0);
  EXPECT_EQ((int)nq.value, 0);

  /* No model: the param refuses, named.  The device's identity is a snapshot
   * taken at open, so the seam change needs a fresh device. */
  drm_probe_down(&pr);
  EXPECT_EQ(nv_rm_mock_set_topo(NV_RM_TOPO_NONE, 0, 0, 0), 0);
  EXPECT_EQ(drm_probe_up(&pr, 125), 0);
  if (pr.up) {
    struct nvshim_drm_getparam r;
    r.param = NVSHIM_PARAM_GRAPH_UNITS;
    r.value = 0xFFFFFFFFFFFFFFFFull;
    EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GETPARAM, &r, (int)sizeof r), -ENOSYS);
    EXPECT_EQ((int)r.value, -1); /* untouched: a refusal writes nothing */
    /* ... and the generic card-reading params still say the card reason. */
    r.param = NVSHIM_PARAM_PTIMER_TIME;
    EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GETPARAM, &r, (int)sizeof r), -ENOSYS);
    drm_probe_down(&pr);
  }

  /* Restore the declared model: the seam is sticky by design. */
  EXPECT_EQ(nv_rm_mock_set_topo(NV_RM_TOPO_DECLARED, 11, 64, 176), 0);
  EXPECT_EQ(nv_rm_mock_live(), 0);
}

void nv_shim_drm_test_suite(void) {
  test_nvshim_drm_layout();
  test_nvshim_drm_refusals();
  test_nvshim_drm_probe();
  test_nvshim_drm_ownership();
  test_nvshim_drm_limits();
  test_nvshim_drm_cpu_map();
  test_nvshim_drm_cpu_map_refusals();
  test_nvshim_drm_prime_export();    /* WP-PRIME: real fd, same pages, both ways */
  test_nvshim_drm_prime_import();    /* WP-PRIME: import, SCM_RIGHTS, close orders */
  test_nvshim_drm_prime_refusals();  /* WP-PRIME: NO_SHARE, cross-device, shapes */
  test_nvshim_nvif_layout();
  test_nvshim_nvif_device();
  test_nvshim_nvif_sclass();
  test_nvshim_nvif_engine();
  test_nvshim_getparam_graph_units();
}

#endif /* KERNEL_MODE_UNIT_TEST */
