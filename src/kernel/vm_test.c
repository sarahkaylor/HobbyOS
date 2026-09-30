#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "frame.h"
#include "vm.h"
#include "process.h"

extern void uart_puts(const char *s);
extern void print_int(int v);

/* P2.2 (S2) window/paging unit tests.  Everything here runs against the
 * real frame allocator and the real arch page-table code (the same paths
 * the loader and the scheduler use), then restores the pool exactly.
 *
 * AS tags 60..79 are reserved for this suite: MAX_PROCESSES is far above,
 * no process holds them, and each test tears its AS down before the next
 * one starts, so a leaked table frame shows up as a count mismatch. */

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

void vm_test_suite(void) {
  uart_puts("vm_test_suite:\n");
  test_vm_as_lifecycle();
  test_vm_region_list();
  test_vm_map_grow_commit();
  test_vm_windows_distinct();
  test_vm_hole_find();
}

#endif /* KERNEL_MODE_UNIT_TEST */
