#ifndef LOGIT_FILE_H
#define LOGIT_FILE_H

#include <stdint.h>

/* An open file description (the thing a file descriptor points at). Shared by
 * dup/dup2/fork via refcount. Three backends:
 *   F_VFS  -- a regular file; `backing` holds the whole file in a kmalloc buffer
 *             with an offset cursor (P2). Avoids touching logitfs block logic.
 *   F_PIPE -- an in-kernel ring buffer with two ends (P3).
 *   F_TTY  -- the serial console (P5).
 *   F_SOCK -- a network socket; `backing` is owned by c/net/core/lsock.c and
 *             read/write/close dispatch into it. It is a type here, rather than
 *             a separate handle table like the client sockets in
 *             c/net/core/sock.c, because the point of an ACCEPTED connection is
 *             being able to hand it to something: dup2 it onto a child's stdin,
 *             inherit it across fork, pass it to a function that takes an fd.  */
#define F_NONE 0
#define F_VFS  1
#define F_PIPE 2
#define F_TTY  3
#define F_SOCK 4

struct file {
    int   type;
    int   refcount;
    int   flags;        /* O_* (O_NONBLOCK / O_APPEND / ...) */
    int   is_write;     /* F_PIPE: 1 = write end, 0 = read end (kept out of `flags`
                         * so a SYS_SETNB O_NONBLOCK can't flip the close accounting) */
    int   amode;        /* O_RDONLY / O_WRONLY / O_RDWR, decided once at open and
                         * a property of THIS description -- a dup shares it, a
                         * fork inherits it, and a later SYS_SETNB cannot alter it */
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
int           file_fsync(struct file *f);   /* F_VFS: flush dirty data to disk now */
int           file_pipe(struct file **rd, struct file **wr);  /* P3 */

/* Peak simultaneous open descriptions since boot, and refused allocations.
 * NFILE is a guess until it is checked against one of these. */
void          file_watermark(int *peak, long *exhausted);

#endif /* LOGIT_FILE_H */
