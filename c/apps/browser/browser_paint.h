#ifndef BROWSER_PAINT_H
#define BROWSER_PAINT_H

struct node;
struct item;
/* Prefix advance uses the same spacing segmentation as native glyph paint. */
int browser_text_run_advance(const struct item *item, int byte_offset);

/* Paint the current layout display list into the window viewport (vx,vy,vw,vh)
 * at the given pixel scroll, using the GUI render syscalls. */
void browser_paint(int vx, int vy, int vw, int vh, int scroll);
struct node;
int browser_content_width(struct node *root, int viewport_width);
/* Two-axis page origin; viewport clip remains at vx/vy. */
void browser_paint_scroll(int vx, int vy, int vw, int vh, int scroll_x, int scroll_y);
/* Viewport-local x/y, explicit scroll offsets. Modal layers are viewport-fixed
 * and backdrop hits target the dialog (never an underlying link/control). */
int browser_hittest_node_scroll(int x, int y, int scroll_x, int scroll_y,
                               struct node **node, char *href, int max);
/* Input geometry after the same inner-scroll and pure-translation projection
 * used by paint. vx/vy are the actual paint origin. Coordinates use
 * viewport+pageScroll, including fixed/top-layer items, so existing caret,
 * selection and popup callers subtract page scroll once.
 * The complete border box is retained; this does not crop it to its clip.
 * Scale/rotation and translated overflow-ancestor clipping are not extended. */
void browser_input_item_geometry(const struct item *source, int vx, int vy,
                                  int scroll_x, int scroll_y, struct item *out);
/* x/y are viewport-local; vx/vy must match paint, preserving fractional-edge
 * rounding in actual window coordinates. The older scroll entry uses (0,0). */
int browser_hittest_node_viewport(int vx, int vy, int x, int y,
                                  int scroll_x, int scroll_y,
                                  struct node **node, char *href, int max);

/* Native affordance in an unrendered iframe; caller supplies the trusted
 * topmost hit target. No DOM event API calls this default action. */
int browser_frame_open_hit(int x,int y,int scroll_x,int scroll_y,const struct node *node);

/* ---- WHAT CHANGED, for gui_flush_rect --------------------------------------
 *
 * The union of every item whose on-screen appearance differed from the LAST
 * browser_paint() pass, in WINDOW coordinates -- ready to hand straight to
 * gui_flush_rect(). See the block comment above pd_item_sig() in
 * browser_paint.c for the method (a positional diff against the previous
 * pass, not a second DOM walk) and why it is the one place this decision is
 * made: a caller that recomputed its own idea of "what moved" would be
 * exactly the one-jar-two-doors trap CLAUDE.md warns about, applied to
 * pixels.
 *
 * Returns -1 if no honest answer is available (first paint, the viewport
 * itself moved/resized since the last pass, or the snapshot buffers could
 * not grow) -- the caller must gui_flush() the whole canvas. Returns 0 if a
 * rect WAS computed and it is empty -- nothing actually changed, and the
 * caller may skip flushing entirely. Returns 1 and fills *x,*y,*w,*h with a
 * real, nonempty, viewport-clamped rectangle otherwise.
 *
 * Valid only immediately after a browser_paint() call for the SAME viewport
 * the caller is about to flush -- it answers for the pass that just ran, not
 * a general "what is dirty right now" query. */
int browser_paint_dirty_rect(int *x, int *y, int *w, int *h);

/* ---- WHAT WORDS REACHED THE SCREEN --------------------------------------
 *
 * The site scoreboard's own header states the gap this closes, and states it
 * as a limitation it cannot fix: "`changed px` counts pixels that differ from
 * an empty tab ... It cannot tell a rendered page from a flat dark block.
 * Nothing here checks whether the RIGHT pixels changed -- that is what
 * reftests are for, and none of WPT's 17,155 of them run on this machine."
 *
 * Reftests are the right answer to "is the layout correct" and they are a
 * long way off. This is the cheap middle: not WHERE the pixels are, but WHICH
 * WORDS are among them. It cannot judge a layout and does not try. It answers
 * the question every BLANK and ERRORS row on that scoreboard is really
 * asking -- did the text appear at all -- which today is answered by a person
 * squinting at a PNG.
 *
 * Measured on bilibili the day this was written: the page paints 267,376
 * changed pixels and scores PAINTED, and its video cards are thumbnails above
 * an EMPTY grey rectangle where every title should be. No exception, no failed
 * request, no missing subresource. Nothing in the record said so.
 *
 * Every browser_paint() pass records the text runs it emits; the record is
 * whole-pass, so the last paint wins and a partial repaint does not leave a
 * mixture. Bounded and honest about it: past the cap the counts keep counting
 * and the bytes stop being kept, and the dump says which happened. */
void browser_paint_text_dump(void);

/* Arm the automatic one-line-per-change record. OFF by default, because this
 * TU is linked by host harnesses that render pages and are not browsers, and
 * an instrument that writes into the output of the thing it measures has
 * replaced it. browser.c arms it; nothing else should. */
void browser_paint_text_log(int on);

/* Find-in-page, over the SAME record browser_paint_text_dump() prints -- not a
 * second DOM walk. One traversal answers about:text, about:boxes-adjacent
 * diagnostics and this; a second walk here is exactly the one-jar-two-doors
 * trap this tree has paid for three times (CLAUDE.md), and the two walks
 * would disagree about which words are on screen -- the one fact the whole
 * diagnostic apparatus rests on.
 *
 * SCOPE, STATED RATHER THAN DISCOVERED: the record is PAINTED text, i.e. only
 * what the last paint actually put on screen -- browser_paint's own culling
 * skips a run whose box is entirely outside the viewport, so a match that is
 * scrolled out of view is not in the record and this cannot find it. This is
 * therefore "does the needle appear anywhere CURRENTLY VISIBLE", not
 * whole-document find; the caller must say so in the status line rather than
 * imply a full-document search that was not done (rule 5: a control that
 * cannot be watched failing is worse than no control).
 *
 * Case-insensitive substring match. Returns the number of RUNS containing at
 * least one occurrence (not the occurrence count -- a run is a paragraph's
 * worth of text in one style, and "3 runs" is what a person reading the
 * status line means by "3 matches" more often than "3 occurrences" does). */
int browser_paint_text_find(const char *needle);

/* The DevTools chain panel's "text painted" link -- the same g_ptx_runs /
 * g_ptx_chars browser_paint_text_dump() prints, so the panel cannot report a
 * different number than the serial console does for the one question that
 * separates a page that RENDERED from a page that just did not error:
 * stripe.com went 69 painted text runs -> 38 -> 0 with no failed request and
 * no missing subresource, and this is the counter that showed it. Either
 * pointer may be NULL. */
void browser_paint_text_counts(int *runs, int *chars);

/* ---- WHAT THE PAINTER REFUSED, counted ----------------------------------
 *
 * Three visual features landed with a named, measured limit rather than an
 * approximation, and a limit nobody can query is indistinguishable from a bug
 * in whatever the page put there. Same reasoning as gfx_mask_refused(): a
 * degradation is acceptable only once it is a number someone can read.
 *
 * Both are cumulative for the process's life and never reset -- a harness
 * takes a reading before and after, exactly as gfx_mask_stats() is used.
 *
 *   inset_blur_skipped   blurred `inset` box-shadows not painted. The inward
 *                        falloff needs a tile whose ramp runs the other way,
 *                        and inverting GFX_MASK_SHADOW does not produce it
 *                        (255*(2t-t^2) against the 255*t^2 wanted). 22 of the
 *                        320 literal shadows in tests/fixtures/cssweb.
 *   blur_tightened       shadows whose blur was shrunk to fit the engine's
 *                        largest corner tile. A tighter shadow, never a
 *                        missing one.
 *   xf_unbounded         rotated/skewed boxes whose transformed bounding box
 *                        was past XF_MAX device pixels a side; drawn at that
 *                        bounding box, unrotated.
 *   xf_unrotated         non-rect items (text, images) under a rotation:
 *                        placed at their transformed box and drawn level,
 *                        because the ABI has no rotated text or blit call.
 *   rclip applied/refused
 *                        boxes painted through the ROUNDED overflow clip
 *                        (gfx_fill_mask_clipped), and boxes whose clipper's
 *                        radius was past the same bound and fell back to the
 *                        rectangular clip. `applied` is the useful one in the
 *                        other direction: it is how a gate proves the path
 *                        clip RAN rather than that a clipped box painted.
 */
void browser_paint_shadow_stats(int *inset_blur_skipped, int *blur_tightened);
void browser_paint_xform_stats(int *xf_unbounded, int *xf_unrotated);
void browser_paint_rclip_stats(int *applied, int *refused);

/* Hit-test a viewport-local point; on a link, copy its href (NUL-terminated)
 * into buf (<= max) and return 1, else 0. */
int  browser_hittest(int x, int y, int scroll, char *buf, int max);

/* The same hit test, but resolving to the DOM. Finds the topmost painted box or inline whitespace region
 * containing the point and reports BOTH what an event needs (`*node`, the
 * element the box came from -- a text box resolves to its parent element) and
 * what the default action needs (`href`, the link target, "" if none).
 *
 * The two are separate on purpose: a click inside <a><span>x</span></a> targets
 * the <span> for dispatch but navigates the <a>'s href, and a click on a plain
 * <button> has a target and no href at all. Returns 1 if any box was hit. */
int  browser_hittest_node(int x, int y, int scroll, struct node **node,
                          char *href, int max);

#endif /* BROWSER_PAINT_H */
