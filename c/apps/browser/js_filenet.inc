/* A `struct webapi_net` that answers GETs out of a directory. HOST ONLY.
 *
 * WHY THIS IS NOT A TEST FIXTURE LIVING IN A TEST FILE. It could be, and it
 * would then be one more thing each harness has to remember to install. The
 * reason it is here is the measurement it unblocks and who is allowed to touch
 * what: the WPT runner never calls js_webapi_set_net(), so EVERY fetch in the
 * corpus fails with "could not open a socket" -- and a large part of WPT is
 * data-driven, meaning the test file's first act is to fetch its own JSON and
 * everything after that is dead. url/url-constructor.any.js is one file, one
 * subtest in the report, and 1,004 URL cases it never got to run. That is not
 * a URL parser scoring 21.7%; it is a URL parser that was never asked.
 *
 * So this is compiled into js_webapi.c's own -DWEBAPI_HOST branch, where the
 * host build already replaces the socket layer, and it is OFF unless the
 * environment says otherwise:
 *
 *     WEBAPI_FILE_ROOT=<dir>     serve GETs from <dir>
 *
 * With the variable unset the default net is the all-NULL one that was here
 * before and every existing host test behaves identically. Any harness that
 * installs its own net (tests/unit/stream_net.h, webapi_test.c) overrides this
 * as it always did -- js_webapi_set_net wins.
 *
 * IT SPEAKS REAL HTTP/1.1 ON PURPOSE. It would be less code to shortcut the
 * fetch state machine and hand back a body. Serving bytes through the same
 * h1_conn parser the network path uses means the corpus exercises the response
 * parser, the header handling, the CORS checks and the streaming sink -- so a
 * bug in any of those still shows up, instead of being bypassed by the very
 * harness meant to find it. The socket is a memory buffer; nothing above it
 * knows that.
 *
 * NOT A SERVER. GET only, no query handling, no ranges, no keep-alive
 * (Connection: close on every response), no directory listings, and paths
 * containing ".." are refused -- see fnet_safe_path.
 */

#include <stdio.h>

#define FNET_MAX      8            /* concurrent connections; WF_MAX is 8 */
#define FNET_REQ_MAX  8192

struct fnet_conn {
    int   used;
    char  req[FNET_REQ_MAX];
    int   reqlen;
    char *resp;                    /* the whole response, headers included */
    int   resplen, sent;
    int   ready;
};

static struct fnet_conn g_fnet[FNET_MAX];
static const char *g_fnet_root;    /* NULL = disabled */

/* Reject anything that could escape the root. A ".." segment is the only real
 * hazard, but a backslash and a NUL are refused too rather than reasoned
 * about: this reads files off the developer's disk on behalf of corpus data. */
static int fnet_safe_path(const char *p)
{
    if (!p || p[0] != '/') return 0;
    for (const char *s = p; *s; s++)
        if (*s == '\\' || (unsigned char)*s < 0x20) return 0;
    for (const char *s = p; *s; s++) {
        if (s[0] != '.' || s[1] != '.') continue;
        int before_ok = (s == p) || s[-1] == '/';
        int after_ok  = (s[2] == 0) || s[2] == '/';
        if (before_ok && after_ok) return 0;
    }
    return 1;
}

static const char *fnet_ctype(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".json")) return "application/json";
    if (!strcmp(dot, ".js"))   return "text/javascript";
    if (!strcmp(dot, ".html") || !strcmp(dot, ".htm")) return "text/html";
    if (!strcmp(dot, ".txt"))  return "text/plain";
    if (!strcmp(dot, ".css"))  return "text/css";
    if (!strcmp(dot, ".xml"))  return "application/xml";
    return "application/octet-stream";
}

/* Build the whole response into c->resp. Status is 200 or 404; a 404 still
 * carries a body so that a test which reads response.text() on a miss sees
 * something rather than hanging. */
static void fnet_serve(struct fnet_conn *c)
{
    char path[1024];
    const char *p = c->req, *e;

    c->ready = 1;
    if (strncmp(p, "GET ", 4) != 0) { p = 0; }
    else { p += 4; }

    int status = 404;
    char *body = 0;
    long blen = 0;
    const char *ctype = "text/plain";

    if (p && (e = strchr(p, ' ')) != 0 && (size_t)(e - p) < sizeof path) {
        int n = (int)(e - p);
        memcpy(path, p, (size_t)n);
        path[n] = 0;
        char *q = strchr(path, '?');            /* the query is not a file name */
        if (q) *q = 0;
        char *h = strchr(path, '#');
        if (h) *h = 0;
        if (fnet_safe_path(path)) {
            char full[2048];
            snprintf(full, sizeof full, "%s%s", g_fnet_root, path);
            FILE *f = fopen(full, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                blen = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (blen >= 0 && blen < (32 << 20)) {
                    body = (char *)malloc((size_t)blen + 1);
                    if (body && fread(body, 1, (size_t)blen, f) == (size_t)blen) {
                        body[blen] = 0;
                        status = 200;
                        ctype = fnet_ctype(path);
                    } else { free(body); body = 0; blen = 0; }
                }
                fclose(f);
            }
        }
    }

    if (status != 200) { body = 0; blen = 0; }
    const char *nf = "not found";
    const char *bp = body ? body : nf;
    long bl = body ? blen : (long)strlen(nf);

    /* Access-Control-Allow-Origin: * because the runner's document origin is
     * http://web-platform.test:8000 and a corpus fetch of its own resources is
     * same-origin only by accident of that host name matching. Letting the
     * CORS check run and pass is the point -- see the file header. */
    char head[512];
    int hn = snprintf(head, sizeof head,
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %ld\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      status, status == 200 ? "OK" : "Not Found", ctype, bl);

    c->resp = (char *)malloc((size_t)hn + (size_t)bl);
    if (!c->resp) { free(body); c->resplen = 0; return; }
    memcpy(c->resp, head, (size_t)hn);
    memcpy(c->resp + hn, bp, (size_t)bl);
    c->resplen = hn + (int)bl;
    c->sent = 0;
    free(body);
}

static int fnet_open(const char *host, int port, int tls)
{
    (void)host; (void)port; (void)tls;
    if (!g_fnet_root) return -1;
    for (int i = 0; i < FNET_MAX; i++)
        if (!g_fnet[i].used) {
            memset(&g_fnet[i], 0, sizeof g_fnet[i]);
            g_fnet[i].used = 1;
            return i;
        }
    return -1;
}

static int fnet_poll(int fd)
{
    if (fd < 0 || fd >= FNET_MAX || !g_fnet[fd].used) return SOCK_P_ERROR;
    struct fnet_conn *c = &g_fnet[fd];
    int bits = SOCK_P_CONNECTED | SOCK_P_WRITABLE;
    if (c->ready && c->sent < c->resplen) bits |= SOCK_P_READABLE;
    if (c->ready && c->sent >= c->resplen) bits |= SOCK_P_EOF;
    return bits;
}

static int fnet_send(int fd, const void *buf, int len)
{
    if (fd < 0 || fd >= FNET_MAX || !g_fnet[fd].used) return -1;
    struct fnet_conn *c = &g_fnet[fd];
    if (c->ready) return len;                      /* body after the head: ignored */
    int room = FNET_REQ_MAX - 1 - c->reqlen;
    int n = len < room ? len : room;
    if (n > 0) { memcpy(c->req + c->reqlen, buf, (size_t)n); c->reqlen += n; c->req[c->reqlen] = 0; }
    if (strstr(c->req, "\r\n\r\n")) fnet_serve(c);
    return len;                                    /* always fully accepted */
}

static int fnet_recv(int fd, void *buf, int max)
{
    if (fd < 0 || fd >= FNET_MAX || !g_fnet[fd].used) return -1;
    struct fnet_conn *c = &g_fnet[fd];
    if (!c->ready) return 0;
    int left = c->resplen - c->sent;
    if (left <= 0) return -1;                      /* stream finished */
    int n = left < max ? left : max;
    memcpy(buf, c->resp + c->sent, (size_t)n);
    c->sent += n;
    return n;
}

static void fnet_close(int fd)
{
    if (fd < 0 || fd >= FNET_MAX) return;
    free(g_fnet[fd].resp);
    memset(&g_fnet[fd], 0, sizeof g_fnet[fd]);
}

/* A monotonic clock that ADVANCES ON EVERY CALL. The host build had no clock
 * at all (now_ms returned 0), which is fine until something waits: fetch arms
 * a deadline of now + WF_TIMEOUT and a clock frozen at zero means a request
 * that goes wrong waits for ever instead of failing. One millisecond per poll
 * is not a real clock, but it is monotonic and it bounds every timeout. */
static unsigned long long g_fnet_clock;
static unsigned long long fnet_now_ms(void) { return ++g_fnet_clock; }

static const struct webapi_net g_fnet_net = {
    fnet_open, fnet_poll, fnet_send, fnet_recv, fnet_close, fnet_now_ms, 0
};
