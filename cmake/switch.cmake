# cmake/switch.cmake
# Nintendo Switch (Horizon homebrew) settings. Included from the top-level
# CMakeLists.txt when the toolchain (cmake/toolchains/switch-devkitA64.cmake)
# sets REXGLUE_PLATFORM_SWITCH. See docs/SWITCH.md.
#
# The ReXGlue SDK is the Switch port from nfsmw-nx, vendored in
# thirdparty/rexglue-sdk (BSD-3-Clause, see thirdparty/rexglue-sdk/README.reblue.md).
# It provides the Horizon runtime: guest memory, exceptions, threads, clocks,
# audio, input, the libnx window and NRO packaging.

# The vendored Switch SDK, built from source (no prebuilt package exists).
if(NOT REXSDK_DIR)
    set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rexglue-sdk" CACHE PATH
        "Path to rexglue-sdk source tree" FORCE)
endif()

# PC-only features. None of them has a Switch implementation.
#   installer: discs are extracted on a PC (re:Blue's PC installer) and copied
#              to the SD card
#   profiling: Tracy is unavailable on Horizon
set(REBLUE_BUILD_INSTALLER OFF CACHE BOOL "Not available on Nintendo Switch" FORCE)
set(REBLUE_PROFILING OFF CACHE BOOL "Not available on Nintendo Switch" FORCE)
set(REBLUE_PSO_CAP OFF CACHE BOOL "Not available on Nintendo Switch" FORCE)

# Host-side outputs made by tools/switch/build-switch.sh before this cross
# build: the XenosRecomp shader cache (SPIR-V of the 152 game shaders) and the
# host dxc used for re:Blue's own HLSL. Neither tool can run as a Switch binary.
set(REBLUE_SWITCH_HOST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/out/switch-host" CACHE PATH
    "Output folder of the host stage (shader cache, host tools)")
set(REBLUE_SWITCH_SHADER_CACHE "${REBLUE_SWITCH_HOST_DIR}/shader_cache.cpp" CACHE FILEPATH
    "XenosRecomp output made on the host")

# NRO metadata shown in the Homebrew Menu.
set(REBLUE_SWITCH_APP_TITLE "re:Blue" CACHE STRING "NRO title")
set(REBLUE_SWITCH_APP_AUTHOR "re:Blue contributors" CACHE STRING "NRO author")
set(REBLUE_SWITCH_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/res/switch/icon.jpg" CACHE FILEPATH
    "256x256 JPEG NRO icon (libnx default when missing)")

# Game data and user files on the SD card. game/ holds the files re:Blue's PC
# installer copies out of the three discs (default.xex, pack/, movie/, ...).
set(REBLUE_SWITCH_DATA_ROOT "/switch/reblue" CACHE STRING
    "SD card folder for game data, settings and logs")
add_compile_definitions(REBLUE_SWITCH_DATA_ROOT="${REBLUE_SWITCH_DATA_ROOT}")

# Code generation (the same experiments as nfsmw-nx; see its docs/toolchain.md).
option(REBLUE_SWITCH_LTO "Link-time optimisation (needs a lot of RAM on the build machine)" OFF)
set(REBLUE_SWITCH_LTO_JOBS "2" CACHE STRING "Parallel LTRANS jobs for the LTO link")
if(REBLUE_SWITCH_LTO)
    add_compile_options(-flto -fno-fat-lto-objects)
    add_link_options(-flto=${REBLUE_SWITCH_LTO_JOBS} -flto-partition=balanced)
endif()

# Development check build (cmake/toolchains/switch-check.cmake): compiles the
# Switch code paths with a stock aarch64 Linux GCC and the libnx headers,
# links nothing.
if(REBLUE_SWITCH_CHECK_ONLY)
    message(STATUS "reblue: Switch compile check only (no NRO)")
endif()

message(STATUS "reblue: Nintendo Switch build (SDK ${REXSDK_DIR}, data ${REBLUE_SWITCH_DATA_ROOT})")
