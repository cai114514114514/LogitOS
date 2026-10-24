/* These are transport boundaries, not a substitute WHATWG URL parser suite.
 * In particular a relative query can fit the browser's 700-byte temporary URL
 * while exceeding this parser's 512-byte request target. A success return in
 * that middle interval used to send a different request to the origin. */
#include <stdio.h>
#include <string.h>
#include "url.h"

static int checks, failures;

static void check(int ok, const char *label)
{
    checks++;
    if (!ok) { failures++; printf("FAIL: %s\n", label); }
}

static void fill(char *out, int count, char value)
{
    memset(out, value, (size_t)count);
    out[count] = 0;
}

static void parse_cases(void)
{
    struct url u;
    char input[1800], host[URL_HOST_MAX + 1], path[URL_PATH_MAX + 1];
    check(url_parse("http://example.test/a?x=1", &u) == 0 && !u.https &&
          u.port == 80 && !strcmp(u.host, "example.test") && !strcmp(u.path, "/a?x=1"),
          "HTTP parse retains the complete request query");
    check(url_parse("https://example.test:8443/", &u) == 0 && u.https && u.port == 8443,
          "HTTPS parse retains a non-default port");
    check(url_parse("http://example.test:65535/", &u) == 0 && u.port == 65535,
          "largest allowed port is preserved");
    check(url_parse("https://example.test", &u) == 0 && !strcmp(u.path, "/") && u.port == 443,
          "missing path becomes a slash");
    check(url_parse("http://example.test/a?x=1#not-sent", &u) == 0 && !strcmp(u.path, "/a?x=1"),
          "wire path excludes a fragment after the query");
    check(url_parse("http://example.test#not-sent", &u) == 0 &&
          !strcmp(u.host, "example.test") && !strcmp(u.path, "/"),
          "missing wire path does not put the fragment in the host");
    check(url_parse("http://example.test:8080/#not-sent", &u) == 0 &&
          u.port == 8080 && !strcmp(u.path, "/"),
          "a fragment is excluded with an explicit port too");

    fill(host, URL_HOST_MAX - 1, 'h');
    snprintf(input, sizeof input, "http://%s/p", host);
    check(url_parse(input, &u) == 0 && strlen(u.host) == URL_HOST_MAX - 1 && !strcmp(u.path, "/p"),
          "largest representable host is preserved");
    fill(host, URL_HOST_MAX, 'h');
    snprintf(input, sizeof input, "http://%s/p", host);
    check(url_parse(input, &u) == -1, "capacity: overlong host is refused");
    fill(path, URL_PATH_MAX - 1, 'p'); path[0] = '/';
    snprintf(input, sizeof input, "http://e.test%s#kept-only-by-navigation", path);
    check(url_parse(input, &u) == 0 && !strcmp(u.path, path),
          "largest wire path is preserved without counting its fragment");
    fill(path, URL_PATH_MAX, 'p'); path[0] = '/';
    snprintf(input, sizeof input, "http://e.test%s", path);
    check(url_parse(input, &u) == -1, "capacity: overlong wire path is refused");
    snprintf(input, sizeof input, "http://e.test/p#%0700d", 0);
    check(url_parse(input, &u) == 0 && !strcmp(u.path, "/p"),
          "a long fragment cannot consume the HTTP wire-path capacity");
    check(url_parse("http:///p", &u) == -1, "empty host is refused");
    check(url_parse("ftp://e.test/p", &u) == -1, "unsupported scheme stays unsupported");
    check(url_parse("http://e.test:0/p", &u) == -1 &&
          url_parse("http://e.test:65536/p", &u) == -1 &&
          url_parse("http://e.test:99999999999999999999/p", &u) == -1,
          "invalid ports are refused without arithmetic overflow");
}

static void resolution_cases(void)
{
    struct url base;
    char out[1900], ref[1800], expected[1900], fragment[701];
    url_parse("https://e.test:8443/dir/page?old=1", &base);
    check(url_resolve(&base, "next?x=1#anchor", out, sizeof out) == 0 &&
          !strcmp(out, "https://e.test:8443/dir/next?x=1#anchor"),
          "relative resolution preserves query and navigation fragment");
    check(url_resolve(&base, "/root?x=1#anchor", out, sizeof out) == 0 &&
          !strcmp(out, "https://e.test:8443/root?x=1#anchor"),
          "root-relative resolution preserves query and navigation fragment");
    check(url_resolve(&base, "//other.test:8080/p#anchor", out, sizeof out) == 0 &&
          !strcmp(out, "https://other.test:8080/p#anchor"),
          "protocol-relative resolution inherits only the scheme");
    check(url_resolve(&base, "http://other.test:80/p#anchor", out, sizeof out) == 0 &&
          !strcmp(out, "http://other.test:80/p#anchor"),
          "absolute resolution copies its original spelling and fragment");
    fill(fragment, 700, 'f');
    snprintf(ref, sizeof ref, "http://other.test/p#%s", fragment);
    check(url_resolve(&base, ref, out, sizeof out) == 0 && !strcmp(out, ref),
          "absolute resolution preserves a fragment larger than the wire-path buffer");
    struct url wire;
    check(url_parse(out, &wire) == 0 && !strcmp(wire.path, "/p"),
          "the resolved fragment is absent when parsed for HTTP transmission");

    const char *absolute = "http://other.test/p#anchor";
    int exact = (int)strlen(absolute) + 1;
    check(url_resolve(&base, absolute, out, exact) == 0 && !strcmp(out, absolute),
          "absolute resolution accepts an exact output boundary");
    check(url_resolve(&base, absolute, out, exact - 1) == -1,
          "capacity: absolute resolution refuses a short output");
    check(url_resolve(&base, absolute, out, 1) == -1,
          "capacity: absolute resolution refuses a NUL-only output");
    struct { unsigned char before; char value[1]; unsigned char after; } guarded = { 0xa5, { 'x' }, 0x5a };
    url_resolve(&base, absolute, guarded.value, sizeof guarded.value);
    check(guarded.before == 0xa5 && guarded.value[0] == 0 && guarded.after == 0x5a,
          "NUL-only output writes remain inside the declared capacity");
    const char *protocol = "//other.test/p#anchor";
    snprintf(expected, sizeof expected, "https:%s", protocol);
    exact = (int)strlen(expected) + 1;
    check(url_resolve(&base, protocol, out, exact) == 0 && !strcmp(out, expected),
          "protocol-relative resolution accepts an exact output boundary");
    check(url_resolve(&base, protocol, out, exact - 1) == -1,
          "capacity: protocol-relative resolution refuses a short output");
    const char *relative = "next?x=1#anchor";
    const char *relative_result = "https://e.test:8443/dir/next?x=1#anchor";
    exact = (int)strlen(relative_result) + 1;
    check(url_resolve(&base, relative, out, exact) == 0 && !strcmp(out, relative_result),
          "unparse accepts its exact output boundary including the port");
    check(url_resolve(&base, relative, out, exact - 1) == -1,
          "capacity: relative unparse refuses a short output");
    check(url_resolve(&base, "/p", out, 8) == -1,
          "capacity: root-relative unparse refuses a short output");

    fill(ref, URL_PATH_MAX - 1, 'r'); ref[0] = '/';
    snprintf(expected, sizeof expected, "https://e.test:8443%s", ref);
    check(url_resolve(&base, ref, out, sizeof out) == 0 && !strcmp(out, expected),
          "root-relative resolution accepts the largest raw path");
    fill(ref, URL_PATH_MAX, 'r'); ref[0] = '/';
    check(url_resolve(&base, ref, out, sizeof out) == -1,
          "capacity: root-relative resolution refuses an overlong path");
    fill(ref, URL_PATH_MAX - 1 - 5, 'r');
    snprintf(expected, sizeof expected, "https://e.test:8443/dir/%s", ref);
    check(url_resolve(&base, ref, out, sizeof out) == 0 && !strcmp(out, expected),
          "relative resolution counts its complete base directory");
    fill(ref, URL_PATH_MAX - 5, 'r');
    check(url_resolve(&base, ref, out, sizeof out) == -1,
          "capacity: relative resolution refuses a combined overlong path");
    snprintf(ref, sizeof ref, "submitted?q=%0600d", 0);
    check(url_resolve(&base, ref, out, sizeof out) == -1,
          "capacity: relative query exceeding the wire path is refused");
    fill(ref, URL_PATH_MAX, 'p'); ref[0] = '/';
    snprintf(expected, sizeof expected, "http://e.test%s", ref);
    check(url_resolve(&base, expected, out, sizeof out) == -1,
          "capacity: absolute resolution refuses an overlong wire path");
    snprintf(expected, sizeof expected, "//e.test%s", ref);
    check(url_resolve(&base, expected, out, sizeof out) == -1,
          "capacity: protocol-relative resolution refuses an overlong wire path");
    fill(ref, URL_HOST_MAX, 'h');
    snprintf(expected, sizeof expected, "http://%s/p", ref);
    check(url_resolve(&base, expected, out, sizeof out) == -1,
          "capacity: absolute resolution refuses an overlong host");
    snprintf(expected, sizeof expected, "//%s/p", ref);
    check(url_resolve(&base, expected, out, sizeof out) == -1,
          "capacity: protocol-relative resolution refuses an overlong host");

    out[0] = 'z';
    check(url_resolve(&base, "/p", out, 0) == -1 && out[0] == 'z' &&
          url_resolve(&base, "/p", out, -1) == -1 && out[0] == 'z',
          "invalid output capacities are refused without writes");
    strcpy(out, absolute);
    check(url_resolve(&base, out, out, sizeof out) == 0 && !strcmp(out, absolute),
          "absolute copy preserves an in-place reference");
    url_parse("https://e.test/dir/page", &base);
    check(url_resolve(&base, "next", out, sizeof out) == 0 && !strcmp(out, "https://e.test/dir/next"),
          "unparse omits the default HTTPS port");
    url_parse("http://e.test:65535/dir/page", &base);
    check(url_resolve(&base, "next", out, sizeof out) == 0 && !strcmp(out, "http://e.test:65535/dir/next"),
          "unparse preserves all five digits of the largest port");
}

int main(void)
{
    parse_cases();
    resolution_cases();
    printf("http-url: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
