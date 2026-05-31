#include <stdint.h>
#include <stddef.h>
#include "aquafs.h"
#include "ata.h"
#include "kheap.h"
#include "kprintf.h"

void *memcpy(void *, const void *, size_t);   /* lib/string.c */
void *memset(void *, int, size_t);

/* --- on-disk layout (must match tools/mkfs.py) --- */
#define SECTOR     512
#define BS         4096                 /* block size */
#define SPB        (BS / SECTOR)        /* sectors per block (8) */
#define MAGIC      0x41515541u          /* "AQUA" */
#define VERSION    3
#define INODE_SIZE 128
#define NDIRECT    12
#define PPB        (BS / 4)             /* u32 pointers per indirect block */
#define DIRENT_SZ  64
#define NAME_MAX   60
#define T_FREE     0
#define T_FILE     1
#define T_DIR      2
#define MAX_PATH   128
#define NOINO      0xFFFFFFFFu

struct dinode {                         /* 128 bytes on disk */
    uint16_t type;
    uint16_t pad;
    uint32_t size;
    uint32_t direct[NDIRECT];
    uint32_t indirect;
    uint8_t  reserved[INODE_SIZE - 8 - NDIRECT * 4 - 4];
} __attribute__((packed));

struct dirent {                         /* 64 bytes on disk */
    uint32_t ino;
    char     name[NAME_MAX];
} __attribute__((packed));

static struct {
    uint32_t magic, version, block_size, total_blocks, inode_count;
    uint32_t bitmap_start, bitmap_blocks, inode_start, inode_blocks;
    uint32_t data_start, root_ino;
} sb;

static uint8_t      *bitmap;            /* bitmap_blocks * BS, in RAM */
static struct dinode *inodes;           /* inode_blocks  * BS, in RAM */

static uint8_t  blk_buf[BS];            /* general block staging */
static uint32_t ind_buf[PPB];           /* indirect-block staging */
static char     namebuf[NAME_MAX];      /* ent_name return storage */

/* --- helpers --- */
static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int bread(uint32_t blk, void *buf)        { return ata_read(blk * SPB, SPB, buf); }
static int bwrite(uint32_t blk, const void *buf) { return ata_write(blk * SPB, SPB, buf); }

static int  bit_test(uint32_t b)  { return bitmap[b >> 3] & (1 << (b & 7)); }
static void bit_set(uint32_t b)   { bitmap[b >> 3] |=  (1 << (b & 7)); }
static void bit_clear(uint32_t b) { bitmap[b >> 3] &= ~(1 << (b & 7)); }

static uint32_t balloc(void)
{
    for (uint32_t b = sb.data_start; b < sb.total_blocks; b++)
        if (!bit_test(b)) { bit_set(b); return b; }
    return 0;                            /* disk full (0 = none) */
}
static void bfree(uint32_t b) { if (b) bit_clear(b); }

static struct dinode *iget(uint32_t ino)
{
    return (ino < sb.inode_count) ? &inodes[ino] : NULL;
}

static int ialloc(uint16_t type)
{
    for (uint32_t i = 0; i < sb.inode_count; i++)
        if (inodes[i].type == T_FREE) {
            memset(&inodes[i], 0, sizeof(struct dinode));
            inodes[i].type = type;
            return (int)i;
        }
    return -1;
}

/* block number of a file's i-th data block (clobbers ind_buf), or 0 */
static uint32_t imap(struct dinode *in, uint32_t i)
{
    if (i < NDIRECT) return in->direct[i];
    i -= NDIRECT;
    if (!in->indirect || i >= PPB) return 0;
    if (bread(in->indirect, ind_buf)) return 0;
    return ind_buf[i];
}

static int flush_meta(void)             /* persist bitmap + inode table */
{
    for (uint32_t i = 0; i < sb.bitmap_blocks; i++)
        if (bwrite(sb.bitmap_start + i, bitmap + i * BS)) return -1;
    for (uint32_t i = 0; i < sb.inode_blocks; i++)
        if (bwrite(sb.inode_start + i, (uint8_t *)inodes + i * BS)) return -1;
    return 0;
}

/* --- whole-file I/O --- */
static int inode_read(struct dinode *in, void *buf, int max)
{
    uint32_t size = in->size;
    if ((int)size > max) return -1;
    uint8_t  *out  = buf;
    uint32_t  nblk = (size + BS - 1) / BS;
    int       have_ind = 0;
    for (uint32_t i = 0; i < nblk; i++) {
        uint32_t blk;
        if (i < NDIRECT) blk = in->direct[i];
        else {
            if (!have_ind) { if (bread(in->indirect, ind_buf)) return -1; have_ind = 1; }
            blk = ind_buf[i - NDIRECT];
        }
        uint32_t off = i * BS;
        uint32_t n   = size - off < BS ? size - off : BS;
        if (n == BS) { if (bread(blk, out + off)) return -1; }
        else { if (bread(blk, blk_buf)) return -1; memcpy(out + off, blk_buf, n); }
    }
    return (int)size;
}

static void inode_trunc(struct dinode *in)
{
    uint32_t nblk = (in->size + BS - 1) / BS;
    for (uint32_t i = 0; i < nblk && i < NDIRECT; i++) { bfree(in->direct[i]); in->direct[i] = 0; }
    if (in->indirect) {
        if (!bread(in->indirect, ind_buf))
            for (uint32_t i = NDIRECT; i < nblk; i++) bfree(ind_buf[i - NDIRECT]);
        bfree(in->indirect);
        in->indirect = 0;
    }
    in->size = 0;
}

static int inode_write(struct dinode *in, const void *buf, int size)
{
    if (size < 0) return -1;
    uint32_t nblk = ((uint32_t)size + BS - 1) / BS;
    if (nblk > NDIRECT + PPB) return -1;        /* exceeds single-indirect reach */

    inode_trunc(in);
    const uint8_t *src = buf;
    uint32_t ind = 0;
    if (nblk > NDIRECT) { ind = balloc(); if (!ind) return -1; memset(ind_buf, 0, BS); }

    for (uint32_t i = 0; i < nblk; i++) {
        uint32_t blk = balloc();
        if (!blk) return -1;
        uint32_t off = i * BS;
        uint32_t n   = (uint32_t)size - off < BS ? (uint32_t)size - off : BS;
        if (n == BS) { if (bwrite(blk, src + off)) return -1; }
        else { memset(blk_buf, 0, BS); memcpy(blk_buf, src + off, n); if (bwrite(blk, blk_buf)) return -1; }
        if (i < NDIRECT) in->direct[i] = blk;
        else ind_buf[i - NDIRECT] = blk;
    }
    if (nblk > NDIRECT) { in->indirect = ind; if (bwrite(ind, ind_buf)) return -1; }
    in->size = (uint32_t)size;
    return size;
}

/* --- directory ops --- */
static uint32_t dir_lookup(struct dinode *d, const char *name)
{
    uint32_t sz = d->size, nblk = (sz + BS - 1) / BS;
    for (uint32_t bi = 0; bi < nblk; bi++) {
        uint32_t blk = imap(d, bi);
        if (!blk || bread(blk, blk_buf)) continue;
        struct dirent *de = (struct dirent *)blk_buf;
        for (int j = 0; j < BS / DIRENT_SZ; j++) {
            if (bi * BS + (uint32_t)j * DIRENT_SZ >= sz) break;
            if (de[j].name[0] && streq(de[j].name, name)) return de[j].ino;
        }
    }
    return 0;                            /* not found (root ino 0 is never a child) */
}

/* enumerate: return ino of the idx-th live entry of dir `dino`, fill nameout */
static uint32_t dir_nth(uint32_t dino, int idx, char *nameout)
{
    struct dinode *d = iget(dino);
    if (!d || d->type != T_DIR) return 0;
    uint32_t sz = d->size, nblk = (sz + BS - 1) / BS;
    int seen = 0;
    for (uint32_t bi = 0; bi < nblk; bi++) {
        uint32_t blk = imap(d, bi);
        if (!blk || bread(blk, blk_buf)) continue;
        struct dirent *de = (struct dirent *)blk_buf;
        for (int j = 0; j < BS / DIRENT_SZ; j++) {
            if (bi * BS + (uint32_t)j * DIRENT_SZ >= sz) break;
            if (de[j].name[0] == 0) continue;
            if (seen == idx) {
                if (nameout) {
                    int k = 0;
                    while (de[j].name[k] && k < NAME_MAX - 1) { nameout[k] = de[j].name[k]; k++; }
                    nameout[k] = 0;
                }
                return de[j].ino;
            }
            seen++;
        }
    }
    return 0;
}

static int dir_count_live(uint32_t dino)
{
    int n = 0;
    while (dir_nth(dino, n, NULL)) n++;
    return n;
}

static int dir_is_empty(struct dinode *d)
{
    uint32_t sz = d->size, nblk = (sz + BS - 1) / BS;
    for (uint32_t bi = 0; bi < nblk; bi++) {
        uint32_t blk = imap(d, bi);
        if (!blk || bread(blk, blk_buf)) continue;
        struct dirent *de = (struct dirent *)blk_buf;
        for (int j = 0; j < BS / DIRENT_SZ; j++) {
            if (bi * BS + (uint32_t)j * DIRENT_SZ >= sz) break;
            if (de[j].name[0]) return 0;
        }
    }
    return 1;
}

static int dir_add(uint32_t dino, const char *name, uint32_t child)
{
    struct dinode *d = iget(dino);
    uint32_t old = d->size, cap = old + DIRENT_SZ;
    uint8_t *buf = kmalloc(cap);
    if (!buf) return -1;
    memset(buf, 0, cap);
    if (old && inode_read(d, buf, (int)old) < 0) { kfree(buf); return -1; }

    struct dirent *de = (struct dirent *)buf;
    int n = (int)(old / DIRENT_SZ), slot = -1;
    for (int i = 0; i < n; i++) if (de[i].name[0] == 0) { slot = i; break; }
    uint32_t newsize = old;
    if (slot < 0) { slot = n; newsize = old + DIRENT_SZ; }

    de[slot].ino = child;
    int k = 0;
    while (name[k] && k < NAME_MAX - 1) { de[slot].name[k] = name[k]; k++; }
    de[slot].name[k] = 0;

    int rc = inode_write(d, buf, (int)newsize);
    kfree(buf);
    return rc < 0 ? -1 : 0;
}

static int dir_remove(uint32_t dino, const char *name)
{
    struct dinode *d = iget(dino);
    uint32_t sz = d->size;
    if (!sz) return -1;
    uint8_t *buf = kmalloc(sz);
    if (!buf) return -1;
    if (inode_read(d, buf, (int)sz) < 0) { kfree(buf); return -1; }

    struct dirent *de = (struct dirent *)buf;
    int n = (int)(sz / DIRENT_SZ), found = -1;
    for (int i = 0; i < n; i++) if (de[i].name[0] && streq(de[i].name, name)) { found = i; break; }
    if (found < 0) { kfree(buf); return -1; }
    de[found].name[0] = 0;
    de[found].ino = 0;

    int rc = inode_write(d, buf, (int)sz);
    kfree(buf);
    return rc < 0 ? -1 : 0;
}

/* --- path resolution --- */
static uint32_t resolve(const char *path)
{
    uint32_t stack[MAX_PATH / 2];
    int sp = 0;
    stack[sp++] = sb.root_ino;
    uint32_t cur = sb.root_ino;
    const char *p = path;
    char comp[NAME_MAX];

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        int k = 0;
        while (*p && *p != '/' && k < NAME_MAX - 1) comp[k++] = *p++;
        comp[k] = 0;
        while (*p && *p != '/') p++;          /* skip an over-long component tail */

        if (comp[0] == '.' && comp[1] == 0) continue;
        if (comp[0] == '.' && comp[1] == '.' && comp[2] == 0) {
            if (sp > 1) sp--;
            cur = stack[sp - 1];
            continue;
        }
        struct dinode *d = iget(cur);
        if (!d || d->type != T_DIR) return NOINO;
        uint32_t child = dir_lookup(d, comp);
        if (!child) return NOINO;
        cur = child;
        if (sp < (int)(sizeof stack / sizeof stack[0])) stack[sp++] = cur;
    }
    return cur;
}

/* split path into (parent dir ino, leaf name); NOINO on error */
static uint32_t resolve_parent(const char *path, char *leaf)
{
    int len = 0; while (path[len]) len++;
    int e = len; while (e > 0 && path[e - 1] == '/') e--;
    int s = e;   while (s > 0 && path[s - 1] != '/') s--;

    int k = 0;
    for (int i = s; i < e && k < NAME_MAX - 1; i++) leaf[k++] = path[i];
    leaf[k] = 0;
    if (k == 0) return NOINO;

    char dirpath[MAX_PATH];
    int dl = 0;
    for (int i = 0; i < s && dl < MAX_PATH - 1; i++) dirpath[dl++] = path[i];
    dirpath[dl] = 0;
    if (dl == 0) { dirpath[0] = '/'; dirpath[1] = 0; }
    return resolve(dirpath);
}

/* --- VFS ops --- */
static int aquafs_mount(void)
{
    uint8_t b0[SECTOR];
    if (ata_read(0, 1, b0)) return -1;
    uint32_t *w = (uint32_t *)b0;
    if (w[0] != MAGIC || w[1] != VERSION || w[2] != BS) return -1;
    sb.magic = w[0]; sb.version = w[1]; sb.block_size = w[2];
    sb.total_blocks = w[3]; sb.inode_count = w[4];
    sb.bitmap_start = w[5]; sb.bitmap_blocks = w[6];
    sb.inode_start = w[7]; sb.inode_blocks = w[8];
    sb.data_start = w[9]; sb.root_ino = w[10];

    bitmap = kmalloc(sb.bitmap_blocks * BS);
    inodes = kmalloc(sb.inode_blocks * BS);
    if (!bitmap || !inodes) return -1;
    for (uint32_t i = 0; i < sb.bitmap_blocks; i++)
        if (bread(sb.bitmap_start + i, bitmap + i * BS)) return -1;
    for (uint32_t i = 0; i < sb.inode_blocks; i++)
        if (bread(sb.inode_start + i, (uint8_t *)inodes + i * BS)) return -1;
    return 0;
}

static int aquafs_size(const char *path)
{
    uint32_t ino = resolve(path);
    if (ino == NOINO) return -1;
    struct dinode *in = iget(ino);
    return in ? (int)in->size : -1;
}

static int aquafs_read(const char *path, void *buf, int max)
{
    uint32_t ino = resolve(path);
    if (ino == NOINO) return -1;
    struct dinode *in = iget(ino);
    if (!in || in->type != T_FILE) return -1;
    return inode_read(in, buf, max);
}

static int aquafs_write(const char *path, const void *buf, int size)
{
    uint32_t ino = resolve(path);
    if (ino != NOINO) {                          /* overwrite existing file */
        struct dinode *in = iget(ino);
        if (!in || in->type != T_FILE) return -1;
        if (inode_write(in, buf, size) < 0) return -1;
        return flush_meta() ? -1 : size;
    }
    char leaf[NAME_MAX];                          /* else create */
    uint32_t parent = resolve_parent(path, leaf);
    if (parent == NOINO) return -1;
    struct dinode *pd = iget(parent);
    if (!pd || pd->type != T_DIR) return -1;
    int ni = ialloc(T_FILE);
    if (ni < 0) return -1;
    struct dinode *in = iget((uint32_t)ni);
    if (inode_write(in, buf, size) < 0) { in->type = T_FREE; return -1; }
    if (dir_add(parent, leaf, (uint32_t)ni) < 0) { inode_trunc(in); in->type = T_FREE; return -1; }
    return flush_meta() ? -1 : size;
}

static int aquafs_mkdir(const char *path)
{
    if (resolve(path) != NOINO) return -1;        /* already exists */
    char leaf[NAME_MAX];
    uint32_t parent = resolve_parent(path, leaf);
    if (parent == NOINO) return -1;
    struct dinode *pd = iget(parent);
    if (!pd || pd->type != T_DIR) return -1;
    int ni = ialloc(T_DIR);
    if (ni < 0) return -1;
    if (dir_add(parent, leaf, (uint32_t)ni) < 0) { inodes[ni].type = T_FREE; return -1; }
    return flush_meta() ? -1 : 0;
}

static int aquafs_delete(const char *path)
{
    uint32_t ino = resolve(path);
    if (ino == NOINO || ino == sb.root_ino) return -1;
    struct dinode *in = iget(ino);
    if (!in) return -1;
    if (in->type == T_DIR && !dir_is_empty(in)) return -1;
    char leaf[NAME_MAX];
    uint32_t parent = resolve_parent(path, leaf);
    if (parent == NOINO) return -1;
    if (dir_remove(parent, leaf) < 0) return -1;
    inode_trunc(in);
    in->type = T_FREE;
    return flush_meta() ? -1 : 0;
}

/* Directory-scoped enumeration. */
static uint32_t resolve_dir(const char *dir)
{
    uint32_t ino = resolve(dir);
    if (ino == NOINO) return NOINO;
    struct dinode *d = iget(ino);
    return (d && d->type == T_DIR) ? ino : NOINO;
}

static int aquafs_count(const char *dir)
{
    uint32_t ino = resolve_dir(dir);
    return ino == NOINO ? -1 : dir_count_live(ino);   /* -1: not a directory */
}

static const char *aquafs_ent_name(const char *dir, int i)
{
    namebuf[0] = 0;
    uint32_t ino = resolve_dir(dir);
    if (ino != NOINO) dir_nth(ino, i, namebuf);
    return namebuf;
}

static int aquafs_ent_size(const char *dir, int i)
{
    uint32_t dino = resolve_dir(dir);
    if (dino == NOINO) return 0;
    uint32_t ino = dir_nth(dino, i, NULL);
    struct dinode *in = ino ? iget(ino) : NULL;
    return in ? (int)in->size : 0;
}

static int aquafs_ent_is_dir(const char *dir, int i)
{
    uint32_t dino = resolve_dir(dir);
    if (dino == NOINO) return 0;
    uint32_t ino = dir_nth(dino, i, NULL);
    struct dinode *in = ino ? iget(ino) : NULL;
    return in && in->type == T_DIR;
}

static void aquafs_list(void)
{
    int n = dir_count_live(sb.root_ino);
    kprintf("[fs] AquaFS v3: %d entr(ies) in /:\n", n);
    for (int i = 0; i < n; i++) {
        char nm[NAME_MAX]; nm[0] = 0;
        uint32_t ino = dir_nth(sb.root_ino, i, nm);
        struct dinode *in = iget(ino);
        kprintf("[fs]   %-16s %s %u bytes\n", nm,
                (in && in->type == T_DIR) ? "<dir>" : "     ",
                in ? in->size : 0);
    }
}

struct filesystem aquafs = {
    .name     = "aquafs",
    .mount    = aquafs_mount,
    .list     = aquafs_list,
    .size     = aquafs_size,
    .read     = aquafs_read,
    .count      = aquafs_count,
    .ent_name   = aquafs_ent_name,
    .ent_size   = aquafs_ent_size,
    .ent_is_dir = aquafs_ent_is_dir,
    .write    = aquafs_write,
    .del      = aquafs_delete,
    .mkdir    = aquafs_mkdir,
};
