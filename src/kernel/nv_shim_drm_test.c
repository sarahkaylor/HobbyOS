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
  EXPECT_EQ(nv_shim_drm_req_size(NVSHIM_DRM_IOC_NVIF), 0);
  EXPECT_EQ(nv_shim_drm_req_size(NVSHIM_DRM_IOC_GEM_PUSHBUF), 0);
  EXPECT_EQ(nv_shim_drm_req_size(NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_NVIF, 8)), 0);
  EXPECT_EQ(nv_shim_drm_req_size(NVSHIM_DRM_IOC_ANY(0x7e, 8)), -1);
  EXPECT_EQ(nv_shim_drm_req_size(NVSHIM_DRM_IOC_ANY(0x50, 8)), 16); /* same nr, size checked later */

  /* Refusal names exist for every refused row, and for nothing served. */
  EXPECT_EQ((int)(nv_shim_drm_refusal_name(NVSHIM_DRM_IOC_NVIF) != 0), 1);
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
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_NVIF, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_SVM_INIT, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_SVM_BIND, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_GET_ZCULL_INFO, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_GEM_PUSHBUF, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_GEM_CPU_PREP, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_GEM_CPU_FINI, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_GEM_FLINK, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_PRIME_HANDLE_TO_FD, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_DRM_IOC_PRIME_FD_TO_HANDLE, 0), -ENOSYS);
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

  /* GEM_NEW geometry: zero size, CPU domain, a domain with no space, a
   * non-arena alignment, a foreign channel hint, over the per-client cap. */
  struct nvshim_drm_gem_new bad;
  for (unsigned i = 0; i < sizeof(bad); i++) ((unsigned char *)&bad)[i] = 0;
  bad.info.domain = NVSHIM_DOMAIN_VRAM;
  bad.info.size = 0;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_NEW, &bad, (int)sizeof bad), -EINVAL);
  bad.info.size = 4096;
  bad.info.domain = NVSHIM_DOMAIN_CPU;
  EXPECT_EQ(drm_ioc(&pr, NVSHIM_DRM_IOC_GEM_NEW, &bad, (int)sizeof bad), -EINVAL);
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

void nv_shim_drm_test_suite(void) {
  test_nvshim_drm_layout();
  test_nvshim_drm_refusals();
  test_nvshim_drm_probe();
  test_nvshim_drm_ownership();
  test_nvshim_drm_limits();
}

#endif /* KERNEL_MODE_UNIT_TEST */
