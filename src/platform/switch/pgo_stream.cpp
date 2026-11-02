/**
 * @file    platform/switch/pgo_stream.cpp
 * @brief   Branch counts of the instrumented Switch build (REBLUE_PGO=generate)
 *          written to the SD card for profile-guided optimisation.
 *
 * libgcov normally writes one .gcda file per object when the program exits, at
 * the build machine's paths. On the console the program is mostly closed from
 * the HOME menu, and those paths do not exist. So the instrumented build puts a
 * pointer to each object's counters in the section gcov_info
 * (-fprofile-info-section) and this file writes every object, in GCC's stream
 * format (__gcov_info_to_gcda, as GCC's manual describes for freestanding
 * programs), to <install>/pgo/reblue_pgo_<session>.stream: every minute and when
 * the game shuts down. Each write replaces the last one of the session through a
 * .tmp file, so a write cut short (console switched off) keeps the previous one.
 * tools/switch/pgo-merge.sh turns the streams into .gcda files (gcov-tool
 * merge-stream). See docs/SWITCH.md, "Profile-guided optimisation".
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "platform/switch/pgo_stream.h"

#if defined(REBLUE_PGO_GENERATE)

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

extern "C" {
#include <gcov.h>
}

#include "core/app_root.h"
#include "core/logging.h"

// GNU ld defines these around the gcov_info section: one pointer per object.
extern "C" const struct gcov_info *const __start_gcov_info[];
extern "C" const struct gcov_info *const __stop_gcov_info[];

namespace bd::platform {
namespace {

constexpr auto kInterval = std::chrono::seconds(60);

std::mutex g_writeMutex;
std::filesystem::path g_path;

std::mutex g_waitMutex;
std::condition_variable g_wait;
bool g_quit = false;
std::thread g_writer;

struct Out {
  std::FILE *file = nullptr;
  bool failed = false;
  std::vector<void *> blocks; // libgcov's scratch memory for one write
};

void Put(const void *data, unsigned size, void *arg) {
  auto *out = static_cast<Out *>(arg);
  if (!out->failed && size && std::fwrite(data, 1, size, out->file) != size)
    out->failed = true;
}

void Name(const char *file, void *arg) {
  __gcov_filename_to_gcfn(file, Put, arg);
}

void *Scratch(unsigned size, void *arg) {
  auto *out = static_cast<Out *>(arg);
  void *block = std::malloc(size ? size : 1);
  out->blocks.push_back(block);
  return block;
}

size_t Objects() {
  const gcov_info *const *begin = __start_gcov_info;
  const gcov_info *const *end = __stop_gcov_info;
  // GCC's manual: hide the linker symbols from the optimiser.
  __asm__("" : "+r"(begin));
  __asm__("" : "+r"(end));
  return size_t(end - begin);
}

} // namespace

void PgoWriteNow() {
  std::lock_guard lock(g_writeMutex);
  if (g_path.empty())
    return;
  std::filesystem::path tmp = g_path;
  tmp += ".tmp";
  Out out;
  out.file = std::fopen(tmp.string().c_str(), "wb");
  if (!out.file)
    return;
  static std::vector<char> buffer(1 << 20);
  std::setvbuf(out.file, buffer.data(), _IOFBF, buffer.size());
  const gcov_info *const *info = __start_gcov_info;
  const size_t count = Objects();
  for (size_t i = 0; i < count && !out.failed; ++i)
    if (info[i])
      __gcov_info_to_gcda(info[i], Name, Put, Scratch, &out);
  const bool closed = std::fclose(out.file) == 0;
  for (void *block : out.blocks)
    std::free(block);
  if (out.failed || !closed)
    return;
  std::error_code ec;
  std::filesystem::rename(tmp, g_path, ec);
  if (ec) {
    std::filesystem::remove(g_path, ec);
    std::filesystem::rename(tmp, g_path, ec);
  }
}

void PgoStart() {
  const std::filesystem::path dir = bd::AppRootFolder() / "pgo";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  for (int n = 1; n < 10000 && g_path.empty(); ++n) {
    const auto file = dir / ("reblue_pgo_" + std::to_string(n) + ".stream");
    auto tmp = file;
    tmp += ".tmp";
    if (!std::filesystem::exists(file, ec) && !std::filesystem::exists(tmp, ec))
      g_path = file;
  }
  BD_INFO("[pgo] instrumented build: {} objects counted, written every {} s "
          "to {}",
          Objects(), kInterval.count(), g_path.string());
  if (g_path.empty())
    return;
  g_writer = std::thread([] {
    std::unique_lock lock(g_waitMutex);
    while (!g_wait.wait_for(lock, kInterval, [] { return g_quit; })) {
      lock.unlock();
      PgoWriteNow();
      lock.lock();
    }
  });
}

void PgoStop() {
  {
    std::lock_guard lock(g_waitMutex);
    g_quit = true;
  }
  g_wait.notify_all();
  if (g_writer.joinable())
    g_writer.join();
  PgoWriteNow();
}

} // namespace bd::platform

#else

namespace bd::platform {
void PgoStart() {}
void PgoWriteNow() {}
void PgoStop() {}
} // namespace bd::platform

#endif
