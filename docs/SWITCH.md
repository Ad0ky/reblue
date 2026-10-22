# re:Blue for Nintendo Switch (experimental)

A homebrew build of re:Blue for Switch consoles running custom firmware
(Atmosphère + Homebrew Menu). The recompiled game runs natively on the console:
re:Blue's own renderer draws through plume on Vulkan, on the Mesa NVK driver
linked into the NRO. **Work in progress: expect it not to boot yet.**

Branch: `reblueNX`. Everything Switch-specific is listed in
[How the port works](#how-the-port-works).

## What you need

- A Switch with Atmosphère and the Homebrew Menu, an exFAT microSD card with
  ~15 GB free.
- The game folder of a **re:Blue PC install** (the one the PC installer copies
  out of your three discs: `default.xex`, `pack\`, `movie\`, `map\`, `snd_*`).
  The Switch has no installer.
- To build: Windows with **Docker Desktop** (WSL 2 backend), ~20 GB of disk for
  the images and the build, and enough memory for Docker (~1.5 GB per compile
  job; lower the jobs with `-Jobs 4` if it runs out).

## Building

1. Check out the `reblueNX` branch. The submodules, the SDK's third-party
   sources and the game inputs are fetched by the build itself.
2. Run **`run-switch-build.bat`** (double-click). It uses the folder `..\game`
   next to the repository; pass another as the first argument:
   `run-switch-build.bat D:\Games\reblue\game`.
3. Progress and errors go to `switch-build.log`. The result is copied to
   `..\SD-card\switch\reblue\reblue.nro` (and `out\switch\`).

The first run is long: it builds the host image, fetches the SDK's
third-party sources, recompiles `default.xex` and compiles everything. Later
runs only redo what changed.

`tools\switch\build-switch.ps1` takes options:

| Option | Effect |
|---|---|
| `-Game <folder>` | the game folder (default `..\game`) |
| `-Jobs N` | parallel compile jobs (default: all CPUs) |
| `-Clean` | rebuild the host tools, codegen and the Switch build from scratch |
| `-BuildType RelWithDebInfo` | symbols in the `.elf` |
| `-SwitchImage <image>` | Switch toolchain image (default `lo-switch-dev` if present, else `ghcr.io/autorunhq/switch-dev:2026.10.05`) |

### The two stages

| Stage | Image | Script | Does |
|---|---|---|---|
| 1. host | `reblue-switch-host` (`tools/switch/Dockerfile.host`, Ubuntu 24.04 + Clang 19), built on first use | `tools/switch/host-stage.sh` | submodules and patches; SDK third-party sources; `assets/` from the game folder (`default.xex` and the 152 shaders out of `pack\!necessity.ipk`); the host SDK; **codegen** (`generated/`); **XenosRecomp** (`out/switch-host/shader_cache.cpp`) |
| 2. Switch | `lo-switch-dev` / `switch-dev` (devkitA64, libnx, mesa-switch NVK) | `tools/switch/build-switch.sh` | cross-compiles the SDK, plume and re:Blue, links NVK statically, packs `reblue.nro` |

Nothing made from the game is committed: `assets/`, `generated/` (except
`rexglue.cmake`) and `out/` are git-ignored.

### Compile check without devkitPro (Linux)

`cmake/toolchains/switch-check.cmake` compiles the Switch code paths with
Ubuntu's `g++-14-aarch64-linux-gnu` and the libnx headers, without linking
(newlib differences are not caught). It needs the host stage's outputs.

```sh
git clone --depth 1 https://github.com/switchbrew/libnx /tmp/libnx
cmake -S . -B out/switch-check -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/switch-check.cmake \
      -DREBLUE_SWITCH_CHECK_LIBNX=/tmp/libnx -DCMAKE_BUILD_TYPE=Release
cmake --build out/switch-check --target reblue_common -- -k 0
```

## SD card layout

```
sdmc:/switch/reblue/
    reblue.nro
    game/            the game folder of the PC install (default.xex, pack/, movie/, ...)
    profiles/        created on first start: settings (reblue.toml), saves
    logs/            created on first start
    cache/           created on first start
```

## Starting

Start the Homebrew Menu through **title takeover** (hold **R** while starting
any installed game) and pick re:Blue. Not from the Album: the applet mode does
not have the memory. A forwarder must use the **39-bit address space**.

## Logs and crashes

`sdmc:/switch/reblue/logs/` has the runtime log. A crash writes a block with the
faulting offset (`reblue.elf+0x...`) and the code addresses on the stack;
resolve them against the `reblue.elf` of the same build:

```sh
aarch64-none-elf-addr2line -fCe out/switch/reblue.elf 0x1234
```

The SDK also writes `stderr`/abort reports beside the NRO (nfsmw-nx's
`switch_crash_hooks.c`).

## How the port works

| Area | Switch implementation |
|---|---|
| Runtime | ReXGlue SDK nightly-20261002 (the one re:Blue v1.3.1 uses) merged with nfsmw-nx's Horizon layer, vendored in `thirdparty/rexglue-sdk` (`README.reblue.md` there): guest memory window and mirrors, exceptions, threads, fibers, clocks, audio (audren), input (HID), the libnx window, NRO packaging |
| Toolchain | devkitA64 GCC, libnx (`cmake/toolchains/switch-devkitA64.cmake`, from nfsmw-nx); re:Blue's Switch settings in `cmake/switch.cmake` |
| Code generation | host `rexglue codegen` in stage 1; the cross build uses `generated/` as is (`generated/rexglue.cmake`) |
| Graphics | re:Blue's renderer on plume Vulkan, Mesa NVK linked statically, `VK_NN_vi_surface` on the default `NWindow` (`tools/switch/patches/plume-switch.patch`, `volk-switch.patch`, from the Lost Odyssey port) |
| GPU memory | one VMA, the SDK's (3.3.0, built into its UI library); plume skips its own copy on Switch and uses the SDK's header. Everything links into one image with `--allow-multiple-definition` (needed for NVK's Rust libraries), so a second VMA of another version would be merged silently and break at run time |
| Duplicate symbols | the link allows multiple definitions and keeps the first, silently. Checked: SDK export stubs are weak on Switch (the game's own D3D/XAPI code wins); stb_image is compiled once (the SDK's) |
| Game shaders | the SPIR-V cache XenosRecomp makes on the host; there is no shader compiler on the console |
| Paths | the NRO's folder is the install root: `game/` beside it is used as a portable install (`reblue_app.cpp`, `core/app_root.cpp`) |
| Dialogs | the system error applet; no file dialogs (`platform/switch/dialogs_switch.cpp`) |
| Language | the console's system language (`platform/os_language.cpp`) |
| Exit | the applet exit, not `_Exit` (`core/threading.cpp`) |
| Not on Switch | installer, updater and content downloads, D3D12, Tracy, mouse and keyboard, file dialogs |

## Performance

What the build does already:

- **Direct calls.** The host stage runs `tools/switch/direct_calls.py` on the
  recompiled code: calls to game functions that have no hook go straight to
  `__imp__NAME`, which the compiler can inline (a call to the hookable weak
  alias cannot be). Every run re-checks the hooks, so a hook added later gets
  its plain calls back. `-NoDirectCalls` turns it off.
- **LTO** (`run-switch-build.bat -Lto`): link-time optimisation of the game code
  and re:Blue (not the SDK). Faster code, a much slower link, and about 16 GB of
  RAM for Docker (Docker Desktop, Settings, Resources).

- **Handheld GPU profile.** At start re:Blue asks the system for the official
  460.8 MHz handheld GPU profile (memory stays at 1331.2 MHz); the default is
  307.2 MHz. `bd_switch_gpu_mhz` (0, 384 or 460) changes it; docked is never
  touched.
- **960x540 in handheld mode.** The game renders at 960x540 and the frame is
  scaled to the screen. `bd_switch_handheld_height` = 720 or 0 renders at the
  full 1280x720. Docked renders at the display size. The views BD sizes from
  its 1280x720 design canvas (the field's depth-of-field view, event views)
  are scaled down with it; on the PC they are only ever scaled up.
- **Native movies.** The cutscene movies (`.sfd`: MPEG-1 720p video and ADX
  audio) are decoded on a host thread with FFmpeg's MPEG-1 decoder, NEON
  included (`src/engine/movie/`). The game's own CRI player still reads the
  file, plays the audio and keeps the timing; only its slice decoding, which
  took more than a whole core, is skipped, and each frame buffer is filled
  from FFmpeg right before the game copies it into its textures. The first
  frame of every movie is decoded both ways and compared before skipping
  starts; the log says `native movie: ... matches the game's` and, at the
  end, how many frames came from FFmpeg. Any mismatch or failure leaves that
  movie to the game's decoder. `bd_native_movies = false` turns it off.
- **Shader constants in uniform buffers.** On the PC's Vulkan path the game
  shaders read their constants (lights, fog, texture and sampler indices, the
  boolean switches) through device addresses in push constants: a memory load
  per read, 146 of them per pixel in `bd_normal_ps`, the shader that took half
  of the GPU time. On the Switch they are the same cbuffers as on D3D12, bound
  as dynamic uniform buffers in descriptor set 5 (vertex b0, pixel b1, shared
  b2), which NVK keeps in the Maxwell constant banks, as Lost Odyssey Recomp
  does. `tools/switch/patches/xenosrecomp-switch.patch` (the host stage builds
  XenosRecomp with `REBLUE_SPIRV_UBO`), `plume-switch.patch`
  (`setGraphicsDescriptorSetDynamic`), and `src/gpu/constant_buffers.cpp`.
  **Off by default** (`bd_switch_ubo_constants = false`): the first console run
  lost the GPU at start. The NRO carries both SPIR-V caches; with the setting
  on, `rex_stderr.log` gets `[ubo]` lines (the descriptor sets made, the first
  binds and their offsets, every bind that fails). Delete
  `cache/vulkan_pipelines.bin` when switching it.
- **Pipeline cache.** `cache/vulkan_pipelines.bin` beside the NRO keeps every
  Vulkan pipeline built so far (each one costs ~60 ms to build on NVK). The
  first run builds them, later runs read them. Delete the file to start over.

How to measure on the console:

- The game log (`logs/`) gets a `[frames]` line every 10 s: fps, mean and worst
  frame time, and per frame the time waiting for the GPU, in the swapchain, in
  the frame limiter and the rest (CPU). Frames over 33/50/100 ms are counted.
- `[gpu-passes]` lines, every 10 s: GPU time per frame from timestamps, and
  the passes that cost the most, named by their render target (size and
  format), with how often each runs per frame.
- `[gpu-shaders]` lines, every 10 s: one frame in 8 is timed per shader (a
  timestamp at every change of shaders between draws), and the pixel shaders
  that cost the most GPU time per frame are listed by their shader cache hash
  (the file name in the HLSL dump, `reblue_shader_hlsl_dump`), with draws per
  frame, the main vertex shader and the render target.
- `logs/rex/rex_perfil.log` (the SDK profiler) has CPU per thread every 10 s.
  For where each thread spends its time, create an empty file
  `sdmc:/switch/reblue/logs/rex/perfil_pilas.flag`: the profiler then samples
  the stacks every 1 ms (the game runs a little slower while it does). Keep the
  matching `reblue.elf` to read the addresses.

## Known gaps

- Boots to the first frames on a console; not played further yet.
- Shift-JIS conversion: newlib has no iconv, so non-ASCII text in guest debug
  prints and in mod rows added to game tables becomes `?`.
- Performance work still open: PGO, function ordering, patched NVK.
