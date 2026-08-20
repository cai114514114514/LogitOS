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
 *             inherit it across fork, pass it to a function that takes an fd.
 *   F_EVENT -- an eventfd or a timerfd: a 64-bit counter with a wait queue and
 *             no I/O behind it at all. ONE type for both, because they differ
 *             only in what increments the counter -- a write() from another
 *             thread, or the 100 Hz tick. Two types would have duplicated the
 *             read side, the wait side and the poll side to distinguish two
 *             producers. See `struct eventobj` in file.c.  */
#define F_NONE  0
#define F_VFS   1
#define F_PIPE  2
#define F_TTY   3
#define F_SOCK  4
#define F_EVENT 5

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
    /* F_VFS: this file is GENERATED (/proc), so it has no bytes to hold.
     *
     * The ordinary F_VFS description slurps the whole file into `backing` at
     * open and serves reads out of it. For a generated file that would make
     * every read return a snapshot taken at OPEN -- correct-looking, correctly
     * formatted, and describing a machine that has moved on. `live` says
     * "there is nothing here; go and ask at read() time", so the offset is the
     * only state this description carries. See c/fs/procfs.h, point 4, for
     * what makes a multi-read pass over one such file still coherent.
     *
     * A field and not a sixth F_* type: the type tag decides which BACKEND
     * owns the description, and the backend here is still the VFS. Every one
     * of file.c's F_VFS paths -- close, fsync, poll, the exhaustion census --
     * stays correct with `backing` NULL and `dirty` 0, which is exactly what a
     * live description is. A new type would have needed all six of them
     * duplicated to say the same thing. */
    int   live;
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

/* --- readiness: what poll() asks a descriptor ------------------------------
 * Returns the LPOLL* mask for `f` RIGHT NOW, having first registered `pt` on
 * whatever wait queue would announce a change. `pt` may be NULL, which makes
 * this a pure probe that registers nothing.
 *
 * THE ORDER INSIDE IS THE CONTRACT and it is stated in c/kernel/exec/kpoll.h:
 * register before reading state, or an event landing between the two is lost.
 * This declaration takes `struct poll_table *` by name only, so file.h does not
 * have to pull kpoll.h in for every one of its readers. */
struct poll_table;
short         file_poll(struct file *f, struct poll_table *pt);

/* --- eventfd / timerfd (F_EVENT) ------------------------------------------
 * Both return a description with refcount 1, or NULL. `flags` carries
 * O_NONBLOCK / EFD_SEMAPHORE. file_timerfd_arm() sets or clears the deadline;
 * value_ms == 0 disarms. file_timerfd_tick() is called from the 100 Hz timer
 * and is the ONLY thing that advances a timerfd's counter. */
struct file  *file_eventfd(uint64_t initval, int flags);
struct file  *file_timerfd(int flags);
int           file_timerfd_arm(struct file *f, long value_ms, long interval_ms);
void          file_timerfd_tick(void);

/* Peak simultaneous open descriptions since boot, and refused allocations.
 * NFILE is a guess until it is checked against one of these. */
void          file_watermark(int *peak, long *exhausted);

#endif /* LOGIT_FILE_H */
