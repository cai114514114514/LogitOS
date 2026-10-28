/* aui_form.h -- form groups and rows (design 4.16, 3.5, 3.4).
 * Included by aui.h (apps include only aui.h); it relies on struct aui_rect. */
#ifndef AUI_FORM_H
#define AUI_FORM_H

/* ---- the grouped form (Settings panes, popovers, sheets) ----
 *
 *     struct aui_rect col = aui_r(20, y, w - 40, 0);          // the content column
 *     col.h = aui_form_h(2, 1);                                // 2 plain rows + 1 subtitled
 *     aui_form_group_begin("外观", &col);
 *       struct aui_rect r = aui_form_row("深色模式", 0);
 *       aui_toggle(aui_form_row_switch_x(r), r.y + (r.h - 22) / 2, &dark, 1);
 *       ...
 *     aui_form_group_end();                                    // col.y moves past the card + 12
 *     aui_form_help("跟随系统外观自动切换。");                     // optional, under the group
 *
 * Immediate mode cannot know a card's height before its rows are drawn, and the
 * card must be painted first, so the caller states it: col->h is the card's
 * height (aui_form_h(n44, n56) for plain / subtitled rows). Rows draw past a
 * card that was declared too short; they do not move.
 *
 * The card is bg.card, r 12 (a squircle PATH: Squircles only for opaque PATH
 * fills), E1 in light and the 1 dev rim in dark, width = col->w. A label (NULL
 * for none) is footnote 12 / 600 label.secondary, 8 above the card at x + 4.
 * Rows are 44 pt (56 with a subtitle), padding x 16, separators sep.hairline from
 * x + 16 to the right edge between rows. Group begin/end do not nest. */
#define AUI_FORM_ROW_H      44
#define AUI_FORM_ROW_SUB_H  56
#define AUI_FORM_PAD_X      16
#define AUI_FORM_R          12          /* r.lg */
#define AUI_FORM_SWITCH_W   38
#define AUI_FORM_LABEL_H    16          /* footnote line */
#define AUI_FORM_GAP        12          /* between cards */
#define AUI_FORM_KV_LABEL_W 112         /* label-column variant */
#define AUI_FORM_KV_VALUE_X 128

int  aui_form_h(int rows44, int rows56);            /* card height for that many rows */
void aui_form_group_begin(const char *label, struct aui_rect *col);
/* One row of the open group: draws its title (body 14 label.primary) and, if
 * subtitle is non-NULL, the subtitle (footnote label.secondary, 2 below). The
 * return value is the ROW rect (x, y, w, 44 or 56); the control goes in the
 * slot aui_form_slot() cuts from it. */
struct aui_rect aui_form_row(const char *title, const char *subtitle);
/* ...reserving ctl_w points on the right (plus a 12 gap) so a long title elides
 * before it reaches the control. */
struct aui_rect aui_form_row_ex(const char *title, const char *subtitle, int ctl_w);
/* The control slot of a row: w x h, right-aligned at inset 16, vertically centred. */
struct aui_rect aui_form_slot(struct aui_rect row, int w, int h);
int  aui_form_row_switch_x(struct aui_rect row);    /* row.x + row.w - 16 - 38 */
/* Label-column row: the label right-aligned in a 112 column (callout
 * label.secondary), the value from x + 128 (callout label.primary, tail-elided). */
struct aui_rect aui_form_kv(const char *label, const char *value);
void aui_form_group_end(void);
/* footnote label.tertiary, 8 under the group just ended, at most 2 lines; col.y
 * moves below it (plus the 12 gap). */
void aui_form_help(const char *text);

/* ---- empty state (design 4.13) ----
 * Centred in `region` with the visual centre of the block at 42 % of its
 * height: a 64 squircle tile (identity colour @ 36) holding a 32 symbol, a title2
 * title, a callout body (max 300 wide, centred) and at most one Primary and one
 * Secondary button (either may be NULL). `icon` is a GICON_* id.
 * Returns 0, 1 (primary clicked) or 2 (secondary clicked). */
int aui_empty_state(struct aui_rect region, int icon, unsigned identity_rgb, const char *title,
                    const char *body, const char *primary, const char *secondary);
/* The block's height for those arguments (what aui_empty_state centres). */
int aui_empty_state_h(const char *body, const char *primary, const char *secondary);
/* ...and where it puts it: x/w of the region, y/h of the block (its centre is the
 * region's 42 % line). */
struct aui_rect aui_empty_state_rect(struct aui_rect region, const char *body, const char *primary,
                                     const char *secondary);

#endif /* AUI_FORM_H */
