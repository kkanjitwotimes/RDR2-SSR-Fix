// Optional zoom + CAS sharpening pass (replaces the bilinear vkCmdBlitImage when Sharpness > 0).
// Everything here uses driver function pointers (never the hooked ones), so it does not recurse into the mod's own hooks.
// Resources are created lazily on the first sharpened frame; any failure leaves the plain blit path in charge.
// Shaders: src/shaders/cas.vert, cas.frag (compiled with `glslangValidator -V`, embedded below as spv.inc).
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace sharpen {
#include "shaders/spv.inc"

struct Fns {
  PFN_vkCreateShaderModule createShader{}; PFN_vkDestroyShaderModule destroyShader{};
  PFN_vkCreateRenderPass createRP{}; PFN_vkDestroyRenderPass destroyRP{};
  PFN_vkCreateFramebuffer createFB{}; PFN_vkDestroyFramebuffer destroyFB{};
  PFN_vkCreateImageView createView{}; PFN_vkDestroyImageView destroyView{};
  PFN_vkCreateSampler createSampler{}; PFN_vkDestroySampler destroySampler{};
  PFN_vkCreateDescriptorSetLayout createDSL{}; PFN_vkDestroyDescriptorSetLayout destroyDSL{};
  PFN_vkCreateDescriptorPool createPool{}; PFN_vkDestroyDescriptorPool destroyPool{};
  PFN_vkAllocateDescriptorSets allocSets{}; PFN_vkUpdateDescriptorSets updateSets{};
  PFN_vkCreatePipelineLayout createPL{}; PFN_vkDestroyPipelineLayout destroyPL{};
  PFN_vkCreateGraphicsPipelines createPipe{}; PFN_vkDestroyPipeline destroyPipe{};
  PFN_vkCmdBeginRenderPass beginRP{}; PFN_vkCmdEndRenderPass endRP{}; PFN_vkCmdBindPipeline bindPipe{};
  PFN_vkCmdBindDescriptorSets bindSets{}; PFN_vkCmdPushConstants push{}; PFN_vkCmdDraw draw{};
  PFN_vkCmdSetViewport setViewport{}; PFN_vkCmdSetScissor setScissor{};
  bool Complete() const {
    return createShader && destroyShader && createRP && destroyRP && createFB && destroyFB && createView && destroyView && createSampler && destroySampler &&
           createDSL && destroyDSL && createPool && destroyPool && allocSets && updateSets && createPL && destroyPL && createPipe && destroyPipe &&
           beginRP && endRP && bindPipe && bindSets && push && draw && setViewport && setScissor;
  }
};
template <class GetProc> inline void LoadFns(VkDevice d, GetProc gp, Fns& f) {
#define L(field, name) f.field = (PFN_vk##name)gp(d, "vk" #name)
  L(createShader, CreateShaderModule); L(destroyShader, DestroyShaderModule); L(createRP, CreateRenderPass); L(destroyRP, DestroyRenderPass);
  L(createFB, CreateFramebuffer); L(destroyFB, DestroyFramebuffer); L(createView, CreateImageView); L(destroyView, DestroyImageView);
  L(createSampler, CreateSampler); L(destroySampler, DestroySampler); L(createDSL, CreateDescriptorSetLayout); L(destroyDSL, DestroyDescriptorSetLayout);
  L(createPool, CreateDescriptorPool); L(destroyPool, DestroyDescriptorPool); L(allocSets, AllocateDescriptorSets); L(updateSets, UpdateDescriptorSets);
  L(createPL, CreatePipelineLayout); L(destroyPL, DestroyPipelineLayout); L(createPipe, CreateGraphicsPipelines); L(destroyPipe, DestroyPipeline);
  L(beginRP, CmdBeginRenderPass); L(endRP, CmdEndRenderPass); L(bindPipe, CmdBindPipeline); L(bindSets, CmdBindDescriptorSets);
  L(push, CmdPushConstants); L(draw, CmdDraw); L(setViewport, CmdSetViewport); L(setScissor, CmdSetScissor);
#undef L
}

struct PC { float rect[4]; float dstInv[2]; float sharp; float pad; };   // matches the fragment shader's push constants (32 bytes)

struct Res {
  std::mutex mu;                 // guards creation only; recording reads the fields once ok == true
  bool tried = false, ok = false; std::string why;
  Fns f; VkDevice dev{};
  VkRenderPass rp{}; VkPipelineLayout pl{}; VkPipeline pipe{}; VkDescriptorSetLayout dsl{}; VkDescriptorPool pool{}; VkSampler samp{};
  std::vector<VkImageView> scratchView, swapView; std::vector<VkFramebuffer> fb; std::vector<VkDescriptorSet> set;
  VkExtent2D ext{};
};

// Frees everything that was created (safe on a partly built Res). Caller must be sure the GPU is done with it.
inline void Destroy(Res& r) {
  if (!r.dev || !r.f.Complete()) { r.ok = false; return; }
  for (auto x : r.fb) if (x) r.f.destroyFB(r.dev, x, nullptr);
  for (auto x : r.scratchView) if (x) r.f.destroyView(r.dev, x, nullptr);
  for (auto x : r.swapView) if (x) r.f.destroyView(r.dev, x, nullptr);
  if (r.pool) r.f.destroyPool(r.dev, r.pool, nullptr);        // frees the sets with it
  if (r.pipe) r.f.destroyPipe(r.dev, r.pipe, nullptr);
  if (r.pl) r.f.destroyPL(r.dev, r.pl, nullptr);
  if (r.dsl) r.f.destroyDSL(r.dev, r.dsl, nullptr);
  if (r.samp) r.f.destroySampler(r.dev, r.samp, nullptr);
  if (r.rp) r.f.destroyRP(r.dev, r.rp, nullptr);
  r.fb.clear(); r.scratchView.clear(); r.swapView.clear(); r.set.clear();
  r.pool = nullptr; r.pipe = nullptr; r.pl = nullptr; r.dsl = nullptr; r.samp = nullptr; r.rp = nullptr; r.ok = false;
}

#define SHARP_FAIL(msg, ...) do { char b_[200]; snprintf(b_, sizeof b_, msg, ##__VA_ARGS__); r.why = b_; Destroy(r); return false; } while (0)

// Builds the render pass, pipeline, sampler and per-image views/framebuffers/descriptor sets. Tries once; the result sticks.
template <class GetProc>
inline bool Init(Res& r, VkDevice dev, GetProc gp, VkFormat fmt, VkExtent2D ext, const std::vector<VkImage>& swapImgs, const std::vector<VkImage>& scratchImgs) {
  std::lock_guard<std::mutex> lk(r.mu);
  if (r.tried) return r.ok;
  r.tried = true; r.dev = dev; r.ext = ext; LoadFns(dev, gp, r.f);
  if (!r.f.Complete()) { r.why = "device functions for the sharpen pass unavailable"; return false; }
  const size_t n = swapImgs.size(); if (!n || scratchImgs.size() != n) { r.why = "no images"; return false; }

  VkAttachmentDescription ad{}; ad.format = fmt; ad.samples = VK_SAMPLE_COUNT_1_BIT;
  ad.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;   // the whole image is rewritten from the scratch copy
  ad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  ad.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ad.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sp{}; sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = 1; sp.pColorAttachments = &ar;
  VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rpi.attachmentCount = 1; rpi.pAttachments = &ad; rpi.subpassCount = 1; rpi.pSubpasses = &sp;
  VkResult vr = r.f.createRP(dev, &rpi, nullptr, &r.rp); if (vr != VK_SUCCESS) SHARP_FAIL("vkCreateRenderPass failed: %d", (int)vr);

  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; si.magFilter = si.minFilter = VK_FILTER_LINEAR; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; si.maxLod = 0.25f; si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  vr = r.f.createSampler(dev, &si, nullptr, &r.samp); if (vr != VK_SUCCESS) SHARP_FAIL("vkCreateSampler failed: %d", (int)vr);

  VkDescriptorSetLayoutBinding lb{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dli.bindingCount = 1; dli.pBindings = &lb;
  vr = r.f.createDSL(dev, &dli, nullptr, &r.dsl); if (vr != VK_SUCCESS) SHARP_FAIL("vkCreateDescriptorSetLayout failed: %d", (int)vr);
  VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, (uint32_t)sizeof(PC)};
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &r.dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
  vr = r.f.createPL(dev, &pli, nullptr, &r.pl); if (vr != VK_SUCCESS) SHARP_FAIL("vkCreatePipelineLayout failed: %d", (int)vr);

  VkShaderModule vs{}, fs{};
  VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  mi.codeSize = sizeof kCasVertSpv; mi.pCode = kCasVertSpv; vr = r.f.createShader(dev, &mi, nullptr, &vs); if (vr != VK_SUCCESS) SHARP_FAIL("vkCreateShaderModule(vert) failed: %d", (int)vr);
  mi.codeSize = sizeof kCasFragSpv; mi.pCode = kCasFragSpv; vr = r.f.createShader(dev, &mi, nullptr, &fs);
  if (vr != VK_SUCCESS) { r.f.destroyShader(dev, vs, nullptr); SHARP_FAIL("vkCreateShaderModule(frag) failed: %d", (int)vr); }
  VkPipelineShaderStageCreateInfo st[2]{}; st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main"; st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vps.viewportCount = 1; vps.scissorCount = 1;   // both dynamic (set in Record, game's values restored afterwards)
  // Every state this pipeline would otherwise bake in (and so overwrite in the game's command buffer) is declared dynamic and left alone.
  const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
  VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dsi.dynamicStateCount = (uint32_t)(sizeof dyn / sizeof dyn[0]); dsi.pDynamicStates = dyn;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = 1; cb.pAttachments = &cba;
  VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; gi.stageCount = 2; gi.pStages = st; gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia;
  gi.pViewportState = &vps; gi.pRasterizationState = &rs; gi.pMultisampleState = &ms; gi.pColorBlendState = &cb; gi.pDynamicState = &dsi; gi.layout = r.pl; gi.renderPass = r.rp; gi.subpass = 0;
  vr = r.f.createPipe(dev, VK_NULL_HANDLE, 1, &gi, nullptr, &r.pipe);
  r.f.destroyShader(dev, vs, nullptr); r.f.destroyShader(dev, fs, nullptr);
  if (vr != VK_SUCCESS) SHARP_FAIL("vkCreateGraphicsPipelines failed: %d", (int)vr);

  VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)n};
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = (uint32_t)n; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
  vr = r.f.createPool(dev, &dpi, nullptr, &r.pool); if (vr != VK_SUCCESS) SHARP_FAIL("vkCreateDescriptorPool failed: %d", (int)vr);
  std::vector<VkDescriptorSetLayout> ls(n, r.dsl); r.set.assign(n, VK_NULL_HANDLE);
  VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = r.pool; dai.descriptorSetCount = (uint32_t)n; dai.pSetLayouts = ls.data();
  vr = r.f.allocSets(dev, &dai, r.set.data()); if (vr != VK_SUCCESS) SHARP_FAIL("vkAllocateDescriptorSets failed: %d", (int)vr);

  r.scratchView.assign(n, VK_NULL_HANDLE); r.swapView.assign(n, VK_NULL_HANDLE); r.fb.assign(n, VK_NULL_HANDLE);
  for (size_t i = 0; i < n; ++i) {
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vc.viewType = VK_IMAGE_VIEW_TYPE_2D; vc.format = fmt;
    vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vc.image = scratchImgs[i]; vr = r.f.createView(dev, &vc, nullptr, &r.scratchView[i]); if (vr != VK_SUCCESS) SHARP_FAIL("vkCreateImageView(scratch) failed: %d", (int)vr);
    vc.image = swapImgs[i]; vr = r.f.createView(dev, &vc, nullptr, &r.swapView[i]); if (vr != VK_SUCCESS) SHARP_FAIL("vkCreateImageView(swapchain) failed: %d", (int)vr);
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fi.renderPass = r.rp; fi.attachmentCount = 1; fi.pAttachments = &r.swapView[i]; fi.width = ext.width; fi.height = ext.height; fi.layers = 1;
    vr = r.f.createFB(dev, &fi, nullptr, &r.fb[i]); if (vr != VK_SUCCESS) SHARP_FAIL("vkCreateFramebuffer failed: %d", (int)vr);
    VkDescriptorImageInfo ii{r.samp, r.scratchView[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = r.set[i]; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
    r.f.updateSets(dev, 1, &w, 0, nullptr);
  }
  r.ok = true; return true;
}
#undef SHARP_FAIL

// Records the pass (must be outside any render pass instance; the scratch image is already SHADER_READ_ONLY, the swapchain image COLOR_ATTACHMENT_OPTIMAL).
inline void Record(const Res& r, VkCommandBuffer cmd, uint32_t idx, int32_t dx, int32_t dy, uint32_t srcW, uint32_t srcH, float sharp) {
  PC pc{}; const float W = (float)r.ext.width, H = (float)r.ext.height;
  pc.rect[0] = dx / W; pc.rect[1] = dy / H; pc.rect[2] = srcW / W; pc.rect[3] = srcH / H; pc.dstInv[0] = 1.f / W; pc.dstInv[1] = 1.f / H; pc.sharp = sharp;
  VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; bi.renderPass = r.rp; bi.framebuffer = r.fb[idx]; bi.renderArea = {{0, 0}, r.ext};
  r.f.beginRP(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
  r.f.bindPipe(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipe);
  r.f.bindSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pl, 0, 1, &r.set[idx], 0, nullptr);
  r.f.push(cmd, r.pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
  const VkViewport vp{0.f, 0.f, W, H, 0.f, 1.f}; const VkRect2D sc{{0, 0}, r.ext};
  r.f.setViewport(cmd, 0, 1, &vp); r.f.setScissor(cmd, 0, 1, &sc);
  r.f.draw(cmd, 3, 1, 0, 0);
  r.f.endRP(cmd);
}

// ---- what the game had bound in a command buffer, so the pass can put it back -------------------------------------------
// The pass overwrites the graphics pipeline, descriptor sets, push constants [0,32) and the dynamic viewport/scissor of the game's command
// buffer. Passes recorded after it may rely on state set before it, so the mod shadows those calls and re-issues them afterwards.
struct Shadow {
  bool havePipe = false; VkPipeline pipe{};
  uint32_t vpMask = 0, scMask = 0; VkViewport vp[4]{}; VkRect2D sc[4]{};
  struct SetCall { uint32_t first = 0, count = 0, dynCount = 0; VkPipelineLayout layout{}; VkDescriptorSet sets[8]{}; uint32_t dyn[16]{}; };
  std::vector<SetCall> sets;
  VkPipelineLayout pushLayout{}; uint32_t pushMask = 0; uint8_t pushData[32]{}; uint32_t pushStage[32]{};

  void OnPipeline(VkPipelineBindPoint bp, VkPipeline p) { if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) { havePipe = true; pipe = p; } }
  void OnViewports(uint32_t first, uint32_t n, const VkViewport* v) { for (uint32_t i = 0; v && i < n && first + i < 4; ++i) { vp[first + i] = v[i]; vpMask |= 1u << (first + i); } }
  void OnScissors(uint32_t first, uint32_t n, const VkRect2D* v) { for (uint32_t i = 0; v && i < n && first + i < 4; ++i) { sc[first + i] = v[i]; scMask |= 1u << (first + i); } }
  void OnPush(VkPipelineLayout l, VkShaderStageFlags st, uint32_t off, uint32_t size, const void* d) {
    if (!d || off >= 32) return; pushLayout = l;
    for (uint32_t b = off; b < off + size && b < 32; ++b) { pushData[b] = ((const uint8_t*)d)[b - off]; pushStage[b] = st; pushMask |= 1u << b; }
  }
  void OnSets(VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet* s, uint32_t dc, const uint32_t* dyn) {
    if (bp != VK_PIPELINE_BIND_POINT_GRAPHICS || !s || !n || n > 8 || dc > 16) return;
    for (size_t i = 0; i < sets.size();) { if (sets[i].first >= first && sets[i].first + sets[i].count <= first + n) sets.erase(sets.begin() + i); else ++i; }   // fully replaced
    if (sets.size() >= 16) sets.erase(sets.begin());
    SetCall c; c.first = first; c.count = n; c.dynCount = dc; c.layout = l;
    for (uint32_t i = 0; i < n; ++i) c.sets[i] = s[i];
    for (uint32_t i = 0; i < dc && dyn; ++i) c.dyn[i] = dyn[i];
    sets.push_back(c);
  }
};

inline void Restore(const Res& r, VkCommandBuffer cmd, const Shadow& s) {
  if (s.havePipe) r.f.bindPipe(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipe);
  for (const Shadow::SetCall& c : s.sets) r.f.bindSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.layout, c.first, c.count, c.sets, c.dynCount, c.dynCount ? c.dyn : nullptr);
  for (uint32_t i = 0; i < 32;) {
    if (!(s.pushMask >> i & 1)) { ++i; continue; }
    uint32_t j = i; while (j < 32 && (s.pushMask >> j & 1) && s.pushStage[j] == s.pushStage[i]) ++j;
    r.f.push(cmd, s.pushLayout, s.pushStage[i], i, j - i, s.pushData + i); i = j;
  }
  for (uint32_t i = 0; i < 4; ++i) { if (s.vpMask >> i & 1) r.f.setViewport(cmd, i, 1, &s.vp[i]); if (s.scMask >> i & 1) r.f.setScissor(cmd, i, 1, &s.sc[i]); }
}
}  // namespace sharpen
