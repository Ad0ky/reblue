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
| Game shaders | the SPIR-V cache XenosRecomp makes on the host; there is no shader compiler on the console |
| Paths | the NRO's folder is the install root: `game/` beside it is used as a portable install (`reblue_app.cpp`, `core/app_root.cpp`) |
| Dialogs | the system error applet; no file dialogs (`platform/switch/dialogs_switch.cpp`) |
| Language | the console's system language (`platform/os_language.cpp`) |
| Exit | the applet exit, not `_Exit` (`core/threading.cpp`) |
| Not on Switch | installer, updater and content downloads, D3D12, Tracy, mouse and keyboard, file dialogs |

## Known gaps

- Boots to the first frames on a console; not played further yet.
- Shift-JIS conversion: newlib has no iconv, so non-ASCII text in guest debug
  prints and in mod rows added to game tables becomes `?`.
- No performance work yet (nfsmw-nx's direct calls, LTO/PGO, function ordering,
  patched NVK).
