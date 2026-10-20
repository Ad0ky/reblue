if(REBLUE_D3D12)
    set(PLUME_D3D12_AGILITY_SDK_ENABLED ON CACHE BOOL "" FORCE)
endif()
# Linux picks X11 or Wayland at runtime, so plume builds its surface through the
# SDL3 window the SDK already owns. rexglue.cmake has imported SDL3 by now, so
# plume finds that target rather than a second copy on the system.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(PLUME_SDL_VULKAN_ENABLED ON CACHE BOOL "" FORCE)
endif()
# The SDK's subdirectory build has no rex::o1heap (only its installed package
# exports it); the Switch build always takes the SDK from source.
if(TARGET o1heap AND NOT TARGET rex::o1heap)
    add_library(rex::o1heap ALIAS o1heap)
endif()
add_subdirectory(plume EXCLUDE_FROM_ALL)

if(REBLUE_SWITCH)
    # XenosRecomp and dxc are host programs; the host stage of
    # tools/switch/build-switch.sh runs them. Only dxc's command line is needed
    # here, for re:Blue's own HLSL (cmake/shaders.cmake), and the vendored
    # Linux x64 binary runs on the build machine.
    set(_reblue_dxc_bin "${CMAKE_CURRENT_SOURCE_DIR}/XenosRecomp/thirdparty/dxc-bin")
    set(DIRECTX_DXC_TOOL "LD_LIBRARY_PATH=${_reblue_dxc_bin}/lib/x64" "${_reblue_dxc_bin}/bin/x64/dxc-linux"
        CACHE INTERNAL "")
else()
    add_subdirectory(XenosRecomp EXCLUDE_FROM_ALL)
    target_compile_definitions(XenosRecomp PRIVATE REBLUE_RECOMP)
    target_precompile_headers(XenosRecomp PRIVATE <fmt/format.h>)
endif()

if(NOT TARGET xxHash::xxhash)
    set(XXHASH_BUILD_XXHSUM OFF CACHE BOOL "" FORCE)
    set(build_shared_prev ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    add_subdirectory(XenosRecomp/thirdparty/xxHash/cmake_unofficial EXCLUDE_FROM_ALL)
    set(BUILD_SHARED_LIBS ${build_shared_prev} CACHE BOOL "" FORCE)
endif()

add_library(reblue_thirdparty OBJECT
    miniz/miniz.cpp
    stb/stb_image.cpp
    zstd/zstddeclib.c)

if(REBLUE_VULKAN_TARGETS)
    target_sources(reblue_thirdparty PRIVATE
        XenosRecomp/thirdparty/smol-v/source/smolv.cpp)
endif()

target_include_directories(reblue_thirdparty PUBLIC
    miniz
    picosha2
    stb
    zstd
    XenosRecomp/thirdparty/smol-v/source)
target_compile_options(reblue_thirdparty PRIVATE -w)
target_link_libraries(reblue_thirdparty PRIVATE rex::runtime)
