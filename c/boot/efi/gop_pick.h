/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_GOP_PICK_H
#define LOGIT_GOP_PICK_H

#include <stddef.h>
#include <stdint.h>

/* bpp describes the storage stride, not the sum of the RGB channel widths.
 * The EFI adapter admits only formats whose linear 32-bit layout is known. */
struct gop_mode_info {
    uint32_t width, height, bpp;
};

/* Returns the array index, or -1 to retain the firmware's current mode.
 * why receives a bounded, NUL-terminated reason when its capacity is nonzero.
 * No framebuffer, firmware service, or GPU register is accessed here. */
int gop_pick_mode(const struct gop_mode_info *modes, int n,
                  const uint8_t *edid, size_t edid_len,
                  char *why, size_t why_len);

#endif
