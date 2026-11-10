// Native simulation of the real hook code in src/rdr2_overscan.cpp.
// The source is #included whole; Win32/Detours are fakes (tests/shim) and the "driver" is a set of mocks that
// record what the hooks send to it. This checks logic and command ordering, NOT behaviour inside RDR2 or on a GPU.
#include "../src/rdr2_overscan.cpp"
#include <cassert>
#include <cmath>
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
static VkImageUsageFlags lastCreateUsage = 0, lastScratchUsage = 0;
static bool rejectBump = false, sharpMocks = false;
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
static uint32_t lastPresentWaitCount = 0; static VkSemaphore lastPresentWait0 = nullptr;
static VKAPI_ATTR VkResult VKAPI_CALL mPresent(VkQueue, const VkPresentInfoKHR* pi) { lastPresentWaitCount = pi->waitSemaphoreCount; lastPresentWait0 = pi->waitSemaphoreCount ? pi->pWaitSemaphores[0] : nullptr; ev.push_back("REAL_PRESENT"); return VK_SUCCESS; }
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
  lastScratchUsage = ci->usage; assert((ci->usage & ~VK_IMAGE_USAGE_SAMPLED_BIT) == (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)); assert(ci->extent.width == 1920 && ci->extent.height == 1080);
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
// ---- sharpen pass mocks (all driver calls the CAS pass makes)
static int shCreated = 0, shDestroyed = 0; static bool shFailPipe = false;
#define SHNEW(T) { *o = Hn<T>(nextHandle++); ++shCreated; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL sCreateShader(VkDevice, const VkShaderModuleCreateInfo* c, const VkAllocationCallbacks*, VkShaderModule* o) { assert(c->codeSize % 4 == 0 && c->pCode[0] == 0x07230203u); SHNEW(VkShaderModule) }
static VKAPI_ATTR void VKAPI_CALL sDestroyShader(VkDevice, VkShaderModule, const VkAllocationCallbacks*) { ++shDestroyed; }
static VKAPI_ATTR VkResult VKAPI_CALL sCreateRP(VkDevice, const VkRenderPassCreateInfo*, const VkAllocationCallbacks*, VkRenderPass* o) SHNEW(VkRenderPass)
static VKAPI_ATTR void VKAPI_CALL sDestroyRP(VkDevice, VkRenderPass, const VkAllocationCallbacks*) { ++shDestroyed; }
static VKAPI_ATTR VkResult VKAPI_CALL sCreateFB(VkDevice, const VkFramebufferCreateInfo*, const VkAllocationCallbacks*, VkFramebuffer* o) SHNEW(VkFramebuffer)
static VKAPI_ATTR void VKAPI_CALL sDestroyFB(VkDevice, VkFramebuffer, const VkAllocationCallbacks*) { ++shDestroyed; }
static VKAPI_ATTR VkResult VKAPI_CALL sCreateView(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView* o) SHNEW(VkImageView)
static VKAPI_ATTR void VKAPI_CALL sDestroyView(VkDevice, VkImageView, const VkAllocationCallbacks*) { ++shDestroyed; }
static VKAPI_ATTR VkResult VKAPI_CALL sCreateSampler(VkDevice, const VkSamplerCreateInfo*, const VkAllocationCallbacks*, VkSampler* o) SHNEW(VkSampler)
static VKAPI_ATTR void VKAPI_CALL sDestroySampler(VkDevice, VkSampler, const VkAllocationCallbacks*) { ++shDestroyed; }
static VKAPI_ATTR VkResult VKAPI_CALL sCreateDSL(VkDevice, const VkDescriptorSetLayoutCreateInfo*, const VkAllocationCallbacks*, VkDescriptorSetLayout* o) SHNEW(VkDescriptorSetLayout)
static VKAPI_ATTR void VKAPI_CALL sDestroyDSL(VkDevice, VkDescriptorSetLayout, const VkAllocationCallbacks*) { ++shDestroyed; }
static VKAPI_ATTR VkResult VKAPI_CALL sCreatePool(VkDevice, const VkDescriptorPoolCreateInfo*, const VkAllocationCallbacks*, VkDescriptorPool* o) SHNEW(VkDescriptorPool)
static VKAPI_ATTR void VKAPI_CALL sDestroyPool(VkDevice, VkDescriptorPool, const VkAllocationCallbacks*) { ++shDestroyed; }
static VKAPI_ATTR VkResult VKAPI_CALL sAllocSets(VkDevice, const VkDescriptorSetAllocateInfo* a, VkDescriptorSet* o) { for (uint32_t i = 0; i < a->descriptorSetCount; ++i) o[i] = Hn<VkDescriptorSet>(nextHandle++); return VK_SUCCESS; }
static VKAPI_ATTR void VKAPI_CALL sUpdateSets(VkDevice, uint32_t, const VkWriteDescriptorSet*, uint32_t, const VkCopyDescriptorSet*) {}
static VKAPI_ATTR VkResult VKAPI_CALL sCreatePL(VkDevice, const VkPipelineLayoutCreateInfo* c, const VkAllocationCallbacks*, VkPipelineLayout* o) { assert(c->pPushConstantRanges[0].size == 32); SHNEW(VkPipelineLayout) }
static VKAPI_ATTR void VKAPI_CALL sDestroyPL(VkDevice, VkPipelineLayout, const VkAllocationCallbacks*) { ++shDestroyed; }
static VKAPI_ATTR VkResult VKAPI_CALL sCreatePipe(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline* o) { if (shFailPipe) return VK_ERROR_INITIALIZATION_FAILED; SHNEW(VkPipeline) }
static VKAPI_ATTR void VKAPI_CALL sDestroyPipe(VkDevice, VkPipeline, const VkAllocationCallbacks*) { ++shDestroyed; }
static VKAPI_ATTR void VKAPI_CALL sBeginRP(VkCommandBuffer, const VkRenderPassBeginInfo* b, VkSubpassContents) { assert(b->renderArea.extent.width == 1920 && b->renderArea.extent.height == 1080); ev.push_back("SHARP_BEGIN_RP"); }
static VKAPI_ATTR void VKAPI_CALL sEndRP(VkCommandBuffer) { ev.push_back("SHARP_END_RP"); }
static VKAPI_ATTR void VKAPI_CALL sBindPipe(VkCommandBuffer, VkPipelineBindPoint, VkPipeline p) { ev.push_back("SHARP_BIND_PIPE " + std::to_string((uintptr_t)p)); }
static VKAPI_ATTR void VKAPI_CALL sBindSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t first, uint32_t n, const VkDescriptorSet* s, uint32_t dc, const uint32_t* d) { ev.push_back("SHARP_BIND_SET first=" + std::to_string(first) + " n=" + std::to_string(n) + " s0=" + std::to_string((uintptr_t)s[0]) + " dc=" + std::to_string(dc) + " d0=" + std::to_string(dc ? d[0] : 0)); }
static float lastPcRect[4]; static float lastPcSharp;
static VKAPI_ATTR void VKAPI_CALL sPush(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags st, uint32_t off, uint32_t sz, const void* p) { if (sz == 32 && off == 0) { const float* f = (const float*)p; for (int i = 0; i < 4; ++i) lastPcRect[i] = f[i]; lastPcSharp = f[6]; ev.push_back("SHARP_PUSH"); } else ev.push_back("SHARP_RESTORE_PUSH off=" + std::to_string(off) + " sz=" + std::to_string(sz) + " st=" + std::to_string(st) + " b0=" + std::to_string(((const uint8_t*)p)[0])); }
static VKAPI_ATTR void VKAPI_CALL sSetVp(VkCommandBuffer, uint32_t first, uint32_t n, const VkViewport* v) { ev.push_back("SETVP i=" + std::to_string(first) + " h=" + std::to_string((int)v[0].height)); }
static VKAPI_ATTR void VKAPI_CALL sSetSc(VkCommandBuffer, uint32_t first, uint32_t n, const VkRect2D* v) { ev.push_back("SETSC i=" + std::to_string(first) + " w=" + std::to_string(v[0].extent.width)); }
// game-side (hooked) pass-throughs
static VKAPI_ATTR void VKAPI_CALL gBindPipe(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) { ev.push_back("GAME_PIPE"); }
static VKAPI_ATTR void VKAPI_CALL gBindSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*) { ev.push_back("GAME_SETS"); }
static VKAPI_ATTR void VKAPI_CALL gPush(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t, uint32_t, const void*) { ev.push_back("GAME_PUSH"); }
static VKAPI_ATTR void VKAPI_CALL gSetVp(VkCommandBuffer, uint32_t, uint32_t, const VkViewport*) { ev.push_back("GAME_VP"); }
static VKAPI_ATTR void VKAPI_CALL gSetSc(VkCommandBuffer, uint32_t, uint32_t, const VkRect2D*) { ev.push_back("GAME_SC"); }
static VKAPI_ATTR VkResult VKAPI_CALL gBeginCmd(VkCommandBuffer, const VkCommandBufferBeginInfo*) { return VK_SUCCESS; }
static VKAPI_ATTR void VKAPI_CALL sDraw(VkCommandBuffer, uint32_t v, uint32_t, uint32_t, uint32_t) { assert(v == 3); ev.push_back("SHARP_DRAW"); }
// present-time zoom (PauseGapFix) mocks
static int livePools = 0, liveSems = 0, liveCbs = 0; static uint32_t lastSubmitWaits = 0; static VkSemaphore lastSubmitSignal = nullptr; static bool failPool = false;
static VKAPI_ATTR VkResult VKAPI_CALL zCreatePool(VkDevice, const VkCommandPoolCreateInfo* c, const VkAllocationCallbacks*, VkCommandPool* o) { if (failPool) return VK_ERROR_OUT_OF_HOST_MEMORY; assert(c->flags & VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT); *o = Hn<VkCommandPool>(nextHandle++); ++livePools; return VK_SUCCESS; }
static VKAPI_ATTR void VKAPI_CALL zDestroyPool(VkDevice, VkCommandPool, const VkAllocationCallbacks*) { --livePools; }
static VKAPI_ATTR VkResult VKAPI_CALL zAllocCb(VkDevice, const VkCommandBufferAllocateInfo* a, VkCommandBuffer* o) { for (uint32_t i = 0; i < a->commandBufferCount; ++i) o[i] = Hn<VkCommandBuffer>(0x500000 + liveCbs++); return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL zBegin(VkCommandBuffer, const VkCommandBufferBeginInfo*) { ev.push_back("PZ_BEGIN"); return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL zEnd(VkCommandBuffer) { ev.push_back("PZ_END"); return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL zReset(VkCommandBuffer, VkCommandBufferResetFlags) { return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL zCreateSem(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore* o) { *o = Hn<VkSemaphore>(nextHandle++); ++liveSems; return VK_SUCCESS; }
static VKAPI_ATTR void VKAPI_CALL zDestroySem(VkDevice, VkSemaphore, const VkAllocationCallbacks*) { --liveSems; }
static VKAPI_ATTR VkResult VKAPI_CALL zSubmit(VkQueue, uint32_t n, const VkSubmitInfo* s, VkFence) { assert(n == 1 && s->commandBufferCount == 1 && s->signalSemaphoreCount == 1); lastSubmitWaits = s->waitSemaphoreCount; lastSubmitSignal = s->pSignalSemaphores[0];
  for (uint32_t i = 0; i < s->waitSemaphoreCount; ++i) assert(s->pWaitDstStageMask[i] != 0); ev.push_back("PZ_SUBMIT"); return VK_SUCCESS; }
static VKAPI_ATTR void VKAPI_CALL mQFProps(VkPhysicalDevice, uint32_t* n, VkQueueFamilyProperties* p) { if (!p) { *n = 2; return; } p[0] = {}; p[0].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT; if (*n > 1) { p[1] = {}; p[1].queueFlags = VK_QUEUE_TRANSFER_BIT; } }
// diagnostics mocks
static VKAPI_ATTR VkResult VKAPI_CALL mCreateImage(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage* o) { *o = Hn<VkImage>(0x7000); return VK_SUCCESS; }
static VKAPI_ATTR void VKAPI_CALL mDestroyImageG(VkDevice, VkImage, const VkAllocationCallbacks*) {}
static VKAPI_ATTR void VKAPI_CALL mGetQueue(VkDevice, uint32_t, uint32_t, VkQueue* q) { *q = Hn<VkQueue>(0x40); }
static VKAPI_ATTR void VKAPI_CALL mNoopEndRp(VkCommandBuffer) {}
static VKAPI_ATTR void VKAPI_CALL mNoopDraw(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t) {}
static VKAPI_ATTR void VKAPI_CALL mNoopDrawIdx(VkCommandBuffer, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) {}
static VKAPI_ATTR void VKAPI_CALL mNoopUpdate(VkDevice, uint32_t, const VkWriteDescriptorSet*, uint32_t, const VkCopyDescriptorSet*) {}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL mGDPA(VkDevice, const char* n) {
#define R(name, fn) if (!strcmp(n, name)) return (PFN_vkVoidFunction)fn
  R("vkCreateImage", dCreateImage); R("vkDestroyImage", dDestroyImage); R("vkGetImageMemoryRequirements", dGetReq);
  R("vkAllocateMemory", dAlloc); R("vkFreeMemory", dFree); R("vkBindImageMemory", dBind);
  R("vkCmdPipelineBarrier", dBarrier); R("vkCmdCopyImage", dCopy); R("vkCmdBlitImage", dBlit);
  R("vkCreateCommandPool", zCreatePool); R("vkDestroyCommandPool", zDestroyPool); R("vkAllocateCommandBuffers", zAllocCb);
  R("vkBeginCommandBuffer", zBegin); R("vkEndCommandBuffer", zEnd); R("vkResetCommandBuffer", zReset);
  R("vkCreateSemaphore", zCreateSem); R("vkDestroySemaphore", zDestroySem); R("vkQueueSubmit", zSubmit);
  if (sharpMocks) {
    R("vkCreateShaderModule", sCreateShader); R("vkDestroyShaderModule", sDestroyShader); R("vkCreateRenderPass", sCreateRP); R("vkDestroyRenderPass", sDestroyRP);
    R("vkCreateFramebuffer", sCreateFB); R("vkDestroyFramebuffer", sDestroyFB); R("vkCreateImageView", sCreateView); R("vkDestroyImageView", sDestroyView);
    R("vkCreateSampler", sCreateSampler); R("vkDestroySampler", sDestroySampler); R("vkCreateDescriptorSetLayout", sCreateDSL); R("vkDestroyDescriptorSetLayout", sDestroyDSL);
    R("vkCreateDescriptorPool", sCreatePool); R("vkDestroyDescriptorPool", sDestroyPool); R("vkAllocateDescriptorSets", sAllocSets); R("vkUpdateDescriptorSets", sUpdateSets);
    R("vkCreatePipelineLayout", sCreatePL); R("vkDestroyPipelineLayout", sDestroyPL); R("vkCreateGraphicsPipelines", sCreatePipe); R("vkDestroyPipeline", sDestroyPipe);
    R("vkCmdBeginRenderPass", sBeginRP); R("vkCmdEndRenderPass", sEndRP); R("vkCmdBindPipeline", sBindPipe); R("vkCmdBindDescriptorSets", sBindSets);
    R("vkCmdPushConstants", sPush); R("vkCmdDraw", sDraw); R("vkCmdSetViewport", sSetVp); R("vkCmdSetScissor", sSetSc);
  }
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
  { std::lock_guard<std::mutex> l(g_mu); g_queueFam.clear(); g_imgInfo.clear(); } { std::lock_guard<std::mutex> l(g_dmu); g_desc.clear(); }
  g_gapRun = 0; g_hadScene = false; g_frameHist = 0xFFFFFFFFu; g_menuMode = false; g_cfg.menuGuard = false; g_cfg.pauseGap = false; g_cfg.gapMax = 3; g_cfg.logDraws = true; g_cfg.logDesc = 1; g_fnQFProps = mQFProps;
  g_altMode = false; g_frameCand = false; g_frameWritable = false; g_altRun = 0; g_cfg.altPass = false; g_cfg.altEngage = 3;
  g_pdiag.clear(); g_bound.clear(); livePools = liveSems = liveCbs = 0; failPool = false; lastPresentWaitCount = 0; lastPresentWait0 = nullptr;
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
static void Present(VkSwapchainKHR sc, uint32_t idx, uint32_t nwait = 0, const VkSemaphore* waits = nullptr) {
  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; pi.swapchainCount = 1; pi.pSwapchains = &sc; pi.pImageIndices = &idx; pi.waitSemaphoreCount = nwait; pi.pWaitSemaphores = waits; H<0>::QueuePresentKHR(Hn<VkQueue>(0x40), &pi);
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
  { Reset(); sc = MakeSwapchain(); g_cfg.engageFrames = 4; g_cfg.rampFrames = 10; g_cfg.holdZoom = false;   // legacy reset-on-pause path
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
  // ================= 9e. CAS pass: lazy build, command order, layouts, fallback, teardown
  { const VkFormatFeatureFlags saved = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    Pass a = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    auto Setup = [&](bool mocks, bool samplable) { Reset(); sharpMocks = mocks; shCreated = shDestroyed = 0; shFailPipe = false;
      formatFeatures = saved | (samplable ? (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) : 0); g_cfg.sharp = 0.5; sc = MakeSwapchain(); a = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); Present(sc, 0); };
    // sharpness 0 -> plain blit, nothing built
    Setup(true, true); g_cfg.sharp = 0.0; blits.clear(); Begin(kCmd, a); CHECK(blits.size() == 1 && shCreated == 0 && (lastScratchUsage & VK_IMAGE_USAGE_SAMPLED_BIT));
    // sharpness > 0 -> shader pass, no blit
    g_cfg.sharp = 0.5; Present(sc, 0); blits.clear(); ev.clear(); Begin(kCmd, a);
    CHECK(blits.empty() && shCreated > 0 && g_sc[sc]->sharp->ok);
    int ibeg = -1, idraw = -1, iend = -1, icopy = -1, ibar = 0; for (size_t i = 0; i < ev.size(); ++i) { if (ev[i] == "SHARP_BEGIN_RP") ibeg = (int)i; if (ev[i] == "SHARP_DRAW") idraw = (int)i; if (ev[i] == "SHARP_END_RP") iend = (int)i; if (ev[i].rfind("COPY", 0) == 0) icopy = (int)i; if (ev[i].rfind("BARRIER", 0) == 0) ++ibar; }
    CHECK(icopy >= 0 && ibeg > icopy && idraw > ibeg && iend > idraw && ibar == 3 && ev[iend + 1] == "BARRIER" && ev.back() == "REAL_BEGIN_RP");   // copy -> RP -> draw -> end -> final memory barrier
    CHECK(Has(ev[icopy + 1], "TRANSFER_DST_OPTIMAL>SHADER_READ_ONLY_OPTIMAL") && Has(ev[icopy + 1], "img0 TRANSFER_SRC_OPTIMAL>COLOR_ATTACHMENT_OPTIMAL"));
    CHECK(lastPcSharp == 0.5f && std::fabs(lastPcRect[0] - 46.0 / 1920) < 1e-6 && std::fabs(lastPcRect[2] - 1828.0 / 1920) < 1e-6 && std::fabs(lastPcRect[3] - 1028.0 / 1080) < 1e-6);
    int built = shCreated; Present(sc, 0); ev.clear(); Begin(kCmd, a); CHECK(shCreated == built);                         // built once
    // pass layout other than COLOR_ATTACHMENT_OPTIMAL -> plain blit
    Pass ro = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_GENERAL); Present(sc, 0); blits.clear(); Begin(kCmd, ro); CHECK(blits.size() == 1);
    // retire: sharpen resources are freed together with the scratch images
    shDestroyed = 0; H<0>::DestroySwapchainKHR(kDev, sc, nullptr); for (int i = 0; i < 20; ++i) { VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; H<0>::QueuePresentKHR(nullptr, &pi); }
    CHECK(shDestroyed >= built - 3 && liveScratch == 0);
    // pipeline creation fails -> permanent fallback to the blit, partial objects cleaned up
    Setup(true, true); shFailPipe = true; blits.clear(); Begin(kCmd, a); CHECK(blits.size() == 1 && !g_sc[sc]->sharp->ok && g_sc[sc]->sharp->tried && shDestroyed > 0 && Has(g_sc[sc]->sharp->why, "vkCreateGraphicsPipelines"));
    shFailPipe = false; Present(sc, 0); blits.clear(); Begin(kCmd, a); CHECK(blits.size() == 1);                       // does not retry every frame
    // format without SAMPLED/COLOR_ATTACHMENT -> sharpening unsupported, blit
    Setup(true, false); blits.clear(); Begin(kCmd, a); CHECK(blits.size() == 1 && !g_sc[sc]->sharpPossible);
    // driver lacks the functions -> blit
    Setup(false, true); blits.clear(); Begin(kCmd, a); CHECK(blits.size() == 1 && !g_sc[sc]->sharp->ok);
    sharpMocks = false; g_cfg.sharp = 0.0; formatFeatures = saved; }
  // ================= 9f. the pass puts the game's pipeline / descriptor sets / push constants / viewport back afterwards
  { Reset(); sharpMocks = true; shCreated = shDestroyed = 0; formatFeatures |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT; g_cfg.sharp = 0.5;
    sc = MakeSwapchain(); Pass a = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); Present(sc, 0);
    P_CmdBindPipeline.gp = gBindPipe; P_CmdBindDescriptorSets.gp = gBindSets; P_CmdPushConstants.gp = gPush; P_CmdSetViewport.gp = gSetVp; P_CmdSetScissor.gp = gSetSc; P_BeginCommandBuffer.gp = gBeginCmd;
    const VkPipelineLayout lay = Hn<VkPipelineLayout>(0x66); const VkDescriptorSet ds[2] = {Hn<VkDescriptorSet>(0x71), Hn<VkDescriptorSet>(0x72)}; const uint32_t dynOff = 256, pc[4] = {7, 2, 3, 4};
    const VkViewport gv{0.f, 1080.f, 1920.f, -1080.f, 0.f, 1.f}; const VkRect2D gs{{0, 0}, {1920, 1080}};
    ev.clear(); H<0>::BeginCommandBuffer(kCmd, nullptr);
    H<0>::CmdBindPipeline(kCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, Hn<VkPipeline>(0x777)); H<0>::CmdBindPipeline(kCmd, VK_PIPELINE_BIND_POINT_COMPUTE, Hn<VkPipeline>(0x888));
    H<0>::CmdBindDescriptorSets(kCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lay, 1, 2, ds, 1, &dynOff);
    H<0>::CmdPushConstants(kCmd, lay, VK_SHADER_STAGE_VERTEX_BIT, 4, 16, pc); H<0>::CmdSetViewport(kCmd, 0, 1, &gv); H<0>::CmdSetScissor(kCmd, 0, 1, &gs);
    CHECK(Count("GAME_") == 6);                                                              // every call still reaches the driver
    ev.clear(); Begin(kCmd, a);
    size_t e = 0; while (e < ev.size() && ev[e] != "SHARP_END_RP") ++e; CHECK(e < ev.size());
    std::vector<std::string> after(ev.begin() + (e < ev.size() ? e + 1 : 0), ev.end());
    CHECK(after.size() >= 7 && after[0] == "SHARP_BIND_PIPE 1911");                           // 0x777: the graphics pipeline (compute bind ignored)
    CHECK(after.size() >= 7 && after[1] == "SHARP_BIND_SET first=1 n=2 s0=113 dc=1 d0=256");
    CHECK(after.size() >= 7 && after[2] == "SHARP_RESTORE_PUSH off=4 sz=16 st=1 b0=7");
    CHECK(after.size() >= 7 && after[3] == "SETVP i=0 h=-1080" && after[4] == "SETSC i=0 w=1920");
    CHECK(after.size() >= 6 && after[5] == "BARRIER");                                          // restore comes before the final memory barrier
    bool ourVp = false; for (size_t i = 0; i < e; ++i) if (ev[i] == "SETVP i=0 h=1080") ourVp = true; CHECK(ourVp);   // our own pass used a positive-height viewport
    // a new recording starts from nothing: no restore calls
    Present(sc, 0); ev.clear(); H<0>::BeginCommandBuffer(kCmd, nullptr); Begin(kCmd, a);
    e = 0; while (e < ev.size() && ev[e] != "SHARP_END_RP") ++e; CHECK(e < ev.size() && ev[e + 1] == "BARRIER");
    // Sharpness 0 -> nothing is shadowed at all
    g_cfg.sharp = 0.0; g_cs.clear(); H<0>::CmdBindPipeline(kCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, Hn<VkPipeline>(0x777)); CHECK(g_cs.empty());
    sharpMocks = false; formatFeatures = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT; }
  // ================= 9b. HoldZoomOnPause: pausing keeps the zoom engaged, no re-ramp while paused
  { Reset(); sc = MakeSwapchain(); g_cfg.engageFrames = 4; g_cfg.rampFrames = 10; g_cfg.holdZoom = true;
    Pass a = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    auto Frame = [&](bool scene) { blits.clear(); ev.clear(); if (scene) Begin(kCmd, a); Present(sc, 0); };
    for (int i = 0; i < 20; ++i) Frame(true); CHECK(blits.size() == 1 && blits[0].s0.x == 46);
    Frame(false); Frame(false); Frame(true); CHECK(blits.size() == 1 && blits[0].s0.x == 46);      // paused background: full zoom at once
    for (int i = 0; i < 5; ++i) { Frame(i % 2 == 0); if (i % 2 == 0) CHECK(blits.size() == 1 && blits[0].s0.x == 46); }
    g_cfg.holdZoom = true; g_cfg.engageFrames = 0; g_cfg.rampFrames = 0; }

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

  // ================= 13. PauseGapFix: scene-less frames right after scene frames are zoomed at present time
  { const VkQueue q = Hn<VkQueue>(0x40); const VkSemaphore gw[2] = {Hn<VkSemaphore>(0xA1), Hn<VkSemaphore>(0xA2)};
    auto Setup = [&]() { Reset(); g_cfg.pauseGap = true; { std::lock_guard<std::mutex> l(g_mu); g_queueFam[q] = 0; } sc = MakeSwapchain(); };
    Pass a = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);   // (handles are fixtures; Reset clears tracking, so rebuild below)
    Setup(); a = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    auto Frame = [&](bool scene) { blits.clear(); ev.clear(); if (scene) Begin(kCmd, a); Present(sc, 0, 2, gw); };
    Frame(true); CHECK(Count("PZ_SUBMIT") == 0 && lastPresentWaitCount == 2 && lastPresentWait0 == gw[0]);         // scene frame: the pass zooms, present untouched
    Frame(false);                                                                                                   // pause-transition frame 1
    CHECK(Count("PZ_SUBMIT") == 1 && g_sc[sc]->pz.ok && livePools == 1 && liveSems == 3 && liveCbs == 3);
    { size_t ib = 0; while (ib < ev.size() && ev[ib] != "PZ_BEGIN") ++ib; CHECK(ib + 8 < ev.size() + 0);
      if (ib + 8 < ev.size()) {
        const std::string scr = imgName(g_sc[sc]->slots[0].scratch);
        CHECK(ev[ib + 1] == "BARRIER [img0 PRESENT_SRC_KHR>TRANSFER_SRC_OPTIMAL] [" + scr + " UNDEFINED>TRANSFER_DST_OPTIMAL]");   // the presented layout is PRESENT_SRC
        CHECK(ev[ib + 2] == "COPY img0(TRANSFER_SRC_OPTIMAL)->" + scr + "(TRANSFER_DST_OPTIMAL)");
        CHECK(ev[ib + 4] == "BLIT " + scr + "(TRANSFER_SRC_OPTIMAL)->img0(TRANSFER_DST_OPTIMAL)");
        CHECK(ev[ib + 5] == "BARRIER [img0 TRANSFER_DST_OPTIMAL>PRESENT_SRC_KHR]" && ev[ib + 6] == "PZ_END" && ev[ib + 7] == "PZ_SUBMIT" && ev[ib + 8] == "REAL_PRESENT");   // layout restored, submit before present
      } }
    CHECK(blits.size() == 1 && blits[0].s0.x == 46 && blits[0].s0.y == 26 && blits[0].d1.x == 1920);
    CHECK(lastSubmitWaits == 2 && lastPresentWaitCount == 1 && lastPresentWait0 == lastSubmitSignal);                // our submit waits for the game's semaphores; present waits for ours
    Frame(false); CHECK(Count("PZ_SUBMIT") == 1 && livePools == 1);                                                  // frame 2 of the gap: reuses the resources
    Frame(false); CHECK(Count("PZ_SUBMIT") == 1);                                                                    // 3rd: still within PauseGapMaxFrames=3
    Frame(false); CHECK(Count("PZ_SUBMIT") == 0 && lastPresentWaitCount == 2 && lastPresentWait0 == gw[0]);         // 4th: gap too long -> untouched, original semaphores
    Frame(true); Frame(false); CHECK(Count("PZ_SUBMIT") == 1);                                                       // a scene frame re-arms it
    // zero wait semaphores is fine too
    ev.clear(); Present(sc, 0); CHECK(Count("PZ_SUBMIT") == 1 && lastSubmitWaits == 0);
    // first pause after launch: ~11 scene-less frames in a row; with the v0.1.1 default (30) all are zoomed, frame 31 is not
    g_cfg.gapMax = 30; Frame(true); for (int i = 1; i <= 30; ++i) { Frame(false); CHECK(Count("PZ_SUBMIT") == 1); } Frame(false); CHECK(Count("PZ_SUBMIT") == 0); g_cfg.gapMax = 3;
    // gates
    Setup(); a = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    ev.clear(); Present(sc, 0); Present(sc, 0); CHECK(Count("PZ_SUBMIT") == 0);                                      // never saw a scene frame (startup / menus)
    g_cfg.pauseGap = false; Begin(kCmd, a); Present(sc, 0); ev.clear(); Present(sc, 0); CHECK(Count("PZ_SUBMIT") == 0); g_cfg.pauseGap = true;   // switched off
    g_cfg.enabled = false; Begin(kCmd, a); Present(sc, 0); ev.clear(); Present(sc, 0); CHECK(Count("PZ_SUBMIT") == 0); g_cfg.enabled = true;
    Setup(); a = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); g_cfg.engageFrames = 4;
    Begin(kCmd, a); Present(sc, 0); ev.clear(); Present(sc, 0); CHECK(Count("PZ_SUBMIT") == 0); g_cfg.engageFrames = 0;   // zoom not engaged yet -> nothing to do
    // unusable setups fall back to the untouched present and say why
    Setup(); a = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); { std::lock_guard<std::mutex> l(g_mu); g_queueFam.clear(); }
    Begin(kCmd, a); Present(sc, 0); ev.clear(); Present(sc, 0); CHECK(Count("PZ_SUBMIT") == 0 && Count("REAL_PRESENT") == 1 && g_sc[sc]->pz.tried && !g_sc[sc]->pz.ok && Has(g_sc[sc]->pz.why, "queue unknown"));
    Setup(); a = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); { std::lock_guard<std::mutex> l(g_mu); g_queueFam[q] = 1; }
    Begin(kCmd, a); Present(sc, 0); ev.clear(); Present(sc, 0); CHECK(Count("PZ_SUBMIT") == 0 && Has(g_sc[sc]->pz.why, "GRAPHICS"));
    Setup(); a = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); failPool = true;
    Begin(kCmd, a); Present(sc, 0); ev.clear(); Present(sc, 0); CHECK(Count("PZ_SUBMIT") == 0 && Has(g_sc[sc]->pz.why, "vkCreateCommandPool") && livePools == 0); Present(sc, 0); CHECK(g_sc[sc]->pz.tried);   // permanent, no retry storm
    // multi-swapchain presents are left alone
    Setup(); a = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); Begin(kCmd, a); Present(sc, 0);
    { VkSwapchainKHR two[2] = {sc, sc}; uint32_t ix[2] = {0, 0}; VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; pi.swapchainCount = 2; pi.pSwapchains = two; pi.pImageIndices = ix; ev.clear(); H<0>::QueuePresentKHR(q, &pi); CHECK(Count("PZ_SUBMIT") == 0); }
    // teardown: pool, command buffers and semaphores are released with the retired scratch images
    Setup(); a = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); Begin(kCmd, a); Present(sc, 0); Present(sc, 0); CHECK(livePools == 1 && liveSems == 3);
    H<0>::DestroySwapchainKHR(kDev, sc, nullptr); CHECK(livePools == 1);                                              // still alive: the GPU may be using them
    for (int i = 0; i < 20; ++i) Present(Hn<VkSwapchainKHR>(0xdead), 0); CHECK(livePools == 0 && liveSems == 0 && liveScratch == 0);
    // swapchain recreation (new images) rebuilds the resources
    Setup(); a = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); Begin(kCmd, a); Present(sc, 0); Present(sc, 0); CHECK(g_sc[sc]->pz.ok);
    { VkImage fresh[3] = {Hn<VkImage>(0x1100), Hn<VkImage>(0x1101), Hn<VkImage>(0x1102)}; OnSwapchainImages(sc, 3, fresh); CHECK(!g_sc[sc]->pz.tried && g_sc[sc]->pz.cb.empty() && livePools == 1); }
  }

  // ================= 14. queue tracking and image tracking hooks
  { Reset(); P_GetDeviceQueue.gp = mGetQueue; P_CreateImage.gp = mCreateImage; P_DestroyImage.gp = mDestroyImageG;
    VkQueue got{}; H<0>::GetDeviceQueue(kDev, 3, 0, &got); CHECK(g_queueFam.count(got) == 1 && g_queueFam[got] == 3);
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ici.extent = {1280, 720, 1}; ici.format = VK_FORMAT_R16G16B16A16_SFLOAT; ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT; ici.arrayLayers = 1; ici.mipLevels = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT;
    VkImage im{}; H<0>::CreateImage(kDev, &ici, nullptr, &im); CHECK(g_imgInfo.count(im) == 1 && g_imgInfo[im].w == 1280 && g_imgInfo[im].h == 720);
    H<0>::DestroyImage(kDev, im, nullptr); CHECK(g_imgInfo.count(im) == 0); }

  // ================= 15. capture diagnostics: draws, bound sets and what they sample
  { Reset(); g_diagSeq = 0; P_CreateImage.gp = mCreateImage; P_CmdDraw.gp = mNoopDraw; P_CmdDrawIndexed.gp = mNoopDrawIdx; P_CmdEndRenderPass.gp = mNoopEndRp; P_UpdateDescriptorSets.gp = mNoopUpdate;
    P_CmdBindDescriptorSets.gp = gBindSets; P_BeginCommandBuffer.gp = gBeginCmd; g_cfg.captureFrames = 1;
    sc = MakeSwapchain(); Pass a = MakePass(0, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ici.extent = {1920, 1080, 1}; ici.format = VK_FORMAT_R16G16B16A16_SFLOAT; ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT; ici.arrayLayers = 1; ici.mipLevels = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT;
    VkImage off{}; H<0>::CreateImage(kDev, &ici, nullptr, &off);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = off; vi.format = ici.format; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}; VkImageView offView{}; H<0>::CreateImageView(kDev, &vi, nullptr, &offView);
    vi.image = Hn<VkImage>(0x1001); vi.format = VK_FORMAT_B8G8R8A8_UNORM; VkImageView scView{}; H<0>::CreateImageView(kDev, &vi, nullptr, &scView);   // a view of swapchain image 1
    const VkDescriptorSet set1 = Hn<VkDescriptorSet>(0x71), set2 = Hn<VkDescriptorSet>(0x72), set3 = Hn<VkDescriptorSet>(0x73); const VkPipelineLayout lay = Hn<VkPipelineLayout>(0x66);
    g_fakeKeyDown = true; Present(sc, 0); CHECK(Capturing());                                                       // press -> capture starts
    H<0>::BeginCommandBuffer(kCmd, nullptr);
    VkDescriptorImageInfo di[2] = {{nullptr, offView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {nullptr, scView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    w[0].dstSet = set1; w[0].dstBinding = 2; w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &di[0];
    w[1].dstSet = set2; w[1].dstBinding = 0; w[1].descriptorCount = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; w[1].pImageInfo = &di[1];
    H<0>::UpdateDescriptorSets(kDev, 2, w, 0, nullptr);
    H<0>::CmdBindDescriptorSets(kCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lay, 0, 1, &set1, 0, nullptr);                // bound before the pass: must still show up
    Begin(kCmd, a);
    const VkDescriptorSet inPass[2] = {set2, set3}; H<0>::CmdBindDescriptorSets(kCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lay, 1, 2, inPass, 0, nullptr);   // bound inside the pass
    H<0>::CmdDraw(kCmd, 3, 1, 0, 0); H<0>::CmdDrawIndexed(kCmd, 6, 2, 0, 0, 0); H<0>::CmdDraw(kCmd, 4, 1, 0, 0);
    H<0>::CmdEndRenderPass(kCmd);
    Present(sc, 0); g_fakeKeyDown = false;
    std::string lg = ReadLog();
    CHECK(Has(lg, "[pass#1]") && Has(lg, "pass#1 END cmd=0x20 draws=2 drawsIndexed=1 drawsIndirect=0"));
    CHECK(Has(lg, "firstDraws=[draw count=3 instances=1; drawIndexed count=6 instances=2; draw count=4 instances=1]") && Has(lg, "descriptorSetsSeen=3"));
    CHECK(Has(lg, "pass#1 samples set 0x71 binding 2[0] -> image 0x7000 1920x1080 fmt=R16G16B16A16_SFLOAT(97) RT"));      // offscreen render target
    CHECK(Has(lg, "pass#1 samples set 0x72 binding 0[0] -> SWAPCHAIN image[1]"));                                          // sampling a swapchain image
    CHECK(Has(lg, "sets with no known image descriptors") && Has(lg, "0x73"));                                             // set 3 was never written
    CHECK(Has(lg, "config v0.1.1: pauseGapFix=0") && Has(lg, "gapFix=idle") && Has(lg, "present #") && Has(lg, "PauseGapFix off"));
    // outside a capture the draw hooks record nothing and descriptor writes are not tracked (LogDescriptorImages=1)
    g_pdiag.clear(); { std::lock_guard<std::mutex> l(g_dmu); g_desc.clear(); }
    H<0>::CmdDraw(kCmd, 3, 1, 0, 0); H<0>::UpdateDescriptorSets(kDev, 2, w, 0, nullptr); CHECK(g_pdiag.empty() && g_desc.empty());
    g_cfg.logDesc = 2; H<0>::UpdateDescriptorSets(kDev, 2, w, 0, nullptr); CHECK(g_desc.size() == 2); g_cfg.logDesc = 1;     // 2 = always track
    // LogDrawDiagnostics=0 -> no pass ids at all
    g_cfg.logDraws = false; g_fakeKeyDown = true; Present(sc, 0); Begin(kCmd, a); Present(sc, 0); g_fakeKeyDown = false; CHECK(!Has(ReadLog().substr(lg.size()), "pass#")); g_cfg.logDraws = true;
    // a pass that does not touch the swapchain gets no diagnostics
    { Pass off2 = MakePass(0, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false); g_fakeKeyDown = false; Present(sc, 0); g_fakeKeyDown = true; Present(sc, 0); Begin(kCmd, off2); H<0>::CmdEndRenderPass(kCmd); CHECK(g_pdiag.empty()); g_fakeKeyDown = false; } }

  // ================= 16. MenuCadenceGuard: title menu alternates scene / scene-less frames -> zoom suspended, no per-frame UI size flip
  { const VkQueue q = Hn<VkQueue>(0x40);
    auto Setup = [&]() { Reset(); g_cfg.pauseGap = true; g_cfg.menuGuard = true; g_cfg.engageFrames = 2; { std::lock_guard<std::mutex> l(g_mu); g_queueFam[q] = 0; } sc = MakeSwapchain(); };
    Setup(); Pass a = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    auto Frame = [&](bool scene) { blits.clear(); ev.clear(); if (scene) Begin(kCmd, a); Present(sc, 0); };
    for (int i = 0; i < 6; ++i) Frame(true);                                   // gameplay: zoom engaged
    Frame(true); CHECK(blits.size() == 1);
    Frame(false); CHECK(Count("PZ_SUBMIT") == 1 && !g_menuMode);               // first scene-less frame: could be a pause gap -> zoomed
    Frame(true); CHECK(blits.size() == 1 && !g_menuMode);
    Frame(false); CHECK(g_menuMode && Count("PZ_SUBMIT") == 0);                 // S G S G = alternating: guard on, this frame is left alone
    Frame(true); CHECK(blits.empty() && Count("PZ_SUBMIT") == 0);               // scene pass no longer zooms
    for (int i = 0; i < 6; ++i) { Frame(false); CHECK(Count("PZ_SUBMIT") == 0 && blits.empty()); Frame(true); CHECK(blits.empty()); }   // steady alternation: nothing is zoomed, UI stays put
    CHECK(g_stableStreak == 0);                                                 // ramp restarts when gameplay resumes
    for (int i = 0; i < 10; ++i) { Frame(true); CHECK(blits.empty() && g_menuMode); }    // 11 scene frames in a row (counting the last alternation frame): still guarded
    Frame(true); CHECK(!g_menuMode);                                            // 12 in a row: guard off
    Frame(true); Frame(true); Frame(true); CHECK(blits.size() == 1);            // engage delay (2 frames), then the zoom is back
    // a plain pause gap (2 scene-less frames after gameplay) is NOT mistaken for the menu
    Setup(); a = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    for (int i = 0; i < 20; ++i) Frame(true);
    Frame(false); CHECK(Count("PZ_SUBMIT") == 1); Frame(false); CHECK(Count("PZ_SUBMIT") == 1 && !g_menuMode); Frame(true); CHECK(blits.size() == 1 && !g_menuMode);
    // guard switched off: old behaviour even for alternation
    g_cfg.menuGuard = false; for (int i = 0; i < 6; ++i) { Frame(i % 2 == 0); } CHECK(!g_menuMode);
    // the capture log says so
    Setup(); a = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); g_cfg.captureFrames = 8; for (int i = 0; i < 4; ++i) Frame(true);
    g_fakeKeyDown = true; Present(sc, 0); for (int i = 0; i < 8; ++i) { Frame(i % 2 == 1); g_fakeKeyDown = true; } g_fakeKeyDown = false;
    { std::string lg = ReadLog(); CHECK(Has(lg, "menu cadence guard active: zoom suspended") && Has(lg, "skip: menu cadence guard (scene / scene-less frames alternate), zoom suspended") && Has(lg, "menuGuard=1(active=")); } }

  // ================= 17. alt pass mode (startup / NPC focus camera: no depth-writing scene pass, only a read-only-depth swapchain pass)
  { const VkImageLayout C = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    Pass wr{}, ro{}, clr{};
    auto Setup = [&]() { Reset(); g_cfg.requireDepth = true; g_cfg.altPass = true; sc = MakeSwapchain();   // Reset() forgets all views / framebuffers: rebuild the passes
      wr = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, C, true, (int)VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
      ro = MakePass(0, VK_ATTACHMENT_LOAD_OP_LOAD, C, true, 1000117000);
      clr = MakePass(0, VK_ATTACHMENT_LOAD_OP_CLEAR, C); };
    Setup();
    auto Alt = [&]() { blits.clear(); ev.clear(); Begin(kCmd, clr); Begin(kCmd, ro); Present(sc, 0); };      // alt frame: CLEAR + read-only-depth LOAD
    auto Norm = [&]() { blits.clear(); ev.clear(); Begin(kCmd, clr); Begin(kCmd, ro); Begin(kCmd, wr); Present(sc, 0); };  // normal frame (ro recorded first)
    Norm(); Norm(); CHECK(!g_altMode && g_altRun == 0);                    // normal gameplay never arms alt mode
    Alt(); CHECK(blits.empty() && g_altRun == 1);                          // 1st alt frame: not engaged yet
    Alt(); CHECK(blits.empty() && !g_altMode);
    Alt(); CHECK(blits.empty() && g_altMode);                               // 3rd frame ends -> mode on
    Alt(); CHECK(blits.size() == 1 && g_altMode);                           // 4th frame: the read-only-depth pass is zoomed
    Alt(); CHECK(blits.size() == 1);
    CHECK(g_stableStreak > 0);                                              // counted as scene frames (engage / ramp / gap logic)
    Norm(); CHECK(blits.size() == 1 && !g_altMode && g_altRun == 0);        // depth-writing pass is back: exactly one zoom on the transition frame, mode off
    Norm(); CHECK(blits.size() == 1 && !g_altMode);
    // double-zoom guard: writable pass recorded first, ro pass after it, while alt mode is on
    Setup(); for (int i = 0; i < 3; ++i) Alt(); CHECK(g_altMode);
    blits.clear(); Begin(kCmd, wr); Begin(kCmd, ro); CHECK(blits.size() == 1); Present(sc, 0); CHECK(!g_altMode);
    // INI switch off: nothing changes (old behaviour)
    Setup(); g_cfg.altPass = false; for (int i = 0; i < 6; ++i) { Alt(); CHECK(blits.empty() && !g_altMode); }
    // longer engage threshold; requireDepth=0 users are unaffected
    Setup(); g_cfg.altEngage = 5; for (int i = 0; i < 4; ++i) Alt(); CHECK(!g_altMode); Alt(); CHECK(g_altMode);
    // interaction with the pause gap fix: scene-less ro-only frames are covered at present until alt mode takes over
    Setup(); g_cfg.pauseGap = true; g_cfg.engageFrames = 0; Norm(); Norm(); lastPresentWaitCount = 0;
    { std::lock_guard<std::mutex> l(g_mu); g_queueFam[Hn<VkQueue>(0x40)] = 0; }
    Alt(); Alt(); Alt(); Alt(); CHECK(g_altMode && g_gapRun == 0 && blits.size() == 1);   // 4th alt frame transformed by the pass, gap run reset
    // capture log shows the mode
    Setup(); g_cfg.captureFrames = 5; for (int i = 0; i < 3; ++i) Alt(); g_fakeKeyDown = true; Present(sc, 0); for (int i = 0; i < 5; ++i) { Alt(); g_fakeKeyDown = true; } g_fakeKeyDown = false;
    { std::string lg = ReadLog(); CHECK(Has(lg, "alt pass mode: enabled=1") && Has(lg, "TRANSFORMED (alt pass: read-only depth)") && Has(lg, "alt=1(run=")); } }

  std::printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
