/* ui_aim.h -- test geometry over serial: `[<app>] aim <name> <x> <y>`.
 *
 * Design P13 (design_language.md 1) and addendum I. Every control a test or
 * the tour clicks publishes its centre, in window-local content POINTS, from
 * the code that draws it. A driver (tests/qmp/qmp_aim.py) reads the real
 * layout instead of keeping a second copy of it, so layouts can move without
 * moving a single test constant. This is Settings' private `aim()`
 * (settings/view.inc) made shared, with three differences that matter:
 *
 *   - NAMES ARE STORED BY VALUE (char[UI_AIM_NAME_MAX]). Settings kept the
 *     caller's pointer, which is only safe for string literals; a name built
 *     in a stack buffer (`row-12`) would alias the next one.
 *   - THE TABLE IS LRU, NOT FIXED. A full table evicts the least recently
 *     seen name; it never drops a new one (a 25th control in Settings was
 *     silently never published). An evicted name re-emits the next time it is
 *     seen, which costs one line and loses nothing.
 *   - ONE LINE IS ONE WRITE. The line is formatted here and handed to
 *     SYS_WRITE once; printf on this runtime emits fragments and WM log lines
 *     splice between them. (aui.o links no libc -- clock.aex is crt0 + aui +
 *     gfx -- so the one format is this header's own, not snprintf.)
 *
 * A line is printed only when the name is new or its x/y changed, so an
 * unchanged frame prints nothing (make test-ui-aim). Nothing prints until
 * ui_aim_set_app() names the app: aims are opt-in per process.
 *
 * ONE EMITTER PER NAME. Controls drawn by aui widgets (tabs, segmented, list
 * and table rows, select and its popup rows, dialogs) get their aims from aui
 * itself (aui_aim_prefix / aui_aim_names in aui.h); an app calls ui_aim only
 * for controls it draws by hand.
 *
 * The state is ONE weak global, so aui.o and the app's own objects share the
 * table and the app name although each includes this header. Freestanding:
 * no libc, no memset/memcpy (every loop below is data-terminated). */
#ifndef UI_AIM_H
#define UI_AIM_H

#include "logit.h"

#define UI_AIM_NAME_MAX 32      /* bytes, NUL included: longer names are cut */
#define UI_AIM_APP_MAX  24
#define UI_AIM_SLOTS    64

struct ui_aim_slot {
    char name[UI_AIM_NAME_MAX];
    int x, y;
    unsigned seen;              /* LRU tick of the last ui_aim(); 0 = free */
};
struct ui_aim_state {
    char app[UI_AIM_APP_MAX];   /* "" = aims off */
    unsigned tick;
    struct ui_aim_slot slot[UI_AIM_SLOTS];
};
/* Weak: every object that includes this header carries a definition and the
 * linker keeps one, so the app and aui.o see the same table. */
__attribute__((weak)) struct ui_aim_state ui_aim_g;

static inline int ui_aim_on(void) { return ui_aim_g.app[0] != 0; }

/* Name the process in the `[<app>]` prefix and switch aims on. NULL or ""
 * switches them off. Copied by value. */
static inline void ui_aim_set_app(const char *app)
{
    int i = 0;
    if (app)
        for (; app[i] && i < UI_AIM_APP_MAX - 1; i++) ui_aim_g.app[i] = app[i];
    ui_aim_g.app[i] = 0;
}

static inline int ui_aim__put(char *o, int n, int cap, const char *s)
{
    for (; s && *s && n < cap; s++) o[n++] = *s;
    return n;
}
static inline int ui_aim__int(char *o, int n, int cap, int v)
{
    char d[12];
    int k = 0;
    unsigned u = v < 0 ? 0u - (unsigned)v : (unsigned)v;
    if (v < 0 && n < cap) o[n++] = '-';
    do { d[k++] = (char)('0' + u % 10u); u /= 10u; } while (u && k < 12);
    while (k > 0 && n < cap) o[n++] = d[--k];
    return n;
}
/* Same name? `a` is a stored (cut) name, `b` the caller's, compared over the
 * stored length so a cut name keeps matching its caller. */
static inline int ui_aim__same(const char *a, const char *b)
{
    int i = 0;
    for (; i < UI_AIM_NAME_MAX - 1 && a[i]; i++) if (a[i] != b[i]) return 0;
    return i == UI_AIM_NAME_MAX - 1 || b[i] == 0;
}

/* Publish control `name` centred at (x, y), window-local points. */
static inline void ui_aim(const char *name, int x, int y)
{
    struct ui_aim_state *g = &ui_aim_g;
    struct ui_aim_slot *s = 0, *lru = 0, *fr = 0;
    int i;
    if (!g->app[0] || !name || !name[0]) return;
    if (++g->tick == 0) g->tick = 1;
    for (i = 0; i < UI_AIM_SLOTS; i++) {
        struct ui_aim_slot *c = &g->slot[i];
        if (!c->seen) { if (!fr) fr = c; continue; }
        if (ui_aim__same(c->name, name)) { s = c; break; }
        if (!lru || c->seen < lru->seen) lru = c;
    }
    if (s) {
        s->seen = g->tick;
        if (s->x == x && s->y == y) return;          /* unchanged: no line */
    } else {
#ifdef UI_AIM_NEGCTL
        /* THE CONTROL: the fixed-size table that drops a name once it is
         * full (Settings' original). make test-ui-aim-negctl needs
         * 'FAIL aim-lru' from it. */
        if (!fr) return;
#endif
        s = fr ? fr : lru;
        for (i = 0; i < UI_AIM_NAME_MAX - 1 && name[i]; i++) s->name[i] = name[i];
        s->name[i] = 0;
        s->seen = g->tick;
    }
    s->x = x; s->y = y;
    {
        char line[UI_AIM_APP_MAX + UI_AIM_NAME_MAX + 40];
        int n = 0, cap = (int)sizeof line - 1;
        n = ui_aim__put(line, n, cap, "[");
        n = ui_aim__put(line, n, cap, g->app);
        n = ui_aim__put(line, n, cap, "] aim ");
        n = ui_aim__put(line, n, cap, s->name);
        n = ui_aim__put(line, n, cap, " ");
        n = ui_aim__int(line, n, cap, x);
        n = ui_aim__put(line, n, cap, " ");
        n = ui_aim__int(line, n, cap, y);
        line[n++] = '\n';
        sys_write(1, line, n);
    }
}

/* The centre of (x, y, w, h). */
static inline void ui_aim_rect(const char *name, int x, int y, int w, int h)
{
    ui_aim(name, x + w / 2, y + h / 2);
}

/* `<prefix>-<i>`: one name per item of a row of controls. */
static inline void ui_aim_i(const char *prefix, int i, int x, int y)
{
    char nm[UI_AIM_NAME_MAX];
    int n = ui_aim__put(nm, 0, UI_AIM_NAME_MAX - 13, prefix);
    nm[n++] = '-';
    n = ui_aim__int(nm, n, UI_AIM_NAME_MAX - 1, i);
    nm[n] = 0;
    ui_aim(nm, x, y);
}

#endif /* UI_AIM_H */
