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
