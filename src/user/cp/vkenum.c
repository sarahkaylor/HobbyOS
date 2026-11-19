/* src/user/cp/vkenum.c — a `vulkaninfo --summary` equivalent for HobbyOS's
 * compat class (lane C5-vk, docs/cp-vk.md).
 *
 * dlopen("libvulkan.so.1") -> instance version + extensions -> vkCreateInstance
 * (1.3) -> every physical device: name, type, API/driver version, driver
 * id/name/info, conformance version, memory heaps, queue families, device
 * extension count, and the features/extensions vkd3d-proton and DXVK lean on
 * (so P5's next probes know up front what lavapipe gives them).
 *
 * Output: "[VK] ..." rows, "[VK] done rc=0" on success.  Build:
 * tools/cp/vk/build.sh probes.
 */
#include "vkcommon.h"

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) {
  for (uint32_t i = 0; i < n; i++)
    if (strcmp(e[i].extensionName, name) == 0) return 1;
  return 0;
}

int main(void) {
  vk_row("vkenum start (C5-vk)");
  vk_report_env();
  if (vk_load_loader() != 0) { vk_row("done rc=1"); return 1; }

  VK_IFN(NULL, vkEnumerateInstanceVersion);
  VK_IFN(NULL, vkEnumerateInstanceExtensionProperties);
  VK_IFN(NULL, vkCreateInstance);
  uint32_t iv = 0;
  VK_CHECK(vkEnumerateInstanceVersion(&iv));
  vk_row("Vulkan Instance Version: %u.%u.%u", VK_API_VERSION_MAJOR(iv),
         VK_API_VERSION_MINOR(iv), VK_API_VERSION_PATCH(iv));

  uint32_t nie = 0;
  VK_CHECK(vkEnumerateInstanceExtensionProperties(NULL, &nie, NULL));
  VkExtensionProperties *ie = calloc(nie ? nie : 1, sizeof(*ie));
  VK_CHECK(vkEnumerateInstanceExtensionProperties(NULL, &nie, ie));
  vk_row("Instance Extensions: count = %u", nie);
  for (uint32_t i = 0; i < nie; i++)
    vk_row("  %-40s : extension revision %u", ie[i].extensionName, ie[i].specVersion);

  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                           .pApplicationName = "vkenum",
                           .apiVersion = VK_API_VERSION_1_3};
  const char *want_ext[1];
  uint32_t nwant = 0;
  if (has_ext(ie, nie, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
    want_ext[nwant++] = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
  VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                              .pApplicationInfo = &app,
                              .enabledExtensionCount = nwant,
                              .ppEnabledExtensionNames = want_ext};
  VkInstance inst;
  double t0 = vk_now_ms();
  VK_CHECK(vkCreateInstance(&ici, NULL, &inst));
  vk_row("vkCreateInstance ok in %.1f ms", vk_now_ms() - t0);

  VK_IFN(inst, vkEnumeratePhysicalDevices);
  VK_IFN(inst, vkGetPhysicalDeviceProperties2);
  VK_IFN(inst, vkGetPhysicalDeviceFeatures2);
  VK_IFN(inst, vkGetPhysicalDeviceMemoryProperties);
  VK_IFN(inst, vkGetPhysicalDeviceQueueFamilyProperties);
  VK_IFN(inst, vkEnumerateDeviceExtensionProperties);
  VK_IFN(inst, vkDestroyInstance);

  uint32_t ndev = 0;
  t0 = vk_now_ms();
  VK_CHECK(vkEnumeratePhysicalDevices(inst, &ndev, NULL));
  VkPhysicalDevice *devs = calloc(ndev ? ndev : 1, sizeof(*devs));
  VK_CHECK(vkEnumeratePhysicalDevices(inst, &ndev, devs));
  vk_row("vkEnumeratePhysicalDevices: %u device(s) in %.1f ms", ndev, vk_now_ms() - t0);
  vk_row("Devices:");
  vk_row("========");

  int saw_llvmpipe = 0;
  for (uint32_t d = 0; d < ndev; d++) {
    VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
    vkGetPhysicalDeviceProperties2(devs[d], &p2);
    const VkPhysicalDeviceProperties *p = &p2.properties;
    vk_row("GPU%u:", d);
    vk_row("    apiVersion         = %u.%u.%u", VK_API_VERSION_MAJOR(p->apiVersion),
           VK_API_VERSION_MINOR(p->apiVersion), VK_API_VERSION_PATCH(p->apiVersion));
    vk_row("    driverVersion      = %u.%u.%u", VK_VERSION_MAJOR(p->driverVersion),
           VK_VERSION_MINOR(p->driverVersion), VK_VERSION_PATCH(p->driverVersion));
    vk_row("    vendorID           = 0x%04x", p->vendorID);
    vk_row("    deviceID           = 0x%04x", p->deviceID);
    vk_row("    deviceType         = %s", vk_type_name(p->deviceType));
    vk_row("    deviceName         = %s", p->deviceName);
    vk_row("    driverID           = %d", (int)drv.driverID);
    vk_row("    driverName         = %s", drv.driverName);
    vk_row("    driverInfo         = %s", drv.driverInfo);
    vk_row("    conformanceVersion = %u.%u.%u.%u", drv.conformanceVersion.major,
           drv.conformanceVersion.minor, drv.conformanceVersion.subminor,
           drv.conformanceVersion.patch);
    vk_row("    maxImageDimension2D = %u, maxPushConstantsSize = %u, maxBoundDescriptorSets = %u",
           p->limits.maxImageDimension2D, p->limits.maxPushConstantsSize,
           p->limits.maxBoundDescriptorSets);
    if (strstr(p->deviceName, "llvmpipe")) saw_llvmpipe = 1;

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(devs[d], &mp);
    for (uint32_t h = 0; h < mp.memoryHeapCount; h++)
      vk_row("    memoryHeap[%u] size = %llu MiB flags = 0x%x", h,
             (unsigned long long)(mp.memoryHeaps[h].size >> 20), mp.memoryHeaps[h].flags);
    for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
      vk_row("    memoryType[%u] heap %u flags = 0x%x", t, mp.memoryTypes[t].heapIndex,
             mp.memoryTypes[t].propertyFlags);

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &nq, NULL);
    VkQueueFamilyProperties qf[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &nq, qf);
    for (uint32_t q = 0; q < nq; q++)
      vk_row("    queueFamily[%u] count = %u flags = 0x%x timestampValidBits = %u", q,
             qf[q].queueCount, qf[q].queueFlags, qf[q].timestampValidBits);

    uint32_t nde = 0;
    VK_CHECK(vkEnumerateDeviceExtensionProperties(devs[d], NULL, &nde, NULL));
    VkExtensionProperties *de = calloc(nde ? nde : 1, sizeof(*de));
    VK_CHECK(vkEnumerateDeviceExtensionProperties(devs[d], NULL, &nde, de));
    vk_row("    Device Extensions: count = %u", nde);
    static const char *probe_ext[] = {
        "VK_KHR_swapchain", "VK_KHR_push_descriptor", "VK_EXT_robustness2",
        "VK_EXT_shader_demote_to_helper_invocation", "VK_KHR_dynamic_rendering",
        "VK_EXT_extended_dynamic_state", "VK_EXT_extended_dynamic_state2",
        "VK_EXT_extended_dynamic_state3", "VK_EXT_mutable_descriptor_type",
        "VK_VALVE_mutable_descriptor_type", "VK_EXT_descriptor_buffer",
        "VK_EXT_mesh_shader", "VK_EXT_transform_feedback",
        "VK_EXT_vertex_attribute_divisor", "VK_KHR_maintenance5",
        "VK_KHR_maintenance6", "VK_EXT_memory_budget",
        "VK_EXT_external_memory_host", "VK_KHR_external_memory_fd",
        "VK_EXT_conditional_rendering", "VK_EXT_depth_clip_enable",
        "VK_EXT_custom_border_color", "VK_KHR_shader_float16_int8",
        "VK_EXT_shader_stencil_export", "VK_KHR_fragment_shading_rate",
        "VK_EXT_image_view_min_lod", "VK_EXT_graphics_pipeline_library",
        "VK_KHR_pipeline_library", "VK_EXT_shader_object",
        "VK_KHR_ray_tracing_pipeline", "VK_EXT_headless_surface", NULL};
    for (int i = 0; probe_ext[i]; i++)
      vk_row("      %-44s %s", probe_ext[i], has_ext(de, nde, probe_ext[i]) ? "yes" : "no");
    free(de);

    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vkGetPhysicalDeviceFeatures2(devs[d], &f2);
    vk_row("    features: geometryShader=%u tessellationShader=%u shaderFloat64=%u "
           "shaderInt64=%u multiDrawIndirect=%u depthClamp=%u samplerAnisotropy=%u "
           "textureCompressionBC=%u",
           f2.features.geometryShader, f2.features.tessellationShader,
           f2.features.shaderFloat64, f2.features.shaderInt64,
           f2.features.multiDrawIndirect, f2.features.depthClamp,
           f2.features.samplerAnisotropy, f2.features.textureCompressionBC);
    vk_row("    1.2: timelineSemaphore=%u bufferDeviceAddress=%u descriptorIndexing=%u "
           "runtimeDescriptorArray=%u descriptorBindingPartiallyBound=%u "
           "shaderFloat16=%u vulkanMemoryModel=%u",
           f12.timelineSemaphore, f12.bufferDeviceAddress, f12.descriptorIndexing,
           f12.runtimeDescriptorArray, f12.descriptorBindingPartiallyBound,
           f12.shaderFloat16, f12.vulkanMemoryModel);
    vk_row("    1.3: dynamicRendering=%u synchronization2=%u maintenance4=%u "
           "shaderDemoteToHelperInvocation=%u robustImageAccess=%u",
           f13.dynamicRendering, f13.synchronization2, f13.maintenance4,
           f13.shaderDemoteToHelperInvocation, f13.robustImageAccess);
  }

  vkDestroyInstance(inst, NULL);
  free(devs);
  free(ie);
  if (!saw_llvmpipe) {
    vk_row("FAIL no physical device named llvmpipe");
    vk_row("done rc=1");
    return 1;
  }
  vk_row("done rc=0");
  return 0;
}
