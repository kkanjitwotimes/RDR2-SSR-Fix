// RDR2 Vulkan overscan v0.1.1 (ASI).
//
// What it does: just before the first full-screen render pass that LOADs the backbuffer (the point the
// reference FG mod uses as "pre-HUD"), it records  copy backbuffer -> private scratch, then
// blit scratch(centre crop) -> backbuffer (full image)  into the game's own command buffer. Everything
// the game draws afterwards (UI) lands on the zoomed image at its normal size.
//
// What it logs: nothing, until the capture key is pressed. Then it records CaptureFrames frames of
// pass / submit / hook information to RDR2Overscan.log (opened and closed per write, so the file can be
// edited or deleted while the game runs). The INI is re-read while the game runs.
//
// v0.1.1: PauseGapMaxFrames default 3 -> 30. The first pause after launch loads the pause menu for ~11 scene-less
// frames (later pauses: 2-3), so the tail of that gap stayed unzoomed; the present-time zoom now covers it.
//
// v0.1.0: alt-pass mode. Startup / NPC-focus camera frames have no depth-writing scene pass, only a swapchain LOAD pass
// with read-only depth at the same point of the frame. After AltEngageFrames such frames in a row that pass is zoomed
// instead; the mode ends as soon as a depth-writing scene pass is seen again (INI [Debug] AltPassMode / AltEngageFrames).
//
// v0.0.4: (1) PauseGapFix: frames that have no scene pass but directly follow scene frames (the 2-frame gap at the
// pause transition) are zoomed at vkQueuePresentKHR with an extra submit; (2) capture diagnostics: per-swapchain-pass
// draw counts, bound descriptor sets and the images they sample, so one capture shows what those gap frames draw.
// MenuCadenceGuard: the title menu alternates frames with a separate scene pass and frames that draw everything in one
// CLEAR pass; zooming only one kind made the UI flip size every frame. That cadence is detected and the zoom is suspended.
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
#include <algorithm>
#include <array>
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
  X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdSetViewport) X(CmdSetScissor) X(CmdSetViewportWithCount) X(CmdSetScissorWithCount) \
  X(CmdEndRenderPass) X(CmdEndRenderPass2) X(CmdEndRendering) X(CmdDraw) X(CmdDrawIndexed) X(CmdDrawIndirect) X(CmdDrawIndexedIndirect) X(CmdDrawIndirectCount) X(CmdDrawIndexedIndirectCount) \
  X(UpdateDescriptorSets) X(CreateImage) X(DestroyImage) X(GetDeviceQueue) X(GetDeviceQueue2)

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
static PFN_vkGetPhysicalDeviceQueueFamilyProperties g_fnQFProps;
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
  std::atomic<bool> pauseGap{true};     // v0.0.4: zoom scene-less frames that directly follow scene frames, at present time
  std::atomic<bool> menuGuard{true};    // suspend the zoom while scene / scene-less frames alternate (title menu)
  std::atomic<bool> altPass{true};      // alt-pass mode: scene frames that only have a read-only-depth swapchain pass (startup / NPC focus camera)
  std::atomic<int> altEngage{3};        // consecutive such frames before the alt pass is zoomed
  std::atomic<int> gapMax{30};           // longest run of scene-less frames that still gets the present-time zoom
  std::atomic<bool> logDraws{true};     // v0.0.4: per-swapchain-pass draw/descriptor summary in captures
  std::atomic<int> logDesc{1};          // 0 off, 1 track descriptor image writes only while capturing, 2 always (costly)
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
    "; RDR2 Vulkan overscan v0.1.1\r\n"
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
    "; Keeps the zoom on while the game is paused or a menu is open.\r\n"
    ";   1 = on (default), 0 = off.\r\n"
    "HoldZoomOnPause=1\r\n"
    "\r\n"
    "; Sharpens the zoomed 3D scene (AMD CAS). May cost a little performance.\r\n"
    ";   0.0 = off (default), 0.3 - 0.6 is a good start, 1.0 = strongest.\r\n"
    "Sharpness=0.0\r\n"
    "\r\n"
    "; Fixes the unzoomed flicker for a couple of frames right after pausing. Experimental.\r\n"
    ";   1 = on (default), 0 = off.\r\n"
    "PauseGapFix=1\r\n"
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
    "LogCommandBufferBegin=1\r\n"
    "\r\n"
    "; Per swapchain pass: draw counts, first draws and bound descriptor sets (1 = on, 0 = off).\r\n"
    "LogDrawDiagnostics=1\r\n"
    "\r\n"
    "; Resolve the images those descriptor sets sample (what a pass reads).\r\n"
    ";   0 = off, 1 = track only while a capture is running (cheap, may miss sets written earlier),\r\n"
    ";   2 = track always (complete, costs some CPU; use for one diagnostic capture).\r\n"
    "LogDescriptorImages=1\r\n"
    "\r\n"
    "\r\n"
    "[Debug]\r\n"
    "\r\n"
    "; Advanced settings. Leave them at their defaults unless you are debugging.\r\n"
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
    "; The title menu alternates between frames with a separate 3D scene pass and frames that\r\n"
    "; draw everything in one pass. Zooming only one kind makes the UI flip size every frame.\r\n"
    ";   1 = when frames alternate like that, the zoom is suspended (it eases back in after\r\n"
    ";       gameplay resumes). 0 = off.\r\n"
    "MenuCadenceGuard=1\r\n"
    "\r\n"
    "; Longest run of scene-less frames that still gets the PauseGapFix zoom.\r\n"
    "; Longer runs (loading screens, main menu) are left untouched. The first pause after\r\n"
    "; launch needs ~11 frames, later ones 2-3; too low a value leaves part of the first\r\n"
    "; pause menu unzoomed (max 60).\r\n"
    "PauseGapMaxFrames=30\r\n"
    "\r\n"
    "; Some camera modes (right after launch, NPC focus) have no scene pass that writes depth; the\r\n"
    "; scene is then followed by a pass with read-only depth instead. 1 = after AltEngageFrames such\r\n"
    "; frames in a row, that pass is zoomed. Leaves normal gameplay alone (it leaves this mode as\r\n"
    "; soon as a depth-writing scene pass is back). 0 = off.\r\n"
    "AltPassMode=1\r\n"
    "AltEngageFrames=3\r\n";
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
  // Keys that moved to [Debug]: the old [Overscan] value (from an older INI) is the default, so existing INIs keep working.
  auto DI = [&](const wchar_t* key, int def) { const int old = (int)GetPrivateProfileIntW(L"Overscan", key, def, f); return (int)GetPrivateProfileIntW(L"Debug", key, old, f); };
  g_cfg.engageFrames = 8; g_cfg.rampFrames = 30;   // fixed (no longer INI keys): zoom engages after 8 stable frames, eases in over 30
  g_cfg.requireDepth = DI(L"RequireDepthWrite", 1) != 0;
  g_cfg.holdZoom = GetPrivateProfileIntW(L"Overscan", L"HoldZoomOnPause", 1, f) != 0;
  { wchar_t ss[32]{}; GetPrivateProfileStringW(L"Overscan", L"Sharpness", L"0.0", ss, 32, f); double sv = wcstod(ss, nullptr); if (!(sv > 0.0)) sv = 0.0; if (sv > 1.0) sv = 1.0; g_cfg.sharp = sv; }
  int pi = DI(L"TransformPassIndex", 1); g_cfg.passIndex = pi < 1 ? 1 : pi;
  wchar_t ks[32]{}; GetPrivateProfileStringW(L"Log", L"CaptureKey", L"0xDC", ks, 32, f);
  long k = wcstol(ks, nullptr, 0); g_cfg.captureKey = k > 0 && k < 256 ? (int)k : 0xDC;
  int cf = (int)GetPrivateProfileIntW(L"Log", L"CaptureFrames", 3, f); g_cfg.captureFrames = cf < 1 ? 1 : cf > 30 ? 30 : cf;
  g_cfg.logPasses = GetPrivateProfileIntW(L"Log", L"LogPasses", 1, f) != 0;
  g_cfg.logSubmits = GetPrivateProfileIntW(L"Log", L"LogSubmits", 1, f) != 0;
  g_cfg.logCmdBegin = GetPrivateProfileIntW(L"Log", L"LogCommandBufferBegin", 1, f) != 0;
  g_cfg.pauseGap = GetPrivateProfileIntW(L"Overscan", L"PauseGapFix", 1, f) != 0;
  { int gm = DI(L"PauseGapMaxFrames", 30); g_cfg.gapMax = gm < 0 ? 0 : gm > 60 ? 60 : gm; }
  g_cfg.menuGuard = DI(L"MenuCadenceGuard", 1) != 0;
  g_cfg.altPass = DI(L"AltPassMode", 1) != 0;
  { int ae = DI(L"AltEngageFrames", 3); g_cfg.altEngage = ae < 1 ? 1 : ae > 60 ? 60 : ae; }
  g_cfg.logDraws = GetPrivateProfileIntW(L"Log", L"LogDrawDiagnostics", 1, f) != 0;
  { int ld = (int)GetPrivateProfileIntW(L"Log", L"LogDescriptorImages", 1, f); g_cfg.logDesc = ld < 0 ? 0 : ld > 2 ? 2 : ld; }
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
  // present-time zoom (PauseGapFix): own command pool / buffers / semaphores and an own submit
  PFN_vkCreateCommandPool createPool{}; PFN_vkDestroyCommandPool destroyPool{}; PFN_vkAllocateCommandBuffers allocCb{};
  PFN_vkBeginCommandBuffer beginCb{}; PFN_vkEndCommandBuffer endCb{}; PFN_vkResetCommandBuffer resetCb{};
  PFN_vkCreateSemaphore createSem{}; PFN_vkDestroySemaphore destroySem{}; PFN_vkQueueSubmit submit{};
  bool Complete() const { return createImage && destroyImage && getReq && alloc && freeMem && bind && barrier && copy && blit; }
  bool PzComplete() const { return createPool && destroyPool && allocCb && beginCb && endCb && resetCb && createSem && destroySem && submit; }
};
// Present-time zoom resources of one swapchain (created lazily the first time a gap frame needs them).
struct PZ { bool tried = false, ok = false; std::string why; uint32_t family = UINT32_MAX; VkCommandPool pool{}; std::vector<VkCommandBuffer> cb; std::vector<VkSemaphore> sem; };
struct Slot { VkImage image{}, scratch{}; VkDeviceMemory mem{}; uint32_t qualSeen = 0; };
struct Swapchain {
  VkSwapchainKHR h{}; VkDevice dev{}; VkFormat fmt{}; VkExtent2D ext{};
  VkImageUsageFlags requestedUsage = 0, usage = 0; VkPresentModeKHR mode{}; uint32_t minImages = 0;
  bool bumped = false, usable = false, sharpPossible = false; std::shared_ptr<sharpen::Res> sharp; PZ pz;
  std::string whyNot = "images not yet retrieved";
  DevFns fn; std::vector<Slot> slots;
};
struct ViewInfo { VkImage image; VkFormat format; VkImageSubresourceRange range; };
struct AttDesc { VkFormat format; VkSampleCountFlagBits samples; VkAttachmentLoadOp load; VkAttachmentStoreOp store; VkImageLayout initial; bool color; };
struct FbInfo { VkRenderPass pass; uint32_t w, h, layers; bool imageless; std::vector<VkImageView> views; };
struct Retired { VkDevice dev; DevFns fn; std::vector<Slot> slots; uint64_t freeAt; std::shared_ptr<sharpen::Res> sharp; PZ pz; };

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
static std::unordered_map<VkQueue, uint32_t> g_queueFam;          // queue -> family index (from vkGetDeviceQueue*), guarded by g_mu
struct ImgInfo { uint32_t w, h, layers, mips; VkFormat fmt; VkImageUsageFlags usage; VkSampleCountFlagBits samples; };
static std::unordered_map<VkImage, ImgInfo> g_imgInfo;            // every image the game creates (diagnostics), guarded by g_mu
// Image descriptors written through vkUpdateDescriptorSets: set -> (binding<<32|arrayElement) -> view. Guarded by g_dmu (never held together with g_mu).
static std::mutex g_dmu;
static std::unordered_map<VkDescriptorSet, std::unordered_map<uint64_t, VkImageView>> g_desc;
// Per-command-buffer diagnostics state (thread-local: one thread records a command buffer at a time).
struct PassDiag { uint32_t id = 0, draws = 0, indexed = 0, indirect = 0; std::vector<std::string> first; std::vector<VkDescriptorSet> sets; };
static thread_local std::unordered_map<VkCommandBuffer, PassDiag> g_pdiag;
static thread_local std::unordered_map<VkCommandBuffer, std::array<VkDescriptorSet, 8>> g_bound;
static std::atomic<uint32_t> g_diagSeq{0};
// Pause-gap tracking (updated once per present).
static std::atomic<uint32_t> g_gapRun{0};
static std::atomic<bool> g_hadScene{false};
static std::atomic<uint32_t> g_frameHist{0xFFFFFFFFu};   // one bit per presented frame, newest in bit 0: 1 = had a scene pass
static std::atomic<bool> g_altMode{false};               // alt-pass mode active: the read-only-depth swapchain pass is the transform point
static std::atomic<bool> g_frameCand{false};             // this frame had a full-area swapchain LOAD pass with read-only depth
static std::atomic<bool> g_frameWritable{false};         // this frame had a writable-depth qualifying pass (normal scene pass)
static std::atomic<uint32_t> g_altRun{0};                // consecutive frames with a candidate and no writable-depth pass
static std::atomic<bool> g_menuMode{false};              // MenuCadenceGuard: alternating scene / scene-less frames detected
static int ZeroRuns(uint32_t h, int n) { int runs = 0; bool in = false; for (int i = n - 1; i >= 0; --i) { const bool z = !((h >> i) & 1u); if (z && !in) ++runs; in = z; } return runs; }
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
// High-frequency hooks (draws, descriptor writes) only count their hits while a capture runs.
static inline void HitC(int id, int path) { if (Capturing()) Hit(id, path); }
static void CapLine(const std::string& s) {
  size_t ind = s.find_first_not_of(' ');  // keep the indent in front of the stamp so nesting reads left-to-right
  if (ind == std::string::npos) ind = 0;
  std::string line = std::string(ind, ' ') + Fmt("[+%8.3fms tid=%5lu] ", QpcMs() - (double)g_cap.t0 / 1000.0, (unsigned long)GetCurrentThreadId()) + s.substr(ind);
  std::lock_guard<std::mutex> l(g_cap.mu); g_cap.lines.push_back(std::move(line));
}
#define H64(x) ((unsigned long long)(uintptr_t)(x))

// ============================================================================================
// Diagnostics (capture only): what does each swapchain pass draw and sample?
// ============================================================================================
static void TrackDescWrites(uint32_t n, const VkWriteDescriptorSet* w) {
  if (!w) return;
  std::lock_guard<std::mutex> l(g_dmu);
  if (g_desc.size() > 300000) g_desc.clear();
  for (uint32_t i = 0; i < n; ++i) {
    const VkWriteDescriptorSet& x = w[i];
    if (!x.pImageInfo) continue;
    if (x.descriptorType != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && x.descriptorType != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
        x.descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && x.descriptorType != VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) continue;
    auto& m = g_desc[x.dstSet];
    for (uint32_t j = 0; j < x.descriptorCount; ++j) m[((uint64_t)x.dstBinding << 32) | (uint64_t)(x.dstArrayElement + j)] = x.pImageInfo[j].imageView;
  }
}
static inline bool DescTracking() { const int m = g_cfg.logDesc.load(); return m == 2 || (m == 1 && Capturing()); }

static void DiagBegin(VkCommandBuffer cmd, uint32_t id) {
  PassDiag d; d.id = id;
  auto b = g_bound.find(cmd);
  if (b != g_bound.end()) for (VkDescriptorSet s : b->second) if (s) d.sets.push_back(s);
  g_pdiag[cmd] = std::move(d);
}
static void DiagSets(VkCommandBuffer cmd, uint32_t first, uint32_t n, const VkDescriptorSet* sets) {
  if (!sets) return;
  auto& arr = g_bound[cmd];
  for (uint32_t i = 0; i < n; ++i) if (first + i < arr.size()) arr[first + i] = sets[i];
  auto it = g_pdiag.find(cmd); if (it == g_pdiag.end()) return;
  for (uint32_t i = 0; i < n; ++i) {
    bool dup = false; for (VkDescriptorSet s : it->second.sets) if (s == sets[i]) dup = true;
    if (!dup && it->second.sets.size() < 16) it->second.sets.push_back(sets[i]);
  }
}
static void DiagDraw(VkCommandBuffer cmd, int kind, uint32_t a, uint32_t b) {   // kind 0 draw, 1 indexed, 2 indirect
  auto it = g_pdiag.find(cmd); if (it == g_pdiag.end()) return;
  PassDiag& d = it->second;
  if (kind == 0) ++d.draws; else if (kind == 1) ++d.indexed; else ++d.indirect;
  if (d.first.size() < 3 && kind != 2) d.first.push_back(Fmt("%s count=%u instances=%u", kind ? "drawIndexed" : "draw", a, b));
}
// Caller holds g_mu.
static std::string DescribeView(VkImageView v) {
  auto vi = g_views.find(v);
  if (vi == g_views.end()) return Fmt("view 0x%llx (view not tracked)", H64(v));
  const VkImage img = vi->second.image;
  auto ref = g_imgRef.find(img);
  if (ref != g_imgRef.end()) return Fmt("SWAPCHAIN image[%u] 0x%llx (view fmt %s)", ref->second.second, H64(img), FormatName((int)vi->second.format));
  auto ii = g_imgInfo.find(img);
  if (ii == g_imgInfo.end()) return Fmt("image 0x%llx (image not tracked) view fmt %s", H64(img), FormatName((int)vi->second.format));
  const ImgInfo& m = ii->second;
  return Fmt("image 0x%llx %ux%u fmt=%s(%d) %s%s%s%s%s%s", H64(img), m.w, m.h, FormatName((int)m.fmt), (int)m.fmt, m.layers > 1 ? "layers>1 " : "", m.mips > 1 ? "mips>1 " : "",
             (m.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ? "RT " : "", (m.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ? "DS " : "",
             (m.usage & VK_IMAGE_USAGE_STORAGE_BIT) ? "storage " : "", m.samples != VK_SAMPLE_COUNT_1_BIT ? "MSAA" : "");
}
static void DiagEnd(VkCommandBuffer cmd) {
  auto it = g_pdiag.find(cmd); if (it == g_pdiag.end()) return;
  PassDiag d = std::move(it->second); g_pdiag.erase(it);
  std::string f; for (size_t i = 0; i < d.first.size(); ++i) f += (i ? "; " : "") + d.first[i];
  CapLine(Fmt("%spass#%u END cmd=0x%llx draws=%u drawsIndexed=%u drawsIndirect=%u firstDraws=[%s] descriptorSetsSeen=%zu", Indent(3), d.id, H64(cmd), d.draws, d.indexed, d.indirect, f.c_str(), d.sets.size()));
  if (!g_cfg.logDesc.load()) return;
  struct Row { VkDescriptorSet set; uint64_t key; VkImageView view; };
  std::vector<Row> rows; std::vector<VkDescriptorSet> unknown;
  { std::lock_guard<std::mutex> l(g_dmu);
    for (VkDescriptorSet s : d.sets) { auto m = g_desc.find(s); if (m == g_desc.end() || m->second.empty()) { unknown.push_back(s); continue; }
      for (auto& kv : m->second) if (kv.second && rows.size() < 40) rows.push_back({s, kv.first, kv.second}); } }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.set != b.set ? a.set < b.set : a.key < b.key; });
  std::lock_guard<std::mutex> l(g_mu);
  for (const Row& r : rows) CapLine(Fmt("%spass#%u samples set 0x%llx binding %u[%u] -> %s", Indent(3), d.id, H64(r.set), (unsigned)(r.key >> 32), (unsigned)(r.key & 0xffffffffu), DescribeView(r.view).c_str()));
  if (!unknown.empty()) { std::string u; for (size_t i = 0; i < unknown.size() && i < 8; ++i) u += Fmt("%s0x%llx", i ? " " : "", H64(unknown[i])); CapLine(Fmt("%spass#%u sets with no known image descriptors (LogDescriptorImages=%d): %s", Indent(3), d.id, g_cfg.logDesc.load(), u.c_str())); }
}

// ============================================================================================
// Scratch image management
// ============================================================================================
static void LoadDevFns(VkDevice d, DevFns& f) {
#define L(field, name) f.field = (PFN_vk##name)o_GDPA(d, "vk" #name)
  L(createImage, CreateImage); L(destroyImage, DestroyImage); L(getReq, GetImageMemoryRequirements);
  L(alloc, AllocateMemory); L(freeMem, FreeMemory); L(bind, BindImageMemory);
  L(barrier, CmdPipelineBarrier); L(copy, CmdCopyImage); L(blit, CmdBlitImage);
  L(createPool, CreateCommandPool); L(destroyPool, DestroyCommandPool); L(allocCb, AllocateCommandBuffers);
  L(beginCb, BeginCommandBuffer); L(endCb, EndCommandBuffer); L(resetCb, ResetCommandBuffer);
  L(createSem, CreateSemaphore); L(destroySem, DestroySemaphore); L(submit, QueueSubmit);
#undef L
}
static void FreePZ(VkDevice d, const DevFns& f, PZ& pz) {
  for (VkSemaphore s : pz.sem) if (s && f.destroySem) f.destroySem(d, s, nullptr);
  if (pz.pool && f.destroyPool) f.destroyPool(d, pz.pool, nullptr);   // frees its command buffers too
  pz.sem.clear(); pz.cb.clear(); pz.pool = VK_NULL_HANDLE; pz.ok = false;
}
static PZ TakePZ(Swapchain& sc) { PZ t = std::move(sc.pz); sc.pz = PZ(); return t; }
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
struct Decision { Skip skip = Skip::NotSwapchain; Swapchain* sc = nullptr; Slot* slot = nullptr; VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED; uint32_t ordinal = 0; bool alt = false; bool touchesSwapchain = false; std::string why; };

// Caller holds g_mu. atts = every attachment of the pass, already resolved to images.
static Decision Decide(const std::vector<AttSeen>& atts, int32_t offX, int32_t offY, uint32_t w, uint32_t h, uint32_t layers, uint32_t viewMask, bool resumeSuspend) {
  Decision best; bool haveSc = false;
  bool depthWrite = false, depthAny = false;
  for (const AttSeen& a : atts) if (!a.color && !a.sc) { depthAny = true; if (a.layout == 3 || a.layout == 1000314001) depthWrite = true; }
  for (const AttSeen& a : atts) {
    if (!a.sc) continue;
    auto ref = g_imgRef.find(a.image);
    Facts f; f.isSwapchain = ref != g_imgRef.end(); f.isColor = a.color; f.loadOp = a.load; f.samples = (int)a.samples; f.layout = a.layout;
    f.offX = offX; f.offY = offY; f.areaW = w; f.areaH = h; f.layers = layers; f.viewMask = viewMask; f.resumeSuspend = resumeSuspend; f.viewRangeOk = a.viewRangeOk;
    if (f.isSwapchain) { f.scW = ref->second.first->ext.width; f.scH = ref->second.first->ext.height; }
    Skip k = Classify(f);
    bool alt = false;
    if (k == Skip::None) {
      if (depthWrite) g_frameWritable = true;
      else if (depthAny) {
        g_frameCand = true;   // read-only-depth full-area LOAD pass: the scene overlay pass of normal frames, the only one in startup / focus camera frames
        if (g_cfg.altPass.load() && g_altMode.load()) alt = true;
      }
      if (g_cfg.requireDepth.load() && !depthWrite && !alt) k = Skip::NoDepthWrite;
    }
    if (!haveSc) { best.skip = k; haveSc = true; best.touchesSwapchain = true; }
    if (k != Skip::None) continue;
    Swapchain* sc = ref->second.first; Slot& slot = sc->slots[ref->second.second];
    best = Decision{}; best.touchesSwapchain = true; best.skip = Skip::None; best.sc = sc; best.slot = &slot; best.layout = (VkImageLayout)a.layout;
    best.alt = alt;
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
  bool did = false, menuSuspended = false; CropRect cr{};
  if (d.skip == Skip::None) {
    g_frameQual = true;
    if (g_menuMode.load()) menuSuspended = true;
    else {
      cr = ComputeCrop(d.sc->ext.width, d.sc->ext.height, EffectiveZoom());
      if (cr.dx || cr.dy) { RecordTransform(*d.sc, *d.slot, cmd, d.layout, cr); did = true; }
    }
  }
  g_cap.fs.passes++;
  if (d.touchesSwapchain) g_cap.fs.scPasses++;
  if (d.skip == Skip::None || d.skip == Skip::Disabled || d.skip == Skip::NoSlot || d.skip == Skip::WrongOrdinal) g_cap.fs.qualifying++;
  if (did) g_cap.fs.transformed++;
  if (!cap || !g_cfg.logPasses.load()) return;
  uint32_t diagId = 0;
  if (d.touchesSwapchain && g_cfg.logDraws.load()) { diagId = ++g_diagSeq; DiagBegin(cmd, diagId); }
  std::string verdict;
  if (did) verdict = Fmt("TRANSFORMED%s zoom=%.4fx%.4f crop=%d,%d src=%ux%u", d.alt ? " (alt pass: read-only depth)" : "", cr.zx, cr.zy, cr.dx, cr.dy, cr.srcW, cr.srcH);
  else if (menuSuspended) verdict = "skip: menu cadence guard (scene / scene-less frames alternate), zoom suspended";
  else if (d.skip == Skip::WrongOrdinal) verdict = Fmt("skip: qualifying pass #%u, transform is set to #%d", d.ordinal, g_cfg.passIndex.load());
  else if (d.skip == Skip::NoSlot) verdict = Fmt("skip: %s (%s)", SkipName(d.skip), d.why.c_str());
  else verdict = Fmt("skip: %s", SkipName(d.skip));
  if (diagId) verdict += Fmt("  [pass#%u]", diagId);
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
  if (!sc.slots.empty()) { for (auto& s : sc.slots) g_imgRef.erase(s.image); g_retired.push_back({sc.dev, sc.fn, std::move(sc.slots), g_presents.load() + 16, std::move(sc.sharp), TakePZ(sc)}); sc.slots.clear(); }
  std::vector<VkImage> v(imgs, imgs + n);
  BuildSlots(sc, v);
  Event(sc.usable ? Fmt("swapchain 0x%llx: %u images, scratch images allocated, transform usable", H64(h), n) : Fmt("swapchain 0x%llx: %u images, transform NOT usable: %s", H64(h), n, sc.whyNot.c_str()));
}

static void OnSwapchainDestroy(VkSwapchainKHR h) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_sc.find(h); if (it == g_sc.end()) return;
  Swapchain& sc = *it->second;
  for (auto& s : sc.slots) g_imgRef.erase(s.image);
  if (!sc.slots.empty()) g_retired.push_back({sc.dev, sc.fn, std::move(sc.slots), g_presents.load() + 16, std::move(sc.sharp), TakePZ(sc)});
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
  h += Fmt("%sconfig v0.1.1: pauseGapFix=%d pauseGapMaxFrames=%d menuGuard=%d(active=%d) holdZoom=%d requireDepth=%d engageFrames=%d rampFrames=%d logDrawDiagnostics=%d logDescriptorImages=%d tracked images=%zu descriptor sets=%zu\n", Indent(1), (int)g_cfg.pauseGap.load(), g_cfg.gapMax.load(), (int)g_cfg.menuGuard.load(), (int)g_menuMode.load(), (int)g_cfg.holdZoom.load(), (int)g_cfg.requireDepth.load(), g_cfg.engageFrames.load(), g_cfg.rampFrames.load(), (int)g_cfg.logDraws.load(), g_cfg.logDesc.load(), g_imgInfo.size(), g_desc.size());
  h += Fmt("%salt pass mode: enabled=%d active=%d engageFrames=%d run=%u\n", Indent(1), (int)g_cfg.altPass.load(), (int)g_altMode.load(), g_cfg.altEngage.load(), g_altRun.load());
  h += Fmt("%sprocess: pid=%lu presents so far=%llu\n", Indent(1), (unsigned long)GetCurrentProcessId(), (unsigned long long)g_presents.load());
  {
    std::lock_guard<std::mutex> lk(g_mu);
    h += Fmt("%sphysical device=0x%llx first device=0x%llx tracked views=%zu framebuffers=%zu render passes=%zu\n", Indent(1), H64(g_pd), H64(g_firstDevice), g_views.size(), g_fbs.size(), g_passes.size());
    h += Fmt("%sswapchains: %zu\n", Indent(1), g_sc.size());
    for (auto& kv : g_sc) {
      const Swapchain& sc = *kv.second;
      h += Fmt("%s0x%llx %ux%u fmt=%s(%d) presentMode=%d minImages=%u images=%zu usage requested=0x%x actual=0x%x bumped=%s usable=%s%s%s sharpen=%s gapFix=%s\n", Indent(2), H64(sc.h), sc.ext.width, sc.ext.height, FormatName((int)sc.fmt), (int)sc.fmt, (int)sc.mode, sc.minImages, sc.slots.size(), (unsigned)sc.requestedUsage, (unsigned)sc.usage, sc.bumped ? "yes" : "no", sc.usable ? "yes" : "no", sc.usable ? "" : " reason: ", sc.usable ? "" : sc.whyNot.c_str(), SharpStatus(sc).c_str(), sc.pz.ok ? "ready" : sc.pz.tried ? ("FAILED: " + sc.pz.why).c_str() : "idle");
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

// ---- PauseGapFix: zoom a whole scene-less frame at present time -------------------------------
// The swapchain image is in PRESENT_SRC_KHR when it is presented (required by the spec), so the layout is known. We submit our own
// command buffer (copy -> scratch, zoomed blit back) on the present queue; it waits for the game's present semaphores and signals
// its own one, which replaces them in the present call. Caller holds g_mu.
static bool PzInit(Swapchain& sc, VkQueue q) {
  PZ& pz = sc.pz; pz.tried = true;
  const DevFns& f = sc.fn;
  auto fam = g_queueFam.find(q);
  if (fam == g_queueFam.end()) { pz.why = "present queue unknown (vkGetDeviceQueue was not seen)"; return false; }
  pz.family = fam->second;
  if (!f.PzComplete()) { pz.why = "device functions for command pool / submit unavailable"; return false; }
  if (!g_fnQFProps || !g_pd) { pz.why = "queue family properties unavailable"; return false; }
  uint32_t n = 0; g_fnQFProps(g_pd, &n, nullptr); std::vector<VkQueueFamilyProperties> qp(n); g_fnQFProps(g_pd, &n, qp.data());
  if (pz.family >= n || !(qp[pz.family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { pz.why = Fmt("present queue family %u has no GRAPHICS capability (needed for blit)", pz.family); return false; }
  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = pz.family;
  VkResult r = f.createPool(sc.dev, &pci, nullptr, &pz.pool);
  if (r != VK_SUCCESS) { pz.why = Fmt("vkCreateCommandPool failed: %d", (int)r); pz.pool = VK_NULL_HANDLE; return false; }
  pz.cb.assign(sc.slots.size(), VK_NULL_HANDLE);
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ai.commandPool = pz.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = (uint32_t)pz.cb.size();
  r = f.allocCb(sc.dev, &ai, pz.cb.data());
  if (r != VK_SUCCESS) { pz.why = Fmt("vkAllocateCommandBuffers failed: %d", (int)r); FreePZ(sc.dev, f, pz); return false; }
  for (size_t i = 0; i < sc.slots.size(); ++i) {
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VkSemaphore sem{};
    r = f.createSem(sc.dev, &si, nullptr, &sem);
    if (r != VK_SUCCESS) { pz.why = Fmt("vkCreateSemaphore failed: %d", (int)r); FreePZ(sc.dev, f, pz); return false; }
    pz.sem.push_back(sem);
  }
  pz.ok = true; pz.why.clear(); return true;
}

// Returns the present info to hand to the driver (mod when our submit was inserted, otherwise pi unchanged). note explains the verdict.
static const VkPresentInfoKHR* PresentZoom(VkQueue q, const VkPresentInfoKHR* pi, VkPresentInfoKHR& mod, double zoom, std::string& note) {
  if (!pi || pi->swapchainCount != 1 || !pi->pSwapchains || !pi->pImageIndices) { note = "skipped: not a single-swapchain present"; return pi; }
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_sc.find(pi->pSwapchains[0]); if (it == g_sc.end()) { note = "skipped: swapchain not tracked"; return pi; }
  Swapchain& sc = *it->second; const uint32_t idx = pi->pImageIndices[0];
  if (!sc.usable || idx >= sc.slots.size() || !sc.slots[idx].scratch) { note = "skipped: swapchain has no usable scratch image"; return pi; }
  const CropRect cr = ComputeCrop(sc.ext.width, sc.ext.height, zoom);
  if (!cr.dx && !cr.dy) { note = "skipped: crop is empty"; return pi; }
  if (!sc.pz.tried && !PzInit(sc, q)) { note = "unavailable: " + sc.pz.why; return pi; }
  if (!sc.pz.ok) { note = "unavailable: " + sc.pz.why; return pi; }
  PZ& pz = sc.pz; const DevFns& f = sc.fn; const Slot& s = sc.slots[idx]; const VkCommandBuffer cb = pz.cb[idx];
  f.resetCb(cb, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (f.beginCb(cb, &bi) != VK_SUCCESS) { note = "skipped: vkBeginCommandBuffer failed"; return pi; }
  RecordTransform(sc, s, cb, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, cr);
  if (f.endCb(cb) != VK_SUCCESS) { note = "skipped: vkEndCommandBuffer failed"; return pi; }
  std::vector<VkPipelineStageFlags> stages(pi->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.waitSemaphoreCount = pi->waitSemaphoreCount; si.pWaitSemaphores = pi->pWaitSemaphores; si.pWaitDstStageMask = stages.empty() ? nullptr : stages.data();
  si.commandBufferCount = 1; si.pCommandBuffers = &cb; si.signalSemaphoreCount = 1; si.pSignalSemaphores = &pz.sem[idx];
  const VkResult r = f.submit(q, 1, &si, VK_NULL_HANDLE);
  if (r != VK_SUCCESS) { note = Fmt("skipped: vkQueueSubmit failed (%d)", (int)r); return pi; }
  mod = *pi; mod.waitSemaphoreCount = 1; mod.pWaitSemaphores = &pz.sem[idx];
  note = Fmt("ZOOMED at present zoom=%.4fx%.4f crop=%d,%d src=%ux%u image[%u]", cr.zx, cr.zy, cr.dx, cr.dy, cr.srcW, cr.srcH, idx);
  return &mod;
}

static const VkPresentInfoKHR* OnPresent(VkQueue q, const VkPresentInfoKHR* pi, VkPresentInfoKHR& mod) {
  const uint64_t n = ++g_presents;
  // Did this frame contain a qualifying scene pass? (recorded before the present, on any thread)
  const bool sceneFrame = g_frameQual.exchange(false);
  // MenuCadenceGuard: >= 2 separate scene-less runs within the last 8 frames = alternating cadence (title menu); gone after 12 scene frames in a row.
  { const uint32_t h = (g_frameHist.load() << 1) | (sceneFrame ? 1u : 0u); g_frameHist = h;
    if (!g_cfg.menuGuard.load()) g_menuMode = false;
    else if (!g_menuMode.load()) { if (ZeroRuns(h, 8) >= 2) { g_menuMode = true; Event("menu cadence guard ON (alternating scene / scene-less frames): zoom suspended"); } }
    else if ((h & 0xFFFu) == 0xFFFu) { g_menuMode = false; Event("menu cadence guard OFF (12 scene frames in a row)"); } }
  // Alt-pass mode: frames with a read-only-depth swapchain pass but no depth-writing scene pass (startup, NPC focus camera).
  { const bool cand = g_frameCand.exchange(false), wr = g_frameWritable.exchange(false);
    if (!g_cfg.altPass.load()) { g_altMode = false; g_altRun = 0; }
    else {
      if (cand && !wr) { if (g_altRun < 100000) ++g_altRun; } else g_altRun = 0;
      if (!g_altMode.load()) { if (g_altRun.load() >= (uint32_t)g_cfg.altEngage.load()) { g_altMode = true; Event("alt pass mode ON (no depth-writing scene pass; zooming the read-only-depth pass)"); } }
      else if (wr) { g_altMode = false; Event("alt pass mode OFF (depth-writing scene pass is back)"); }
    } }
  if (sceneFrame) { if (g_stableStreak < 100000) ++g_stableStreak; g_gapRun = 0; g_hadScene = true; }
  else { if (!g_cfg.holdZoom.load()) g_stableStreak = 0; if (g_hadScene.load()) ++g_gapRun; }
  // PauseGapFix: a scene-less frame that directly follows scene frames (pause transition) is zoomed as a whole here.
  std::string note; const VkPresentInfoKHR* use = pi;
  if (g_menuMode.load()) g_stableStreak = 0;   // the zoom eases back in once gameplay resumes
  if (!g_cfg.pauseGap.load()) note = "PauseGapFix off";
  else if (!g_cfg.enabled.load()) note = "mod disabled";
  else if (g_menuMode.load()) note = "menu cadence guard active: zoom suspended";
  else if (sceneFrame) note = "scene frame (zoomed by the scene pass if engaged)";
  else if (!g_hadScene.load()) note = "no scene frame seen yet (startup / menus): untouched";
  else if (g_gapRun.load() > (uint32_t)g_cfg.gapMax.load()) note = Fmt("gap run %u > PauseGapMaxFrames %d: untouched", g_gapRun.load(), g_cfg.gapMax.load());
  else { const double z = EffectiveZoom(); if (!(z > 1.0)) note = "zoom not engaged yet"; else use = PresentZoom(q, pi, mod, z, note); }
  if (Capturing()) {
    CapLine(Fmt("%spresent #%llu sceneFrame=%d alt=%d(run=%u) hadScene=%d gapRun=%u => %s", Indent(2), (unsigned long long)n, (int)sceneFrame, (int)g_altMode.load(), g_altRun.load(), (int)g_hadScene.load(), g_gapRun.load(), note.c_str()));
    FlushFrame();
  }
  // key edge-detect, only while this process owns the foreground window
  const int vk = g_cfg.captureKey.load();
  const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
  if (down && !g_cap.keyDown && !Capturing()) {
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid == GetCurrentProcessId()) BeginCapture();
  }
  g_cap.keyDown = down;
  MaybeReloadIni();
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (pi) for (uint32_t i = 0; i < pi->swapchainCount; ++i) {
      auto it = g_sc.find(pi->pSwapchains[i]);
      if (it != g_sc.end() && pi->pImageIndices[i] < it->second->slots.size()) it->second->slots[pi->pImageIndices[i]].qualSeen = 0;
    }
    for (size_t i = 0; i < g_retired.size();) {
      if (g_retired[i].freeAt <= n) { if (g_retired[i].sharp) sharpen::Destroy(*g_retired[i].sharp); FreePZ(g_retired[i].dev, g_retired[i].fn, g_retired[i].pz); FreeSlots(g_retired[i].dev, g_retired[i].fn, g_retired[i].slots); g_retired.erase(g_retired.begin() + i); } else ++i;
    }
  }
  return use;
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
    Hit(HK_QueuePresentKHR, P); VkPresentInfoKHR mod{}; const VkPresentInfoKHR* use = OnPresent(q, pi, mod); return Pick(P_QueuePresentKHR, P)(q, use);
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
    Hit(HK_CmdBindDescriptorSets, P); Shadowed(c, [&](sharpen::Shadow& s) { s.OnSets(bp, l, first, n, sets, dc, dyn); }); if (Capturing() && bp == VK_PIPELINE_BIND_POINT_GRAPHICS) DiagSets(c, first, n, sets); Pick(P_CmdBindDescriptorSets, P)(c, bp, l, first, n, sets, dc, dyn); }
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
  // ---- v0.0.4: pass ends / draws / descriptor writes (diagnostics) and queue + image tracking
  static void VKAPI_CALL CmdEndRenderPass(VkCommandBuffer c) { HitC(HK_CmdEndRenderPass, P); if (Capturing()) DiagEnd(c); Pick(P_CmdEndRenderPass, P)(c); }
  static void VKAPI_CALL CmdEndRenderPass2(VkCommandBuffer c, const VkSubpassEndInfo* e) { HitC(HK_CmdEndRenderPass2, P); if (Capturing()) DiagEnd(c); Pick(P_CmdEndRenderPass2, P)(c, e); }
  static void VKAPI_CALL CmdEndRendering(VkCommandBuffer c) { HitC(HK_CmdEndRendering, P); if (Capturing()) DiagEnd(c); Pick(P_CmdEndRendering, P)(c); }
  static void VKAPI_CALL CmdDraw(VkCommandBuffer c, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) { if (Capturing()) { Hit(HK_CmdDraw, P); DiagDraw(c, 0, vc, ic); } Pick(P_CmdDraw, P)(c, vc, ic, fv, fi); }
  static void VKAPI_CALL CmdDrawIndexed(VkCommandBuffer c, uint32_t xc, uint32_t ic, uint32_t fi, int32_t vo, uint32_t fin) { if (Capturing()) { Hit(HK_CmdDrawIndexed, P); DiagDraw(c, 1, xc, ic); } Pick(P_CmdDrawIndexed, P)(c, xc, ic, fi, vo, fin); }
  static void VKAPI_CALL CmdDrawIndirect(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, uint32_t dc, uint32_t st) { if (Capturing()) { Hit(HK_CmdDrawIndirect, P); DiagDraw(c, 2, dc, 0); } Pick(P_CmdDrawIndirect, P)(c, b, o, dc, st); }
  static void VKAPI_CALL CmdDrawIndexedIndirect(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, uint32_t dc, uint32_t st) { if (Capturing()) { Hit(HK_CmdDrawIndexedIndirect, P); DiagDraw(c, 2, dc, 0); } Pick(P_CmdDrawIndexedIndirect, P)(c, b, o, dc, st); }
  static void VKAPI_CALL CmdDrawIndirectCount(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkBuffer cb, VkDeviceSize co, uint32_t md, uint32_t st) { if (Capturing()) { Hit(HK_CmdDrawIndirectCount, P); DiagDraw(c, 2, md, 0); } Pick(P_CmdDrawIndirectCount, P)(c, b, o, cb, co, md, st); }
  static void VKAPI_CALL CmdDrawIndexedIndirectCount(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkBuffer cb, VkDeviceSize co, uint32_t md, uint32_t st) { if (Capturing()) { Hit(HK_CmdDrawIndexedIndirectCount, P); DiagDraw(c, 2, md, 0); } Pick(P_CmdDrawIndexedIndirectCount, P)(c, b, o, cb, co, md, st); }
  static void VKAPI_CALL UpdateDescriptorSets(VkDevice d, uint32_t nw, const VkWriteDescriptorSet* w, uint32_t nc, const VkCopyDescriptorSet* cp) {
    if (DescTracking()) { HitC(HK_UpdateDescriptorSets, P); TrackDescWrites(nw, w); }
    Pick(P_UpdateDescriptorSets, P)(d, nw, w, nc, cp);
  }
  static VkResult VKAPI_CALL CreateImage(VkDevice d, const VkImageCreateInfo* ci, const VkAllocationCallbacks* a, VkImage* out) {
    Hit(HK_CreateImage, P);
    VkResult r = Pick(P_CreateImage, P)(d, ci, a, out);
    if (r == VK_SUCCESS && ci && out) { std::lock_guard<std::mutex> l(g_mu); g_imgInfo[*out] = {ci->extent.width, ci->extent.height, ci->arrayLayers, ci->mipLevels, ci->format, ci->usage, ci->samples}; }
    return r;
  }
  static void VKAPI_CALL DestroyImage(VkDevice d, VkImage img, const VkAllocationCallbacks* a) {
    Hit(HK_DestroyImage, P); { std::lock_guard<std::mutex> l(g_mu); g_imgInfo.erase(img); } Pick(P_DestroyImage, P)(d, img, a);
  }
  static void VKAPI_CALL GetDeviceQueue(VkDevice d, uint32_t fam, uint32_t idx, VkQueue* q) {
    Hit(HK_GetDeviceQueue, P); Pick(P_GetDeviceQueue, P)(d, fam, idx, q);
    if (q && *q) { std::lock_guard<std::mutex> l(g_mu); g_queueFam[*q] = fam; }
  }
  static void VKAPI_CALL GetDeviceQueue2(VkDevice d, const VkDeviceQueueInfo2* info, VkQueue* q) {
    Hit(HK_GetDeviceQueue2, P); Pick(P_GetDeviceQueue2, P)(d, info, q);
    if (q && *q && info) { std::lock_guard<std::mutex> l(g_mu); g_queueFam[*q] = info->queueFamilyIndex; }
  }
  static VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer c, const VkCommandBufferBeginInfo* bi) {
    Hit(HK_BeginCommandBuffer, P);
    if (!g_cs.empty()) g_cs.erase(c);
    if (!g_bound.empty()) g_bound.erase(c);
    if (!g_pdiag.empty()) g_pdiag.erase(c);
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
    {"CmdPipelineBarrier2KHR", "CmdPipelineBarrier2"}, {"CmdEndRenderPass2KHR", "CmdEndRenderPass2"}, {"CmdEndRenderingKHR", "CmdEndRendering"},
    {"CmdDrawIndirectCountKHR", "CmdDrawIndirectCount"}, {"CmdDrawIndirectCountAMD", "CmdDrawIndirectCount"},
    {"CmdDrawIndexedIndirectCountKHR", "CmdDrawIndexedIndirectCount"}, {"CmdDrawIndexedIndirectCountAMD", "CmdDrawIndexedIndirectCount"}};
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
  g_fnQFProps = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)GetProcAddress(vk, "vkGetPhysicalDeviceQueueFamilyProperties");
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
