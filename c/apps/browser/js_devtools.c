#include "js_devtools.h"

/* No #include <string.h> on purpose -- see the header's "NO GUI CALLS" note
 * for the parallel argument about hosts: this file is meant to build
 * unmodified both under BROWSER_JS_CF (the freestanding target) and under a
 * plain host cc for tests/unit/devtools_test.c, and the one thing it truly
 * needs (a bounded, NUL-terminating string copy) is four lines to write
 * locally rather than one more thing that has to agree between two builds. */
static void dt_strcpy(char *dst, int cap, const char *src)
{
    int i = 0;
    if (!src) src = "";
    if (cap <= 0) return;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

struct dt_script {
    char name[DT_NAME_CAP];
    int  kind;
    int  state;
    char reason[DT_REASON_CAP];
};

static struct dt_script g_dt_script[DT_SCRIPT_MAX];
static int g_dt_script_n;
static int g_dt_script_collected_n;   /* incremented ONLY by dt_script_add */
static int g_dt_script_expect = -1;   /* -1 = not set this page */

/* g_dt_of_res[i]: the dt_script index the record for g_res[i] (browser.c's
 * own resource-table index) landed at, or -1. Sized with real headroom --
 * github.com, the widest script count this tree's own scoreboard corpus has
 * shown, ships on the order of five dozen; 1024 leaves room for a page an
 * order of magnitude busier without silently losing the mapping. An index
 * past the end is not a crash, just "this script's later state update is not
 * tracked" -- see dt_script_mark(). */
#define DT_RES_MAP_MAX 1024
static int g_dt_of_res[DT_RES_MAP_MAX];

struct dt_net {
    char kind[DT_KIND_CAP];
    char url[DT_URL_CAP];
    int  got;      /* 1 arrived, 0 requested but failed, -1 required, never asked */
    int  status;
    char err[DT_ERR_CAP];
};

static struct dt_net g_dt_net[DT_NET_MAX];
static int g_dt_net_n;

void dt_reset(void)
{
    g_dt_script_n = 0;
    g_dt_script_collected_n = 0;
    g_dt_script_expect = -1;
    g_dt_net_n = 0;
    for (int i = 0; i < DT_RES_MAP_MAX; i++) g_dt_of_res[i] = -1;
}

void dt_script_add(int res_idx, const char *name, int kind)
{
    if (g_dt_script_n >= DT_SCRIPT_MAX) return;
    struct dt_script *s = &g_dt_script[g_dt_script_n];
    dt_strcpy(s->name, sizeof s->name, name);
    s->kind = kind;
    s->state = DT_ST_COLLECTED;
    s->reason[0] = 0;
    if (res_idx >= 0 && res_idx < DT_RES_MAP_MAX) g_dt_of_res[res_idx] = g_dt_script_n;
    g_dt_script_n++;
    g_dt_script_collected_n++;
}

void dt_script_skip(const char *name, int kind, const char *reason)
{
    if (g_dt_script_n >= DT_SCRIPT_MAX) return;
    struct dt_script *s = &g_dt_script[g_dt_script_n++];
    dt_strcpy(s->name, sizeof s->name, name);
    s->kind = kind;
    s->state = DT_ST_SKIPPED;
    dt_strcpy(s->reason, sizeof s->reason, reason);
}

void dt_script_mark(int res_idx, int state, const char *reason)
{
    if (res_idx < 0 || res_idx >= DT_RES_MAP_MAX) return;
    int di = g_dt_of_res[res_idx];
    if (di < 0 || di >= g_dt_script_n) return;
    g_dt_script[di].state = state;
    dt_strcpy(g_dt_script[di].reason, sizeof g_dt_script[di].reason, reason);
}

void dt_script_set_expect(int n) { g_dt_script_expect = n; }
int  dt_script_expect(void) { return g_dt_script_expect; }

int dt_script_count(void) { return g_dt_script_n; }
int dt_script_collected_count(void) { return g_dt_script_collected_n; }

int dt_sources_broken(void)
{
    if (g_dt_script_expect < 0) return 0;   /* not measured this page: not a claim either way */
    return g_dt_script_expect != g_dt_script_collected_n;
}

int dt_script_get(int i, char *name, int namecap, int *kind, int *state,
                   char *reason, int reasoncap)
{
    if (i < 0 || i >= g_dt_script_n) {
        if (name && namecap > 0) name[0] = 0;
        if (reason && reasoncap > 0) reason[0] = 0;
        if (kind) *kind = 0;
        if (state) *state = 0;
        return 0;
    }
    struct dt_script *s = &g_dt_script[i];
    if (name) dt_strcpy(name, namecap, s->name);
    if (reason) dt_strcpy(reason, reasoncap, s->reason);
    if (kind) *kind = s->kind;
    if (state) *state = s->state;
    return 1;
}

void dt_net_record(const char *kind, const char *url, int got, int status, const char *err)
{
    if (g_dt_net_n >= DT_NET_MAX) return;
    struct dt_net *r = &g_dt_net[g_dt_net_n++];
    dt_strcpy(r->kind, sizeof r->kind, kind);
    dt_strcpy(r->url, sizeof r->url, url);
    r->got = got ? 1 : 0;
    r->status = status;
    dt_strcpy(r->err, sizeof r->err, err);
}

void dt_net_required_not_requested(const char *kind, const char *url, const char *reason)
{
    if (g_dt_net_n >= DT_NET_MAX) return;
    struct dt_net *r = &g_dt_net[g_dt_net_n++];
    dt_strcpy(r->kind, sizeof r->kind, kind);
    dt_strcpy(r->url, sizeof r->url, url);
    r->got = -1;
    r->status = 0;
    dt_strcpy(r->err, sizeof r->err, reason);
}

int dt_net_count(void) { return g_dt_net_n; }

int dt_net_get(int i, char *kind, int kindcap, char *url, int urlcap,
               int *got, int *status, char *err, int errcap)
{
    if (i < 0 || i >= g_dt_net_n) {
        if (kind && kindcap > 0) kind[0] = 0;
        if (url && urlcap > 0) url[0] = 0;
        if (err && errcap > 0) err[0] = 0;
        if (got) *got = 0;
        if (status) *status = 0;
        return 0;
    }
    struct dt_net *r = &g_dt_net[i];
    if (kind) dt_strcpy(kind, kindcap, r->kind);
    if (url) dt_strcpy(url, urlcap, r->url);
    if (err) dt_strcpy(err, errcap, r->err);
    if (got) *got = r->got;
    if (status) *status = r->status;
    return 1;
}

/* ---- text rendering: one place builds the sentence, see the header ---- */

static void dt_app(char *out, int *p, int cap, const char *s)
{
    if (!s) return;
    while (*s && *p < cap - 1) out[(*p)++] = *s++;
}

static void dt_app_int(char *out, int *p, int cap, int n)
{
    char d[12]; int nd = 0;
    unsigned v;
    if (n < 0) { dt_app(out, p, cap, "-"); v = (unsigned)(-n); }
    else v = (unsigned)n;
    if (v == 0) d[nd++] = '0';
    while (v > 0 && nd < (int)sizeof d) { d[nd++] = (char)('0' + v % 10); v /= 10; }
    while (nd > 0 && *p < cap - 1) out[(*p)++] = d[--nd];
}

static const char *dt_state_name(int state)
{
    switch (state) {
    case DT_ST_COLLECTED:  return "COLLECTED";
    case DT_ST_EXECUTED:   return "EXECUTED";
    case DT_ST_EXEC_ERROR: return "EXECUTED (threw)";
    case DT_ST_SKIPPED:    return "SKIPPED";
    case DT_ST_FAILED:     return "FAILED";
    default:               return "?";
    }
}

void dt_summary_line(char *out, int cap)
{
    int p = 0;
    if (cap <= 0) return;
    if (dt_sources_broken()) {
        dt_app(out, &p, cap, "SOURCES RECORDER DISAGREES WITH THE BROWSER'S OWN COUNT: expected ");
        dt_app_int(out, &p, cap, dt_script_expect());
        dt_app(out, &p, cap, " collected, recorded ");
        dt_app_int(out, &p, cap, dt_script_collected_count());
        dt_app(out, &p, cap, " -- this list is NOT trustworthy");
        out[p] = 0;
        return;
    }
    int collected = 0, executed = 0, error = 0, skipped = 0, failed = 0;
    for (int i = 0; i < g_dt_script_n; i++) {
        switch (g_dt_script[i].state) {
        case DT_ST_COLLECTED:  collected++; break;
        case DT_ST_EXECUTED:   executed++;  break;
        case DT_ST_EXEC_ERROR: error++;     break;
        case DT_ST_SKIPPED:    skipped++;   break;
        case DT_ST_FAILED:     failed++;    break;
        }
    }
    dt_app_int(out, &p, cap, g_dt_script_n);
    dt_app(out, &p, cap, " collected, ");
    dt_app_int(out, &p, cap, executed);
    dt_app(out, &p, cap, " executed, ");
    dt_app_int(out, &p, cap, error);
    dt_app(out, &p, cap, " threw, ");
    dt_app_int(out, &p, cap, skipped);
    dt_app(out, &p, cap, " skipped, ");
    dt_app_int(out, &p, cap, failed);
    dt_app(out, &p, cap, " failed");
    if (collected) { dt_app(out, &p, cap, " ("); dt_app_int(out, &p, cap, collected);
                      dt_app(out, &p, cap, " still pending)"); }
    out[p] = 0;
}

void dt_script_line(int i, char *out, int cap)
{
    int p = 0;
    if (cap <= 0) return;
    if (i < 0 || i >= g_dt_script_n) { out[0] = 0; return; }
    struct dt_script *s = &g_dt_script[i];
    dt_app(out, &p, cap, s->kind == DT_KIND_MODULE ? "[module] " : "[classic] ");
    dt_app(out, &p, cap, s->name);
    dt_app(out, &p, cap, " -- ");
    dt_app(out, &p, cap, dt_state_name(s->state));
    if (s->reason[0]) { dt_app(out, &p, cap, ": "); dt_app(out, &p, cap, s->reason); }
    out[p] = 0;
}

void dt_net_line(int i, char *out, int cap)
{
    int p = 0;
    if (cap <= 0) return;
    if (i < 0 || i >= g_dt_net_n) { out[0] = 0; return; }
    struct dt_net *r = &g_dt_net[i];
    dt_app(out, &p, cap, "[");
    dt_app(out, &p, cap, r->kind);
    dt_app(out, &p, cap, "] ");
    if (r->got < 0) {
        dt_app(out, &p, cap, "NEVER REQUESTED (");
        dt_app(out, &p, cap, r->err);
        dt_app(out, &p, cap, ") ");
    } else if (r->got == 0) {
        dt_app(out, &p, cap, "FAILED");
        if (r->status) { dt_app(out, &p, cap, " "); dt_app_int(out, &p, cap, r->status); }
        dt_app(out, &p, cap, " (");
        dt_app(out, &p, cap, r->err);
        dt_app(out, &p, cap, ") ");
    } else {
        dt_app(out, &p, cap, "OK");
        if (r->status) { dt_app(out, &p, cap, " "); dt_app_int(out, &p, cap, r->status); }
        dt_app(out, &p, cap, " ");
    }
    dt_app(out, &p, cap, r->url);
    out[p] = 0;
}
