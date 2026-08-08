#ifndef LOGIT_FORMS_H
#define LOGIT_FORMS_H

/* forms.h -- HTML form controls: what they are, what they hold, and how a
 * keystroke changes it.
 *
 * THREE FILES SHARE THIS HEADER AND THEY NEED DIFFERENT HALVES OF IT, which is
 * why the split below is the way it is:
 *
 *   layout.c    needs only fc_kind() and the metrics -- what KIND of control an
 *               element is and how big its box is. Both are pure functions of
 *               the DOM, so layout gains NO link dependency on forms.c. That is
 *               load-bearing: layout.c is linked by eight host test binaries
 *               (layout_test, page_test, paint_test, css_perf_test, ...) and
 *               every one of them would have to grow a source in the Makefile
 *               otherwise -- in a file that has been clobbered three times this
 *               week by concurrent sessions.
 *
 *   browser_paint.c needs the STATE -- the text in the box, the caret, the tick
 *               in the checkbox. It takes it through fc_paint_state(), which it
 *               declares __weak__ exactly as it already does media_paint_box().
 *               With forms.c absent the painter still draws the chrome and the
 *               host paint test still links.
 *
 *   browser.c / js_forms.c need all of it.
 *
 * VALUE vs. CONTENT ATTRIBUTE, said once here because every bug in this area
 * is a confusion between them. `<input value="a">` has a *default* value; the
 * element's *current* value starts as a copy of it and, once the user or a
 * script has touched it, stops tracking it. That is what `defaultValue` and the
 * "dirty value flag" are in the spec, and it is why forms.c keeps a value of
 * its own rather than reading the attribute every time: a form that repaints
 * after a re-style must not silently discard what the user typed. Same shape
 * for `checked` / `defaultChecked`, and for <option selected>.
 */

#include "dom.h"

/* ------------------------------------------------------------- the kinds -- */
/* Ordered so the FC_IS_* tests below are range checks. Do not reorder without
 * fixing them; there is no compile-time guard that would catch it. */
enum {
    FC_NONE = 0,
    /* textual: a caret lives in these */
    FC_TEXT, FC_PASSWORD, FC_SEARCH, FC_EMAIL, FC_URL, FC_TEL, FC_NUMBER,
    FC_TEXTAREA,
    /* toggles */
    FC_CHECKBOX, FC_RADIO,
    /* buttons */
    FC_SUBMIT, FC_RESET, FC_BUTTON, FC_IMAGEBTN,
    /* the rest */
    FC_SELECT, FC_HIDDEN, FC_FILE, FC_RANGE, FC_COLOR
};

#define FC_IS_TEXTUAL(k) ((k) >= FC_TEXT && (k) <= FC_TEXTAREA)
#define FC_IS_TOGGLE(k)  ((k) == FC_CHECKBOX || (k) == FC_RADIO)
#define FC_IS_BUTTON(k)  ((k) >= FC_SUBMIT && (k) <= FC_IMAGEBTN)

/* ASCII case-insensitive compare of a NUL-terminated attribute value against a
 * lowercase literal. Inline because fc_kind() is inline and this is its only
 * caller; HTML attribute keyword matching is ASCII-case-insensitive by
 * definition, so a locale-aware compare would be wrong as well as absent. */
static inline int fc__ieq(const char *v, const char *lit)
{
    if (!v) return 0;
    int i = 0;
    for (; lit[i]; i++) {
        char c = v[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != lit[i]) return 0;
    }
    return v[i] == 0;
}

static inline int fc__tag(const char *t, const char *lit)
{ int i = 0; for (; lit[i]; i++) if (t[i] != lit[i]) return 0; return t[i] == 0; }

/* What kind of control is this element? FC_NONE for everything that is not one.
 *
 * A pure function of the DOM: no state, no allocation, no link dependency. An
 * unknown `type` falls back to FC_TEXT, which is what the spec's "invalid value
 * default" says and what makes `<input type=date>` a typeable box here instead
 * of an invisible one. */
static inline int fc_kind(const struct node *n)
{
    if (!n || n->type != N_ELEM) return FC_NONE;
    const char *t = n->tag;
    if (fc__tag(t, "textarea")) return FC_TEXTAREA;
    if (fc__tag(t, "select"))   return FC_SELECT;
    if (fc__tag(t, "button")) {
        const char *ty = dom_attr(n, "type");
        if (fc__ieq(ty, "reset"))  return FC_RESET;
        if (fc__ieq(ty, "button")) return FC_BUTTON;
        return FC_SUBMIT;                 /* <button> with no type submits */
    }
    if (!fc__tag(t, "input")) return FC_NONE;
    const char *ty = dom_attr(n, "type");
    if (!ty || !ty[0])              return FC_TEXT;
    if (fc__ieq(ty, "text"))        return FC_TEXT;
    if (fc__ieq(ty, "password"))    return FC_PASSWORD;
    if (fc__ieq(ty, "search"))      return FC_SEARCH;
    if (fc__ieq(ty, "email"))       return FC_EMAIL;
    if (fc__ieq(ty, "url"))         return FC_URL;
    if (fc__ieq(ty, "tel"))         return FC_TEL;
    if (fc__ieq(ty, "number"))      return FC_NUMBER;
    if (fc__ieq(ty, "checkbox"))    return FC_CHECKBOX;
    if (fc__ieq(ty, "radio"))       return FC_RADIO;
    if (fc__ieq(ty, "submit"))      return FC_SUBMIT;
    if (fc__ieq(ty, "reset"))       return FC_RESET;
    if (fc__ieq(ty, "button"))      return FC_BUTTON;
    if (fc__ieq(ty, "image"))       return FC_IMAGEBTN;
    if (fc__ieq(ty, "hidden"))      return FC_HIDDEN;
    if (fc__ieq(ty, "file"))        return FC_FILE;
    if (fc__ieq(ty, "range"))       return FC_RANGE;
    if (fc__ieq(ty, "color"))       return FC_COLOR;
    /* date, time, month, week, datetime-local: no picker, so they behave as
     * text boxes -- which is a downgrade a page can live with (the value round
     * trips) and an empty box is not. */
    return FC_TEXT;
}

/* ------------------------------------------------------------- metrics -- */
/* The content inset inside a control's border box. Shared here because layout.c
 * SIZES the box with it and forms.c/browser_paint.c place the text inside it --
 * two files deriving the same number independently is how a caret ends up one
 * pixel outside the field it belongs to. */
#define FC_PAD_X 5
#define FC_PAD_Y 3
/* The control's border thickness, in points, when the page has not styled it. */
#define FC_BORDER 1

/* The control's intrinsic border-box size at font size `font_px`, before CSS
 * width/height override it, is computed in layout.c (fc_metrics there): it
 * needs text_measure() for the <select>'s widest option and a button's label,
 * and keeping it there is what lets layout.c stay free of any link dependency
 * on this file. */

/* -------------------------------------------------------------- state -- */
/* Everything below lives in forms.c and is what the painter takes weakly. */

/* Drop every control's state. Called on navigation, BEFORE the DOM is freed. */
void fc_reset(void);

/* The control's current value. Never NULL; *len is set. For a <select> this is
 * the selected option's value. */
const char *fc_value(struct node *n, int *len);
/* Replace it. `len` < 0 means strlen. Returns 1 on success (0 = out of memory).
 * Sets the dirty-value flag, so later changes to the `value` ATTRIBUTE stop
 * being reflected -- which is the spec's rule and also what stops a repaint
 * from eating what the user typed. */
int  fc_set_value(struct node *n, const char *s, int len);
/* The default value: the `value` attribute, or a <textarea>'s text content. */
const char *fc_default_value(struct node *n, int *len);
/* Undo the dirty flag and re-read the default. This is form.reset(). */
void fc_reset_control(struct node *n);
void fc_reset_form(struct node *form);

int  fc_checked(struct node *n);
/* Setting a radio to checked unchecks the rest of its group, which is the only
 * reason radio grouping needs to exist in C at all. */
void fc_set_checked(struct node *n, int on);
int  fc_default_checked(struct node *n);

/* <select>: index of the selected <option>, or -1. */
int  fc_selected_index(struct node *n);
void fc_set_selected_index(struct node *n, int i);
/* Is the <select>'s dropdown showing? The list itself is drawn by browser.c
 * (it has to float above everything the display list contains, and the display
 * list has no z-order above itself), so forms.c owns only the flag. */
int  fc_select_open(struct node *n);
void fc_select_set_open(struct node *n, int on);

/* The i'th <option> of a <select> (flattening <optgroup>), or NULL. */
struct node *fc_option_at(struct node *sel, int i);
int  fc_option_count(struct node *sel);
/* An option's value: its `value` attribute, else its text content. `buf` is
 * used only when the text content has to be gathered; returns the length. */
int  fc_option_value(struct node *opt, char *buf, int max);
int  fc_option_label(struct node *opt, char *buf, int max);

/* The text caret / selection, in BYTES into the value. */
void fc_selection(struct node *n, int *start, int *end);
void fc_set_selection(struct node *n, int start, int end);

/* Is this control disabled (own attribute or an ancestor <fieldset disabled>)? */
int  fc_disabled(struct node *n);
int  fc_readonly(struct node *n);

/* The <form> this control belongs to: the `form` attribute's target if it
 * names one, else the nearest <form> ancestor. NULL for neither. */
struct node *fc_form_of(struct node *n);
/* The <label>'s labelled control: `for` by id, else the first control inside
 * the label. Clicking a label focuses (and for a checkbox, toggles) it, which
 * is how a very large fraction of real checkboxes are actually clicked. */
struct node *fc_label_target(struct node *label);

/* -------------------------------------------------------- text editing -- */
/* Every one of these operates on the FOCUSED control's value and raises the
 * editing events through fc_dispatch() (focus.h) in the order the spec gives:
 * beforeinput (cancelable) -> mutate -> input (bubbles). `change` is raised
 * separately, on commit -- blur or Enter for a text field, immediately for a
 * checkbox or a select, which is the rule pages are written against.
 *
 * Returns 1 if the value changed (so the caller repaints). */
int  fc_edit_insert(struct node *n, const char *s, int len);
int  fc_edit_backspace(struct node *n);
int  fc_edit_delete(struct node *n);
/* dir: -1 left, +1 right. `word` steps by word, `extend` keeps the anchor
 * (Shift+Arrow). Returns 1 if the caret or selection moved. */
int  fc_edit_move(struct node *n, int dir, int word, int extend);
int  fc_edit_home(struct node *n, int extend);
int  fc_edit_end(struct node *n, int extend);
int  fc_edit_select_all(struct node *n);
/* Fires `change` if the value differs from what it was when the control was
 * focused. Called on blur and on Enter. Returns 1 if it fired. */
int  fc_commit(struct node *n);
/* Remember the current value as the one `change` compares against. Called when
 * a control gains focus. */
int  fc_mark_focus(struct node *n);

/* ---------------------------------------------------------- submission -- */
/* Submitting is a NAVIGATION, so the embedder owns it: browser.c installs the
 * handler and it is the only thing in the tree allowed to load a page.
 * `fire_event` distinguishes the two JS entry points -- form.submit() bypasses
 * the `submit` event entirely and form.requestSubmit() fires it, which is the
 * whole reason both exist. Returns 1 if the browser navigated. */
typedef int (*fc_submit_fn)(struct node *form, struct node *submitter, int fire_event);
void fc_set_submit(fc_submit_fn fn);
int  fc_submit(struct node *form, struct node *submitter, int fire_event);

/* Build `application/x-www-form-urlencoded` for `form`, with `submitter` (the
 * button that was activated, may be NULL) contributing its own name/value.
 * Returns the byte count written to `out`, or -1 if it would not fit. */
int  fc_encode(struct node *form, struct node *submitter, char *out, int max);
/* 1 if the form's method is POST. */
int  fc_method_post(struct node *form);
/* The form's `action`, or "" for "this page". */
const char *fc_action(struct node *form);

/* ------------------------------------------------------------- painting -- */
/* Everything browser_paint.c needs to draw one control, in one call, so that
 * the painter does no text measuring and no state lookup of its own. Pixel
 * offsets are relative to the control's CONTENT origin (border box inset by
 * the control's own padding, which fc_paint_state fills in).
 *
 * Why the geometry is computed HERE and not in the painter: the caret's x is
 * a text measurement, and the host paint test's logit.h has no text_measure.
 * Putting it here keeps the painter to the five primitives it is documented to
 * use, and keeps the weak-link fallback honest -- without forms.c the painter
 * draws an empty control instead of a control with a wrong caret. */
struct fpaint {
    int kind;
    const char *text; int len;       /* what to draw inside (masked, for a password) */
    int placeholder;                 /* `text` is the placeholder: draw it grey */
    int checked;
    int focused, disabled, readonly;
    int caret_x;                     /* px from the content origin; -1 = no caret */
    int sel_x0, sel_x1;              /* selection highlight; equal = none */
    int scroll_x;                    /* px the content is scrolled left by */
    int text_w;                      /* measured width of `text` */
    int pad_x, pad_y;                /* content inset inside the border box */
    int line_h;                      /* textarea: line advance */
    int nline;                       /* textarea: lines in `text` */
    int caret_line;                  /* textarea: which line the caret is on */
};
/* 1 if `n` is a control and *out was filled. `content_w` is the width the text
 * has to live in, which is what decides the horizontal scroll of a field whose
 * value is longer than its box. */
int fc_paint_state(struct node *n, int font_px, int mono, int content_w,
                   struct fpaint *out);

/* The byte offset in the value nearest to `px` from the content origin: where
 * a click puts the caret. */
int fc_offset_at_px(struct node *n, int px, int font_px, int mono);

#endif /* LOGIT_FORMS_H */
