/* getopt_long and friends.
 *
 * The short-option half (getopt, optarg, optind, opterr, optopt) is declared by
 * <unistd.h>, which is where POSIX puts it; this header adds the GNU long-option
 * extension, and including it also gets you the short half. That split is not
 * ours -- it is what glibc does, and a ported program that includes only one of
 * the two headers expects exactly this.
 *
 * BASENAME CHECK, per CLAUDE.md's rule about the flat INCDIRS list: there is no
 * other getopt.h under c/kernel, c/drivers, c/net, c/fs or c/lib, so this can
 * live at the top level and does not need include/uonly/.
 *
 * getopt_long_only is deliberately absent rather than stubbed -- see the note in
 * c/apps/libc/src/getopt.c. A program that wants it fails to compile, which is
 * the honest outcome; a stub that quietly treated -foo as a short cluster would
 * mis-parse silently. */
#ifndef _GETOPT_H
#define _GETOPT_H

#include <unistd.h>          /* getopt, optarg, optind, opterr, optopt */

#ifdef __cplusplus
extern "C" {
#endif

#define no_argument        0
#define required_argument  1
#define optional_argument  2

struct option {
    const char *name;
    int         has_arg;     /* no_argument / required_argument / optional_argument */
    int        *flag;        /* if non-NULL, *flag = val and getopt_long returns 0 */
    int         val;
};

int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex);

#ifdef __cplusplus
}
#endif

#endif /* _GETOPT_H */
