#include <stdint.h>
#include <stddef.h>
#include "file.h"
#include "kheap.h"

/* Open-file-description pool. Small fixed pool: every fd in every process points
 * at one of these; dup/fork bump refcount rather than copying. */
#define NFILE 64
static struct file files[NFILE];

void file_init(void)
{
    for (int i = 0; i < NFILE; i++) files[i].type = F_NONE, files[i].refcount = 0;
}

struct file *file_alloc(void)
{
    for (int i = 0; i < NFILE; i++) {
        if (files[i].refcount == 0) {
            struct file *f = &files[i];
            f->type = F_NONE; f->refcount = 1; f->flags = 0;
            f->off = 0; f->size = 0; f->cap = 0; f->dirty = 0;
            f->backing = 0; f->path[0] = 0;
            return f;
        }
    }
    return 0;
}

void file_dup(struct file *f) { if (f) f->refcount++; }

/* --- backends: filled in by P2 (VFS), P3 (pipe), P5 (tty). P1 stubs. --- */
struct file *file_open_vfs(const char *path, int flags) { (void)path; (void)flags; return 0; }
long file_read(struct file *f, void *buf, long len)  { (void)f; (void)buf; (void)len; return -1; }
long file_write(struct file *f, const void *buf, long len) { (void)f; (void)buf; (void)len; return -1; }
long file_lseek(struct file *f, long off, int whence) { (void)f; (void)off; (void)whence; return -1; }
int  file_pipe(struct file **rd, struct file **wr) { (void)rd; (void)wr; return -1; }

/* Release a reference. When the last one goes, release the backend. The F_VFS
 * write-back and F_PIPE end-tracking are layered on in P2/P3; here we just free
 * the generic backing buffer and return the slot to the pool. */
void file_close(struct file *f)
{
    if (!f || f->refcount <= 0) return;
    if (--f->refcount > 0) return;
    if (f->backing) kfree(f->backing);
    f->backing = 0;
    f->type = F_NONE;
}
