#ifndef _WCHAR_H
#define _WCHAR_H
#include <stddef.h>
#include <stdarg.h>

/* WIDE CHARACTERS ON LOGITOS.
 *
 * wchar_t is 32 bits and holds a Unicode code point (UTF-32), which is the
 * x86-64 ELF ABI's choice and the only one that makes wchar_t useful.
 *
 * ONE DELIBERATE DEVIATION, STATED HERE AND AT EVERY DEFINITION IT AFFECTS:
 * the multibyte encoding of the "C" locale is UTF-8, not ASCII. The C standard
 * leaves the C locale's encoding implementation-defined, and glibc chooses
 * ASCII; LogitOS is UTF-8 end to end -- the fonts, the DOM, the terminal, the
 * filesystem (see M14) -- so an ASCII C locale would make mbstowcs the one
 * thing in the system that cannot read the text everything else produces.
 * MB_CUR_MAX is therefore 4, and mbrtowc rejects overlong forms, surrogates and
 * anything above U+10FFFF exactly as a UTF-8 decoder must. */

#ifndef __LIBC_WCHAR_T_DEFINED
#define __LIBC_WCHAR_T_DEFINED
typedef __WCHAR_TYPE__ wchar_t;
#endif

typedef int wint_t;
#define WEOF ((wint_t)-1)
#define WCHAR_MIN 0
#define WCHAR_MAX 0x7fffffff

/* mbstate_t is a real conversion state: mbrtowc can be handed one byte of a
 * four-byte character at a time and must remember. */
typedef struct { unsigned int __wch; unsigned char __have, __want; } mbstate_t;

struct tm;
typedef struct _FILE FILE;

/* --- wide string handling ---------------------------------------------- */
size_t   wcslen(const wchar_t *);
size_t   wcsnlen(const wchar_t *, size_t);
wchar_t *wcscpy(wchar_t *, const wchar_t *);
wchar_t *wcpcpy(wchar_t *, const wchar_t *);
wchar_t *wcsncpy(wchar_t *, const wchar_t *, size_t);
wchar_t *wcscat(wchar_t *, const wchar_t *);
wchar_t *wcsncat(wchar_t *, const wchar_t *, size_t);
int      wcscmp(const wchar_t *, const wchar_t *);
int      wcsncmp(const wchar_t *, const wchar_t *, size_t);
int      wcscasecmp(const wchar_t *, const wchar_t *);
int      wcsncasecmp(const wchar_t *, const wchar_t *, size_t);
int      wcscoll(const wchar_t *, const wchar_t *);
size_t   wcsxfrm(wchar_t *, const wchar_t *, size_t);
wchar_t *wcschr(const wchar_t *, wchar_t);
wchar_t *wcsrchr(const wchar_t *, wchar_t);
wchar_t *wcsstr(const wchar_t *, const wchar_t *);
wchar_t *wcspbrk(const wchar_t *, const wchar_t *);
size_t   wcsspn(const wchar_t *, const wchar_t *);
size_t   wcscspn(const wchar_t *, const wchar_t *);
wchar_t *wcstok(wchar_t *, const wchar_t *, wchar_t **);
wchar_t *wcsdup(const wchar_t *);

wchar_t *wmemcpy(wchar_t *, const wchar_t *, size_t);
wchar_t *wmemmove(wchar_t *, const wchar_t *, size_t);
wchar_t *wmemset(wchar_t *, wchar_t, size_t);
int      wmemcmp(const wchar_t *, const wchar_t *, size_t);
wchar_t *wmemchr(const wchar_t *, wchar_t, size_t);

/* --- wide numeric conversion ------------------------------------------- */
double             wcstod(const wchar_t *, wchar_t **);
float              wcstof(const wchar_t *, wchar_t **);
long double        wcstold(const wchar_t *, wchar_t **);
long               wcstol(const wchar_t *, wchar_t **, int);
unsigned long      wcstoul(const wchar_t *, wchar_t **, int);
long long          wcstoll(const wchar_t *, wchar_t **, int);
unsigned long long wcstoull(const wchar_t *, wchar_t **, int);

/* --- multibyte <-> wide ------------------------------------------------- */
wint_t btowc(int);
int    wctob(wint_t);
int    mbsinit(const mbstate_t *);
size_t mbrlen(const char *, size_t, mbstate_t *);
size_t mbrtowc(wchar_t *, const char *, size_t, mbstate_t *);
size_t wcrtomb(char *, wchar_t, mbstate_t *);
size_t mbsrtowcs(wchar_t *, const char **, size_t, mbstate_t *);
size_t wcsrtombs(char *, const wchar_t **, size_t, mbstate_t *);

/* --- wide formatted I/O ------------------------------------------------- */
int swprintf(wchar_t *, size_t, const wchar_t *, ...);
int vswprintf(wchar_t *, size_t, const wchar_t *, va_list);
int wprintf(const wchar_t *, ...);
int fwprintf(FILE *, const wchar_t *, ...);
int vwprintf(const wchar_t *, va_list);
int vfwprintf(FILE *, const wchar_t *, va_list);
int swscanf(const wchar_t *, const wchar_t *, ...);
int vswscanf(const wchar_t *, const wchar_t *, va_list);
int wscanf(const wchar_t *, ...);
int fwscanf(FILE *, const wchar_t *, ...);
int vwscanf(const wchar_t *, va_list);
int vfwscanf(FILE *, const wchar_t *, va_list);

wint_t   fgetwc(FILE *);
wchar_t *fgetws(wchar_t *, int, FILE *);
wint_t   fputwc(wchar_t, FILE *);
int      fputws(const wchar_t *, FILE *);
wint_t   getwc(FILE *);
wint_t   getwchar(void);
wint_t   putwc(wchar_t, FILE *);
wint_t   putwchar(wchar_t);
wint_t   ungetwc(wint_t, FILE *);
int      fwide(FILE *, int);

size_t wcsftime(wchar_t *, size_t, const wchar_t *, const struct tm *);

#endif /* _WCHAR_H */
