/* aui_lists.h -- lists, tables, tabs, segmented control, document tabs.
 * Included by aui.h (apps include only aui.h); it relies on logit.h and
 * struct aui_rect / enum aui_variant from there. Design 4.5, 4.8 and 4.11
 * (ui-aui-lists). The four-int / legacy signatures keep compiling; they take
 * the new look. */
#ifndef AUI_LISTS_H
#define AUI_LISTS_H

/* ---- lists and tables (design 4.8) ------------------------------------
 * Directly on bg.content: no outer box, no capsule header. A row is 28 pt
 * (aui_table_ex takes 26 for a dense table), text at x 12. The selection is
 * an inset pill (6 pt each side, row y + 1, h row - 2, r 6): solid accent
 * with onAccent text while the WINDOW has keyboard focus, fill.inactiveSel
 * with primary text otherwise. A pointer-hovered row gets a fill.hover pill.
 * The pill glides between rows on the snappy spring.
 *
 * WINDOW FOCUS. aui has no focus state of its own yet: a list widget keeps
 * one, set when a frame it draws sees EV_WINDOW_FOCUS (a = 1 gained, 0 lost),
 * and treats the window as focused until it has seen one.
 *
 * `items` is an array of `n` NUL-terminated strings; `cols` an array of `ncol`
 * header names with `widths` in points (the LAST column fills the rest).
 * Returns the row index that was activated (clicked or Enter'd), or -1.
 * Arrow keys move the selection when the list has focus. */
int aui_list(int x, int y, int w, int h, const char *const *items, int n,
             int *sel, int *scroll);
int aui_table(int x, int y, int w, int h, const char *const *cols,
              const int *widths, int ncol,
              const char *const *cells, int nrow, int *sel, int *scroll);

struct aui_col {
    const char *title;
    int width;          /* points; the last column ignores it and fills */
    unsigned flags;     /* AUI_COL_* */
};
#define AUI_COL_NUM    1u   /* numeric: tabular digits, right-aligned (header too) */
#define AUI_COL_NOSORT 2u   /* a click on its header does not sort */

/* The sort state the app owns. A click on a sortable header sets col and toggles
 * desc (a different column starts ascending) and sets `changed` for one frame
 * pass: the APP re-orders its rows; the widget slides them to their new y. */
struct aui_sort { int col; int desc; int changed; };

#define AUI_TBL_RANGE   1u  /* sel -> int[2] {anchor, extent}: Shift extends a merged block */
#define AUI_TBL_EMPTY   2u  /* the body is empty: header only; the app draws its empty state (4.13) over (x, y + 28, w, h - 28) */
#define AUI_TBL_NOHEAD  4u  /* a list: no header (cols may be NULL) */

/* aui_table_ex: the full table. `cells` is nrow * ncol strings, row-major;
 * `sort` may be NULL (no sorting); `row_h` 0 = 28 (Monitor passes 26). Columns
 * are resized by dragging the 6 pt handle between headers (writes cols[].width).
 * Zebra striping is automatic from 4 columns. Rows are identified by their
 * FIRST cell: re-ordering them slides each to its new y (smooth spring,
 * <= 30 rows; beyond that they snap), and removing one collapses its gap. */
int aui_table_ex(int x, int y, int w, int h, struct aui_col *cols, int ncol,
                 const char *const *cells, int nrow, int *sel, int *scroll,
                 struct aui_sort *sort, unsigned flags, int row_h);

/* ---- tabs / segmented control (design 4.5) ----------------------------
 * `*sel` is the chosen index. Returns 1 on change. A capsule track
 * (fill.control, h = the h given, 24 or 28) with a sliding thumb on the snappy
 * spring (white / #5A5A64 dark, elevation E1, never glass). Segments are equal,
 * max label + 24 wide (icons 36); w <= 0 hugs them, a larger w stretches them.
 * aui_tabs keeps its signature: a segmented control h 28, centred in w. */
int aui_tabs(int x, int y, int w, const char *const *items, int n, int *sel);
int aui_segmented(int x, int y, int w, int h, const char *const *items, int n, int *sel);
/* The same with a GICON_* symbol per segment (16 pt in a 36 pt segment). */
int aui_segmented_icons(int x, int y, int w, int h, const int *icons, int n, int *sel);

/* ---- document tabs (design 4.11) --------------------------------------
 * A 30 pt strip on bg.toolbar; the active tab is bg.content with r 8 top
 * corners, fused to the toolbar or editor below. `key` identifies a tab across
 * frames (0 = its index + 1): a new key's width springs open from 0, a key that
 * vanishes keeps its title and collapses (snappy). Up to 16 tabs. */
struct aui_doctab {
    const char *title;
    int icon;           /* GICON_*, or -1 for none (14 pt at x + 10) */
    unsigned flags;     /* AUI_DTAB_* */
    int key;
};
#define AUI_DTAB_LOADING 1u     /* a 12 pt spinner replaces the icon */
#define AUI_DTAB_UNSAVED 2u     /* a 6 pt dot that becomes the close button on hover */
#define AUI_DT_NO_PLUS  1u      /* flags: no new-tab button */
#define AUI_DT_NONE      0
#define AUI_DT_ACTIVATE 1       /* result action: tab clicked (*active already set) */
#define AUI_DT_CLOSE    2       /* the close button of tab INDEX: the app removes it */
#define AUI_DT_NEW      3       /* the + button */
#define AUI_DT_ACTION(r) ((r) & 0xFF)
#define AUI_DT_INDEX(r)  ((r) >> 8)
int aui_doctabs(int x, int y, int w, const struct aui_doctab *items, int n,
                int *active, unsigned flags);

#endif /* AUI_LISTS_H */
