#ifndef LOGIT_FOCUS_H
#define LOGIT_FOCUS_H

/* focus.h -- the document's focused element, and how the keyboard reaches it.
 *
 * WHY THIS FILE EXISTS. Until it did, browser.c line 1860 read
 *
 *     /-* No focus model yet, so keys go to <body> ... *-/
 *
 * and that one comment was the whole of the reason this machine could not type
 * into a single web page. Not "text fields render badly" -- there was nowhere
 * for a keystroke to GO. Every search box, login form and comment field on the
 * web was inert, and no amount of layout or paint work could change that,
 * because the missing piece was a pointer: which element owns the keyboard.
 *
 * WHAT IT IS. One pointer (plus the serial that validates it) and the rules
 * around it: who may hold it (`focus_is_focusable`), how it moves on Tab
 * (`focus_advance`), what happens to the old holder, and which events fire in
 * which order. Deliberately NOT in forms.c: focus is a document concept, not a
 * form-control one -- a link, a `tabindex` div and a contenteditable all take
 * focus, and none of them is a form control.
 *
 * THE DISPATCH SEAM. focus.c raises DOM events but cannot include js_dom.h:
 * that header pulls in quickjs.h, and this file is compiled into BROWSER_PIPE
 * where the QuickJS include path is not on the command line. So the embedder
 * INSTALLS the dispatcher (`focus_set_dispatch`) and focus.c calls through a
 * pointer. Three things fall out of that and all three are wanted:
 *
 *   - the events line owns event construction, in their file, not this one;
 *   - a host test installs a recorder and asserts the exact event ORDER,
 *     which is the part of focus that specs pin down and implementations get
 *     wrong;
 *   - with no dispatcher installed (a page with no script runtime) focus still
 *     works, because moving focus is not a thing a page is allowed to veto.
 */

struct node;

/* The dispatcher the embedder installs. Returns 1 if the default action should
 * proceed (i.e. nothing called preventDefault); focus events are not
 * cancelable, so the return is ignored for those and honoured for the editing
 * events forms.c raises through the same seam. */
typedef int (*fc_dispatch_fn)(struct node *target, const char *type,
                              int bubbles, int cancelable);
void fc_set_dispatch(fc_dispatch_fn fn);
/* The installed dispatcher, or a no-op that returns 1. forms.c raises
 * beforeinput/input/change through the same seam rather than opening a second
 * one. */
int  fc_dispatch(struct node *target, const char *type, int bubbles, int cancelable);

/* ---- the editing events, which need one more field than the seam above ----
 *
 * `input` and `beforeinput` carry an **inputType**, and that is not decoration:
 * a React-managed composer reads it to decide what happened, so an `input`
 * event without one is an input event about nothing. The base seam cannot carry
 * it -- js_dom.c builds every event it raises out of `struct js_event_init`,
 * which has no inputType, and that file belongs to another line. So the
 * embedder installs a SECOND dispatcher that can, and js_forms.c implements it
 * by constructing a real `InputEvent` in the page's own runtime (js_events.c
 * already defines the interface, with `inputType` and `data`).
 *
 * UNINSTALLED IT STILL FIRES, through fc_dispatch() with the inputType dropped
 * -- which is what BROWSER_PIPE and a host test that only wants the ORDER see.
 * That fallback is deliberate and it is also why this is a second pointer
 * rather than a wider first one: nothing that works today stops working when
 * the richer dispatcher is absent.
 *
 * `data` is InputEvent.data: the inserted string for an insertion, NULL for a
 * deletion. */
typedef int (*fc_dispatch_input_fn)(struct node *target, const char *type,
                                    const char *input_type, const char *data,
                                    int bubbles, int cancelable);
void fc_set_dispatch_input(fc_dispatch_input_fn fn);
int  fc_dispatch_input(struct node *target, const char *type,
                       const char *input_type, const char *data,
                       int bubbles, int cancelable);

/* ---------------- the focused element ---------------- */

/* Forget everything. Called on navigation, BEFORE the old DOM is freed --
 * the stored pointer would otherwise dangle into a recycled node slot. */
void focus_reset(void);

/* document.activeElement, or NULL when the body has it (the DOM says
 * activeElement is <body> in that case; the caller resolves that, because only
 * it knows the document). The serial is re-checked on every read, so a node
 * destroyed by a script cannot be handed back. */
struct node *focus_current(void);

/* Move focus to `n` (NULL = clear). Fires, in this order and only when the
 * focused element actually CHANGES:
 *
 *     blur     at the old element   (does not bubble)
 *     focusout at the old element   (bubbles)
 *     focus    at the new element   (does not bubble)
 *     focusin  at the new element   (bubbles)
 *
 * That is the order the UI Events spec gives and the order jQuery, React and
 * every focus-trap library are written against. Returns 1 if focus moved.
 *
 * A non-focusable `n` is NOT an error and does NOT clear focus silently: it
 * blurs, exactly like clicking on a paragraph does. Pass NULL for that
 * explicitly; pass a node and get the spec's answer for that node.
 */
int  focus_set(struct node *n);

/* Same, but never raises events -- for restoring focus after a re-layout, and
 * for tests. */
void focus_set_quiet(struct node *n);

/* Is `n` able to hold focus at all? Disabled controls, hidden subtrees and
 * elements with no focusable behaviour answer 0. */
int  focus_is_focusable(struct node *n);

/* The element's tabindex as the spec's THREE-way answer, because two of the
 * three are the interesting ones:
 *
 *     >  0   explicit order, visited before the tabindex-0 group
 *     == 0   in document order (the default for a control or a link)
 *     <  0   focusable programmatically, NOT reachable by Tab -- tabindex="-1"
 *            is how every modal, menu and roving-tabindex widget works, and
 *            treating it as "not focusable" is the classic wrong answer.
 *
 * `*explicit_out` (may be NULL) says whether a tabindex ATTRIBUTE was present,
 * which is what distinguishes a <div tabindex="0"> from a <p>.
 */
int  focus_tab_index(struct node *n, int *explicit_out);

/* Sequential focus navigation: the element Tab (back == 0) or Shift+Tab
 * (back != 0) moves to, starting from `from` (NULL = start of document).
 * Wraps. NULL when the document holds nothing focusable.
 *
 * Order is the spec's: the positive-tabindex group first in ascending index
 * (ties in document order), then everything at tabindex 0 in document order.
 * Negative tabindex is skipped entirely -- that is what negative MEANS. */
struct node *focus_next(struct node *root, struct node *from, int back);

/* focus_next + focus_set. 1 if focus moved. */
int  focus_advance(struct node *root, int back);

/* How many elements Tab can reach. Only for tests and diagnostics. */
int  focus_tabbable_count(struct node *root);

#endif /* LOGIT_FOCUS_H */
