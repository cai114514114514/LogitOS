/* aui_data.h -- chips, kbd chips, squircle tiles, ring gauge, sparkline,
 * multi-segment bar (design 4.14, 4.15, 3.7.2, 3.7.3).
 * Included by aui.h (apps include only aui.h); it relies on struct aui_rect. */
#ifndef AUI_DATA_H
#define AUI_DATA_H

/* ---- toggle chip (design 4.15) ----
 * A capsule h 24 (AUI_CHIP_LARGE: 28), fill.control, footnote 12 / 500
 * label.primary, an optional leading 12 pt symbol (icon = a GICON_* id, -1 for
 * none). ON: accent.tint with a leading CHECK in accent.ink (it replaces the
 * symbol); hover lays fill.hover over it, cross-faded. Returns 1 on the frame
 * the chip is pressed (the caller flips `on`). aui_chip_w is its width. */
#define AUI_CHIP_LARGE  0x1
#define AUI_CHIP_OFF    0x2          /* disabled */
int aui_chip(int x, int y, const char *label, int icon, int on, int flags);
int aui_chip_w(const char *label, int icon, int on, int flags);

/* ---- kbd chip: h 18, r 4, padding x 4, fill.control, caption 11 / 500
 * label.secondary ("↩", "⌘", "esc"). Returns its width. ---- */
int aui_kbd(int x, int y, const char *s);

/* ---- squircle tiles (design 3.7.2 / 3.7.3, r.icon = 0.225 x size) ----
 * A squircle PATH filled with a 2-stop vertical gradient top -> bottom and a
 * white (icon_rgb) symbol at 56 % of the tile (`icon` a GICON_* id, -1 none).
 * AUI_TILE_EDGE adds the 1 dev sep.opaque border (the "text" file kind). */
#define AUI_TILE_EDGE 0x1
void aui_tile(struct aui_rect rect, unsigned top, unsigned bottom, int icon, unsigned icon_rgb, int flags);

enum aui_file_kind {
    AUI_FK_FOLDER, AUI_FK_TEXT, AUI_FK_CODE, AUI_FK_IMAGE, AUI_FK_AUDIO, AUI_FK_VIDEO,
    AUI_FK_OTHER, AUI_FK_COUNT
};
struct aui_tile_colors {
    unsigned top, bottom;      /* the gradient (3.7.3 table); `bottom` is also the list-row symbol colour */
    unsigned icon_rgb;         /* the symbol on the tile */
    int icon;                  /* GICON_* (the table's fallback where the set-C symbol is absent) */
    int flags;                 /* AUI_TILE_EDGE for text */
};
struct aui_tile_colors aui_file_kind_colors(enum aui_file_kind kind);
/* A Settings chip (3.7.2): a 20 pt tile of `rgb` (top = +8 % lightness) with a
 * white 13 pt symbol. */
void aui_settings_chip(int x, int y, unsigned rgb, int icon);

/* ---- ring gauge (design 4.14) ----
 * Drawn in `rect` (the shorter side is the diameter). Track fill.control; the
 * value arc from 12 o'clock clockwise with round caps in `rgb`; stroke 6..14
 * points. per_mille 0..1000 eases on the `smooth` spring (and requests frames
 * only while it moves). value (title2 17 / 600, tabular) and caption (footnote
 * label.secondary) are centred; either may be NULL. The painted shape stays
 * inside `rect`, damage included. */
void aui_ring_gauge(struct aui_rect rect, int per_mille, int stroke, unsigned rgb,
                    const char *value, const char *caption);

/* ---- sparkline (design 4.14) ----
 * One 1.5 pt PATH stroke (round joins) over a LINEAR fill from `rgb` @ 77 at the
 * top to @ 0 at the bottom. samples[0..n) are drawn oldest -> newest across the
 * rect, on a 0..max scale (aui_sparkline_range: lo..hi). The newest point eases
 * in over ~120 ms (the snappy spring) and requests frames only while it moves;
 * the line is inset 5 points so its damage (the stroke's halo) stays inside `rect`. */
void aui_sparkline(struct aui_rect rect, const int *samples, int n, unsigned rgb);
void aui_sparkline_range(struct aui_rect rect, const int *samples, int n, unsigned rgb, int lo, int hi);

/* ---- multi-segment bar (memory, storage; design 4.14) ----
 * h 10, radius 5, segments with 2 DEVICE pixel gaps on a fill.control track.
 * Segment widths are proportional to `value` of the sum (aui_multibar_total:
 * of `total`, the rest stays track). The legend sits below: 8 x 8 radius-2
 * swatches, footnote labels, wrapped to `w`; it returns the height it used. */
struct aui_seg {
    int value;
    unsigned rgb;
    const char *label;        /* legend text (the caller formats numbers with logit_fmt.h) */
};
#define AUI_MULTIBAR_H 10
void aui_multibar(struct aui_rect rect, const struct aui_seg *segs, int n);
void aui_multibar_total(struct aui_rect rect, const struct aui_seg *segs, int n, int total);
int  aui_multibar_legend(int x, int y, int w, const struct aui_seg *segs, int n);
/* The device-pixel span [x0, x1) of segment i inside the bar (for tests and
 * callers that overlay hit areas); 0 for a segment of no value. */
int  aui_multibar_span(struct aui_rect rect, const struct aui_seg *segs, int n, int total,
                       int i, int *x0, int *x1);

#endif /* AUI_DATA_H */
