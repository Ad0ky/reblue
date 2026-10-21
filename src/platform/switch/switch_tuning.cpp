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

#include <filesystem>
#include <string>
#include <system_error>

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

namespace bd::platform {

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
