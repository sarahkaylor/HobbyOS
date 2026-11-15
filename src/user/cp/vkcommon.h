/* src/user/cp/vkcommon.h — shared helpers for the C5-vk Vulkan probes
 * (vkenum.c, vkcube.c; docs/cp-vk.md).
 *
 * These are ordinary glibc 2.31 compat programs (AD-2), built by
 * tools/cp/vk/build.sh against the pinned sysroot, never against HobbyOS
 * headers.  They load the Vulkan loader exactly the way Wine's winevulkan
 * does: dlopen("libvulkan.so.1") and vkGetInstanceProcAddr, with no link-time
 * dependency on libvulkan.  Every function pointer comes from the loader.
 *
 * Output protocol (same shape as the T3 probes): rows prefixed "[VK] ",
 * finished by "[VK] done rc=<n>".
 */
#ifndef CP_VKCOMMON_H
#define CP_VKCOMMON_H

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void vk_row(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void vk_row(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("[VK] ", stdout);
  vprintf(fmt, ap);
  fputc('\n', stdout);
  fflush(stdout);
  va_end(ap);
}

static double vk_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static PFN_vkGetInstanceProcAddr vk_gipa;
static void *vk_lib;

/* dlopen the loader by soname (RUNPATH /CP/VK/lib finds it in the guest). */
static int vk_load_loader(void) {
  double t0 = vk_now_ms();
  vk_lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!vk_lib) {
    vk_row("FAIL dlopen libvulkan.so.1: %s", dlerror());
    return -1;
  }
  vk_gipa = (PFN_vkGetInstanceProcAddr)dlsym(vk_lib, "vkGetInstanceProcAddr");
  if (!vk_gipa) {
    vk_row("FAIL dlsym vkGetInstanceProcAddr: %s", dlerror());
    return -1;
  }
  vk_row("loader libvulkan.so.1 loaded in %.1f ms", vk_now_ms() - t0);
  return 0;
}

#define VK_IFN(inst, name) PFN_##name name = (PFN_##name)vk_gipa((inst), #name)

static __attribute__((unused)) const char *vk_type_name(VkPhysicalDeviceType t) {
  switch (t) {
  case VK_PHYSICAL_DEVICE_TYPE_OTHER: return "PHYSICAL_DEVICE_TYPE_OTHER";
  case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU";
  case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "PHYSICAL_DEVICE_TYPE_DISCRETE_GPU";
  case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU";
  case VK_PHYSICAL_DEVICE_TYPE_CPU: return "PHYSICAL_DEVICE_TYPE_CPU";
  default: return "?";
  }
}

#define VK_CHECK(expr)                                                         \
  do {                                                                         \
    VkResult vk_r_ = (expr);                                                   \
    if (vk_r_ != VK_SUCCESS) {                                                 \
      vk_row("FAIL %s -> VkResult %d (line %d)", #expr, (int)vk_r_, __LINE__); \
      vk_row("done rc=1");                                                     \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

/* A wave boot has no environment.  The loader finds the ICD through its
 * compiled-in fallback (/CP/VK/share/vulkan/icd.d, tools/cp/vk/build.sh), so
 * nothing needs setting; report what discovery will see. */
static void vk_report_env(void) {
  const char *vars[] = {"VK_DRIVER_FILES", "VK_ICD_FILENAMES", "XDG_DATA_DIRS",
                        "LP_NUM_THREADS", NULL};
  for (int i = 0; vars[i]; i++) {
    const char *v = getenv(vars[i]);
    vk_row("env %s=%s", vars[i], v ? v : "(unset)");
  }
}

#endif
