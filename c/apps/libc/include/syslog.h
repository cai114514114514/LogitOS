#ifndef _SYSLOG_H
#define _SYSLOG_H
#include <stdarg.h>

/* There is no syslogd on LogitOS -- no log daemon, no /dev/log socket -- so
 * this is not a client for one. It is the same shape autoconf/configure and
 * daemonizing programs assume, wired to the one log sink this system actually
 * has: SYS_WRITE fd 1, which the kernel fans to both the VGA/GUI console and
 * the serial line (see kprintf in CLAUDE.md's Conventions). A message sent
 * through syslog() really is written, really does appear, and really is
 * prefixed with its priority -- it just is not filed by facility or rotated,
 * because nothing here reads it back. */

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

/* openlog() options: accepted, and LOG_PERROR is the one that is real (every
 * message already goes to fd 1, which the kernel fans to serial too -- see
 * above -- so honouring it costs nothing and NOT honouring the others is
 * still true: LOG_PID really does tag each line, because getpid() works). */
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
