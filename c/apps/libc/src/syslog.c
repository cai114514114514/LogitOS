/* <syslog.h>. See the header for what is real here (everything writes; there
 * is just one sink and no daemon behind it). */
#include <syslog.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char ident_buf[64];
static int  opt;
static int  mask = 0xff;    /* LOG_UPTO(LOG_DEBUG): nothing filtered by default */

void openlog(const char *ident, int option, int facility)
{
    (void)facility;   /* recorded per-call via LOG_MAKEPRI, not here */
    if (ident) { strlcpy(ident_buf, ident, sizeof ident_buf); }
    else ident_buf[0] = 0;
    opt = option;
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
    write(1, line, (size_t)n);
    if (opt & LOG_PERROR) write(2, line, (size_t)n);
}

void syslog(int priority, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vsyslog(priority, format, ap);
    va_end(ap);
}

void closelog(void) { ident_buf[0] = 0; opt = 0; }

int setlogmask(int newmask)
{
    int old = mask;
    if (newmask) mask = newmask;
    return old;
}
