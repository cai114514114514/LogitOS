#ifndef _SYSEXITS_H
#define _SYSEXITS_H

/* BSD <sysexits.h>: exit-status conventions for programs that call err()/
 * errx() with a "real" status instead of a bare 1. Header only -- there is
 * nothing here for this OS to get wrong, because these are just numbers a
 * program passes to exit()/err(); the kernel does not interpret any of
 * them specially (waitpid() hands the raw status back to the parent, same
 * as any other exit code). Values below are copied from the actual 4.4BSD
 * table (unchanged by glibc, which ships the identical file) rather than
 * re-derived, because these constants are a fixed, published convention --
 * "re-deriving" them would just mean retyping the same numbers with a
 * chance to get one wrong. */

#define EX_OK          0   /* successful termination */

#define EX__BASE       64  /* base value for error messages */

#define EX_USAGE       64  /* command line usage error */
#define EX_DATAERR     65  /* data format error */
#define EX_NOINPUT     66  /* cannot open input */
#define EX_NOUSER      67  /* addressee unknown */
#define EX_NOHOST      68  /* host name unknown */
#define EX_UNAVAILABLE 69  /* service unavailable */
#define EX_SOFTWARE    70  /* internal software error */
#define EX_OSERR       71  /* system error (e.g., can't fork) */
#define EX_OSFILE      72  /* critical OS file missing */
#define EX_CANTCREAT   73  /* can't create (user) output file */
#define EX_IOERR       74  /* input/output error */
#define EX_TEMPFAIL    75  /* temp failure; user is invited to retry */
#define EX_PROTOCOL    76  /* remote error in protocol */
#define EX_NOPERM      77  /* permission denied */
#define EX_CONFIG      78  /* configuration error */

#define EX__MAX        78  /* maximum listed value */

#endif /* _SYSEXITS_H */
