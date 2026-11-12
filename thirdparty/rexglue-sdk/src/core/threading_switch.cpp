/**
 * @file        rex/core/threading_switch.cpp
 * @brief       Thread shim for Horizon: affinity, priority and suspension
 *
 * See threading_switch.h for why this layer exists and for the priority
 * scheme, which is what decides whether the game stutters or not.
 */

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH

#include "threading_switch.h"

/*
 * switch.h is included here, privately. This file uses none of the rex::thread
 * types, so libnx polluting the global namespace (Thread, Timer, Event,
 * Handle...) does no harm. The header above does not include it, precisely so
 * that threading_posix.cpp does not suffer from it.
 */
#include <switch.h>

#include <errno.h>
#include <sched.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <sys/stat.h>  // mkdir (RexSwitchEnsureForwarderNpdm)

#include <rex/cvar.h>

// re:Blue (2026-11-12): four-core mode, from renut-nx / banjo-nx (NetherSX2_nx's, MIT, NaGaa95). See threading_switch.h.
REXCVAR_DEFINE_INT32(switch_system_core, 1, "Kernel",
                     "Switch: when the process is granted all four cores (a forwarder whose NPDM allows CPU 0-3): "
                     "0 = leave core 3 to the system, 1 = the audio output thread, the Audio Worker and shader "
                     "compiles (and what switch_system_core_xma / _mixer add) each pinned to core 3, every other "
                     "thread on cores 0-2, 2 = as 1 and the other threads allowed on core 3 too. No effect with the "
                     "usual three cores")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(switch_system_core_xma, false, "Kernel",
                    "Switch four-core mode: the XMA Decoder on the system core too")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(switch_system_core_mixer, 0, "Kernel",
                     "Switch four-core mode: how many of the game's XAudio engine threads (switch_guest_sound_entries) "
                     "go to the system core. 0 = none (the reverb can take most of a core, and the system keeps about "
                     "half of core 3); watch 'system core 3: idle' in rex_perfil.log before raising it")
    .range(0, 8)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(switch_system_core_mixer_priority, 0x2D, "Kernel",
                     "Switch four-core mode: the XAudio engine threads' priority on the system core (0x2D: below the "
                     "audio output and the Audio Worker, above the game threads' 0x3B)")
    .range(0x2C, 0x3B)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(switch_system_core_compiles, true, "Kernel",
                    "Switch four-core mode: the deko3d shader compile thread on the system core")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(switch_forwarder_program_id, "", "Kernel",
                      "Switch: program id (16 hex digits) of the forwarder that starts the game. Under that id, or "
                      "under the id Sphaira gives a forwarder of this NRO (found without this key), the game writes "
                      "sdmc:/atmosphere/contents/<id>/exefs/main.npdm (an NPDM granting CPU 0-3, Atmosphere's ExeFS "
                      "file replacement) when it is missing or different; the next launch gets all four cores. Under "
                      "any other id nothing is written. \"off\": never")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

/*
 * Registry pthread_t -> Horizon Handle.
 *
 * libnx exposes no way to get the handle of another thread, and the SDK
 * changes the priority and affinity of other threads, so it has to be tracked
 * by hand.
 *
 * Not a hot path: it is touched when threads are created and destroyed, never
 * per frame. A plain mutex is more than enough.
 */
struct Registro {
  std::mutex m;
  std::unordered_map<uintptr_t, RexSwitchHandle> mapa;
};

Registro& registro() {
  static Registro r;
  return r;
}

}  // namespace

extern "C" {

void RexSwitchRegisterCurrentThread(void) {
  auto& r = registro();
  std::lock_guard<std::mutex> lock(r.m);
  r.mapa[reinterpret_cast<uintptr_t>(pthread_self())] = threadGetCurHandle();
}

void RexSwitchUnregisterCurrentThread(void) {
  auto& r = registro();
  std::lock_guard<std::mutex> lock(r.m);
  r.mapa.erase(reinterpret_cast<uintptr_t>(pthread_self()));
}

RexSwitchHandle RexSwitchHandleFor(pthread_t thread) {
  if (pthread_equal(thread, pthread_self()))
    return threadGetCurHandle();

  auto& r = registro();
  std::lock_guard<std::mutex> lock(r.m);
  auto it = r.mapa.find(reinterpret_cast<uintptr_t>(thread));
  return it == r.mapa.end() ? REX_SWITCH_INVALID_HANDLE : it->second;
}

/* --- afinidad ---------------------------------------------------------- */

int pthread_getaffinity_np(pthread_t thread, size_t size, cpu_set_t* set) {
  if (set == nullptr || size < sizeof(cpu_set_t))
    return EINVAL;

  RexSwitchHandle h = RexSwitchHandleFor(thread);
  if (h == REX_SWITCH_INVALID_HANDLE)
    return ESRCH;

  s32 preferido = 0;
  u64 mascara = 0;
  if (R_FAILED(svcGetThreadCoreMask(&preferido, &mascara, h)))
    return EINVAL;

  set->bits = mascara;
  return 0;
}

int pthread_setaffinity_np(pthread_t thread, size_t size, const cpu_set_t* set) {
  if (set == nullptr || size < sizeof(cpu_set_t))
    return EINVAL;

  RexSwitchHandle h = RexSwitchHandleFor(thread);
  if (h == REX_SWITCH_INVALID_HANDLE)
    return ESRCH;

  /*
   * Horizon also wants a preferred core. The lowest one in the mask is
   * taken; -1 would mean "whatever it already had", which is not what the
   * POSIX API being imitated asks for.
   */
  s32 preferido = -1;
  for (int i = 0; i < 4; i++) {
    if (set->bits & (1ull << i)) { preferido = i; break; }
  }
  if (preferido < 0)
    return EINVAL;  /* mascara vacia: POSIX tampoco lo permite */

  /*
   * Only 4 cores exist. Asking for more is not a caller error (the SDK
   * passes 64-bit masks), so it is trimmed silently.
   */
  const u32 mascara = static_cast<u32>(set->bits & 0xF);

  if (R_FAILED(svcSetThreadCoreMask(h, preferido, mascara)))
    return EPERM;
  return 0;
}

/*
 * --- priority ------------------------------------------------------------
 *
 * sched_param::sched_priority is passed straight through as the Horizon
 * priority, without inverting the scale. This is deliberate: callers using the
 * REX_SWITCH_PRIO_* constants expect Horizon numbers, where lower means higher
 * priority. Inverting it here to look like POSIX would only turn the priority
 * scheme upside down, which is exactly the bug that causes stutter and audio
 * dropouts.
 */

int pthread_getschedparam(pthread_t thread, int* policy, struct sched_param* param) {
  RexSwitchHandle h = RexSwitchHandleFor(thread);
  if (h == REX_SWITCH_INVALID_HANDLE)
    return ESRCH;

  s32 prio = 0;
  if (R_FAILED(svcGetThreadPriority(&prio, h)))
    return EINVAL;

  if (policy) *policy = SCHED_FIFO;
  if (param) param->sched_priority = prio;
  return 0;
}

int pthread_setschedparam(pthread_t thread, int policy, const struct sched_param* param) {
  (void)policy;
  if (param == nullptr)
    return EINVAL;

  RexSwitchHandle h = RexSwitchHandleFor(thread);
  if (h == REX_SWITCH_INVALID_HANDLE)
    return ESRCH;

  if (R_FAILED(svcSetThreadPriority(h, static_cast<u32>(param->sched_priority))))
    return EPERM;
  return 0;
}

int pthread_attr_setschedpolicy(pthread_attr_t* attr, int policy) {
  (void)attr;
  (void)policy;
  /* Horizon has a single policy. Accepting and doing nothing is correct. */
  return 0;
}

/* --- mutex robustos: no existen ---------------------------------------- */

int pthread_mutexattr_setrobust(pthread_mutexattr_t* attr, int robustness) {
  (void)attr;
  /*
   * PTHREAD_MUTEX_STALLED is accepted (the real behavior), and ROBUST is
   * accepted without providing it: the SDK uses it as a safeguard against a
   * thread dying while holding the mutex, which should not happen in the guest.
   */
  if (robustness != PTHREAD_MUTEX_STALLED && robustness != PTHREAD_MUTEX_ROBUST)
    return EINVAL;
  return 0;
}

int pthread_mutex_consistent(pthread_mutex_t* mutex) {
  (void)mutex;
  return 0;
}

int pthread_mutexattr_getrobust(const pthread_mutexattr_t* attr, int* robustness) {
  (void)attr;
  if (robustness) *robustness = PTHREAD_MUTEX_STALLED;
  return 0;
}

/*
 * --- suspension ----------------------------------------------------------
 *
 * The SDK suspends threads by sending them a signal. Horizon does not deliver
 * signals, so that cannot work however much shimming is added: the system
 * mechanism has to be used.
 *
 * A difference in semantics worth knowing: svcSetThreadActivity stops the
 * thread wherever it is, not at a safe point chosen by the thread itself. For
 * what the SDK does with this (suspending guest threads, which is what the
 * original console did) that is correct. For any use that needs the thread to
 * stop at a specific place, it is not.
 */

int RexSwitchPauseThread(pthread_t thread) {
  RexSwitchHandle h = RexSwitchHandleFor(thread);
  if (h == REX_SWITCH_INVALID_HANDLE)
    return ESRCH;
  if (pthread_equal(thread, pthread_self()))
    return EDEADLK;  /* pausing oneself cannot be undone */
  return R_SUCCEEDED(svcSetThreadActivity(h, ThreadActivity_Paused)) ? 0 : EPERM;
}

int RexSwitchResumeThread(pthread_t thread) {
  RexSwitchHandle h = RexSwitchHandleFor(thread);
  if (h == REX_SWITCH_INVALID_HANDLE)
    return ESRCH;
  return R_SUCCEEDED(svcSetThreadActivity(h, ThreadActivity_Runnable)) ? 0 : EPERM;
}

/* --- priority of the current thread ------------------------------------ */

uint32_t RexSwitchCurrentThreadId(void) {
  /* The current thread's handle serves as a unique id, stable while it lives. */
  return static_cast<uint32_t>(threadGetCurHandle());
}

/*
 * threadGetSelf() only returns something for threads that libnx created with threadCreate. Ours
 * come from pthread_create (rex::thread::Thread::Create -> PosixThread), so if the TLS does not carry
 * libnx's ThreadVars, this used to be a silent no-op: the priority was not applied and the log said
 * it was, because it only printed the cvar value.
 *
 * CUR_THREAD_HANDLE (0xFFFF8000) is the pseudo-handle that the Horizon kernel always understands as
 * "the calling thread", wherever it came from. The result is returned so that it can be checked in
 * the log instead of assumed.
 */
bool RexSwitchSetCurrentThreadPriorityOk(int priority) {
  Thread* yo = threadGetSelf();
  Handle h = (yo != nullptr) ? yo->handle : CUR_THREAD_HANDLE;
  if (R_SUCCEEDED(svcSetThreadPriority(h, static_cast<u32>(priority)))) {
    return true;
  }
  if (h != CUR_THREAD_HANDLE) {
    return R_SUCCEEDED(svcSetThreadPriority(CUR_THREAD_HANDLE, static_cast<u32>(priority)));
  }
  return false;
}

void RexSwitchSetCurrentThreadPriority(int priority) {
  (void)RexSwitchSetCurrentThreadPriorityOk(priority);
}

/*
 * Preferred core of the current thread.
 *
 * All our threads run with "preferred core -1", which on Horizon means the process default core:
 * the same one for all of them. With the ring at 94.7 % of a core and the game's main thread at
 * 88.2 %, both prefer the same place and the kernel has to keep moving them. Each migration throws
 * away the thread's L1 and part of its L2, and that does not show in the mean: it shows in the
 * variance, which is exactly what the player notices.
 *
 * The mask is not touched. The thread's existing mask is passed, so this only says "start here",
 * not "you may only run here": if the core is busy, the kernel can still move the thread. That is
 * why it cannot cause starvation, which is what makes real affinity pinning dangerous.
 *
 * Returns false if the kernel does not accept it (for example if the core is not in the process
 * mask: that is the "Too few processor cores" error seen when trying to use the fourth core).
 */
bool RexSwitchSetCurrentThreadCore(int core) {
  Thread* yo = threadGetSelf();
  Handle h = (yo != nullptr) ? yo->handle : CUR_THREAD_HANDLE;
  s32 preferido = 0;
  u64 mascara = 0;
  if (R_FAILED(svcGetThreadCoreMask(&preferido, &mascara, h))) {
    if (h == CUR_THREAD_HANDLE ||
        R_FAILED(svcGetThreadCoreMask(&preferido, &mascara, CUR_THREAD_HANDLE))) {
      return false;
    }
    h = CUR_THREAD_HANDLE;
  }
  if (core >= 0 && (mascara & (u64(1) << core)) == 0) {
    return false;  // that core is not this process's: do not force it
  }
  return R_SUCCEEDED(svcSetThreadCoreMask(h, core, mascara));
}

/*
 * The real pin, with the mask reduced to a single core.
 *
 * Measured: a preferred core is not enough. Horizon accepts it and moves the thread anyway (0.24
 * migrations per loop, wherever it is set). This takes the other cores away from the thread.
 *
 * Different from RexSwitchSetCurrentThreadCore and more dangerous: if that core saturates, the
 * thread does not get to run. That is why it sits behind a cvar that is off by default and is
 * checked in the log. See docs/platform-notes.md (Threads).
 */
bool RexSwitchPinCurrentThreadToCore(int core) {
  if (core < 0) {
    return false;
  }
  Thread* yo = threadGetSelf();
  Handle h = (yo != nullptr) ? yo->handle : CUR_THREAD_HANDLE;
  s32 preferido = 0;
  u64 mascara = 0;
  if (R_FAILED(svcGetThreadCoreMask(&preferido, &mascara, h))) {
    if (h == CUR_THREAD_HANDLE ||
        R_FAILED(svcGetThreadCoreMask(&preferido, &mascara, CUR_THREAD_HANDLE))) {
      return false;
    }
    h = CUR_THREAD_HANDLE;
  }
  const u64 solo_ese = u64(1) << core;
  if ((mascara & solo_ese) == 0) {
    return false;  // that core is not this process's
  }
  return R_SUCCEEDED(svcSetThreadCoreMask(h, core, solo_ese));
}

/*
 * Which core the calling thread is running on right now. This is what allows counting migrations:
 * it is read on every loop iteration and counted each time it changes. There is no syscall to ask
 * it about another thread, so each thread has to look at itself.
 */
int RexSwitchCurrentCore(void) { return int(svcGetCurrentProcessorNumber()); }

/*
 * --- the system core (re:Blue 2026-11-12, from renut-nx / banjo-nx 2026-10-27; NetherSX2_nx's 4-core mode) ----
 */
namespace {
std::atomic<int> g_sys_state{0};
std::atomic<unsigned> g_process_mask{0x7};
std::atomic<int> g_system_core{-1};
std::atomic<int> g_system_mode{0};
std::atomic<unsigned> g_strip_mask{0};
std::mutex g_sys_mutex;

void ReadSystemCore(bool with_cvar) {
  u64 mask = 0;
  if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || mask == 0) mask = 0x7;
  mask &= 0xF;
  const int top = __builtin_popcountll(mask) >= 4 ? 63 - __builtin_clzll(mask) : -1;
  const int mode = top < 0 ? 0 : (with_cvar ? int(REXCVAR_GET(switch_system_core)) : 1);
  g_process_mask.store(unsigned(mask), std::memory_order_relaxed);
  g_system_mode.store(mode, std::memory_order_relaxed);
  g_system_core.store(mode >= 1 ? top : -1, std::memory_order_relaxed);
  g_strip_mask.store((top >= 0 && mode <= 1) ? (1u << top) : 0u, std::memory_order_relaxed);
  g_sys_state.store(1, std::memory_order_release);
}

inline void EnsureSystemCore() {
  if (g_sys_state.load(std::memory_order_acquire) == 0) {
    std::lock_guard<std::mutex> lock(g_sys_mutex);
    if (g_sys_state.load(std::memory_order_relaxed) == 0) ReadSystemCore(false);
  }
}

#include "forwarder_npdm.inc"  // kForwarderNpdm (renut-nx tools/forwarder, NetherSX2_nx's hbl.json, 39-bit)
std::string g_npdm_status = "not checked";

struct IdleSample {
  std::atomic<u64> idle{0}, at{0};
};
IdleSample g_idle[4];
}  // namespace

Result __real_svcSetThreadCoreMask(Handle handle, s32 preferred_core, u32 affinity_mask);
Result __wrap_svcSetThreadCoreMask(Handle handle, s32 preferred_core, u32 affinity_mask) {
  EnsureSystemCore();
  const u32 strip = g_strip_mask.load(std::memory_order_relaxed);
  // A mask of more than one core loses the system core; a request for the system core alone goes through.
  if (strip != 0 && (affinity_mask & strip) != 0 && (affinity_mask & ~strip) != 0) {
    affinity_mask &= ~strip;
    if (preferred_core >= 0 && (strip & (1u << preferred_core)) != 0) preferred_core = -1;
  }
  return __real_svcSetThreadCoreMask(handle, preferred_core, affinity_mask);
}

int RexSwitchInitSystemCore(void) {
  {
    std::lock_guard<std::mutex> lock(g_sys_mutex);
    ReadSystemCore(true);
  }
  const u32 strip = g_strip_mask.load(std::memory_order_relaxed);
  if (strip != 0) {
    s32 preferred = 0;
    u64 mask = 0;
    if (R_SUCCEEDED(svcGetThreadCoreMask(&preferred, &mask, CUR_THREAD_HANDLE)) && (mask & strip) != 0 &&
        (mask & ~u64(strip)) != 0) {
      svcSetThreadCoreMask(CUR_THREAD_HANDLE, -1, u32(mask));  // (the wrap takes the system core out)
    }
  }
  return g_system_core.load(std::memory_order_relaxed);
}

unsigned RexSwitchProcessCoreMask(void) {
  EnsureSystemCore();
  return g_process_mask.load(std::memory_order_relaxed);
}

int RexSwitchSystemCore(void) {
  EnsureSystemCore();
  return g_system_core.load(std::memory_order_relaxed);
}

int RexSwitchSystemCoreMode(void) {
  EnsureSystemCore();
  return g_system_mode.load(std::memory_order_relaxed);
}

unsigned RexSwitchAppCoreMask(void) {
  EnsureSystemCore();
  return g_process_mask.load(std::memory_order_relaxed) & ~g_strip_mask.load(std::memory_order_relaxed);
}

bool RexSwitchMoveCurrentThreadToSystemCore(int priority) {
  const int core = RexSwitchSystemCore();
  if (core < 0) return false;
  if (R_FAILED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core))) return false;
  if (priority >= 0) (void)RexSwitchSetCurrentThreadPriorityOk(priority);
  return true;
}

void RexSwitchSampleCoreIdle(void) {
  static std::atomic<u64> last_try{0};
  const u64 now = armGetSystemTick();
  if (now - last_try.load(std::memory_order_relaxed) < armNsToTicks(100000000ull)) return;  // 100 ms
  last_try.store(now, std::memory_order_relaxed);
  const int core = int(svcGetCurrentProcessorNumber());
  if (core < 0 || core >= 4) return;
  IdleSample& s = g_idle[core];
  u64 idle = 0;
  if (R_FAILED(svcGetInfo(&idle, InfoType_IdleTickCount, INVALID_HANDLE, u64(core)))) return;
  s.idle.store(idle, std::memory_order_relaxed);
  s.at.store(now, std::memory_order_release);
}

// Whether 'program' is the id Sphaira's "Install Forwarder" gives a forwarder of this NRO (sphaira source/owo.cpp):
// the first 8 bytes of the SHA-256 of the NRO path written twice, as 05xxxxxxxxxxx000, or in its native-game mode
// 01xxxxxxxxxxx000 with bit 12 clear; the path with "sdmc:" and without. A retail game's id never matches.
extern "C" int __system_argc;
extern "C" char** __system_argv;
static bool SphairaForwarderOfThisNro(u64 program) {
  if (__system_argc <= 0 || !__system_argv || !__system_argv[0] || !__system_argv[0][0]) return false;
  const std::string path = __system_argv[0];
  const std::string rooted = path.rfind("sdmc:", 0) == 0 ? path.substr(5) : path;
  for (const std::string& p : {std::string("sdmc:") + rooted, rooted}) {
    const std::string hashed = p + p;
    u64 digest[4] = {};
    sha256CalculateHash(digest, hashed.data(), hashed.size());
    const u64 h = digest[0];
    if (program == (0x0500000000000000ull | (h & 0x00FFFFFFFFFFF000ull)) ||
        program == (0x0100000000000000ull | (h & 0x00FFFFFFFFFFE000ull)))
      return true;
  }
  return false;
}

int RexSwitchEnsureForwarderNpdm(void) {
  const std::string want_text = REXCVAR_GET(switch_forwarder_program_id);
  if (want_text == "off") {
    g_npdm_status = "off (switch_forwarder_program_id = \"off\")";
    return 0;
  }
  const u64 want = want_text.empty() ? 0 : std::strtoull(want_text.c_str(), nullptr, 16);
  u64 program = 0;
  if (R_FAILED(svcGetInfo(&program, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0)) ||
      ((want == 0 || program != want) && !SphairaForwarderOfThisNro(program))) {
    char text[160];
    std::snprintf(text, sizeof(text), "not written: running as %016llX, not a forwarder of this NRO",
                  (unsigned long long)program);
    g_npdm_status = text;
    return 0;
  }
  unsigned char npdm[sizeof(kForwarderNpdm)];
  std::memcpy(npdm, kForwarderNpdm, sizeof(npdm));
  std::memcpy(npdm + kForwarderNpdmAci0 + 0x10, &program, 8);
  std::memcpy(npdm + kForwarderNpdmAcid + 0x210, &program, 8);
  std::memcpy(npdm + kForwarderNpdmAcid + 0x218, &program, 8);
  char dir[96], path[128], part[96];
  std::snprintf(dir, sizeof(dir), "sdmc:/atmosphere/contents/%016llX/exefs", (unsigned long long)program);
  std::snprintf(path, sizeof(path), "%s/main.npdm", dir);
  if (FILE* f = std::fopen(path, "rb")) {
    unsigned char have[sizeof(npdm) + 1];
    const size_t n = std::fread(have, 1, sizeof(have), f);
    std::fclose(f);
    if (n == sizeof(npdm) && std::memcmp(have, npdm, sizeof(npdm)) == 0) {
      g_npdm_status = std::string("in place: ") + path;
      return 1;
    }
  }
  for (const char* sub : {"sdmc:/atmosphere", "sdmc:/atmosphere/contents"}) mkdir(sub, 0777);
  std::snprintf(part, sizeof(part), "sdmc:/atmosphere/contents/%016llX", (unsigned long long)program);
  mkdir(part, 0777);
  mkdir(dir, 0777);
  FILE* f = std::fopen(path, "wb");
  const bool ok = f && std::fwrite(npdm, 1, sizeof(npdm), f) == sizeof(npdm);
  if (f) std::fclose(f);
  g_npdm_status =
      std::string(ok ? "written (start the game again for four cores): " : "could NOT be written: ") + path;
  return ok ? 2 : -1;
}

bool RexSwitchSystemCoreXma(void) { return RexSwitchSystemCore() >= 0 && REXCVAR_GET(switch_system_core_xma); }
int RexSwitchSystemCoreMixerThreads(void) {
  return RexSwitchSystemCore() >= 0 ? int(REXCVAR_GET(switch_system_core_mixer)) : 0;
}
int RexSwitchSystemCoreMixerPriority(void) { return REXCVAR_GET(switch_system_core_mixer_priority); }
bool RexSwitchSystemCoreCompiles(void) {
  return RexSwitchSystemCore() >= 0 && REXCVAR_GET(switch_system_core_compiles);
}

void RexSwitchSystemCoreDescribe(char* out, size_t out_len) {
  if (!out || !out_len) return;
  std::snprintf(out, out_len,
                "process cores 0x%X, system core %d (switch_system_core %d; game threads on 0x%X); forwarder NPDM %s",
                RexSwitchProcessCoreMask(), RexSwitchSystemCore(), RexSwitchSystemCoreMode(), RexSwitchAppCoreMask(),
                g_npdm_status.c_str());
}

bool RexSwitchCoreIdleSample(int core, unsigned long long* idle_ticks, unsigned long long* at_ticks) {
  if (core < 0 || core >= 4) return false;
  const IdleSample& s = g_idle[core];
  const u64 at = s.at.load(std::memory_order_acquire);
  if (at == 0) return false;
  *idle_ticks = s.idle.load(std::memory_order_relaxed);
  *at_ticks = at;
  return true;
}

}  // extern "C"

#endif  /* REX_PLATFORM_SWITCH */
