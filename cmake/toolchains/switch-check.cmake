# Development aid, not a release toolchain: compiles the Switch code paths with
# a stock aarch64-linux-gnu GCC (Ubuntu g++-14-aarch64-linux-gnu) and the libnx
# headers, so most compile errors can be found on a machine without devkitPro.
# Linux macros are removed so the SDK and re:Blue take their Switch branches;
# glibc still supplies the C library headers, so a function newlib lacks is not
# caught here. Nothing links and no NRO is made.
# Adapted from Lost Odyssey Recomp's tools/switch/check-toolchain.cmake.
#
#   git clone --depth 1 https://github.com/switchbrew/libnx /path/to/libnx
#   cmake -S . -B out/switch-check -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/switch-check.cmake \
#     -DREBLUE_SWITCH_CHECK_LIBNX=/path/to/libnx
#   cmake --build out/switch-check -- -k 0
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(REXGLUE_PLATFORM_SWITCH ON CACHE BOOL "" FORCE)
set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "" FORCE)
set(REXGLUE_ENABLE_PERF_COUNTERS OFF CACHE BOOL "" FORCE)
set(REXGLUE_USE_VULKAN ON CACHE BOOL "" FORCE)
set(REXGLUE_USE_D3D12 OFF CACHE BOOL "" FORCE)
set(REBLUE_SWITCH_CHECK_ONLY ON CACHE BOOL "" FORCE)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc-14)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++-14)
set(_flags "-march=armv8-a+crc+crypto -mtune=cortex-a57 -D__SWITCH__ -DNX -D_GNU_SOURCE -DSPDLOG_NO_TZ_OFFSET -U__linux__ -U__linux -Ulinux -U__gnu_linux__ -U__unix__ -U__unix -Uunix -Wno-psabi")
set(CMAKE_C_FLAGS_INIT "${_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_flags}")

# The SDK's rexglue_switch.cmake wants a Mesa SDK folder with lib/libvulkan.a and
# a devkitPro root. Neither is used without linking: point both at empty stand-ins.
set(_check_stub "${CMAKE_BINARY_DIR}/switch-check-stub")
file(MAKE_DIRECTORY "${_check_stub}/lib" "${_check_stub}/libnx")
if(NOT EXISTS "${_check_stub}/lib/libvulkan.a")
    file(WRITE "${_check_stub}/lib/libvulkan.a" "")
endif()
set(REXGLUE_SWITCH_NVK_SDK "${_check_stub}" CACHE PATH "" FORCE)
set(DEVKITPRO "${_check_stub}" CACHE PATH "" FORCE)

if(REBLUE_SWITCH_CHECK_LIBNX)
    include_directories(SYSTEM "${REBLUE_SWITCH_CHECK_LIBNX}/nx/include"
        "${CMAKE_CURRENT_LIST_DIR}/switch-check-include")
endif()
