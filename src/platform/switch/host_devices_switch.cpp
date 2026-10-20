/**
 * @file    platform/switch/host_devices_switch.cpp
 * @brief   Nintendo Switch version of host_devices.h.
 *
 *          The pad is always a Switch controller (Joy-Con or Pro Controller),
 *          so the prompts wear Switch glyphs, and there is one display: the
 *          console screen or the TV through the dock.
 * @license BSD 3-Clause, see LICENSE
 */
#include "platform/host_devices.h"

namespace bd::platform {

PadBrand ConnectedPad() { return PadBrand::Switch; }

int DisplayCount() { return 1; }

std::string DisplayName(int index) {
  return index == 1 ? std::string("Nintendo Switch") : std::string();
}

} // namespace bd::platform
