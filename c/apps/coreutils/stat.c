#include "clib.h"
#include "logit_stat.h"

/* stat [-L] PATH...        print a file's metadata
 * stat -c MODE PATH        chmod
 * stat -o UID:GID PATH     chown
 * stat -u [MASK]           umask (no MASK = query)
 * stat -s TARGET LINK      symlink
 * stat -h OLD NEW          hard link
 *
 * A coreutil rather than a test fixture, because the point of the metadata work
 * is that a program can ASK. It exists in this shape for one more reason: the
 * on-device durability harness (tests/boot/run-statmeta-test.sh) sets a mode in
 * one boot and reads it back in the next, and it needs a reader whose output is
 * a stable line that grep can judge, not a formatted table.
 *
 * The output line, one per path:
 *
 *   STAT <path> mode=<octal> type=<reg|dir|lnk|chr|fifo> uid=<n> gid=<n>
 *        nlink=<n> ino=<n> dev=<n> size=<n> blocks=<n>
 *        mtime=<n> ctime=<n> atime=<n> attr=<hex>
 *
 * `attr` is the field to read before believing `mode`: without LSTA_MODE_STORED
 * (0x1) that mode is the default nobody chose, and without LSTA_MODE_DURABLE
 * (0x2) it will not be there after a reboot. Printing the mode and not the
 * provenance is exactly the quiet lie this whole line exists to remove, so it
 * is on every line whether or not anyone reads it. */

static void put_oct(unsigned long v)
{
    char t[16]; int k = 0;
    if (!v) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + (v & 7)); v >>= 3; }
    while (k) outc(t[--k]);
}

static void put_hex(unsigned long v)
{
    char t[20]; int k = 0;
    const char *d = "0123456789abcdef";
    if (!v) t[k++] = '0';
    while (v) { t[k++] = d[v & 15]; v >>= 4; }
    outs("0x");
    while (k) outc(t[--k]);
}

static const char *typename_of(unsigned int mode)
{
    switch (mode & LST_IFMT) {
    case LST_IFDIR:  return "dir";
    case LST_IFLNK:  return "lnk";
    case LST_IFCHR:  return "chr";
    case LST_IFIFO:  return "fifo";
    default:         return "reg";
    }
}

static void kv(const char *k, long long v) { outs(k); outn((long)v); }

static int show(const char *path, int follow)
{
    struct logit_stat s;
    int rc = follow ? st_stat(path, &s) : st_lstat(path, &s);
    if (rc < 0) { errs("stat: cannot stat "); errs(path); errs("\n"); return 1; }

    outs("STAT "); outs(path);
    outs(" mode="); put_oct(s.mode & 07777);
    outs(" type="); outs(typename_of(s.mode));
    kv(" uid=", s.uid); kv(" gid=", s.gid); kv(" nlink=", s.nlink);
    kv(" ino=", (long long)s.ino); kv(" dev=", (long long)s.dev);
    kv(" size=", (long long)s.size); kv(" blocks=", (long long)s.blocks);
    kv(" mtime=", s.mtime); kv(" ctime=", s.ctime); kv(" atime=", s.atime);
    outs(" attr="); put_hex(s.attr);
    outc('\n');

    if ((s.mode & LST_IFMT) == LST_IFLNK) {
        char tgt[128];
        int n = st_readlink(path, tgt, (int)sizeof tgt - 1);
        if (n > 0) { tgt[n] = 0; outs("STAT "); outs(path); outs(" target="); outs(tgt); outc('\n'); }
    }
    return 0;
}

static int oct(const char *s)
{ int v = 0; while (*s >= '0' && *s <= '7') v = v * 8 + (*s++ - '0'); return v; }

int main(int argc, char **argv)
{
    if (argc < 2) { errs("usage: stat [-L] PATH... | -c MODE PATH | -o U:G PATH | -u [MASK] | -s T L | -h O N\n"); return 1; }

    if (c_streq(argv[1], "-c")) {
        if (argc < 4) { errs("stat: -c MODE PATH\n"); return 1; }
        if (st_chmod(argv[3], oct(argv[2])) < 0) { errs("stat: chmod refused: "); errs(argv[3]); errs("\n"); return 1; }
        outs("STAT-CHMOD-OK "); outs(argv[3]); outc('\n');
        return 0;
    }
    if (c_streq(argv[1], "-o")) {
        if (argc < 4) { errs("stat: -o UID:GID PATH\n"); return 1; }
        const char *p = argv[2];
        int uid = c_atoi(p), gid = 0;
        while (*p && *p != ':') p++;
        if (*p == ':') gid = c_atoi(p + 1);
        if (st_chown(argv[3], uid, gid) < 0) { errs("stat: chown refused: "); errs(argv[3]); errs("\n"); return 1; }
        outs("STAT-CHOWN-OK "); outs(argv[3]); outc('\n');
        return 0;
    }
    if (c_streq(argv[1], "-u")) {
        int prev = st_umask(argc >= 3 ? oct(argv[2]) : -1);
        outs("STAT-UMASK "); put_oct((unsigned long)prev); outc('\n');
        return 0;
    }
    if (c_streq(argv[1], "-s")) {
        if (argc < 4) { errs("stat: -s TARGET LINK\n"); return 1; }
        if (st_symlink(argv[2], argv[3]) < 0) { errs("stat: symlink failed\n"); return 1; }
        outs("STAT-SYMLINK-OK "); outs(argv[3]); outc('\n');
        return 0;
    }
    if (c_streq(argv[1], "-h")) {
        if (argc < 4) { errs("stat: -h OLD NEW\n"); return 1; }
        if (st_link(argv[2], argv[3]) < 0) { errs("stat: link failed\n"); return 1; }
        outs("STAT-LINK-OK "); outs(argv[3]); outc('\n');
        return 0;
    }

    int follow = 1, first = 1, bad = 0;
    for (int i = 1; i < argc; i++) {
        if (first && c_streq(argv[i], "-L")) { follow = 1; continue; }
        if (first && c_streq(argv[i], "-P")) { follow = 0; continue; }
        first = 0;
        bad |= show(argv[i], follow);
    }
    return bad;
}
