/* <assert.h> deliberately has NO include guard: C requires that it can be
 * included repeatedly and that the definition of assert track the CURRENT
 * setting of NDEBUG each time. */

#undef assert

#ifdef NDEBUG
#define assert(x) ((void)0)
#else
/* This used to be `((void)0)` unconditionally, with a comment about keeping
 * QuickJS lean. That is the wrong place to make that decision: a header that
 * silently deletes every assertion in a program it did not write is the exact
 * failure mode this library is being cleaned up to stop having -- the program
 * believes it is checking its invariants and is not. NDEBUG is where "no
 * assertions" belongs, and LogitOS's userland build passes -DNDEBUG (see
 * UCFLAGS in the Makefile), so the shipped .aex files are unchanged. Anything
 * compiled without it now gets a real check with a real message. */
void __assert_fail(const char *expr, const char *file, int line, const char *fn);
#define assert(x) \
    ((x) ? (void)0 : __assert_fail(#x, __FILE__, __LINE__, __func__))
#endif

#ifndef _ASSERT_H
#define _ASSERT_H
#define static_assert _Static_assert
#endif
