/* SPDX-License-Identifier: MIT */
#ifndef AS_CAPABILITY_BITS_H
#define AS_CAPABILITY_BITS_H

/* Language capabilities distinguish file reading from writing. Kernel CAP_*
 * uses a different bit assignment: translate at the platform boundary, never
 * pass this mask straight to a kernel capability syscall. These names carry
 * numeric identities only; possessing an integer does not grant a capability. */
enum {
    AS_CAP_FS_READ = 1u << 0,
    AS_CAP_FS_WRITE = 1u << 1,
    AS_CAP_NET = 1u << 2,
    AS_CAP_PROC = 1u << 3,
    AS_CAP_GUI = 1u << 4,
    AS_CAP_RAW = 1u << 5
};

#endif
