#ifndef _CTYPE_H
#define _CTYPE_H

/* ASCII ctype for the "C" locale, which is the only locale (see <locale.h>).
 *
 * These are static inline rather than out-of-line functions on purpose -- they
 * are on the hot path of every parser in the tree -- but C requires that each
 * one also be usable as a function designator, so `int (*f)(int) = tolower;`
 * and `(isdigit)(c)` both work: an inline definition still has an address.
 *
 * All of them accept any value representable as unsigned char, plus EOF. Note
 * the casts: passing a negative char (any byte >= 0x80 on this target) to a
 * table-driven ctype is the classic C bug, and these are written so that it is
 * merely false rather than out of bounds. */

static inline int isascii(int c) { return (unsigned)c < 128; }
static inline int toascii(int c) { return c & 0x7f; }
static inline int isspace(int c) { return c==' '||c=='\t'||c=='\n'||c=='\v'||c=='\f'||c=='\r'; }
static inline int isblank(int c) { return c==' '||c=='\t'; }
static inline int isdigit(int c) { return c>='0'&&c<='9'; }
static inline int isxdigit(int c){ return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'); }
static inline int isupper(int c) { return c>='A'&&c<='Z'; }
static inline int islower(int c) { return c>='a'&&c<='z'; }
static inline int isalpha(int c) { return isupper(c)||islower(c); }
static inline int isalnum(int c) { return isalpha(c)||isdigit(c); }
static inline int iscntrl(int c) { return (unsigned)c<32||c==127; }
static inline int isprint(int c) { return (unsigned)(c-32)<95; }
static inline int isgraph(int c) { return (unsigned)(c-33)<94; }
static inline int ispunct(int c) { return isgraph(c)&&!isalnum(c); }
static inline int toupper(int c) { return islower(c)?c-32:c; }
static inline int tolower(int c) { return isupper(c)?c+32:c; }
/* SVID/BSD spellings some ported sources still use; unlike toupper/tolower
 * these are only defined for arguments that are already the wrong case. */
static inline int _toupper(int c) { return c-32; }
static inline int _tolower(int c) { return c+32; }

#endif /* _CTYPE_H */
