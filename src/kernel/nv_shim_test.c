/* ============================================================================
 *  nv_shim_test: kernel unit-tier coverage for /dev/nvshim (WP-F, rung N2).
 *
 *  Three layers, deliberately separate:
 *
 *  1. The frozen interface: struct sizes, request numbers and directions, and
 *     the route table (implemented vs refused).  These are the numbers a
 *     winsys compiles against, so they are asserted, not assumed.
 *
 *  2. The refusals: every deliberately-unimplemented row answers -ENOSYS and
 *     is named, with no process and with a garbage argument.  A row that
 *     answered -ENOTTY would be indistinguishable from a typo'd request.
 *
 *  3. The probe -- the gate.  A real process, a real address space and a real
 *     file_open("/dev/nvshim") drive the ioctl path end to end: DEVICE_ALLOC,
 *     GET_PARAM, ALLOC, VM_INIT, VM_BIND, CHANNEL_ALLOC, SUBMIT, FENCE_WAIT.
 *     The fence path is exercised in BOTH directions: with the mock's kick
 *     held, a polled wait must time out; after a kick it must retire.  A test
 *     that only ever saw "retired" would pass against a fence that is true by
 *     construction (PLAN.md §6.5).
 *
 *  What passing here proves: the ABI's bookkeeping, ownership isolation and
 *  fence state machine, with no card.  What it does NOT prove: that any of it
 *  matches what the GPU/RM does -- that is what a real GSP-RM backend and the
 *  rung-N5 enumeration will say.
 * ========================================================================== */

#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"

#include "errno.h"
#include "file_obj.h" /* file_is_devnvshim()/file_is_devnv() */
#include "fs.h"
#include "kmem.h" /* phys_to_virt: the kernel's view of a resolved VA */
#include "nv_rm.h"
#include "nv_shim.h"
#include "nv_nvif_uapi.h" /* NVSHIM_NVIF_CLASS_*: the ids SCLASS reports */
#include "process.h"
#include "vm.h"

extern int process_kill(int pid); /* fs_test precedent: not in process.h */

/* ---- 1. the frozen interface ------------------------------------------- */

static void test_nvshim_layout(void) {
  uart_puts("  Running test_nvshim_layout...\n");
  tests_run++;

  /* Sizes are the user ABI: glibc and the kernel must agree byte for byte. */
  EXPECT_EQ((int)sizeof(struct nvshim_device), 40);
  EXPECT_EQ((int)sizeof(struct nvshim_device_free), 8);
  EXPECT_EQ((int)sizeof(struct nvshim_param), 16);
  EXPECT_EQ((int)sizeof(struct nvshim_alloc), 32);
  EXPECT_EQ((int)sizeof(struct nvshim_free), 8);
  EXPECT_EQ((int)sizeof(struct nvshim_vm_init), 16);
  EXPECT_EQ((int)sizeof(struct nvshim_vm_bind), 40);
  EXPECT_EQ((int)sizeof(struct nvshim_channel), 32);
  EXPECT_EQ((int)sizeof(struct nvshim_channel_free), 8);
  EXPECT_EQ((int)sizeof(struct nvshim_engine_class), 16);
  EXPECT_EQ((int)sizeof(struct nvshim_engine_class_free), 8);
  EXPECT_EQ((int)sizeof(struct nvshim_submit), 32);
  EXPECT_EQ((int)sizeof(struct nvshim_fence_wait), 16);

  /* Numbers 1..11 implemented, 12..17 refused, magic 'N' (0x4E). */
  EXPECT_EQ((int)(NVSHIM_IOC_DEVICE_ALLOC & 0xFF), 1);
  EXPECT_EQ((int)(NVSHIM_IOC_FENCE_WAIT & 0xFF), 11);
  EXPECT_EQ((int)(NVSHIM_IOC_GET_PARAM & 0xFF), 3);
  EXPECT_EQ((int)(NVSHIM_IOC_SUBMIT & 0xFF), 10);
  EXPECT_EQ((int)((NVSHIM_IOC_SUBMIT >> 8) & 0xFF), 'N');
  EXPECT_EQ((int)((NVSHIM_IOC_SUBMIT >> 16) & 0x3FFF), 32);
  EXPECT_EQ((int)(NVSHIM_IOC_SUBMIT >> 30), 3);   /* read|write: fence out */
  EXPECT_EQ((int)(NVSHIM_IOC_VM_BIND >> 30), 1);  /* write only */
  EXPECT_EQ((int)(NVSHIM_IOC_GROBJ & 0xFF), 12);
  EXPECT_EQ((int)(NVSHIM_IOC_SETPARAM & 0xFF), 17);
  /* 18/19 are the mock-RM object spine's rows (engine class alloc/free); the
   * numbering skips 12..17 because those stay the named refusals above. */
  EXPECT_EQ((int)(NVSHIM_IOC_ENGINE_CLASS_ALLOC & 0xFF), 18);
  EXPECT_EQ((int)(NVSHIM_IOC_ENGINE_CLASS_FREE & 0xFF), 19);
  EXPECT_EQ((int)((NVSHIM_IOC_ENGINE_CLASS_ALLOC >> 16) & 0x3FFF),
            (int)sizeof(struct nvshim_engine_class));
  EXPECT_EQ((int)(NVSHIM_IOC_ENGINE_CLASS_FREE >> 30), 1); /* write only */

  /* Route table: implemented rows carry their size, refusals carry 0, and
   * anything else is not a request (-1). */
  EXPECT_EQ(nv_shim_req_size(NVSHIM_IOC_SUBMIT), (int)sizeof(struct nvshim_submit));
  EXPECT_EQ(nv_shim_req_size(NVSHIM_IOC_VM_BIND), (int)sizeof(struct nvshim_vm_bind));
  EXPECT_EQ(nv_shim_req_size(NVSHIM_IOC_ENGINE_CLASS_ALLOC),
            (int)sizeof(struct nvshim_engine_class));
  EXPECT_EQ(nv_shim_req_size(NVSHIM_IOC_ENGINE_CLASS_FREE),
            (int)sizeof(struct nvshim_engine_class_free));
  EXPECT_EQ(nv_shim_req_size(NVSHIM_IOC_NVIF), 0);
  EXPECT_EQ(nv_shim_req_size(NVSHIM_IOC_SETPARAM), 0);
  EXPECT_EQ(nv_shim_req_size(NVSHIM_IOC(NVSHIM_IOC_READ, NVSHIM_IOC_MAGIC, 18, 8)), -1);
  EXPECT_EQ(nv_shim_req_size(NVSHIM_IOW(NVSHIM_IOC_MAGIC, 10, struct nvshim_fence_wait)),
            -1); /* same number, wrong size: not SUBMIT */

  /* Same number, different magic: not ours (a DRM-ish 'd' request). */
  EXPECT_EQ(nv_shim_req_size(NVSHIM_IOWR('d', 1, struct nvshim_device)), -1);

  /* Refusal names exist for every refused row and for nothing else. */
  EXPECT_EQ((int)(nv_shim_refusal_name(NVSHIM_IOC_NVIF) != 0), 1);
  EXPECT_EQ((int)(nv_shim_refusal_name(NVSHIM_IOC_GEM_PUSHBUF) != 0), 1);
  EXPECT_EQ((int)(nv_shim_refusal_name(NVSHIM_IOC_SUBMIT) != 0), 0);
  EXPECT_EQ((int)(nv_shim_refusal_name(NVSHIM_IOC(NVSHIM_IOC_READ, NVSHIM_IOC_MAGIC, 18, 8)) != 0), 0);
  EXPECT_EQ((int)(nv_shim_refusal_name(NVSHIM_IOC_DEVICE_ALLOC) != 0), 0);

  /* Alignment helper: powers of two, never rounded down, no wrap. */
  EXPECT_EQ((int)nv_shim_align_up(1, 4096), 4096);
  EXPECT_EQ((int)nv_shim_align_up(4096, 4096), 4096);
  EXPECT_EQ((int)nv_shim_align_up(4097, 4096), 8192);
  EXPECT_EQ(nv_shim_align_up(~0ull, 4096), 0ull); /* would wrap: 0, not a small number */
}

/* ---- 2. refusals are named, not silent --------------------------------- */

static void test_nvshim_refusals(void) {
  uart_puts("  Running test_nvshim_refusals...\n");
  tests_run++;

  /* No process, no argument: the refusal is about the request, not the call. */
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_IOC_GROBJ, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_IOC_NOTIFIER, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_IOC_NVIF, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_IOC_SVM_INIT, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_IOC_GEM_PUSHBUF, 0), -ENOSYS);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_IOC_SETPARAM, 0), -ENOSYS);

  /* An implemented row with no process is a bad call (-EINVAL), and a number
   * that is not a request at all is -ENOTTY: three different answers for
   * three different mistakes, which is the point. */
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_IOC_SUBMIT, 0), -EINVAL);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_IOC_DEVICE_ALLOC, 0), -EINVAL);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_IOC_ENGINE_CLASS_ALLOC, 0), -EINVAL);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_IOC_ENGINE_CLASS_FREE, 0), -EINVAL);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_IOC(NVSHIM_IOC_READ, NVSHIM_IOC_MAGIC, 99, 8), 0), -ENOTTY);
  EXPECT_EQ(nv_shim_ioctl(0, -1, NVSHIM_IOWR('g', 8, struct nvshim_submit), 0), -ENOTTY);

  /* The node itself is present without hardware. */
  EXPECT_EQ(nv_shim_present(), 1);
  EXPECT_EQ((nv_shim_features() & NVSHIM_FEAT_RM_MOCK) != 0, 1);
  EXPECT_EQ((nv_shim_features() & NVSHIM_FEAT_FENCE) != 0, 1);
}

/* ---- the probe harness ------------------------------------------------- */

struct shim_probe {
  int pid;
  struct process *p;
  struct addr_space *as;
  int fd;
  uint64_t base;    /* request structs live here (user VA) */
  uint64_t cs_base; /* the command stream lives here */
  int old_pid;
  int up;
};

#define PROBE_BASE USER_MMAP_BASE_V2
#define PROBE_CS (USER_MMAP_BASE_V2 + 0x1000)

static int probe_up(struct shim_probe *pr, uint64_t tgid) {
  for (unsigned i = 0; i < sizeof(*pr); i++) ((unsigned char *)pr)[i] = 0;
  pr->fd = -1;
  pr->pid = process_create();
  if (pr->pid < 0) return -1;
  pr->p = process_get_pcb(pr->pid);
  if (!pr->p) return -1;
  pr->up = 1; /* from here probe_down() is the cleanup */
  pr->p->is_kernel_process = 1;
  pr->old_pid = cpu_current_pids[0];
  set_current_process_pid(0, pr->pid);
  pr->as = vm_as_create(tgid);
  if (!pr->as) return -1;
  pr->p->as = pr->as;
  pr->base = PROBE_BASE;
  if (vm_region_insert(pr->as, pr->base, 0x4000,
                       VM_PROT_READ | VM_PROT_WRITE, VMK_ANON, VM_MAP_PRIVATE,
                       0, 0) != 0)
    return -1;
  pr->cs_base = PROBE_CS;
  pr->fd = file_open(pr->p, "/dev/nvshim", 2 /* O_RDWR */);
  if (pr->fd < 0) return -1;
  return 0;
}

static void probe_down(struct shim_probe *pr) {
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

/* Copy a request struct into the client's user memory, run the ioctl, copy the
 * result back.  The out fields must be prefilled by the caller: a zero that
 * means "the kernel wrote it" would make every assertion vacuous. */
static int probe_ioc(struct shim_probe *pr, unsigned long req, void *st, int len) {
  if (vm_kwrite(pr->p, pr->base, st, len) != 0) return -1000;
  int rc = nv_shim_ioctl(pr->p, pr->fd, req, pr->base);
  if (vm_user_read(pr->p, pr->base, st, len) != 0) return -1001;
  return rc;
}

/* The v2 arena base is above 2^32 on this port; both ends of a range must be
 * below PROBE_CS, and vm_region_insert above proves the mapping exists. */

static void test_nvshim_probe(void) {
  uart_puts("  Running test_nvshim_probe...\n");
  tests_run++;

  struct shim_probe pr;
  EXPECT_EQ(probe_up(&pr, 77), 0);
  if (!pr.up) return;

  /* The fd really is this node: the ioctl route and the close hook both key
   * on this predicate. */
  EXPECT_EQ(file_is_devnvshim(pr.p, pr.fd), 1);
  EXPECT_EQ(file_is_devnv(pr.p, pr.fd), 0);

  /* --- device identify --- */
  struct nvshim_device d;
  for (unsigned i = 0; i < sizeof(d); i++) ((unsigned char *)&d)[i] = 0xFF;
  d.abi = NVSHIM_ABI_VERSION;
  d.flags = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_DEVICE_ALLOC, &d, (int)sizeof d), 0);
  EXPECT_EQ((d.dev_id != 0 && d.dev_id != 0xFFFFFFFFu), 1);
  EXPECT_EQ((int)d.chipset, 0x192);   /* AD102 */
  EXPECT_EQ((int)d.device_id, 0x2684);
  EXPECT_EQ((int)d.mock, 1);
  EXPECT_EQ((int)d.gsp_required, 1);
  EXPECT_EQ((d.features & NVSHIM_FEAT_SUBMIT) != 0, 1);
  EXPECT_EQ((int)d.name[0], 'A');

  /* --- getparam (nouveau's own param numbers) --- */
  struct nvshim_param q;
  q.param = NVSHIM_PARAM_CHIPSET_ID;
  q.pad = 0;
  q.value = 0xFFFFFFFFFFFFFFFFull;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_GET_PARAM, &q, (int)sizeof q), 0);
  EXPECT_EQ((int)q.value, 0x192);
  q.param = NVSHIM_PARAM_PCI_DEVICE;
  q.value = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_GET_PARAM, &q, (int)sizeof q), 0);
  EXPECT_EQ((int)q.value, 0x2684);
  q.param = NVSHIM_PARAM_FB_SIZE;
  q.value = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_GET_PARAM, &q, (int)sizeof q), 0);
  EXPECT_EQ((q.value >= (1ull << 30)), 1);
  /* A param that reads the card is refused by name, not faked. */
  q.param = NVSHIM_PARAM_PTIMER_TIME;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_GET_PARAM, &q, (int)sizeof q), -ENOSYS);
  q.param = 0x1234;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_GET_PARAM, &q, (int)sizeof q), -EINVAL);

  /* --- allocation: a push buffer and a data buffer, both domains --- */
  struct nvshim_alloc a;
  a.dev_id = d.dev_id;
  a.domain = NVSHIM_DOMAIN_VRAM;
  a.handle = 0xFFFFFFFFu;
  a.pad = 0;
  a.size = 65536;
  a.bus = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_ALLOC, &a, (int)sizeof a), 0);
  uint32_t push_handle = a.handle;
  uint64_t push_bus = a.bus;
  EXPECT_EQ((a.handle != 0 && a.handle != 0xFFFFFFFFu), 1);
  EXPECT_EQ(a.bus != 0, 1);
  EXPECT_EQ((int)a.size, 65536);

  struct nvshim_alloc b;
  b.dev_id = d.dev_id;
  b.domain = NVSHIM_DOMAIN_GART;
  b.handle = 0;
  b.pad = 0;
  b.size = 4096;
  b.bus = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_ALLOC, &b, (int)sizeof b), 0);
  EXPECT_EQ(b.bus != push_bus, 1); /* distinct bus ranges */

  /* --- GPU VA space --- */
  struct nvshim_vm_init vm;
  vm.dev_id = d.dev_id;
  vm.vm_id = 0xFFFFFFFFu;
  vm.size = 1ull << 32; /* 4 GiB */
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_VM_INIT, &vm, (int)sizeof vm), 0);
  EXPECT_EQ((vm.vm_id != 0 && vm.vm_id != 0xFFFFFFFFu), 1);

  struct nvshim_vm_bind vb;
  vb.dev_id = d.dev_id;
  vb.vm_id = vm.vm_id;
  vb.handle = push_handle;
  vb.op = NVSHIM_VM_BIND_OP_MAP;
  vb.va = 0x10000000ull;
  vb.range = 65536;
  vb.bo_offset = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_VM_BIND, &vb, (int)sizeof vb), 0);

  /* A double map of the same range is refused by the RM's interval bookkeeping
   * -- the observation could have been negative, and here it is. */
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_VM_BIND, &vb, (int)sizeof vb), -EINVAL);

  /* --- the binding is a MAPPING, not bookkeeping ------------------------
   * Ask the RM to walk the VA: the answer must be the physical address of the
   * byte that VA names, inside the object's own pool frames (nv_dma_alloc's,
   * not a card's VRAM -- this is the mock), and the walk must translate rather
   * than repeat.  Two views of one memory: a store at the resolved physical
   * address is seen through the RM's object handle at the same offset. */
  const struct nv_rm_if *ri = nv_rm_mock_if();
  EXPECT_EQ(ri->vm_resolve != 0, 1);
  uint64_t phys = 0, bus = 0, left = 0;
  EXPECT_EQ(ri->vm_resolve(d.dev_id, vm.vm_id, vb.va, &phys, &bus, &left), 0);
  EXPECT_EQ(phys != 0, 1);
  EXPECT_EQ(left, 65536);          /* the whole binding, from its first byte */
  EXPECT_EQ(bus, phys);            /* GSTS.TES clear: identity, not a promise */
  uint64_t phys2 = 0, bus2 = 0, left2 = 0;
  EXPECT_EQ(ri->vm_resolve(d.dev_id, vm.vm_id, vb.va + 0x2000, &phys2, &bus2,
                           &left2), 0);
  EXPECT_EQ(phys2, phys + 0x2000); /* the walk translates; it does not repeat */
  EXPECT_EQ(left2, 65536 - 0x2000);/* and it reports the extent it walked */
  /* A hole, and a VA past the space, are refused -- never clamped to the
   * nearest binding, which would name memory the client did not map. */
  EXPECT_EQ(ri->vm_resolve(d.dev_id, vm.vm_id, vb.va + 65536, &phys2, &bus2,
                           &left2), -EINVAL);
  EXPECT_EQ(ri->vm_resolve(d.dev_id, vm.vm_id, (1ull << 32), &phys2, &bus2,
                           &left2), -EINVAL);
  /* The resolved address is the object's memory: store through it, read the
   * same offset back through the RM's object handle. */
  uint32_t magic = 0x5a5aa5a5u, back = 0;
  *(volatile uint32_t *)phys_to_virt(phys) = magic;
  EXPECT_EQ(nv_rm_mock_read_bo(d.dev_id, push_handle, 0, &back, 4), 0);
  EXPECT_EQ(back, magic);

  /* --- channel --- */
  struct nvshim_channel ch;
  ch.dev_id = d.dev_id;
  ch.vm_id = vm.vm_id;
  ch.handle = push_handle;
  ch.ch_id = 0xFFFFFFFFu;
  ch.pushbuf = 0;
  ch.pushbuf_size = 0;
  ch.engine = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_CHANNEL_ALLOC, &ch, (int)sizeof ch), 0);
  EXPECT_EQ((ch.ch_id != 0 && ch.ch_id != 0xFFFFFFFFu), 1);
  EXPECT_EQ(ch.pushbuf, push_bus);
  EXPECT_EQ((int)ch.pushbuf_size, 65536);
  EXPECT_EQ((int)ch.engine, NVSHIM_ENGINE_GR);

  /* --- engine class objects (the mock-RM object spine) -------------------
   * The class a stock nvk asks for first is the copy class 0xc7b5
   * (nouveau_context.c's find_class(..., 0xb5)); here it is allocated on the
   * channel through the mock RM, and the handle is the RM's, not the client's.
   * A second class gets its own handle, a class SCLASS never reported is
   * -EINVAL at the lookup, and a channel this client does not own is refused. */
  struct nvshim_engine_class egc;
  egc.dev_id = d.dev_id;
  egc.ch_id = ch.ch_id;
  egc.oclass = NVSHIM_NVIF_CLASS_AMPERE_DMA_COPY_B;
  egc.handle = 0xFFFFFFFFu;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_ENGINE_CLASS_ALLOC, &egc, (int)sizeof egc), 0);
  EXPECT_EQ((egc.handle != 0 && egc.handle != 0xFFFFFFFFu), 1);
  uint32_t ec_copy = egc.handle;

  egc.oclass = NVSHIM_NVIF_CLASS_AMPERE_B;
  egc.handle = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_ENGINE_CLASS_ALLOC, &egc, (int)sizeof egc), 0);
  EXPECT_EQ(egc.handle != ec_copy, 1);
  uint32_t ec_3d = egc.handle;

  egc.oclass = 0x00001234;
  egc.handle = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_ENGINE_CLASS_ALLOC, &egc, (int)sizeof egc), -EINVAL);

  egc.oclass = NVSHIM_NVIF_CLASS_AMPERE_DMA_COPY_B;
  egc.ch_id = ch.ch_id + 99;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_ENGINE_CLASS_ALLOC, &egc, (int)sizeof egc), -EINVAL);
  egc.ch_id = ch.ch_id;

  struct nvshim_engine_class_free ecf;
  ecf.handle = ec_3d;
  ecf.pad = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_ENGINE_CLASS_FREE, &ecf, (int)sizeof ecf), 0);
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_ENGINE_CLASS_FREE, &ecf, (int)sizeof ecf), -EINVAL);
  /* ec_copy is deliberately left to die with its channel below: the RM
   * parents it on the channel, so CHANNEL_FREE must destroy it too. */

  /* --- submit + fence, in both directions --- */
  unsigned char cs[64];
  for (unsigned i = 0; i < sizeof cs; i++) cs[i] = (unsigned char)(i * 7 + 1);
  EXPECT_EQ(vm_kwrite(pr.p, pr.cs_base, cs, (int)sizeof cs), 0);

  struct nvshim_submit s;
  s.ch_id = ch.ch_id;
  s.flags = 0;
  s.len = (uint32_t)sizeof cs;
  s.pad = 0;
  s.va = pr.cs_base;
  s.fence = 0;

  /* Hold the mock's kick: submit enqueues, nothing retires. */
  nv_rm_mock_hold(1);
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_SUBMIT, &s, (int)sizeof s), 0);
  uint64_t f1 = s.fence;
  EXPECT_EQ(f1 != 0, 1);
  EXPECT_EQ(nv_rm_mock_pending(), 1);

  struct nvshim_fence_wait fw;
  fw.ch_id = ch.ch_id;
  fw.timeout_ms = 0;
  fw.fence = f1;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_FENCE_WAIT, &fw, (int)sizeof fw), -ETIMEDOUT);

  fw.timeout_ms = 20;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_FENCE_WAIT, &fw, (int)sizeof fw), -ETIMEDOUT);
  EXPECT_EQ(nv_rm_mock_pending(), 1);

  /* Release the hold and submit again: that submit's kick retires both fences.
   * Nothing about the first wait's timeout was faked -- same fence, same call,
   * different engine state. */
  nv_rm_mock_hold(0);
  s.fence = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_SUBMIT, &s, (int)sizeof s), 0);
  uint64_t f2 = s.fence;
  EXPECT_EQ(f2 > f1, 1);
  EXPECT_EQ(nv_rm_mock_pending(), 0);

  fw.fence = f1;
  fw.timeout_ms = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_FENCE_WAIT, &fw, (int)sizeof fw), 0);
  fw.fence = f2;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_FENCE_WAIT, &fw, (int)sizeof fw), 0);

  /* A fence this channel never issued is -EINVAL, not a silent pass. */
  fw.fence = f2 + 1000;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_FENCE_WAIT, &fw, (int)sizeof fw), -EINVAL);
  fw.fence = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_FENCE_WAIT, &fw, (int)sizeof fw), -EINVAL);

  /* --- teardown, in order --- */
  vb.op = NVSHIM_VM_BIND_OP_UNMAP;
  vb.handle = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_VM_BIND, &vb, (int)sizeof vb), 0);
  /* The walk follows the mapping's lifetime: once unmapped, the VA names
   * nothing and there is no address to answer. */
  uint64_t u_phys = 0, u_bus = 0, u_left = 0;
  EXPECT_EQ(nv_rm_mock_if()->vm_resolve(d.dev_id, vm.vm_id, vb.va, &u_phys,
                                        &u_bus, &u_left), -EINVAL);
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_VM_BIND, &vb, (int)sizeof vb), -EINVAL);

  struct nvshim_channel_free cf;
  cf.ch_id = ch.ch_id;
  cf.pad = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_CHANNEL_FREE, &cf, (int)sizeof cf), 0);
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_CHANNEL_FREE, &cf, (int)sizeof cf), -EINVAL);

  struct nvshim_free fr;
  fr.dev_id = d.dev_id;
  fr.handle = b.handle;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_FREE, &fr, (int)sizeof fr), 0);
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_FREE, &fr, (int)sizeof fr), -EINVAL);

  struct nvshim_device_free df;
  df.dev_id = d.dev_id;
  df.pad = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_DEVICE_FREE, &df, (int)sizeof df), 0);
  /* The push buffer went with the device teardown: nothing may be live. */
  EXPECT_EQ(nv_rm_mock_live(), 0);

  /* Closing the fd also drops a client that never called DEVICE_FREE. */
  struct nvshim_device d2;
  for (unsigned i = 0; i < sizeof(d2); i++) ((unsigned char *)&d2)[i] = 0;
  d2.abi = NVSHIM_ABI_VERSION;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_DEVICE_ALLOC, &d2, (int)sizeof d2), 0);
  struct nvshim_alloc a2;
  a2.dev_id = d2.dev_id;
  a2.domain = NVSHIM_DOMAIN_VRAM;
  a2.handle = 0;
  a2.pad = 0;
  a2.size = 4096;
  a2.bus = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_ALLOC, &a2, (int)sizeof a2), 0);
  EXPECT_EQ(nv_rm_mock_live(), 1);

  uart_puts("  [NVSHIM] probe device+vm+channel+submit+fence: PASS\n");
  probe_down(&pr);
  EXPECT_EQ(nv_rm_mock_live(), 0); /* close destroyed the device and its alloc */
}

/* ---- ownership: one client cannot touch another's objects -------------- */

static void test_nvshim_ownership(void) {
  uart_puts("  Running test_nvshim_ownership...\n");
  tests_run++;

  struct shim_probe pr;
  EXPECT_EQ(probe_up(&pr, 78), 0);
  if (!pr.up) return;

  int fd_b = file_open(pr.p, "/dev/nvshim", 2);
  EXPECT_EQ((fd_b >= 0), 1);
  if (fd_b < 0) {
    probe_down(&pr);
    return;
  }

  struct nvshim_device d;
  for (unsigned i = 0; i < sizeof(d); i++) ((unsigned char *)&d)[i] = 0;
  d.abi = NVSHIM_ABI_VERSION;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_DEVICE_ALLOC, &d, (int)sizeof d), 0);

  /* A second DEVICE_ALLOC on the same fd is -EBUSY, like /dev/gpu's context. */
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_DEVICE_ALLOC, &d, (int)sizeof d), -EBUSY);

  struct nvshim_alloc a;
  a.dev_id = d.dev_id;
  a.domain = NVSHIM_DOMAIN_VRAM;
  a.handle = 0;
  a.pad = 0;
  a.size = 65536;
  a.bus = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_ALLOC, &a, (int)sizeof a), 0);

  struct nvshim_vm_init vm;
  vm.dev_id = d.dev_id;
  vm.vm_id = 0;
  vm.size = 1ull << 30;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_VM_INIT, &vm, (int)sizeof vm), 0);

  struct nvshim_channel ch;
  ch.dev_id = d.dev_id;
  ch.vm_id = vm.vm_id;
  ch.handle = a.handle;
  ch.ch_id = 0;
  ch.pushbuf = 0;
  ch.pushbuf_size = 0;
  ch.engine = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_CHANNEL_ALLOC, &ch, (int)sizeof ch), 0);

  /* Now client B (fd_b, same process) on the same user buffers.  Every
   * id-taking request must refuse A's objects: ownership is (pid, fd). */
  int save_fd = pr.fd;
  pr.fd = fd_b;

  struct nvshim_device db;
  for (unsigned i = 0; i < sizeof(db); i++) ((unsigned char *)&db)[i] = 0;
  db.abi = NVSHIM_ABI_VERSION;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_DEVICE_ALLOC, &db, (int)sizeof db), 0);
  EXPECT_EQ((db.dev_id != 0 && db.dev_id != 0xFFFFFFFFu), 1);

  struct nvshim_submit s;
  s.ch_id = ch.ch_id;
  s.flags = 0;
  s.len = 16;
  s.pad = 0;
  s.va = pr.cs_base;
  s.fence = 0;
  unsigned char cs[16];
  for (unsigned i = 0; i < sizeof cs; i++) cs[i] = (unsigned char)i;
  EXPECT_EQ(vm_kwrite(pr.p, pr.cs_base, cs, (int)sizeof cs), 0);
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_SUBMIT, &s, (int)sizeof s), -EINVAL);

  struct nvshim_fence_wait fw;
  fw.ch_id = ch.ch_id;
  fw.timeout_ms = 0;
  fw.fence = 1;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_FENCE_WAIT, &fw, (int)sizeof fw), -EINVAL);

  struct nvshim_free fr;
  fr.dev_id = db.dev_id;
  fr.handle = a.handle;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_FREE, &fr, (int)sizeof fr), -EINVAL);

  struct nvshim_vm_bind vb;
  vb.dev_id = db.dev_id;
  vb.vm_id = vm.vm_id;
  vb.handle = a.handle;
  vb.op = NVSHIM_VM_BIND_OP_MAP;
  vb.va = 0x20000000ull;
  vb.range = 4096;
  vb.bo_offset = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_VM_BIND, &vb, (int)sizeof vb), -EINVAL);

  struct nvshim_channel_free cf;
  cf.ch_id = ch.ch_id;
  cf.pad = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_CHANNEL_FREE, &cf, (int)sizeof cf), -EINVAL);

  pr.fd = save_fd;
  EXPECT_EQ(file_close(pr.p, fd_b), 0);
  probe_down(&pr);
  EXPECT_EQ(nv_rm_mock_live(), 0);
}

/* ---- argument and limit checks ---------------------------------------- */

static void test_nvshim_limits(void) {
  uart_puts("  Running test_nvshim_limits...\n");
  tests_run++;

  struct shim_probe pr;
  EXPECT_EQ(probe_up(&pr, 79), 0);
  if (!pr.up) return;

  /* Wrong ABI version / non-zero flags: refused before anything is created. */
  struct nvshim_device d;
  for (unsigned i = 0; i < sizeof(d); i++) ((unsigned char *)&d)[i] = 0;
  d.abi = NVSHIM_ABI_VERSION + 1;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_DEVICE_ALLOC, &d, (int)sizeof d), -EINVAL);
  d.abi = NVSHIM_ABI_VERSION;
  d.flags = 1;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_DEVICE_ALLOC, &d, (int)sizeof d), -EINVAL);
  d.flags = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_DEVICE_ALLOC, &d, (int)sizeof d), 0);

  /* ALLOC: zero size, a domain with no address space, over the client cap. */
  struct nvshim_alloc a;
  a.dev_id = d.dev_id;
  a.domain = NVSHIM_DOMAIN_VRAM;
  a.handle = 0;
  a.pad = 0;
  a.size = 0;
  a.bus = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_ALLOC, &a, (int)sizeof a), -EINVAL);
  a.size = 4096;
  a.domain = 1u << 0; /* NOUVEAU_GEM_DOMAIN_CPU: no CPU mapping in this ABI */
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_ALLOC, &a, (int)sizeof a), -EINVAL);
  a.domain = NVSHIM_DOMAIN_VRAM;
  a.size = 513ull * 1024 * 1024; /* over the 512 MiB per-client cap */
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_ALLOC, &a, (int)sizeof a), -ENOMEM);
  a.size = 5ull * 1024 * 1024 * 1024; /* over the 4 GiB single-alloc cap */
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_ALLOC, &a, (int)sizeof a), -EINVAL);

  a.size = 65536;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_ALLOC, &a, (int)sizeof a), 0);

  /* VM_INIT: unaligned and zero sizes. */
  struct nvshim_vm_init vm;
  vm.dev_id = d.dev_id;
  vm.vm_id = 0;
  vm.size = 0x1234;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_VM_INIT, &vm, (int)sizeof vm), -EINVAL);
  vm.size = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_VM_INIT, &vm, (int)sizeof vm), -EINVAL);
  vm.size = 1ull << 30;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_VM_INIT, &vm, (int)sizeof vm), 0);

  /* VM_BIND: unaligned, out of range, beyond the object, unknown VM. */
  struct nvshim_vm_bind vb;
  vb.dev_id = d.dev_id;
  vb.vm_id = vm.vm_id;
  vb.handle = a.handle;
  vb.op = NVSHIM_VM_BIND_OP_MAP;
  vb.va = 0x1000;
  vb.range = 4096;
  vb.bo_offset = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_VM_BIND, &vb, (int)sizeof vb), 0);
  vb.va = 0x1001; /* unaligned */
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_VM_BIND, &vb, (int)sizeof vb), -EINVAL);
  vb.va = (1ull << 30) - 4096; /* one page inside the space */
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_VM_BIND, &vb, (int)sizeof vb), 0);
  vb.va = (1ull << 30); /* first byte outside */
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_VM_BIND, &vb, (int)sizeof vb), -EINVAL);
  vb.va = 0x20000;
  vb.range = 131072; /* object is 64 KiB */
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_VM_BIND, &vb, (int)sizeof vb), -EINVAL);
  vb.vm_id = vm.vm_id + 100; /* a VM this client never created */
  vb.va = 0x30000;
  vb.range = 4096;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_VM_BIND, &vb, (int)sizeof vb), -EINVAL);
  vb.vm_id = vm.vm_id;
  vb.op = 7; /* not MAP or UNMAP */
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_VM_BIND, &vb, (int)sizeof vb), -EINVAL);

  /* CHANNEL_ALLOC with a handle this client does not own. */
  struct nvshim_channel ch;
  ch.dev_id = d.dev_id;
  ch.vm_id = vm.vm_id;
  ch.handle = a.handle + 1000;
  ch.ch_id = 0;
  ch.pushbuf = 0;
  ch.pushbuf_size = 0;
  ch.engine = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_CHANNEL_ALLOC, &ch, (int)sizeof ch), -EINVAL);
  ch.handle = a.handle;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_CHANNEL_ALLOC, &ch, (int)sizeof ch), 0);

  /* SUBMIT shape: zero, unaligned, over NVSHIM_MAX_CS. */
  struct nvshim_submit s;
  s.ch_id = ch.ch_id;
  s.flags = 0;
  s.len = 0;
  s.pad = 0;
  s.va = pr.cs_base;
  s.fence = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_SUBMIT, &s, (int)sizeof s), -EINVAL);
  s.len = 6; /* not a dword multiple */
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_SUBMIT, &s, (int)sizeof s), -EINVAL);
  s.len = NVSHIM_MAX_CS + 4;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_SUBMIT, &s, (int)sizeof s), -EINVAL);
  s.len = 4;
  s.flags = 1; /* no flags defined */
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_SUBMIT, &s, (int)sizeof s), -EINVAL);
  s.flags = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_SUBMIT, &s, (int)sizeof s), 0);

  /* A fence from a channel that issued fewer fences than the number asked:
   * per-channel ids make "never issued here" a real answer. */
  struct nvshim_channel ch2;
  ch2.dev_id = d.dev_id;
  ch2.vm_id = vm.vm_id;
  ch2.handle = a.handle;
  ch2.ch_id = 0;
  ch2.pushbuf = 0;
  ch2.pushbuf_size = 0;
  ch2.engine = 0;
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_CHANNEL_ALLOC, &ch2, (int)sizeof ch2), 0);
  struct nvshim_fence_wait fw;
  fw.ch_id = ch2.ch_id;
  fw.timeout_ms = 0;
  fw.fence = 5; /* ch shipped 1 fence; ch2 issued none */
  EXPECT_EQ(probe_ioc(&pr, NVSHIM_IOC_FENCE_WAIT, &fw, (int)sizeof fw), -EINVAL);

  /* A user pointer that is not mapped is -EFAULT, not a fault. */
  EXPECT_EQ(nv_shim_ioctl(pr.p, pr.fd, NVSHIM_IOC_DEVICE_ALLOC, 0x1000), -EFAULT);
  EXPECT_EQ(nv_shim_ioctl(pr.p, pr.fd, NVSHIM_IOC_DEVICE_ALLOC, 0), -EINVAL);

  probe_down(&pr);
  EXPECT_EQ(nv_rm_mock_live(), 0);
}

void nv_shim_test_suite(void) {
  test_nvshim_layout();
  test_nvshim_refusals();
  test_nvshim_probe();
  test_nvshim_ownership();
  test_nvshim_limits();
}

#endif /* KERNEL_MODE_UNIT_TEST */
