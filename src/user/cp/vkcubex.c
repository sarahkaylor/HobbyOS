/* src/user/cp/vkcubex.c — vkcube through a real Vulkan WSI
 * (lane C5-vk2, docs/cp-vk.md §8; docs/cyberpunk.md Gate P5).
 *
 * ONE source, two programs:
 *   Linux  /CP/VK/bin/vkcubex  VK_KHR_xcb_surface (tools/cp/vk/build.sh,
 *                              CP_VK_WSI=x11)
 *   PE     vkcube.exe          VK_KHR_win32_surface under Wine: vulkan-1.dll ->
 *                              winevulkan -> the host libvulkan.so.1 -> lavapipe,
 *                              presented by winex11 (tools/cp/p5/build-pe.sh)
 *
 * The same spinning cube as vkcube.c (shaders, MVP), but presented the way a
 * real Vulkan application presents: VK_KHR_xcb_surface on an X window of the
 * guest's X session (XHOBBY.BIN :0), a VK_KHR_swapchain, acquire -> render ->
 * present, for a fixed wall-clock duration.  With lavapipe the WSI is Mesa's
 * software X11 path (no DRI3, no MIT-SHM on XHOBBY.BIN: docs/cp-x11.md §5),
 * i.e. every present is an xcb_put_image of the swapchain image.
 *
 *   vkcubex [seconds] [W] [H] [present_mode]   default: 65 640 480 fifo
 *           present_mode: fifo | mailbox | immediate (falls back to fifo)
 *
 * Rows ("[VK] ..."): the WSI facts (surface caps, formats, present modes),
 * one progress row every 5 s (frames, fps over the window, acquire / render /
 * present medians), a RESULT row and "[VK] done rc=0".  A fault or a
 * VkResult error ends with "done rc=1".
 *
 * Links libxcb (DT_RPATH /CP/X11/lib, before XSESSION's LD_LIBRARY_PATH=/LIB64)
 * and dlopens libvulkan.so.1 like winevulkan does (vkcommon.h).
 */
#ifndef _WIN32
#define _GNU_SOURCE 1 /* dl_iterate_phdr */
#endif
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR 1
#define SURFACE_EXT VK_KHR_WIN32_SURFACE_EXTENSION_NAME
#else
#define VK_USE_PLATFORM_XCB_KHR 1
#define SURFACE_EXT VK_KHR_XCB_SURFACE_EXTENSION_NAME
#endif
#include "vkcommon.h"

#ifdef _WIN32
#define getpid() ((int)GetCurrentProcessId())
#else
#include <link.h>
#include <unistd.h>
#include <xcb/xcb.h>
#endif

#include "vkcube_frag_spv.h"
#include "vkcube_math.h"
#include "vkcube_vert_spv.h"

#define CLEAR_R 0.06f
#define CLEAR_G 0.07f
#define CLEAR_B 0.11f
#define MAX_IMG 8

static VkInstance inst;
static VkPhysicalDevice pdev;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkPhysicalDeviceMemoryProperties memprops;
static VkSurfaceKHR surf;

#define DFN(name) static PFN_##name name
DFN(vkGetDeviceProcAddr);
DFN(vkCreateImage); DFN(vkDestroyImage); DFN(vkGetImageMemoryRequirements);
DFN(vkAllocateMemory); DFN(vkFreeMemory); DFN(vkBindImageMemory);
DFN(vkCreateImageView); DFN(vkDestroyImageView);
DFN(vkCreateRenderPass); DFN(vkDestroyRenderPass);
DFN(vkCreateFramebuffer); DFN(vkDestroyFramebuffer);
DFN(vkCreateShaderModule); DFN(vkDestroyShaderModule);
DFN(vkCreatePipelineLayout); DFN(vkDestroyPipelineLayout);
DFN(vkCreateGraphicsPipelines); DFN(vkDestroyPipeline);
DFN(vkCreateCommandPool); DFN(vkDestroyCommandPool); DFN(vkAllocateCommandBuffers);
DFN(vkResetCommandBuffer); DFN(vkBeginCommandBuffer); DFN(vkEndCommandBuffer);
DFN(vkCmdBeginRenderPass); DFN(vkCmdEndRenderPass); DFN(vkCmdBindPipeline);
DFN(vkCmdPushConstants); DFN(vkCmdDraw); DFN(vkCmdSetViewport); DFN(vkCmdSetScissor);
DFN(vkCreateFence); DFN(vkDestroyFence); DFN(vkWaitForFences); DFN(vkResetFences);
DFN(vkCreateSemaphore); DFN(vkDestroySemaphore);
DFN(vkQueueSubmit); DFN(vkGetDeviceQueue); DFN(vkDeviceWaitIdle); DFN(vkDestroyDevice);
DFN(vkCreateSwapchainKHR); DFN(vkDestroySwapchainKHR); DFN(vkGetSwapchainImagesKHR);
DFN(vkAcquireNextImageKHR); DFN(vkQueuePresentKHR);
DFN(vkGetPhysicalDeviceSurfaceSupportKHR); DFN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
DFN(vkGetPhysicalDeviceSurfaceFormatsKHR); DFN(vkGetPhysicalDeviceSurfacePresentModesKHR);
DFN(vkDestroySurfaceKHR);

#define LOADD(name) name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
#define LOADI(name) name = (PFN_##name)vk_gipa(inst, #name)

static void fail(const char *what) {
  vk_row("FAIL %s", what);
  vk_row("done rc=1");
  exit(1);
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
  for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return i;
  fail("no memory type");
  return 0;
}

#ifndef _WIN32
/* The compat kernel can hang when a writev() source page is not resident
 * (docs/cp-x11.md §4.2: #PF inside the kernel's memcpy with IF=0).  Touch
 * every page of every loaded object first, as tools/cp/x11/xsmoke.c does;
 * VKCUBEX_PREFAULT=0 skips it; =all repeats it after vkCreateDevice. */
static int prefault_cb(struct dl_phdr_info *info, size_t size, void *data) {
  (void)size;
  long *acc = data;
  for (int i = 0; i < info->dlpi_phnum; i++) {
    const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
    if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_R)) continue;
    uintptr_t a = (info->dlpi_addr + ph->p_vaddr) & ~(uintptr_t)4095;
    uintptr_t e = info->dlpi_addr + ph->p_vaddr + ph->p_memsz;
    for (; a < e; a += 4096) { acc[2] += *(volatile unsigned char *)a; acc[0]++; }
  }
  acc[1]++;
  return 0;
}

static void prefault(const char *when) {
  const char *e = getenv("VKCUBEX_PREFAULT");
  if (e && *e == '0') { vk_row("prefault (%s): disabled", when); return; }
  long acc[3] = {0, 0, 0};
  double t = vk_now_ms();
  dl_iterate_phdr(prefault_cb, acc);
  vk_row("prefault (%s): touched %ld pages in %ld objects in %.0f ms", when, acc[0], acc[1],
         vk_now_ms() - t);
}
#else
/* Under Wine the PE image is not what writev()s; winex11's own unix side
 * would need the workaround, which is not this probe's to apply. */
static void prefault(const char *when) { vk_row("prefault (%s): n/a for a PE image", when); }
#endif

#ifdef _WIN32
/* ---- Win32 window ------------------------------------------------------------ */
static HWND hwnd;
static HINSTANCE hinst;
static volatile int w32_quit, w32_resized;
static uint32_t w32_w, w32_h;

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_CLOSE: w32_quit = 1; return 0;
  case WM_SIZE:
    if (LOWORD(lp) && HIWORD(lp) && (LOWORD(lp) != w32_w || HIWORD(lp) != w32_h)) {
      w32_w = LOWORD(lp); w32_h = HIWORD(lp); w32_resized = 1;
    }
    return 0;
  case WM_PAINT: ValidateRect(h, NULL); return 0;
  default: return DefWindowProcA(h, msg, wp, lp);
  }
}

static void plat_open(uint32_t W, uint32_t H) {
  hinst = GetModuleHandleA(NULL);
  WNDCLASSA wc = {.style = CS_HREDRAW | CS_VREDRAW, .lpfnWndProc = wndproc, .hInstance = hinst,
                  .hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW), .lpszClassName = "vkcube"};
  if (!RegisterClassA(&wc)) fail("RegisterClass");
  RECT r = {0, 0, (LONG)W, (LONG)H};
  AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
  w32_w = W; w32_h = H;
  double t0 = vk_now_ms();
  hwnd = CreateWindowExA(0, "vkcube", "vkcube.exe (Wine, winevulkan, lavapipe)", WS_OVERLAPPEDWINDOW,
                         40, 40, r.right - r.left, r.bottom - r.top, NULL, NULL, hinst, NULL);
  if (!hwnd) fail("CreateWindowEx");
  ShowWindow(hwnd, SW_SHOWNORMAL);
  UpdateWindow(hwnd);
  w32_resized = 0;
  vk_row("win32: window %p %ux%u client, created+shown in %.1f ms", (void *)hwnd, W, H, vk_now_ms() - t0);
}

static int plat_pump(uint32_t *w, uint32_t *h, int *resized) {
  MSG m;
  while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
    if (m.message == WM_QUIT) w32_quit = 1;
    TranslateMessage(&m);
    DispatchMessageA(&m);
  }
  if (w32_resized) { *w = w32_w; *h = w32_h; *resized = 1; w32_resized = 0; }
  return w32_quit;
}

static void plat_surface(void) {
  VK_IFN(inst, vkCreateWin32SurfaceKHR);
  VkWin32SurfaceCreateInfoKHR sci = {.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                                     .hinstance = hinst, .hwnd = hwnd};
  VK_CHECK(vkCreateWin32SurfaceKHR(inst, &sci, NULL, &surf));
}

static void plat_close(void) { DestroyWindow(hwnd); }
#else
/* ---- X ------------------------------------------------------------------------ */
static xcb_connection_t *xc;
static xcb_window_t xwin;
static xcb_atom_t wm_delete;

static xcb_atom_t atom(const char *name) {
  xcb_intern_atom_reply_t *r =
      xcb_intern_atom_reply(xc, xcb_intern_atom(xc, 0, (uint16_t)strlen(name), name), NULL);
  xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
  free(r);
  return a;
}

static void plat_open(uint32_t W, uint32_t H) {
  int scr_i = 0;
  double t0 = vk_now_ms();
  xc = xcb_connect(NULL, &scr_i);
  if (!xc || xcb_connection_has_error(xc)) fail("xcb_connect (DISPLAY unset or no X server)");
  const xcb_setup_t *setup = xcb_get_setup(xc);
  xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
  for (int i = 0; i < scr_i; i++) xcb_screen_next(&it);
  xcb_screen_t *scr = it.data;
  vk_row("x11: xcb_connect ok in %.1f ms DISPLAY=%s screen %d %ux%u depth %u vendor \"%.*s\"",
         vk_now_ms() - t0, getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)", scr_i,
         scr->width_in_pixels, scr->height_in_pixels, scr->root_depth,
         xcb_setup_vendor_length(setup), xcb_setup_vendor(setup));
  static const char *exts[] = {"Present", "DRI3", "MIT-SHM", "XFIXES", "SYNC", "RANDR"};
  for (unsigned i = 0; i < sizeof exts / sizeof exts[0]; i++) {
    xcb_query_extension_reply_t *q = xcb_query_extension_reply(
        xc, xcb_query_extension(xc, (uint16_t)strlen(exts[i]), exts[i]), NULL);
    vk_row("x11: ext %-8s %s", exts[i], q && q->present ? "present" : "absent");
    free(q);
  }
  xwin = xcb_generate_id(xc);
  uint32_t vals[2] = {scr->black_pixel,
                      XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_KEY_PRESS};
  xcb_create_window(xc, XCB_COPY_FROM_PARENT, xwin, scr->root, 40, 40, (uint16_t)W, (uint16_t)H, 0,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT, scr->root_visual,
                    XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, vals);
  const char *title = "vkcube (lavapipe, X11 WSI)";
  xcb_change_property(xc, XCB_PROP_MODE_REPLACE, xwin, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                      (uint32_t)strlen(title), title);
  xcb_atom_t protos = atom("WM_PROTOCOLS");
  wm_delete = atom("WM_DELETE_WINDOW");
  xcb_change_property(xc, XCB_PROP_MODE_REPLACE, xwin, protos, XCB_ATOM_ATOM, 32, 1, &wm_delete);
  xcb_map_window(xc, xwin);
  xcb_flush(xc);
  vk_row("x11: window 0x%x %ux%u mapped", xwin, W, H);
}

/* returns 0 normally, 1 on close request; updates the size on ConfigureNotify */
static int plat_pump(uint32_t *w, uint32_t *h, int *resized) {
  xcb_generic_event_t *ev;
  int quit = 0;
  while ((ev = xcb_poll_for_event(xc))) {
    switch (ev->response_type & 0x7f) {
    case XCB_CONFIGURE_NOTIFY: {
      xcb_configure_notify_event_t *c = (void *)ev;
      if (c->width != *w || c->height != *h) { *w = c->width; *h = c->height; *resized = 1; }
      break;
    }
    case XCB_CLIENT_MESSAGE:
      if (((xcb_client_message_event_t *)ev)->data.data32[0] == wm_delete) quit = 1;
      break;
    default: break;
    }
    free(ev);
  }
  if (xcb_connection_has_error(xc)) fail("X connection error");
  return quit;
}

static void plat_surface(void) {
  VK_IFN(inst, vkCreateXcbSurfaceKHR);
  VkXcbSurfaceCreateInfoKHR sci = {.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
                                   .connection = xc, .window = xwin};
  VK_CHECK(vkCreateXcbSurfaceKHR(inst, &sci, NULL, &surf));
}

static void plat_close(void) {
  xcb_destroy_window(xc, xwin);
  xcb_disconnect(xc);
}
#endif

/* ---- Vulkan instance / device ------------------------------------------------ */
static void init_vulkan(void) {
  VK_IFN(NULL, vkEnumerateInstanceExtensionProperties);
  uint32_t ne = 0;
  vkEnumerateInstanceExtensionProperties(NULL, &ne, NULL);
  VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof(*ep));
  vkEnumerateInstanceExtensionProperties(NULL, &ne, ep);
  int has_surf = 0, has_plat = 0;
  char list[1024] = "";
  for (uint32_t i = 0; i < ne; i++) {
    if (!strcmp(ep[i].extensionName, VK_KHR_SURFACE_EXTENSION_NAME)) has_surf = 1;
    if (!strcmp(ep[i].extensionName, SURFACE_EXT)) has_plat = 1;
    if (strstr(ep[i].extensionName, "surface") && strlen(list) + 64 < sizeof list) {
      strcat(list, " ");
      strcat(list, ep[i].extensionName);
    }
  }
  free(ep);
  vk_row("instance surface extensions (%u total):%s", ne, list);
  if (!has_surf || !has_plat) fail("loader/ICD offer no VK_KHR_surface + " SURFACE_EXT);

  VK_IFN(NULL, vkCreateInstance);
  const char *iext[] = {VK_KHR_SURFACE_EXTENSION_NAME, SURFACE_EXT};
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                           .pApplicationName = "vkcubex-hobbyos", .apiVersion = VK_API_VERSION_1_3};
  VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                              .pApplicationInfo = &app, .enabledExtensionCount = 2,
                              .ppEnabledExtensionNames = iext};
  double t0 = vk_now_ms();
  VK_CHECK(vkCreateInstance(&ici, NULL, &inst));
  VK_IFN(inst, vkEnumeratePhysicalDevices);
  VK_IFN(inst, vkGetPhysicalDeviceProperties);
  VK_IFN(inst, vkGetPhysicalDeviceQueueFamilyProperties);
  VK_IFN(inst, vkGetPhysicalDeviceMemoryProperties);
  VK_IFN(inst, vkCreateDevice);
  LOADI(vkGetPhysicalDeviceSurfaceSupportKHR);
  LOADI(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
  LOADI(vkGetPhysicalDeviceSurfaceFormatsKHR);
  LOADI(vkGetPhysicalDeviceSurfacePresentModesKHR);
  LOADI(vkDestroySurfaceKHR);
  uint32_t n = 1;
  VkResult r = vkEnumeratePhysicalDevices(inst, &n, &pdev);
  if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n == 0) fail("no physical device");
  VkPhysicalDeviceProperties pp;
  vkGetPhysicalDeviceProperties(pdev, &pp);
  vk_row("device: %s (api %u.%u.%u) instance+enumerate %.1f ms", pp.deviceName,
         VK_API_VERSION_MAJOR(pp.apiVersion), VK_API_VERSION_MINOR(pp.apiVersion),
         VK_API_VERSION_PATCH(pp.apiVersion), vk_now_ms() - t0);
  vkGetPhysicalDeviceMemoryProperties(pdev, &memprops);

  plat_surface();

  uint32_t nq = 0;
  VkQueueFamilyProperties qf[8];
  vkGetPhysicalDeviceQueueFamilyProperties(pdev, &nq, NULL);
  if (nq > 8) nq = 8;
  vkGetPhysicalDeviceQueueFamilyProperties(pdev, &nq, qf);
  qfam = UINT32_MAX;
  for (uint32_t i = 0; i < nq; i++) {
    VkBool32 pres = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(pdev, i, surf, &pres);
    if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && pres) { qfam = i; break; }
  }
  if (qfam == UINT32_MAX) fail("no graphics queue that can present to the surface");
  float prio = 1.0f;
  const char *dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                 .queueFamilyIndex = qfam, .queueCount = 1, .pQueuePriorities = &prio};
  VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                            .pQueueCreateInfos = &qci, .enabledExtensionCount = 1,
                            .ppEnabledExtensionNames = dext};
  t0 = vk_now_ms();
  VK_CHECK(vkCreateDevice(pdev, &dci, NULL, &dev));
  vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)vk_gipa(inst, "vkGetDeviceProcAddr");
  LOADD(vkCreateImage); LOADD(vkDestroyImage); LOADD(vkGetImageMemoryRequirements);
  LOADD(vkAllocateMemory); LOADD(vkFreeMemory); LOADD(vkBindImageMemory);
  LOADD(vkCreateImageView); LOADD(vkDestroyImageView);
  LOADD(vkCreateRenderPass); LOADD(vkDestroyRenderPass);
  LOADD(vkCreateFramebuffer); LOADD(vkDestroyFramebuffer);
  LOADD(vkCreateShaderModule); LOADD(vkDestroyShaderModule);
  LOADD(vkCreatePipelineLayout); LOADD(vkDestroyPipelineLayout);
  LOADD(vkCreateGraphicsPipelines); LOADD(vkDestroyPipeline);
  LOADD(vkCreateCommandPool); LOADD(vkDestroyCommandPool); LOADD(vkAllocateCommandBuffers);
  LOADD(vkResetCommandBuffer); LOADD(vkBeginCommandBuffer); LOADD(vkEndCommandBuffer);
  LOADD(vkCmdBeginRenderPass); LOADD(vkCmdEndRenderPass); LOADD(vkCmdBindPipeline);
  LOADD(vkCmdPushConstants); LOADD(vkCmdDraw); LOADD(vkCmdSetViewport); LOADD(vkCmdSetScissor);
  LOADD(vkCreateFence); LOADD(vkDestroyFence); LOADD(vkWaitForFences); LOADD(vkResetFences);
  LOADD(vkCreateSemaphore); LOADD(vkDestroySemaphore);
  LOADD(vkQueueSubmit); LOADD(vkGetDeviceQueue); LOADD(vkDeviceWaitIdle); LOADD(vkDestroyDevice);
  LOADD(vkCreateSwapchainKHR); LOADD(vkDestroySwapchainKHR); LOADD(vkGetSwapchainImagesKHR);
  LOADD(vkAcquireNextImageKHR); LOADD(vkQueuePresentKHR);
  vkGetDeviceQueue(dev, qfam, 0, &queue);
  vk_row("vkCreateDevice ok in %.1f ms (queue family %u, VK_KHR_swapchain)", vk_now_ms() - t0, qfam);
}

/* ---- swapchain ----------------------------------------------------------------- */
static VkSwapchainKHR swch;
static VkFormat sfmt;
static VkExtent2D sext;
static uint32_t nimg;
static VkImage simg[MAX_IMG];
static VkImageView sview[MAX_IMG];
static VkFramebuffer sfb[MAX_IMG];
static VkImage dimg;
static VkDeviceMemory dmem;
static VkImageView dview;
static VkRenderPass rp;
static VkPresentModeKHR pmode;

static const char *pm_name(VkPresentModeKHR m) {
  switch (m) {
  case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
  case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
  case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
  case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
  default: return "other";
  }
}

static void choose_surface(VkPresentModeKHR want) {
  uint32_t nf = 0;
  VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(pdev, surf, &nf, NULL));
  VkSurfaceFormatKHR fm[32];
  if (nf > 32) nf = 32;
  VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(pdev, surf, &nf, fm));
  sfmt = fm[0].format;
  char fl[256] = "";
  for (uint32_t i = 0; i < nf; i++) {
    if (fm[i].format == VK_FORMAT_B8G8R8A8_UNORM) sfmt = fm[i].format;
    if (strlen(fl) + 16 < sizeof fl) snprintf(fl + strlen(fl), sizeof fl - strlen(fl), " %d/%d", fm[i].format, fm[i].colorSpace);
  }
  uint32_t np = 0;
  VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(pdev, surf, &np, NULL));
  VkPresentModeKHR pm[8];
  if (np > 8) np = 8;
  VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(pdev, surf, &np, pm));
  char pl[128] = "";
  pmode = VK_PRESENT_MODE_FIFO_KHR;
  for (uint32_t i = 0; i < np; i++) {
    if (pm[i] == want) pmode = want;
    snprintf(pl + strlen(pl), sizeof pl - strlen(pl), " %s", pm_name(pm[i]));
  }
  vk_row("surface: formats(format/colorspace):%s -> %d; present modes:%s -> %s", fl, sfmt, pl,
         pm_name(pmode));
}

static void create_swapchain(uint32_t W, uint32_t H) {
  VkSurfaceCapabilitiesKHR caps;
  VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pdev, surf, &caps));
  sext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){W, H};
  uint32_t want = caps.minImageCount + 1;
  if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
  if (want > MAX_IMG) want = MAX_IMG;
  vk_row("surface caps: images %u..%u current %ux%u min %ux%u max %ux%u usage 0x%x -> %u images %ux%u",
         caps.minImageCount, caps.maxImageCount, caps.currentExtent.width, caps.currentExtent.height,
         caps.minImageExtent.width, caps.minImageExtent.height, caps.maxImageExtent.width,
         caps.maxImageExtent.height, caps.supportedUsageFlags, want, sext.width, sext.height);
  VkSwapchainKHR old = swch;
  VkSwapchainCreateInfoKHR sc = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                 .surface = surf, .minImageCount = want, .imageFormat = sfmt,
                                 .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
                                 .imageExtent = sext, .imageArrayLayers = 1,
                                 .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                 .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                 .preTransform = caps.currentTransform,
                                 .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                 .presentMode = pmode, .clipped = VK_TRUE, .oldSwapchain = old};
  double t0 = vk_now_ms();
  VK_CHECK(vkCreateSwapchainKHR(dev, &sc, NULL, &swch));
  if (old) vkDestroySwapchainKHR(dev, old, NULL);
  nimg = MAX_IMG;
  VK_CHECK(vkGetSwapchainImagesKHR(dev, swch, &nimg, simg));
  vk_row("swapchain: %u images in %.1f ms", nimg, vk_now_ms() - t0);

  /* depth */
  VkImageCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                          .format = VK_FORMAT_D32_SFLOAT, .extent = {sext.width, sext.height, 1},
                          .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                          .tiling = VK_IMAGE_TILING_OPTIMAL,
                          .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
  VK_CHECK(vkCreateImage(dev, &ic, NULL, &dimg));
  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(dev, dimg, &mr);
  VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                             .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
  VK_CHECK(vkAllocateMemory(dev, &ai, NULL, &dmem));
  VK_CHECK(vkBindImageMemory(dev, dimg, dmem, 0));
  VkImageViewCreateInfo dvc = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = dimg,
                               .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT,
                               .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
  VK_CHECK(vkCreateImageView(dev, &dvc, NULL, &dview));

  if (!rp) {
    VkAttachmentDescription att[2] = {
        {.format = sfmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR},
        {.format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
    VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                               .colorAttachmentCount = 1, .pColorAttachments = &cref,
                               .pDepthStencilAttachment = &dref};
    VkSubpassDependency dep = {.srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
                               .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                               .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    VkRenderPassCreateInfo rpc = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                  .attachmentCount = 2, .pAttachments = att, .subpassCount = 1,
                                  .pSubpasses = &sp, .dependencyCount = 1, .pDependencies = &dep};
    VK_CHECK(vkCreateRenderPass(dev, &rpc, NULL, &rp));
  }
  for (uint32_t i = 0; i < nimg; i++) {
    VkImageViewCreateInfo vc = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = simg[i],
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sfmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VK_CHECK(vkCreateImageView(dev, &vc, NULL, &sview[i]));
    VkImageView v[2] = {sview[i], dview};
    VkFramebufferCreateInfo fbc = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
                                   .attachmentCount = 2, .pAttachments = v, .width = sext.width,
                                   .height = sext.height, .layers = 1};
    VK_CHECK(vkCreateFramebuffer(dev, &fbc, NULL, &sfb[i]));
  }
}

static void destroy_swapchain_views(void) {
  for (uint32_t i = 0; i < nimg; i++) {
    vkDestroyFramebuffer(dev, sfb[i], NULL);
    vkDestroyImageView(dev, sview[i], NULL);
  }
  vkDestroyImageView(dev, dview, NULL);
  vkDestroyImage(dev, dimg, NULL);
  vkFreeMemory(dev, dmem, NULL);
}

static VkPipeline make_pipeline(VkPipelineLayout *pl, VkShaderModule sm[2]) {
  VkShaderModuleCreateInfo smc[2] = {
      {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(vkcube_vert_spv), .pCode = vkcube_vert_spv},
      {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(vkcube_frag_spv), .pCode = vkcube_frag_spv}};
  VK_CHECK(vkCreateShaderModule(dev, &smc[0], NULL, &sm[0]));
  VK_CHECK(vkCreateShaderModule(dev, &smc[1], NULL, &sm[1]));
  VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 64};
  VkPipelineLayoutCreateInfo plc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                    .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
  VK_CHECK(vkCreatePipelineLayout(dev, &plc, NULL, pl));
  VkPipelineShaderStageCreateInfo st[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = sm[0], .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = sm[1], .pName = "main"}};
  VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                               .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
  VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                           .viewportCount = 1, .scissorCount = 1};
  VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                               .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                               .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1};
  VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                             .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
  VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                              .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE,
                                              .depthCompareOp = VK_COMPARE_OP_LESS};
  VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
  VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                            .attachmentCount = 1, .pAttachments = &cba};
  VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dsi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                          .dynamicStateCount = 2, .pDynamicStates = dyn};
  VkGraphicsPipelineCreateInfo gpc = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                      .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
                                      .pInputAssemblyState = &ia, .pViewportState = &vps,
                                      .pRasterizationState = &rs, .pMultisampleState = &ms,
                                      .pDepthStencilState = &ds, .pColorBlendState = &cb,
                                      .pDynamicState = &dsi, .layout = *pl, .renderPass = rp};
  VkPipeline p;
  double t0 = vk_now_ms();
  VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpc, NULL, &p));
  vk_row("pipeline created in %.1f ms", vk_now_ms() - t0);
  return p;
}

static int cmp_d(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

static double median(double *v, int n) {
  if (n <= 0) return 0;
  qsort(v, (size_t)n, sizeof(double), cmp_d);
  return v[n / 2];
}

int main(int argc, char **argv) {
  int secs = argc > 1 ? atoi(argv[1]) : 65;
  uint32_t W = argc > 2 ? (uint32_t)atoi(argv[2]) : 640;
  uint32_t H = argc > 3 ? (uint32_t)atoi(argv[3]) : 480;
  const char *pms = argc > 4 ? argv[4] : "fifo";
  VkPresentModeKHR want = !strcmp(pms, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR
                          : !strcmp(pms, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                                       : VK_PRESENT_MODE_FIFO_KHR;
  if (secs < 1) secs = 1;
  if (W < 16 || W > 4096) W = 640;
  if (H < 16 || H > 4096) H = 480;
  vk_row("vkcubex start (C5-vk2, " SURFACE_EXT "): %d s, %ux%u, present mode %s, pid %d", secs, W, H, pms, (int)getpid());
  vk_report_env();
  prefault("startup");
  plat_open(W, H);
  if (vk_load_loader() != 0) { vk_row("done rc=1"); return 1; }
  init_vulkan();
  /* lavapipe's 60 MiB is never a writev() source; touching it took 13 s in
     the first in-OS run, so it is opt-in (VKCUBEX_PREFAULT=all). */
  if (getenv("VKCUBEX_PREFAULT") && !strcmp(getenv("VKCUBEX_PREFAULT"), "all"))
    prefault("after vkCreateDevice");
  choose_surface(want);
  create_swapchain(W, H);
  VkPipelineLayout pl;
  VkShaderModule sm[2];
  VkPipeline pipe = make_pipeline(&pl, sm);

  VkCommandPoolCreateInfo cpc = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                 .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                 .queueFamilyIndex = qfam};
  VkCommandPool pool;
  VK_CHECK(vkCreateCommandPool(dev, &cpc, NULL, &pool));
  VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                     .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                     .commandBufferCount = 1};
  VkCommandBuffer cmd;
  VK_CHECK(vkAllocateCommandBuffers(dev, &cai, &cmd));
  VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence;
  VK_CHECK(vkCreateFence(dev, &fci, NULL, &fence));
  VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkSemaphore s_acq, s_ren[MAX_IMG];
  VK_CHECK(vkCreateSemaphore(dev, &sci, NULL, &s_acq));
  for (int i = 0; i < MAX_IMG; i++) VK_CHECK(vkCreateSemaphore(dev, &sci, NULL, &s_ren[i]));

  enum { WIN = 4096 };
  static double t_acq[WIN], t_gpu[WIN], t_pres[WIN], t_tot[WIN];
  static double all_tot[1 << 16];
  int nwin = 0, nall = 0, frame = 0, recreated = 0, suboptimal = 0;
  double t_start = vk_now_ms(), t_win = t_start, next_row = 5000.0, worst = 0;
  uint32_t ww = W, wh = H;
  vk_row("render loop: %d s", secs);
  for (;;) {
    double now = vk_now_ms();
    if (now - t_start >= secs * 1000.0) break;
    int resized = 0;
    if (plat_pump(&ww, &wh, &resized)) { vk_row("window close requested - stopping early"); break; }
    if (resized) {
      vkDeviceWaitIdle(dev);
      destroy_swapchain_views();
      create_swapchain(ww, wh);
      recreated++;
    }
    double f0 = vk_now_ms();
    uint32_t idx;
    VkResult ar = vkAcquireNextImageKHR(dev, swch, UINT64_MAX, s_acq, VK_NULL_HANDLE, &idx);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
      vkDeviceWaitIdle(dev);
      destroy_swapchain_views();
      create_swapchain(ww, wh);
      recreated++;
      continue;
    }
    if (ar == VK_SUBOPTIMAL_KHR) suboptimal++;
    else VK_CHECK(ar);
    double f1 = vk_now_ms();
    float mvp[16];
    cube_mvp(mvp, (float)sext.width / (float)sext.height, frame);
    VK_CHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    VkClearValue cv[2] = {{.color = {{CLEAR_R, CLEAR_G, CLEAR_B, 1.0f}}}, {.depthStencil = {1.0f, 0}}};
    VkRect2D area = {{0, 0}, sext};
    VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
                                 .framebuffer = sfb[idx], .renderArea = area, .clearValueCount = 2,
                                 .pClearValues = cv};
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport vp = {0, 0, (float)sext.width, (float)sext.height, 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &area);
    vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, mvp);
    vkCmdDraw(cmd, 36, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
                       .pWaitSemaphores = &s_acq, .pWaitDstStageMask = &ws, .commandBufferCount = 1,
                       .pCommandBuffers = &cmd, .signalSemaphoreCount = 1, .pSignalSemaphores = &s_ren[idx]};
    VK_CHECK(vkQueueSubmit(queue, 1, &si, fence));
    VK_CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(dev, 1, &fence));
    double f2 = vk_now_ms();
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &s_ren[idx], .swapchainCount = 1, .pSwapchains = &swch,
                           .pImageIndices = &idx};
    VkResult pr = vkQueuePresentKHR(queue, &pi);
    if (pr == VK_SUBOPTIMAL_KHR) suboptimal++;
    else if (pr != VK_ERROR_OUT_OF_DATE_KHR) VK_CHECK(pr);
    double f3 = vk_now_ms();
    if (frame < 3)
      vk_row("frame %d: acquire %.2f ms, render %.2f ms, present %.2f ms, total %.2f ms (image %u)",
             frame, f1 - f0, f2 - f1, f3 - f2, f3 - f0, idx);
    if (nwin < WIN) {
      t_acq[nwin] = f1 - f0; t_gpu[nwin] = f2 - f1; t_pres[nwin] = f3 - f2; t_tot[nwin] = f3 - f0;
      nwin++;
    }
    if (nall < (int)(sizeof all_tot / sizeof all_tot[0])) all_tot[nall++] = f3 - f0;
    if (f3 - f0 > worst && frame > 0) worst = f3 - f0;
    frame++;
    double el = f3 - t_start;
    if (el >= next_row) {
      vk_row("t=%.0fs frames=%d window fps=%.2f | median ms acquire %.2f render %.2f present %.2f total %.2f",
             el / 1000.0, frame, nwin * 1000.0 / (f3 - t_win), median(t_acq, nwin), median(t_gpu, nwin),
             median(t_pres, nwin), median(t_tot, nwin));
      nwin = 0;
      t_win = f3;
      next_row += 5000.0;
    }
  }
  double wall = vk_now_ms() - t_start;
  double med = median(all_tot, nall);
  double p90 = nall ? all_tot[(nall * 9) / 10] : 0;
  vk_row("RESULT frames=%d wall=%.1f ms fps=%.2f | per-frame total median %.2f p90 %.2f worst(after frame 0) %.2f ms"
         " | swapchain %ux%u %u images %s | recreated=%d suboptimal=%d",
         frame, wall, frame * 1000.0 / wall, med, p90, worst, sext.width, sext.height, nimg,
         pm_name(pmode), recreated, suboptimal);

  vkDeviceWaitIdle(dev);
  for (int i = 0; i < MAX_IMG; i++) vkDestroySemaphore(dev, s_ren[i], NULL);
  vkDestroySemaphore(dev, s_acq, NULL);
  vkDestroyFence(dev, fence, NULL);
  vkDestroyCommandPool(dev, pool, NULL);
  vkDestroyPipeline(dev, pipe, NULL);
  vkDestroyPipelineLayout(dev, pl, NULL);
  vkDestroyShaderModule(dev, sm[0], NULL);
  vkDestroyShaderModule(dev, sm[1], NULL);
  destroy_swapchain_views();
  vkDestroySwapchainKHR(dev, swch, NULL);
  vkDestroyRenderPass(dev, rp, NULL);
  vkDestroySurfaceKHR(inst, surf, NULL);
  VK_IFN(inst, vkDestroyInstance);
  vkDestroyDevice(dev, NULL);
  vkDestroyInstance(inst, NULL);
  plat_close();
  vk_row("teardown: clean");
  vk_row("done rc=0");
  return 0;
}
