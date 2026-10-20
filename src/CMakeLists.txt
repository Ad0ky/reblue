file(GLOB_RECURSE reblue_all RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
     CONFIGURE_DEPENDS "*.cpp" "*.h")

set(reblue_backend_only
    gpu/device.cpp
    gpu/device_lost.cpp
    gpu/device_pipelines.cpp
    gpu/draw.cpp
    gpu/draw_framebuffer.cpp
    gpu/dred.cpp
    gpu/frame_ring.cpp
    gpu/gpu_profiling.cpp
    gpu/hooks/shader.cpp
    gpu/imgui_overlay_drawer.cpp
    gpu/occlusion.cpp
    gpu/pipeline/pipeline_cache.cpp
    gpu/present.cpp
    gpu/resolve.cpp
    gpu/shaders/guest_shaders.cpp
    gpu/vertex_declaration.cpp
    platform/crash_handler.cpp
    reblue_app.cpp)

# The runtime DXC shader linker has no Vulkan counterpart.
set(reblue_d3d12_only
    gpu/shaders/dxc_link.cpp
    gpu/shaders/dxc_link.h
    gpu/shaders/shader_linker.cpp
    gpu/shaders/shader_linker.h)

set(reblue_installer_only
    installer/disc_install.cpp
    installer/disc_install.h
    installer/installer_wizard.cpp
    installer/installer_wizard.h
    installer/self_install.cpp
    installer/self_install.h
    installer/upgrade_prompt.cpp
    installer/upgrade_prompt.h)

list(REMOVE_ITEM reblue_all
     ${reblue_backend_only} ${reblue_d3d12_only} ${reblue_installer_only})

# Nintendo Switch: there is no SDL on Horizon (libnx replaces it in the SDK),
# so the files built on SDL dialogs and device queries give way to the ones in
# platform/switch/. Everywhere else platform/switch/ is left out.
set(reblue_sdl_only
    platform/fatal_dialog.cpp
    platform/file_dialog.cpp
    platform/host_devices.cpp)
set(reblue_switch_only ${reblue_all})
list(FILTER reblue_switch_only INCLUDE REGEX "^platform/switch/")
if(REBLUE_SWITCH)
    list(REMOVE_ITEM reblue_all ${reblue_sdl_only})
    # POSIX signals and dladdr: platform/switch/crash_handler_switch.cpp instead,
    # compiled with the exe like the original (it reads the build config).
    list(REMOVE_ITEM reblue_backend_only platform/crash_handler.cpp)
    list(REMOVE_ITEM reblue_all platform/switch/crash_handler_switch.cpp)
    list(APPEND reblue_backend_only platform/switch/crash_handler_switch.cpp)
else()
    list(REMOVE_ITEM reblue_all ${reblue_switch_only})
endif()

reblue_sources(${reblue_all})
reblue_backend_sources(${reblue_backend_only})
reblue_d3d12_sources(${reblue_d3d12_only})
if(REBLUE_BUILD_INSTALLER)
    reblue_sources(${reblue_installer_only})
endif()
