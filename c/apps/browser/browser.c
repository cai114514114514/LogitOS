#include "logit.h"
#include "dom.h"
#include "css.h"
#include "layout.h"
#include "browser_paint.h"
#include "js_dom.h"
#include "../../../include/weaksym.h"   /* the weak declarations below are an ELF idiom */
#include "js_page.h"
#include "js_module.h"           /* <script type="module"> + the module loader */
/* js_webapi.o is absent from browser-nofetch.aex (the negative control for
 * test-webapi-page), so every entry point has to be weak here exactly as it is
 * in js_page.c -- otherwise that link breaks. */
#define JS_WEBAPI_OPTIONAL
#include "js_webapi.h"           /* script-initiated navigation (location.*) */
/* THE NEGATIVE CONTROL, and it costs one #define because the control IS the
 * behaviour that shipped yesterday. -DBROWSER_NO_FOCUS compiles the routing
 * out: no element takes focus from a click, Tab does not move it, and a
 * keystroke goes to <body> exactly as it did before this change. The device
 * test (tests/qmp/qmp_forms.py --expect-no-focus) must FAIL against that build,
 * and if it does not then it is not measuring the focus model. */
#ifdef BROWSER_NO_FOCUS
#define FOCUS_ROUTING 0
#else
#define FOCUS_ROUTING 1
#endif

#include "forms.h"               /* form control state + submission */
#include "focus.h"               /* the focused element and Tab navigation */

/* js_forms.c's teardown: it holds the page's JSContext for the editing-event
 * dispatcher, and that context dies with the page. Weak because that
 * translation unit is absent from BROWSER_PIPE and from the host loader test,
 * where there is nothing to clean up. */
void js_forms_cleanup(void) LOGIT_WEAK;
LOGIT_WEAK_STUB(js_forms_cleanup);

/* The CSS animation clock's engine hooks (js_anim.c part 2). Weak for the
 * same reason: the host loader test links no js_anim.o and must keep the
 * pre-clock behaviour exactly. css_anim_tick is called by js_page_run_due
 * -- the tick rides the page's one deadline queue -- and this file reads
 * the frame kind back after it. */
void css_anim_reset(void) LOGIT_WEAK;
void css_anim_snapshot(struct node *root) LOGIT_WEAK;
int  css_anim_needs_layout(void) LOGIT_WEAK;
LOGIT_WEAK_STUB(css_anim_reset);
LOGIT_WEAK_STUB(css_anim_snapshot);
LOGIT_WEAK_STUB(css_anim_needs_layout);

#include "bfetch.h"              /* the pooled ring-3 resource fetcher */
#include "tabs.h"                /* per-tab state, session, history, bookmarks */
#include "url.h"                 /* url_parse + url_resolve for link clicks */
#include "css_report.h"          /* the ONE accounting of the stylesheet pipeline */
#include <stdlib.h>              /* malloc/realloc/free -- resources are sized to fit */
#include <string.h>

/* A web browser. The whole render pipeline runs in this ring-3 app: the
 * kernel does DNS+TCP+TLS+HTTP (SYS_HTTP_GET) and hands us the raw body
 * (SYS_HTTP_BODY); we parse HTML->DOM (dom.c), apply CSS (css_engine.c), lay
 * out a display list (layout.c) and paint it via the GUI render syscalls
 * (browser_paint.c).
 *
 * The page's JavaScript is LIVE: js_page.c holds a runtime that is opened with
 * the document and closed on navigation, so a handler registered by an inline
 * <script> is still there when the user clicks a minute later. This loop is
 * that runtime's event loop -- it delivers input as DOM events, services due
 * timers, drains the microtask queue, and only then performs the DEFAULT ACTION
 * (following a link, scrolling) if no listener called preventDefault(). */

/* http_get error codes (mirror include/http.h) */
#define HTTP_ERR_URL  -2
#define HTTP_ERR_DNS  -3
#define HTTP_ERR_CONN -4
#define HTTP_ERR_TLS  -5

#define WINW 1180                        /* the size the window is BORN at */
#define WINH 620
#define BARH 30
#define TABH 30                          /* the tab strip, above the address bar */

/* The window is resizable (EV_RESIZE, see include/abi/logit_abi.h) and the tab
 * strip has to lay out at any width, so nothing below may derive geometry from
 * WINW/WINH. These two are the truth, and they are updated from EV_RESIZE and
 * from SYS_GUI_WIN_STATE at startup. */
static int win_w = WINW, win_h = WINH;

#define VIEW_Y   (TABH + BARH)
#define VIEW_H   (win_h - VIEW_Y - 18)   /* viewport (below the bars, above status) */

/* ---- the two numbers the main loop's sleep is allowed to invent -----------
 *
 * Everything else the loop waits for has a real deadline (a JS timer) or a
 * real wakeup (an event). These two cover the one thing that has neither: work
 * inside js_webapi.c -- a fetch stepping its socket, an EventSource waiting to
 * reconnect -- which is driven by js_page_run_due() and exposes no time.
 *
 * BROWSER_PUMP_MS is 10 because that is the kernel's tick: monotonic_ms()
 * advances in steps of ten (see logit.h), so anything smaller asks for a
 * shorter sleep than the clock can express and gets a tick anyway. It bounds
 * the sleep ONLY while js_webapi_pending() is true, so an idle page with a
 * five-second setInterval still sleeps five seconds.
 *
 * BROWSER_WAIT_MAX_MS is not a poll interval -- it is a ceiling on the int the
 * syscall takes, so a page that schedules a timer an hour out cannot turn into
 * an overflowed negative timeout (which wm.c reads as "no timeout"). One
 * syscall a second when nothing at all is happening is not measurable. */
#define BROWSER_PUMP_MS      10
#define BROWSER_WAIT_MAX_MS  1000

/* THE START PAGE. This used to be "http://example.com/" -- plain http, on a
 * machine with a real TLS 1.3 stack, and typed-over rather than edited (see
 * ucaret/usel below). Both were the owner's complaints verbatim: "it opens on
 * example" and "you have to type http/https by hand".
 *
 * Empty, not a homepage: the browser boots exactly like Ctrl+T / a new tab
 * does elsewhere in this file (url[0]=0, ulen=0, editing=1, a status line
 * inviting a URL) rather than inventing a second "first tab" behaviour next
 * to the one that already exists for every tab after it. That also sidesteps
 * fabricating a local homepage page with its own fetch/render path.
 *
 * example.com is UNCHANGED as a URL a person or a harness can navigate to --
 * this only changes what greets a fresh boot. It stays the site scoreboard's
 * control row: tests/qmp/qmp_site.py and every scoreboard driver reach it by
 * pressing Ctrl+L and typing the full URL, never by reading this default, so
 * the control keeps measuring the same page it always has. */
static char url[600] = "";
static int  ulen = 0;
/* The address bar's caret and selection, both BYTE offsets into url[] (UTF-8
 * stepped, like every other caret in this file -- see ce_step/fc_edit's own
 * comments on why "the last character" cannot mean "the last byte"). usel is
 * the selection anchor; usel == ucaret means no selection, exactly like the
 * contenteditable and form-control caret models this mirrors. Before this,
 * the address bar had NO caret at all and its own comment said so ("append-
 * at-end only... no caret to move") -- KEY_LEFT/RIGHT navigated history
 * instead of moving it, and Backspace could only ever delete the last
 * character, which is the owner's fourth complaint: clearing a Google search
 * URL's tracking suffix took on the order of 60 backspaces. */
static int  ucaret = 0;
static int  usel = 0;

/* THIS MIRRORS forms.c's fc_edit_* / fc_ce_* (same UTF-8-by-character
 * stepping, same anchor/focus shape for a shift-extended selection) rather
 * than CALLING it: forms.c's caret lives inside a struct fctl hung off a DOM
 * text node, and this bar is a flat char[600] with no DOM node at all, torn
 * down and refilled by session restore, tab switching, Ctrl+L, the
 * history/bookmark panel and script navigation -- none of which forms.c's
 * lifetime rules have any business governing. There is no INVARIANT shared
 * between the two to make this a "one jar, two doors" risk, only a
 * stateless ALGORITHM (skip UTF-8 continuation bytes), safe to have twice
 * for the same reason step_left/step_right themselves are five lines: a
 * second copy of five lines does not rot the way a second copy of a NUMBER
 * does.
 *
 * EVERY call site that replaces the WHOLE address -- a navigation
 * completing, a tab switch, Ctrl+T/Ctrl+W opening an empty bar, a picked
 * history/bookmark row, a same-document pushState/hash move -- assigns
 * url[]/ulen directly and then MUST call addr_sync(), below, to put the
 * caret back in bounds. Forgetting it is not silent: the caret would sit
 * where the OLD text ended, which is either inside the new text (harmless
 * looking, wrong) or past its end (an out-of-bounds draw the moment editing
 * resumes) -- so a call site that forgets is left for the next reader to
 * find by the caret landing somewhere that is visibly not the end of the
 * new address. */

/* Caret to the end, selection collapsed. Call after any direct url[]/ulen
 * assignment -- see the block comment above. (The rest of this bar's
 * editing primitives -- addr_step/addr_insert/addr_backspace/addr_move/
 * addr_home/addr_end/addr_select_all -- live further down, next to
 * hist_go/set_status, where a SECOND concurrent pass at this same file also
 * landed one; this one function stayed here because hist_go, right below,
 * already depends on it and hist_go is defined before that block.) */
static void addr_sync(void) { ucaret = ulen; usel = ulen; }

static int  scroll;                      /* pixel scroll offset */
static int  ph;                          /* laid-out page height */
static char status[96] = "ready -- Enter loads; Cmd+T new tab, Ctrl+Tab switches";
static struct node *g_root;              /* current page DOM (owns display-list strings) */

/* ===================== history: now PER TAB, not global =====================
 *
 * These three used to own a `char hist[32][600]` of their own. They now
 * delegate to the active tab, and that is the whole of the change at the call
 * sites -- follow_link, the address bar, the redirect chain and the arrow keys
 * all still say hist_push/hist_replace/hist_go and all of them are now about
 * the tab the user is looking at.
 *
 * Doing it this way rather than threading a `struct tab *` through every caller
 * is deliberate: a back/forward stack that belongs to the wrong tab is a bug
 * you find by clicking Back, and the smaller the diff at the call sites the
 * fewer places that bug can hide. */
static void hist_push(const char *u)    { tab_hist_push(tab_cur(), u); }
static void hist_replace(const char *u) { tab_hist_replace(tab_cur(), u); }

/* -1 back, +1 forward. Returns 0 (nothing to do), 1 (a real navigation
 * happened -- url[] holds the target and the caller MUST call load()), or 2
 * (a same-document move happened -- url[] holds the new address, a popstate
 * is already queued for the next pump, and the caller must NOT call load():
 * the DOM, the JS heap and every running timer are exactly as they were).
 *
 * The joint session history this button walks is split across two modules
 * that cannot see each other's counter (tabs.c's per-tab hist[] for full
 * document loads, js_webapi.c's g_hist[] for pushState/replaceState/fragment
 * entries within whatever document is CURRENTLY loaded -- see tabs.c's
 * comment above tab_hist_behind for the whole shape). A page using
 * history.pushState expects the physical Back button to undo its OWN last
 * route change first -- firing popstate, no reload -- before it ever reaches
 * the previous full document, so the same-document half is tried FIRST and
 * the full-document stack is the fallback, not the other way around. */
static int hist_go(int delta)
{
    if (LOGIT_HAVE(js_webapi_hist_step)) {
        char u[600];
        if (js_webapi_hist_step(js_page_ctx(), delta, u, (int)sizeof u)) {
            int i = 0; while (u[i] && i < (int)sizeof url - 1) { url[i] = u[i]; i++; }
            url[i] = 0; ulen = i; addr_sync();
            return 2;
        }
    }
    if (!tab_hist_go(tab_cur(), delta, url, (int)sizeof url)) return 0;
    ulen = 0; while (url[ulen]) ulen++;
    addr_sync();
    return 1;
}

static void set_status(const char *s)
{ int i = 0; while (s[i] && i < (int)sizeof status - 1) { status[i] = s[i]; i++; } status[i] = 0; }

/* ---- the address bar's caret + selection, over the flat url[]/ulen buffer -
 *
 * Mirrors control_key()/fc_edit_* (a form field's caret) and ce_key()/fc_ce_*
 * (a contenteditable's caret) -- same model, byte offset + UTF-8 stepping,
 * anchor-and-caret selection -- but over url[]/ulen rather than a form
 * control, because there is no <input> node behind the chrome's own address
 * bar for forms.c to own. One caret MODEL, three owners of a buffer each;
 * not a fourth model invented for this one field. */

/* Step one UTF-8 character in url[], the same continuation-byte walk the old
 * backspace-only code used ("delete a whole character, not one byte" -- a
 * mid-string edit needs the same care the end-only one already had). */
static void addr_step(int *p, int dir)
{
    int q = *p;
    if (dir < 0) { if (q > 0) { q--; while (q > 0 && ((unsigned char)url[q] & 0xC0) == 0x80) q--; } }
    else         { if (q < ulen) { q++; while (q < ulen && ((unsigned char)url[q] & 0xC0) == 0x80) q++; } }
    *p = q;
}

static int addr_word_char(unsigned char c)
{ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

static void addr_select_all(void) { usel = 0; ucaret = ulen; }

/* Remove bytes [a,b) from url[], a<=b, both clamped. Selection-agnostic --
 * callers collapse ucaret/usel themselves, same split as fc_edit_insert. */
static void addr_delete_range(int a, int b)
{
    if (a < 0) a = 0; if (b > ulen) b = ulen; if (a >= b) return;
    int i = a, j = b;
    while (j < ulen) url[i++] = url[j++];
    ulen = i; url[ulen] = 0;
}

/* Insert `s` (sl bytes) at the caret, REPLACING the selection first if there
 * is one -- typing over a selection is what every text field does, and it is
 * also the fast path for "delete this whole tracking suffix": select it,
 * type the replacement (or nothing). */
static void addr_insert(const char *s, int sl)
{
    int a = ucaret < usel ? ucaret : usel;
    int b = ucaret < usel ? usel : ucaret;
    if (b > a) { addr_delete_range(a, b); ucaret = usel = a; }
    if (sl <= 0) return;
    if (ulen + sl > (int)sizeof url - 1) sl = (int)sizeof url - 1 - ulen;
    if (sl <= 0) return;
    for (int i = ulen - 1; i >= ucaret; i--) url[i + sl] = url[i];
    for (int i = 0; i < sl; i++) url[ucaret + i] = s[i];
    ulen += sl; url[ulen] = 0;
    ucaret += sl; usel = ucaret;
}

/* Backspace: the selection if there is one, else one UTF-8 character to the
 * left of the caret -- which, now that the caret can be anywhere, is no
 * longer necessarily "the last character of url[]". */
static void addr_backspace(void)
{
    int a = ucaret < usel ? ucaret : usel;
    int b = ucaret < usel ? usel : ucaret;
    if (b > a) { addr_delete_range(a, b); ucaret = usel = a; return; }
    if (ucaret <= 0) return;
    int p = ucaret; addr_step(&p, -1);
    addr_delete_range(p, ucaret);
    ucaret = usel = p;
}

/* Left/Right. `ctrl` jumps a word (delimiter run then word run, same shape as
 * fc_edit_move's ctrl case); `shift` extends the selection instead of moving
 * it. Collapsing an existing selection without shift goes to its near/far
 * edge, not one character past the caret -- what every real text field does,
 * and what a person expects after Ctrl+A then Right. */
static void addr_move(int dir, int ctrl, int shift)
{
    if (!shift && ucaret != usel) {
        int p = dir < 0 ? (ucaret < usel ? ucaret : usel) : (ucaret > usel ? ucaret : usel);
        ucaret = usel = p;
        return;
    }
    int p = ucaret;
    if (ctrl) {
        if (dir < 0) {
            while (p > 0 && !addr_word_char((unsigned char)url[p - 1])) p--;
            while (p > 0 && addr_word_char((unsigned char)url[p - 1])) p--;
        } else {
            while (p < ulen && !addr_word_char((unsigned char)url[p])) p++;
            while (p < ulen && addr_word_char((unsigned char)url[p])) p++;
        }
    } else {
        addr_step(&p, dir);
    }
    ucaret = p;
    if (!shift) usel = p;
}

static void addr_home(int shift) { ucaret = 0;    if (!shift) usel = ucaret; }
static void addr_end(int shift)  { ucaret = ulen;  if (!shift) usel = ucaret; }

/* Forward-delete (the Delete key, delivered as 0x7f -- see ce_key()'s
 * `case 0x7f` for the same convention on a contenteditable). A selection is
 * deleted whole, same as addr_backspace(); otherwise the character AT the
 * caret goes, not before it. */
static void addr_delete_fwd(void)
{
    int a = ucaret < usel ? ucaret : usel;
    int b = ucaret < usel ? usel : ucaret;
    if (b > a) { addr_delete_range(a, b); ucaret = usel = a; return; }
    if (ucaret >= ulen) return;
    int p = ucaret; addr_step(&p, +1);
    addr_delete_range(ucaret, p);
    usel = ucaret;
}

/* Copy the selected bytes out for Ctrl+C/Ctrl+X, for clip_set() -- 0 for an
 * empty (collapsed) selection, same as forms.c's fc_ce_selection_text(). */
static int addr_selection_text(char *buf, int max)
{
    int a = ucaret < usel ? ucaret : usel;
    int b = ucaret < usel ? usel : ucaret;
    int n = b - a; if (n > max) n = max; if (n < 0) n = 0;
    for (int i = 0; i < n; i++) buf[i] = url[a + i];
    return n;
}

static void redraw(int editing);
/* The page-text selection's anchor/focus reset -- defined with the rest of
 * that machinery further down (it needs the display-list helpers), declared
 * here because the navigation teardown above load_once()'s two dom_free()
 * call sites is the earliest caller in the file. */
static void psel_clear(void);
/* The open <select>'s list. Drawn last, over everything, because the display
 * list has no z-order above itself -- see the popup section further down. */
static void draw_select_popup(void);
/* Dismiss it. Called from the two teardown paths as well, which run long before
 * the popup section itself. */
static void popup_close(void);

int printf(const char *, ...);
unsigned long strlen(const char *);

#ifdef LOADERHOST_LOGIT_H
/* THE HOST LOADER HARNESS HAS NO WINDOW TO SLEEP ON. tests/unit/loaderhost's
 * logit.h replaces the window with five recorders and stubs sys_yield() to
 * nothing; it has no wait_idle(), and there is nothing there for one to park
 * on -- test-loader drives load() directly and never enters app_main's loop.
 * Shimmed HERE rather than in the harness header so the whole change lives in
 * one file, and it is a no-op for the same reason sys_yield() is one. */
static inline void wait_idle(int ms) { (void)ms; }
#endif

/* js_page.c is written against an injected clock so the host tests can step
 * time by hand; in the OS it is the kernel's 100 Hz monotonic counter. */
static unsigned long long clock_ms(void) { return monotonic_ms(); }

/* ---- DOM helpers: collect <style>/<script> text (moved from the kernel) ---- */
static int tag_is(const char *t, const char *lit){ int i=0; for(;lit[i];i++) if(t[i]!=lit[i]) return 0; return t[i]==0; }

/* THE MEDIA ATTRIBUTE, ON EITHER DOOR. Neither collect_style's <style media=""> nor
 * collect_css_links' <link media=""> ever read this attribute -- verified by grep:
 * zero occurrences of dom_attr(*, "media") anywhere in this file before this change.
 * The result is not "some CSS is missing", it is worse: a stylesheet meant for
 * print, speech, or a narrow viewport was concatenated into author_css and applied
 * UNCONDITIONALLY, so a page that ships one screen sheet and one narrow/mobile
 * override sheet (a very ordinary responsive-CSS pattern, not specific to any one
 * site) has the override win regardless of the real window width -- indistinguishable
 * from "the CSS never arrived" from outside, because css_report.c's counters all
 * read exactly as if every declaration were kept: no request failed, nothing was
 * dropped by the parser, the rule matched and applied -- to the wrong medium.
 *
 * The fix routes the attribute through LibCSS's OWN @media matching (already used
 * for <style>/<link> content that embeds an @media block, and already exercised by
 * css_select_ctx_media_matches at the media-query test above) rather than inventing
 * a second evaluator here: wrap the linked/inline text in `@media <value> { ... }`
 * before it reaches author_css, and the cascade's existing spec-correct media
 * matching decides whether it counts -- ONE evaluator, not two that could disagree. */
static int str_eq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}
/* Empty and "all" are the two spellings of "unconditional" (HTML's default
 * attribute value is the empty string, meaning "all"); wrapping those adds bytes
 * for zero behavioural change, so they are the two cases that skip it.
 *
 * MEDIA_ATTR_IGNORE is the negative control (tests/loader.mk's test-media-negctl):
 * built with it defined, this reverts to exactly what shipped before this
 * change -- every <style>/<link>'s media attribute read and then thrown away,
 * both doors unconditional again. */
static int media_needs_wrap(const char *m)
{
#ifdef MEDIA_ATTR_IGNORE
    (void)m; return 0;
#else
    return m && *m && !str_eq_ci(m, "all");
#endif
}

static int append_media_open(char *out, int o, int max, const char *media)
{
    const char *pre = "@media ";
    while (*pre && o < max - 1) out[o++] = *pre++;
    for (const char *p = media; *p && o < max - 1; p++) out[o++] = *p;
    const char *mid = " {\n";
    while (*mid && o < max - 1) out[o++] = *mid++;
    return o;
}
static int append_media_close(char *out, int o, int max)
{
    const char *post = "\n}\n";
    while (*post && o < max - 1) out[o++] = *post++;
    return o;
}

static int collect_style(struct node *n, char *out, int o, int max)
{
    if (!n) return o;
    if (n->type == N_ELEM && tag_is(n->tag, "style")) {
        const char *media = dom_attr(n, "media");
        int wrap = media_needs_wrap(media);
        if (wrap) o = append_media_open(out, o, max, media);
        int had = 0;
        for (struct node *c = n->first_child; c; c = c->next)
            if (c->type == N_TEXT && c->text) {
                had += c->textlen;
                for (int i = 0; i < c->textlen && o < max - 1; i++) out[o++] = c->text[i];
            }
        if (wrap) o = append_media_close(out, o, max);
        css_report_style(had);
    }
    for (struct node *c = n->first_child; c; c = c->next)
        o = collect_style(c, out, o, max);
    return o;
}

static int has_ci(const char *h, const char *n);   /* defined below */
static int str_eq(const char *a, const char *b);

/* case-insensitive substring test (rel may be "stylesheet", "preload stylesheet", ...) */
static int has_ci(const char *h, const char *n)
{
    if (!h) return 0;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b) { int ca=(*a>='A'&&*a<='Z')?*a+32:*a, cb=(*b>='A'&&*b<='Z')?*b+32:*b; if(ca!=cb)break; a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

static int str_eq(const char *a, const char *b)
{ int i = 0; while (a[i] && a[i] == b[i]) i++; return a[i] == b[i]; }

/* case-insensitive PREFIX test, for URL schemes. has_ci() is a substring
 * search, and using it to ask "is this a javascript:/data: URL" silently
 * dropped any http(s) URL that merely CONTAINED the word -- e.g. a script src
 * with `?fallback=data:...` in its query string vanished with no line in the
 * log. A scheme is a prefix; test it as one. */
static int starts_ci(const char *h, const char *pre)
{
    if (!h) return 0;
    for (; *pre; h++, pre++) {
        int ca = (*h >= 'A' && *h <= 'Z') ? *h + 32 : *h;
        int cb = (*pre >= 'A' && *pre <= 'Z') ? *pre + 32 : *pre;
        if (ca != cb) return 0;
    }
    return 1;
}

/* ---- scheme inference, for what a PERSON TYPES into the address bar -----
 *
 * Never applied to an internal navigation -- follow_link(), the redirect
 * chain, hist_go(), a picked history/bookmark row and a script nav all
 * already carry a complete URL and go straight to load(). This runs from
 * exactly one place: Enter while `editing` is true, below.
 *
 * RFC 3986's own scheme grammar: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )
 * ":". Returns the index of the ':' if `s` starts with one, 0 if not --
 * which doubles as "no scheme" since a scheme can never be zero bytes long. */
static int url_scheme_len(const char *s)
{
    if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z'))) return 0;
    for (int i = 1; s[i]; i++) {
        char c = s[i];
        if (c == ':') return i;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '+' || c == '-' || c == '.')) return 0;
    }
    return 0;
}

/* "8080" or "8080/path" -- digits, then end-of-string or '/'. The one real
 * ambiguity in url_scheme_len(): "localhost:8080" and "myserver:8080" both
 * satisfy the scheme grammar above (an all-ALPHA "scheme" up to a ':'), and
 * without this check they would be read as a URI with scheme "localhost"/
 * "myserver" and path "8080" -- which is not what typing a host:port means. */
static int addr_looks_like_port(const char *s)
{
    int i = 0;
    if (s[0] < '0' || s[0] > '9') return 0;
    while (s[i] >= '0' && s[i] <= '9') i++;
    return s[i] == 0 || s[i] == '/';
}

/* The decision, written down rather than felt (this is the owner's third
 * complaint: "you have to type http/https by hand", plus the case CLAUDE.md
 * calls out by name -- a bare word with no dot and no scheme):
 *
 *   - already has a scheme (about:, file:, javascript:, http(s):, an
 *     unrecognised one -- ANY of them) -- left untouched. An explicit
 *     `http://` URL is NOT upgraded behind the user's back either: they
 *     typed a scheme, and overriding a scheme a person typed on purpose is a
 *     different kind of surprise than adding one they never typed.
 *   - looks like a HOST -- contains a dot (a domain or a bare IPv4
 *     literal), a bracketed [IPv6] literal, "localhost" bare or with a
 *     :port/path, or a bare "host:port" shape -- gets `https://` prepended,
 *     never `http://`: this machine negotiates TLS 1.3 with a real
 *     X25519MLKEM768 hybrid, and defaulting to plaintext would be a
 *     downgrade, not a convenience.
 *   - anything else -- a bare word with NO dot and NO scheme, e.g. "python"
 *     -- is REFUSED. THERE IS NO SEARCH ENGINE CONFIGURED ON THIS MACHINE:
 *     silently mailing the user's keystrokes to one would be worse than
 *     doing nothing, and doing nothing VISIBLE is worse than both, so this
 *     sets the status line to say so and does not navigate.
 *
 * Returns 1 if `url` is ready to load (unchanged, or a scheme was just
 * prepended), 0 if the caller must not navigate -- the status line already
 * says why. */
static int addr_infer_scheme(void)
{
    if (ulen <= 0) return 0;
    int sl = url_scheme_len(url);
    int port_after_scheme = sl > 0 && addr_looks_like_port(url + sl + 1);
    if (sl > 0 && !port_after_scheme) return 1;         /* a real scheme: untouched */

    int dot = 0, bracket = (url[0] == '[');
    for (int i = 0; i < ulen; i++) if (url[i] == '.') dot = 1;
    int is_localhost = ulen >= 9 && starts_ci(url, "localhost") &&
                        (ulen == 9 || url[9] == ':' || url[9] == '/');
    int host_like = dot || bracket || port_after_scheme || is_localhost;
    if (host_like) {
        char tmp[600]; int n = 0;
        const char *pre = "https://";
        while (pre[n]) { tmp[n] = pre[n]; n++; }
        for (int j = 0; j < ulen && n < (int)sizeof tmp - 1; j++) tmp[n++] = url[j];
        tmp[n] = 0;
        int k = 0; while (tmp[k] && k < (int)sizeof url - 1) { url[k] = tmp[k]; k++; }
        url[k] = 0; ulen = k;
        addr_sync();
        return 1;
    }
    set_status("not a URL -- no search engine is configured here; type a full address");
    return 0;
}

/* =========================== the persistent store ==========================
 *
 * tabs.c writes the session, the history list and the bookmarks through a
 * three-function interface rather than calling the syscalls itself, for the two
 * reasons in tabs.h: the host loader test drives this same code with no kernel
 * under it, and a settings/persistence line is building a config store this
 * round -- when it lands, this struct is the one place that changes.
 *
 * The host build (tests/unit/loaderhost/logit.h) has no file syscalls, so it
 * installs its own backend and this one is not compiled at all. */
#ifndef LOADERHOST_LOGIT_H
static int os_store_read(const char *p, void *b, int m)  { return read_file(p, b, m); }
static int os_store_write(const char *p, const void *b, int l) { return write_file(p, b, l); }
static int os_store_mkdir(const char *p) { return make_dir(p); }
static const struct bstore_ops os_store = { os_store_read, os_store_write, os_store_mkdir };
#endif

/* The window's real size. EV_RESIZE tells us when it changes, but an app that
 * has not received one yet (and one that missed one) has to ask -- re-deriving
 * it from gui_create's argument is exactly the assumption resize invalidates.
 *
 * Both are no-ops in the host test, which has no window manager: there the
 * window is whatever host_win_w/h say and never changes. */
static void win_query_size(void)
{
#if !defined(LOADERHOST_LOGIT_H) && defined(SYS_GUI_WIN_STATE)
    int w = (int)_sys(SYS_GUI_WIN_STATE, WINS_W, 0, 0);
    int h = (int)_sys(SYS_GUI_WIN_STATE, WINS_H, 0, 0);
    if (w > 200 && h > 200) { win_w = w; win_h = h; }
#endif
}

/* The smallest window the browser will accept. Used by win_set_min() (which
 * tells the WM) and by pick_born_size() (which clamps to it), so it is defined
 * before both rather than repeated in either. */
#define WIN_MIN_W 480
#define WIN_MIN_H 320

static void win_set_min(void)
{
#if !defined(LOADERHOST_LOGIT_H) && defined(SYS_GUI_WIN_MIN)
    /* Below this the tab strip cannot show a tab AND its close button, and the
     * address bar cannot show a URL. A floor is the honest answer to "lay out
     * at any width" -- the layout is fluid down to here and refuses below it. */
    _sys(SYS_GUI_WIN_MIN, ((long)WIN_MIN_W << 16) | WIN_MIN_H, 0, 0);
#endif
}

/* ---- how big the window is BORN --------------------------------------------
 *
 * WINW/WINH used to be the answer, full stop: 1180x620, whatever the display.
 * On the 1280x800 test screen that fills most of it and looks deliberate; on
 * 2560x1600 it is a small box in a corner, which is what "the browser window is
 * too small" was about.
 *
 * TWO RULES, in this order, and the order is the design decision:
 *
 *   1. WHAT THE USER CHOSE WINS. If they have resized the window before, that
 *      size is what they meant, and no proportion of the screen is a better
 *      guess than an explicit one. This is the whole reason a machine has a
 *      settings store -- and the store is the machine's, not a second one built
 *      here: two small integers are precisely what SET_VALLEN's 80 bytes and
 *      SET_MAXKV's 64 keys are FOR, which is the same measurement that said the
 *      tab session could not live there.
 *   2. OTHERWISE, PROPORTIONAL. A first run should use the display it is on.
 *      88% of the width and 80% of the height leaves the menu bar and the dock
 *      visible without this app having to model either -- it does not place its
 *      own window, the WM does, so it only picks a size and leaves room.
 *
 * Clamped both ways: never below the floor win_set_min() enforces (a smaller
 * window is one the WM will refuse anyway), and never so large that the frame
 * cannot fit on the desktop. On 1280x800 this yields 1126x640, which is within
 * a few percent of the old constants -- so nothing about the existing screens
 * or screenshots moves, and 2560x1600 gets 2252x1280 instead of a corner box.
 */
static void pick_born_size(void)
{
#ifndef LOADERHOST_LOGIT_H
    int sw = screen_w(), sh = screen_h();
    if (sw < WIN_MIN_W || sh < WIN_MIN_H) return;    /* no usable answer: keep WINW/WINH */
    int w = setting_int("browser.win.w", 0);
    int h = setting_int("browser.win.h", 0);
    if (w < WIN_MIN_W || h < WIN_MIN_H) {            /* nothing remembered */
        w = sw * 88 / 100;
        h = sh * 80 / 100;
    }
    if (w > sw - 40)  w = sw - 40;                   /* leave the frame somewhere to be */
    if (h > sh - 120) h = sh - 120;                  /* menu bar + title bar + dock */
    if (w < WIN_MIN_W) w = WIN_MIN_W;
    if (h < WIN_MIN_H) h = WIN_MIN_H;
    win_w = w; win_h = h;
#endif
}

/* Remember a size the user chose. RAM only (commit = 0): a resize drag produces
 * a stream of sizes and each commit is a whole-file LogitFS write, so the
 * setting is updated on every one and written ONCE, at close. The cost of that
 * choice is bounded and worth naming: a machine that loses power mid-session
 * forgets the window size. It does not forget the tabs -- those are saved
 * eagerly, because losing them is the loss that matters. */
static void remember_size(void)
{
#ifndef LOADERHOST_LOGIT_H
    char b[16];
    int v[2] = { win_w, win_h };
    const char *k[2] = { "browser.win.w", "browser.win.h" };
    for (int i = 0; i < 2; i++) {
        int p = 0, n = v[i] < 0 ? 0 : v[i], d = 1;
        while (n / d >= 10) d *= 10;
        while (d) { b[p++] = (char)('0' + (n / d) % 10); d /= 10; }
        b[p] = 0;
        setting_set(k[i], b, 0);
    }
#endif
}

/* ---- the page's <title>, which is what a tab is labelled with ----
 *
 * Falls back to the host, then to the URL: a tab strip where three tabs all say
 * the same thing is a tab strip you cannot use, and plenty of real pages have
 * no <title> at all. */
static void title_of(struct node *n, char *out, int max, int *done)
{
    if (!n || *done) return;
    if (n->type == N_ELEM && tag_is(n->tag, "title")) {
        int o = 0;
        for (struct node *c = n->first_child; c; c = c->next)
            if (c->type == N_TEXT && c->text)
                for (int i = 0; i < c->textlen && o < max - 1; i++) {
                    char ch = c->text[i];
                    if (ch == '\n' || ch == '\t' || ch == '\r') ch = ' ';
                    /* collapse runs of space: a <title> is often indented markup */
                    if (ch == ' ' && (o == 0 || out[o-1] == ' ')) continue;
                    out[o++] = ch;
                }
        while (o > 0 && out[o-1] == ' ') o--;
        out[o] = 0;
        if (o > 0) { *done = 1; return; }
    }
    for (struct node *c = n->first_child; c; c = c->next) title_of(c, out, max, done);
}

static void url_label(const char *u, char *out, int max)
{
    /* host + first path segment is what a tab has room for */
    const char *p = u;
    if (!p) { out[0] = 0; return; }
    const char *h = p;
    for (const char *s = p; *s; s++)
        if (s[0] == ':' && s[1] == '/' && s[2] == '/') { h = s + 3; break; }
    int o = 0;
    if (h[0] == 'w' && h[1] == 'w' && h[2] == 'w' && h[3] == '.') h += 4;
    for (const char *s = h; *s && *s != '/' && o < max - 1; s++) out[o++] = *s;
    out[o] = 0;
    if (o == 0) { int i = 0; while (u[i] && i < max - 1) { out[i] = u[i]; i++; } out[i] = 0; }
}

/* Update the active tab's label from whatever the document offered. */
static void tab_retitle(void)
{
    struct tab *t = tab_cur();
    if (!t) return;
    char title[TAB_TITLE]; title[0] = 0;
    int done = 0;
    if (g_root) title_of(g_root, title, (int)sizeof title, &done);
    if (!done || !title[0]) url_label(url, title, (int)sizeof title);
    if (!title[0]) { const char *b = "Untitled"; int i = 0; while (b[i]) { title[i] = b[i]; i++; } title[i] = 0; }
    int i = 0; while (title[i] && i < TAB_TITLE - 1) { t->title[i] = title[i]; i++; }
    t->title[i] = 0;
}

/* ======================= sub-resources: the fetch table =======================
 *
 * THE BUDGETS ARE GONE, and it is worth writing down what they were and why
 * they could not stay.
 *
 *   g_css_budget = 24, g_js_budget = 8, MAX_CSS_SEEN = 32.
 *
 * They existed because every fetch went through SYS_RES_FETCH, which is one
 * blocking kernel request with `Connection: close` -- so the count of resources
 * WAS the count of TLS handshakes, and the count of handshakes was the page
 * load time. Capping the count was the only lever there was.
 *
 * Two things retired them. Commit 65eb2c7 made each <script> its own program,
 * so more scripts no longer means more truncation; and bfetch pools
 * connections, so 28 stylesheets from one CDN cost ONE handshake rather than
 * 28. The caps are now pure loss: kimi.com links 28 stylesheets and would have
 * lost four of them to a limit that no longer buys anything.
 *
 * So the table grows instead. It holds a pointer into the DOM for the
 * reference, and the fetched bytes are malloc'd to the size that actually
 * arrived -- which also retires the other silent limit, the 1 MiB static
 * scratch buffer that kimi's 1.55 MB main bundle already exceeded. */
struct resent {
    struct node *node;
    const char  *ref;         /* the raw attribute value; NULL for an inline script */
    int   module;             /* <script type="module"> */
    int   id;                 /* bfetch request id while in flight, else -1 */
    unsigned char *data;      /* fetched (or inline) source, owned */
    int   len;
    char *url;                /* absolute URL after redirects; the module name */
    const char *err;          /* why the fetch failed; bfetch's static strings */
    int   status;             /* HTTP status of a failed fetch, 0 = no response */
};

static struct resent *g_res;
static int g_nres, g_cres;

/* ---------------------------------------------------------------------------
 * LATE IMAGES. Declared here rather than in layout.h on purpose: layout.h is
 * owned by another line this week, and these three are a private contract
 * between this file and layout.c -- the eight other host harnesses that link
 * layout.c neither call them nor need to see them. If the contract outlives
 * the week it belongs in the header.
 *
 *   layout_img_cached()      has this src already been answered (decoded, or
 *                            proven undecodable)? Asked before queueing a
 *                            network fetch, so a URL is requested once a page.
 *   layout_images_pending()  how much new work layout still owes -- the cue to
 *                            run another pass on the next frame.
 *   layout_images_reset()    drop the decoded-image cache. MUST be called on
 *                            navigation and on a tab switch: the cache key is
 *                            the raw src attribute, which means something else
 *                            under a different base URL.
 */
int  layout_img_cached(const char *url);
int  layout_images_pending(void);
void layout_images_reset(void);

/* THE TWO BUDGETS, and where the numbers come from.
 *
 * IMG_LOAD_MAX is the first pass, which happens while the page is still
 * loading and the user is looking at a progress line. It was 16, in two places
 * (this call and the prefetch loop's `g_nres < 16`), and 16 is smaller than
 * six of the nineteen sites in tests/qmp/sites_corpus.tsv: counted from the
 * host inventory in tests/scoreboard/2026-08-16-full/*.json, apple.com serves
 * 98 <img> tags, stripe 35, wikipedia 26, github 24, baidu 20, qq 17. Every
 * one of those lost pictures to the cap alone -- wikipedia's own census line
 * from that run reads `[img] 11/22 decoded`. 128 covers the corpus maximum
 * with headroom and is bounded underneath by the cache's 64 MiB.
 *
 * IMG_FRAME_BUDGET is the per-frame allowance for images discovered AFTER
 * load. It is RES_INFLIGHT, i.e. one round of pooled requests, because that is
 * the largest amount of network a frame can start and finish without the
 * window going unresponsive -- res_fetch_all() blocks until its batch lands
 * and drops everything but the close button while it does. A page that reveals
 * forty images gets eight a frame for five frames rather than one stall. */
#define IMG_LOAD_MAX     128
#define IMG_FRAME_BUDGET 8

/* 1 when the image pass has work it has not finished -- see settle_frame(). */
static int g_img_owed;

static void res_reset(void)
{
    for (int i = 0; i < g_nres; i++) {
        if (g_res[i].id >= 0) bfetch_release(g_res[i].id);
        free(g_res[i].data);
        free(g_res[i].url);
    }
    g_nres = 0;
}

static struct resent *res_add(struct node *n, const char *ref, int module)
{
    if (g_nres == g_cres) {
        int nc = g_cres ? g_cres * 2 : 16;
        struct resent *nv = realloc(g_res, (size_t)nc * sizeof *nv);
        if (!nv) return 0;
        g_res = nv; g_cres = nc;
    }
    struct resent *e = &g_res[g_nres++];
    e->node = n; e->ref = ref; e->module = module;
    e->id = -1; e->data = 0; e->len = 0; e->url = 0;
    e->err = 0; e->status = 0;
    return e;
}

static char *dupstr(const char *s)
{
    int n = 0; while (s[n]) n++;
    char *p = malloc((size_t)n + 1);
    if (p) { for (int i = 0; i <= n; i++) p[i] = s[i]; }
    return p;
}

/* Keep the window answering while a load is in flight.
 *
 * Nothing below this point blocks in the kernel any more -- bfetch runs over
 * the non-blocking socket ABI, so the WM thread keeps composing the desktop and
 * running net_poll (which is what advances our own sockets). This hook is what
 * the BROWSER'S OWN window does with that: honour the close button, and show
 * progress instead of a frozen "loading...".
 *
 * Input other than close is dropped on purpose while loading. There is no page
 * to deliver it to yet, and queueing it would replay a burst of keystrokes into
 * whatever document finally arrives. */
static int  g_prog_done, g_prog_total;
static unsigned long long g_prog_last;
static const char *g_prog_what = "";

/* getBoundingClientRect reports VIEWPORT coordinates, and js_dom.c cannot know
 * the scroll offset -- the embedder owns it. Push it whenever it moves.
 *
 * The two coincide at scroll 0, which is where every page starts, so a missing
 * call here is invisible to every test and wrong the instant a user scrolls.
 * Hence one function called from every site that touches `scroll`, rather than
 * an assignment sprinkled next to each of them. */
static int g_scroll_pushed;
static void sync_scroll(void)
{
    if (scroll == g_scroll_pushed) return;
    g_scroll_pushed = scroll;
    js_dom_set_scroll(0, scroll);
    /* The `scroll` event: fired at the document, does not bubble (per spec it
     * is dispatched from the "run the scroll steps" of the update-the-rendering
     * task, once the position has settled -- never once per wheel notch or key
     * repeat). The guard above already coalesces every caller in this file
     * (wheel, PgUp/PgDn, drag, hydrate-restore, resize-induced clamping) down
     * to one dispatch per actual change, which is the same guarantee under a
     * different name: nothing here changes `scroll` and skips calling
     * sync_scroll(), so "changed" and "about to be pushed" are the same event.
     *
     * This is also what makes below-the-fold IntersectionObserver content
     * arrive: js_platform.c's shim listens for this and re-measures instead of
     * stopping after a bounded number of timer rechecks. */
    struct js_event_init si = { 0 };
    js_dom_dispatch(js_dom_root(), "scroll", &si);
}

static void num_append(char *st, int *p, int v)
{
    if (v < 0) v = 0;
    int d = 1; while (v / d >= 10) d *= 10;
    while (d) { st[(*p)++] = (char)('0' + (v / d) % 10); d /= 10; }
}

static void load_tick(void)
{
    struct logit_event e;
    while (poll_event(&e))
        if (e.type == EV_CLOSE) { js_page_close(); bfetch_close_all(); app_exit(0); }

    /* REPAINT BETWEEN RESOURCES, NEVER DURING ONE.
     *
     * This used to repaint on a 400 ms timer, and that timer was a bug with a
     * measurable failure: repainting a wikipedia page is thousands of text-run
     * syscalls, and while it runs nobody drains the socket. TCP's receive ring
     * is 64 KiB, so a 216 KB stylesheet arriving during a repaint overran the
     * window and the transfer died with `connection closed mid-message` -- on
     * the first attempt AND on the retry, which is why the page then rendered
     * with none of its stylesheets.
     *
     * A progress counter only moves when a resource FINISHES, so keying the
     * repaint to it puts the expensive work in the gaps between transfers,
     * which is exactly where it belongs. The close button is still serviced on
     * every single tick, because that is cheap and it is what makes the window
     * feel alive. */
    unsigned long long now = monotonic_ms();
    static int last_done = -1;
    if (g_prog_done == last_done && now - g_prog_last < 3000) return;
    last_done = g_prog_done;
    g_prog_last = now;
    char st[96]; int p = 0;
    for (const char *s = g_prog_what; *s && p < 60; s++) st[p++] = *s;
    if (g_prog_total > 0) {
        st[p++] = ' ';
        num_append(st, &p, g_prog_done);
        st[p++] = '/';
        num_append(st, &p, g_prog_total);
    }
    st[p++] = ' '; st[p++] = '.'; st[p++] = '.'; st[p++] = '.';
    st[p] = 0;
    set_status(st);
    if (g_root) redraw(0);
}

/* Fetch every entry in the table CONCURRENTLY.
 *
 * The old code fetched one resource at a time inside a recursive DOM walk, and
 * each of those was a full DNS+TCP+TLS+HTTP round trip in ring 0. Here the
 * requests are all in flight together over pooled connections, so a page with
 * 28 stylesheets on one CDN is one handshake and 28 pipelined-in-parallel
 * requests rather than 28 handshakes end to end. */
#define RES_INFLIGHT 8            /* < bfetch's table, and <= the pool's caps */

/* Bytes served out of the active tab's retained set instead of the network,
 * counted so the tab test can assert that a re-hydrate did not dial. */
static int g_res_from_tab, g_res_from_net;

/* Fill `e` from the tab's own bytes if it has them. This is what makes bringing
 * a background tab back cost NO connections: the tab kept every script and
 * image it was built from (tabs.h), keyed by absolute URL, and bfetch_resolve
 * gives us that key from the raw attribute value. */
static int res_try_tab(struct resent *e)
{
    struct tab *t = tab_cur();
    if (!t || !e->ref) return 0;
    char abs[600];
    if (bfetch_resolve(0, e->ref, abs, (int)sizeof abs) != 0) return 0;
    const struct tabres *r = tab_res_find(t, abs);
    if (!r || !r->data || r->len <= 0) return 0;
    e->data = malloc((size_t)r->len + 1);
    if (!e->data) return 0;
    for (int i = 0; i < r->len; i++) e->data[i] = r->data[i];
    e->data[r->len] = 0;
    e->len = r->len;
    e->url = dupstr(abs);
    g_res_from_tab++;
    return 1;
}

/* `keep` = record what arrives in the active tab, so a later re-hydrate can
 * replay it. Stylesheets pass 0: their bytes are retained ONCE, concatenated,
 * as the tab's `css` -- keeping them a second time individually would double
 * the largest thing a page ships (github's is 3.25 MB). */
static void res_fetch_all(const char *what, int keep)
{
    int next = 0, inflight = 0;
    g_prog_done = 0; g_prog_total = g_nres; g_prog_what = what; g_prog_last = 0;
    while (next < g_nres || inflight > 0) {
        while (next < g_nres && inflight < RES_INFLIGHT) {
            struct resent *e = &g_res[next++];
            if (!e->ref) { g_prog_done++; continue; }      /* inline: nothing to fetch */
            if (res_try_tab(e)) { g_prog_done++; continue; }
            e->id = bfetch_start(e->ref);
            if (e->id < 0) { printf("[browser] cannot fetch %s\n", e->ref); g_prog_done++; continue; }
            inflight++;
        }
        if (inflight == 0) break;
        bfetch_pump();
        for (int i = 0; i < next; i++) {
            struct resent *e = &g_res[i];
            if (e->id < 0) continue;
            int st = bfetch_state(e->id);
            if (st == BF_PENDING) continue;
            if (st == BF_DONE && bfetch_status(e->id) / 100 == 2) {
                e->status = bfetch_status(e->id);
                e->url = dupstr(bfetch_url(e->id));
                e->len = bfetch_take(e->id, &e->data);
                if (e->len < 0) { e->len = 0; e->data = 0; }
                g_res_from_net++;
                if (keep && e->data && e->len > 0)
                    tab_keep_res(tab_cur(), e->url, e->data, e->len);
            } else {
                /* Keep the reason on the entry: the consumer of a script entry
                 * (run_collected_scripts) must be able to say LOUDLY that a
                 * script the document asked for will not run, and by then the
                 * bfetch slot is long gone. bfetch_error() returns static
                 * strings, so holding the pointer past release is fine; the
                 * final URL is dup'd because it is not. */
                e->err    = bfetch_error(e->id);
                e->status = bfetch_status(e->id);
                e->url    = dupstr(bfetch_url(e->id));
                printf("[browser] fetch failed (status %d) %s: %s\n",
                       e->status, e->ref, e->err);
                bfetch_release(e->id);
            }
            e->id = -1;
            inflight--;
            g_prog_done++;
        }
        load_tick();
        sys_yield();
    }
}

/* ============================ THE IMAGE PASS ==============================
 *
 * ONE function for every image load on the page, first or late, because there
 * used to be one and it ran ONCE -- straight after the first layout -- and
 * anything the page discovered afterwards was never fetched at all:
 *
 *   - an <img> a script inserted (every video card on bilibili.com; they were
 *     empty grey boxes for exactly this reason);
 *   - a lazy image whose real URL sits in data-src until script moves it into
 *     src (layout.c reads data-src as a fallback, but only at the moment the
 *     item is built, so a swap after load was invisible);
 *   - an image revealed by a re-layout;
 *   - and, worst and least obvious, images that HAD loaded: layout_page()
 *     opens with layout_free(), which frees every decoded bitmap, so the first
 *     script mutation stripped the pictures off a page that had them. That one
 *     is fixed in layout.c by the URL-keyed cache -- a re-layout now gets its
 *     pictures back for free -- and this function is what finds the new ones.
 *
 * SHAPE: queue every wanted URL into the resource table FIRST, fetch the batch
 * concurrently over the pooled connections, push the bodies into bfetch's
 * cache, and only then let layout decode. That is the same ordering the first
 * load has used since the pool landed and for the same measured reason (see
 * the long note at the original call site): a decode is seconds on an emulated
 * CPU and a CDN keep-alive is often five, so fetching one image at a time
 * inside the decode loop handed back sockets the server had already closed.
 *
 * `budget` bounds the NEW work: URLs the cache has not already answered.
 * Anything already decoded costs a pointer copy and is not charged. Returns >0
 * if the display list changed and the caller should repaint. */
static int load_late_images(int budget)
{
    if (budget <= 0) return 0;
    int n = layout_count();
    const struct item *items = layout_items();
    if (n <= 0 || !items) return 0;

    res_reset();
    int queued = 0;
    for (int i = 0; i < n && queued < budget; i++) {
        if (items[i].type != IT_IMAGE || items[i].img || !items[i].imgsrc) continue;
        if (layout_img_cached(items[i].imgsrc)) continue;   /* answered already */
        /* A `data:` image is bytes that already arrived; res_fetch() decodes
         * it. Queueing it here would send a 900-character base64 payload to
         * bfetch as a hostname, which is what produced bing's
         * `fetch failed (status 404) data:image/png;base64,...` on the
         * scoreboard -- a sub-resource failure for a resource that was never
         * missing. layout_load_images() below calls res_fetch() directly, so
         * skipping the network queue does not skip the image. */
        if (starts_ci(items[i].imgsrc, "data:")) continue;
        int dup = 0;
        for (int k = 0; k < g_nres; k++)
            if (g_res[k].ref && str_eq(g_res[k].ref, items[i].imgsrc)) { dup = 1; break; }
        if (!dup) { res_add(items[i].node, items[i].imgsrc, 0); queued++; }
    }
    if (queued == 0) { res_reset(); return 0; }

    /* res_fetch_all()'s progress ticker writes the status bar, which on a LATE
     * pass is overwriting whatever the loaded page put there (a console line,
     * an error). Put it back afterwards -- the fetch is a few hundred
     * milliseconds and the message it replaced is the page's, not ours. */
    char keep[sizeof status];
    for (unsigned k = 0; k < sizeof status; k++) keep[k] = status[k];

    g_prog_what = "images"; g_prog_total = 0; g_prog_last = 0;
    res_fetch_all("images", 1);
    for (int i = 0; i < g_nres; i++)
        if (g_res[i].data && g_res[i].len > 0 && g_res[i].url)
            bfetch_cache_put(g_res[i].url, g_res[i].data, g_res[i].len);
    res_reset();
    int got = layout_load_images(budget);
    set_status(keep);
    return got;
}

/* ---- what the DOM offers ---- */

static void collect_css_links(struct node *n)
{
    if (!n) return;
    if (n->type == N_ELEM && tag_is(n->tag, "link")) {
        const char *rel = dom_attr(n, "rel"), *href = dom_attr(n, "href");
        /* Every branch below reports itself. A stylesheet the document asks for
         * and this browser chooses not to fetch is indistinguishable, from the
         * outside, from one that 404'd and from one that was never linked --
         * three different bugs, one unstyled page. The counts go to
         * css_report.c and nowhere else. */
        if (href && has_ci(rel, "stylesheet")) {
            /* a11y override themes are inactive unless the user selected them;
             * skipping saves ~1 MiB of CSS on github.com. This is a CORRECTNESS
             * filter (the sheets do not apply), not a budget. */
            if (has_ci(href, "high_contrast") || has_ci(href, "colorblind") ||
                has_ci(href, "tritanopia")) {
                css_report_link(href, 1, "a11y theme (inactive)");
            } else if (starts_ci(href, "data:")) {
                css_report_link(href, 1, "data: URI (not fetched)");
            } else {
                int dup = 0;                   /* github links the same module CSS 3x */
                for (int i = 0; i < g_nres; i++)
                    if (g_res[i].ref && str_eq(g_res[i].ref, href)) { dup = 1; break; }
                if (dup) css_report_link(href, 1, "duplicate href");
                else if (!res_add(n, href, 0)) css_report_link(href, 1, "resource table full");
                else css_report_link(href, 0, 0);
            }
        }
    }
    for (struct node *c = n->first_child; c; c = c->next) collect_css_links(c);
}

/* Every <script> in document order, classified.
 *
 * `type` is a whitelist per spec: "module" selects the module goal, a
 * JavaScript MIME type (or nothing) selects the classic goal, and ANYTHING ELSE
 * is a data block that must not be executed -- which is how <script
 * type="application/json"> and <script type="importmap"> stop being reported as
 * the page's own syntax errors. `nomodule` marks a fallback for engines without
 * module support; we have it, so those are skipped. */
static void collect_scripts(struct node *n)
{
    if (!n) return;
    if (n->type == N_ELEM && tag_is(n->tag, "script")) {
        const char *type = dom_attr(n, "type");
        const char *src  = dom_attr(n, "src");
        int module = js_module_is_module_type(type);
        int classic = !module && js_module_is_classic_type(type);
        if (!module && !classic) {
            printf("[browser] skipping <script type=\"%s\"> (not executable)\n", type ? type : "");
        } else if (!module && dom_attr(n, "nomodule")) {
            /* the fallback for a browser without modules; we are not one */
        } else if (src) {
            /* Scheme filter as a PREFIX, not a substring (see starts_ci), and
             * every drop says so: a <script src> the document asks for and we
             * choose not to fetch must leave a line, because the scoreboard's
             * asked/got gap is exactly the count of silent drops. */
            if (starts_ci(src, "javascript:") || starts_ci(src, "data:"))
                printf("[browser] skipping <script src=\"%.60s\"> (unsupported scheme)\n", src);
            else if (!res_add(n, src, module))
                printf("[browser] script LOST: %.200s: resource table alloc failed\n", src);
        } else {
            /* inline: reassemble the text nodes into one exactly-sized buffer.
             * The old path had a 256 KiB static cap and skipped anything over
             * it; sizing to the content removes the question. */
            int total = 0;
            for (struct node *c = n->first_child; c; c = c->next)
                if (c->type == N_TEXT && c->text) total += c->textlen;
            if (total <= 0) { /* empty inline script */ }
            else {
                struct resent *e = res_add(n, 0, module);
                if (e) {
                    e->data = malloc((size_t)total + 1);
                    if (e->data) {
                        int o = 0;
                        for (struct node *c = n->first_child; c; c = c->next)
                            if (c->type == N_TEXT && c->text)
                                for (int i = 0; i < c->textlen; i++) e->data[o++] = (unsigned char)c->text[i];
                        e->data[o] = 0;
                        e->len = o;
                    }
                }
            }
        }
    }
    for (struct node *c = n->first_child; c; c = c->next) collect_scripts(c);
}

static void load(const char *u);

/* Follow a clicked link: resolve relative hrefs against the current page URL
 * (url_resolve handles absolute/protocol-relative refs itself), skip schemes we
 * can't act on. Without this every "/wiki/Foo"-style href died in url_parse. */
static void follow_link(const char *href)
{
    if (!href[0] || href[0] == '#') return;              /* pure fragment: no anchor support */
    if (has_ci(href, "javascript:") || has_ci(href, "mailto:") || has_ci(href, "data:")) return;
    char abs[600];
    struct url base;
    const char *target = href;
    if (url_parse(url, &base) == 0 && url_resolve(&base, href, abs, sizeof abs) == 0)
        target = abs;
    int i = 0; while (target[i] && i < (int)sizeof url - 1) { url[i] = target[i]; i++; }
    url[i] = 0; ulen = i; addr_sync();
    hist_push(url);
    load(url);
}

/* The document source. The DOM borrows text out of it, so it has to outlive the
 * tree -- which is also why it is a malloc'd buffer sized to the response and no
 * longer a 1 MiB static: a page bigger than that used to be silently cut. */
static unsigned char *g_page_src;
static char author_css[4194304];         /* inline <style> + fetched external <link> CSS (4 MiB; github.com ships ~3.25 MiB) */
static char css_expanded[4718592];       /* author_css after var() expansion -> LibCSS (4.5 MiB) */
static int  css_exlen;

/* Re-style + re-lay-out after script changed the DOM. Every path that can run
 * JS ends here, so a mutation from a click handler and one from a timer take
 * exactly the same route back to the screen. Returns 1 if the page actually
 * changed and needs repainting. */
static int restyle(void);

static int settle_dom(void)
{
    if (!js_dom_dirty()) return 0;
    int changed = restyle();
    js_dom_clear_dirty();
    return changed;
}

/* settle_dom() plus the image pass the re-layout it may have done makes
 * necessary. THE FRAME LOOP CALLS THIS, not settle_dom().
 *
 * Two conditions, and neither alone is enough:
 *
 *   - the DOM settled into a new layout, so there may be <img> boxes that did
 *     not exist a frame ago (script inserted one, or moved data-src into src);
 *   - layout still owes work from the last pass, because IMG_FRAME_BUDGET
 *     stopped it. Nothing marks the DOM dirty for that, so without the second
 *     test a page that reveals forty images at once would load eight and then
 *     sit there. layout_images_pending() is what makes the drain terminate:
 *     every URL it counts becomes a cache entry, positive or negative, on the
 *     pass that reaches it.
 *
 * settle_dom() itself is left alone because browser_settle() is its exported
 * form and the host loader harness calls that with no network underneath it. */
static int settle_frame(void)
{
    int changed = settle_dom();
    if (changed) g_img_owed = 1;
    /* THE GATE IS A FLAG, NOT THE SCAN. layout_images_pending() walks the whole
     * display list -- up to MAXITEM entries -- and this function runs on every
     * turn of the event loop, including the idle ones. Asking it unconditionally
     * would put a 16k-entry walk into the hot path to answer "no" almost every
     * time. The flag is set by the two things that can create work (a settled
     * mutation, and a pass that stopped at its budget) and the scan runs only
     * then. */
    if (g_img_owed) {
        if (load_late_images(IMG_FRAME_BUDGET) > 0) { ph = layout_height(); changed = 1; }
        g_img_owed = layout_images_pending() > 0;
    }
    return changed;
}

/* Console bytes already reflected in the status bar. The status line only ever
 * shows the FIRST line of console output, so re-rendering it when nothing new
 * was logged is a repaint that changes no pixels -- and a setInterval firing
 * every tick would demand one every tick. */
static int js_out_shown;

/* Copy the page's first console line into the status bar -- the only channel a
 * headless screenshot test has for "the script ran". */
static void status_from_js(const char *fallback)
{
    js_out_shown = js_page_output_len();
    const char *out = js_page_output();
    if (!out || !out[0]) { set_status(fallback); return; }
    char st[96]; int p = 0;
    const char *pre = "JS: ";
    while (*pre) st[p++] = *pre++;
    for (int i = 0; out[i] && out[i] != '\n' && p < 92; i++) st[p++] = out[i];
    st[p] = 0;
    set_status(st);
}

/* Run the page's scripts.
 *
 * ORDER IS THE SPEC, and it is two passes rather than one:
 *
 *   - classic scripts run first, in document order. (In a real browser they run
 *     as the parser reaches them; we have already finished parsing, so document
 *     order is the same answer.)
 *   - MODULES ARE DEFERRED. Every <script type="module"> is implicitly `defer`,
 *     so they all run after the document is parsed and after every classic
 *     script, still in document order among themselves.
 *
 * Each script is its own program -- see 65eb2c7 -- so a bundle that throws no
 * longer takes the page's inline scripts down with it. Returns how many ran. */

/* Does a fetched "script" body actually look like an HTML document?
 *
 * The failure this closes, measured on www.2345.com: a <script src> fetch
 * follows a redirect to an HTML page (an error page, a login stub, the site's
 * own homepage), arrives with status 200, and gets handed to JS_Eval -- which
 * reports `SyntaxError: unexpected token '<'` at line 1 UNDER THE PAGE'S OWN
 * URL, i.e. the page is blamed for a CDN's error page. Refuse it before eval,
 * out loud, naming the URL the bytes actually came from.
 *
 * The one '<' opener that IS JavaScript is `<!--`: HTML-like comments are
 * grammar (Annex B.1.1) and 1990s-era scripts really start with them, so that
 * prefix is exempt. `<!DOCTYPE`, `<html`, `<?xml` are not. */
static int body_is_html_not_js(const unsigned char *p, int len)
{
    int i = 0;
    if (len >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) i = 3;  /* UTF-8 BOM */
    while (i < len && (p[i] == ' ' || p[i] == '\t' || p[i] == '\r' || p[i] == '\n' || p[i] == '\f'))
        i++;
    if (i >= len || p[i] != '<') return 0;
    if (len - i >= 4 && p[i+1] == '!' && p[i+2] == '-' && p[i+3] == '-') return 0;
    return 1;
}

/* out_lost / out_refused / out_exc: the SAME conditions the printf lines
 * right beside them already narrate to the serial log, counted rather than
 * only printed -- so the DevTools chain panel (the "library panel" section,
 * below) reads exactly what this loop already decided about each script
 * instead of re-deriving it a second way that could disagree. Either
 * pointer may be NULL (the host loader test does not care). Local to this
 * function on purpose: no shared struct field to collide with whatever else
 * touches `struct resent` this week.
 *
 * out_exc IS THE FIX FOR THE FAILURE THIS ORDER NAMED. js_page_eval() and
 * js_module_eval() already RETURN "ran without an uncaught exception" --
 * this loop used to throw that answer away, so "executed" meant only
 * "invoked", and a script that ran and threw on its very first line looked
 * identical to one that ran cleanly: both counted as "executed 1 of 1".
 * Measured on this order's own test fixture (a page whose one script calls
 * an undefined function): before this, the panel said "scripts executed:
 * OK, executed 1 of 1" on a page whose script never got past its first
 * statement. */
static int run_collected_scripts(const char *page_url, int *out_lost, int *out_refused, int *out_exc)
{
    int ran = 0, inline_n = 0, classic_n = 0, lost_n = 0, refused_n = 0, exc_n = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < g_nres; i++) {
            struct resent *e = &g_res[i];
            if (e->module != pass) continue;            /* pass 0 classic, pass 1 module */
            if (!e->data || e->len <= 0) {
                /* An EXTERNAL script with no bytes is a script the page asked
                 * for that will never run -- jQuery on jd.com was this, and the
                 * only trace was seven downstream ReferenceErrors blamed on
                 * other files. Say it once, plainly, with the reason the fetch
                 * recorded. (An inline entry with no bytes is just an empty
                 * <script></script>; nothing was lost.) */
                if (e->ref) {
                    printf("[browser] script LOST: %s: %s (status %d)\n",
                           e->url ? e->url : e->ref,
                           e->err ? e->err : "no body", e->status);
                    lost_n++;
                }
                continue;
            }
            if (e->ref && body_is_html_not_js(e->data, e->len)) {
                printf("[browser] script REFUSED (HTML, not JS): %s (status %d, %d bytes)\n",
                       e->url ? e->url : e->ref, e->status, e->len);
                refused_n++;
                continue;
            }
            if (!e->module) {
                /* A CLASSIC script's URL is not decoration either: it is the
                 * base a dynamic import() inside it resolves against. An
                 * inline classic script used to be handed the literal
                 * "<inline>", which is not a URL, so `import('./x.js')` from
                 * an inline <script> resolved against nothing and failed --
                 * while the identical call in an external script, or in the
                 * inline MODULE path ten lines below, worked. Same rule as the
                 * module path: the document's URL with a discriminator.
                 *
                 * AND THE NODE GOES WITH IT, separately, because it is a
                 * separate fact. `e->node` is the <script> element these bytes
                 * came from and it becomes document.currentScript; the runtime
                 * used to have to work that out by pattern-matching cnm, which
                 * for the string built right below can never succeed -- every
                 * page URL contains "https:" and the inline test was "no ':'
                 * anywhere". So the SAME commit that gave an inline script a
                 * real import() base took its currentScript away, and neither
                 * half was wrong on its own. See js_page.c. */
                char cname[600];
                const char *cnm = e->url;
                if (!cnm) {
                    int p = 0;
                    for (const char *s = page_url; *s && p < 560; s++) cname[p++] = *s;
                    const char *tag = "#inline-script-";
                    for (const char *s = tag; *s && p < 590; s++) cname[p++] = *s;
                    num_append(cname, &p, ++classic_n);
                    cname[p] = 0;
                    cnm = cname;
                }
                if (!js_page_eval((const char *)e->data, e->len, cnm, e->node)) exc_n++;
                dom_script_mark_done(e->node);   /* never re-run via DOM insertion */
                ran++;
                continue;
            }
            /* A module needs a UNIQUE ABSOLUTE URL: it is both the key in the
             * module map (so an import of the same file twice instantiates it
             * once) and the base every specifier inside it resolves against.
             * An inline module has no URL of its own, so it gets the document's
             * with a discriminator -- which resolves relative specifiers exactly
             * as the spec says, against the document. */
            char name[600];
            const char *nm = e->url;
            if (!nm) {
                int p = 0;
                for (const char *s = page_url; *s && p < 560; s++) name[p++] = *s;
                const char *tag = "#inline-module-";
                for (const char *s = tag; *s && p < 590; s++) name[p++] = *s;
                num_append(name, &p, ++inline_n);
                name[p] = 0;
                nm = name;
            }
            if (!js_module_eval((const char *)e->data, e->len, nm)) exc_n++;
            dom_script_mark_done(e->node);
            ran++;
        }
    }
    if (out_lost)    *out_lost    = lost_n;
    if (out_refused) *out_refused = refused_n;
    if (out_exc)     *out_exc     = exc_n;
    return ran;
}

/* ---- dynamically-inserted scripts --------------------------------------
 * A <script> that enters the document by DOM insertion (appendChild etc.)
 * after parse must be prepared and run per HTML5. js_dom.c's insert_run
 * detects that and calls the sink below -- which does NOT run anything on
 * that stack (the inserting script is still executing; recursing the
 * evaluator through a DOM callback is the trap the spec doc names). It
 * ENQUEUES the node; run_pending_inserted_scripts() drains the queue from
 * the per-frame loop, exactly where run_collected_scripts already runs and
 * where res_fetch_all already pumps the network.
 *
 * A src script is fetched (synchronously here, on the frame loop, not the
 * insertion stack -- the same bfetch_sync the module loader uses) and run;
 * an inline script runs from its own text. Insertion order is preserved by
 * the queue being FIFO. dom_script_mark_done stamps each before running so a
 * re-insertion never re-runs it, and so offer_scripts never re-queues one
 * already queued (the stamp is set at run, but the QUEUED set is checked by
 * pointer below to stop a double-enqueue between insertion and drain). */
#define PENDING_MAX 64
static struct node *g_pending[PENDING_MAX];
static int g_pending_n;

static void on_script_inserted(struct node *n)
{
    if (!n || g_pending_n >= PENDING_MAX) {
        if (n) printf("[browser] inserted-script queue full -- dropping one\n");
        return;
    }
    for (int i = 0; i < g_pending_n; i++) if (g_pending[i] == n) return;  /* already queued */
    g_pending[g_pending_n++] = n;
}

/* Run every queued inserted script, in order. Re-entrant by construction: a
 * script run here that inserts another appends to g_pending (via the sink)
 * and this loop, which re-reads g_pending_n each turn, picks it up -- without
 * ever recursing, because the sink only enqueues. Returns how many ran. */
static int run_pending_inserted_scripts(const char *page_url)
{
    int ran = 0, guard = 0;
    while (g_pending_n > 0) {
        if (++guard > 4 * PENDING_MAX) {          /* a script re-inserting forever */
            printf("[browser] inserted-script drain guard tripped -- stopping\n");
            g_pending_n = 0;
            break;
        }
        struct node *n = g_pending[0];
        for (int i = 1; i < g_pending_n; i++) g_pending[i - 1] = g_pending[i];
        g_pending_n--;
        if (dom_script_is_done(n)) continue;
        dom_script_mark_done(n);                  /* stamp BEFORE running: run-once even if it throws */

        int is_module = 0;
        const char *type = dom_attr(n, "type");
        if (type && (has_ci(type, "module"))) is_module = 1;
        const char *src = dom_attr(n, "src");

        unsigned char *data = 0; int len = 0; char urlbuf[600];
        const char *name = page_url;
        if (src && src[0]) {
            /* Fetch on the frame loop, not the insertion stack. bfetch_sync
             * pumps the network the same way the module loader's mod_loader
             * does. */
            int rc = bfetch_resolve(page_url, src, urlbuf, sizeof urlbuf);
            if (rc != 0) { printf("[browser] inserted script: bad src %s\n", src); continue; }
            int fd = bfetch_start(urlbuf);
            if (fd < 0) { printf("[browser] inserted script: cannot fetch %s\n", urlbuf); continue; }
            while (bfetch_state(fd) == BF_PENDING) bfetch_pump();
            if (bfetch_state(fd) == BF_DONE && bfetch_status(fd) / 100 == 2) {
                len = bfetch_take(fd, &data);
                if (len < 0) { len = 0; data = 0; }
                name = urlbuf;
            } else {
                printf("[browser] inserted script LOST: %s: %s (status %d)\n",
                       urlbuf, bfetch_error(fd), bfetch_status(fd));
                bfetch_release(fd);
                continue;
            }
        } else {
            /* Inline: reassemble the child text nodes, exactly as
             * collect_scripts does at parse time. */
            int total = 0;
            for (struct node *c = n->first_child; c; c = c->next)
                if (c->type == N_TEXT && c->text) total += c->textlen;
            if (total <= 0) continue;
            data = malloc((size_t)total + 1);
            if (!data) continue;
            int o = 0;
            for (struct node *c = n->first_child; c; c = c->next)
                if (c->type == N_TEXT && c->text)
                    for (int i = 0; i < c->textlen; i++) data[o++] = (unsigned char)c->text[i];
            data[o] = 0; len = o;
        }
        if (data && len > 0 && !(src && body_is_html_not_js(data, len))) {
            if (is_module) js_module_eval((const char *)data, len, name);
            /* `n` is the node, and a dynamically inserted script's
             * document.currentScript is itself exactly as a parsed one's is --
             * which is how a bundler's chunk loader finds its own <script>
             * after appendChild has run it. */
            else           js_page_eval((const char *)data, len, name, n);
            ran++;
        }
        free(data);
    }
    return ran;
}

/* ===================== script-initiated navigation ==========================
 *
 * A page is allowed to move itself: `location.href = ...`, location.assign(),
 * location.replace(), location.reload(). js_webapi.c parses and records those
 * and NOTHING USED TO READ THE RECORD -- it printed "the loader does not
 * consume this yet" and the browser sat on the document.
 *
 * That is not a corner case. It is how https://www.baidu.com/ renders blank:
 * baidu sniffs the User-Agent, and to ours it serves 227 bytes whose entire
 * body is
 *
 *     <script>location.replace(location.href.replace("https://","http://"));</script>
 *
 * with no stylesheet, no <script src>, no image and no text. So "the loader
 * found no sub-resources" and "the page was blank" were one fact, not two: we
 * rendered a redirect stub, correctly, for ever. (The `<noscript><meta
 * http-equiv=refresh>` beside it is NOT a second chance -- with scripting
 * enabled, noscript content is raw text by spec and our tokenizer treats it as
 * such, which is right.)
 *
 * WHERE IT IS CONSUMED, and why it is not consumed where it is produced: this
 * runs from the loader and from the top of the event loop, never from inside a
 * JS callback. loc_set() is reached from arbitrarily deep inside a script; if
 * it navigated in place it would dom_free() and js_page_close() the very
 * document whose handler is on the stack. So the record is taken at a point
 * where nothing of the old page is live any more.
 *
 * THE BUDGET IS A LOOP GUARD, not a policy. A page that replaces itself with
 * itself never stops, and a pair of pages that bounce to each other never stop
 * either. Real browsers cap the chain; so does this, and it says so in the
 * status bar rather than freezing. The budget is per user-initiated load, so
 * following ten redirects and then clicking a link gives the next chain a
 * fresh ten.
 *
 * Overridable so that the host test can build a loader with a budget of ZERO,
 * which is byte-for-byte the behaviour this fix replaced: the record is taken
 * and thrown away, the stub stays on screen. `make test-loader-negctl` builds
 * exactly that and requires the test to FAIL -- without it, test-loader passing
 * would not be evidence that it measures anything. */
#ifndef NAV_MAX_HOPS
#define NAV_MAX_HOPS 10
#endif

/* Take a pending script navigation into `out`. 0 if there is none, or if this
 * build has no js_webapi.o at all (browser-nofetch). */
static int take_script_nav(char *out, int max)
{
    if (!LOGIT_HAVE(js_webapi_take_navigation)) return 0;
    return js_webapi_take_navigation(out, max) ? 1 : 0;
}

static void load_once(const char *u);

/* A user-initiated load, plus every navigation the page itself then asks for.
 *
 * The chain is a LOOP rather than recursion on purpose: a redirect chain of n
 * hops must cost one stack frame, not n, and each hop tears down the previous
 * document completely before the next one starts. */
static void load(const char *u)
{
    char cur[600], next[600];
    /* `u` is usually &url[0] -- follow_link and the address bar both write it
     * before calling. Copy first: the chain rewrites `url` on every hop. */
    { int i = 0; while (u[i] && i < (int)sizeof cur - 1) { cur[i] = u[i]; i++; } cur[i] = 0; }

    /* RFC 3986: a URL is ASCII on the wire, and `cur` may not be -- a word
     * typed through the pinyin IME lands in the address bar as UTF-8, or a
     * restored session/history entry could hold one from before this line
     * existed. %XX-encode every byte outside ASCII in place; nothing else
     * moves, because every reserved/unreserved character a hand-typed URL
     * uses (`:/?&=#` etc.) is itself ASCII and passes through untouched, and
     * an already-percent-encoded "%20" is unaffected for the same reason
     * ('%' is ASCII). What this does NOT do is IDNA/punycode a non-ASCII
     * HOSTNAME -- percent-encoding a host is not valid per the URL Standard,
     * and that is a different feature; today a non-ASCII host is still
     * escaped byte-for-byte rather than sent raw, which is the narrower claim
     * this line makes. */
    { int nonascii = 0;
      for (int i = 0; cur[i]; i++) if ((unsigned char)cur[i] > 0x7E) { nonascii = 1; break; }
      if (nonascii) {
          static const char H[] = "0123456789ABCDEF";
          char enc[2048]; int o = 0;
          for (int i = 0; cur[i] && o < (int)sizeof enc - 4; i++) {
              unsigned char c = (unsigned char)cur[i];
              if (c < 0x80) enc[o++] = (char)c;
              else { enc[o++] = '%'; enc[o++] = H[c >> 4]; enc[o++] = H[c & 15]; }
          }
          enc[o] = 0;
          { int i = 0; while (enc[i] && i < (int)sizeof cur - 1) { cur[i] = enc[i]; i++; } cur[i] = 0; }
      }
    }

    /* Scheme inference (the owner's third complaint: "you have to type
     * http/https by hand") lives at addr_infer_scheme(), called once, at the
     * address bar's Enter key -- not here. An earlier version of this patch
     * put a second, cruder version at this exact spot: same idea, no
     * bare-word refusal, and reachable from every OTHER caller of load() too
     * (follow_link, panel picks, script navigation) where the string is
     * already an absolute URL and there is nothing to infer. Two schemes for
     * "does this need a scheme" is the one-jar-two-doors trap this file's own
     * comments warn about elsewhere -- addr_infer_scheme() is the one door,
     * and it runs before `url` is ever handed to load(). */

    /* about:text -- print the words the LAST paint put on the screen, and stay
     * where we are. Not a navigation and not a page: it answers a question
     * about the page already loaded, so navigating away to answer it would
     * destroy the thing being asked about.
     *
     * THE ADDRESS BAR IS THE CHANNEL BECAUSE IT IS THE ONE THAT IS PROVEN.
     * This started as a Ctrl+Alt+D chord, which is the obvious shape and which
     * produced no output at all through the QMP harness -- twice, once unpaced
     * and once paced one scancode at a time. Nothing in the kernel explains it
     * (kbd_mods reports EV_MOD_ALT, wm_shortcut only claims SUPER chords), so
     * the failure is somewhere in a path this line cannot observe, and an
     * instrument whose trigger cannot be observed is not an instrument. Typing
     * a URL is what every driver in tests/qmp already does forty times a run.
     * The chord stays wired as well; if it ever starts arriving it costs
     * nothing, and it is the convenient one for a person at the keyboard. */
    /* The address every load() was actually handed, once. Two lines cost
     * nothing and both were missing when they were wanted: `[browser] load
     * done` says a load finished and never said OF WHAT, so a dropped or
     * doubled keystroke in a QMP harness -- the failure this file's own
     * comment at the fetch-failed branch calls "the whole question" -- was
     * only visible when the fetch failed. It is equally the whole question
     * when the fetch SUCCEEDS and the wrong page arrives. */
    printf("[browser] load: %s\n", cur);
    if (cur[0] == 'a' && cur[1] == 'b' && cur[2] == 'o' && cur[3] == 'u' &&
        cur[4] == 't' && cur[5] == ':' && cur[6] == 't' && cur[7] == 'e' &&
        cur[8] == 'x' && cur[9] == 't' && cur[10] == 0) {
        browser_paint_text_dump();
        set_status("painted text dumped to the serial console");
        return;
    }
    /* about:boxes -- the display list, one line per item, through the same
     * channel and for the same reason as about:text.
     *
     * WHAT IT IS FOR, and it is a specific question rather than a general
     * curiosity. `[dl] painted text` says WHICH WORDS reached the screen; the
     * screenshot then shows them in the wrong place, and the next question is
     * always "how wide does layout think that box is". Today that is answered
     * by a person counting pixels in a PNG. MEASURED on stripe.com the day
     * this went in: two <h1>s that a real browser stacks are laid out side by
     * side, each about 40 px wide, so the headline wraps one character per
     * line -- and nothing in any log said 40.
     *
     * Bounded and honest about it, like the text dump: past the cap the count
     * keeps counting and the lines stop, and the trailer says which happened. */
    if (cur[0] == 'a' && cur[1] == 'b' && cur[2] == 'o' && cur[3] == 'u' &&
        cur[4] == 't' && cur[5] == ':' && cur[6] == 'b' && cur[7] == 'o' &&
        cur[8] == 'x' && cur[9] == 'e' && cur[10] == 's' && cur[11] == 0) {
        const struct item *it = layout_items();
        int n = layout_count(), shown = 0;
        static const char *KIND[] = { "rect", "text", "img", "video", "ctrl", "canvas" };
        printf("[dl] boxes: %d item(s)\n", n);
        printf("[dl] ---8<--- begin boxes\n");
        for (int i = 0; i < n; i++) {
            if (shown >= 2048) break;
            const struct item *e = &it[i];
            const char *k = (e->type >= 0 && e->type < 6) ? KIND[e->type] : "?";
            const char *tag = (e->node && e->node->tag) ? e->node->tag : "-";
            const char *id = 0; int idl = 0;
            const char *cls = 0; int cll = 0;
            if (e->node) {
                id  = js_dom_attr_len(e->node, "id", &idl);
                cls = js_dom_attr_len(e->node, "class", &cll);
            }
            /* class is truncated at 48: a utility-class page carries two
             * hundred characters of them and the line would stop being
             * readable, which is the same trade the text dump makes. */
            if (cll > 48) cll = 48;
            printf("[dl] %-6s %5d,%-5d %4dx%-4d <%s>%s%.*s%s%.*s\n",
                   k, e->x, e->y, e->w, e->h, tag,
                   idl ? " #" : "", idl, id ? id : "",
                   cll ? " ." : "", cll, cls ? cls : "");
            shown++;
        }
        printf("[dl] ---8<--- end boxes (%d shown of %d)\n", shown, n);
        set_status("display list dumped to the serial console");
        return;
    }
    /* THE INVARIANT: after load(u), the browser is AT u. Every in-app caller
     * already wrote `url` before calling, so this is a no-op for them -- but it
     * has to be stated, because the moment it is only true by convention it
     * stops being true. It was not true for browser_load() from the host test,
     * and tab_dehydrate() -- which records `url` as the tab's address -- then
     * stamped a stale URL onto the tab it was putting away. */
    { int i = 0; while (cur[i] && i < (int)sizeof url - 1) { url[i] = cur[i]; i++; }
      url[i] = 0; ulen = i; addr_sync(); }

    /* Discard anything the PREVIOUS page left pending. js_webapi_install does
     * not clear the record, so a navigation requested by a page the user then
     * abandoned would otherwise fire against the new one. */
    take_script_nav(next, sizeof next);

    for (int hops = 0; ; hops++) {
        load_once(cur);
        if (!take_script_nav(next, sizeof next)) return;
        if (hops >= NAV_MAX_HOPS) {
            set_status("stopped: too many redirects");
            redraw(0);
            return;
        }
        /* The address bar follows, and so does history -- but by REPLACING the
         * current entry. See hist_replace: a redirect that pushes is a Back
         * button that cannot escape. */
        { int i = 0; while (next[i] && i < (int)sizeof url - 1) { url[i] = next[i]; i++; }
          url[i] = 0; ulen = i; addr_sync(); }
        hist_replace(url);
        { int i = 0; while (url[i] && i < (int)sizeof cur - 1) { cur[i] = url[i]; i++; } cur[i] = 0; }
    }
}

/* Exposed for the host loader test (tests/unit/loader_test.c), which links this
 * file against a fake bfetch and a recording GUI so that the redirect chain
 * above is testable without QEMU. Nothing in the app calls it. */
void browser_load(const char *u);
void browser_load(const char *u) { load(u); }

/* 1 while a tab is being replayed from its own retained bytes rather than
 * loaded from the network. Everything it changes is marked `hydrating` below. */
static int g_hydrating;

/* ====================== DevTools: the chain panel's data ===================
 * A page rendering is a PRODUCT over dependent stages, not a sum -- zero at
 * any one stage means a blank page no matter how well every other stage did.
 * This is that chain's dataset, snapshotted ONCE per load_once(), from the
 * exact counters load_once was already computing to print to the serial log
 * (see the "scripts collected" / "load done" / "pool" lines below). NOTHING
 * here is a second accounting: every field is the value already printed,
 * read a second time rather than derived a second way -- so the panel can
 * never disagree with the log a person would otherwise have to go find.
 * `have` is 0 until the first load_once() completes, which is how the panel
 * says "no page has loaded yet" instead of showing every count as a
 * (indistinguishable) zero. */
struct dt_chain {
    int have;
    int bytes;                   /* document arrived: bytes fetched */
    int xc, xm, in;               /* scripts collected: ext classic, ext module, inline */
    int ran, lost, refused;       /* scripts: executed / LOST (no body) / REFUSED (HTML) */
    int exc;                      /* of `ran`, how many THREW (uncaught) -- js_page_eval()/
                                    * js_module_eval()'s own return value, not a guess */
    int dyn_ran;                  /* + scripts run later via DOM insertion */
    int reqs, dials, reuses;      /* network (bfetch_stats) */
    int mods, modfail;            /* ES module graph (js_module_stats) */
};
static struct dt_chain g_dt;

static void load_once(const char *u)
{
    set_status(g_hydrating ? "restoring tab..." : "loading...");
    /* The animation clock's entries point into the document that is about
     * to be freed, and unlike the JS wrappers they have no teardown hook
     * of their own -- dropped here, before dom_free, for the same reason
     * focus_reset() is: a recycled slot would silently name a different
     * element. Weak: the host loader test links no js_anim.o. */
    if (LOGIT_HAVE(css_anim_reset)) css_anim_reset();
    /* pagehide, on the document about to be torn down -- mirrors the pageshow
     * dispatch near the bottom of this function. Must run BEFORE js_page_close:
     * that call frees the runtime this event needs to run any listener at all.
     * On the very first navigation there is no prior runtime, so js_dom.c's own
     * g_ctx is still NULL and js_dom_dispatch's no-runtime guard makes this a
     * no-op -- no `if (g_root)` needed here to say the same thing twice. */
    struct js_event_init ph_out = { 0 };
    js_dom_dispatch(js_dom_root(), "pagehide", &ph_out);
    /* ORDER: the runtime dies before the DOM does. Every JS wrapper holds a
     * {node, serial} handle and every node holds a weak pointer back to its
     * wrapper, so freeing the document first would leave the runtime's
     * finalizers walking nodes that no longer exist -- and freeing the runtime
     * first is what clears the wrapper slots. */
    js_page_close();
    /* js_forms.c's editing-event dispatcher holds the JS context js_page_close
     * just freed. Left installed, the next keystroke would call into it. Weak
     * so a build without js_forms.o (BROWSER_PIPE, the host loader test) still
     * links -- there is nothing to clean up there. */
    if (LOGIT_HAVE(js_forms_cleanup)) js_forms_cleanup();
    /* Focus and every control's state point INTO the document that is about to
     * be freed. dom.c recycles node slots, so a pointer kept across this line
     * would not merely dangle -- it would silently name a DIFFERENT element in
     * the next document, which is the worse failure. Dropped BEFORE dom_free,
     * on this path and on the tab-switch one, because both free the tree.
     * The page-text selection (g_psel_an/g_psel_fo) is guarded by a serial
     * check like hover_node/press_node/lastclick_node below, which is enough
     * to survive a slot RECYCLED within the SAME document -- but dom.c's
     * per-node serial counter is scoped to the document (dom.c: `next_serial`
     * lives on `struct doc`), so a freed slot reused by the NEXT document's
     * parse can legally mint the very same serial number again. Cleared here
     * explicitly rather than trusted to the same guard everything else uses,
     * because unlike those three -- which only misroute a synthetic DOM
     * event -- a wrong psel match would highlight and let Ctrl+C copy text
     * from a page the user never selected anything on. */
    popup_close();
    focus_reset();
    fc_reset();
    psel_clear();
    if (g_root) { dom_free(g_root); g_root = 0; }
    layout_free();
    /* The decoded-image cache goes with the document. Its key is the raw
     * src attribute, so under a different base URL the same key names a
     * different picture -- keeping it across a navigation would paint the
     * previous page's images onto this one. layout_free() deliberately does
     * NOT do this: it runs at the top of every layout_page(), and dropping
     * the cache there is precisely the defect being fixed. */
    layout_images_reset();
    res_reset();
    free(g_page_src); g_page_src = 0;
    ph = 0;
    if (!g_hydrating) scroll = 0;     /* hydrating: the tab's scroll is restored */
    g_scroll_pushed = 0;          /* js_dom_init resets its side to 0 as well */

    /* A navigation ends the old page's connections: keeping them would hold
     * pool slots (and kernel socket slots) for an origin the new page may have
     * nothing to do with.
     *
     * A TAB SWITCH IS NOT A NAVIGATION and must not do either of those. Closing
     * the pool on a switch would make N tabs cost N times the handshakes, which
     * is precisely the multiplication tabs are supposed not to cause -- and the
     * pool is process-global (bfetch_init is idempotent), so leaving it alone is
     * all that "the pool is shared across tabs" requires. */
    bfetch_init();
    bfetch_set_tick(load_tick);
    if (!g_hydrating) {
        bfetch_cache_clear();
        bfetch_close_all();
        /* A NAVIGATION REPLACES WHAT THE TAB IS, so what it retained goes with
         * it. Two things depend on this and both are silent if it is missing:
         * res_try_tab serves by absolute URL, so the old page's script bytes
         * would be handed to the new page whenever the two share a URL -- a
         * cache with no expiry that nobody asked for -- and tab_keep_res only
         * ever appends, so a tab navigated ten times would hold ten pages'
         * sub-resources. The old document is already freed by the teardown
         * above, so nothing is pointing into these bytes. */
        tab_drop_content(tab_cur());
    }
    bfetch_reset_stats();
    g_res_from_tab = g_res_from_net = 0;
    js_module_reset();
    bfetch_set_base(u);

    g_prog_what = "fetching page"; g_prog_total = 0; g_prog_done = 0; g_prog_last = 0;
    char base[600];
    int code = 200, blen = 0;
    struct tab *ht = g_hydrating ? tab_cur() : 0;
    if (ht && ht->src && ht->srclen > 0) {
        /* HYDRATING: the document is already here. Copy rather than borrow --
         * the tab has to still own its master copy for the next switch, and
         * the DOM borrows text straight out of whatever we hand dom_parse. */
        blen = ht->srclen;
        g_page_src = malloc((size_t)blen + 1);
        if (!g_page_src) { set_status("restore failed: out of memory"); return; }
        for (int i = 0; i < blen; i++) g_page_src[i] = ht->src[i];
        g_page_src[blen] = 0;
        { const char *f = ht->base[0] ? ht->base : u; int i = 0;
          while (f[i] && i < (int)sizeof base - 1) { base[i] = f[i]; i++; } base[i] = 0; }
        bfetch_set_base(base);
        /* Put every byte the tab kept back where res_fetch() will look for it,
         * so layout's <img> loop finds its images without a connection. */
        for (int i = 0; i < ht->nres; i++)
            bfetch_cache_put(ht->res[i].url, ht->res[i].data, ht->res[i].len);
    } else {
    /* Not hydrating after all -- either this is a real navigation, or the tab
     * had no retained bytes (a session-restored tab, which is empty by design).
     * Clearing `ht` is what makes every "am I replaying?" test below correct;
     * leaving it set would apply a stale stylesheet to fresh markup. */
    ht = 0;
    /* The ONE navigation in this file; everything else bfetch_start()s is a
     * subresource. That distinction is a cookie rule, not a label -- bfetch.h. */
    int doc = bfetch_start_nav(u);
    if (doc < 0) { set_status("load failed: bad URL (need http:// or https://)"); return; }
    bfetch_wait(doc, load_tick);
    if (bfetch_state(doc) != BF_DONE) {
        /* Name the URL as the fetcher saw it: when this fires under a test
         * harness, "which exact string did the address bar hand over" is the
         * whole question (a dropped or doubled keystroke lives right here). */
        const char *why = bfetch_error(doc);
        printf("[browser] page fetch failed: %s (%s)\n", why, bfetch_url(doc));
        /* The REASON, in the one place a person is looking. "could not fetch
         * the page" was the same sentence for a rejected certificate, a name
         * that does not resolve and a network that is down -- three different
         * things to do about it, and the diagnosis that separates them already
         * existed the whole way up from x509 through sock_poll's error byte
         * (see the SOCK_P_ERROR branch in browser_rt.c). It was thrown away
         * here. No snprintf in this TU on purpose -- see build_get's note in
         * browser_rt.c; status[] is 96 and the longest sock_why() sentence is
         * 57, so nothing here can truncate. */
        { char st[sizeof status];
          const char *pre = "load failed: ";
          int p = 0;
          while (pre[p] && p < (int)sizeof st - 1) { st[p] = pre[p]; p++; }
          for (int i = 0; why[i] && p < (int)sizeof st - 1; i++) st[p++] = why[i];
          st[p] = 0;
          set_status(st); }
        bfetch_release(doc);
        return;
    }
    code = bfetch_status(doc);
    /* The URL AFTER redirects is the base for every relative reference on the
     * page. Resolving against the typed URL instead is how a redirected page
     * ends up asking the wrong origin for its own stylesheets. */
    { const char *f = bfetch_url(doc); int i = 0;
      while (f[i] && i < (int)sizeof base - 1) { base[i] = f[i]; i++; } base[i] = 0; }
    bfetch_set_base(base);
    blen = bfetch_take(doc, &g_page_src);
    /* A DOWNLOAD is a body that goes to the disk instead of the parser. It is
     * decided here and not earlier because "is this a page" is a property of
     * the response, and this is the first point at which the response exists. */
    if (code / 100 == 2 && blen > 0 && download_is_downloadable(base)) {
        int d = download_record(base, g_page_src, blen);
        const struct download *rec = download_at(d);
        char st[96]; int p = 0;
        const char *pre = rec && rec->ok ? "downloaded to " : "download FAILED: ";
        while (*pre) st[p++] = *pre++;
        for (const char *s = rec ? rec->path : "?"; *s && p < 92; s++) st[p++] = *s;
        st[p] = 0;
        set_status(st);
        printf("[browser] %s (%d bytes)\n", st, blen);
        free(g_page_src); g_page_src = 0;
        /* Stay on the page that linked it, exactly as a real browser does. */
        { struct tab *t = tab_cur(); if (t && t->url[0]) {
            int i = 0; while (t->url[i] && i < (int)sizeof url - 1) { url[i] = t->url[i]; i++; }
            url[i] = 0; ulen = i; addr_sync(); } }
        return;
    }
    }
    { struct tab *t = tab_cur(); if (t) {
        int i = 0; while (base[i] && i < TAB_URL - 1) { t->base[i] = base[i]; i++; } t->base[i] = 0; } }
    if (code / 100 != 2) {
        char st[64]; int p = 0; const char *pre = "error: HTTP ";
        while (*pre) st[p++] = *pre++;
        num_append(st, &p, code); st[p] = 0;
        set_status(st);
        if (blen <= 0) return;                       /* still render an error body */
    }
    if (blen <= 0) { set_status("error: empty response"); return; }
    g_root = dom_parse((const char *)g_page_src, blen);
    if (!g_root) { set_status("error: parse failed"); return; }
    /* Where forms.c resolves a Selection API position FROM. It normally starts
     * at the caret, and the one call that has no caret to start at is the one
     * that matters: a page placing the caret itself before the user has
     * clicked anything. */
    fc_ce_set_root(g_root);
    /* Before the first byte of CSS is looked at: this both clears the previous
     * page's numbers and INSTALLS the parser drop hooks. Both halves matter --
     * see css_report.h on why a missing reset must read as a dead instrument
     * rather than as a plausible report. */
    css_report_reset();
    /* :target's ONE input, and the only place the URL and the cascade meet.
     *
     * css_engine.c answers :target from a fragment somebody has to hand it, and
     * until this line nobody did -- the handler was live, gated on the host, and
     * fed nothing, so on the machine every `:target` rule still matched nothing.
     * That is this tree's "built with no real consumer" shape and it is exactly
     * what a host gate cannot see: css_selstatic_test.c calls
     * css_set_target_fragment() itself, so it passes either way.
     *
     * Set from `url` rather than re-derived from a parsed struct because `url`
     * is the post-redirect document URL that every other reference on this page
     * resolves against; taking the fragment from the typed URL instead would
     * disagree with the document after a 302. A URL with no '#' clears it, and
     * clearing means "no target" -- never "match everything". */
    { const char *frag = 0;
      for (int i = 0; url[i]; i++) if (url[i] == '#') { frag = url + i + 1; break; }
      css_set_target_fragment(frag, -1); }
    int css_len = collect_style(g_root, author_css, 0, (int)sizeof author_css);
    /* HYDRATING: the tab kept the FULL author stylesheet (inline + every
     * external sheet, concatenated exactly as assembled below), so the whole
     * stylesheet phase -- discovery, fetch, concatenation -- is replaced by a
     * copy and costs no connection at all. */
    if (ht && ht->css && ht->csslen > 0) {
        css_len = ht->csslen < (int)sizeof author_css - 1 ? ht->csslen : (int)sizeof author_css - 1;
        for (int i = 0; i < css_len; i++) author_css[i] = ht->css[i];
        author_css[css_len] = 0;
    }
    css_exlen = css_expand_vars(author_css, css_len, css_expanded, (int)sizeof css_expanded);
    css_apply(g_root, css_expanded, css_exlen);
    css_extra_apply(g_root, css_expanded, css_exlen);
    layout_page(g_root, win_w);
    ph = layout_height();
    set_status(ht ? "restoring tab..." : "loaded -- fetching stylesheets...");
    redraw(0);                       /* first paint: HTML + inline CSS, before slow CDN fetches */

    /* ---- external stylesheets, all at once, no budget ---- */
    int css2 = css_len, got_sheets = 0, nsheets = 0;
    if (!ht) {
    res_reset();
    collect_css_links(g_root);
    nsheets = g_nres;
    res_fetch_all("stylesheets", 0);
    int css_offered = 0;         /* bytes the sheets contained */
    for (int i = 0; i < g_nres; i++) {
        struct resent *e = &g_res[i];
        const char *u = e->url ? e->url : e->ref;
        if (!e->data || e->len <= 0) {
            /* THE SILENT HALF, until now. A sheet that did not arrive left no
             * line anywhere: res_fetch_all prints only on a hard failure, and
             * an empty 200 printed nothing at all. */
            css_report_fetched(u, e->status,
                               e->len > 0 ? e->len : 0,
                               e->err ? CSSSH_ERROR :
                               (e->status && e->status / 100 != 2) ? CSSSH_HTTP
                                                                   : CSSSH_EMPTY,
                               e->err);
            continue;
        }
        got_sheets++;
        css_offered += e->len;
        css_report_fetched(u, e->status, e->len, CSSSH_OK, 0);
        /* e->node is the <link> element itself (res_add(n, href, 0) above),
         * so this is the same attribute, the same wrap, as collect_style's
         * <style media>. See the comment above collect_style. */
        const char *media = dom_attr(e->node, "media");
        int wrap = media_needs_wrap(media);
        if (wrap) css2 = append_media_open(author_css, css2, (int)sizeof author_css, media);
        for (int k = 0; k < e->len && css2 < (int)sizeof author_css - 1; k++)
            author_css[css2++] = (char)e->data[k];
        if (wrap) css2 = append_media_close(author_css, css2, (int)sizeof author_css);
        else if (css2 < (int)sizeof author_css - 1) author_css[css2++] = '\n';
    }
    /* offered vs kept. They differ when author_css filled up -- the 216 KB-
     * stylesheet failure this file already carries a comment about, and which,
     * before this line, cut the tail off a page's CSS and said nothing -- OR
     * when a media-scoped sheet was wrapped in `@media ... { }` above, which
     * legitimately makes kept > offered. Only the first direction (offered >
     * kept) is truncation; css_report_concat only flags that direction. css2
     * counts the '\n' separators too, so subtract them. */
    css_report_concat(css_len, css_offered,
                      css2 - css_len - (got_sheets ? got_sheets : 0));
    res_reset();
    /* Retain ONE copy of the finished stylesheet, not one per sheet: this is
     * the byte count a background tab actually costs for its CSS. */
    author_css[css2 < (int)sizeof author_css ? css2 : (int)sizeof author_css - 1] = 0;
    tab_keep_css(tab_cur(), author_css, css2);
    }
    /* report what actually arrived: sheet count + KiB (debug aid for CDN fetch issues) */
    { char st[96]; int p = 0; const char *pre = "loaded, ";
      while (*pre) st[p++] = *pre++;
      num_append(st, &p, got_sheets);
      st[p++] = '/'; num_append(st, &p, nsheets);
      const char *mid = " sheets, ";
      while (*mid) st[p++] = *mid++;
      num_append(st, &p, css2 / 1024);
      st[p++] = 'K'; st[p] = 0; set_status(st); }
    if (css2 > css_len) {
        css_len = css2;
        css_exlen = css_expand_vars(author_css, css_len, css_expanded, (int)sizeof css_expanded);
        css_report_expand(css_len, css_exlen, (int)sizeof css_expanded);
        css_apply(g_root, css_expanded, css_exlen);
    css_extra_apply(g_root, css_expanded, css_exlen);
        layout_page(g_root, win_w);
        ph = layout_height();
        { extern size_t malloc_peak; printf("[browser] heap peak %uK\n", (unsigned)(malloc_peak / 1024)); }
        redraw(0);                   /* re-paint with the page's real stylesheets */
    }
    /* The stylesheet half of "load done". Printed unconditionally, INCLUDING
     * on the hydrating path where every count is legitimately zero, because a
     * page that reports nothing about its CSS is the state this whole record
     * exists to end. The parser drop counts are already in it by now: the
     * hooks fed them during css_apply above. */
    css_report_print();
    /* Images ride the same pooled connections, so eight of them from one host
     * is one handshake rather than eight. That is why IMG_LOAD_MAX can go up
     * without the load time going with it. The queue-everything-then-decode
     * ordering and the measurement behind it now live in load_late_images(),
     * which is where they belong: they apply to every pass, not this one.
     *
     * The first pass is load_late_images() too -- ONE image path, so a fix to
     * either the queueing or the budget cannot land on only one of them. The
     * two differ in exactly one number, see IMG_LOAD_MAX. */
    if (load_late_images(IMG_LOAD_MAX) > 0) {
        ph = layout_height();
        redraw(0);
    }
    /* A page with more than IMG_LOAD_MAX images has some left; the frame loop
     * drains them. Set unconditionally rather than from the pass's own count,
     * because the scripts about to run are the other producer. */
    g_img_owed = 1;
    /* Open the page's JS runtime. It stays open until the next navigation --
     * that is the whole point: listeners, timers and pending promises all live
     * past the end of the script that created them. It is opened even when the
     * page has no <script>, so that a later dispatchEvent/on-attribute path has
     * a context to run in. */
    js_page_output_clear();
    js_out_shown = 0;
    js_page_set_location(base);
    js_page_open(g_root);
    g_pending_n = 0;                       /* fresh page, empty inserted-script queue */
    js_dom_set_script_sink(on_script_inserted);
    /* The form/focus JS surface (element.value, .checked, form.submit(),
     * document.activeElement) is installed by js_page_open() itself, alongside
     * every other module's -- NOT from here.
     *
     * It was called from here first, to avoid touching js_page.c at all, and
     * then ALSO from js_page.c so that the WPT runner (which links js_page.c
     * and not browser.c) would see the bindings. Installing it twice into one
     * context aborts the process during page load -- reproduced on the device,
     * bisected to exactly that, and not diagnosed further because one install
     * is the correct shape anyway and matches every other module here. Do not
     * "helpfully" add a second call back. */

    /* Fetch every external script CONCURRENTLY, then run them in spec order.
     * Real sites ship huge minified bundles that assume a full browser env;
     * with no real DOM they just throw -- but they throw ALONE. The runtime's
     * 2 MiB stack guard bounds recursive scripts so a bad bundle raises a
     * catchable RangeError instead of faulting. */
    res_reset();
    collect_scripts(g_root);
    /* The guest's own inventory, printed BEFORE fetching: the scoreboard's
     * asked/got gap compares the host's count of the document's <script src>
     * against requests we issued, and without this line a shortfall cannot be
     * split into "the parser never produced the element" vs "the fetch never
     * happened". One number from each side of that boundary. */
    { int xc = 0, xm = 0, in = 0;
      for (int i = 0; i < g_nres; i++) {
          if (!g_res[i].ref) in++;
          else if (g_res[i].module) xm++;
          else xc++;
      }
      printf("[browser] scripts collected: %d external classic, %d external module, %d inline\n",
             xc, xm, in);
      g_dt.xc = xc; g_dt.xm = xm; g_dt.in = in; }
    res_fetch_all("scripts", 1);
    g_prog_what = "running scripts"; g_prog_total = 0; g_prog_last = 0;
    int dt_lost = 0, dt_refused = 0, dt_exc = 0;
    int dt_ran = run_collected_scripts(base, &dt_lost, &dt_refused, &dt_exc);
    int had_script = dt_ran > 0;
    /* A parse-time script may have inserted more <script>s (an AMD/loader
     * shim is the common case). Drain them here, on this stack, before the
     * page is declared loaded -- run_pending_inserted_scripts is itself
     * re-entrant, so a chain of loaders resolves fully. */
    int dt_dyn_ran = run_pending_inserted_scripts(base);
    if (dt_dyn_ran > 0) had_script = 1;
    g_dt.ran = dt_ran; g_dt.lost = dt_lost; g_dt.refused = dt_refused;
    g_dt.exc = dt_exc;
    g_dt.dyn_ran = dt_dyn_ran;
    { int dials = 0, reuses = 0, reqs = 0, mods = 0, modfail = 0;
      int hits = 0, evicted = 0, closed = 0;
      bfetch_stats(&dials, &reuses, &reqs);
      bfetch_pool_stats(&hits, &evicted, &closed);
      js_module_stats(&mods, &modfail);
      printf("[browser] load done: %d requests, %d connections dialled, %d reused"
             ", %d modules loaded (%d failed)\n", reqs, dials, reuses, mods, modfail);
      printf("[browser] pool: %d hits, %d evicted, %d closed\n",
             hits, evicted, closed);
      /* The claim tabs have to keep: a switch replays, it does not reload.
       * `from tab` counting the whole resource set and `dialled 0` are the two
       * halves of it, and they are printed on every load so a regression shows
       * up in the serial log of any test that loads a page twice. */
      printf("[browser] resources: %d from tab, %d from network (tab %d of %d)\n",
             g_res_from_tab, g_res_from_net, tabs_active(), tabs_count());
      g_dt.reqs = reqs; g_dt.dials = dials; g_dt.reuses = reuses;
      g_dt.mods = mods; g_dt.modfail = modfail;
      g_dt.bytes = blen; g_dt.have = 1; }
    res_reset();

    /* The tab keeps the DOCUMENT bytes. Handing over ownership rather than
     * copying: g_page_src is re-made from the tab's copy on the next hydrate,
     * so there is exactly one master copy per tab at any moment. */
    if (!ht) {
        tab_keep_src(tab_cur(), g_page_src, blen);
        g_page_src = 0;
    }

    /* The document is parsed and the scripts have run: fire the lifecycle events
     * pages hang their initialisation on. Without these, every page that defers
     * its work to `window.addEventListener('load', ...)` -- which is most of
     * them -- would sit there fully parsed and completely inert. */
    struct js_event_init li = { 0 };
    li.bubbles = 1;
    js_dom_dispatch(js_dom_root(), "DOMContentLoaded", &li);
    li.bubbles = 0;
    js_dom_dispatch(js_dom_root(), "load", &li);
    /* HTML requires this at window on EVERY load, unconditionally -- it is not
     * a bfcache-restore-only event, whatever its name suggests. A bootstrap
     * written as `addEventListener('pageshow', init)` (a real, if uncommon,
     * alternative to a `load` listener) sat inert forever without this: the
     * property existed (js_cssom.c's SHIM_BODY_HANDLERS reflects onpageshow),
     * assignment worked, and nothing ever called it. */
    js_dom_dispatch(js_dom_root(), "pageshow", &li);

    /* settle_FRAME: a `load` handler that inserts images is the single most
     * common way a real page's pictures arrive after the first image pass, and
     * this is the first point at which they exist. */
    if (settle_frame() || had_script) {
        status_from_js(had_script ? "loaded (ran script, no output)" : "loaded");
        redraw(0);
    }

    /* The tab is now what it will look like in the strip and in the history
     * list. Both are done HERE and not in load(): a redirect chain calls
     * load_once once per hop, and only the hop that actually rendered should
     * name the tab or leave a history entry. */
    { struct tab *t = tab_cur();
      if (t) {
          t->loaded = 1;
          t->ph = ph;
          int i = 0; while (base[i] && i < TAB_URL - 1) { t->url[i] = base[i]; i++; }
          t->url[i] = 0;
          tab_retitle();
          if (!ht) {                       /* a replay is not a new visit */
              history_add(t->url, t->title, (unsigned)(monotonic_ms() / 1000));
              history_save();
          }
          if (ht) {                        /* restore where the user had scrolled to */
              int maxs = ph - VIEW_H; if (maxs < 0) maxs = 0;
              scroll = t->scroll > maxs ? maxs : t->scroll;
              sync_scroll();
          }
          session_save();
      } }
}

/* ======================= tabs: dehydrate / hydrate =========================
 *
 * The two halves of a tab switch. See tabs.h for why they exist at all: the
 * engine is a singleton, so a switch is not "show the other document", it is
 * "put this one away and take that one out".
 *
 * WHAT DEHYDRATION KEEPS is decided in load_once, not here -- the tab already
 * holds its document bytes, its finished stylesheet and every sub-resource by
 * the time it goes into the background. This function's whole job is to record
 * the two things that only exist while the tab is live (where the user had
 * scrolled to, and how tall the page turned out) and then to let go of
 * everything derived. */
static void tab_dehydrate(void)
{
    struct tab *t = tab_cur();
    if (t) {
        t->scroll = scroll;
        t->ph = ph;
        int i = 0; while (url[i] && i < TAB_URL - 1) { t->url[i] = url[i]; i++; }
        t->url[i] = 0;
    }
    /* Same teardown order as a navigation, and for the same reason: the runtime
     * holds {node, serial} handles into the DOM, so it dies first. */
    js_page_close();
    /* js_forms.c's editing-event dispatcher holds the JS context js_page_close
     * just freed. Left installed, the next keystroke would call into it. Weak
     * so a build without js_forms.o (BROWSER_PIPE, the host loader test) still
     * links -- there is nothing to clean up there. */
    if (LOGIT_HAVE(js_forms_cleanup)) js_forms_cleanup();
    /* Focus and every control's state point INTO the document that is about to
     * be freed. dom.c recycles node slots, so a pointer kept across this line
     * would not merely dangle -- it would silently name a DIFFERENT element in
     * the next document, which is the worse failure. Dropped BEFORE dom_free,
     * on this path and on the tab-switch one, because both free the tree.
     * The page-text selection (g_psel_an/g_psel_fo) is guarded by a serial
     * check like hover_node/press_node/lastclick_node below, which is enough
     * to survive a slot RECYCLED within the SAME document -- but dom.c's
     * per-node serial counter is scoped to the document (dom.c: `next_serial`
     * lives on `struct doc`), so a freed slot reused by the NEXT document's
     * parse can legally mint the very same serial number again. Cleared here
     * explicitly rather than trusted to the same guard everything else uses,
     * because unlike those three -- which only misroute a synthetic DOM
     * event -- a wrong psel match would highlight and let Ctrl+C copy text
     * from a page the user never selected anything on. */
    popup_close();
    focus_reset();
    fc_reset();
    psel_clear();
    if (g_root) { dom_free(g_root); g_root = 0; }
    layout_free();
    /* The decoded-image cache goes with the document. Its key is the raw
     * src attribute, so under a different base URL the same key names a
     * different picture -- keeping it across a navigation would paint the
     * previous page's images onto this one. layout_free() deliberately does
     * NOT do this: it runs at the top of every layout_page(), and dropping
     * the cache there is precisely the defect being fixed. */
    layout_images_reset();
    res_reset();
    free(g_page_src); g_page_src = 0;
    ph = 0; scroll = 0;
    g_scroll_pushed = 0;
}

/* Bring the active tab back to the screen. Returns 1 if it rendered from its
 * own bytes (no network), 0 if it has none and needs a real load. */
static int tab_hydrate(void)
{
    struct tab *t = tab_cur();
    if (!t) return 0;
    int i = 0; while (t->url[i] && i < (int)sizeof url - 1) { url[i] = t->url[i]; i++; }
    url[i] = 0; ulen = i; addr_sync();
    if (!t->src || t->srclen <= 0) return 0;
    scroll = t->scroll;
    g_hydrating = 1;
    load_once(url);
    g_hydrating = 0;
    return 1;
}

/* Switch to tab `i`. The one function every caller uses -- the strip, the
 * keyboard and session restore -- so there is exactly one order of operations
 * for "put one document away and bring another out". */
static void tab_switch_to(int i)
{
    if (i == tabs_active() || !tab_at(i)) return;
    tab_dehydrate();
    tabs_select(i);
    if (!tab_hydrate()) {
        struct tab *t = tab_cur();
        if (t && t->url[0]) load(url);     /* restored-but-never-loaded: fetch it */
        else { set_status("new tab -- type a URL and press Enter"); redraw(1); }
    }
    session_save();
}

/* Seams for the host tab test, alongside browser_load. Nothing in the app calls
 * them -- the app reaches the same code through the strip and the keyboard --
 * but a switch is the operation the whole design rests on, and testing it
 * through a synthesised mouse click would be testing the hit test. */
void browser_tab_switch(int i);
void browser_tab_switch(int i) { tab_switch_to(i); }
/* One full repaint, for the test that counts what the tab strip costs. */
void browser_redraw_now(void);
void browser_redraw_now(void) { redraw(0); }
/* What the event loop does after a script has run: take the DOM's invalidation
 * record through the cascade and layout. The seam exists because THAT is the
 * path that laid out at the wrong width, and a test that only resizes cannot
 * reach it. */
int browser_settle(void);
int browser_settle(void) { return settle_dom(); }
int  browser_view_h(void);
int  browser_view_h(void) { return VIEW_H; }
/* How many resources the last load took from the tab's own bytes, and how many
 * from the network. The proof that a switch replays. */
void browser_res_split(int *from_tab, int *from_net);
void browser_res_split(int *from_tab, int *from_net)
{ if (from_tab) *from_tab = g_res_from_tab; if (from_net) *from_net = g_res_from_net; }

/* Re-run the cascade + layout after a script mutation. The expanded stylesheet
 * is whatever the last fetch produced, so this is safe to call at any point
 * after the first css_apply.
 *
 * This used to be "css_apply over the whole document, then layout_page over the
 * whole document", unconditionally, for every mutation. With pages live, that
 * is what a setInterval nudging one element cost every single tick. Now the
 * mutation says WHERE it happened (js_dom.c's invalidation record) and HOW
 * much can have moved, and the work follows:
 *
 *   - a marked scope re-styles that subtree (+ its following siblings, for the
 *     sibling combinators) instead of the document;
 *   - the cascade reports whether anything actually came out different, so a
 *     class toggle that matches no rule costs no layout and no repaint at all;
 *   - a structural change skips that question, because inserting or removing a
 *     node moves boxes whatever the computed styles say.
 *
 * The fallbacks are all in the safe direction: no scopes, too many scopes, or
 * a scope whose node was destroyed before we got here all mean "do what this
 * function used to do". */
static int restyle(void)
{
    if (!g_root) return 0;
    int level = js_dom_inval_level();
    if (level == INVAL_NONE) return 0;

    /* BEFORE the cascade runs: the animation clock snapshots the current
     * effective opacity/transform of every element it watches, so that a
     * class change can be TRANSITIONED from the value the user was seeing
     * rather than discovering the old value only after it is gone. This is
     * the pre-half of the css_anim_note() css_extra_apply performs inside
     * the cascade below. Weak like the reset in load_once(). */
    if (LOGIT_HAVE(css_anim_snapshot)) css_anim_snapshot(g_root);

    int nroots = js_dom_inval_roots();
    int changed = CSS_CHANGED_NONE;
    for (int i = 0; i < nroots; i++) {
        int sib = 0;
        struct node *n = js_dom_inval_root(i, &sib);
        if (!n) { nroots = 0; break; }        /* destroyed since it was marked */
        changed |= css_apply_scoped(n, sib, css_expanded, css_exlen);
    }
    if (nroots == 0) {                        /* whole document */
        css_apply(g_root, css_expanded, css_exlen);
        css_extra_apply(g_root, css_expanded, css_exlen);
        changed = CSS_CHANGED_LAYOUT;
    }
    /* Nodes came or went: the box tree changed even if every computed style
     * came back identical. */
    if (level >= INVAL_LAYOUT) changed |= CSS_CHANGED_LAYOUT;

    if (changed == CSS_CHANGED_NONE) return 0;
    /* CSS_CHANGED_PAINT still rebuilds the display list: layout_page is what
     * fills in every painted colour, so there is no cheaper path to take until
     * layout grows one. The tier is already carried this far, so adding it is
     * a change on the layout side alone.
     *
     * `win_w`, NOT `WINW`. This was the last call site still laying out at the
     * born-at constant, and it is the worst one to miss: it is the INVALIDATION
     * path, so it does not run on load -- it runs when a script mutates the DOM.
     * A resized window therefore laid out correctly until the page changed
     * anything, and then snapped back to 1180 px and stayed there, because every
     * subsequent mutation did it again. Any real application mutates
     * constantly, so on a resized window this fired immediately and repeatedly
     * and looked like "the sizing adaptation is wrong" rather than like a
     * layout width.
     *
     * A resize handler that re-lays-out is not enough on its own: EVERY path
     * that lays out has to agree about the width, and the one that does not is
     * invisible to any test that resizes without mutating. See the test in
     * tests/unit/loader_test.c part 3 (f), which mutates on purpose. */
    layout_page(g_root, win_w);
    ph = layout_height();
    return 1;
}

/* ============================== the tab strip ==============================
 *
 * Hand-drawn over gui_*, and that is a decision with a reason rather than an
 * omission. c/apps/gui/aui.c -- the toolkit, which does have tabs, hover states
 * and keyboard focus -- is NOT in browser.aex's link (BROWSER_PIPE in the
 * Makefile), and putting it there costs two things this line should not spend:
 * a hunk in the Makefile, which has been clobbered three times this week, and a
 * toolkit dependency in the host loader test, whose window is five drawing
 * recorders. The browser's own chrome (the glass bar, the URL field) is already
 * drawn this way, so the strip matches what is beside it. When the browser does
 * link aui, aui_tabs/aui_text_ellipsis/aui_icon_button replace this block and
 * the geometry functions below stay as they are.
 *
 * HOVER IS DELIBERATELY ABSENT. SYS_GUI_FLUSH carries no rectangle, so any
 * repaint costs the whole window canvas -- 24-27 ms at 1920x1200. A tab strip
 * that highlights under the pointer demands one of those per pointer sample,
 * which is the difference between a window that feels alive and one that feels
 * slow. The close button therefore lives on the ACTIVE tab only, where its
 * presence is stable and costs nothing to keep drawn. */
#define TAB_MINW  84
#define TAB_MAXW  200
#define TAB_GAP   4
#define TAB_PLUSW 26
#define TAB_LEFT  6

static int g_tab_first;                  /* first tab shown, when they overflow */

static int tab_slot_w(int nvis)
{
    if (nvis < 1) nvis = 1;
    int avail = win_w - TAB_LEFT * 2 - TAB_PLUSW - TAB_GAP;
    int w = (avail - (nvis - 1) * TAB_GAP) / nvis;
    if (w > TAB_MAXW) w = TAB_MAXW;
    if (w < TAB_MINW) w = TAB_MINW;
    return w;
}

/* How many tabs fit at the minimum width. At least one, always: a window
 * narrow enough to fit no tab still has to show the one you are looking at. */
static int tab_visible_max(void)
{
    int avail = win_w - TAB_LEFT * 2 - TAB_PLUSW - TAB_GAP;
    int n = (avail + TAB_GAP) / (TAB_MINW + TAB_GAP);
    return n < 1 ? 1 : n;
}

/* The used-slot indices, in strip order. Returns the count. */
static int tab_order(int *out)
{
    int n = 0;
    for (int i = 0; i < TAB_MAX; i++) if (tab_at(i)) out[n++] = i;
    return n;
}

/* Keep the active tab inside the visible window after any change to the set. */
static void tab_scroll_into_view(void)
{
    int ord[TAB_MAX], n = tab_order(ord), vis = tab_visible_max();
    int pos = 0;
    for (int i = 0; i < n; i++) if (ord[i] == tabs_active()) { pos = i; break; }
    if (g_tab_first > n - vis) g_tab_first = n - vis;
    if (g_tab_first < 0) g_tab_first = 0;
    if (pos < g_tab_first) g_tab_first = pos;
    if (pos >= g_tab_first + vis) g_tab_first = pos - vis + 1;
}

/* Truncate `s` to `maxpx` at the 8-px-per-character estimate the address bar
 * already uses, appending an ellipsis. Deliberately not text_measure_px: this
 * file links host-side against a window that has no font, and a tab label that
 * is a few pixels wide of ideal is not worth the divergence. */
static void tab_label(const char *s, int maxpx, char *out, int max)
{
    int budget = maxpx / 8;
    if (budget > max - 1) budget = max - 1;
    if (budget < 1) budget = 1;
    int n = 0; while (s[n]) n++;
    if (n <= budget) { int i = 0; for (; s[i]; i++) out[i] = s[i]; out[i] = 0; return; }
    int keep = budget - 1; if (keep < 1) keep = 1;
    for (int i = 0; i < keep; i++) out[i] = s[i];
    out[keep] = '~';                       /* one byte, and the font has it */
    out[keep + 1] = 0;
}

static void draw_tab_strip(void)
{
    int ord[TAB_MAX], n = tab_order(ord);
    int vis = tab_visible_max();
    if (vis > n) vis = n;
    int w = tab_slot_w(vis);
    tab_scroll_into_view();

    gui_glass(0, 0, win_w, TABH, 1, 255, 255, 255, 60);
    int x = TAB_LEFT;
    for (int k = g_tab_first; k < n && k < g_tab_first + vis; k++) {
        int idx = ord[k];
        struct tab *t = tab_at(idx);
        int on = (idx == tabs_active());
        gui_glass(x, 4, w, TABH - 6, 7, 255, 255, 255, on ? 210 : 80);
        /* Two pixels of accent under the active tab. The glass alone does not
         * carry enough contrast over a light page to say WHICH tab you are on,
         * and "which tab am I on" is the one question a tab strip exists to
         * answer. It costs one rect and it is not animated, so it is free at
         * the only rate that matters -- repaints, of which it causes none. */
        if (on) gui_rect(x + 6, TABH - 4, w - 12, 2, rgb(90, 150, 240));
        char lab[64];
        tab_label(t->title[0] ? t->title : "New Tab", w - (on ? 32 : 16), lab, (int)sizeof lab);
        gui_text(x + 8, 8, on ? rgb(25, 25, 35) : rgb(105, 105, 118), lab);
        if (on && w >= TAB_MINW) {
            /* the close box, on the active tab only -- see the note above */
            gui_glass(x + w - 20, 9, 14, 14, 7, 255, 255, 255, 140);
            gui_text(x + w - 16, 8, rgb(90, 90, 100), "x");
        }
        x += w + TAB_GAP;
    }
    /* new tab */
    gui_glass(x, 4, TAB_PLUSW, TABH - 6, 7, 255, 255, 255, 90);
    gui_text(x + 9, 8, rgb(80, 80, 92), "+");
}

/* The hit test, sharing the geometry above so the two cannot drift. Returns the
 * tab index, or -1; -2 means the "+" button. `*close` is set when the point
 * landed on the active tab's close box. */
static int tab_strip_hit(int mx, int my, int *close)
{
    if (close) *close = 0;
    if (my < 0 || my >= TABH) return -1;
    int ord[TAB_MAX], n = tab_order(ord);
    int vis = tab_visible_max();
    if (vis > n) vis = n;
    int w = tab_slot_w(vis), x = TAB_LEFT;
    for (int k = g_tab_first; k < n && k < g_tab_first + vis; k++) {
        if (mx >= x && mx < x + w) {
            if (close && ord[k] == tabs_active() && mx >= x + w - 22) *close = 1;
            return ord[k];
        }
        x += w + TAB_GAP;
    }
    if (mx >= x && mx < x + TAB_PLUSW) return -2;
    return -1;
}

/* ============================ the library panel ============================
 * History, bookmarks and downloads are LISTS, and a list needs somewhere to be.
 * One overlay serves all three because they are the same shape (a title, a URL,
 * a row you can activate) and three panels would be three sets of scrolling and
 * selection bugs. */
enum { PANEL_NONE = 0, PANEL_HISTORY, PANEL_BOOKMARKS, PANEL_DOWNLOADS, PANEL_DEVTOOLS };
static int  g_panel, g_panel_sel, g_panel_top;
static char g_find[64];
static int  g_findlen;

/* ---- Ctrl+F: find-in-page ---- a SEPARATE query buffer from g_find above,
 * which belongs to the history panel's own inline filter -- reusing it would
 * make opening the history panel silently clobber whatever a person was
 * searching for on the page, and vice versa. */
static int  g_finding;
static char g_pfq[64];
static int  g_pfqlen;

#define PANEL_ROW 22

static int panel_rows(void) { int r = (VIEW_H - 46) / PANEL_ROW; return r < 1 ? 1 : r; }

/* The rows the panel is currently showing, as indices into the underlying list.
 * History filters through the search box; the other two do not (a bookmark list
 * you can search is a nice-to-have, a history you cannot search is not a
 * history). Returns the count. */
static int panel_list(int *out, int max)
{
    if (g_panel == PANEL_HISTORY) return history_search(g_find, out, max);
    int n = g_panel == PANEL_BOOKMARKS ? bookmark_count()
          : g_panel == PANEL_DOWNLOADS ? download_count() : 0;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = i;
    return n;
}

static void panel_row_text(int which, int idx, char *url_out, char *title_out)
{
    url_out[0] = title_out[0] = 0;
    if (which == PANEL_DOWNLOADS) {
        const struct download *d = download_at(idx);
        if (!d) return;
        int i = 0; for (; d->path[i] && i < TAB_TITLE - 1; i++) title_out[i] = d->path[i];
        title_out[i] = 0;
        i = 0; for (; d->url[i] && i < TAB_URL - 1; i++) url_out[i] = d->url[i];
        url_out[i] = 0;
        return;
    }
    const struct hist_entry *e = which == PANEL_BOOKMARKS ? bookmark_at(idx) : history_at(idx);
    if (!e) return;
    int i = 0; for (; e->title[i] && i < TAB_TITLE - 1; i++) title_out[i] = e->title[i];
    title_out[i] = 0;
    i = 0; for (; e->url[i] && i < TAB_URL - 1; i++) url_out[i] = e->url[i];
    url_out[i] = 0;
}

/* ============================ DevTools: the chain panel ====================
 *
 * A page rendering is a PRODUCT over dependent stages, not a sum: one zero
 * factor blanks the page no matter how many other stages succeeded. Every
 * failure this order was written against was exactly that shape --
 * isEqualNode missing (0) lost React hydration and 69 painted text runs went
 * to 0 with no failed request and no missing subresource; a Worker missing
 * (0) blanked deepseek outright. WPT's 39.3% is a SUM and cannot see any of
 * that; this panel names the first ZERO link instead.
 *
 * THE RULE THAT MAKES THIS TRUSTWORTHY: every link is either OBSERVED from a
 * counter load_once() already computes (see `struct dt_chain` above, and
 * browser_paint_text_counts()) or it is UNKNOWN. A link reported "ok" because
 * nothing said otherwise is exactly the lie that let a blank page score
 * healthy before -- so a link this build cannot instrument (hydration; there
 * is no framework-hydration signal anywhere in this tree) says UNKNOWN,
 * drawn in a visibly different colour and word, rather than a guessed OK. */
enum { CH_OK = 0, CH_PARTIAL, CH_ZERO, CH_UNKNOWN };

static uint32_t ch_color(int state)
{
    switch (state) {
    case CH_OK:      return rgb(70, 175, 100);   /* green  -- observed, non-zero */
    case CH_PARTIAL: return rgb(220, 160, 50);    /* amber  -- observed, some loss */
    case CH_ZERO:    return rgb(215, 70, 70);     /* red    -- observed, the zero factor */
    default:         return rgb(150, 150, 160);   /* gray   -- UNKNOWN, not observed */
    }
}
static const char *ch_word(int state)
{
    switch (state) {
    case CH_OK:      return "OK";
    case CH_PARTIAL: return "PARTIAL";
    case CH_ZERO:    return "ZERO";
    default:         return "UNKNOWN";
    }
}

/* One link's name + state + a plain-English detail sentence, built into a
 * caller-owned buffer with num_append (this file's convention for building a
 * status string without pulling in sprintf). */
struct ch_link { const char *name; int state; char detail[100]; };

static void ch_put(char *b, int *p, int cap, const char *s)
{ while (*s && *p < cap - 1) b[(*p)++] = *s++; }

static int devtools_chain(struct ch_link *out, int max)
{
    int n = 0;
    if (n >= max) return n;
    /* 1. document arrived */
    { struct ch_link *l = &out[n++]; l->name = "document arrived";
      int p = 0;
      if (!g_dt.have) { l->state = CH_UNKNOWN; ch_put(l->detail, &p, 100, "no page loaded in this tab yet"); }
      else if (g_dt.bytes <= 0) { l->state = CH_ZERO; ch_put(l->detail, &p, 100, "0 bytes -- the fetch produced nothing"); }
      else { l->state = CH_OK; num_append(l->detail, &p, g_dt.bytes); ch_put(l->detail, &p, 100, " bytes fetched"); }
      l->detail[p] = 0; }
    /* 2. parsed -- read LIVE (g_root persists across a paint, freed only on
     * the next navigation/dehydrate), so this always reflects the document
     * on screen right now, not a stale snapshot from the last load. */
    if (n < max) { struct ch_link *l = &out[n++]; l->name = "parsed";
      int p = 0;
      if (g_root) { l->state = CH_OK; ch_put(l->detail, &p, 100, "DOM tree built"); }
      else if (g_dt.have) { l->state = CH_ZERO; ch_put(l->detail, &p, 100, "no DOM root -- the HTML parser produced no tree"); }
      else { l->state = CH_UNKNOWN; ch_put(l->detail, &p, 100, "no page loaded in this tab yet"); }
      l->detail[p] = 0; }
    /* 3. scripts executed -- g_dt.{xc,xm,in,ran,dyn_ran,lost,refused,exc}, the
     * SAME counters "[browser] scripts collected" / "load done" print, PLUS
     * js_page_eval()/js_module_eval()'s own return value (g_dt.exc): a
     * script that was INVOKED but threw on its first statement used to
     * count as "executed" indistinguishably from one that ran clean --
     * verified on this order's own control fixture (a page whose one script
     * calls an undefined function), which said "executed 1 of 1, OK" before
     * this and "1 of 1 invoked, but it threw" after. */
    if (n < max) { struct ch_link *l = &out[n++]; l->name = "scripts executed";
      int p = 0;
      int collected = g_dt.xc + g_dt.xm + g_dt.in;
      int invoked = g_dt.ran + g_dt.dyn_ran;
      int clean = invoked - g_dt.exc; if (clean < 0) clean = 0;
      if (!g_dt.have) { l->state = CH_UNKNOWN; ch_put(l->detail, &p, 100, "no page loaded in this tab yet"); }
      else if (collected == 0) { l->state = CH_OK; ch_put(l->detail, &p, 100, "no <script> on this page"); }
      else if (invoked == 0) {
          l->state = CH_ZERO; ch_put(l->detail, &p, 100, "collected ");
          num_append(l->detail, &p, collected); ch_put(l->detail, &p, 100, ", executed 0 (");
          num_append(l->detail, &p, g_dt.lost); ch_put(l->detail, &p, 100, " lost, ");
          num_append(l->detail, &p, g_dt.refused); ch_put(l->detail, &p, 100, " refused)");
      } else if (clean == 0) {
          l->state = CH_ZERO; num_append(l->detail, &p, invoked);
          ch_put(l->detail, &p, 100, " of "); num_append(l->detail, &p, collected);
          ch_put(l->detail, &p, 100, " invoked, but every one threw uncaught");
      } else if (g_dt.lost > 0 || g_dt.refused > 0 || g_dt.exc > 0) {
          l->state = CH_PARTIAL; ch_put(l->detail, &p, 100, "executed ");
          num_append(l->detail, &p, invoked); ch_put(l->detail, &p, 100, " of ");
          num_append(l->detail, &p, collected); ch_put(l->detail, &p, 100, " (");
          num_append(l->detail, &p, g_dt.lost); ch_put(l->detail, &p, 100, " lost, ");
          num_append(l->detail, &p, g_dt.refused); ch_put(l->detail, &p, 100, " refused, ");
          num_append(l->detail, &p, g_dt.exc); ch_put(l->detail, &p, 100, " threw)");
      } else {
          l->state = CH_OK; ch_put(l->detail, &p, 100, "executed ");
          num_append(l->detail, &p, invoked); ch_put(l->detail, &p, 100, " of ");
          num_append(l->detail, &p, collected);
      }
      l->detail[p] = 0; }
    /* 4. module graph resolved -- js_module_stats() is a live cumulative
     * counter (the same one "load done" prints), read fresh here. */
    if (n < max) { struct ch_link *l = &out[n++]; l->name = "module graph resolved";
      int p = 0; int mods = 0, modfail = 0; js_module_stats(&mods, &modfail);
      if (!g_dt.have) { l->state = CH_UNKNOWN; ch_put(l->detail, &p, 100, "no page loaded in this tab yet"); }
      else if (mods == 0 && modfail == 0) { l->state = CH_OK; ch_put(l->detail, &p, 100, "no ES modules on this page"); }
      else if (mods == 0 && modfail > 0) { l->state = CH_ZERO; ch_put(l->detail, &p, 100, "0 loaded, "); num_append(l->detail, &p, modfail); ch_put(l->detail, &p, 100, " failed"); }
      else if (modfail > 0) { l->state = CH_PARTIAL; num_append(l->detail, &p, mods); ch_put(l->detail, &p, 100, " loaded, "); num_append(l->detail, &p, modfail); ch_put(l->detail, &p, 100, " failed"); }
      else { l->state = CH_OK; num_append(l->detail, &p, mods); ch_put(l->detail, &p, 100, " module(s) loaded"); }
      l->detail[p] = 0; }
    /* 5. CSS applied -- css_exlen is this file's own static (set by
     * load_once/browser_resize after css_expand_vars), read live. */
    if (n < max) { struct ch_link *l = &out[n++]; l->name = "CSS applied";
      int p = 0;
      if (css_exlen > 0) { l->state = CH_OK; num_append(l->detail, &p, css_exlen); ch_put(l->detail, &p, 100, " bytes in the cascade"); }
      else if (g_dt.have) { l->state = CH_ZERO; ch_put(l->detail, &p, 100, "0 bytes -- no stylesheet reached the cascade"); }
      else { l->state = CH_UNKNOWN; ch_put(l->detail, &p, 100, "no page loaded in this tab yet"); }
      l->detail[p] = 0; }
    /* 6. layout ran -- `ph` is this file's own static page height, read live. */
    if (n < max) { struct ch_link *l = &out[n++]; l->name = "layout ran";
      int p = 0;
      if (ph > 0) { l->state = CH_OK; num_append(l->detail, &p, ph); ch_put(l->detail, &p, 100, " px tall"); }
      else if (g_dt.have) { l->state = CH_ZERO; ch_put(l->detail, &p, 100, "page height is 0 -- layout produced no boxes"); }
      else { l->state = CH_UNKNOWN; ch_put(l->detail, &p, 100, "no page loaded in this tab yet"); }
      l->detail[p] = 0; }
    /* 7. hydration held -- ALWAYS UNKNOWN. Said out loud rather than guessed:
     * nothing in this tree counts a framework's own hydration pass (React,
     * Vue, ...) succeeding or failing; the nearest proxy, an uncaught
     * exception during the load/DOMContentLoaded dispatch, is not currently
     * exposed by js_page.c as a counter this file can read. Rule 5: a link
     * that cannot be watched failing is worse than no link. */
    if (n < max) { struct ch_link *l = &out[n++]; l->name = "hydration held";
      int p = 0; l->state = CH_UNKNOWN;
      ch_put(l->detail, &p, 100, "not instrumented -- no framework-hydration signal exists in this build");
      l->detail[p] = 0; }
    /* 8. text painted -- browser_paint_text_counts(), the SAME g_ptx_runs /
     * g_ptx_chars browser_paint_text_dump() prints to the serial console.
     * This is the headline: stripe.com went 69 -> 38 -> 0 painted text runs
     * with no failed request and no missing subresource anywhere else in
     * this chain, which is exactly why this link exists. */
    if (n < max) { struct ch_link *l = &out[n++]; l->name = "text painted";
      int p = 0; int runs = 0, chars = 0; browser_paint_text_counts(&runs, &chars);
      if (!g_dt.have) { l->state = CH_UNKNOWN; ch_put(l->detail, &p, 100, "no page loaded in this tab yet"); }
      else if (runs == 0) { l->state = CH_ZERO; ch_put(l->detail, &p, 100, "0 runs painted -- the page is visually blank"); }
      else { l->state = CH_OK; num_append(l->detail, &p, runs); ch_put(l->detail, &p, 100, " run(s), "); num_append(l->detail, &p, chars); ch_put(l->detail, &p, 100, " char(s) painted"); }
      l->detail[p] = 0; }
    return n;
}

#define DT_ROW 24
#define DT_MAXROWS 8

static void draw_devtools_panel(void)
{
    int dh = DT_ROW * DT_MAXROWS + 44;
    if (dh > win_h - VIEW_Y - 18) dh = win_h - VIEW_Y - 18;
    int px = 0, pw = win_w, py = win_h - 18 - dh, phh = dh;
    gui_glass(px, py, pw, phh, 0, 20, 22, 28, 245);
    gui_text(px + 14, py + 8, rgb(230, 230, 235), "DevTools -- the rendering chain");
    gui_text(px + pw - 210, py + 8, rgb(150, 150, 160), "F12 / Cmd+Alt+I closes");
    struct ch_link links[DT_MAXROWS];
    int n = devtools_chain(links, DT_MAXROWS);
    int y = py + 32;
    int first_zero = -1;
    for (int i = 0; i < n; i++) if (links[i].state == CH_ZERO) { first_zero = i; break; }
    for (int i = 0; i < n; i++) {
        struct ch_link *l = &links[i];
        if (i == first_zero) gui_glass(px + 6, y - 3, pw - 12, DT_ROW - 2, 5, 220, 70, 70, 40);
        gui_rect(px + 14, y + 3, 10, 10, ch_color(l->state));
        gui_text(px + 32, y, rgb(225, 225, 230), l->name);
        gui_text(px + 240, y, ch_color(l->state), ch_word(l->state));
        gui_text(px + 340, y, rgb(170, 170, 180), l->detail);
        y += DT_ROW;
    }
    if (first_zero >= 0) {
        char msg[140]; int p = 0;
        ch_put(msg, &p, 140, "first zero factor: ");
        ch_put(msg, &p, 140, links[first_zero].name);
        msg[p] = 0;
        gui_text(px + 14, py + phh - 16, rgb(255, 150, 150), msg);
    }
}

static void draw_panel(void)
{
    if (g_panel == PANEL_DEVTOOLS) { draw_devtools_panel(); return; }
    int px = 40, pw = win_w - 80;
    if (pw < 240) { px = 4; pw = win_w - 8; }
    int py = VIEW_Y + 8, phh = VIEW_H - 16;
    gui_glass(px, py, pw, phh, 12, 255, 255, 255, 235);
    const char *name = g_panel == PANEL_HISTORY ? "History"
                     : g_panel == PANEL_BOOKMARKS ? "Bookmarks" : "Downloads";
    gui_text(px + 14, py + 8, rgb(30, 30, 40), name);
    if (g_panel == PANEL_HISTORY) {
        gui_glass(px + 110, py + 6, pw - 130, 20, 6, 255, 255, 255, 200);
        gui_text(px + 116, py + 8, rgb(60, 60, 72), g_findlen ? g_find : "type to search");
        gui_rect(px + 116 + g_findlen * 8, py + 8, 8, 16, rgb(90, 150, 240));
    }
    int rows[HISTORY_MAX];
    int n = panel_list(rows, HISTORY_MAX);
    int vis = panel_rows();
    if (g_panel_sel >= n) g_panel_sel = n - 1;
    if (g_panel_sel < 0) g_panel_sel = 0;
    if (g_panel_sel < g_panel_top) g_panel_top = g_panel_sel;
    if (g_panel_sel >= g_panel_top + vis) g_panel_top = g_panel_sel - vis + 1;
    if (g_panel_top < 0) g_panel_top = 0;
    int y = py + 36;
    for (int i = g_panel_top; i < n && i < g_panel_top + vis; i++) {
        char u[TAB_URL], ti[TAB_TITLE];
        panel_row_text(g_panel, rows[i], u, ti);
        if (i == g_panel_sel) gui_glass(px + 8, y - 2, pw - 16, PANEL_ROW, 5, 120, 170, 255, 120);
        char lab[80];
        tab_label(ti[0] ? ti : u, (pw / 2) - 24, lab, (int)sizeof lab);
        gui_text(px + 14, y, rgb(25, 25, 35), lab);
        tab_label(u, (pw / 2) - 24, lab, (int)sizeof lab);
        gui_text(px + pw / 2, y, rgb(120, 120, 132), lab);
        y += PANEL_ROW;
    }
    if (n == 0)
        gui_text(px + 14, py + 40, rgb(140, 140, 150),
                 g_panel == PANEL_HISTORY ? "nothing matches" : "nothing here yet");
}

/* The window changed size.
 *
 * A FUNCTION and not four lines inside the event switch, for one reason: the
 * bug this file just carried was a second layout path that disagreed about the
 * width, and the way to stop that recurring is for there to be one place that
 * answers "the window is now this big" -- reachable by the test, so the test
 * drives the shipped code rather than a copy of it.
 *
 * EV_RESIZE is NOT ADVISORY: the canvas behind the window has already been
 * reallocated when it arrives, and the compositor is showing a STRETCHED copy
 * of the old one until we paint. An app that ignores it does not keep its old
 * layout -- it shows a magnified one for ever.
 *
 * The cascade re-runs before layout because @media, vw and vh are all functions
 * of the viewport: laying out again without re-styling would move the boxes and
 * leave every width:50vw box at its old size. */
void browser_resize(int w, int h);
void browser_resize(int w, int h)
{
    if (w > 100 && h > 100) { win_w = w; win_h = h; remember_size(); }
    css_viewport(win_w, win_h);
    if (g_root) {
        css_apply(g_root, css_expanded, css_exlen);
        css_extra_apply(g_root, css_expanded, css_exlen);
        layout_page(g_root, win_w);
        ph = layout_height();
    }
    int maxs = ph - VIEW_H; if (maxs < 0) maxs = 0;
    if (scroll > maxs) scroll = maxs;
    sync_scroll();
    /* The `resize` event, fired once per real EV_RESIZE, after the new
     * viewport is installed and the page has been re-styled and re-laid-out
     * against it -- so a listener's own getBoundingClientRect() calls see the
     * new geometry, not the old one. The caller (the EV_RESIZE case in the
     * main loop) sets `need = 1` and repaints on its next iteration, which is
     * after this returns -- so this always runs before the frame that shows
     * the new size, never during the initial layout of a fresh page (nothing
     * calls browser_resize() from load_once/load, only EV_RESIZE does), which
     * is the one case the spec says must NOT fire it. */
    struct js_event_init ri = { 0 };
    js_dom_dispatch(js_dom_root(), "resize", &ri);
}

/* The contenteditable caret + selection, drawn over the page. Defined with the
 * rest of the editing wiring further down (it needs the display list and the
 * geometry helpers); declared here because redraw() is the only caller. */
static void draw_ce_overlay(void);
/* The ordinary page-text selection highlight -- see its own header, next to
 * doc_pos_from_click, for why this is a separate overlay from draw_ce_overlay
 * rather than a case inside it. */
static void draw_doc_selection(void);

/* THE ADDRESS BAR, alone -- pulled out of redraw() so there is exactly one
 * place that draws it, called both by a full redraw() and by redraw_chrome()
 * below. Two copies of this block would have been the address bar's "one jar,
 * two doors": the day one of them gained a control the other did not, a
 * chrome-only repaint would show a caret in the wrong place, or not show one
 * at all, and nothing would say why. */
static void draw_address_bar(int editing)
{
    /* Liquid Glass address bar + a glass URL field */
    gui_glass(0, TABH, win_w, BARH, 1, 255, 255, 255, 70);
    gui_glass(10, TABH + 5, win_w - 20, 20, 8, 255, 255, 255, 95);
    /* The selection highlight goes UNDER the text, exactly like every real
     * text field -- drawn first so gui_text's glyphs paint over it. usel ==
     * ucaret is "no selection" by construction (addr_move/addr_home/addr_end
     * collapse it there), so this is a no-op then. */
    if (editing && usel != ucaret) {
        int a = ucaret < usel ? ucaret : usel;
        int b = ucaret < usel ? usel : ucaret;
        gui_rect(14 + a * 8, TABH + 6, (b - a) * 8, 18, rgb(140, 180, 250));
    }
    gui_text(14, TABH + 7, rgb(40, 40, 48), url);
    /* The caret: a thin bar AT ucaret, not at ulen -- this is the whole fix
     * for "append-at-end only". Before this the address bar had no caret to
     * draw, only a block glued to the end of whatever had been typed. */
    if (editing) gui_rect(14 + ucaret * 8, TABH + 7, 2, 16, rgb(90, 150, 240));
    /* a star for "this page is bookmarked", right-aligned in the field */
    if (bookmark_find(url) >= 0) gui_text(win_w - 26, TABH + 7, rgb(240, 180, 60), "*");
}

static void redraw(int editing)
{
    gui_clear(rgb(252, 252, 253));
    draw_tab_strip();
    draw_address_bar(editing);
    /* the page */
    browser_paint(0, VIEW_Y, win_w, VIEW_H, scroll);
    draw_doc_selection();
    draw_ce_overlay();
    draw_select_popup();
    if (g_panel) draw_panel();
    /* glass status line (frosts the bottom of the page) */
    gui_glass(0, win_h - 18, win_w, 18, 1, 255, 255, 255, 70);
    gui_text(10, win_h - 16, rgb(110, 110, 120), status);
    gui_flush();
}

/* THE NARROW CASE gui_flush_rect exists for: the caller (the EV_KEY handling
 * below) has already proven that the ONLY thing this frame changed is the
 * address bar's text and caret -- not scroll, not the DOM, not the tab strip,
 * not the panel, not the window size. Every OTHER pixel on the canvas is
 * already exactly what a full redraw() would draw again, because nothing that
 * feeds it changed, so re-drawing just this band and telling the compositor
 * only THIS band moved is the honest report, not an approximation of one.
 *
 * gui_clear() is NOT called here, deliberately: redraw()'s whole-canvas clear
 * is what makes drawing on top of stale pixels safe there, and this function
 * has no such clear to lean on -- gui_rect() over exactly the address bar's
 * band stands in for it, so draw_address_bar() starts from the same solid
 * background it always does and this band ends up BYTE-IDENTICAL to what a
 * full redraw would have put there. Skipping that reset (on the theory that
 * "we are only adding a character, the glass is already right") would double
 * the address bar's own glass tint on every single keystroke -- gui_glass()
 * frosts whatever is already in the surface, and it is not its own inverse. */
static void redraw_chrome(int editing)
{
    gui_rect(0, TABH, win_w, BARH, rgb(252, 252, 253));
    draw_address_bar(editing);
    gui_flush_rect(0, TABH, win_w, BARH);
}

/* ---- input -> DOM events ----
 *
 * Everything below turns a `struct logit_event` into a trusted DOM event and
 * lets the page have first refusal on it. The value the dispatch returns is the
 * "proceed with the default action" answer -- that is the whole contract, and
 * it is what makes preventDefault() observable instead of decorative. */

static int mods_of(const struct logit_event *e, struct js_event_init *ji)
{
    ji->shift = (e->mods & EV_MOD_SHIFT) != 0;
    ji->ctrl  = (e->mods & EV_MOD_CTRL) != 0;
    ji->alt   = (e->mods & EV_MOD_ALT) != 0;
    return e->mods;
}

/* DOM button numbering: 0 left, 1 middle, 2 right. The ABI numbers them 1/2/3
 * with right == 2, so it is a remap, not a subtraction. */
static int dom_button(int btn)
{ return btn == EV_BTN_RIGHT ? 2 : btn == EV_BTN_MIDDLE ? 1 : 0; }

/* Is `anc` `n` itself, or one of its ancestors? Walks up from `n`, which is
 * the only direction a `struct node` can be walked -- there is no downward
 * "is descendant" test available cheaply, so every caller below is written
 * to ask the question this way around. */
static int node_is_self_or_ancestor(struct node *anc, struct node *n)
{
    if (!anc) return 0;
    for (; n; n = n->parent) if (n == anc) return 1;
    return 0;
}

/* mouseover/mouseout/mouseenter/mouseleave, synthesised from consecutive
 * EV_MOUSE_MOVE hit-test results -- the ABI has no "entered element" event
 * of its own (logit_abi.h:249 is coalesced motion, nothing else), so this is
 * the browser reconstructing it the way every real one does.
 *
 * `from`/`to` are already-validated element nodes (or NULL); the caller owns
 * the serial check, because only the caller holds the generation this
 * pointer was tracked under across the frames in between.
 *
 * THE SPEC DISTINCTION THAT MATTERS: over/out bubble and fire once each, at
 * the old/new target; enter/leave do NOT bubble, and instead fire
 * individually at every node between the target and the nearest common
 * ancestor of `from` and `to` (exclusive) -- because that is the only way a
 * non-bubbling event can tell an ancestor "the pointer is now inside you"
 * without also telling it about a transition between two of ITS children.
 * Getting the two pairs' semantics swapped produces events that look
 * plausible and break every hover menu that nests a submenu.
 *
 * ORDER, per spec and reproduced here: out, then leave (innermost target
 * first, walking outward to the common ancestor), then over, then enter
 * (outermost newly-entered ancestor first, walking inward to the target).
 * leave and enter walk in opposite directions on purpose -- leave is
 * "tell each node it is no longer inside", starting at the leaf; enter is
 * "tell each node it is now inside", which has to start at the top or an
 * inner ancestor would learn about the pointer before its own parent did.
 *
 * relatedTarget is NOT set -- this Event implementation has no property for
 * it (grep confirms zero existing EG_* getters or MouseEvent-constructor
 * support for it) and rule 1 says absent beats present-and-wrong: a
 * fabricated relatedTarget that always reads null would pass a page's
 * `typeof e.relatedTarget` check and then make every
 * `if (this.contains(e.relatedTarget)) return;` boundary guard useless,
 * which is worse than the property not existing at all. */
static void fire_hover_transition(struct node *from, struct node *to,
                                   const struct js_event_init *base)
{
    if (from == to) return;
    struct js_event_init ji = *base;
    ji.detail = 0;

    if (from) {
        ji.bubbles = 1; ji.cancelable = 1;
        js_dom_dispatch(from, "mouseout", &ji);
    }
    if (from) {
        ji.bubbles = 0; ji.cancelable = 0;
        for (struct node *p = from; p; p = p->parent) {
            if (node_is_self_or_ancestor(p, to)) break;
            js_dom_dispatch(p, "mouseleave", &ji);
        }
    }
    if (to) {
        ji.bubbles = 1; ji.cancelable = 1;
        js_dom_dispatch(to, "mouseover", &ji);
    }
    if (to) {
        /* Collect innermost-to-outermost (same walk as the leave loop above),
         * then fire in the reverse order -- see the ORDER note above for why
         * enter must reach the target last, not first. A fixed-depth stack is
         * fine: this only holds one page's worth of ancestors between the
         * pointer target and the deepest shared one, and a page nested deeper
         * than this just stops synthesising `enter` for the outermost few,
         * which is a truncation, not a wrong answer, for a case nothing in
         * this corpus reaches. */
        struct node *stack[64];
        int n2 = 0;
        for (struct node *p = to; p; p = p->parent) {
            if (node_is_self_or_ancestor(p, from)) break;
            if (n2 < (int)(sizeof stack / sizeof stack[0])) stack[n2++] = p;
        }
        ji.bubbles = 0; ji.cancelable = 0;
        for (int i = n2 - 1; i >= 0; i--)
            js_dom_dispatch(stack[i], "mouseenter", &ji);
    }
}

/* Is `k` one of the eight enumerated KEY_* navigation codes (logit_abi.h),
 * rather than a character -- ASCII or a Unicode code point above it (the
 * pinyin IME commits CJK this way)? Enumerated rather than range-tested
 * against KEY_UP/KEY_RIGHT so a future non-contiguous KEY_* addition cannot
 * silently start being typed into a field or a contenteditable. */
static int is_nav_key(int k)
{
    return k == KEY_UP || k == KEY_DOWN || k == KEY_PGUP || k == KEY_PGDN ||
           k == KEY_HOME || k == KEY_END || k == KEY_LEFT || k == KEY_RIGHT;
}

static int key_utf8_encode(unsigned cp, char out[4])
{
    if (cp < 0x80)    { out[0] = (char)cp; return 1; }
    if (cp < 0x800)   { out[0] = (char)(0xC0 | (cp >> 6));
                        out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12));
                        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* The KeyboardEvent `key`/`code` for our key codes. Only the named keys need a
 * table; a printable character is its own `key`, which is exactly what the DOM
 * says -- for a code point above ASCII (a pinyin candidate committed through
 * EV_KEY) that means the UTF-8 encoding of it, not "Unidentified": `one` must
 * hold up to 4 bytes + NUL, which is why every caller declares it `char
 * one[5]` and not the `char one[2]` this used to be. */
static const char *key_name(int k, char *one)
{
    switch (k) {
    case KEY_UP:    return "ArrowUp";
    case KEY_DOWN:  return "ArrowDown";
    case KEY_LEFT:  return "ArrowLeft";
    case KEY_RIGHT: return "ArrowRight";
    case KEY_PGUP:  return "PageUp";
    case KEY_PGDN:  return "PageDown";
    case KEY_HOME:  return "Home";
    case KEY_END:   return "End";
    case '\n':      return "Enter";
    case '\b':      return "Backspace";
    case '\t':      return "Tab";
    case 0x1b:      return "Escape";
    }
    if (k >= ' ' && k < 0x7f) { one[0] = (char)k; one[1] = 0; return one; }
    if (k > 0x7F && !is_nav_key(k)) {
        int n = key_utf8_encode((unsigned)k, one);
        one[n] = 0;
        return one;
    }
    return "Unidentified";
}

/* ============================ keyboard shortcuts ===========================
 *
 * ONE TABLE, because the window-management line owns the system shortcut table
 * and this one has to be handed over rather than rewritten. What the ABI
 * already settles (include/abi/logit_abi.h, EV_MOD_SUPER): the WM intercepts a
 * CLOSED list -- Cmd+W/Q/M/Tab/` -- before the focused app sees it, and
 * forwards every other Cmd combination to the app with EV_MOD_SUPER set.
 *
 * So the browser gets Cmd+T, Cmd+1..9, Cmd+D, Cmd+Y, Cmd+B and cannot have
 * Cmd+W or Cmd+Tab. Closing a TAB is therefore Ctrl+W and cycling tabs is
 * Ctrl+Tab -- Ctrl and not Cmd precisely because Cmd+W already means "close the
 * WINDOW" and a browser where the same chord sometimes closes a tab and
 * sometimes the window is worse than one where they are different keys.
 *
 * The Ctrl fallbacks also mean every one of these works TODAY: the keyboard
 * driver reports EV_MOD_SUPER, but wm.c's interception list is this round's
 * work on another line, and an app that only listened for Cmd would do nothing
 * at all until that lands. Ctrl+letter arrives as a CONTROL CODE (the driver
 * folds it: Ctrl+T is 0x14), which is why the table carries both forms. */
#define CTRL_OF(c)  ((c) - 'a' + 1)

/* Returns 1 if the key was a chrome shortcut and has been handled. */
static int is_cmd(const struct logit_event *e) { return (e->mods & EV_MOD_SUPER) != 0; }

/* ======================= focus, form controls, submission ==================
 *
 * The piece that was missing. Until this block existed, browser.c line 1860
 * routed every keystroke to <body> with the comment "No focus model yet", and
 * the consequence -- stated the way it should have been stated then -- was that
 * NO WEB PAGE ON THIS MACHINE COULD ACCEPT A SINGLE CHARACTER.
 *
 * Three seams meet here and nowhere else:
 *
 *   focus.c owns WHICH element has the keyboard, and knows nothing about the
 *   window, the scroll or QuickJS.
 *
 *   forms.c owns WHAT a control holds and what a keystroke does to it, and
 *   knows nothing about events beyond a function pointer.
 *
 *   this file owns the WINDOW: it installs that function pointer (so the DOM
 *   events focus.c and forms.c raise are built by whoever owns event
 *   construction), it decides what the default action is when a page does not
 *   preventDefault it, and it is the only place that may navigate.
 */

/* The dispatcher focus.c and forms.c call through. `bubbles`/`cancelable` come
 * from the caller because the spec assigns them per event type and the two
 * files that raise them are the ones that know which type they are raising. */
static int forms_dispatch(struct node *target, const char *type,
                          int bubbles, int cancelable)
{
    struct js_event_init ji = { 0 };
    ji.bubbles = bubbles;
    ji.cancelable = cancelable;
    /* `key` non-NULL is what makes js_dom_dispatch build a KeyboardEvent, and a
     * focus or input event is not one -- so it is deliberately left unset and
     * these arrive on the generic pointer-shaped prototype. The event line owns
     * FocusEvent/InputEvent proper; this is the seam, not the shape. */
    return js_dom_dispatch(target, type, &ji);
}

/* Is `n` inside the current document? Everything below refuses to act on a node
 * that a script has already detached. */
static struct node *doc_root_of(struct node *n)
{
    while (n && n->parent) n = n->parent;
    return n;
}

/* The element a click at `n` should give focus to. A click on the TEXT of a
 * <label> focuses (and toggles) the control the label is for, which is how a
 * very large share of real checkboxes are actually operated -- and a click that
 * lands on a non-focusable element must walk UP, because a <span> inside a
 * <button> is what the hit test returns. */
static struct node *focus_target_for_click(struct node *n, struct node **label_out)
{
    if (label_out) *label_out = 0;
    for (struct node *p = n; p && p->type == N_ELEM; p = p->parent) {
        if (fc_kind(p) != FC_NONE && focus_is_focusable(p)) return p;
        if (p->tag[0] == 'l' && p->tag[1] == 'a' && p->tag[2] == 'b' &&
            p->tag[3] == 'e' && p->tag[4] == 'l' && p->tag[5] == 0) {
            struct node *t = fc_label_target(p);
            if (t && focus_is_focusable(t)) { if (label_out) *label_out = p; return t; }
        }
        if (focus_is_focusable(p)) return p;
    }
    return 0;
}

/* Focus a control and remember its value, so `change` has something to compare
 * against when focus leaves. */
static void focus_control(struct node *n)
{
    struct node *old = focus_current();
    if (old == n) return;
    if (old && fc_kind(old) != FC_NONE) fc_commit(old);
    focus_set(n);
    if (n && fc_kind(n) != FC_NONE) fc_mark_focus(n);
}

/* ---- submission -------------------------------------------------------- */

/* A form submission is a NAVIGATION, and this browser has exactly one of those
 * (load() through bfetch). So a GET form is turned into a URL and handed to the
 * same follow_link() a clicked <a> goes through -- no second network path, and
 * therefore no second set of redirect, cookie and history rules to get wrong.
 *
 * POST IS NOT WIRED, and saying so is the honest answer rather than sending the
 * fields as a query string and calling it done. bfetch (browser_rt.c, another
 * line's file) builds a GET and only a GET; giving it a method and a body is a
 * change to that file, not to this one. What IS here: the payload is built and
 * unit-tested, the `submit` event fires and is cancelable, and a POST form says
 * so in the status bar instead of silently navigating to the wrong URL. */
static char g_submit_buf[8192];

/* ---- the <select> popup ------------------------------------------------ */
/* Drawn by browser.c and not by the painter, because it has to float above
 * everything in the display list and the display list has no z-order above
 * itself. The open list's geometry is recomputed from the control's box each
 * frame, so a scroll or a re-layout can never leave it stranded. */
static struct node *g_popup;             /* the open <select>, or NULL */
static uint32_t     g_popup_serial;
static int          g_popup_hi;          /* highlighted row */

#define POPUP_ROW 22
#define POPUP_MAXROWS 12

static void popup_close(void)
{ if (g_popup) fc_select_set_open(g_popup, 0); g_popup = 0; g_popup_serial = 0; }

static struct node *popup_live(void)
{
    if (!g_popup) return 0;
    if (g_popup->serial != g_popup_serial) { g_popup = 0; return 0; }
    return g_popup;
}

/* The control's border box in DOCUMENT coordinates, from the display list.
 * Returns 0 if the control has no box (display:none, or not laid out yet). */
static int control_box(struct node *n, int *bx, int *by, int *bw, int *bh)
{
    const struct item *it = layout_items();
    int cnt = layout_count();
    for (int i = 0; i < cnt; i++) {
        if (it[i].type != IT_CONTROL || it[i].node != n) continue;
        *bx = it[i].x; *by = it[i].y; *bw = it[i].w; *bh = it[i].h;
        return 1;
    }
    return 0;
}

/* ---- the contenteditable caret, on screen -------------------------------
 *
 * WHY THE GEOMETRY IS HERE AND NOT IN forms.c. A contenteditable's text is not
 * a control's string -- it is ordinary page text, laid out by layout.c into the
 * display list like every other word on the page. So the caret's pixel position
 * is a question about the DISPLAY LIST, and forms.c deliberately holds no
 * layout dependency (that is what keeps eight host test binaries linking). The
 * caret is drawn as an OVERLAY after browser_paint, the same way the <select>
 * popup is and for the same reason: it has to sit above content the display
 * list has no z-order above.
 *
 * The link between the two is one subtraction. layout.c emits one IT_TEXT per
 * word with `text` pointing INTO the text node's own buffer, so
 * `item.text - node->text` is that word's byte offset within the node, and the
 * caret's offset picks out the word it falls in. */
/* browser_rt.c's cached measurer -- the same one layout.c measured the runs
 * with. Declared rather than included for the reason layout.c and forms.c both
 * give: measuring through logit.h's raw text_measure_px would issue a syscall
 * per word and, worse, could disagree with the widths layout already used.
 *
 * The last argument is a FACE MASK (LOGIT_FACE_MONO | LOGIT_FACE_BOLD), and
 * every call below composes it from the display ITEM rather than passing
 * it->mono alone. That is not tidiness: these four calls place the caret and
 * the selection highlight by re-measuring a prefix of a run the painter has
 * already drawn, so measuring a <strong> or an <h1> at regular weight while
 * it is drawn at bold puts the caret progressively further left the further
 * into the run it goes -- a drift, not an offset, which is the kind nobody
 * reproduces. */
int text_measure(const char *s, int len, int px, int face);
#define ITEM_FACE(it) ((it)->mono | ((it)->bold ? LOGIT_FACE_BOLD : 0))

static int ce_run_for(struct node *t, int off, const struct item **out, int *rel)
{
    const struct item *it = layout_items();
    int cnt = layout_count();
    const struct item *best = 0;
    int brel = 0;
    for (int i = 0; i < cnt; i++) {
        if (it[i].type != IT_TEXT || it[i].node != t || !it[i].text) continue;
        long base = it[i].text - t->text;
        if (base < 0 || base > t->textlen) continue;           /* not this buffer */
        if (off < base || off > base + it[i].len) continue;
        best = &it[i];
        brel = off - (int)base;
        break;                    /* the first run that covers it: a caret at a
                                   * word's end belongs to that word, not to the
                                   * start of the next one */
    }
    if (!best) return 0;
    if (out) *out = best;
    if (rel) *rel = brel;
    return 1;
}

/* The caret rectangle in DOCUMENT coordinates. 0 when there is no caret, or the
 * page it belonged to has been replaced. */
static int ce_caret_box(int *cx, int *cy, int *ch)
{
    struct node *n = 0;
    int off = 0;
    if (!fc_ce_selection(0, 0, &n, &off)) return 0;   /* the FOCUS end blinks */
    if (!fc_ce_host(n)) return 0;

    if (n->type == N_TEXT) {
        const struct item *r = 0;
        int rel = 0;
        if (ce_run_for(n, off, &r, &rel)) {
            *cx = r->x + text_measure(r->text, rel, r->font_px, ITEM_FACE(r));
            *cy = r->y;
            *ch = r->h > 0 ? r->h : r->font_px;
            return 1;
        }
        /* An empty run, or a node laid out nowhere (collapsed whitespace):
         * fall through to the containing element's box. */
        n = n->parent;
    }
    /* An ELEMENT position -- the empty composer. Its box is a plain IT_RECT, so
     * the caret goes at the content's start. Without this the composer nobody
     * has typed into yet shows no caret at all, which is indistinguishable from
     * a click that did not focus anything. */
    for (struct node *e = n; e; e = e->parent) {
        const struct item *it = layout_items();
        int cnt = layout_count();
        for (int i = 0; i < cnt; i++) {
            if (it[i].node != e || it[i].type == IT_TEXT) continue;
            int fh = it[i].font_px > 0 ? it[i].font_px : 16;
            *cx = it[i].x + 2;
            *cy = it[i].y + 2;
            *ch = it[i].h > 4 && it[i].h < fh * 3 ? it[i].h - 4 : fh;
            return 1;
        }
    }
    return 0;
}

/* Caret + selection, over the painted page. The selection is a TINT rather than
 * a filled rect: this runs after the text is on screen, so anything opaque
 * would hide the very characters it is meant to show as selected. */
static void draw_ce_overlay(void)
{
    if (!FOCUS_ROUTING) return;
    const struct item *it = layout_items();
    int cnt = layout_count();
    for (int i = 0; i < cnt; i++) {
        if (it[i].type != IT_TEXT || !it[i].node || it[i].node->type != N_TEXT) continue;
        int a = 0, b = 0;
        if (!fc_ce_run_range(it[i].node, &a, &b)) continue;
        long base = it[i].text - it[i].node->text;
        int r0 = a - (int)base, r1 = b - (int)base;
        if (r0 < 0) r0 = 0;
        if (r1 > it[i].len) r1 = it[i].len;
        if (r1 <= r0) continue;
        int x0 = it[i].x + text_measure(it[i].text, r0, it[i].font_px, ITEM_FACE(&it[i]));
        int x1 = it[i].x + text_measure(it[i].text, r1, it[i].font_px, ITEM_FACE(&it[i]));
        int sy = VIEW_Y + it[i].y - scroll;
        if (sy + it[i].h < VIEW_Y || sy > VIEW_Y + VIEW_H) continue;
        /* radius 1, not 0: fb_liquid_glass_cut() (c/kernel/gui/fb.c) reads
         * "radius < 1" as "nothing to draw" and returns before touching a
         * single pixel -- a guard written for a genuinely empty w<=0/h<=0
         * call that also silently swallows a caller asking for a SQUARE
         * panel. Measured on the device: a radius-0 call here produced 0
         * changed pixels across a whole selected word, confirmed by
         * comparing before/after screendumps byte-for-byte. 1px of corner
         * rounding on a text-height band is not visible; a highlight nobody
         * can see is not a highlight. */
        gui_glass(x0, sy, x1 - x0, it[i].h, 1, 90, 150, 240, 110);
    }
    int cx, cy, chh;
    if (!ce_caret_box(&cx, &cy, &chh)) return;
    int sy = VIEW_Y + cy - scroll;
    if (sy + chh < VIEW_Y || sy > VIEW_Y + VIEW_H) return;
    gui_rect(cx, sy, 2, chh, rgb(30, 30, 40));
}

/* Place the caret from a click inside an editing host. `vx`,`vy` are viewport
 * coordinates (the caller has already subtracted VIEW_Y).
 *
 * Aims at the TEXT RUN under the pointer, not at the element the hit test
 * returned: browser_hittest_node() climbs to an element because a DOM event
 * target cannot be a text node, and a caret has to go the other way. Finding no
 * run is the empty composer, and fc_ce_caret_in handles it -- that path is not
 * a fallback, it is the case that matters most. */
static void ce_caret_from_click(struct node *host, int vx, int vy)
{
    int dy = vy + scroll;
    const struct item *it = layout_items();
    int cnt = layout_count();
    const struct item *hit = 0;
    long bestd = -1;
    for (int i = 0; i < cnt; i++) {
        if (it[i].type != IT_TEXT || it[i].hidden) continue;
        if (!it[i].node || it[i].node->type != N_TEXT) continue;
        if (fc_ce_host(it[i].node) != host) continue;
        if (dy < it[i].y || dy >= it[i].y + it[i].h) continue;
        /* On the pointer's LINE. Nearest run horizontally, so a click past the
         * end of a short line still lands on that line's last word instead of
         * missing everything. */
        long d = 0;
        if (vx < it[i].x) d = it[i].x - vx;
        else if (vx > it[i].x + it[i].w) d = vx - (it[i].x + it[i].w);
        if (bestd < 0 || d < bestd) { bestd = d; hit = &it[i]; }
    }
    if (!hit) { fc_ce_caret_in(host, 1); return; }

    /* The byte in the run nearest the pointer. Linear over the run's characters
     * for the reason fc_offset_at_px gives: the measurement is monotone in
     * characters and not in bytes, so a binary search over bytes is a bug
     * waiting for a multi-byte character. */
    int relx = vx - hit->x;
    if (relx < 0) relx = 0;
    int best = 0;
    long bd = -1;
    for (int i = 0; i <= hit->len; ) {
        int w = text_measure(hit->text, i, hit->font_px, ITEM_FACE(hit));
        long d = w > relx ? w - relx : relx - w;
        if (bd < 0 || d < bd) { bd = d; best = i; }
        if (i >= hit->len) break;
        i++;
        while (i < hit->len && ((unsigned char)hit->text[i] & 0xC0) == 0x80) i++;
    }
    long base = hit->text - hit->node->text;
    fc_ce_set_caret(hit->node, (int)base + best);
}

/* ====================== PAGE TEXT SELECTION ================================
 *
 * "no copy and paste" was the owner's complaint in its most literal form:
 * there was no way to select the PAGE's own text at all -- inside a form
 * field or a contenteditable it already worked (forms.c, and ce_* just
 * above), but the words of an ordinary paragraph could not be touched. This
 * is that gap, built as the SAME KIND of overlay ce_caret_from_click and
 * draw_ce_overlay already are: a click/drag hit test against layout_items()
 * (the exact array browser_paint() drew from -- see its own header on why
 * that is the one true source of "what is on screen"), a highlight drawn as
 * a translucent overlay after the page, and Ctrl+C into the real kernel
 * clipboard (SYS_CLIP_SET, c/kernel/gui/clipboard.c).
 *
 * NOT layered on fc_ce_*: that model's selection lives inside a `struct
 * fctl` keyed to ONE editing host and is walked/committed by forms.c's own
 * lifetime rules (see the comment above ce_caret_from_click). Ordinary page
 * text has no host and no fctl -- it is two bare (node, byte offset) points,
 * ANCHOR (where the drag/shift-click started) and FOCUS (where the pointer
 * is now), each re-validated by the node's serial before every use exactly
 * the way press_node/hover_node/lastclick_node already are in app_main --
 * so a selection spanning a subtree a script deletes mid-drag collapses to
 * "no selection" instead of reading freed memory.
 *
 * ce_run_for() above is reused AS-IS for turning a (node, offset) back into
 * the display-list run that covers it -- it was already host-agnostic (it
 * takes a text node and an offset, nothing about `host`), so this is the
 * SAME lookup ce_caret_box uses, not a second one. */

static struct node *g_psel_an, *g_psel_fo;   /* anchor / focus TEXT nodes */
static int          g_psel_ano, g_psel_foo;  /* their byte offsets */
static uint32_t     g_psel_anser, g_psel_foser;
static int          g_psel_active;           /* focus has actually moved from anchor */
static int          g_psel_dragging;         /* left button down, tracking a drag */

static int psel_live(void)
{
    return g_psel_active && g_psel_an && g_psel_fo &&
           g_psel_an->serial == g_psel_anser && g_psel_fo->serial == g_psel_foser;
}

static void psel_clear(void)
{ g_psel_active = 0; g_psel_dragging = 0; g_psel_an = g_psel_fo = 0; }

static void psel_begin(struct node *n, int off)
{
    if (!n) { psel_clear(); return; }
    g_psel_an = g_psel_fo = n;
    g_psel_ano = g_psel_foo = off;
    g_psel_anser = g_psel_foser = n->serial;
    g_psel_active = 0;      /* a bare click: no highlight until FOCUS moves */
}

static void psel_extend_to(struct node *n, int off)
{
    if (!n || !g_psel_an) return;
    g_psel_fo = n; g_psel_foo = off; g_psel_foser = n->serial;
    g_psel_active = !(g_psel_fo == g_psel_an && g_psel_foo == g_psel_ano);
}

/* Byte classification for double-click word selection. >=0x80 (any UTF-8
 * continuation or lead byte) counts as a word character too, so a
 * double-click inside a run of Chinese/Japanese text -- which this bar's own
 * IME can commit -- selects the whole run instead of one byte of it. */
static int psel_is_wordch(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}

/* Expand (node, off) to the word boundaries around it, over the WHOLE text
 * node's buffer rather than just the one wrapped run under the pointer --
 * layout only ever wraps BETWEEN words, so a word never straddles two runs,
 * and scanning the run alone would silently stop at its edge for the first
 * or last word on a wrapped line. Landing on a boundary BETWEEN two words
 * (whitespace/punctuation) collapses to a zero-width selection there rather
 * than guessing which side was meant. */
static void psel_word_at(struct node *t, int off, int *a, int *b)
{
    int len = t->textlen; const char *s = t->text;
    int i = off; if (i < 0) i = 0; if (i > len) i = len;
    int want;
    if (i < len && psel_is_wordch((unsigned char)s[i])) want = 1;
    else if (i > 0 && psel_is_wordch((unsigned char)s[i - 1])) { want = 1; i--; }
    else { *a = off; *b = off; return; }
    int lo = i, hi = i + 1;
    while (lo > 0 && psel_is_wordch((unsigned char)s[lo - 1])) lo--;
    while (hi < len && psel_is_wordch((unsigned char)s[hi])) hi++;
    *a = lo; *b = hi;
}

/* The nearest (text node, byte offset) to a viewport click point -- the same
 * hit test as ce_caret_from_click just above, generalised to the WHOLE
 * document instead of one editing host's runs (dropping the
 * `fc_ce_host(...) != host` filter is the entire difference). Returns 0 if
 * the click did not land on any line of text at all. */
static int doc_pos_from_click(int vx, int vy, struct node **out_n, int *out_off)
{
    int dy = vy + scroll;
    const struct item *it = layout_items();
    int cnt = layout_count();
    const struct item *hit = 0;
    long bestd = -1;
    for (int i = 0; i < cnt; i++) {
        if (it[i].type != IT_TEXT || it[i].hidden) continue;
        if (!it[i].node || it[i].node->type != N_TEXT) continue;
        if (dy < it[i].y || dy >= it[i].y + it[i].h) continue;
        long d = 0;
        if (vx < it[i].x) d = it[i].x - vx;
        else if (vx > it[i].x + it[i].w) d = vx - (it[i].x + it[i].w);
        if (bestd < 0 || d < bestd) { bestd = d; hit = &it[i]; }
    }
    if (!hit) return 0;
    int relx = vx - hit->x;
    if (relx < 0) relx = 0;
    int best = 0;
    long bd = -1;
    for (int i = 0; i <= hit->len; ) {
        int w = text_measure(hit->text, i, hit->font_px, ITEM_FACE(hit));
        long d = w > relx ? w - relx : relx - w;
        if (bd < 0 || d < bd) { bd = d; best = i; }
        if (i >= hit->len) break;
        i++;
        while (i < hit->len && ((unsigned char)hit->text[i] & 0xC0) == 0x80) i++;
    }
    long base = hit->text - hit->node->text;
    if (out_n)   *out_n = hit->node;
    if (out_off) *out_off = (int)base + best;
    return 1;
}

/* Anchor and focus in VISUAL order, as (layout_items() index, byte-within-run)
 * pairs -- item index is a fair stand-in for reading order because layout
 * emits IT_TEXT runs in the order it laid them out, top to bottom, left to
 * right within a line, which is paint order for every document this engine
 * lays out (no bidi reordering of the display list itself -- see the text
 * section of this tree's own notes on where bidi analysis does and does not
 * reach). Returns 0 if either end no longer resolves to a run (a re-layout
 * moved the node's text, or DOM edits shrank it under the stored offset). */
static int psel_bounds(int *lo_idx, int *lo_rel, int *hi_idx, int *hi_rel)
{
    if (!psel_live() || !g_psel_active) return 0;
    const struct item *ra = 0, *rb = 0; int rela = 0, relb = 0;
    if (!ce_run_for(g_psel_an, g_psel_ano, &ra, &rela)) return 0;
    if (!ce_run_for(g_psel_fo, g_psel_foo, &rb, &relb)) return 0;
    const struct item *base = layout_items();
    int ia = (int)(ra - base), ib = (int)(rb - base);
    if (ia < ib || (ia == ib && rela <= relb)) { *lo_idx = ia; *lo_rel = rela; *hi_idx = ib; *hi_rel = relb; }
    else                                       { *lo_idx = ib; *lo_rel = relb; *hi_idx = ia; *hi_rel = rela; }
    return 1;
}

/* Select the whole document: anchor at byte 0 of the first painted text run,
 * focus at the end of the last -- what Ctrl+A means for a page (as opposed
 * to a focused field, which forms.c's own Ctrl+A already owns and claims
 * first; see control_key/ce_key above). */
static int psel_select_all(void)
{
    const struct item *it = layout_items();
    int cnt = layout_count();
    const struct item *first = 0, *last = 0;
    for (int i = 0; i < cnt; i++) {
        if (it[i].type != IT_TEXT || it[i].hidden) continue;
        if (!it[i].node || it[i].node->type != N_TEXT) continue;
        if (!first) first = &it[i];
        last = &it[i];
    }
    if (!first) return 0;
    long lb = last->text - last->node->text;
    g_psel_an = first->node; g_psel_ano = 0;                     g_psel_anser = first->node->serial;
    g_psel_fo = last->node;  g_psel_foo = (int)lb + last->len;   g_psel_foser = last->node->serial;
    g_psel_active = 1;
    return 1;
}

/* The selection highlight, drawn as a translucent overlay AFTER the page --
 * same reasoning as draw_ce_overlay's own comment: this runs once the text
 * is already on screen, so an opaque fill would hide the very words it is
 * meant to show as selected. Called from redraw() beside draw_ce_overlay(). */
static void draw_doc_selection(void)
{
    int lo_idx, lo_rel, hi_idx, hi_rel;
    if (!psel_bounds(&lo_idx, &lo_rel, &hi_idx, &hi_rel)) return;
    const struct item *it = layout_items();
    int cnt = layout_count();
    if (hi_idx >= cnt) hi_idx = cnt - 1;
    for (int i = lo_idx; i <= hi_idx; i++) {
        if (it[i].type != IT_TEXT || it[i].hidden) continue;
        int r0 = (i == lo_idx) ? lo_rel : 0;
        int r1 = (i == hi_idx) ? hi_rel : it[i].len;
        if (r0 < 0) r0 = 0;
        if (r1 > it[i].len) r1 = it[i].len;
        if (r1 <= r0) continue;
        int x0 = it[i].x + text_measure(it[i].text, r0, it[i].font_px, ITEM_FACE(&it[i]));
        int x1 = it[i].x + text_measure(it[i].text, r1, it[i].font_px, ITEM_FACE(&it[i]));
        int sy = VIEW_Y + it[i].y - scroll;
        if (sy + it[i].h < VIEW_Y || sy > VIEW_Y + VIEW_H) continue;
        /* radius must be >= 1: fb_liquid_glass_cut (c/kernel/gui/fb.c) has
         * `if (w <= 0 || h <= 0 || radius < 1) return` -- a radius of 0 is
         * silently a NO-OP, not a square-cornered glass panel. Measured: the
         * selection bookkeeping (psel_bounds/psel_copy) was already correct
         * -- Ctrl+A + Ctrl+C round-tripped the right text through the real
         * clipboard on device -- but this call passed radius 0 and painted
         * NOTHING, so the highlight silently never appeared. 1px is not
         * visually distinguishable from 0 at this box size; it just crosses
         * fb.c's own floor. */
        gui_glass(x0, sy, x1 - x0, it[i].h, 1, 90, 150, 240, 110);
    }
}

/* Ctrl+C: the selected text, concatenated in visual order, out to the real
 * kernel clipboard. A single space is inserted between two runs that are
 * not each other's immediate continuation in the SAME text node (a run
 * boundary crossing into a different element, or a wrapped line) -- without
 * it, "<span>New</span><span>York</span>" and a line-wrapped "New York"
 * would both paste as "NewYork", silently gluing two words into one. Exact
 * whitespace fidelity is not the goal; not corrupting a word boundary is.
 * Returns 1 if anything was copied. */
static int psel_copy(void)
{
    int lo_idx, lo_rel, hi_idx, hi_rel;
    if (!psel_bounds(&lo_idx, &lo_rel, &hi_idx, &hi_rel)) return 0;
    const struct item *it = layout_items();
    int cnt = layout_count();
    if (hi_idx >= cnt) hi_idx = cnt - 1;
    static char buf[8192];
    int o = 0;
    const char *prev_end = 0;
    for (int i = lo_idx; i <= hi_idx; i++) {
        if (it[i].type != IT_TEXT || it[i].hidden) continue;
        int r0 = (i == lo_idx) ? lo_rel : 0;
        int r1 = (i == hi_idx) ? hi_rel : it[i].len;
        if (r0 < 0) r0 = 0;
        if (r1 > it[i].len) r1 = it[i].len;
        if (r1 <= r0) continue;
        if (o > 0 && it[i].text != prev_end && o < (int)sizeof buf &&
            buf[o - 1] != ' ' && buf[o - 1] != '\n')
            buf[o++] = ' ';
        for (int k = r0; k < r1 && o < (int)sizeof buf - 1; k++) buf[o++] = it[i].text[k];
        prev_end = it[i].text + r1;
    }
    if (o <= 0) return 0;
    clip_set(CLIP_F_TEXT, buf, o);
    return 1;
}

/* Re-style and re-lay-out after an EDIT changed the DOM.
 *
 * Not settle_dom(): that one asks js_dom.c what a SCRIPT invalidated, and an
 * edit made by the keyboard is not a script mutation -- js_dom.c never saw it
 * and would report INVAL_NONE. The scope is the editing host, which is the
 * smallest thing that is certainly enough: Enter creates elements that have no
 * computed style at all yet, and layout.c reads `node->style`. */
static int ce_settle(struct node *host)
{
    if (!g_root) return 0;
    if (host) css_apply_scoped(host, 0, css_expanded, css_exlen);
    else      css_apply(g_root, css_expanded, css_exlen);
    layout_page(g_root, win_w);
    ph = layout_height();
    return 1;
}

/* 1 if the browser navigated (so the caller stops draining events).
 *
 * `fire_event` is 0 only for form.submit(), which the spec defines as NOT
 * firing the submit event -- the difference between it and requestSubmit() is
 * exactly that, and a page that calls submit() from inside its own submit
 * handler would otherwise recurse. */
static int form_submit_ex(struct node *form, struct node *submitter, int fire_event)
{
    if (!form) return 0;
    /* `submit` fires AT THE FORM, bubbles and is cancelable -- and a page that
     * cancels it and does its own fetch() is the single most common shape of
     * form handling on the modern web, so honouring the cancel matters more
     * than the navigation does. */
    if (fire_event && !forms_dispatch(form, "submit", 1, 1)) {
        set_status("submit cancelled by the page");
        return 0;
    }
    int n = fc_encode(form, submitter, g_submit_buf, (int)sizeof g_submit_buf);
    if (n < 0) { set_status("form is too large to submit"); return 0; }

    char target[700];
    int o = 0;
    const char *act = fc_action(form);
    if (act[0]) {
        while (act[o] && o < 640) { target[o] = act[o]; o++; }
    } else {
        /* No action: this page. The existing query and fragment are dropped,
         * which is what the spec's "URL record with the query replaced" means
         * and what a search box on a results page depends on. */
        for (int i = 0; url[i] && url[i] != '?' && url[i] != '#' && o < 640; i++)
            target[o++] = url[i];
    }
    target[o] = 0;

    if (fc_method_post(form)) {
        /* Deliberately loud rather than silently wrong. See the comment above:
         * the payload is built and correct, the network path is not this
         * line's file. */
        set_status("POST form: payload built, but POST is not wired yet");
        printf("[browser] FORM-POST %s body=%s\n", target, g_submit_buf);
        return 0;
    }

    int q = 0;
    while (target[q] && target[q] != '?' && target[q] != '#') q++;
    target[q] = 0;
    if (n > 0 && q < 660) {
        target[q++] = '?';
        for (int i = 0; i < n && q < (int)sizeof target - 1; i++) target[q++] = g_submit_buf[i];
    }
    target[q] = 0;
    printf("[browser] FORM-GET %s\n", target);
    follow_link(target);
    return 1;
}

static int form_submit(struct node *form, struct node *submitter)
{ return form_submit_ex(form, submitter, 1); }

/* The default form for an implicit submission (Enter in a text field), and the
 * form a submit button belongs to. */
static int implicit_submit(struct node *ctl)
{
    struct node *form = fc_form_of(ctl);
    if (!form) return 0;
    return form_submit(form, 0);
}

static void draw_select_popup(void)
{
    struct node *sel = popup_live();
    if (!sel) return;
    int bx, by, bw, bh;
    if (!control_box(sel, &bx, &by, &bw, &bh)) { popup_close(); return; }
    int n = fc_option_count(sel);
    int rows = n > POPUP_MAXROWS ? POPUP_MAXROWS : n;
    if (rows <= 0) { popup_close(); return; }
    int px = bx, py = VIEW_Y + by - scroll + bh;
    int pw = bw < 140 ? 140 : bw;
    int ph2 = rows * POPUP_ROW + 8;
    /* Flip above the control when there is no room below -- a dropdown that
     * runs off the bottom of the window is a dropdown you cannot use. */
    if (py + ph2 > win_h - 18) {
        int above = VIEW_Y + by - scroll - ph2;
        if (above > VIEW_Y) py = above;
    }
    gui_clip(0, VIEW_Y, win_w, VIEW_H);
    gui_rrect(px, py, pw, ph2, 6, rgb(0xB0, 0xB4, 0xBA));
    gui_rrect(px + 1, py + 1, pw - 2, ph2 - 2, 5, rgb(0xFF, 0xFF, 0xFF));
    int cur = fc_selected_index(sel);
    for (int i = 0; i < rows; i++) {
        char lbl[128];
        struct node *o = fc_option_at(sel, i);
        int l = o ? fc_option_label(o, lbl, (int)sizeof lbl) : 0;
        int ry = py + 4 + i * POPUP_ROW;
        if (i == g_popup_hi || (g_popup_hi < 0 && i == cur))
            gui_rrect(px + 3, ry, pw - 6, POPUP_ROW, 4, rgb(0x25, 0x63, 0xEB));
        unsigned col = (i == g_popup_hi) ? rgb(255, 255, 255) : rgb(0x1D, 0x1D, 0x1F);
        gui_text_run(px + 10, ry + 3, 14, 0, col, lbl, l);
    }
    if (n > rows) {
        char more[32];
        int p = 0;
        const char *pre = "...";
        while (*pre) more[p++] = *pre++;
        more[p] = 0;
        gui_text_run(px + 10, py + 4 + rows * POPUP_ROW - 12, 12, 0, rgb(150, 150, 158), more, p);
    }
    gui_clip(0, 0, 0, 0);
}

/* Activate a control the way a click or Space/Enter does. */
static int control_activate(struct node *n, int *navigated)
{
    int k = fc_kind(n);
    if (FC_IS_TOGGLE(k)) {
        if (fc_disabled(n)) return 1;
        if (k == FC_RADIO) fc_set_checked(n, 1);
        else               fc_set_checked(n, !fc_checked(n));
        forms_dispatch(n, "input", 1, 0);
        forms_dispatch(n, "change", 1, 0);
        return 1;
    }
    if (FC_IS_BUTTON(k)) {
        if (fc_disabled(n)) return 1;
        /* The button's own `click` has already been dispatched by the caller
         * for a mouse click; for a keyboard activation it has not, so it is
         * raised here and its cancellation suppresses the submit. */
        if (k == FC_RESET) { fc_reset_form(fc_form_of(n)); return 1; }
        if (k == FC_SUBMIT || k == FC_IMAGEBTN) {
            struct node *form = fc_form_of(n);
            if (form && form_submit(form, n)) { if (navigated) *navigated = 1; }
        }
        return 1;
    }
    if (k == FC_SELECT) {
        if (fc_disabled(n)) return 1;
        if (popup_live() == n) popup_close();
        else { popup_close(); g_popup = n; g_popup_serial = n->serial;
               g_popup_hi = fc_selected_index(n); fc_select_set_open(n, 1); }
        return 1;
    }
    return 0;
}

/* Move the caret one visual line in a <textarea>. Kept here rather than in
 * forms.c because "a line" is a wrapping question and forms.c does not lay
 * anything out -- this is the HARD-BREAK version, which is right for a textarea
 * whose content has explicit newlines and approximate for one relying on soft
 * wrapping. Named as an approximation rather than hidden as one. */
static int textarea_line_move(struct node *n, int down, int extend)
{
    int vl = 0;
    const char *v = fc_value(n, &vl);
    int s0, s1;
    fc_selection(n, &s0, &s1);
    int pos = down ? s1 : s0;
    int ls = pos; while (ls > 0 && v[ls - 1] != '\n') ls--;
    int col = pos - ls;
    int np;
    if (down) {
        int le = pos; while (le < vl && v[le] != '\n') le++;
        if (le >= vl) return 0;
        int ns = le + 1, ne = ns;
        while (ne < vl && v[ne] != '\n') ne++;
        np = ns + col; if (np > ne) np = ne;
    } else {
        if (ls == 0) return 0;
        int pe = ls - 1, ps = pe;
        while (ps > 0 && v[ps - 1] != '\n') ps--;
        np = ps + col; if (np > pe) np = pe;
    }
    if (extend) { int a = s0 == s1 ? pos : (down ? s0 : s1);
                  fc_set_selection(n, np < a ? np : a, np < a ? a : np); }
    else fc_set_selection(n, np, np);
    return 1;
}

/* A keystroke that reached a focused control. Returns 1 if the control
 * consumed it -- in which case the browser's own default action (scrolling,
 * history) must NOT also happen, which is the whole point of a focus model. */
static int control_key(struct node *n, int k, const struct logit_event *ev,
                       int *navigated)
{
    int kind = fc_kind(n);
    if (kind == FC_NONE) return 0;
    int shift = (ev->mods & EV_MOD_SHIFT) != 0;
    int ctrl  = (ev->mods & EV_MOD_CTRL) != 0 || (ev->mods & EV_MOD_SUPER) != 0;

    if (kind == FC_SELECT) {
        int cnt = fc_option_count(n), cur = fc_selected_index(n);
        if (k == KEY_DOWN || k == KEY_UP) {
            int nx = cur + (k == KEY_DOWN ? 1 : -1);
            if (nx < 0) nx = 0;
            if (nx >= cnt) nx = cnt - 1;
            if (nx != cur && nx >= 0) {
                fc_set_selected_index(n, nx);
                g_popup_hi = nx;
                forms_dispatch(n, "input", 1, 0);
                forms_dispatch(n, "change", 1, 0);
            }
            return 1;
        }
        if (k == '\n' || k == ' ') { control_activate(n, navigated); return 1; }
        if (k == 0x1b) { popup_close(); return 1; }
        return 0;
    }

    if (FC_IS_TOGGLE(kind) || FC_IS_BUTTON(kind)) {
        if (k == ' ' || (k == '\n' && FC_IS_BUTTON(kind))) {
            /* A keyboard activation still raises `click`, and a page that
             * preventDefaults it must not get the submit. */
            struct js_event_init ji = { 0 };
            ji.bubbles = 1; ji.cancelable = 1; ji.detail = 1;
            if (!js_dom_dispatch(n, "click", &ji)) return 1;
            control_activate(n, navigated);
            return 1;
        }
        if (k == '\n' && FC_IS_TOGGLE(kind)) { if (implicit_submit(n)) *navigated = 1; return 1; }
        return 0;
    }

    if (!FC_IS_TEXTUAL(kind)) return 0;

    /* ---- a text field ---- */
    if (k == '\n') {
        if (kind == FC_TEXTAREA) return fc_edit_insert(n, "\n", 1) ? 1 : 1;
        /* Enter in a single-line field COMMITS (fires `change`) and then
         * implicitly submits the form -- which is exactly what a search box
         * is, and the reason this whole line of work exists. */
        fc_commit(n);
        if (implicit_submit(n)) *navigated = 1;
        return 1;
    }
    if (k == '\b') return fc_edit_backspace(n) ? 1 : 1;
    if (k == 0x1b) { focus_control(0); return 1; }
    if (k == KEY_LEFT)  { fc_edit_move(n, -1, ctrl, shift); return 1; }
    if (k == KEY_RIGHT) { fc_edit_move(n, +1, ctrl, shift); return 1; }
    if (k == KEY_HOME)  { fc_edit_home(n, shift); return 1; }
    if (k == KEY_END)   { fc_edit_end(n, shift); return 1; }
    if (k == KEY_UP || k == KEY_DOWN) {
        if (kind != FC_TEXTAREA) return 0;      /* let the page scroll */
        textarea_line_move(n, k == KEY_DOWN, shift);
        return 1;
    }
    /* Ctrl+letter arrives folded to a control code (keyboard.c). The chrome's
     * own shortcut table has already had its turn on these, so only the ones it
     * does not claim reach here. */
    if (k == 0x01) { fc_edit_select_all(n); return 1; }                 /* Ctrl+A */
    if (k == 0x03 || k == 0x18) {                                       /* copy / cut */
        int s0, s1, vl = 0;
        fc_selection(n, &s0, &s1);
        const char *v = fc_value(n, &vl);
        if (s1 > s0) clip_set(CLIP_F_TEXT, v + s0, s1 - s0);
        if (k == 0x18) fc_edit_insert(n, "", 0);
        return 1;
    }
    if (k == 0x16) {                                                    /* Ctrl+V */
        static char pb[4096];
        int got = clip_get(CLIP_F_TEXT, pb, (int)sizeof pb);
        if (got > 0) fc_edit_insert(n, pb, got);
        return 1;
    }
    if (k >= ' ' && k < 0x7f) { char c = (char)k; fc_edit_insert(n, &c, 1); return 1; }
    /* A code point above ASCII (the pinyin IME commits a CJK candidate this
     * way): UTF-8 encode it and hand fc_edit_insert the whole character as one
     * splice. fc_edit_insert/splice are already byte-transparent -- forms.c's
     * own comment above step_left says UTF-8 is stepped by character there --
     * so the only bug was here, one truncating `(char)k` away. */
    if (k > 0x7F && !is_nav_key(k)) {
        char enc[4]; int el = key_utf8_encode((unsigned)k, enc);
        fc_edit_insert(n, enc, el);
        return 1;
    }
    return 0;
}

/* A keystroke that reached a focused CONTENTEDITABLE. Same contract as
 * control_key: 1 means the editing host consumed it, so the browser's own
 * default (scrolling, history) must not also happen.
 *
 * `*dirty` is set when the DOM changed, because unlike a text field -- whose
 * value is a string forms.c owns -- an edit here moves boxes and the page has
 * to be re-styled and re-laid-out before it can be painted.
 *
 * ENTER IS NOT SPECIAL-CASED HERE and that is the point: a chat composer
 * cancels it in its own keydown handler to send the message, the page's keydown
 * has already had its turn by the time this runs, and the caller only calls
 * this when the page did NOT cancel. So "Enter sends" and "Enter makes a new
 * paragraph" are the same code path with the page choosing. */
static int ce_key(struct node *host, int k, const struct logit_event *ev, int *dirty)
{
    int shift = (ev->mods & EV_MOD_SHIFT) != 0;
    int ctrl  = (ev->mods & EV_MOD_CTRL) != 0 || (ev->mods & EV_MOD_SUPER) != 0;
    if (!host || fc_disabled(host)) return 0;
    /* The caret may never have been placed: focus arrived by Tab, or a script
     * called focus(). Put it at the end of the content, which is where a real
     * browser puts it. */
    if (!fc_ce_selection(0, 0, 0, 0) || fc_ce_caret_host() != host)
        fc_ce_caret_in(host, 1);

    switch (k) {
    case '\n':   if (fc_ce_enter(shift)) *dirty = 1; return 1;
    case '\b':   if (fc_ce_backspace())  *dirty = 1; return 1;
    /* NO KEY REACHES THIS ON THIS MACHINE. The PS/2 driver
     * (c/drivers/char/keyboard.c, a kernel file and another line's) maps the
     * E0 arrows, Home, End and the page keys and does not map E0 0x53, so
     * forward-Delete never arrives. The operation is real and host-tested; the
     * key that would invoke it is one line in a file this line may not edit. */
    case 0x7f:   if (fc_ce_delete())     *dirty = 1; return 1;
    case 0x1b:   focus_control(0); fc_ce_clear(); return 1;
    case KEY_LEFT:  fc_ce_move(-1, ctrl, shift); return 1;
    case KEY_RIGHT: fc_ce_move(+1, ctrl, shift); return 1;
    case KEY_HOME:  fc_ce_home(shift); return 1;
    case KEY_END:   fc_ce_end(shift); return 1;
    case 0x01:      fc_ce_select_all(host); return 1;                  /* Ctrl+A */
    default: break;
    }
    if (k == 0x03 || k == 0x18) {                                      /* copy / cut */
        static char cb[4096];
        int got = fc_ce_selection_text(cb, (int)sizeof cb);
        if (got > 0) clip_set(CLIP_F_TEXT, cb, got);
        if (k == 0x18 && got > 0 && fc_ce_backspace()) *dirty = 1;
        return 1;
    }
    if (k == 0x16) {                                                   /* Ctrl+V */
        static char pb[4096];
        int got = clip_get(CLIP_F_TEXT, pb, (int)sizeof pb);
        if (got > 0 && fc_ce_insert(pb, got)) *dirty = 1;
        return 1;
    }
    /* Up/Down are left to the page: this caret is paragraph-scoped and has no
     * notion of a visual line, so moving by one would be a guess. Saying so is
     * better than guessing wrong -- the page scrolls, which is what an
     * unhandled arrow does. */
    if (k >= ' ' && k < 0x7f) {
        char c = (char)k;
        if (fc_ce_insert(&c, 1)) *dirty = 1;
        return 1;
    }
    /* A code point above ASCII: UTF-8 encode it and insert all of its bytes as
     * one call, same shape as the ASCII branch. Same bug this file's control_key
     * had -- a chat composer (a contenteditable, not a form control) could not
     * receive a CJK candidate at all before this. */
    if (k > 0x7F && !is_nav_key(k)) {
        char enc[4]; int el = key_utf8_encode((unsigned)k, enc);
        if (fc_ce_insert(enc, el)) *dirty = 1;
        return 1;
    }
    return 0;
}

void app_main(void)
{
    /* Arm the painted-text record. Only here: browser_paint.c is linked by
     * five host harnesses that render pages without being a browser, and it
     * must not write into their output. */
    browser_paint_text_log(1);
    css_init();             /* build the UA default stylesheet */
    win_query_size();
    css_viewport(win_w, win_h);  /* @media/vw/vh evaluate against the real window */
    /* css_extra patches node->style after the cascade, so a scoped re-style has
     * to run it before it decides whether anything changed -- see css.h. */
    css_set_post_pass(css_extra_apply);
    img_init();             /* register PNG + GIF decoders */
    js_page_set_clock(clock_ms);
    /* focus.c and forms.c raise DOM events through a function pointer rather
     * than including js_dom.h -- they are compiled into BROWSER_PIPE, which has
     * no QuickJS include path. This is where that pointer is installed, and it
     * is the ONLY coupling between the focus/forms model and the script engine:
     * uninstalled (the host tests), focus still moves and typing still works,
     * the page simply does not hear about it. */
    fc_set_dispatch(forms_dispatch);
    /* Submitting is a navigation and this file owns navigation, so forms.c and
     * the JS bindings reach it through a pointer rather than the other way
     * round. */
    fc_set_submit(form_submit_ex);
    /* The size is chosen BEFORE the window exists, and the cascade is told
     * about it again afterwards: css_viewport was called above with the
     * placeholder, and @media/vw/vh would otherwise evaluate against a window
     * that never existed. */
    pick_born_size();
    gui_create("Browser", win_w, win_h);
    win_set_min();
    win_query_size();       /* the WM may have clamped what we asked for */
    css_viewport(win_w, win_h);

    /* ---- the session, before the first paint ----
     *
     * Order matters: the store has to exist before anything reads it, and the
     * tab list has to exist before the strip is drawn. A restored tab comes
     * back with its URL and title and NO bytes, so restoring eight tabs costs
     * eight strings, not eight page loads -- only the one you are looking at
     * loads, and the others load when you first select them. */
    int want_restore = 1;
#ifndef LOADERHOST_LOGIT_H
    tabs_set_store(&os_store);
    /* WHETHER to restore is a PREFERENCE, and preferences belong in the
     * machine's settings store (SYS_SETTING_*, c/kernel/core/settings.c), which
     * says in its own header that another line should use it rather than build
     * a second one. So this line does.
     *
     * WHAT to restore does not go there, and the reason is a measurement, not a
     * preference: SET_VALLEN is 80 bytes and SET_MAXKV is 64 keys for the whole
     * machine. A URL is up to 600 bytes and a session is up to twelve of them,
     * with a 256-entry history beside it -- one tab would not fit in one value,
     * and the history alone would exhaust the machine's entire key budget. The
     * bulk therefore lives in /browser/*, which is what /etc/settings.conf is
     * too: a file. The store holds the switch; the files hold the data. */
    want_restore = setting_int("browser.restore_session", 1);
#endif
    tabs_init();
    history_load();
    bookmarks_load();
    int restored = want_restore ? session_restore() : 0;
    /* On the serial console, because a tab strip's labels are too small to read
     * out of a screendump and "did the session come back" needs an answer that
     * a CI harness can grep for. */
    printf("[browser] session restored %d tabs (restore=%d)\n", restored, want_restore);
#ifndef LOADERHOST_LOGIT_H
    /* DIAGNOSTIC PROBE (2026-08-16). What it found, so the next reader does not
     * re-derive it: every GUI app launched by the Dock runs with caps=0x0 --
     * wm_launch's proc_create() never grants a capability (named as `not_done`
     * in the comment above proc.c's `p->caps = 0`), and since the M28 gate
     * landed (24130fcef, c/kernel/exec/syscall.c syscall_dispatch's cap check)
     * every CAP_NET and CAP_FS syscall from a GUI process is refused with -1.
     * That is why this line prints `rodata=-1 stack=-1 bss=-1 caps=0x0` on
     * today's builds, why session restore reads 0 tabs, and why the site
     * scoreboard's self-test fails machine-wide. DELETE this probe when
     * wm_launch grants capabilities and the scoreboard self-test passes again;
     * until then it is the one serial line that names the blocker. Sockets are
     * closed immediately; gateway:9 answers nothing, which is fine --
     * sock_open returning a handle is the whole measurement. */
    { static char bsshost[16] = "10.0.2.2";
      char stackhost[16]; for (int i = 0; i < 9; i++) stackhost[i] = "10.0.2.2"[i];
      int a = sock_open("10.0.2.2", 9, 0);
      int b = sock_open(stackhost, 9, 0);
      int c = sock_open(bsshost, 9, 0);
      long caps = _sys(SYS_CAP_QUERY, 0, 0, 0);
      printf("[browser] sock probe: rodata=%d stack=%d bss=%d caps=0x%x\n",
             a, b, c, (unsigned)caps);
      if (a >= 0) sock_close(a);
      if (b >= 0) sock_close(b);
      if (c >= 0) sock_close(c); }
#endif
    if (restored <= 0) tabs_new(url);
    { struct tab *t = tab_cur();
      if (t && t->url[0]) { int i = 0;
          while (t->url[i] && i < (int)sizeof url - 1) { url[i] = t->url[i]; i++; }
          url[i] = 0; ulen = i; addr_sync(); } }
    if (restored > 0) {
        char st[96]; int p = 0; const char *pre = "restored ";
        while (*pre) st[p++] = *pre++;
        num_append(st, &p, restored);
        const char *post = " tabs -- Enter loads this one";
        while (*post) st[p++] = *post++;
        st[p] = 0;
        set_status(st);
    }

    redraw(1);
    int editing = 1;
    struct node *press_node = 0;      /* the element the last mousedown landed on */
    uint32_t press_serial = 0;
    struct node *hover_node = 0;      /* the element the pointer is currently over,
                                        * for synthesising mouseover/out/enter/leave
                                        * -- see fire_hover_transition(). */
    uint32_t hover_serial = 0;
    struct node *lastclick_node = 0;  /* dblclick: same-target, close-in-time,
                                        * close-in-space state, one slot -- a
                                        * third click clears it rather than
                                        * chaining, matching titlebar_double_click()
                                        * in wm.c (the platform's other double-click
                                        * detector, same 400ms window). */
    uint32_t lastclick_serial = 0;
    unsigned long long lastclick_ms = 0;
    int lastclick_x = 0, lastclick_y = 0;

    for (;;) {
        struct logit_event e;
        int need = 0;                 /* coalesce: drain the whole event burst, repaint once */
        int navigated = 0;
        /* CHROME-ONLY REPAINT TRACKING. `nev` counts events actually
         * processed this burst; `chrome_edit_only` is reset at the top of
         * EVERY iteration and set true by exactly two branches below (typing
         * or backspacing in the address bar) -- so after the loop it holds
         * the LAST event's classification, which is the ONLY event's
         * classification whenever nev == 1. That combination -- a burst of
         * exactly one event, and that event provably touched nothing but
         * url/ulen/the caret -- is the only condition redraw_chrome() is
         * trusted under; a multi-event burst always falls back to redraw(),
         * because proving every event in it was chrome-only would mean
         * auditing this whole loop instead of two branches in it. */
        int nev = 0, chrome_edit_only = 0;
        while (!navigated && poll_event(&e)) {
            nev++;
            chrome_edit_only = 0;
            sync_scroll();
            if (e.type == EV_CLOSE) {
                /* Record where the user was BEFORE tearing anything down: the
                 * whole value of a session is that it survives the thing that
                 * ended it. */
                { struct tab *t = tab_cur(); if (t) t->scroll = scroll; }
                session_save(); history_save(); bookmarks_save();
                /* The window size the user settled on. Set in RAM by every
                 * resize; this is the one write to disk. */
#ifndef LOADERHOST_LOGIT_H
                remember_size(); setting_commit();
#endif
                js_page_close(); bfetch_close_all(); app_exit(0);
            }
            if (e.type == EV_RESIZE) {
                browser_resize(e.a, e.b);
                need = 1;
                continue;
            }
            if (e.type == EV_KEY) {
                int k = e.a;
                int maxs = ph - VIEW_H; if (maxs < 0) maxs = 0;

                /* ---- chrome shortcuts, before anything else can eat them ----
                 * See the table above app_main for why both a Cmd and a Ctrl
                 * form exist. Handled here rather than after the page's keydown
                 * because Cmd+T must open a tab whatever the page thinks. */
                int handled = 0;
                /* F12: DevTools, unmodified (arrives via keyboard.c's F-key
                 * block, see KEY_F12 in logit_abi.h). Handled OUTSIDE the
                 * Cmd/Ctrl gate below because it carries no modifier at all.
                 *
                 * DEBOUNCED, not merely edge-triggered: this ABI has no
                 * key-release event, but the real hazard is not the missing
                 * release -- keyboard.c already forwards exactly one EV_KEY
                 * per physical press. It is the PS/2 controller's own
                 * typematic auto-repeat, which resends the make code ~30x/s
                 * after a ~250ms delay while the key is held; without this,
                 * a held F12 would toggle the panel thirty times a second.
                 * One transition per 200ms, against the same monotonic_ms()
                 * clock the load loop already uses elsewhere in this file --
                 * not a QMP `sendkey` gate, which sends one make+break and
                 * cannot reproduce typematic (the test-ime-os shape). */
                if (k == KEY_F12) {
                    static unsigned long long last_f12;
                    unsigned long long now = monotonic_ms();
                    if (now - last_f12 >= 200) {
                        last_f12 = now;
                        g_panel = g_panel == PANEL_DEVTOOLS ? PANEL_NONE : PANEL_DEVTOOLS;
                        g_panel_sel = g_panel_top = 0;
                    }
                    handled = 1;
                }
                if (!handled && (is_cmd(&e) || (e.mods & EV_MOD_CTRL))) {
                    int c = k;
                    if (c >= 1 && c <= 26) c = c + 'a' - 1;      /* the folded form */
                    if (c >= 'A' && c <= 'Z') c += 32;
                    /* FIRST IN THE CHAIN, and that is not a style choice:
                     * Ctrl+D three branches below bookmarks the page, so a
                     * chord that merely ADDS Alt to it would never be reached.
                     * Ctrl+Alt+D dumps the words that reached the screen to the
                     * serial console -- an instrument rather than a feature,
                     * which is also why it takes a modifier no page and no
                     * hand sends by accident. browser_paint.h says what it
                     * answers that `changed px` cannot. */
                    if (c == 'd' && (e.mods & EV_MOD_ALT)) {
                        browser_paint_text_dump();
                        set_status("painted text dumped to the serial console");
                        handled = 1;
                    } else if (c == 'i' && (e.mods & EV_MOD_ALT)) {
                        /* Cmd+Alt+I: DevTools. A SECOND accelerator next to
                         * F12 on purpose, not decoration -- see keyboard.c
                         * for why F12 needs a kernel-side change to arrive at
                         * all, and CLAUDE.md's IME-toggle story for why a
                         * single host-keyboard-dependent chord is not enough
                         * (F12 is a media key on this machine's actual
                         * keyboard by default). This chord needs no kernel
                         * change and is also the control for the key path
                         * itself: if this opens DevTools and F12 does not,
                         * the fault is in the key path, not in this panel. */
                        g_panel = g_panel == PANEL_DEVTOOLS ? PANEL_NONE : PANEL_DEVTOOLS;
                        g_panel_sel = g_panel_top = 0;
                        handled = 1;
                    } else if (c == 't') {                       /* new tab */
                        int n = tabs_new("");
                        if (n >= 0) { tab_dehydrate(); tabs_select(n);
                            url[0] = 0; ulen = 0; editing = 1; addr_sync();
                            set_status("new tab -- type a URL and press Enter");
                            session_save(); }
                        else set_status("too many tabs");
                        handled = 1;
                    } else if (c == 'w') {                       /* close tab */
                        int cur = tabs_active();
                        tab_dehydrate();
                        int nx = tabs_close(cur);
                        tabs_select(nx);
                        if (!tab_hydrate()) {
                            struct tab *t = tab_cur();
                            if (t && t->url[0]) { load(url); navigated = 1; }
                            else { url[0] = 0; ulen = 0; editing = 1; addr_sync();
                                   set_status("new tab -- type a URL and press Enter"); }
                        }
                        session_save();
                        handled = 1;
                    } else if (k == '\t') {                      /* cycle tabs */
                        /* The position is computed and THEN switched to, rather
                         * than using tabs_next(): that call moves the active
                         * index on its own, which would leave the old document
                         * live in the engine with a different tab selected --
                         * every switch has to go through tab_switch_to. */
                        int ord[TAB_MAX], n = tab_order(ord);
                        if (n > 1) {
                            int pos = 0;
                            for (int i = 0; i < n; i++) if (ord[i] == tabs_active()) { pos = i; break; }
                            pos += (e.mods & EV_MOD_SHIFT) ? -1 : 1;
                            if (pos < 0) pos = n - 1;
                            if (pos >= n) pos = 0;
                            tab_switch_to(ord[pos]);
                            editing = 0; navigated = 1;
                        }
                        handled = 1;
                    } else if (c >= '1' && c <= '9') {           /* nth tab */
                        int ord[TAB_MAX], n = tab_order(ord), want = c - '1';
                        if (c == '9') want = n - 1;              /* Cmd+9 = last, as everywhere */
                        if (want >= 0 && want < n && ord[want] != tabs_active()) {
                            tab_switch_to(ord[want]);
                            editing = 0; navigated = 1;
                        }
                        handled = 1;
                    } else if (c == 'd') {                       /* bookmark this page */
                        struct tab *t = tab_cur();
                        int at = bookmark_find(url);
                        if (at >= 0) { bookmark_remove(at); set_status("bookmark removed"); }
                        else { bookmark_add(url, t ? t->title : url); set_status("bookmarked"); }
                        bookmarks_save();
                        handled = 1;
                    } else if (c == 'y' || c == 'h') {           /* history */
                        g_panel = g_panel == PANEL_HISTORY ? PANEL_NONE : PANEL_HISTORY;
                        g_panel_sel = g_panel_top = 0; g_findlen = 0; g_find[0] = 0;
                        handled = 1;
                    } else if (c == 'b') {                       /* bookmarks */
                        g_panel = g_panel == PANEL_BOOKMARKS ? PANEL_NONE : PANEL_BOOKMARKS;
                        g_panel_sel = g_panel_top = 0;
                        handled = 1;
                    } else if (c == 'j') {                       /* downloads */
                        g_panel = g_panel == PANEL_DOWNLOADS ? PANEL_NONE : PANEL_DOWNLOADS;
                        g_panel_sel = g_panel_top = 0;
                        handled = 1;
                    } else if (c == 'l') {                       /* focus the bar */
                        /* FOCUS AND *SELECT*, which is what Ctrl+L does in
                         * every browser: the next keystroke REPLACES the
                         * address, it does not append to it. This USED to
                         * mean "clear it outright" -- ulen=0, url[0]=0 --
                         * with a comment admitting why: "There is no
                         * selection model in this bar, and clearing is what
                         * 'type over the selection' looks like from the
                         * outside." Now there is one (addr_select_all,
                         * addr_insert), so this does what the comment always
                         * said it wanted: the OLD address stays visible and
                         * selected -- see draw_address_bar's highlight band
                         * -- and the first keystroke replaces it via
                         * addr_insert's "delete the selection, then insert"
                         * path below, same as it always did. A person can
                         * also now press End (or Right) first to edit the
                         * existing address instead of retyping it whole.
                         *
                         * It used to only set `editing`, and the bug that hid
                         * behind that is worth naming because it hid well:
                         * tests/qmp/qmp_site.py drives every navigation with
                         * Ctrl+L then the URL, and its FIRST navigation is out
                         * of an empty tab -- so appending and replacing are
                         * the same thing and it worked for a year. The second
                         * navigation in a boot silently produced
                         * `https://site/what-was-typed`. Select-all-then-type
                         * preserves that: the harness's second navigation
                         * still ends with exactly what it typed, because the
                         * old address is the thing typing REPLACES. */
                        editing = 1; addr_select_all();
                        /* ONE LINE, because "the keystroke never arrived" and
                         * "it arrived and the bar did not take it" are
                         * different failures and the log could not tell them
                         * apart. MEASURED: after the canvas context landed,
                         * qmp_site.py's about:text step stopped reaching the
                         * guest on stripe -- three retries, no `[browser]
                         * load: about:text` -- and nothing anywhere said
                         * whether Ctrl+L had been seen at all. An instrument
                         * whose trigger cannot be observed is not an
                         * instrument; this makes the trigger observable. */
                        printf("[browser] ctrl+L: address bar focused\n");
                        handled = 1;
                    } else if (c == 'r') {                       /* reload */
                        /* Re-fetch the CURRENT address. No hist_push: a
                         * reload replaces what is on screen, it does not add
                         * a stop to Back/Forward -- the same distinction
                         * load()'s own redirect chain draws with
                         * hist_replace vs hist_push. Refused while the bar is
                         * being edited (a half-typed address is not "the
                         * current page") and on an empty bar (nothing has
                         * loaded yet, freshly booted or a bare new tab).
                         *
                         * RELOAD BYPASSES THE HTTP CACHE (webaccel,
                         * 2026-08-30). load() alone is indistinguishable
                         * from a link navigation at the fetcher's layer --
                         * same door, same flags -- so without this arm a
                         * reload serves heuristic-fresh subresources from
                         * memory, which is what any OTHER revisit does but
                         * is not what a user asking the server again means
                         * (RFC 9111's reload semantics). bfetch_set_bypass
                         * closes both doors for the duration of the load:
                         * no lookups, no stores. Minimal edit inside this
                         * branch on purpose; the rest of load() is anim's
                         * and is not touched. */
                        if (!editing && url[0]) {
                            bfetch_set_bypass(1);
                            load(url);
                            bfetch_set_bypass(0);
                            navigated = 1;
                        }
                        handled = 1;
                    } else if (c == 'f') {                       /* find in page */
                        /* browser_paint_text_find()'s own comment says the
                         * scope this can answer: the SAME record
                         * about:text prints, i.e. PAINTED text -- only what
                         * the last paint put on screen, not the whole
                         * document. One traversal answers both, on purpose
                         * (CLAUDE.md: a second walk here is the one-jar-
                         * two-doors trap this tree has paid for three
                         * times). The status line says so rather than
                         * implying a full-document search that was not
                         * done. Toggled by the same chord a second time. */
                        g_finding = !g_finding;
                        if (g_finding) {
                            g_pfqlen = 0; g_pfq[0] = 0; editing = 0;
                            set_status("find (on screen): type, Enter = count, Esc = close");
                        } else {
                            set_status("ready");
                        }
                        handled = 1;
                    }
                }
                if (handled) { need = 1; continue; }

                /* ---- the library panel owns the keyboard while it is open ---- */
                if (g_panel) {
                    int rows[HISTORY_MAX];
                    int n = panel_list(rows, HISTORY_MAX);
                    if (k == 0x1b) { g_panel = PANEL_NONE; }
                    else if (k == KEY_DOWN) g_panel_sel++;
                    else if (k == KEY_UP)   g_panel_sel--;
                    else if (k == KEY_PGDN) g_panel_sel += panel_rows();
                    else if (k == KEY_PGUP) g_panel_sel -= panel_rows();
                    else if (k == '\n') {
                        if (g_panel_sel >= 0 && g_panel_sel < n) {
                            char u[TAB_URL], ti[TAB_TITLE];
                            panel_row_text(g_panel, rows[g_panel_sel], u, ti);
                            if (u[0]) {
                                g_panel = PANEL_NONE; editing = 0;
                                int i = 0; while (u[i] && i < (int)sizeof url - 1) { url[i] = u[i]; i++; }
                                url[i] = 0; ulen = i; addr_sync();
                                hist_push(url); load(url); navigated = 1;
                            }
                        }
                    } else if (k == '\b') {
                        if (g_panel == PANEL_HISTORY && g_findlen > 0) g_find[--g_findlen] = 0;
                    } else if (g_panel == PANEL_HISTORY && k >= ' ' && k < 0x7f &&
                               g_findlen < (int)sizeof g_find - 1) {
                        g_find[g_findlen++] = (char)k; g_find[g_findlen] = 0;
                        g_panel_sel = g_panel_top = 0;
                    }
                    if (g_panel_sel < 0) g_panel_sel = 0;
                    need = 1;
                    continue;
                }
                /* ---- find-in-page owns the keyboard while it is open ----
                 *
                 * A minimal bar: type, Enter reports how many of the runs
                 * about:text would print contain the query (case-
                 * insensitive substring), Esc closes. There is no on-screen
                 * highlight yet -- browser_paint.h's own comment says why
                 * this cannot become "jump to the Nth occurrence" without
                 * either scrolling first (the record only covers what is
                 * already visible) or a second walk of the layout tree,
                 * which is exactly the duplicate traversal this was built to
                 * avoid. */
                if (g_finding) {
                    if (k == 0x1b) { g_finding = 0; set_status("ready"); }
                    else if (k == '\b') { if (g_pfqlen > 0) g_pfq[--g_pfqlen] = 0; }
                    else if (k == '\n') {
                        if (g_pfqlen > 0) {
                            int runs = browser_paint_text_find(g_pfq);
                            char st[96]; int p = 0;
                            const char *pre = "find: "; while (*pre) st[p++] = *pre++;
                            for (int i = 0; i < g_pfqlen && p < 60; i++) st[p++] = g_pfq[i];
                            const char *mid = runs > 0 ? " -- " : " -- no matches on screen";
                            while (*mid) st[p++] = *mid++;
                            if (runs > 0) { num_append(st, &p, runs);
                                const char *suf = runs == 1 ? " match on screen" : " matches on screen";
                                while (*suf) st[p++] = *suf++; }
                            st[p] = 0;
                            set_status(st);
                        }
                    }
                    else if (k >= ' ' && k < 0x7f && g_pfqlen < (int)sizeof g_pfq - 1) {
                        g_pfq[g_pfqlen++] = (char)k; g_pfq[g_pfqlen] = 0;
                    }
                    need = 1;
                    continue;
                }
                /* Chrome keys belong to the chrome. While the address bar has
                 * focus the page never sees the keystroke -- otherwise a page
                 * could swallow the Enter that loads the next URL. */
                int allow = 1;
                struct node *fnode = 0;
                if (!editing) {
                    char one[5];               /* up to 4 UTF-8 bytes + NUL -- see key_name */
                    struct js_event_init ji = { 0 };
                    ji.bubbles = 1; ji.cancelable = 1;
                    ji.key = key_name(k, one);
                    ji.code = ji.key;
                    /* Legacy .keyCode: the ASCII fast path is unchanged; a
                     * navigation code keeps its old (masked) value, and a code
                     * point above ASCII carries itself rather than an
                     * arbitrary low byte -- k & 0xFF used to fold every CJK
                     * candidate above U+00FF onto some other character's
                     * keyCode entirely. */
                    ji.key_code = (k >= ' ' && k < 0x7f) ? k : (is_nav_key(k) ? (k & 0xFF) : k);
                    mods_of(&e, &ji);
                    /* THE KEYSTROKE GOES TO THE FOCUSED ELEMENT.
                     *
                     * This line used to read "No focus model yet, so keys go to
                     * <body>", and that was the entire reason no web page on
                     * this machine could be typed into. The fallback is kept
                     * and is not a compromise: with nothing focused, <body> IS
                     * the DOM's answer for activeElement, so a document-level
                     * listener sees the key bubble past exactly as before. */
                    fnode = FOCUS_ROUTING ? focus_current() : 0;
                    struct node *body = g_root ? dom_doc_body(g_root->doc) : 0;
                    struct node *tgt = fnode ? fnode : (body ? body : js_dom_root());
                    allow = js_dom_dispatch(tgt, "keydown", &ji);
                    /* A keydown handler is entitled to move focus, or to remove
                     * the focused element outright. Re-read rather than trust
                     * the pointer taken three lines ago. */
                    fnode = FOCUS_ROUTING ? focus_current() : 0;
                    if (allow && ((k >= ' ' && k < 0x7f) || (k > 0x7F && !is_nav_key(k)))) {
                        /* keypress: legacy, but a very large amount of real
                         * form code still cancels typing through it -- and a
                         * page that specifically filters keypress to reject
                         * non-Latin input (a "digits only" field, say) needs to
                         * see a CJK candidate arrive here too, or it can never
                         * refuse one. */
                        struct js_event_init jp = ji;
                        allow = js_dom_dispatch(tgt, "keypress", &jp);
                        fnode = FOCUS_ROUTING ? focus_current() : 0;
                    }
                }

                /* Tab moves focus. Before the control's own handling, because a
                 * text field must not eat the key that leaves it, and after the
                 * page's keydown, because a focus trap cancels Tab. */
                if (FOCUS_ROUTING && allow && !editing && k == '\t' && g_root) {
                    popup_close();
                    struct node *cur = focus_current();
                    if (cur && fc_kind(cur) != FC_NONE) fc_commit(cur);
                    if (focus_advance(g_root, (e.mods & EV_MOD_SHIFT) != 0)) {
                        struct node *nf = focus_current();
                        if (nf && fc_kind(nf) != FC_NONE) fc_mark_focus(nf);
                        /* Scroll it into view: a Tab that focuses something off
                         * screen is indistinguishable from a Tab that did
                         * nothing. */
                        int bx, by, bw, bh;
                        if (nf && control_box(nf, &bx, &by, &bw, &bh)) {
                            if (by < scroll + 8) scroll = by - 8;
                            else if (by + bh > scroll + VIEW_H - 8) scroll = by + bh - VIEW_H + 8;
                            if (scroll < 0) scroll = 0;
                            if (scroll > maxs) scroll = maxs;
                            sync_scroll();
                        }
                    }
                    need = 1;
                    allow = 0;
                }

                /* The open <select> owns the keyboard. */
                if (FOCUS_ROUTING && allow && !editing && popup_live()) {
                    struct node *sel = popup_live();
                    int cnt = fc_option_count(sel);
                    if (k == KEY_DOWN) { g_popup_hi++; if (g_popup_hi >= cnt) g_popup_hi = cnt - 1; allow = 0; }
                    else if (k == KEY_UP) { g_popup_hi--; if (g_popup_hi < 0) g_popup_hi = 0; allow = 0; }
                    else if (k == '\n' || k == ' ') {
                        if (g_popup_hi >= 0 && g_popup_hi != fc_selected_index(sel)) {
                            fc_set_selected_index(sel, g_popup_hi);
                            forms_dispatch(sel, "input", 1, 0);
                            forms_dispatch(sel, "change", 1, 0);
                        }
                        popup_close();
                        allow = 0;
                    } else if (k == 0x1b) { popup_close(); allow = 0; }
                    if (!allow) need = 1;
                }

                /* The focused control gets first refusal on everything else. */
                if (FOCUS_ROUTING && allow && !editing && fnode && fc_kind(fnode) != FC_NONE) {
                    if (control_key(fnode, k, &e, &navigated)) {
                        allow = 0;
                        need = 1;
                    }
                }

                /* ...and so does a focused contenteditable, which is not a form
                 * control and therefore never reached the branch above. THIS
                 * WAS THE WHOLE GAP: focus.c has always known a contenteditable
                 * can hold focus, so the keystroke arrived at a focused
                 * composer and fc_kind() answered FC_NONE and it was dropped on
                 * the floor -- silently, with nothing on the serial. */
                if (FOCUS_ROUTING && allow && !editing && fnode) {
                    struct node *ceh = fc_ce_host(fnode);
                    uint32_t ceser = ceh ? ceh->serial : 0;
                    int ce_dirty = 0;
                    if (ceh && ce_key(ceh, k, &e, &ce_dirty)) {
                        allow = 0;
                        need = 1;
                        /* An edit moved boxes: re-style the host (Enter creates
                         * elements with no computed style at all) and re-lay
                         * out before anything is painted.
                         *
                         * THE HOST MAY BE GONE. The `input` event ran the
                         * page's own handler, and a React composer's response
                         * to one is routinely to tear the subtree down and
                         * rebuild it -- so the pointer we came in with can name
                         * a recycled slot by now. The serial says which, and
                         * the fallback is the whole document, which is always
                         * correct and merely slower. */
                        if (ce_dirty) {
                            int live = ceh->serial == ceser &&
                                       doc_root_of(ceh) == g_root;
                            ce_settle(live ? ceh : 0);
                        }
                        /* A script may have reacted to the `input` event. */
                        if (settle_frame()) need = 1;
                    }
                }

                if (allow) {
                    if      (k == KEY_DOWN) scroll += 40;
                    else if (k == KEY_UP)   scroll -= 40;
                    else if (k == KEY_PGDN) scroll += VIEW_H - 40;
                    else if (k == KEY_PGUP) scroll -= VIEW_H - 40;
                    /* KEY_HOME/KEY_END/KEY_LEFT/KEY_RIGHT are the ADDRESS
                     * BAR'S caret keys while `editing` is true, and the page-
                     * scroll / session-history keys they always were
                     * otherwise. The SAME key meaning two things is safe here
                     * only because which one is live is never a guess:
                     * draw_address_bar() paints a caret AND, when there is a
                     * selection, a highlighted band ONLY while editing, and
                     * paints neither the instant editing goes false -- so a
                     * person reads the mode off the bar itself, not off
                     * memory. Before this, KEY_LEFT/KEY_RIGHT ran hist_go()
                     * UNCONDITIONALLY, even while typing: pressing Left to
                     * move a caret that did not exist silently navigated the
                     * page away from under the half-typed address. */
                    else if (editing && k == KEY_HOME) { addr_home((e.mods & EV_MOD_SHIFT) != 0); chrome_edit_only = 1; }
                    else if (editing && k == KEY_END)  { addr_end((e.mods & EV_MOD_SHIFT) != 0);  chrome_edit_only = 1; }
                    else if (!editing && k == KEY_HOME) scroll = 0;
                    else if (!editing && k == KEY_END)  scroll = maxs;
                    else if (editing && k == KEY_LEFT)  { addr_move(-1, (e.mods & EV_MOD_CTRL) != 0, (e.mods & EV_MOD_SHIFT) != 0); chrome_edit_only = 1; }
                    else if (editing && k == KEY_RIGHT) { addr_move(+1, (e.mods & EV_MOD_CTRL) != 0, (e.mods & EV_MOD_SHIFT) != 0); chrome_edit_only = 1; }
                    /* hist_go's three outcomes: 0 nothing, 1 a real navigation
                     * (load() the new url, tear the document down), 2 a
                     * same-document pushState/hash move (address bar only --
                     * the popstate it queued fires from js_page_pending()
                     * further down THIS SAME iteration; navigated must stay 0
                     * or that never runs). Reached only when !editing now --
                     * see the caret-key block just above. */
                    else if (!editing && k == KEY_LEFT)  { int hg = hist_go(-1); if (hg == 1) { load(url); navigated = 1; } else if (hg == 2) { need = 1; } }
                    else if (!editing && k == KEY_RIGHT) { int hg = hist_go(+1); if (hg == 1) { load(url); navigated = 1; } else if (hg == 2) { need = 1; } }
                    /* addr_infer_scheme() -- see its own comment -- either
                     * leaves a real URL alone, prepends https:// to
                     * something host-shaped, or refuses a bare word and sets
                     * the status line. Refusing must NOT navigate and must
                     * NOT set chrome_edit_only (the status line is outside
                     * the address-bar band redraw_chrome() repaints), so the
                     * bar stays in edit mode and the ordinary full redraw()
                     * this iteration already asks for shows why. */
                    else if (editing && k == '\n') {
                        if (addr_infer_scheme()) { editing = 0; hist_push(url); load(url); navigated = 1; }
                    }
                    /* Ctrl+A / Ctrl+C / Ctrl+X / Ctrl+V, folded to a control
                     * byte by the keyboard driver exactly as forms.c's
                     * control_key()/ce_key() already read them (0x01/0x03/
                     * 0x16/0x18) -- no need to also check EV_MOD_CTRL here,
                     * same as those two. clip_set/clip_get are the real
                     * kernel clipboard (SYS_CLIP_SET/GET); CLIP_F_TEXT
                     * validates UTF-8 on the way in and clip_get never hands
                     * back a torn character, so pasting a byte range straight
                     * into url[] cannot corrupt it. */
                    else if (editing && k == 0x01) { addr_select_all(); chrome_edit_only = 1; }              /* Ctrl+A */
                    else if (editing && (k == 0x03 || k == 0x18)) {                                          /* copy / cut */
                        char cb[600];
                        int got = addr_selection_text(cb, (int)sizeof cb);
                        if (got > 0) clip_set(CLIP_F_TEXT, cb, got);
                        if (k == 0x18 && got > 0) addr_backspace();   /* cut: also remove the selection */
                        chrome_edit_only = 1;
                    }
                    else if (editing && k == 0x16) {                                                          /* Ctrl+V */
                        char pb[600];
                        int got = clip_get(CLIP_F_TEXT, pb, (int)sizeof pb);
                        if (got > 0) addr_insert(pb, got);
                        chrome_edit_only = 1;
                    }
                    /* The PAGE's own Ctrl+A / Ctrl+C -- reached only when the
                     * address bar is not editing AND no focused form control
                     * or contenteditable claimed the key above (control_key/
                     * ce_key's own 0x01/0x03 cases return 1 and set allow=0
                     * first, exactly like their Ctrl+V does) -- so this is
                     * genuinely "nothing more specific wanted this keystroke,
                     * treat it as a page-level shortcut", the same standing
                     * every other unclaimed key in this block already has.
                     * No Ctrl+X/Ctrl+V here: the page's own text cannot be
                     * cut (it is not editable) and there is nothing on a
                     * page for a paste to go into. */
                    else if (!editing && k == 0x01) { psel_select_all(); need = 1; }                          /* Ctrl+A */
                    else if (!editing && k == 0x03) { psel_copy(); }                                          /* Ctrl+C */
                    else if (k == '\b') {
                        if (editing) {
                            /* addr_backspace(): the selection if there is
                             * one, else one UTF-8 character LEFT OF THE
                             * CARET -- which, now that the caret can be
                             * anywhere, is no longer necessarily "the last
                             * character of url[]". Before this, Backspace
                             * always deleted off the END regardless of where
                             * (nowhere) the caret was, because there was no
                             * caret: clearing a 69-byte Google search URL's
                             * tracking suffix took on the order of 60
                             * presses, one per character, all from the end. */
                            addr_backspace();
                            /* CHROME-ONLY: touches url/ulen/ucaret/usel and
                             * nothing else this iteration reached (editing
                             * was already true, so none of the scroll/
                             * hist_go/navigation branches above or below this
                             * one ran). See the comment on `chrome_edit_only`
                             * above the burst loop. */
                            chrome_edit_only = 1;
                        }
                        else {
                            int hg = hist_go(-1);   /* Backspace = back */
                            if (hg == 1) { load(url); navigated = 1; }
                            else if (hg == 2) { need = 1; }
                        }
                    }
                    /* Forward-delete (the Delete key -- 0x7f, the same code
                     * ce_key()'s `case 0x7f` reads for a contenteditable).
                     * Excluded from is_nav_key() and from the printable-ASCII
                     * range test below on purpose, so it cannot double-fire
                     * as either. */
                    else if (editing && k == 0x7f) { addr_delete_fwd(); chrome_edit_only = 1; }
                    else if (editing && k >= ' ' && k < 0x7f) {
                        /* addr_insert(): replaces the selection first, when
                         * there is one -- typing over a selected address
                         * deletes the old one and types the new one in the
                         * SAME keystroke, which is the concrete fix for "the
                         * enormous Google URL suffix cannot be deleted":
                         * Ctrl+A (or Ctrl+L), then just start typing. */
                        char c = (char)k;
                        addr_insert(&c, 1);
                        chrome_edit_only = 1;   /* see the comment above the burst loop */
                    }
                    /* A code point above ASCII -- e.g. a pinyin candidate --
                     * UTF-8 encoded and inserted whole (replacing a
                     * selection first, exactly like the ASCII branch above).
                     * load() percent-encodes any non-ASCII byte in `url`
                     * before it reaches the wire (RFC 3986); this is only
                     * about not corrupting what the user sees typed in the
                     * bar before that happens. */
                    else if (editing && k > 0x7F && !is_nav_key(k)) {
                        char enc[4]; int el = key_utf8_encode((unsigned)k, enc);
                        addr_insert(enc, el);
                        chrome_edit_only = 1;
                    }
                }
                if (scroll < 0) scroll = 0; if (scroll > maxs) scroll = maxs;
                sync_scroll();
                need = 1;
            } else if (e.type == EV_MOUSE || e.type == EV_MOUSE_R) {
                int mx = e.a, my = e.b;              /* window-local */
                if (my < TABH) {                     /* the tab strip */
                    int close = 0;
                    int hit = tab_strip_hit(mx, my, &close);
                    press_node = 0;
                    if (hit == -2) {                             /* the + button */
                        int n = tabs_new("");
                        if (n >= 0) { tab_dehydrate(); tabs_select(n);
                            url[0] = 0; ulen = 0; editing = 1; addr_sync();
                            set_status("new tab -- type a URL and press Enter");
                            session_save(); }
                    } else if (hit >= 0 && close) {
                        int cur = tabs_active();
                        tab_dehydrate();
                        tabs_select(tabs_close(cur));
                        if (!tab_hydrate()) {
                            struct tab *t = tab_cur();
                            if (t && t->url[0]) { load(url); navigated = 1; }
                            else { url[0] = 0; ulen = 0; editing = 1; addr_sync();
                                   set_status("new tab -- type a URL and press Enter"); }
                        }
                        session_save();
                    } else if (hit >= 0 && hit != tabs_active()) {
                        tab_switch_to(hit);
                        editing = 0; navigated = 1;
                    }
                    need = 1;
                }
                else if (my < VIEW_Y) { editing = 1; press_node = 0; }   /* click the bar to edit */
                else if (g_panel && my < VIEW_Y + VIEW_H) {
                    /* The panel is modal over the viewport: a click in it picks
                     * a row, and a click outside it dismisses. Routing it to the
                     * page underneath would hit-test a document the user cannot
                     * even see. */
                    int px = 40, pw = win_w - 80;
                    if (pw < 240) { px = 4; pw = win_w - 8; }
                    int py = VIEW_Y + 8;
                    if (mx < px || mx >= px + pw) g_panel = PANEL_NONE;
                    else {
                        int row = (my - (py + 36 - 2)) / PANEL_ROW;
                        int rows[HISTORY_MAX];
                        int n = panel_list(rows, HISTORY_MAX);
                        int sel = g_panel_top + row;
                        if (row >= 0 && sel < n) {
                            g_panel_sel = sel;
                            char u[TAB_URL], ti[TAB_TITLE];
                            panel_row_text(g_panel, rows[sel], u, ti);
                            if (u[0] && g_panel != PANEL_DOWNLOADS) {
                                g_panel = PANEL_NONE; editing = 0;
                                int i = 0; while (u[i] && i < (int)sizeof url - 1) { url[i] = u[i]; i++; }
                                url[i] = 0; ulen = i; addr_sync();
                                hist_push(url); load(url); navigated = 1;
                            }
                        }
                    }
                    need = 1;
                }
                else if (my >= VIEW_Y && my < VIEW_Y + VIEW_H) {
                    editing = 0;
                    /* An open <select> is modal over the viewport, exactly like
                     * the library panel above: a click in the list picks a row,
                     * a click outside it dismisses, and neither reaches the
                     * page underneath. */
                    struct node *pop = popup_live();
                    if (pop) {
                        int bx, by, bw, bh;
                        if (control_box(pop, &bx, &by, &bw, &bh)) {
                            int n2 = fc_option_count(pop);
                            int rows = n2 > POPUP_MAXROWS ? POPUP_MAXROWS : n2;
                            int px = bx, py = VIEW_Y + by - scroll + bh;
                            int pw = bw < 120 ? 120 : bw;
                            int ph2 = rows * POPUP_ROW + 8;
                            if (mx >= px && mx < px + pw && my >= py && my < py + ph2) {
                                int row = (my - py - 4) / POPUP_ROW;
                                if (row >= 0 && row < rows) {
                                    if (row != fc_selected_index(pop)) {
                                        fc_set_selected_index(pop, row);
                                        forms_dispatch(pop, "input", 1, 0);
                                        forms_dispatch(pop, "change", 1, 0);
                                    }
                                }
                            }
                        }
                        popup_close();
                        press_node = 0;
                        need = 1;
                        continue;
                    }
                    struct node *n = 0;
                    browser_hittest_node(mx, my - VIEW_Y, scroll, &n, 0, 0);
                    press_node = n;
                    press_serial = n ? n->serial : 0;
                    struct js_event_init ji = { 0 };
                    ji.bubbles = 1; ji.cancelable = 1; ji.detail = 1;
                    ji.client_x = mx; ji.client_y = my - VIEW_Y;
                    ji.button = dom_button(e.button);
                    ji.buttons = 1 << ji.button;
                    mods_of(&e, &ji);
                    int okdown = js_dom_dispatch(n, e.type == EV_MOUSE_R ? "contextmenu" : "mousedown", &ji);
                    /* FOCUS FOLLOWS THE MOUSE DOWN, not the click -- that is
                     * what makes click-and-drag inside a field select text in
                     * every real browser, and what makes preventDefault() on
                     * mousedown the documented way to stop a control taking
                     * focus (every custom dropdown on the web relies on it). */
                    if (FOCUS_ROUTING && okdown && e.type == EV_MOUSE && e.button == EV_BTN_LEFT) {
                        struct node *lbl = 0;
                        struct node *tgt = focus_target_for_click(n, &lbl);
                        focus_control(tgt);
                        if (tgt && FC_IS_TEXTUAL(fc_kind(tgt))) {
                            /* Put the caret where the pointer is. */
                            int bx, by, bw, bh;
                            if (control_box(tgt, &bx, &by, &bw, &bh)) {
                                const struct item *its = layout_items();
                                int cnt = layout_count(), font = 14, mono = 0;
                                for (int i = 0; i < cnt; i++)
                                    if (its[i].type == IT_CONTROL && its[i].node == tgt) {
                                        font = its[i].ctl_font ? its[i].ctl_font : its[i].font_px;
                                        mono = its[i].ctl_mono; break;
                                    }
                                int relx = mx - (bx + FC_BORDER + FC_PAD_X);
                                int off = fc_offset_at_px(tgt, relx, font, mono);
                                fc_set_selection(tgt, off, off);
                            }
                        }
                        /* A click inside an editing host puts the CARET there.
                         * Separate from the control case above and it has to
                         * be: focus_target_for_click walks UP to the nearest
                         * focusable element, which for a composer is the host
                         * -- but the caret belongs at the character under the
                         * pointer, which is a fact about the TEXT NODE the hit
                         * landed in, several levels down. */
                        if (fc_ce_host(tgt)) ce_caret_from_click(tgt, mx, my - VIEW_Y);
                        /* PAGE TEXT SELECTION begins here, and only here: a
                         * plain mousedown that neither of the two branches
                         * above claimed (a textual control takes its OWN
                         * drag-to-select via fc_set_selection above; a
                         * contenteditable likewise via ce_caret_from_click).
                         * Shift+click EXTENDS whatever selection already
                         * exists instead of restarting it -- the same rule
                         * every other shift-extend in this file uses
                         * (addr_move, fc_edit_move, fc_ce_move). A plain
                         * click that lands on no text at all clears any
                         * existing selection, which is "click elsewhere
                         * deselects" built out of psel_clear() rather than a
                         * separate deselect path. */
                        if (!(tgt && (FC_IS_TEXTUAL(fc_kind(tgt)) || fc_ce_host(tgt)))) {
                            struct node *pn = 0; int poff = 0;
                            int dpfc_ok = doc_pos_from_click(mx, my - VIEW_Y, &pn, &poff);
                            if (dpfc_ok) {
                                if (e.mods & EV_MOD_SHIFT) {
                                    if (!g_psel_an) psel_begin(pn, poff);
                                    psel_extend_to(pn, poff);
                                } else {
                                    psel_begin(pn, poff);
                                }
                                g_psel_dragging = 1;
                                need = 1;
                            } else if (!(e.mods & EV_MOD_SHIFT)) {
                                psel_clear();
                            }
                        }
                    }
                    need = 1;
                }
            } else if (e.type == EV_MOUSE_UP) {
                int mx = e.a, my = e.b;
                /* A drag ends wherever the button comes up, including off the
                 * viewport (over the address bar, a panel, the tab strip) --
                 * gating this on `my` the way the click-target logic below
                 * does would leave g_psel_dragging stuck at 1 until the next
                 * click, extending a selection from a mouse MOVE that never
                 * had a release. */
                g_psel_dragging = 0;
                if (my >= VIEW_Y && my < VIEW_Y + VIEW_H) {
                    struct node *n = 0;
                    char href[512]; href[0] = 0;
                    browser_hittest_node(mx, my - VIEW_Y, scroll, &n, href, sizeof href);
                    struct js_event_init ji = { 0 };
                    ji.bubbles = 1; ji.cancelable = 1; ji.detail = 1;
                    ji.client_x = mx; ji.client_y = my - VIEW_Y;
                    ji.button = dom_button(e.button);
                    mods_of(&e, &ji);
                    js_dom_dispatch(n, "mouseup", &ji);
                    /* A click needs a press and a release on the same element --
                     * dragging off a link and letting go must not navigate. The
                     * press target is re-validated by serial, because a mousedown
                     * handler is perfectly entitled to have deleted it. */
                    int same = press_node && n == press_node && press_node->serial == press_serial;
                    if (same && e.button == EV_BTN_LEFT) {
                        /* THIS is the default action. Navigation used to happen
                         * unconditionally on mousedown; now it is what happens
                         * when the click event survives the page's handlers. */
                        int go = js_dom_dispatch(n, "click", &ji);
                        if (settle_frame()) need = 1;
                        /* dblclick: two clicks on the SAME target, close in time
                         * and space. 400ms + 6px slop -- the same numbers
                         * titlebar_double_click() in wm.c uses for the window
                         * manager's own double-click. Not shared code (a DOM
                         * event and a titlebar hit-test answer different
                         * questions and have no common header to hold a
                         * constant), but a human's second click has to land
                         * inside the same platform timing either way, so
                         * picking a different number here would just be a
                         * second, competing answer to "how fast is a double
                         * click on this machine" -- the one jar, two doors
                         * trap, avoided by copying the value instead of a
                         * pointer to it. A third click does not chain into a
                         * second dblclick: state is cleared on every hit,
                         * exactly like the titlebar's. */
                        unsigned long long nowms = monotonic_ms();
                        int dbl = lastclick_node && n == lastclick_node &&
                                  n->serial == lastclick_serial &&
                                  nowms - lastclick_ms <= 400 &&
                                  mx - lastclick_x <= 6 && lastclick_x - mx <= 6 &&
                                  my - lastclick_y <= 6 && lastclick_y - my <= 6;
                        if (dbl) {
                            struct js_event_init ji2 = ji;
                            ji2.detail = 2;
                            js_dom_dispatch(n, "dblclick", &ji2);
                            lastclick_node = 0; lastclick_serial = 0;
                            /* Double-click selects the WORD under the second
                             * click -- the page-text counterpart of the DOM
                             * dblclick event just dispatched above, reusing
                             * its own same-target/close-in-time/close-in-
                             * space determination rather than a second timer.
                             * A double-click inside a form field or a
                             * contenteditable does not reach here at all:
                             * FOCUS_ROUTING's mousedown handling above only
                             * begins a psel_* selection when neither claimed
                             * the click, so `n` having landed there matches
                             * the same page-text-only scope. */
                            struct node *pn = 0; int poff = 0;
                            if (doc_pos_from_click(mx, my - VIEW_Y, &pn, &poff)) {
                                int wa, wb; psel_word_at(pn, poff, &wa, &wb);
                                if (wb > wa) { psel_begin(pn, wa); psel_extend_to(pn, wb); need = 1; }
                            }
                        } else {
                            lastclick_node = n; lastclick_serial = n->serial;
                            lastclick_ms = nowms; lastclick_x = mx; lastclick_y = my;
                        }
                        /* THE CONTROL'S DEFAULT ACTION. A checkbox toggles, a
                         * submit button submits, a <select> opens -- and every
                         * one of them is suppressed by preventDefault(), which
                         * is what `go` carries. A click on a <label> activates
                         * the control it labels, which is how most checkboxes
                         * on the web are actually ticked. */
                        if (FOCUS_ROUTING && go) {
                            struct node *lbl = 0;
                            struct node *tgt = focus_target_for_click(n, &lbl);
                            if (tgt && fc_kind(tgt) != FC_NONE) {
                                if (control_activate(tgt, &navigated)) need = 1;
                            }
                        }
                        if (go && !navigated && href[0]) { follow_link(href); navigated = 1; }
                    }
                    press_node = 0;
                    need = 1;
                }
            } else if (e.type == EV_MOUSE_MOVE) {
                /* Motion is the one event that arrives continuously, so it is
                 * the one worth not paying for: with no listeners registered
                 * anywhere, building an Event per sample is pure waste. Inline
                 * on-attributes are compiled lazily and so are invisible to this
                 * count -- onmousemove= in markup is the accepted casualty. The
                 * same guard covers the hover transitions below: a page with
                 * zero listeners of ANY kind cannot observe a mouseover
                 * either, and the hit test they'd need is exactly as
                 * expensive as the one mousemove was already paying for. */
                if (js_dom_listener_count() > 0) {
                    int in_view = e.b >= VIEW_Y && e.b < VIEW_Y + VIEW_H;
                    struct node *n = 0;
                    if (in_view) browser_hittest_node(e.a, e.b - VIEW_Y, scroll, &n, 0, 0);
                    struct js_event_init ji = { 0 };
                    ji.bubbles = 1;
                    ji.client_x = e.a; ji.client_y = e.b - VIEW_Y;
                    mods_of(&e, &ji);
                    if (in_view) js_dom_dispatch(n, "mousemove", &ji);
                    /* over/out/enter/leave, synthesised from this hit test
                     * against the LAST one. Deliberately not gated on
                     * `in_view`: leaving the viewport for the URL bar or a
                     * panel is the "pointer is now over nothing" case, and
                     * whatever it was over a moment ago must still hear
                     * mouseout/mouseleave, or a hover menu opened over the
                     * page stays open forever once the pointer leaves
                     * through the chrome instead of back over the page. */
                    struct node *from = (hover_node && hover_node->serial == hover_serial)
                                         ? hover_node : 0;
                    if (from != n) {
                        fire_hover_transition(from, n, &ji);
                        hover_node = n;
                        hover_serial = n ? n->serial : 0;
                    }
                }
                /* Extending a page-text selection is chrome behaviour, not a
                 * DOM event -- unlike mousemove/hover just above, it must run
                 * whether or not the page registered any listener at all, so
                 * it sits OUTSIDE that gate rather than as another case
                 * inside it. Repainting only when the FOCUS end actually
                 * moved to a different (node, offset) matters here more than
                 * anywhere else in this file: this is coalesced motion
                 * (logit_abi.h), so a slow drag can still deliver several
                 * samples that land in the same word, and the compositor is
                 * already the bottleneck this machine is slowest at -- see
                 * CLAUDE.md's own measurement of where a frame's time goes.
                 * Re-painting on a sample that changed nothing on screen
                 * would be exactly the mistake it warns against. */
                if (g_psel_dragging && e.b >= VIEW_Y && e.b < VIEW_Y + VIEW_H) {
                    struct node *pn = 0; int poff = 0;
                    if (doc_pos_from_click(e.a, e.b - VIEW_Y, &pn, &poff)) {
                        struct node *ofo = g_psel_fo; int ofoo = g_psel_foo; int oact = g_psel_active;
                        psel_extend_to(pn, poff);
                        if (g_psel_fo != ofo || g_psel_foo != ofoo || g_psel_active != oact) need = 1;
                    }
                }
            } else if (e.type == EV_WHEEL) {
                int maxs = ph - VIEW_H; if (maxs < 0) maxs = 0;
                int allow = 1;
                if (e.b >= VIEW_Y && e.b < VIEW_Y + VIEW_H) {
                    struct js_event_init ji = { 0 };
                    ji.bubbles = 1; ji.cancelable = 1;
                    ji.client_x = e.a; ji.client_y = e.b - VIEW_Y;
                    ji.detail = e.wheel;
                    ji.delta_y = (double)e.wheel * 40.0;
                    mods_of(&e, &ji);
                    struct node *n = 0;
                    browser_hittest_node(e.a, e.b - VIEW_Y, scroll, &n, 0, 0);
                    allow = js_dom_dispatch(n, "wheel", &ji);
                }
                if (allow) scroll += e.wheel * 40;
                if (scroll < 0) scroll = 0; if (scroll > maxs) scroll = maxs;
                sync_scroll();
                need = 1;
            }
            if (!navigated && settle_frame()) need = 1;   /* a handler rewrote the DOM */
        }

        /* Due timers + animation frames. `js_page_pending()` is a pointer test,
         * so an idle page does not even read the clock -- the loop is exactly as
         * hot as it was before timers existed. */
        if (!navigated && js_page_pending()) {
            if (js_page_run_due() > 0) {
                /* A timer/rAF callback can inject a <script> too -- drain the
                 * queue on the frame loop, never on the callback's own stack. */
                if (g_pending_n > 0) run_pending_inserted_scripts(url);
                if (settle_frame()) need = 1;
                if (js_page_output_len() != js_out_shown) { status_from_js("loaded"); need = 1; }
                /* The CSS animation tick ran inside run_due and overlayed
                 * values on cstyle; the frame it is owed depends on WHAT
                 * moved. opacity is snapshotted into the display list at
                 * layout, so an opacity frame costs one layout_page; a
                 * transform-only frame is read live by the painter and
                 * costs only the repaint. css_anim_needs_layout() answers
                 * 2 / 1 / 0 and clears itself, so a pass where nothing
                 * animated buys neither. */
                if (LOGIT_HAVE(css_anim_needs_layout)) {
                    int fk = css_anim_needs_layout();
                    if (fk == 2 && g_root) {
                        layout_page(g_root, win_w);
                        ph = layout_height();
                    }
                    if (fk) need = 1;
                }
            }
        }

        /* A navigation the LIVE page asked for -- a click handler setting
         * location.href, a timer calling location.replace, a router. Taken
         * here, at the top of the loop, because this is the first point after
         * the callback returned at which tearing the document down is safe.
         *
         * This one PUSHES history: a page that moves seconds or minutes after
         * it loaded is acting on the user, and Back must come back here. The
         * redirect chain inside load() replaces instead -- see hist_replace. */
        if (!navigated) {
            char want[600];
            if (take_script_nav(want, sizeof want)) {
                editing = 0;
                int i = 0;
                while (want[i] && i < (int)sizeof url - 1) { url[i] = want[i]; i++; }
                url[i] = 0; ulen = i; addr_sync();
                hist_push(url);
                load(url);
                navigated = 1; need = 1;
            }
        }

        /* CHROME-ONLY DISPATCH. nev == 1 && chrome_edit_only means the single
         * event this burst processed was proven (by the two branches above)
         * to have touched nothing but the address bar's text and caret; the
         * take_script_nav() check just above this line is the only other
         * thing that can set `need` or `navigated` between the burst and
         * here, so `!navigated` covers it too. Anything else -- a multi-event
         * burst, a scroll, a click, a resize, a DOM mutation, a navigation --
         * takes the full redraw() it always has, unchanged. This is a
         * PERFORMANCE choice, never a correctness one: when the classification
         * is not airtight, the fallback is the whole canvas, exactly as
         * before this change existed. */
        if (need) {
            if (nev == 1 && chrome_edit_only && !navigated) redraw_chrome(editing);
            else redraw(editing);
        }

        /* ---- THE SLEEP, and it is the entire cost of an idle browser -------
         *
         * This was `sys_yield()`, i.e. a spin: two syscalls per turn, each one
         * taking the BKL to be told nothing had happened. kprof over a browser
         * launch put `app_main` at 22.1% of ALL samples and 98% of user-mode
         * ones -- and under TCG a spinning ring-3 thread is not merely wasting
         * guest cycles, it is taking host CPU away from the compositor thread,
         * which is the thing the user is actually waiting for.
         *
         * THE SHAPE IS "COMPUTE THE DEADLINE TO THE NEXT THING THAT IS DUE",
         * never a fixed small timeout -- a 10 ms poll is a spin with extra
         * steps, and it would put the rAF cadence on the timeout's clock
         * instead of the page's. Three wake sources, in the order they matter:
         *
         *   an event      wait_idle() parks on THIS window's queue and the
         *                 compositor wakes it on enqueue. No lost wakeup: the
         *                 kernel evaluates `evq_empty` while still holding the
         *                 BKL that the only writer needs to push (wm.c:2122),
         *                 so nothing can arrive between the test and the park.
         *                 ev == NULL means the wait does not CONSUME, so the
         *                 poll_event() drain at the top of the loop is
         *                 unchanged and no event can be eaten here.
         *   a JS timer    js_page_next_due() is the earliest setTimeout /
         *                 setInterval / rAF deadline in monotonic ms. Sleeping
         *                 exactly to it is what keeps an animating page on its
         *                 own 16 ms boundary rather than on ours.
         *   the pump      a fetch in flight or an EventSource retry is stepped
         *                 by js_page_run_due() and has no deadline visible
         *                 here, so it -- and ONLY it -- caps the sleep.
         *
         * `dt <= 0` does NOT sleep. A callback that schedules for "now" is
         * deferred to the next pass by js_page_run_due()'s seq snapshot, so a
         * setTimeout(f, 0) chain has to be able to turn the loop at full speed
         * or chunked work would drop from thousands of steps a second to 100.
         * That case is the loop doing work, not polling for it.
         *
         * js_webapi_pending is WEAK here (JS_WEBAPI_OPTIONAL above):
         * browser-nofetch.aex links without js_webapi.o. */
        {
            int wait_ms = 0;                     /* 0 = park until an event */
            if (js_page_pending()) {
                long long due = js_page_next_due();       /* -1: no timer armed */
                long long now = (long long)monotonic_ms();
                long long dt  = (due >= 0) ? due - now : (long long)BROWSER_PUMP_MS;
                if (LOGIT_HAVE(js_webapi_pending) && js_webapi_pending() && dt > BROWSER_PUMP_MS)
                    dt = BROWSER_PUMP_MS;
                if (dt <= 0) continue;           /* already due: run it, do not sleep */
                if (dt > BROWSER_WAIT_MAX_MS) dt = BROWSER_WAIT_MAX_MS;
                wait_ms = (int)dt;
            }
            wait_idle(wait_ms);
        }
    }
}
