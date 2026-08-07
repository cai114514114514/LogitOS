#include "clib.h"
#include "logit_rich.h"

/* dir [path]
 *
 * A directory listing that is a TABLE when the terminal can draw one -- ruled
 * columns, and a name cell that opens the file with its associated app when you
 * click it (SYS_OPEN_PATH, the same association the Finder uses) -- and three
 * plain columns of text everywhere else.
 *
 * It is a separate program from `ls` on purpose: changing what `ls` writes would
 * be a cross-cutting decision about every script and test in the tree, and this
 * protocol is supposed to make rich output possible WITHOUT that.
 */

static struct rt_enc E;

static void abspath(const char *in, char *out, int max)
{
    if (in[0] == '/') { c_strcpy(out, in, max); return; }
    char cwd[128];
    sys_getcwd(cwd, sizeof cwd);
    path_join(out, cwd, in, max);
}

static void num(char *o, long v)
{ char t[24]; int i = 0; if (!v) { o[0] = '0'; o[1] = 0; return; }
  while (v > 0) { t[i++] = (char)('0' + v % 10); v /= 10; }
  int k = 0; while (i) o[k++] = t[--i]; o[k] = 0; }

static void pad_out(const char *s, int w)
{
    rt_out(s);
    int n = c_strlen(s);
    for (int i = n; i < w; i++) rt_out(" ");
}

int main(int argc, char **argv)
{
    rt_init(argc, argv);

    char dir[160];
    abspath(argc > 1 ? argv[1] : ".", dir, sizeof dir);
    int n = dir_count(dir);
    if (n < 0) { errs("dir: not a directory: "); errs(dir); errs("\n"); return 1; }

    if (rt_isrich()) {
        rt_reset(&E);
        rt_u16(&E, 3);
        rt_u16(&E, n);
        rt_str(&E, dir);
        rt_str(&E, "name"); rt_str(&E, "size"); rt_str(&E, "kind");
        for (int i = 0; i < n; i++) {
            char nm[64];
            int sz = dir_name(dir, i, nm);
            if (sz == -1) { nm[0] = 0; sz = 0; }
            char full[224];
            path_join(full, dir, nm, sizeof full);
            char sn[24];
            if (sz == -2) c_strcpy(sn, "-", sizeof sn); else num(sn, sz);
            /* name: a link, so the row is clickable */
            rt_u8(&E, RT_CELL_LINK); rt_str(&E, nm);   rt_str(&E, full);
            rt_u8(&E, RT_CELL_NUM);  rt_str(&E, sn);   rt_str(&E, "");
            rt_u8(&E, RT_CELL_TEXT); rt_str(&E, sz == -2 ? "dir" : "file"); rt_str(&E, "");
        }
        if (rt_send(RT_T_TABLE, &E) == 0) return 0;
        /* fall through to plain if the channel went away mid-frame */
    }

    for (int i = 0; i < n; i++) {
        char nm[64];
        int sz = dir_name(dir, i, nm);
        if (sz == -1) continue;
        char sn[24];
        if (sz == -2) c_strcpy(sn, "-", sizeof sn); else num(sn, sz);
        pad_out(nm, 24);
        pad_out(sn, 10);
        rt_out(sz == -2 ? "dir" : "file");
        rt_out("\n");
    }
    return 0;
}
