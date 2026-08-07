#ifndef _STRING_H
#define _STRING_H
#include <stddef.h>

/* C11 7.24 */
void  *memcpy(void *, const void *, size_t);
void  *memmove(void *, const void *, size_t);
void  *memset(void *, int, size_t);
int    memcmp(const void *, const void *, size_t);
void  *memchr(const void *, int, size_t);
size_t strlen(const char *);
int    strcmp(const char *, const char *);
int    strncmp(const char *, const char *, size_t);
int    strcoll(const char *, const char *);
size_t strxfrm(char *, const char *, size_t);
char  *strcpy(char *, const char *);
char  *strncpy(char *, const char *, size_t);
char  *strcat(char *, const char *);
char  *strncat(char *, const char *, size_t);
char  *strchr(const char *, int);
char  *strrchr(const char *, int);
char  *strstr(const char *, const char *);
char  *strpbrk(const char *, const char *);
char  *strtok(char *, const char *);
size_t strspn(const char *, const char *);
size_t strcspn(const char *, const char *);
char  *strerror(int);

/* POSIX */
size_t strnlen(const char *, size_t);
char  *strtok_r(char *, const char *, char **);
char  *strdup(const char *);
char  *strndup(const char *, size_t);
char  *stpcpy(char *, const char *);
char  *stpncpy(char *, const char *, size_t);
void  *memccpy(void *, const void *, int, size_t);
int    strerror_r(int, char *, size_t);
char  *strsignal(int);
int    strcasecmp(const char *, const char *);
int    strncasecmp(const char *, const char *, size_t);

/* GNU / BSD extensions that ported C reaches for constantly */
void  *memmem(const void *, size_t, const void *, size_t);
void  *memrchr(const void *, int, size_t);
void  *mempcpy(void *, const void *, size_t);
char  *strchrnul(const char *, int);
char  *strcasestr(const char *, const char *);
int    strverscmp(const char *, const char *);
size_t strlcpy(char *, const char *, size_t);   /* BSD: bounded copy, always NUL-term, returns src len */
size_t strlcat(char *, const char *, size_t);   /* BSD: bounded concat, returns intended total len */
char  *strsep(char **, const char *);           /* tokenize: split *sp at any delim char */

#endif
