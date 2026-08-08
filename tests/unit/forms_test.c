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

static void ev_clear(void) { ev_n = 0; }

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

int main(void)
{
    fc_set_dispatch(rec);

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

    printf("\nforms_test: %d checks, %s\n", checks, fail ? "FAILURES" : "ALL PASS");
    return fail;
}
