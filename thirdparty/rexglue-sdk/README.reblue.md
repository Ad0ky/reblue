# ReXGlue SDK, Nintendo Switch port (vendored)

This folder is the ReXGlue SDK at **nightly-20261002-bd833a2a**, the SDK build
re:Blue v1.3.1 is made with, merged with the Nintendo Switch (Horizon, libnx,
devkitA64) layer of the [nfsmw-nx](https://github.com/StevensND/nfsmw-nx)
project. Only the Switch build (`docs/SWITCH.md`) compiles this copy; the PC
builds keep using the official SDK packages.

How it was made (so it can be redone against a newer SDK or nfsmw-nx):

1. upstream `v0.10.0`, with nfsmw-nx's `sdk/` (commit `266f943`, branch
   `deko3d-implementation`, 2026-10-10) copied over it as one commit;
2. `git merge nightly-20261002-bd833a2a` (24 upstream commits after v0.10.0);
3. conflicts resolved as in the table below; `.github`, `docs`, `tests`,
   `scripts` and `tools` left out, as nfsmw-nx does.

**License:** the SDK, including the Horizon layer nfsmw-nx added, is
BSD-3-Clause (`LICENSE`; nfsmw-nx states this in
`THIRD_PARTY_NOTICES.nfsmw-nx.md`). nfsmw-nx's own game code is GPL-3.0
(`LICENSE.nfsmw-nx`), but none of it is in this folder. The libraries the NRO
links keep their licenses (Mesa: MIT, FFmpeg/libmspack: LGPL-2.1).

## What is in here

- The SDK sources with the Switch layer (`src/core/*_switch.cpp`,
  `src/ui/*switch*`, `src/audio/switch`, `src/input/switch`,
  `cmake/rexglue_switch.cmake`, `switch_compat/`) and the few third-party files
  nfsmw-nx patched (`thirdparty/FFmpeg/...`, `thirdparty/ffmpeg-overlay/`).
- The rest of `thirdparty/` is **not** in git. `tools/switch/fetch_thirdparty.py`
  copies it from the upstream nightly and its submodules the first time (the
  host stage of the build runs it); `.fetched-<commit>` marks it done.

## Merge resolutions and re:Blue changes

Keep this list current.

| File | Change |
|---|---|
| `CMakeLists.txt`, `cmake/rex_version.cmake` | merge: upstream's `SOURCE_DIR` and nfsmw-nx's `NO_GIT` (in a vendored copy git sees re:Blue's tags) |
| `CMakeLists.txt` | `REXGLUE_HOST_TOOLS_ONLY` option |
| `thirdparty/CMakeLists.txt` | with `REXGLUE_HOST_TOOLS_ONLY`, SDL is built headless (no X11/Wayland/audio dev packages on the build machine) |
| `include/rex/input/input_system.h` | merge: both sides' members (nfsmw-nx's refresh throttle, upstream's per-user state) |
| `src/input/input_system.cpp` | merge: upstream's `vibration` cvar replaces nfsmw-nx's `input_vibracion`; off by default on Switch, as nfsmw-nx had it |
| `src/core/logging.cpp` | merge: upstream's (`LogConfig`, directory budget), which re:Blue uses; nfsmw-nx's optional async logger (off by default, measured harmful) dropped |
| `src/ui/rex_app.cpp` | merge: upstream's input gate (window focus) with nfsmw-nx's Switch rule (the menu owns the pad while it has focus) |
| `resources/templates/init/rexglue_cmake.inja` | merge: upstream's `names.original`; in a cross build an empty `<title>_codegen` target (re:Blue orders its own targets after it) |
| `cmake/rexglue_export_targets.cmake`, `CMakeLists.txt` | skip the targets a Switch build does not have (the code generator, the GPU plugin) when exporting the `rex::` aliases |
| `src/ui/window_switch.cpp`, `include/rex/ui/window_switch.h` | the nightly's two-argument `Window::Create`; `GetDisplayPixelSize` returns the display mode's resolution |
| `include/rex/hook.h` | on Switch, `REX_EXPORT_STUB`/`REX_EXPORT_STUB_RETURN` are weak, so a title's own recompiled function of the same name wins as on PC (the one-image link with `--allow-multiple-definition` otherwise keeps whichever comes first) |
| `src/system/gpu_plugin_loader.cpp` | the statically linked GPU plugin's entry points are weak, so a title without the plugin (re:Blue, its own renderer) links |

The Switch layer's own documentation is in nfsmw-nx's `docs/`
(`platform-notes.md`, `toolchain.md`, `porting-another-game.md`).
