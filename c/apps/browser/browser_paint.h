#ifndef BROWSER_PAINT_H
#define BROWSER_PAINT_H

struct node;

/* Paint the current layout display list into the window viewport (vx,vy,vw,vh)
 * at the given pixel scroll, using the GUI render syscalls. */
void browser_paint(int vx, int vy, int vw, int vh, int scroll);

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

/* The same hit test, but resolving to the DOM. Finds the topmost painted box
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
