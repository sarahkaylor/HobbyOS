/* src/user/cp/vkcube.c — the first Vulkan frame on HobbyOS's screen
 * (lane C5-vk, docs/cp-vk.md; docs/cyberpunk.md P5).
 *
 * Headless lavapipe: no WSI, no swapchain.  For each requested size it builds
 * an offscreen B8G8R8A8 colour + D32 depth target, renders a spinning cube
 * (geometry from gl_VertexIndex, MVP in a push constant), copies the image
 * into a host-visible buffer, and blits it into /dev/fb0 — the compat
 * framebuffer node RimWorld's X11/SDL clients use (src/kernel/vm.c mmap of
 * the scanout slot; fsync(fd) is the present verb, linux_compat_misc.c).
 * Without /dev/fb0 (the host smoke run) it renders and reads back only.
 *
 *   vkcube [frames] [hold_s] [WxH ...]      default: 90 15 640x480 1280x720
 *
 * Per frame it times: command recording, submit->fence (render + readback
 * copy, i.e. all lavapipe work), and the CPU blit + present.  The cube angle
 * is a function of the frame index, so a host run and a guest run of the same
 * binaries are comparable by the per-size fnv1a64 of the last frame.
 *
 * Layout on the 1920x1080 scanout: the 1280x720 target at (0,0), the
 * 640x480 target at (1280,0); anything else is centred.
 * Output: "[VK] ..." rows, "[VK] done rc=0".
 */
#include "vkcommon.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <math.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "vkcube_frag_spv.h"
#include "vkcube_vert_spv.h"

#define CLEAR_R 0.06f
#define CLEAR_G 0.07f
#define CLEAR_B 0.11f

/* ---- device-level function table ------------------------------------------ */
static VkInstance inst;
static VkPhysicalDevice pdev;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkPhysicalDeviceMemoryProperties memprops;

#define DFN(name) static PFN_##name name
DFN(vkGetDeviceProcAddr);
DFN(vkCreateImage); DFN(vkDestroyImage); DFN(vkGetImageMemoryRequirements);
DFN(vkAllocateMemory); DFN(vkFreeMemory); DFN(vkBindImageMemory);
DFN(vkCreateImageView); DFN(vkDestroyImageView);
DFN(vkCreateBuffer); DFN(vkDestroyBuffer); DFN(vkGetBufferMemoryRequirements);
DFN(vkBindBufferMemory); DFN(vkMapMemory); DFN(vkUnmapMemory);
DFN(vkCreateRenderPass); DFN(vkDestroyRenderPass);
DFN(vkCreateFramebuffer); DFN(vkDestroyFramebuffer);
DFN(vkCreateShaderModule); DFN(vkDestroyShaderModule);
DFN(vkCreatePipelineLayout); DFN(vkDestroyPipelineLayout);
DFN(vkCreateGraphicsPipelines); DFN(vkDestroyPipeline);
DFN(vkCreateCommandPool); DFN(vkDestroyCommandPool); DFN(vkAllocateCommandBuffers);
DFN(vkResetCommandBuffer); DFN(vkBeginCommandBuffer); DFN(vkEndCommandBuffer);
DFN(vkCmdBeginRenderPass); DFN(vkCmdEndRenderPass); DFN(vkCmdBindPipeline);
DFN(vkCmdPushConstants); DFN(vkCmdDraw); DFN(vkCmdCopyImageToBuffer);
DFN(vkCmdPipelineBarrier);
DFN(vkCreateFence); DFN(vkDestroyFence); DFN(vkWaitForFences); DFN(vkResetFences);
DFN(vkQueueSubmit); DFN(vkGetDeviceQueue); DFN(vkDeviceWaitIdle); DFN(vkDestroyDevice);

#define LOADD(name) name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
  for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want)
      return i;
  vk_row("FAIL no memory type for bits 0x%x flags 0x%x", bits, want);
  vk_row("done rc=1");
  exit(1);
}

static void init_vulkan(void) {
  VK_IFN(NULL, vkCreateInstance);
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                           .pApplicationName = "vkcube-hobbyos",
                           .apiVersion = VK_API_VERSION_1_3};
  VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                              .pApplicationInfo = &app};
  double t0 = vk_now_ms();
  VK_CHECK(vkCreateInstance(&ici, NULL, &inst));
  VK_IFN(inst, vkEnumeratePhysicalDevices);
  VK_IFN(inst, vkGetPhysicalDeviceProperties);
  VK_IFN(inst, vkGetPhysicalDeviceQueueFamilyProperties);
  VK_IFN(inst, vkGetPhysicalDeviceMemoryProperties);
  VK_IFN(inst, vkCreateDevice);
  uint32_t n = 1;
  VkResult r = vkEnumeratePhysicalDevices(inst, &n, &pdev);
  if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n == 0) {
    vk_row("FAIL no physical device (VkResult %d, n=%u)", (int)r, n);
    vk_row("done rc=1");
    exit(1);
  }
  VkPhysicalDeviceProperties pp;
  vkGetPhysicalDeviceProperties(pdev, &pp);
  vk_row("device: %s (api %u.%u.%u) instance+enumerate %.1f ms", pp.deviceName,
         VK_API_VERSION_MAJOR(pp.apiVersion), VK_API_VERSION_MINOR(pp.apiVersion),
         VK_API_VERSION_PATCH(pp.apiVersion), vk_now_ms() - t0);
  vkGetPhysicalDeviceMemoryProperties(pdev, &memprops);
  uint32_t nq = 0;
  VkQueueFamilyProperties qf[8];
  vkGetPhysicalDeviceQueueFamilyProperties(pdev, &nq, NULL);
  if (nq > 8) nq = 8;
  vkGetPhysicalDeviceQueueFamilyProperties(pdev, &nq, qf);
  qfam = UINT32_MAX;
  for (uint32_t i = 0; i < nq; i++)
    if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfam = i; break; }
  if (qfam == UINT32_MAX) { vk_row("FAIL no graphics queue"); vk_row("done rc=1"); exit(1); }
  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                 .queueFamilyIndex = qfam, .queueCount = 1,
                                 .pQueuePriorities = &prio};
  VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                            .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
  t0 = vk_now_ms();
  VK_CHECK(vkCreateDevice(pdev, &dci, NULL, &dev));
  vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)vk_gipa(inst, "vkGetDeviceProcAddr");
  LOADD(vkCreateImage); LOADD(vkDestroyImage); LOADD(vkGetImageMemoryRequirements);
  LOADD(vkAllocateMemory); LOADD(vkFreeMemory); LOADD(vkBindImageMemory);
  LOADD(vkCreateImageView); LOADD(vkDestroyImageView);
  LOADD(vkCreateBuffer); LOADD(vkDestroyBuffer); LOADD(vkGetBufferMemoryRequirements);
  LOADD(vkBindBufferMemory); LOADD(vkMapMemory); LOADD(vkUnmapMemory);
  LOADD(vkCreateRenderPass); LOADD(vkDestroyRenderPass);
  LOADD(vkCreateFramebuffer); LOADD(vkDestroyFramebuffer);
  LOADD(vkCreateShaderModule); LOADD(vkDestroyShaderModule);
  LOADD(vkCreatePipelineLayout); LOADD(vkDestroyPipelineLayout);
  LOADD(vkCreateGraphicsPipelines); LOADD(vkDestroyPipeline);
  LOADD(vkCreateCommandPool); LOADD(vkDestroyCommandPool); LOADD(vkAllocateCommandBuffers);
  LOADD(vkResetCommandBuffer); LOADD(vkBeginCommandBuffer); LOADD(vkEndCommandBuffer);
  LOADD(vkCmdBeginRenderPass); LOADD(vkCmdEndRenderPass); LOADD(vkCmdBindPipeline);
  LOADD(vkCmdPushConstants); LOADD(vkCmdDraw); LOADD(vkCmdCopyImageToBuffer);
  LOADD(vkCmdPipelineBarrier);
  LOADD(vkCreateFence); LOADD(vkDestroyFence); LOADD(vkWaitForFences); LOADD(vkResetFences);
  LOADD(vkQueueSubmit); LOADD(vkGetDeviceQueue); LOADD(vkDeviceWaitIdle);
  LOADD(vkDestroyDevice);
  vkGetDeviceQueue(dev, qfam, 0, &queue);
  vk_row("vkCreateDevice ok in %.1f ms (queue family %u)", vk_now_ms() - t0, qfam);
}

/* ---- /dev/fb0 ----------------------------------------------------------------- */
static int fb_fd = -1;
static uint8_t *fb_mem;
static uint32_t fb_w, fb_h, fb_stride;

static void fb_open(void) {
  fb_fd = open("/dev/fb0", O_RDWR);
  if (fb_fd < 0) { vk_row("fb: /dev/fb0 not available - offscreen only"); return; }
  struct fb_var_screeninfo v;
  struct fb_fix_screeninfo f;
  if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &v) != 0 || ioctl(fb_fd, FBIOGET_FSCREENINFO, &f) != 0) {
    vk_row("fb: FBIOGET_* failed - offscreen only");
    close(fb_fd); fb_fd = -1; return;
  }
  fb_w = v.xres; fb_h = v.yres; fb_stride = f.line_length;
  void *m = mmap(NULL, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
  if (m == MAP_FAILED) {
    vk_row("fb: mmap failed - offscreen only");
    close(fb_fd); fb_fd = -1; return;
  }
  fb_mem = m;
  vk_row("fb: /dev/fb0 %ux%u bpp=%u stride=%u smem=%u (R@%u G@%u B@%u)", fb_w, fb_h,
         v.bits_per_pixel, fb_stride, f.smem_len, v.red.offset, v.green.offset, v.blue.offset);
  for (uint32_t y = 0; y < fb_h; y++) { /* dark backdrop so the frames stand out */
    uint32_t *row = (uint32_t *)(fb_mem + (size_t)y * fb_stride);
    for (uint32_t x = 0; x < fb_w; x++) row[x] = 0xff202020u;
  }
  fsync(fb_fd);
}

static void fb_place(uint32_t w, uint32_t h, uint32_t *ox, uint32_t *oy) {
  if (w == 1280 && h == 720) { *ox = 0; *oy = 0; return; }
  if (w == 640 && h == 480 && fb_w >= 1920) { *ox = 1280; *oy = 0; return; }
  *ox = fb_w > w ? (fb_w - w) / 2 : 0;
  *oy = fb_h > h ? (fb_h - h) / 2 : 0;
}

/* ---- maths (column-major, Vulkan clip space: y down, z in [0,1]) ------------- */
static void mat_mul(float *o, const float *a, const float *b) {
  float t[16];
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++) {
      float s = 0;
      for (int k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
      t[c * 4 + r] = s;
    }
  memcpy(o, t, sizeof(t));
}

static void cube_mvp(float *mvp, float aspect, int frame) {
  float a = (float)frame * (6.2831853f / 90.0f);
  float f = 1.0f / tanf(0.5f * 0.9f), n = 0.1f, fa = 50.0f;
  float P[16] = {f / aspect, 0, 0, 0,  0, -f, 0, 0,  0, 0, fa / (n - fa), -1,
                 0, 0, n * fa / (n - fa), 0};
  float V[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -5.0f, 1};
  float cy = cosf(a), sy = sinf(a), cx = cosf(a * 0.7f), sx = sinf(a * 0.7f);
  float RY[16] = {cy, 0, -sy, 0, 0, 1, 0, 0, sy, 0, cy, 0, 0, 0, 0, 1};
  float RX[16] = {1, 0, 0, 0, 0, cx, sx, 0, 0, -sx, cx, 0, 0, 0, 0, 1};
  float M[16];
  mat_mul(M, RY, RX);
  mat_mul(mvp, V, M);
  mat_mul(mvp, P, mvp);
}

static int cmp_d(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

/* ---- one size ------------------------------------------------------------------ */
static void run_size(uint32_t W, uint32_t H, int frames) {
  vk_row("size %ux%u: %d frames", W, H, frames);
  double ts = vk_now_ms();
  VkImage img[2];
  VkDeviceMemory imem[2];
  VkImageView view[2];
  VkFormat fmt[2] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_D32_SFLOAT};
  VkImageUsageFlags use[2] = {VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT};
  VkImageAspectFlags asp[2] = {VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_DEPTH_BIT};
  for (int i = 0; i < 2; i++) {
    VkImageCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D, .format = fmt[i],
                            .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = use[i], .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VK_CHECK(vkCreateImage(dev, &ic, NULL, &img[i]));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, img[i], &mr);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = mr.size,
                               .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VK_CHECK(vkAllocateMemory(dev, &ai, NULL, &imem[i]));
    VK_CHECK(vkBindImageMemory(dev, img[i], imem[i], 0));
    VkImageViewCreateInfo vc = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = img[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = fmt[i],
                                .subresourceRange = {asp[i], 0, 1, 0, 1}};
    VK_CHECK(vkCreateImageView(dev, &vc, NULL, &view[i]));
  }
  VkDeviceSize bytes = (VkDeviceSize)W * H * 4;
  VkBuffer rb;
  VkDeviceMemory rbmem;
  VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
                           .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
  VK_CHECK(vkCreateBuffer(dev, &bc, NULL, &rb));
  VkMemoryRequirements bmr;
  vkGetBufferMemoryRequirements(dev, rb, &bmr);
  VkMemoryAllocateInfo bai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = bmr.size,
                              .memoryTypeIndex = mem_type(bmr.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
  VK_CHECK(vkAllocateMemory(dev, &bai, NULL, &rbmem));
  VK_CHECK(vkBindBufferMemory(dev, rb, rbmem, 0));
  void *rbp;
  VK_CHECK(vkMapMemory(dev, rbmem, 0, bytes, 0, &rbp));

  VkAttachmentDescription att[2] = {
      {.format = fmt[0], .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
       .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
       .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
       .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
      {.format = fmt[1], .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
       .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
       .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
       .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
  VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference dref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                             .colorAttachmentCount = 1, .pColorAttachments = &cref,
                             .pDepthStencilAttachment = &dref};
  VkSubpassDependency dep = {.srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
                             .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
                             .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                             .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
  VkRenderPassCreateInfo rpc = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                .attachmentCount = 2, .pAttachments = att, .subpassCount = 1,
                                .pSubpasses = &sp, .dependencyCount = 1, .pDependencies = &dep};
  VkRenderPass rp;
  VK_CHECK(vkCreateRenderPass(dev, &rpc, NULL, &rp));
  VkFramebufferCreateInfo fbc = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                 .renderPass = rp, .attachmentCount = 2, .pAttachments = view,
                                 .width = W, .height = H, .layers = 1};
  VkFramebuffer fbo;
  VK_CHECK(vkCreateFramebuffer(dev, &fbc, NULL, &fbo));

  VkShaderModule sm[2];
  VkShaderModuleCreateInfo smc[2] = {
      {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(vkcube_vert_spv), .pCode = vkcube_vert_spv},
      {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(vkcube_frag_spv), .pCode = vkcube_frag_spv}};
  VK_CHECK(vkCreateShaderModule(dev, &smc[0], NULL, &sm[0]));
  VK_CHECK(vkCreateShaderModule(dev, &smc[1], NULL, &sm[1]));
  VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 64};
  VkPipelineLayoutCreateInfo plc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                    .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
  VkPipelineLayout pl;
  VK_CHECK(vkCreatePipelineLayout(dev, &plc, NULL, &pl));
  VkPipelineShaderStageCreateInfo st[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = sm[0], .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = sm[1], .pName = "main"}};
  VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                               .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
  VkViewport vp = {0, 0, (float)W, (float)H, 0, 1};
  VkRect2D sc = {{0, 0}, {W, H}};
  VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                           .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc};
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
  VkGraphicsPipelineCreateInfo gpc = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                      .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
                                      .pInputAssemblyState = &ia, .pViewportState = &vps,
                                      .pRasterizationState = &rs, .pMultisampleState = &ms,
                                      .pDepthStencilState = &ds, .pColorBlendState = &cb,
                                      .layout = pl, .renderPass = rp, .subpass = 0};
  VkPipeline pipe;
  double tp = vk_now_ms();
  VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpc, NULL, &pipe));
  double pipe_ms = vk_now_ms() - tp;

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
  vk_row("size %ux%u: setup %.1f ms (pipeline create %.1f ms)", W, H, vk_now_ms() - ts, pipe_ms);

  uint32_t ox = 0, oy = 0;
  if (fb_mem) fb_place(W, H, &ox, &oy);
  double *t_gpu = calloc((size_t)frames, sizeof(double));
  double *t_tot = calloc((size_t)frames, sizeof(double));
  double *t_blit = calloc((size_t)frames, sizeof(double));
  double t_rec_sum = 0, t_all0 = vk_now_ms();
  for (int fr = 0; fr < frames; fr++) {
    double f0 = vk_now_ms();
    float mvp[16];
    cube_mvp(mvp, (float)W / (float)H, fr);
    VK_CHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    VkClearValue cv[2] = {{.color = {{CLEAR_R, CLEAR_G, CLEAR_B, 1.0f}}}, {.depthStencil = {1.0f, 0}}};
    VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                 .renderPass = rp, .framebuffer = fbo, .renderArea = sc,
                                 .clearValueCount = 2, .pClearValues = cv};
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, mvp);
    vkCmdDraw(cmd, 36, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    VkBufferImageCopy cr = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {W, H, 1}};
    vkCmdCopyImageToBuffer(cmd, img[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &cr);
    VkBufferMemoryBarrier hb = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .buffer = rb, .offset = 0, .size = VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0, NULL, 1, &hb, 0, NULL);
    VK_CHECK(vkEndCommandBuffer(cmd));
    double f1 = vk_now_ms();
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                       .pCommandBuffers = &cmd};
    VK_CHECK(vkQueueSubmit(queue, 1, &si, fence));
    VK_CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(dev, 1, &fence));
    double f2 = vk_now_ms();
    if (fb_mem) {
      uint32_t cw = W, ch = H;
      if (ox + cw > fb_w) cw = fb_w - ox;
      if (oy + ch > fb_h) ch = fb_h - oy;
      for (uint32_t y = 0; y < ch; y++)
        memcpy(fb_mem + (size_t)(oy + y) * fb_stride + (size_t)ox * 4,
               (const uint8_t *)rbp + (size_t)y * W * 4, (size_t)cw * 4);
      fsync(fb_fd);
    }
    double f3 = vk_now_ms();
    t_rec_sum += f1 - f0;
    t_gpu[fr] = f2 - f1;
    t_blit[fr] = f3 - f2;
    t_tot[fr] = f3 - f0;
    if (fr < 3 || fr == frames - 1)
      vk_row("size %ux%u frame %d: record %.2f ms, render+readback %.2f ms, blit+present %.2f ms, total %.2f ms",
             W, H, fr, f1 - f0, f2 - f1, f3 - f2, f3 - f0);
  }
  double wall = vk_now_ms() - t_all0;

  /* last frame: identity + coverage */
  const uint8_t *px = rbp;
  uint64_t h = 1469598103934665603ULL;
  for (VkDeviceSize i = 0; i < bytes; i++) { h ^= px[i]; h *= 1099511628211ULL; }
  uint32_t bg = (uint32_t)(CLEAR_B * 255.0f + 0.5f) | (uint32_t)(CLEAR_G * 255.0f + 0.5f) << 8 |
                (uint32_t)(CLEAR_R * 255.0f + 0.5f) << 16;
  uint64_t cover = 0;
  for (uint64_t i = 0; i < (uint64_t)W * H; i++) {
    uint32_t p = ((const uint32_t *)rbp)[i] & 0xffffffu;
    int d = abs((int)(p & 0xff) - (int)(bg & 0xff)) + abs((int)((p >> 8) & 0xff) - (int)((bg >> 8) & 0xff)) +
            abs((int)((p >> 16) & 0xff) - (int)((bg >> 16) & 0xff));
    if (d > 6) cover++;
  }
  qsort(t_gpu, (size_t)frames, sizeof(double), cmp_d);
  qsort(t_tot, (size_t)frames, sizeof(double), cmp_d);
  qsort(t_blit, (size_t)frames, sizeof(double), cmp_d);
  vk_row("size %ux%u RESULT frames=%d wall=%.1f ms fps=%.2f | per-frame total ms min/median/p90/max "
         "%.2f/%.2f/%.2f/%.2f | render+readback median %.2f | blit+present median %.2f | record mean %.3f",
         W, H, frames, wall, frames * 1000.0 / wall, t_tot[0], t_tot[frames / 2],
         t_tot[(frames * 9) / 10], t_tot[frames - 1], t_gpu[frames / 2], t_blit[frames / 2],
         t_rec_sum / frames);
  vk_row("size %ux%u last frame fnv1a64=0x%016llx covered=%llu/%llu px (%.1f%%) placed at (%u,%u)%s",
         W, H, (unsigned long long)h, (unsigned long long)cover,
         (unsigned long long)W * H, 100.0 * (double)cover / ((double)W * H), ox, oy,
         fb_mem ? "" : " [no fb]");
  free(t_gpu); free(t_tot); free(t_blit);

  vkDeviceWaitIdle(dev);
  vkDestroyFence(dev, fence, NULL);
  vkDestroyCommandPool(dev, pool, NULL);
  vkDestroyPipeline(dev, pipe, NULL);
  vkDestroyPipelineLayout(dev, pl, NULL);
  vkDestroyShaderModule(dev, sm[0], NULL);
  vkDestroyShaderModule(dev, sm[1], NULL);
  vkDestroyFramebuffer(dev, fbo, NULL);
  vkDestroyRenderPass(dev, rp, NULL);
  vkUnmapMemory(dev, rbmem);
  vkDestroyBuffer(dev, rb, NULL);
  vkFreeMemory(dev, rbmem, NULL);
  for (int i = 0; i < 2; i++) {
    vkDestroyImageView(dev, view[i], NULL);
    vkDestroyImage(dev, img[i], NULL);
    vkFreeMemory(dev, imem[i], NULL);
  }
}

int main(int argc, char **argv) {
  int frames = argc > 1 ? atoi(argv[1]) : 90;
  int hold = argc > 2 ? atoi(argv[2]) : 15;
  if (frames < 1) frames = 1;
  vk_row("vkcube start (C5-vk): frames=%d hold=%ds", frames, hold);
  vk_report_env();
  if (vk_load_loader() != 0) { vk_row("done rc=1"); return 1; }
  init_vulkan();
  fb_open();
  int nsizes = 0;
  for (int i = 3; i < argc; i++) {
    unsigned w, h;
    if (sscanf(argv[i], "%ux%u", &w, &h) == 2 && w && h && w <= 4096 && h <= 4096) {
      run_size(w, h, frames);
      nsizes++;
    }
  }
  if (!nsizes) {
    run_size(640, 480, frames);
    run_size(1280, 720, frames);
  }
  if (fb_mem && hold > 0) {
    vk_row("hold %d s: frames are on /dev/fb0 now (screendump window)", hold);
    sleep((unsigned)hold);
  }
  VK_IFN(inst, vkDestroyInstance);
  vkDestroyDevice(dev, NULL);
  vkDestroyInstance(inst, NULL);
  vk_row("teardown: clean");
  vk_row("done rc=0");
  return 0;
}
