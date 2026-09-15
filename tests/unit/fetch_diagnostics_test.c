/* SPDX-License-Identifier: MIT */
/* Exercise the browser's real fetch/Response code over a local byte-stream
 * fixture. An HTTP refusal is still a Response; a closed/truncated exchange is
 * a rejection. These distinctions are what a generic connection toast hides.
 * The deliberately distinctive fixture secrets must never reach stdout. */
#include "stream_net.h"

static int reply_mode;
static void route(struct fakesock *s, const char *method, const char *target)
{
    (void)target;
    if (reply_mode == 2) return; /* Clean EOF before any HTTP response. */
    if (reply_mode == 3) {
        rsp_add(s, "HTTP/1.1 200 OK\r\nContent-Length: 80\r\n\r\ncut");
        return;
    }
    if (reply_mode == 5 && req_count() == 1) {
        rsp_add(s, "HTTP/1.1 302 Found\r\nLocation: /fixture_path_secret?token=fixture_query_secret\r\nContent-Length: 0\r\n\r\n");
        return;
    }
    if (reply_mode == 6 && !strcmp(method, "OPTIONS")) {
        rsp_add(s, "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: https://page.example\r\n"
                   "Access-Control-Allow-Methods: POST\r\nAccess-Control-Allow-Headers: x-test\r\n\r\n");
        return;
    }
    rsp_add(s, reply_mode == 1 ? "HTTP/1.1 403 fixture_reason_secret\r\n" : "HTTP/1.1 200 fixture_reason_secret\r\n");
    if (reply_mode == 6) rsp_add(s, "Access-Control-Allow-Origin: https://page.example\r\n");
    rsp_add(s, "Content-Type: application/json\r\nX-Secret: fixture_header_secret\r\n"
               "Set-Cookie: diag=fixture_cookie_secret; Secure; Path=/\r\n");
    const char *body = "{\"ok\":false,\"message\":\"fixture_body_secret\"}";
    char length[80];
    snprintf(length, sizeof length, "Content-Length: %d\r\n\r\n", (int)strlen(body));
    rsp_add(s, length); rsp_add(s, body);
}

static void sample(const char *label, int mode, int expected_status, int rejected, int requests)
{
    printf("DIAG-CASE %s\n", label);
    fs_reset(); fake_now = 1000; reply_mode = mode;
    fs_set_router(route);
    open_ctx("https://page.example/");
    run("document.cookie='client=fixture_cookie_secret; Secure; Path=/';");
    const char *base = mode == 7 ? "https://unreachable.example" :
        mode == 4 || mode == 6 ? "https://other.example" : "https://page.example";
    const char *method = mode == 2 || mode == 3 || mode == 6 ? "POST" : "GET";
    char script[1600];
    snprintf(script, sizeof script,
        "var done=false,rejected=false,status=-1,body='';"
        "fetch('%s/fixture_path_secret?token=fixture_query_secret#fixture_fragment_secret',"
        "{method:'%s',headers:%s%s}).then(function(r){status=r.status;return r.text()})"
        ".then(function(t){body=t;done=true},function(e){rejected=true;done=true});",
        base, method,
        mode == 6 ? "{'X-Test':'fixture_authorization_secret'}" :
        mode == 4 ? "{}" : "{'Authorization':'Bearer fixture_authorization_secret'}",
        !strcmp(method, "POST") ? ",body:'fixture_request_body_secret'" : "");
    run(script); settle(600);
    ckjs("done", "request reaches a terminal result");
    ckjs(rejected ? "rejected" : "!rejected", "transport and HTTP results remain distinct");
    char expression[100]; snprintf(expression, sizeof expression, "status===%d", expected_status);
    ckjs(expression, "page sees the correct HTTP status boundary");
    if (!rejected) ckjs("JSON.parse(body).ok===false", "application error body remains available to the page");
    ck(req_count() == requests, "only the expected requests were sent");
    ck(!js_webapi_pending(), "request releases its transport");
    close_ctx();
}

int main(void)
{
    /* The legacy URL splitter can retain userinfo in its host buffer. Even
     * malformed authority must not turn the new host diagnostic into a
     * credential logger. No real socket is opened by this fixture. */
    fs_reset(); fake_now = 1000; reply_mode = 0; fs_set_router(route);
    open_ctx("https://page.example/");
    run("var privacyDone=false;fetch('https://fixture_userinfo_secret@page.example/')"
        ".then(function(){privacyDone=true},function(){privacyDone=true});");
    settle(600);
    ckjs("privacyDone", "malformed authority privacy control completes");
    close_ctx();
    sample("application", 0, 200, 0, 1);
    sample("http-refusal", 1, 403, 0, 1);
    sample("no-response", 2, -1, 1, 1);
    sample("truncated", 3, 200, 1, 1);
    sample("cors", 4, -1, 1, 1);
    sample("redirect", 5, 200, 0, 2);
    sample("preflight", 6, 200, 0, 2);
    sample("connect-start", 7, -1, 1, 0);
    printf("fetch-diagnostics-behavior: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
