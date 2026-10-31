/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gop_pick.h"

static void pick_reason(char *out, size_t cap, const char *reason)
{
    if (!out || !cap) return;
    size_t i = 0;
    while (i + 1 < cap && reason[i]) { out[i] = reason[i]; i++; }
    out[i] = 0;
}

/* EDID block layout/checksum and DTD active-pixel fields: Linux v6.12
 * include/drm/drm_edid.h and drivers/gpu/drm/drm_edid.c (edid_block_check,
 * drm_mode_detailed, add_detailed_modes):
 * https://github.com/torvalds/linux/blob/v6.12/include/drm/drm_edid.h
 * https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/drm_edid.c
 * This is a small independent parser, not Linux's monitor-quirk machinery.
 * Reject every advertised truncated/bad block, rather than trusting a good
 * base checksum while silently accepting a corrupt extension. */
static int valid_edid(const uint8_t *edid, size_t len)
{
    static const uint8_t header[8] = {0,255,255,255,255,255,255,0};
    if (len < 128) return 0;
    for (int i = 0; i < 8; i++) if (edid[i] != header[i]) return 0;
    if (edid[18] != 1 || edid[19] > 4) return 0;
    size_t bytes = ((size_t)edid[126] + 1) * 128;
    if (len < bytes) return 0;
    for (size_t block = 0; block < bytes; block += 128) {
        uint8_t sum = 0;
        for (size_t i = 0; i < 128; i++) sum = (uint8_t)(sum + edid[block + i]);
        if (sum != 0) return 0;
    }
    return 1;
}

int gop_pick_mode(const struct gop_mode_info *modes, int n,
                  const uint8_t *edid, size_t edid_len,
                  char *why, size_t why_len)
{
    int largest = -1;
    uint64_t area = 0;
    for (int i = 0; modes && i < n; i++) {
        if (modes[i].bpp != 32 || !modes[i].width || !modes[i].height) continue;
        uint64_t pixels = (uint64_t)modes[i].width * modes[i].height;
        if (pixels > area) { largest = i; area = pixels; }
    }
    if (largest < 0) {
        pick_reason(why, why_len, "keep-firmware");
        return -1;
    }
    const char *fallback = "no-edid-largest";
    if (edid && edid_len) {
        fallback = "bad-edid-largest";
        if (valid_edid(edid, edid_len)) {
            fallback = "edid-unavailable-largest";
            const uint8_t *dtd = edid + 54;
            uint32_t w = dtd[2] | ((uint32_t)(dtd[4] & 0xf0) << 4);
            uint32_t h = dtd[5] | ((uint32_t)(dtd[7] & 0xf0) << 4);
            /* GOP offers geometry, not timing controls. Do not double an
             * interlaced field height or invent a mode from CTA/DisplayID:
             * only a progressive preferred base DTD can select a GOP mode.
             * EDID 1.4 mandates preferred DTD; older revisions flag it. */
            int preferred = edid[19] == 4 || (edid[24] & 2);
            if (preferred && (dtd[0] || dtd[1]) && !(dtd[17] & 0x80)) {
                for (int i = 0; i < n; i++) {
                    if (modes[i].bpp == 32 && w && h &&
                        modes[i].width == w && modes[i].height == h) {
                        pick_reason(why, why_len, "edid-preferred");
                        return i;
                    }
                }
            }
        }
    }
    pick_reason(why, why_len, fallback);
    return largest;
}
