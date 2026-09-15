/* Drive shipping app_main using only bounded host events and its real drawing
 * recorder. The HTML fixture has no hover/active/focus listeners: CSS changes
 * must reach the painter even when the listener-count optimization is idle.
 * The two inline click actions are ordinary UI, also usable in the guest.
 * No assertion writes native interaction state or requests a synthetic cascade. */
#define main loader_existing_main
#include "loader_test.c"
#undef main
#include "js_dom.h"
void app_main(void);

static int stage, steps, cooldown, phases;
static int press_x, press_y, blocked_x, blocked_y, program_x, program_y;
static const struct paintop *last_text(const char *s)
{
    int n = (int)strlen(s);
    for (int i = paint_nops - 1; i >= 0; i--)
        if (paint_ops[i].kind == OP_TEXT && paint_ops[i].len == n &&
            !memcmp(paint_ops[i].text, s, n)) return &paint_ops[i];
    return 0;
}
static int painted_color(const char *s, unsigned color)
{
    const struct paintop *p = last_text(s);
    return p && (p->color & 0xffffff) == color;
}
static int active_is(const char *id)
{
    JSContext *ctx = js_page_ctx();
    if (!ctx) return 0;
    char src[160];
    snprintf(src, sizeof src, "document.activeElement===document.getElementById('%s')", id);
    JSValue v = JS_Eval(ctx, src, strlen(src), "<interaction-observer>", JS_EVAL_TYPE_GLOBAL);
    int ok = !JS_IsException(v) && JS_ToBool(ctx, v) == 1;
    if (JS_IsException(v)) JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, v);
    return ok;
}
static void post(int type, int x, int y, int button)
{
    struct logit_event e = {0}; e.type = type; e.a = x; e.b = y; e.button = button;
    host_post_event(&e);
    /* A no-event poll ends browser.c's event burst, so the real frame loop
     * gets to consume dirty styles and paint BEFORE the next observation. */
    cooldown = 3;
}
static void fresh_post(int type, int x, int y, int button)
{
    /* Recorder text pointers belong to layout items; a relayout may release
     * them. Drop the previous frame before every event, then inspect only the
     * next actual painted frame. An absent repaint remains an assertion fail. */
    paint_nops = 0;
    post(type, x, y, button);
}
static int point(const char *label, int *x, int *y)
{
    const struct paintop *p = last_text(label);
    if (!p) return 0;
    *x = p->x + 5; *y = p->y + 5; return 1;
}
void loader_poll_hook(void)
{
    host_clock += 5;
    if (++steps > 400) {
        if (stage != 99) {
            printf("interaction-runtime budget: stage=%d phases=%d ops=%d\n", stage, phases, paint_nops);
            CHECK(0, "bounded interaction loop reached step budget");
            stage = 99; post(EV_CLOSE, 0, 0, 0);
        }
        return;
    }
    if (stage == 99 || !js_page_live()) return;
    if (cooldown) { cooldown--; return; }
    switch (stage) {
    case 0: {
        int x, y;
        if (!point("HOVER-TO-OPEN", &x, &y)) return;
        /* The old zero-listener assertion counted browser-installed focus
         * listeners as page code (and this fixture also has click actions).
         * Check the actual CSS-only hover element instead of a global count. */
        JSContext *ctx = js_page_ctx();
        const char *probe = "(function(){var n=document.getElementById('menu-parent');return n.onmouseover==null&&n.onmouseenter==null&&n.onmousemove==null})()";
        JSValue pure = JS_Eval(ctx, probe, strlen(probe), "<hover-observer>", JS_EVAL_TYPE_GLOBAL);
        CHECK(!JS_IsException(pure) && JS_ToBool(ctx, pure) == 1, "hover element has no page hover handlers");
        JS_FreeValue(ctx, pure);
        CHECK(!last_text("CSS-MENU-OPEN"), "CSS menu starts hidden");
        CHECK(point("PRESS-AND-RELEASE", &press_x, &press_y) &&
              point("INERT-CANNOT-ACTIVATE", &blocked_x, &blocked_y) &&
              point("CLICK-TO-FOCUS-TARGET", &program_x, &program_y), "real painter supplies event coordinates");
        stage++; fresh_post(EV_MOUSE_MOVE, x, y, 0); break;
    }
    case 1:
        CHECK(last_text("CSS-MENU-OPEN") != 0, "native hover opens menu without JS listeners");
        phases++; stage++; fresh_post(EV_MOUSE_MOVE, 1100, 500, 0); break;
    case 2:
        CHECK(last_text("CSS-MENU-OPEN") == 0, "native pointer leave hides CSS menu");
        stage++; fresh_post(EV_MOUSE, press_x, press_y, EV_BTN_LEFT); break;
    case 3:
        CHECK(painted_color("PRESS-AND-RELEASE", 0xcc2200), "native mouse down paints active color");
        phases++; stage++; fresh_post(EV_MOUSE_UP, press_x, press_y, EV_BTN_LEFT); break;
    case 4:
        CHECK(painted_color("PRESS-AND-RELEASE", 0x222222), "native mouse release restores normal color");
        CHECK(active_is("press"), "mouse default action focuses press target");
        stage++; fresh_post(EV_KEY, '\t', 0, 0); break;
    case 5:
        CHECK(active_is("first"), "Tab moves to first ordinary focus target");
        CHECK(painted_color("FIRST-TAB-STOP", 0x8800cc), "native Tab paints focus selector");
        phases++; stage++; fresh_post(EV_KEY, '\t', 0, 0); break;
    case 6:
        CHECK(active_is("after"), "Tab skips the inert subtree");
        CHECK(painted_color("AFTER-INERT-TAB-STOP", 0x8800cc), "focus style follows target after inert subtree");
        phases++; stage++; fresh_post(EV_MOUSE, blocked_x, blocked_y, EV_BTN_LEFT); break;
    case 7:
        stage++; fresh_post(EV_MOUSE_UP, blocked_x, blocked_y, EV_BTN_LEFT); break;
    case 8:
        CHECK(last_text("NO-INERT-CLICK") != 0 && !last_text("BLOCKED-HANDLER-RAN"),
              "native inert click never invokes its handler");
        CHECK(!active_is("blocked"), "native inert click cannot focus descendant");
        phases++; stage++; fresh_post(EV_MOUSE, program_x, program_y, EV_BTN_LEFT); break;
    case 9:
        stage++; fresh_post(EV_MOUSE_UP, program_x, program_y, EV_BTN_LEFT); break;
    case 10:
        CHECK(active_is("last"), "ordinary click action calls element.focus");
        CHECK(painted_color("PROGRAMMATIC-FOCUS-TARGET", 0x8800cc), "element.focus reaches actual CSS focus paint");
        phases++; stage = 99; post(EV_CLOSE, 0, 0, 0); break;
    }
}
int main(void)
{
    int len = 0;
    char *page = slurp("tests/fixtures/engine-interaction/index.html", &len);
    /* fake_site_add borrows bytes until the loader consumes the response.
     * Freeing here used to serve reclaimed allocator metadata as HTML, so the
     * control timed out before any interaction was exercised. */
    fake_site_reset(); fake_site_add("http://fixture.test/interaction.html", page);
    tabs_set_store(&memfs);
    const char *session = "logit-browser-session\t1\t0\n0\thttp://fixture.test/interaction.html\tinteraction\t0\n";
    memfs_write(SESSION_PATH, session, (int)strlen(session));
    post(EV_KEY, '\n', 0, 0);
    if (setjmp(host_exit_jmp) == 0) app_main();
    free(page);
    CHECK(host_exited && host_exit_code == 0, "real interaction loop exits normally");
    CHECK(phases == 6, "all six native interaction phases were observed");
    printf("interaction-runtime: %s (%d polls)\n", fail ? "FAIL" : "PASS", steps);
    return fail;
}
