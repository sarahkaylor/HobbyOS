#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "errno.h"
#include "fat16.h"
#include "file_obj.h"
#include "frame.h"
#include "fs.h"
#include "vm.h"
#include "process.h"

extern void uart_puts(const char *s);
extern void print_int(int v);
extern void uart_print_hex(uint64_t v); /* so-vm lane A: physaddr evidence */

/* P2.2 (S2) window/paging unit tests.  Everything here runs against the
 * real frame allocator and the real arch page-table code (the same paths
 * the loader and the scheduler use), then restores the pool exactly.
 *
 * AS tags 60..63 are reserved for this suite (L12 raised MAX_PROCESSES to
 * 192, so 60..63 is inside the table but no process holds those slots at
 * test time; each test tears its AS down before the next one starts, so a
 * leaked table frame shows up as a count mismatch). */

#define VM_TEST_TAG 60

static void test_vm_as_lifecycle(void) {
  tests_run++;
  uart_puts("  Running test_vm_as_lifecycle...\n");

  int free0 = frame_free_count();
  struct addr_space *as = vm_as_create(VM_TEST_TAG);
  EXPECT_EQ((as != 0), 1);
  if (!as) {
    uart_puts("ASSERTION FAILED: vm_as_create returned NULL\n");
    return;
  }
  EXPECT_EQ(as->ver, AS_V2);
  EXPECT_EQ((as->root_phys != 0), 1);
  EXPECT_EQ((as->root_phys & 0xFFF), 0);
  EXPECT_EQ((as->asid == VM_TEST_TAG), 1);
  EXPECT_EQ(as->nr, 0);
  EXPECT_EQ(as->resident_frames, 0);
  EXPECT_EQ((as->table_frames >= 1), 1); /* at least the root */
  EXPECT_EQ((frame_free_count() < free0), 1);

  /* A second create for the same tag is rejected while in use. */
  EXPECT_EQ((vm_as_create(VM_TEST_TAG) == 0), 1);
  /* Tag 0 is illegal. */
  EXPECT_EQ((vm_as_create(0) == 0), 1);

  vm_as_teardown(as);
  /* Teardown returns the pool to exactly the prior state: root frame,
     table frames and the region-array frame all come back. */
  EXPECT_EQ(frame_free_count(), free0);

  /* Idempotent: a second teardown is a no-op. */
  vm_as_teardown(as);
  EXPECT_EQ(frame_free_count(), free0);

  /* And the tag can be recreated after teardown. */
  struct addr_space *as2 = vm_as_create(VM_TEST_TAG);
  EXPECT_EQ((as2 != 0), 1);
  vm_as_teardown(as2);
  EXPECT_EQ(frame_free_count(), free0);
}

static void test_vm_region_list(void) {
  tests_run++;
  uart_puts("  Running test_vm_region_list...\n");

  int free0 = frame_free_count();
  struct addr_space *as = vm_as_create(VM_TEST_TAG);
  EXPECT_EQ((as != 0), 1);
  if (!as)
    return;

  /* Insert one region at the image slot. */
  EXPECT_EQ(vm_region_insert(as, USER_IMG_BASE, 0x4000,
                             VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC,
                             VMK_IMG, VM_MAP_PRIVATE, 0, 0),
            0);
  EXPECT_EQ(as->nr, 1);
  struct vm_region *r = vm_region_find(as, USER_IMG_BASE);
  EXPECT_EQ((r != 0), 1);
  if (r) {
    EXPECT_EQ(r->kind, VMK_IMG);
    EXPECT_EQ(r->len, 0x4000ULL);
  }
  /* Every page of the region hits; one byte past misses; a hole misses. */
  EXPECT_EQ((vm_region_find(as, USER_IMG_BASE + 0x3FFF) != 0), 1);
  EXPECT_EQ((vm_region_find(as, USER_IMG_BASE + 0x4000) == 0), 1);
  EXPECT_EQ((vm_region_find(as, USER_IMG_BASE - 0x1000) == 0), 1);

  /* Overlap is rejected. */
  EXPECT_EQ(vm_region_insert(as, USER_IMG_BASE + 0x2000, 0x2000,
                             VM_PROT_READ, VMK_ANON, VM_MAP_PRIVATE, 0, 0),
            -1);
  EXPECT_EQ(as->nr, 1);

  /* Adjacent + identical merges (prot of the second insert differs ->
     no merge; then identical -> merge). */
  EXPECT_EQ(vm_region_insert(as, USER_IMG_BASE + 0x4000, 0x4000,
                             VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC,
                             VMK_IMG, VM_MAP_PRIVATE, 0, 0),
            0);
  EXPECT_EQ(as->nr, 1);
  r = vm_region_find(as, USER_IMG_BASE);
  EXPECT_EQ((r && r->len == 0x8000), 1);

  /* A different kind is a separate region, sorted after. */
  EXPECT_EQ(vm_region_insert(as, USER_MAIN_STK_LIMIT_V2, USER_MAIN_STK_SIZE,
                             VM_PROT_READ | VM_PROT_WRITE, VMK_STACK,
                             VM_MAP_PRIVATE, 0, 0),
            0);
  EXPECT_EQ(as->nr, 2);
  EXPECT_EQ((vm_region_find(as, USER_MAIN_STK_LIMIT_V2)->kind == VMK_STACK), 1);
  /* The guard gap below the stack limit is a hole. */
  EXPECT_EQ((vm_region_find(as, USER_MAIN_STK_GUARD_LIMIT_V2) == 0), 1);

  /* Above the window is rejected; unaligned is rejected. */
  EXPECT_EQ(vm_region_insert(as, USER_VA_TOP - 0x1000, 0x2000, VM_PROT_READ,
                             VMK_ANON, VM_MAP_PRIVATE, 0, 0),
            -1);
  EXPECT_EQ(vm_region_insert(as, USER_IMG_BASE, 0x2345, VM_PROT_READ,
                             VMK_ANON, VM_MAP_PRIVATE, 0, 0),
            -1);

  /* Removal: whole-region, then a trimmed region vanishes cleanly. */
  EXPECT_EQ(vm_region_remove(as, USER_IMG_BASE, 0x8000), 0);
  EXPECT_EQ(as->nr, 1);
  EXPECT_EQ(vm_region_remove(as, USER_IMG_BASE, 0x8000), 0); /* idempotent */
  EXPECT_EQ(vm_region_remove(as, USER_MAIN_STK_LIMIT_V2, USER_MAIN_STK_SIZE), 0);
  EXPECT_EQ(as->nr, 0);

  vm_as_teardown(as);
  EXPECT_EQ(frame_free_count(), free0);
}

static void test_vm_map_grow_commit(void) {
  tests_run++;
  uart_puts("  Running test_vm_map_grow_commit...\n");

  int free0 = frame_free_count();
  struct addr_space *as = vm_as_create(VM_TEST_TAG);
  EXPECT_EQ((as != 0), 1);
  if (!as)
    return;
  int after_create = frame_free_count();

  /* Region over two pages at the heap slot; map two frames. */
  uint64_t base = USER_HEAP_BASE_V2;
  EXPECT_EQ(vm_region_insert(as, base, 0x2000, VM_PROT_READ | VM_PROT_WRITE,
                             VMK_HEAP, VM_MAP_PRIVATE, 0, 0),
            0);
  int f0 = frame_free_count();
  uint64_t p0 = frame_alloc_zeroed();
  uint64_t p1 = frame_alloc_zeroed();
  EXPECT_EQ((p0 != 0 && p1 != 0), 1);
  EXPECT_EQ(vm_map_page(as, base, p0, VM_PROT_READ | VM_PROT_WRITE, VMK_HEAP), 0);
  EXPECT_EQ(vm_map_page(as, base + 0x1000, p1, VM_PROT_READ | VM_PROT_WRITE,
                        VMK_HEAP),
            0);
  EXPECT_EQ(as->resident_frames, 2);
  /* The maps may also have grown the AS's table set (L2/L3 allocation);
     those frames are the AS's until teardown, so the unmap baseline is
     taken here, after the tables exist. */
  int f_mapped = frame_free_count();

  /* The arch walk sees them (this is the CR3/TTBR0 translation the user
     will use), and vm_range_ok accepts the whole span. */
  uint64_t leaf = 0;
  EXPECT_EQ(vm_arch_walk(as, base, &leaf), 0);
  EXPECT_EQ((leaf != 0), 1);
  EXPECT_EQ(vm_arch_walk(as, base + 0x2000, 0), -1);
  struct process fake;
  for (unsigned i = 0; i < sizeof(fake); i++)
    ((unsigned char *)&fake)[i] = 0;
  fake.as = as;
  EXPECT_EQ(vm_range_ok(&fake, base, 0x2000, 1), 0);
  /* Unmapped third page inside no region -> rejected. */
  EXPECT_EQ(vm_range_ok(&fake, base + 0x2000, 0x1000, 0), -1);
  /* Write check against a read-only... region has RW, so flip and check. */
  EXPECT_EQ(vm_region_remove(as, base, 0x2000), 0);
  EXPECT_EQ(vm_range_ok(&fake, base, 0x1000, 0), -1);

  /* Unmap frees the leaves back to the pool (exactly the two pages; the
     table frames stay with the AS until teardown). */
  EXPECT_EQ(vm_unmap_page(as, base), 0);
  EXPECT_EQ(vm_unmap_page(as, base + 0x1000), 0);
  EXPECT_EQ(as->resident_frames, 0);
  EXPECT_EQ(frame_free_count(), f_mapped + 2);

  /* Bad-argument paths: misaligned VA, out-of-window VA. */
  EXPECT_EQ(vm_map_page(as, base + 1, p0, VM_PROT_READ, VMK_HEAP), -1);
  EXPECT_EQ(vm_map_page(as, USER_VA_TOP, p0, VM_PROT_READ, VMK_HEAP), -1);
  EXPECT_EQ(vm_unmap_page(as, USER_VA_BASE - 0x1000), 0); /* absent = ok */

  vm_as_teardown(as);
  EXPECT_EQ(frame_free_count(), free0);
  (void)after_create;
  (void)f0;
}

static void test_vm_windows_distinct(void) {
  tests_run++;
  uart_puts("  Running test_vm_windows_distinct...\n");

  int free0 = frame_free_count();
  struct addr_space *as = vm_as_create(VM_TEST_TAG);
  EXPECT_EQ((as != 0), 1);
  if (!as)
    return;

  /* One region per slot: the windows must not overlap each other. */
  EXPECT_EQ(vm_region_insert(as, USER_IMG_BASE, 0x1000,
                             VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC,
                             VMK_IMG, VM_MAP_PRIVATE, 0, 0),
            0);
  EXPECT_EQ(vm_region_insert(as, USER_HEAP_BASE_V2, 0x1000,
                             VM_PROT_READ | VM_PROT_WRITE, VMK_HEAP,
                             VM_MAP_PRIVATE, 0, 0),
            0);
  EXPECT_EQ(vm_region_insert(as, USER_MMAP_BASE_V2, 0x1000,
                             VM_PROT_READ | VM_PROT_WRITE, VMK_ANON,
                             VM_MAP_PRIVATE, 0, 0),
            0);
  EXPECT_EQ(vm_region_insert(as, USER_TSTK_BASE_V2, 0x1000,
                             VM_PROT_READ | VM_PROT_WRITE, VMK_STACK,
                             VM_MAP_PRIVATE, 0, 0),
            0);
  EXPECT_EQ(vm_region_insert(as, USER_MAIN_STK_LIMIT_V2, 0x1000,
                             VM_PROT_READ | VM_PROT_WRITE, VMK_STACK,
                             VM_MAP_PRIVATE, 0, 0),
            0);
  EXPECT_EQ(as->nr, 5);

  /* Slot ordering holds: each base is inside its own region only. */
  EXPECT_EQ((vm_region_find(as, USER_IMG_BASE)->kind == VMK_IMG), 1);
  EXPECT_EQ((vm_region_find(as, USER_HEAP_BASE_V2)->kind == VMK_HEAP), 1);
  EXPECT_EQ((vm_region_find(as, USER_MMAP_BASE_V2)->kind == VMK_ANON), 1);
  EXPECT_EQ((vm_region_find(as, USER_TSTK_BASE_V2)->kind == VMK_STACK), 1);
  EXPECT_EQ(
      (vm_region_find(as, USER_MAIN_STK_LIMIT_V2)->kind == VMK_STACK), 1);
  /* Inter-slot holes are holes. */
  EXPECT_EQ((vm_region_find(as, USER_IMG_BASE + 0x1000) == 0), 1);
  EXPECT_EQ((vm_region_find(as, USER_HEAP_BASE_V2 - 0x1000) == 0), 1);
  EXPECT_EQ((vm_region_find(as, USER_MMAP_BASE_V2 - 0x1000) == 0), 1);

  /* PROT_NONE region (design section 1.3): the range is covered but the
     access check fails both read and write. */
  EXPECT_EQ(vm_region_insert(as, USER_MMAP_BASE_V2 + 0x10000, 0x1000, 0,
                             VMK_ANON, VM_MAP_PRIVATE, 0, 0),
            0);
  struct process fake;
  for (unsigned i = 0; i < sizeof(fake); i++)
    ((unsigned char *)&fake)[i] = 0;
  fake.as = as;
  EXPECT_EQ(vm_range_ok(&fake, USER_MMAP_BASE_V2 + 0x10000, 0x1000, 0), -1);
  EXPECT_EQ(vm_range_ok(&fake, USER_MMAP_BASE_V2 + 0x10000, 0x1000, 1), -1);

  /* The stack guard gap is NOT covered by the stack region. */
  EXPECT_EQ(vm_range_ok(&fake, USER_MAIN_STK_GUARD_LIMIT_V2, 0x1000, 0), -1);

  vm_as_teardown(as);
  EXPECT_EQ(frame_free_count(), free0);
}

static void test_vm_hole_find(void) {
  tests_run++;
  uart_puts("  Running test_vm_hole_find...\n");

  int free0 = frame_free_count();
  struct addr_space *as = vm_as_create(VM_TEST_TAG);
  EXPECT_EQ((as != 0), 1);
  if (!as)
    return;

  /* Empty AS: the first hole is the aligned start. */
  EXPECT_EQ((vm_hole_find(as, USER_MMAP_BASE_V2, USER_MMAP_LIMIT_V2, 0x2000) ==
             USER_MMAP_BASE_V2),
            1);

  EXPECT_EQ(vm_region_insert(as, USER_MMAP_BASE_V2 + 0x10000, 0x1000,
                             VM_PROT_READ | VM_PROT_WRITE, VMK_ANON,
                             VM_MAP_PRIVATE, 0, 0),
            0);
  /* The VM_HOLE_GAP invariant: a new mapping must keep a raw >=64 KiB gap
     from the existing region, so the 0x1000 request skips past it even
     though the space below is physically free. */
  uint64_t h = vm_hole_find(as, USER_MMAP_BASE_V2, USER_MMAP_LIMIT_V2, 0x1000);
  EXPECT_EQ((h == USER_MMAP_BASE_V2 + 0x11000), 1);
  /* A span that starts at the existing region and has less room after it
     than requested finds no hole at all. */
  EXPECT_EQ((vm_hole_find(as, USER_MMAP_BASE_V2 + 0x10000,
                          USER_MMAP_BASE_V2 + 0x12000, 0x2000) == 0),
            1);
  uint64_t h2 = vm_hole_find(as, USER_MMAP_BASE_V2, USER_MMAP_LIMIT_V2, 0x8000);
  EXPECT_EQ((h2 == USER_MMAP_BASE_V2 + 0x11000), 1);

  vm_as_teardown(as);
  EXPECT_EQ(frame_free_count(), free0);
}

/* P2.3 (S3, design sections 3/5.1-5.5): demand + the fault classifier. */
static void test_vm_demand_fault(void) {
  tests_run++;
  uart_puts("  Running test_vm_demand_fault...\n");

  int free0 = frame_free_count();
  struct addr_space *as = vm_as_create(VM_TEST_TAG);
  EXPECT_EQ((as != 0), 1);
  if (!as)
    return;

  EXPECT_EQ(vm_region_insert(as, USER_MMAP_BASE_V2, 0x2000,
                             VM_PROT_READ | VM_PROT_WRITE, VMK_ANON,
                             VM_MAP_PRIVATE, 0, 0),
            0);
  struct process fake;
  for (unsigned i = 0; i < sizeof(fake); i++)
    ((unsigned char *)&fake)[i] = 0;
  fake.as = as;
  int f1 = frame_free_count();

  /* S3 change: the range check now materializes (demand) instead of
     failing on non-residency. */
  EXPECT_EQ(vm_range_ok(&fake, USER_MMAP_BASE_V2, 0x1000, 0), 0);
  EXPECT_EQ(as->resident_frames, 1);
  EXPECT_EQ((frame_free_count() < f1), 1);
  /* ... and the second page too. */
  EXPECT_EQ(vm_touch(&fake, USER_MMAP_BASE_V2 + 0x1000, 1, 1), 0);
  EXPECT_EQ(as->resident_frames, 2);

  /* Holes fail: below the region, and the inter-slot gap. */
  EXPECT_EQ(vm_touch(&fake, USER_MMAP_BASE_V2 - 0x1000, 1, 0), -1);
  EXPECT_EQ(vm_range_ok(&fake, USER_MMAP_BASE_V2 + 0x2000, 0x1000, 0), -1);

  /* The fault classifier: hole -> HOLE (kill); absent in-region -> demand.
     w5 (iv): a present-but-permissive page is the L9 prefetch race (the
     prefetch mapped it between the hardware's walk and this handler), so
     it is a TLB-flush-and-RETRY (0), bounded: after FAULT_RETRY_MAX
     consecutive present-faults on the SAME VA it is a real fault (1,
     "PROT").  Genuine permission violations still kill (line 355+). */
  const char *why = 0;
  EXPECT_EQ(vm_handle_fault(&fake, USER_MMAP_BASE_V2 + 0x2000, 0, 0, &why), 1);
  EXPECT_EQ((why && why[0] == 'H'), 1); /* "HOLE" */
  /* present + permissive: retry, never kill */
  EXPECT_EQ(vm_handle_fault(&fake, USER_MMAP_BASE_V2, 0, 0, &why), 0);
  EXPECT_EQ(vm_handle_fault(&fake, USER_MMAP_BASE_V2, 0, 0, &why), 0);
  EXPECT_EQ(vm_handle_fault(&fake, USER_MMAP_BASE_V2, 0, 0, &why), 0);
  EXPECT_EQ(vm_handle_fault(&fake, USER_MMAP_BASE_V2, 0, 0, &why), 0);
  EXPECT_EQ(vm_handle_fault(&fake, USER_MMAP_BASE_V2, 0, 0, &why), 1);
  EXPECT_EQ((why && why[0] == 'P'), 1); /* budget exhausted: "PROT" */
  /* a different VA resets the retry budget */
  EXPECT_EQ(vm_handle_fault(&fake, USER_MMAP_BASE_V2 + 0x1000, 0, 0, &why), 0);
  EXPECT_EQ(vm_handle_fault(&fake, USER_MMAP_BASE_V2 + 0x1000, 0, 0, &why), 0);

  /* A fresh page faults in as zero-fill: the walk must now succeed (the
     zero-content half of demand-zero is proven end to end by MMTEST,
     which reads back pages under its own AS). */
  struct vm_region *r = vm_region_find(as, USER_MMAP_BASE_V2);
  EXPECT_EQ((r != 0), 1);
  uint64_t leaf = 0;
  EXPECT_EQ(vm_arch_walk(as, USER_MMAP_BASE_V2, &leaf), 0);
  EXPECT_EQ((leaf & 1), 1);

  /* Write-protect the whole span: a read-only region kills a write. */
  EXPECT_EQ(vm_mprotect(&fake, USER_MMAP_BASE_V2, 0x2000, VM_PROT_READ), 0);
  EXPECT_EQ(vm_handle_fault(&fake, USER_MMAP_BASE_V2 + 0x1000, 1, 0, &why), 1);
  EXPECT_EQ((why && why[0] == 'P'), 1);
  /* And the pages were re-encoded, not zapped, by the RW->RO change. */
  EXPECT_EQ(as->resident_frames, 2);

  /* PROT_NONE zaps the resident leaves (design 4.4 divergence). */
  EXPECT_EQ(vm_mprotect(&fake, USER_MMAP_BASE_V2, 0x2000, 0), 0);
  EXPECT_EQ(as->resident_frames, 0);
  EXPECT_EQ(vm_arch_walk(as, USER_MMAP_BASE_V2, 0), -1);

  vm_as_teardown(as);
  EXPECT_EQ(frame_free_count(), free0);
}

/* P2.3 (S3, design section 4.1): the v2 mmap family at the vm layer. */
static void test_vm_mmap_family(void) {
  tests_run++;
  uart_puts("  Running test_vm_mmap_family...\n");

  int free0 = frame_free_count();
  struct addr_space *as = vm_as_create(VM_TEST_TAG);
  EXPECT_EQ((as != 0), 1);
  if (!as)
    return;
  struct process fake;
  for (unsigned i = 0; i < sizeof(fake); i++)
    ((unsigned char *)&fake)[i] = 0;
  fake.as = as;

  /* First anonymous mapping lands at the arena base. */
  int64_t b1 = vm_mmap(&fake, 0, 0x2000, VM_PROT_READ | VM_PROT_WRITE,
                       VM_MAP_PRIVATE | VM_MAP_ANONYMOUS, -1, 0);
  EXPECT_EQ((b1 == (int64_t)USER_MMAP_BASE_V2), 1);
  /* The second keeps the hook gap. */
  int64_t b2 = vm_mmap(&fake, 0, 0x1000, VM_PROT_READ,
                       VM_MAP_PRIVATE | VM_MAP_ANONYMOUS, -1, 0);
  EXPECT_EQ((b2 >= (int64_t)USER_MMAP_BASE_V2 + 0x2000), 1);
  EXPECT_EQ((b2 > b1), 1);

  /* fd-backed and MAP_SHARED are S4: rejected, and a bad len too.  Pin the
     fake fd table first: a zeroed struct reads global fd 0, which an earlier
     suite may have left holding a real file — and a regular-file fd is now
     a legal (file-backed) mmap source.  fd 3 CLOSED is the deterministic
     "not a mappable fd" case: -ENOTSUP, unchanged. */
  fake.open_fds[3] = -1;
  EXPECT_EQ(vm_mmap(&fake, 0, 0x1000, VM_PROT_READ, VM_MAP_PRIVATE, 3, 0),
            -ENOTSUP);
  EXPECT_EQ(vm_mmap(&fake, 0, 0, VM_PROT_READ, VM_MAP_PRIVATE, -1, 0), -EINVAL);

  /* MAP_FIXED places exactly. */
  uint64_t fix = USER_MMAP_BASE_V2 + 0x100000;
  int64_t b3 = vm_mmap(&fake, fix, 0x1000, VM_PROT_READ | VM_PROT_WRITE,
                       VM_MAP_PRIVATE | VM_MAP_ANONYMOUS | VM_MAP_FIXED, -1, 0);
  EXPECT_EQ((b3 == (int64_t)fix), 1);

  /* mprotect: hole -> -ENOMEM; covered -> ok and visible in the list. */
  EXPECT_EQ(vm_mprotect(&fake, USER_MMAP_BASE_V2 + 0x200000, 0x1000,
                        VM_PROT_READ),
            -ENOMEM);
  EXPECT_EQ(vm_mprotect(&fake, fix, 0x1000, VM_PROT_READ), 0);
  struct vm_region *r = vm_region_find(as, fix);
  EXPECT_EQ((r && r->prot == VM_PROT_READ), 1);

  /* Demand one page of the fixed mapping, then madvise it away. */
  EXPECT_EQ(vm_touch(&fake, fix, 1, 0), 0);
  EXPECT_EQ(as->resident_frames, 1);
  EXPECT_EQ(vm_madvise(&fake, fix, 0x1000, VM_MADV_DONTNEED), 0);
  EXPECT_EQ(as->resident_frames, 0);
  EXPECT_EQ(vm_arch_walk(as, fix, 0), -1);
  /* The mapping itself survives DONTNEED. */
  EXPECT_EQ((vm_region_find(as, fix) != 0), 1);
  /* Unknown advice is a no-op; an unmapped address is -EINVAL. */
  EXPECT_EQ(vm_madvise(&fake, fix, 0x1000, 99), 0);
  EXPECT_EQ(vm_madvise(&fake, USER_MMAP_BASE_V2 + 0x300000, 0x1000,
                       VM_MADV_DONTNEED),
            -EINVAL);

  /* munmap drops the mapping (and its resident pages). */
  EXPECT_EQ(vm_touch(&fake, fix, 1, 0), 0);
  EXPECT_EQ(vm_munmap_range(&fake, fix, 0x1000), 0);
  EXPECT_EQ((vm_region_find(as, fix) == 0), 1);
  EXPECT_EQ(vm_munmap_range(&fake, fix, 0x1000), -EINVAL);

  vm_as_teardown(as);
  EXPECT_EQ(frame_free_count(), free0);
}

/* P2.4 (S4, design section 10 item 5): shared objects -- the memfd-object
 * / MAP_SHARED core at the vm layer.  Object create/ftruncate/seals; two
 * independent address spaces mapping one object materialize the SAME
 * frame; refcounts unwind back to the exact baseline frame count. */
static void test_vm_shared_objects(void) {
  tests_run++;
  uart_puts("  Running test_vm_shared_objects...\n");

  int free0 = frame_free_count();
  struct vm_object *o = vm_object_create(VM_OBJ_MEMFD, 0);
  EXPECT_EQ((o != 0), 1);
  if (!o)
    return;

  /* ftruncate sets the capacity; pages stay sparse until touched. */
  EXPECT_EQ(vm_object_size(o), 0);
  EXPECT_EQ(vm_object_truncate(o, 0x3000), 0);
  EXPECT_EQ(vm_object_size(o), 0x3000);

  /* Seals (design 4.3, OQ7 minimal set): SHRINK blocks shrink, GROW
     blocks grow, SEAL latches against any further seal. */
  EXPECT_EQ(vm_object_add_seals(o, VM_SEAL_SHRINK), 0);
  EXPECT_EQ(vm_object_truncate(o, 0x1000), -EPERM);
  EXPECT_EQ(vm_object_truncate(o, 0x4000), 0);
  EXPECT_EQ(vm_object_add_seals(o, VM_SEAL_GROW | VM_SEAL_WRITE), 0);
  EXPECT_EQ(vm_object_get_seals(o),
            VM_SEAL_SHRINK | VM_SEAL_GROW | VM_SEAL_WRITE);
  EXPECT_EQ(vm_object_truncate(o, 0x8000), -EPERM);
  EXPECT_EQ(vm_object_add_seals(o, VM_SEAL_SEAL), 0);
  EXPECT_EQ(vm_object_add_seals(o, VM_SEAL_SHRINK), -EPERM);

  /* Two fake groups map the same object span. */
  struct process f1, f2;
  for (unsigned i = 0; i < sizeof(f1); i++)
    ((unsigned char *)&f1)[i] = 0;
  for (unsigned i = 0; i < sizeof(f2); i++)
    ((unsigned char *)&f2)[i] = 0;
  f1.as = vm_as_create(VM_TEST_TAG + 1);
  f2.as = vm_as_create(VM_TEST_TAG + 2);
  EXPECT_EQ((f1.as != 0 && f2.as != 0), 1);
  if (!f1.as || !f2.as) {
    if (f1.as)
      vm_as_teardown(f1.as);
    if (f2.as)
      vm_as_teardown(f2.as);
    vm_object_unref(o);
    return;
  }
  EXPECT_EQ(vm_region_insert(f1.as, USER_MMAP_BASE_V2, 0x2000,
                             VM_PROT_READ | VM_PROT_WRITE, VMK_SHARED,
                             VM_MAP_SHARED, o, 0),
            0);
  EXPECT_EQ(vm_region_insert(f2.as, USER_MMAP_BASE_V2, 0x2000,
                             VM_PROT_READ | VM_PROT_WRITE, VMK_SHARED,
                             VM_MAP_SHARED, o, 0),
            0);

  /* First touch in each AS: the same backing frame must come back. */
  EXPECT_EQ(vm_touch(&f1, USER_MMAP_BASE_V2, 8, 1), 0);
  EXPECT_EQ(vm_touch(&f2, USER_MMAP_BASE_V2, 8, 1), 0);
  uint64_t l1 = 0, l2 = 0;
  EXPECT_EQ(vm_arch_walk(f1.as, USER_MMAP_BASE_V2, &l1), 0);
  EXPECT_EQ(vm_arch_walk(f2.as, USER_MMAP_BASE_V2, &l2), 0);
  l1 = vm_arch_leaf_phys(l1); /* walk returns the full leaf: mask attrs */
  l2 = vm_arch_leaf_phys(l2);
  EXPECT_EQ((l1 != 0 && l1 == l2), 1);
  EXPECT_EQ(vm_object_page_locked(o, 0), l1);

  /* Write visibility both ways (one physical frame). */
  *(volatile uint64_t *)l1 = 0x5A5A1234ULL;
  EXPECT_EQ(*(volatile uint64_t *)l2, 0x5A5A1234ULL);
  *(volatile uint64_t *)l2 = 0x0BADF00DULL;
  EXPECT_EQ(*(volatile uint64_t *)l1, 0x0BADF00DULL);

  /* Page 1 is sparse and independent (fresh zero frame). */
  EXPECT_EQ(vm_touch(&f1, USER_MMAP_BASE_V2 + 0x1000, 8, 1), 0);
  uint64_t l1b = 0;
  EXPECT_EQ(vm_arch_walk(f1.as, USER_MMAP_BASE_V2 + 0x1000, &l1b), 0);
  l1b = vm_arch_leaf_phys(l1b);
  EXPECT_EQ((l1b != 0 && l1b != l1), 1);
  EXPECT_EQ(*(volatile uint64_t *)l1b, 0);

  /* Teardown drops the region record refs; the create ref keeps the
     object (and its pages) alive until the final unref. */
  vm_as_teardown(f2.as);
  vm_as_teardown(f1.as);
  EXPECT_EQ((frame_free_count() < free0), 1);
  EXPECT_EQ(vm_object_page_locked(o, 0), l1);
  vm_object_unref(o);
  EXPECT_EQ(frame_free_count(), free0);
}

/* P2.4 (S4, design section 10 item 6): v2 fork clone at the vm layer.
 * Resident private pages are copied frame-by-frame (distinct frames, same
 * content, isolated); untouched pages stay holes; object pages re-map the
 * same shared frame; teardown returns the pool to the baseline. */
static void test_vm_as_clone_fork(void) {
  tests_run++;
  uart_puts("  Running test_vm_as_clone_fork...\n");

  int free0 = frame_free_count();
  struct addr_space *as = vm_as_create(VM_TEST_TAG + 3);
  EXPECT_EQ((as != 0), 1);
  if (!as)
    return;
  struct process p;
  for (unsigned i = 0; i < sizeof(p); i++)
    ((unsigned char *)&p)[i] = 0;
  p.as = as;

  int64_t b = vm_mmap(&p, 0, 0x4000, VM_PROT_READ | VM_PROT_WRITE,
                      VM_MAP_PRIVATE | VM_MAP_ANONYMOUS, -1, 0);
  EXPECT_EQ((b > 0), 1);
  if (b <= 0) {
    vm_as_teardown(as);
    return;
  }
  /* Touch page 0 only; write a pattern. */
  EXPECT_EQ(vm_touch(&p, (uint64_t)b, 8, 1), 0);
  uint64_t lp = 0;
  EXPECT_EQ(vm_arch_walk(as, (uint64_t)b, &lp), 0);
  lp = vm_arch_leaf_phys(lp); /* mask the leaf's attribute bits */
  *(volatile uint64_t *)lp = 0xA1B2C3D4ULL;

  /* An object-backed (memfd-style) region as well. */
  struct vm_object *o = vm_object_create(VM_OBJ_MEMFD, 0x1000);
  EXPECT_EQ((o != 0), 1);
  uint64_t ob = USER_MMAP_LIMIT_V2 - 0x2000;
  EXPECT_EQ(vm_region_insert(as, ob, 0x1000, VM_PROT_READ | VM_PROT_WRITE,
                             VMK_SHARED, VM_MAP_SHARED, o, 0),
            0);
  EXPECT_EQ(vm_touch(&p, ob, 8, 1), 0);
  uint64_t lo = 0;
  EXPECT_EQ(vm_arch_walk(as, ob, &lo), 0);
  lo = vm_arch_leaf_phys(lo);
  *(volatile uint64_t *)lo = 0x1111222233334444ULL;

  struct addr_space *c = vm_as_clone(as, VM_TEST_TAG); /* tag 60, free again */
  EXPECT_EQ((c != 0), 1);
  if (!c) {
    vm_as_teardown(as);
    vm_object_unref(o);
    return;
  }

  /* Private page: present in the clone, DIFFERENT frame, same content. */
  uint64_t lc = 0;
  EXPECT_EQ(vm_arch_walk(c, (uint64_t)b, &lc), 0);
  lc = vm_arch_leaf_phys(lc);
  EXPECT_EQ((lc != 0 && lc != lp), 1);
  EXPECT_EQ(*(volatile uint64_t *)lc, 0xA1B2C3D4ULL);
  /* ...and the copies are isolated. */
  *(volatile uint64_t *)lc = 0xDEADULL;
  EXPECT_EQ(*(volatile uint64_t *)lp, 0xA1B2C3D4ULL);
  /* The untouched page of the same region stays a hole in the clone. */
  EXPECT_EQ(vm_arch_walk(c, (uint64_t)b + 0x1000, 0), -1);
  /* Object page: the SAME shared frame, not a copy. */
  uint64_t lo2 = 0;
  EXPECT_EQ(vm_arch_walk(c, ob, &lo2), 0);
  lo2 = vm_arch_leaf_phys(lo2);
  EXPECT_EQ((lo2 == lo), 1);
  EXPECT_EQ(*(volatile uint64_t *)lo2, 0x1111222233334444ULL);

  /* Teardowns return every private frame; the object survives on the
     create ref (both records dropped the child/parent refs). */
  vm_as_teardown(c);
  vm_as_teardown(as);
  EXPECT_EQ(vm_object_page_locked(o, 0), lo);
  vm_object_unref(o);
  EXPECT_EQ(frame_free_count(), free0);
}

/* P2.4 (S4, design section 10 item 7): repeated create/map/touch/teardown
 * cycles leave the used-frame count exactly unchanged (leak check). */
static void test_vm_teardown_leak_cycles(void) {
  tests_run++;
  uart_puts("  Running test_vm_teardown_leak_cycles...\n");

  int free0 = frame_free_count();
  for (int i = 0; i < 4; i++) {
    struct addr_space *as = vm_as_create(VM_TEST_TAG + 1); /* tag 61 */
    EXPECT_EQ((as != 0), 1);
    if (!as)
      return;
    struct process p;
    for (unsigned j = 0; j < sizeof(p); j++)
      ((unsigned char *)&p)[j] = 0;
    p.as = as;
    int64_t b = vm_mmap(&p, 0, 0x8000, VM_PROT_READ | VM_PROT_WRITE,
                        VM_MAP_PRIVATE | VM_MAP_ANONYMOUS, -1, 0);
    EXPECT_EQ((b > 0), 1);
    if (b <= 0) {
      vm_as_teardown(as);
      return;
    }
    EXPECT_EQ(vm_touch(&p, (uint64_t)b, 8, 1), 0);
    EXPECT_EQ(vm_touch(&p, (uint64_t)b + 0x4000, 8, 1), 0);
    /* churn: partial unmap, then a second mapping */
    EXPECT_EQ(vm_munmap_range(&p, (uint64_t)b + 0x4000, 0x1000), 0);
    int64_t b2 = vm_mmap(&p, 0, 0x1000, VM_PROT_READ | VM_PROT_WRITE,
                         VM_MAP_PRIVATE | VM_MAP_ANONYMOUS, -1, 0);
    EXPECT_EQ((b2 > 0), 1);
    vm_as_teardown(as);
  }
  EXPECT_EQ(frame_free_count(), free0);
}

/* L9 (prefetch): the eager first page — the entry page is resident right
 * after the loader mapped the image, everything else stays lazy. */
static void test_v2_image_first_page(void) {
  tests_run++;
  uart_puts("  Running test_v2_image_first_page...\n");

  int free0 = frame_free_count();
  struct addr_space *as = vm_as_create(VM_TEST_TAG + 1);
  EXPECT_EQ((as != 0), 1);
  if (!as)
    return;

  struct file f;
  if (fat16_open("BIG.BIN", &f) != 0) {
    uart_puts("ASSERTION FAILED: BIG.BIN missing from the boot disk\n");
    vm_as_teardown(as);
    return;
  }
  as->img_active = 1;
  as->img_file_size = f.fat16.entry.file_size;
  as->img_start_cluster = f.fat16.entry.start_cluster;
  fat16_close(&f);

  EXPECT_EQ(vm_region_insert(as, USER_IMG_BASE, 4 * FRAME_SIZE,
                             VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC,
                             VMK_IMG, VM_MAP_PRIVATE, 0, 0),
            0);
  EXPECT_EQ(vm_image_load_first_page(as), 0);
  uint64_t leaf = 0;
  EXPECT_EQ(vm_arch_walk(as, USER_IMG_BASE, &leaf), 0);
  const char *pat = "HO_BIGLOAD_30MB\n"; /* BIG.BIN's 16-byte generator unit */
  volatile uint8_t *p0 = (volatile uint8_t *)vm_arch_leaf_phys(leaf);
  int ok = 1;
  for (int i = 0; i < 16; i++) {
    if (p0[i] != (volatile uint8_t)pat[i])
      ok = 0;
  }
  EXPECT_EQ(ok, 1);
  /* The rest of the region stays lazy. */
  EXPECT_EQ(vm_arch_walk(as, USER_IMG_BASE + FRAME_SIZE, 0), -1);

  vm_as_teardown(as);
  EXPECT_EQ(frame_free_count(), free0);
}

/* L9 (prefetch): the background-loader engine driven synchronously (the
 * unit tier has no scheduler, so the kernel task itself cannot run here).
 * Sources the real 30 MB BIG.BIN from the boot disk and checks the
 * batched fill, the skip protocol, content parity and frame accounting. */
static void test_v2_image_prefetch(void) {
  tests_run++;
  uart_puts("  Running test_v2_image_prefetch...\n");

  int free0 = frame_free_count();
  struct addr_space *as = vm_as_create(VM_TEST_TAG);
  EXPECT_EQ((as != 0), 1);
  if (!as)
    return;

  struct file f;
  if (fat16_open("BIG.BIN", &f) != 0) {
    uart_puts("ASSERTION FAILED: BIG.BIN missing from the boot disk\n");
    vm_as_teardown(as);
    return;
  }
  uint32_t fsize = f.fat16.entry.file_size;
  uint16_t cluster = f.fat16.entry.start_cluster;
  fat16_close(&f);
  EXPECT_EQ((fsize >= (4u << 20)), 1);

  const uint64_t pages = 300; /* 1.2 MB window: 2 full batches + a tail */
  EXPECT_EQ(vm_region_insert(as, USER_IMG_BASE, pages * FRAME_SIZE,
                             VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC,
                             VMK_IMG, VM_MAP_PRIVATE, 0, 0),
            0);
  as->img_active = 1;
  as->img_file_size = fsize;
  as->img_start_cluster = cluster;

  /* The loader's eager first page; capture its frame to prove the
     prefetch pass skips (never remaps) resident pages. */
  EXPECT_EQ(vm_image_load_first_page(as), 0);
  uint64_t leaf0 = 0;
  EXPECT_EQ(vm_arch_walk(as, USER_IMG_BASE, &leaf0), 0);
  EXPECT_EQ((vm_arch_leaf_phys(leaf0) != 0), 1);

  struct vm_prefetch_stats st;
  int mapped = vm_image_prefetch_run(as, 0, pages, &st);
  EXPECT_EQ(st.stopped, 0);
  EXPECT_EQ(st.pages, pages);
  EXPECT_EQ(st.processed, pages);
  EXPECT_EQ(st.mapped, pages - 1); /* page 0 was already resident */
  EXPECT_EQ(st.skipped, 1);
  EXPECT_EQ((mapped == (int)(pages - 1)), 1);
  EXPECT_EQ((st.batches >= 3 && st.batches <= 8), 1); /* 300/128 -> 3 */

  uint64_t leaf0b = 0;
  EXPECT_EQ(vm_arch_walk(as, USER_IMG_BASE, &leaf0b), 0);
  EXPECT_EQ((leaf0b == leaf0), 1); /* skipped, not remapped */

  /* Every page resident; content byte-exact against a fresh positional
     read of the same file, plus the generator's 16-byte pattern at the
     window's page starts. */
  {
    uint64_t ref = frame_alloc();
    EXPECT_EQ((ref != 0), 1);
    int bad = 0;
    for (uint64_t p = 0; p < pages && ref; p++) {
      uint64_t leaf = 0;
      if (vm_arch_walk(as, USER_IMG_BASE + p * FRAME_SIZE, &leaf) != 0) {
        bad++;
        continue;
      }
      int n = fat16_read_direct_pos(cluster, fsize, ref, FRAME_SIZE,
                                    (uint32_t)(p * FRAME_SIZE));
      if (n != (int)FRAME_SIZE) {
        bad++;
        continue;
      }
      volatile uint8_t *a = (volatile uint8_t *)vm_arch_leaf_phys(leaf);
      volatile uint8_t *b = (volatile uint8_t *)ref;
      for (int i = 0; i < (int)FRAME_SIZE; i++) {
        if (a[i] != b[i]) {
          bad++;
          break;
        }
      }
      if (p == 0 || p == pages - 1) {
        const char *pat = "HO_BIGLOAD_30MB\n";
        for (int i = 0; i < 16; i++) {
          if (a[i] != (volatile uint8_t)pat[i])
            bad++;
        }
      }
    }
    if (ref)
      frame_free(ref);
    EXPECT_EQ(bad, 0);
  }

  /* A pass against a torn-down AS aborts without touching anything. */
  vm_as_teardown(as);
  struct vm_prefetch_stats st2;
  EXPECT_EQ(vm_image_prefetch_run(as, 0, pages, &st2) < 0, 1);
  EXPECT_EQ(frame_free_count(), free0);
}

/* w5 (iii): a page-table walk on a torn-down (zero-root) AS must return
 * -1 (absent) without dereferencing anything — the L9 prefetch kernel
 * task races a peer CPU's group teardown, and the w4-clang pid=5/pid=4
 * housekeeping EL1 fault (FAR 0xFFFF...F000, vm_arch_walk+0x2c) was a
 * walk deref of a dead/reused table chain.  Also (w5 iv): a recreated
 * AS must not inherit a stale fault-retry budget. */
static void test_vm_walk_dead_as_safe(void) {
  tests_run++;
  uart_puts("  Running test_vm_walk_dead_as_safe...\n");

  int free0 = frame_free_count();
  struct addr_space *as = vm_as_create(VM_TEST_TAG);
  EXPECT_EQ((as != 0), 1);
  if (!as)
    return;

  /* Live walks: absent reports -1; after a map the same VA reports 0. */
  EXPECT_EQ(vm_arch_walk(as, USER_IMG_BASE, 0), -1);
  uint64_t fr = frame_alloc();
  EXPECT_EQ((fr != 0), 1);
  EXPECT_EQ(vm_arch_map(as, USER_IMG_BASE, fr,
                        VM_PROT_READ | VM_PROT_EXEC, VMK_IMG),
            0);
  EXPECT_EQ(vm_arch_walk(as, USER_IMG_BASE, 0), 0);

  /* Teardown zeroes the root; a subsequent walk must return absent
     WITHOUT dereferencing (any deref here is a kernel-mode fault). */
  vm_as_teardown(as);
  EXPECT_EQ(as->root_phys, 0);
  EXPECT_EQ(vm_arch_walk(as, USER_IMG_BASE, 0), -1);
  EXPECT_EQ(vm_arch_walk(as, USER_IMG_BASE + 0x1000, 0), -1);

  /* w5 (iv): a fresh AS on the same slot starts with a clean budget. */
  as->retry_va = 0x12345000u;
  as->retry_count = 9;
  struct addr_space *as2 = vm_as_create(VM_TEST_TAG);
  EXPECT_EQ((as2 != 0), 1);
  if (as2) {
    EXPECT_EQ(as2->retry_count, 0);
    EXPECT_EQ(as2->retry_va, 0);
    vm_as_teardown(as2);
    EXPECT_EQ(frame_free_count(), free0);
  }
}

/* SEC-2 (security-plan.md #2): vm_user_read must validate and copy
 * atomically — never hand back a mapping it did not confirm under the same
 * lock, and never fault on holes / protection changes. */
static void test_vm_user_read(void) {
  tests_run++;
  uart_puts("  Running test_vm_user_read...\n");

  int free0 = frame_free_count();
  struct addr_space *as = vm_as_create(VM_TEST_TAG);
  EXPECT_EQ((as != 0), 1);
  if (!as)
    return;
  EXPECT_EQ(vm_region_insert(as, USER_MMAP_BASE_V2, 0x2000,
                             VM_PROT_READ | VM_PROT_WRITE, VMK_ANON,
                             VM_MAP_PRIVATE, 0, 0),
            0);
  struct process fake;
  for (unsigned i = 0; i < sizeof(fake); i++)
    ((unsigned char *)&fake)[i] = 0;
  fake.as = as;

  /* Materialize page one and stamp a pattern into its frame. */
  EXPECT_EQ(vm_touch(&fake, USER_MMAP_BASE_V2, 1, 1), 0);
  uint64_t leaf = 0;
  EXPECT_EQ(vm_arch_walk(as, USER_MMAP_BASE_V2, &leaf), 0);
  unsigned char *pa = (unsigned char *)vm_arch_leaf_phys(leaf);
  for (int i = 0; i < 64; i++)
    pa[i] = (unsigned char)(i + 0xA0);

  /* Read-back through the atomic primitive matches. */
  unsigned char out[80];
  EXPECT_EQ(vm_user_read(&fake, USER_MMAP_BASE_V2, out, 64), 0);
  int differs = 0;
  for (int i = 0; i < 64; i++)
    if (out[i] != (unsigned char)(i + 0xA0))
      differs = 1;
  EXPECT_EQ(differs, 0);

  /* Cross-page read materializes the second (lazy) page: succeeds. */
  for (int i = 0; i < 80; i++)
    out[i] = 0xFF;
  EXPECT_EQ(vm_user_read(&fake, USER_MMAP_BASE_V2 + 0xFF8, out, 16), 0);
  int nonzero = 0;
  for (int i = 0; i < 16; i++)
    if (out[i])
      nonzero = 1; /* both pages are zero-filled anon */
  EXPECT_EQ(nonzero, 0);

  /* Holes reject with -1 instead of faulting. */
  EXPECT_EQ(vm_user_read(&fake, USER_MMAP_BASE_V2 - 0x10, out, 16), -1);
  EXPECT_EQ(vm_user_read(&fake, USER_MMAP_BASE_V2 + 0x2000, out, 16), -1);

  /* Protection change is honored at read time: vm_mprotect rewrites the
     region (the same r->prot this primitive re-checks under lock). */
  EXPECT_EQ(vm_mprotect(&fake, USER_MMAP_BASE_V2, 0x1000, VM_PROT_WRITE), 0);
  EXPECT_EQ(vm_user_read(&fake, USER_MMAP_BASE_V2, out, 8), -1);

  /* Regression for the validate-then-use race: after a peer unmaps the
     page, an atomic read must NOT return stale bytes from the freed frame
     (the old pattern would have dereferenced it).  The anon region gets a
     fresh zeroed remap instead. */
  EXPECT_EQ(vm_mprotect(&fake, USER_MMAP_BASE_V2, 0x1000,
                        VM_PROT_READ | VM_PROT_WRITE),
            0);
  EXPECT_EQ(vm_unmap_page(as, USER_MMAP_BASE_V2), 0);
  for (int i = 0; i < 80; i++)
    out[i] = 0xFF;
  EXPECT_EQ(vm_user_read(&fake, USER_MMAP_BASE_V2, out, 64), 0);
  nonzero = 0;
  for (int i = 0; i < 64; i++)
    if (out[i])
      nonzero = 1; /* rematerialized zero page, never the old pattern */
  EXPECT_EQ(nonzero, 0);

  /* No process context: nothing to validate against -> reject. */
  EXPECT_EQ(vm_user_read(0, USER_MMAP_BASE_V2, out, 4), -1);
  /* Zero length is a no-op success. */
  EXPECT_EQ(vm_user_read(&fake, 0x12345678ULL, out, 0), 0);

  vm_as_teardown(as);
  EXPECT_EQ(frame_free_count(), free0);
}

/* SEC-8 (security-plan.md #8): vm_kwrite must self-validate per page —
 * materialize, re-check region + write protection + leaf PTE under
 * vm_lock, and only then store.  Holes reject, read-only ranges reject
 * (the old form wrote through any resolvable PTE regardless of the
 * region's current protection), and demand pages materialize on write. */
static void test_vm_kwrite(void) {
  tests_run++;
  uart_puts("  Running test_vm_kwrite...\n");

  int free0 = frame_free_count();
  struct addr_space *as = vm_as_create(VM_TEST_TAG);
  EXPECT_EQ((as != 0), 1);
  if (!as)
    return;
  EXPECT_EQ(vm_region_insert(as, USER_MMAP_BASE_V2, 0x2000,
                             VM_PROT_READ | VM_PROT_WRITE, VMK_ANON,
                             VM_MAP_PRIVATE, 0, 0),
            0);
  struct process fake;
  for (unsigned i = 0; i < sizeof(fake); i++)
    ((unsigned char *)&fake)[i] = 0;
  fake.as = as;

  unsigned char pat[80];
  for (int i = 0; i < 80; i++)
    pat[i] = (unsigned char)(i + 0x3C);

  /* Fully lazy region: the write materializes page one on demand. */
  EXPECT_EQ(vm_kwrite(&fake, USER_MMAP_BASE_V2, pat, 64), 0);
  uint64_t leaf = 0;
  EXPECT_EQ(vm_arch_walk(as, USER_MMAP_BASE_V2, &leaf), 0);
  unsigned char *pa1 = (unsigned char *)vm_arch_leaf_phys(leaf);
  int differs = 0;
  for (int i = 0; i < 64; i++)
    if (pa1[i] != pat[i])
      differs = 1;
  EXPECT_EQ(differs, 0);

  /* Cross-page store lands on both pages. */
  EXPECT_EQ(vm_kwrite(&fake, USER_MMAP_BASE_V2 + 0xFF8, pat, 16), 0);
  for (int i = 0; i < 8; i++)
    if (pa1[0xFF8 + i] != pat[i])
      differs = 1;
  uint64_t leaf2 = 0;
  EXPECT_EQ(vm_arch_walk(as, USER_MMAP_BASE_V2 + 0x1000, &leaf2), 0);
  unsigned char *pa2 = (unsigned char *)vm_arch_leaf_phys(leaf2);
  for (int i = 0; i < 8; i++)
    if (pa2[i] != pat[8 + i])
      differs = 1;
  EXPECT_EQ(differs, 0);

  /* Holes reject with -1 instead of faulting or writing stale frames. */
  EXPECT_EQ(vm_kwrite(&fake, USER_MMAP_BASE_V2 - 0x10, pat, 16), -1);
  EXPECT_EQ(vm_kwrite(&fake, USER_MMAP_BASE_V2 + 0x2000, pat, 16), -1);

  /* Protection change is honored at write time: vm_mprotect rewrites the
     region (the same r->prot this primitive re-checks under lock).  The
     pre-existing bytes must survive the rejected call untouched. */
  EXPECT_EQ(vm_mprotect(&fake, USER_MMAP_BASE_V2, 0x1000, VM_PROT_READ), 0);
  unsigned char before = pa1[0];
  EXPECT_EQ(vm_kwrite(&fake, USER_MMAP_BASE_V2, pat, 8), -1);
  EXPECT_EQ((int)pa1[0], (int)before);

  /* v1 process (no address space): caller owns translation -> reject. */
  struct process v1;
  for (unsigned i = 0; i < sizeof(v1); i++)
    ((unsigned char *)&v1)[i] = 0;
  EXPECT_EQ(vm_kwrite(&v1, USER_MMAP_BASE_V2, pat, 4), -1);

  /* Zero length is a no-op success (contract preserved). */
  EXPECT_EQ(vm_kwrite(&fake, 0x12345678ULL, pat, 0), 0);

  vm_as_teardown(as);
  EXPECT_EQ(frame_free_count(), free0);
}

/* =====================================================================
 * so-vm lane A (docs/shared-libs.md section 4): file-backed shared
 * mappings.  SYS_MMAP of a regular FAT16 file goes through the
 * VM_OBJ_FILE page cache, so two address spaces mapping one file share
 * the same physical frames until a MAP_PRIVATE write breaks the copy.
 * The fixture is created through the FAT16 API on the unit-test disk
 * (VMOBJ1.DAT, 8.3) and tolerates a leftover copy from an earlier run:
 * it is truncated and rewritten every time.
 * ===================================================================== */

#define VM_FILE_TAG1 60
#define VM_FILE_TAG2 61
#define VM_FILE_NAME "/VMOBJ1.DAT"
#define VM_FILE_LEN 5000u /* 0x1388: 2 pages, 0x388-byte zero tail past EOF */

static void vm_file_pat(unsigned char *b, int n, uint32_t start) {
  for (int i = 0; i < n; i++)
    b[i] = (unsigned char)(((start + (uint32_t)i) * 7u + 3u) & 0xFFu);
}

/* (Re)write the fixture and report the identity SYS_MMAP keys the page
 * cache by.  Returns 0 / -1. */
static int vm_file_fixture(uint16_t *cluster, uint32_t *size) {
  struct file f;
  for (unsigned i = 0; i < sizeof(f); i++)
    ((unsigned char *)&f)[i] = 0;
  if (fat16_open(VM_FILE_NAME, &f) != 0)
    return -1;
  if (fat16_truncate_to(&f, 0) != 0) {
    fat16_close(&f);
    return -1;
  }
  unsigned char buf[125]; /* chunked: no large kernel stack frames */
  uint32_t done = 0;
  while (done < VM_FILE_LEN) {
    int n = (int)(VM_FILE_LEN - done);
    if (n > (int)sizeof(buf))
      n = (int)sizeof(buf);
    vm_file_pat(buf, n, done);
    int w = fat16_write(&f, buf, n);
    if (w != n) {
      fat16_close(&f);
      return -1;
    }
    done += (uint32_t)n;
  }
  fat16_close(&f);

  struct file g;
  for (unsigned i = 0; i < sizeof(g); i++)
    ((unsigned char *)&g)[i] = 0;
  if (fat16_open(VM_FILE_NAME, &g) != 0)
    return -1;
  *cluster = g.fat16.entry.start_cluster;
  *size = g.fat16.entry.file_size;
  fat16_close(&g);
  return (*cluster != 0 && *size == VM_FILE_LEN) ? 0 : -1;
}

/* A fake process with a usable fd table (open_fds[] must read -1 = free). */
static void vm_file_proc_init(struct process *p, struct addr_space *as) {
  for (unsigned i = 0; i < sizeof(*p); i++)
    ((unsigned char *)p)[i] = 0;
  for (int i = 0; i < MAX_OPEN_FDS; i++)
    p->open_fds[i] = -1;
  p->num_open_fds = 0;
  p->as = as;
}

/* (a) one file, two address spaces, the SAME physical frames;
 * (b) a MAP_PRIVATE write is invisible to the peer and to the object page;
 * (c) a MAP_SHARED write IS visible through the other mapping. */
static void test_vm_file_map_shared_frames(void) {
  tests_run++;
  uart_puts("  Running test_vm_file_map_shared_frames...\n");

  int free0 = frame_free_count();
  int c0 = file_obj_count();
  uint16_t cluster = 0;
  uint32_t fsize = 0;
  if (vm_file_fixture(&cluster, &fsize) != 0) {
    uart_puts("ASSERTION FAILED: VMOBJ1.DAT fixture not creatable\n");
    return;
  }

  struct process p1, p2;
  vm_file_proc_init(&p1, vm_as_create(VM_FILE_TAG1));
  vm_file_proc_init(&p2, vm_as_create(VM_FILE_TAG2));
  EXPECT_EQ((p1.as != 0 && p2.as != 0), 1);
  if (!p1.as || !p2.as) {
    if (p1.as)
      vm_as_teardown(p1.as);
    if (p2.as)
      vm_as_teardown(p2.as);
    return;
  }
  int fd1 = file_open(&p1, VM_FILE_NAME, 0);
  int fd2 = file_open(&p2, VM_FILE_NAME, 0);
  EXPECT_EQ((fd1 >= 0 && fd2 >= 0), 1);
  if (fd1 < 0 || fd2 < 0)
    goto out;

  /* (a) MAP_PRIVATE in both spaces — the DSO text case. */
  int64_t b1 = vm_mmap(&p1, 0, fsize, VM_PROT_READ | VM_PROT_WRITE,
                       VM_MAP_PRIVATE, fd1, 0);
  int64_t b2 = vm_mmap(&p2, 0, fsize, VM_PROT_READ | VM_PROT_WRITE,
                       VM_MAP_PRIVATE, fd2, 0);
  EXPECT_EQ((b1 > 0 && b2 > 0), 1);
  EXPECT_EQ((b1 == b2), 1); /* same slot in two independent ASs */
  EXPECT_EQ(file_obj_count(), c0 + 1); /* ONE cache object for the file */
  if (b1 <= 0 || b2 <= 0)
    goto out;

  struct vm_region *r = vm_region_find(p1.as, (uint64_t)b1);
  EXPECT_EQ((r != 0), 1);
  EXPECT_EQ((r && r->kind == VMK_LIB), 1);
  EXPECT_EQ((r && r->obj != 0), 1);
  EXPECT_EQ((r && r->obj && r->obj->kind == VM_OBJ_FILE), 1);

  EXPECT_EQ(vm_touch(&p1, (uint64_t)b1, 8, 0), 0);
  EXPECT_EQ(vm_touch(&p2, (uint64_t)b2, 8, 0), 0);
  uint64_t l1 = 0, l2 = 0;
  EXPECT_EQ(vm_arch_walk(p1.as, (uint64_t)b1, &l1), 0);
  EXPECT_EQ(vm_arch_walk(p2.as, (uint64_t)b2, &l2), 0);
  uint64_t ph1 = vm_arch_leaf_phys(l1);
  uint64_t ph2 = vm_arch_leaf_phys(l2);
  /* THE acceptance property: two address spaces, one physical frame. */
  EXPECT_EQ((ph1 != 0 && ph1 == ph2), 1);
  /* ... and that frame is the page cache's own page 0. */
  EXPECT_EQ((r && r->obj && vm_object_peek(r->obj, 0) == ph1), 1);
  /* Printed as quotable evidence: the two ASs' leaves, and the object page,
     are one and the same physical frame. */
  uart_puts("  [VM-FILE] as1 leaf=");
  uart_print_hex(ph1);
  uart_puts(" as2 leaf=");
  uart_print_hex(ph2);
  uart_puts(" object page0=");
  uart_print_hex(r ? vm_object_peek(r->obj, 0) : 0);
  uart_puts("\n");

  /* The page really came from the file (not a zero fill). */
  {
    int bad = 0;
    for (int i = 0; i < 64; i++) {
      if (((const volatile unsigned char *)ph1)[i] !=
          (unsigned char)((i * 7 + 3) & 0xFF))
        bad = 1;
    }
    EXPECT_EQ(bad, 0);
  }

  /* (b) a write fault in p1 copies: new private frame, same bytes. */
  const char *why = 0;
  EXPECT_EQ(vm_handle_fault(&p1, (uint64_t)b1, 1, 0, &why), 0);
  uint64_t l1b = 0;
  EXPECT_EQ(vm_arch_walk(p1.as, (uint64_t)b1, &l1b), 0);
  uint64_t ph1b = vm_arch_leaf_phys(l1b);
  EXPECT_EQ((ph1b != 0 && ph1b != ph1), 1);
  {
    int bad = 0;
    for (int i = 0; i < 64; i++) {
      if (((const volatile unsigned char *)ph1b)[i] !=
          (unsigned char)((i * 7 + 3) & 0xFF))
        bad = 1;
    }
    EXPECT_EQ(bad, 0); /* the copy carries the file's bytes */
  }
  ((volatile unsigned char *)ph1b)[0] = 0xEE;
  /* Peer mapping and object page are untouched. */
  EXPECT_EQ((int)(((const volatile unsigned char *)ph2)[0]), 3);
  EXPECT_EQ((int)(((const volatile unsigned char *)vm_object_peek(r->obj, 0))[0]),
            3);
  /* The private page stays private: a second write never COWs again. */
  EXPECT_EQ(vm_touch(&p1, (uint64_t)b1, 1, 1), 0);
  uint64_t l1c = 0;
  EXPECT_EQ(vm_arch_walk(p1.as, (uint64_t)b1, &l1c), 0);
  EXPECT_EQ((vm_arch_leaf_phys(l1c) == ph1b), 1);
  EXPECT_EQ((int)(((const volatile unsigned char *)ph1b)[0]), 0xEE);

  /* (c) MAP_SHARED in both spaces: one frame, write-through both ways. */
  int64_t s1 = vm_mmap(&p1, 0, fsize, VM_PROT_READ | VM_PROT_WRITE,
                       VM_MAP_SHARED, fd1, 0);
  int64_t s2 = vm_mmap(&p2, 0, fsize, VM_PROT_READ | VM_PROT_WRITE,
                       VM_MAP_SHARED, fd2, 0);
  EXPECT_EQ((s1 > 0 && s2 > 0), 1);
  EXPECT_EQ((s1 == s2), 1);
  if (s1 > 0 && s2 > 0) {
    struct vm_region *rs = vm_region_find(p1.as, (uint64_t)s1);
    EXPECT_EQ((rs && rs->kind == VMK_LIB && rs->obj == r->obj), 1);
    EXPECT_EQ(vm_touch(&p1, (uint64_t)s1, 8, 1), 0);
    EXPECT_EQ(vm_touch(&p2, (uint64_t)s2, 8, 0), 0);
    uint64_t ls1 = 0, ls2 = 0;
    EXPECT_EQ(vm_arch_walk(p1.as, (uint64_t)s1, &ls1), 0);
    EXPECT_EQ(vm_arch_walk(p2.as, (uint64_t)s2, &ls2), 0);
    uint64_t shp1 = vm_arch_leaf_phys(ls1);
    uint64_t shp2 = vm_arch_leaf_phys(ls2);
    EXPECT_EQ((shp1 != 0 && shp1 == shp2), 1);
    EXPECT_EQ((shp1 == ph1), 1); /* the very same page-cache page */
    *(volatile uint64_t *)shp1 = 0xCAFEF00D12345678ULL;
    EXPECT_EQ((*(volatile uint64_t *)shp2 == 0xCAFEF00D12345678ULL), 1);
    /* ... and a third view (the object page the private mapping bypassed)
       sees the same write: MAP_SHARED really is write-through. */
    EXPECT_EQ((*(volatile uint64_t *)vm_object_peek(r->obj, 0) ==
               0xCAFEF00D12345678ULL),
              1);
  }

out:
  if (p1.as)
    vm_as_teardown(p1.as);
  if (p2.as)
    vm_as_teardown(p2.as);
  if (fd1 >= 0)
    file_close(&p1, fd1);
  if (fd2 >= 0)
    file_close(&p2, fd2);
  /* The page cache owns its frames by design; flush it so the suite's
     exact frame accounting starts from the recorded baseline. */
  file_obj_invalidate_all();
  EXPECT_EQ(file_obj_count(), c0);
  EXPECT_EQ(frame_free_count(), free0);
}

/* (d) a file whose size is not a page multiple: the bytes past EOF in the
 * last page read as zeros.  (5) mprotect can drop PROT_WRITE on a mapped
 * file region (W^X) and the next write faults cleanly; mprotect that ADDS
 * PROT_WRITE never hands out a writable shared text page. */
static void test_vm_file_map_tail_wx(void) {
  tests_run++;
  uart_puts("  Running test_vm_file_map_tail_wx...\n");

  int free0 = frame_free_count();
  int c0 = file_obj_count();
  uint16_t cluster = 0;
  uint32_t fsize = 0;
  if (vm_file_fixture(&cluster, &fsize) != 0) {
    uart_puts("ASSERTION FAILED: VMOBJ1.DAT fixture not creatable\n");
    return;
  }

  struct process p;
  vm_file_proc_init(&p, vm_as_create(VM_FILE_TAG1));
  EXPECT_EQ((p.as != 0), 1);
  if (!p.as)
    return;
  int fd = file_open(&p, VM_FILE_NAME, 0);
  EXPECT_EQ((fd >= 0), 1);

  int64_t b = vm_mmap(&p, 0, fsize, VM_PROT_READ, VM_MAP_PRIVATE, fd, 0);
  EXPECT_EQ((b > 0), 1);
  if (fd < 0 || b <= 0)
    goto out;

  /* (d) page 1 = file bytes 4096..4999 then zeros to the page end. */
  EXPECT_EQ(vm_touch(&p, (uint64_t)b + 0x1000, 8, 0), 0);
  {
    uint64_t leaf = 0;
    EXPECT_EQ(vm_arch_walk(p.as, (uint64_t)b + 0x1000, &leaf), 0);
    const volatile unsigned char *pa =
        (const volatile unsigned char *)vm_arch_leaf_phys(leaf);
    EXPECT_EQ((int)pa[0], (int)((4096u * 7u + 3u) & 0xFFu));
    EXPECT_EQ((int)pa[903], (int)((4999u * 7u + 3u) & 0xFFu));
    EXPECT_EQ((int)pa[904], 0); /* first byte past EOF */
    EXPECT_EQ((int)pa[4095], 0);
    int nz = 0;
    for (int i = 904; i < 4096; i++) {
      if (pa[i])
        nz = 1;
    }
    EXPECT_EQ(nz, 0); /* the whole tail is zero-filled */
  }

  /* (5) W^X: adding PROT_WRITE to a resident file page must not make the
     shared object frame writable — the write breaks the copy instead. */
  uint64_t objleaf = 0;
  EXPECT_EQ(vm_arch_walk(p.as, (uint64_t)b + 0x1000, &objleaf), 0);
  uint64_t objph = vm_arch_leaf_phys(objleaf);
  EXPECT_EQ(vm_mprotect(&p, (uint64_t)b + 0x1000, 0x1000,
                        VM_PROT_READ | VM_PROT_WRITE),
            0);
  const char *why = 0;
  EXPECT_EQ(vm_handle_fault(&p, (uint64_t)b + 0x1000, 1, 0, &why), 0);
  {
    uint64_t leaf = 0;
    EXPECT_EQ(vm_arch_walk(p.as, (uint64_t)b + 0x1000, &leaf), 0);
    EXPECT_EQ((vm_arch_leaf_phys(leaf) != objph), 1);
    /* the object page still carries the file's byte, not our write */
    EXPECT_EQ((int)(((const volatile unsigned char *)objph)[0]), 
              (int)((4096u * 7u + 3u) & 0xFFu));
  }

  /* A lazy page (never touched) under mprotect(PROT_READ|PROT_WRITE): the
     fill installs it read-only, so the write fault is the COW, never a
     direct store into the page cache. */
  EXPECT_EQ(vm_mprotect(&p, (uint64_t)b, 0x1000,
                        VM_PROT_READ | VM_PROT_WRITE),
            0);
  EXPECT_EQ(vm_arch_walk(p.as, (uint64_t)b, 0), -1);
  EXPECT_EQ(vm_handle_fault(&p, (uint64_t)b, 1, 0, &why), 0);
  {
    uint64_t leaf = 0;
    EXPECT_EQ(vm_arch_walk(p.as, (uint64_t)b, &leaf), 0);
    uint64_t ph = vm_arch_leaf_phys(leaf);
    struct vm_region *rr = vm_region_find(p.as, (uint64_t)b);
    EXPECT_EQ((ph != 0 && rr && rr->obj && ph != vm_object_peek(rr->obj, 0)), 1);
  }

  /* Drop PROT_WRITE (the interpreter's post-relocation W^X step): the
     resident page goes read-only and the next write is a clean PROT kill,
     never a silent store. */
  EXPECT_EQ(vm_mprotect(&p, (uint64_t)b, 0x2000, VM_PROT_READ), 0);
  EXPECT_EQ(vm_handle_fault(&p, (uint64_t)b, 1, 0, &why), 1);
  EXPECT_EQ((why && why[0] == 'P'), 1); /* "PROT" */

out:
  if (p.as)
    vm_as_teardown(p.as);
  if (fd >= 0)
    file_close(&p, fd);
  file_obj_invalidate_all();
  EXPECT_EQ(file_obj_count(), c0);
  EXPECT_EQ(frame_free_count(), free0);
}

/* (e) munmap and AS teardown never free the page cache's frames: the
 * object keeps them (other mappings may share them), and only the cache
 * flush returns the pool to the exact baseline. */
static void test_vm_file_map_accounting(void) {
  tests_run++;
  uart_puts("  Running test_vm_file_map_accounting...\n");

  int free0 = frame_free_count();
  int c0 = file_obj_count();
  uint16_t cluster = 0;
  uint32_t fsize = 0;
  if (vm_file_fixture(&cluster, &fsize) != 0) {
    uart_puts("ASSERTION FAILED: VMOBJ1.DAT fixture not creatable\n");
    return;
  }

  struct process p;
  vm_file_proc_init(&p, vm_as_create(VM_FILE_TAG1));
  EXPECT_EQ((p.as != 0), 1);
  if (!p.as)
    return;
  int fd = file_open(&p, VM_FILE_NAME, 0);
  EXPECT_EQ((fd >= 0), 1);
  int64_t b = vm_mmap(&p, 0, fsize, VM_PROT_READ | VM_PROT_WRITE,
                      VM_MAP_PRIVATE, fd, 0);
  EXPECT_EQ((b > 0), 1);
  if (fd < 0 || b <= 0)
    goto out;

  /* Fault both pages in: two page-cache frames + the AS's own frames. */
  EXPECT_EQ(vm_touch(&p, (uint64_t)b, 8, 0), 0);
  EXPECT_EQ(vm_touch(&p, (uint64_t)b + 0x1000, 8, 0), 0);
  EXPECT_EQ(p.as->resident_frames, 2);
  struct vm_region *r = vm_region_find(p.as, (uint64_t)b);
  EXPECT_EQ((r && r->obj != 0), 1);
  uint64_t ph0 = vm_object_peek(r->obj, 0);
  uint64_t ph1 = vm_object_peek(r->obj, 0x1000);
  EXPECT_EQ((ph0 != 0 && ph1 != 0 && ph0 != ph1), 1);
  int f_mapped = frame_free_count();
  EXPECT_EQ((f_mapped < free0), 1);

  /* munmap: resident object pages are dropped from the mapping WITHOUT
     freeing their frames (the object still owns them). */
  EXPECT_EQ(vm_munmap_range(&p, (uint64_t)b, fsize), 0);
  EXPECT_EQ(p.as->resident_frames, 0);
  EXPECT_EQ(frame_free_count(), f_mapped);
  EXPECT_EQ((vm_region_find(p.as, (uint64_t)b) == 0), 1);
  EXPECT_EQ((vm_object_peek(r->obj, 0) == ph0), 1);
  EXPECT_EQ((vm_object_peek(r->obj, 0x1000) == ph1), 1);

  /* Teardown: only the AS's own table frames and region array come back;
     the cache's frames stay allocated. */
  {
    int tabs = p.as->table_frames;
    int arrf = (int)(((uint64_t)p.as->cap * sizeof(struct vm_region)) /
                     FRAME_SIZE) + 1;
    int f_pre = frame_free_count();
    vm_as_teardown(p.as);
    p.as = 0;
    EXPECT_EQ(frame_free_count(), f_pre + tabs + arrf);
    EXPECT_EQ((frame_free_count() < free0), 1); /* cache frames still out */
  }

  /* Flushing the cache returns every remaining frame: exact baseline. */
  file_obj_invalidate_all();
  EXPECT_EQ(file_obj_count(), c0);
  EXPECT_EQ(frame_free_count(), free0);

out:
  if (p.as)
    vm_as_teardown(p.as);
  if (fd >= 0)
    file_close(&p, fd);
  file_obj_invalidate_all();
  EXPECT_EQ(frame_free_count(), free0);
}

/* (f) fork with a file mapping (vm_as_clone_into): a page still pointing at
 * the object's frame is re-mapped SHARED in the child (same physical frame);
 * a page the parent already copied-on-write is copied again, so the child
 * never inherits the object's stale text. */
static void test_vm_file_map_fork(void) {
  tests_run++;
  uart_puts("  Running test_vm_file_map_fork...\n");

  int free0 = frame_free_count();
  int c0 = file_obj_count();
  uint16_t cluster = 0;
  uint32_t fsize = 0;
  if (vm_file_fixture(&cluster, &fsize) != 0) {
    uart_puts("ASSERTION FAILED: VMOBJ1.DAT fixture not creatable\n");
    return;
  }

  struct process p;
  vm_file_proc_init(&p, vm_as_create(VM_FILE_TAG1 + 1)); /* tag 61 */
  EXPECT_EQ((p.as != 0), 1);
  if (!p.as)
    return;
  int fd = file_open(&p, VM_FILE_NAME, 0);
  EXPECT_EQ((fd >= 0), 1);
  int64_t b = vm_mmap(&p, 0, fsize, VM_PROT_READ | VM_PROT_WRITE,
                      VM_MAP_PRIVATE, fd, 0);
  EXPECT_EQ((b > 0), 1);
  struct addr_space *child = 0;
  if (fd < 0 || b <= 0)
    goto out;

  struct vm_region *r = vm_region_find(p.as, (uint64_t)b);
  EXPECT_EQ((r && r->obj != 0), 1);
  if (!r || !r->obj)
    goto out;

  /* Page 0: read-touch (object frame), then a write fault copies it. */
  EXPECT_EQ(vm_touch(&p, (uint64_t)b, 8, 0), 0);
  const char *why = 0;
  EXPECT_EQ(vm_handle_fault(&p, (uint64_t)b, 1, 0, &why), 0);
  uint64_t lp0 = 0;
  EXPECT_EQ(vm_arch_walk(p.as, (uint64_t)b, &lp0), 0);
  uint64_t priv0 = vm_arch_leaf_phys(lp0);
  EXPECT_EQ((priv0 != 0 && priv0 != vm_object_peek(r->obj, 0)), 1);
  ((volatile unsigned char *)priv0)[0] = 0x5A; /* parent-private edit */

  /* Page 1: read-touch only — still the object's shared frame. */
  EXPECT_EQ(vm_touch(&p, (uint64_t)b + 0x1000, 8, 0), 0);
  uint64_t lp1 = 0;
  EXPECT_EQ(vm_arch_walk(p.as, (uint64_t)b + 0x1000, &lp1), 0);
  uint64_t shared1 = vm_arch_leaf_phys(lp1);
  EXPECT_EQ((shared1 != 0 && shared1 == vm_object_peek(r->obj, 0x1000)), 1);

  child = vm_as_clone(p.as, VM_FILE_TAG1 + 2); /* tag 62 */
  EXPECT_EQ((child != 0), 1);
  if (!child)
    goto out;
  struct process pc;
  vm_file_proc_init(&pc, child); /* the child's fake PCB (fault context) */

  /* The COW'd page is copied: different frame, same content (0x5A), and the
     object page still holds the file's byte. */
  uint64_t lc0 = 0;
  EXPECT_EQ(vm_arch_walk(child, (uint64_t)b, &lc0), 0);
  uint64_t c0phys = vm_arch_leaf_phys(lc0);
  EXPECT_EQ((c0phys != 0 && c0phys != priv0), 1);
  EXPECT_EQ((int)(((const volatile unsigned char *)c0phys)[0]), 0x5A);
  EXPECT_EQ((int)(((const volatile unsigned char *)vm_object_peek(r->obj, 0))[0]),
            3);
  /* The shared page keeps the SAME physical frame across the fork. */
  uint64_t lc1 = 0;
  EXPECT_EQ(vm_arch_walk(child, (uint64_t)b + 0x1000, &lc1), 0);
  EXPECT_EQ((vm_arch_leaf_phys(lc1) == shared1), 1);
  /* A child write to it copies instead of dirtying the page cache. */
  EXPECT_EQ(vm_handle_fault(&pc, (uint64_t)b + 0x1000, 1, 0, &why), 0);
  uint64_t lc1b = 0;
  EXPECT_EQ(vm_arch_walk(child, (uint64_t)b + 0x1000, &lc1b), 0);
  EXPECT_EQ((vm_arch_leaf_phys(lc1b) != shared1), 1);
  EXPECT_EQ((int)(((const volatile unsigned char *)shared1)[0]),
            (int)((4096u * 7u + 3u) & 0xFFu));

out:
  if (child)
    vm_as_teardown(child);
  if (p.as)
    vm_as_teardown(p.as);
  if (fd >= 0)
    file_close(&p, fd);
  file_obj_invalidate_all();
  EXPECT_EQ(file_obj_count(), c0);
  EXPECT_EQ(frame_free_count(), free0);
}

/* M6/L30 (a): the COW decision must not depend on the resident leaf being
 * the region object's page.  Measured state from the RimWorld boot: the VMA
 * permits the write, the PTE is read-only, and the leaf is NOT what the
 * region's object would serve for that offset — the old test refused the
 * copy, the "present but permissive" retry refaulted FAULT_RETRY_MAX times
 * and the process was killed with in=PROT.  Build that state exactly (a
 * private page a protection change left read-only under a writable VMA) and
 * require: the write fault succeeds, the old page is untouched, and the new
 * leaf is a writable private copy carrying the old bytes.
 *
 * (b) it also proves vm_arch_leaf_writable() and the fill path agree: the
 * object's page-cache frame stays read-only and is never written. */
static void test_vm_file_cow_private_leaf(void) {
  tests_run++;
  uart_puts("  Running test_vm_file_cow_private_leaf...\n");

  int free0 = frame_free_count();
  int c0 = file_obj_count();
  uint16_t cluster = 0;
  uint32_t fsize = 0;
  if (vm_file_fixture(&cluster, &fsize) != 0) {
    uart_puts("ASSERTION FAILED: VMOBJ1.DAT fixture not creatable\n");
    return;
  }

  struct process p;
  vm_file_proc_init(&p, vm_as_create(VM_FILE_TAG1));
  EXPECT_EQ((p.as != 0), 1);
  if (!p.as)
    return;
  int fd = file_open(&p, VM_FILE_NAME, 0);
  EXPECT_EQ((fd >= 0), 1);

  int64_t b = vm_mmap(&p, 0, fsize, VM_PROT_READ | VM_PROT_WRITE,
                      VM_MAP_PRIVATE, fd, 0);
  EXPECT_EQ((b > 0), 1);
  if (fd < 0 || b <= 0)
    goto out;

  /* First touch is a READ fill: the object's frame, installed read-only. */
  EXPECT_EQ(vm_touch(&p, (uint64_t)b, 8, 0), 0);
  uint64_t l0 = 0;
  EXPECT_EQ(vm_arch_walk(p.as, (uint64_t)b, &l0), 0);
  uint64_t objfr = vm_arch_leaf_phys(l0);
  EXPECT_EQ(vm_arch_leaf_writable(l0), 0); /* read-only: the COW setup */
  struct vm_region *r = vm_region_find(p.as, (uint64_t)b);
  EXPECT_EQ((r && r->obj && vm_object_peek(r->obj, 0) == objfr), 1);

  /* The write fault breaks the copy: private, writable, same bytes. */
  const char *why = 0;
  EXPECT_EQ(vm_handle_fault(&p, (uint64_t)b, 1, 0, &why), 0);
  uint64_t l1 = 0;
  EXPECT_EQ(vm_arch_walk(p.as, (uint64_t)b, &l1), 0);
  uint64_t priv = vm_arch_leaf_phys(l1);
  EXPECT_EQ((priv != 0 && priv != objfr), 1);
  EXPECT_EQ(vm_arch_leaf_writable(l1), 1);
  EXPECT_EQ((int)(((const volatile unsigned char *)priv)[0]),
            (int)((0u * 7u + 3u) & 0xFFu));

  /* A protection change that leaves the PRIVATE page read-only while the VMA
     still says read-write: exactly the disagreement the boot hit.  The leaf
     is no longer the object's frame, so the old identity test said "not a
     COW" and looped on the retry path until it killed the process. */
  vm_arch_prot(p.as, (uint64_t)b, VM_PROT_READ);
  uint64_t l2 = 0;
  EXPECT_EQ(vm_arch_walk(p.as, (uint64_t)b, &l2), 0);
  EXPECT_EQ(vm_arch_leaf_writable(l2), 0);
  EXPECT_EQ(vm_arch_leaf_phys(l2), priv); /* still our private page */
  struct vm_region *r2 = vm_region_find(p.as, (uint64_t)b);
  EXPECT_EQ((r2 && (r2->prot & VM_PROT_WRITE) && r2->obj &&
             vm_object_peek(r2->obj, 0) != priv),
            1); /* VMA writable, leaf neither writable nor the object page */

  ((volatile unsigned char *)priv)[0] = 0x5A; /* a private byte to carry */
  EXPECT_EQ((int)(((const volatile unsigned char *)priv)[0]), 0x5A);
  why = 0;
  EXPECT_EQ(vm_handle_fault(&p, (uint64_t)b, 1, 0, &why), 0);
  uint64_t l3 = 0;
  EXPECT_EQ(vm_arch_walk(p.as, (uint64_t)b, &l3), 0);
  uint64_t priv2 = vm_arch_leaf_phys(l3);
  EXPECT_EQ((priv2 != 0 && priv2 != priv), 1);   /* a NEW private copy */
  EXPECT_EQ(vm_arch_leaf_writable(l3), 1);       /* and it is writable */
  EXPECT_EQ((int)(((const volatile unsigned char *)priv2)[0]), 0x5A);
  /* The object's page-cache frame still carries the file's byte: the copy
     never touched the page every other mapping shares. */
  EXPECT_EQ((int)(((const volatile unsigned char *)objfr)[0]),
            (int)((0u * 7u + 3u) & 0xFFu));

  /* REGRESSION (measured, docs/rimworld-launch-log.md §19.6/§20): a loader
     that dlopen/dlcloses probe DSOs re-uses one base, so the SAME VA is
     copy-on-write relocated once per load — five times in the RimWorld audio
     probe.  Charging each correct COW to the "keeps refaulting" budget killed
     the fifth with in=PROT.  Eight consecutive COWs on one VA must all
     succeed and each must leave a writable private page. */
  for (int i = 0; i < 8; i++) {
    vm_arch_prot(p.as, (uint64_t)b, VM_PROT_READ); /* next load's RO fill */
    const char *w2 = 0;
    if (vm_handle_fault(&p, (uint64_t)b, 1, 0, &w2) != 0) {
      uart_puts("ASSERTION FAILED: COW #");
      print_int(i + 1);
      uart_puts(" on the same VA was killed (in=");
      uart_puts(w2 ? w2 : "?");
      uart_puts(")\n");
      EXPECT_EQ(1, 0);
      break;
    }
    uint64_t lr = 0;
    if (vm_arch_walk(p.as, (uint64_t)b, &lr) != 0 ||
        !vm_arch_leaf_writable(lr)) {
      EXPECT_EQ(1, 0); /* a COW must install a writable page */
      break;
    }
  }

out:
  if (p.as)
    vm_as_teardown(p.as);
  if (fd >= 0)
    file_close(&p, fd);
  file_obj_invalidate_all();
  uart_puts("  [VM-FILE] cow_private_leaf objects c0=");
  print_int(c0);
  uart_puts(" now=");
  print_int(file_obj_count());
  uart_puts(" frames free0=");
  print_int(free0);
  uart_puts(" now=");
  print_int(frame_free_count());
  uart_puts("\n");
  EXPECT_EQ(file_obj_count(), c0);
  EXPECT_EQ(frame_free_count(), free0);
}

/* M6/L30 (b): the DSO slot allocator.  A loader that maps several DSOs of
 * colliding size/hash (our audio shims were byte-identical) must get a
 * DISTINCT base for every live mapping, and a hint that names an occupied
 * base must be skipped rather than honoured.  Also: one page-cache object
 * per file no matter how many times it is mapped. */
static void test_vm_dso_bases_distinct(void) {
  tests_run++;
  uart_puts("  Running test_vm_dso_bases_distinct...\n");

  int free0 = frame_free_count();
  int c0 = file_obj_count();
  uint16_t cluster = 0;
  uint32_t fsize = 0;
  if (vm_file_fixture(&cluster, &fsize) != 0) {
    uart_puts("ASSERTION FAILED: VMOBJ1.DAT fixture not creatable\n");
    return;
  }

  struct process p;
  vm_file_proc_init(&p, vm_as_create(VM_FILE_TAG1));
  EXPECT_EQ((p.as != 0), 1);
  if (!p.as)
    return;
  int fd = file_open(&p, VM_FILE_NAME, 0);
  EXPECT_EQ((fd >= 0), 1);
  if (fd < 0)
    goto out;

#define DSO_N 6
  int64_t b[DSO_N];
  int ok = 1;
  for (int i = 0; i < DSO_N; i++) {
    b[i] = vm_mmap(&p, 0, fsize, VM_PROT_READ | VM_PROT_WRITE,
                   VM_MAP_PRIVATE, fd, 0);
    if (b[i] <= 0)
      ok = 0;
  }
  EXPECT_EQ(ok, 1);
  /* Every base is inside the mmap arena, distinct, and owned by the region
     that was just created there. */
  for (int i = 0; ok && i < DSO_N; i++) {
    EXPECT_EQ((b[i] >= (int64_t)USER_MMAP_BASE_V2 &&
               b[i] + (int64_t)fsize <= (int64_t)USER_MMAP_LIMIT_V2),
              1);
    struct vm_region *ri = vm_region_find(p.as, (uint64_t)b[i]);
    EXPECT_EQ((ri && ri->base == (uint64_t)b[i]), 1);
    for (int j = 0; j < i; j++) {
      uint64_t lo1 = (uint64_t)b[i], lo2 = (uint64_t)b[j];
      uint64_t end1 = lo1 + ((fsize + 0xFFFu) & ~0xFFFu);
      uint64_t end2 = lo2 + ((fsize + 0xFFFu) & ~0xFFFu);
      EXPECT_EQ((end1 <= lo2 || end2 <= lo1), 1); /* no overlap */
      EXPECT_EQ((lo1 != lo2), 1);                /* distinct base */
    }
  }
  /* Colliding size/hash still means ONE page-cache object.  (Region pointers
     are re-fetched: the registry array can grow under an insert.) */
  struct vm_object *ob0 = 0;
  {
    struct vm_region *r0 = vm_region_find(p.as, (uint64_t)b[0]);
    ob0 = r0 ? r0->obj : 0;
  }
  EXPECT_EQ((ob0 != 0), 1);
  for (int i = 1; ok && i < DSO_N; i++) {
    struct vm_region *ri = vm_region_find(p.as, (uint64_t)b[i]);
    EXPECT_EQ((ri && ri->obj == ob0), 1);
  }
  /* A hint that names an OCCUPIED base is not honoured: the allocator must
     skip the live mapping, not land on top of it. */
  int64_t hb = vm_mmap(&p, (uint64_t)b[0], fsize, VM_PROT_READ | VM_PROT_WRITE,
                       VM_MAP_PRIVATE, fd, 0);
  EXPECT_EQ((hb > 0 && hb != b[0]), 1);
  struct vm_region *rh = hb > 0 ? vm_region_find(p.as, (uint64_t)hb) : 0;
  EXPECT_EQ((rh && rh->base == (uint64_t)hb && rh->obj == ob0), 1);

  /* Independent private copies: a write fault in each mapping must not
     disturb the others (no shared base means no shared leaves). */
  for (int i = 0; ok && i < DSO_N; i++) {
    const char *why = 0;
    if (vm_handle_fault(&p, (uint64_t)b[i], 1, 0, &why) != 0) {
      EXPECT_EQ(1, 0); /* a write fault must COW, never kill */
      break;
    }
    uint64_t li = 0;
    EXPECT_EQ(vm_arch_walk(p.as, (uint64_t)b[i], &li), 0);
    ((volatile unsigned char *)vm_arch_leaf_phys(li))[0] =
        (unsigned char)(0xA0 + i);
  }
  for (int i = 0; ok && i < DSO_N; i++) {
    uint64_t li = 0;
    EXPECT_EQ(vm_arch_walk(p.as, (uint64_t)b[i], &li), 0);
    EXPECT_EQ((int)(((const volatile unsigned char *)
                         vm_arch_leaf_phys(li))[0]),
              (int)(0xA0 + i));
  }
  /* The file's page-cache page never saw any of those writes. */
  EXPECT_EQ((int)(((const volatile unsigned char *)
                       vm_object_peek(ob0, 0))[0]),
            (int)((0u * 7u + 3u) & 0xFFu));
#undef DSO_N

out:
  if (p.as)
    vm_as_teardown(p.as);
  if (fd >= 0)
    file_close(&p, fd);
  file_obj_invalidate_all();
  EXPECT_EQ(file_obj_count(), c0);
  EXPECT_EQ(frame_free_count(), free0);
}

/* M6/L30 (c): file identity as a loader sees it.  Two DSOs with IDENTICAL
 * contents under different names (the audio shims were byte-identical) must
 * report distinct (st_dev, st_ino) pairs, and stat(path) must agree with
 * fstat(fd) for the same file — otherwise glibc's dlopen "already loaded"
 * test hands back the same link map for two different libraries. */
static void test_fs_dso_identity_pair(void) {
  tests_run++;
  uart_puts("  Running test_fs_dso_identity_pair...\n");

  static const char *names[2] = {"/VMSOID1.DAT", "/VMSOID2.DAT"};
  struct file f;
  for (int i = 0; i < 2; i++) {
    for (unsigned k = 0; k < sizeof(f); k++)
      ((unsigned char *)&f)[k] = 0;
    if (fat16_open(names[i], &f) != 0) {
      uart_puts("ASSERTION FAILED: identity fixture not creatable\n");
      return;
    }
    fat16_truncate_to(&f, 0);
    unsigned char buf[256];
    for (int k = 0; k < 256; k++)
      buf[k] = (unsigned char)(0x40 + k); /* identical contents */
    int w = fat16_write(&f, buf, 256);
    fat16_close(&f);
    EXPECT_EQ(w, 256);
  }

  struct k_stat sp[2], sf[2];
  int e = 0;
  EXPECT_EQ(file_stat_path(0, names[0], &sp[0], &e), 0);
  EXPECT_EQ(file_stat_path(0, names[1], &sp[1], &e), 0);

  struct process p;
  vm_file_proc_init(&p, 0); /* stat/fstat need only the fd table */
  for (int i = 0; i < 2; i++) {
    int fd = file_open(&p, names[i], 0);
    EXPECT_EQ((fd >= 0), 1);
    if (fd < 0)
      return;
    EXPECT_EQ(file_stat_fd(&p, fd, &sf[i], &e), 0);
    file_close(&p, fd);
  }

  uart_puts("  [FS-ID] ");
  uart_print_hex((uint64_t)sp[0].st_dev);
  uart_puts(":");
  uart_print_hex((uint64_t)sp[0].st_ino);
  uart_puts(" ");
  uart_print_hex((uint64_t)sp[1].st_dev);
  uart_puts(":");
  uart_print_hex((uint64_t)sp[1].st_ino);
  uart_puts("\n");

  /* Both are regular files on the same volume... */
  EXPECT_EQ((sp[0].st_dev != 0 && sp[0].st_dev == sp[1].st_dev), 1);
  /* ...with different directory entries, so different inodes... */
  EXPECT_EQ((sp[0].st_ino != sp[1].st_ino), 1);
  /* ...and the pair a loader compares is never equal. */
  EXPECT_EQ((sp[0].st_dev != sp[1].st_dev || sp[0].st_ino != sp[1].st_ino), 1);
  /* stat(path) and fstat(fd) name the same object. */
  for (int i = 0; i < 2; i++) {
    EXPECT_EQ((sp[i].st_dev == sf[i].st_dev), 1);
    EXPECT_EQ((sp[i].st_ino == sf[i].st_ino), 1);
  }
  {
    struct k_stat sd = {0};
    int ep = 0;
    if (file_stat_path(0, "/dev/console", &sd, &ep) == 0) {
      /* A device on its own pseudo-volume is never "the same file" as a FAT
         volume object, and never (0,0). */
      EXPECT_EQ((sd.st_dev != 0), 1);
      EXPECT_EQ((sd.st_dev != sp[0].st_dev), 1);
    }
  }
}

void vm_test_suite(void) {
  uart_puts("vm_test_suite:\n");
  test_vm_as_lifecycle();
  test_vm_region_list();
  test_vm_map_grow_commit();
  test_vm_windows_distinct();
  test_vm_hole_find();
  test_vm_demand_fault();
  test_v2_image_first_page();
  test_v2_image_prefetch();
  test_vm_walk_dead_as_safe(); /* w5 (iii)/(iv) */
  test_vm_mmap_family();
  test_vm_shared_objects();
  test_vm_as_clone_fork();
  test_vm_teardown_leak_cycles();
  test_vm_user_read(); /* SEC-2 */
  test_vm_kwrite();    /* SEC-8 */
  /* so-vm lane A: file-backed shared mappings (docs/shared-libs.md §4) */
  test_vm_file_map_shared_frames();
  test_vm_file_map_tail_wx();
  test_vm_file_map_accounting();
  test_vm_file_map_fork();
  /* M6/L30: COW without the frame-identity gate; DSO base allocation */
  test_vm_file_cow_private_leaf();
  test_vm_dso_bases_distinct();
  test_fs_dso_identity_pair();
}

#endif /* KERNEL_MODE_UNIT_TEST */
