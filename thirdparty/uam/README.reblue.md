# uam in re:Blue

devkitPro's uam (the deko3d shader compiler, GLSL to DKSH), as vendored by sa2-nx
(`app/thirdparty/uam` at sa2-nx `adf3a4e`, 2026-10-20): SwitchWakerHD's patched copy built by CMake
as the static `uamlib` (`PATCHES.md`), with banjo-nx's codegen passes (`uam_api.h`, `setBanjoFlags`).

Licences: zlib for uam's own files (`LICENSE`), MIT for `mesa-imported/`, MPL-2.0 for SwitchWakerHD's
patches. Used by plume's deko3d back end on the Switch (docs/SWITCH-deko3d.md), and as a host tool for
the shader checks.
