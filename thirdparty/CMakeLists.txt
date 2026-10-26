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
    # One VMA in the NRO: the SDK's UI library (rexui) already compiles its own,
    # newer VMA, so on Switch plume does not (see plume_vulkan.cpp) and builds
    # against the SDK's vk_mem_alloc.h instead of its contrib copy. Two
    # versions silently merged by --allow-multiple-definition crashed in
    # vmaDestroyImage.
    set(_reblue_sdk_vma "${REXSDK_DIR}/thirdparty/vulkan-memory-allocator/include")
    if(NOT EXISTS "${_reblue_sdk_vma}/vk_mem_alloc.h")
        message(FATAL_ERROR "SDK VMA header missing at ${_reblue_sdk_vma}: run the host stage first")
    endif()
    target_include_directories(plume BEFORE PUBLIC "${_reblue_sdk_vma}")
    # deko3d back end (docs/SWITCH-deko3d.md): banjo-nx's plume_deko3d*.cpp, built beside the Vulkan one so the
    # NRO can run either (bd_switch_backend). uam compiles the shaders on the console; its Mesa 19 and NVK's newer
    # Mesa define C functions of the same names with other signatures, so uam goes in as one object whose strong
    # symbols are local but its API (plume_uam_localize.cmake, as banjo-nx and sa2-nx do).
    if(REBLUE_SWITCH_DEKO3D)
        if(NOT EXISTS "${DEVKITPRO}/libnx/lib/libdeko3d.a")
            message(FATAL_ERROR "REBLUE_SWITCH_DEKO3D: devkitPro's deko3d is not installed (dkp-pacman -S deko3d)")
        endif()
        if(NOT TARGET uamlib)
            set(NX_ROOT "${DEVKITPRO}/libnx")
            add_subdirectory(uam "${CMAKE_CURRENT_BINARY_DIR}/uam" EXCLUDE_FROM_ALL)
        endif()
        target_sources(plume PRIVATE plume/plume_deko3d.cpp plume/plume_deko3d_commands.cpp)
        target_compile_definitions(plume PUBLIC PLUME_DEKO3D=1)
        target_include_directories(plume PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/uam")
        set(_dk_bin "${DEVKITPRO}/devkitA64/bin")
        set(_uam_local "${CMAKE_CURRENT_BINARY_DIR}/uam_local.o")
        add_custom_command(
            OUTPUT "${_uam_local}"
            COMMAND "${CMAKE_COMMAND}" "-DLD=${_dk_bin}/aarch64-none-elf-ld" "-DNM=${_dk_bin}/aarch64-none-elf-nm"
                    "-DOBJCOPY=${_dk_bin}/aarch64-none-elf-objcopy" "-DARCHIVE=$<TARGET_FILE:uamlib>"
                    "-DOUT=${_uam_local}" -P "${CMAKE_CURRENT_SOURCE_DIR}/plume/plume_uam_localize.cmake"
            DEPENDS uamlib "${CMAKE_CURRENT_SOURCE_DIR}/plume/plume_uam_localize.cmake"
            COMMENT "uam with its strong symbols local but its API (uam_local.o)"
            VERBATIM)
        add_custom_target(reblue_uam_local DEPENDS "${_uam_local}")
        add_dependencies(plume reblue_uam_local)
        target_link_libraries(plume PUBLIC "${_uam_local}" "${DEVKITPRO}/libnx/lib/libdeko3d.a")
        message(STATUS "re:Blue: plume's deko3d back end with uam (thirdparty/uam)")
    endif()
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

if(REBLUE_SWITCH)
    # One stb_image in the NRO: the SDK's rexui compiles it too (PNG only, as
    # here), and --allow-multiple-definition would merge the two silently.
    get_target_property(_reblue_tp_sources reblue_thirdparty SOURCES)
    list(REMOVE_ITEM _reblue_tp_sources stb/stb_image.cpp)
    set_property(TARGET reblue_thirdparty PROPERTY SOURCES ${_reblue_tp_sources})
endif()

target_include_directories(reblue_thirdparty PUBLIC
    miniz
    picosha2
    stb
    zstd
    XenosRecomp/thirdparty/smol-v/source)
target_compile_options(reblue_thirdparty PRIVATE -w)
target_link_libraries(reblue_thirdparty PRIVATE rex::runtime)
