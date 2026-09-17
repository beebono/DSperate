/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors. */

/* The gamma LUT's pow() call, isolated so its symbol version can be pinned.
 *
 * glibc 2.29 introduced pow@GLIBC_2.29, and on an aarch64 build against a
 * modern glibc that one symbol is the *only* import above GLIBC_2.18 that we
 * can do anything about -- it alone would set the runtime floor at 2.29 and
 * lock out the handhelds we ship to (spruceOS measures a 2.28 arm64 ceiling;
 * the A30 sysroot is 2.23). pow@GLIBC_2.17 exists in every aarch64 glibc
 * since 2.17 and the two produce a byte-identical gamma LUT (verified under
 * qemu), so pinning it cannot move a scene hash.
 *
 * This lives in its own C file, built with -fno-lto (see CMakeLists), because
 * a .symver directive does NOT survive our -flto=auto link: placed in
 * gpu.cpp it is present in the preprocessed TU and silently dropped by the
 * time the binary is written, which is a trap worth only falling into once.
 *
 * Note this fixes one symbol, not the floor: stat/fstat and the __isoc23_*
 * redirects have no old version to bind to, so a genuinely portable tarball
 * still has to be built against an old sysroot.
 */
#include <math.h>

#if defined(__aarch64__) && defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 29)
__asm__(".symver pow, pow@GLIBC_2.17");
#endif
#endif

double ds_pow_compat(double x, double y) { return pow(x, y); }
