#ifndef _UCHAR_H
#define _UCHAR_H

/* C11 UTF-16/UTF-32 restartable conversion.  wchar_t is UTF-32 on this ABI,
 * so the implementation can share wchar.c's strict UTF-8 decoder and needs
 * state only for UTF-16 surrogate pairs. */
#include <stddef.h>
#include <wchar.h>

#ifndef __LIBC_CHAR16_T_DEFINED
#define __LIBC_CHAR16_T_DEFINED
#ifdef __CHAR16_TYPE__
typedef __CHAR16_TYPE__ char16_t;
#else
/* TCC 0.9.27 has no __CHAR16_TYPE__. On this x86-64 ABI, clang's type is
 * unsigned short; keep the public type identical across both compilers. */
typedef unsigned short char16_t;
#endif
#endif
#ifndef __LIBC_CHAR32_T_DEFINED
#define __LIBC_CHAR32_T_DEFINED
#ifdef __CHAR32_TYPE__
typedef __CHAR32_TYPE__ char32_t;
#else
/* As above, clang uses unsigned int for the 32-bit code-unit type. */
typedef unsigned int char32_t;
#endif
#endif

size_t mbrtoc16(char16_t *restrict pc16, const char *restrict s, size_t n,
                mbstate_t *restrict ps);
size_t c16rtomb(char *restrict s, char16_t c16, mbstate_t *restrict ps);
size_t mbrtoc32(char32_t *restrict pc32, const char *restrict s, size_t n,
                mbstate_t *restrict ps);
size_t c32rtomb(char *restrict s, char32_t c32, mbstate_t *restrict ps);

#endif /* _UCHAR_H */
