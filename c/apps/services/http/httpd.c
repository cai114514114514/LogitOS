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

/* 2026-09-11: supersedes the sequential/no-poll description above. A bounded
 * poll/accept parent owns four forked workers; each HTTPS worker consumes the
 * existing TLS engine with private process state. The real guest gate checks
 * simultaneous clients and exact file bytes, with explicit certificate trust.
 * Responses still close the connection after one request. */
#include "logit.h"
#include "clib.h"
#include "logit_stat.h"
#include "httpd_protocol.h"
#ifdef HTTPD_TLS
#include "https_transport.h"
#else
static int client_read(int fd, void *b, int n) { return sys_read(fd,b,n); }
static int client_write(int fd, const void *b, int n) { return sys_write(fd,b,n); }
static void client_close(int fd) { sys_shutdown(fd,LOGIT_SHUT_WR);sys_close(fd); }
#endif

/* 2026-09-10: still sequential, with bounded nonblocking socket waits.
 * Poll exists now (historical note above predates it). Static GET supports
 * byte ranges, percent-encoded filenames and browser resource MIME types. */

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

/* Write every byte or give up. sys_write on a socket is a short-write
 * interface -- the send ring is 32 KiB and a response can be bigger -- and a
 * loop that ignores that truncates exactly the large files worth serving. */
static int write_all(int fd, const char *buf, int len)
{
    int off = 0;
    unsigned long long deadline = monotonic_ns() + 15000000000ull;
    while (off < len) {
        int n = client_write(fd, buf + off, len - off);
        if (n == LSK_E_AGAIN) {
            if (monotonic_ns() >= deadline) return -1;
            sys_sleep_ms(1); continue;
        }
        if (n <= 0) return -1;
        off += n;
    }
    return 0;
}

/* Refuse symlinks at every component: lexical docroot checks alone do not
 * constrain a filesystem that now supports links. Use a trusted docroot;
 * this ABI has no openat/O_NOFOLLOW for atomic traversal during mutations. */
static int regular_path(char *path)
{
    struct logit_stat st;
    for (int i = 1;; i++) {
        if (path[i] && path[i] != '/') continue;
        char c = path[i]; path[i] = 0;
        int rc = st_lstat(path, &st);
        path[i] = c;
        if (rc < 0 || (st.mode & LST_IFMT) == LST_IFLNK) return 0;
        if (!c) return (st.mode & LST_IFMT) == LST_IFREG;
        if ((st.mode & LST_IFMT) != LST_IFDIR) return 0;
    }
}

static void send_status(int fd, int code, const char *reason, const char *text, int head_only)
{
    int at = 0;
    at = append(g_out, at, "HTTP/1.0 ");
    at = append_n(g_out, at, code);
    at = append(g_out, at, " ");
    at = append(g_out, at, reason);
    at = append(g_out, at, "\r\nContent-Type: text/plain\r\nContent-Length: ");
    at = append_n(g_out, at, c_strlen(text));
    at = append(g_out, at, "\r\nConnection: close\r\n\r\n");
    if (!head_only) at = append(g_out, at, text);
    write_all(fd, g_out, at);
}

/* GET /_stat -- the kernel's own socket counters, as text. Not decoration:
 * this is how a test tells "the backlog limit refused that connection" apart
 * from "the connection vanished", from the outside, over the network. */
static void send_stat(int fd, int head_only)
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
    write_all(fd, g_out, at + (head_only ? 0 : b));
}

/* Serve one accepted connection, then close it. */
static void serve(int fd, const char *root, const struct logit_sockaddr *peer)
{
    /* Read the request head. One read is usually the whole of it, but a
     * request split across segments is normal and not an error, so keep
     * reading until the blank line or the buffer is full. */
    int len = 0, done = 0;
    sys_set_nonblock(fd);
    unsigned long long deadline = monotonic_ns() + 15000000000ull;
    while (len < REQ_MAX - 1) {
        int n = client_read(fd, g_req + len, REQ_MAX - 1 - len);
        if (n == LSK_E_AGAIN) {
            if (monotonic_ns() >= deadline) break;
            sys_sleep_ms(1); continue;
        }
        if (n <= 0) break;
        len += n;
        g_req[len] = 0;
        done = 0;
        for (int i = 3; i < len; i++)
            if (g_req[i - 3] == '\r' && g_req[i - 2] == '\n' &&
                g_req[i - 1] == '\r' && g_req[i] == '\n') { done = 1; break; }
        for (int i = 1; !done && i < len; i++)
            if (g_req[i - 1] == '\n' && g_req[i] == '\n') { done = 1; break; }
        if (done) break;
    }
    g_req[len < 0 ? 0 : len] = 0;
    if (len <= 0) { client_close(fd); return; }

    if (!done) { client_close(fd); return; }

    /* "GET /path HTTP/1.x" -- method and target, nothing else is consulted. */
    if (c_strncmp(g_req, "GET ", 4) != 0 && c_strncmp(g_req, "HEAD ", 5) != 0) {
        send_status(fd, 501, "Not Implemented", "only GET and HEAD\n", 0);
        client_close(fd);
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
    if (g_req[i] != ' ' || !j) {
        send_status(fd, 414, "URI Too Long", "invalid target\n", head_only);
        client_close(fd); return;
    }
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
    log_s(head_only ? " HEAD " : " GET "); log_s(target);

    if (c_streq(target, "/_stat")) {
        send_stat(fd, head_only);
        log_s(" -> 200 (stat)\n");
        client_close(fd);
        return;
    }

    if (safe_path(root, target, g_path, (int)sizeof g_path) != 0) {
        send_status(fd, 400, "Bad Request", "bad path\n", head_only);
        log_s(" -> 400\n");
        client_close(fd);
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
    int ffd = regular_path(g_path) ? sys_open(g_path, O_RDONLY) : -1;
    if (ffd < 0) {
        send_status(fd, 404, "Not Found", "not found\n", head_only);
        log_s(" -> 404 ("); log_s(g_path); log_s(")\n");
        client_close(fd);
        return;
    }
    long size = sys_lseek(ffd, 0, SEEK_END);
    if (size < 0 || sys_lseek(ffd, 0, SEEK_SET) != 0) {
        sys_close(ffd);
        send_status(fd, 500, "Internal Server Error", "cannot seek\n", head_only);
        log_s(" -> 500 (seek)\n");
        client_close(fd);
        return;
    }

    long first = 0, count = size;
    char range[128], if_range[128];
    /* HEAD ignores Range; without validators, If-Range always gets full GET. */
    int partial = !head_only && hd_header(g_req, "Range", range, sizeof range) == 1 &&
                  !hd_header(g_req, "If-Range", if_range, sizeof if_range)
                ? hd_range(range, size, &first, &count) : 0;
    int at = 0;
    if (partial < 0) {
        at = append(g_out, at, "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */");
        at = append_n(g_out, at, size);
        at = append(g_out, at, "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        write_all(fd, g_out, at); sys_close(ffd); client_close(fd);
        log_s(" -> 416\n"); return;
    }
    if (sys_lseek(ffd, first, SEEK_SET) != first) { sys_close(ffd); client_close(fd); return; }
    at = append(g_out, at, partial ? "HTTP/1.1 206 Partial Content" : "HTTP/1.1 200 OK");
    at = append(g_out, at, "\r\nContent-Type: ");
    at = append(g_out, at, mime_of(g_path));
    at = append(g_out, at, "\r\nAccept-Ranges: bytes\r\nX-Content-Type-Options: nosniff\r\nContent-Length: ");
    at = append_n(g_out, at, count);
    if (partial) {
        at = append(g_out, at, "\r\nContent-Range: bytes ");
        at = append_n(g_out, at, first); at = append(g_out, at, "-");
        at = append_n(g_out, at, first + count - 1); at = append(g_out, at, "/");
        at = append_n(g_out, at, size);
    }
    at = append(g_out, at, "\r\nConnection: close\r\n\r\n");
    int rc = write_all(fd, g_out, at);
    long sent = 0;
    while (rc == 0 && !head_only && sent < count) {
        int want = count - sent > BODY_MAX ? BODY_MAX : (int)(count - sent);
        int r = sys_read(ffd, g_body, want);
        if (r <= 0 || write_all(fd, g_body, r) != 0) { rc = -1; break; }
        sent += r;
    }
    sys_close(ffd);
    log_s(partial ? " -> 206 " : " -> 200 ");
    if (rc != 0) log_s("(SHORT) ");
    log_n(head_only ? count : sent);
    log_s(" of "); log_n(count); log_s(" bytes\n");

    /* Half-close before closing: the FIN says "the body ends here" while the
     * connection stays up long enough for the peer to finish. */
    client_close(fd);
}

int main(int argc, char **argv)
{
#ifdef HTTPD_TLS
    int port = argc > 1 ? c_atoi(argv[1]) : 8443;
    const char *name = argc > 3 ? argv[3] : "localhost";
    const char *prefix = argc > 4 ? argv[4] : "/etc/httpsd";
    if (https_init(prefix,name)<0) { errs("httpsd: identity unavailable, mismatched, or not private\n");return 1; }
#else
    int port = argc > 1 ? c_atoi(argv[1]) : 8080;
#endif
    const char *root = argc > 2 ? argv[2] : "/www";
#ifdef HTTPD_TLS
    long maxreq = 0;
#else
    long maxreq = argc > 3 ? c_atoi(argv[3]) : 0;
#endif
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

#ifdef HTTPD_TLS
    log_s("HTTPSD_READY port="); log_n(me.port);
#else
    log_s("HTTPD_READY port="); log_n(me.port);
#endif
    log_s(" root="); log_s(root);
    log_s(" listeners="); log_n(sys_sockstat(SOCKSTAT_LISTENERS));
    log_s("\n");

    _sys(SYS_FSYNC, 2, 0, 0); /* make startup diagnostics visible in the boot service log */
    /* Four forked workers give each request/TLS session private buffers and
     * credentials. The listener polls with a bounded wait to reap children;
     * no worker inherits a different connection's descriptor. */
    long served = 0;
    int workers[4] = {0};
    sys_set_nonblock(lfd);
    for (;;) {
        int slot = -1, active = 0;
        for (int i=0;i<4;i++) {
            if (workers[i] && _sys(SYS_WAITPID,workers[i],0,1)!=0) workers[i]=0;
            if (workers[i]) active++; else slot=i;
        }
        if (maxreq && served>=maxreq) { if(!active)break;sys_sleep_ms(10);continue; }
        if (slot<0) { sys_sleep_ms(10);continue; }
        struct logit_pollfd ready={lfd,LPOLLIN,0};
        if (_sys(SYS_POLL,(long)&ready,1,100)<=0) continue;
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
        int pid=sys_fork();
        if (pid==0) {
            sys_close(lfd);
#ifdef HTTPD_TLS
            if (https_accept(cfd)<0) { client_close(cfd);app_exit(1); }
#endif
            serve(cfd,root,&peer);app_exit(0);
        }
        sys_close(cfd);
        if(pid<0) { log_s("httpd: worker creation failed\n");continue; }
        workers[slot]=pid;served++;
    }

    log_s("HTTPD_DONE served="); log_n(served);
    log_s(" accepted="); log_n(sys_sockstat(SOCKSTAT_ACCEPTED));
    log_s(" refused_backlog="); log_n(sys_sockstat(SOCKSTAT_REFUSED_BACKLOG));
    log_s(" refused_slots="); log_n(sys_sockstat(SOCKSTAT_REFUSED_SLOTS));
    log_s("\n");
    sys_close(lfd);
    return 0;
}
