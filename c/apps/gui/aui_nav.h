/* aui_nav.h -- floating sidebar, toolbar, split view, disclosure, inspector
 * (design_language.md 4.1, 4.2, 4.17, 4.18 and signature 6.4; ui-aui-nav).
 * Included by aui.h (apps include only aui.h); it relies on logit.h and
 * struct aui_rect from there. Every rect is in points in the current drawing
 * coordinates, like every other aui widget. Faces and symbols come from the
 * 「澄」 tokens (aui_tok()) and the GICON_* set; nothing here calls the
 * button or field modules.
 *
 * MOTION. Every moving part (the selection pill, the sidebar width, a split
 * divider, a disclosure, the inspector) rides a design 3.8 spring preset
 * (LOGIT_SPRINGS), settles by the design's rule (|dx| < 0.25 pt and
 * |v| < 0.5 pt/s), then stops asking for frames; Reduce Motion snaps. Keep
 * the aui_anim_due()/aui_anim_wait() loop of aui_motion.h. */
#ifndef AUI_NAV_H
#define AUI_NAV_H

/* SYS_GUI2_INFO bit 11: the WM keeps a cached blur of the desktop under a
 * backdrop region (design P9 C2). ui-wm-abi reserves this exact value in
 * wave 2 and the WM advertises it in wave 4; defined here when the ABI header
 * does not have it yet, so the glass path compiles and is host-tested now. */
#ifndef LOGIT_GUI2_F_BACKDROP_CACHE
#define LOGIT_GUI2_F_BACKDROP_CACHE (1u << 11)
#endif

/* 1 when this window has the keyboard (EV_WINDOW_FOCUS in the frame that
 * delivers it, otherwise SYS_GUI_WIN_STATE WINS_FOCUSED, asked once per
 * frame). The sidebar pill, its material and the toolbar follow it. */
int aui_nav_focused(void);

/* ---------------------------------------------------------- 4.1 sidebar --
 *
 *     aui_sidebar_begin(1, aui_r(8, 0, 220, H - 8), 0);
 *     struct aui_rect s = aui_sidebar_search_slot();   // optional: (12,10,SW-24,28)
 *     aui_textfield(s.x, s.y, s.w, q, sizeof q);        // the app draws the field
 *     aui_sidebar_section("个人收藏", 1);
 *     if (aui_sidebar_row(GICON_HOME, "主目录", 0, page == 0, 0)) page = 0;
 *     if (aui_sidebar_row(GICON_DOWNLOAD, "下载", 3, page == 1, 0)) page = 1;
 *     aui_sidebar_end();
 *     int content_x = 8 + aui_sidebar_width(1) + 8;
 *
 * The container is the floating panel of 4.1: radius 12, a 1 dev hairline all
 * round, E1 in light and the dark rim in dark. Its material is BACKDROP id 1
 * (LOGIT_MAT_SIDEBAR) when the window is focused and SYS_GUI2_INFO reports
 * LOGIT_GUI2_F_BACKDROP_CACHE, otherwise the bg.sidebar solid, crossfaded over
 * 120 ms; Reduce Transparency and AUI_SB_SOLID are always the solid. Apps do
 * not track window drags: the WM shows a backdrop region as its fill_rgb (the
 * same solid) while it drags or resizes the window.
 *
 * `r.w` is the initial width; the 6 pt to the right of the panel is a divider
 * the user drags (180..280, live), and on release the width springs to the
 * nearest 4 pt (snappy). aui_sidebar_width(id) answers the live width (0 for
 * an id never begun) so the content can follow it.
 *
 * Rows are recorded as they are called and drawn by aui_sidebar_end(), under
 * ONE selection pill that glides between rows (signature 6.4). `selected` is
 * the app's state; a row returns AUI_SB_PICK when the user picks it (press,
 * ↑/↓, type-to-select with an 800 ms prefix buffer) and AUI_SB_OPEN on ⏎, and
 * the pill moves to a picked row in that same frame. `label` is copied (63
 * bytes, cut on a UTF-8 boundary), so a reused buffer is fine. An armed
 * aui_aim_prefix / aui_aim_names publishes one aim per visible row (prefix-i
 * or names[i], i counting rows from 0) once the pill is at rest.
 *
 * Sections: a `footnote` 12/600 header. A collapsible one shows a chevron on
 * hover; clicking it collapses its rows (height on `smooth`, the chevron
 * turning -90 degrees on `snappy`). Keep drawing the rows of a collapsed
 * section: aui hides them. aui_sidebar_section returns 1 while it is open. */
#define AUI_SB_SOLID     0x0001u      /* aui_sidebar_begin: never a backdrop */
#define AUI_SB_TINT      0x0002u      /* symbols in accent on every row (Finder, 7.1) */

#define AUI_SB_ROW_OFF     0x0001u    /* aui_sidebar_row: disabled */
#define AUI_SB_ROW_ACTION  0x0002u    /* an action ("新建项目"): label.tertiary ink, picking it
                                       * does not move the pill */

#define AUI_SB_NONE      (-1)         /* icon_or_chip: no symbol */
/* A 20 pt Settings chip (3.7.2): a squircle of `rgb` with a white symbol. */
#define AUI_SB_CHIP(icon, rgb) (0x40000000 | (((icon) & 0x3F) << 24) | ((rgb) & 0xFFFFFF))

#define AUI_SB_PICK      1            /* aui_sidebar_row result */
#define AUI_SB_OPEN      2

/* Metrics (4.1). */
#define AUI_SB_INSET     8            /* panel inset from the window edge */
#define AUI_SB_RADIUS    12
#define AUI_SB_W_DEFAULT 220
#define AUI_SB_W_MIN     180
#define AUI_SB_W_MAX     280
#define AUI_SB_DIVIDER   6            /* divider hit zone right of the panel */
#define AUI_SB_PITCH     30
#define AUI_SB_PILL_INSET 6
#define AUI_SB_PILL_H    28
#define AUI_SB_PILL_R    6

void aui_sidebar_begin(int id, struct aui_rect r, unsigned flags);
struct aui_rect aui_sidebar_search_slot(void);
int  aui_sidebar_section(const char *title, int collapsible);
int  aui_sidebar_row(int icon_or_chip, const char *label, int count, int selected, unsigned flags);
void aui_sidebar_end(void);
int  aui_sidebar_width(int id);

/* ---------------------------------------------------------- 4.2 toolbar --
 *
 *     aui_toolbar_begin(aui_r(x, 0, w, 38), aui_tok()->bg_content, 1);
 *     aui_tb_group_begin(2);                       // back | forward
 *       if (aui_tb_icon(GICON_CHEVRON_L, 0)) back();
 *       if (aui_tb_icon(GICON_CHEVRON_R, 0)) fwd();
 *     aui_tb_group_end();
 *     aui_tb_title("下载", "12 项");
 *     struct aui_rect sr = aui_tb_trailing(180);   // the app's search field
 *     aui_toolbar_end();
 *
 * Mode A band: height 38 (band.h <= 0 takes it), filled with the app's header
 * token, controls 28 high at y 5, insets 12, 8 between items and 16 between
 * groups (aui_tb_gap starts a new group). No bottom line at rest: a
 * scroll-edge hairline fades in over 90 ms while aui_scroll_offset(scroll_id)
 * is above 0 (scroll_id <= 0: never). Unfocused, the items render as ONE
 * opacity group at 85 % with symbols in label.tertiary.
 *
 * A group (aui_tb_group_begin(n)) is a Secondary capsule of n 28 pt segments
 * split by 1 dev hairlines inset 6; inside it aui_tb_item and aui_tb_icon take
 * the next segment and ignore their width. aui_tb_icon is the 4.3 Icon button
 * (28 x 28, symbol 16); AUI_TB_ON is the toggle look, AUI_TB_TRAIL places it
 * from the right like aui_tb_trailing. It returns 1 on activation and takes an
 * aui_aim_prefix arming as its aim. */
#define AUI_TB_H       38
#define AUI_TB_CTL_Y   5
#define AUI_TB_CTL_H   28
#define AUI_TB_INSET   12
#define AUI_TB_SEG_W   28           /* the 7.1 capsule [< | >] is 56 wide */

#define AUI_TB_OFF     0x0001u
#define AUI_TB_ON      0x0002u
#define AUI_TB_TRAIL   0x0004u

void aui_toolbar_begin(struct aui_rect band, unsigned header_rgb, int scroll_id);
struct aui_rect aui_tb_item(int w);
void aui_tb_gap(void);
struct aui_rect aui_tb_trailing(int w);
void aui_tb_title(const char *title, const char *subtitle);
void aui_tb_group_begin(int n);
void aui_tb_group_end(void);
int  aui_tb_icon(int icon, unsigned flags);
void aui_toolbar_end(void);

/* ------------------------------------------------- 4.17 split, disclosure --
 * aui_split: a 1 dev sep.opaque divider at *pos points from r's left edge
 * (vertical = 1: a vertical line between left and right panes) or top edge
 * (vertical = 0), with an 8 pt hit zone. Dragging moves it live, clamped to
 * [lo, hi]; on release it springs to the nearest 4 pt (snappy). Returns 1 in
 * a frame in which *pos changed.
 *
 * aui_disclosure: a 28 pt row taken from the current layout stack (aui_row,
 * AUI_FILL wide) with a CHEVRON_R 10 in label.tertiary that turns 90 degrees
 * on snappy and the label in body; a press (or Enter / Space when focused)
 * flips *open and returns 1. aui_disclosure_at is the same in an explicit
 * rect. aui_disclosure_h answers the height the content should have this
 * frame, 0..full_h on smooth: clip the content to it. */
int aui_split(int id, struct aui_rect r, int *pos, int lo, int hi, int vertical);
int aui_disclosure(int id, const char *label, int *open);
int aui_disclosure_at(int id, struct aui_rect r, const char *label, int *open);
int aui_disclosure_h(int id, int full_h);

/* ---------------------------------------------------- 4.18 inspector -----
 *
 *     int iw = aui_inspector_reflow_w(2);          // content to the left reflows
 *     ...content in width W - iw...
 *     if (aui_inspector_begin(2, aui_r(W - 260, 38, 260, H - 38), shown)) {
 *         aui_inspector_group("信息");
 *         aui_row(&r, AUI_FILL, 20); ...
 *         aui_inspector_end();
 *     }
 *
 * `r` is where the panel sits when shown: flush right, from the toolbar
 * bottom down. It slides in and out on `smooth`; bg.inspector with a 1 dev
 * sep.opaque left edge. Between begin (when it returns 1) and end, (0, 0) is
 * the panel's top-left, drawing is clipped to it, and a vertical layout stack
 * is open at padding 16 with an 8 gap: aui_inspector_group draws a `headline`
 * title (24 between groups, 8 below). aui_inspector_reflow_w answers the
 * width the panel covers this frame (0 when hidden), from the same spring. */
int  aui_inspector_begin(int id, struct aui_rect r, int shown);
int  aui_inspector_group(const char *title);
void aui_inspector_end(void);
int  aui_inspector_reflow_w(int id);

#endif /* AUI_NAV_H */
