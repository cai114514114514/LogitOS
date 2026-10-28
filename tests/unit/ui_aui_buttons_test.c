/* ui-aui-buttons: the buttons of design 4.3, driven through the REAL ring-3 aui.c
 * over the host harness with the REAL SYS_GUI_PATH / SHADOW2 / GLASS2 handlers
 * (tests/unit/aui_host/v2_*.c), recording every call a button makes.
 *
 * Variants x states x modes (light/dark, 100/150 %) x Reduce Transparency, each in
 * a forked child (aui.c keeps its scale and capabilities in file-scope statics):
 *
 *   no-glass         a button makes no GLASS2 call, with or without Reduce Transparency
 *   token-colour     the fill / gradient stops / overlay and the label colour are
 *                    the aui_tok() roles of design 4.3 for rest, hover and pressed
 *   capsule-radius   the fill path's corner radius is h / 2 (28 -> 14, 36 -> 18)
 *   label-weight     the label is callout 13 / 500
 *   press-scale      pressed, the SHAPE is 248/256 of its rest width (label unscaled)
 *   disabled-group   a disabled filled button is drawn in a 102 opacity group
 *   ai-stroke        the AI stroke is a LINEAR 3-stop Logit gradient of 1.5 pt
 *   ai-rotation      its paint axis turns 40 degrees over 600 ms on hover entry,
 *                    holds, and the animation asks for no more frames afterwards
 *   reduce-motion    with ui.reduce_motion the rotation and the press scale snap
 *   legacy           a kernel without GLASS2 / PATH still draws every variant
 *
 * -DUI_BUTTONS_NEGCTL caps the radius at min(h/2, 11): 'FAIL capsule-radius'. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "aui.h"
#include "aui_host.h"
#include "wm_draw2.h"

long wm2_glass2(struct wm2_env *e, long a, long b, long c);
long wm2_path(struct wm2_env *e, long a, long b, long c);
extern void (*v2_text_spy)(const struct logit_run2 *);

static int bad;
#define FAIL(...) do { printf("FAIL " __VA_ARGS__); printf("\n"); bad = 1; } while (0)

#define NP 8
struct prec {
    int kind, nstop, stroke, minx, maxx, miny, maxy, firstx, x0, y0, x1, y1;
    unsigned argb, stop[3];
};
struct shot {
    int status, nglass, nshadow, ngroup, group_op, npath, ntext, tpx, tweight, nrect, nblit, unhandled, due, wait;
    unsigned tcol;
    struct prec p[NP];
    struct logit_tokens tok;
};
static struct shot g_rec;
static int g_dark, g_rt, g_rm;

static int host_copyin(void *dst, const void *usrc, unsigned long n) { memcpy(dst, usrc, n); return 0; }
static void mkenv(struct wm2_env *e, struct aui_host *h)
{
    memset(e, 0, sizeof *e);
    e->d = &h->disp; e->scale_pct = h->pct; e->surf_w = h->w; e->surf_h = h->h;
    e->copyin = host_copyin; e->alloc = malloc; e->release = free;
}
static long glass_hook(struct aui_host *h, long a, long b, long c)
{ struct wm2_env e; g_rec.nglass++; mkenv(&e, h); return wm2_glass2(&e, a, b, c); }

static long path_hook(struct aui_host *h, long a, long b, long c)
{
    struct wm2_env e;
    const struct logit_path2 *pa = (const struct logit_path2 *)a;
    const struct logit_paint2 *pt = (const struct logit_paint2 *)b;
    const struct logit_stroke2 *st = (const struct logit_stroke2 *)c;
    if (g_rec.npath < NP && pa && pt) {
        struct prec *p = &g_rec.p[g_rec.npath];
        p->kind = pt->kind; p->nstop = pt->nstop; p->stroke = st ? st->width : 0; p->argb = pt->argb;
        for (int i = 0; i < pt->nstop && i < 3; i++) p->stop[i] = pt->stop_argb[i];
        p->x0 = pt->x0; p->y0 = pt->y0; p->x1 = pt->x1; p->y1 = pt->y1;
        p->minx = p->miny = 0x7fffffff; p->maxx = p->maxy = -0x7fffffff;
        for (unsigned i = 0; i + 1 < pa->nxy; i += 2) {
            if (pa->xy[i] < p->minx) p->minx = pa->xy[i];
            if (pa->xy[i] > p->maxx) p->maxx = pa->xy[i];
            if (pa->xy[i + 1] < p->miny) p->miny = pa->xy[i + 1];
            if (pa->xy[i + 1] > p->maxy) p->maxy = pa->xy[i + 1];
        }
        p->firstx = pa->nxy ? pa->xy[0] : 0;
    }
    g_rec.npath++;
    mkenv(&e, h);
    return wm2_path(&e, a, b, c);
}
static long shadow_count(struct aui_host *h, long a, long b, long c)
{ (void)h; (void)a; (void)b; (void)c; g_rec.nshadow++; return 0; }
static long group_begin(struct aui_host *h, long a, long b, long c)
{ (void)h; (void)b; (void)c; g_rec.ngroup++; g_rec.group_op = ((const struct logit_group2 *)a)->opacity; return 0; }
static long group_end(struct aui_host *h, long a, long b, long c)
{ (void)h; (void)a; (void)b; (void)c; return 0; }
static long setting_hook(struct aui_host *h, long a, long b, long c)
{
    (void)h; (void)c;
    if (g_rt && !strcmp((const char *)a, "ui.reduce_transparency")) { strcpy((char *)b, "1"); return 1; }
    if (g_rm && !strcmp((const char *)a, "ui.reduce_motion")) { strcpy((char *)b, "1"); return 1; }
    return -1;
}
static void text_spy(const struct logit_run2 *r)
{ g_rec.ntext++; g_rec.tpx = r->px; g_rec.tweight = r->weight; g_rec.tcol = r->color; }

/* ------------------------------------------------------------------ scenes */
#define BX 30
#define BY 40
enum { ST_REST, ST_HOVER, ST_PRESS, ST_OFF };
enum { V_PRIMARY, V_SECONDARY, V_PLAIN, V_PLAIN_TB, V_DESTR, V_DESTR_CONFIRM, V_ICON, V_TOGGLE_ON, V_AI, V_AI_ICON, V_POPUP, V_COUNT };
static const char *const vname[V_COUNT] = { "primary", "secondary", "plain", "plain-toolbar", "destructive", "destructive-confirm",
                                            "icon", "toggle-on", "ai", "ai-icon", "popup" };
struct spec { int v, state, w, h; };

static void draw(const struct spec *sp, int enabled)
{
    static const char *const items[2] = { "OK", "No" };
    int sel = 0;
    aui_begin(AUI_BG);
    switch (sp->v) {
    case V_PRIMARY:     aui_button_ex(BX, BY, sp->w, sp->h, "OK", AUI_V_PRIMARY, enabled); break;
    case V_SECONDARY:   aui_button_ex(BX, BY, sp->w, sp->h, "OK", AUI_BTN_SECONDARY, enabled); break;
    case V_PLAIN:       aui_button_ex(BX, BY, sp->w, sp->h, "OK", AUI_BTN_PLAIN, enabled); break;
    case V_PLAIN_TB:    aui_button_ex(BX, BY, sp->w, sp->h, "OK", (enum aui_variant)(AUI_BTN_PLAIN | AUI_BTN_F_TOOLBAR), enabled); break;
    case V_DESTR:       aui_button_ex(BX, BY, sp->w, sp->h, "OK", AUI_BTN_DESTRUCTIVE, enabled); break;
    case V_DESTR_CONFIRM: aui_button_ex(BX, BY, sp->w, sp->h, "OK", (enum aui_variant)(AUI_BTN_DESTRUCTIVE | AUI_BTN_F_CONFIRM), enabled); break;
    case V_ICON:        aui_icon_button(BX, BY, sp->w, GICON_GEAR, enabled); break;
    case V_TOGGLE_ON:   aui_icon_toggle(BX, BY, sp->w, GICON_SIDEBAR, 1, enabled); break;
    case V_AI:          aui_button_ai(BX, BY, sp->w, sp->h, "OK", enabled); break;
    case V_AI_ICON:     aui_button_ai(BX, BY, sp->w, sp->h, 0, enabled); break;
    case V_POPUP:       aui_select(BX, BY, sp->w, items, 2, &sel); break;
    }
    aui_end(); aui_feed_done();
}
static void ev(int type, const struct spec *sp)
{
    struct logit_event e;
    memset(&e, 0, sizeof e);
    e.type = type; e.a = BX + sp->w / 2; e.b = BY + sp->h / 2; e.button = EV_BTN_LEFT;
    aui_feed(&e);
}

static unsigned long long T0 = 50000;
/* Run one scene in a child; leaves the LAST frame's calls in *out. */
static struct shot *render(int v2, int pct, const struct spec *sp, int ai_series, int ai_ms)
{
    struct shot *s = mmap(0, sizeof *s, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        struct aui_host *h = aui_host_new(160, 120, pct);
        h->dark = g_dark;
        if (v2) aui_host_enable_v2(h);
        aui_host_on(SYS_GUI_GLASS2, glass_hook);
        aui_host_on(SYS_GUI_PATH, path_hook);
        aui_host_on(SYS_GUI_SHADOW2, shadow_count);
        aui_host_on(SYS_GUI_GROUP_BEGIN, group_begin);
        aui_host_on(SYS_GUI_GROUP_END, group_end);
        aui_host_on(SYS_SETTING_GET, setting_hook);
        v2_text_spy = text_spy;
        gui_create("aui-buttons", 160, 120);
        aui_set_size(160, 120);
        aui_round_a(-100, -100, 8, 8, 2, 0xFFFFFF, 128);
        aui_host_clock_set(T0);
        draw(sp, 1);                                       /* the slot is born at rest */
        if (ai_series) {
            ev(EV_MOUSE_MOVE, sp);
            aui_host_clock_advance(16);
            memset(&g_rec, 0, sizeof g_rec); aui_host_stats_reset(h);
            draw(sp, 1);                                   /* the hover entry frame */
            if (ai_ms > 16) {
                aui_host_clock_advance(ai_ms - 16);
                memset(&g_rec, 0, sizeof g_rec); aui_host_stats_reset(h);
                draw(sp, 1);
            }
        } else {
            if (sp->state == ST_HOVER) { ev(EV_MOUSE_MOVE, sp); draw(sp, 1); }
            if (sp->state == ST_PRESS) { ev(EV_MOUSE_MOVE, sp); draw(sp, 1); ev(EV_MOUSE, sp); draw(sp, 1); }
            if (sp->state != ST_REST && sp->state != ST_OFF) aui_host_clock_advance(250);
            memset(&g_rec, 0, sizeof g_rec); aui_host_stats_reset(h);
            draw(sp, sp->state != ST_OFF);
        }
        g_rec.nrect = (int)h->calls[SYS_GUI_RECT];
        g_rec.nblit = (int)h->calls[SYS_GUI_BLIT];
        g_rec.unhandled = aui_host_unhandled_total(h);
        g_rec.tok = *aui_tok();
        g_rec.due = aui_anim_due(); g_rec.wait = aui_anim_wait();
        *s = g_rec;
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    s->status = st;
    if (!WIFEXITED(st) || WEXITSTATUS(st)) FAIL("harness child died (status 0x%x)", st);
    return s;
}

static const int pcts[2] = { 100, 150 };
static unsigned opq(unsigned c) { return 0xFF000000u | (c & 0xFFFFFFu); }

/* the first FILL path of the capsule */
static const struct prec *fillp(const struct shot *s)
{
    for (int i = 0; i < NP && i < s->npath; i++)
        if (!s->p[i].stroke && s->p[i].kind != 99 && (s->p[i].maxy - s->p[i].miny) > 256 * 8) return &s->p[i];
    return 0;
}

static int near(int a, int b, int tol) { return abs(a - b) <= tol; }
static void expect_u(const char *chk, const char *what, unsigned got, unsigned want)
{ if ((got & 0xFFFFFFFFu) != want) FAIL("%s %s: %08x, want %08x", chk, what, got, want); }

static unsigned over(unsigned base, unsigned argb) { return aui_mix(base & 0xFFFFFFu, argb & 0xFFFFFFu, (int)(argb >> 24)); }

static void check_variant(int v, int dark, int pct, int rt)
{
    struct spec sp = { v, ST_REST, 80, 28 };
    char tag[96];
    if (v == V_ICON || v == V_TOGGLE_ON) { sp.w = 28; sp.h = 28; }
    if (v == V_AI_ICON) { sp.w = 28; sp.h = 28; }
    snprintf(tag, sizeof tag, "%s %s @%d%%%s", vname[v], dark ? "dark" : "light", pct, rt ? " RT" : "");
    g_dark = dark; g_rt = rt; g_rm = 0;
    struct shot *st[4];
    for (int k = 0; k < 4; k++) {
        sp.state = k;
        st[k] = render(1, pct, &sp, 0, 0);
        if (st[k]->nglass) FAIL("no-glass %s state %d: %d GLASS2 calls", tag, k, st[k]->nglass);
        if (st[k]->unhandled) FAIL("legacy %s state %d: unhandled syscall numbers", tag, k);
    }
    const struct logit_tokens *t = &st[0]->tok;
    int filled = v == V_PRIMARY || v == V_SECONDARY || v == V_DESTR_CONFIRM || v == V_AI || v == V_AI_ICON || v == V_POPUP;
    unsigned base = dark ? 0x3A3A43u : t->bg_raised;
    /* colour of the fill per state */
    for (int k = 0; k < 3; k++) {
        const struct prec *f = fillp(st[k]);
        unsigned want = 0, wantg = 0;
        if (filled) {
            if (!f) { FAIL("token-colour %s state %d: no fill path", tag, k); continue; }
            if (v == V_PRIMARY) {
                unsigned c = k == 2 ? t->accent_pressed : k == 1 ? t->accent_hover : t->accent;
                if (f->kind != LOGIT_PAINT2_LINEAR || f->nstop != 2) FAIL("token-colour %s state %d: Primary is not a 2-stop LINEAR gradient", tag, k);
                expect_u("token-colour", tag, f->stop[0], opq(aui_mix(c, 0xFFFFFF, 10)));
                expect_u("token-colour", tag, f->stop[1], opq(aui_mix(c, 0, 10)));
                continue;
            }
            if (v == V_DESTR_CONFIRM) want = k == 2 ? aui_mix(t->destructive_fill, 0, 31) : k == 1 ? aui_mix(t->destructive_fill, 0, 15) : t->destructive_fill;
            else want = k == 2 ? over(base, t->fill_pressed) : k == 1 ? over(base, t->fill_hover) : base;
            wantg = opq(want);
            if (f->kind != LOGIT_PAINT2_SOLID) FAIL("token-colour %s state %d: fill is not SOLID", tag, k);
            expect_u("token-colour", tag, f->argb, wantg);
        } else {
            int on = v == V_TOGGLE_ON;
            if (k == 0 && !on) { if (f) FAIL("token-colour %s rest: a resting Plain/Icon draws a fill (%08x)", tag, f->argb); continue; }
            if (!f) { FAIL("token-colour %s state %d: no overlay path", tag, k); continue; }
            if (on) {
                unsigned ha = k == 2 ? t->fill_pressed >> 24 : k == 1 ? t->fill_hover >> 24 : 0;
                unsigned a = (t->accent_tint >> 24) + ha;
                expect_u("token-colour", tag, f->argb, ((a > 255 ? 255 : a) << 24) | (t->accent_tint & 0xFFFFFF));
            } else expect_u("token-colour", tag, f->argb, k == 2 ? t->fill_pressed : t->fill_hover);
        }
    }
    /* capsule radius: the fill path's first point sits `r` right of its left edge */
    {
        const struct prec *f = fillp(st[filled || v == V_TOGGLE_ON ? 0 : 1]);
        if (!f) FAIL("capsule-radius %s: no capsule path", tag);
        else {
            int r = f->firstx - f->minx, want = sp.h / 2 * 256;
            if (!near(r, want, 3)) FAIL("capsule-radius %s: radius %.2f pt, want %d (h/2)", tag, r / 256.0, sp.h / 2);
        }
    }
    /* label: callout 13 / 500 in the right token */
    if (v != V_ICON && v != V_TOGGLE_ON && v != V_AI_ICON) {
        unsigned want = v == V_PRIMARY ? t->label_on_accent : v == V_PLAIN ? t->accent_ink : v == V_DESTR ? t->error_ink :
                        v == V_DESTR_CONFIRM ? t->label_on_accent : t->label_primary;
        if (st[0]->ntext != 1) FAIL("label-weight %s: %d text runs, want 1", tag, st[0]->ntext);
        else {
            if (st[0]->tpx != 13 * 256 || st[0]->tweight != 500) FAIL("label-weight %s: px %d weight %d, want 3328 / 500", tag, st[0]->tpx, st[0]->tweight);
            expect_u("token-colour", tag, st[0]->tcol & 0xFFFFFF, want & 0xFFFFFF);
        }
        if (v != V_PLAIN && v != V_PLAIN_TB && v != V_DESTR && st[3]->npath == 0) FAIL("disabled-group %s: nothing drawn when disabled", tag);
    }
    /* press: the shape is 248/256 of its rest width */
    if (filled) {
        const struct prec *a = fillp(st[0]), *b = fillp(st[2]);
        if (a && b) {
            long wa = a->maxx - a->minx, wb = b->maxx - b->minx;
            long q = wb * 256 / wa;
            if (q < 246 || q > 250) FAIL("press-scale %s: pressed shape is %ld/256 of rest, want 248", tag, q);
        }
        if (v != V_POPUP && (st[3]->ngroup != 1 || st[3]->group_op != 102)) FAIL("disabled-group %s: %d groups, opacity %d, want 1 @ 102", tag, st[3]->ngroup, st[3]->group_op);
        if (st[0]->ngroup) FAIL("disabled-group %s: an enabled button opened a group", tag);
    } else if (v == V_PLAIN || v == V_PLAIN_TB) {
        if (st[3]->ntext != 1 || (st[3]->tcol & 0xFFFFFF) != (t->label_quaternary & 0xFFFFFF))
            FAIL("token-colour %s disabled: label %06x, want label.quaternary %06x", tag, st[3]->tcol & 0xFFFFFF, t->label_quaternary & 0xFFFFFF);
    }
    for (int k = 0; k < 4; k++) munmap(st[k], sizeof *st[k]);
}

static double axis_deg(const struct prec *p) { return atan2((double)(p->y1 - p->y0), (double)(p->x1 - p->x0)) * 180.0 / M_PI; }
static const struct prec *strokep(const struct shot *s)
{ for (int i = 0; i < NP && i < s->npath; i++) if (s->p[i].stroke && s->p[i].kind == LOGIT_PAINT2_LINEAR) return &s->p[i]; return 0; }

static void check_ai(void)
{
    static const int ms[5] = { 16, 300, 600, 700, 1500 };
    struct spec sp = { V_AI, ST_REST, 80, 28 };
    double deg[5];
    int ok = 1;
    g_dark = 0; g_rt = 0; g_rm = 0;
    for (int i = 0; i < 5; i++) {
        struct shot *s = render(1, 100, &sp, 1, ms[i]);
        const struct prec *k = strokep(s);
        if (!k) { FAIL("ai-stroke: no stroke path at %d ms", ms[i]); ok = 0; deg[i] = 0; munmap(s, sizeof *s); continue; }
        if (k->kind != LOGIT_PAINT2_LINEAR || k->nstop != 3 || k->stroke != 384 ||
            (k->stop[0] & 0xFFFFFF) != LOGIT_BRAND_STOP0 || (k->stop[1] & 0xFFFFFF) != LOGIT_BRAND_STOP1 || (k->stop[2] & 0xFFFFFF) != LOGIT_BRAND_STOP2) {
            FAIL("ai-stroke: kind %d nstop %d width %d stops %06x %06x %06x", k->kind, k->nstop, k->stroke, k->stop[0] & 0xFFFFFF, k->stop[1] & 0xFFFFFF, k->stop[2] & 0xFFFFFF);
            ok = 0;
        }
        deg[i] = axis_deg(k);
        if (ms[i] == 1500 && (s->due || s->wait)) { FAIL("ai-rotation: still asking for frames 1500 ms after hover (due %d wait %d)", s->due, s->wait); ok = 0; }
        if (ms[i] == 300 && !s->wait) { FAIL("ai-rotation: no frame requested mid-rotation"); ok = 0; }
        munmap(s, sizeof *s);
    }
    if (ok) {
        double total = deg[4] - deg[0];
        if (!(fabs(deg[1] - deg[0]) > 1.0)) FAIL("ai-rotation: matrix unchanged 300 ms after hover (%.2f -> %.2f)", deg[0], deg[1]);
        if (fabs(total - 40.0) > 1.5) FAIL("ai-rotation: total turn %.2f deg, want 40", total);
        if (fabs(deg[3] - deg[4]) > 0.05) FAIL("ai-rotation: still turning after 600 ms (%.2f vs %.2f)", deg[3], deg[4]);
        if (!(deg[1] - deg[0] > 0.3 * total && deg[1] - deg[0] < 0.7 * total)) FAIL("ai-rotation: 300 ms is not halfway (%.2f of %.2f)", deg[1] - deg[0], total);
        if (!bad) printf("     ai axis %.1f %.1f %.1f %.1f %.1f deg at 16/300/600/700/1500 ms\n", deg[0], deg[1], deg[2], deg[3], deg[4]);
    }
    /* Reduce Motion: the rotation lands on the first hovered frame, the press scale on the first pressed one */
    g_rm = 1;
    {
        struct shot *a = render(1, 100, &sp, 1, 16), *b = render(1, 100, &sp, 1, 1500);
        const struct prec *ka = strokep(a), *kb = strokep(b);
        if (!ka || !kb || fabs(axis_deg(ka) - axis_deg(kb)) > 0.05) FAIL("reduce-motion: the AI rotation did not snap");
        munmap(a, sizeof *a); munmap(b, sizeof *b);
        struct spec ps = { V_SECONDARY, ST_PRESS, 80, 28 }, rs = { V_SECONDARY, ST_REST, 80, 28 };
        a = render(1, 100, &rs, 0, 0); b = render(1, 100, &ps, 0, 0);
        const struct prec *fa = fillp(a), *fb = fillp(b);
        if (!fa || !fb || (fb->maxx - fb->minx) * 256 / (fa->maxx - fa->minx) != 248) FAIL("reduce-motion: press scale is not 248 at once");
        munmap(a, sizeof *a); munmap(b, sizeof *b);
    }
    g_rm = 0;
}

static void check_legacy(void)
{
    for (int v = 0; v < V_COUNT; v++)
        for (int p = 0; p < 2; p++) {
            struct spec sp = { v, ST_REST, 80, 28 };
            if (v == V_ICON || v == V_TOGGLE_ON || v == V_AI_ICON) { sp.w = 28; }
            g_dark = p; g_rt = 0; g_rm = 0;
            struct shot *s = render(0, pcts[p], &sp, 0, 0);
            if (s->nglass) FAIL("no-glass %s legacy kernel: %d GLASS2 calls", vname[v], s->nglass);
            if (s->unhandled) FAIL("legacy %s: unhandled syscall numbers", vname[v]);
            munmap(s, sizeof *s);
        }
}

int main(void)
{
    for (int v = 0; v < V_COUNT; v++)
        for (int d = 0; d < 2; d++)
            for (int p = 0; p < 2; p++)
                for (int rt = 0; rt < 2; rt++)
                    check_variant(v, d, pcts[p], rt);
    if (!bad) printf("ok   variants: 11 variants x rest/hover/pressed/disabled x light/dark x 100/150%% x RT: tokens, capsule h/2, 13/500, press 248, group 102\n");
    check_ai();
    check_legacy();
    if (!bad) printf("ok   ai, reduce-motion, legacy kernel\n");
    printf(bad ? "FAIL ui-aui-buttons\n" : "ok   ui-aui-buttons\n");
    return bad;
}
