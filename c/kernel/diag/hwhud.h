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
    /* wm_system_overlay_visible(), read under the WM state lock just before the
     * call: nonzero while a modal dialog, menu, switcher, picker, panel or
     * notification is on screen. The panel is painted over the composited
     * frame, so it must stand aside for these or it covers them. */
    int system_ui_visible;
};
/* From the WM thread, outside the WM state lock; redraws at most once a second.
 * While system_ui_visible is set it draws nothing and asks the WM to repaint the
 * rectangle it covered; once that clears it draws again. */
void hwhud_tick(const struct hwhud_wm *wm);

/* From the WM's pointer path, on the WM thread, for a left-button press at
 * (x, y) in device pixels. Returns 1 when the press landed on the panel's title
 * row (it then collapses or expands the panel and the WM must not deliver the
 * press to anything else), 0 when the press is not the panel's. */
int hwhud_click(int x, int y);

#endif
