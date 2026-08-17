/* Host tests for the two rules that only matter when they REFUSE: which
 * requests carry the user's cookies, and which cross-origin responses a page is
 * allowed to read.
 *
 *     make test-cookie-cors
 *
 * tests/unit/cookie_test.c already covers the jar's own rules (domain-match,
 * path-match, the 6265bis tightenings) against their near-misses. This file
 * covers the wiring: that a Set-Cookie really comes back on the wire, that
 * document.cookie cannot see an HttpOnly one, that a cross-site fetch does not
 * ride the session, and that a cross-origin response without the server's
 * opt-in is refused BEFORE its body could reach script.
 *
 * Every refusal here is a test that would pass trivially against a browser that
 * simply ignored CORS -- so each one is paired with the permitted case, which
 * that browser would also pass, and with an assertion about what went on the
 * wire, which it would not.
 */

#include "stream_net.h"

/* ---- the routes -------------------------------------------------------
 * Each route decides its own CORS headers, because "which header did the
 * server send" is the entire subject. */

static void router(struct fakesock *s, const char *method, const char *target)
{
    char hdr[1024];

    if (!strcmp(target, "/login")) {
        rsp_add(s, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                   "Set-Cookie: sid=abc123; Path=/\r\n"
                   "Set-Cookie: pref=dark; Path=/; Max-Age=3600\r\n"
                   "Set-Cookie: secret=zzz; Path=/; HttpOnly\r\n"
                   "Content-Length: 2\r\n\r\nok");
    } else if (!strcmp(target, "/secure-cookie")) {
        rsp_add(s, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                   "Set-Cookie: onlytls=1; Path=/; Secure\r\n"
                   "Content-Length: 2\r\n\r\nok");
    } else if (!strcmp(target, "/samesite")) {
        rsp_add(s, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                   "Access-Control-Allow-Origin: http://page.example\r\n"
                   "Access-Control-Allow-Credentials: true\r\n"
                   "Set-Cookie: lax=1; Path=/\r\n"
                   "Set-Cookie: strict=1; Path=/; SameSite=Strict\r\n"
                   "Content-Length: 2\r\n\r\nok");
    } else if (!strcmp(target, "/echo")) {
        /* The body is the request verbatim: a test can assert on the exact
         * bytes that left, including the ones that should not be there. */
        snprintf(hdr, sizeof hdr, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                                  "Access-Control-Allow-Origin: *\r\n"
                                  "Content-Length: %d\r\n\r\n", s->req_len);
        rsp_add(s, hdr);
        if (s->rsp_len + s->req_len <= RSP_MAX) {
            memcpy(s->rsp + s->rsp_len, s->req, (size_t)s->req_len);
            s->rsp_len += s->req_len;
        }

    /* ---- CORS routes ---- */
    } else if (!strcmp(target, "/cors-none")) {
        rsp_add(s, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                   "Content-Length: 6\r\nX-Secret: leak\r\n\r\nsecret");
    } else if (!strcmp(target, "/cors-star")) {
        rsp_add(s, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                   "Access-Control-Allow-Origin: *\r\n"
                   "X-Secret: hidden\r\nX-Shown: yes\r\n"
                   "Access-Control-Expose-Headers: X-Shown\r\n"
                   "Content-Length: 4\r\n\r\nopen");
    } else if (!strcmp(target, "/cors-other")) {
        rsp_add(s, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                   "Access-Control-Allow-Origin: http://evil.example\r\n"
                   "Content-Length: 4\r\n\r\nnope");
    } else if (!strcmp(target, "/cors-dup")) {
        rsp_add(s, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                   "Access-Control-Allow-Origin: *\r\n"
                   "Access-Control-Allow-Origin: http://page.example\r\n"
                   "Content-Length: 4\r\n\r\nnope");
    } else if (!strcmp(target, "/cors-creds-star")) {
        rsp_add(s, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                   "Access-Control-Allow-Origin: *\r\n"
                   "Access-Control-Allow-Credentials: true\r\n"
                   "Content-Length: 4\r\n\r\nnope");
    } else if (!strcmp(target, "/cors-creds-ok")) {
        rsp_add(s, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                   "Access-Control-Allow-Origin: http://page.example\r\n"
                   "Access-Control-Allow-Credentials: true\r\n"
                   "Set-Cookie: third=1; Path=/\r\n"
                   "Content-Length: 2\r\n\r\nok");
    } else if (!strcmp(target, "/cors-nocreds-setcookie")) {
        rsp_add(s, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                   "Access-Control-Allow-Origin: *\r\n"
                   "Set-Cookie: planted=1; Path=/\r\n"
                   "Content-Length: 2\r\n\r\nok");

    /* ---- preflighted routes ---- */
    } else if (!strcmp(target, "/pf-allow")) {
        if (!strcmp(method, "OPTIONS")) {
            rsp_add(s, "HTTP/1.1 204 No Content\r\n"
                       "Access-Control-Allow-Origin: http://page.example\r\n"
                       "Access-Control-Allow-Methods: PUT, DELETE\r\n"
                       "Access-Control-Allow-Headers: x-token\r\n"
                       "Access-Control-Max-Age: 600\r\n"
                       "Content-Length: 0\r\n\r\n");
        } else {
            rsp_add(s, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                       "Access-Control-Allow-Origin: http://page.example\r\n"
                       "Content-Length: 4\r\n\r\ndone");
        }
    } else if (!strcmp(target, "/pf-deny-header")) {
        if (!strcmp(method, "OPTIONS")) {
            rsp_add(s, "HTTP/1.1 204 No Content\r\n"
                       "Access-Control-Allow-Origin: http://page.example\r\n"
                       "Access-Control-Allow-Methods: PUT\r\n"
                       "Content-Length: 0\r\n\r\n");
        } else {
            rsp_add(s, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                       "Access-Control-Allow-Origin: *\r\n"
                       "Content-Length: 9\r\n\r\nSHOULDNOT");
        }
    } else if (!strcmp(target, "/pf-deny-method")) {
        if (!strcmp(method, "OPTIONS")) {
            rsp_add(s, "HTTP/1.1 204 No Content\r\n"
                       "Access-Control-Allow-Origin: http://page.example\r\n"
                       "Access-Control-Allow-Methods: GET\r\n"
                       "Content-Length: 0\r\n\r\n");
        } else {
            rsp_add(s, "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\n"
                       "Content-Length: 9\r\n\r\nSHOULDNOT");
        }
    } else {
        rsp_add(s, "HTTP/1.1 404 Not Found\r\nAccess-Control-Allow-Origin: *\r\n"
                   "Content-Length: 8\r\n\r\nno route");
    }
}

/* Did any request that went out use this method? */
static int saw_method(const char *m)
{
    int n = (int)strlen(m);
    for (int i = 0; i < req_count(); i++)
        if (!strncmp(nth_req(i), m, (size_t)n) && nth_req(i)[n] == ' ') return 1;
    return 0;
}

/* ---- 1. cookies on the fetch path ------------------------------------- */

static void test_cookies(void)
{
    printf("\n-- cookies reach the wire --\n");
    fs_reset();
    run("var L = null; fetch('/login').then(function (r) { return r.text(); })"
        ".then(function (t) { L = t; });");
    settle(60);
    ckjs("L === 'ok'", "the response that sets the cookies arrived");

    fs_reset();
    run("var E = null; fetch('/echo').then(function (r) { return r.text(); })"
        ".then(function (t) { E = t; });");
    settle(60);
    ckjs("E && /Cookie: /.test(E)", "the NEXT request carries a Cookie header");
    ckjs("E && /sid=abc123/.test(E)", "...with the session cookie");
    ckjs("E && /pref=dark/.test(E)", "...and the persistent one");
    ckjs("E && /secret=zzz/.test(E)",
         "...and the HttpOnly one, which the network may see");

    ckjs("document.cookie.indexOf('sid=abc123') >= 0", "document.cookie reads the jar");
    ckjs("document.cookie.indexOf('secret') < 0",
         "...and CANNOT see the HttpOnly cookie -- which is the whole point of it");

    run("document.cookie = 'fromjs=1; Path=/';");
    ckjs("document.cookie.indexOf('fromjs=1') >= 0", "document.cookie writes the jar");
    fs_reset();
    run("var E2 = null; fetch('/echo').then(function (r) { return r.text(); })"
        ".then(function (t) { E2 = t; });");
    settle(60);
    ckjs("E2 && /fromjs=1/.test(E2)", "...and the write reaches the next request");

    run("document.cookie = 'secret=hijacked; Path=/';");
    fs_reset();
    run("var E3 = null; fetch('/echo').then(function (r) { return r.text(); })"
        ".then(function (t) { E3 = t; });");
    settle(60);
    ckjs("E3 && /secret=zzz/.test(E3) && !/secret=hijacked/.test(E3)",
         "script cannot overwrite an HttpOnly cookie either");

    /* A Secure cookie must not be settable, or sent, over plaintext. */
    fs_reset();
    run("var S = null; fetch('/secure-cookie').then(function (r) { return r.text(); })"
        ".then(function (t) { S = t; });");
    settle(60);
    ckjs("document.cookie.indexOf('onlytls') < 0",
         "a Secure cookie set over http is refused");

    /* A page cannot smuggle its own Cookie header past the jar. */
    fs_reset();
    run("var F = null; fetch('/echo', { headers: { 'Cookie': 'sid=forged' } })"
        ".then(function (r) { return r.text(); }).then(function (t) { F = t; });");
    settle(60);
    ckjs("F && !/sid=forged/.test(F)",
         "a page-supplied Cookie header is dropped -- the jar decides, not the page");
    ckjs("F && /sid=abc123/.test(F)", "...and the real cookie still goes");
}

static void test_samesite(void)
{
    printf("\n-- SameSite: a cross-site fetch does not ride the session --\n");
    fs_reset();
    /* Set lax + strict cookies on ANOTHER site, credentialed so they store. */
    run("var SS = null; fetch('http://other.example/samesite', { credentials: 'include' })"
        ".then(function (r) { return r.text(); }).then(function (t) { SS = t; })"
        ".catch(function (e) { SS = 'ERR ' + e.message; });");
    settle(60);
    ckjs("SS === 'ok'", "the cross-origin response was allowed (it opted in)");

    fs_reset();
    run("var CS = null; fetch('http://other.example/echo', { credentials: 'include' })"
        ".then(function (r) { return r.text(); }).then(function (t) { CS = t; })"
        ".catch(function (e) { CS = 'ERR ' + e.message; });");
    settle(60);
    ckjs("CS && !/lax=1/.test(CS)",
         "an unattributed cookie (= Lax) is NOT sent on a cross-site fetch");
    ckjs("CS && !/strict=1/.test(CS)", "nor is a SameSite=Strict one");
}

/* ---- 2. CORS, and what it refuses ------------------------------------- */

static void test_cors_simple(void)
{
    printf("\n-- CORS: simple requests --\n");

    fs_reset();
    run("var N = 'pending', NB = null;"
        "fetch('http://other.example/cors-none').then(function (r) { N = 'resolved';"
        "  return r.text(); }).then(function (t) { NB = t; },"
        "  function (e) { if (N === 'pending') N = e.name; });");
    settle(80);
    ckjs("N === 'TypeError'",
         "REFUSED: a cross-origin response with no Access-Control-Allow-Origin");
    ckjs("NB === null", "...and not one byte of its body reached script");

    fs_reset();
    run("var O = 'pending'; fetch('http://other.example/cors-other')"
        ".then(function () { O = 'resolved'; }, function (e) { O = e.name; });");
    settle(80);
    ckjs("O === 'TypeError'",
         "REFUSED: Access-Control-Allow-Origin naming a DIFFERENT origin");

    fs_reset();
    run("var D = 'pending'; fetch('http://other.example/cors-dup')"
        ".then(function () { D = 'resolved'; }, function (e) { D = e.name; });");
    settle(80);
    ckjs("D === 'TypeError'",
         "REFUSED: two Access-Control-Allow-Origin headers (pick-one is a bypass)");

    fs_reset();
    run("var S = null, SH = null, SS2 = null;"
        "fetch('http://other.example/cors-star').then(function (r) {"
        "  SH = r.headers.get('x-shown'); SS2 = r.headers.get('x-secret');"
        "  return r.text(); }).then(function (t) { S = t; });");
    settle(80);
    ckjs("S === 'open'", "ALLOWED: Access-Control-Allow-Origin: * on a simple GET");
    ckjs("SH === 'yes'", "a header named by Access-Control-Expose-Headers is readable");
    ckjs("SS2 === null",
         "a header that is NOT exposed is invisible, even though it arrived");
    ck(req_has(nth_req(0), "Origin: http://page.example"),
       "the cross-origin request carried Origin");

    fs_reset();
    run("var SO = 'pending'; fetch('http://other.example/cors-star', { mode: 'same-origin' })"
        ".then(function () { SO = 'resolved'; }, function (e) { SO = e.name; });");
    settle(40);
    ckjs("SO === 'TypeError'", "REFUSED: mode 'same-origin' on a cross-origin URL");
    ck(req_count() == 0, "...and nothing was sent at all");

    fs_reset();
    run("var NC = null; fetch('http://other.example/cors-none', { mode: 'no-cors' })"
        ".then(function (r) { NC = { s: r.status, t: r.type, h: r.headers.get('x-secret') }; });");
    settle(80);
    ckjs("NC && NC.s === 0 && NC.t === 'opaque' && NC.h === null",
         "mode 'no-cors' yields an OPAQUE response: no status, no headers, no body");
}

static void test_cors_credentials(void)
{
    printf("\n-- CORS: credentials --\n");
    fs_reset();
    run("var CS = 'pending'; fetch('http://other.example/cors-creds-star',"
        "  { credentials: 'include' })"
        ".then(function () { CS = 'resolved'; }, function (e) { CS = e.name; });");
    settle(80);
    ckjs("CS === 'TypeError'",
         "REFUSED: Allow-Origin '*' with credentials -- the combination the spec bans");

    fs_reset();
    run("var CO = null; fetch('http://other.example/cors-creds-ok',"
        "  { credentials: 'include' })"
        ".then(function (r) { return r.text(); }).then(function (t) { CO = t; },"
        " function (e) { CO = 'ERR ' + e.name; });");
    settle(80);
    ckjs("CO === 'ok'",
         "ALLOWED: an exact Allow-Origin plus Allow-Credentials: true");

    /* An uncredentialed cross-origin response must not be able to plant a
     * cookie in that origin's jar. */
    fs_reset();
    run("var PL = null; fetch('http://other.example/cors-nocreds-setcookie')"
        ".then(function (r) { return r.text(); }).then(function (t) { PL = t; });");
    settle(80);
    ckjs("PL === 'ok'", "the uncredentialed cross-origin request itself is fine");
    fs_reset();
    run("var PE = null; fetch('http://other.example/echo', { credentials: 'include' })"
        ".then(function (r) { return r.text(); }).then(function (t) { PE = t; },"
        " function (e) { PE = 'ERR'; });");
    settle(80);
    ckjs("PE && !/planted=1/.test(PE)",
         "REFUSED: a response that was not allowed credentials cannot SET a cookie");
}

static void test_cors_preflight(void)
{
    printf("\n-- CORS: the OPTIONS preflight --\n");

    fs_reset();
    run("var PD = 'pending';"
        "fetch('http://other.example/pf-deny-header',"
        "      { method: 'PUT', headers: { 'X-Token': 'abc' }, body: 'x' })"
        ".then(function () { PD = 'resolved'; }, function (e) { PD = e.name; });");
    settle(120);
    ckjs("PD === 'TypeError'",
         "REFUSED: the preflight did not allow the request header");
    ck(saw_method("OPTIONS"), "...an OPTIONS preflight was sent");
    ck(!saw_method("PUT"),
       "...and the PUT was NEVER SENT -- which is what a preflight is for");

    fs_reset();
    run("var PM = 'pending';"
        "fetch('http://other.example/pf-deny-method', { method: 'DELETE' })"
        ".then(function () { PM = 'resolved'; }, function (e) { PM = e.name; });");
    settle(120);
    ckjs("PM === 'TypeError'", "REFUSED: the preflight did not allow the method");
    ck(!saw_method("DELETE"), "...and the DELETE never left");

    fs_reset();
    run("var PA = null;"
        "fetch('http://other.example/pf-allow',"
        "      { method: 'PUT', headers: { 'X-Token': 'abc' }, body: 'x' })"
        ".then(function (r) { return r.text(); }).then(function (t) { PA = t; },"
        " function (e) { PA = 'ERR ' + e.message; });");
    settle(160);
    ckjs("PA === 'done'", "ALLOWED: the preflight allowed the method and the header");
    ck(saw_method("OPTIONS") && saw_method("PUT"), "...both requests went out");
    ck(req_has(nth_req(0), "Access-Control-Request-Method: PUT"),
       "the preflight declared the method it was asking about");
    ck(req_has(nth_req(0), "Access-Control-Request-Headers: x-token"),
       "...and the non-safelisted header");
    ck(!req_has(nth_req(0), "X-Token: abc"),
       "the preflight itself does not carry the header, only its name");

    /* Access-Control-Max-Age: the second identical request must not preflight
     * again, or an app that PATCHes per keystroke pays two round trips each. */
    fs_reset();
    run("var PC = null;"
        "fetch('http://other.example/pf-allow',"
        "      { method: 'PUT', headers: { 'X-Token': 'abc' }, body: 'y' })"
        ".then(function (r) { return r.text(); }).then(function (t) { PC = t; });");
    settle(160);
    ckjs("PC === 'done'", "the cached-preflight request succeeds");
    ck(!saw_method("OPTIONS"),
       "...with NO second OPTIONS: Access-Control-Max-Age was honoured");

    /* A POST with a JSON content-type is the commonest preflighted request in
     * the world, and the commonest thing a naive client gets wrong. */
    fs_reset();
    run("var PJ = null;"
        "fetch('http://other.example/pf-allow',"
        "      { method: 'PUT', headers: { 'Content-Type': 'application/json' },"
        "        body: '{\\\"a\\\":1}' })"
        ".then(function (r) { return r.text(); }).then(function (t) { PJ = t; },"
        " function (e) { PJ = 'ERR ' + e.message; });");
    settle(160);
    ck(saw_method("OPTIONS"),
       "an application/json content-type is not safelisted, so it preflights");

    /* Same-origin traffic is not preflighted at all. */
    fs_reset();
    run("var SM = null;"
        "fetch('/echo', { method: 'PUT', headers: { 'X-Token': 'abc' }, body: 'z' })"
        ".then(function (r) { return r.text(); }).then(function (t) { SM = t; });");
    settle(120);
    ckjs("SM && /^PUT \\/echo/.test(SM)", "a same-origin PUT goes straight out");
    ck(!saw_method("OPTIONS"), "...with no preflight");
}

/* ---- the transport path -------------------------------------------------
 *
 * WHY THIS IS HERE AND NOT ONE MORE fetch() TEST. Everything above drives the
 * jar through fetch()/XHR, and that path computed SameSite correctly from the
 * day it was written. The browser has a SECOND door into the same jar --
 * webapi_cookie_line(), which browser_rt.c calls for the navigation and for
 * every subresource -- and it went in wired to cookie_header(), i.e. to
 * CK_REQ_SAME_SITE unconditionally. So every cross-origin <script src> and
 * <link rel=stylesheet> a page named carried the target's Secure + HttpOnly +
 * SameSite=Strict session, and the reply was evaluated in the requesting
 * page's realm.
 *
 * This file's own title is "which requests carry the session, and which", and
 * it did not see that for one reason: it only ever asked the door that was
 * already right. A gate aimed at a rule has to be aimed at every caller of
 * the rule, or it certifies the caller it happens to know.
 *
 * The document is http://page.example/ (main's open_ctx), so other.example is
 * cross-site and page.example is not. */
int  webapi_cookie_line(const char *host, const char *path, int secure,
                        int nav, char *out, int cap);
void webapi_cookie_store_line(const char *host, const char *path, int secure,
                              const char *setcookie);

static void test_transport_samesite(void)
{
    printf("\n-- the transport door: a subresource is not a navigation --\n");
    /* Stored by the NETWORK, which is the only way a Strict cookie for another
     * site gets into the jar at all. Secure throughout: SameSite=None without
     * it is refused by cookies.c, and holding the other three to the same
     * attribute keeps the four differing in exactly one thing. */
    webapi_cookie_store_line("other.example", "/", 1, "u=1; Path=/; Secure");
    webapi_cookie_store_line("other.example", "/", 1, "l=1; Path=/; Secure; SameSite=Lax");
    webapi_cookie_store_line("other.example", "/", 1, "s=1; Path=/; Secure; SameSite=Strict");
    webapi_cookie_store_line("other.example", "/", 1, "n=1; Path=/; Secure; SameSite=None");
    webapi_cookie_store_line("page.example", "/", 1, "own=1; Path=/; Secure; SameSite=Strict");

    char line[1024];
    int n;

    n = webapi_cookie_line("other.example", "/", 1, 0, line, (int)sizeof line);
    if (n <= 0) line[0] = 0;
    printf("      subresource -> Cookie: %s\n", line[0] ? line : "(none)");
    ck(!strstr(line, "s=1"), "a cross-site SUBRESOURCE carries no Strict cookie");
    ck(!strstr(line, "l=1"), "...nor a Lax one");
    ck(!strstr(line, "u=1"), "...nor an unattributed one, which 6265bis reads as Lax");
    ck(strstr(line, "n=1") != NULL, "...and does carry SameSite=None, or nothing opted in");

    n = webapi_cookie_line("other.example", "/", 1, 1, line, (int)sizeof line);
    if (n <= 0) line[0] = 0;
    printf("      navigation  -> Cookie: %s\n", line[0] ? line : "(none)");
    ck(!strstr(line, "s=1"), "a cross-site NAVIGATION still carries no Strict cookie");
    ck(strstr(line, "l=1") != NULL, "...but does carry Lax, which is what Lax means");
    ck(strstr(line, "u=1") != NULL, "...and unattributed with it");

    n = webapi_cookie_line("page.example", "/", 1, 0, line, (int)sizeof line);
    if (n <= 0) line[0] = 0;
    ck(strstr(line, "own=1") != NULL,
       "a SAME-site subresource carries the Strict cookie -- the rule refuses, it does not block");
}

int main(void)
{
    fake_now = 1000;
    fs_reset();
    fs_set_router(router);
    open_ctx("http://page.example/dir/index.html");

    test_cookies();
    test_samesite();
    test_cors_simple();
    test_cors_credentials();
    test_cors_preflight();
    test_transport_samesite();

    close_ctx();

    printf("\n%d checks, %d failures\n", checks, failures);
    if (failures) { printf("cookie_cors_test: FAIL\n"); return 1; }
    printf("cookie_cors_test: ALL PASS\n");
    return 0;
}
