/* Host unit test for c/net/http/cookies.c.
 *
 * The interesting assertions here are the NEGATIVE ones. Any implementation
 * passes "example.com gets example.com's cookie"; what separates a correct jar
 * from an exploitable one is that evil.com does not, that notexample.com does
 * not, that 10.0.0.1 does not domain-match 0.0.1, that /foobar does not match
 * path=/foo, and that a Secure cookie never crosses a plaintext connection.
 * Those are the cases below.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "cookies.h"

static int fails;
#define OK(cond) do { if (cond) printf("ok   %s\n", #cond); \
                      else { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

#define NOW ((int64_t)1700000000)     /* 2023-11-14T22:13:20Z */

static struct cookie_ctx CTX(const char *host, const char *path, int secure, int http)
{
    struct cookie_ctx c;
    c.host = host; c.path = path; c.secure = secure; c.http_api = http;
    return c;
}

/* Build the Cookie: value that `host`+`path` would receive. */
static const char *hdr_for(struct cookie_jar *j, const char *host, const char *path,
                           int secure, int http)
{
    static char buf[2048];
    struct cookie_ctx c = CTX(host, path, secure, http);
    int n = cookie_header(j, &c, NOW, buf, (int)sizeof buf);
    if (n < 0) { strcpy(buf, "<error>"); }
    return buf;
}

static int set_from(struct cookie_jar *j, const char *host, const char *path,
                    int secure, const char *sc)
{
    struct cookie_ctx c = CTX(host, path, secure, 1);
    return cookie_set(j, &c, sc, NOW);
}

/* ------------------------------------------------------- domain matching */

static void t_domain_match(void)
{
    OK(cookie_domain_match("example.com", "example.com"));
    OK(cookie_domain_match("EXAMPLE.com", "example.COM"));
    OK(cookie_domain_match("a.example.com", "example.com"));
    OK(cookie_domain_match("a.b.c.example.com", "example.com"));
    OK(cookie_domain_match("a.example.com", "a.example.com"));

    /* The suffix-without-a-dot-boundary family. Drop the boundary check and
     * every one of these becomes a cookie leak. */
    OK(!cookie_domain_match("notexample.com", "example.com"));
    OK(!cookie_domain_match("evilexample.com", "example.com"));
    OK(!cookie_domain_match("example.com.evil.com", "example.com"));
    OK(!cookie_domain_match("example.com", "a.example.com"));   /* parent != child */
    OK(!cookie_domain_match("evil.com", "example.com"));
    OK(!cookie_domain_match("example.com", "xample.com"));
    OK(!cookie_domain_match("example.com", ".example.com"));    /* dot already stripped by set */
    OK(!cookie_domain_match("example.com", ""));
    OK(!cookie_domain_match("", "example.com"));

    /* An IP literal is never suffix-matched: 10.0.0.1 must not be "in" 0.0.1. */
    OK(cookie_domain_match("10.0.0.1", "10.0.0.1"));
    OK(!cookie_domain_match("10.0.0.1", "0.0.1"));
    OK(!cookie_domain_match("192.168.1.10", "168.1.10"));
    OK(!cookie_domain_match("10.0.0.1", "1"));
}

static void t_path_match(void)
{
    OK(cookie_path_match("/", "/"));
    OK(cookie_path_match("/foo", "/"));
    OK(cookie_path_match("/foo", "/foo"));
    OK(cookie_path_match("/foo/bar", "/foo"));
    OK(cookie_path_match("/foo/bar", "/foo/"));
    OK(cookie_path_match("/foo/", "/foo"));

    /* The classic near-miss: /foo must not cover /foobar. */
    OK(!cookie_path_match("/foobar", "/foo"));
    OK(!cookie_path_match("/foo", "/foo/bar"));
    OK(!cookie_path_match("/bar", "/foo"));
    OK(!cookie_path_match("/", "/foo"));
    OK(!cookie_path_match("foo", "/foo"));       /* not a path */
    OK(!cookie_path_match("/foo", ""));
}

static void t_default_path(void)
{
    char p[64];
    OK(cookie_default_path("/a/b/c", p, sizeof p) == 0 && !strcmp(p, "/a/b"));
    OK(cookie_default_path("/a/b/", p, sizeof p) == 0 && !strcmp(p, "/a/b"));
    OK(cookie_default_path("/a", p, sizeof p) == 0 && !strcmp(p, "/"));
    OK(cookie_default_path("/", p, sizeof p) == 0 && !strcmp(p, "/"));
    OK(cookie_default_path("", p, sizeof p) == 0 && !strcmp(p, "/"));
    OK(cookie_default_path("relative/x", p, sizeof p) == 0 && !strcmp(p, "/"));
    OK(cookie_default_path(NULL, p, sizeof p) == 0 && !strcmp(p, "/"));
    OK(cookie_default_path("/a/b?q=/x/y", p, sizeof p) == 0 && !strcmp(p, "/a"));
    OK(cookie_default_path("/a/b#/x/y", p, sizeof p) == 0 && !strcmp(p, "/a"));
}

static void t_canon_host(void)
{
    char h[64];
    OK(cookie_canon_host("Example.COM", h, sizeof h) == 0 && !strcmp(h, "example.com"));
    OK(cookie_canon_host("example.com.", h, sizeof h) == 0 && !strcmp(h, "example.com"));
    OK(cookie_canon_host("", h, sizeof h) == -1);
    OK(cookie_canon_host(".", h, sizeof h) == -1);
    OK(cookie_canon_host("example.com", h, 5) == -1);
}

/* ------------------------------------------------------------- the dates */

static void t_dates(void)
{
    /* The three spellings every real server uses. */
    OK(cookie_parse_date("Wed, 21 Oct 2015 07:28:00 GMT") == 1445412480);
    OK(cookie_parse_date("Wednesday, 21-Oct-15 07:28:00 GMT") == 1445412480);
    OK(cookie_parse_date("Wed Oct 21 07:28:00 2015") == 1445412480);
    /* Field order is not fixed by RFC 6265 5.1.1 -- it scans for whichever
     * token looks like what it still needs. */
    OK(cookie_parse_date("07:28:00 21 Oct 2015") == 1445412480);
    OK(cookie_parse_date("Thu, 01 Jan 1970 00:00:01 GMT") == 1);
    OK(cookie_parse_date("Tue, 19 Jan 2038 03:14:07 GMT") == 2147483647);
    OK(cookie_parse_date("Fri, 31 Dec 1999 23:59:59 GMT") == 946684799);

    /* Two-digit years: 70..99 -> 19xx, 00..69 -> 20xx. */
    OK(cookie_parse_date("Sat, 01 Jan 2000 00:00:00 GMT") ==
       cookie_parse_date("Sat, 01-Jan-00 00:00:00 GMT"));
    OK(cookie_parse_date("Thu, 01 Jan 1998 00:00:00 GMT") ==
       cookie_parse_date("Thu, 01-Jan-98 00:00:00 GMT"));

    /* Unparseable is 0, and must never be a wild value: an Expires the jar
     * misreads as the year 30000 is a cookie that never goes away. */
    const char *bad[] = {
        "", "garbage", "Wed, 21 Oct 2015", "21 Oct 07:28:00", "Oct 2015 07:28:00",
        "Wed, 99 Oct 2015 07:28:00 GMT",          /* day > 31 */
        "Wed, 21 Oct 2015 25:28:00 GMT",          /* hour > 23 */
        "Wed, 21 Oct 2015 07:61:00 GMT",          /* minute > 59 */
        "Wed, 21 Oct 1500 07:28:00 GMT",          /* year < 1601 */
        "::::::::::", ";;;;;;;;;;", "0 0 0 0 0 0",
        NULL
    };
    for (int i = 0; bad[i]; i++)
        if (cookie_parse_date(bad[i]) != 0) { printf("FAIL parsed bad date '%s'\n", bad[i]); fails++; }
    printf("ok   12 unparseable dates all return 0\n");
}

/* --------------------------------------------------------- the jar proper */

static void t_basic(void)
{
    struct cookie_jar j;
    cookie_jar_init(&j);

    OK(set_from(&j, "example.com", "/", 0, "sid=abc123") == 0);
    OK(!strcmp(hdr_for(&j, "example.com", "/", 0, 1), "sid=abc123"));

    /* A host-only cookie (no Domain attribute) does NOT widen to subdomains. */
    OK(!strcmp(hdr_for(&j, "www.example.com", "/", 0, 1), ""));

    /* And most importantly, nobody else gets it. */
    OK(!strcmp(hdr_for(&j, "evil.com", "/", 0, 1), ""));
    OK(!strcmp(hdr_for(&j, "notexample.com", "/", 0, 1), ""));
    OK(!strcmp(hdr_for(&j, "example.com.evil.com", "/", 0, 1), ""));

    /* Overwrite by (name, domain, path). */
    OK(set_from(&j, "example.com", "/", 0, "sid=xyz") == 0);
    OK(cookie_jar_count(&j) == 1);
    OK(!strcmp(hdr_for(&j, "example.com", "/", 0, 1), "sid=xyz"));

    /* Several cookies join with "; ". */
    OK(set_from(&j, "example.com", "/", 0, "theme=dark") == 0);
    OK(!strcmp(hdr_for(&j, "example.com", "/", 0, 1), "sid=xyz; theme=dark"));

    cookie_jar_free(&j);
}

static void t_domain_attribute(void)
{
    struct cookie_jar j;
    cookie_jar_init(&j);

    /* Domain= widens to subdomains, which is its entire purpose. */
    OK(set_from(&j, "example.com", "/", 0, "w=1; Domain=example.com") == 0);
    OK(!strcmp(hdr_for(&j, "example.com", "/", 0, 1), "w=1"));
    OK(!strcmp(hdr_for(&j, "a.example.com", "/", 0, 1), "w=1"));
    OK(!strcmp(hdr_for(&j, "a.b.example.com", "/", 0, 1), "w=1"));
    OK(!strcmp(hdr_for(&j, "evil.com", "/", 0, 1), ""));
    OK(!strcmp(hdr_for(&j, "notexample.com", "/", 0, 1), ""));

    /* A leading dot is legacy syntax for the same thing (RFC 6265 5.2.3). */
    OK(set_from(&j, "example.com", "/", 0, "d=2; Domain=.example.com") == 0);
    OK(strstr(hdr_for(&j, "x.example.com", "/", 0, 1), "d=2") != NULL);

    /* Case is irrelevant. */
    OK(set_from(&j, "example.com", "/", 0, "c=3; Domain=EXAMPLE.COM") == 0);
    OK(strstr(hdr_for(&j, "x.example.com", "/", 0, 1), "c=3") != NULL);

    cookie_jar_free(&j);
    cookie_jar_init(&j);

    /* A subdomain may widen to its parent... */
    OK(set_from(&j, "a.example.com", "/", 0, "up=1; Domain=example.com") == 0);
    OK(strstr(hdr_for(&j, "b.example.com", "/", 0, 1), "up=1") != NULL);

    /* ...but may not name a sibling, a child it is not under, or a stranger.
     * Each of these is a cookie planted on someone else's host. */
    OK(set_from(&j, "a.example.com", "/", 0, "x=1; Domain=b.example.com") == -1);
    OK(set_from(&j, "example.com", "/", 0, "x=1; Domain=a.example.com") == -1);
    OK(set_from(&j, "example.com", "/", 0, "x=1; Domain=evil.com") == -1);
    OK(set_from(&j, "evil.com", "/", 0, "x=1; Domain=example.com") == -1);
    OK(!strcmp(hdr_for(&j, "b.example.com", "/", 0, 1), "up=1"));
    OK(!strcmp(hdr_for(&j, "evil.com", "/", 0, 1), ""));

    cookie_jar_free(&j);
}

static void t_public_suffix(void)
{
    /* No PSL, so the conservative rule: >= 2 labels and not in the built-in
     * table. See the comment at the top of cookies.c for the residual gap. */
    OK(cookie_domain_is_public_suffix("com"));
    OK(cookie_domain_is_public_suffix("uk"));
    OK(cookie_domain_is_public_suffix("localhost"));
    OK(cookie_domain_is_public_suffix("co.uk"));
    OK(cookie_domain_is_public_suffix("com.au"));
    OK(cookie_domain_is_public_suffix("co.jp"));
    OK(!cookie_domain_is_public_suffix("example.com"));
    OK(!cookie_domain_is_public_suffix("bbc.co.uk"));
    OK(!cookie_domain_is_public_suffix("a.example.com"));

    struct cookie_jar j;
    cookie_jar_init(&j);
    /* The supercookie: without this check, one .com site cookies every .com. */
    OK(set_from(&j, "evil.com", "/", 0, "super=1; Domain=com") == -1);
    OK(set_from(&j, "evil.co.uk", "/", 0, "super=1; Domain=co.uk") == -1);
    OK(cookie_jar_count(&j) == 0);
    /* One level down is a normal registrable domain and must still work. */
    OK(set_from(&j, "www.bbc.co.uk", "/", 0, "ok=1; Domain=bbc.co.uk") == 0);
    OK(strstr(hdr_for(&j, "news.bbc.co.uk", "/", 0, 1), "ok=1") != NULL);
    OK(!strcmp(hdr_for(&j, "www.itv.co.uk", "/", 0, 1), ""));
    cookie_jar_free(&j);
}

static void t_ip_host(void)
{
    struct cookie_jar j;
    cookie_jar_init(&j);
    OK(set_from(&j, "10.0.2.2", "/", 0, "a=1") == 0);
    OK(!strcmp(hdr_for(&j, "10.0.2.2", "/", 0, 1), "a=1"));
    OK(!strcmp(hdr_for(&j, "10.0.2.3", "/", 0, 1), ""));
    /* An IP host cannot set a domain cookie for a suffix of its own digits. */
    OK(set_from(&j, "10.0.2.2", "/", 0, "b=1; Domain=0.2.2") == -1);
    OK(set_from(&j, "10.0.2.2", "/", 0, "b=1; Domain=2.2") == -1);
    cookie_jar_free(&j);
}

static void t_paths(void)
{
    struct cookie_jar j;
    cookie_jar_init(&j);

    OK(set_from(&j, "example.com", "/", 0, "root=1; Path=/") == 0);
    OK(set_from(&j, "example.com", "/", 0, "app=2; Path=/app") == 0);
    OK(set_from(&j, "example.com", "/", 0, "deep=3; Path=/app/sub") == 0);

    OK(!strcmp(hdr_for(&j, "example.com", "/", 0, 1), "root=1"));
    OK(!strcmp(hdr_for(&j, "example.com", "/other", 0, 1), "root=1"));
    OK(!strcmp(hdr_for(&j, "example.com", "/appendix", 0, 1), "root=1"));   /* not /app */
    /* RFC 6265 5.4: longest path first. Servers rely on the narrow cookie
     * shadowing the broad one. */
    OK(!strcmp(hdr_for(&j, "example.com", "/app", 0, 1), "app=2; root=1"));
    OK(!strcmp(hdr_for(&j, "example.com", "/app/sub/x", 0, 1), "deep=3; app=2; root=1"));

    /* The default path comes from the request, not from "/". */
    OK(set_from(&j, "example.com", "/a/b/c", 0, "dflt=9") == 0);
    OK(strstr(hdr_for(&j, "example.com", "/a/b/", 0, 1), "dflt=9") != NULL);
    OK(strstr(hdr_for(&j, "example.com", "/a/", 0, 1), "dflt=9") == NULL);

    /* A Path that is not absolute is ignored, falling back to the default. */
    OK(set_from(&j, "example.com", "/x/y", 0, "rel=1; Path=nope") == 0);
    OK(strstr(hdr_for(&j, "example.com", "/x/z", 0, 1), "rel=1") != NULL);
    OK(strstr(hdr_for(&j, "example.com", "/other", 0, 1), "rel=1") == NULL);

    /* The query string is not part of the path. */
    OK(strstr(hdr_for(&j, "example.com", "/app?x=/etc", 0, 1), "app=2") != NULL);

    cookie_jar_free(&j);
}

static void t_secure_httponly(void)
{
    struct cookie_jar j;
    cookie_jar_init(&j);

    /* Secure cookies may only be SET over https: otherwise an attacker on the
     * plaintext origin overwrites the https session cookie. */
    OK(set_from(&j, "example.com", "/", 0, "s=1; Secure") == -1);
    OK(set_from(&j, "example.com", "/", 1, "s=1; Secure") == 0);

    /* And may only be SENT over https. This is the whole point of the flag. */
    OK(!strcmp(hdr_for(&j, "example.com", "/", 1, 1), "s=1"));
    OK(!strcmp(hdr_for(&j, "example.com", "/", 0, 1), ""));

    /* HttpOnly: invisible to script, both ways. */
    OK(set_from(&j, "example.com", "/", 1, "h=2; HttpOnly") == 0);
    OK(strstr(hdr_for(&j, "example.com", "/", 1, 1), "h=2") != NULL);   /* network sees it */
    OK(strstr(hdr_for(&j, "example.com", "/", 1, 0), "h=2") == NULL);   /* script does not */

    {   /* script may neither create nor clobber one */
        struct cookie_ctx sc = CTX("example.com", "/", 1, 0);
        OK(cookie_set(&j, &sc, "evil=1; HttpOnly", NOW) == -1);
        OK(cookie_set(&j, &sc, "h=stolen", NOW) == -1);
        OK(strstr(hdr_for(&j, "example.com", "/", 1, 1), "h=2") != NULL);
    }

    /* Name prefixes. __Host- is the only way a cookie name can promise it was
     * not planted by a subdomain. */
    OK(set_from(&j, "example.com", "/", 1, "__Secure-a=1") == -1);           /* no Secure attr */
    OK(set_from(&j, "example.com", "/", 1, "__Secure-a=1; Secure") == 0);
    OK(set_from(&j, "example.com", "/", 1, "__Host-b=1; Secure; Path=/") == 0);
    OK(set_from(&j, "example.com", "/", 1, "__Host-c=1; Secure; Path=/x") == -1);
    OK(set_from(&j, "example.com", "/", 1, "__Host-d=1; Secure; Path=/; Domain=example.com") == -1);
    OK(set_from(&j, "example.com", "/", 0, "__Host-e=1; Secure; Path=/") == -1);

    /* SameSite=None without Secure is rejected, as browsers do. */
    OK(set_from(&j, "example.com", "/", 1, "n=1; SameSite=None") == -1);
    OK(set_from(&j, "example.com", "/", 1, "n=1; SameSite=None; Secure") == 0);
    OK(set_from(&j, "example.com", "/", 1, "l=1; SameSite=Lax") == 0);
    OK(set_from(&j, "example.com", "/", 1, "t=1; SameSite=Strict") == 0);

    cookie_jar_free(&j);
}

static void t_samesite_values(void)
{
    struct cookie_jar j;
    cookie_jar_init(&j);
    set_from(&j, "example.com", "/", 1, "a=1");
    set_from(&j, "example.com", "/", 1, "b=1; SameSite=lax");
    set_from(&j, "example.com", "/", 1, "c=1; SameSite=STRICT");
    set_from(&j, "example.com", "/", 1, "d=1; SameSite=None; Secure");
    set_from(&j, "example.com", "/", 1, "e=1; SameSite=bogus");
    int seen[5] = {0,0,0,0,0};
    for (int i = 0; i < cookie_jar_count(&j); i++) {
        const char *n = j.v[i].name;
        if (!strcmp(n, "a")) seen[0] = (j.v[i].samesite == CK_SS_UNSET);
        if (!strcmp(n, "b")) seen[1] = (j.v[i].samesite == CK_SS_LAX);
        if (!strcmp(n, "c")) seen[2] = (j.v[i].samesite == CK_SS_STRICT);
        if (!strcmp(n, "d")) seen[3] = (j.v[i].samesite == CK_SS_NONE);
        if (!strcmp(n, "e")) seen[4] = (j.v[i].samesite == CK_SS_UNSET);
    }
    OK(seen[0] && seen[1] && seen[2] && seen[3] && seen[4]);
    cookie_jar_free(&j);
}

static void t_expiry(void)
{
    struct cookie_jar j;
    cookie_jar_init(&j);

    /* Session cookie: no Expires, no Max-Age. Never gc'd by time. */
    OK(set_from(&j, "example.com", "/", 0, "sess=1") == 0);
    OK(cookie_jar_gc(&j, NOW + 100000000) == 0);
    OK(strstr(hdr_for(&j, "example.com", "/", 0, 1), "sess=1") != NULL);

    /* Expires in the past deletes -- that is how a logout works. */
    OK(set_from(&j, "example.com", "/", 0, "sess=1; Expires=Thu, 01 Jan 1970 00:00:00 GMT") == 0);
    OK(strstr(hdr_for(&j, "example.com", "/", 0, 1), "sess=1") == NULL);
    OK(cookie_jar_count(&j) == 0);

    /* Max-Age wins over Expires, whichever order they appear in. */
    OK(set_from(&j, "example.com", "/", 0,
                "a=1; Expires=Wed, 21 Oct 2015 07:28:00 GMT; Max-Age=3600") == 0);
    OK(strstr(hdr_for(&j, "example.com", "/", 0, 1), "a=1") != NULL);
    OK(set_from(&j, "example.com", "/", 0,
                "b=1; Max-Age=3600; Expires=Wed, 21 Oct 2015 07:28:00 GMT") == 0);
    OK(strstr(hdr_for(&j, "example.com", "/", 0, 1), "b=1") != NULL);

    /* Max-Age 0 and negative both mean delete now. */
    OK(set_from(&j, "example.com", "/", 0, "a=1; Max-Age=0") == 0);
    OK(strstr(hdr_for(&j, "example.com", "/", 0, 1), "a=1") == NULL);
    OK(set_from(&j, "example.com", "/", 0, "b=1; Max-Age=-1") == 0);
    OK(strstr(hdr_for(&j, "example.com", "/", 0, 1), "b=1") == NULL);

    /* A malformed Max-Age is IGNORED, not clamped -- so the Expires (or the
     * session default) still applies. */
    OK(set_from(&j, "example.com", "/", 0, "c=1; Max-Age=abc") == 0);
    OK(strstr(hdr_for(&j, "example.com", "/", 0, 1), "c=1") != NULL);
    OK(set_from(&j, "example.com", "/", 0, "d=1; Max-Age=") == 0);
    OK(strstr(hdr_for(&j, "example.com", "/", 0, 1), "d=1") != NULL);

    /* An absurd Max-Age saturates instead of overflowing into the past. */
    OK(set_from(&j, "example.com", "/", 0, "e=1; Max-Age=99999999999999999999999") == 0);
    OK(strstr(hdr_for(&j, "example.com", "/", 0, 1), "e=1") != NULL);
    OK(cookie_jar_gc(&j, NOW + 1000000000) >= 0);
    OK(strstr(hdr_for(&j, "example.com", "/", 0, 1), "e=1") != NULL);

    /* Expired cookies are not served even before gc runs. */
    OK(set_from(&j, "example.com", "/", 0, "f=1; Max-Age=10") == 0);
    {
        char buf[512];
        struct cookie_ctx c = CTX("example.com", "/", 0, 1);
        cookie_header(&j, &c, NOW + 20, buf, sizeof buf);
        OK(strstr(buf, "f=1") == NULL);
    }
    OK(cookie_jar_gc(&j, NOW + 20) == 1);

    cookie_jar_free(&j);
}

static void t_malformed(void)
{
    struct cookie_jar j;
    cookie_jar_init(&j);

    /* A Set-Cookie with no '=' is ignored outright (RFC 6265 5.2 step 1). */
    OK(set_from(&j, "example.com", "/", 0, "justaname") == -1);
    OK(set_from(&j, "example.com", "/", 0, "") == -1);
    OK(set_from(&j, "example.com", "/", 0, ";") == -1);
    OK(set_from(&j, "example.com", "/", 0, "=value") == -1);      /* empty name */
    OK(set_from(&j, "example.com", "/", 0, "   =v") == -1);

    /* CR/LF or a control character in a name or value would later be spliced
     * into a Cookie: request header. Rejected at the door. */
    OK(set_from(&j, "example.com", "/", 0, "a=b\r\nEvil: yes") == -1);
    OK(set_from(&j, "example.com", "/", 0, "a=b\nEvil: yes") == -1);
    OK(set_from(&j, "example.com", "/", 0, "a\r\nX=b") == -1);
    OK(set_from(&j, "example.com", "/", 0, "a=b\x01") == -1);
    OK(set_from(&j, "example.com", "/", 0, "a\x7f=b") == -1);
    OK(cookie_jar_count(&j) == 0);

    /* An empty value is legal (it is how a lot of feature flags are spelled). */
    OK(set_from(&j, "example.com", "/", 0, "empty=") == 0);
    OK(!strcmp(hdr_for(&j, "example.com", "/", 0, 1), "empty="));

    /* Surrounding whitespace is trimmed. */
    OK(set_from(&j, "example.com", "/", 0, "  sp  =  val  ; Path=/") == 0);
    OK(strstr(hdr_for(&j, "example.com", "/", 0, 1), "sp=val") != NULL);

    /* Unknown attributes are ignored, not fatal. */
    OK(set_from(&j, "example.com", "/", 0, "u=1; Priority=High; Partitioned; Weird=;;;") == 0);
    OK(strstr(hdr_for(&j, "example.com", "/", 0, 1), "u=1") != NULL);

    /* Absurd lengths are refused rather than stored. */
    {
        char *big = (char *)malloc(70000);
        int o = sprintf(big, "big=");
        for (int i = 0; i < 60000; i++) big[o++] = 'x';
        big[o] = 0;
        OK(set_from(&j, "example.com", "/", 0, big) == -1);
        free(big);
    }
    /* NULL arguments are refused, not dereferenced. */
    {
        struct cookie_ctx c = CTX("example.com", "/", 0, 1);
        OK(cookie_set(&j, &c, NULL, NOW) == -1);
        OK(cookie_set(NULL, &c, "a=1", NOW) == -1);
        OK(cookie_set(&j, NULL, "a=1", NOW) == -1);
        char buf[8];
        OK(cookie_header(&j, &c, NOW, buf, 0) == -1);
        OK(cookie_header(&j, &c, NOW, NULL, 8) == -1);
    }
    cookie_jar_free(&j);
}

/* The three request kinds against all four SameSite values -- a 3x4 table,
 * written out rather than reduced, because the whole defect this replaced was
 * a rule stated for one caller and applied to every caller.
 *
 * The row that did not exist before is CROSS_SITE_NAV, and it is the ONLY one
 * where Lax and unset part company with Strict. If a future edit collapses it
 * back into CROSS_SITE, the lax/unset cells of that row fail and nothing
 * else does; if it collapses into SAME_SITE, the strict cell fails alone. */
static void t_samesite_request_kinds(void)
{
    struct cookie_jar j;
    cookie_jar_init(&j);
    set_from(&j, "example.com", "/", 1, "u=1");                       /* unset */
    set_from(&j, "example.com", "/", 1, "l=1; SameSite=Lax");
    set_from(&j, "example.com", "/", 1, "s=1; SameSite=Strict");
    set_from(&j, "example.com", "/", 1, "n=1; SameSite=None; Secure");
    struct cookie_ctx c = CTX("example.com", "/", 1, 1);
    char buf[512];

    cookie_header_ex(&j, &c, CK_REQ_SAME_SITE, NOW, buf, (int)sizeof buf);
    OK(strstr(buf, "u=1") && strstr(buf, "l=1") &&
       strstr(buf, "s=1") && strstr(buf, "n=1"));

    cookie_header_ex(&j, &c, CK_REQ_CROSS_SITE, NOW, buf, (int)sizeof buf);
    OK(!strstr(buf, "u=1") && !strstr(buf, "l=1") &&
       !strstr(buf, "s=1") &&  strstr(buf, "n=1"));

    cookie_header_ex(&j, &c, CK_REQ_CROSS_SITE_NAV, NOW, buf, (int)sizeof buf);
    OK(strstr(buf, "u=1") && strstr(buf, "l=1") && strstr(buf, "n=1"));
    OK(!strstr(buf, "s=1"));      /* the one Lax does not relax */

    cookie_jar_free(&j);
}

/* Eviction must not prefer the cookie the flag exists to protect.
 *
 * THE MEASUREMENT THIS ENCODES, taken before the fix: a page that reads
 * document.cookie and writes fresh names evicted the HttpOnly session after 49
 * writes and its own ordinary cookie after 50 -- the protected one FIRST. The
 * cause is not in evict_lru alone; it is that a script read refreshes
 * `accessed` on exactly the cookies a script can see, so HttpOnly sinks by
 * construction.
 *
 * THE CLOCK HAS TO ADVANCE HERE and every other test in this file passes the
 * constant NOW. With one timestamp for everything, `accessed` never separates
 * anything, evict_lru falls through to its lowest-index tie-break, and the
 * test would still go green for a reason that has nothing to do with the
 * defect -- a green built on the SLOT ORDER of the two cookies. The drift is
 * the whole mechanism, so the drift is what is reproduced. */
static void t_evict_prefers_visible(void)
{
    struct cookie_jar j;
    cookie_jar_init(&j);
    struct cookie_ctx net = CTX("example.com", "/", 0, 1);   /* HttpOnly allowed */
    struct cookie_ctx scr = CTX("example.com", "/", 0, 0);   /* a page: not */
    char buf[2048];
    int64_t t = NOW;
    cookie_set(&j, &net, "SESS=secret; HttpOnly", t);
    cookie_set(&j, &net, "pub=1", t);
    for (int i = 0; i < 80; i++) {
        char sc[64];
        snprintf(sc, sizeof sc, "f%d=x", i);
        cookie_set(&j, &scr, sc, ++t);
        cookie_header(&j, &scr, ++t, buf, (int)sizeof buf);   /* renews what it sees */
    }
    /* The flood ran well past max_per_domain, so most of it is gone too -- the
     * claim is about WHICH cookie survives, not how many. */
    cookie_header(&j, &net, t, buf, (int)sizeof buf);
    OK(strstr(buf, "SESS=secret") != NULL);
    /* ...against a jar that survived by not evicting at all, which would pass
     * the line above for the wrong reason. The cap really ran: the oldest
     * flood cookie is gone and the jar is at its per-domain limit.
     *
     * `pub` is NOT asserted either way, and the reason is worth the line: one
     * script read renews every cookie that read can see, so pub and the whole
     * live flood share a single `accessed` and the choice among them falls
     * through to evict_lru's lowest-index tie-break -- i.e. to slot order,
     * which is insertion order. pub is the oldest slot and loses. That is
     * FIFO-among-equals, not a defect, but it is also not the property this
     * test is for, and pinning it here would pin a tie-break. */
    OK(strstr(buf, "f0=x") == NULL);
    OK(cookie_jar_count(&j) <= 50);
    cookie_jar_free(&j);
}

/* ...and the preference must not become a refusal: a domain holding nothing
 * but HttpOnly cookies still has to accept a new one, or the cap is a leak
 * that fails closed instead of an eviction. Pass 2 of evict_lru is what this
 * watches; deleting it turns this into an infinite... no, into a jar that
 * silently stops storing, which is why the assertion is on the NEW name. */
static void t_evict_falls_back_to_httponly(void)
{
    struct cookie_jar j;
    cookie_jar_init(&j);
    for (int i = 0; i < 60; i++) {
        char sc[64];
        snprintf(sc, sizeof sc, "h%d=x; HttpOnly", i);
        set_from(&j, "example.com", "/", 0, sc);
    }
    set_from(&j, "example.com", "/", 0, "last=1; HttpOnly");
    OK(strstr(hdr_for(&j, "example.com", "/", 0, 1), "last=1") != NULL);
    cookie_jar_free(&j);
}

static void t_limits(void)
{
    struct cookie_jar j;
    cookie_jar_init(&j);
    cookie_jar_limits(&j, 20, 5);

    /* Per-domain cap: the 6th cookie evicts, it does not fail. */
    for (int i = 0; i < 12; i++) {
        char sc[64];
        sprintf(sc, "c%d=v%d", i, i);
        OK(set_from(&j, "example.com", "/", 0, sc) == 0);
    }
    OK(cookie_jar_count(&j) == 5);

    /* A second origin gets its own budget. */
    for (int i = 0; i < 5; i++) {
        char sc[64];
        sprintf(sc, "d%d=v%d", i, i);
        set_from(&j, "other.com", "/", 0, sc);
    }
    OK(cookie_jar_count(&j) == 10);
    OK(strstr(hdr_for(&j, "other.com", "/", 0, 1), "d4=v4") != NULL);
    OK(!strcmp(hdr_for(&j, "example.com", "/", 0, 1), "c7=v7; c8=v8; c9=v9; c10=v10; c11=v11"));

    /* Global cap. */
    for (int d = 0; d < 10; d++) {
        char host[64];
        sprintf(host, "h%d.test", d);
        for (int i = 0; i < 5; i++) {
            char sc[64];
            sprintf(sc, "e%d=v%d", i, i);
            set_from(&j, host, "/", 0, sc);
        }
    }
    OK(cookie_jar_count(&j) <= 20);
    cookie_jar_free(&j);
}

static void t_ordering_stability(void)
{
    struct cookie_jar j;
    cookie_jar_init(&j);
    struct cookie_ctx c = CTX("example.com", "/a/b", 0, 1);
    /* Equal path length -> creation order, and refreshing a cookie's value
     * must not reshuffle the header (5.3 step 11 keeps the creation time). */
    cookie_set(&j, &c, "first=1; Path=/a", NOW);
    cookie_set(&j, &c, "second=2; Path=/a", NOW + 1);
    char buf[256];
    cookie_header(&j, &c, NOW + 2, buf, sizeof buf);
    OK(!strcmp(buf, "first=1; second=2"));
    cookie_set(&j, &c, "first=updated; Path=/a", NOW + 3);
    cookie_header(&j, &c, NOW + 4, buf, sizeof buf);
    OK(!strcmp(buf, "first=updated; second=2"));
    cookie_jar_free(&j);
}

static void t_header_buffer(void)
{
    struct cookie_jar j;
    cookie_jar_init(&j);
    set_from(&j, "example.com", "/", 0, "aaaa=1111");
    set_from(&j, "example.com", "/", 0, "bbbb=2222");
    set_from(&j, "example.com", "/", 0, "cccc=3333");
    struct cookie_ctx c = CTX("example.com", "/", 0, 1);
    char small[16];
    int n = cookie_header(&j, &c, NOW, small, (int)sizeof small);
    /* Truncation is by whole cookies: half a "name=val" is a corrupt header. */
    OK(n >= 0 && n < (int)sizeof small && !strcmp(small, "aaaa=1111"));
    /* NOTHING fit. This line used to assert == 0 and so PINNED AS CORRECT the
     * one thing wrong with the contract: 0 also means "the user has no cookies
     * for this request", and out[0] is 0 either way, so no caller could tell a
     * jar that is empty from a jar whose contents would not fit. A page saw
     * document.cookie == "" for a cookie the jar was holding -- while
     * navigator.cookieEnabled, the property that exists to disambiguate
     * exactly that, said cookies work. */
    char tiny[4];
    OK(cookie_header(&j, &c, NOW, tiny, (int)sizeof tiny) == CK_E_NOFIT && tiny[0] == 0);
    /* ...and the other half of the distinction, which is what makes the line
     * above a distinction rather than a rename: a request no cookie applies to
     * still answers 0. */
    struct cookie_ctx other = CTX("nothing-here.example", "/", 0, 1);
    char room[256];
    OK(cookie_header(&j, &other, NOW, room, (int)sizeof room) == 0 && room[0] == 0);
    cookie_jar_free(&j);
}

/* The cap is ONE number now, and this is the size it has to clear.
 *
 * Both values are measured, not invented: 1100 bytes is a JWT / OIDC id_token
 * / cf_clearance, and the 1039-byte set is a bilibili-shaped one built to
 * realistic name and value sizes. Under the old three-way split -- 4096 for
 * document.cookie, 2048 for fetch, 1024 for the navigation and every
 * subresource -- the first was READABLE FROM SCRIPT while the page load sent
 * no Cookie header at all, and the second dropped exactly one cookie on the
 * wire: the newest, because 5.4 orders by longest path then earliest
 * creation. The newest is the one a WAF just set. */
static void t_header_cap(void)
{
    struct cookie_jar j;
    cookie_jar_init(&j);
    char sc[1400];
    int o = 0;
    o += sprintf(sc + o, "cf_clearance=");
    for (int i = 0; i < 1100; i++) sc[o++] = 'A' + (i % 26);
    sc[o] = 0;
    OK(set_from(&j, "example.com", "/", 0, sc) == 0);
    struct cookie_ctx c = CTX("example.com", "/", 0, 1);
    char out[CK_HEADER_MAX];
    int n = cookie_header(&j, &c, NOW, out, (int)sizeof out);
    OK(n > 1100 && strstr(out, "cf_clearance=") != NULL);
    /* The old transport buffer, for the record: same jar, same call, and the
     * request went out with no Cookie header. */
    char old_transport[1024];
    OK(cookie_header(&j, &c, NOW, old_transport, (int)sizeof old_transport) == CK_E_NOFIT);
    cookie_jar_free(&j);
}

int main(void)
{
    t_domain_match();
    t_path_match();
    t_default_path();
    t_canon_host();
    t_dates();
    t_basic();
    t_domain_attribute();
    t_public_suffix();
    t_ip_host();
    t_paths();
    t_secure_httponly();
    t_samesite_values();
    t_samesite_request_kinds();
    t_evict_prefers_visible();
    t_evict_falls_back_to_httponly();
    t_header_cap();
    t_expiry();
    t_malformed();
    t_limits();
    t_ordering_stability();
    t_header_buffer();

    if (fails) { printf("\n%d FAILURES\n", fails); return 1; }
    printf("\ncookie_test: ALL PASS\n");
    return 0;
}
