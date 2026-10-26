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

#if defined(PLUME_DEKO3D)
REXCVAR_DEFINE_INT32(bd_deko3d_uam_flags, 431, "Switch",
                     "deko3d: uam's extra code generation passes (plume "
                     "Deko3DOptions::uamFlags). 431 = banjo-nx's 447 without 16, "
                     "the second compile per pixel shader that picks the "
                     "cheaper code: on the console that compile never ended "
                     "for one of the game's shaders. Each value has its own "
                     "shader cache file.")
    .range(0, 1023);
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
    plume::Deko3DSetOptions(options);
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

bool SwitchHandheldRenderSize(u32 &w, u32 &h) {
  const int height = REXCVAR_GET(bd_switch_handheld_height);
  if (height <= 0 || height >= 720)
    return false;
  if (appletGetOperationMode() != AppletOperationMode_Handheld)
    return false;
  h = static_cast<u32>(height) & ~1u;
  w = ((h * 16 + 8) / 9) & ~1u; // 540 -> 960
  return true;
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

} // namespace bd::platform
