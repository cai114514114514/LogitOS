/* arena_page_mem: how much heap a REAL page costs, measured through the REAL
 * allocator.
 *
 * WHY NOT css_bench. css_bench runs the same pipeline and answers "where does
 * the time go". It links the host's glibc malloc, so it cannot answer "how much
 * memory does this page need", which is the question that decides whether a
 * 12.77 MB web application can be held at all. This links
 * c/apps/libc/src/malloc.c under its real names, so every allocation in the
 * process -- the pipeline's kmalloc, LibCSS's internal malloc/realloc, and
 * stdio's own buffers -- goes through the allocator the guest runs, and the
 * numbers below are that allocator's own accounting.
 *
 * WHAT THE TWO NUMBERS MEAN, because they are not the same and the difference
 * is the entire point of the mmap change:
 *   peak   the most PAYLOAD that was live at once. What the page needs.
 *   hwm    the highest arena offset ever occupied. What the process made
 *          RESIDENT, and therefore what it costs the machine. Before the arena
 *          was mmap'd this number was irrelevant, because the cost was
 *          ARENA_SIZE whatever the page did.
 * hwm >= peak always: freeing lowers `peak`'s live count but does not un-touch
 * a page. A hwm far above peak is fragmentation, and worth looking at.
 *
 * Usage:  arena_page_mem <dir>...      each dir = a tests/fixtures/cssweb capture
 *                                      (index.html + sheet-*.css)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include "logit.h"                 /* the paint recorder, via -Itests/unit/painthost */
#include "layout.h"
#include "css.h"
#include "dom.h"
#include "browser_paint.h"

struct paintop paint_ops[PAINT_MAXOPS];
int paint_nops;

extern size_t malloc_hwm;
extern size_t malloc_peak;
size_t malloc_arena_size(void);

void *kmalloc(unsigned long n){ return malloc(n); }
void  kfree(void *p){ free(p); }
int text_measure(const char *s, int len, int px, int mono){ (void)s;(void)mono; return len * (px/2); }
int res_fetch(const char *url, uint8_t **buf, int *len){ (void)url;(void)buf;(void)len; return -1; }
void img_free(struct image *o){ (void)o; }
int img_decode(const uint8_t *p, int n, struct image *out){ (void)p;(void)n;(void)out; return -1; }

#define WINW 760
#define VIEWH 520

static char *slurp(const char *path, int *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, n, f);
    b[got] = 0; fclose(f);
    *len = (int)got;
    return b;
}

static int tag_is(const char *t, const char *lit)
{ int i = 0; for (; lit[i]; i++) if (t[i] != lit[i]) return 0; return t[i] == 0; }

static int collect_style(struct node *n, char *out, int o, int max)
{
    if (!n) return o;
    if (n->type == N_ELEM && tag_is(n->tag, "style"))
        for (struct node *c = n->first_child; c; c = c->next)
            if (c->type == N_TEXT && c->text)
                for (int i = 0; i < c->textlen && o < max - 1; i++) out[o++] = c->text[i];
    for (struct node *c = n->first_child; c; c = c->next) o = collect_style(c, out, o, max);
    return o;
}

static long count_elems(struct node *n)
{
    if (!n) return 0;
    long c = (n->type == N_ELEM);
    for (struct node *k = n->first_child; k; k = k->next) c += count_elems(k);
    return c;
}

static int cmp_str(const void *a, const void *b)
{ return strcmp(*(const char *const *)a, *(const char *const *)b); }

/* Last path component, tolerating the trailing slash that `$(dir ...)` leaves. */
static const char *dir_base(const char *p)
{
    static char buf[256];
    size_t n = strlen(p);
    while (n && p[n - 1] == '/') n--;
    size_t s = n;
    while (s && p[s - 1] != '/') s--;
    size_t k = n - s; if (k >= sizeof buf) k = sizeof buf - 1;
    memcpy(buf, p + s, k); buf[k] = 0;
    return buf;
}

/* CSSMAX mirrors browser.c's author_css/css_expanded pair, so the buffers this
 * measures are the ones the browser really has. */
#define CSSMAX (4 * 1024 * 1024)
static char author_css[CSSMAX];
static char expanded[CSSMAX + CSSMAX / 8];

static void run_one(const char *dir)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/index.html", dir);
    int htmllen = 0;
    char *html = slurp(path, &htmllen);
    if (!html) { printf("  %-10s (no index.html)\n", dir); return; }

    /* Every sheet-*.css in the capture, in name order, the way browser.c
     * appends them once the <link>s have been fetched. */
    char *names[64]; int nn = 0;
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && nn < 64)
            if (!strncmp(e->d_name, "sheet-", 6)) names[nn++] = strdup(e->d_name);
        closedir(d);
    }
    qsort(names, nn, sizeof *names, cmp_str);

    int sheetlen = 0;
    for (int i = 0; i < nn; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        int l = 0; char *s = slurp(path, &l);
        if (s && sheetlen + l + 1 < CSSMAX) {
            memcpy(author_css + sheetlen, s, l);   /* staged; prefixed below */
            sheetlen += l;
            author_css[sheetlen++] = '\n';
        }
        free(s); free(names[i]);
    }
    /* stash the external sheets, then rebuild author_css as inline-then-external */
    char *ext = malloc(sheetlen ? sheetlen : 1);
    memcpy(ext, author_css, sheetlen);

    /* Reset the accounting so each page's numbers are its own. peak is a live
     * count and returns to ~0 between pages; hwm cannot be reset (pages stay
     * touched), so it is reported as a DELTA. */
    size_t hwm0 = malloc_hwm;
    malloc_peak = 0;

    struct node *root = dom_parse(html, htmllen);
    if (!root) { printf("  %-10s (parse failed)\n", dir); free(html); free(ext); return; }
    size_t after_dom = malloc_peak;

    int css_len = collect_style(root, author_css, 0, CSSMAX);
    int inline_css = css_len;
    if (sheetlen && css_len + sheetlen < CSSMAX) {
        memcpy(author_css + css_len, ext, sheetlen);
        css_len += sheetlen;
    }

    css_init();
    css_viewport(WINW, VIEWH);
    css_set_post_pass(css_extra_apply);
    int exlen = css_expand_vars(author_css, css_len, expanded, (int)sizeof expanded);
    css_apply(root, expanded, exlen);
    css_extra_apply(root, expanded, exlen);
    size_t after_css = malloc_peak;

    layout_page(root, WINW);
    paint_nops = 0;
    browser_paint(0, 0, WINW, VIEWH, 0);
    size_t after_all = malloc_peak;

    printf("  %-10s %6d %6d %6d %7ld %9zu %9zu %9zu %9zu\n",
           dir_base(dir), htmllen / 1024, inline_css / 1024, css_len / 1024,
           count_elems(root),
           after_dom / 1024, (after_css > after_dom ? after_css - after_dom : 0) / 1024,
           after_all / 1024,
           (malloc_hwm - hwm0) / 1024);

    free(html); free(ext);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: arena_page_mem <cssweb-dir>...\n"); return 2; }
    printf("arena_page_mem: heap cost of a real page, through mini-libc's own allocator\n");
    printf("reservation %zu MiB (address space; frames appear on touch)\n\n",
           malloc_arena_size() >> 20);
    printf("  %-10s %6s %6s %6s %7s %9s %9s %9s %9s\n",
           "page", "htmlK", "inlnK", "cssK", "elems",
           "domKiB", "+cssKiB", "peakKiB", "residKiB");
    printf("  %-10s %6s %6s %6s %7s %9s %9s %9s %9s\n",
           "----------", "------", "------", "------", "-------",
           "---------", "---------", "---------", "---------");
    /* ONE PROCESS PER PAGE. The render pipeline has no teardown -- nothing frees
     * a DOM tree or a computed-style table -- so running the corpus in a single
     * process makes every page's figures include every page before it, which is
     * how the first version of this file reported a 34 KiB page as costing
     * 43 MiB. fork() gives each page a pristine heap; the arena is MAP_PRIVATE,
     * so the child's copy is genuinely its own. */
    for (int i = 1; i < argc; i++) {
        fflush(stdout);          /* or the child inherits a copy of the buffer
                                  * and every header is printed once per page */
        pid_t pid = fork();
        if (pid == 0) { run_one(argv[i]); fflush(stdout); _exit(0); }
        if (pid > 0) { int st; waitpid(pid, &st, 0); }
    }
    printf("\nEach row is a separate process, so the figures are the page's own.\n");
    return 0;
}
