/* nvk_probe.c -- the smallest loader-equivalent probe for HobbyOS's nvk ICD.
 *
 * Why this exists: vulkaninfo is a Vulkan *loader* client.  HobbyOS has no
 * loader in the guest yet, and the loader's only job here is to find an ICD,
 * call vk_icdGetInstanceProcAddr(NULL, ...) and hand the resulting function
 * pointers to the app -- so this probe does exactly that, through the ICD ABI,
 * with no loader, no ICD JSON and no dispatch table in between.  What it
 * exercises is nvk: vkCreateInstance, vkEnumeratePhysicalDevices and the
 * per-device properties a `vulkaninfo --summary` would print.
 *
 * It is a glibc 2.31 PIE (the compat class), linked against the pinned
 * sysroot, and it reports every step so a wall is a named row, not a silence.
 *
 *   [NVK] icd negotiate=5
 *   [NVK] createInstance: VK_SUCCESS
 *   [NVK] enumeratePhysicalDevices: count=N result=0
 *   [NVK] dev[0] name="..." vendor=0x10de device=0x2684 api=1.4.x driver=...
 *   [NVK] done rc=0
 */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>

/* The two ICD entry points.  Declared here rather than dlsym()ed so the probe
 * can also be linked directly against libvulkan_nouveau.so (DT_NEEDED), which
 * is what HobbyOS's compat loader resolves at exec time. */
typedef PFN_vkVoidFunction (*PFN_vk_icdGetInstanceProcAddr)(
   VkInstance instance, const char *pName);
typedef VkResult (*PFN_vk_icdNegotiateLoaderICDInterfaceVersion)(uint32_t *pVersion);

/* The ICD's two entry points, as vulkan_icd.h declares them.  Spelled out here
 * because the probe links against the ICD itself, not the loader's headers. */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);
VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pVersion);

#define LOG(...) do { printf("[NVK] " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static const char *
vkres(VkResult r)
{
   switch (r) {
   case VK_SUCCESS: return "VK_SUCCESS";
   case VK_NOT_READY: return "VK_NOT_READY";
   case VK_TIMEOUT: return "VK_TIMEOUT";
   case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
   case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
   case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
   case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
   case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
   case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
   case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
   case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
   case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
   case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
   case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
   default: return "VK_RESULT_OTHER";
   }
}

int
main(void)
{
   PFN_vk_icdGetInstanceProcAddr gipa =
      (PFN_vk_icdGetInstanceProcAddr)vk_icdGetInstanceProcAddr;
   PFN_vk_icdNegotiateLoaderICDInterfaceVersion negotiate =
      (PFN_vk_icdNegotiateLoaderICDInterfaceVersion)
         vk_icdNegotiateLoaderICDInterfaceVersion;

   LOG("probe start (ICD used directly, no loader)");

   uint32_t icd_version = 5;
   VkResult res = negotiate(&icd_version);
   LOG("icd negotiate: result=%d version=%u", (int)res, icd_version);

   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   PFN_vkEnumerateInstanceExtensionProperties enum_ext =
      (PFN_vkEnumerateInstanceExtensionProperties)
         gipa(NULL, "vkEnumerateInstanceExtensionProperties");
   if (create_instance == NULL) {
      LOG("FAIL: vk_icdGetInstanceProcAddr returned no vkCreateInstance");
      LOG("done rc=1");
      return 1;
   }

   uint32_t ext_count = 0;
   res = enum_ext(NULL, &ext_count, NULL);
   LOG("instanceExtensionProperties: result=%d count=%u", (int)res, ext_count);

   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "hobbyos-nvk-probe",
      .apiVersion = VK_API_VERSION_1_3,
   };
   const VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   VkInstance instance = VK_NULL_HANDLE;
   res = create_instance(&ici, NULL, &instance);
   LOG("createInstance: %s (%d)", vkres(res), (int)res);
   if (res != VK_SUCCESS) {
      LOG("done rc=1");
      return 1;
   }

   PFN_vkEnumeratePhysicalDevices enum_pdev =
      (PFN_vkEnumeratePhysicalDevices)
         gipa(instance, "vkEnumeratePhysicalDevices");
   PFN_vkGetPhysicalDeviceProperties get_props =
      (PFN_vkGetPhysicalDeviceProperties)
         gipa(instance, "vkGetPhysicalDeviceProperties");
   PFN_vkDestroyInstance destroy_instance =
      (PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance");

   uint32_t count = 0;
   res = enum_pdev(instance, &count, NULL);
   LOG("enumeratePhysicalDevices: count=%u %s (%d)", count, vkres(res), (int)res);

   int rc = 1;
   if (res == VK_SUCCESS && count > 0) {
      VkPhysicalDevice pdevs[8];
      uint32_t n = count < 8 ? count : 8;
      res = enum_pdev(instance, &n, pdevs);
      LOG("enumeratePhysicalDevices(handles): n=%u %s (%d)", n, vkres(res), (int)res);
      for (uint32_t i = 0; i < n; i++) {
         VkPhysicalDeviceProperties p;
         get_props(pdevs[i], &p);
         LOG("dev[%u] name=\"%s\" type=%d vendor=0x%04x device=0x%04x "
             "api=%u.%u.%u driver=%u.%u.%u",
             i, p.deviceName, (int)p.deviceType, p.vendorID, p.deviceID,
             VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
             VK_VERSION_PATCH(p.apiVersion),
             VK_VERSION_MAJOR(p.driverVersion), VK_VERSION_MINOR(p.driverVersion),
             VK_VERSION_PATCH(p.driverVersion));
      }
      rc = 0;
   } else if (res != VK_SUCCESS) {
      LOG("wall: vkEnumeratePhysicalDevices returned %s", vkres(res));
   } else {
      LOG("wall: nvk enumerated 0 physical devices (see the nvk:/[NVSHIM] rows above)");
   }

   if (destroy_instance)
      destroy_instance(instance, NULL);

   LOG("done rc=%d", rc);
   return rc;
}
