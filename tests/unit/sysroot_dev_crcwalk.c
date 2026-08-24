/* sysroot_dev_crcwalk.c -- walk a directory tree ON THE DEVICE and print one
 * line per regular file: CRC-32, size, path; then one total line.
 *
 * It is COMPILED ON THE DEVICE by /bin/tcc against the packed sysroot, with
 * no flags (tests/boot/run-sysroot-device.sh), so it is three measurements
 * at once: that <dirent.h>/<sys/stat.h>/<stdio.h>/<stdlib.h>/<string.h> parse
 * under tcc from /usr/include, that opendir/readdir/stat/fopen/qsort link out
 * of /usr/lib/libc.a, and -- the one it exists for -- that every byte of
 * every file under /usr/include and /usr/lib on the disk is the byte the
 * host packed. The host oracle is python's zlib.crc32 over build/sysroot;
 * the polynomial is zlib's (0xEDB88320, reflected) so no second table is
 * typed anywhere. A CRC rather than cat-ing 674 KB over a serial console,
 * and every file rather than one: a packer that gets dirents right and data
 * blocks wrong can put the right bytes in stdio.h and somebody else's in
 * signal.h (CLAUDE.md, Storage: never a length check).
 *
 * Names are SORTED before descent, so the output order is a function of the
 * tree's contents and the host diff is line-for-line, not set-against-set.
 * Written in the C tcc 0.9.27 accepts: no VLAs, no _Static_assert, no
 * math.h (its NAN/isnan are clang builtins tcc lacks -- sysroot_hdr_test). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#define MAXENT  512          /* entries per directory; 64 is the most here */
#define NAMEW   64           /* LFS_NAME_MAX is 59 usable bytes */

static unsigned long tab[256];
static long nfiles, ndirs, nbytes, nerr;

static void crc_init(void)
{
    unsigned i; int k;
    for (i = 0; i < 256; i++) {
        unsigned long c = i;
        for (k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
        tab[i] = c;
    }
}

static int crc_file(const char *path, unsigned long *crc, long *size)
{
    static unsigned char buf[4096];
    unsigned long c = 0xFFFFFFFFUL;
    long total = 0;
    size_t n, i;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        for (i = 0; i < n; i++) c = tab[(c ^ buf[i]) & 0xff] ^ (c >> 8);
        total += (long)n;
    }
    fclose(f);
    *crc = (c ^ 0xFFFFFFFFUL) & 0xFFFFFFFFUL;
    *size = total;
    return 0;
}

static int cmpname(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static void walk(const char *dir)
{
    /* One directory is read completely and its names copied out BEFORE any
     * descent, so the static scratch array is never live across recursion.
     * (A per-call array would be 32 KB of stack per level.) */
    static char names[MAXENT][NAMEW];
    char (*mine)[NAMEW];
    struct dirent *e;
    int n = 0, i;
    DIR *d = opendir(dir);
    if (!d) { printf("ERR opendir %s\n", dir); nerr++; return; }
    ndirs++;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n >= MAXENT) { printf("ERR too many entries in %s\n", dir); nerr++; break; }
        strncpy(names[n], e->d_name, NAMEW - 1);
        names[n][NAMEW - 1] = 0;
        n++;
    }
    closedir(d);
    qsort(names, (size_t)n, NAMEW, cmpname);
    mine = malloc((size_t)(n > 0 ? n : 1) * NAMEW);
    if (!mine) { printf("ERR malloc\n"); nerr++; return; }
    memcpy(mine, names, (size_t)n * NAMEW);
    for (i = 0; i < n; i++) {
        char path[512];
        struct stat st;
        unsigned long crc; long size;
        if (strlen(dir) + 1 + strlen(mine[i]) + 1 > sizeof path) { printf("ERR path too long\n"); nerr++; continue; }
        strcpy(path, dir);
        if (path[strlen(path) - 1] != '/') strcat(path, "/");
        strcat(path, mine[i]);
        if (stat(path, &st) < 0) { printf("ERR stat %s\n", path); nerr++; continue; }
        if (S_ISDIR(st.st_mode)) {
            walk(path);
        } else if (S_ISREG(st.st_mode)) {
            if (crc_file(path, &crc, &size) < 0) { printf("ERR open %s\n", path); nerr++; continue; }
            if (size != (long)st.st_size) {
                printf("ERR size %s: read %ld, stat says %ld\n", path, size, (long)st.st_size);
                nerr++;
            }
            printf("%08lx %ld %s\n", crc, size, path);
            nfiles++;
            nbytes += size;
        } else {
            printf("ERR not a file or directory: %s\n", path); nerr++;
        }
    }
    free(mine);
}

int main(int argc, char **argv)
{
    int i;
    crc_init();
    if (argc < 2) { printf("usage: crcwalk DIR...\n"); return 2; }
    for (i = 1; i < argc; i++) walk(argv[i]);
    printf("CRCWALK files=%ld dirs=%ld bytes=%ld errors=%ld\n", nfiles, ndirs, nbytes, nerr);
    return nerr ? 1 : 0;
}
