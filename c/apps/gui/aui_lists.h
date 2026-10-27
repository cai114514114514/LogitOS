/* aui_lists.h -- lists, tables, tabs, segmented control.
 * Included by aui.h (apps include only aui.h); it relies on logit.h and
 * struct aui_rect / enum aui_variant from there. Declarations moved
 * verbatim from aui.h (ui-aui-core). */
#ifndef AUI_LISTS_H
#define AUI_LISTS_H

/* Lists and tables. `items` is an array of `n` NUL-terminated strings; `cols`
 * an array of `ncol` header names with `widths` in points. Returns the row
 * index that was activated (clicked or Enter'd), or -1. Arrow keys move the
 * selection when the list has focus. */
int aui_list(int x, int y, int w, int h, const char *const *items, int n,
             int *sel, int *scroll);
int aui_table(int x, int y, int w, int h, const char *const *cols,
              const int *widths, int ncol,
              const char *const *cells, int nrow, int *sel, int *scroll);

/* Tabs / segmented control: `*sel` is the chosen index. Returns 1 on change. */
int aui_tabs(int x, int y, int w, const char *const *items, int n, int *sel);
int aui_segmented(int x, int y, int w, int h, const char *const *items, int n, int *sel);

#endif /* AUI_LISTS_H */
