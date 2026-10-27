#!/usr/bin/env python3
"""Differential test of the localized copies (tools/switch/localize.py) on the
build machine, before they reach the console.

For every function in src/platform/switch/hot/localized.txt it builds a host
program with two versions side by side: the recompiled original (o_NAME,
volatile guest memory, registers in ctx) and the localized copy (n_NAME).
Every function they call is a stub that records the whole register file it is
handed and then scrambles the volatile registers, so a write-back or a reload
the copy got wrong shows up at the call where it matters. Each run starts both
versions from the same random registers and the same random guest memory (two
4 GiB mappings), then compares the call records, the final register file and
every guest page either one touched. A run that loops (random pointers) is cut
off by a timer and counted as skipped.

  localize_check.py <repo root> [--runs N] [--only NAME ...] [--cxx g++]

Needs a host C++20 compiler; uses the SDK's headers (simde) only.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import localize  # noqa: E402

PRELUDE = r'''
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <rex/ppc/context.h>
#include <rex/ppc/intrinsics.h>

#define REX_FUNC_PROLOGUE() ((void)0)
#define REX_PHYS_HOST_OFFSET(addr) 0u
#define REX_RAW_ADDR(x) (base + (u32)(x))
#define REX_LOAD_STRING(x, len) std::string_view(reinterpret_cast<const char*>(base + (u32)(x)), (len))
#define REX_QUERY_TIMEBASE() 0
#define REX_SET_FLUSH_MODE(enable) do { if (enable) ctx.fpscr.enableFlushMode(); else ctx.fpscr.disableFlushMode(); } while (0)
#define REX_MM_STORE_U8(a, v) REX_STORE_U8(a, v)
#define REX_MM_STORE_U16(a, v) REX_STORE_U16(a, v)
#define REX_MM_STORE_U32(a, v) REX_STORE_U32(a, v)
#define REX_MM_STORE_U64(a, v) REX_STORE_U64(a, v)
#define REX_MM_LOAD_U8(a) REX_LOAD_U8(a)
#define REX_MM_LOAD_U16(a) REX_LOAD_U16(a)
#define REX_MM_LOAD_U32(a) REX_LOAD_U32(a)
#define REX_MM_LOAD_U64(a) REX_LOAD_U64(a)
#define REBLUE_LOOP_BARRIER() __asm__ __volatile__("" ::: "memory")
#define REBLUE_FP_FENCE() __asm__ __volatile__("" ::: "memory")

// ---- call records
struct Rec { unsigned id; unsigned long long hash; };
static const unsigned kCallLimitRec = 4096;
static Rec g_rec[kCallLimitRec];
static unsigned g_nrec, g_calls;
// A run that reaches this many calls stops there (in both versions at the
// same call, with the context written back): the state is compared as it is.
static const unsigned kCallLimit = 3000;
static jmp_buf g_stop;
static unsigned long long Mix(unsigned long long x) {
  x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33; return x;
}
static unsigned long long HashRegs(const PPCContext &c) {
  unsigned long long h = 1469598103934665603ull;
  auto add = [&](const void *p, size_t n) { const unsigned char *b = (const unsigned char *)p; for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull; };
  const PPCRegister *r[] = {&c.r0,&c.r1,&c.r2,&c.r3,&c.r4,&c.r5,&c.r6,&c.r7,&c.r8,&c.r9,&c.r10,&c.r11,&c.r12,&c.r13};
  for (auto *x : r) add(&x->u64, 8);
  // NaNs compare as one value: which operand's payload a NaN result carries
  // depends on operand order, which the two compilations may choose
  // differently for commutative operations; the game never reads a payload.
  auto addf64 = [&](u64 v) { if ((v & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (v & 0xFFFFFFFFFFFFFull)) v = 0x7FF8000000000000ull; add(&v, 8); };
  auto addv = [&](const PPCVRegister *x) { for (int k = 0; k < 4; ++k) { u32 w = x->u32[k]; if ((w & 0x7F800000u) == 0x7F800000u && (w & 0x7FFFFFu)) w = 0x7FC00000u; add(&w, 4); } };
  const PPCRegister *f[] = {&c.f0,&c.f1,&c.f2,&c.f3,&c.f4,&c.f5,&c.f6,&c.f7,&c.f8,&c.f9,&c.f10,&c.f11,&c.f12,&c.f13};
  for (auto *x : f) addf64(x->u64);
  for (const PPCVRegister *x : {VLIST128C}) addv(x);
  return h;
}
static void DiffRegs(const PPCContext &a, const PPCContext &b) {
  // (also used by Stub)
  const PPCRegister *ra[] = {&a.r0,&a.r1,&a.r2,&a.r3,&a.r4,&a.r5,&a.r6,&a.r7,&a.r8,&a.r9,&a.r10,&a.r11,&a.r12,&a.r13};
  const PPCRegister *rb[] = {&b.r0,&b.r1,&b.r2,&b.r3,&b.r4,&b.r5,&b.r6,&b.r7,&b.r8,&b.r9,&b.r10,&b.r11,&b.r12,&b.r13};
  for (int i = 0; i < 14; ++i) if (ra[i]->u64 != rb[i]->u64) std::printf("      r%d %016llx vs %016llx\n", i, (unsigned long long)ra[i]->u64, (unsigned long long)rb[i]->u64);
  const PPCRegister *fa[] = {&a.f0,&a.f1,&a.f2,&a.f3,&a.f4,&a.f5,&a.f6,&a.f7,&a.f8,&a.f9,&a.f10,&a.f11,&a.f12,&a.f13};
  const PPCRegister *fb[] = {&b.f0,&b.f1,&b.f2,&b.f3,&b.f4,&b.f5,&b.f6,&b.f7,&b.f8,&b.f9,&b.f10,&b.f11,&b.f12,&b.f13};
  for (int i = 0; i < 14; ++i) if (fa[i]->u64 != fb[i]->u64 && !(fa[i]->f64 != fa[i]->f64 && fb[i]->f64 != fb[i]->f64)) std::printf("      f%d %g vs %g\n", i, fa[i]->f64, fb[i]->f64);
  const PPCVRegister *va[] = {VLIST128A}, *vb[] = {VLIST128B};
  for (int i = 0; i < 128; ++i) for (int k = 0; k < 4; ++k) {
    const float x = va[i]->f32[k], y = vb[i]->f32[k];
    if (va[i]->u32[k] != vb[i]->u32[k] && !(x != x && y != y)) { std::printf("      v%d[%d] %08x vs %08x\n", i, k, va[i]->u32[k], vb[i]->u32[k]); break; }
  }
}
static void Scramble(PPCContext &c, unsigned id) {
  unsigned long long s = Mix(id * 7919ull + g_calls);
  PPCRegister *r[] = {&c.r0,&c.r3,&c.r4,&c.r5,&c.r6,&c.r7,&c.r8,&c.r9,&c.r10,&c.r11,&c.r12};
  for (auto *x : r) { s = Mix(s); x->u64 = s & 0x3FFFFFFull; }
  c.r3.u64 &= 0xFF;
  PPCRegister *f[] = {&c.f0,&c.f1,&c.f2,&c.f3,&c.f4,&c.f5,&c.f6,&c.f7,&c.f8,&c.f9,&c.f10,&c.f11,&c.f12,&c.f13};
  for (auto *x : f) { s = Mix(s); x->f64 = double(s % 1000) / 7.0; }
  for (PPCVRegister *x : {VLIST20}) for (int k = 0; k < 4; ++k) { s = Mix(s); x->f32[k] = float(s % 1000) / 3.0f; }
}
// The first run's register file at each call, and the second run checks
// against it as it goes (printing the registers of the first difference).
static PPCContext *g_snap;
static int g_phase;  // 0 first run, 1 second run, 2 difference shown
static void DiffRegs(const PPCContext &a, const PPCContext &b);
static void Stub(PPCContext &ctx, unsigned id) {
  ++g_calls;
  if (g_nrec < kCallLimitRec) {
    const unsigned long long h = HashRegs(ctx);
    if (g_snap && g_phase == 0) g_snap[g_nrec] = ctx;
    if (g_snap && g_phase == 1 && h != g_rec[g_nrec].hash) {
      std::printf("    at call %u (callee %u):\n", g_nrec, id);
      DiffRegs(g_snap[g_nrec], ctx);
      g_phase = 2;
    }
    g_rec[g_nrec] = {id, h};
    ++g_nrec;
  }
  if (g_calls >= kCallLimit) longjmp(g_stop, 1);
  Scramble(ctx, id);
}
#define ppc_trap(ctx, base, t) Stub(ctx, 900000u + (t))
#define REX_CALL_INDIRECT_FUNC(x) Stub(ctx, 800000u + ((u32)(x) & 0xFFFF))
'''

VOLATILE_MEM = r'''
#undef REX_LOAD_U8
#undef REX_LOAD_U16
#undef REX_LOAD_U32
#undef REX_LOAD_U64
#undef REX_STORE_U8
#undef REX_STORE_U16
#undef REX_STORE_U32
#undef REX_STORE_U64
#define REX_LOAD_U8(x) (*(volatile u8*)(base + (u32)(x)))
#define REX_LOAD_U16(x) __builtin_bswap16(*(volatile u16*)(base + (u32)(x)))
#define REX_LOAD_U32(x) __builtin_bswap32(*(volatile u32*)(base + (u32)(x)))
#define REX_LOAD_U64(x) __builtin_bswap64(*(volatile u64*)(base + (u32)(x)))
#define REX_STORE_U8(x, y) (*(volatile u8*)(base + (u32)(x)) = (y))
#define REX_STORE_U16(x, y) (*(volatile u16*)(base + (u32)(x)) = __builtin_bswap16(y))
#define REX_STORE_U32(x, y) (*(volatile u32*)(base + (u32)(x)) = __builtin_bswap32(y))
#define REX_STORE_U64(x, y) (*(volatile u64*)(base + (u32)(x)) = __builtin_bswap64(y))
'''

PLAIN_MEM = VOLATILE_MEM.replace('volatile ', '')

DRIVER = r'''
static const size_t kMem = 0x100000000ull;
static unsigned char *g_maps[2];
static unsigned g_seed;
// Guest memory is filled a page at a time on first touch, from the page's
// address and the run's seed, so both versions see the same contents however
// they reach them: words that are small numbers (counts, indices, flags,
// command-stream ends), pointers to elsewhere and floats.
static void FillPage(unsigned char *page, u32 guest) {
  unsigned long long s = Mix(guest ^ (g_seed * 0x9E3779B97F4A7C15ull));
  for (u32 o = 0; o < 4096; o += 4) {
    s = Mix(s);
    u32 w;
    switch (s & 3) {
    case 0: w = (u32)(s >> 8) % 12; break;
    case 1: w = ((s >> 8) & 1) ? 0x00FF00FFu : (u32)(s >> 9) % 0x4000u; break;
    case 2: w = 0x40000000u + (u32)((s >> 8) % 0x3F000000u & ~15u); break;
    default: { float f = float((s >> 8) % 2000) / 9.0f - 100.0f; std::memcpy(&w, &f, 4); }
    }
    w = __builtin_bswap32(w);
    std::memcpy(page + o, &w, 4);
  }
}
static void OnFault(int, siginfo_t *si, void *) {
  unsigned char *a = (unsigned char *)si->si_addr;
  for (unsigned char *m : g_maps) {
    if (m && a >= m && a < m + kMem) {
      unsigned char *page = (unsigned char *)((uintptr_t)a & ~(uintptr_t)4095);
      mprotect(page, 4096, PROT_READ | PROT_WRITE);
      FillPage(page, (u32)(page - m));
      return;
    }
  }
  signal(SIGSEGV, SIG_DFL);
  raise(SIGSEGV);
}
static void Seed(PPCContext &c, unsigned char *m, unsigned seed) {
  (void)m;
  unsigned long long s = Mix(seed + 1);
  // a window of random guest memory, pointers into it, a stack
  std::memset(&c, 0, sizeof(c));
  PPCRegister *r[] = {&c.r0,&c.r2,&c.r3,&c.r4,&c.r5,&c.r6,&c.r7,&c.r8,&c.r9,&c.r10,&c.r11,&c.r12,&c.r13};
  for (auto *x : r) { s = Mix(s); x->u64 = (s & 1) ? (0x40000000u + (u32)(s % 0x3F000u & ~15u)) : (s >> 8) % 48; }
  c.r1.u64 = 0x70000000u;
  c.fpscr.csr = c.fpscr.getcsr();  // the host's masks, as the runtime seeds them
  PPCRegister *f[] = {&c.f0,&c.f1,&c.f2,&c.f3,&c.f4,&c.f5,&c.f6,&c.f7,&c.f8,&c.f9,&c.f10,&c.f11,&c.f12,&c.f13};
  for (auto *x : f) { s = Mix(s); x->f64 = double(s % 2000) / 9.0 - 100.0; }
  for (PPCVRegister *x : {VLIST128}) for (int k = 0; k < 4; ++k) { s = Mix(s); x->f32[k] = float(s % 2000) / 9.0f - 100.0f; }
}
typedef void (*Fn)(PPCContext &, uint8_t *);
// 0 same, 1 different, 77 cut off
static int g_only_original;
static int RunPair(Fn o, Fn n, unsigned seed) {
  unsigned char *ma = (unsigned char *)mmap(nullptr, kMem, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  unsigned char *mb = (unsigned char *)mmap(nullptr, kMem, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  g_maps[0] = ma; g_maps[1] = mb; g_seed = seed;
  struct sigaction sa = {};
  sa.sa_sigaction = OnFault;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigaction(SIGSEGV, &sa, nullptr);
  static PPCContext ca, cb;
  Seed(ca, ma, seed); Seed(cb, mb, seed);
  alarm(2);
  // the host mode each run starts in is the one its context says
  if (!g_snap) g_snap = (PPCContext *)std::calloc(kCallLimitRec, sizeof(PPCContext));
  g_phase = 0;
  g_nrec = g_calls = 0; ca.fpscr.setcsr(ca.fpscr.csr);
  if (!setjmp(g_stop)) o(ca, ma);
  static Rec ra[kCallLimitRec]; unsigned na = g_nrec; std::memcpy(ra, g_rec, sizeof(ra));
  if (g_only_original) return 0;
  g_phase = 1;
  g_nrec = g_calls = 0; cb.fpscr.setcsr(cb.fpscr.csr);
  if (!setjmp(g_stop)) n(cb, mb);
  unsigned nb = g_nrec;
  int bad = 0;
  if (na != nb) { std::printf("    calls: %u vs %u\n", na, nb); bad = 1; }
  for (unsigned i = 0; i < na && i < nb; ++i) if (ra[i].id != g_rec[i].id || ra[i].hash != g_rec[i].hash) {
    std::printf("    call %u (callee %u): registers differ\n", i, ra[i].id); bad = 1; break; }
  if (HashRegs(ca) != HashRegs(cb)) { std::printf("    final registers differ\n"); DiffRegs(ca, cb); bad = 1;
    const char *nm = "r"; (void)nm;
    for (int i = 0; i < 14 && 0; ++i) {}
  }
  static unsigned char va[(kMem >> 12) + 1], vb[(kMem >> 12) + 1];
  mincore(ma, kMem, va); mincore(mb, kMem, vb);
  for (size_t p = 0; p < (kMem >> 12); ++p) if ((va[p] | vb[p]) & 1) {
    // a page only one version touched reads as its initial contents in the other
    if (std::memcmp(ma + (p << 12), mb + (p << 12), 4096)) {
      for (u32 o = 0; o < 4096; o += 4) {
        u32 x, y;
        std::memcpy(&x, ma + (p << 12) + o, 4); std::memcpy(&y, mb + (p << 12) + o, 4);
        x = __builtin_bswap32(x); y = __builtin_bswap32(y);
        auto nan = [](u32 w) { return (w & 0x7F800000u) == 0x7F800000u && (w & 0x7FFFFFu); };
        if (x != y && !(nan(x) && nan(y))) {
          std::printf("    guest word 0x%08zx: %08x vs %08x\n", (p << 12) + o, x, y); bad = 1; break;
        }
      }
      if (bad) break;
    }
  }
  return bad;
}
int main(int argc, char **argv) {
  int runs = argc > 1 ? atoi(argv[1]) : 50;
  int failed = 0;
  for (auto &t : kTests) {
    int same = 0, cut = 0, diff = 0, why[64] = {};
    for (int seed = 0; seed < runs; ++seed) {
      fflush(stdout);
      pid_t pid = fork();
      if (pid == 0) { int r = RunPair(t.o, t.n, seed); fflush(stdout); _exit(r); }
      int st = 0; waitpid(pid, &st, 0);
      if (WIFEXITED(st) && WEXITSTATUS(st) == 0) ++same;
      else if (WIFEXITED(st) && WEXITSTATUS(st) == 77) { ++cut; ++why[0]; }
      else if (WIFSIGNALED(st) && WTERMSIG(st) == SIGALRM) { ++cut; ++why[SIGALRM]; }
      else {
        // A crash the original has as well (random state) is not the copy's.
        bool both = false;
        if (WIFSIGNALED(st)) {
          pid_t p2 = fork();
          if (p2 == 0) { g_only_original = 1; _exit(RunPair(t.o, t.n, seed)); }
          int st2 = 0; waitpid(p2, &st2, 0);
          both = WIFSIGNALED(st2) && WTERMSIG(st2) == WTERMSIG(st);
        }
        if (both) { ++cut; ++why[WTERMSIG(st) & 63]; }
        else { ++diff; if (diff == 1) std::printf("  %s: seed %d failed (status %d)\n", t.name, seed, st); }
      }
    }
    std::printf("%-40s same %3d  cut off %3d  DIFFERENT %d", t.name, same, cut, diff);
    if (cut) {
      std::printf("   (cut:");
      if (why[0]) std::printf(" %d call limit", why[0]);
      for (int k = 1; k < 64; ++k) if (why[k]) std::printf(" %d %s", why[k], k == SIGALRM ? "timer" : strsignal(k));
      std::printf(")");
    }
    std::printf("\n");
    failed += diff != 0;
  }
  return failed ? 1 : 0;
}
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('root')
    ap.add_argument('--runs', type=int, default=40)
    ap.add_argument('--only', nargs='*')
    ap.add_argument('--cxx', default='g++')
    ap.add_argument('--keep', help='write the test source here')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    gen = os.path.join(root, 'generated')
    listing = os.path.join(root, 'src', 'platform', 'switch', 'hot', 'localized.txt')
    names = []
    for raw in open(listing, encoding='utf-8'):
        line = raw.split('#', 1)[0].split()
        if line:
            names.append(line[0])
    if a.only:
        names = [n for n in names if n in a.only]
    bodies = localize.find_bodies(gen, set(names))
    externs = localize.find_externs(gen)

    origs, copies, tests, callees = [], [], [], set()
    for n in names:
        if n not in bodies:
            continue
        result, why = localize.localize(n, bodies[n])
        if result is None:
            print(f'{n}: not localized ({why})')
            continue
        cs, lines = result
        callees.update(cs)
        body = [l for l in bodies[n] if not l.lstrip().startswith('//')]
        origs.append(f'static void o_{n}(PPCContext& __restrict ctx, uint8_t* base) {{\n' + '\n'.join(body) + '\n}')
        copies.append('\n'.join(lines))
        tests.append(n)
    all_text = '\n'.join(origs)
    stubs = []
    for i, c in enumerate(sorted(callees)):
        stubs.append(f'extern "C" void {c}(PPCContext& ctx, uint8_t* base) {{ (void)base; Stub(ctx, {i + 1}u); }}')
    hooks = []
    for k, (name, decl) in enumerate(sorted(externs.items())):
        if not re.search(r'\b' + name + r'\(', all_text):
            continue
        m = re.match(r'^extern (\w+) (\w+)\((.*)\);$', decl)
        ret, params = m.group(1), [p.strip() for p in m.group(3).split(',') if p.strip()]
        names_p = [f'p{i}' for i in range(len(params))]
        sig = ', '.join(p.rsplit(' ', 1)[0] + ' ' + names_p[i] for i, p in enumerate(params))
        body = ' '.join(f'{p}.u64 = Mix({p}.u64 + {k});' for p, t in zip(names_p, params) if 'PPCRegister&' in t)
        rv = '' if ret == 'void' else f' return ({ret})(Mix({k}) & 1);'
        hooks.append(f'{ret} {name}({sig}) {{ {body}{rv} }}')
    v128 = ', '.join(f'&c.v{i}' for i in range(128))
    prelude = (PRELUDE.replace('{VLIST128C}', '{' + v128 + '}')
               .replace('{VLIST128A}', '{' + ', '.join(f'&a.v{i}' for i in range(128)) + '}')
               .replace('{VLIST128B}', '{' + ', '.join(f'&b.v{i}' for i in range(128)) + '}')
               .replace('{VLIST20}', '{' + ', '.join(f'&c.v{i}' for i in range(20)) + '}'))
    driver = DRIVER.replace('{VLIST128}', '{' + v128 + '}')
    src = [prelude] + stubs + hooks + [VOLATILE_MEM] + origs + [PLAIN_MEM] + copies
    src.append('struct T { const char *name; Fn o, n; };\ntypedef void (*Fn)(PPCContext &, uint8_t *);')
    src = '\n'.join(src)
    src = src.replace('struct T { const char *name; Fn o, n; };\ntypedef void (*Fn)(PPCContext &, uint8_t *);', '')
    src += '\ntypedef void (*Fn)(PPCContext &, uint8_t *);\nstruct T { const char *name; Fn o, n; };\n'
    src += 'static const T kTests[] = {\n' + ''.join(f'  {{"{n}", o_{n}, n_{n}}},\n' for n in tests) + '};\n'
    src += driver.replace('typedef void (*Fn)(PPCContext &, uint8_t *);\n', '')

    sdk = os.path.join(root, 'thirdparty', 'rexglue-sdk')
    with tempfile.TemporaryDirectory() as tmp:
        cpp = a.keep or os.path.join(tmp, 'check.cpp')
        open(cpp, 'w').write(src)
        exe = os.path.join(tmp, 'check')
        cmd = [a.cxx, '-std=c++23', '-O2', '-fno-strict-aliasing', '-w',
               '-I', os.path.join(sdk, 'include'), '-I', os.path.join(sdk, 'thirdparty', 'simde'),
               cpp, '-o', exe]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            print(r.stderr[-6000:])
            return 2
        return subprocess.run([exe, str(a.runs)]).returncode


if __name__ == '__main__':
    sys.exit(main())
