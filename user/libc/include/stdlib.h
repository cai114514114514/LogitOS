#ifndef _STDLIB_H
#define _STDLIB_H
#include <stddef.h>

typedef long ssize_t;
#define alloca(n) __builtin_alloca(n)

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7fffffff

void  *malloc(size_t);
void   free(void *);
void  *realloc(void *, size_t);
void  *calloc(size_t, size_t);
size_t malloc_usable_size(void *);

void   abort(void);
void   exit(int);
void   _Exit(int);

int    abs(int);
long   labs(long);
long long llabs(long long);

int    atoi(const char *);
long   atol(const char *);
long long atoll(const char *);
double atof(const char *);
double strtod(const char *, char **);
float  strtof(const char *, char **);
long   strtol(const char *, char **, int);
unsigned long strtoul(const char *, char **, int);
long long strtoll(const char *, char **, int);
unsigned long long strtoull(const char *, char **, int);

void   qsort(void *, size_t, size_t, int (*)(const void *, const void *));
void  *bsearch(const void *, const void *, size_t, size_t,
               int (*)(const void *, const void *));

int    rand(void);
void   srand(unsigned);

char  *getenv(const char *);

#endif
