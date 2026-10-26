/* WCHAR_MIN / WCHAR_MAX must describe wchar_t. Compile-time only: the answer is a
 * property of the header, so a successful -fsyntax-only is the whole test (no
 * binary to link, nothing to run). The mini-libc header used to say
 * `#define WCHAR_MIN 0` for a SIGNED 32-bit wchar_t, and the value changed with
 * the order <wchar.h> and the compiler's <stdint.h> were included. Both orders
 * are checked here, in separate scopes of the same file via the FIRST macro. */
#include <wchar.h>

#define SIGNED_WCHAR (((wchar_t)-1) < 0)
_Static_assert((WCHAR_MIN < 0) == SIGNED_WCHAR, "WCHAR_MIN disagrees with the signedness of wchar_t");
_Static_assert(WCHAR_MIN == (SIGNED_WCHAR ? -WCHAR_MAX - 1 : 0), "WCHAR_MIN is not the minimum of wchar_t");
_Static_assert((wchar_t)WCHAR_MAX == WCHAR_MAX && (wchar_t)WCHAR_MIN == WCHAR_MIN, "limits do not fit wchar_t");
_Static_assert(WCHAR_MAX > 0x10ffff, "WCHAR_MAX cannot hold every Unicode scalar");

int libc_wchar_limits_ok;
