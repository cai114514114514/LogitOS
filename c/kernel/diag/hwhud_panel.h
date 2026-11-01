/* The HWHUD panel's decisions, kept pure: where it sits, when it may be on the
 * screen, and what a click on it means. hwhud.c does the sampling and the
 * drawing around them; the host gate (tests/hw/hudfix/gate_test.c) drives the
 * same functions with a fake clock. Static inline so a host test that includes
 * hwhud.c for another formatter does not trip -Wunused-function. */
#ifndef HWHUD_PANEL_H
#define HWHUD_PANEL_H
#include <stdint.h>

/* ---- where the panel is, and when it may be on the screen -----------------
 * The panel is not a window. It is painted straight into the back buffer after
 * the compositor has finished the frame, so nothing composited later knows it
 * is there and it lands on top of whatever the WM drew -- the power
 * confirmation dialog included, which on the X79-VG4 left the user unable to
 * reach the Shut Down button. Everything that decides whether and where the
 * panel is drawn, and what a click on it means, is pure so the host gate drives
 * the production decisions with a fake clock. Not verified on X79-VG4
 * hardware. */

/* A rectangle in device pixels. */
struct hwhud_box {
    int x, y, w, h;
};

/* What the panel remembers between calls. */
struct hwhud_panel {
    int shown;                  /* panel pixels may be on the screen right now */
    struct hwhud_box box;       /* where they were drawn; only meaningful while shown */
    int collapsed;              /* title row only; set by a click, kept for the session */
    int stale;                  /* the picture must be drawn again before the next sample */
};

/* What one call of the panel's tick has to do. */
enum hwhud_action {
    HWHUD_IDLE,                 /* nothing: not due, or hidden and still hidden */
    HWHUD_SAMPLE,               /* a second has passed: read the sources, then draw */
    HWHUD_REDRAW,               /* draw the last sample again: the picture was lost or reshaped */
    HWHUD_ERASE                 /* system UI appeared over a drawn panel: repaint its box */
};

/* The panel's rectangle for a given number of text rows: flush to the right
 * edge with an 8 pixel margin, 40 pixels down (below the menu bar), 4 pixels of
 * padding around the text. A screen narrower than the panel pins it to x = 0. */
static inline struct hwhud_box hwhud_box_for(unsigned screen_width, int cols, int rows,
                                             int cell_width, int cell_height)
{
    struct hwhud_box box;
    box.w = cols * cell_width + 8;
    box.h = rows * cell_height + 8;
    box.x = (int)screen_width - box.w - 8;
    if (box.x < 0) {
        box.x = 0;
    }
    box.y = 40;
    return box;
}

/* Decide what this call does. System UI wins over everything: while any is
 * visible the panel draws nothing at all, and the one call that finds a drawn
 * panel under it hands back ERASE so the caller asks the WM to recompose that
 * rectangle -- the stale panel pixels are then replaced by the real frame, not
 * left behind. A hidden panel comes back on the first call after the system UI
 * is gone: as a fresh sample when one is due, else from the last one. Updates
 * the panel's shown and stale flags for the ERASE case only; the caller records
 * a finished draw with hwhud_note_drawn. HUD_NEGCTL_IGNORE_OVERLAY is the
 * watched failure: the old behaviour, a panel that never looks at the system UI. */
static inline enum hwhud_action hwhud_step(struct hwhud_panel *panel, int system_ui_visible,
                                           uint64_t now_ms, uint64_t last_sample_ms)
{
#ifdef HUD_NEGCTL_IGNORE_OVERLAY
    system_ui_visible = 0;
#endif
    if (system_ui_visible) {
        if (!panel->shown) {
            return HWHUD_IDLE;
        }
        panel->shown = 0;
        panel->stale = 1;
        return HWHUD_ERASE;
    }
    int sample_due = last_sample_ms == 0 || now_ms - last_sample_ms >= 1000;
    if (sample_due) {
        return HWHUD_SAMPLE;
    }
    if (panel->stale) {
        return HWHUD_REDRAW;
    }
    return HWHUD_IDLE;
}

/* Record that the panel has just been painted into BOX, so the next ERASE or
 * click knows what is on the screen and nothing is waiting to be redrawn. */
static inline void hwhud_note_drawn(struct hwhud_panel *panel, const struct hwhud_box *box)
{
    panel->shown = 1;
    panel->box = *box;
    panel->stale = 0;
}

/* A left press at (x, y). The mouse is the only input on a board whose USB
 * keyboard is not up yet, so the panel's title row is a button: it flips the
 * panel between full and one line. Returns 1 when the press is the panel's
 * (and the caller must consume it), with *repaint set to the rectangle the
 * panel covered, which the caller hands to the WM because a smaller panel
 * leaves pixels behind. Only a panel that is on the screen can be pressed; a
 * collapsed one is all title row. HUD_NEGCTL_PRESS_PASSES_THROUGH is the watched
 * failure: a panel whose title row never takes the press, so it reaches the
 * window underneath and nothing collapses. */
static inline int hwhud_press(struct hwhud_panel *panel, int x, int y, int cell_height,
                              struct hwhud_box *repaint)
{
    int pressable = panel->shown;
#ifdef HUD_NEGCTL_PRESS_PASSES_THROUGH
    pressable = 0;
#endif
    if (!pressable) {
        return 0;
    }
    const struct hwhud_box *box = &panel->box;
    int row_bottom = panel->collapsed ? box->y + box->h : box->y + 4 + cell_height;
    if (x < box->x || x >= box->x + box->w || y < box->y || y >= row_bottom) {
        return 0;
    }
    *repaint = *box;
    panel->collapsed = !panel->collapsed;
    panel->stale = 1;
    return 1;
}

#endif
