#ifndef LOGIT_CLIPBOARD_H
#define LOGIT_CLIPBOARD_H

/* The system clipboard: one store, held by the kernel, shared by every process.
 *
 * Read the ABI comment above SYS_CLIP_SET in include/abi/logit_abi.h first --
 * it is where the ownership, lifetime and cap decisions are argued. This header
 * is just the kernel-side surface.
 *
 * There is no clip_init(). Every piece of state in clipboard.c is a static that
 * means "empty" when it is zero, so the service is live from the first
 * instruction the kernel executes and needs no hook in anyone else's boot path.
 *
 * LOCKING: none of its own. Every entry point below runs under the big kernel
 * lock -- the syscall path takes it (SYS_CLIP_* is not in syscall_is_bkl_free)
 * and the window manager holds it whenever it is running kernel code. Do not
 * call any of this from an interrupt handler. */

/* The SYS_CLIP_* back end. `pid` is the calling process's id, passed in rather
 * than looked up so that this file has no dependency on the process table and
 * can therefore be compiled and tested on the host (tests/unit/clipboard_test.c).
 * A pid of 0 means "the kernel itself". */
long clip_syscall(long num, long a, long b, long c, int pid);

/* The kernel-side face of the same store, in kernel pointers.
 *
 * This is what the window-management line's Cmd+C / Cmd+V routing calls: the
 * shortcut table belongs to them, the store belongs here, and there is exactly
 * one of each. Do not add a second clipboard for keyboard-originated copies.
 *
 * clip_set_text: `len` bytes of UTF-8, replacing the clipboard. Returns the
 *   bytes stored, or a negative CLIP_E_* code (it enforces the same cap and the
 *   same UTF-8 validation as the syscall -- there is one implementation).
 * clip_get_text: copies at most `max` bytes into `buf`, always ending on a
 *   character boundary, and returns how many. 0 means the clipboard has no
 *   text. Writes no terminator; `max` is a byte count, not a string size. */
int clip_set_text(const char *s, int len);
int clip_get_text(char *buf, int max);

/* How many bytes of a given flavour are held (0 if none). For a paste command
 * that wants to grey itself out without moving the payload. */
int clip_len(int flavour);

#endif /* LOGIT_CLIPBOARD_H */
