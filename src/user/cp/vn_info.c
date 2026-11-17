/* src/user/cp/vn_info.c -- Gate P6 Vulkan probe on Venus (lane C6-venus,
 * docs/cp-venus.md).  A glibc 2.31 compat program in the C5-vk probe style
 * (vkcommon.h: dlopen("libvulkan.so.1"), "[VK] " rows, "[VK] done rc=N").
 *
 *   vn_info [info] [work] [fence N] [bw]     (default: info work fence 10000)
 *
 * info   a `vulkaninfo --summary` block per physical device (the same fields),
 *        plus memory heaps/types and queue families;
 * work   on the first Venus device: vkCmdFillBuffer into device-local memory,
 *        vkCmdCopyBuffer into a host-visible buffer mapped through the
 *        host-visible window (GPU_IOC_RES_MAP_BLOB), fence, and check every
 *        word on the CPU -- the host GPU really executed the commands;
 * fence  N x (vkQueueSubmit of an empty batch with a fence -> vkWaitForFences
 *        -> vkResetFences), timed with the TSC (the guest's CLOCK_MONOTONIC is
 *        millisecond-granular): submit and submit->signal p50/p90/p99/max us,
 *        then the same with one empty command buffer per submit;
 * bw     memcpy bandwidth into / out of a 64 MiB host-visible mapping.
 *
 * VK_DRIVER_FILES defaults to the Venus ICD (/CP/VK/share/vulkan/icd.d/
 * virtio_icd.x86_64.json) unless the environment names one; `info` with
 * VN_ALL_ICDS=1 leaves discovery alone (lavapipe and Venus side by side).
 */
#include "vkcommon.h"

#include <stdint.h>

static double tsc_per_ns;
static inline uint64_t rdtsc(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}
static void tsc_calibrate(void) {
  double m0 = vk_now_ms();
  uint64_t t0 = rdtsc();
  while (vk_now_ms() - m0 < 500.0) {
  }
  double m1 = vk_now_ms();
  uint64_t t1 = rdtsc();
  tsc_per_ns = (double)(t1 - t0) / ((m1 - m0) * 1e6);
}
static double tsc_us(uint64_t d) { return (double)d / tsc_per_ns / 1000.0; }

static int cmp_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return x < y ? -1 : x > y;
}

static const char *driver_id_name(VkDriverId id) {
  switch (id) {
  case VK_DRIVER_ID_MESA_VENUS: return "DRIVER_ID_MESA_VENUS";
  case VK_DRIVER_ID_MESA_LLVMPIPE: return "DRIVER_ID_MESA_LLVMPIPE";
  case VK_DRIVER_ID_MESA_RADV: return "DRIVER_ID_MESA_RADV";
  default: return "DRIVER_ID_OTHER";
  }
}

static int fails;

static VkInstance inst;
static PFN_vkEnumeratePhysicalDevices pEnumeratePhysicalDevices;
static PFN_vkGetPhysicalDeviceProperties2 pGetPhysicalDeviceProperties2;
static PFN_vkGetPhysicalDeviceMemoryProperties pGetPhysicalDeviceMemoryProperties;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties pGetPhysicalDeviceQueueFamilyProperties;
static PFN_vkEnumerateDeviceExtensionProperties pEnumerateDeviceExtensionProperties;

static void print_summary(uint32_t idx, VkPhysicalDevice pd) {
  VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
  VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
  pGetPhysicalDeviceProperties2(pd, &p2);
  VkPhysicalDeviceProperties *p = &p2.properties;
  vk_row("GPU%u:", idx);
  vk_row("\tapiVersion         = %u.%u.%u", VK_API_VERSION_MAJOR(p->apiVersion),
         VK_API_VERSION_MINOR(p->apiVersion), VK_API_VERSION_PATCH(p->apiVersion));
  vk_row("\tdriverVersion      = %u.%u.%u", VK_VERSION_MAJOR(p->driverVersion),
         VK_VERSION_MINOR(p->driverVersion), VK_VERSION_PATCH(p->driverVersion));
  vk_row("\tvendorID           = 0x%04x", p->vendorID);
  vk_row("\tdeviceID           = 0x%04x", p->deviceID);
  vk_row("\tdeviceType         = %s", vk_type_name(p->deviceType));
  vk_row("\tdeviceName         = %s", p->deviceName);
  vk_row("\tdriverID           = %s", driver_id_name(drv.driverID));
  vk_row("\tdriverName         = %s", drv.driverName);
  vk_row("\tdriverInfo         = %s", drv.driverInfo);
  vk_row("\tconformanceVersion = %u.%u.%u.%u", drv.conformanceVersion.major,
         drv.conformanceVersion.minor, drv.conformanceVersion.subminor, drv.conformanceVersion.patch);
  uint32_t ne = 0;
  pEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
  vk_row("\tdeviceExtensions   = %u", ne);
  VkPhysicalDeviceMemoryProperties mp;
  pGetPhysicalDeviceMemoryProperties(pd, &mp);
  for (uint32_t h = 0; h < mp.memoryHeapCount; h++)
    vk_row("\tmemoryHeap[%u]      = %.1f MiB%s", h, mp.memoryHeaps[h].size / 1048576.0,
           (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " DEVICE_LOCAL" : "");
  for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
    VkMemoryPropertyFlags f = mp.memoryTypes[t].propertyFlags;
    vk_row("\tmemoryType[%u]      = heap %u%s%s%s%s", t, mp.memoryTypes[t].heapIndex,
           (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? " DEVICE_LOCAL" : "",
           (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? " HOST_VISIBLE" : "",
           (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? " HOST_COHERENT" : "",
           (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? " HOST_CACHED" : "");
  }
  uint32_t nq = 0;
  pGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
  VkQueueFamilyProperties qf[16];
  if (nq > 16) nq = 16;
  pGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
  for (uint32_t q = 0; q < nq; q++)
    vk_row("\tqueueFamily[%u]     = count %u flags%s%s%s", q, qf[q].queueCount,
           (qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) ? " GRAPHICS" : "",
           (qf[q].queueFlags & VK_QUEUE_COMPUTE_BIT) ? " COMPUTE" : "",
           (qf[q].queueFlags & VK_QUEUE_TRANSFER_BIT) ? " TRANSFER" : "");
}

/* ---- device + helpers ---------------------------------------------------- */

static VkPhysicalDevice gpd;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkPhysicalDeviceMemoryProperties gmp;

#define DFN(name) static PFN_##name p##name
DFN(vkCreateDevice);
DFN(vkGetDeviceQueue);
DFN(vkCreateBuffer);
DFN(vkGetBufferMemoryRequirements);
DFN(vkAllocateMemory);
DFN(vkBindBufferMemory);
DFN(vkMapMemory);
DFN(vkUnmapMemory);
DFN(vkCreateCommandPool);
DFN(vkAllocateCommandBuffers);
DFN(vkBeginCommandBuffer);
DFN(vkEndCommandBuffer);
DFN(vkCmdFillBuffer);
DFN(vkCmdCopyBuffer);
DFN(vkCmdPipelineBarrier);
DFN(vkCreateFence);
DFN(vkQueueSubmit);
DFN(vkWaitForFences);
DFN(vkResetFences);
DFN(vkDeviceWaitIdle);
DFN(vkInvalidateMappedMemoryRanges);
DFN(vkCreateSemaphore);
DFN(vkWaitSemaphores);
DFN(vkGetSemaphoreCounterValue);
DFN(vkSignalSemaphore);
#define DLOAD(name) p##name = (PFN_##name)vk_gipa(inst, #name)

static int find_mem(uint32_t bits, VkMemoryPropertyFlags want) {
  for (uint32_t i = 0; i < gmp.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (gmp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
  return -1;
}

static int make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want,
                       VkBuffer *buf, VkDeviceMemory *mem, int *type_out) {
  VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                            .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
  if (pvkCreateBuffer(dev, &bci, NULL, buf) != VK_SUCCESS) return -1;
  VkMemoryRequirements mr;
  pvkGetBufferMemoryRequirements(dev, *buf, &mr);
  int t = find_mem(mr.memoryTypeBits, want);
  if (t < 0) return -1;
  VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)t};
  if (pvkAllocateMemory(dev, &mai, NULL, mem) != VK_SUCCESS) return -1;
  if (pvkBindBufferMemory(dev, *buf, *mem, 0) != VK_SUCCESS) return -1;
  if (type_out) *type_out = t;
  return 0;
}

static int open_device(void) {
  uint32_t n = 0;
  pEnumeratePhysicalDevices(inst, &n, NULL);
  VkPhysicalDevice pds[8];
  if (n > 8) n = 8;
  pEnumeratePhysicalDevices(inst, &n, pds);
  for (uint32_t i = 0; i < n; i++) {
    VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
    pGetPhysicalDeviceProperties2(pds[i], &p2);
    if (drv.driverID == VK_DRIVER_ID_MESA_VENUS) {
      gpd = pds[i];
      vk_row("using GPU%u %s", i, p2.properties.deviceName);
      break;
    }
  }
  if (!gpd) {
    vk_row("FAIL no Venus physical device");
    return -1;
  }
  pGetPhysicalDeviceMemoryProperties(gpd, &gmp);
  uint32_t nq = 0;
  pGetPhysicalDeviceQueueFamilyProperties(gpd, &nq, NULL);
  VkQueueFamilyProperties qf[16];
  if (nq > 16) nq = 16;
  pGetPhysicalDeviceQueueFamilyProperties(gpd, &nq, qf);
  qfam = 0;
  for (uint32_t q = 0; q < nq; q++)
    if (qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfam = q; break; }
  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                 .queueFamilyIndex = qfam, .queueCount = 1, .pQueuePriorities = &prio};
  VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                         .timelineSemaphore = VK_TRUE};
  VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f12,
                            .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
  DLOAD(vkCreateDevice);
  double t0 = vk_now_ms();
  VK_CHECK(pvkCreateDevice(gpd, &dci, NULL, &dev));
  vk_row("vkCreateDevice ok in %.1f ms (queue family %u)", vk_now_ms() - t0, qfam);
  DLOAD(vkGetDeviceQueue); DLOAD(vkCreateBuffer); DLOAD(vkGetBufferMemoryRequirements);
  DLOAD(vkAllocateMemory); DLOAD(vkBindBufferMemory); DLOAD(vkMapMemory); DLOAD(vkUnmapMemory);
  DLOAD(vkCreateCommandPool); DLOAD(vkAllocateCommandBuffers); DLOAD(vkBeginCommandBuffer);
  DLOAD(vkEndCommandBuffer); DLOAD(vkCmdFillBuffer); DLOAD(vkCmdCopyBuffer);
  DLOAD(vkCmdPipelineBarrier); DLOAD(vkCreateFence); DLOAD(vkQueueSubmit);
  DLOAD(vkWaitForFences); DLOAD(vkResetFences); DLOAD(vkDeviceWaitIdle);
  DLOAD(vkInvalidateMappedMemoryRanges);
  DLOAD(vkCreateSemaphore); DLOAD(vkWaitSemaphores); DLOAD(vkGetSemaphoreCounterValue);
  DLOAD(vkSignalSemaphore);
  pvkGetDeviceQueue(dev, qfam, 0, &queue);
  return 0;
}

/* GPU fill + copy into a host-visible buffer, checked on the CPU. */
static void do_work(void) {
  const VkDeviceSize sz = 4u << 20;
  VkBuffer dbuf, hbuf;
  VkDeviceMemory dmem, hmem;
  int ht = -1;
  if (make_buffer(sz, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &dbuf, &dmem, NULL) ||
      make_buffer(sz, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  &hbuf, &hmem, &ht)) {
    vk_row("FAIL work: buffer setup");
    fails++;
    return;
  }
  void *ptr = NULL;
  VK_CHECK(pvkMapMemory(dev, hmem, 0, VK_WHOLE_SIZE, 0, &ptr));
  memset(ptr, 0x11, sz);
  VkCommandPool pool;
  VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                 .queueFamilyIndex = qfam};
  VK_CHECK(pvkCreateCommandPool(dev, &pci, NULL, &pool));
  VkCommandBuffer cb;
  VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                     .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                     .commandBufferCount = 1};
  VK_CHECK(pvkAllocateCommandBuffers(dev, &cai, &cb));
  VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  VK_CHECK(pvkBeginCommandBuffer(cb, &bi));
  pvkCmdFillBuffer(cb, dbuf, 0, sz, 0xC0FFEE11u);
  VkBufferMemoryBarrier b1 = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                              .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .buffer = dbuf, .offset = 0, .size = VK_WHOLE_SIZE};
  pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                        NULL, 1, &b1, 0, NULL);
  VkBufferCopy rg = {.srcOffset = 0, .dstOffset = 0, .size = sz};
  pvkCmdCopyBuffer(cb, dbuf, hbuf, 1, &rg);
  VkBufferMemoryBarrier b2 = b1;
  b2.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  b2.buffer = hbuf;
  pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL,
                        1, &b2, 0, NULL);
  VK_CHECK(pvkEndCommandBuffer(cb));
  VkFence f;
  VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VK_CHECK(pvkCreateFence(dev, &fci, NULL, &f));
  VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                     .pCommandBuffers = &cb};
  double t0 = vk_now_ms();
  VK_CHECK(pvkQueueSubmit(queue, 1, &si, f));
  VK_CHECK(pvkWaitForFences(dev, 1, &f, VK_TRUE, 5000000000ull));
  double t1 = vk_now_ms();
  const uint32_t *w = (const uint32_t *)ptr;
  uint32_t bad = 0, first_bad = 0;
  for (uint32_t i = 0; i < sz / 4; i++)
    if (w[i] != 0xC0FFEE11u) {
      if (!bad) first_bad = i;
      bad++;
    }
  vk_row("%s work: GPU fill 0xC0FFEE11 + copy of %u KiB into host-visible type %d (%s), "
         "fence after %.1f ms, mismatches=%u%s",
         bad ? "FAIL" : "ok  ", (unsigned)(sz >> 10), ht,
         (gmp.memoryTypes[ht].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? "cached" : "uncached/WC",
         t1 - t0, bad, bad ? "" : " (the host GPU executed it)");
  if (bad) {
    vk_row("  first bad word %u = 0x%08x", first_bad, w[first_bad]);
    fails++;
  }
}

static void fence_loop(int n, int with_cb) {
  VkFence f;
  VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VK_CHECK(pvkCreateFence(dev, &fci, NULL, &f));
  VkCommandBuffer cb = VK_NULL_HANDLE;
  if (with_cb) {
    VkCommandPool pool;
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .queueFamilyIndex = qfam};
    VK_CHECK(pvkCreateCommandPool(dev, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1};
    VK_CHECK(pvkAllocateCommandBuffers(dev, &cai, &cb));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_CHECK(pvkBeginCommandBuffer(cb, &bi));
    VK_CHECK(pvkEndCommandBuffer(cb));
  }
  VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = with_cb ? 1 : 0,
                     .pCommandBuffers = with_cb ? &cb : NULL};
  uint64_t *sub = calloc((size_t)n, sizeof(uint64_t));
  uint64_t *rt = calloc((size_t)n, sizeof(uint64_t));
  int ok = 0;
  double w0 = vk_now_ms();
  for (int i = 0; i < n; i++) {
    uint64_t a = rdtsc();
    if (pvkQueueSubmit(queue, 1, &si, f) != VK_SUCCESS) break;
    uint64_t b = rdtsc();
    if (pvkWaitForFences(dev, 1, &f, VK_TRUE, 2000000000ull) != VK_SUCCESS) break;
    uint64_t c = rdtsc();
    pvkResetFences(dev, 1, &f);
    sub[i] = b - a;
    rt[i] = c - a;
    ok++;
  }
  double w1 = vk_now_ms();
  if (ok != n) {
    vk_row("FAIL fence loop stopped at %d/%d", ok, n);
    fails++;
  }
  if (ok) {
    qsort(sub, (size_t)ok, sizeof(uint64_t), cmp_u64);
    qsort(rt, (size_t)ok, sizeof(uint64_t), cmp_u64);
    vk_row("FENCE %s n=%d wall=%.0f ms | submit_us p50=%.1f p99=%.1f | submit->signal_us p50=%.1f "
           "p90=%.1f p99=%.1f max=%.1f",
           with_cb ? "empty-cmdbuf" : "empty-batch", ok, w1 - w0, tsc_us(sub[ok / 2]),
           tsc_us(sub[(ok * 99) / 100]), tsc_us(rt[ok / 2]), tsc_us(rt[(ok * 9) / 10]),
           tsc_us(rt[(ok * 99) / 100]), tsc_us(rt[ok - 1]));
  }
  free(sub);
  free(rt);
}

/* vkd3d-proton's D3D12 fences are timeline semaphores signalled by
 * vkQueueSubmit and waited on the host (vkWaitSemaphores). */
static void do_timeline(int n) {
  VkSemaphoreTypeCreateInfo tci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                   .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
  VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &tci};
  VkSemaphore sem;
  VK_CHECK(pvkCreateSemaphore(dev, &sci, NULL, &sem));
  uint64_t *rt = calloc((size_t)n, sizeof(uint64_t));
  int ok = 0;
  for (int i = 0; i < n; i++) {
    uint64_t v = (uint64_t)i + 1;
    VkTimelineSemaphoreSubmitInfo ts = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                        .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &v};
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ts,
                       .signalSemaphoreCount = 1, .pSignalSemaphores = &sem};
    uint64_t a = rdtsc();
    VkResult r = pvkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) { vk_row("FAIL timeline submit %d -> %d", i, (int)r); break; }
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                              .pSemaphores = &sem, .pValues = &v};
    r = pvkWaitSemaphores(dev, &wi, 2000000000ull);
    if (r != VK_SUCCESS) {
      uint64_t cur = 0;
      pvkGetSemaphoreCounterValue(dev, sem, &cur);
      vk_row("FAIL timeline wait %d for %llu -> %d (counter %llu)", i, (unsigned long long)v, (int)r,
             (unsigned long long)cur);
      break;
    }
    rt[i] = rdtsc() - a;
    ok++;
  }
  if (ok != n) fails++;
  if (ok) {
    qsort(rt, (size_t)ok, sizeof(uint64_t), cmp_u64);
    vk_row("TIMELINE n=%d submit->vkWaitSemaphores_us p50=%.1f p90=%.1f p99=%.1f max=%.1f", ok,
           tsc_us(rt[ok / 2]), tsc_us(rt[(ok * 9) / 10]), tsc_us(rt[(ok * 99) / 100]), tsc_us(rt[ok - 1]));
  }
  free(rt);
}

static void do_bw(void) {
  const VkDeviceSize sz = 64u << 20;
  VkBuffer hbuf;
  VkDeviceMemory hmem;
  int ht = -1;
  if (make_buffer(sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &hbuf,
                  &hmem, &ht)) {
    vk_row("FAIL bw: buffer");
    fails++;
    return;
  }
  void *ptr = NULL;
  VK_CHECK(pvkMapMemory(dev, hmem, 0, VK_WHOLE_SIZE, 0, &ptr));
  char *src = malloc(sz);
  memset(src, 0x5A, sz);
  uint64_t a = rdtsc();
  for (int k = 0; k < 4; k++) memcpy(ptr, src, sz);
  uint64_t b = rdtsc();
  for (int k = 0; k < 2; k++) memcpy(src, ptr, sz);
  uint64_t c = rdtsc();
  vk_row("bw host-visible type %d: write %.0f MB/s, read %.0f MB/s (64 MiB, memcpy)", ht,
         4.0 * 64.0 / (tsc_us(b - a) / 1e6), 2.0 * 64.0 / (tsc_us(c - b) / 1e6));
  free(src);
}

int main(int argc, char **argv) {
  int want_info = 0, want_work = 0, want_bw = 0, nfence = 0, ntl = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "info")) want_info = 1;
    else if (!strcmp(argv[i], "work")) want_work = 1;
    else if (!strcmp(argv[i], "bw")) want_bw = 1;
    else if (!strcmp(argv[i], "fence") && i + 1 < argc) nfence = atoi(argv[++i]);
    else if (!strcmp(argv[i], "timeline") && i + 1 < argc) ntl = atoi(argv[++i]);
  }
  if (argc == 1) {
    want_info = want_work = 1;
    nfence = 10000;
  }
  if (!getenv("VK_DRIVER_FILES") && !getenv("VN_ALL_ICDS"))
    setenv("VK_DRIVER_FILES", "/CP/VK/share/vulkan/icd.d/virtio_icd.x86_64.json", 1);
  vk_row("vn_info start (C6-venus)");
  vk_report_env();
  tsc_calibrate();
  vk_row("tsc %.3f GHz", tsc_per_ns);
  if (vk_load_loader() != 0) { vk_row("done rc=1"); return 1; }
  VK_IFN(NULL, vkEnumerateInstanceVersion);
  VK_IFN(NULL, vkCreateInstance);
  uint32_t iv = 0;
  VK_CHECK(vkEnumerateInstanceVersion(&iv));
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vn_info",
                           .apiVersion = VK_API_VERSION_1_3};
  VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
  double t0 = vk_now_ms();
  VK_CHECK(vkCreateInstance(&ici, NULL, &inst));
  vk_row("vkCreateInstance ok in %.1f ms", vk_now_ms() - t0);
  pEnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)vk_gipa(inst, "vkEnumeratePhysicalDevices");
  pGetPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2)vk_gipa(inst, "vkGetPhysicalDeviceProperties2");
  pGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)vk_gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
  pGetPhysicalDeviceQueueFamilyProperties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)vk_gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
  pEnumerateDeviceExtensionProperties = (PFN_vkEnumerateDeviceExtensionProperties)vk_gipa(inst, "vkEnumerateDeviceExtensionProperties");

  uint32_t n = 0;
  VK_CHECK(pEnumeratePhysicalDevices(inst, &n, NULL));
  VkPhysicalDevice pds[8];
  if (n > 8) n = 8;
  VK_CHECK(pEnumeratePhysicalDevices(inst, &n, pds));
  if (want_info) {
    vk_row("==========");
    vk_row("VULKANINFO");
    vk_row("==========");
    vk_row("Vulkan Instance Version: %u.%u.%u", VK_API_VERSION_MAJOR(iv), VK_API_VERSION_MINOR(iv),
           VK_API_VERSION_PATCH(iv));
    vk_row("Devices:");
    vk_row("========");
    for (uint32_t i = 0; i < n; i++) print_summary(i, pds[i]);
  }
  if (want_work || nfence || want_bw || ntl) {
    if (open_device() != 0) {
      fails++;
    } else {
      if (want_work) do_work();
      if (nfence) {
        fence_loop(nfence, 0);
        fence_loop(nfence, 1);
      }
      if (ntl) do_timeline(ntl);
      if (want_bw) do_bw();
      pvkDeviceWaitIdle(dev);
    }
  }
  vk_row("done rc=%d", fails);
  return fails;
}
