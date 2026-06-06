#ifndef AQUA_FILE_H
#define AQUA_FILE_H

#include <stdint.h>

/* An open file description (the thing a file descriptor points at). Shared by
 * dup/dup2/fork via refcount. Three backends:
 *   F_VFS  -- a regular file; `backing` holds the whole file in a kmalloc buffer
 *             with an offset cursor (P2). Avoids touching aquafs block logic.
 *   F_PIPE -- an in-kernel ring buffer with two ends (P3).
 *   F_TTY  -- the serial console (P5).                                       */
#define F_NONE 0
#define F_VFS  1
#define F_PIPE 2
#define F_TTY  3

struct file {
    int   type;
    int   refcount;
    int   flags;        /* O_* (O_NONBLOCK / O_APPEND / ...) */
    int   is_write;     /* F_PIPE: 1 = write end, 0 = read end (kept out of `flags`
                         * so a SYS_SETNB O_NONBLOCK can't flip the close accounting) */
    long  off;          /* F_VFS cursor */
    long  size;         /* F_VFS current length */
    long  cap;          /* F_VFS buffer capacity */
    int   dirty;        /* F_VFS needs write-back at last close */
    void *backing;      /* F_VFS: file bytes; F_PIPE: struct pipe* */
    char  path[128];    /* F_VFS write-back path */
};

void          file_init(void);
struct file  *file_alloc(void);     /* a fresh F_NONE file, refcount = 1 */
void          file_dup(struct file *f);    /* refcount++ */
void          file_close(struct file *f);  /* refcount--; release backend at 0 */

/* Backends (P2/P3/P5). */
struct file  *file_open_vfs(const char *path, int flags);   /* P2 */
struct file  *file_open_tty(void);                          /* P5: serial console */
long          file_read(struct file *f, void *buf, long len);
long          file_write(struct file *f, const void *buf, long len);
long          file_lseek(struct file *f, long off, int whence);
int           file_pipe(struct file **rd, struct file **wr);  /* P3 */

#endif /* AQUA_FILE_H */
