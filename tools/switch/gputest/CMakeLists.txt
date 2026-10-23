# GPU test NRO (gputest.cpp): the game's heavy pixel shaders timed on deko3d and on NVK.
# Built by tools/switch/build-switch.sh with GPUTEST=1 (run-switch-build.bat -GpuTest). It links NVK the way the
# title does (rex::switch_gpu, the static Mesa driver) and devkitPro's deko3d.
find_library(REBLUE_DEKO3D_LIB deko3d PATHS "${DEVKITPRO}/libnx/lib" NO_DEFAULT_PATH)
if(NOT REBLUE_DEKO3D_LIB)
    message(FATAL_ERROR "gputest: devkitPro's deko3d is not installed (dkp-pacman -S deko3d)")
endif()
add_executable(reblue_gputest
    gputest.cpp
    "${CMAKE_SOURCE_DIR}/thirdparty/rexglue-sdk/src/ui/switch_elf_stubs.c")
target_compile_features(reblue_gputest PRIVATE cxx_std_20)
target_include_directories(reblue_gputest PRIVATE
    "${CMAKE_SOURCE_DIR}/thirdparty/plume/contrib/Vulkan-Headers/include")
target_link_libraries(reblue_gputest PRIVATE "${REBLUE_DEKO3D_LIB}" rex::switch_gpu)
rexglue_switch_add_nro(reblue_gputest NAME "re:Blue GPU test" AUTHOR "re:Blue" VERSION "${PROJECT_VERSION}")
