/* Public UAX #14 wrapper over the shared core.  See linebreak.h.
 *
 * TL_LINEBREAK_NEGCTL (tests only) allows a break after every code point: the
 * plausible-looking wrong answer a layout gets from "wrap anywhere", which the
 * test's exact expected positions must reject. */

#include <stdint.h>
#include "linebreak.h"

/* The shared core carries the whole generated table, including the case maps
 * the browser's text-transform needs; this TU uses only part of it. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-function"
#include "linebreak_core.inc"
#pragma clang diagnostic pop

_Static_assert(LBC_BRK_PROHIBITED == TL_BRK_NONE &&
               LBC_BRK_ALLOWED == TL_BRK_ALLOWED &&
               LBC_BRK_MANDATORY == TL_BRK_MANDATORY,
               "core and enum tl_brk must agree");

/* Static scratch: ring 3, one caller at a time (documented in the header). */
static uint32_t      g_cp[TL_BREAK_MAX];
static uint8_t       g_cls[TL_BREAK_MAX];
static struct chain  g_chain[TL_BREAK_MAX];
static unsigned char g_nt[TL_BREAK_MAX + 1];
static unsigned char g_brk[TL_BREAK_MAX + 1];

int tl_break_utf8(const char *s, int len, unsigned char *out, int flags)
{
    int n = 0, i, j, adv;
    int cj_loose = (flags & TL_LB_MASK) == TL_LB_LOOSE;

    if (len < 0) return -1;
    for (i = 0; i <= len; i++) out[i] = TL_BRK_NONE;
    out[len] = TL_BRK_MANDATORY;
    if (len == 0) return 1;

    for (i = 0; i < len; i += adv) {
        uint32_t c;
        if (n >= TL_BREAK_MAX) return -1;
        adv = u8_next(s, len, i, &c);
        g_cp[n++] = c;
    }
    for (j = 0; j < n; j++)
        g_cls[j] = (uint8_t)lb_resolve_cls(lb_raw(g_cp[j]), 0, cj_loose);
    lb_engine(g_cp, g_cls, n, g_chain, g_brk, g_nt);

    if (flags & TL_WB_BREAK_ALL) {
        /* Every prohibited position becomes an opportunity except those a
         * non-tailorable rule (LB2..LB12) decided. */
        for (j = 1; j < n; j++)
            if (g_brk[j] == LBC_BRK_PROHIBITED && !g_nt[j])
                g_brk[j] = LBC_BRK_ALLOWED;
    } else if (flags & TL_WB_KEEP_ALL) {
        /* An opportunity survives only after a space-like class. */
        for (j = 1; j < n; j++) {
            int pa;
            if (g_brk[j] != LBC_BRK_ALLOWED) continue;
            pa = g_cls[j - 1];
            if (pa == LB_SP || pa == LB_BA || pa == LB_HY || pa == LB_ZW ||
                pa == LB_B2 || pa == LB_CB)
                continue;
            g_brk[j] = LBC_BRK_PROHIBITED;
        }
    }

#ifdef TL_LINEBREAK_NEGCTL
    for (j = 1; j < n; j++) g_brk[j] = TL_BRK_ALLOWED;
#endif

    /* Map code point indices back to byte offsets (re-walking the string
     * costs less than a 2048-entry offset table of static storage). */
    for (i = 0, j = 0; i < len; j++) {
        uint32_t c;
        adv = u8_next(s, len, i, &c);
        out[i] = g_brk[j];
        i += adv;
    }
    out[0] = TL_BRK_NONE;
    return len + 1;
}
