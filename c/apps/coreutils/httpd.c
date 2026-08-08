/* /bin/httpd -- the demonstration that this machine can answer a connection.
 *
 * WHY A WEB SERVER AND NOT A "hello" ECHO. An echo proves a socket accepted
 * something. A web server proves the thing on the other end -- curl, a browser,
 * python's urllib -- accepts what came back, which is the only definition of
 * "serving" that means anything. It is also what the WPT runner needs: that
 * corpus is currently fed to the browser off the local disk because nothing
 * here could serve it over HTTP, and a machine that can listen is how that
 * stops being a simulation.
 *
 * WHAT IT IS: HTTP/1.0, one request per connection, served SEQUENTIALLY --
 * accept, read the request, write the response, close, accept again.
 *
 * WHY SEQUENTIALLY, said plainly rather than dressed up as a design. There is
 * no select/poll/epoll in this ABI, so a single thread cannot wait on several
 * connections at once; the alternative is a thread per connection, and the
 * ceiling on that is about thirteen threads per process (see
 * LOGIT_THREADS_MAX in logit_abi.h -- the binding limit is VMA_MAXAREA, not
 * the thread table). A sequential server is the honest first version: it is
 * not slow because of the network, it is serial because the machine has no way
 * to wait on two descriptors. Concurrency here is not a missing feature of
 * this program, it is a missing primitive underneath it.
 *
 * IT IS STILL NOT A ONE-CONNECTION TOY. The listen backlog means a second
 * client that arrives mid-response is queued by the KERNEL and served next
 * rather than refused -- so two simultaneous clients both get their own
 * answer, which is exactly what the passive-open suite's negative control
 * exists to prove the kernel really does.
 *
 * usage: httpd [port] [docroot] [max_requests]
 *   port          default 8080
 *   docroot       default /www  (a path prefix, joined to the request path)
 *   max_requests  0 = serve forever; a number makes it exit, which is what a
 *                 boot harness needs so the test can end.
 */

#include "logit.h"
#include "clib.h"

#define REQ_MAX  2048
#define BODY_MAX 8192    /* a streaming chunk, not a file size */

static char  g_req[REQ_MAX];
static char  g_body[BODY_MAX];
static char  g_path[256];
static char  g_out[1024];        /* one response head */

/* Everything this program says goes to fd 2, so it cannot be confused with the
 * bytes it is serving even when both land on the same serial console. */
static void log_s(const char *s) { fputs_fd(2, s); }
static void log_n(long v)        { outn_fd(2, v); }

static int append(char *dst, int at, const char *s)
{
    int i = 0;
    while (s[i]) dst[at + i] = s[i], i++;
    return at + i;
}

static int append_n(char *dst, int at, long v)
{
    char tmp[24];
    int k = 0;
    if (v == 0) tmp[k++] = '0';
    while (v > 0) { tmp[k++] = (char)('0' + (v % 10)); v /= 10; }
    while (k > 0) dst[at++] = tmp[--k];
    return at;
}

/* The MIME type, from the extension. Short and deliberately not clever: a
 * wrong Content-Type makes a browser render markup as text, which looks like a
 * server bug and is one. */
static const char *mime_of(const char *path)
{
    int n = c_strlen(path);
    const char *e = path + n;
    while (e > path && *(e - 1) != '.' && *(e - 1) != '/') e--;
    if (e == path || *(e - 1) != '.') return "application/octet-stream";
    if (c_streq(e, "html") || c_streq(e, "htm")) return "text/html";
    if (c_streq(e, "txt"))  return "text/plain";
    if (c_streq(e, "css"))  return "text/css";
    if (c_streq(e, "js"))   return "application/javascript";
    if (c_streq(e, "json")) return "application/json";
    if (c_streq(e, "png"))  return "image/png";
    if (c_streq(e, "gif"))  return "image/gif";
    if (c_streq(e, "jpg") || c_streq(e, "jpeg")) return "image/jpeg";
    if (c_streq(e, "svg"))  return "image/svg+xml";
    if (c_streq(e, "bin"))  return "application/octet-stream";
    return "text/plain";
}

/* Write every byte or give up. sys_write on a socket is a short-write
 * interface -- the send ring is 32 KiB and a response can be bigger -- and a
 * loop that ignores that truncates exactly the large files worth serving. */
static int write_all(int fd, const char *buf, int len)
{
    int off = 0;
    while (off < len) {
        int n = sys_write(fd, buf + off, len - off);
        if (n <= 0) return -1;
        off += n;
    }
    return 0;
}

/* Resolve a request path against the docroot, refusing anything that tries to
 * leave it. THE CHECK IS ON THE REQUEST TEXT, not on a resolved path, because
 * this filesystem has no realpath: so ".." is refused outright rather than
 * normalised, which rejects a few legal paths and lets through none. */
static int safe_path(const char *root, const char *req, char *out, int max)
{
    if (req[0] != '/') return -1;
    for (int i = 0; req[i]; i++) {
        if (req[i] == '.' && req[i + 1] == '.') return -1;
        if (req[i] == '\\') return -1;
        if ((unsigned char)req[i] < 0x20) return -1;
    }
    int n = 0;
    for (int i = 0; root[i] && n < max - 1; i++) out[n++] = root[i];
    /* Strip EVERY trailing slash, including the last one -- the request path
     * supplies its own leading '/'. A docroot of "/" must become "" and not
     * "/", or every path served from the volume root is "//licenses/..." and
     * the whole server 404s while looking like it works. */
    while (n > 0 && out[n - 1] == '/') n--;
    const char *r = req;
    if (c_streq(req, "/")) r = "/index.html";            /* the directory index */
    for (int i = 0; r[i] && n < max - 1; i++) out[n++] = r[i];
    out[n] = 0;
    return 0;
}

static void send_status(int fd, int code, const char *reason, const char *text)
{
    int at = 0;
    at = append(g_out, at, "HTTP/1.0 ");
    at = append_n(g_out, at, code);
    at = append(g_out, at, " ");
    at = append(g_out, at, reason);
    at = append(g_out, at, "\r\nContent-Type: text/plain\r\nContent-Length: ");
    at = append_n(g_out, at, c_strlen(text));
    at = append(g_out, at, "\r\nConnection: close\r\n\r\n");
    at = append(g_out, at, text);
    write_all(fd, g_out, at);
}

/* GET /_stat -- the kernel's own socket counters, as text. Not decoration:
 * this is how a test tells "the backlog limit refused that connection" apart
 * from "the connection vanished", from the outside, over the network. */
static void send_stat(int fd)
{
    static const struct { int sel; const char *name; } fields[] = {
        { SOCKSTAT_SYN_RECEIVED,    "syn_received"    },
        { SOCKSTAT_ACCEPTED,        "accepted"        },
        { SOCKSTAT_REFUSED_BACKLOG, "refused_backlog" },
        { SOCKSTAT_REFUSED_SLOTS,   "refused_slots"   },
        { SOCKSTAT_REFUSED_NOPORT,  "refused_noport"  },
        { SOCKSTAT_FREE_CONNS,      "free_conns"      },
        { SOCKSTAT_LISTENERS,       "listeners"       },
    };
    int b = 0;
    for (unsigned i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        b = append(g_body, b, fields[i].name);
        b = append(g_body, b, " ");
        b = append_n(g_body, b, sys_sockstat(fields[i].sel));
        b = append(g_body, b, "\n");
    }
    int at = 0;
    at = append(g_out, at, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
                           "Content-Length: ");
    at = append_n(g_out, at, b);
    at = append(g_out, at, "\r\nConnection: close\r\n\r\n");
    for (int i = 0; i < b; i++) g_out[at + i] = g_body[i];
    write_all(fd, g_out, at + b);
}

/* Serve one accepted connection, then close it. */
static void serve(int fd, const char *root, const struct logit_sockaddr *peer)
{
    /* Read the request head. One read is usually the whole of it, but a
     * request split across segments is normal and not an error, so keep
     * reading until the blank line or the buffer is full. */
    int len = 0;
    while (len < REQ_MAX - 1) {
        int n = sys_read(fd, g_req + len, REQ_MAX - 1 - len);
        if (n <= 0) break;
        len += n;
        g_req[len] = 0;
        int done = 0;
        for (int i = 3; i < len; i++)
            if (g_req[i - 3] == '\r' && g_req[i - 2] == '\n' &&
                g_req[i - 1] == '\r' && g_req[i] == '\n') { done = 1; break; }
        for (int i = 1; !done && i < len; i++)
            if (g_req[i - 1] == '\n' && g_req[i] == '\n') { done = 1; break; }
        if (done) break;
    }
    g_req[len < 0 ? 0 : len] = 0;
    if (len <= 0) { sys_close(fd); return; }

    /* "GET /path HTTP/1.x" -- method and target, nothing else is consulted. */
    if (c_strncmp(g_req, "GET ", 4) != 0 && c_strncmp(g_req, "HEAD ", 5) != 0) {
        send_status(fd, 501, "Not Implemented", "only GET and HEAD\n");
        sys_close(fd);
        return;
    }
    int head_only = (g_req[0] == 'H');
    int i = head_only ? 5 : 4;
    int j = 0;
    char target[256];
    while (g_req[i] && g_req[i] != ' ' && g_req[i] != '\r' && g_req[i] != '\n' &&
           j < (int)sizeof target - 1)
        target[j++] = g_req[i++];
    target[j] = 0;
    /* Strip a query string: this server has no dynamic content, and treating
     * "?x=1" as part of the filename turns every such request into a 404 that
     * looks like a missing file. */
    for (int k = 0; target[k]; k++) if (target[k] == '?') { target[k] = 0; break; }

    log_s("[httpd] ");
    log_n((peer->addr >> 24) & 255); log_s(".");
    log_n((peer->addr >> 16) & 255); log_s(".");
    log_n((peer->addr >> 8) & 255);  log_s(".");
    log_n(peer->addr & 255);         log_s(":");
    log_n(peer->port);
    log_s(" GET "); log_s(target);

    if (c_streq(target, "/_stat")) {
        send_stat(fd);
        log_s(" -> 200 (stat)\n");
        sys_close(fd);
        return;
    }

    if (safe_path(root, target, g_path, (int)sizeof g_path) != 0) {
        send_status(fd, 400, "Bad Request", "bad path\n");
        log_s(" -> 400\n");
        sys_close(fd);
        return;
    }

    /* THE FILE IS STREAMED THROUGH AN ORDINARY DESCRIPTOR, not slurped.
     *
     * The first version of this used SYS_READ_FILE, which is the whole-file
     * call the GUI apps use -- and it returns -1 from a CLI process, which
     * cost an afternoon to find because NO OTHER COREUTIL CALLS IT: ls, cat
     * and wc all go through sys_open/sys_read, so the path was simply never
     * exercised from a program without a window. Using the ordinary fd path
     * is what the rest of userland does, it streams instead of demanding a
     * buffer as large as the file, and it removes the size cap entirely.
     *
     * Content-Length comes from a seek to the end and back, because HTTP/1.0
     * has no chunked encoding: without a length the only way to delimit a
     * body is to close the connection, and then a truncated transfer is
     * indistinguishable from a complete one to the client. */
    int ffd = sys_open(g_path, O_RDONLY);
    if (ffd < 0) {
        send_status(fd, 404, "Not Found", "not found\n");
        log_s(" -> 404 ("); log_s(g_path); log_s(")\n");
        sys_close(fd);
        return;
    }
    long size = sys_lseek(ffd, 0, SEEK_END);
    if (size < 0 || sys_lseek(ffd, 0, SEEK_SET) != 0) {
        sys_close(ffd);
        send_status(fd, 500, "Internal Server Error", "cannot seek\n");
        log_s(" -> 500 (seek)\n");
        sys_close(fd);
        return;
    }

    int at = 0;
    at = append(g_out, at, "HTTP/1.0 200 OK\r\nContent-Type: ");
    at = append(g_out, at, mime_of(g_path));
    at = append(g_out, at, "\r\nContent-Length: ");
    at = append_n(g_out, at, size);
    at = append(g_out, at, "\r\nConnection: close\r\n\r\n");
    int rc = write_all(fd, g_out, at);
    long sent = 0;
    if (rc == 0 && !head_only) {
        int r;
        while ((r = sys_read(ffd, g_body, (int)sizeof g_body)) > 0) {
            if (write_all(fd, g_body, r) != 0) { rc = -1; break; }
            sent += r;
        }
    }
    sys_close(ffd);
    log_s(rc == 0 ? " -> 200 " : " -> 200 (SHORT) ");
    log_n(head_only ? size : sent);
    log_s(" of "); log_n(size); log_s(" bytes\n");

    /* Half-close before closing: the FIN says "the body ends here" while the
     * connection stays up long enough for the peer to finish. */
    sys_shutdown(fd, LOGIT_SHUT_WR);
    sys_close(fd);
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? c_atoi(argv[1]) : 8080;
    const char *root = argc > 2 ? argv[2] : "/www";
    long maxreq = argc > 3 ? c_atoi(argv[3]) : 0;
    if (port <= 0 || port > 65535) port = 8080;

    int lfd = sys_socket(LOGIT_AF_INET, LOGIT_SOCK_STREAM, 0);
    if (lfd < 0) {
        log_s("HTTPD_FAIL socket "); log_n(lfd); log_s("\n");
        return 1;
    }
    /* Set before bind, as everywhere else -- it is recorded rather than
     * powerful on this stack (see logit_abi.h), and setting it here is what
     * keeps ported source working unchanged. */
    sys_setsockopt(lfd, LOGIT_SOL_SOCKET, LOGIT_SO_REUSEADDR, 1);

    struct logit_sockaddr me;
    sockaddr_set(&me, 0, port);                  /* 0 = any local address */
    int rc = sys_bind(lfd, &me);
    if (rc < 0) {
        log_s("HTTPD_FAIL bind "); log_n(rc); log_s("\n");
        return 1;
    }
    rc = sys_listen(lfd, 8);
    if (rc < 0) {
        log_s("HTTPD_FAIL listen "); log_n(rc); log_s("\n");
        return 1;
    }
    sys_getsockname(lfd, &me);                   /* what we actually got */

    log_s("HTTPD_READY port="); log_n(me.port);
    log_s(" root="); log_s(root);
    log_s(" listeners="); log_n(sys_sockstat(SOCKSTAT_LISTENERS));
    log_s("\n");

    long served = 0;
    for (;;) {
        struct logit_sockaddr peer;
        peer.family = 0; peer.port = 0; peer.addr = 0;
        /* Blocking accept: the thread PARKS here. It is off the scheduler's run
         * ring until a handshake completes -- this loop costs nothing while
         * nobody is connecting, which a yield loop would not manage. */
        int cfd = sys_accept(lfd, &peer, 0);
        if (cfd < 0) {
            if (cfd == LSK_E_AGAIN) continue;
            log_s("HTTPD_FAIL accept "); log_n(cfd); log_s("\n");
            break;
        }
        serve(cfd, root, &peer);
        served++;
        if (maxreq && served >= maxreq) break;
    }

    log_s("HTTPD_DONE served="); log_n(served);
    log_s(" accepted="); log_n(sys_sockstat(SOCKSTAT_ACCEPTED));
    log_s(" refused_backlog="); log_n(sys_sockstat(SOCKSTAT_REFUSED_BACKLOG));
    log_s(" refused_slots="); log_n(sys_sockstat(SOCKSTAT_REFUSED_SLOTS));
    log_s("\n");
    sys_close(lfd);
    return 0;
}
