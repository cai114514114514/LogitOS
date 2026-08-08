#include "clib.h"
#include "hidden.h"
#include "logit_stat.h"

/* ls [-a] [-l] [dir] -- list a directory (default: cwd). Directories get a '/'.
 *
 * -a shows dot-prefixed entries. The system directories are NOT filtered here:
 * see c/apps/hidden.h -- a shell that hides /bin is lying to the person who
 * typed the command.
 *
 * -l EXISTS NOW AND IS HONEST, which it could not have been before: until
 * SYS_STAT there was no way to learn a mode, an owner, a link count or a
 * timestamp, so a long listing could only have printed invented constants.
 * Every column below comes off the medium. The one that needs a word:
 *
 *   a mode with no LSTA_MODE_STORED bit is the DEFAULT for a file nobody has
 *   ever chmod'd, not a choice anyone made. `ls -l` marks it with a trailing
 *   '.' in the mode column -- the same place GNU ls puts its SELinux dot, and
 *   for a related reason: the ten characters to its left are not the whole
 *   truth about this file's permissions.
 *
 * Enumeration is SYS_GETDENTS, so a long listing of a 200-entry directory is a
 * few syscalls plus one stat per entry, not two syscalls per entry. */

static void pad_to(int have, int want) { for (int i = have; i < want; i++) outc(' '); }

static int digits(unsigned long long v)
{ int n = 1; while (v >= 10) { v /= 10; n++; } return n; }

/* Seconds since the epoch -> "YYYY-MM-DD HH:MM". Civil-from-days (Howard
 * Hinnant's algorithm), which is exact for every date and needs no table. */
static void putime(long long t)
{
    if (t <= 0) { outs("       -         "); return; }
    long long days = t / 86400, secs = t % 86400;
    if (secs < 0) { secs += 86400; days--; }
    long long z = days + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned long long doe = (unsigned long long)(z - era * 146097);
    unsigned long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = (long long)yoe + era * 400;
    unsigned long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned long long mp = (5 * doy + 2) / 153;
    unsigned long long d = doy - (153 * mp + 2) / 5 + 1;
    unsigned long long m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);

    char b[20]; int k = 0;
    unsigned long long yy = (unsigned long long)y;
    b[k++] = (char)('0' + (yy / 1000) % 10); b[k++] = (char)('0' + (yy / 100) % 10);
    b[k++] = (char)('0' + (yy / 10) % 10);   b[k++] = (char)('0' + yy % 10);
    b[k++] = '-'; b[k++] = (char)('0' + m / 10); b[k++] = (char)('0' + m % 10);
    b[k++] = '-'; b[k++] = (char)('0' + d / 10); b[k++] = (char)('0' + d % 10);
    b[k++] = ' ';
    long long hh = secs / 3600, mi = (secs / 60) % 60;
    b[k++] = (char)('0' + hh / 10); b[k++] = (char)('0' + hh % 10); b[k++] = ':';
    b[k++] = (char)('0' + mi / 10); b[k++] = (char)('0' + mi % 10);
    b[k] = 0;
    outs(b);
}

int main(int argc, char **argv)
{
    char path[128];
    int all = 0, lng = 0, have_path = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] && !have_path) {
            for (int j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 'a') all = 1;
                else if (argv[i][j] == 'l') lng = 1;
                else { errs("ls: unknown flag: "); errs(argv[i]); errs("\n"); return 1; }
            }
        } else { c_strcpy(path, argv[i], sizeof path); have_path = 1; }
    }
    if (!have_path) sys_getcwd(path, sizeof path);

    if (dir_count(path) < 0) { errs("ls: no such directory: "); errs(path); errs("\n"); return 1; }

    /* Two passes when -l, so the size column can be as wide as it needs to be
     * and no wider. Without it a 4 MB font next to a 12-byte file wastes eight
     * columns on every line of the listing. */
    unsigned long long widest = 0;
    if (lng) {
        int cur = 0;
        struct logit_dirent e[8];
        int n;
        while ((n = st_getdents(path, &cur, e, 8)) > 0)
            for (int i = 0; i < n; i++) {
                if (!all && hidden_dotfile(e[i].name)) continue;
                if (e[i].size > widest) widest = e[i].size;
            }
    }
    int szw = lng ? digits(widest) : 0;

    int cursor = 0, n;
    struct logit_dirent ents[8];
    while ((n = st_getdents(path, &cursor, ents, 8)) > 0) {
        for (int i = 0; i < n; i++) {
            struct logit_dirent *e = &ents[i];
            if (!e->name[0]) continue;
            if (!all && hidden_dotfile(e->name)) continue;

            if (lng) {
                char full[192];
                path_join(full, path, e->name, sizeof full);
                struct logit_stat st;
                /* lstat: a long listing describes the NAME, so a symlink shows
                 * as a symlink rather than as whatever it points at. */
                if (st_lstat(full, &st) < 0) { outs("?????????? "); }
                else {
                    char m[11];
                    st_modestr(st.mode, m);
                    outs(m);
                    /* The dot: these bits are the default, not a decision. */
                    outc((st.attr & LSTA_MODE_STORED) ? ' ' : '.');
                    outc(' ');
                    outn(st.nlink);
                    outc(' ');
                    outn(st.uid); outc(':'); outn(st.gid);
                    outc(' ');
                    int d = digits(st.size);
                    pad_to(d, szw);
                    outn((long)st.size);
                    outc(' ');
                    putime(st.mtime);
                    outc(' ');
                }
            }

            outs(e->name);
            if (e->type == LST_IFDIR) outc('/');
            if (e->type == LST_IFLNK) {
                char full[192], tgt[128];
                path_join(full, path, e->name, sizeof full);
                int k = st_readlink(full, tgt, (int)sizeof tgt - 1);
                if (k > 0) { tgt[k] = 0; outs(" -> "); outs(tgt); }
            }
            outc('\n');
        }
    }
    return 0;
}
