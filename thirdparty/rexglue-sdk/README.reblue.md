# ReXGlue SDK, Nintendo Switch port (vendored)

This folder is the ReXGlue SDK **v0.10.0** with the Nintendo Switch (Horizon,
libnx, devkitA64) port made by the
[nfsmw-nx](https://github.com/StevensND/nfsmw-nx) project, copied from its
`sdk/` folder at commit `266f943` (branch `deko3d-implementation`,
2026-10-10). re:Blue's PC builds keep using the official SDK packages; only
the Switch build (`docs/SWITCH.md`) compiles this copy.

**License:** the SDK, including the Horizon layer nfsmw-nx added, is
BSD-3-Clause (`LICENSE`; nfsmw-nx states this in
`THIRD_PARTY_NOTICES.nfsmw-nx.md`). nfsmw-nx's own game code is GPL-3.0
(`LICENSE.nfsmw-nx`), but none of it is in this folder. The libraries the NRO
links keep their licenses (Mesa: MIT, FFmpeg/libmspack: LGPL-2.1).

## What is in here

- Everything nfsmw-nx carries in `sdk/`: the SDK sources, its Switch layer
  (`src/core/*_switch.cpp`, `src/ui/*switch*`, `src/audio/switch`,
  `src/input/switch`, `cmake/rexglue_switch.cmake`, `switch_compat/`) and the
  few third-party files it patched.
- The rest of `thirdparty/` is **not** in git. `tools/switch/fetch_thirdparty.py`
  copies it from the upstream v0.10.0 release and its submodules the first
  time (the host stage of the build runs it); `.fetched-<commit>` marks it done.

## Changes made for re:Blue

Keep this list current, so the copy can be brought up to a newer nfsmw-nx SDK.

| File | Change |
|---|---|
| `CMakeLists.txt` | `REXGLUE_HOST_TOOLS_ONLY` option |
| `resources/templates/init/rexglue_cmake.inja` | in a cross build, an empty `<title>_codegen` target (re:Blue orders its own targets after it) |
| `thirdparty/CMakeLists.txt` | with `REXGLUE_HOST_TOOLS_ONLY`, SDL is built headless (no X11/Wayland/audio dev packages on the build machine) |

The upstream SDK documentation for the Switch layer is in nfsmw-nx's
`docs/` (`platform-notes.md`, `toolchain.md`, `porting-another-game.md`).
