// Pure logic for the RDR2 overscan prototype: no Windows or driver calls, so it can be unit-tested on any host.
#pragma once
#include <vulkan/vulkan_core.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace ovs {

// ---- crop math -------------------------------------------------------------------------------
// Zoom z means the centered 1/z of the image is stretched to fill the whole image.
// Blit offsets are integers, so the zoom actually applied is quantised; zx/zy report it.
struct CropRect { int32_t dx = 0, dy = 0; uint32_t srcW = 0, srcH = 0; double zx = 1.0, zy = 1.0; };

inline CropRect ComputeCrop(uint32_t W, uint32_t H, double zoom) {
  CropRect c; c.srcW = W; c.srcH = H;
  if (!W || !H) return c;
  if (!(zoom > 1.0)) return c;          // also rejects NaN
  if (zoom > 2.0) zoom = 2.0;
  const double inv = 1.0 / zoom;
  int32_t dx = (int32_t)std::lround(W * (1.0 - inv) * 0.5);
  int32_t dy = (int32_t)std::lround(H * (1.0 - inv) * 0.5);
  if ((uint32_t)dx * 2 >= W) dx = (int32_t)((W - 1) / 2);
  if ((uint32_t)dy * 2 >= H) dy = (int32_t)((H - 1) / 2);
  c.dx = dx; c.dy = dy; c.srcW = W - 2 * dx; c.srcH = H - 2 * dy;
  c.zx = (double)W / c.srcW; c.zy = (double)H / c.srcH;
  return c;
}

// ---- pass classification ---------------------------------------------------------------------
enum class Skip {
  None, NotSwapchain, NotColor, NotLoad, Multisampled, LayoutUndefined, AreaMismatch, Layers,
  ViewMask, ResumeSuspend, BadViewRange, UnknownPass,
  // decided by the caller after classification
  Disabled, NoSlot, WrongOrdinal, NoDepthWrite
};

inline const char* SkipName(Skip s) {
  switch (s) {
    case Skip::None: return "qualifies";
    case Skip::NotSwapchain: return "no swapchain attachment";
    case Skip::NotColor: return "attachment not used as color";
    case Skip::NotLoad: return "loadOp is not LOAD";
    case Skip::Multisampled: return "multisampled";
    case Skip::LayoutUndefined: return "layout undefined";
    case Skip::AreaMismatch: return "render area is not the full swapchain extent";
    case Skip::Layers: return "layers != 1";
    case Skip::ViewMask: return "multiview";
    case Skip::ResumeSuspend: return "dynamic rendering suspend/resume";
    case Skip::BadViewRange: return "view is not mip0/layer0 only";
    case Skip::UnknownPass: return "framebuffer or render pass unknown to the hook";
    case Skip::Disabled: return "disabled in INI";
    case Skip::NoSlot: return "no usable scratch slot";
    case Skip::WrongOrdinal: return "not the configured qualifying pass";
    case Skip::NoDepthWrite: return "no writable depth attachment (not the main scene pass)";
  }
  return "?";
}

struct Facts {
  bool isSwapchain = false, isColor = false, viewRangeOk = false, resumeSuspend = false;
  int loadOp = 0, samples = 1, layout = 0;
  int32_t offX = 0, offY = 0;
  uint32_t areaW = 0, areaH = 0, scW = 0, scH = 0, layers = 1, viewMask = 0;
};

inline Skip Classify(const Facts& f) {
  if (!f.isSwapchain) return Skip::NotSwapchain;
  if (!f.isColor) return Skip::NotColor;
  if (f.loadOp != VK_ATTACHMENT_LOAD_OP_LOAD) return Skip::NotLoad;
  if (f.samples != 1) return Skip::Multisampled;
  if (f.layout == VK_IMAGE_LAYOUT_UNDEFINED || f.layout == VK_IMAGE_LAYOUT_PREINITIALIZED) return Skip::LayoutUndefined;
  if (f.offX || f.offY || f.areaW != f.scW || f.areaH != f.scH) return Skip::AreaMismatch;
  if (f.layers != 1) return Skip::Layers;
  if (f.viewMask) return Skip::ViewMask;
  if (f.resumeSuspend) return Skip::ResumeSuspend;
  if (!f.viewRangeOk) return Skip::BadViewRange;
  return Skip::None;
}

// ---- names for the log -----------------------------------------------------------------------
inline const char* LoadOpName(int v) {
  switch (v) { case 0: return "LOAD"; case 1: return "CLEAR"; case 2: return "DONT_CARE"; case 1000400000: return "NONE"; }
  return "?";
}
inline const char* StoreOpName(int v) {
  switch (v) { case 0: return "STORE"; case 1: return "DONT_CARE"; case 1000301000: return "NONE"; }
  return "?";
}
inline const char* LayoutName(int v) {
  switch (v) {
    case 0: return "UNDEFINED"; case 1: return "GENERAL"; case 2: return "COLOR_ATTACHMENT_OPTIMAL";
    case 3: return "DEPTH_STENCIL_ATTACHMENT_OPTIMAL"; case 4: return "DEPTH_STENCIL_READ_ONLY_OPTIMAL";
    case 5: return "SHADER_READ_ONLY_OPTIMAL"; case 6: return "TRANSFER_SRC_OPTIMAL";
    case 7: return "TRANSFER_DST_OPTIMAL"; case 8: return "PREINITIALIZED";
    case 1000001002: return "PRESENT_SRC_KHR"; case 1000314000: return "READ_ONLY_OPTIMAL";
    case 1000314001: return "ATTACHMENT_OPTIMAL";
  }
  return "?";
}
// Same, but unknown values keep their number so a log can be read without a header lookup.
inline std::string LayoutLabel(int v) { const char* n = LayoutName(v); return n[0] == '?' ? "?(" + std::to_string(v) + ")" : std::string(n); }
inline const char* FormatName(int v) {
  switch (v) {
    case 37: return "R8G8B8A8_UNORM"; case 43: return "R8G8B8A8_SRGB"; case 44: return "B8G8R8A8_UNORM";
    case 50: return "B8G8R8A8_SRGB"; case 58: return "A2R10G10B10_UNORM"; case 64: return "A2B10G10R10_UNORM";
    case 83: return "R16G16_SFLOAT"; case 97: return "R16G16B16A16_SFLOAT"; case 122: return "B10G11R11_UFLOAT";
    case 126: return "D32_SFLOAT"; case 129: return "D24_UNORM_S8_UINT"; case 130: return "D32_SFLOAT_S8_UINT";
    case 124: return "D16_UNORM"; case 0: return "UNDEFINED";
  }
  return "?";
}

// ---- log text --------------------------------------------------------------------------------
inline int FormatTimestamp(char* buf, size_t n, int y, int mo, int d, int h, int mi, int s, int ms) {
  return std::snprintf(buf, n, "%04d-%02d-%02d %02d:%02d:%02d.%03d", y, mo, d, h, mi, s, ms);
}
// Indent unit for the capture file: level 0 = capture header, 1 = frame / section, 2 = entries.
inline const char* Indent(int level) {
  static const char* k[] = {"", "    ", "        ", "            "};
  return k[level < 0 ? 0 : level > 3 ? 3 : level];
}

}  // namespace ovs
