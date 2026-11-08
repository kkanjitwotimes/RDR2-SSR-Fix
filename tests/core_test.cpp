// Native (Linux/any host) tests for the pure logic in overscan_core.h.
#include "overscan_core.h"
#include <cassert>
#include <cstdio>
#include <string>
using namespace ovs;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static Facts Good() {
  Facts f; f.isSwapchain = f.isColor = f.viewRangeOk = true; f.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; f.samples = 1;
  f.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; f.areaW = f.scW = 1920; f.areaH = f.scH = 1080; f.layers = 1; return f;
}

int main() {
  // ---- crop math: the user's setup, 1920x1080 at 1.05 ----
  CropRect c = ComputeCrop(1920, 1080, 1.05);
  CHECK(c.dx == 46 && c.dy == 26);                      // 1920*(1-1/1.05)/2 = 45.71 ; 1080*... = 25.71
  CHECK(c.srcW == 1828 && c.srcH == 1028);
  CHECK(std::fabs(c.zx - 1.0503) < 0.0005 && std::fabs(c.zy - 1.0506) < 0.0005);
  CHECK(std::fabs(c.zx - 1.05) < 0.002 && std::fabs(c.zy - 1.05) < 0.002);   // quantisation stays small
  // source rect is centred: equal margin both sides
  CHECK(c.dx + (int)c.srcW + c.dx == 1920 && c.dy + (int)c.srcH + c.dy == 1080);
  // no zoom / invalid zoom = no crop
  CHECK(ComputeCrop(1920, 1080, 1.0).dx == 0 && ComputeCrop(1920, 1080, 1.0).srcW == 1920);
  CHECK(ComputeCrop(1920, 1080, 0.5).dx == 0);
  CHECK(ComputeCrop(1920, 1080, std::nan("")).dx == 0);
  CHECK(ComputeCrop(0, 0, 1.05).srcW == 0);
  // clamps: huge zoom never produces an empty or negative source
  CropRect big = ComputeCrop(1920, 1080, 1000.0);
  CHECK(big.srcW >= 1 && big.srcH >= 1 && big.dx >= 0 && big.dy >= 0);
  CropRect tiny = ComputeCrop(3, 3, 2.0);
  CHECK(tiny.srcW >= 1 && tiny.srcH >= 1);
  // other sizes stay centred and within bounds
  for (uint32_t W : {1280u, 1920u, 2560u, 3840u}) for (double z : {1.01, 1.05, 1.10, 1.5}) {
    CropRect r = ComputeCrop(W, W * 9 / 16, z);
    CHECK(r.dx * 2 + (int)r.srcW == (int)W && r.srcW >= 1 && r.zx >= 1.0);
    CHECK(std::fabs(r.zx - z) < 0.01);
  }

  // ---- classification: each rule fires, in order ----
  CHECK(Classify(Good()) == Skip::None);
  { Facts f = Good(); f.isSwapchain = false; CHECK(Classify(f) == Skip::NotSwapchain); }
  { Facts f = Good(); f.isColor = false; CHECK(Classify(f) == Skip::NotColor); }
  { Facts f = Good(); f.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; CHECK(Classify(f) == Skip::NotLoad); }
  { Facts f = Good(); f.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; CHECK(Classify(f) == Skip::NotLoad); }
  { Facts f = Good(); f.samples = 4; CHECK(Classify(f) == Skip::Multisampled); }
  { Facts f = Good(); f.layout = VK_IMAGE_LAYOUT_UNDEFINED; CHECK(Classify(f) == Skip::LayoutUndefined); }
  { Facts f = Good(); f.layout = VK_IMAGE_LAYOUT_PREINITIALIZED; CHECK(Classify(f) == Skip::LayoutUndefined); }
  { Facts f = Good(); f.areaW = 960; CHECK(Classify(f) == Skip::AreaMismatch); }
  { Facts f = Good(); f.offX = 1; CHECK(Classify(f) == Skip::AreaMismatch); }
  { Facts f = Good(); f.layers = 2; CHECK(Classify(f) == Skip::Layers); }
  { Facts f = Good(); f.viewMask = 3; CHECK(Classify(f) == Skip::ViewMask); }
  { Facts f = Good(); f.resumeSuspend = true; CHECK(Classify(f) == Skip::ResumeSuspend); }
  { Facts f = Good(); f.viewRangeOk = false; CHECK(Classify(f) == Skip::BadViewRange); }
  { Facts f = Good(); f.layout = VK_IMAGE_LAYOUT_GENERAL; CHECK(Classify(f) == Skip::None); }
  { Facts f = Good(); f.layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; CHECK(Classify(f) == Skip::None); }
  // every Skip value has a distinct, non-empty explanation
  { std::string seen; for (int i = (int)Skip::None; i <= (int)Skip::WrongOrdinal; ++i) { const char* n = SkipName((Skip)i); CHECK(n && n[0] && n[0] != '?'); CHECK(seen.find(std::string("|") + n + "|") == std::string::npos); seen += std::string("|") + n + "|"; } }

  // ---- names match the Vulkan enums they claim to describe ----
  CHECK(std::string(LoadOpName(VK_ATTACHMENT_LOAD_OP_LOAD)) == "LOAD" && std::string(LoadOpName(VK_ATTACHMENT_LOAD_OP_CLEAR)) == "CLEAR");
  CHECK(std::string(LoadOpName(VK_ATTACHMENT_LOAD_OP_NONE_KHR)) == "NONE");
  CHECK(std::string(StoreOpName(VK_ATTACHMENT_STORE_OP_STORE)) == "STORE" && std::string(StoreOpName(VK_ATTACHMENT_STORE_OP_NONE)) == "NONE");
  CHECK(std::string(LayoutName(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)) == "COLOR_ATTACHMENT_OPTIMAL");
  CHECK(std::string(LayoutName(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)) == "PRESENT_SRC_KHR");
  CHECK(std::string(LayoutName(VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL)) == "ATTACHMENT_OPTIMAL");
  CHECK(std::string(LayoutName(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) == "SHADER_READ_ONLY_OPTIMAL");
  CHECK(std::string(FormatName(VK_FORMAT_B8G8R8A8_UNORM)) == "B8G8R8A8_UNORM" && std::string(FormatName(VK_FORMAT_R8G8B8A8_SRGB)) == "R8G8B8A8_SRGB");
  CHECK(std::string(FormatName(VK_FORMAT_R16G16B16A16_SFLOAT)) == "R16G16B16A16_SFLOAT" && std::string(FormatName(VK_FORMAT_D32_SFLOAT_S8_UINT)) == "D32_SFLOAT_S8_UINT");
  CHECK(std::string(FormatName(VK_FORMAT_A2B10G10R10_UNORM_PACK32)) == "A2B10G10R10_UNORM" && std::string(FormatName(VK_FORMAT_B10G11R11_UFLOAT_PACK32)) == "B10G11R11_UFLOAT");

  // ---- log text ----
  char ts[40]; FormatTimestamp(ts, sizeof ts, 2026, 11, 7, 18, 5, 9, 42);
  CHECK(std::string(ts) == "2026-11-07 18:05:09.042");
  CHECK(std::string(Indent(0)) == "" && std::string(Indent(1)) == "    " && std::string(Indent(2)) == "        " && std::string(Indent(3)) == "            ");
  CHECK(std::string(Indent(-5)) == "" && std::string(Indent(99)) == std::string(Indent(3)));

  std::printf(fails ? "%d FAILED\n" : "all core tests passed\n", fails);
  return fails ? 1 : 0;
}
