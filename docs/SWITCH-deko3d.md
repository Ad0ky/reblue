# re:Blue on deko3d (Switch): plan

Started 2026-10-23. The Switch build draws through plume on Vulkan (Mesa NVK). This moves it to
[deko3d](https://github.com/devkitPro/deko3d), devkitPro's own API for the Switch's GPU, as sa2-nx, banjo-nx and
nfsmw-nx did. The NVK build stays buildable and is the reference until deko3d is at parity on the console.

Read first:

- sa2-nx, `docs/deko3d-renderer-guide.md` (ChanseyIsTheBest): the whole route for a ReXGlue port, with banjo-nx's
  **plume deko3d back end** (`app/thirdparty/plume/plume_deko3d*.{h,cpp}`, MIT). re:Blue already renders through
  plume, so this back end is the core of the work, not a rewrite of the renderer.
- nfsmw-nx, branch `deko3d-implementation`, `docs/HowToDeko3d.md` and `docs/deko3d-devlog.md`: the Vulkan-to-deko3d
  mapping, WWHD's uam patches and the deko3d 0.5.0 pitfalls (tile height, y axis, vertex buffer bind overrun,
  ZCULL, `std::thread::detach`).

## What to expect

From the sibling ports, measured on consoles at 1020 MHz:

- NVK's CPU cost per call on the A57 (banjo-nx): ~3 us a pipeline bind, 2.4-2.9 us a draw, 1.3-1.6 us a descriptor
  or uniform buffer update. deko3d writes the GPU's command words itself. re:Blue's busy areas run 1,800-2,400 draws a
  frame on one draw thread, so this is several ms a frame there; the game's own code on that thread is not affected.
- No pipelines: the ~60 ms NVK pipeline builds (the first-run stutters, `vulkan_pipelines.bin`) go. A new shader
  variant costs a uam compile instead (~50-100 ms on the console) unless a DKSH cache has it.
- Presentation is ours: three swapchain images on `nwindowGetDefault()`, the swap interval, late input (sa2-nx).
- GPU time is the open question: uam is Mesa 19's GLSL front end and nouveau's code generator, not NAK. Measure the
  heavy shaders early (P0.4).
- The NRO loses Mesa (~14 MB).

## P0: shaders (no renderer)

Path: XenosRecomp HLSL -> DXC SPIR-V (the uniform-buffer variant, `REBLUE_UBO_CONSTANTS`, vertex shaders without
`-fvk-invert-y`) -> SPIRV-Cross (Vulkan GLSL) -> `tools/switch/deko3d/spv_to_dk_glsl.py` -> uam -> DKSH.

Done on the host (2026-10-23): uam (devkitPro, built with meson) and SPIRV-Cross; **all 141 game shaders (86 pixel, 55
vertex) compile, with no uam warning** (no integer division decayed to float, no dynamic index into a vector's
components). `bd_normal_ps`: 15 KB of code. Textures are bindless deko3d handles (`t | s << 20`) read from the shared
constants, so re:Blue's descriptor heaps map onto deko3d's image and sampler descriptor sets as they are.

Still to do:

1. Specialization variants: one DKSH per (shader, `SPEC_CONSTANTS` masked to the bits it reads).
2. re:Blue's own shaders (`src/gpu/shaders/hlsl`: 2D blit, bright-pass clamps, copies, resolves, ImGui, gamma): a
   uniform-buffer build without `vk::RawBufferLoad`, then the same chain, compiled at build time into the NRO.
3. uam as a library in the NRO, with WWHD's seven patches (sa2-nx `app/thirdparty/uam`, `PATCHES.md`; patch 7 turns
   off dual issue, which made warp-sized blocks flicker). A compile thread at priority 0x3B, a DKSH cache on the card.
4. **GPU time** (`tools/switch/gputest`): `reblue_gputest.nro` draws the 12 pixel shaders that cost the most in the
   field and battles (`bd_normal_ps`, `bd_toon_ps`, the post effects...) full screen at 960x536 into RGBA16F, 20 draws
   between two timestamps, the fastest of three runs, first on deko3d (uam's DKSH) and then on NVK (the Switch
   build's SPIR-V), with the same constants (every boolean on), textures and GPU profile (460.8 MHz). It writes
   `logs/gputest.log` with ms per draw on each and the ratio. Build it with `run-switch-build.bat -GpuTest` (it
   needs devkitPro's `deko3d` in the Switch image; the build installs it when it can). Its shaders come from
   `gputest.bin` beside it, made from the game's shaders by `make_pack.py` (XenosRecomp's HLSL dump, DXC,
   SPIRV-Cross, `spv_to_dk_glsl.py`, uam), so the pack stays on the SD card, out of the repository.
   Go/no-go for the GPU side: the field is GPU-bound, and nfsmw-nx's first deko3d runs needed twice NVK's GPU time.

   **Result (2026-10-23, console, 460.8 MHz): go.** deko3d with uam's code took 13.8 ms for the twelve shaders
   against NVK's 15.3 (0.91x): `bd_normal_ps` 3.47 against 3.65 ms per full-screen draw (0.95x), `bd_toon_ps` 0.88x,
   `bd_normal_ps_wind` 0.87x, `bd_normal_ps_nolight` 0.75x, `bd_shadowmap_ps` 0.54x; the simple post effects within
   3% either way, `bd_pe_ps_dof` 1.09x. uam's shaders are not slower than NAK's for this game.
5. The shader cache: the DKSH are made from the player's game, so they stay out of the repository like `generated/`.

## Status (2026-10-26): first build with deko3d

The NRO carries both back ends; `bd_switch_backend` in the config picks one at start: `deko3d` (the default) or
`nvk`. `run-switch-build.bat` builds deko3d in (`-NoDeko3d` leaves it out). What is in:

- `thirdparty/plume`: banjo-nx's `plume_deko3d*.{h,cpp}` (in `tools/switch/patches/plume-switch.patch`), with
  `RenderShaderFormat::DEKO3D` and the few interface additions it uses; `thirdparty/uam` from sa2-nx (WWHD's
  patches), linked as one object with its Mesa symbols local (`plume_uam_localize.cmake`) so it lives beside NVK's
  Mesa.
- Shaders: the host stage (`DEKO3D=1`, the default) runs `tools/switch/deko3d/make_dk_shaders.py`: the 141 game
  shaders from XenosRecomp's HLSL dump and re:Blue's 17 own shaders, DXC (uniform-buffer form, no
  `-fvk-invert-y`), SPIRV-Cross, `spv_to_dk_glsl.py`, into `out/switch-host/deko3d_shaders.cpp` (GLSL, zlib, about
  0.5 MB in the NRO). uam compiles a variant on the console the first time a pipeline needs it; the DKSH go to
  `cache/deko3d_shaders_<uam flags>.bin` on the card, so the second run has them.
- Runtime: `UsingDeko3D()` / `CreateHostShader()` (`gpu/device.cpp`, `gpu/backend.h`); guest shaders from the pack by
  their cache hash (`gpu/shaders/guest_shaders.cpp`); constants always through uniform buffers (set 5); device lost
  from `dkQueueIsInErrorState`; plume's messages in the log as `[deko3d]`.

If it fails on the console, `bd_switch_backend = "nvk"` gives the NVK renderer back without another build.

First console run (2026-10-26): the title and menus ran at 59 fps on deko3d (GPU 6.9 ms a frame). Loading the field
hung: uam's thread spent minutes in nv50_ir's register allocation, in the second compile banjo-nx's uam flag 16 makes
of a pixel shader, while the draw thread waited for that pipeline. The same variants compile on the PC in under 3 s
each, so it was not reproduced. Changes after it:

- `bd_deko3d_uam_flags` (default 431 = 447 without 16): no second compile.
- The build compiles every variant with uam on the PC (`tools/switch/deko3d/uam_variants.cpp`,
  `make_dk_shaders.py --uam-variants`): 286 variants for the 142 game shaders (every combination of the
  `g_SpecConstants` bits each one reads) and re:Blue's 17, 1.8 MB, 0.6 MB in the NRO. The console loads them as its
  DKSH cache and compiles only what the build could not foresee. plume masks constant 0 by the bits a shader reads
  (`// reblue-spec0-mask=` in its text), so the keys are known in advance; two threads asking for one variant compile
  it once.
- A uam compile that runs past 10 s is reported in the log and its GLSL written to `logs/uam_stuck_*.glsl`.
- XenosRecomp's HLSL dump named files by their source name, and `bd_cri_movie.vso` and `.pso` overwrote each other:
  the movie vertex shader (0x815DF1B3CCBFAA4E) was missing. The names now carry the hash.

## P1: device and presentation

- banjo-nx's plume deko3d back end into `thirdparty/plume` (it already has `setGraphicsDescriptorSetDynamic`, as
  our Switch patch does), adapted to this plume revision; `RenderShaderFormat::DEKO3D` blobs (GLSL + optional DKSH).
- The SDK without NVK (sa2-nx's `REXGLUE_SWITCH_NO_NVK`, nfsmw-nx's `REXGLUE_SWITCH_DEKO3D`): Vulkan instance stubs,
  `posix_memalign`.
- Swapchain of three images, ImGui on deko3d, `libdeko3dd` while bringing up (the release library aborts silently).

## P2-P3: re:Blue's renderer on the back end, then parity

Pipelines become state on bind; bindless heaps become the descriptor sets; barriers per hazard; clears with the full
scissor; resolves and copies; occlusion queries; the depth formats (deko3d samples depth as (D, D, D, 1)). Compare
frames with the NVK build: title, field, battle, events, movies.

## P4: performance (each behind a setting, measured on the console)

Unchanged state skipped, constants by differences (`dkCmdBufPushConstants`), cached texture handles, ZCULL, submit
size, the render-thread work in sa2-nx's guide section 7.

## Licences

deko3d zlib; uam zlib (its own files), MIT (Mesa's), MPL-2.0 (WWHD's patches); plume and its deko3d back end MIT;
SPIRV-Cross Apache-2.0. sa2-nx's own tools are GPL-3.0: used as references, not copied into this BSD repository.
