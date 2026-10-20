/**
 * @file    platform/switch/dialogs_switch.cpp
 * @brief   Nintendo Switch versions of fatal_dialog.h and file_dialog.h.
 *
 *          A fatal error shows the system error applet (the same screen the
 *          console uses for its own errors) with the short title in the dialog
 *          and the full text behind "Details". There is no file picker on
 *          Horizon: the game files are put on the SD card by hand, so every
 *          file dialog answers "cancelled".
 * @license BSD 3-Clause, see LICENSE
 */
#include "platform/fatal_dialog.h"
#include "platform/file_dialog.h"

#include <string>

extern "C" {
#include <switch/types.h>
#include <switch/result.h>
#include <switch/applets/error.h>
}

#include "core/logging.h"

namespace bd::platform {
namespace {

void ShowErrorApplet(std::string_view title, std::string_view body) {
  const std::string t(title);
  std::string b(title);
  b += "\n\n";
  b += body;
  ErrorApplicationConfig config;
  if (R_FAILED(errorApplicationCreate(&config, t.c_str(), b.c_str())))
    return;
  // The applet needs a running application (title takeover). From the album
  // applet it fails, and the error stays in the log.
  if (R_FAILED(errorApplicationShow(&config)))
    BD_WARN("error applet unavailable; the message is only in the log");
}

} // namespace

void ShowFatalError(std::string_view title, std::string_view body) {
  BD_ERROR("Fatal: {} - {}", title, body);
  ShowErrorApplet(title, body);
}

void ShowWarning(std::string_view title, std::string_view body) {
  BD_WARN("{} - {}", title, body);
  ShowErrorApplet(title, body);
}

bool ShowFatalErrorWithAction(std::string_view title, std::string_view body,
                              std::string_view action,
                              rex::ui::Window *parent) {
  (void)action;
  (void)parent;
  // The error applet has a single button: the fatal path, never the action.
  BD_ERROR("Fatal: {} - {}", title, body);
  ShowErrorApplet(title, body);
  return false;
}

std::optional<std::filesystem::path>
ShowOpenFileDialog(const wchar_t *title, std::span<const FileFilter> filters) {
  (void)title;
  (void)filters;
  BD_WARN("file dialogs are not available on Nintendo Switch");
  return std::nullopt;
}

std::vector<std::filesystem::path>
ShowOpenFilesDialog(const wchar_t *title, std::span<const FileFilter> filters) {
  (void)title;
  (void)filters;
  BD_WARN("file dialogs are not available on Nintendo Switch");
  return {};
}

std::optional<std::filesystem::path> ShowOpenFolderDialog(const wchar_t *title) {
  (void)title;
  BD_WARN("folder dialogs are not available on Nintendo Switch");
  return std::nullopt;
}

} // namespace bd::platform
