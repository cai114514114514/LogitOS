/* wm_theme.c -- theme state.
 *
 * What the WM derives from the theme and the appearance settings, refreshed
 * on the prefs tick and on every flip: the chrome's colour tokens (design P3,
 * 3.1). wm_tok() is the kernel twin of aui's theme: the logit_tokens.h table
 * for the theme in force with the seven accent roles derived from ui.accent
 * by logit_tokens_derive(), the function aui calls, so the WM's selection
 * pill and an app's selected row can never disagree.
 *
 * ui.accent = 0x5E96FF is the kernel schema's pre-「澄」 default: a store
 * nobody has written answers it, and no swatch can pick it, so it reads as
 * the new default, 靛 indigo (aui_theme.inc does the same).
 *
 * LOCKING. wm_theme_refresh runs on the WM thread from the prefs tick and
 * from wm_set_dark, both under wm_lock, so refreshes are serialised. Readers
 * are the painters, which may run on band workers in parallel with the
 * thread that refreshes; they get a pointer into one of two generations and
 * the refresh writes the OTHER one before publishing it with a release
 * store, so a reader never sees a half-derived table. A reader must not keep
 * the pointer across frames (it is good until the refresh after next, i.e.
 * at least one prefs tick, half a second).
 *
 * Unlike the other wm_*.c files this one includes only wm_theme.h, wm.h and
 * settings.h, not wm_internal.h, so tests/unit/ui_wm_abi_theme_test.c can link
 * it unmodified on the host. */
#include "wm_theme.h"
#include "wm.h"          /* wm_dark() */
#include "settings.h"    /* settings_get_color() */

#define WM_ACCENT_LEGACY_STORE 0x5E96FFu

/* [generation][dark] */
static struct logit_tokens wm_tok_gen[2][2];
static int wm_tok_cur = -1;          /* -1: never derived (wm_tok derives on demand) */
static unsigned wm_tok_accent;       /* the base the current generation was derived from */

static void wm_tok_build(struct logit_tokens out[2], unsigned base)
{
    for (int dark = 0; dark < 2; dark++) {
        out[dark] = *logit_tok(dark);
#ifdef UI_WMTOK_NEGCTL
        /* THE CONTROL of test-ui-wm-abi-theme: the accent is never derived,
         * every theme keeps the default indigo roles. */
        (void)base;
#else
        logit_tokens_derive(base, dark, &out[dark]);
#endif
    }
}

/* The accent base in force: ui.accent, the legacy default reading as 靛. */
static unsigned wm_accent_base(void)
{
    unsigned c = settings_get_color("ui.accent", LOGIT_ACCENT_DEFAULT) & 0xFFFFFFu;
    return c == WM_ACCENT_LEGACY_STORE ? LOGIT_ACCENT_DEFAULT : c;
}

static void wm_tok_publish(unsigned base)
{
    int next = wm_tok_cur < 0 ? 0 : !wm_tok_cur;
    wm_tok_build(wm_tok_gen[next], base);
    wm_tok_accent = base;
    __atomic_store_n(&wm_tok_cur, next, __ATOMIC_RELEASE);
}

void wm_theme_refresh(void)
{
    unsigned base = wm_accent_base();
    if (__atomic_load_n(&wm_tok_cur, __ATOMIC_ACQUIRE) < 0 || base != wm_tok_accent)
        wm_tok_publish(base);
}

const struct logit_tokens *wm_tok(void)
{
    int cur = __atomic_load_n(&wm_tok_cur, __ATOMIC_ACQUIRE);
    if (cur < 0) {
        /* Before the first refresh (early boot paint): the defaults, which
         * is exactly what an unwritten store derives to. */
        return logit_tok(wm_dark());
    }
    return &wm_tok_gen[cur][wm_dark() ? 1 : 0];
}
