#!/usr/bin/env bash
# Stage 1 of the Nintendo Switch build: everything that runs on the build
# machine (x86-64 Linux). Runs inside the reblue-switch-host image
# (tools/switch/Dockerfile.host); tools/switch/build-switch.ps1 starts it.
#
#   docker run --rm -v "$PWD:/work" -v "<game folder>:/game:ro" -w /work \
#       reblue-switch-host bash tools/switch/host-stage.sh
#
# Makes, in the source tree (all git-ignored):
#   thirdparty/rexglue-sdk/thirdparty/...   SDK third-party sources (fetched once)
#   assets/default.xex, assets/shaders/     from the game folder (/game)
#   out/switch-host/sdk/                    host build of the SDK (rexglue)
#   generated/                              the recompiled game code (rexglue codegen)
#   out/switch-host/xenos/                  host build of XenosRecomp
#   out/switch-host/shader_cache.cpp        the 152 game shaders as SPIR-V
#
# Environment:
#   JOBS=N      parallel compile jobs (default: CPUs)
#   CLEAN=1     rebuild the host tools and redo codegen and the shader cache
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
JOBS="${JOBS:-$(nproc)}"
host="$root/out/switch-host"
CC=clang-19
CXX=clang++-19
HOST_FLAGS="-march=x86-64-v2"

log() { printf '\n==== %s ====\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

command -v "$CXX" >/dev/null || die "$CXX missing (run inside the reblue-switch-host image)"
git config --global --add safe.directory '*' 2>/dev/null || true

if [ "${CLEAN:-0}" = 1 ]; then
    log "Clean host outputs"
    rm -rf "$host" "$root/generated/codegen.build.stamp" "$root/generated/sources.cmake"
fi
mkdir -p "$host"

# ---------------------------------------------------------------- sources
log "Submodules"
git submodule update --init --recursive --depth 1

log "Patches"
# Apply a chain of patches to a submodule. A stamp in its git directory records
# the chain; when it matches the tree is ready, otherwise the submodule is reset
# to its pinned commit and the whole chain applied again.
apply_chain() { # <dir> <patch>...
    local dir="$1"; shift
    local stamp chain
    stamp="$(git -C "$dir" rev-parse --absolute-git-dir)/reblue-switch-patches"
    chain="$(git -C "$dir" rev-parse HEAD) $(cat "$@" | sha256sum | cut -d' ' -f1)"
    if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$chain" ]; then
        echo "$dir: already patched"
        return
    fi
    git -C "$dir" checkout -q -- .
    local patch
    for patch in "$@"; do
        git -C "$dir" apply "$patch" || die "$(basename "$patch") does not apply to $dir"
        echo "$dir: applied $(basename "$patch")"
    done
    echo "$chain" > "$stamp"
}
P="$root/tools/switch/patches"
apply_chain thirdparty/plume "$P/plume-switch.patch"
apply_chain thirdparty/plume/contrib/volk "$P/volk-switch.patch"

log "SDK third-party sources"
python3 tools/switch/fetch_thirdparty.py

# ---------------------------------------------------------------- game inputs
log "Game inputs"
if [ -d /game ]; then
    python3 tools/switch/prepare_assets.py /game
fi
[ -f assets/default.xex ] || die "assets/default.xex missing: mount your game folder at /game (build-switch.ps1 -Game <folder>)"
n_shaders=$(find assets/shaders -name '*.[vp]so' 2>/dev/null | wc -l)
[ "$n_shaders" -ge 152 ] || die "assets/shaders holds $n_shaders shaders, expected 152 (prepare_assets.py)"

# ---------------------------------------------------------------- rexglue
log "Host SDK (rexglue code generator)"
cmake -S thirdparty/rexglue-sdk -B "$host/sdk" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=$CC -DCMAKE_CXX_COMPILER=$CXX \
    -DCMAKE_C_FLAGS="$HOST_FLAGS" -DCMAKE_CXX_FLAGS="$HOST_FLAGS" \
    -DREXGLUE_HOST_TOOLS_ONLY=ON -DREXGLUE_ENABLE_TRACY=OFF
cmake --build "$host/sdk" --target rexglue -j "$JOBS"
# The SDK writes its programs to its own out/<platform>/ folder.
rexglue="$(find thirdparty/rexglue-sdk/out "$host/sdk" -name rexglue -type f -perm -u+x 2>/dev/null | head -n1)"
[ -x "$rexglue" ] || die "rexglue was not built"

# Codegen again only when one of its inputs changed: the manifest, the hook and
# function configs, the executable or the code generator itself.
codegen_key="$( { cat reblue_manifest.toml; find config -type f -print0 | sort -z | xargs -0 cat; \
                  sha256sum assets/default.xex "$rexglue"; } | sha256sum | cut -d' ' -f1)"
codegen_stamp="$host/codegen.key"
if [ -f generated/sources.cmake ] && [ -f "$codegen_stamp" ] && [ "$(cat "$codegen_stamp")" = "$codegen_key" ]; then
    log "Codegen: up to date"
else
    log "Codegen (recompiling default.xex)"
    "$rexglue" codegen reblue_manifest.toml
    echo "$codegen_key" > "$codegen_stamp"
fi
[ -f generated/sources.cmake ] || die "codegen wrote no generated/sources.cmake"

# ---------------------------------------------------------------- shaders
log "Host XenosRecomp"
cmake -S thirdparty/XenosRecomp -B "$host/xenos" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=$CC -DCMAKE_CXX_COMPILER=$CXX \
    -DCMAKE_C_FLAGS="$HOST_FLAGS" -DCMAKE_CXX_FLAGS="$HOST_FLAGS -DREBLUE_RECOMP"
cmake --build "$host/xenos" --target XenosRecomp -j "$JOBS"
xenos="$host/xenos/XenosRecomp/XenosRecomp"
[ -x "$xenos" ] || die "XenosRecomp was not built"

shader_key="$( { sha256sum "$xenos" thirdparty/XenosRecomp/XenosRecomp/shader_common.h; \
                 find assets/shaders -type f -print0 | sort -z | xargs -0 sha256sum; } | sha256sum | cut -d' ' -f1)"
shader_stamp="$host/shader_cache.key"
if [ -f "$host/shader_cache.cpp" ] && [ -f "$shader_stamp" ] && [ "$(cat "$shader_stamp")" = "$shader_key" ]; then
    log "Shader cache: up to date"
else
    log "Shader cache (XenosRecomp, 152 shaders)"
    LD_LIBRARY_PATH="$root/thirdparty/XenosRecomp/thirdparty/dxc-bin/lib/x64" \
        "$xenos" assets/shaders "$host/shader_cache.cpp" thirdparty/XenosRecomp/XenosRecomp/shader_common.h
    echo "$shader_key" > "$shader_stamp"
fi

log "Host stage done"
