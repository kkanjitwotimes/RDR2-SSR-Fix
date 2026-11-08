// Native simulation of the real hook code in src/rdr2_overscan.cpp.
// The source is #included whole; Win32/Detours are fakes (tests/shim) and the "driver" is a set of mocks that
// record what the hooks send to it. This checks logic and command ordering, NOT behaviour inside RDR2 or on a GPU.
#include "../src/rdr2_overscan.cpp"
#include <cassert>
#include <fstream>
#include <regex>
#include <sstream>

using namespace ovs;
static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)
template <class T> static T Hn(uintptr_t n) { return (T)n; }
#define HV(x) ((unsigned long long)(uintptr_t)(x))

// ---------------------------------------------------------------- mock driver
static std::vector<std::string> ev;
static int liveScratch = 0, liveMem = 0;
static uintptr_t nextHandle = 0x2000, nextScratch = 0x9000;
static VkImageUsageFlags lastCreateUsage = 0;
static bool rejectBump = false;
static VkImageUsageFlags surfaceSupports = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
static VkFormatFeatureFlags formatFeatures = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
static uint32_t nextAcquire = 0;
struct BlitRec { VkImage src, dst; VkOffset3D s0, s1, d0, d1; VkFilter f; };
static std::vector<BlitRec> blits;
static std::string imgName(VkImage i) { return (uintptr_t)i >= 0x9000 ? "scratch" + std::to_string((uintptr_t)i - 0x9000) : "img" + std::to_string((uintptr_t)i - 0x1000); }

static VKAPI_ATTR VkResult VKAPI_CALL mCreateSwapchain(VkDevice, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks*, VkSwapchainKHR* out) {
  lastCreateUsage = ci->imageUsage;
  if (rejectBump && (ci->imageUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return VK_ERROR_INITIALIZATION_FAILED;
  *out = Hn<VkSwapchainKHR>(0x500 + nextHandle++); return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL mGetImages(VkDevice, VkSwapchainKHR, uint32_t* n, VkImage* imgs) {
  if (!imgs) { *n = 3; return VK_SUCCESS; }
  for (uint32_t i = 0; i < *n && i < 3; ++i) imgs[i] = Hn<VkImage>(0x1000 + i);
  *n = 3; return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL mDestroySwapchain(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*) { ev.push_back("REAL_DESTROY_SC"); }
static VKAPI_ATTR VkResult VKAPI_CALL mAcquire(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t* idx) { *idx = nextAcquire; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL mPresent(VkQueue, const VkPresentInfoKHR*) { ev.push_back("REAL_PRESENT"); return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL mCreateView(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView* o) { *o = Hn<VkImageView>(nextHandle++); return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL mCreateFb(VkDevice, const VkFramebufferCreateInfo*, const VkAllocationCallbacks*, VkFramebuffer* o) { *o = Hn<VkFramebuffer>(nextHandle++); return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL mCreateRp(VkDevice, const VkRenderPassCreateInfo*, const VkAllocationCallbacks*, VkRenderPass* o) { *o = Hn<VkRenderPass>(nextHandle++); return VK_SUCCESS; }
static VKAPI_ATTR void VKAPI_CALL mBeginRp(VkCommandBuffer, const VkRenderPassBeginInfo*, VkSubpassContents) { ev.push_back("REAL_BEGIN_RP"); }
static VKAPI_ATTR void VKAPI_CALL mBeginRendering(VkCommandBuffer, const VkRenderingInfo*) { ev.push_back("REAL_BEGIN_RENDERING"); }
static VKAPI_ATTR void VKAPI_CALL mRealBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t, const VkImageMemoryBarrier*) { ev.push_back("REAL_GAME_BARRIER"); }
static VKAPI_ATTR void VKAPI_CALL mRealBlit(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageBlit*, VkFilter) { ev.push_back("REAL_GAME_BLIT"); }
static VKAPI_ATTR void VKAPI_CALL mRealCopy(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageCopy*) { ev.push_back("REAL_GAME_COPY"); }
static VKAPI_ATTR void VKAPI_CALL mRealClear(VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue*, uint32_t, const VkImageSubresourceRange*) { ev.push_back("REAL_GAME_CLEAR"); }

// device functions the mod resolves for itself
static VKAPI_ATTR VkResult VKAPI_CALL dCreateImage(VkDevice, const VkImageCreateInfo* ci, const VkAllocationCallbacks*, VkImage* o) {
  assert(ci->usage == (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)); assert(ci->extent.width == 1920 && ci->extent.height == 1080);
  assert(ci->format == VK_FORMAT_B8G8R8A8_UNORM && ci->tiling == VK_IMAGE_TILING_OPTIMAL && ci->samples == VK_SAMPLE_COUNT_1_BIT);
  *o = Hn<VkImage>(nextScratch++); ++liveScratch; return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL dDestroyImage(VkDevice, VkImage, const VkAllocationCallbacks*) { --liveScratch; }
static VKAPI_ATTR void VKAPI_CALL dGetReq(VkDevice, VkImage, VkMemoryRequirements* r) { r->size = 1920 * 1080 * 4; r->alignment = 256; r->memoryTypeBits = 0x3; }
static VKAPI_ATTR VkResult VKAPI_CALL dAlloc(VkDevice, const VkMemoryAllocateInfo* ai, const VkAllocationCallbacks*, VkDeviceMemory* o) { assert(ai->memoryTypeIndex == 1); *o = Hn<VkDeviceMemory>(nextHandle++); ++liveMem; return VK_SUCCESS; }
static VKAPI_ATTR void VKAPI_CALL dFree(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*) { --liveMem; }
static VKAPI_ATTR VkResult VKAPI_CALL dBind(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) { return VK_SUCCESS; }
static VKAPI_ATTR void VKAPI_CALL dBarrier(VkCommandBuffer, VkPipelineStageFlags ss, VkPipelineStageFlags ds, VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t n, const VkImageMemoryBarrier* b) {
  std::string s = "BARRIER";
  for (uint32_t i = 0; i < n; ++i) {
    assert(b[i].subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && b[i].subresourceRange.levelCount == 1 && b[i].subresourceRange.layerCount == 1);
    assert(b[i].srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED && b[i].dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
    assert(b[i].newLayout != VK_IMAGE_LAYOUT_UNDEFINED && b[i].newLayout != VK_IMAGE_LAYOUT_PREINITIALIZED);
    s += " [" + imgName(b[i].image) + " " + LayoutName((int)b[i].oldLayout) + ">" + LayoutName((int)b[i].newLayout) + "]";
  }
  (void)ss; (void)ds; ev.push_back(s);
}
static VKAPI_ATTR void VKAPI_CALL dCopy(VkCommandBuffer, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy* r) {
  assert(n == 1 && r->extent.width == 1920 && r->extent.height == 1080 && r->extent.depth == 1);
  ev.push_back("COPY " + imgName(s) + "(" + LayoutName((int)sl) + ")->" + imgName(d) + "(" + LayoutName((int)dl) + ")");
}
static VKAPI_ATTR void VKAPI_CALL dBlit(VkCommandBuffer, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit* r, VkFilter f) {
  assert(n == 1); blits.push_back({s, d, r->srcOffsets[0], r->srcOffsets[1], r->dstOffsets[0], r->dstOffsets[1], f});
  ev.push_back("BLIT " + imgName(s) + "(" + LayoutName((int)sl) + ")->" + imgName(d) + "(" + LayoutName((int)dl) + ")");
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL mGDPA(VkDevice, const char* n) {
#define R(name, fn) if (!strcmp(n, name)) return (PFN_vkVoidFunction)fn
  R("vkCreateImage", dCreateImage); R("vkDestroyImage", dDestroyImage); R("vkGetImageMemoryRequirements", dGetReq);
  R("vkAllocateMemory", dAlloc); R("vkFreeMemory", dFree); R("vkBindImageMemory", dBind);
  R("vkCmdPipelineBarrier", dBarrier); R("vkCmdCopyImage", dCopy); R("vkCmdBlitImage", dBlit);
#undef R
  return nullptr;
}
static VKAPI_ATTR void VKAPI_CALL mFmtProps(VkPhysicalDevice, VkFormat, VkFormatProperties* p) { p->optimalTilingFeatures = formatFeatures; }
static VKAPI_ATTR void VKAPI_CALL mMemProps(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* p) {
  p->memoryTypeCount = 2; p->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT; p->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
}
static VKAPI_ATTR VkResult VKAPI_CALL mSurfCaps(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR* c) { c->supportedUsageFlags = surfaceSupports; return VK_SUCCESS; }

// ---------------------------------------------------------------- fixtures
static const VkDevice kDev = Hn<VkDevice>(0x10);
static const VkCommandBuffer kCmd = Hn<VkCommandBuffer>(0x20);
static const VkCommandBuffer kCmd2 = Hn<VkCommandBuffer>(0x21);
static const char* kLog = "/tmp/ovs_sim.log";

static void Reset() {
  { std::lock_guard<std::mutex> l(g_mu); g_sc.clear(); g_imgRef.clear(); g_views.clear(); g_fbs.clear(); g_passes.clear(); g_retired.clear(); }
  g_presents = 0; ev.clear(); blits.clear(); liveScratch = liveMem = 0; nextAcquire = 0; rejectBump = false;
  surfaceSupports = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  formatFeatures = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  g_cap.active = false; g_cap.keyDown = false; g_cap.id = 0; g_fakeKeyDown = false;
  g_cfg.enabled = true; g_cfg.zoom = 1.05; g_cfg.passIndex = 1; g_cfg.requireDepth = false; g_cfg.engageFrames = 0; g_cfg.rampFrames = 0; g_stableStreak = 0; g_frameQual = false; g_cfg.captureFrames = 3; g_cfg.captureKey = 0xDC; g_cfg.logPasses = g_cfg.logSubmits = g_cfg.logCmdBegin = true;
  g_pd = Hn<VkPhysicalDevice>(0x30); g_firstDevice = kDev;
  g_fnFormatProps = mFmtProps; g_fnMemProps = mMemProps; g_fnSurfCaps = mSurfCaps; o_GDPA = mGDPA;
  P_CreateSwapchainKHR.gp = mCreateSwapchain; P_GetSwapchainImagesKHR.gp = mGetImages; P_DestroySwapchainKHR.gp = mDestroySwapchain;
  P_AcquireNextImageKHR.gp = mAcquire; P_QueuePresentKHR.gp = mPresent; P_CreateImageView.gp = mCreateView; P_CreateFramebuffer.gp = mCreateFb;
  P_CreateRenderPass.gp = mCreateRp; P_CmdBeginRenderPass.gp = mBeginRp; P_CmdBeginRendering.gp = mBeginRendering;
  P_CmdPipelineBarrier.gp = mRealBarrier; P_CmdBlitImage.gp = mRealBlit; P_CmdCopyImage.gp = mRealCopy; P_CmdClearColorImage.gp = mRealClear;
  g_logPath = L"/tmp/ovs_sim.log"; unlink(kLog);
}

static VkSwapchainKHR MakeSwapchain(VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) {
  VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  ci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM; ci.imageExtent = {1920, 1080}; ci.imageUsage = usage; ci.presentMode = VK_PRESENT_MODE_FIFO_KHR; ci.minImageCount = 3;
  VkSwapchainKHR sc{}; VkResult r = H<0>::CreateSwapchainKHR(kDev, &ci, nullptr, &sc); assert(r == VK_SUCCESS);
  uint32_t n = 0; H<0>::GetSwapchainImagesKHR(kDev, sc, &n, nullptr);
  std::vector<VkImage> imgs(n); H<0>::GetSwapchainImagesKHR(kDev, sc, &n, imgs.data());
  return sc;
}
struct Pass { VkRenderPass rp; VkFramebuffer fb; VkImageView view; };
static Pass MakePass(int imgIdx, VkAttachmentLoadOp load, VkImageLayout initial, bool swapchainImage = true, int depthLayout = -1) {
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = swapchainImage ? Hn<VkImage>(0x1000 + imgIdx) : Hn<VkImage>(0x7777);
  vi.format = VK_FORMAT_B8G8R8A8_UNORM; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageView v{}; H<0>::CreateImageView(kDev, &vi, nullptr, &v);
  VkAttachmentDescription a{}; a.format = VK_FORMAT_B8G8R8A8_UNORM; a.samples = VK_SAMPLE_COUNT_1_BIT; a.loadOp = load; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE; a.initialLayout = initial; a.finalLayout = initial;
  VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}; VkSubpassDescription sp{}; sp.colorAttachmentCount = 1; sp.pColorAttachments = &ref;
  VkImageViewCreateInfo dvi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; dvi.image = Hn<VkImage>(0x7778); dvi.format = VK_FORMAT_D32_SFLOAT; dvi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
  VkImageView dv{}; if (depthLayout >= 0) H<0>::CreateImageView(kDev, &dvi, nullptr, &dv);
  VkAttachmentDescription ads[2] = {a, a}; ads[1].format = VK_FORMAT_D32_SFLOAT; ads[1].initialLayout = ads[1].finalLayout = (VkImageLayout)depthLayout;
  VkImageView vs[2] = {v, dv}; const uint32_t na = depthLayout >= 0 ? 2 : 1;
  VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rci.attachmentCount = na; rci.pAttachments = ads; rci.subpassCount = 1; rci.pSubpasses = &sp;
  VkRenderPass rp{}; H<0>::CreateRenderPass(kDev, &rci, nullptr, &rp);
  VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fci.renderPass = rp; fci.attachmentCount = na; fci.pAttachments = vs; fci.width = 1920; fci.height = 1080; fci.layers = 1;
  VkFramebuffer fb{}; H<0>::CreateFramebuffer(kDev, &fci, nullptr, &fb);
  return {rp, fb, v};
}
static void Begin(VkCommandBuffer cmd, const Pass& p, uint32_t w = 1920, uint32_t h = 1080) {
  VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; bi.renderPass = p.rp; bi.framebuffer = p.fb; bi.renderArea = {{0, 0}, {w, h}};
  H<0>::CmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
}
static void Present(VkSwapchainKHR sc, uint32_t idx) {
  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; pi.swapchainCount = 1; pi.pSwapchains = &sc; pi.pImageIndices = &idx; H<0>::QueuePresentKHR(Hn<VkQueue>(0x40), &pi);
}
static int Count(const char* prefix) { int n = 0; for (auto& e : ev) if (e.rfind(prefix, 0) == 0) ++n; return n; }
static std::string ReadLog() { std::ifstream f(kLog); std::stringstream s; s << f.rdbuf(); return s.str(); }
static bool Has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

int main() {
  // ================= 1. swapchain creation, usage bump, scratch images
  Reset();
  VkSwapchainKHR sc = MakeSwapchain();
  CHECK((lastCreateUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && (lastCreateUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) && (lastCreateUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT));
  CHECK(g_sc.size() == 1 && g_sc[sc]->usable && g_sc[sc]->bumped && g_sc[sc]->slots.size() == 3);
  CHECK(liveScratch == 3 && liveMem == 3);
  CHECK(g_imgRef.size() == 3);
  { uint32_t n = 3; VkImage again[3]; H<0>::GetSwapchainImagesKHR(kDev, sc, &n, again); CHECK(liveScratch == 3); }   // repeated query must not reallocate

  // ================= 2. the transform: exact command sequence on the first qualifying pass
  Pass p0 = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  ev.clear(); Begin(kCmd, p0);
  CHECK(ev.size() == 6);
  if (ev.size() == 6) {
    CHECK(ev[0] == "BARRIER [img0 COLOR_ATTACHMENT_OPTIMAL>TRANSFER_SRC_OPTIMAL] [scratch0 UNDEFINED>TRANSFER_DST_OPTIMAL]");
    CHECK(ev[1] == "COPY img0(TRANSFER_SRC_OPTIMAL)->scratch0(TRANSFER_DST_OPTIMAL)");
    CHECK(ev[2] == "BARRIER [scratch0 TRANSFER_DST_OPTIMAL>TRANSFER_SRC_OPTIMAL] [img0 TRANSFER_SRC_OPTIMAL>TRANSFER_DST_OPTIMAL]");
    CHECK(ev[3] == "BLIT scratch0(TRANSFER_SRC_OPTIMAL)->img0(TRANSFER_DST_OPTIMAL)");
    CHECK(ev[4] == "BARRIER [img0 TRANSFER_DST_OPTIMAL>COLOR_ATTACHMENT_OPTIMAL]");
    CHECK(ev[5] == "REAL_BEGIN_RP");          // the game's own begin comes last, after our commands
  }
  CHECK(blits.size() == 1);
  if (blits.size() == 1) {
    const BlitRec& b = blits[0];
    CHECK(b.s0.x == 46 && b.s0.y == 26 && b.s0.z == 0 && b.s1.x == 1920 - 46 && b.s1.y == 1080 - 26 && b.s1.z == 1);   // centred crop
    CHECK(b.d0.x == 0 && b.d0.y == 0 && b.d1.x == 1920 && b.d1.y == 1080 && b.d1.z == 1);                              // fills the whole backbuffer
    CHECK(b.f == VK_FILTER_LINEAR && b.src != b.dst);                                                                  // distinct images, as vkCmdBlitImage requires
  }

  // ================= 3. once per frame per image: a second qualifying pass is left alone until the image is presented
  ev.clear(); blits.clear(); Begin(kCmd2, p0);
  CHECK(ev.size() == 1 && ev[0] == "REAL_BEGIN_RP" && blits.empty());
  Present(sc, 0); ev.clear(); Begin(kCmd, p0);
  CHECK(Count("BLIT") == 1);                                         // re-armed by present
  nextAcquire = 0; Present(sc, 0); (void)0;
  { uint32_t idx = 0; H<0>::AcquireNextImageKHR(kDev, sc, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, &idx); ev.clear(); Begin(kCmd, p0); CHECK(Count("BLIT") == 1); }  // and by acquire

  // ================= 4. things that must NOT be transformed
  Reset(); sc = MakeSwapchain();
  { Pass clr = MakePass(0, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); ev.clear(); Begin(kCmd, clr); CHECK(ev.size() == 1); }     // CLEAR pass
  { Pass dc = MakePass(0, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); ev.clear(); Begin(kCmd, dc); CHECK(ev.size() == 1); }
  { Pass un = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_UNDEFINED); ev.clear(); Begin(kCmd, un); CHECK(ev.size() == 1); }            // undefined layout
  { Pass ok = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); ev.clear(); Begin(kCmd, ok, 960, 540); CHECK(ev.size() == 1); }  // partial render area
  { Pass other = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false); ev.clear(); Begin(kCmd, other); CHECK(ev.size() == 1); } // not a swapchain image
  { Pass ok = MakePass(1, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_GENERAL); ev.clear(); Begin(kCmd, ok); CHECK(Count("BLIT") == 1 && ev[0] == "BARRIER [img1 GENERAL>TRANSFER_SRC_OPTIMAL] [scratch4 UNDEFINED>TRANSFER_DST_OPTIMAL]" && ev[4] == "BARRIER [img1 TRANSFER_DST_OPTIMAL>GENERAL]"); } // layout is restored to what the pass declared
  { VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; bi.renderPass = Hn<VkRenderPass>(0xdead); bi.framebuffer = Hn<VkFramebuffer>(0xbeef); bi.renderArea = {{0, 0}, {1920, 1080}}; ev.clear(); H<0>::CmdBeginRenderPass(kCmd, &bi, VK_SUBPASS_CONTENTS_INLINE); CHECK(ev.size() == 1); } // unknown handles: pass through
  { H<0>::CmdBeginRenderPass(kCmd, nullptr, VK_SUBPASS_CONTENTS_INLINE); }  // null info must not crash

  // ================= 5. config: disabled, pass index, zoom
  Reset(); sc = MakeSwapchain(); Pass a = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  g_cfg.enabled = false; ev.clear(); Begin(kCmd, a); CHECK(ev.size() == 1); g_cfg.enabled = true; Present(sc, 0);
  g_cfg.passIndex = 2; ev.clear(); Begin(kCmd, a); CHECK(ev.size() == 1);            // 1st qualifying pass: skipped
  ev.clear(); Begin(kCmd2, a); CHECK(Count("BLIT") == 1);                            // 2nd: transformed
  g_cfg.passIndex = 1; Present(sc, 0); g_cfg.zoom = 1.0; ev.clear(); Begin(kCmd, a); CHECK(ev.size() == 1);   // zoom 1.0 = nothing to do
  Present(sc, 0); g_cfg.zoom = 1.10; blits.clear(); Begin(kCmd, a); CHECK(blits.size() == 1 && blits[0].s0.x == ComputeCrop(1920, 1080, 1.10).dx);

  // ================= 6. dynamic rendering
  Reset(); sc = MakeSwapchain(); Pass d = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  VkRenderingAttachmentInfo ra{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; ra.imageView = d.view; ra.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ra.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ra.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO}; ri.renderArea = {{0, 0}, {1920, 1080}}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ra;
  ev.clear(); H<0>::CmdBeginRendering(kCmd, &ri); CHECK(ev.size() == 6 && ev[5] == "REAL_BEGIN_RENDERING" && Count("BLIT") == 1);
  Present(sc, 0); ri.flags = VK_RENDERING_RESUMING_BIT; ev.clear(); H<0>::CmdBeginRendering(kCmd, &ri); CHECK(ev.size() == 1);       // resuming split: never insert transfers
  ri.flags = VK_RENDERING_SUSPENDING_BIT; ev.clear(); H<0>::CmdBeginRendering(kCmd, &ri); CHECK(ev.size() == 1);
  ri.flags = 0; ri.viewMask = 1; ev.clear(); H<0>::CmdBeginRendering(kCmd, &ri); CHECK(ev.size() == 1); ri.viewMask = 0;                  // multiview
  ra.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ev.clear(); H<0>::CmdBeginRendering(kCmd, &ri); CHECK(ev.size() == 1);
  H<0>::CmdBeginRendering(kCmd, nullptr);

  // ================= 7. swapchain creation edge cases
  Reset(); rejectBump = true; sc = MakeSwapchain();                                  // driver refuses the extra usage -> retry with the game's own
  CHECK(g_sc.size() == 1 && !g_sc[sc]->bumped && !g_sc[sc]->usable && g_sc[sc]->slots.empty() && liveScratch == 0);
  { Pass x = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); ev.clear(); Begin(kCmd, x); CHECK(ev.size() == 1); }  // unusable -> game untouched
  Reset(); surfaceSupports = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; sc = MakeSwapchain();   // surface does not offer TRANSFER usage: never even ask
  CHECK(!(lastCreateUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) && !g_sc[sc]->usable);
  Reset(); formatFeatures = 0; sc = MakeSwapchain(); CHECK(!g_sc[sc]->usable && liveScratch == 0 && Has(g_sc[sc]->whyNot, "BLIT"));
  Reset(); sc = MakeSwapchain(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);   // game already asked for it
  CHECK(!g_sc[sc]->bumped && g_sc[sc]->usable);
  Reset(); g_pd = nullptr; sc = MakeSwapchain(); CHECK(!g_sc[sc]->usable);               // no physical device known

  // ================= 9. pause/resume: zoom stays off until stable, then eases in; flicker never zooms
  { Reset(); sc = MakeSwapchain(); g_cfg.engageFrames = 4; g_cfg.rampFrames = 10;
    Pass a = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    auto Frame = [&](bool scene) { blits.clear(); ev.clear(); if (scene) Begin(kCmd, a); Present(sc, 0); };
    int zoomed = 0; for (int i = 0; i < 4; ++i) { Frame(true); zoomed += !blits.empty(); }
    CHECK(zoomed == 0);                                                                  // engage delay: first 4 frames untouched
    Frame(true); CHECK(blits.size() == 1); int dx1 = blits[0].s0.x; CHECK(dx1 >= 1 && dx1 < 46);   // ramp starts small
    for (int i = 0; i < 4; ++i) Frame(true); Frame(true); int dxMid = blits[0].s0.x; CHECK(dxMid > dx1 && dxMid < 46);
    for (int i = 0; i < 10; ++i) Frame(true); CHECK(blits.size() == 1 && blits[0].s0.x == 46);     // fully zoomed
    Frame(false); Frame(true); CHECK(blits.empty());                                     // pause resets the streak: off again
    for (int i = 0; i < 20; ++i) { Frame(i % 2 == 0); CHECK(blits.empty()); }            // flicker never reaches the engage threshold
    g_cfg.engageFrames = 0; g_cfg.rampFrames = 0; }

  // ================= 10. RequireDepthWrite: only the main scene pass (swapchain LOAD + writable depth) is transformed, whatever the recording order
  { Reset(); sc = MakeSwapchain(); g_cfg.requireDepth = true;
    Pass scene = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true, (int)VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    Pass ro = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true, (int)VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    Pass nodepth = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    blits.clear(); Begin(kCmd, ro); Begin(kCmd, nodepth); CHECK(blits.empty()); Begin(kCmd, scene); CHECK(blits.size() == 1);   // scene recorded last: still the one that is zoomed
    Present(sc, 0); blits.clear(); Begin(kCmd, scene); Begin(kCmd, ro); Begin(kCmd, nodepth); CHECK(blits.size() == 1);        // scene recorded first: same result
    g_cfg.requireDepth = false; Present(sc, 0); blits.clear(); Begin(kCmd, nodepth); CHECK(blits.size() == 1); }

  // ================= 8. lifecycle: destroy retires scratch images after 16 presents, never immediately
  Reset(); sc = MakeSwapchain(); VkSwapchainKHR sc2 = MakeSwapchain();
  CHECK(liveScratch == 6);
  H<0>::DestroySwapchainKHR(kDev, sc, nullptr);
  CHECK(g_sc.size() == 1 && liveScratch == 6);                    // still alive: the GPU may be using them
  for (int i = 0; i < 15; ++i) Present(sc2, 0);
  CHECK(liveScratch == 6);
  Present(sc2, 0); CHECK(liveScratch == 3 && liveMem == 3);
  { Pass x = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); (void)x; }
  // recreation: new images for the same swapchain handle replace the old slots
  { uint32_t n = 3; VkImage fresh[3] = {Hn<VkImage>(0x1100), Hn<VkImage>(0x1101), Hn<VkImage>(0x1102)}; OnSwapchainImages(sc2, n, fresh); CHECK(g_sc[sc2]->slots[0].image == fresh[0] && g_imgRef.count(Hn<VkImage>(0x1000)) == 0 && g_imgRef.count(fresh[0]) == 1); }

  // ================= 9. nothing is written to disk until the key is pressed
  Reset(); sc = MakeSwapchain(); Pass c = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  for (int i = 0; i < 20; ++i) { Begin(kCmd, c); Present(sc, 0); }
  CHECK(access(kLog, F_OK) != 0);                                                        // no file at all

  // ================= 10. capture: key press -> exactly 3 frames -> stops; instances separated by timestamps and indents
  g_fakeKeyDown = true; Present(sc, 0);                                                  // press (edge) -> header
  g_fakeKeyDown = true; CHECK(Capturing());
  for (int f = 0; f < 3; ++f) {
    g_fakeTid = 100 + f;
    Begin(kCmd, c);                                                                      // transformed
    Begin(kCmd2, c);                                                                     // second qualifying pass, skipped
    Pass noise = MakePass(2, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); Begin(kCmd, noise);
    { VkImageMemoryBarrier mb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; mb.image = Hn<VkImage>(0x1000); mb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; mb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      P_CmdPipelineBarrier.gp = mRealBarrier; H<0>::CmdPipelineBarrier(kCmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &mb);
      mb.image = Hn<VkImage>(0x7777); H<0>::CmdPipelineBarrier(kCmd, 1, 1, 0, 0, nullptr, 0, nullptr, 1, &mb); }                 // non-swapchain image: not logged
    H<0>::CmdBlitImage(kCmd, Hn<VkImage>(0x7777), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, Hn<VkImage>(0x1000), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, nullptr, VK_FILTER_LINEAR);
    { VkCommandBuffer secs[2] = {Hn<VkCommandBuffer>(0x60), Hn<VkCommandBuffer>(0x61)}; P_CmdExecuteCommands.gp = [](VkCommandBuffer, uint32_t, const VkCommandBuffer*) {}; H<0>::CmdExecuteCommands(kCmd, 2, secs); }
    { VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; VkCommandBuffer cbs[1] = {kCmd}; si.commandBufferCount = 1; si.pCommandBuffers = cbs; P_QueueSubmit.gp = [](VkQueue, uint32_t, const VkSubmitInfo*, VkFence) { return VK_SUCCESS; }; H<0>::QueueSubmit(Hn<VkQueue>(0x40), 1, &si, VK_NULL_HANDLE); }
    { VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; P_BeginCommandBuffer.gp = [](VkCommandBuffer, const VkCommandBufferBeginInfo*) { return VK_SUCCESS; }; H<0>::BeginCommandBuffer(kCmd, &bi); }
    Present(sc, 0); g_fakeKeyDown = true;
  }
  CHECK(!Capturing());                                                                   // stopped by itself after 3 frames
  std::string log = ReadLog();
  { Present(sc, 0); Present(sc, 0); CHECK(ReadLog() == log); }                           // key still held: no new capture, nothing appended
  CHECK(Has(log, "CAPTURE #1 BEGIN") && Has(log, "CAPTURE #1 END"));
  CHECK(Has(log, "frame 1/3") && Has(log, "frame 2/3") && Has(log, "frame 3/3") && !Has(log, "frame 4/3"));
  { std::regex ts(R"(\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}\] CAPTURE #1 BEGIN)"); CHECK(std::regex_search(log, ts)); }
  { std::regex fr(R"(\n    \[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}\] frame \d/3)"); auto it = std::sregex_iterator(log.begin(), log.end(), fr); CHECK(std::distance(it, std::sregex_iterator()) == 3); }   // indented, timestamped
  CHECK(Has(log, "\n        [+") );                                                      // entries are indented one level deeper than the frame header
  CHECK(Has(log, "TRANSFORMED zoom=1.0503x1.0506 crop=46,26 src=1828x1028"));
  CHECK(Has(log, "skip: qualifying pass #2, transform is set to #1"));
  CHECK(Has(log, "skip: loadOp is not LOAD"));
  CHECK(Has(log, "swapchain=Y") && Has(log, "fmt=B8G8R8A8_UNORM(44)") && Has(log, "load=LOAD") && Has(log, "layout=COLOR_ATTACHMENT_OPTIMAL"));
  CHECK(Has(log, "vkCmdPipelineBarrier cmd=0x20 swapchain image[0]=0x1000 layout UNDEFINED -> GENERAL"));
  CHECK(!Has(log, "0x7777 layout"));                                                     // barriers on other images are not logged
  CHECK(Has(log, "vkCmdBlitImage cmd=0x20 WRITES swapchain image[0]=0x1000"));
  CHECK(Has(log, "vkCmdExecuteCommands primary=0x20 secondaries=[0x60 0x61]"));
  CHECK(Has(log, "vkQueueSubmit queue=0x40 batches=1") && Has(log, "cmds=[0x20]"));
  CHECK(Has(log, "vkBeginCommandBuffer cmd=0x20 flags=0x1"));
  CHECK(Has(log, "tid=  100") && Has(log, "tid=  101") && Has(log, "tid=  102"));       // per-thread attribution
  CHECK(Has(log, "swapchains: 1") && Has(log, "usable=yes") && Has(log, "bumped=yes") && Has(log, "usage requested=0x10 actual=0x13"));
  CHECK(Has(log, "CmdBeginRenderPass") && Has(log, "export=not exported") && Has(log, "recent events:") && Has(log, "scratch images allocated"));
  CHECK(Has(log, "hook calls during this capture (proc-address / export):"));
  CHECK(Has(log, "summary:") && Has(log, "transformed=1"));
  g_fakeTid = 4242;

  // ================= 11. a second press makes a second, separate instance; CaptureFrames and log toggles are honoured
  g_cfg.captureFrames = 1; g_cfg.logPasses = false; g_fakeKeyDown = false; Present(sc, 0); g_fakeKeyDown = true; Present(sc, 0);
  Begin(kCmd, c); Present(sc, 0); g_fakeKeyDown = false;
  std::string log2 = ReadLog();
  CHECK(Has(log2, "CAPTURE #2 BEGIN") && Has(log2, "CAPTURE #2 END") && Has(log2, "frame 1/1") && log2.size() > log.size() && log2.compare(0, log.size(), log) == 0);   // appended, earlier text untouched
  { std::string second = log2.substr(log.size()); CHECK(!Has(second, "RenderPass cmd=")); }                                                                   // LogPasses=0 suppresses pass lines

  // ================= 12. the log file is editable/deletable while the game runs
  unlink(kLog); g_cfg.captureFrames = 1; g_cfg.logPasses = true; g_fakeKeyDown = false; Present(sc, 0); g_fakeKeyDown = true; Present(sc, 0); Present(sc, 0);
  { std::string fresh = ReadLog(); CHECK(Has(fresh, "CAPTURE #3 BEGIN") && !Has(fresh, "CAPTURE #1")); }   // deleted file is simply recreated

  std::printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
