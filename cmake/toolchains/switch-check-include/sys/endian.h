/* Check builds only (cmake/toolchains/switch-check.cmake): devkitPro's newlib
 * has <sys/endian.h>, which the SDK's switch_compat/endian.h includes. With
 * glibc, the real byte-order header is the system <endian.h> (shadowed here by
 * switch_compat's), so it is included by its full path. */
#pragma once
#include </usr/aarch64-linux-gnu/include/endian.h>
