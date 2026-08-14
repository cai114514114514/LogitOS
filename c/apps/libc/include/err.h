#ifndef _ERR_H
#define _ERR_H

#include <stdarg.h>

/* BSD <err.h>: the eight err/warn functions, plus the program-name state
 * they all print. This is one of the first two headers (with <libgen.h>)
 * almost any ported CLI program includes, because "print a diagnostic with
 * my own name on it and (for err/errx) exit" is the very first thing a
 * command-line program does when something goes wrong.
 *
 * FORMAT, EXACTLY (this is glibc's own behaviour, verified against it host-
 * side -- see the probe notes in err.c and the deviations reported by the
 * unit that added this file):
 *   err/warn:   "<progname>: " [fmt-expanded ": "] "<strerror(errno)>" "\n"
 *   errx/warnx: "<progname>: " [fmt-expanded]                          "\n"
 * The "[fmt-expanded ...]" part is skipped ENTIRELY (not printed as an
 * empty string) when fmt is NULL -- glibc special-cases that, and so does
 * this. Passing "" (empty but non-NULL) is NOT the same as NULL: "" still
 * gets its trailing ": " printed, producing a visible double colon before
 * strerror(). Both behaviours are real glibc output, not an assumption --
 * see the case-by-case probe in err.c's header comment.
 *
 * NO __progname GLOBAL ON THIS SYSTEM, AND WHY THAT IS A REAL GAP, NOT A
 * WORKAROUND. On glibc, the C runtime that runs before main() populates
 * `__progname`/`program_invocation_short_name` from the argv[0] the kernel
 * handed the process, so a ported program's very first err() call already
 * has the right name with no work on the program's part. LogitOS's CLI
 * runtime (c/apps/crt0_cli.asm) hands argc/argv to main() as ABI-required
 * registers and does NOT stash argv[0] anywhere else first -- and this unit
 * does not own crt0_cli.asm, so it cannot add that hook. The BSD pair
 * getprogname()/setprogname() below is the real, standard answer to exactly
 * this gap (it predates glibc's automatic version and is what programs
 * written for systems without CRT support call themselves, typically as the
 * first line of main): `setprogname(argv[0])`. Until a program calls it,
 * getprogname() returns a placeholder ("?") -- an honest "I don't know my
 * name yet", not a silent empty string that would print "": diagnostic"
 * with no cue that something is missing. */

const char *getprogname(void);
void        setprogname(const char *name);

void err(int status, const char *fmt, ...);
void errx(int status, const char *fmt, ...);
void warn(const char *fmt, ...);
void warnx(const char *fmt, ...);

void verr(int status, const char *fmt, va_list ap);
void verrx(int status, const char *fmt, va_list ap);
void vwarn(const char *fmt, va_list ap);
void vwarnx(const char *fmt, va_list ap);

#endif /* _ERR_H */
