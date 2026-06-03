#ifndef _STRINGS_H
#define _STRINGS_H

#include <stddef.h>

/* Case-insensitive compares (POSIX <strings.h>); used by LibCSS parse/mq. */
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);

#endif /* _STRINGS_H */
