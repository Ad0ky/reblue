/**
 * @file    platform/switch/switch_tuning.cpp
 * @brief   Nintendo Switch performance settings, see platform/switch_tuning.h.
 *
 *          GPU clock: in handheld mode a program starts with the GPU at
 *          307.2 MHz. Horizon has an official 460.8 MHz handheld profile,
 *          asked for through apm like any commercial game does (not an
 *          overclock). The SDK's switch_apm.cpp (from nfsmw-nx) picks the
 *          configuration that keeps the memory at 1331.2 MHz. Measured on the
 *          console at 307.2 MHz the field ran at 7.7 fps with the GPU the
 *          limit, so this is +50 % on the limiting part.
 *
 *          Handheld resolution: the game renders at 960x540 in handheld mode
 *          (as the Lost Odyssey port does) and the frame is scaled to the
 *          1280x720 screen: 44 % fewer pixels to shade.
 * @license BSD 3-Clause, see LICENSE
 */
#include "platform/switch_tuning.h"

#include <switch.h>

#include <rex/cvar.h>
#include <rex/ui/switch_apm.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>
#if defined(PLUME_DEKO3D)
#include <plume_deko3d.h>

#include "gpu/shaders/deko3d_shaders.h"
#endif

#include "core/app_root.h"
#include "core/logging.h"

// Four-core mode (SDK core/threading_switch.cpp).
extern "C" int RexSwitchSystemCore(void);
extern "C" bool RexSwitchSystemCoreCompiles(void);
extern "C" void RexSwitchSystemCoreDescribe(char* out, size_t out_len);
// The localized copies' keys and their "[hot]" line (hot_natives.cpp).
extern "C" void ReblueHotInit();

namespace plume {
void VulkanSetPipelineCachePath(const std::string &path);
}

REXCVAR_DEFINE_INT32(bd_switch_gpu_mhz, 460, "Switch",
                     "GPU clock profile to ask for in handheld mode: 0 = leave "
                     "the system default (307.2 MHz), 384 or 460. Docked is "
                     "never touched.")
    .range(0, 460);

REXCVAR_DEFINE_INT32(bd_switch_handheld_height, 540, "Switch",
                     "Render height in handheld mode (16:9): 540 = 960x540, "
                     "720 or 0 = the full 1280x720. Applied at start.")
    .range(0, 1080);

// Dynamic resolution (2026-11-12, the user's request; the picture is not the
// same while it acts). From rr6-nx / the renut-nx base's
// banjo_renut_dynamic_resolution in spirit: steps of 36 lines (16:9 widths stay
// whole), down when the GPU's time a frame stays over a budget, up when the next
// step is predicted to fit. Each change rebuilds the game's render targets at
// the next frame submit (the path a window resize takes on the PC), a short
// hitch, so changes are at least 2 s apart.
REXCVAR_DEFINE_BOOL(bd_switch_dynamic_resolution, false, "Switch",
                    "Handheld only: the render height moves between "
                    "bd_switch_dynamic_low_height and bd_switch_handheld_height "
                    "in steps of 36 lines with the GPU's load (the picture is "
                    "softer while it is lower); FSR 1 scales it up (bd_switch_fsr)");
REXCVAR_DEFINE_INT32(bd_switch_dynamic_low_height, 432, "Switch",
                     "bd_switch_dynamic_resolution: the lowest render height "
                     "(432 = 768x432)")
    .range(288, 1080);
REXCVAR_DEFINE_DOUBLE(bd_switch_dynamic_gpu_ms, 33.3, "Switch",
                      "bd_switch_dynamic_resolution: the GPU time a frame (ms, "
                      "a moving average) above which the height steps down; it "
                      "steps up when the next height is predicted under 95% of it");
REXCVAR_DEFINE_INT32(bd_switch_dynamic_test_s, 0, "Switch",
                     "bd_switch_dynamic_resolution test: alternate the highest "
                     "and lowest heights every N seconds whatever the load (0 off)")
    .range(0, 600);
REXCVAR_DEFINE_INT32(bd_switch_fsr, 1, "Switch",
                     "Upscale the rendered image to the screen with AMD FSR 1 "
                     "(EASU) instead of bilinear filtering: 0 never, 1 while "
                     "bd_switch_dynamic_resolution has lowered the height "
                     "(at bd_switch_handheld_height the present is as before), "
                     "2 whenever the image is smaller than the screen "
                     "(handheld 960x540 too). It costs ~5.7 ms a frame on the "
                     "console against ~0.9 for the bilinear present")
    .range(0, 2);

#if defined(PLUME_DEKO3D)
REXCVAR_DEFINE_INT32(bd_deko3d_uam_flags, 431, "Switch",
                     "deko3d: uam's extra code generation passes (plume "
                     "Deko3DOptions::uamFlags). 431 = banjo-nx's 447 without 16, "
                     "the second compile per pixel shader that picks the "
                     "cheaper code: on the console that compile never ended "
                     "for one of the game's shaders. Each value has its own "
                     "shader cache file.")
    .range(0, 1023);

REXCVAR_DEFINE_BOOL(bd_deko3d_zcull, true, "Switch",
                    "deko3d: hierarchical depth culling (Zcull). The GPU drops "
                    "blocks of pixels hidden behind depth already drawn before "
                    "running their pixel shader. Set false if decals or other "
                    "surfaces go missing in 4x8-pixel blocks. Read at start.");

REXCVAR_DEFINE_BOOL(bd_deko3d_depth_compression, true, "Switch",
                    "deko3d: depth/stencil targets with hardware compression, "
                    "as colour targets already have: less memory traffic for "
                    "depth tests. Read at start.");
REXCVAR_DEFINE_BOOL(bd_deko3d_merge_barriers, false, "Switch",
                    "deko3d: barriers recorded one after another with no GPU "
                    "work between them are sent as one (the strongest wait and "
                    "every cache invalidation of them), right before the next "
                    "command that does work: the same order, one drain instead "
                    "of several. Read at start.");
REXCVAR_DEFINE_BOOL(bd_deko3d_lean_barriers, false, "Switch",
                    "deko3d: a texture about to be rendered to (not read) waits "
                    "for the work before it but keeps the texture cache; the "
                    "transition that reads it later still drops the cache. "
                    "Read at start.");
REXCVAR_DEFINE_INT32(bd_deko3d_diag, 0, "Switch",
                     "deko3d GPU cost experiments, for measuring only: they "
                     "change or break the picture. Bits: 1 draws not sent, 2 "
                     "every scissor one pixel (no pixel work), 4 fragment "
                     "barriers do not wait, 8 every draw cut to one triangle, 16 "
                     "every vertex buffer at stride 0 (vertex shaders only), 32 "
                     "every bone read of the vertex shaders reads bone 0. It runs every other 10 s GPU report "
                     "window ([gpu-passes] says which), so the screen shows "
                     "between them. 0 in play. Read at start.");
#endif

namespace bd::platform {

#if defined(PLUME_DEKO3D)
namespace {

// The deko3d back end's compiled shaders (DKSH, by the hash of the GLSL variant
// uam compiled) in one file, <install>/cache/deko3d_shaders.bin: records of
// [u32 magic][u64 hash][u32 size][size bytes], appended as uam makes them, all
// read at start. A torn last record (power cut mid-write) ends the read and is
// overwritten by the next append. Changing uam's options changes the file name.
struct DkshCache {
  std::mutex mutex;
  std::unordered_map<uint64_t, std::vector<uint8_t>> entries;
  FILE *file = nullptr;
  size_t bytes = 0;
};
constexpr uint32_t kDkshRecordMagic = 0x48534B44; // "DKSH"

DkshCache &Dksh() {
  static DkshCache c;
  return c;
}

// Records [magic][hash][size][DKSH] from data into the cache; the bytes read up to the last whole record.
size_t ReadDkshRecords(const uint8_t *data, size_t size, size_t *added) {
  auto &c = Dksh();
  size_t at = 0;
  while (size - at >= 16) {
    uint32_t magic = 0, n = 0;
    uint64_t hash = 0;
    std::memcpy(&magic, data + at, 4);
    std::memcpy(&hash, data + at + 4, 8);
    std::memcpy(&n, data + at + 12, 4);
    if (magic != kDkshRecordMagic || n == 0 || n > size - at - 16)
      break;
    if (c.entries.emplace(hash, std::vector<uint8_t>(data + at + 16, data + at + 16 + n)).second) {
      c.bytes += n;
      ++*added;
    }
    at += 16 + n;
  }
  return at;
}

void OpenDkshCache(const std::string &path, unsigned uam_flags) {
  auto &c = Dksh();
  {
    // The build's variants first (make_dk_shaders.py --uam-variants): with them the console compiles only what
    // the build could not foresee.
    std::vector<uint8_t> built;
    size_t added = 0;
    if (bd::gpu::Deko3DBuiltinDksh(uam_flags, built))
      ReadDkshRecords(built.data(), built.size(), &added);
    BD_INFO("Switch: deko3d shaders compiled by the build: {} (uam flags {}, the build's {})", added, uam_flags,
            bd::gpu::g_deko3dDkshFlags);
  }
  const size_t from_build = c.entries.size();
  long good_end = 0;
  if (FILE *f = std::fopen(path.c_str(), "rb")) {
    for (;;) {
      uint32_t magic = 0, size = 0;
      uint64_t hash = 0;
      if (std::fread(&magic, 4, 1, f) != 1 || magic != kDkshRecordMagic ||
          std::fread(&hash, 8, 1, f) != 1 || std::fread(&size, 4, 1, f) != 1 ||
          size == 0 || size > (16u << 20))
        break;
      std::vector<uint8_t> data(size);
      if (std::fread(data.data(), 1, size, f) != size)
        break;
      if (c.entries.emplace(hash, std::move(data)).second)
        c.bytes += size;
      good_end = std::ftell(f);
    }
    std::fclose(f);
  }
  // r+ keeps the records, then the torn tail (if any) is overwritten.
  c.file = std::fopen(path.c_str(), good_end ? "r+b" : "wb");
  if (c.file)
    std::fseek(c.file, good_end, SEEK_SET);
  BD_INFO("Switch: deko3d shader cache {}: {} shaders ({} from the build), {} KB{}", path,
          c.entries.size(), from_build, c.bytes >> 10, c.file ? "" : " (read only)");
  plume::SetDeko3DShaderCache(
      [](uint64_t hash, std::vector<uint8_t> &dksh) {
        auto &c = Dksh();
        std::lock_guard lock(c.mutex);
        auto it = c.entries.find(hash);
        if (it == c.entries.end())
          return false;
        dksh = it->second;
        return true;
      },
      [](uint64_t hash, const std::vector<uint8_t> &dksh) {
        auto &c = Dksh();
        std::lock_guard lock(c.mutex);
        if (dksh.empty() || !c.entries.emplace(hash, dksh).second || !c.file)
          return;
        const uint32_t size = static_cast<uint32_t>(dksh.size());
        std::fwrite(&kDkshRecordMagic, 4, 1, c.file);
        std::fwrite(&hash, 8, 1, c.file);
        std::fwrite(&size, 4, 1, c.file);
        std::fwrite(dksh.data(), 1, size, c.file);
        std::fflush(c.file);
      });
}

} // namespace
#endif

void ApplySwitchClocks() {
  ReblueHotInit();
  {
    // Four-core mode (re:Blue 2026-11-12, from renut-nx; SDK core/threading_switch.h).
    char cores[320];
    RexSwitchSystemCoreDescribe(cores, sizeof(cores));
    BD_INFO("Switch: {}", cores);
  }
  const int mhz = REXCVAR_GET(bd_switch_gpu_mhz);
  RexSwitchApmPedirGpuMhz(mhz);
  RexSwitchApmPermitirRam1600(0);
  RexSwitchApmAplicar();
  BD_INFO("Switch: handheld GPU profile requested: {}",
          mhz ? std::to_string(mhz) + " MHz" : std::string("system default"));

  // The Vulkan pipeline cache in <install>/cache/, read when the device is
  // created. re:Blue already pre-builds its known pipelines at start on a
  // background pool; with the cache those come from the SD card after the
  // first run instead of being compiled again.
  const std::filesystem::path dir = bd::CacheRootFor(bd::AppRootFolder());
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::string file = (dir / "vulkan_pipelines.bin").string();
  plume::VulkanSetPipelineCachePath(file);
  BD_INFO("Switch: Vulkan pipeline cache at {}", file);
#if defined(PLUME_DEKO3D)
  {
    plume::Deko3DOptions options = plume::Deko3DGetOptions();
    options.uamFlags = static_cast<unsigned>(REXCVAR_GET(bd_deko3d_uam_flags));
    options.zcull = REXCVAR_GET(bd_deko3d_zcull);
    options.depthCompression = REXCVAR_GET(bd_deko3d_depth_compression);
    options.mergeBarriers = REXCVAR_GET(bd_deko3d_merge_barriers);
    options.leanBarriers = REXCVAR_GET(bd_deko3d_lean_barriers);
    options.diag = static_cast<unsigned>(REXCVAR_GET(bd_deko3d_diag));
    // Four-core mode (from renut-nx): the uam compile thread on the system core when there is one, at 0x2C: below
    // the audio output (0x2A) and the Audio Worker (0x2B), above the XAudio engine threads moved there (0x2D). At
    // the 0x3B it had otherwise, an engine thread busy with the reverb after a load (switch_system_core_mixer = 1)
    // kept it from running and the game hung at a warp waiting for its shaders (2026-11-12).
    if (RexSwitchSystemCoreCompiles()) {
      options.compileCore = RexSwitchSystemCore();
      options.compilePriority = 0x2C;
    }
    plume::Deko3DSetOptions(options);
    BD_INFO("Switch: deko3d barriers: merged {}, lean {}{}",
            options.mergeBarriers ? "on" : "off", options.leanBarriers ? "on" : "off",
            options.diag ? " | GPU COST EXPERIMENT " + std::to_string(options.diag) +
                               " (bd_deko3d_diag) every other 10 s window: the picture is wrong in those"
                         : std::string());
    // A shader whose compile does not end is written here (uam_stuck_*.glsl).
    static const std::string logs = (bd::AppRootFolder() / "logs").string();
    plume::Deko3DSetDumpFolder(logs.c_str());
  }
  const unsigned uam_flags = plume::Deko3DGetOptions().uamFlags;
  OpenDkshCache(
      (dir / ("deko3d_shaders_" + std::to_string(uam_flags) + ".bin")).string(),
      uam_flags);
#endif
}

namespace {
// The height dynamic resolution has chosen (0: none yet, the configured one).
std::atomic<int> g_dynamic_height{0};
} // namespace

bool SwitchHandheldRenderSize(u32 &w, u32 &h) {
  const int configured = REXCVAR_GET(bd_switch_handheld_height);
  const int dynamic = g_dynamic_height.load(std::memory_order_relaxed);
  const int height = (dynamic > 0 && REXCVAR_GET(bd_switch_dynamic_resolution))
                         ? dynamic
                         : configured;
  if (height <= 0 || height >= 720)
    return false;
  if (appletGetOperationMode() != AppletOperationMode_Handheld)
    return false;
  h = static_cast<u32>(height) & ~1u;
  w = ((h * 16 + 8) / 9) & ~1u; // 540 -> 960
  return true;
}

bool SwitchDynamicResolutionFrame(double gpu_ms) {
  if (!REXCVAR_GET(bd_switch_dynamic_resolution) || !(gpu_ms > 0.0))
    return false;
  if (appletGetOperationMode() != AppletOperationMode_Handheld)
    return false;
  int high = REXCVAR_GET(bd_switch_handheld_height);
  if (high <= 0 || high >= 720)
    high = 720;
  high -= high % 36;
  int low = std::clamp(REXCVAR_GET(bd_switch_dynamic_low_height), 288, high);
  low -= low % 36;
  const double budget = REXCVAR_GET(bd_switch_dynamic_gpu_ms);
  const int test_s = REXCVAR_GET(bd_switch_dynamic_test_s);

  using Clock = std::chrono::steady_clock;
  static Clock::time_point last_change{}, over_since{}, fit_since{};
  static double ema = 0.0;
  static bool settled = false;
  const Clock::time_point now = Clock::now();
  const auto seconds = [&](Clock::time_point t) {
    return std::chrono::duration<double>(now - t).count();
  };
  int cur = g_dynamic_height.load(std::memory_order_relaxed);
  if (cur <= 0)
    cur = high;
  if (!settled) {
    settled = true;
    last_change = now;
    ema = gpu_ms;
  }
  ema = ema * 0.9 + gpu_ms * 0.1;

  int next = cur;
  if (test_s > 0) {
    if (seconds(last_change) >= test_s)
      next = cur == high ? low : high;
  } else if (seconds(last_change) >= 2.0) {
    // Down: a whole second over the budget.
    if (ema > budget && cur > low) {
      if (over_since == Clock::time_point{})
        over_since = now;
      if (seconds(over_since) >= 1.0)
        next = cur - 36;
    } else {
      over_since = {};
    }
    // Up: the next height predicted under 95% of the budget for 2 s (the
    // pixel ratio scales the whole GPU time, which overstates it: cautious).
    const int up = REXCVAR_GET(bd_switch_fsr) >= 1 ? high : std::min(cur + 36, high);
    // FSR's extra cost (measured ~4.8 ms over the bilinear present) goes away
    // at the configured height (bd_switch_fsr 1).
    const double fsr_extra =
        (REXCVAR_GET(bd_switch_fsr) == 1 && cur < high && up == high) ? 4.8 : 0.0;
    const double predicted = std::max(0.0, ema - fsr_extra) *
                             (double(up) * double(up)) / (double(cur) * double(cur));
    if (next == cur && up != cur && predicted < budget * 0.95) {
      if (fit_since == Clock::time_point{})
        fit_since = now;
      if (seconds(fit_since) >= 2.0)
        next = up;
    } else {
      fit_since = {};
    }
  }
  // With FSR (bd_switch_fsr >= 1) the steps between cost more than they save
  // (FSR's ~4.8 ms over the bilinear present against ~2.5 ms for a step), so
  // the height goes straight to either end.
  if (next != cur && test_s == 0 && REXCVAR_GET(bd_switch_fsr) >= 1)
    next = next < cur ? low : high;
  if (next == cur)
    return false;
  BD_INFO("[dynres] render height {} -> {} (GPU {:.1f} ms a frame, budget {:.1f}{})",
          cur, next, ema, budget, test_s > 0 ? ", test mode" : "");
  g_dynamic_height.store(next, std::memory_order_relaxed);
  last_change = now;
  over_since = {};
  fit_since = {};
  return true;
}

bool SwitchFsrWanted(u32 src_w, u32 src_h, u32 dst_w, u32 dst_h) {
  if (src_w >= dst_w && src_h >= dst_h)
    return false;
  switch (REXCVAR_GET(bd_switch_fsr)) {
  case 2:
    return true;
  case 1: {
    // Only below the configured handheld height: there the present stays the
    // bilinear one it always was.
    int high = REXCVAR_GET(bd_switch_handheld_height);
    if (high <= 0 || high >= 720)
      high = 720;
    return REXCVAR_GET(bd_switch_dynamic_resolution) && src_h + 8 < u32(high);
  }
  default:
    return false;
  }
}

void SpreadCurrentThread(int core) {
  u64 mask = 0;
  if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) ||
      mask == 0)
    return;
  if (core < 0 || (mask & (u64(1) << core)) == 0)
    core = __builtin_ctzll(mask);
  const Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, mask);
  if (R_FAILED(rc))
    BD_WARN("Switch: svcSetThreadCoreMask(core {}, mask {:#x}) failed: {:#x}",
            core, mask, rc);
}

void SetCurrentThreadPriority(int priority) {
  const Result rc = svcSetThreadPriority(CUR_THREAD_HANDLE, u32(priority));
  if (R_FAILED(rc))
    BD_WARN("Switch: svcSetThreadPriority({:#x}) failed: {:#x}", priority, rc);
}

} // namespace bd::platform
