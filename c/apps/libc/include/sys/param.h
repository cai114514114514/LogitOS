#ifndef _SYS_PARAM_H
#define _SYS_PARAM_H

#include <limits.h>
#include <stddef.h>

#define NBBY CHAR_BIT
#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#define roundup(x, y) (howmany((x), (y)) * (y))
#define rounddown(x, y) ((x) - ((x) % (y)))
#define powerof2(x) ((((x) - 1) & (x)) == 0)
#define nitems(a) (sizeof(a) / sizeof((a)[0]))

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef MAXPATHLEN
#define MAXPATHLEN PATH_MAX
#endif
#ifndef MAXHOSTNAMELEN
#define MAXHOSTNAMELEN 256
#endif

#endif /* _SYS_PARAM_H */
