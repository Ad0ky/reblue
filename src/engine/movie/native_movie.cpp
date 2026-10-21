/**
 * @file    engine/movie/native_movie.cpp
 * @brief   Cutscene video decoded on the host with FFmpeg instead of by the
 *          game's recompiled CRI Sofdec decoder.
 *
 * Blue Dragon's prerendered movies are .sfd files: MPEG-1 video at 1280x720
 * and 29.97 fps, with B pictures, plus CRI ADX audio. The game plays them with
 * CRI's mwPly player, whose MPEG decoder (MPV) runs as recompiled guest code on
 * the movie thread (ADXM_MwThreadMain). On the Switch that decoder takes more
 * than a whole core (103 % in the profile, 80 % of it in slice decoding), falls
 * behind, and the cutscene video and voices stall.
 *
 * With bd_native_movies on, the guest player keeps doing everything but the
 * pixel work: it still reads the file, splits the streams, plays the ADX
 * audio, parses every picture header, keeps the timing and hands the game a
 * frame buffer per displayed picture. Only its slice decoding is skipped,
 * and right before the game copies a frame buffer's Y/U/V planes into its
 * textures they are filled from the same picture decoded by FFmpeg on a host
 * thread (engine/movie/sfd_video.cpp).
 *
 * Which picture a frame buffer holds: the guest decodes the pictures of the
 * stream in order, one sub_827144A8 call each, and its parameter block names
 * the frame buffer the picture goes into (+48). Counting those calls gives the
 * picture's place in stream order, which is also how the host decoder numbers
 * them. A buffer is reused, so the map always holds its latest picture.
 *
 * Checks before any slice is skipped: the guest decodes the movie normally
 * until its first frame is displayed, and that frame is compared with the host
 * picture of the same number. Only if they agree (which also proves the
 * numbering and settles the U/V plane order) does skipping start. Otherwise,
 * or if the host decoder fails later on, the movie stays on (or goes back to)
 * the guest decoder.
 *
 * Guest functions (CRI Sofdec, unnamed in the image):
 *   sub_826FDB98(ply, name)        mwPlyStartFname
 *   sub_826FC430(ply, name)        mwPlyEntryFname (seamless list)
 *   sub_826FC800(ply)              mwPlyStartSeamless
 *   sub_826FDA10(ply)              mwPlyStop
 *   sub_826FEDB8(buf, w, h, out)   Y/U/V addresses (out+0..8) and row sizes
 *                                  (out+12..20) of a frame buffer
 *   sub_827144A8(dec, sj, prm)     MPV: decode one picture; prm+48 the frame
 *                                  buffer, prm+52 the picture info (+24 type)
 *   sub_8271AED8(dec, sj, out)     MPV: decode one slice
 * The bitstream (sj) is CRI's SJ stream interface: vtable +24 GetChunk(sj,
 * lane, max, ck), +28 UngetChunk(sj, lane, ck), +32 PutChunk(sj, lane, ck),
 * with ck = {data, length}; lane 1 holds the data, lane 0 takes what is used.
 */
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/ppc/func.h>
#include <rex/types.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "engine/movie/sfd_video.h"
#include "platform/switch_tuning.h"
#include "vfs/key.h"
#include "vfs/vfs.h"

#if defined(__SWITCH__)
#define BD_NATIVE_MOVIES_DEFAULT true
#else
#define BD_NATIVE_MOVIES_DEFAULT false
#endif

REXCVAR_DEFINE_BOOL(bd_native_movies, BD_NATIVE_MOVIES_DEFAULT, "Movies",
                    "Decode the cutscene video on the host with FFmpeg instead "
                    "of the game's own MPEG decoder (on by default on the "
                    "Switch, where the game's decoder cannot keep up). Read "
                    "when a movie starts.");

REX_EXTERN(__imp__sub_826FDB98);
REX_EXTERN(__imp__sub_826FC430);
REX_EXTERN(__imp__sub_826FC800);
REX_EXTERN(__imp__sub_826FDA10);
REX_EXTERN(__imp__sub_826FEDB8);
REX_EXTERN(__imp__sub_827144A8);
REX_EXTERN(__imp__sub_8271AED8);

namespace bd::engine::movie {

namespace {

namespace fs = std::filesystem;

enum class State {
  Checking, // the guest decodes; waiting for its first frame to compare
  Native,   // slices skipped, frames from the host decoder
  Off,      // the guest decoder, as without this file
};

const char *ToString(State s) {
  switch (s) {
  case State::Checking:
    return "checking";
  case State::Native:
    return "native";
  case State::Off:
    return "guest decoder";
  }
  return "?";
}

// One mwPly playback: from a start to its stop.
struct Session {
  u32 player = 0;
  std::vector<std::string> names; // as the game gave them
  std::vector<std::string> paths; // on the host
  std::vector<std::string> pending; // entered after the start
  bool started = false;
  State state = State::Off;
  std::unique_ptr<bd::movie::SfdVideo> video;

  u64 pictures = 0; // pictures the guest decoder has started, in stream order
  std::unordered_map<u32, u64> bufferPicture;
  bool swapChroma = false;

  // Statistics, for the log line at the end.
  u64 shown = 0, missing = 0, unknown = 0, checks = 0;
  u32 loggedDecodes = 0, loggedFrames = 0;

  std::string Label() const {
    std::string s;
    for (const auto &n : names) {
      if (!s.empty())
        s += " + ";
      s += n;
    }
    return s.empty() ? "?" : s;
  }
};

std::mutex g_mutex;
std::shared_ptr<Session> g_session;

// Calls made from inside another hooked player call (mwPlyStartFname runs
// its own stop, entry and start) are the player's business, not a new list.
thread_local int t_depth = 0;
// Set by the picture hook for the slices of a picture whose pixels the host
// provides.
thread_local bool t_skipSlices = false;

struct Depth {
  Depth() { ++t_depth; }
  ~Depth() { --t_depth; }
};

std::string GuestString(u32 address) {
  const char *s = bd::mem::str(address);
  return std::string(s, strnlen(s, 260));
}

// The host file for a movie name. The engine's paths are case-insensitive;
// the host file system may not be.
std::string Resolve(const std::string &name) {
  const auto key = bd::vfs::Key::FromGuestPath(name);
  std::string rel = key.string();
  std::replace(rel.begin(), rel.end(), '\\', '/');
  const fs::path root = bd::vfs::VFS::Get().Paths().Game();
  std::error_code ec;
  fs::path direct = root / rel;
  if (fs::is_regular_file(direct, ec))
    return direct.string();
  fs::path at = root;
  for (const auto &part : fs::path(rel)) {
    fs::path next;
    for (const auto &entry : fs::directory_iterator(at, ec)) {
      std::string a = entry.path().filename().string();
      std::string b = part.string();
      if (a.size() == b.size() &&
          std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
            return std::tolower(u8(x)) == std::tolower(u8(y));
          })) {
        next = entry.path();
        break;
      }
    }
    if (next.empty())
      return {};
    at = next;
  }
  return fs::is_regular_file(at, ec) ? at.string() : std::string();
}

void LogEnd(const Session &s, const char *why) {
  if (!s.video) {
    return;
  }
  const auto stats = s.video->Stats();
  const std::string error = s.video->LastError();
  BD_INFO("native movie: {} {} ({}): {} pictures from the game, {} frames "
          "shown from FFmpeg, {} not ready, {} unknown buffers; FFmpeg decoded "
          "{} ({:.0f} fps of decoding){}{}",
          s.Label(), why, ToString(s.state), s.pictures, s.shown, s.missing,
          s.unknown, stats.decoded,
          stats.decodeSeconds > 0 ? stats.decoded / stats.decodeSeconds : 0.0,
          error.empty() ? "" : ", error: ", error);
}

// Under g_mutex. Replaces any session there was.
void Begin(u32 player, std::vector<std::string> names) {
  if (g_session)
    LogEnd(*g_session, "replaced");
  auto s = std::make_shared<Session>();
  s->player = player;
  s->names = std::move(names);
  s->started = true;
  g_session = s;
  if (!REXCVAR_GET(bd_native_movies))
    return;
  for (const auto &n : s->names) {
    std::string path = Resolve(n);
    if (path.empty()) {
      BD_WARN("native movie: {} not found under the game folder; the game's "
              "decoder plays it",
              n);
      return;
    }
    s->paths.push_back(std::move(path));
  }
  auto video = std::make_unique<bd::movie::SfdVideo>();
  std::string error;
  if (!video->Open(s->paths, error)) {
    BD_WARN("native movie: {}: {}; the game's decoder plays it", s->Label(),
            error);
    return;
  }
  video->Start([] { bd::platform::SpreadCurrentThread(2); });
  BD_INFO("native movie: {} ({}x{}, {})", s->Label(), video->Width(),
          video->Height(), s->paths.front());
  s->video = std::move(video);
  s->state = State::Checking;
}

void End(u32 player, const char *why) {
  std::shared_ptr<Session> s;
  {
    std::lock_guard lock(g_mutex);
    if (!g_session || g_session->player != player)
      return;
    s = std::move(g_session);
  }
  LogEnd(*s, why);
}

// Mean absolute difference of two planes, every 4th sample of every 4th row.
double Difference(const u8 *a, int strideA, const u8 *b, int strideB, u32 w,
                  u32 h) {
  u64 sum = 0, n = 0;
  for (u32 y = 0; y < h; y += 4) {
    const u8 *ra = a + size_t(y) * strideA;
    const u8 *rb = b + size_t(y) * strideB;
    for (u32 x = 0; x < w; x += 4) {
      sum += u32(std::abs(int(ra[x]) - int(rb[x])));
      ++n;
    }
  }
  return n ? double(sum) / double(n) : 255.0;
}

// The game is about to copy frame buffer 'buffer' into its textures.
void Present(u32 buffer, u32 w, u32 h, u32 out) {
  std::shared_ptr<Session> s;
  u64 picture = 0;
  {
    std::lock_guard lock(g_mutex);
    s = g_session;
    if (!s || s->state == State::Off || !s->video)
      return;
    const auto it = s->bufferPicture.find(buffer);
    if (s->loggedFrames < 3) {
      ++s->loggedFrames;
      BD_INFO("native movie: frame {} in buffer {:#x}, picture {}", s->shown,
              buffer,
              it == s->bufferPicture.end() ? std::string("unknown")
                                           : std::to_string(it->second));
    }
    if (it == s->bufferPicture.end()) {
      ++s->unknown;
      if (s->state == State::Checking && s->unknown >= 30) {
        s->state = State::Off;
        BD_WARN("native movie: {}: the game shows frame buffers the decoder "
                "was never seen writing; the game's decoder plays it",
                s->Label());
      }
      return;
    }
    picture = it->second;
  }

  const bool checking = s->state == State::Checking;
  auto frame = s->video->ByDecodeIndex(picture, checking ? 1000 : 100);
  std::lock_guard lock(g_mutex);
  if (s->state == State::Off)
    return;
  if (!frame) {
    ++s->missing;
    if (s->video->Failed() || checking) {
      // The guest decoder has to come back; while slices were skipped the
      // pictures it holds are empty until its next I picture.
      s->state = State::Off;
      BD_WARN("native movie: {}: no picture {} from FFmpeg ({}); the game's "
              "decoder plays the rest",
              s->Label(), picture,
              s->video->Failed() ? s->video->LastError() : "not in time");
    }
    return;
  }
  if (frame->Width() != w || frame->Height() != h) {
    s->state = State::Off;
    BD_WARN("native movie: {}: the game's frames are {}x{}, FFmpeg's {}x{}; "
            "the game's decoder plays it",
            s->Label(), w, h, frame->Width(), frame->Height());
    return;
  }

  const u32 y = bd::mem::load<u32>(out + 0);
  const u32 u = bd::mem::load<u32>(out + 4);
  const u32 v = bd::mem::load<u32>(out + 8);
  const u32 py = bd::mem::load<u32>(out + 12);
  const u32 pu = bd::mem::load<u32>(out + 16);
  const u32 pv = bd::mem::load<u32>(out + 20);
  u8 *gy = bd::mem::at<u8>(y);
  u8 *gu = bd::mem::at<u8>(u);
  u8 *gv = bd::mem::at<u8>(v);
  if (!gy || !gu || !gv || py < w || pu < w / 2 || pv < w / 2) {
    s->state = State::Off;
    BD_WARN("native movie: {}: unexpected planes Y {:#x}/{} U {:#x}/{} V "
            "{:#x}/{}; the game's decoder plays it",
            s->Label(), y, py, u, pu, v, pv);
    return;
  }
  const u32 cw = w / 2, ch = h / 2;

  if (checking) {
    // This one the game decoded itself: it must be the same picture.
    const double dy = Difference(gy, int(py), frame->Plane(0),
                                 frame->Stride(0), w, h);
    const double dcb = Difference(gu, int(pu), frame->Plane(1),
                                  frame->Stride(1), cw, ch);
    const double dcr = Difference(gu, int(pu), frame->Plane(2),
                                  frame->Stride(2), cw, ch);
    ++s->checks;
    if (dy < 4.0 && std::min(dcb, dcr) < 4.0) {
      s->swapChroma = dcr < dcb;
      s->state = State::Native;
      BD_INFO("native movie: {}: picture {} matches the game's (difference "
              "Y {:.2f}, U {:.2f}){}; FFmpeg decodes the video from here",
              s->Label(), picture, dy, std::min(dcb, dcr),
              s->swapChroma ? ", planes U/V swapped" : "");
    } else {
      BD_WARN("native movie: {}: picture {} differs from the game's "
              "(difference Y {:.2f}, U {:.2f} / {:.2f})",
              s->Label(), picture, dy, dcb, dcr);
      if (s->checks >= 3) {
        s->state = State::Off;
        BD_WARN("native movie: {}: the game's decoder plays it", s->Label());
      }
    }
    return; // the game's own frame is shown this time
  }

  const int pcb = s->swapChroma ? 2 : 1, pcr = s->swapChroma ? 1 : 2;
  for (u32 row = 0; row < h; ++row)
    std::memcpy(gy + size_t(row) * py,
                frame->Plane(0) + size_t(row) * frame->Stride(0), w);
  for (u32 row = 0; row < ch; ++row) {
    std::memcpy(gu + size_t(row) * pu,
                frame->Plane(pcb) + size_t(row) * frame->Stride(pcb), cw);
    std::memcpy(gv + size_t(row) * pv,
                frame->Plane(pcr) + size_t(row) * frame->Stride(pcr), cw);
  }
  ++s->shown;
}

// Calls guest function 'target' with the context as set up.
void CallGuest(PPCContext &ctx, u8 *base, u32 target) {
  rex::runtime::ResolveIndirectFunction(target)(ctx, base);
}

} // namespace

} // namespace bd::engine::movie

using namespace bd::engine::movie;

// mwPlyStartFname(ply, name)
REX_HOOK_RAW(sub_826FDB98) {
  if (t_depth) {
    __imp__sub_826FDB98(ctx, base);
    return;
  }
  Depth depth;
  {
    std::lock_guard lock(g_mutex);
    Begin(ctx.r3.u32, {GuestString(ctx.r4.u32)});
  }
  __imp__sub_826FDB98(ctx, base);
}

// mwPlyEntryFname(ply, name): one more file for the seamless list.
REX_HOOK_RAW(sub_826FC430) {
  if (!t_depth) {
    const u32 player = ctx.r3.u32;
    std::string name = GuestString(ctx.r4.u32);
    std::lock_guard lock(g_mutex);
    if (!g_session || g_session->player != player) {
      if (g_session)
        LogEnd(*g_session, "replaced");
      g_session = std::make_shared<Session>();
      g_session->player = player;
    }
    Session &s = *g_session;
    if (!s.started) {
      s.names.push_back(std::move(name));
    } else {
      // During playback: the list grows, or a new list is being entered for
      // the next mwPlyStartSeamless.
      if (s.video && s.state != State::Off) {
        const std::string path = Resolve(name);
        if (!path.empty())
          s.video->Append(path);
      }
      s.pending.push_back(std::move(name));
    }
  }
  __imp__sub_826FC430(ctx, base);
}

// mwPlyStartSeamless(ply): plays the entered list.
REX_HOOK_RAW(sub_826FC800) {
  if (t_depth) {
    __imp__sub_826FC800(ctx, base);
    return;
  }
  Depth depth;
  {
    const u32 player = ctx.r3.u32;
    std::lock_guard lock(g_mutex);
    if (g_session && g_session->player == player) {
      std::vector<std::string> names = g_session->started
                                           ? g_session->pending
                                           : g_session->names;
      if (!names.empty())
        Begin(player, std::move(names));
    }
  }
  __imp__sub_826FC800(ctx, base);
}

// mwPlyStop(ply)
REX_HOOK_RAW(sub_826FDA10) {
  if (!t_depth)
    End(ctx.r3.u32, "stopped");
  __imp__sub_826FDA10(ctx, base);
}

// MPV: decode one picture. Runs on the movie thread.
REX_HOOK_RAW(sub_827144A8) {
  const u32 params = ctx.r5.u32;
  bool skip = false;
  {
    std::lock_guard lock(g_mutex);
    Session *s = g_session.get();
    if (s && s->started && s->state != State::Off && s->video) {
      const u32 buffer = bd::mem::load<u32>(params + 48);
      const u64 picture = s->pictures++;
      s->bufferPicture[buffer] = picture;
      skip = s->state == State::Native;
      if (s->loggedDecodes < 3) {
        ++s->loggedDecodes;
        const u32 info = bd::mem::load<u32>(params + 52);
        BD_INFO("native movie: picture {} (type {}) into buffer {:#x}",
                picture, info ? bd::mem::load<u32>(info + 24) : 0, buffer);
      }
    }
  }
  t_skipSlices = skip;
  __imp__sub_827144A8(ctx, base);
  t_skipSlices = false;
}

// MPV: decode one slice. While the host provides the pixels the slice is
// stepped over instead: its bytes up to the next start code are consumed, and
// the outputs are those of a slice that decoded without errors. After the
// last slice of the picture (the next code is not a slice) the macroblock
// position is the last one, so no concealment runs, and the picture is
// reported complete, as the decoder does. When the next start code is not in
// the bytes at hand (the end of the buffer's contiguous part), only the slice
// start code is consumed: the decoder's own search for the next start code
// (sub_82713468, before every slice) then steps over the rest.
REX_HOOK_RAW(sub_8271AED8) {
  if (!t_skipSlices) {
    __imp__sub_8271AED8(ctx, base);
    return;
  }
  const u32 dec = ctx.r3.u32, sj = ctx.r4.u32, out = ctx.r5.u32;

  const u32 sp = ctx.r1.u32;
  ctx.r1.u32 = sp - 128;
  const u32 chunk = ctx.r1.u32 + 80, rest = chunk + 8;
  const u32 vtable = bd::mem::load<u32>(sj);
  ctx.r3.u64 = sj;
  ctx.r4.u64 = 1;
  ctx.r5.u64 = 0x7FFFFFFF;
  ctx.r6.u64 = chunk;
  CallGuest(ctx, base, bd::mem::load<u32>(vtable + 24)); // GetChunk
  const u32 data = bd::mem::load<u32>(chunk + 0);
  const u32 length = bd::mem::load<u32>(chunk + 4);
  const u8 *bytes = length ? bd::mem::at<u8>(data) : nullptr;

  u32 used = length >= 4 ? 4 : 0;
  bool lastSlice = false;
  if (bytes && length >= 4) {
    for (u32 i = 4; i + 4 <= length; ++i) {
      if (bytes[i + 2] > 1) {
        i += 2;
        continue;
      }
      if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1) {
        used = i;
        const u8 code = bytes[i + 3];
        lastSlice = code == 0 || code > 0xAF;
        break;
      }
    }
  }
  if (used) {
    bd::mem::store<u32>(rest + 0, data + used);
    bd::mem::store<u32>(rest + 4, length - used);
    bd::mem::store<u32>(chunk + 4, used);
    ctx.r3.u64 = sj;
    ctx.r4.u64 = 0;
    ctx.r5.u64 = chunk;
    CallGuest(ctx, base, bd::mem::load<u32>(vtable + 32)); // PutChunk
    ctx.r3.u64 = sj;
    ctx.r4.u64 = 1;
    ctx.r5.u64 = rest;
    CallGuest(ctx, base, bd::mem::load<u32>(vtable + 28)); // UngetChunk
  } else {
    ctx.r3.u64 = sj;
    ctx.r4.u64 = 1;
    ctx.r5.u64 = chunk;
    CallGuest(ctx, base, bd::mem::load<u32>(vtable + 28)); // UngetChunk
  }
  ctx.r1.u32 = sp;

  const u32 lastMb = bd::mem::load<u32>(dec + 868);
  bd::mem::store<u32>(dec + 856, lastMb); // the macroblock reached
  bd::mem::store<u32>(out + 0, 0);
  bd::mem::store<u32>(out + 4, 0);
  bd::mem::store<u32>(out + 8, 0); // errors
  bd::mem::store<u32>(out + 12, lastMb);
  bd::mem::store<u32>(out + 16, lastMb);
  bd::mem::store<u32>(out + 20, lastSlice ? 1 : 0);
}

// The frame buffer's planes, right before the game copies them into its
// textures (sub_8212FF10): put the host picture in them.
REX_HOOK_RAW(sub_826FEDB8) {
  const u32 buffer = ctx.r3.u32, w = ctx.r4.u32, h = ctx.r5.u32,
            out = ctx.r6.u32;
  __imp__sub_826FEDB8(ctx, base);
  if (buffer && out)
    Present(buffer, w, h, out);
}
