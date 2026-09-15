#ifndef _STRINGS_H
#define _STRINGS_H

#include <stddef.h>

/* Case-insensitive compares (POSIX <strings.h>); used by LibCSS parse/mq. */
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
int bcmp(const void *, const void *, size_t);
void bcopy(const void *, void *, size_t);
void bzero(void *, size_t);
char *index(const char *, int);
char *rindex(const char *, int);
int ffs(int);
int ffsl(long);
int ffsll(long long);
int timingsafe_bcmp(const void *, const void *, size_t);
int timingsafe_memcmp(const void *, const void *, size_t);
#include <locale.h>
int strcasecmp_l(const char *, const char *, locale_t);
int strncasecmp_l(const char *, const char *, size_t, locale_t);

#endif /* _STRINGS_H */
