/* Browser input while the synchronous document loader owns the event thread.
 *
 * This links the real browser.c, DOM, CSS/layout, QuickJS and page lifecycle.
 * The established in-memory transport is the only fake. Deterministic edges
 * pin the close/cancellation and first-paint input contracts:
 *
 *   stage    EV_CLOSE arrives just after the top-level response. A document
 *            with no subresources gave old browser.c no later load_tick().
 *   script   EV_CLOSE arrives at a QuickJS interrupt poll inside while(1).
 *   resource EV_CLOSE arrives after first paint with a stylesheet still live.
 *   wheel    EV_WHEEL arrives at that same visible/loading boundary and must
 *            move the real paint while the stylesheet remains pending.
 *   wheel-script  The same visual proof while the initial external script is
 *            pending after runtime setup but before any page source executes.
 *   wheel-cancel  An inline handler retains the ordinary cancelable dispatch
 *            path, so preventDefault still suppresses native scrolling.
 *   module   EV_CLOSE lands inside a module dependency's native fetch tick;
 *            transport stops there, runtime teardown waits for JS to unwind.
 *
 * app_exit longjmps in loaderhost/logit.h. This proves close never returns to
 * loading code, without introducing a racing helper thread. */
#define main loader_fixture_main
#define res_fetch loader_fixture_res_fetch
#include "loader_test.c"
#undef res_fetch
#undef main

/* Reuse every byte/ownership rule in the established fixture transport. Only
 * wait/state/close are wrapped to place events at exact lifecycle edges and to
 * model a request which remains live until cancellation. */
#define bfetch_wait fixture_bfetch_wait
#define bfetch_state fixture_bfetch_state
#define bfetch_close_all fixture_bfetch_close_all
#define img_register fixture_img_register
#define img_register_anim fixture_img_register_anim
#include "loader_fakebfetch.c"
#undef bfetch_wait
#undef bfetch_state
#undef bfetch_close_all
#undef img_register
#undef img_register_anim

/* loader_test.c has already pulled in img.h, so these link-only codec stubs
 * use the real callback types. The fixture's historical void-pointer stubs are
 * renamed above to keep its source reusable without conflicting prototypes. */
__attribute__((weak)) void img_register(img_detect_fn detect, img_decode_fn decode)
{ (void)detect; (void)decode; }
__attribute__((weak)) void img_register_anim(img_detect_fn detect, img_decode_fn decode,
                                              img_anim_fn anim)
{ (void)detect; (void)decode; (void)anim; }

void app_main(void);
enum { CLOSE_STAGE = 1, CLOSE_SCRIPT = 2, CLOSE_RESOURCE = 3,
       LOAD_WHEEL = 4, CLOSE_MODULE = 5, LOAD_WHEEL_CANCEL = 6,
       LOAD_WHEEL_SCRIPT = 7 };
static int close_mode, close_posted, close_clock_samples, poll_steps;
static int slow_ready, slow_cancelled, first_paint_seen;
static int wheel_posted, wheel_stage, wheel_scroll_y, wheel_painted;
static int wheel_initial_y, wheel_painted_while_pending;
static int transport_cancel_in_entry, failed;

#undef CHECK
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); failed = 1; } \
                           else printf("ok: %s\n", msg); } while (0)

static void post_close(void)
{
    struct logit_event e = {0}; e.type = EV_CLOSE;
    host_post_event(&e); close_posted++;
}

static void post_wheel(void)
{
    struct logit_event e = {0};
    e.type = EV_WHEEL; e.a = 100; e.b = 240; e.wheel = 6;
    host_post_event(&e); wheel_posted++;
}

static const struct paintop *last_text(const char *s)
{
    int n = (int)strlen(s);
    for (int i = paint_nops - 1; i >= 0; i--) {
        const struct paintop *o = &paint_ops[i];
        if (o->kind != OP_TEXT || !o->text) continue;
        for (int k = 0; k + n <= o->len; k++)
            if (!memcmp(o->text + k, s, (size_t)n)) return o;
    }
    return 0;
}

static int slow_request(int id)
{
    const char *u = bfetch_url(id);
    return u && (strstr(u, "/slow.css") || strstr(u, "/slow.js") ||
                 strstr(u, "/dep.js"));
}

int bfetch_state(int id)
{
    if ((close_mode == CLOSE_RESOURCE || close_mode == LOAD_WHEEL ||
         close_mode == LOAD_WHEEL_CANCEL ||
         close_mode == LOAD_WHEEL_SCRIPT ||
         close_mode == CLOSE_MODULE) &&
        slow_request(id) && !slow_ready) return BF_PENDING;
    return fixture_bfetch_state(id);
}

void bfetch_wait(int id, void (*tick)(void))
{
    /* The response is complete first. This models a click on the titlebar just
     * as network work hands the document to parsing. */
    fixture_bfetch_wait(id, close_mode == CLOSE_STAGE ? 0 : tick);
    if (close_mode == CLOSE_STAGE && !close_posted) post_close();
}

/* loader_test normally rejects layout image loads through a one-line
 * res_fetch stub. A module dependency needs the established fake transport's
 * real ownership path so its blocking tick reaches browser.c. */
int res_fetch(const char *src, unsigned char **out, int *outlen)
{
    int id = bfetch_start(src);
    if (id < 0) return -1;
    bfetch_wait(id, g_tick);
    if (bfetch_state(id) != BF_DONE || bfetch_status(id) / 100 != 2) {
        bfetch_release(id);
        return -1;
    }
    int n = bfetch_take(id, out);
    if (n < 0) return -1;
    *outlen = n;
    return 0;
}

void bfetch_close_all(void)
{
    if (js_page_entry_active()) transport_cancel_in_entry++;
    fixture_bfetch_close_all();
    /* The generic fixture has no asynchronous owner, so its close is a no-op.
     * This wrapper gives the deliberately pending request real cancellation
     * semantics and makes retained bodies observable to the test. */
    for (int i = 0; i < NREQ; i++) if (g_req[i].used) {
        if (slow_request(i)) slow_cancelled++;
        bfetch_release(i);
    }
}

void loader_poll_hook(void)
{
    host_clock += 5;
    poll_steps++;
    if (poll_steps > 400 &&
        (close_mode == CLOSE_RESOURCE || close_mode == LOAD_WHEEL ||
         close_mode == LOAD_WHEEL_CANCEL ||
         close_mode == LOAD_WHEEL_SCRIPT ||
         close_mode == CLOSE_MODULE) &&
        !close_posted) {
        const struct paintop *m = last_text("LOAD-INPUT-MARKER");
        printf("FAIL: bounded loading input test reached its poll budget "
               "(stage=%d initialY=%d lastY=%d ready=%d)\n",
               wheel_stage, wheel_initial_y, m ? m->y : -999, slow_ready);
        failed = 1; post_close(); return;
    }
    if (close_mode == CLOSE_RESOURCE) {
        const struct paintop *m = last_text("LOAD-INPUT-MARKER");
        if (!close_posted && fake_site_fetched("/slow.css") == 1 && m) {
            first_paint_seen = 1;
            post_close();
        }
        return;
    }
    if (close_mode == CLOSE_MODULE) {
        const struct paintop *m = last_text("MODULE-CLOSE-MARKER");
        if (!close_posted && fake_site_fetched("/dep.js") == 1 && m) {
            first_paint_seen = 1;
            post_close();
        }
        return;
    }
    if ((close_mode != LOAD_WHEEL && close_mode != LOAD_WHEEL_CANCEL &&
         close_mode != LOAD_WHEEL_SCRIPT) || close_posted) return;
    if (wheel_stage == 0) {
        const struct paintop *m = last_text("LOAD-INPUT-MARKER");
        const char *held = close_mode == LOAD_WHEEL_SCRIPT ? "/slow.js" : "/slow.css";
        if (fake_site_fetched(held) != 1 || !m) return;
        first_paint_seen = 1; wheel_initial_y = m->y;
        post_wheel(); wheel_stage = 1; return;
    }
    if (wheel_stage == 1) {
        const struct paintop *m = last_text("LOAD-INPUT-MARKER");
        if (close_mode == LOAD_WHEEL_CANCEL) {
            /* onwheel makes the fast path decline. Finish the transport only
             * after the event pump has had the chance to defer the gesture. */
            slow_ready = 1; wheel_stage = 2; return;
        }
        /* This hook runs from poll_event while bfetch_state still reports
         * BF_PENDING. The candidate consumes the wheel and flushes a frame
         * before polling again; the old deferred-only path stays here until
         * the bounded failure rail posts close. */
        if (!m || m->y >= wheel_initial_y) return;
        wheel_scroll_y = wheel_initial_y - m->y;
        wheel_painted = wheel_painted_while_pending = 1;
        slow_ready = 1; wheel_stage = 2; return;
    }
    if (wheel_stage == 2) {
        const struct paintop *m = last_text("LOAD-INPUT-MARKER");
        if (close_mode == LOAD_WHEEL_CANCEL) {
            if (!strstr(js_page_output(), "WHEEL-CANCELLED")) return;
            wheel_scroll_y = m ? wheel_initial_y - m->y : -999;
            wheel_stage = 4; post_close(); return;
        }
        if (!m || m->y >= wheel_initial_y) return;
        /* The sheet changes color only. This coordinate delta can therefore
         * come only from the real viewport scroll and its subsequent paint. */
        wheel_scroll_y = wheel_initial_y - m->y;
        wheel_painted = 1; wheel_stage = 4; post_close();
    }
}

void loader_clock_hook(void)
{
    host_clock++;
    if (close_mode != CLOSE_SCRIPT || close_posted || !js_page_live()) return;
    close_clock_samples++;
    /* slice_interrupt reads the clock before incrementing fuel. A value of one
     * means one complete 10,000-branch interval has run. */
    if (js_page_slice_fuel_used() >= 1) post_close();
}

static unsigned long long test_clock(void) { return monotonic_ms(); }

static void site_setup(void)
{
    fake_site_reset();
    if (close_mode == CLOSE_STAGE)
        fake_site_add("http://fixture.test/close.html",
            "<!doctype html><title>close-stage</title><p>must not finish paint</p>");
    else if (close_mode == CLOSE_SCRIPT)
        fake_site_add("http://fixture.test/close.html",
            "<!doctype html><script>while(1);</script><p>must not run after close</p>");
    else if (close_mode == CLOSE_MODULE) {
        fake_site_add("http://fixture.test/close.html",
            "<!doctype html><body><p>MODULE-CLOSE-MARKER</p>"
            "<script type='module' src='/entry.js'></script></body>");
        fake_site_add("http://fixture.test/entry.js",
            "import './dep.js'; window.moduleReachedEnd=1;");
        fake_site_add("http://fixture.test/dep.js", "export const value=1;");
    } else if (close_mode == LOAD_WHEEL_CANCEL) {
        fake_site_add("http://fixture.test/close.html",
            "<!doctype html><link rel='stylesheet' href='/slow.css'>"
            "<body onwheel=\"console.log('WHEEL-CANCELLED');event.preventDefault()\" "
            "style='margin:0;height:2600px'>"
            "<div style='padding-top:320px'>LOAD-INPUT-MARKER</div></body>");
        fake_site_add("http://fixture.test/slow.css", "body{color:#223344}");
    } else if (close_mode == LOAD_WHEEL_SCRIPT) {
        fake_site_add("http://fixture.test/close.html",
            "<!doctype html><body style='margin:0;height:2600px'>"
            "<div style='padding-top:320px'>LOAD-INPUT-MARKER</div>"
            "<script src='/slow.js'></script></body>");
        fake_site_add("http://fixture.test/slow.js",
            "console.log('SLOW-SCRIPT-RAN')");
    } else {
        fake_site_add("http://fixture.test/close.html",
            "<!doctype html><link rel='stylesheet' href='/slow.css'>"
            "<body style='margin:0;height:2600px'>"
            "<div style='padding-top:320px'>LOAD-INPUT-MARKER</div></body>");
        fake_site_add("http://fixture.test/slow.css", "body{color:#223344}");
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    close_mode = !strcmp(argv[1], "stage") ? CLOSE_STAGE :
                 !strcmp(argv[1], "script") ? CLOSE_SCRIPT :
                 !strcmp(argv[1], "resource") ? CLOSE_RESOURCE :
                 !strcmp(argv[1], "wheel") ? LOAD_WHEEL :
                 !strcmp(argv[1], "module") ? CLOSE_MODULE :
                 !strcmp(argv[1], "wheel-cancel") ? LOAD_WHEEL_CANCEL :
                 !strcmp(argv[1], "wheel-script") ? LOAD_WHEEL_SCRIPT : 0;
    if (!close_mode) return 2;
    site_setup();

    int jumped;
    if (close_mode == LOAD_WHEEL || close_mode == LOAD_WHEEL_CANCEL ||
        close_mode == LOAD_WHEEL_SCRIPT) {
        tabs_set_store(&memfs);
        const char *session = "logit-browser-session\t1\t0\n"
            "0\thttp://fixture.test/close.html\tfixture\t0\n";
        memfs_write(SESSION_PATH, session, (int)strlen(session));
        struct logit_event enter = {0}; enter.type = EV_KEY; enter.a = '\n';
        host_post_event(&enter);
        jumped = setjmp(host_exit_jmp);
        if (!jumped) app_main();
    } else {
        css_init(); css_viewport(1180, 620); css_set_post_pass(css_extra_apply);
        js_page_set_clock(test_clock);
        /* Keep the old-path control finite. Production retains the normal
         * 45-second wall rail and two-million-poll fuel backstop. */
        js_page_set_slice_fuel(8);
        jumped = setjmp(host_exit_jmp);
        if (!jumped) browser_load("http://fixture.test/close.html");
    }

    CHECK(close_posted == 1, "one close request terminates the loading scenario");
    CHECK(host_exited && host_exit_code == 0,
          close_mode == CLOSE_STAGE ? "close queued after document fetch stops CPU-side loading" :
          close_mode == CLOSE_SCRIPT ? "close during active page script exits the browser" :
          close_mode == CLOSE_RESOURCE ? "close after first paint cancels a live subresource" :
          close_mode == LOAD_WHEEL ? "browser exits after live loading-time wheel input" :
          close_mode == LOAD_WHEEL_CANCEL ? "browser exits after deferred cancelable wheel input" :
          close_mode == LOAD_WHEEL_SCRIPT ? "browser exits after live wheel during script fetch" :
          "module-fetch close exits only after the QuickJS entry unwinds");
    if (close_mode == CLOSE_SCRIPT)
        CHECK(js_page_slice_hits() == 0,
              "close interrupts the active page script before the watchdog");
    if (close_mode == CLOSE_RESOURCE) {
        CHECK(first_paint_seen, "subresource close is injected only after real first paint");
        CHECK(slow_cancelled == 1, "close releases the one live subresource request");
    }
    if (close_mode == LOAD_WHEEL || close_mode == LOAD_WHEEL_SCRIPT) {
        CHECK(first_paint_seen && wheel_posted == 1,
              "wheel is queued while first paint is visible and a resource is live");
        CHECK(wheel_scroll_y == 240,
              "wheel queued during loading reaches the page scroll path");
        CHECK(wheel_painted_while_pending && !slow_cancelled,
              close_mode == LOAD_WHEEL ?
              "wheel paints while the stylesheet response is still pending" :
              "wheel paints while the script response is still pending");
        CHECK(wheel_painted && wheel_stage == 4,
              "loading-time wheel changes the subsequent real page paint");
    }
    if (close_mode == LOAD_WHEEL_CANCEL) {
        CHECK(first_paint_seen && wheel_posted == 1,
              "cancelable wheel is injected after first paint while CSS is pending");
        CHECK(strstr(js_page_output(), "WHEEL-CANCELLED") != 0,
              "deferred loading-time wheel reaches its inline handler");
        CHECK(wheel_scroll_y == 0 && !wheel_painted_while_pending,
              "preventDefault suppresses both live and replayed native scrolling");
        CHECK(wheel_stage == 4,
              "cancelable wheel completes through the ordinary DOM event path");
    }
    if (close_mode == CLOSE_MODULE) {
        CHECK(first_paint_seen, "module dependency close begins after a real page paint");
        CHECK(slow_cancelled == 1 && transport_cancel_in_entry == 1,
              "module close cancels transport while deferring runtime teardown");
        CHECK(!js_page_entry_active(), "module close reaches app_exit outside the QuickJS entry");
    }
    int expected_requests = close_mode == CLOSE_MODULE ? 3 :
                            close_mode >= CLOSE_RESOURCE ? 2 : 1;
    CHECK(fake_site_requests() == expected_requests,
          "scenario issues only its expected network requests");

    if (!host_exited) { js_page_close(); bfetch_close_all(); }
    printf("browser-close-load %s: %s (polls=%d clock=%d watchdog=%d scroll_delta=%d)\n",
           argv[1], failed ? "FAIL" : "PASS", poll_steps, close_clock_samples,
           js_page_slice_hits(), wheel_scroll_y);
    return failed;
}
