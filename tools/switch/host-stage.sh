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
    # Files a patch added last time (untracked) would make it fail to apply again.
    git -C "$dir" clean -fdq
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
# The shader recompiler: on the Switch the constant banks are uniform buffers
# (REBLUE_SPIRV_UBO below), and the cbuffers SPIR-V needs for that.
apply_chain thirdparty/XenosRecomp "$P/xenosrecomp-switch.patch"

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

# Every run, not only after codegen: a hook added to the sources turns its call
# sites back into plain calls. DIRECT_CALLS=0 restores all plain calls.
log "Direct calls (tools/switch/direct_calls.py)"
if [ "${DIRECT_CALLS:-1}" = 0 ]; then
    python3 tools/switch/direct_calls.py "$root" --undo
else
    python3 tools/switch/direct_calls.py "$root"
fi

# ---------------------------------------------------------------- shaders
log "Host XenosRecomp"
cmake -S thirdparty/XenosRecomp -B "$host/xenos" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=$CC -DCMAKE_CXX_COMPILER=$CXX \
    -DCMAKE_C_FLAGS="$HOST_FLAGS" -DCMAKE_CXX_FLAGS="$HOST_FLAGS -DREBLUE_RECOMP -DREBLUE_SPIRV_UBO"
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

# ---------------------------------------------------------------- deko3d shaders (DEKO3D=1, the default)
# Every shader as GLSL for uam (docs/SWITCH-deko3d.md): XenosRecomp's HLSL of the game shaders and re:Blue's own
# HLSL, through DXC, SPIRV-Cross and tools/switch/deko3d/spv_to_dk_glsl.py, into out/switch-host/deko3d_shaders.cpp.
if [ "${DEKO3D:-1}" = 1 ]; then
    if ! command -v spirv-dis >/dev/null; then
        log "Installing spirv-tools (spirv-dis)"
        (apt-get update -qq && apt-get install -y -qq --no-install-recommends spirv-tools) >/dev/null \
            || die "spirv-dis is missing and spirv-tools could not be installed"
    fi
    spirv_cross_tag="vulkan-sdk-1.4.363.0"
    spirv_cross="$host/spirv-cross-$spirv_cross_tag"
    if [ ! -x "$spirv_cross/build/spirv-cross" ]; then
        log "SPIRV-Cross $spirv_cross_tag"
        rm -rf "$spirv_cross"
        git clone -q --depth 1 --branch "$spirv_cross_tag" https://github.com/KhronosGroup/SPIRV-Cross "$spirv_cross"
        cmake -S "$spirv_cross" -B "$spirv_cross/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_COMPILER=$CC -DCMAKE_CXX_COMPILER=$CXX \
            -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_CLI=ON >/dev/null
        cmake --build "$spirv_cross/build" --target spirv-cross -j "$JOBS" >/dev/null
    fi
    dk_key="$( { echo "$shader_key"; sha256sum tools/switch/deko3d/*.py src/gpu/shaders/hlsl/*; \
                 echo "$spirv_cross_tag"; } | sha256sum | cut -d' ' -f1)"
    dk_stamp="$host/deko3d_shaders.key"
    if [ -f "$host/deko3d_shaders.cpp" ] && [ -f "$dk_stamp" ] && [ "$(cat "$dk_stamp")" = "$dk_key" ]; then
        log "deko3d shaders: up to date"
    else
        log "deko3d shaders (HLSL dump, DXC, SPIRV-Cross, GLSL for uam)"
        rm -rf "$host/hlsl"
        mkdir -p "$host/hlsl"
        LD_LIBRARY_PATH="$root/thirdparty/XenosRecomp/thirdparty/dxc-bin/lib/x64" \
            "$xenos" assets/shaders "$host/hlsl/.shader_cache.cpp" thirdparty/XenosRecomp/XenosRecomp/shader_common.h \
            "$host/hlsl" >/dev/null
        SPIRV_CROSS="$spirv_cross/build/spirv-cross" python3 tools/switch/deko3d/make_dk_shaders.py \
            --repo "$root" --hlsl-dump "$host/hlsl" --jobs "$JOBS" \
            --dxc "$root/thirdparty/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxc-linux" \
            --out "$host/deko3d_shaders.cpp" || die "deko3d shaders failed"
        echo "$dk_key" > "$dk_stamp"
    fi
fi

log "Host stage done"
