/* A performance and hardware panel drawn over the desktop, for machines with
 * no serial port: one photograph carries the display modes, CPU load and
 * frequency, SMIs, interrupt rates, clock cost, compositor timing, MTRRs and
 * USB HID traffic. Built only with HWHUD=1 (LOGIT_HWHUD). X79-VG4 bring-up. */
#ifndef HWHUD_H
#define HWHUD_H
#include <stdint.h>

/* The firmware's display report (LOGIT_BOOT_TAG_DISPLAY), kept for the panel
 * and logged once. Call after the boot information block is reachable. */
void hwhud_capture_boot_info(uint64_t info);

/* The compositor's cumulative counters; the panel shows per-second deltas. */
struct hwhud_wm {
    uint64_t composites, comp_ns, comp_ns_max, present_ns;
    uint64_t cursor_moves, cursor_ns, motions;
};
/* From the WM thread, outside the WM state lock; redraws at most once a second. */
void hwhud_tick(const struct hwhud_wm *wm);

#endif
