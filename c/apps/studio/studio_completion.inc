/* SPDX-License-Identifier: MIT */
/* Drawing and mouse input must use the same rectangle, including the rows
 * scrolled into view by the keyboard. Otherwise a click can accept a different
 * symbol, or fall through and move the caret underneath the popup. */
typedef struct {
    int x, y, width, rows, first;
} StCompletionPopup;

static int completion_popup(StCompletionPopup *popup)
{
    const StDocument *d = document();
    if (!state->completion_count || !d || dialog || context_open) {
        return 0;
    }

    int row = st_caret_line(d);
    popup->width = 230;
    popup->rows = state->completion_count < 6 ? state->completion_count : 6;
    popup->first = completion_selected >= popup->rows ? completion_selected - popup->rows + 1 : 0;
    popup->x = edit_x + 54 + line_width(d, st_line_start(d, row), d->caret) - d->left;
    popup->y = edit_y + (row - d->top + 1) * ST_ROW;
    if (popup->x + popup->width > work_x) {
        popup->x = work_x - popup->width;
    }
    if (popup->x < edit_x) {
        popup->x = edit_x;
    }
    if (popup->y + popup->rows * ST_ROW > out_y) {
        popup->y -= ST_ROW * (popup->rows + 1);
    }
    if (popup->y < edit_y) {
        popup->y = edit_y;
    }
    return 1;
}

static void draw_completion(void)
{
    StCompletionPopup popup;
    if (!completion_popup(&popup)) {
        return;
    }
    gui_rect(popup.x, popup.y, popup.width, popup.rows * ST_ROW, AUI_SURFACE_2);
    for (int row = 0; row < popup.rows; row++) {
        int index = popup.first + row;
        int y = popup.y + row * ST_ROW;
        if (index == completion_selected) {
            gui_rect(popup.x, y, popup.width, ST_ROW, AUI_SELECTION);
        }
        aui_text_ellipsis(popup.x + 8, y + 3, popup.width - 16, state->completions[index].label,
                          AUI_TEXT, 13);
    }
}

/* Return true only when the popup consumes the press. An outside click still
 * reaches its normal control after dismissing completion and its pending timer. */
static int click_completion(int x, int y)
{
    StCompletionPopup popup;
    completion_due = 0;
    if (completion_popup(&popup) && x >= popup.x && x < popup.x + popup.width && y >= popup.y &&
        y < popup.y + popup.rows * ST_ROW) {
        int index = popup.first + (y - popup.y) / ST_ROW;
        st_engine_accept_completion(engine, index);
        focus = 0;
        reveal(document());
        return 1;
    }
    st_engine_dismiss_completion(engine);
    return 0;
}
