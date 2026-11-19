// tools/cp/p0host/vkdump/vkdump.cpp — a pass-through Vulkan layer that records what an
// application ENABLES: vkCreateInstance (apiVersion, app/engine, instance extensions) and
// vkCreateDevice (physical device, queues, device extensions, VkPhysicalDeviceFeatures and
// every *Features struct in the pNext chain, true members only).  Output: $P0HOST_VKDUMP
// (appended).  Built on strix against Ubuntu's libvulkan-dev + vulkan-utility-libraries-dev.
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <vulkan/utility/vk_struct_helper.hpp>
#include <vulkan/vk_enum_string_helper.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <mutex>
#include <map>
#include <unistd.h>
#include <time.h>

#define EXPORT extern "C" __attribute__((visibility("default")))

static std::mutex g_lock;
static std::map<void *, PFN_vkGetInstanceProcAddr> g_inst_gipa;   // dispatch key -> next gipa
static std::map<void *, VkInstance> g_inst_of_key;
static std::map<void *, PFN_vkGetDeviceProcAddr> g_dev_gdpa;
static std::map<VkPhysicalDevice, VkInstance> g_phys_inst;

static inline void *key(const void *h) { return *(void **)h; }

static FILE *out() {
  static FILE *f;
  if (!f) {
    const char *p = getenv("P0HOST_VKDUMP");
    f = p ? fopen(p, "a") : stderr;
    if (!f) f = stderr;
  }
  return f;
}

static void hdr(const char *what) {
  struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
  char exe[256] = {0};
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1); (void)n;
  fprintf(out(), "=== %s t=%ld.%09ld pid=%d exe=%s\n", what, (long)ts.tv_sec, ts.tv_nsec, getpid(), exe);
}

struct FieldDesc { const char *name; size_t off; };
static void dump_struct(VkStructureType st, const void *p) {
#define BEGIN(S) if (st == vku::GetSType<S>()) { const char *sn = #S; int any = 0; fprintf(out(), "  pNext %s:", sn);
#define F(S, m) if (((const S *)p)->m) { fprintf(out(), " %s", #m); any = 1; }
#define END(S) if (!any) fprintf(out(), " (none true)"); fprintf(out(), "\n"); return; }
#include "features.inc"
#undef BEGIN
#undef F
#undef END
  fprintf(out(), "  pNext %s (not a Features struct)\n", string_VkStructureType(st));
}

static void dump_core_features(const VkPhysicalDeviceFeatures *f, const char *label) {
  fprintf(out(), "  %s:", label);
  const VkBool32 *b = (const VkBool32 *)f;
  static const char *names[] = {
#define N(x) #x,
    N(robustBufferAccess) N(fullDrawIndexUint32) N(imageCubeArray) N(independentBlend) N(geometryShader)
    N(tessellationShader) N(sampleRateShading) N(dualSrcBlend) N(logicOp) N(multiDrawIndirect)
    N(drawIndirectFirstInstance) N(depthClamp) N(depthBiasClamp) N(fillModeNonSolid) N(depthBounds)
    N(wideLines) N(largePoints) N(alphaToOne) N(multiViewport) N(samplerAnisotropy)
    N(textureCompressionETC2) N(textureCompressionASTC_LDR) N(textureCompressionBC) N(occlusionQueryPrecise)
    N(pipelineStatisticsQuery) N(vertexPipelineStoresAndAtomics) N(fragmentStoresAndAtomics)
    N(shaderTessellationAndGeometryPointSize) N(shaderImageGatherExtended) N(shaderStorageImageExtendedFormats)
    N(shaderStorageImageMultisample) N(shaderStorageImageReadWithoutFormat) N(shaderStorageImageWriteWithoutFormat)
    N(shaderUniformBufferArrayDynamicIndexing) N(shaderSampledImageArrayDynamicIndexing)
    N(shaderStorageBufferArrayDynamicIndexing) N(shaderStorageImageArrayDynamicIndexing) N(shaderClipDistance)
    N(shaderCullDistance) N(shaderFloat64) N(shaderInt64) N(shaderInt16) N(shaderResourceResidency)
    N(shaderResourceMinLod) N(sparseBinding) N(sparseResidencyBuffer) N(sparseResidencyImage2D)
    N(sparseResidencyImage3D) N(sparseResidency2Samples) N(sparseResidency4Samples) N(sparseResidency8Samples)
    N(sparseResidency16Samples) N(sparseResidencyAliased) N(variableMultisampleRate) N(inheritedQueries)
#undef N
  };
  static_assert(sizeof(names) / sizeof(names[0]) == sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32), "count");
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    if (b[i]) fprintf(out(), " %s", names[i]);
  fprintf(out(), "\n");
}

static VkLayerInstanceCreateInfo *inst_link(const VkInstanceCreateInfo *ci) {
  auto *c = (VkLayerInstanceCreateInfo *)ci->pNext;
  while (c && !(c->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && c->function == VK_LAYER_LINK_INFO))
    c = (VkLayerInstanceCreateInfo *)c->pNext;
  return c;
}
static VkLayerDeviceCreateInfo *dev_link(const VkDeviceCreateInfo *ci) {
  auto *c = (VkLayerDeviceCreateInfo *)ci->pNext;
  while (c && !(c->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && c->function == VK_LAYER_LINK_INFO))
    c = (VkLayerDeviceCreateInfo *)c->pNext;
  return c;
}

static VKAPI_ATTR VkResult VKAPI_CALL d_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *inst) {
  auto *link = inst_link(ci);
  if (!link) return VK_ERROR_INITIALIZATION_FAILED;
  PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  link->u.pLayerInfo = link->u.pLayerInfo->pNext;
  auto create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
  VkResult r = create(ci, a, inst);
  std::lock_guard<std::mutex> g(g_lock);
  hdr("vkCreateInstance");
  const VkApplicationInfo *ai = ci->pApplicationInfo;
  if (ai)
    fprintf(out(), "  app=%s appVersion=0x%x engine=%s engineVersion=0x%x apiVersion=%u.%u.%u\n",
            ai->pApplicationName ? ai->pApplicationName : "(null)", ai->applicationVersion,
            ai->pEngineName ? ai->pEngineName : "(null)", ai->engineVersion,
            VK_API_VERSION_MAJOR(ai->apiVersion), VK_API_VERSION_MINOR(ai->apiVersion), VK_API_VERSION_PATCH(ai->apiVersion));
  for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) fprintf(out(), "  iext %s\n", ci->ppEnabledExtensionNames[i]);
  for (uint32_t i = 0; i < ci->enabledLayerCount; i++) fprintf(out(), "  ilayer %s\n", ci->ppEnabledLayerNames[i]);
  fprintf(out(), "  result=%s\n", string_VkResult(r));
  fflush(out());
  if (r == VK_SUCCESS) { g_inst_gipa[key(*inst)] = gipa; g_inst_of_key[key(*inst)] = *inst; }
  return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL d_EnumeratePhysicalDevices(VkInstance inst, uint32_t *n, VkPhysicalDevice *pd) {
  PFN_vkGetInstanceProcAddr gipa;
  { std::lock_guard<std::mutex> g(g_lock); gipa = g_inst_gipa[key(inst)]; }
  auto f = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
  VkResult r = f(inst, n, pd);
  if (pd && (r == VK_SUCCESS || r == VK_INCOMPLETE)) {
    std::lock_guard<std::mutex> g(g_lock);
    for (uint32_t i = 0; i < *n; i++) g_phys_inst[pd[i]] = inst;
  }
  return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL d_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *dev) {
  auto *link = dev_link(ci);
  if (!link) return VK_ERROR_INITIALIZATION_FAILED;
  PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
  link->u.pLayerInfo = link->u.pLayerInfo->pNext;
  VkInstance inst;
  { std::lock_guard<std::mutex> g(g_lock); inst = g_phys_inst.count(pd) ? g_phys_inst[pd] : g_inst_of_key.begin()->second; }
  auto create = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
  auto props = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
  VkResult r = create(pd, ci, a, dev);
  std::lock_guard<std::mutex> g(g_lock);
  hdr("vkCreateDevice");
  VkPhysicalDeviceProperties pp; props(pd, &pp);
  fprintf(out(), "  physicalDevice=%s vendor=0x%04x device=0x%04x driverVersion=0x%x deviceApi=%u.%u.%u\n", pp.deviceName,
          pp.vendorID, pp.deviceID, pp.driverVersion, VK_API_VERSION_MAJOR(pp.apiVersion),
          VK_API_VERSION_MINOR(pp.apiVersion), VK_API_VERSION_PATCH(pp.apiVersion));
  for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++)
    fprintf(out(), "  queue family=%u count=%u flags=0x%x\n", ci->pQueueCreateInfos[i].queueFamilyIndex,
            ci->pQueueCreateInfos[i].queueCount, ci->pQueueCreateInfos[i].flags);
  for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) fprintf(out(), "  dext %s\n", ci->ppEnabledExtensionNames[i]);
  if (ci->pEnabledFeatures) dump_core_features(ci->pEnabledFeatures, "pEnabledFeatures");
  for (auto *s = (const VkBaseInStructure *)ci->pNext; s; s = s->pNext) {
    if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
      dump_core_features(&((const VkPhysicalDeviceFeatures2 *)s)->features, "VkPhysicalDeviceFeatures2.features");
    else if (s->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO)
      continue;
    else
      dump_struct(s->sType, s);
  }
  fprintf(out(), "  result=%s\n", string_VkResult(r));
  fflush(out());
  if (r == VK_SUCCESS) g_dev_gdpa[key(*dev)] = gdpa;
  return r;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkdump_GetDeviceProcAddr(VkDevice dev, const char *name) {
  PFN_vkGetDeviceProcAddr gdpa;
  { std::lock_guard<std::mutex> g(g_lock); gdpa = g_dev_gdpa[key(dev)]; }
  return gdpa ? gdpa(dev, name) : nullptr;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkdump_GetInstanceProcAddr(VkInstance inst, const char *name) {
  if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)d_CreateInstance;
  if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)d_CreateDevice;
  if (!strcmp(name, "vkEnumeratePhysicalDevices")) return (PFN_vkVoidFunction)d_EnumeratePhysicalDevices;
  if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vkdump_GetInstanceProcAddr;
  if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkdump_GetDeviceProcAddr;
  if (!inst) return nullptr;
  PFN_vkGetInstanceProcAddr gipa;
  { std::lock_guard<std::mutex> g(g_lock); gipa = g_inst_gipa[key(inst)]; }
  return gipa ? gipa(inst, name) : nullptr;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v) {
  if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
  v->pfnGetInstanceProcAddr = vkdump_GetInstanceProcAddr;
  v->pfnGetDeviceProcAddr = vkdump_GetDeviceProcAddr;
  v->pfnGetPhysicalDeviceProcAddr = nullptr;
  return VK_SUCCESS;
}
