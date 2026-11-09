// RDR2 Vulkan overscan v0.0.3 (ASI).
//
// What it does: just before the first full-screen render pass that LOADs the backbuffer (the point the
// reference FG mod uses as "pre-HUD"), it records  copy backbuffer -> private scratch, then
// blit scratch(centre crop) -> backbuffer (full image)  into the game's own command buffer. Everything
// the game draws afterwards (UI) lands on the zoomed image at its normal size.
//
// What it logs: nothing, until the capture key is pressed. Then it records CaptureFrames frames of
// pass / submit / hook information to RDR2Overscan.log (opened and closed per write, so the file can be
// edited or deleted while the game runs). The INI is re-read while the game runs.
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vulkan/vulkan.h>
#include <detours.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "overscan_core.h"
#include "sharpen.h"

namespace ovs {

// ============================================================================================
// Hook table
// ============================================================================================
#define OVS_HOOKS(X) \
  X(CreateDevice) X(CreateSwapchainKHR) X(GetSwapchainImagesKHR) X(DestroySwapchainKHR) \
  X(AcquireNextImageKHR) X(AcquireNextImage2KHR) X(QueuePresentKHR) \
  X(CreateImageView) X(DestroyImageView) X(CreateFramebuffer) X(DestroyFramebuffer) \
  X(CreateRenderPass) X(CreateRenderPass2) X(DestroyRenderPass) \
  X(CmdBeginRenderPass) X(CmdBeginRenderPass2) X(CmdBeginRendering) \
  X(QueueSubmit) X(QueueSubmit2) X(CmdExecuteCommands) X(BeginCommandBuffer) \
  X(CmdPipelineBarrier) X(CmdPipelineBarrier2) X(CmdCopyImage) X(CmdBlitImage) X(CmdClearColorImage) \
  X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdSetViewport) X(CmdSetScissor) X(CmdSetViewportWithCount) X(CmdSetScissorWithCount)

enum HookId {
#define X(n) HK_##n,
  OVS_HOOKS(X)
#undef X
  HK_COUNT
};
static const char* const kHookName[HK_COUNT] = {
#define X(n) #n,
  OVS_HOOKS(X)
#undef X
};

// gp = pointer obtained through vkGet{Instance,Device}ProcAddr (what the game gets when it resolves
// functions itself); ex = Detours trampoline for the vulkan-1.dll export (what the game gets when it
// calls the export directly). The reference repo shows RDR2 does both, so both are hooked and counted.
template <class T> struct Pair { T gp{}; T ex{}; };
#define X(n) static Pair<PFN_vk##n> P_##n;
OVS_HOOKS(X)
#undef X

static std::atomic<uint64_t> g_hits[HK_COUNT][2];
static std::atomic<uint64_t> g_hitGIPA{0}, g_hitGDPA{0};
static bool g_wrapped[HK_COUNT];     // a proc-address wrapper was handed out
static int g_exportState[HK_COUNT];  // 0 not exported, 1 attached, 2 attach failed
static PFN_vkGetInstanceProcAddr o_GIPA;
static PFN_vkGetDeviceProcAddr o_GDPA;
static PFN_vkGetPhysicalDeviceFormatProperties g_fnFormatProps;
static PFN_vkGetPhysicalDeviceMemoryProperties g_fnMemProps;
static PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR g_fnSurfCaps;
static int g_exportStateGIPA, g_exportStateGDPA;

static inline void Hit(int id, int path) { g_hits[id][path].fetch_add(1, std::memory_order_relaxed); }

// ============================================================================================
// Config (INI is re-read while the game runs)
// ============================================================================================
struct Config {
  std::atomic<bool> enabled{true};
  std::atomic<double> zoom{1.05};
  std::atomic<int> engageFrames{8}, rampFrames{30};
  std::atomic<bool> requireDepth{true};
  std::atomic<double> sharp{0.0};       // CAS sharpening strength 0..1 (0 = off: plain bilinear blit)
  std::atomic<bool> holdZoom{true};     // keep the engaged zoom across pause/menu frames (no re-ramp while paused)
  std::atomic<int> passIndex{1};        // which qualifying pass per frame gets the transform (1 = first)
  std::atomic<int> captureFrames{3};
  std::atomic<int> captureKey{0xDC};    // VK_OEM_5, the backslash key on a US layout
  std::atomic<bool> logPasses{true}, logSubmits{true}, logCmdBegin{true};
};
static Config g_cfg;
static std::wstring g_dir, g_iniPath, g_logPath;
static FILETIME g_iniStamp{};

static std::wstring ModuleDir(HMODULE m) {
  wchar_t p[MAX_PATH]{}; GetModuleFileNameW(m, p, MAX_PATH);
  std::wstring s(p); auto k = s.find_last_of(L"\\/"); return k == std::wstring::npos ? L"" : s.substr(0, k + 1);
}

static void WriteDefaultIni() {
  if (GetFileAttributesW(g_iniPath.c_str()) != INVALID_FILE_ATTRIBUTES) return;
  const char* text =
    "; ==============================================================================\r\n"
    "; RDR2 Vulkan overscan v0.0.3\r\n"
    "; Saved changes are picked up while the game is running (checked about once a second).\r\n"
    "; ==============================================================================\r\n"
    "\r\n"
    "[Overscan]\r\n"
    "\r\n"
    "; Master switch: 1 = on, 0 = off.\r\n"
    "Enabled=1\r\n"
    "\r\n"
    "; Zoom factor for the 3D scene (the UI is left untouched).\r\n"
    ";   1.05 = the centre 1/1.05 of the image is stretched to fill the screen.\r\n"
    ";   Range: 1.00 - 1.50 (values outside are clamped).\r\n"
    "Zoom=1.05\r\n"
    "\r\n"
    "; Which qualifying full-screen LOAD pass on the backbuffer gets the transform (1 = first).\r\n"
    ";   UI is zoomed too            -> the transform is too early in the frame, try a higher number\r\n"
    ";   nothing changes at all      -> try 2, 3, ...\r\n"
    "TransformPassIndex=1\r\n"
    "\r\n"
    "; 1 = only the pass that also writes depth (the main 3D scene pass) is transformed.\r\n"
    ";   RDR2 records its command buffers on several threads, so the recording order\r\n"
    ";   varies from frame to frame. Counting passes alone made the zoom land on\r\n"
    ";   different passes (flicker). Leave this at 1 unless you are debugging.\r\n"
    "RequireDepthWrite=1\r\n"
    "\r\n"
    "; The zoom stays off until the scene has been stable for EngageFrames presented\r\n"
    "; frames, then eases in over RampFrames (smoothstep).\r\n"
    ";   EngageFrames=0 and RampFrames=0 -> instant (old behaviour).\r\n"
    ";   NOTE: since HoldZoomOnPause (below) was added, pausing no longer resets this\r\n"
    ";   state, so these two settings mostly matter right after launch. Their\r\n"
    ";   behaviour may be altered or reworked in a future version.\r\n"
    "EngageFrames=8\r\n"
    "RampFrames=30\r\n"
    "\r\n"
    "; 1 = pausing/opening menus does NOT reset the engage/ramp state, so the game\r\n"
    ";   behind a pause menu stays zoomed instead of unzoomed-then-zooming-in.\r\n"
    "; 0 = old behaviour (state resets on every frame without a scene pass).\r\n"
    "HoldZoomOnPause=1\r\n"
    "\r\n"
    "; Sharpening of the zoomed 3D scene (AMD FidelityFX CAS, contrast adaptive). The UI is untouched.\r\n"
    ";   EXPERIMENTAL: runs an extra full-screen shader pass and may cause a small\r\n"
    ";   performance penalty.\r\n"
    ";   0.0 = off (plain bilinear zoom, default).  0.3-0.6 is a sensible starting range, 1.0 = strongest.\r\n"
    ";   Applies live. If the pass cannot be built the mod silently keeps the plain zoom\r\n"
    ";   (the capture log shows sharpen=on / FAILED: reason per swapchain).\r\n"
    "Sharpness=0.0\r\n"
    "\r\n"
    "\r\n"
    "[Log]\r\n"
    "\r\n"
    "; Nothing is written until the capture key is pressed in the game window.\r\n"
    ";   0xDC = backslash (virtual-key code)\r\n"
    "CaptureKey=0xDC\r\n"
    "\r\n"
    "; Number of frames recorded per capture (1 - 30).\r\n"
    "CaptureFrames=3\r\n"
    "\r\n"
    "; What to include in the capture log (1 = on, 0 = off).\r\n"
    "; Submits and command-buffer begins make the log very large; turn them off\r\n"
    "; if you only need the pass verdicts.\r\n"
    "LogPasses=1\r\n"
    "LogSubmits=1\r\n"
    "LogCommandBufferBegin=1\r\n";
  HANDLE h = CreateFileW(g_iniPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  DWORD w; WriteFile(h, text, (DWORD)strlen(text), &w, nullptr); CloseHandle(h);
}

static void LoadIni() {
  const wchar_t* f = g_iniPath.c_str();
  g_cfg.enabled = GetPrivateProfileIntW(L"Overscan", L"Enabled", 1, f) != 0;
  wchar_t zs[32]{}; GetPrivateProfileStringW(L"Overscan", L"Zoom", L"1.05", zs, 32, f);
  double z = wcstod(zs, nullptr); if (!(z >= 1.0)) z = 1.0; if (z > 1.5) z = 1.5;
  g_cfg.zoom = z;
  { int e = (int)GetPrivateProfileIntW(L"Overscan", L"EngageFrames", 8, f), r = (int)GetPrivateProfileIntW(L"Overscan", L"RampFrames", 30, f);
    g_cfg.engageFrames = e < 0 ? 0 : e > 600 ? 600 : e; g_cfg.rampFrames = r < 0 ? 0 : r > 600 ? 600 : r; }
  g_cfg.requireDepth = GetPrivateProfileIntW(L"Overscan", L"RequireDepthWrite", 1, f) != 0;
  g_cfg.holdZoom = GetPrivateProfileIntW(L"Overscan", L"HoldZoomOnPause", 1, f) != 0;
  { wchar_t ss[32]{}; GetPrivateProfileStringW(L"Overscan", L"Sharpness", L"0.0", ss, 32, f); double sv = wcstod(ss, nullptr); if (!(sv > 0.0)) sv = 0.0; if (sv > 1.0) sv = 1.0; g_cfg.sharp = sv; }
  int pi = (int)GetPrivateProfileIntW(L"Overscan", L"TransformPassIndex", 1, f); g_cfg.passIndex = pi < 1 ? 1 : pi;
  wchar_t ks[32]{}; GetPrivateProfileStringW(L"Log", L"CaptureKey", L"0xDC", ks, 32, f);
  long k = wcstol(ks, nullptr, 0); g_cfg.captureKey = k > 0 && k < 256 ? (int)k : 0xDC;
  int cf = (int)GetPrivateProfileIntW(L"Log", L"CaptureFrames", 3, f); g_cfg.captureFrames = cf < 1 ? 1 : cf > 30 ? 30 : cf;
  g_cfg.logPasses = GetPrivateProfileIntW(L"Log", L"LogPasses", 1, f) != 0;
  g_cfg.logSubmits = GetPrivateProfileIntW(L"Log", L"LogSubmits", 1, f) != 0;
  g_cfg.logCmdBegin = GetPrivateProfileIntW(L"Log", L"LogCommandBufferBegin", 1, f) != 0;
}

// Polled from Present; wall-clock throttled (~1 s) so it is frame-rate independent. Only the file's
// metadata (last-write time) is queried; the INI is re-read solely when that timestamp has changed.
static constexpr ULONGLONG kIniPollMs = 1000;
static void MaybeReloadIni() {
  static ULONGLONG last = 0; const ULONGLONG now = GetTickCount64();
  if (last && now - last < kIniPollMs) return;
  last = now;
  WIN32_FILE_ATTRIBUTE_DATA d{};
  if (!GetFileAttributesExW(g_iniPath.c_str(), GetFileExInfoStandard, &d)) return;
  if (CompareFileTime(&d.ftLastWriteTime, &g_iniStamp) != 0) { g_iniStamp = d.ftLastWriteTime; LoadIni(); }
}

// ============================================================================================
// Log file: opened and closed for every write, with full sharing, so it can be edited/deleted live
// ============================================================================================
static std::string Fmt(const char* fmt, ...) {
  char buf[4096]; va_list ap; va_start(ap, fmt); int n = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  if (n < 0) return {};
  if ((size_t)n >= sizeof buf) { std::string s(buf, sizeof buf - 1); s += "...(truncated)"; return s; }
  return std::string(buf, (size_t)n);
}
static std::string Now() {
  SYSTEMTIME t; GetLocalTime(&t); char b[40];
  FormatTimestamp(b, sizeof b, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  return b;
}
static void AppendLog(const std::string& text) {
  if (text.empty()) return;
  HANDLE h = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;   // locked exclusively by an editor: skip this write rather than stall the game
  DWORD w; WriteFile(h, text.data(), (DWORD)text.size(), &w, nullptr); CloseHandle(h);
}

// In-memory event ring (nothing touches the disk). Dumped into each capture header.
static std::mutex g_evMu;
static std::deque<std::string> g_events;
static void Event(const std::string& s) {
  std::lock_guard<std::mutex> l(g_evMu);
  g_events.push_back(Now() + "  " + s);
  while (g_events.size() > 64) g_events.pop_front();
}

// ============================================================================================
// Tracked state
// ============================================================================================
struct DevFns {
  PFN_vkCreateImage createImage{}; PFN_vkDestroyImage destroyImage{}; PFN_vkGetImageMemoryRequirements getReq{};
  PFN_vkAllocateMemory alloc{}; PFN_vkFreeMemory freeMem{}; PFN_vkBindImageMemory bind{};
  PFN_vkCmdPipelineBarrier barrier{}; PFN_vkCmdCopyImage copy{}; PFN_vkCmdBlitImage blit{};
  bool Complete() const { return createImage && destroyImage && getReq && alloc && freeMem && bind && barrier && copy && blit; }
};
struct Slot { VkImage image{}, scratch{}; VkDeviceMemory mem{}; uint32_t qualSeen = 0; };
struct Swapchain {
  VkSwapchainKHR h{}; VkDevice dev{}; VkFormat fmt{}; VkExtent2D ext{};
  VkImageUsageFlags requestedUsage = 0, usage = 0; VkPresentModeKHR mode{}; uint32_t minImages = 0;
  bool bumped = false, usable = false, sharpPossible = false; std::shared_ptr<sharpen::Res> sharp;
  std::string whyNot = "images not yet retrieved";
  DevFns fn; std::vector<Slot> slots;
};
struct ViewInfo { VkImage image; VkFormat format; VkImageSubresourceRange range; };
struct AttDesc { VkFormat format; VkSampleCountFlagBits samples; VkAttachmentLoadOp load; VkAttachmentStoreOp store; VkImageLayout initial; bool color; };
struct FbInfo { VkRenderPass pass; uint32_t w, h, layers; bool imageless; std::vector<VkImageView> views; };
struct Retired { VkDevice dev; DevFns fn; std::vector<Slot> slots; uint64_t freeAt; std::shared_ptr<sharpen::Res> sharp; };

static std::mutex g_mu;   // guards every map below
// Shadow of the graphics state the CAS pass overwrites, per command buffer (only maintained while Sharpness > 0).
// A command buffer is recorded by one thread at a time and the pass is recorded on that same thread, so the shadow is thread-local (no locking).
static thread_local std::unordered_map<VkCommandBuffer, sharpen::Shadow> g_cs;
template <class F> static inline void Shadowed(VkCommandBuffer c, F fn) { if (g_cfg.sharp.load() <= 0.0) return; fn(g_cs[c]); }
static std::unordered_map<VkSwapchainKHR, std::unique_ptr<Swapchain>> g_sc;
static std::unordered_map<VkImage, std::pair<Swapchain*, uint32_t>> g_imgRef;
static std::unordered_map<VkImageView, ViewInfo> g_views;
static std::unordered_map<VkFramebuffer, FbInfo> g_fbs;
static std::unordered_map<VkRenderPass, std::vector<AttDesc>> g_passes;
static std::vector<Retired> g_retired;
static VkPhysicalDevice g_pd{};
static VkDevice g_firstDevice{};
static std::atomic<uint64_t> g_presents{0};

// ============================================================================================
// Capture (3 frames on key press)
// ============================================================================================
struct FrameStats { std::atomic<uint32_t> begins{0}, passes{0}, scPasses{0}, qualifying{0}, transformed{0}, submits{0}, executes{0}; };
static struct Capture {
  std::atomic<bool> active{false};
  std::mutex mu; std::vector<std::string> lines;
  uint32_t id = 0, frame = 0, total = 0; uint64_t t0 = 0, presentAtStart = 0;
  uint64_t hitsAtStart[HK_COUNT][2]{}; uint64_t gipaAtStart = 0, gdpaAtStart = 0;
  FrameStats fs; bool keyDown = false;
} g_cap;

static double QpcMs() {
  static const double f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return 1000.0 / (double)q.QuadPart; }();
  LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart * f;
}
static inline bool Capturing() { return g_cap.active.load(std::memory_order_relaxed); }
static void CapLine(const std::string& s) {
  size_t ind = s.find_first_not_of(' ');  // keep the indent in front of the stamp so nesting reads left-to-right
  if (ind == std::string::npos) ind = 0;
  std::string line = std::string(ind, ' ') + Fmt("[+%8.3fms tid=%5lu] ", QpcMs() - (double)g_cap.t0 / 1000.0, (unsigned long)GetCurrentThreadId()) + s.substr(ind);
  std::lock_guard<std::mutex> l(g_cap.mu); g_cap.lines.push_back(std::move(line));
}
#define H64(x) ((unsigned long long)(uintptr_t)(x))

// ============================================================================================
// Scratch image management
// ============================================================================================
static void LoadDevFns(VkDevice d, DevFns& f) {
#define L(field, name) f.field = (PFN_vk##name)o_GDPA(d, "vk" #name)
  L(createImage, CreateImage); L(destroyImage, DestroyImage); L(getReq, GetImageMemoryRequirements);
  L(alloc, AllocateMemory); L(freeMem, FreeMemory); L(bind, BindImageMemory);
  L(barrier, CmdPipelineBarrier); L(copy, CmdCopyImage); L(blit, CmdBlitImage);
#undef L
}
static void FreeSlots(VkDevice d, const DevFns& f, std::vector<Slot>& slots) {
  for (auto& s : slots) {
    if (s.scratch && f.destroyImage) f.destroyImage(d, s.scratch, nullptr);
    if (s.mem && f.freeMem) f.freeMem(d, s.mem, nullptr);
    s.scratch = VK_NULL_HANDLE; s.mem = VK_NULL_HANDLE;
  }
}
// Caller holds g_mu.
static void BuildSlots(Swapchain& sc, const std::vector<VkImage>& images) {
  sc.usable = false;
  if (!sc.bumped && (sc.usage & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) != (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { sc.whyNot = "swapchain lacks TRANSFER_SRC|TRANSFER_DST and the usage bump was rejected"; return; }
  if (!g_pd || !g_fnFormatProps || !g_fnMemProps) { sc.whyNot = "physical device or format/memory query unavailable"; return; }
  sc.fn = DevFns{}; LoadDevFns(sc.dev, sc.fn);
  if (!sc.fn.Complete()) { sc.whyNot = "device functions for image/copy/blit unavailable"; return; }
  VkFormatProperties fp{}; g_fnFormatProps(g_pd, sc.fmt, &fp);
  const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  if ((fp.optimalTilingFeatures & need) != need) { sc.whyNot = Fmt("format %d lacks BLIT_SRC/BLIT_DST/FILTER_LINEAR (features=0x%x)", (int)sc.fmt, (unsigned)fp.optimalTilingFeatures); return; }
  sc.sharpPossible = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) && (sc.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
  sc.sharp = sc.sharpPossible ? std::make_shared<sharpen::Res>() : nullptr;
  VkPhysicalDeviceMemoryProperties mp{}; g_fnMemProps(g_pd, &mp);
  std::vector<Slot> slots(images.size());
  for (size_t i = 0; i < images.size(); ++i) {
    Slot& s = slots[i]; s.image = images[i];
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = sc.fmt; ci.extent = {sc.ext.width, sc.ext.height, 1};
    ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (sc.sharpPossible ? VK_IMAGE_USAGE_SAMPLED_BIT : 0); ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = sc.fn.createImage(sc.dev, &ci, nullptr, &s.scratch);
    if (r != VK_SUCCESS) { sc.whyNot = Fmt("vkCreateImage(scratch) failed: %d", (int)r); FreeSlots(sc.dev, sc.fn, slots); return; }
    VkMemoryRequirements req{}; sc.fn.getReq(sc.dev, s.scratch, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t t = 0; t < mp.memoryTypeCount; ++t)
      if ((req.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = t; break; }
    if (type == UINT32_MAX) { sc.whyNot = "no device-local memory type for scratch image"; FreeSlots(sc.dev, sc.fn, slots); return; }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size; ai.memoryTypeIndex = type;
    r = sc.fn.alloc(sc.dev, &ai, nullptr, &s.mem);
    if (r != VK_SUCCESS) { sc.whyNot = Fmt("vkAllocateMemory(scratch) failed: %d", (int)r); FreeSlots(sc.dev, sc.fn, slots); return; }
    r = sc.fn.bind(sc.dev, s.scratch, s.mem, 0);
    if (r != VK_SUCCESS) { sc.whyNot = Fmt("vkBindImageMemory(scratch) failed: %d", (int)r); FreeSlots(sc.dev, sc.fn, slots); return; }
  }
  sc.slots = std::move(slots); sc.usable = true; sc.whyNot.clear();
  for (uint32_t i = 0; i < sc.slots.size(); ++i) g_imgRef[sc.slots[i].image] = {&sc, i};
}

// ============================================================================================
// The transform: recorded into the game's command buffer, outside any render pass instance
// ============================================================================================
static void RecordTransform(const Swapchain& sc, const Slot& s, VkCommandBuffer cmd, VkImageLayout layout, const CropRect& cr) {
  const DevFns& f = sc.fn;
  const VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  auto bar = [&](VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = sa; b.dstAccessMask = da; b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = img; b.subresourceRange = rng; return b;
  };
  // 1. backbuffer: pass layout -> TRANSFER_SRC ; scratch: UNDEFINED -> TRANSFER_DST (its old contents are dead)
  VkImageMemoryBarrier b1[2] = {
    bar(s.image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
    bar(s.scratch, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT)};
  f.barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, b1);
  // 2. full copy
  VkImageCopy cp{}; cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; cp.dstSubresource = cp.srcSubresource; cp.extent = {sc.ext.width, sc.ext.height, 1};
  f.copy(cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.scratch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
  // 2b. optional CAS pass: sample the scratch copy, rewrite the backbuffer with a fragment shader (no blit)
  if (const float sh = (float)g_cfg.sharp.load(); sh > 0.f && sc.sharpPossible && sc.sharp && layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
    sharpen::Res& res = *sc.sharp; const uint32_t idx = (uint32_t)(&s - sc.slots.data());
    if (!res.ok && !res.tried) {
      std::vector<VkImage> sw, sr; for (const Slot& q : sc.slots) { sw.push_back(q.image); sr.push_back(q.scratch); }
      const bool ok = sharpen::Init(res, sc.dev, o_GDPA, sc.fmt, sc.ext, sw, sr);
      Event(ok ? Fmt("sharpen pass ready for swapchain 0x%llx", H64(sc.h)) : Fmt("sharpen pass unavailable for swapchain 0x%llx (using plain blit): %s", H64(sc.h), res.why.c_str()));
    }
    if (res.ok && idx < res.set.size()) {
      VkImageMemoryBarrier b2s[2] = {
        bar(s.scratch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
        bar(s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)};
      f.barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 2, b2s);
      sharpen::Record(res, cmd, idx, cr.dx, cr.dy, cr.srcW, cr.srcH, sh);
      { auto it = g_cs.find(cmd); if (it != g_cs.end()) sharpen::Restore(res, cmd, it->second); }   // give the game its state back
      VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      f.barrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
      return;
    }
    // not ready: fall through to the plain blit below
  }
  // 3. scratch -> TRANSFER_SRC ; backbuffer -> TRANSFER_DST
  VkImageMemoryBarrier b2[2] = {
    bar(s.scratch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
    bar(s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT)};
  f.barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, b2);
  // 4. centre crop of the copy -> whole backbuffer. Source and destination are different images, as required.
  VkImageBlit bl{}; bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bl.dstSubresource = bl.srcSubresource;
  bl.srcOffsets[0] = {cr.dx, cr.dy, 0}; bl.srcOffsets[1] = {(int32_t)sc.ext.width - cr.dx, (int32_t)sc.ext.height - cr.dy, 1};
  bl.dstOffsets[0] = {0, 0, 0}; bl.dstOffsets[1] = {(int32_t)sc.ext.width, (int32_t)sc.ext.height, 1};
  f.blit(cmd, s.scratch, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
  // 5. backbuffer back to the layout the pass expects, visible to the pass that follows
  VkImageMemoryBarrier b3 = bar(s.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layout, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
  f.barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b3);
}

// ============================================================================================
// Pass analysis (shared by render-pass and dynamic-rendering begins)
// ============================================================================================
struct AttSeen { VkImageView view; VkImage image; VkFormat fmt; int load, store, layout; bool color, sc; VkSampleCountFlagBits samples; bool viewRangeOk; };
struct Decision { Skip skip = Skip::NotSwapchain; Swapchain* sc = nullptr; Slot* slot = nullptr; VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED; uint32_t ordinal = 0; bool touchesSwapchain = false; std::string why; };

// Caller holds g_mu. atts = every attachment of the pass, already resolved to images.
static Decision Decide(const std::vector<AttSeen>& atts, int32_t offX, int32_t offY, uint32_t w, uint32_t h, uint32_t layers, uint32_t viewMask, bool resumeSuspend) {
  Decision best; bool haveSc = false;
  bool depthWrite = false; for (const AttSeen& a : atts) if (!a.color && !a.sc && (a.layout == 3 || a.layout == 1000314001)) depthWrite = true;
  for (const AttSeen& a : atts) {
    if (!a.sc) continue;
    auto ref = g_imgRef.find(a.image);
    Facts f; f.isSwapchain = ref != g_imgRef.end(); f.isColor = a.color; f.loadOp = a.load; f.samples = (int)a.samples; f.layout = a.layout;
    f.offX = offX; f.offY = offY; f.areaW = w; f.areaH = h; f.layers = layers; f.viewMask = viewMask; f.resumeSuspend = resumeSuspend; f.viewRangeOk = a.viewRangeOk;
    if (f.isSwapchain) { f.scW = ref->second.first->ext.width; f.scH = ref->second.first->ext.height; }
    Skip k = Classify(f);
    if (k == Skip::None && g_cfg.requireDepth.load() && !depthWrite) k = Skip::NoDepthWrite;
    if (!haveSc) { best.skip = k; haveSc = true; best.touchesSwapchain = true; }
    if (k != Skip::None) continue;
    Swapchain* sc = ref->second.first; Slot& slot = sc->slots[ref->second.second];
    best = Decision{}; best.touchesSwapchain = true; best.skip = Skip::None; best.sc = sc; best.slot = &slot; best.layout = (VkImageLayout)a.layout;
    best.ordinal = ++slot.qualSeen;
    if (!g_cfg.enabled.load()) best.skip = Skip::Disabled;
    else if (!sc->usable || !slot.scratch) { best.skip = Skip::NoSlot; best.why = sc->whyNot; }
    else if ((int)best.ordinal != g_cfg.passIndex.load()) best.skip = Skip::WrongOrdinal;
    return best;
  }
  return best;
}

static std::string DescribeAtts(const std::vector<AttSeen>& atts) {
  std::string s;
  for (size_t i = 0; i < atts.size(); ++i) {
    const AttSeen& a = atts[i];
    s += Fmt("\n%s[%zu] image=0x%llx view=0x%llx fmt=%s(%d) load=%s store=%s layout=%s color=%c swapchain=%c%s", Indent(3), i, H64(a.image), H64(a.view),
             FormatName((int)a.fmt), (int)a.fmt, LoadOpName(a.load), StoreOpName(a.store), LayoutLabel(a.layout).c_str(), a.color ? 'Y' : 'N', a.sc ? 'Y' : 'N', a.viewRangeOk ? "" : " viewRange=non-trivial");
  }
  return s;
}

// Frames in a row in which a qualifying scene pass was seen (0 on pause/menus/loading). Drives the delayed, eased zoom.
static std::atomic<uint32_t> g_stableStreak{0};
static std::atomic<bool> g_frameQual{false};
static double EffectiveZoom() {
  const double z = g_cfg.zoom.load(); const uint32_t s = g_stableStreak.load(), e = (uint32_t)g_cfg.engageFrames.load(), r = (uint32_t)g_cfg.rampFrames.load();
  if (s < e) return 1.0;
  if (r == 0 || s >= e + r) return z;
  double x = (double)(s - e + 1) / (double)(r + 1); x = x * x * (3.0 - 2.0 * x);
  return 1.0 + (z - 1.0) * x;
}
static void Conclude(VkCommandBuffer cmd, const Decision& d, const char* api, const char* head, const std::string& attText, bool cap) {
  bool did = false; CropRect cr{};
  if (d.skip == Skip::None) {
    g_frameQual = true;
    cr = ComputeCrop(d.sc->ext.width, d.sc->ext.height, EffectiveZoom());
    if (cr.dx || cr.dy) { RecordTransform(*d.sc, *d.slot, cmd, d.layout, cr); did = true; }
  }
  g_cap.fs.passes++;
  if (d.touchesSwapchain) g_cap.fs.scPasses++;
  if (d.skip == Skip::None || d.skip == Skip::Disabled || d.skip == Skip::NoSlot || d.skip == Skip::WrongOrdinal) g_cap.fs.qualifying++;
  if (did) g_cap.fs.transformed++;
  if (!cap || !g_cfg.logPasses.load()) return;
  std::string verdict;
  if (did) verdict = Fmt("TRANSFORMED zoom=%.4fx%.4f crop=%d,%d src=%ux%u", cr.zx, cr.zy, cr.dx, cr.dy, cr.srcW, cr.srcH);
  else if (d.skip == Skip::WrongOrdinal) verdict = Fmt("skip: qualifying pass #%u, transform is set to #%d", d.ordinal, g_cfg.passIndex.load());
  else if (d.skip == Skip::NoSlot) verdict = Fmt("skip: %s (%s)", SkipName(d.skip), d.why.c_str());
  else verdict = Fmt("skip: %s", SkipName(d.skip));
  CapLine(std::string(Indent(2)) + api + Fmt(" cmd=0x%llx ", H64(cmd)) + head + attText + "\n" + Indent(3) + "=> " + verdict);
}

static void OnBeginRenderPass(VkCommandBuffer cmd, const VkRenderPassBeginInfo* bi, const char* api) {
  if (!bi) return;
  g_cap.fs.begins++;
  const bool cap = Capturing();
  Decision d; std::string head, attText;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto fb = g_fbs.find(bi->framebuffer); auto rp = g_passes.find(bi->renderPass);
    if (fb == g_fbs.end() || rp == g_passes.end()) { d.skip = Skip::UnknownPass; head = Fmt("area=%ux%u framebuffer/renderpass not tracked (created before the hook?)", bi->renderArea.extent.width, bi->renderArea.extent.height); }
    else {
      std::vector<VkImageView> views = fb->second.views;
      if (fb->second.imageless)
        for (auto n = (const VkBaseInStructure*)bi->pNext; n; n = n->pNext)
          if (n->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) { auto ab = (const VkRenderPassAttachmentBeginInfo*)n; views.assign(ab->pAttachments, ab->pAttachments + ab->attachmentCount); }
      std::vector<AttSeen> atts;
      for (size_t i = 0; i < rp->second.size() && i < views.size(); ++i) {
        const AttDesc& ad = rp->second[i]; AttSeen a{}; a.view = views[i]; a.fmt = ad.format; a.load = ad.load; a.store = ad.store; a.layout = ad.initial; a.color = ad.color; a.samples = ad.samples;
        auto v = g_views.find(views[i]);
        if (v != g_views.end()) { a.image = v->second.image; a.viewRangeOk = v->second.range.baseMipLevel == 0 && v->second.range.levelCount == 1 && v->second.range.baseArrayLayer == 0 && v->second.range.layerCount == 1; a.sc = g_imgRef.count(a.image) != 0; }
        atts.push_back(a);
      }
      d = Decide(atts, bi->renderArea.offset.x, bi->renderArea.offset.y, bi->renderArea.extent.width, bi->renderArea.extent.height, fb->second.layers, 0, false);
      if (cap) { head = Fmt("tid=%lu area=%ux%u+%d+%d fb=0x%llx rp=0x%llx layers=%u attachments=%zu", (unsigned long)GetCurrentThreadId(), bi->renderArea.extent.width, bi->renderArea.extent.height, bi->renderArea.offset.x, bi->renderArea.offset.y, H64(bi->framebuffer), H64(bi->renderPass), fb->second.layers, atts.size()); attText = DescribeAtts(atts); }
    }
  }
  Conclude(cmd, d, api, head.c_str(), attText, cap);
}

static void OnBeginRendering(VkCommandBuffer cmd, const VkRenderingInfo* ri, const char* api) {
  if (!ri) return;
  g_cap.fs.begins++;
  const bool cap = Capturing();
  Decision d; std::string head, attText;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    std::vector<AttSeen> atts;
    for (uint32_t i = 0; i < ri->colorAttachmentCount; ++i) {
      const VkRenderingAttachmentInfo& ra = ri->pColorAttachments[i]; AttSeen a{};
      a.view = ra.imageView; a.layout = ra.imageLayout; a.load = ra.loadOp; a.store = ra.storeOp; a.color = true; a.samples = VK_SAMPLE_COUNT_1_BIT;
      auto v = g_views.find(ra.imageView);
      if (v != g_views.end()) { a.image = v->second.image; a.fmt = v->second.format; a.viewRangeOk = v->second.range.baseMipLevel == 0 && v->second.range.levelCount == 1 && v->second.range.baseArrayLayer == 0 && v->second.range.layerCount == 1; a.sc = g_imgRef.count(a.image) != 0; }
      atts.push_back(a);
    }
    d = Decide(atts, ri->renderArea.offset.x, ri->renderArea.offset.y, ri->renderArea.extent.width, ri->renderArea.extent.height, ri->layerCount, ri->viewMask, (ri->flags & (VK_RENDERING_RESUMING_BIT | VK_RENDERING_SUSPENDING_BIT)) != 0);
    if (cap) { head = Fmt("tid=%lu area=%ux%u+%d+%d layers=%u flags=0x%x colorAttachments=%u depth=%s", (unsigned long)GetCurrentThreadId(), ri->renderArea.extent.width, ri->renderArea.extent.height, ri->renderArea.offset.x, ri->renderArea.offset.y, ri->layerCount, (unsigned)ri->flags, ri->colorAttachmentCount, ri->pDepthAttachment ? "yes" : "no"); attText = DescribeAtts(atts); }
  }
  Conclude(cmd, d, api, head.c_str(), attText, cap);
}

// ============================================================================================
// Swapchain / resource lifecycle
// ============================================================================================
static std::string SharpStatus(const Swapchain& sc) {
  if (!sc.sharpPossible || !sc.sharp) return "unsupported";
  if (sc.sharp->ok) return g_cfg.sharp.load() > 0 ? "on" : "ready(off)";
  if (sc.sharp->tried) return "FAILED: " + sc.sharp->why;
  return g_cfg.sharp.load() > 0 ? "pending" : "off";
}
static void OnDeviceCreated(VkPhysicalDevice pd, VkDevice dev) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_firstDevice) { g_firstDevice = dev; g_pd = pd; Event(Fmt("first device 0x%llx on physical device 0x%llx", H64(dev), H64(pd))); }
  else Event(Fmt("additional device 0x%llx (physical 0x%llx) ignored for physical-device tracking", H64(dev), H64(pd)));
}

static void OnSwapchainCreated(VkDevice d, VkSwapchainKHR h, const VkSwapchainCreateInfoKHR& ci, VkImageUsageFlags requested, bool bumped) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto sc = std::make_unique<Swapchain>();
  sc->h = h; sc->dev = d; sc->fmt = ci.imageFormat; sc->ext = ci.imageExtent; sc->requestedUsage = requested; sc->usage = ci.imageUsage;
  sc->mode = ci.presentMode; sc->minImages = ci.minImageCount; sc->bumped = bumped;
  Event(Fmt("swapchain 0x%llx created: %ux%u fmt=%s(%d) presentMode=%d minImages=%u usage requested=0x%x actual=0x%x (%s)", H64(h), ci.imageExtent.width, ci.imageExtent.height, FormatName((int)ci.imageFormat), (int)ci.imageFormat, (int)ci.presentMode, ci.minImageCount, (unsigned)requested, (unsigned)ci.imageUsage, bumped ? "TRANSFER_SRC|DST added" : "usage left as requested"));
  g_sc[h] = std::move(sc);
}

static void OnSwapchainImages(VkSwapchainKHR h, uint32_t n, const VkImage* imgs) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_sc.find(h); if (it == g_sc.end()) { Event(Fmt("images queried for unknown swapchain 0x%llx (created before the hook?)", H64(h))); return; }
  Swapchain& sc = *it->second;
  if (sc.slots.size() == n) { bool same = true; for (uint32_t i = 0; i < n; ++i) if (sc.slots[i].image != imgs[i]) same = false; if (same) return; }
  if (!sc.slots.empty()) { for (auto& s : sc.slots) g_imgRef.erase(s.image); g_retired.push_back({sc.dev, sc.fn, std::move(sc.slots), g_presents.load() + 16, std::move(sc.sharp)}); sc.slots.clear(); }
  std::vector<VkImage> v(imgs, imgs + n);
  BuildSlots(sc, v);
  Event(sc.usable ? Fmt("swapchain 0x%llx: %u images, scratch images allocated, transform usable", H64(h), n) : Fmt("swapchain 0x%llx: %u images, transform NOT usable: %s", H64(h), n, sc.whyNot.c_str()));
}

static void OnSwapchainDestroy(VkSwapchainKHR h) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_sc.find(h); if (it == g_sc.end()) return;
  Swapchain& sc = *it->second;
  for (auto& s : sc.slots) g_imgRef.erase(s.image);
  if (!sc.slots.empty()) g_retired.push_back({sc.dev, sc.fn, std::move(sc.slots), g_presents.load() + 16, std::move(sc.sharp)});
  Event(Fmt("swapchain 0x%llx destroyed; scratch images retired for 16 presents", H64(h)));
  g_sc.erase(it);
}

static void OnAcquired(VkSwapchainKHR h, uint32_t idx) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_sc.find(h); if (it != g_sc.end() && idx < it->second->slots.size()) it->second->slots[idx].qualSeen = 0;
}

template <class Info> static void RememberPass(VkRenderPass h, const Info* ci) {
  std::vector<AttDesc> v(ci->attachmentCount);
  for (uint32_t i = 0; i < ci->attachmentCount; ++i) { auto& a = ci->pAttachments[i]; v[i] = {a.format, a.samples, a.loadOp, a.storeOp, a.initialLayout, false}; }
  for (uint32_t s = 0; s < ci->subpassCount; ++s)
    for (uint32_t j = 0; j < ci->pSubpasses[s].colorAttachmentCount; ++j) { uint32_t at = ci->pSubpasses[s].pColorAttachments[j].attachment; if (at < v.size()) v[at].color = true; }
  std::lock_guard<std::mutex> lk(g_mu); g_passes[h] = std::move(v);
}

// ============================================================================================
// Present tick: key polling, INI reload, capture bookkeeping, scratch retirement
// ============================================================================================
static std::string HookTable() {
  std::string s;
  s += Fmt("%sGetInstanceProcAddr: export=%s calls=%llu\n", Indent(2), g_exportStateGIPA == 1 ? "hooked" : "NOT hooked", (unsigned long long)g_hitGIPA.load());
  s += Fmt("%sGetDeviceProcAddr:   export=%s calls=%llu\n", Indent(2), g_exportStateGDPA == 1 ? "hooked" : "NOT hooked", (unsigned long long)g_hitGDPA.load());
  for (int i = 0; i < HK_COUNT; ++i)
    s += Fmt("%s%-22s export=%-12s proc-address wrapper handed out=%-3s calls via proc-address=%llu via export=%llu\n", Indent(2), kHookName[i],
             g_exportState[i] == 1 ? "hooked" : g_exportState[i] == 2 ? "ATTACH FAILED" : "not exported", g_wrapped[i] ? "yes" : "no",
             (unsigned long long)g_hits[i][0].load(), (unsigned long long)g_hits[i][1].load());
  return s;
}

static void BeginCapture() {
  g_cap.id++; g_cap.frame = 1; g_cap.total = (uint32_t)g_cfg.captureFrames.load();
  { std::lock_guard<std::mutex> l(g_cap.mu); g_cap.lines.clear(); }
  g_cap.t0 = (uint64_t)(QpcMs() * 1000.0); g_cap.presentAtStart = g_presents.load();
  for (int i = 0; i < HK_COUNT; ++i) for (int p = 0; p < 2; ++p) g_cap.hitsAtStart[i][p] = g_hits[i][p].load();
  g_cap.gipaAtStart = g_hitGIPA.load(); g_cap.gdpaAtStart = g_hitGDPA.load();
  g_cap.fs.begins = g_cap.fs.passes = g_cap.fs.scPasses = g_cap.fs.qualifying = g_cap.fs.transformed = g_cap.fs.submits = g_cap.fs.executes = 0;

  std::string h = Fmt("[%s] CAPTURE #%u BEGIN  frames=%u key=0x%x\n", Now().c_str(), g_cap.id, g_cap.total, g_cfg.captureKey.load());
  h += Fmt("%sconfig: enabled=%d zoom=%.4f transformPassIndex=%d logPasses=%d logSubmits=%d logCmdBegin=%d\n", Indent(1), (int)g_cfg.enabled.load(), g_cfg.zoom.load(), g_cfg.passIndex.load(), (int)g_cfg.logPasses.load(), (int)g_cfg.logSubmits.load(), (int)g_cfg.logCmdBegin.load());
  h += Fmt("%sprocess: pid=%lu presents so far=%llu\n", Indent(1), (unsigned long)GetCurrentProcessId(), (unsigned long long)g_presents.load());
  {
    std::lock_guard<std::mutex> lk(g_mu);
    h += Fmt("%sphysical device=0x%llx first device=0x%llx tracked views=%zu framebuffers=%zu render passes=%zu\n", Indent(1), H64(g_pd), H64(g_firstDevice), g_views.size(), g_fbs.size(), g_passes.size());
    h += Fmt("%sswapchains: %zu\n", Indent(1), g_sc.size());
    for (auto& kv : g_sc) {
      const Swapchain& sc = *kv.second;
      h += Fmt("%s0x%llx %ux%u fmt=%s(%d) presentMode=%d minImages=%u images=%zu usage requested=0x%x actual=0x%x bumped=%s usable=%s%s%s sharpen=%s\n", Indent(2), H64(sc.h), sc.ext.width, sc.ext.height, FormatName((int)sc.fmt), (int)sc.fmt, (int)sc.mode, sc.minImages, sc.slots.size(), (unsigned)sc.requestedUsage, (unsigned)sc.usage, sc.bumped ? "yes" : "no", sc.usable ? "yes" : "no", sc.usable ? "" : " reason: ", sc.usable ? "" : sc.whyNot.c_str(), SharpStatus(sc).c_str());
      for (size_t i = 0; i < sc.slots.size(); ++i) h += Fmt("%simage[%zu]=0x%llx scratch=0x%llx\n", Indent(3), i, H64(sc.slots[i].image), H64(sc.slots[i].scratch));
    }
  }
  h += Fmt("%shooks (totals since launch):\n%s", Indent(1), HookTable().c_str());
  h += Fmt("%srecent events:\n", Indent(1));
  { std::lock_guard<std::mutex> l(g_evMu); for (auto& e : g_events) h += Fmt("%s%s\n", Indent(2), e.c_str()); if (g_events.empty()) h += Fmt("%s(none)\n", Indent(2)); }
  AppendLog(h);
  g_cap.active.store(true);
}

static void FlushFrame() {
  std::vector<std::string> lines;
  { std::lock_guard<std::mutex> l(g_cap.mu); lines.swap(g_cap.lines); }
  std::string b = Fmt("\n%s[%s] frame %u/%u  (ends at present #%llu)\n", Indent(1), Now().c_str(), g_cap.frame, g_cap.total, (unsigned long long)g_presents.load());
  b += Fmt("%ssummary: beginCommandBuffer-or-pass calls=%u  passes=%u  passes touching a swapchain image=%u  qualifying=%u  transformed=%u  queueSubmits=%u  executeCommands=%u\n", Indent(2),
           g_cap.fs.begins.load(), g_cap.fs.passes.load(), g_cap.fs.scPasses.load(), g_cap.fs.qualifying.load(), g_cap.fs.transformed.load(), g_cap.fs.submits.load(), g_cap.fs.executes.load());
  for (auto& l : lines) b += l + "\n";
  g_cap.fs.begins = g_cap.fs.passes = g_cap.fs.scPasses = g_cap.fs.qualifying = g_cap.fs.transformed = g_cap.fs.submits = g_cap.fs.executes = 0;
  if (g_cap.frame >= g_cap.total) {
    g_cap.active.store(false);
    b += Fmt("\n[%s] CAPTURE #%u END  presents covered=%llu\n", Now().c_str(), g_cap.id, (unsigned long long)(g_presents.load() - g_cap.presentAtStart));
    b += Fmt("%shook calls during this capture (proc-address / export):\n", Indent(1));
    for (int i = 0; i < HK_COUNT; ++i) {
      uint64_t a = g_hits[i][0].load() - g_cap.hitsAtStart[i][0], e = g_hits[i][1].load() - g_cap.hitsAtStart[i][1];
      if (a || e) b += Fmt("%s%-22s %llu / %llu\n", Indent(2), kHookName[i], (unsigned long long)a, (unsigned long long)e);
    }
    b += "\n";
  }
  g_cap.frame++;
  AppendLog(b);
}

static void OnPresent(const VkPresentInfoKHR* pi) {
  const uint64_t n = ++g_presents;
  if (Capturing()) FlushFrame();
  // key edge-detect, only while this process owns the foreground window
  const int vk = g_cfg.captureKey.load();
  const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
  if (down && !g_cap.keyDown && !Capturing()) {
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid == GetCurrentProcessId()) BeginCapture();
  }
  g_cap.keyDown = down;
  if (g_frameQual.exchange(false)) { if (g_stableStreak < 100000) ++g_stableStreak; } else if (!g_cfg.holdZoom.load()) g_stableStreak = 0;
  MaybeReloadIni();
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (pi) for (uint32_t i = 0; i < pi->swapchainCount; ++i) {
      auto it = g_sc.find(pi->pSwapchains[i]);
      if (it != g_sc.end() && pi->pImageIndices[i] < it->second->slots.size()) it->second->slots[pi->pImageIndices[i]].qualSeen = 0;
    }
    for (size_t i = 0; i < g_retired.size();) {
      if (g_retired[i].freeAt <= n) { if (g_retired[i].sharp) sharpen::Destroy(*g_retired[i].sharp); FreeSlots(g_retired[i].dev, g_retired[i].fn, g_retired[i].slots); g_retired.erase(g_retired.begin() + i); } else ++i;
    }
  }
}

// ============================================================================================
// Hook bodies. P = 0 proc-address path, 1 export path.
// ============================================================================================
template <class T> static inline T Pick(const Pair<T>& p, int path) { return path ? p.ex : p.gp; }

// Who writes the swapchain image before the UI pass? Barriers, copies, blits and clears that name a swapchain
// image are logged (capture only). Layouts tell the story: GENERAL = compute/storage, TRANSFER_DST = copy/blit,
// COLOR_ATTACHMENT_OPTIMAL = draw.
static bool SwapchainImageIndex(VkImage img, long long& idx) {
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_imgRef.find(img); if (it == g_imgRef.end()) return false;
  idx = it->second.second; return true;
}
static void LogBarrier1(VkCommandBuffer c, VkPipelineStageFlags ss, VkPipelineStageFlags ds, uint32_t n, const VkImageMemoryBarrier* b) {
  if (!n || !b) return;
  for (uint32_t i = 0; i < n; ++i) { long long idx; if (!SwapchainImageIndex(b[i].image, idx)) continue;
    CapLine(Fmt("%svkCmdPipelineBarrier cmd=0x%llx swapchain image[%lld]=0x%llx layout %s -> %s  stages 0x%x -> 0x%x  access 0x%x -> 0x%x", Indent(2), H64(c), idx, H64(b[i].image), LayoutName((int)b[i].oldLayout), LayoutName((int)b[i].newLayout), (unsigned)ss, (unsigned)ds, (unsigned)b[i].srcAccessMask, (unsigned)b[i].dstAccessMask)); }
}
static void LogBarrier2(VkCommandBuffer c, const VkDependencyInfo* di) {
  if (!di) return;
  for (uint32_t i = 0; i < di->imageMemoryBarrierCount; ++i) { const VkImageMemoryBarrier2& b = di->pImageMemoryBarriers[i]; long long idx; if (!SwapchainImageIndex(b.image, idx)) continue;
    CapLine(Fmt("%svkCmdPipelineBarrier2 cmd=0x%llx swapchain image[%lld]=0x%llx layout %s -> %s  stages 0x%llx -> 0x%llx  access 0x%llx -> 0x%llx", Indent(2), H64(c), idx, H64(b.image), LayoutName((int)b.oldLayout), LayoutName((int)b.newLayout), (unsigned long long)b.srcStageMask, (unsigned long long)b.dstStageMask, (unsigned long long)b.srcAccessMask, (unsigned long long)b.dstAccessMask)); }
}
static void LogImageWrite(VkCommandBuffer c, const char* api, VkImage dst, VkImageLayout layout, const char* extra) {
  long long idx; if (!SwapchainImageIndex(dst, idx)) return;
  CapLine(Fmt("%s%s cmd=0x%llx WRITES swapchain image[%lld]=0x%llx dstLayout=%s %s", Indent(2), api, H64(c), idx, H64(dst), LayoutName((int)layout), extra));
}

template <int P> struct H {
  static VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* out) {
    Hit(HK_CreateDevice, P);
    VkResult r = Pick(P_CreateDevice, P)(pd, ci, a, out);
    if (r == VK_SUCCESS && out && *out) OnDeviceCreated(pd, *out);
    return r;
  }
  static VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice d, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* a, VkSwapchainKHR* out) {
    Hit(HK_CreateSwapchainKHR, P);
    auto real = Pick(P_CreateSwapchainKHR, P);
    if (!ci) return real(d, ci, a, out);
    const VkImageUsageFlags want = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkSwapchainCreateInfoKHR mod = *ci; bool bumped = false;
    if ((ci->imageUsage & want) != want) {
      // Only ask for the extra usage if the surface says it supports it.
      VkSurfaceCapabilitiesKHR caps{}; bool known = g_fnSurfCaps && g_pd && g_fnSurfCaps(g_pd, ci->surface, &caps) == VK_SUCCESS;
      if (known && (caps.supportedUsageFlags & want) == want) { mod.imageUsage |= want; bumped = true; }
      else Event(Fmt("swapchain usage NOT bumped: surface support %s (supportedUsageFlags=0x%x); transform will be unavailable", known ? "lacks TRANSFER_SRC|DST" : "could not be queried", known ? (unsigned)caps.supportedUsageFlags : 0u));
    }
    VkResult r = real(d, bumped ? &mod : ci, a, out);
    if (r != VK_SUCCESS && bumped) { Event(Fmt("swapchain creation rejected the TRANSFER usage bump (result %d); retrying with the game's own usage", (int)r)); bumped = false; r = real(d, ci, a, out); }
    if (r == VK_SUCCESS && out) OnSwapchainCreated(d, *out, bumped ? mod : *ci, ci->imageUsage, bumped);
    return r;
  }
  static VkResult VKAPI_CALL GetSwapchainImagesKHR(VkDevice d, VkSwapchainKHR sc, uint32_t* n, VkImage* imgs) {
    Hit(HK_GetSwapchainImagesKHR, P);
    VkResult r = Pick(P_GetSwapchainImagesKHR, P)(d, sc, n, imgs);
    if (imgs && n && (r == VK_SUCCESS || r == VK_INCOMPLETE)) OnSwapchainImages(sc, *n, imgs);
    return r;
  }
  static void VKAPI_CALL DestroySwapchainKHR(VkDevice d, VkSwapchainKHR sc, const VkAllocationCallbacks* a) {
    Hit(HK_DestroySwapchainKHR, P); OnSwapchainDestroy(sc); Pick(P_DestroySwapchainKHR, P)(d, sc, a);
  }
  static VkResult VKAPI_CALL AcquireNextImageKHR(VkDevice d, VkSwapchainKHR sc, uint64_t t, VkSemaphore s, VkFence f, uint32_t* idx) {
    Hit(HK_AcquireNextImageKHR, P);
    VkResult r = Pick(P_AcquireNextImageKHR, P)(d, sc, t, s, f, idx);
    if ((r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) && idx) OnAcquired(sc, *idx);
    return r;
  }
  static VkResult VKAPI_CALL AcquireNextImage2KHR(VkDevice d, const VkAcquireNextImageInfoKHR* info, uint32_t* idx) {
    Hit(HK_AcquireNextImage2KHR, P);
    VkResult r = Pick(P_AcquireNextImage2KHR, P)(d, info, idx);
    if ((r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) && idx && info) OnAcquired(info->swapchain, *idx);
    return r;
  }
  static VkResult VKAPI_CALL QueuePresentKHR(VkQueue q, const VkPresentInfoKHR* pi) {
    Hit(HK_QueuePresentKHR, P); OnPresent(pi); return Pick(P_QueuePresentKHR, P)(q, pi);
  }
  static VkResult VKAPI_CALL CreateImageView(VkDevice d, const VkImageViewCreateInfo* ci, const VkAllocationCallbacks* a, VkImageView* out) {
    Hit(HK_CreateImageView, P);
    VkResult r = Pick(P_CreateImageView, P)(d, ci, a, out);
    if (r == VK_SUCCESS && ci && out) { std::lock_guard<std::mutex> l(g_mu); g_views[*out] = {ci->image, ci->format, ci->subresourceRange}; }
    return r;
  }
  static void VKAPI_CALL DestroyImageView(VkDevice d, VkImageView v, const VkAllocationCallbacks* a) {
    Hit(HK_DestroyImageView, P); { std::lock_guard<std::mutex> l(g_mu); g_views.erase(v); } Pick(P_DestroyImageView, P)(d, v, a);
  }
  static VkResult VKAPI_CALL CreateFramebuffer(VkDevice d, const VkFramebufferCreateInfo* ci, const VkAllocationCallbacks* a, VkFramebuffer* out) {
    Hit(HK_CreateFramebuffer, P);
    VkResult r = Pick(P_CreateFramebuffer, P)(d, ci, a, out);
    if (r == VK_SUCCESS && ci && out) {
      FbInfo f{ci->renderPass, ci->width, ci->height, ci->layers, (ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) != 0, {}};
      if (!f.imageless && ci->pAttachments) f.views.assign(ci->pAttachments, ci->pAttachments + ci->attachmentCount);
      std::lock_guard<std::mutex> l(g_mu); g_fbs[*out] = std::move(f);
    }
    return r;
  }
  static void VKAPI_CALL DestroyFramebuffer(VkDevice d, VkFramebuffer f, const VkAllocationCallbacks* a) {
    Hit(HK_DestroyFramebuffer, P); { std::lock_guard<std::mutex> l(g_mu); g_fbs.erase(f); } Pick(P_DestroyFramebuffer, P)(d, f, a);
  }
  static VkResult VKAPI_CALL CreateRenderPass(VkDevice d, const VkRenderPassCreateInfo* ci, const VkAllocationCallbacks* a, VkRenderPass* out) {
    Hit(HK_CreateRenderPass, P); VkResult r = Pick(P_CreateRenderPass, P)(d, ci, a, out);
    if (r == VK_SUCCESS && ci && out) RememberPass(*out, ci);
    return r;
  }
  static VkResult VKAPI_CALL CreateRenderPass2(VkDevice d, const VkRenderPassCreateInfo2* ci, const VkAllocationCallbacks* a, VkRenderPass* out) {
    Hit(HK_CreateRenderPass2, P); VkResult r = Pick(P_CreateRenderPass2, P)(d, ci, a, out);
    if (r == VK_SUCCESS && ci && out) RememberPass(*out, ci);
    return r;
  }
  static void VKAPI_CALL DestroyRenderPass(VkDevice d, VkRenderPass p, const VkAllocationCallbacks* a) {
    Hit(HK_DestroyRenderPass, P); { std::lock_guard<std::mutex> l(g_mu); g_passes.erase(p); } Pick(P_DestroyRenderPass, P)(d, p, a);
  }
  static void VKAPI_CALL CmdBeginRenderPass(VkCommandBuffer c, const VkRenderPassBeginInfo* bi, VkSubpassContents sc) {
    Hit(HK_CmdBeginRenderPass, P); OnBeginRenderPass(c, bi, "vkCmdBeginRenderPass"); Pick(P_CmdBeginRenderPass, P)(c, bi, sc);
  }
  static void VKAPI_CALL CmdBeginRenderPass2(VkCommandBuffer c, const VkRenderPassBeginInfo* bi, const VkSubpassBeginInfo* si) {
    Hit(HK_CmdBeginRenderPass2, P); OnBeginRenderPass(c, bi, "vkCmdBeginRenderPass2"); Pick(P_CmdBeginRenderPass2, P)(c, bi, si);
  }
  static void VKAPI_CALL CmdBeginRendering(VkCommandBuffer c, const VkRenderingInfo* ri) {
    Hit(HK_CmdBeginRendering, P); OnBeginRendering(c, ri, "vkCmdBeginRendering"); Pick(P_CmdBeginRendering, P)(c, ri);
  }
  static VkResult VKAPI_CALL QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f) {
    Hit(HK_QueueSubmit, P); g_cap.fs.submits++;
    if (Capturing() && g_cfg.logSubmits.load() && s) {
      std::string t = Fmt("%svkQueueSubmit queue=0x%llx batches=%u fence=0x%llx", Indent(2), H64(q), n, H64(f));
      for (uint32_t i = 0; i < n; ++i) { t += Fmt("\n%sbatch[%u] wait=%u signal=%u cmds=[", Indent(3), i, s[i].waitSemaphoreCount, s[i].signalSemaphoreCount); for (uint32_t j = 0; j < s[i].commandBufferCount; ++j) t += Fmt("%s0x%llx", j ? " " : "", H64(s[i].pCommandBuffers[j])); t += "]"; }
      CapLine(t);
    }
    return Pick(P_QueueSubmit, P)(q, n, s, f);
  }
  static VkResult VKAPI_CALL QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    Hit(HK_QueueSubmit2, P); g_cap.fs.submits++;
    if (Capturing() && g_cfg.logSubmits.load() && s) {
      std::string t = Fmt("%svkQueueSubmit2 queue=0x%llx batches=%u fence=0x%llx", Indent(2), H64(q), n, H64(f));
      for (uint32_t i = 0; i < n; ++i) { t += Fmt("\n%sbatch[%u] wait=%u signal=%u cmds=[", Indent(3), i, s[i].waitSemaphoreInfoCount, s[i].signalSemaphoreInfoCount); for (uint32_t j = 0; j < s[i].commandBufferInfoCount; ++j) t += Fmt("%s0x%llx", j ? " " : "", H64(s[i].pCommandBufferInfos[j].commandBuffer)); t += "]"; }
      CapLine(t);
    }
    return Pick(P_QueueSubmit2, P)(q, n, s, f);
  }
  static void VKAPI_CALL CmdExecuteCommands(VkCommandBuffer c, uint32_t n, const VkCommandBuffer* cbs) {
    Hit(HK_CmdExecuteCommands, P); g_cap.fs.executes++;
    if (Capturing() && g_cfg.logSubmits.load() && cbs) { std::string t = Fmt("%svkCmdExecuteCommands primary=0x%llx secondaries=[", Indent(2), H64(c)); for (uint32_t i = 0; i < n; ++i) t += Fmt("%s0x%llx", i ? " " : "", H64(cbs[i])); CapLine(t + "]"); }
    Pick(P_CmdExecuteCommands, P)(c, n, cbs);
  }
  static void VKAPI_CALL CmdPipelineBarrier(VkCommandBuffer c, VkPipelineStageFlags ss, VkPipelineStageFlags ds, VkDependencyFlags df, uint32_t nm, const VkMemoryBarrier* m, uint32_t nb, const VkBufferMemoryBarrier* b, uint32_t ni, const VkImageMemoryBarrier* i) {
    Hit(HK_CmdPipelineBarrier, P);
    if (Capturing() && g_cfg.logPasses.load()) LogBarrier1(c, ss, ds, ni, i);
    Pick(P_CmdPipelineBarrier, P)(c, ss, ds, df, nm, m, nb, b, ni, i);
  }
  static void VKAPI_CALL CmdPipelineBarrier2(VkCommandBuffer c, const VkDependencyInfo* di) {
    Hit(HK_CmdPipelineBarrier2, P);
    if (Capturing() && g_cfg.logPasses.load()) LogBarrier2(c, di);
    Pick(P_CmdPipelineBarrier2, P)(c, di);
  }
  static void VKAPI_CALL CmdCopyImage(VkCommandBuffer c, VkImage si, VkImageLayout sl, VkImage di, VkImageLayout dl, uint32_t n, const VkImageCopy* r) {
    Hit(HK_CmdCopyImage, P);
    if (Capturing() && g_cfg.logPasses.load()) LogImageWrite(c, "vkCmdCopyImage", di, dl, Fmt("srcImage=0x%llx regions=%u", H64(si), n).c_str());
    Pick(P_CmdCopyImage, P)(c, si, sl, di, dl, n, r);
  }
  static void VKAPI_CALL CmdBindPipeline(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipeline p) { Hit(HK_CmdBindPipeline, P); Shadowed(c, [&](sharpen::Shadow& s) { s.OnPipeline(bp, p); }); Pick(P_CmdBindPipeline, P)(c, bp, p); }
  static void VKAPI_CALL CmdBindDescriptorSets(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet* sets, uint32_t dc, const uint32_t* dyn) {
    Hit(HK_CmdBindDescriptorSets, P); Shadowed(c, [&](sharpen::Shadow& s) { s.OnSets(bp, l, first, n, sets, dc, dyn); }); Pick(P_CmdBindDescriptorSets, P)(c, bp, l, first, n, sets, dc, dyn); }
  static void VKAPI_CALL CmdPushConstants(VkCommandBuffer c, VkPipelineLayout l, VkShaderStageFlags st, uint32_t off, uint32_t size, const void* d) { Hit(HK_CmdPushConstants, P); Shadowed(c, [&](sharpen::Shadow& s) { s.OnPush(l, st, off, size, d); }); Pick(P_CmdPushConstants, P)(c, l, st, off, size, d); }
  static void VKAPI_CALL CmdSetViewport(VkCommandBuffer c, uint32_t first, uint32_t n, const VkViewport* v) { Hit(HK_CmdSetViewport, P); Shadowed(c, [&](sharpen::Shadow& s) { s.OnViewports(first, n, v); }); Pick(P_CmdSetViewport, P)(c, first, n, v); }
  static void VKAPI_CALL CmdSetScissor(VkCommandBuffer c, uint32_t first, uint32_t n, const VkRect2D* v) { Hit(HK_CmdSetScissor, P); Shadowed(c, [&](sharpen::Shadow& s) { s.OnScissors(first, n, v); }); Pick(P_CmdSetScissor, P)(c, first, n, v); }
  static void VKAPI_CALL CmdSetViewportWithCount(VkCommandBuffer c, uint32_t n, const VkViewport* v) { Hit(HK_CmdSetViewportWithCount, P); Shadowed(c, [&](sharpen::Shadow& s) { s.OnViewports(0, n, v); }); Pick(P_CmdSetViewportWithCount, P)(c, n, v); }
  static void VKAPI_CALL CmdSetScissorWithCount(VkCommandBuffer c, uint32_t n, const VkRect2D* v) { Hit(HK_CmdSetScissorWithCount, P); Shadowed(c, [&](sharpen::Shadow& s) { s.OnScissors(0, n, v); }); Pick(P_CmdSetScissorWithCount, P)(c, n, v); }
  static void VKAPI_CALL CmdBlitImage(VkCommandBuffer c, VkImage si, VkImageLayout sl, VkImage di, VkImageLayout dl, uint32_t n, const VkImageBlit* r, VkFilter f) {
    Hit(HK_CmdBlitImage, P);
    if (Capturing() && g_cfg.logPasses.load()) LogImageWrite(c, "vkCmdBlitImage", di, dl, Fmt("srcImage=0x%llx regions=%u filter=%d", H64(si), n, (int)f).c_str());
    Pick(P_CmdBlitImage, P)(c, si, sl, di, dl, n, r, f);
  }
  static void VKAPI_CALL CmdClearColorImage(VkCommandBuffer c, VkImage img, VkImageLayout l, const VkClearColorValue* v, uint32_t n, const VkImageSubresourceRange* r) {
    Hit(HK_CmdClearColorImage, P);
    if (Capturing() && g_cfg.logPasses.load()) LogImageWrite(c, "vkCmdClearColorImage", img, l, "");
    Pick(P_CmdClearColorImage, P)(c, img, l, v, n, r);
  }
  static VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer c, const VkCommandBufferBeginInfo* bi) {
    Hit(HK_BeginCommandBuffer, P);
    if (!g_cs.empty()) g_cs.erase(c);
    if (Capturing() && g_cfg.logCmdBegin.load()) CapLine(Fmt("%svkBeginCommandBuffer cmd=0x%llx flags=0x%x", Indent(2), H64(c), bi ? (unsigned)bi->flags : 0u));
    return Pick(P_BeginCommandBuffer, P)(c, bi);
  }
};

// ============================================================================================
// Proc-address hooks
// ============================================================================================
static PFN_vkVoidFunction WrapProc(const char* name, PFN_vkVoidFunction real) {
  if (!real || !name || name[0] != 'v' || name[1] != 'k') return real;
  const char* n = name + 2;
  // KHR aliases of core 1.2/1.3 functions share one wrapper; the driver implements both with one function.
  static const struct { const char* alias; const char* core; } aliases[] = {
    {"CreateRenderPass2KHR", "CreateRenderPass2"}, {"CmdBeginRenderPass2KHR", "CmdBeginRenderPass2"},
    {"CmdBeginRenderingKHR", "CmdBeginRendering"}, {"QueueSubmit2KHR", "QueueSubmit2"},
    {"CmdPipelineBarrier2KHR", "CmdPipelineBarrier2"}};
  for (auto& a : aliases) if (!strcmp(n, a.alias)) { n = a.core; break; }
#define X(h) if (!strcmp(n, #h)) { if (!P_##h.gp) P_##h.gp = (PFN_vk##h)real; g_wrapped[HK_##h] = true; return (PFN_vkVoidFunction)&H<0>::h; }
  OVS_HOOKS(X)
#undef X
  return real;
}
static PFN_vkVoidFunction VKAPI_CALL h_GDPA(VkDevice d, const char* name);
static PFN_vkVoidFunction VKAPI_CALL h_GIPA(VkInstance i, const char* name) {
  g_hitGIPA.fetch_add(1, std::memory_order_relaxed);
  PFN_vkVoidFunction real = o_GIPA(i, name);
  if (!real || !name) return real;
  if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)&h_GIPA;
  if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)&h_GDPA;
  return WrapProc(name, real);
}
static PFN_vkVoidFunction VKAPI_CALL h_GDPA(VkDevice d, const char* name) {
  g_hitGDPA.fetch_add(1, std::memory_order_relaxed);
  PFN_vkVoidFunction real = o_GDPA(d, name);
  if (!real || !name) return real;
  if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)&h_GDPA;
  return WrapProc(name, real);
}

// ============================================================================================
// Install
// ============================================================================================
static bool AttachOne(void** target, void* detour) {
  if (DetourTransactionBegin() != NO_ERROR) return false;
  DetourUpdateThread(GetCurrentThread());
  if (DetourAttach(target, detour) != NO_ERROR) { DetourTransactionAbort(); return false; }
  return DetourTransactionCommit() == NO_ERROR;
}
template <class T> static int AttachExport(HMODULE vk, const char* name, Pair<T>& p, void* detour) {
  p.ex = (T)GetProcAddress(vk, name);
  if (!p.ex) return 0;
  return AttachOne((void**)&p.ex, detour) ? 1 : 2;
}

static bool Install(HMODULE self) {
  g_dir = ModuleDir(self); g_iniPath = g_dir + L"RDR2Overscan.ini"; g_logPath = g_dir + L"RDR2Overscan.log";
  WriteDefaultIni(); LoadIni();
  WIN32_FILE_ATTRIBUTE_DATA d{}; if (GetFileAttributesExW(g_iniPath.c_str(), GetFileExInfoStandard, &d)) g_iniStamp = d.ftLastWriteTime;
  HMODULE vk = GetModuleHandleW(L"vulkan-1.dll"); if (!vk) vk = LoadLibraryW(L"vulkan-1.dll");
  if (!vk) { AppendLog(Fmt("[%s] INIT FAILED: vulkan-1.dll could not be loaded (error %lu)\n", Now().c_str(), GetLastError())); return false; }
  o_GIPA = (PFN_vkGetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");
  o_GDPA = (PFN_vkGetDeviceProcAddr)GetProcAddress(vk, "vkGetDeviceProcAddr");
  g_fnFormatProps = (PFN_vkGetPhysicalDeviceFormatProperties)GetProcAddress(vk, "vkGetPhysicalDeviceFormatProperties");
  g_fnMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)GetProcAddress(vk, "vkGetPhysicalDeviceMemoryProperties");
  g_fnSurfCaps = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)GetProcAddress(vk, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
  if (!o_GIPA || !o_GDPA) { AppendLog(Fmt("[%s] INIT FAILED: vkGetInstanceProcAddr/vkGetDeviceProcAddr exports missing\n", Now().c_str())); return false; }
  g_exportStateGIPA = AttachOne((void**)&o_GIPA, (void*)&h_GIPA) ? 1 : 2;
  g_exportStateGDPA = AttachOne((void**)&o_GDPA, (void*)&h_GDPA) ? 1 : 2;
#define X(n) g_exportState[HK_##n] = AttachExport(vk, "vk" #n, P_##n, (void*)&H<1>::n);
  OVS_HOOKS(X)
#undef X
  if (g_exportStateGIPA != 1 || g_exportStateGDPA != 1) { AppendLog(Fmt("[%s] INIT WARNING: proc-address hook attach failed (GIPA=%d GDPA=%d); only export-level hooks are active\n", Now().c_str(), g_exportStateGIPA, g_exportStateGDPA)); }
  Event("hooks installed");
  return true;
}

}  // namespace ovs

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(h);
    char exe[MAX_PATH]{}; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const char* base = strrchr(exe, '\\'); base = base ? base + 1 : exe;
    if (_stricmp(base, "RDR2.exe") != 0) return TRUE;   // load only inside the game
    ovs::Install(h);
  }
  return TRUE;
}
