# re:Blue for Nintendo Switch (experimental)

A homebrew build of re:Blue for Switch consoles running custom firmware
(Atmosphère + Homebrew Menu). The recompiled game runs natively on the console:
re:Blue's own renderer draws through plume on deko3d, the Switch's own graphics
API (Vulkan on Mesa NVK is still built in, `bd_switch_backend`). **Experimental:
playable, with rough edges.** Players: see [INSTALL-SWITCH.md](INSTALL-SWITCH.md).

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
- **Localized copies of the hottest game functions.** The host stage also runs
  `tools/switch/localize.py`, which writes a copy of every function listed in
  `src/platform/switch/hot/localized.txt` (picked from Draw Thread profiles)
  into `generated/hot/localized.inc`: the same statements as the recompiled
  function, but with the context registers it uses in C++ locals and guest
  memory accessed without `volatile` (`src/platform/switch/hot_natives.cpp`).
  The recompiled code keeps every register in the `PPCContext` and GCC
  reloads them around each guest store; the hottest loop of
  `bdSceneNodeDrawSingle`, a 24-word fill, went from 10 instructions per word
  (two context loads and a store among them) to 4. A copy writes back before a call or return
  only the registers it changed, and reloads after a call only those it reads
  again (a dataflow over the function's statements); loops get a compiler
  barrier at their head. `tools/switch/localize_check.py` runs both versions
  of every listed function on the build machine from random registers and
  memory, with stub callees that record the register file at every call, and
  compares calls, registers and memory. `bd_cpu_natives = false` runs the
  recompiled originals; the log says `[hot] localized copies: on`.
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
  `bd_switch_ubo_constants` (on by default) picks it; the NRO carries both
  SPIR-V caches, so `false` goes back to device addresses. Delete
  `cache/vulkan_pipelines.bin` when switching it. The device addresses are
  pushed in both modes: re:Blue's own substitute shaders (`hlsl/`) read
  through them. `rex_stderr.log` gets `[ubo]` lines (descriptor sets made,
  the first binds, every bind that fails). Measured in the field at 460 MHz:
  `bd_normal_ps` 29.5 -> 17.6 ms, GPU 60 -> 40 ms per frame.
- **Smaller constant uploads.** XenosRecomp records how much of the 4 KiB
  float constant block each shader reads (`g_shaderCacheFloatConstantBytes`);
  only that part is byte-swapped (NEON) into the upload, and a clean block is
  re-uploaded only when the next shader reads further. Most pixel shaders read
  under 1 KiB; vertex shaders with indexed arrays read all of it.
- **Lookup caches.** The guest resource registry and the vertex/index buffer
  struct map answer hits from a lock-free direct-mapped cache
  (`gpu/lookup_cache.h`, a seqlock per slot) that every change to the map
  updates for its own key, instead of a mutex and a hash probe per draw.
- **Shadow gathers.** The sun-shadow receivers (`bd_normal_ps` and the other
  lit shaders) do six bilinear shadow compares per pixel; each read its 2x2
  footprint with four loads. `shadowCmp2D` (`xenosrecomp-switch.patch`,
  `shader_common.h`) reads it with one gather at the footprint's center, and
  falls back to the clamped loads only where the footprint leaves the map.
- **Sun shadow map reuse.** `bd_shadow_update_interval` (default 1, off) draws the
  sun shadow map every Nth frame and reuses it in between, with the light
  matrix it was drawn with (`engine/shadow_cache.cpp`). The shadow pass is
  about 3000 of the 4000 draws of a busy field frame. Static shadows stay put;
  shadows of moving things trail by up to N-1 frames. A camera cut or a new
  scene draws it at once. 1 draws it every frame. The game log gets a
  `[shadow] sun map drawn .. reused ..` line every 10 s.
- **Small far shadow casters.** `bd_shadow_min_pixels` (default 4) leaves out
  of the sun shadow map every caster whose bounding sphere looks smaller than
  that many pixels across from the camera: grass, bushes and posts in the
  distance, which `bd_shadow_distance` 2.0 brings into the map by the
  hundreds. Big casters keep their shadows at any distance. The `[shadow]
  casters` log line counts them.
- **Water reflections reused.** `bd_reflection_update_interval` (default 2)
  draws the plane reflections (the water's, 320x180) every Nth frame and reuses
  them with the mirror matrix they were drawn with (`engine/reflection_cache.cpp`);
  in water areas that pass drew 1400-1500 objects a frame. Objects smaller than
  `bd_reflection_min_pixels` (default 3) in the reflection texture are left out
  of it. The game log gets a `[reflection]` line every 10 s.
- **Zcull and depth compression** (`bd_deko3d_zcull`, `bd_deko3d_depth_compression`,
  both on): the GPU drops blocks of hidden pixels before shading them, and depth
  targets are compressed. The Zcull invalidate after copies only runs for copies
  into a depth target (it lacked braces, as renut-nx found in the same code).
- **Constant dirty marks.** `D3DDevice_Set*ShaderConstantF` marks the block
  dirty without the renderer lock once it is already dirty.
- **Pipeline cache.** `cache/vulkan_pipelines.bin` beside the NRO keeps every
  Vulkan pipeline built so far (each one costs ~60 ms to build on NVK). The
  first run builds them, later runs read them. Delete the file to start over.

### Render thread plan

In the slow scenes the game's Draw Thread is busy all the time, and close to
half of that is the renderer recording the draws (shader constants, the shared
texture/sampler block, bindings, framebuffers). The plan moves the recording to
a thread of its own, so the game prepares the next draws while the previous
ones are recorded on another core.

1. **Guest reads at the draw** (done). Everything a draw reads of guest memory
   is captured when the game issues it: the render state registers and BD's
   render state cache, the float constants that may have changed, the fetch
   (sampler) and bool constants, the render pass id (`CaptureGuestDrawState`,
   `CaptureDeltaConstants`). Recording reads only the capture.
2. **The queue** (`gpu/render_queue.cpp`). The D3D calls that set the draw
   state (targets, viewport, shaders, vertex declaration, textures, streams,
   indices, alpha test), the draws, clears, resolves, occlusion queries and the
   2D canvas drain are records: run at once inline, or copied into a ring for
   the render thread. A draw's record carries its capture and its UP vertices.
   Every other D3D call (resource creation, locks, releases, Present) first
   waits until the queue is empty, so it finds the renderer as it would have
   inline. `bd_render_thread` (default **on**) picks the mode at start-up; it
   needs the in-place constants. The calls are batched per thread and sent on
   every 16 draws; resource release and surface creation do not wait. Threaded, the log gets a `[render-queue]` line
   every 10 s: records per frame and how often and how long the game waited.
3. **On by default** (done): on the console route, 27.4 fps median and 15.8
   in the worst 10% against 27.3 / 14.4 before, with the Draw Thread at 14.5
   µs of CPU a draw against 17.4. Next: Present queued too, so a frame's
   recording can overlap the next frame's game work.
4. **Draw-path work.** Threaded, the D3D float constant setters send the
   registers they wrote to the render thread at once (`ConstantsWritten`), so
   a draw only re-reads the few registers the engine writes without a setter
   (the capture was the largest renderer cost left on the Draw Thread).

The game code itself: `engine/fast_math.cpp` runs `bdSinCos` (~8% of the Main
Thread in slow scenes) as native code with the recompiled function's
arithmetic. On its first call it compares itself with the recompiled function
over 80000 angles, bit for bit, and stays off if any result differs (the
stderr log says which). `bd_native_sincos = false` turns it off.
`engine/anim_curve.cpp` does the same for the animation curve key search
(`sub_8272EB40`, a linear scan over a curve's keys, ~13% of the Main Thread):
its first 50000 calls also run the recompiled function and compare every
register and word it leaves; it stays off on any difference
(`bd_native_anim_curve`). The interpolation between the two keys
(`sub_8272EC00`, a cubic Hermite segment) is native the same way, checked
the same way (`bd_native_anim_interp`).

The localized copies cover every guest function with samples in the console
profiles (843 functions, as katamari-nx does with its machine copies): the
hand-picked hot list, then a bulk batch of every other sampled function that
no hook or SDK native replaces and that is more than a 12-instruction leaf.
Each one passed `tools/switch/localize_check.py` (the copies that differed on
any random run, or never completed one, stay recompiled). After a batch like
this the PGO profile should be recorded again: copies made after it have no
counts. `bd_cpu_natives_off` runs chosen copies as the
recompiled originals without a new build: function names, a batch (`fifth`)
or positions in one (`fifth:0-374`), comma-separated; halving the range finds
a copy that misbehaves in a few launches. `bd_cpu_natives_trace = true`
writes each copy's first call to stderr (`[hot] first call: name
batch:position`): after a crash, only the copies listed can be the cause.

GPU, 2026-11-05/06: in the slowest field scenes (15-18 fps, GPU 55-64 ms a
frame) `bd_toon_ps` took ~25 ms over ~2000 draws of ~40 pixels and ~290
vertices each (the pass timer's pixel and vertex counters); drawn flat grey
(`bd_debug_flat_ps`) it still took 9.5 of its 14.5 us per draw, so two thirds
are vertex and triangle work. `make_dk_shaders.py` now drops the vertex
outputs no fragment shader reads (XenosRecomp writes all 18 interpolants, the
pixel shaders read texcoords 0..7 and the two colours, centroid, at most): 8
of the toon vertex shader's 18, 448 over the vertex shaders. Same picture.
(The first version missed the colours' `centroid in` and the screen was
black.)

The fifth batch keeps the recompiled code's volatile guest accesses (its
batch line in `localized.txt` says `volatile`): with plain accesses it
corrupted data on the console (shaders at startup, the title menu, loading a
save), with volatile ones it runs the whole test route. Its copies still keep
the registers in locals. The first four batches, which ran for weeks with
plain accesses, keep them. `REBLUE_HOT_VOLATILE=1` makes every copy volatile
(a diagnostic build).

The localized copies (`tools/switch/localize.py`) store `stvlx`/`stvrx` with
one call each instead of the recompiled byte loop (a store and a bound check
per byte): 248 of them in the hot functions, 80 in
`CharaVO__BuildBoneMatrices` alone, ~20% of `bdSceneNodeDrawSingle`.

Flush-mode skip (`bd_cpu_flush_skip`, default on), 2026-11-07. The
recompiled code runs vector float math with FPCR.FZ set and scalar math with
it clear, writing FPCR at every switch, and on the A57 an `msr fpcr` drains
the pipeline. In the CPU-bound windows of the busy castle street (18-26 fps,
GPU 26-45 ms) the Main Thread was 80% busy, 11% of its samples on those
writes: `sub_82278EC0` (a sphere test, 8.6% of the thread) does a vector
subtract and a dot product between scalar code, four writes for ~40
instructions. FZ only changes a result through a denormal (read as zero; a
result below 2^-126 before rounding becomes zero), so `localize.py` rewrites
vector sequences of at most four float operations between two switches into
the checked operations of `src/platform/switch/hot_flush_skip.h`: they run
with FZ clear and note every input, intermediate (the products and pair sums
of a dot product) and result in 0 < |x| <= 2^-126; any one makes that
statement switch FZ on and compute again (inputs and result through empty
asm, so GCC neither reuses the first result nor moves it past the write).
A sequence that ends in a jump instead of the switch back is taken when
every path from there meets a flush-mode call (they test the mode they find)
before any float work, call or return. GCC compiles the game code with its
default contraction, fusing a multiply into the add or subtract it feeds; a
checked multiply could not fuse, so a multiply whose result may reach an add
or subtract (and the add or subtract) stays as it was: a first version
without this rule changed results by one ulp in `localize_check.py` on
AArch64. 73 sequences in 40 of the 843 copies; `localize_check.py` (now `--static --exec qemu-aarch64-static` for an
AArch64 run) finds the copies equal to the originals; on QEMU 39 million random operations, biased to
denormals and the 2^-126 boundary, matched the FZ results bit for bit.
`bd_cpu_flush_skip = false` writes FPCR as before.

### Profile-guided optimisation

GCC can optimise the game code and re:Blue's from what actually runs: hot paths
inlined and laid out together, cold code kept small. Three steps:

1. **Instrumented NRO:** `run-switch-build.bat -Pgo generate` (build folder
   `out\switch-pgo-gen`). Copy that `reblue.nro` to the SD card and play: the
   areas that are slow, battles, menus, a cutscene. It runs slower than the
   normal build. Every minute and when the game closes it writes its counts to
   `sdmc:/switch/reblue/pgo/reblue_pgo_<N>.stream` (one file per session;
   the game log says `[pgo] instrumented build: N objects counted`).
2. **Merge:** copy the `pgo` folder from the SD card to `pgo-streams\` in the
   repository and run `run-switch-build.bat -PgoMerge`. It writes one `.gcda`
   per object into `pgo\` (`tools/switch/pgo-merge.sh`, then
   `tools/switch/pgo_summary.py`, which fills in the summary the stream format
   leaves at zero). Merge on the PC that built the instrumented NRO; an older
   profile is moved to `pgo.old.<date>`.
3. **Optimised NRO:** `run-switch-build.bat -Pgo use -Lto` (build folder
   `out\switch-pgo-use`; LTO needs ~16 GB of RAM for Docker, leave `-Lto` out
   without it). Functions changed since the profile lose their counts, so take a
   new profile after large changes.

Both `pgo\` and `pgo-streams\` are git-ignored.

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

2026-11-09 sessions (further into the game). Crackling: the audio output got
no data from the game in 5855 and 11643 requests (12 in the castle runs), all
in windows where one game XThread, the audio effect thread, was at 80-95% of
a core (`sub_8267F4D8`, 256-sample blocks, 60% of it). Its eleven functions
are the sixth batch of localized copies (volatile; `localize_check.py` equal on
AArch64, 40 runs each). Crash in the cutscene `ev213_00.sfd`: the game
presented the same movie buffer frame after frame, `SfdVideo::ByDecodeIndex`
waited 100 ms for a picture that had left the queue, and while a caller
waited the decoder had no limit: the heap went from 1.8 to 3 GB in 30 s and
an allocation threw `std::bad_alloc`. Now the last picture handed out is
kept for a repeat, a picture FFmpeg is more than 8 stream positions past
returns at once, and the queue never holds more than 26 frames.
2026-11-10 run: ev213_00.sfd played to the end but 1176 of its 4636 frames
had no picture from FFmpeg; with the slices skipped the game's frame buffer
still held an older picture, shown again. Any missing picture now hands the
movie back to the game's decoder (correct from its next I picture, at most
half a second), with the queue's state in the warning to find the cause.
Crackling: 830 audio requests without data in 29 minutes (5855 and 11643
before the sixth batch), all in 4 windows where the effect thread was still
at 75-92%: what is left there is its delay-line reads (dependent loads).
2026-11-10, second run: the fallback named the cause. ev213_00.sfd asked for
picture 87 after 88 (held 88..98): the game goes back to the previous
picture, which had left the queue. The pictures handed out or passed over
lately (10) are now kept for that. The fallback to the game's decoder made
the rest of the cutscene stutter (CPU 40-66 ms a frame). Crackling with
audio_maxqframes = 32 too: 1361 requests without data, all in 30 s where the
effect thread sat at 93% of a core, so a longer queue cannot cover it. The
sixth batch is now plain (15 KB instead of 17.7 KB for sub_8267F4D8: the
delay-line reads can overlap).
2026-11-10, third run: with recent pictures kept, ev213_00.sfd showed every
picture it asked for (0 not ready), and jumped back and forth: the game asks
for an earlier picture in display order after a later one. Playback is now
forward only (the newest picture shown stays), and the end-of-movie line
counts those requests. Crackling unchanged with plain copies (effect thread
still 88-94%): it shared the time slices with Main and Draw at 0x3B; its
thread (entry 0x8268A410, `switch_guest_sound_entries`) now gets the sound
thread's priority, 0x3A.

Audio engine threads (`bd_audio_engine_threads`, default 2 on the Switch),
2026-11-10. The `[hot-time]` line showed what the crackling after loading is:
up to 1880 reverb calls a second (ten instances, 440 us each for a 5 ms
block, ~1800 instructions a sample), 83% of a core, on a thread at 94% with a
fourth of the CPU free. Two attempts to shorten the reverb's sample loop
(frame and filter words kept in locals; pointers computed instead of
reloaded) were exact but no shorter once compiled, and are not in the tree.
The thread is the worker of the Xbox 360 XAudio2 library linked into the
game, and that library is multi-threaded by design: one worker per hardware
thread in its processor mask, all taking voices from the same per-stage
queues (`sub_8268A7A0`, `sub_82689D88`, `sub_82689A00`); the game asks for
one. `src/engine/audio_threads.cpp` adds a hardware thread to the mask where
the library decodes it (`sub_82688DD8`), so its sizes and its thread
creation agree; the second worker (entry `0x8268A4D8`) gets the sound
priority too. `bd_audio_engine_threads = 0` is the game's own setting.
