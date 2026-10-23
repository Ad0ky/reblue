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
4. **GPU time**: a test NRO that times `bd_normal_ps` and the other heavy pixel shaders on full-screen passes with
   `dkCmdBufReportCounter`, against the same passes on NVK. Go/no-go for the GPU side.
5. The shader cache: the DKSH are made from the player's game, so they stay out of the repository like `generated/`.

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
