#ifndef _SYSLOG_H
#define _SYSLOG_H
#include <stdarg.h>

/* THIS IS NOW A REAL SYSLOG CLIENT. The paragraph that stood here said "There
 * is no syslogd on LogitOS -- no log daemon, no /dev/log socket -- so this is
 * not a client for one", and described writing to fd 1 instead. Both halves
 * have changed: AF_UNIX landed (c/net/core/unix.c) and /bin/syslogd binds
 * /dev/log (c/apps/coreutils/syslogd.c), so syslog() connects a SOCK_DGRAM
 * socket to that path and sends each message as one record.
 *
 * The old sink was actively wrong, not merely limited: fd 1 is the program's
 * STDOUT, so a log line from inside a pipeline became part of the pipeline's
 * data. The fallback when no daemon is running is fd 2 now -- the descriptor
 * that is for diagnostics and the one a shell leaves on the console when it
 * redirects stdout.
 *
 * Still not done, and still not pretended: messages are not filed by facility,
 * not rotated, and not forwarded anywhere. See the daemon for why each of those
 * is absent rather than stubbed. */

#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7

#define LOG_PRIMASK 0x07
#define LOG_PRI(p)  ((p) & LOG_PRIMASK)

/* facility codes: accepted, recorded in the LOG_MAKEPRI encoding, and NOT
 * otherwise acted on -- there is one log stream, not one per facility. */
#define LOG_KERN     (0<<3)
#define LOG_USER     (1<<3)
#define LOG_MAIL     (2<<3)
#define LOG_DAEMON   (3<<3)
#define LOG_AUTH     (4<<3)
#define LOG_SYSLOG   (5<<3)
#define LOG_LPR      (6<<3)
#define LOG_NEWS     (7<<3)
#define LOG_UUCP     (8<<3)
#define LOG_CRON     (9<<3)
#define LOG_LOCAL0   (16<<3)
#define LOG_LOCAL1   (17<<3)
#define LOG_LOCAL2   (18<<3)
#define LOG_LOCAL3   (19<<3)
#define LOG_LOCAL4   (20<<3)
#define LOG_LOCAL5   (21<<3)
#define LOG_LOCAL6   (22<<3)
#define LOG_LOCAL7   (23<<3)
#define LOG_MAKEPRI(fac, pri) ((fac) | (pri))

/* openlog() options. THREE are real now, and which three matters:
 *   LOG_PID     tags each line with getpid(), as it always did.
 *   LOG_PERROR  writes the message to stderr AS WELL AS to the daemon. When
 *               there is no daemon it does NOT double the line -- the fallback
 *               already goes to stderr, and printing twice on exactly the
 *               machines that have no daemon is the bug this note exists to
 *               prevent.
 *   LOG_NDELAY  connects at openlog() instead of at the first message. The one
 *               option here that is about the socket, and it only became
 *               meaningful when there was a socket.
 * LOG_CONS, LOG_ODELAY and LOG_NOWAIT are accepted and do nothing: ODELAY is
 * the default behaviour, and the other two describe failure handling this
 * client does not have a second path for. */
#define LOG_PID    0x01
#define LOG_CONS   0x02
#define LOG_ODELAY 0x04
#define LOG_NDELAY 0x08
#define LOG_NOWAIT 0x10
#define LOG_PERROR 0x20

void openlog(const char *ident, int option, int facility);
void syslog(int priority, const char *format, ...);
void vsyslog(int priority, const char *format, va_list ap);
void closelog(void);
int  setlogmask(int mask);

#endif /* _SYSLOG_H */
