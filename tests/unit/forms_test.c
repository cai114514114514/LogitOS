/* Host test for the focus model (focus.c) and the form controls (forms.c),
 * plus the boxes layout.c reserves for them.
 *
 * WHAT THIS CAN AND CANNOT ESTABLISH, said first because the whole point of
 * this line of work is a device-level failure. A host test can prove that
 * `value` changes when a keystroke is applied, that Tab visits the right
 * elements in the right order, and that a form serialises to the right bytes.
 * It CANNOT prove that a human can type into a search box: that needs the
 * keyboard, the window and the painter, and it is what
 * tests/qmp/qmp_forms.py does with a screendump. Neither replaces the other,
 * and the make target runs both.
 *
 * THE EVENT ORDER IS ASSERTED, not just the values. Focus is one of the few
 * areas where implementations differ in the ORDER of what they fire and pages
 * genuinely depend on it (blur before focus, focusout before focusin,
 * beforeinput before the mutation, change only on commit). The recorder below
 * installs itself through the same fc_set_dispatch() seam the browser uses, so
 * what is asserted here is what a page would see.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "layout.h"
#include "css.h"
#include "dom.h"
#include "forms.h"
#include "focus.h"

/* ---- stubs: the same set layout_test.c uses ---- */
void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }
/* A fixed half-em advance. Deliberately not a real font: every geometry
 * assertion below is then an exact integer the test can state, and a caret
 * position that is off by one character is off by px/2 and visible. */
int text_measure(const char *s, int len, int px, int mono)
{ (void)s; (void)mono; return len * (px / 2); }
int res_fetch(const char *url, uint8_t **buf, int *len)
{ (void)url; (void)buf; (void)len; return -1; }
void img_free(struct image *o) { (void)o; }
int img_decode(const uint8_t *p, int n, struct image *out)
{ (void)p; (void)n; (void)out; return -1; }

static int fail;
static int checks;
#define CHECK(c, msg) do { checks++; if (!(c)) { printf("FAIL: %s\n", msg); fail = 1; } \
                           else printf("ok: %s\n", msg); } while (0)

/* ---- the event recorder ---- */
#define EVMAX 64
static char ev_log[EVMAX][48];
static int  ev_n;
static int  ev_cancel_beforeinput;

static int rec(struct node *t, const char *type, int bubbles, int cancelable)
{
    if (ev_n < EVMAX) {
        const char *tag = t && t->tag ? t->tag : "?";
        const char *id = t ? dom_attr(t, "id") : 0;
        snprintf(ev_log[ev_n], sizeof ev_log[0], "%s@%s%s%s",
                 type, tag, id ? "#" : "", id ? id : "");
        ev_n++;
    }
    (void)bubbles;
    if (cancelable && ev_cancel_beforeinput && strcmp(type, "beforeinput") == 0) return 0;
    return 1;
}

/* The SAME log, through the richer seam. Logging in the identical format is
 * deliberate: every ordering assertion written before inputType existed keeps
 * passing unchanged, and the new field lands in a parallel array that only the
 * checks that care about it read. A recorder that reformatted the log would
 * have made adding inputType look like a regression in twenty other checks. */
static char ev_it[EVMAX][32];       /* the inputType of ev_log[i], "" if none */
static char ev_data[EVMAX][32];     /* InputEvent.data, "-" for NULL */

static int rec_input(struct node *t, const char *type, const char *itype,
                     const char *data, int bubbles, int cancelable)
{
    int slot = ev_n;
    int r = rec(t, type, bubbles, cancelable);
    if (slot < EVMAX && ev_n > slot) {
        snprintf(ev_it[slot], sizeof ev_it[0], "%s", itype ? itype : "");
        snprintf(ev_data[slot], sizeof ev_data[0], "%s", data ? data : "-");
    }
    return r;
}

static void ev_clear(void) { ev_n = 0; }

/* The inputType logged alongside event `i`. "" when the event carried none. */
static int it_is(int i, const char *want)
{ return i < ev_n && strcmp(ev_it[i], want) == 0; }

static int ev_is(int i, const char *want)
{ return i < ev_n && strcmp(ev_log[i], want) == 0; }

static void ev_dump(const char *what)
{
    printf("   [%s]", what);
    for (int i = 0; i < ev_n; i++) printf(" %s", ev_log[i]);
    printf("\n");
}

static struct node *by_id(struct node *n, const char *id)
{
    if (n->type == N_ELEM) {
        const char *v = dom_attr(n, "id");
        if (v && strcmp(v, id) == 0) return n;
    }
    for (struct node *c = n->first_child; c; c = c->next) {
        struct node *r = by_id(c, id);
        if (r) return r;
    }
    return 0;
}

static const struct item *item_for(struct node *n)
{
    const struct item *it = layout_items();
    for (int i = 0; i < layout_count(); i++)
        if (it[i].node == n && it[i].type == IT_CONTROL) return &it[i];
    return 0;
}

/* Type a string one character at a time, exactly as the keyboard delivers it. */
static void typ(struct node *n, const char *s)
{ for (; *s; s++) fc_edit_insert(n, s, 1); }

/* browser_paint.c's hit test, minus the framebuffer it needs to link: the LAST
 * item in the display list covering the point (parent boxes are emitted before
 * their children, so the last one is the innermost), climbed to an element
 * because a DOM event target cannot be a text node. Kept in step with
 * browser_hittest_node() by hand; section 15 is the reason it exists. */
static struct node *hittest_like_browser(int x, int y)
{
    const struct item *it = layout_items();
    for (int i = layout_count() - 1; i >= 0; i--) {
        const struct item *e = &it[i];
        if (e->hidden) continue;
        if (!(x >= e->x && x < e->x + e->w && y >= e->y && y < e->y + e->h)) continue;
        struct node *p = e->node;
        while (p && p->type != N_ELEM && p->type != N_DOCUMENT) p = p->parent;
        return p;
    }
    return 0;
}

/* Serialise a subtree the way the assertions want to read it: tags and text,
 * nothing else. A structural edit is asserted against a STRING, because
 * "backspace merged the paragraphs" is a claim about the shape of the tree and
 * checking a character count cannot tell a merge from a deletion. */
static void ser(const struct node *n, char *buf, int max, int *o)
{
    for (const struct node *c = n->first_child; c; c = c->next) {
        if (c->type == N_TEXT) {
            for (int i = 0; i < c->textlen && *o < max - 1; i++) buf[(*o)++] = c->text[i];
        } else if (c->type == N_ELEM) {
            *o += snprintf(buf + *o, (size_t)(max - *o), "<%s>", c->tag);
            if (*o >= max - 1) return;
            /* A void element has no end tag, and printing one turns a correct
             * DOM into a failing assertion that looks like a product bug. */
            if (strcmp(c->tag, "br") == 0 || strcmp(c->tag, "hr") == 0 ||
                strcmp(c->tag, "img") == 0 || strcmp(c->tag, "input") == 0) continue;
            ser(c, buf, max, o);
            *o += snprintf(buf + *o, (size_t)(max - *o), "</%s>", c->tag);
            if (*o >= max - 1) return;
        }
    }
    buf[*o < max ? *o : max - 1] = 0;
}

static const char *shape(const struct node *n)
{
    static char buf[1024];
    int o = 0;
    buf[0] = 0;
    ser(n, buf, (int)sizeof buf, &o);
    return buf;
}

/* Type into the caret one character at a time, exactly as the keyboard
 * delivers it -- five calls, not one, because a bug that only shows on the
 * second keystroke (a stale caret, a text node re-created each time) is
 * invisible to a single insert of the whole string. */
static void ce_typ(const char *s)
{ for (; *s; s++) fc_ce_insert(s, 1); }

int main(void)
{
    fc_set_dispatch(rec);
    fc_set_dispatch_input(rec_input);

    /* ================================================================ 1 ==
     * The classification, which everything else keys off. */
    {
        const char *html =
            "<body><form id='f'>"
            "<input id='t' type='text'><input id='p' type='password'>"
            "<input id='c' type='checkbox'><input id='r' type='radio'>"
            "<input id='s' type='submit'><input id='h' type='hidden'>"
            "<input id='x' type='date'><input id='n'>"
            "<textarea id='ta'></textarea><select id='se'><option>a</option></select>"
            "<button id='b'>Go</button><button id='b2' type='button'>x</button>"
            "</form></body>";
        struct node *root = dom_parse(html, (int)strlen(html));
        CHECK(root != 0, "1. document parsed");
        CHECK(fc_kind(by_id(root, "t")) == FC_TEXT, "1. input[type=text] -> FC_TEXT");
        CHECK(fc_kind(by_id(root, "p")) == FC_PASSWORD, "1. password");
        CHECK(fc_kind(by_id(root, "c")) == FC_CHECKBOX, "1. checkbox");
        CHECK(fc_kind(by_id(root, "r")) == FC_RADIO, "1. radio");
        CHECK(fc_kind(by_id(root, "s")) == FC_SUBMIT, "1. submit");
        CHECK(fc_kind(by_id(root, "h")) == FC_HIDDEN, "1. hidden");
        CHECK(fc_kind(by_id(root, "n")) == FC_TEXT, "1. <input> with no type is text");
        /* The invalid-value default. `date` has no picker here, and the spec's
         * answer for an unknown type is text -- which makes it a field the user
         * can type into rather than an invisible element. */
        CHECK(fc_kind(by_id(root, "x")) == FC_TEXT, "1. unknown input type falls back to text");
        CHECK(fc_kind(by_id(root, "ta")) == FC_TEXTAREA, "1. textarea");
        CHECK(fc_kind(by_id(root, "se")) == FC_SELECT, "1. select");
        CHECK(fc_kind(by_id(root, "b")) == FC_SUBMIT, "1. <button> defaults to submit");
        CHECK(fc_kind(by_id(root, "b2")) == FC_BUTTON, "1. <button type=button>");
        CHECK(fc_kind(by_id(root, "f")) == FC_NONE, "1. the <form> is not itself a control");
        fc_reset();
        dom_free(root);
    }

    /* ================================================================ 2 ==
     * Value vs. content attribute -- the dirty value flag. */
    {
        const char *html = "<body><input id='a' value='start'>"
                           "<textarea id='t'>hello</textarea></body>";
        struct node *root = dom_parse(html, (int)strlen(html));
        struct node *a = by_id(root, "a"), *t = by_id(root, "t");
        int len = 0;
        CHECK(strcmp(fc_value(a, &len), "start") == 0 && len == 5,
              "2. value starts as the content attribute");
        CHECK(strcmp(fc_value(t, &len), "hello") == 0,
              "2. a textarea's value is its text content");
        /* Not dirty yet: changing the attribute still shows through. */
        dom_set_attr(a, "value", "changed");
        CHECK(strcmp(fc_value(a, &len), "changed") == 0,
              "2. before the user types, value TRACKS the attribute");
        fc_set_value(a, "typed", -1);
        dom_set_attr(a, "value", "ignored");
        CHECK(strcmp(fc_value(a, &len), "typed") == 0,
              "2. after it is set, the attribute no longer overwrites it "
              "(the dirty value flag)");
        CHECK(strcmp(fc_default_value(a, &len), "ignored") == 0,
              "2. ...and defaultValue still reads the attribute");
        fc_reset_control(a);
        CHECK(strcmp(fc_value(a, &len), "ignored") == 0,
              "2. form reset clears the dirty flag and re-reads the default");
        fc_reset();
        dom_free(root);
    }

    /* ================================================================ 3 ==
     * Typing. THE thing that did not exist. */
    {
        const char *html = "<body><input id='a'></body>";
        struct node *root = dom_parse(html, (int)strlen(html));
        struct node *a = by_id(root, "a");
        int len = 0;
        ev_clear();
        typ(a, "hello");
        CHECK(strcmp(fc_value(a, &len), "hello") == 0 && len == 5,
              "3. five keystrokes produce five characters");
        ev_dump("typing 'hello'");
        CHECK(ev_n == 10, "3. and ten events: beforeinput+input per character");
        CHECK(ev_is(0, "beforeinput@input#a") && ev_is(1, "input@input#a"),
              "3. in that order -- beforeinput BEFORE the mutation");

        int s, e;
        fc_selection(a, &s, &e);
        CHECK(s == 5 && e == 5, "3. the caret ends after the last character");

        fc_edit_backspace(a);
        CHECK(strcmp(fc_value(a, &len), "hell") == 0, "3. backspace deletes one");
        fc_edit_move(a, -1, 0, 0);
        fc_edit_move(a, -1, 0, 0);
        fc_selection(a, &s, &e);
        CHECK(s == 2 && e == 2, "3. two arrow-lefts move the caret two back");
        fc_edit_insert(a, "X", 1);
        CHECK(strcmp(fc_value(a, &len), "heXll") == 0,
              "3. insertion happens AT the caret, not at the end");
        fc_edit_delete(a);
        CHECK(strcmp(fc_value(a, &len), "heXl") == 0, "3. forward delete");
        fc_edit_home(a, 0);
        fc_edit_end(a, 1);
        fc_selection(a, &s, &e);
        CHECK(s == 0 && e == 4, "3. Home then Shift+End selects the whole value");
        fc_edit_insert(a, "Z", 1);
        CHECK(strcmp(fc_value(a, &len), "Z") == 0,
              "3. typing over a selection REPLACES it");

        /* preventDefault on beforeinput. */
        ev_cancel_beforeinput = 1;
        ev_clear();
        int changed = fc_edit_insert(a, "q", 1);
        ev_cancel_beforeinput = 0;
        CHECK(!changed && strcmp(fc_value(a, &len), "Z") == 0,
              "3. a page that cancels beforeinput stops the character");
        CHECK(ev_n == 1 && ev_is(0, "beforeinput@input#a"),
              "3. ...and no `input` event follows a cancelled one");
        fc_reset();
        dom_free(root);
    }

    /* ================================================================ 4 ==
     * UTF-8 is stepped by character. A caret inside a multi-byte sequence is
     * not a cosmetic bug: it produces bytes the rasterizer cannot decode. */
    {
        const char *html = "<body><input id='a'></body>";
        struct node *root = dom_parse(html, (int)strlen(html));
        struct node *a = by_id(root, "a");
        int len = 0;
        fc_set_value(a, "a\xe4\xb8\xad" "b", -1);    /* a <CJK> b, 5 bytes */
        int s, e;
        fc_selection(a, &s, &e);
        CHECK(s == 5, "4. caret starts at the end (5 bytes)");
        fc_edit_move(a, -1, 0, 0);
        fc_selection(a, &s, &e);
        CHECK(s == 4, "4. one left over an ASCII byte");
        fc_edit_move(a, -1, 0, 0);
        fc_selection(a, &s, &e);
        CHECK(s == 1, "4. one left over a THREE-BYTE character moves three bytes");
        fc_edit_delete(a);
        CHECK(strcmp(fc_value(a, &len), "ab") == 0 && len == 2,
              "4. deleting it removes all three bytes, not one");
        fc_reset();
        dom_free(root);
    }

    /* ================================================================ 5 ==
     * maxlength, and that it counts characters rather than bytes. */
    {
        const char *html = "<body><input id='a' maxlength='3'></body>";
        struct node *root = dom_parse(html, (int)strlen(html));
        struct node *a = by_id(root, "a");
        int len = 0;
        typ(a, "abcdef");
        CHECK(strcmp(fc_value(a, &len), "abc") == 0, "5. maxlength stops the fourth character");
        fc_set_value(a, "", 0);
        fc_edit_insert(a, "\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97\xe5\x85\x83", 12);
        CHECK(strcmp(fc_value(a, &len), "\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97") == 0,
              "5. and it counts CHARACTERS: three CJK, nine bytes");
        fc_reset();
        dom_free(root);
    }

    /* ================================================================ 6 ==
     * Checkboxes and radio grouping. */
    {
        const char *html =
            "<body><form id='f'>"
            "<input id='c' type='checkbox' name='k' value='v'>"
            "<input id='r1' type='radio' name='g' value='1' checked>"
            "<input id='r2' type='radio' name='g' value='2'>"
            "<input id='r3' type='radio' name='other' value='3'>"
            "</form></body>";
        struct node *root = dom_parse(html, (int)strlen(html));
        struct node *c = by_id(root, "c"), *r1 = by_id(root, "r1");
        struct node *r2 = by_id(root, "r2"), *r3 = by_id(root, "r3");
        CHECK(!fc_checked(c), "6. an unchecked checkbox reads false");
        CHECK(fc_checked(r1), "6. `checked` in the markup is the default checkedness");
        fc_set_checked(c, 1);
        CHECK(fc_checked(c), "6. ticking it reads back");
        fc_set_checked(r2, 1);
        CHECK(fc_checked(r2) && !fc_checked(r1),
              "6. checking a radio UNCHECKS the rest of its name group");
        CHECK(!fc_checked(r3), "6. ...and leaves a different group alone");
        fc_set_checked(r3, 1);
        CHECK(fc_checked(r2) && fc_checked(r3),
              "6. two groups hold one selection each");
        fc_reset();
        dom_free(root);
    }

    /* ================================================================ 7 ==
     * The focus model. */
    {
        const char *html =
            "<body>"
            "<a id='lnk' href='/x'>link</a>"
            "<input id='i1'>"
            "<input id='dis' disabled>"
            "<input id='neg' tabindex='-1'>"
            "<div id='div0' tabindex='0'>div</div>"
            "<input id='first' tabindex='1'>"
            "<p id='plain'>text</p>"
            "<input id='i2'>"
            "</body>";
        struct node *root = dom_parse(html, (int)strlen(html));
        css_apply(root, 0, 0);
        struct node *i1 = by_id(root, "i1"), *i2 = by_id(root, "i2");
        struct node *dis = by_id(root, "dis"), *neg = by_id(root, "neg");
        struct node *div0 = by_id(root, "div0"), *first = by_id(root, "first");
        struct node *plain = by_id(root, "plain"), *lnk = by_id(root, "lnk");

        CHECK(focus_is_focusable(i1), "7. a text input is focusable");
        CHECK(!focus_is_focusable(dis), "7. a disabled one is not");
        CHECK(!focus_is_focusable(plain), "7. a <p> is not");
        CHECK(focus_is_focusable(lnk), "7. an <a href> is");
        CHECK(focus_is_focusable(div0), "7. a div with tabindex=0 is");
        CHECK(focus_is_focusable(neg),
              "7. tabindex=-1 IS focusable -- programmatically, which is how "
              "every modal and roving-tabindex widget works");
        int ex = 0;
        CHECK(focus_tab_index(neg, &ex) == -1 && ex,
              "7. ...and its tab index is negative, which is what excludes it from Tab");

        CHECK(focus_current() == 0, "7. nothing is focused to begin with");
        ev_clear();
        focus_set(i1);
        CHECK(focus_current() == i1, "7. focus_set moves focus");
        ev_dump("focus i1");
        CHECK(ev_n == 2 && ev_is(0, "focus@input#i1") && ev_is(1, "focusin@input#i1"),
              "7. focus then focusin, and nothing else");
        ev_clear();
        focus_set(i2);
        ev_dump("i1 -> i2");
        CHECK(ev_n == 4 && ev_is(0, "blur@input#i1") && ev_is(1, "focusout@input#i1") &&
              ev_is(2, "focus@input#i2") && ev_is(3, "focusin@input#i2"),
              "7. moving focus is blur, focusout, focus, focusin -- IN THAT ORDER");
        ev_clear();
        focus_set(i2);
        CHECK(ev_n == 0, "7. re-focusing the same element fires nothing");
        ev_clear();
        focus_set(dis);
        CHECK(focus_current() == 0 && ev_n == 2,
              "7. focusing a disabled control blurs instead, exactly like "
              "clicking on a paragraph");

        /* Tab order: tabindex=1 first, then document order at 0, skipping the
         * disabled one and the negative one. */
        focus_set(0);
        ev_clear();
        struct node *ord[8];
        int n = 0;
        struct node *cur = 0;
        for (int i = 0; i < 6; i++) { cur = focus_next(root, cur, 0); ord[n++] = cur; }
        CHECK(ord[0] == first,
              "7. Tab visits the positive tabindex FIRST, whatever the document order");
        CHECK(ord[1] == lnk && ord[2] == i1 && ord[3] == div0 && ord[4] == i2,
              "7. then the tabindex-0 group in document order");
        CHECK(ord[5] == first, "7. and it wraps");
        for (int i = 0; i < 5; i++)
            CHECK(ord[i] != dis && ord[i] != neg,
                  "7. Tab reaches neither the disabled control nor tabindex=-1");
        CHECK(focus_tabbable_count(root) == 5, "7. five tabbable elements in all");

        struct node *back = focus_next(root, i1, 1);
        CHECK(back == lnk, "7. Shift+Tab goes the other way");

        /* A detached element loses focus rather than keeping it. */
        focus_set(i2);
        dom_remove_child(i2->parent, i2);
        CHECK(focus_current() == 0,
              "7. removing the focused element from the document drops focus, "
              "instead of leaving the keyboard pointed at a node nobody can see");
        dom_destroy_subtree(i2);
        focus_reset();
        fc_reset();
        dom_free(root);
    }

    /* ================================================================ 8 ==
     * `change` fires on commit, not on every keystroke. */
    {
        const char *html = "<body><input id='a' value='x'></body>";
        struct node *root = dom_parse(html, (int)strlen(html));
        struct node *a = by_id(root, "a");
        fc_mark_focus(a);
        ev_clear();
        typ(a, "yz");
        int fired = 0;
        for (int i = 0; i < ev_n; i++) if (strncmp(ev_log[i], "change@", 7) == 0) fired = 1;
        CHECK(!fired, "8. typing does not fire `change`");
        ev_clear();
        CHECK(fc_commit(a) == 1 && ev_n == 1 && ev_is(0, "change@input#a"),
              "8. committing (blur / Enter) fires it once");
        ev_clear();
        CHECK(fc_commit(a) == 0 && ev_n == 0,
              "8. committing again with the value unchanged fires nothing");
        fc_reset();
        dom_free(root);
    }

    /* ================================================================ 9 ==
     * Form submission: the payload. */
    {
        const char *html =
            "<body><form id='f' action='/search' method='get'>"
            "<input name='q' value='hello world'>"
            "<input name='e' value='a&b=c'>"
            "<input name='skip' value='v' disabled>"
            "<input value='noname'>"
            "<input type='checkbox' name='on1' checked>"
            "<input type='checkbox' name='off1'>"
            "<input type='checkbox' name='on2' value='yes' checked>"
            "<input type='radio' name='g' value='1'>"
            "<input type='radio' name='g' value='2' checked>"
            "<input type='hidden' name='h' value='hv'>"
            "<select name='s'><option value='o1'>one</option>"
            "<option value='o2' selected>two</option></select>"
            "<textarea name='ta'>l1\nl2</textarea>"
            "<button name='b1' value='v1'>one</button>"
            "<button name='b2' value='v2'>two</button>"
            "</form></body>";
        struct node *root = dom_parse(html, (int)strlen(html));
        struct node *f = by_id(root, "f");
        char buf[2048];
        int n = fc_encode(f, 0, buf, (int)sizeof buf);
        printf("   payload: %s\n", buf);
        CHECK(n > 0, "9. the form encodes");
        CHECK(strstr(buf, "q=hello+world") != 0,
              "9. a space becomes '+' (urlencoded, not percent-encoded)");
        CHECK(strstr(buf, "e=a%26b%3Dc") != 0, "9. '&' and '=' are percent-encoded");
        CHECK(strstr(buf, "skip=") == 0, "9. a disabled control is not submitted");
        CHECK(strstr(buf, "noname") == 0, "9. neither is one with no name");
        CHECK(strstr(buf, "on1=on") != 0,
              "9. a checked checkbox with no value submits the HTML default 'on'");
        CHECK(strstr(buf, "off1") == 0, "9. an unchecked one submits nothing at all");
        CHECK(strstr(buf, "on2=yes") != 0, "9. a checked one with a value submits it");
        CHECK(strstr(buf, "g=2") != 0 && strstr(buf, "g=1") == 0,
              "9. only the checked radio of a group is submitted");
        CHECK(strstr(buf, "h=hv") != 0, "9. a hidden input IS submitted");
        CHECK(strstr(buf, "s=o2") != 0, "9. the select submits its selected option's value");
        CHECK(strstr(buf, "ta=l1%0D%0Al2") != 0,
              "9. a textarea's newline is normalised to CRLF before encoding");
        CHECK(strstr(buf, "b1") == 0 && strstr(buf, "b2") == 0,
              "9. NO button is submitted when there is no submitter");

        struct node *b2 = 0;
        for (struct node *c = f->first_child; c; c = c->next) {
            const char *nm = c->type == N_ELEM ? dom_attr(c, "name") : 0;
            if (nm && strcmp(nm, "b2") == 0) b2 = c;
        }
        n = fc_encode(f, b2, buf, (int)sizeof buf);
        CHECK(strstr(buf, "b2=v2") != 0 && strstr(buf, "b1") == 0,
              "9. and exactly ONE button is: the one that was activated");
        CHECK(!fc_method_post(f) && strcmp(fc_action(f), "/search") == 0,
              "9. method and action read back");

        /* A payload that does not fit is refused, not truncated -- a truncated
         * query string is a request that silently means something else. */
        char tiny[8];
        CHECK(fc_encode(f, 0, tiny, (int)sizeof tiny) == -1,
              "9. an over-long payload is REFUSED rather than cut short");
        fc_reset();
        dom_free(root);
    }

    /* =============================================================== 10 ==
     * The `form` attribute: a control outside the form's subtree. */
    {
        const char *html =
            "<body><form id='f' action='/a'><input name='in' value='1'></form>"
            "<input name='out' value='2' form='f'>"
            "<input name='none' value='3'></body>";
        struct node *root = dom_parse(html, (int)strlen(html));
        struct node *f = by_id(root, "f");
        char buf[512];
        fc_encode(f, 0, buf, (int)sizeof buf);
        printf("   payload: %s\n", buf);
        CHECK(strstr(buf, "in=1") != 0, "10. the descendant control is submitted");
        CHECK(strstr(buf, "out=2") != 0,
              "10. and so is one OUTSIDE the form that names it with form=");
        CHECK(strstr(buf, "none=3") == 0, "10. an unassociated control is not");
        fc_reset();
        dom_free(root);
    }

    /* =============================================================== 11 ==
     * <label> association. */
    {
        const char *html =
            "<body>"
            "<label id='l1' for='c1'>Remember me</label><input id='c1' type='checkbox'>"
            "<label id='l2'>Name <input id='c2'></label>"
            "<label id='l3' for='nope'>orphan</label>"
            "</body>";
        struct node *root = dom_parse(html, (int)strlen(html));
        CHECK(fc_label_target(by_id(root, "l1")) == by_id(root, "c1"),
              "11. label[for] finds its control by id");
        CHECK(fc_label_target(by_id(root, "l2")) == by_id(root, "c2"),
              "11. a label with no `for` labels the control INSIDE it");
        CHECK(fc_label_target(by_id(root, "l3")) == 0,
              "11. a label pointing at nothing labels nothing");
        fc_reset();
        dom_free(root);
    }

    /* =============================================================== 12 ==
     * Layout: a control gets a BOX, and its option list is not page text. */
    {
        const char *html =
            "<body style='width:600px'>"
            "<input id='t' size='10'>"
            "<input id='c' type='checkbox'>"
            "<input id='h' type='hidden'>"
            "<select id='s'><option>alpha</option><option>a much longer one</option></select>"
            "<textarea id='ta' cols='5' rows='3'></textarea>"
            "<input id='w' style='width:222px'>"
            "</body>";
        struct node *root = dom_parse(html, (int)strlen(html));
        css_apply(root, 0, 0);
        layout_page(root, 600);

        const struct item *t = item_for(by_id(root, "t"));
        const struct item *c = item_for(by_id(root, "c"));
        const struct item *s = item_for(by_id(root, "s"));
        const struct item *ta = item_for(by_id(root, "ta"));
        const struct item *w = item_for(by_id(root, "w"));
        CHECK(t && t->ctl == FC_TEXT, "12. the text input produced an IT_CONTROL box");
        CHECK(c && c->w == 13 && c->h == 13, "12. a checkbox is 13x13");
        CHECK(item_for(by_id(root, "h")) == 0,
              "12. <input type=hidden> reserves NO box");
        /* size=10 at 16px with a px/2 advance: 10*8 + 2*5 padding + 2 border. */
        CHECK(t->w == 10 * 8 + 2 * FC_PAD_X + 2 * FC_BORDER,
              "12. size=10 is ten '0' advances plus the padding and the frame");
        CHECK(ta && ta->h > t->h * 2,
              "12. a rows=3 textarea is taller than a single-line input");
        CHECK(w && w->w >= 222, "12. author CSS width wins over the intrinsic size");
        CHECK(s && s->ctl == FC_SELECT, "12. the select produced a box");
        /* The whole point of not descending: the option text must not be laid
         * out as page content. */
        int option_text = 0;
        const struct item *it = layout_items();
        for (int i = 0; i < layout_count(); i++)
            if (it[i].type == IT_TEXT && it[i].text &&
                strncmp(it[i].text, "alpha", 5) == 0) option_text = 1;
        CHECK(!option_text,
              "12. the <option> labels are NOT emitted as page text -- an "
              "unstyled <select> used to print its whole list beside the box");
        /* The box is wide enough for the widest option plus the triangle. */
        CHECK(s->w >= 17 * 8, "12. the select sized itself to its widest option");
        layout_free();
        fc_reset();
        dom_free(root);
    }

    /* =============================================================== 13 ==
     * What the painter is handed. */
    {
        const char *html =
            "<body style='width:600px'>"
            "<input id='p' type='password' value='abc'>"
            "<input id='ph' placeholder='Search'>"
            "<input id='v' value='0123456789012345678901234567890123456789' size='10'>"
            "</body>";
        struct node *root = dom_parse(html, (int)strlen(html));
        css_apply(root, 0, 0);
        layout_page(root, 600);
        struct fpaint fp;

        struct node *p = by_id(root, "p");
        CHECK(fc_paint_state(p, 16, 0, 80, &fp), "13. the painter gets a state for a control");
        CHECK(fp.len == 9 && (unsigned char)fp.text[0] == 0xE2,
              "13. a password is drawn as bullets (3 UTF-8 bytes each), never as its value");
        CHECK(memcmp(fp.text, "abc", 3) != 0, "13. ...and the value is nowhere in what is drawn");

        struct node *ph = by_id(root, "ph");
        fc_paint_state(ph, 16, 0, 80, &fp);
        CHECK(fp.placeholder && fp.len == 6,
              "13. an empty field draws its placeholder, flagged so it is drawn grey");
        fc_set_value(ph, "x", 1);
        fc_paint_state(ph, 16, 0, 80, &fp);
        CHECK(!fp.placeholder && fp.len == 1, "13. ...and stops as soon as it has a value");

        /* The caret follows the focus, and a long value scrolls so the caret
         * stays inside the box. */
        struct node *v = by_id(root, "v");
        fc_paint_state(v, 16, 0, 80, &fp);
        CHECK(fp.caret_x < 0, "13. an unfocused field draws no caret");
        focus_set(v);
        fc_set_selection(v, 40, 40);
        fc_paint_state(v, 16, 0, 80, &fp);
        CHECK(fp.caret_x == 40 * 8, "13. the caret is measured at the byte offset");
        CHECK(fp.scroll_x > 0 && fp.caret_x - fp.scroll_x <= 80,
              "13. and a long value scrolled so the caret is INSIDE the box");
        fc_set_selection(v, 0, 0);
        fc_paint_state(v, 16, 0, 80, &fp);
        CHECK(fp.scroll_x == 0, "13. moving back to the start scrolls back");

        /* Clicking maps a pixel to a character. */
        int off = fc_offset_at_px(v, 3 * 8 + 2, 16, 0);
        CHECK(off == 3, "13. a click three characters in puts the caret at offset 3");
        focus_reset();
        layout_free();
        fc_reset();
        dom_free(root);
    }

    /* =============================================================== 14 ==
     * A recycled node slot must not inherit the previous element's typing.
     * This is the failure the {node, serial} pair exists to prevent, and it
     * is invisible without a test that deliberately provokes it. */
    {
        const char *html = "<body><div id='host'><input id='a'></div></body>";
        struct node *root = dom_parse(html, (int)strlen(html));
        struct node *host = by_id(root, "host");
        struct node *a = by_id(root, "a");
        fc_set_value(a, "secret", -1);
        int len = 0;
        CHECK(strcmp(fc_value(a, &len), "secret") == 0, "14. the first control holds a value");
        dom_destroy_subtree(a);
        struct node *b = dom_create_element(root->doc, "input", 5);
        dom_append_child(host, b);
        CHECK(strcmp(fc_value(b, &len), "") == 0 && len == 0,
              "14. a NEW control in the recycled slot starts empty -- the serial "
              "check is what stops it inheriting the old one's text");
        fc_reset();
        dom_free(root);
    }

    /* =============================================================== 15 ==
     * THE LABEL IS CLICKABLE, which is a LAYOUT question and not a forms one.
     *
     * Found by the device test, not by this file: on the machine, clicking the
     * word TICKBOX ticked nothing, and no `click` listener on the <label> ever
     * fired -- so the failure was upstream of everything section 11 checks.
     * fc_label_target() was right the whole time; the hit test never reached
     * the label, because layout emitted its text with no box to hit.
     *
     * The hit test itself lives in browser_paint.c, which needs a GUI to link,
     * so what is reproduced here is its RULE (the last item covering the point,
     * climbed to an element) over the same display list. That is the part that
     * was wrong, and it is testable without a framebuffer. */
    {
        /* The device page, verbatim -- the label follows a block-level <form>
         * with margins, which the first version of this section left out and
         * which is exactly where the difference turned out to be. */
        const char *html =
            "<body>"
            "<div id='anchor'>ANCHOR</div>"
            "<form id='f' action='/search' method='get'>"
            "<input id='q' name='q'><br>"
            "<input id='q2' name='r'><br>"
            "</form>"
            "<label id='lab' for='cb'>TICKBOX</label>"
            "<input id='cb' type='checkbox' name='cb'>"
            "</body>";
        const char *page_css =
            "html, body { background:#ffffff; margin:0; padding:0; color:#000000; }"
            "#anchor { background:#fe01fe; width:240px; height:30px; }"
            "#q  { font-size:24px; width:300px; border:3px solid #01fe02; }"
            "#q2 { font-size:24px; width:200px; border:3px solid #fe0102; }"
            "form { display:block; margin:12px 0; }"
            "label { font-size:20px; }";
        struct node *root = dom_parse(html, (int)strlen(html));
        css_apply(root, page_css, (int)strlen(page_css));
        layout_page(root, 600);

        struct node *lab = by_id(root, "lab");
        struct node *cb = by_id(root, "cb");

        /* Find the text box that carries the label's own text. */
        const struct item *it = layout_items();
        const struct item *txt = 0;
        for (int i = 0; i < layout_count(); i++)
            if (it[i].type == IT_TEXT && it[i].text &&
                strncmp(it[i].text, "TICKBOX", 7) == 0) txt = &it[i];
        CHECK(txt != 0, "15. the label's text is laid out");

        if (txt) {
            /* The device clicked the first dark pixel of the glyph, so aim at
             * the top-left of the text box -- the exact spot that failed. */
            int px = txt->x + 2, py = txt->y + 2;
            struct node *hit = hittest_like_browser(px, py);
            CHECK(hit != 0, "15. a click on the label's first glyph hits SOMETHING");
            CHECK(hit == lab,
                  "15. and that something is the <label> -- the click that "
                  "ticks a checkbox on every real page");
            CHECK(fc_label_target(hit) == cb,
                  "15. so the label resolves to the control it labels");
            /* And the far end of the word, which is a different box column. */
            struct node *hit2 = hittest_like_browser(txt->x + txt->w - 2,
                                                    txt->y + txt->h / 2);
            CHECK(hit2 == lab, "15. and so does a click at the end of the word");
        }
        focus_reset();
        layout_free();
        fc_reset();
        dom_free(root);
    }


    /* ================================================================ 16 ==
     * contenteditable: the editing host.
     *
     * Everything after this section depends on this one answer, and it is the
     * one focus.c already had (it knows a contenteditable is FOCUSABLE) while
     * forms.c had nothing at all -- which is precisely why a keystroke reached
     * a focused composer and fell on the floor. */
    {
        const char *html =
            "<body>"
            "<div id='plain'>no</div>"
            "<div id='host' contenteditable><p id='p1'>hello</p></div>"
            "<div id='host2' contenteditable='true'>x</div>"
            "<div id='host3' contenteditable='false'>x</div>"
            "<div id='outer' contenteditable><span id='chip' contenteditable='false'>"
            "<span id='inchip'>@bob</span></span></div>"
            "</body>";
        struct node *root = dom_parse(html, (int)strlen(html));
        css_apply(root, "", 0);

        struct node *host = by_id(root, "host");
        struct node *p1 = by_id(root, "p1");
        CHECK(fc_ce_host(by_id(root, "plain")) == 0,
              "16. an ordinary <div> is not an editing host");
        CHECK(fc_ce_host(host) == host, "16. contenteditable=\"\" IS one -- the empty "
              "string is the attribute's `true` keyword and real markup uses it");
        CHECK(fc_ce_host(by_id(root, "host2")) == by_id(root, "host2"),
              "16. so is contenteditable=\"true\"");
        CHECK(fc_ce_host(by_id(root, "host3")) == 0,
              "16. contenteditable=\"false\" is not");
        CHECK(fc_ce_host(p1) == host,
              "16. and a descendant answers with the host above it -- which is "
              "what makes the caret's node resolve to something to type into");
        CHECK(fc_ce_host(p1->first_child) == host,
              "16. including a TEXT node, which is what the caret actually holds");
        CHECK(fc_ce_host(by_id(root, "inchip")) == 0,
              "16. a contenteditable=\"false\" island inside a host shadows it -- "
              "the mention chip every chat composer has");
        CHECK(fc_ce_editable(p1) == 1 && fc_ce_editable(by_id(root, "plain")) == 0,
              "16. fc_ce_editable agrees");

        fc_ce_clear();
        fc_reset();
        dom_free(root);
    }

    /* ================================================================ 17 ==
     * THE EMPTY COMPOSER. This is the state every LLM chat page is in at the
     * moment a human first clicks it: a styled box, a CSS placeholder, and not
     * one text node to put an offset into. A caret model that can only be
     * placed from a text run cannot be placed here at all -- and if the caret
     * cannot be placed, the first keystroke has nowhere to go. */
    {
        const char *html =
            "<body><div id='c' contenteditable></div>"
            "<div id='c2' contenteditable><p id='p'><br></p></div></body>";
        struct node *root = dom_parse(html, (int)strlen(html));
        css_apply(root, "", 0);
        struct node *c = by_id(root, "c");

        fc_ce_caret_in(c, 1);
        struct node *cn = 0; int co = -1;
        CHECK(fc_ce_selection(&cn, &co, 0, 0) == 1,
              "17. a caret can be placed in a composer with no content at all");
        CHECK(cn == c && co == 0,
              "17. and it is an ELEMENT position -- (host, child index 0), which "
              "is the only thing an empty element HAS");
        CHECK(fc_ce_collapsed() == 1, "17. collapsed, as a fresh caret is");

        ev_clear();
        CHECK(fc_ce_insert("h", 1) == 1, "17. and a character can be inserted at it");
        CHECK(strcmp(shape(c), "h") == 0,
              "17. THE TEXT NODE WAS CREATED: the composer now holds `h`");
        ce_typ("ello");
        CHECK(strcmp(shape(c), "hello") == 0,
              "17. and four more keystrokes extend the SAME node, not five nodes");
        fc_ce_selection(&cn, &co, 0, 0);
        CHECK(cn && cn->type == N_TEXT && co == 5,
              "17. the caret followed the text: (text node, offset 5)");

        /* The <p><br></p> shape -- what an editor leaves behind and what a
         * page's own initialiser usually writes. */
        struct node *c2 = by_id(root, "c2");
        fc_ce_caret_in(c2, 1);
        fc_ce_insert("x", 1);
        CHECK(strcmp(shape(c2), "<p>x</p>") == 0,
              "17. typing into <p><br></p> replaces the filler <br> instead of "
              "landing under it -- aiming after the <br> leaves a permanent "
              "blank first line, which is what it looks like when this is wrong");

        fc_ce_clear();
        fc_reset();
        dom_free(root);
    }

    /* ================================================================ 18 ==
     * THE EVENTS, AND THE inputType. This is the section the negative control
     * is built against, and it is the one that decides whether a React composer
     * ever learns anything happened. `input` without an inputType is, to a
     * framework, an input event about nothing. */
    {
        const char *html = "<body><div id='c' contenteditable><p id='p'>ab</p></div></body>";
        struct node *root = dom_parse(html, (int)strlen(html));
        css_apply(root, "", 0);
        struct node *c = by_id(root, "c");
        struct node *t = by_id(root, "p")->first_child;

        fc_ce_set_caret(t, 2);
        ev_clear();
        fc_ce_insert("c", 1);
        ev_dump("insert");
        CHECK(ev_n == 2, "18. an insertion raises exactly two events");
        CHECK(ev_is(0, "beforeinput@div#c") && ev_is(1, "input@div#c"),
              "18. beforeinput then input, both AT THE EDITING HOST -- which is "
              "where a page listens, not at the text node");
        CHECK(it_is(0, "insertText") && it_is(1, "insertText"),
              "18. and both carry inputType=insertText");
        CHECK(strcmp(ev_data[1], "c") == 0,
              "18. with InputEvent.data = the inserted character");

        ev_clear();
        fc_ce_backspace();
        CHECK(ev_n == 2 && it_is(0, "deleteContentBackward") &&
              it_is(1, "deleteContentBackward"),
              "18. backspace says deleteContentBackward, not insertText");
        CHECK(strcmp(ev_data[1], "-") == 0,
              "18. and carries data = null, which is what a deletion's data IS");

        fc_ce_set_caret(t, 0);
        ev_clear();
        fc_ce_delete();
        CHECK(ev_n == 2 && it_is(0, "deleteContentForward"),
              "18. Delete says deleteContentForward -- the two are different "
              "operations and a framework acts on the difference");

        ev_clear();
        fc_ce_enter(0);
        CHECK(ev_n == 2 && it_is(0, "insertParagraph"),
              "18. Enter says insertParagraph");
        ev_clear();
        fc_ce_enter(1);
        CHECK(ev_n == 2 && it_is(0, "insertLineBreak"),
              "18. and Shift+Enter says insertLineBreak");

        /* The <input> path was carrying no inputType either, and it is the
         * same wall for the same reason. */
        fc_ce_clear();
        fc_reset();
        dom_free(root);
    }

    /* ================================================================ 19 ==
     * beforeinput is CANCELABLE, and this is the only place that can be
     * honoured: a page that returns false expects the DOM not to change. */
    {
        const char *html = "<body><div id='c' contenteditable><p>ab</p></div></body>";
        struct node *root = dom_parse(html, (int)strlen(html));
        css_apply(root, "", 0);
        struct node *c = by_id(root, "c");
        struct node *t = c->first_child->first_child;

        fc_ce_set_caret(t, 2);
        ev_cancel_beforeinput = 1;
        ev_clear();
        int r = fc_ce_insert("z", 1);
        ev_cancel_beforeinput = 0;
        CHECK(r == 0, "19. a cancelled beforeinput stops the insertion");
        CHECK(strcmp(shape(c), "<p>ab</p>") == 0, "19. and the DOM is untouched");
        CHECK(ev_n == 1 && ev_is(0, "beforeinput@div#c"),
              "19. and no `input` follows -- an input event for a change that "
              "did not happen is worse than none");

        fc_ce_clear();
        fc_reset();
        dom_free(root);
    }

    /* ================================================================ 20 ==
     * Insertion into existing text, and UTF-8. The caret steps by CHARACTER;
     * backspacing one byte off a CJK character turns it into mojibake rather
     * than deleting it, which is the failure the <input> path already learned. */
    {
        const char *html = "<body><div id='c' contenteditable><p id='p'>hello world</p></div></body>";
        struct node *root = dom_parse(html, (int)strlen(html));
        css_apply(root, "", 0);
        struct node *c = by_id(root, "c");
        struct node *t = by_id(root, "p")->first_child;

        fc_ce_set_caret(t, 5);
        fc_ce_insert(",", 1);
        CHECK(strcmp(shape(c), "<p>hello, world</p>") == 0,
              "20. a character inserted in the MIDDLE of a run lands there");

        fc_ce_set_caret(t, t->textlen);
        ce_typ("\xe4\xbd\xa0\xe5\xa5\xbd");         /* U+4F60 U+597D, 3 bytes each */
        CHECK(strcmp(shape(c), "<p>hello, world\xe4\xbd\xa0\xe5\xa5\xbd</p>") == 0,
              "20. and multi-byte UTF-8 goes in whole");
        fc_ce_backspace();
        CHECK(strcmp(shape(c), "<p>hello, world\xe4\xbd\xa0</p>") == 0,
              "20. backspace removes a whole CHARACTER (3 bytes), not one byte -- "
              "one byte would leave a broken sequence the rasterizer cannot decode");

        fc_ce_clear();
        fc_reset();
        dom_free(root);
    }

    /* ================================================================ 21 ==
     * Deletion ACROSS BOUNDARIES -- the part an <input>'s flat string has no
     * equivalent of at all. Backspace at the start of a run must reach into the
     * previous run; at the start of a paragraph it must delete the PARAGRAPH
     * BREAK and join the two. */
    {
        const char *html =
            "<body><div id='c' contenteditable>"
            "<p id='p1'>one</p><p id='p2'>two</p>"
            "<p id='p3'>a<b id='b'>B</b>c</p>"
            "</div></body>";
        struct node *root = dom_parse(html, (int)strlen(html));
        css_apply(root, "", 0);
        struct node *c = by_id(root, "c");

        /* text-node boundary, same paragraph: the caret is at the start of "c",
         * and what backspace must eat is the "B" inside the <b>. */
        struct node *tc = by_id(root, "b")->next;
        fc_ce_set_caret(tc, 0);
        fc_ce_backspace();
        CHECK(strcmp(shape(by_id(root, "p3")), "a<b></b>c") == 0,
              "21. backspace at the start of a run deletes from the PREVIOUS "
              "run, crossing an element boundary to do it");

        /* paragraph boundary */
        struct node *t2 = by_id(root, "p2")->first_child;
        fc_ce_set_caret(t2, 0);
        ev_clear();
        fc_ce_backspace();
        CHECK(strcmp(shape(c), "<p>onetwo</p><p>a<b></b>c</p>") == 0,
              "21. backspace at the START of a paragraph deletes the break and "
              "MERGES the two -- one <p>, not two, and no text lost");
        CHECK(it_is(0, "deleteContentBackward"),
              "21. still reported as deleteContentBackward");
        struct node *cn = 0; int co = -1;
        fc_ce_selection(&cn, &co, 0, 0);
        CHECK(cn && cn->type == N_TEXT && co == 3 && cn->text[0] == 'o',
              "21. and the caret sits at the join, offset 3 of `onetwo`");

        /* forward Delete does the same in the other direction. The caret goes
         * to the end of the paragraph with fc_ce_end and NOT with an offset:
         * the merge left `onetwo` as TWO text nodes in one <p> (that is what a
         * merge is), so offset 6 of the node holding `one` is past its end. */
        fc_ce_end(0);
        fc_ce_delete();
        CHECK(strcmp(shape(c), "<p>onetwoa<b></b>c</p>") == 0,
              "21. Delete at the END of a paragraph pulls the NEXT one up");

        fc_ce_clear();
        fc_reset();
        dom_free(root);
    }

    /* ================================================================ 22 ==
     * Enter. Which of the two spec-permitted behaviours this is, stated in
     * forms.c: a plain Enter SPLITS THE BLOCK. */
    {
        const char *html =
            "<body><div id='c' contenteditable><p id='p'>abcd</p></div></body>";
        struct node *root = dom_parse(html, (int)strlen(html));
        css_apply(root, "", 0);
        struct node *c = by_id(root, "c");
        struct node *t = by_id(root, "p")->first_child;

        fc_ce_set_caret(t, 2);
        fc_ce_enter(0);
        CHECK(strcmp(shape(c), "<p>ab</p><p>cd</p>") == 0,
              "22. Enter splits the paragraph in two at the caret");
        struct node *cn = 0; int co = -1;
        fc_ce_selection(&cn, &co, 0, 0);
        CHECK(cn && cn->type == N_TEXT && co == 0 && cn->text[0] == 'c',
              "22. and the caret is at the START of the new one");
        ce_typ("X");
        CHECK(strcmp(shape(c), "<p>ab</p><p>Xcd</p>") == 0,
              "22. so the next keystroke lands there");

        /* at the very end of a paragraph: the new one is empty and needs a
         * filler <br>, or it collapses to no line box and the caret vanishes */
        fc_ce_selection(&cn, &co, 0, 0);
        fc_ce_set_caret(cn, cn->textlen);
        fc_ce_enter(0);
        CHECK(strcmp(shape(c), "<p>ab</p><p>Xcd</p><p><br></p>") == 0,
              "22. Enter at the end makes an EMPTY paragraph with a filler <br> "
              "-- without it the block has no line box and the caret has no "
              "height to be drawn at");
        ce_typ("y");
        CHECK(strcmp(shape(c), "<p>ab</p><p>Xcd</p><p>y</p>") == 0,
              "22. and the filler goes away as soon as it has content");

        /* Shift+Enter */
        fc_ce_selection(&cn, &co, 0, 0);
        fc_ce_set_caret(cn, cn->textlen);
        fc_ce_enter(1);
        ce_typ("z");
        CHECK(strcmp(shape(c), "<p>ab</p><p>Xcd</p><p>y<br>z</p>") == 0,
              "22. Shift+Enter inserts a <br> INSIDE the paragraph instead");

        fc_ce_clear();
        fc_reset();
        dom_free(root);
    }

    /* ================================================================ 23 ==
     * Enter with no paragraph to split -- a bare <div contenteditable> whose
     * content is loose inline text, which is how the simplest composers ship.
     * Splitting inside inline formatting is the case that loses the <b> if the
     * chain is not re-created on the right-hand side. */
    {
        const char *html = "<body><div id='c' contenteditable>abcd</div></body>";
        struct node *root = dom_parse(html, (int)strlen(html));
        css_apply(root, "", 0);
        struct node *c = by_id(root, "c");
        fc_ce_set_caret(c->first_child, 2);
        fc_ce_enter(0);
        CHECK(strcmp(shape(c), "<div>ab</div><div>cd</div>") == 0,
              "23. with no paragraph to split, the host's inline content becomes "
              "two of them -- what a real browser does on the first Enter");

        const char *html2 = "<body><div id='d' contenteditable><p>x<b>bold</b>y</p></div></body>";
        struct node *r2 = dom_parse(html2, (int)strlen(html2));
        css_apply(r2, "", 0);
        struct node *d = by_id(r2, "d");
        struct node *bold = d->first_child->first_child->next->first_child;
        fc_ce_set_caret(bold, 2);
        fc_ce_enter(0);
        CHECK(strcmp(shape(d), "<p>x<b>bo</b></p><p><b>ld</b>y</p>") == 0,
              "23. and a split INSIDE a <b> re-creates the <b> on the right -- "
              "moving the tail up to the paragraph would silently unbold it");

        fc_ce_clear();
        fc_reset();
        dom_free(r2);
        dom_free(root);
    }

    /* ================================================================ 24 ==
     * Caret movement and Shift-selection, and typing over a selection. */
    {
        const char *html =
            "<body><div id='c' contenteditable>"
            "<p id='p1'>hello</p><p id='p2'>world</p></div></body>";
        struct node *root = dom_parse(html, (int)strlen(html));
        css_apply(root, "", 0);
        struct node *c = by_id(root, "c");
        struct node *t1 = by_id(root, "p1")->first_child;
        struct node *t2 = by_id(root, "p2")->first_child;
        struct node *cn = 0, *en = 0;
        int co = 0, eo = 0;

        fc_ce_set_caret(t1, 0);
        fc_ce_move(+1, 0, 0);
        fc_ce_selection(&cn, &co, 0, 0);
        CHECK(cn == t1 && co == 1, "24. ArrowRight steps one character");
        fc_ce_move(-1, 0, 0);
        fc_ce_selection(&cn, &co, 0, 0);
        CHECK(cn == t1 && co == 0, "24. ArrowLeft steps back");
        fc_ce_move(-1, 0, 0);
        fc_ce_selection(&cn, &co, 0, 0);
        CHECK(cn == t1 && co == 0, "24. and stops at the start of the host");

        /* CROSSING, and the two cases are different on purpose. Across a
         * PARAGRAPH the end of one line and the start of the next are two
         * places, so the crossing is the whole move. */
        fc_ce_set_caret(t1, 5);
        fc_ce_move(+1, 0, 0);
        fc_ce_selection(&cn, &co, 0, 0);
        CHECK(cn == t2 && co == 0,
              "24. ArrowRight at the end of a paragraph lands at the START of "
              "the next one -- the paragraph break is a position");
        fc_ce_move(-1, 0, 0);
        fc_ce_selection(&cn, &co, 0, 0);
        CHECK(cn == t1 && co == 5,
              "24. and one ArrowLeft comes back -- one press each way. An "
              "arrow pair that does not undo itself reads as a stuck caret");

        /* Inside ONE paragraph the two sides of a run boundary are the same
         * place, so the caret must consume a character as well or the arrow
         * would appear not to move. */
        {
            const char *h2 = "<body><div id='d' contenteditable><p>a<b>B</b>c</p></div></body>";
            struct node *r2 = dom_parse(h2, (int)strlen(h2));
            css_apply(r2, "", 0);
            struct node *d = by_id(r2, "d");
            struct node *ta = d->first_child->first_child;      /* "a" */
            struct node *tb = ta->next->first_child;            /* "B" inside <b> */
            struct node *x = 0; int xo = 0;
            fc_ce_set_caret(ta, 1);
            fc_ce_move(+1, 0, 0);
            fc_ce_selection(&x, &xo, 0, 0);
            CHECK(x == tb && xo == 1,
                  "24. crossing a run boundary INSIDE a paragraph consumes a "
                  "character too -- the two sides of it are one place on screen");
            fc_ce_move(-1, 0, 0);
            fc_ce_move(-1, 0, 0);
            fc_ce_selection(&x, &xo, 0, 0);
            CHECK(x == ta && xo == 0,
                  "24. and two presses back is where two presses forward came "
                  "from: the step count is symmetric");
            fc_ce_clear();
            dom_free(r2);
        }

        /* Home / End are paragraph-scoped: stated in forms.h, asserted here. */
        fc_ce_set_caret(t2, 3);
        fc_ce_home(0);
        fc_ce_selection(&cn, &co, 0, 0);
        CHECK(cn == t2 && co == 0, "24. Home goes to the start of the paragraph");
        fc_ce_end(0);
        fc_ce_selection(&cn, &co, 0, 0);
        CHECK(cn == t2 && co == 5, "24. End to its end -- and NOT into the next "
              "paragraph, which is what makes it a paragraph and not a document");

        /* Shift-arrow */
        fc_ce_set_caret(t2, 0);
        fc_ce_move(+1, 0, 1);
        fc_ce_move(+1, 0, 1);
        fc_ce_move(+1, 0, 1);
        CHECK(fc_ce_collapsed() == 0, "24. Shift+Arrow makes a selection");
        char sel[64];
        fc_ce_selection_text(sel, sizeof sel);
        CHECK(strcmp(sel, "wor") == 0, "24. of the three characters it crossed");
        fc_ce_move(-1, 0, 1);
        fc_ce_selection_text(sel, sizeof sel);
        CHECK(strcmp(sel, "wo") == 0,
              "24. and Shift+Left SHRINKS it -- the anchor is the fixed end, "
              "which is the thing a caret model gets wrong by default");

        ev_clear();
        fc_ce_insert("W", 1);
        CHECK(strcmp(shape(c), "<p>hello</p><p>Wrld</p>") == 0,
              "24. typing REPLACES the selection");
        CHECK(ev_n == 2, "24. as ONE edit, so one beforeinput and one input");
        CHECK(it_is(1, "insertText"), "24. reported as insertText");

        /* a selection spanning paragraphs, deleted */
        fc_ce_set_range(t1, 2, t2, 2);
        fc_ce_selection_text(sel, sizeof sel);
        CHECK(strcmp(sel, "lloWr") == 0,
              "24. a selection can span paragraphs and reads back whole");
        fc_ce_backspace();
        CHECK(strcmp(shape(c), "<p>held</p>") == 0,
              "24. and deleting it joins them -- a selection that crossed a "
              "paragraph break must not leave the break behind");

        /* A BACKWARDS selection -- dragged or Shift+Left'ed from right to left.
         * Its own scope, because the merged paragraph above holds two text
         * nodes (that is what a merge leaves) and an offset of 4 into the first
         * of them is past its end. */
        {
            const char *h3 = "<body><div id='d' contenteditable><p>abcdef</p></div></body>";
            struct node *r3 = dom_parse(h3, (int)strlen(h3));
            css_apply(r3, "", 0);
            struct node *t = by_id(r3, "d")->first_child->first_child;
            struct node *a = 0, *f = 0; int ao = 0, fo = 0;
            fc_ce_set_range(t, 4, t, 1);
            CHECK(fc_ce_anchor_focus(&a, &ao, &f, &fo) && ao == 4 && fo == 1,
                  "24. a BACKWARDS selection keeps anchor AFTER focus -- the "
                  "distinction the Selection API exists to express");
            fc_ce_selection(&a, &ao, &f, &fo);
            CHECK(ao == 1 && fo == 4,
                  "24. ...while the ordered pair every consumer wants is 1..4");
            char s3[32];
            fc_ce_selection_text(s3, sizeof s3);
            CHECK(strcmp(s3, "bcd") == 0, "24. and it reads back forwards");
            fc_ce_clear();
            dom_free(r3);
        }

        fc_ce_select_all(c);
        fc_ce_selection_text(sel, sizeof sel);
        CHECK(strcmp(sel, "held") == 0, "24. select-all covers the whole host");

        fc_ce_clear();
        fc_reset();
        dom_free(root);
    }

    /* ================================================================ 25 ==
     * THE RECYCLED SLOT. A React composer is torn down and rebuilt constantly.
     * Without the serial check this file would hold a pointer into a slot that
     * now belongs to a different node and type into it. */
    {
        const char *html = "<body><div id='c' contenteditable><p id='p'>abc</p></div></body>";
        struct node *root = dom_parse(html, (int)strlen(html));
        css_apply(root, "", 0);
        struct node *p = by_id(root, "p");
        struct node *t = p->first_child;
        fc_ce_set_caret(t, 1);
        CHECK(fc_ce_selection(0, 0, 0, 0) == 1, "25. the caret is live");
        dom_destroy_subtree(p);
        CHECK(fc_ce_selection(0, 0, 0, 0) == 0,
              "25. and is GONE the moment its node is destroyed -- reported as "
              "`no caret`, which is what a real browser does too, rather than "
              "as a caret in whatever element got the recycled slot");
        CHECK(fc_ce_insert("x", 1) == 0,
              "25. so a keystroke after the teardown does nothing at all");

        fc_ce_clear();
        fc_reset();
        dom_free(root);
    }

    /* ================================================================ 26 ==
     * The PATH encoding, which is how a position crosses into JavaScript.
     * js_forms.c cannot turn a `struct node *` into a JS wrapper and a text
     * node cannot even carry the attribute the element workaround uses, so the
     * position travels as child indices from the document element. A round trip
     * that does not land on the same node is a Selection API that reports the
     * caret in the wrong place. */
    {
        const char *html =
            "<body><div id='c' contenteditable><p>one</p><p id='p2'>two</p></div></body>";
        struct node *root = dom_parse(html, (int)strlen(html));
        css_apply(root, "", 0);
        struct node *t2 = by_id(root, "p2")->first_child;
        fc_ce_set_root(root);
        fc_ce_set_caret(t2, 2);

        int path[16], off = -1;
        int d = fc_ce_path(1, path, 16, &off);
        CHECK(d > 0 && off == 2, "26. the focus position encodes to a path");
        printf("   path depth %d ->", d);
        for (int i = 0; i < d; i++) printf(" %d", path[i]);
        printf(" @%d\n", off);

        fc_ce_clear();
        CHECK(fc_ce_selection(0, 0, 0, 0) == 0, "26. (caret cleared)");
        fc_ce_set_root(root);
        CHECK(fc_ce_set_paths(path, d, 2, path, d, 2) == 1,
              "26. and decodes again with no caret to start from -- which is the "
              "call that matters, because it is the one a page makes when it "
              "places the caret itself");
        struct node *cn = 0; int co = -1;
        fc_ce_selection(&cn, &co, 0, 0);
        CHECK(cn == t2 && co == 2, "26. onto the SAME text node and offset");

        fc_ce_clear();
        fc_reset();
        dom_free(root);
    }

    /* ================================================================ 27 ==
     * The <input> path gained the same inputType, and it is the same wall for
     * the same reason: a framework-managed <input> reads it too. */
    {
        const char *html = "<body><input id='q' value='ab'></body>";
        struct node *root = dom_parse(html, (int)strlen(html));
        css_apply(root, "", 0);
        struct node *q = by_id(root, "q");
        focus_set_quiet(q);
        fc_set_selection(q, 2, 2);

        ev_clear();
        fc_edit_insert(q, "c", 1);
        CHECK(it_is(0, "insertText") && it_is(1, "insertText"),
              "27. an <input> keystroke reports insertText on both events");
        CHECK(strcmp(ev_data[1], "c") == 0, "27. with the character as data");
        ev_clear();
        fc_edit_backspace(q);
        CHECK(it_is(1, "deleteContentBackward"),
              "27. and its backspace reports deleteContentBackward");
        ev_clear();
        fc_set_selection(q, 0, 2);
        fc_edit_replace(q, "", 0, "deleteByCut");
        CHECK(it_is(1, "deleteByCut"),
              "27. a CUT is a deletion, not an insertion of nothing -- calling "
              "it insertText is exactly the kind of wrong answer a framework "
              "acts on");

        focus_reset();
        fc_reset();
        dom_free(root);
    }

    printf("\nforms_test: %d checks, %s\n", checks, fail ? "FAILURES" : "ALL PASS");
    return fail;
}
