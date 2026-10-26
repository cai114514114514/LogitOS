/* GUI ABI v2 ring-3 wrappers (c/lib/gfx/include/openlogit_wire2.h), captured
 * through build-prewire's LOGIT_SYS_HOOK seam: c/apps/logit.h then routes
 * every _sys to gav2_sys_hook instead of trapping. Its own translation unit
 * because logit.h is the ring-3 world (hundreds of inline wrappers) and the
 * rest of the suite is the kernel's (fb.h, wm_draw2.h).
 *
 * For every wrapper: the syscall number, that `a` (and b/c where used) is the
 * caller's own struct, that `size` was stamped with sizeof BEFORE the trap,
 * that no other byte of the struct changed, and that the kernel's status
 * comes back unchanged. Run from tests/unit/gui_abi_v2_test.c. */
#define LOGIT_SYS_HOOK gav2_sys_hook
#include "logit.h"
#include <stdio.h>
#include <string.h>

int gav2_wire_run(void);

static struct {
    long n, a, b, c;
    unsigned size_a, size_b, size_c;   /* `size` fields as the kernel saw them */
    int calls;
} cap;
static long hook_ret;

long gav2_sys_hook(long n, long a, long b, long c)
{
    cap.n = n; cap.a = a; cap.b = b; cap.c = c; cap.calls++;
    cap.size_a = a && n != SYS_GUI2_INFO && n != SYS_GUI_BACKDROP_CLEAR ? *(const unsigned *)a : 0;
    cap.size_b = b ? *(const unsigned *)b : 0;
    cap.size_c = c ? *(const unsigned *)c : 0;
    return hook_ret;
}

static int fails;
static void ck(int cond, const char *what, const char *detail)
{
    if (cond) return;
    printf("FAIL wire %s: %s\n", what, detail);
    fails++;
}
static void reset(long ret) { memset(&cap, 0, sizeof cap); hook_ret = ret; }

/* A struct-taking wrapper: fill with a pattern, call, compare. */
#define CHECK_STRUCT1(name, type, sysno, expect_size, call)                  \
    do {                                                                     \
        type v, before;                                                      \
        memset(&v, 0xA5, sizeof v);                                          \
        memcpy(&before, &v, sizeof v);                                       \
        reset(-3);                                                           \
        long r = call;                                                       \
        ck(sizeof(type) == (expect_size), name, "sizeof differs from the ABI table"); \
        ck(cap.calls == 1, name, "not exactly one syscall");                 \
        ck(cap.n == (sysno), name, "wrong syscall number");                  \
        ck(cap.a == (long)&v, name, "a is not the caller's struct");         \
        ck(cap.b == 0 && cap.c == 0, name, "b/c not zero");                  \
        ck(cap.size_a == sizeof(type), name, "size not stamped before the trap"); \
        ck(v.size == sizeof(type), name, "size field not sizeof");          \
        ck(!memcmp((char *)&v + 4, (char *)&before + 4, sizeof v - 4), name, \
           "wrapper changed a byte other than size");                       \
        ck(r == -3, name, "status not passed through");                      \
    } while (0)

int gav2_wire_run(void)
{
    fails = 0;

    reset(LOGIT_GUI2_VERSION << 16);
    long r = ol_gui2_info(1);
    ck(cap.n == SYS_GUI2_INFO && cap.a == 1 && !cap.b && !cap.c, "info", "wrong emission");
    ck(r == LOGIT_GUI2_VERSION << 16, "info", "status not passed through");

    /* path: fill (stroke NULL) and stroke. */
    {
        struct logit_path2 p, pb; struct logit_paint2 q, qb; struct logit_stroke2 s, sb;
        memset(&p, 0x5A, sizeof p); memset(&q, 0x3C, sizeof q); memset(&s, 0x69, sizeof s);
        memcpy(&pb, &p, sizeof p); memcpy(&qb, &q, sizeof q); memcpy(&sb, &s, sizeof s);
        reset(-3);
        r = ol_window_path2(&p, &q, 0);
        ck(cap.n == SYS_GUI_PATH && cap.a == (long)&p && cap.b == (long)&q && cap.c == 0,
           "path-fill", "wrong emission");
        ck(cap.size_a == 64 && cap.size_b == 64, "path-fill", "sizes not stamped (64, 64)");
        ck(!memcmp((char *)&p + 4, (char *)&pb + 4, sizeof p - 4) &&
           !memcmp((char *)&q + 4, (char *)&qb + 4, sizeof q - 4), "path-fill", "other bytes changed");
        ck(!memcmp(&s, &sb, sizeof s), "path-fill", "touched the unused stroke");
        ck(r == -3, "path-fill", "status not passed through");
        reset(-1);
        r = ol_window_path2(&p, &q, &s);
        ck(cap.n == SYS_GUI_PATH && cap.a == (long)&p && cap.b == (long)&q && cap.c == (long)&s,
           "path-stroke", "wrong emission");
        ck(cap.size_c == 52 && s.size == 52, "path-stroke", "stroke size not stamped (52)");
        ck(!memcmp((char *)&s + 4, (char *)&sb + 4, sizeof s - 4), "path-stroke", "other bytes changed");
        ck(r == -1, "path-stroke", "status not passed through");
    }

    CHECK_STRUCT1("clip_push2", struct logit_clip2, SYS_GUI_CLIP_PUSH, 40, ol_window_clip_push2(&v));
    CHECK_STRUCT1("group_begin2", struct logit_group2, SYS_GUI_GROUP_BEGIN, 44, ol_window_group_begin2(&v));
    CHECK_STRUCT1("blit2", struct logit_blit2, SYS_GUI_BLIT2, 72, ol_window_blit2(&v));
    CHECK_STRUCT1("text_run2", struct logit_run2, SYS_GUI_TEXT_RUN2, 48, ol_window_text_run2(&v));
    CHECK_STRUCT1("shadow2", struct logit_shadow2, SYS_GUI_SHADOW2, 60, ol_window_shadow2(&v));
    CHECK_STRUCT1("glass2", struct logit_glass2, SYS_GUI_GLASS2, 76, ol_window_glass2(&v));
    CHECK_STRUCT1("backdrop2", struct logit_backdrop2, SYS_GUI_BACKDROP, 84, ol_window_backdrop2(&v));

    reset(-3);
    r = ol_window_clip_pop2();
    ck(cap.calls == 1 && cap.n == SYS_GUI_CLIP_POP && !cap.a && !cap.b && !cap.c && r == -3,
       "clip_pop2", "wrong emission");
    reset(-3);
    r = ol_window_group_end2();
    ck(cap.calls == 1 && cap.n == SYS_GUI_GROUP_END && !cap.a && !cap.b && !cap.c && r == -3,
       "group_end2", "wrong emission");
    reset(-3);
    r = ol_window_backdrop_clear2(-1);
    ck(cap.calls == 1 && cap.n == SYS_GUI_BACKDROP_CLEAR && cap.a == -1 && !cap.b && !cap.c && r == -3,
       "backdrop_clear2", "wrong emission");
    reset(-3);
    ol_window_backdrop_clear2(7);
    ck(cap.a == 7, "backdrop_clear2", "id not passed as a");

    if (!fails) printf("ok   wire: 12 wrappers emit their number, struct and size\n");
    return fails;
}
