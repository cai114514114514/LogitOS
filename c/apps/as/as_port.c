/* M27 -- ports: OS endpoints as first-class AetherScript values.
 *
 * WHAT THIS REPLACES. Before this file the language reached the kernel through
 * `syscall(SYS_PIPE, addr(buf))` (fsroot/as/lib/sys.as) -- ~40 raw SYS_* numbers
 * injected as globals and a script hand-marshalling structs into buffers. That
 * is the language reaching for the OS through a keyhole we ourselves cut. A port
 * reverses the direction: the OS hands the language a value, and the value
 * carries its kind, its ownership and its lifetime.
 *
 * WHY IT IS NOT A LIBRARY. Three things here cannot be written as a library on
 * top of `syscall()`:
 *   - `run(...)` must NOT start the process. A pipeline needs both stages to
 *     exist before either runs, because the fds are wired between them; an eager
 *     `run()` has already lost that chance. So `|>` is an operator over
 *     unstarted stages (OP_PIPE), not a function over running ones.
 *   - a port's release must be deterministic. `with p = open(...):` closes at
 *     scope exit through a compiler-emitted opcode (OP_WITH_BEGIN/END), on the
 *     normal path, on `break`, on `return` and on a raised exception alike. A
 *     library can only offer a convention.
 *   - a port is its own iterator, which means the VM's OP_LEN and OP_INDEX_GET
 *     have to understand it. See as_port_avail below.
 *
 * PORTABILITY. Everything here is plain POSIX. On Logit those are mini-libc's
 * int-0x80 wrappers (c/apps/libc/src/io.c); on the build host they are the host
 * libc. That is deliberate and it is what makes ports testable in `make test-as`
 * without QEMU: fork/exec/pipe/redirect all really happen, host-side.
 *
 * M28 -- CAPABILITIES CONSUME AT ACQUISITION, NOT AT USE.
 * See docs/superpowers/specs/2026-08-14-m28-capabilities.md and as_native.c's
 * own CAP_RAW section (require_raw there, require_cap here -- same shape) for
 * the sibling half of the story: every native in THIS file that can make the
 * kernel create or start something checks the held set first, the same way
 * peek/poke/syscall do in as_native.c. D9 is explicit that a capability is
 * checked once, at the moment a resource comes into being, never again and
 * never revoked -- so the checks below live exactly at the five places an OS
 * handle is acquired and nowhere else in this file (not in port_method's
 * read/write/etc, which only use handles already granted).
 *
 * Exactly five acquisition points, counted by reading this file rather than
 * assumed (M28 brief's own instruction):
 *   n_open      opens a file,        owns=1
 *   n_port      wraps a borrowed fd, owns=0 (gated on the NUMBER -- see the
 *               comment inside n_port for why kind must not be the test)
 *   n_pipe      opens a pipe pair,   owns=1 (both ends)
 *   n_run       builds an UNSTARTED ObjProc -- no kernel resource exists yet,
 *               gated anyway so a script that may not spawn cannot even build
 *               the intent to
 *   proc_start  the ONE place fork()+exec() actually happen; reached from
 *               n_run's .start()/.wait()/.out() AND from as_proc_launch (the
 *               `with` acquire), so it is the real choke point regardless of
 *               how a script got here -- AND the one place '<-'/'->' redirect
 *               targets are opened (head->in_path / tail->out_path), which is
 *               a file acquisition by path exactly like n_open's and is gated
 *               the same way, at the same spot, for the same reason: a first
 *               pass at this file checked CAP_PROC here and stopped, on the
 *               reasoning "the resource proc_start acquires is the process" --
 *               that reasoning is wrong. A pipeline stage's stdin/stdout
 *               redirect is `open(2)` on a path this file itself calls (see
 *               below in the per-stage loop), with the OS user's real
 *               privileges, whether or not the exec'd program ever touches the
 *               filesystem itself. Left ungated, `run(prog) -> path` and
 *               `run(prog) <- path` are `open()` with the fs check silently
 *               removed: a script holding CAP_PROC and NOT CAP_FS_WRITE can
 *               create or overwrite any file the process can, and a script
 *               scoped to CAP_FS_WRITE with prefix "/tmp" can write outside
 *               "/tmp" through this door while the identical path handed to
 *               open() directly is correctly refused. Proven, not assumed:
 *               with only AS_CAP_PROC held, `run("echo","x") -> "/etc/..."`
 *               used to create the file; with AS_CAP_PROC|AS_CAP_FS_WRITE
 *               scoped to "/tmp", `run("echo","x") -> "/outside/tmp"` used to
 *               write there too. Checked ONCE per pipeline, in the PARENT,
 *               before any fork -- not in the child alongside the open() calls
 *               themselves, so a denial is a normal catchable as_native_fail()
 *               (the child is moments from execvp and has no clean way to
 *               report a language-level error back through the process
 *               boundary) and so every stage's fork is avoided entirely on a
 *               refusal rather than half a pipeline being spawned first.
 *               as_proc_redirect's own invariant (`->` only ever sets the
 *               TAIL stage's out_path, `<-` only ever sets the HEAD's in_path)
 *               means there are at most two paths to check per pipeline,
 *               never one per stage.
 *
 * -DAS_CAP_NO_CHECK removes every check below at compile time (mirrors this
 * file's own -DAS_PORT_NO_FINALIZE). Never "counts but does not enforce": the
 * property under test is behavioral -- a denied script's open("/etc/...")
 * must FAIL in the normal build and SUCCEED in this one, or a failure in the
 * normal build could have an unrelated cause.
 *
 * WHAT THIS FILE DOES NOT DO. It does not decide whether AS_CAP_RAW should
 * also unlock these checks (i.e. whether holding raw exempts a script from
 * the fs/proc gates too). It does not need to: D5 already establishes that a
 * script holding CAP_RAW can reach open/pipe/execve/fork directly through
 * as_native.c's syscall(), unmediated by any port at all -- so there is
 * nothing for a RAW-bypass here to buy that script it does not already have,
 * and adding one would only be a second, harder-to-audit way to reach the
 * same door. */

#include "as.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

#ifndef O_RDONLY
#define O_RDONLY 0
#endif
#ifndef O_WRONLY
#define O_WRONLY 1
#endif
#ifndef O_CREAT
#define O_CREAT 0x100
#endif
#ifndef O_TRUNC
#define O_TRUNC 0x200
#endif
#ifndef O_APPEND
#define O_APPEND 0x400
#endif
#ifndef O_RDWR
#define O_RDWR 2
#endif

#define PORT_MAX_ARGS   256    /* argv entries in one stage */
#define PORT_MAX_STAGES 32     /* stages in one pipeline */
#define PORT_LINE_MAX   (1 << 20)
/* Ceiling on the M28/D10 close-everything-else sweep in proc_start (see the
 * comment there). sysconf(_SC_OPEN_MAX) is trusted for the real number but
 * not for the loop bound: mini-libc answers a small, honest 32 (c/apps/libc/
 * include/limits.h OPEN_MAX, matching c/kernel/exec/proc.h's NFD), but a host
 * test environment can report a soft limit in the millions, and looping
 * close() that many times would turn every process spawn in the test suite
 * into a multi-second stall. 4096 is generous for anything this VM could
 * plausibly have open (a script under NFD==16 on real Logit, or a host build
 * with a handful of test fixtures) and bounded for anything it can't. */
#define PORT_FD_SWEEP_MAX 4096

/* Bookkeeping the tests read through port_stats(). `finalized` and `orphans`
 * count the times the COLLECTOR had to do the cleanup -- a backstop nobody has
 * watched fire is indistinguishable from one that does not exist. */
static long g_open_now, g_finalized, g_orphans;

void as_port_stats(long *open_now, long *finalized, long *orphans)
{
    if (open_now)  *open_now  = g_open_now;
    if (finalized) *finalized = g_finalized;
    if (orphans)   *orphans   = g_orphans;
}

static int proc_start(ObjProc *head, int out_fd);   /* fwd: defined with the pipeline code */
#ifndef AS_CAP_NO_CHECK
/* fwd: M28 capability gate, defined with the other built-ins below. proc_start
 * needs it above its own definition; the natives that also use it are defined
 * later still, so one forward declaration serves both directions. */
static int require_cap(uint32_t bit, const char *bitname, const char *what, const char *why);
#endif

/* ---------------------------------------------------------------- lifetime -- */

/* Close, once. `owns == 0` is a BORROWED handle -- fd 0/1/2 handed to us by
 * whoever spawned this process. Closing one of those would break the parent's
 * pipe, not ours; the port detaches instead. CLAUDE.md records the wm_launch
 * fd 0/1/2 collision as a live hazard of this system, and this flag is where the
 * language declines to repeat it. */
void as_port_close(ObjPort *p)
{
    if (p->closed) return;
    p->closed = 1;
    if (p->fd >= 0 && p->owns) { close(p->fd); g_open_now--; }
    p->fd = -1;
    p->pending = NULL;
    p->eof = 1;
}

/* The collector's side of the same operation, for a port the script dropped
 * without closing. Compile with -DAS_PORT_NO_FINALIZE to remove the backstop:
 * that is the negative control, and a loop that opens files without closing them
 * then exhausts the fd table instead of running forever. */
void as_port_drop(ObjPort *p)
{
    if (p->closed || p->fd < 0 || !p->owns) { p->closed = 1; p->fd = -1; return; }
    g_finalized++;
#ifndef AS_PORT_NO_FINALIZE
    close(p->fd);
    g_open_now--;
#endif
    p->closed = 1;
    p->fd = -1;
}

/* A dropped process is reaped WITHOUT blocking. A finalizer that waits is a
 * finalizer that can hang the collector on a child which never exits, which is a
 * worse failure than a zombie -- so a child still running when its last
 * reference goes is counted as an orphan and left to init. */
void as_proc_drop(ObjProc *p)
{
    if (!p->started || p->waited || p->pid <= 0) return;
    int st = 0;
    if (waitpid(p->pid, &st, WNOHANG) == p->pid) { p->status = st; p->waited = 1; }
    else g_orphans++;
}

/* `with` on a process ACQUIRES it, which for a command means starting it. That
 * is what makes the two resource kinds one rule: a port is opened then closed, a
 * command is started then waited for. It is also what makes a bare
 *   run("ls") |> run("grep","as") -> "out.txt"
 * statement do something -- the compiler wraps it in exactly this scope. */
int as_proc_launch(ObjProc *p)
{
    if (p->started) return 1;
    return proc_start(p, -1);
}

int as_value_release(Value v)
{
    if (IS_PORT(v)) { as_port_close(AS_PORT(v)); return 1; }
    if (IS_PROC(v)) {
        ObjProc *p = AS_PROC(v);
        for (ObjProc *s = p; s; s = s->next)
            if (s->started && !s->waited && s->pid > 0) {
                int st = 0;
                if (waitpid(s->pid, &st, 0) == s->pid) s->status = st;
                s->waited = 1;
            }
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ read -- */

/* One read(2) into the port's read-ahead. Returns bytes gained (0 at EOF, -1 on
 * error). Compacts first so a long stream does not grow the buffer forever. */
static int port_fill(ObjPort *p)
{
    if (p->eof || p->fd < 0) return 0;
    if (p->rpos > 0 && p->rpos == p->rlen) { p->rpos = p->rlen = 0; }
    else if (p->rpos > 0 && p->rpos > p->rcap / 2) {
        memmove(p->rbuf, p->rbuf + p->rpos, (size_t)(p->rlen - p->rpos));
        p->rlen -= p->rpos; p->rpos = 0;
    }
    if (p->rlen + 4096 > p->rcap) {
        int nc = p->rcap ? p->rcap * 2 : 8192;
        while (nc < p->rlen + 4096) nc *= 2;
        char *nb = (char *)as_realloc(p->rbuf, (size_t)nc);
        if (!nb) return -1;                     /* g_oom armed by as_realloc */
        as_gc_account((long)nc - (long)p->rcap);  /* fin_port gives back exactly rcap */
        p->rbuf = nb; p->rcap = nc;
    }
    long n = (long)read(p->fd, p->rbuf + p->rlen, 4096);
    if (n <= 0) { p->eof = 1; return n < 0 ? -1 : 0; }
    p->rlen += (int)n;
    return (int)n;
}

/* The next line, newline stripped (and a trailing CR with it -- a serial console
 * delivers CRLF and a shell that kept the CR would exec "ls\r"). NULL at EOF or
 * on error; *err distinguishes them. */
static ObjStr *port_next_line(ObjPort *p, int *err)
{
    *err = 0;
    for (;;) {
        for (int i = p->rpos; i < p->rlen; i++) {
            if (p->rbuf[i] == '\n') {
                int len = i - p->rpos;
                if (len > 0 && p->rbuf[p->rpos + len - 1] == '\r') len--;
                ObjStr *s = as_str_copy(p->rbuf + p->rpos, len);
                p->rpos = i + 1;
                if (!s) *err = 1;
                return s;
            }
        }
        if (p->rlen - p->rpos > PORT_LINE_MAX) {
            *err = 1;
            snprintf(as_err, sizeof as_err, "port: a single line exceeded %d bytes", PORT_LINE_MAX);
            return NULL;
        }
        int got = port_fill(p);
        if (got < 0) { *err = 1; return NULL; }
        if (got == 0) {                          /* EOF: a final unterminated line still counts */
            if (p->rlen > p->rpos) {
                int len = p->rlen - p->rpos;
                if (len > 0 && p->rbuf[p->rpos + len - 1] == '\r') len--;
                ObjStr *s = as_str_copy(p->rbuf + p->rpos, len);
                p->rpos = p->rlen;
                if (!s) *err = 1;
                return s;
            }
            return NULL;
        }
    }
}

/* ------------------------------------------------------- the port's cursor --
 *
 * A port is its own iterator. `for x in seq` compiles to OP_LEN + OP_INDEX_GET
 * (compiler.c for_statement), so a type that answers both is iterable -- and a
 * stream can answer both honestly if "length" means "how many items are known to
 * exist". avail() pulls at most ONE line and reports nread + (one pending), so
 * the loop's `idx < len` test is exactly "is there another line" and the memory
 * cost is one line, not the file. It also means a port iterated twice CONTINUES
 * rather than restarting, which is the truth about a stream. */
void as_port_iter_begin(ObjPort *p) { p->iter_base = p->nread; }

int as_port_avail(ObjPort *p, int64_t *n)
{
    int64_t done = p->nread - p->iter_base;
    if (p->pending)          { *n = done + 1; return 1; }
    if (p->closed || p->eof) { *n = done;     return 1; }
    int err = 0;
    ObjStr *s = port_next_line(p, &err);
    if (err) return 0;                           /* as_err set (OOM or over-long line) */
    if (!s) { *n = done; return 1; }             /* end of stream */
    p->pending = s;
    *n = done + 1;
    return 1;
}

int as_port_at(ObjPort *p, int64_t i, Value *out)
{
    /* A stream serves each position once, in order, counted from where THIS loop
     * began. Any other index is a random access into bytes that are gone. */
    if (i != p->nread - p->iter_base || !p->pending) return 0;
    *out = OBJ_VAL(p->pending);
    p->pending = NULL;
    p->nread++;
    return 1;
}

/* ------------------------------------------------------------- pipelines -- */

/* Both are called from opcodes, not natives, so they report by setting as_err
 * and returning 0 -- the VM arm turns that into a catchable runtime error. */
static int refuse(const char *msg)
{
    snprintf(as_err, sizeof as_err, "%s", msg);
    return 0;
}

int as_proc_pipe(ObjProc *a, ObjProc *b)
{
    if (a == b)                   return refuse("|>: a stage cannot feed itself");
    if (b->linked)                return refuse("|>: that stage is already in a pipeline");
    if (a->started || b->started) return refuse("|>: a running stage cannot be re-piped");
    if (b->next)                  return refuse("|>: the right side must be a single stage");
    ObjProc *t = a;
    int n = 1;
    while (t->next) { if (t->next == b) return refuse("|>: cycle"); t = t->next; n++; }
    if (n >= PORT_MAX_STAGES) return refuse("|>: too many stages in one pipeline");
    if (t->out_path)          return refuse("|>: cannot pipe past a '->' redirect");
    t->next = b;
    b->linked = 1;
    return 1;
}

int as_proc_redirect(ObjProc *p, ObjStr *path, int outward)
{
    if (p->started) return refuse("redirect: the pipeline has already started");
    if (outward) {
        ObjProc *t = p; while (t->next) t = t->next;   /* stdout belongs to the LAST stage */
        if (t->out_path) return refuse("->: output is already redirected");
        t->out_path = path;
    } else {
        if (p->linked)   return refuse("<-: input belongs to the head of the pipeline");
        if (p->in_path)  return refuse("<-: input is already redirected");
        p->in_path = path;                             /* stdin belongs to the FIRST stage */
    }
    return 1;
}

/* Build a NUL-terminated char*[] for one stage. Done in the PARENT, before the
 * fork: between fork and execve the child may not allocate. */
static char **stage_argv(ObjProc *s)
{
    if (!s->argv || s->argv->count < 1) { as_native_fail("run(): empty command"); return NULL; }
    if (s->argv->count > PORT_MAX_ARGS) { as_native_fail("run(): too many arguments"); return NULL; }
    char **av = (char **)as_malloc(sizeof(char *) * (size_t)(s->argv->count + 1));
    if (!av) return NULL;
    for (int i = 0; i < s->argv->count; i++) {
        if (!IS_STR(s->argv->items[i])) { free(av); as_native_fail("run(): arguments must be strings"); return NULL; }
        av[i] = AS_STR(s->argv->items[i])->chars;      /* ObjStr is always NUL-terminated */
    }
    av[s->argv->count] = NULL;
    return av;
}

/* Start the whole pipeline in ONE fork/dup2/exec pass. out_fd >= 0 overrides the
 * tail stage's stdout (that is how .out() captures without a temp file).
 * Returns 0 on failure with as_err/native-fail armed. */
static int proc_start(ObjProc *head, int out_fd)
{
    if (head->started) return 1;
    if (head->linked) { as_native_fail("this stage is inside a pipeline: start its head"); return 0; }
#ifndef AS_CAP_NO_CHECK
    /* M28 acquisition point 5 of 5, and the one that actually matters: this is
     * where fork()+exec() happen. n_run()'s check (built-ins section below)
     * stops a script from BUILDING a process value in the first place, but
     * every path that could ever start one funnels through here regardless --
     * .start()/.wait()/.out() and as_proc_launch (the `with` acquire) -- so
     * this is the real gate and n_run's is the early, friendlier one. One
     * check for the whole pipeline, not one per stage: every stage of a
     * pipeline was itself built by n_run and already passed that gate, and
     * this process's held set does not narrow between one stage and the next
     * within a single proc_start call. */
    if (!require_cap(AS_CAP_PROC, "proc", "run", "spawning a process")) return 0;
    /* '<-'/'->' name a file by path exactly like open() does, and open() that
     * path below (in the per-stage loop) with no further check -- so the gate
     * has to be here, not there. as_proc_redirect's own invariant puts in_path
     * only on the head stage and out_path only on the tail, so this is at most
     * two checks regardless of how many stages the pipeline has. Parent side,
     * before any fork, so a denial is caught cleanly by the interpreter and no
     * stage of the pipeline gets a chance to run first. */
    if (head->in_path) {
        if (!require_cap(AS_CAP_FS_READ, "fs.read", "run", "'<-' reads a file"))
            return 0;
        if (!as_caps_permit_path(head->in_path->chars)) {
            as_native_fail("run() needs capability 'fs': the '<-' path is outside the held prefix (M28 spec D6)");
            return 0;
        }
    }
    {
        ObjProc *tail = head;
        while (tail->next) tail = tail->next;
        if (tail->out_path) {
            if (!require_cap(AS_CAP_FS_WRITE, "fs.write", "run", "'->' writes a file"))
                return 0;
            if (!as_caps_permit_path(tail->out_path->chars)) {
                as_native_fail("run() needs capability 'fs': the '->' path is outside the held prefix (M28 spec D6)");
                return 0;
            }
        }
    }
#endif

    int prev_read = -1;
    for (ObjProc *s = head; s; s = s->next) {
        char **av = stage_argv(s);
        if (!av) { if (prev_read >= 0) close(prev_read); return 0; }

        int pfd[2] = { -1, -1 };
        if (s->next && pipe(pfd) != 0 && (gc_collect(), pipe(pfd) != 0)) {
            free(av); if (prev_read >= 0) close(prev_read);
            as_native_fail("pipeline: pipe() failed");
            return 0;
        }

        int pid = (int)fork();
        if (pid < 0) {
            free(av);
            if (pfd[0] >= 0) { close(pfd[0]); close(pfd[1]); }
            if (prev_read >= 0) close(prev_read);
            as_native_fail("pipeline: fork() failed");
            return 0;
        }
        if (pid == 0) {
            /* ---- child: no allocation past this point ---- */
            if (s == head && s->in_path) {
                int fd = open(s->in_path->chars, O_RDONLY);
                if (fd < 0) _exit(127);
                dup2(fd, 0); if (fd != 0) close(fd);
            } else if (prev_read >= 0) {
                dup2(prev_read, 0); if (prev_read != 0) close(prev_read);
            }
            if (pfd[1] >= 0) {
                dup2(pfd[1], 1);
                if (pfd[1] != 1) close(pfd[1]);
                if (pfd[0] >= 0) close(pfd[0]);
            } else if (!s->next && out_fd >= 0) {
                dup2(out_fd, 1); if (out_fd != 1) close(out_fd);
            } else if (!s->next && s->out_path) {
                int fd = open(s->out_path->chars, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) _exit(127);
                dup2(fd, 1); if (fd != 1) close(fd);
            }
            /* M28/D10: fork() copies the WHOLE fd table, not just the two ends
             * this stage wired above. Without this sweep, every port the
             * script still has open -- another pipeline's plumbing, a file it
             * forgot to close, whatever fd this whole VM inherited from
             * whoever launched /bin/as -- rides into the child unconditionally,
             * "whatever the child's own grant says" (spec s6). That makes
             * every capability check this milestone adds theatre: a script
             * scoped away from a file can still read it, so long as the
             * PARENT happened to have it open at the moment of spawn.
             *
             * The fix has to be done by hand, in the child, right here. This
             * kernel's execve() implements no close-on-exec at all -- see
             * popen.c's file comment in mini-libc, which hits the identical
             * wall for its own kept pipe fds and fixes it the same way, by an
             * explicit sweep rather than relying on O_CLOEXEC (which mini-libc
             * only remembers for F_GETFD to read back; nothing ever acts on
             * it). Unlike popen.c's sweep, which only knows about its OWN
             * registry of kept fds, this one is unconditional: 0/1/2 are the
             * only descriptors a freshly exec'd program may assume mean
             * anything, this stage's own wiring above has ALREADY landed
             * whatever belongs there (or left inherited stdio alone) and
             * closed its own spares, so anything still open at fd 3 or higher
             * belongs to something this stage never named -- close() on an fd
             * that was never open is a harmless EBADF, so the sweep does not
             * need to know which fds actually exist first. */
            for (int cfd = 3; cfd < PORT_FD_SWEEP_MAX; cfd++) close(cfd);
            execvp(av[0], av);
            _exit(127);                                 /* exec failed: the shell's 127 */
        }
        /* ---- parent ---- */
        free(av);
        s->pid = pid;
        s->started = 1;
        if (prev_read >= 0) close(prev_read);
        if (pfd[1] >= 0) close(pfd[1]);
        prev_read = pfd[0];
    }
    if (prev_read >= 0) close(prev_read);               /* unreachable: the tail has no pipe */
    return 1;
}

/* Wait for every stage. The pipeline's status is the LAST stage's, which is what
 * a shell reports and what `echo x | grep y` means. */
static int proc_wait_all(ObjProc *head, int *status)
{
    int last = 0;
    for (ObjProc *s = head; s; s = s->next) {
        if (!s->started) continue;
        if (!s->waited) {
            int st = 0;
            if (waitpid(s->pid, &st, 0) == s->pid) s->status = st;
            else s->status = -1;
            s->waited = 1;
        }
        last = s->status;
    }
    if (status) *status = last;
    return 1;
}

/* ------------------------------------------------------------- built-ins -- */

/* `need` is filled with the AS_CAP_FS_* bit(s) `mode` requires, alongside the
 * open(2) flags -- one string comparison doing both jobs instead of two, and
 * it keeps the capability answer for a mode defined in exactly the place the
 * mode itself is, so a new mode string cannot add a flag without also adding
 * the bit that gates it. */
static int mode_flags(const char *m, int *flags, uint32_t *need)
{
    if (!strcmp(m, "r"))  { *flags = O_RDONLY; *need = AS_CAP_FS_READ; return 1; }
    if (!strcmp(m, "w"))  { *flags = O_WRONLY | O_CREAT | O_TRUNC;  *need = AS_CAP_FS_WRITE; return 1; }
    if (!strcmp(m, "a"))  { *flags = O_WRONLY | O_CREAT | O_APPEND; *need = AS_CAP_FS_WRITE; return 1; }
    if (!strcmp(m, "rw")) { *flags = O_RDWR | O_CREAT; *need = AS_CAP_FS_READ | AS_CAP_FS_WRITE; return 1; }
    return 0;
}

#ifndef AS_CAP_NO_CHECK
/* The gate every acquisition point below calls. Shaped exactly like
 * as_native.c's require_raw (same file, same milestone, same reason to keep
 * the two recognisably the same check): a catchable as_native_fail() that
 * NAMES the missing capability, checked on every call rather than once at
 * as_install_ports() time -- D1's attenuation chain can narrow the held set
 * between one call and the next, and a check that only ran at registration
 * could not see that a later call is no longer permitted.
 *
 * Returns 1 (granted) or 0 (denied, as_native_fail already armed) -- a plain
 * int rather than a Value for the same reason require_raw is: as_native_fail
 * always returns NIL_VAL whether the native goes on to succeed or not, so a
 * Value here could not tell the caller which happened. */
static int require_cap(uint32_t bit, const char *bitname, const char *what, const char *why)
{
    if (as_caps_have(bit)) return 1;
    char msg[224];
    snprintf(msg, sizeof msg,
             "%s() needs capability '%s': %s (M28 spec s6)", what, bitname, why);
    as_native_fail(msg);
    return 0;
}
#endif

/* Every handle-acquiring call goes through this. An fd table is a resource the
 * collector can free -- ports it has not swept yet are still holding descriptors
 * -- so "out of descriptors" is a reason to COLLECT and try again, not a reason
 * to fail. This is what makes the GC a real backstop rather than an eventual
 * one: LogitOS gives a process 16 fds (proc.h NFD) and the collector's own
 * threshold is 1024 objects, so a script that dropped 20 ports without closing
 * them would otherwise hit the wall long before a collection was due.
 *
 * Exactly one retry, and only after a full collection: a second failure is a
 * real one (the file does not exist, or the descriptors are genuinely live). */
static int acquire_fd(const char *path, int flags)
{
    int fd = open(path, flags, 0644);
    if (fd >= 0) return fd;
    gc_collect();
    return open(path, flags, 0644);
}

static Value n_open(int argc, Value *args)
{
    if (argc < 1 || argc > 2 || !IS_STR(args[0]))
        return as_native_fail("open() takes (path[, mode]) -- mode is \"r\", \"w\", \"a\" or \"rw\"");
    const char *mode = "r";
    if (argc == 2) {
        if (!IS_STR(args[1])) return as_native_fail("open(): mode must be a string");
        mode = AS_STR(args[1])->chars;
    }
    int flags; uint32_t need;
    if (!mode_flags(mode, &flags, &need)) return as_native_fail("open(): mode must be \"r\", \"w\", \"a\" or \"rw\"");
#ifndef AS_CAP_NO_CHECK
    /* M28 acquisition point 1 of 5. Bits before path: a script holding
     * cap.fs.read for the whole tree but asking for "w" learns THAT is the
     * problem, rather than a path-shaped message that has nothing to do with
     * why it failed. Path is checked regardless of which bit(s) `need` is --
     * a script scoped to "/tmp" opening "/etc/passwd" for read is exactly the
     * case the prefix exists to refuse, independent of what it wanted to do
     * with the file once open. This is the language-level convenience half of
     * the check (as_caps_permit_path is a lexical prefix test); the kernel is
     * the real boundary and is symlink-aware where this cannot be (M28 spec
     * D6, out of scope for this file). */
    if ((need & AS_CAP_FS_READ) && !require_cap(AS_CAP_FS_READ, "fs.read", "open", "reading a file"))
        return NIL_VAL;
    if ((need & AS_CAP_FS_WRITE) && !require_cap(AS_CAP_FS_WRITE, "fs.write", "open", "writing a file"))
        return NIL_VAL;
    if (!as_caps_permit_path(AS_STR(args[0])->chars))
        return as_native_fail("open() needs capability 'fs': the path is outside the held prefix (M28 spec D6)");
#endif
    int fd = acquire_fd(AS_STR(args[0])->chars, flags);
    if (fd < 0) return NIL_VAL;                 /* absent file is a value, not an error */
    ObjPort *p = as_port_new(fd, PK_FILE, 1, AS_STR(args[0]));
    if (!p) { close(fd); return NIL_VAL; }
    g_open_now++;
    return OBJ_VAL(p);
}

/* A port over an fd this process was GIVEN (0/1/2, or one another port produced).
 * Borrowed by construction: closing it detaches, it never close(2)s. */
static Value n_port(int argc, Value *args)
{
    if (argc != 1 || !IS_INT(args[0]) || AS_INT(args[0]) < 0 || AS_INT(args[0]) > 65535)
        return as_native_fail("port() takes a file descriptor");
    int fd = (int)AS_INT(args[0]);
#ifndef AS_CAP_NO_CHECK
    /* M28 acquisition point 2 of 5, and the one with no clean answer -- see
     * the class-of-bug note in the file header. fd 0/1/2 are this process's
     * OWN inherited stdio: naming one is not a NEW privilege, it is the
     * privilege this process already runs with by construction, so nothing is
     * charged for it (ash.as does exactly `port(0)`/`port(1)` at the top of
     * the file -- gating those would leave every capability-scoped shell
     * unable to talk to its own terminal, which is not what any bit in this
     * milestone means to restrict).
     *
     * Any OTHER number is a guess this file cannot verify: spec s6 names the
     * exact hole -- "a capability class chosen from kind is spoofable by
     * choosing an fd number" -- and there is no fstat anywhere in this tree to
     * ask the kernel what fd N actually is instead (confirmed by grep before
     * writing this). That is structurally the same position peek()/poke() are
     * in (D4: "there is no syscall to intercept"), so it is gated behind the
     * same bit they are, AS_CAP_RAW, rather than invented as a sixth bit this
     * milestone's locked batch does not have room for. This is also what
     * makes the D10 fd-closing sweep in proc_start load-bearing rather than
     * decorative: once a spawned child inherits only 0/1/2 plus what THIS
     * process explicitly wired, there is nothing behind fd 3+ for a script to
     * reach for except what it opened itself under its own already-checked
     * grant. */
    if (fd > 2 && !require_cap(AS_CAP_RAW, "raw", "port",
            "an fd other than this process's own 0/1/2 cannot be verified here"))
        return NIL_VAL;
#endif
    ObjPort *p = as_port_new(fd, fd <= 2 ? PK_TTY : PK_FILE, 0, NULL);
    return p ? OBJ_VAL(p) : NIL_VAL;
}

static Value n_pipe(int argc, Value *args)
{
    (void)args;
    if (argc != 0) return as_native_fail("pipe() takes no arguments");
#ifndef AS_CAP_NO_CHECK
    /* M28 acquisition point 3 of 5. A pipe pair has no use in this language
     * except to talk to a spawned process -- there is no other IPC target it
     * could be handed to -- so it is gated behind the same bit run()/
     * proc_start are, AS_CAP_PROC, rather than treated as capability-free
     * because it never touches the filesystem or the network. */
    if (!require_cap(AS_CAP_PROC, "proc", "pipe", "a pipe exists to talk to a process"))
        return NIL_VAL;
#endif
    int fds[2];
    if (pipe(fds) != 0) { gc_collect(); if (pipe(fds) != 0) return as_native_fail("pipe() failed"); }
    ObjList *l = as_list_new();
    if (!l) { close(fds[0]); close(fds[1]); return NIL_VAL; }
    as_gc_protect((Obj *)l);
    ObjPort *r = as_port_new(fds[0], PK_PIPE, 1, NULL);
    if (!r) { as_gc_release(1); close(fds[0]); close(fds[1]); return NIL_VAL; }
    g_open_now++;
    as_list_push(l, OBJ_VAL(r));                 /* rooted by the list from here */
    ObjPort *w = as_port_new(fds[1], PK_PIPE, 1, NULL);
    if (!w) { as_gc_release(1); close(fds[1]); return NIL_VAL; }
    g_open_now++;
    as_list_push(l, OBJ_VAL(w));
    as_gc_release(1);
    return OBJ_VAL(l);
}

/* run("ls", "/usr")  or  run(["ls", "/usr"])  -- builds an UNSTARTED stage.
 * The list form is what a shell needs, because a shell's argv comes from parsing
 * text; the varargs form is what a script wants. Both make the same object. */
static Value n_run(int argc, Value *args)
{
#ifndef AS_CAP_NO_CHECK
    /* M28 acquisition point 4 of 5. run() only BUILDS a value -- no kernel
     * resource exists until proc_start, which is gated too (that is the real
     * choke point, reached from every path that can ever start a process) --
     * but a script that may not spawn should not be able to construct the
     * intent to either, so the refusal happens at the first call that names
     * it, not only at the moment it would be acted on. */
    if (!require_cap(AS_CAP_PROC, "proc", "run", "spawning a process"))
        return NIL_VAL;
#endif
    ObjList *av;
    if (argc == 1 && IS_LIST(args[0])) {
        ObjList *src = AS_LIST(args[0]);
        if (src->count < 1) return as_native_fail("run(): empty command");
        av = as_list_new();
        if (!av) return NIL_VAL;
        as_gc_protect((Obj *)av);
        for (int i = 0; i < src->count; i++) {
            if (!IS_STR(src->items[i])) { as_gc_release(1); return as_native_fail("run(): arguments must be strings"); }
            as_list_push(av, src->items[i]);
        }
    } else {
        if (argc < 1) return as_native_fail("run() takes a command and its arguments");
        for (int i = 0; i < argc; i++)
            if (!IS_STR(args[i])) return as_native_fail("run(): arguments must be strings");
        av = as_list_new();
        if (!av) return NIL_VAL;
        as_gc_protect((Obj *)av);
        for (int i = 0; i < argc; i++) as_list_push(av, args[i]);
    }
    ObjProc *p = as_proc_new(av);
    as_gc_release(1);
    return p ? OBJ_VAL(p) : NIL_VAL;
}

static Value n_port_stats(int argc, Value *args)
{
    (void)args;
    if (argc != 0) return as_native_fail("port_stats() takes no arguments");
    ObjDict *d = as_dict_new();
    if (!d) return NIL_VAL;
    as_gc_protect((Obj *)d);
    ObjStr *k;
    if ((k = as_str_copy("open", 4)))      as_dict_set(d, OBJ_VAL(k), INT_VAL(g_open_now));
    if ((k = as_str_copy("finalized", 9))) as_dict_set(d, OBJ_VAL(k), INT_VAL(g_finalized));
    if ((k = as_str_copy("orphans", 7)))   as_dict_set(d, OBJ_VAL(k), INT_VAL(g_orphans));
    as_gc_release(1);
    return OBJ_VAL(d);
}

/* ---------------------------------------------------------------- methods -- */

static int name_is(ObjStr *n, const char *s)
{
    int len = (int)strlen(s);
    return n->len == len && memcmp(n->chars, s, (size_t)len) == 0;
}

static int port_method(ObjPort *p, ObjStr *name, int argc, Value *args, Value *out)
{
    if (name_is(name, "close")) {
        if (argc != 0) { as_native_fail("close() takes no arguments"); return -1; }
        as_port_close(p); *out = NIL_VAL; return 1;
    }
    if (name_is(name, "closed")) { *out = BOOL_VAL(p->closed); return 1; }
    if (name_is(name, "fd"))     { *out = INT_VAL(p->fd); return 1; }
    if (name_is(name, "lines"))  { *out = OBJ_VAL(p); return 1; }   /* a port IS its cursor */
    if (name_is(name, "kind")) {
        const char *k = p->kind == PK_PIPE ? "pipe" : p->kind == PK_TTY ? "tty" : "file";
        ObjStr *s = as_str_copy(k, (int)strlen(k));
        if (!s) return -1;
        *out = OBJ_VAL(s); return 1;
    }
    if (name_is(name, "line")) {
        if (argc != 0) { as_native_fail("line() takes no arguments"); return -1; }
        if (p->pending) { *out = OBJ_VAL(p->pending); p->pending = NULL; p->nread++; return 1; }
        if (p->closed) { *out = NIL_VAL; return 1; }
        int err = 0;
        ObjStr *s = port_next_line(p, &err);
        if (err) return -1;
        if (!s) { *out = NIL_VAL; return 1; }
        p->nread++;
        *out = OBJ_VAL(s); return 1;
    }
    if (name_is(name, "read")) {
        if (argc != 1 || !IS_INT(args[0]) || AS_INT(args[0]) <= 0 || AS_INT(args[0]) > (1 << 24)) {
            as_native_fail("read() takes a positive byte count"); return -1;
        }
        if (p->closed) { *out = NIL_VAL; return 1; }
        int want = (int)AS_INT(args[0]);
        while (p->rlen - p->rpos < want && !p->eof) { if (port_fill(p) < 0) return -1; }
        int have = p->rlen - p->rpos;
        if (have <= 0) { *out = NIL_VAL; return 1; }          /* EOF is nil, not "" */
        if (have > want) have = want;
        ObjStr *s = as_str_copy(p->rbuf + p->rpos, have);
        if (!s) return -1;
        p->rpos += have;
        *out = OBJ_VAL(s); return 1;
    }
    if (name_is(name, "readall")) {
        if (argc != 0) { as_native_fail("readall() takes no arguments"); return -1; }
        while (!p->eof && !p->closed) { if (port_fill(p) < 0) return -1; }
        ObjStr *s = as_str_copy(p->rbuf ? p->rbuf + p->rpos : "", p->rlen - p->rpos);
        if (!s) return -1;
        p->rpos = p->rlen;
        *out = OBJ_VAL(s); return 1;
    }
    if (name_is(name, "write")) {
        if (argc != 1 || !IS_STR(args[0])) { as_native_fail("write() takes a string"); return -1; }
        if (p->closed || p->fd < 0) { as_native_fail("write() on a closed port"); return -1; }
        ObjStr *s = AS_STR(args[0]);
        int off = 0;
        while (off < s->len) {
            long n = (long)write(p->fd, s->chars + off, (size_t)(s->len - off));
            if (n <= 0) break;                                 /* short write: report what landed */
            off += (int)n;
        }
        *out = INT_VAL(off); return 1;
    }
    return 0;
}

static int proc_method(ObjProc *p, ObjStr *name, int argc, Value *args, Value *out)
{
    (void)args;
    if (name_is(name, "start")) {
        if (argc != 0) { as_native_fail("start() takes no arguments"); return -1; }
        if (!proc_start(p, -1)) return -1;
        *out = OBJ_VAL(p); return 1;
    }
    if (name_is(name, "wait")) {
        if (argc != 0) { as_native_fail("wait() takes no arguments"); return -1; }
        if (!p->started && !proc_start(p, -1)) return -1;
        int st = 0;
        proc_wait_all(p, &st);
        *out = INT_VAL(st); return 1;
    }
    if (name_is(name, "pid"))    { *out = INT_VAL(p->pid); return 1; }
    if (name_is(name, "status")) { *out = INT_VAL(p->status); return 1; }
    if (name_is(name, "argv"))   { *out = p->argv ? OBJ_VAL(p->argv) : NIL_VAL; return 1; }
    /* out(): run the pipeline with its tail's stdout on a pipe we hold, and give
     * back everything it wrote. The write end is closed in the parent BEFORE the
     * read, or the read would never see EOF. */
    if (name_is(name, "out")) {
        if (argc != 0) { as_native_fail("out() takes no arguments"); return -1; }
        if (p->started) { as_native_fail("out(): the pipeline has already started"); return -1; }
        int fds[2];
        if (pipe(fds) != 0) { as_native_fail("out(): pipe() failed"); return -1; }
        if (!proc_start(p, fds[1])) { close(fds[0]); close(fds[1]); return -1; }
        close(fds[1]);
        ObjPort tmp;
        memset(&tmp, 0, sizeof tmp);
        tmp.fd = fds[0]; tmp.owns = 1;                 /* a stack-local reader: never GC-visible */
        char *acc = NULL; int alen = 0, acap = 0;
        for (;;) {
            int got = port_fill(&tmp);
            if (got <= 0) break;
            if (alen + (tmp.rlen - tmp.rpos) + 1 > acap) {
                int nc = acap ? acap * 2 : 8192;
                while (nc < alen + (tmp.rlen - tmp.rpos) + 1) nc *= 2;
                char *nb = (char *)as_realloc(acc, (size_t)nc);
                if (!nb) { free(acc); free(tmp.rbuf); close(fds[0]); return -1; }
                acc = nb; acap = nc;
            }
            memcpy(acc + alen, tmp.rbuf + tmp.rpos, (size_t)(tmp.rlen - tmp.rpos));
            alen += tmp.rlen - tmp.rpos;
            tmp.rpos = tmp.rlen;
        }
        free(tmp.rbuf);
        close(fds[0]);
        proc_wait_all(p, NULL);
        ObjStr *s = as_str_copy(acc ? acc : "", alen);
        free(acc);
        if (!s) return -1;
        *out = OBJ_VAL(s); return 1;
    }
    return 0;
}

int as_port_invoke(Value recv, ObjStr *name, int argc, Value *args, Value *out)
{
    if (IS_PORT(recv)) return port_method(AS_PORT(recv), name, argc, args, out);
    if (IS_PROC(recv)) return proc_method(AS_PROC(recv), name, argc, args, out);
    return 0;
}

void as_install_ports(void)
{
    g_open_now = g_finalized = g_orphans = 0;
    as_define_native("open", n_open);
    as_define_native("port", n_port);
    as_define_native("pipe", n_pipe);
    as_define_native("run",  n_run);
    as_define_native("port_stats", n_port_stats);
}
