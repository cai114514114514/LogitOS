/* /bin/syslogd -- the consumer AF_UNIX was built for.
 *
 * WHY THIS PROGRAM AND NOT A TEST. An op nothing calls is an op with no
 * reference: c/net/core/unix.c could pass every check in tests/unit/unix_test.c
 * and still be a mechanism whose only user is its own gate, which is the state
 * CLAUDE.md names about SYS_MMAP_FILE ("a mechanism whose sole consumer is its
 * own check"). So the family needed a real user, and a syslog daemon is the
 * cheapest HONEST one -- not because logging is glamorous, but because it is
 * the exact shape AF_UNIX exists for and NOTHING ELSE IN THIS TREE CAN DO IT:
 *
 *   - the writers are UNRELATED PROCESSES that start LATER. A pipe reaches only
 *     descendants that inherited it; there is no way for a program the shell
 *     launches five minutes from now to find one.
 *   - they must find the sink BY NAME. That is what bind() to a path is.
 *   - a writer dying must not disturb the daemon, and the daemon dying must not
 *     kill the writers with SIGPIPE. A datagram socket gives both: senders come
 *     and go and the inbox stays open, and a send to a daemon that is gone
 *     returns ECONNREFUSED instead of raising a signal.
 *   - MESSAGE BOUNDARIES. Two programs logging at once through a byte stream
 *     interleave into one unreadable line. Every line here arrives as its own
 *     record because SOCK_DGRAM keeps the length, which a pipe cannot.
 *
 * The writer side is c/apps/libc/src/syslog.c: openlog()/syslog() connect to
 * this path and send(). So the consumer is not this program alone -- it is
 * every mini-libc program that calls syslog(), which is the point.
 *
 * WHAT IT DOES NOT DO, deliberately: no facility routing, no /etc/syslog.conf,
 * no network forwarding, no rotation. Each of those is a policy with an obvious
 * right answer only once somebody needs it, and inventing them now would be
 * code with no reference. It receives, timestamps, prints and appends. */

#include "clib.h"

/* Both names come from include/abi/logit_abi.h, which is the ONLY header the
 * writer (mini-libc's syslog.c) and this program both see -- see the comment
 * on LOGIT_PATH_LOG there for why it is not /dev/log, which is where this
 * program looked until it was run on the machine and refused to bind. */
#define LOGPATH  LOGIT_PATH_LOG
#define LOGFILE  LOGIT_PATH_LOG_DIR "/messages"
#define MSGMAX   1024

/* One record, rendered with a timestamp. Kept out of the receive loop so the
 * "where does it go" decision is in one place. */
static void emit(int outfd, int filefd, const char *msg, int n)
{
    char stamp[32];
    /* Deliberately MONOTONIC seconds since boot and not the wall clock.
     * get_time() is the RTC, which logit.h says outright "can be set backwards"
     * -- a log whose timestamps jump backwards while you are reading a boot is
     * worse than one that counts from zero, and reading a boot is this
     * machine's whole reason to have a log. Resolution is 10 ms at the source;
     * seconds is all this prints. */
    unsigned long long t = monotonic_ms() / 1000ull;
    int k = 0;
    stamp[k++] = '[';
    char d[20]; int i = 0;
    if (!t) d[i++] = '0';
    while (t) { d[i++] = (char)('0' + t % 10); t /= 10; }
    while (i) stamp[k++] = d[--i];
    stamp[k++] = ']';
    stamp[k++] = ' ';

    sys_write(outfd, stamp, k);
    sys_write(outfd, msg, n);
    if (msg[n - 1] != '\n') sys_write(outfd, "\n", 1);

    if (filefd >= 0) {
        sys_write(filefd, stamp, k);
        sys_write(filefd, msg, n);
        if (msg[n - 1] != '\n') sys_write(filefd, "\n", 1);
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    int fd = sys_socket(LOGIT_AF_UNIX, LOGIT_SOCK_DGRAM, 0);
    if (fd < 0) {
        errs("syslogd: socket failed: "); outn_fd(2, fd); errs("\n");
        return 1;
    }

    /* The socket's directory is not on the mkfs image, so make it -- outermost
     * first, because SYS_MKDIR creates one level. Both results are IGNORED on
     * purpose: "already exists" and "created" are the same outcome to this
     * program, and a real failure is not worth a second error path because the
     * bind below reports it with the credential that actually mattered. Doing
     * it here rather than in the writer is deliberate: a writer only connect()s
     * and must never create anything, or two programs racing to log would each
     * be able to manufacture the directory the daemon is supposed to own. */
    (void)make_dir(LOGIT_PATH_LOG_DIR0);
    (void)make_dir(LOGIT_PATH_LOG_DIR);

    struct logit_sockaddr_un a;
    if (!sockaddr_un_set(&a, LOGPATH)) {
        errs("syslogd: path too long\n");
        return 1;
    }
    int rc = sys_bind_unix(fd, &a);
    if (rc < 0) {
        /* LSK_E_INUSE means another syslogd already holds the name. Said by
         * name rather than as a generic failure, because "there is already one
         * running" is the single most likely reason this program fails to
         * start and the operator's response to it is different from every
         * other error's. */
        if (rc == LSK_E_INUSE) errs("syslogd: " LOGPATH " is already bound\n");
        else { errs("syslogd: bind failed: "); outn_fd(2, rc); errs("\n"); }
        sys_close(fd);
        return 1;
    }

    /* The file sink is OPTIONAL. A machine whose /var does not exist yet should
     * still get its log on the console rather than refusing to run -- the
     * console is the sink that always works, and losing the file is a
     * degradation, not a failure. */
    int lf = sys_open(LOGFILE, O_WRONLY | O_CREAT | O_APPEND);
    if (lf < 0) errs("syslogd: " LOGFILE " unavailable; console only\n");

    outs("syslogd: listening on " LOGPATH "\n");

    char msg[MSGMAX];
    for (;;) {
        /* ONE read() PER MESSAGE. This is a datagram socket, so a read returns
         * exactly one record however many are queued -- which is why two
         * programs logging at the same moment cannot interleave into one line.
         * A record longer than this buffer is TRUNCATED by the kernel and its
         * remainder discarded, which is what SOCK_DGRAM specifies; the writer
         * side caps a line at 512 bytes, so it cannot happen from syslog(). */
        int n = sys_read(fd, msg, sizeof msg);
        if (n == 0) continue;          /* a zero-length record is not EOF here:
                                        * a bound datagram socket has no peer
                                        * whose departure could end it */
        if (n < 0) {
            if (n == EAGAIN_RC) continue;       /* only if somebody set O_NONBLOCK */
            break;                              /* the descriptor is gone */
        }
        emit(1, lf, msg, n);
    }

    if (lf >= 0) sys_close(lf);
    sys_close(fd);
    return 0;
}
