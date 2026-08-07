#ifndef SH_HOSTSTUB_H
#define SH_HOSTSTUB_H

/* A host stand-in for c/apps/logit.h.
 *
 * It claims logit.h's include guard, so including this FIRST and then #including
 * sh.c compiles the real shell -- not a copy of it -- against a model of the
 * kernel: real pipes with real reader/writer refcounts and real EAGAIN/EOF
 * semantics, because those are exactly what the shell's job-control and control
 * -channel logic depend on. A stub that always succeeded would test nothing.
 *
 * Nothing here is included by the OS build; it exists only under tests/unit.
 */

#define LOGIT_USERLIB_H          /* claim logit.h's guard */
#include "logit_abi.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <setjmp.h>

/* -------------------------------------------------------------- pipes ----- */

#define STUB_PIPESZ 8192
struct stub_pipe {
    unsigned char b[STUB_PIPESZ];
    int head, tail, count;
    int readers, writers;
    int used;
};
#define STUB_NPIPE 32
#define STUB_NFD   32

static struct stub_pipe stub_pipes[STUB_NPIPE];
struct stub_fd { int used, pipe, is_write, nonblock; };
static struct stub_fd stub_fds[STUB_NFD];

static int stub_pipe_alloc(void)
{
    for (int i = 0; i < STUB_NPIPE; i++)
        if (!stub_pipes[i].used) {
            memset(&stub_pipes[i], 0, sizeof stub_pipes[i]);
            stub_pipes[i].used = 1; stub_pipes[i].readers = 1; stub_pipes[i].writers = 1;
            return i;
        }
    return -1;
}
static int stub_fd_alloc(int pipe, int is_write)
{
    for (int i = 0; i < STUB_NFD; i++)
        if (!stub_fds[i].used) {
            stub_fds[i].used = 1; stub_fds[i].pipe = pipe;
            stub_fds[i].is_write = is_write; stub_fds[i].nonblock = 0;
            return i;
        }
    return -1;
}
/* Bind a specific fd number (so the test can put the control channel on 4). */
static int stub_fd_bind(int fd, int pipe, int is_write)
{
    stub_fds[fd].used = 1; stub_fds[fd].pipe = pipe;
    stub_fds[fd].is_write = is_write; stub_fds[fd].nonblock = 0;
    return fd;
}

static inline int sys_pipe(int fds[2])
{
    int p = stub_pipe_alloc();
    if (p < 0) return -1;
    fds[0] = stub_fd_alloc(p, 0);
    fds[1] = stub_fd_alloc(p, 1);
    return (fds[0] < 0 || fds[1] < 0) ? -1 : 0;
}

static inline int sys_close(int fd)
{
    if (fd < 0 || fd >= STUB_NFD || !stub_fds[fd].used) return -1;
    struct stub_pipe *p = &stub_pipes[stub_fds[fd].pipe];
    if (stub_fds[fd].is_write) p->writers--; else p->readers--;
    stub_fds[fd].used = 0;
    return 0;
}

static inline int sys_read(int fd, void *buf, int len)
{
    if (fd < 0 || fd >= STUB_NFD || !stub_fds[fd].used) return -1;
    struct stub_pipe *p = &stub_pipes[stub_fds[fd].pipe];
    unsigned char *o = (unsigned char *)buf;
    if (p->count == 0) {
        if (p->writers == 0) return 0;                  /* EOF */
        return EAGAIN_RC;                               /* the stub is always O_NONBLOCK-ish */
    }
    int n = 0;
    while (n < len && p->count > 0) {
        o[n++] = p->b[p->tail];
        p->tail = (p->tail + 1) % STUB_PIPESZ;
        p->count--;
    }
    return n;
}

static inline int sys_write(int fd, const void *buf, int len)
{
    if (fd < 0 || fd >= STUB_NFD || !stub_fds[fd].used) return -1;
    struct stub_pipe *p = &stub_pipes[stub_fds[fd].pipe];
    const unsigned char *s = (const unsigned char *)buf;
    int n = 0;
    while (n < len && p->count < STUB_PIPESZ) {
        p->b[p->head] = s[n++];
        p->head = (p->head + 1) % STUB_PIPESZ;
        p->count++;
    }
    return n ? n : EAGAIN_RC;
}

static inline int sys_set_nonblock(int fd)
{ if (fd >= 0 && fd < STUB_NFD) stub_fds[fd].nonblock = 1; return 0; }

static inline int sys_dup2(int o, int n) { (void)o; return n; }
static inline int sys_dup(int fd) { return fd; }
static inline long sys_lseek(int fd, long off, int w) { (void)fd; (void)off; (void)w; return -1; }
static inline int sys_open(const char *p, int f) { (void)p; (void)f; return -1; }

/* ----------------------------------------------------------- processes ---- */

static int  stub_next_pid = 100;
static int  stub_child_status = 0;

/* Phantom children. A fork() here runs nothing, but it DOES inherit every open
 * write end -- and that inheritance is the entire mechanism the shell's job
 * liveness pipe rests on, so the stub has to model it rather than assume it. */
struct stub_child { int used, pid, npipe, pipes[STUB_NPIPE]; };
static struct stub_child stub_children[16];

static inline int sys_fork(void)
{
    int ci = -1;
    for (int i = 0; i < 16; i++) if (!stub_children[i].used) { ci = i; break; }
    if (ci < 0) return -1;
    struct stub_child *c = &stub_children[ci];
    c->used = 1; c->pid = stub_next_pid++; c->npipe = 0;
    for (int fd = 0; fd < STUB_NFD; fd++) {
        if (!stub_fds[fd].used || !stub_fds[fd].is_write) continue;
        int p = stub_fds[fd].pipe;
        stub_pipes[p].writers++;
        c->pipes[c->npipe++] = p;
    }
    return c->pid;
}

/* The phantom child exits: drop every write end it inherited, which is what
 * makes the parent's liveness read return EOF instead of EAGAIN. */
static inline void stub_child_exit_all(void)
{
    for (int i = 0; i < 16; i++) {
        if (!stub_children[i].used) continue;
        for (int k = 0; k < stub_children[i].npipe; k++) stub_pipes[stub_children[i].pipes[k]].writers--;
        stub_children[i].used = 0;
    }
}
static inline int sys_waitpid(int pid, int *st) { (void)pid; if (st) *st = stub_child_status; return pid; }
static inline int sys_execve(const char *p, char *const a[], char *const e[])
{ (void)p; (void)a; (void)e; return -1; }
static inline int sys_getpid(void) { return 1; }

static jmp_buf stub_exit_jmp;
static int stub_exit_armed, stub_exit_code;
static inline void app_exit(int c)
{
    stub_exit_code = c;
    if (stub_exit_armed) longjmp(stub_exit_jmp, 1);
    fprintf(stderr, "stub: unexpected app_exit(%d)\n", c);
    exit(c ? c : 1);
}

static inline void sys_yield(void) { }
static int stub_naps;
static void (*stub_nap_hook)(int);
static inline int sys_nanosleep(long s, long ns) { (void)s; (void)ns; return 0; }
static inline int sys_sleep_ms(long ms)
{ (void)ms; stub_naps++; if (stub_nap_hook) stub_nap_hook(stub_naps); return 0; }

/* ---------------------------------------------------------- filesystem ---- */

static char stub_cwd[128] = "/home";
static inline int sys_getcwd(char *b, int n) { strncpy(b, stub_cwd, (size_t)n - 1); b[n - 1] = 0; return (int)strlen(b); }
static inline int sys_chdir(const char *p) { strncpy(stub_cwd, p, sizeof stub_cwd - 1); return 0; }

/* A tiny fake tree, enough for completion and globbing. */
struct stub_ent { const char *dir, *name; int size; };
static const struct stub_ent stub_tree[] = {
    { "/bin", "cat",   100 }, { "/bin", "chart", 100 }, { "/bin", "chmodx", 100 },
    { "/bin", "ls",    100 }, { "/bin", "show",  100 }, { "/bin", "dir",   100 },
    { "/home", "notes.txt", 12 }, { "/home", "note2.txt", 20 }, { "/home", "pics", -2 },
    { ".",    "notes.txt", 12 }, { ".",     "note2.txt", 20 }, { ".",    "pics", -2 },
    { 0, 0, 0 }
};
static inline int dir_count(const char *p)
{
    int n = 0, any = 0;
    for (int i = 0; stub_tree[i].dir; i++) if (!strcmp(stub_tree[i].dir, p)) { n++; any = 1; }
    return any ? n : -1;
}
static inline int dir_name(const char *p, int idx, char *buf)
{
    int n = 0;
    for (int i = 0; stub_tree[i].dir; i++) {
        if (strcmp(stub_tree[i].dir, p)) continue;
        if (n == idx) { strcpy(buf, stub_tree[i].name); return stub_tree[i].size; }
        n++;
    }
    return -1;
}

#endif /* SH_HOSTSTUB_H */
