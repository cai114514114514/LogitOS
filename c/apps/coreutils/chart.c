#include "clib.h"
#include "logit_rich.h"

/* chart [title]
 *
 * Reads "label value" (or "value label") lines from stdin and draws a bar chart.
 * A drawn one when the terminal can, ASCII bars when it cannot -- which is the
 * point: a filter in the middle of a pipeline is never handed the rich channel,
 * so `... | chart | cat` and `chart > file` both produce text.
 */

static struct rt_enc E;

#define MAXN 24
static char  labs[MAXN][16];
static unsigned vals[MAXN];
static int n;

static int isdig(char c) { return c >= '0' && c <= '9'; }

static void split(char *line)
{
    /* two fields, either order; a bare number gets an index label */
    char *a = line;
    while (*a == ' ' || *a == '\t') a++;
    char *b = a;
    while (*b && *b != ' ' && *b != '\t') b++;
    if (*b) { *b++ = 0; while (*b == ' ' || *b == '\t') b++; }
    char *e = b;
    while (*e && *e != ' ' && *e != '\t' && *e != '\r') e++;
    *e = 0;

    if (n >= MAXN) return;
    if (isdig(a[0]) && !b[0]) {
        char idx[16]; int k = 0; int v = n + 1;
        if (v >= 10) idx[k++] = (char)('0' + v / 10);
        idx[k++] = (char)('0' + v % 10); idx[k] = 0;
        c_strcpy(labs[n], idx, sizeof labs[0]);
        vals[n] = (unsigned)c_atoi(a);
    } else if (isdig(a[0]) && b[0]) {
        c_strcpy(labs[n], b, sizeof labs[0]);
        vals[n] = (unsigned)c_atoi(a);
    } else {
        c_strcpy(labs[n], a, sizeof labs[0]);
        vals[n] = (unsigned)c_atoi(b);
    }
    n++;
}

int main(int argc, char **argv)
{
    rt_init(argc, argv);
    const char *title = argc > 1 ? argv[1] : "";

    char line[128];
    while (readline(0, line, sizeof line) >= 0) {
        int blank = 1;
        for (int i = 0; line[i]; i++) if (line[i] != ' ' && line[i] != '\t' && line[i] != '\r') { blank = 0; break; }
        if (!blank) split(line);
        if (n >= MAXN) break;
    }
    if (n == 0) { errs("chart: nothing on stdin\n"); return 1; }

    if (rt_isrich()) {
        rt_reset(&E);
        rt_u16(&E, n);
        rt_u16(&E, 0);                       /* kind 0 = horizontal bars */
        rt_str(&E, title);
        for (int i = 0; i < n; i++) { rt_u32(&E, vals[i]); rt_str(&E, labs[i]); }
        if (rt_send(RT_T_CHART, &E) == 0) return 0;
    }

    unsigned mx = 1;
    for (int i = 0; i < n; i++) if (vals[i] > mx) mx = vals[i];
    if (title[0]) { rt_out(title); rt_out("\n"); }
    for (int i = 0; i < n; i++) {
        int w = c_strlen(labs[i]);
        rt_out(labs[i]);
        for (int k = w; k < 12; k++) rt_out(" ");
        int bars = (int)((unsigned long)vals[i] * 30 / mx);
        for (int k = 0; k < bars; k++) rt_out("#");
        rt_out(" ");
        char t[16]; int j = 0; unsigned v = vals[i];
        if (!v) t[j++] = '0';
        while (v) { t[j++] = (char)('0' + v % 10); v /= 10; }
        char o[17]; int q = 0; while (j) o[q++] = t[--j]; o[q] = 0;
        rt_out(o);
        rt_out("\n");
    }
    return 0;
}
