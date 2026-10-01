#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "errno.h"
#include "frame.h"
#include "vm.h"
#include "process.h"

extern void uart_puts(const char *s);
extern void print_int(int v);

/* P2.2 (S2) window/paging unit tests.  Everything here runs against the
 * real frame allocator and the real arch page-table code (the same paths
 * the loader and the scheduler use), then restores the pool exactly.
 *
 * AS tags 60..63 are reserved for this suite (MAX_PROCESSES is 64, so
 * 60..63 is the free top range; no process holds them, and each test tears
 * its AS down before the next one starts, so a leaked table frame shows up
 * as a count mismatch). */

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

  /* The fault classifier: hole -> HOLE (kill); present -> PROT (kill);
     absent in-region -> demand. */
  const char *why = 0;
  EXPECT_EQ(vm_handle_fault(&fake, USER_MMAP_BASE_V2 + 0x2000, 0, 0, &why), 1);
  EXPECT_EQ((why && why[0] == 'H'), 1); /* "HOLE" */
  EXPECT_EQ(vm_handle_fault(&fake, USER_MMAP_BASE_V2, 0, 0, &why), 1);
  EXPECT_EQ((why && why[0] == 'P'), 1); /* "PROT" (page is live) */

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

  /* fd-backed and MAP_SHARED are S4: rejected, and a bad len too. */
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

void vm_test_suite(void) {
  uart_puts("vm_test_suite:\n");
  test_vm_as_lifecycle();
  test_vm_region_list();
  test_vm_map_grow_commit();
  test_vm_windows_distinct();
  test_vm_hole_find();
  test_vm_demand_fault();
  test_vm_mmap_family();
  test_vm_shared_objects();
  test_vm_as_clone_fork();
  test_vm_teardown_leak_cycles();
}

#endif /* KERNEL_MODE_UNIT_TEST */
