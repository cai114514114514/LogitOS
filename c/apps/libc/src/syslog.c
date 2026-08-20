/* <syslog.h>.
 *
 * WHAT CHANGED. The header used to say "everything writes; there is just one
 * sink and no daemon behind it", and vsyslog() ended in `write(1, line, n)`.
 * That is worse than it sounds: FD 1 IS THE PROGRAM'S STDOUT. A log line from
 * inside a pipeline landed in the pipeline's data, so `ls | wc -l` counted a
 * log message, and a program whose stdout was a file got its log written into
 * the file. There was nothing else to send it to.
 *
 * There is now: an AF_UNIX datagram socket to LOGIT_PATH_LOG (/var/log/sock -- NOT the
 * BSD /dev/log, which vfsctl.c synthesises and which cannot hold a binding
 * on this machine; see LOGIT_PATH_LOG in include/abi/logit_abi.h), with
 * /bin/syslogd on the other end
 * (c/apps/coreutils/syslogd.c). This is the WRITER side of the AF_UNIX
 * consumer -- the family's user is not one daemon but every program in this
 * tree that calls syslog().
 *
 * WHY DATAGRAM AND NOT STREAM, since both now exist: a stream would interleave
 * two programs' half-lines into one, and would raise SIGPIPE at the writer when
 * the daemon restarted. A datagram keeps each line whole (the kernel stores the
 * length beside the bytes) and a send to a daemon that is gone comes back as
 * ECONNREFUSED, which is recoverable, rather than as a signal whose default
 * action is to kill the caller.
 *
 * THE FALLBACK IS FD 2, NOT FD 1. If there is no daemon the line still has to
 * go somewhere, and stderr is the descriptor that is FOR diagnostics -- it is
 * the one a shell leaves pointed at the console when it redirects stdout, which
 * is exactly the case the old code got wrong. */
#include <syslog.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

/* NOT a literal, and NOT /dev/log. include/abi/logit_abi.h is the one header
 * this file and c/apps/coreutils/syslogd.c both see, so the name is defined
 * once there; /dev on this machine is synthesised by vfsctl.c and cannot hold
 * a binding, which is what made the daemon fail to start when both sides
 * agreed on the BSD name. The full account is on LOGIT_PATH_LOG. */
#include "logit_abi.h"
#define SYSLOG_PATH LOGIT_PATH_LOG

static char ident_buf[64];
static int  opt;
static int  mask = 0xff;    /* LOG_UPTO(LOG_DEBUG): nothing filtered by default */
static int  log_fd = -1;
static int  log_tried;      /* so a machine with no daemon does not pay a failed
                             * socket()+connect() on every single line */

/* Open the connection, or leave log_fd at -1. Lazy rather than at openlog(),
 * because POSIX does not require openlog() to be called at all and the first
 * syslog() must work without it. */
static void log_open(void)
{
    if (log_fd >= 0 || log_tried) return;
    log_tried = 1;
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strlcpy(a.sun_path, SYSLOG_PATH, sizeof a.sun_path);
    if (connect(fd, (struct sockaddr *)&a, (socklen_t)sizeof a) < 0) {
        close(fd);
        return;
    }
    log_fd = fd;
}

void openlog(const char *ident, int option, int facility)
{
    (void)facility;   /* recorded per-call via LOG_MAKEPRI, not here */
    if (ident) { strlcpy(ident_buf, ident, sizeof ident_buf); }
    else ident_buf[0] = 0;
    opt = option;
    /* LOG_NDELAY means "connect now rather than at the first message", and it
     * is the one option here that is about the socket. LOG_ODELAY (the default)
     * is the lazy path above. */
    if (opt & LOG_NDELAY) log_open();
}

static const char *prio_name(int p)
{
    switch (LOG_PRI(p)) {
    case LOG_EMERG:   return "emerg";
    case LOG_ALERT:   return "alert";
    case LOG_CRIT:    return "crit";
    case LOG_ERR:     return "err";
    case LOG_WARNING: return "warning";
    case LOG_NOTICE:  return "notice";
    case LOG_INFO:    return "info";
    default:          return "debug";
    }
}

void vsyslog(int priority, const char *format, va_list ap)
{
    if (!(mask & (1 << LOG_PRI(priority)))) return;   /* setlogmask()'d out */
    char line[512];
    int n = 0;
    if (ident_buf[0]) n += snprintf(line + n, sizeof line - n, "%s", ident_buf);
    if (opt & LOG_PID)  n += snprintf(line + n, sizeof line - n, "[%d]", getpid());
    if (n > 0 && n < (int)sizeof line) n += snprintf(line + n, sizeof line - n, ": ");
    n += snprintf(line + n, sizeof line - n, "<%s> ", prio_name(priority));
    if (n < (int)sizeof line) n += vsnprintf(line + n, sizeof line - n, format, ap);
    if (n >= (int)sizeof line) n = (int)sizeof line - 1;
    if (n <= 0 || line[n - 1] != '\n') { if (n < (int)sizeof line - 1) line[n++] = '\n'; }

    log_open();
    if (log_fd >= 0) {
        if (send(log_fd, line, (size_t)n, 0) >= 0) {
            if (opt & LOG_PERROR) write(2, line, (size_t)n);
            return;
        }
        /* ECONNREFUSED here means the daemon that was there when we connected
         * has gone. RECONNECT ONCE and retry -- glibc does the same, and it is
         * what makes restarting syslogd not silently deafen every program that
         * was already running. The kernel re-resolves an AF_UNIX datagram
         * destination by NAME on every send (c/net/core/unix.c by_name), so a
         * restarted daemon that rebound the same path is reachable again
         * without this; the reconnect covers the case where our descriptor
         * itself is the problem. */
        if (errno == ECONNREFUSED) {
            close(log_fd);
            log_fd = -1;
            log_tried = 0;
            log_open();
            if (log_fd >= 0 && send(log_fd, line, (size_t)n, 0) >= 0) {
                if (opt & LOG_PERROR) write(2, line, (size_t)n);
                return;
            }
        }
    }
    /* No daemon. The line still has to be readable by somebody, so it goes to
     * stderr -- and NOT also a second time when LOG_PERROR is set, which would
     * print every message twice on exactly the machines that have no daemon. */
    write(2, line, (size_t)n);
}

void syslog(int priority, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vsyslog(priority, format, ap);
    va_end(ap);
}

void closelog(void)
{
    if (log_fd >= 0) close(log_fd);
    log_fd = -1;
    log_tried = 0;
    ident_buf[0] = 0;
    opt = 0;
}

int setlogmask(int newmask)
{
    int old = mask;
    if (newmask) mask = newmask;
    return old;
}
