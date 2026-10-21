#!/usr/bin/env bash
# Stage 2 of the Nintendo Switch build: cross-compiles reblue.nro. Runs inside
# the switch-dev Docker image (devkitA64, libnx, mesa-switch NVK, CMake, Ninja),
# after tools/switch/host-stage.sh has made the generated code and the shader
# cache. On Windows tools/switch/build-switch.ps1 runs both stages.
#
#   docker run --rm -v "$PWD:/work" -w /work lo-switch-dev bash tools/switch/build-switch.sh
#
# Environment:
#   JOBS=N             parallel compile jobs (default: CPUs; ~1.5 GB of RAM per
#                      job for the recompiled code, lower it if the container
#                      runs out of memory)
#   BUILD_TYPE=...     Release (default) or RelWithDebInfo
#   LTO=1              link-time optimisation of the game code (slow link, ~16 GB
#                      of RAM for Docker)
#   NVK_SDK=path       portlibs/switch folder holding mesa-switch's lib/libvulkan.a
#                      (default: searched under $DEVKITPRO)
#   CLEAN=1            delete the Switch build folder first
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
: "${DEVKITPRO:=/opt/devkitpro}"
export DEVKITPRO
JOBS="${JOBS:-$(nproc)}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
LTO="${LTO:-0}"
build="$root/out/switch-build"

log() { printf '\n==== %s ====\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- toolchain
[ -x "$DEVKITPRO/devkitA64/bin/aarch64-none-elf-g++" ] || die "devkitA64 not found under $DEVKITPRO (run inside the switch-dev image)"
[ -f "$DEVKITPRO/libnx/lib/libnx.a" ] || die "libnx not found under $DEVKITPRO/libnx"
if [ -z "${NVK_SDK:-}" ]; then
    nvk_lib="$(find "$DEVKITPRO" -name libvulkan.a -path '*switch*' 2>/dev/null | head -n1 || true)"
    [ -n "$nvk_lib" ] || die "mesa-switch libvulkan.a not found under $DEVKITPRO; set NVK_SDK"
    NVK_SDK="$(dirname "$(dirname "$nvk_lib")")"
fi
[ -f "$NVK_SDK/lib/libvulkan.a" ] || die "no lib/libvulkan.a under NVK_SDK=$NVK_SDK"
echo "devkitA64: $("$DEVKITPRO/devkitA64/bin/aarch64-none-elf-g++" --version | head -n1)"
echo "NVK:       $NVK_SDK/lib/libvulkan.a"
command -v cmake >/dev/null || die "cmake missing"
command -v ninja >/dev/null || die "ninja missing"

# ---------------------------------------------------------------- inputs
[ -f generated/sources.cmake ] || die "generated/ is empty: run the host stage first (tools/switch/host-stage.sh)"
[ -f out/switch-host/shader_cache.cpp ] || die "out/switch-host/shader_cache.cpp missing: run the host stage first"
[ -f thirdparty/rexglue-sdk/thirdparty/.fetched-bd833a2ab69d ] || die "SDK third-party sources missing: run the host stage first"

# ---------------------------------------------------------------- build
if [ "${CLEAN:-0}" = 1 ]; then rm -rf "$build"; fi
log "Configure ($BUILD_TYPE)"
cmake -S . -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$root/cmake/toolchains/switch-devkitA64.cmake" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
    -DDEVKITPRO="$DEVKITPRO" \
    -DREXGLUE_SWITCH_NVK_SDK="$NVK_SDK" \
    -DREBLUE_SWITCH_LTO="$([ "$LTO" = 1 ] && echo ON || echo OFF)"

log "Build ($JOBS jobs)"
# -k 0: keep going after a failure so one run reports every failing file.
cmake --build "$build" --target reblue -j "$JOBS" -- -k 0

out="$root/out/switch"
mkdir -p "$out"
cp "$build/reblue.nro" "$out/"
cp "$build/reblue.elf" "$out/" 2>/dev/null || cp "$build/reblue" "$out/reblue.elf"
log "Done"
echo "NRO: out/switch/reblue.nro  (copy to sdmc:/switch/reblue/)"
echo "ELF: out/switch/reblue.elf  (keep it: crash addresses resolve against it)"
