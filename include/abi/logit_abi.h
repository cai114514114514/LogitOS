#ifndef LOGIT_ABI_H
#define LOGIT_ABI_H

/* System-call ABI shared between the kernel and userland applications.
 * Convention: rax = number, rdi/rsi/rdx = args, return value in rax. */

#define SYS_WRITE       1   /* (fd, buf, len) -> len  (fd 1 = serial log) */
#define SYS_EXIT        2   /* (code) -> terminate the process */
#define SYS_GUI_CREATE  3   /* (title, (w<<16)|h) -> 0  create the app window */
#define SYS_GUI_CLEAR   4   /* (color) */
#define SYS_GUI_RECT    5   /* ((x<<16)|y, (w<<16)|h, color) */
#define SYS_GUI_TEXT    6   /* ((x<<16)|y, color, str) */
#define SYS_GUI_FLUSH   7   /* () present the window */
#define SYS_POLL_EVENT  8   /* (event*) -> 1 if an event was returned else 0 */
#define SYS_GET_ARG     9   /* (buf, max) -> length of launch argument */
#define SYS_GET_TIME   10   /* (rtc_time*) fill wall-clock time */
#define SYS_READ_FILE  11   /* (name, buf, max) -> bytes read, or -1 */
#define SYS_YIELD      12   /* () yield the CPU */
#define SYS_SYSINFO    13   /* (buf, max) -> kernel writes uptime/mem/process text */
#define SYS_FILE_COUNT 14   /* () -> number of files on the volume */
#define SYS_FILE_NAME  15   /* (i, buf, max) -> file size; fills name */
#define SYS_WRITE_FILE 16   /* (path, buf, size) -> bytes written, or -1 (create/overwrite) */
#define SYS_DELETE_FILE 17  /* (path) -> 0, or -1 (file or empty dir) */
#define SYS_MKDIR      18   /* (path) -> 0, or -1 */
#define SYS_DIR_COUNT  19   /* (dir) -> entries in dir, or -1 if not a directory */
#define SYS_DIR_NAME   20   /* (dir, i, buf<=64) -> file size, -2 if dir, -1 if no entry */
#define SYS_NET_INFO   21   /* (struct logit_netinfo*) -> 1 if a NIC is up, else 0 */
#define SYS_NET_PING   22   /* (ip) start a ping; -> 0 ok, -1 no NIC. ip = a<<24|b<<16|c<<8|d */
#define SYS_NET_PING_RTT 23 /* () -> RTT in ms (>=0), or -1 if no reply yet */
#define SYS_NET_DNS    24   /* (name) start a DNS A-query; -> 0 ok, -1 no NIC */
#define SYS_NET_DNS_RESULT 25 /* () -> resolved IP (host order), 0 pending, 0xFFFFFFFF failed */
#define SYS_HTTP_GET    26  /* (url) fetch+render an http:// page; -> 0 ok start, <0 bad url */
#define SYS_HTTP_STATUS 27  /* () -> 1 busy, 2 done, <0 error code */
#define SYS_GUI_TEXT_MONO 30 /* ((x<<16)|y, (cell<<24)|color, str): monospace text */
/* M17 L1: ring-3 render-pipeline primitives (DOM/CSS/layout/paint live in the app). */
#define SYS_HTTP_BODY    36 /* (buf, max) -> copy the last http_get response body to the app; length */
#define SYS_TEXT_MEASURE 37 /* (s, len, (px<<1)|mono) -> pixel width of a length-delimited run */
#define SYS_GUI_TEXT_RUN 38 /* (struct logit_run*) draw a length-delimited text run (px/mono/color) */
#define SYS_RES_FETCH    39 /* (src, buf, max) -> fetch a sub-resource's raw bytes; length, or <0 */
#define SYS_GUI_BLIT     40 /* (struct logit_blit*) blit an RGBA bitmap into the window surface */
#define SYS_GUI_CLIP     41 /* ((x<<16)|y, (w<<16)|h) set window clip rect; (0,0,0,0) clears it */

/* M18: real processes -- fork/exec/wait, file descriptors, pipes. */
#define SYS_FORK        50 /* () -> child pid in parent, 0 in child, <0 on error */
#define SYS_EXECVE      51 /* (path, argv[], envp[]) -> <0 on error; never returns on success */
#define SYS_WAITPID     52 /* (pid, int *status, opts) -> reaped pid, or <0. pid=-1: any child */
#define SYS_GETPID      53 /* () -> current pid */
#define SYS_OPEN        54 /* (path, flags) -> fd, or <0 */
#define SYS_CLOSE       55 /* (fd) -> 0, or <0 */
#define SYS_READ        56 /* (fd, buf, len) -> bytes read (0 = EOF), or <0 */
#define SYS_LSEEK       57 /* (fd, off, whence) -> new offset, or <0 */
#define SYS_DUP2        58 /* (oldfd, newfd) -> newfd, or <0 */
#define SYS_PIPE        59 /* (int fds[2]) -> 0 (fds[0]=read end, fds[1]=write end), or <0 */
#define SYS_GETCWD      60 /* (buf, max) -> length, or <0 */
#define SYS_CHDIR       61 /* (path) -> 0, or <0 */
#define SYS_DUP         62 /* (fd) -> new fd, or <0 */
#define SYS_SPAWN       63 /* (path, argv[]) -> child pid, or <0 (fork+execve convenience) */
#define SYS_SETNB       64 /* (fd) -> 0; mark the fd non-blocking (reads return -2/EAGAIN) */
#define SYS_RENAME      65 /* (old_path, new_path) -> 0, or -1 (re-link a dir entry) */
#define SYS_OPEN_PATH   66 /* (path) -> 0; open file with its associated app (GUI only) */
#define SYS_IMG_DECODE  67 /* (struct logit_imgreq*) decode an image file -> RGBA in app buffer */
#define SYS_CPU_INDEX   68 /* () -> the index (0..N-1) of the core running the caller (SMP proof) */
#define SYS_KHEAP_STRESS 69 /* (iters, size, seed) -> corruption count; BKL-FREE concurrent kmalloc/kfree stress (M25 P1 gate) */
#define SYS_UI_DARK     70 /* (set) set<0 query, else set system dark mode (0/1); -> current value */
#define SYS_GUI_ICON    71 /* ((x<<16)|y, (id<<16)|px, color) draw a vector icon into the window */
#define SYS_GUI_GLASS   72 /* ((x<<16)|y, (w<<16)|h, (radius<<32)|(tr<<24)|(tg<<16)|(tb<<8)|ta) liquid-glass a region of the window over its own content */
#define SYS_GUI_RRECT   73 /* ((x<<16)|y, (w<<16)|h, (radius<<24)|color) filled rounded rect (web border-radius) */
#define SYS_FSYNC       74 /* (fd) -> 0, or -1; flush a dirty F_VFS file to disk NOW (not at close) */
/* () -> milliseconds since boot, as an unsigned 64-bit count in rax.
 *
 * SYS_GET_TIME answers the wall clock, in whole SECONDS, off a CMOS RTC that a
 * user can set backwards -- useless both for measuring an interval and for
 * pacing anything. This is the other clock: monotonic (never steps back, never
 * jumps), zero at boot, and the only time source an app can subtract.
 *
 * GRANULARITY IS 10 ms, NOT 1 ms. The value is derived from the 100 Hz PIT
 * tick, so it advances in steps of 10; the unit is milliseconds because that is
 * what callers want to compute in (and because a faster tick later changes the
 * step, not the ABI). Measure a 3 ms interval with it and you will read 0 or 10
 * -- that is the clock, not a bug.
 *
 * The counter is 64-bit and only ever increments, so it does not wrap in any
 * uptime this machine will see (2^64 ms is ~585 million years); callers do not
 * need wraparound-safe subtraction. It DOES stop advancing while interrupts are
 * off, which is the state inside the int 0x80 gate -- never spin on it from
 * kernel code that has not re-enabled IF (the same trap SYS_HTTP_GET documents). */
#define SYS_MONOTONIC_MS 75

/* ---- M27: non-blocking client sockets -------------------------------------
 *
 * The network used to be reachable from ring 3 only through SYS_HTTP_GET, which
 * did the whole fetch -- DNS, TCP, the TLS handshake, the request, the body --
 * inside one syscall, holding the big kernel lock, with the window manager told
 * not to poll the network for the duration. One request could exist in the
 * machine at a time and the desktop was frozen while it ran, so a page with
 * forty sub-resources meant forty sequential TLS handshakes behind a dead UI,
 * and a JS fetch() could not be built on it at all.
 *
 * These six calls are the replacement, and every one of them returns at once.
 * Progress happens in net_poll(), pumped from the WM loop, which advances EVERY
 * open socket on every pass -- so connections handshake concurrently, which is
 * the entire point. SYS_HTTP_GET and SYS_RES_FETCH still work, unchanged, so
 * both paths coexist while the ring-3 HTTP client is written.
 *
 * A socket handle is NOT a POSIX fd: sockets live in their own table, scoped to
 * the process that opened them, so that adding them could not perturb the
 * fork/exec/pipe fd semantics in c/kernel/exec/file.c. They are released when
 * the owning process exits. */
#define SYS_SOCK_OPEN   76 /* (host, (port<<16)|flags) -> handle >= 0, or SOCK_E_* */
#define SYS_SOCK_POLL   77 /* (fd) -> SOCK_P_* bits, or SOCK_E_* (negative) */
#define SYS_SOCK_SEND   78 /* (fd, buf, len) -> bytes taken (may be short, may be 0) */
#define SYS_SOCK_RECV   79 /* (fd, buf, max) -> bytes (0 = nothing yet, -1 = closed) */
#define SYS_SOCK_ALPN   80 /* (fd, buf, max) -> length of the negotiated protocol */
#define SYS_SOCK_CLOSE  81 /* (fd) -> 0 */

/* sock_open() flags.
 *
 * ALPN rides in the flag word as a bitmask rather than as a pointer to a list of
 * strings, because the set of protocols a browser ever offers is exactly these
 * two, and a flat integer is one less user pointer for the kernel to copy and
 * validate on every connection. If both bits are set, "h2" is offered first --
 * preference is the fixed order, not the bit order. */
#define SOCK_F_TLS          0x0001  /* wrap the connection in TLS (and send SNI) */
#define SOCK_F_ALPN_H2      0x0002  /* offer "h2" */
#define SOCK_F_ALPN_HTTP11  0x0004  /* offer "http/1.1" */
#define SOCK_F_ALPN_ANY     (SOCK_F_ALPN_H2 | SOCK_F_ALPN_HTTP11)

/* sock_poll() result bits. CONNECTED means the transport is up -- including the
 * TLS handshake, if one was asked for -- so an app has exactly one thing to wait
 * for. READABLE and EOF are distinct: a peer can send its FIN with data still
 * buffered, and an app that treated the FIN as "no more bytes" would truncate
 * the response. */
#define SOCK_P_CONNECTED    0x01
#define SOCK_P_READABLE     0x02
#define SOCK_P_WRITABLE     0x04
#define SOCK_P_EOF          0x08
#define SOCK_P_ERROR        0x10
/* On SOCK_P_ERROR, bits 8..15 carry the negated SOCK_E_* code: a poll result of
 * 0 would otherwise be the same answer for "still connecting" and "failed", and
 * the app has no other way to tell a DNS failure from a refused connection. */
#define SOCK_ERR_CODE(v)    (-(((v) >> 8) & 0xFF))

#define SOCK_E_ARG    -1   /* bad handle, bad length, not the caller's socket */
#define SOCK_E_DNS    -2   /* the name did not resolve */
#define SOCK_E_CONN   -3   /* TCP never established, or died */
#define SOCK_E_TLS    -4   /* handshake or certificate verification failed */
#define SOCK_E_NOSLOT -5   /* the socket or connection table is full */

/* ---- Display geometry ------------------------------------------------------
 *
 * EVERY SIZE AND COORDINATE AN APP EXCHANGES WITH THE WINDOW MANAGER IS IN
 * POINTS, NOT DEVICE PIXELS. gui_create(w,h), the rects, the text origins, the
 * coordinates in struct logit_event -- all points. The compositor multiplies by
 * the display's scale factor on the way in and divides on the way out, and it
 * re-rasterizes text and vector icons at the device size, so a denser display
 * makes the same app look SHARPER at the same physical size. That is why this
 * arrived without an ABI break and without touching a single app: at scale 100 a
 * point is a pixel and every existing number still means what it always meant.
 *
 * This call is how an app finds out what it is drawing onto. Fields are selected
 * by the argument rather than filled into a struct deliberately -- a struct here
 * would change the generated AetherScript ABI bindings, and a display query is
 * not worth that blast radius.
 *
 * SCALE IS INFORMATION, NOT AN INSTRUCTION. An app that multiplies its own
 * coordinates by SCREEN_SCALE will draw everything twice as large as it meant
 * to, because the kernel has already applied it. Read it to choose an asset or
 * to report the display, not to do arithmetic on layout. */
#define SYS_SCREEN_INFO 82 /* (what) -> one SCREEN_* field below, or -1 */
#define SCREEN_W       0   /* logical desktop width, in points  */
#define SCREEN_H       1   /* logical desktop height, in points */
#define SCREEN_SCALE   2   /* backing scale factor, in percent (100 = 1x)       */
#define SCREEN_DEV_W   3   /* real framebuffer width, in device pixels          */
#define SCREEN_DEV_H   4   /* real framebuffer height, in device pixels         */

/* open() flags */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT    0x100
#define O_TRUNC    0x200
#define O_APPEND   0x400
#define O_NONBLOCK 0x800
#define EAGAIN_RC  (-2)    /* a non-blocking read with no data yet (vs 0 = EOF) */

/* lseek() whence */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Event types returned by SYS_POLL_EVENT. */
#define EV_NONE   0
#define EV_KEY    1   /* a = character, or a KEY_* code below for non-printable keys */

/* Non-printable key codes (delivered via EV_KEY, a = code; all > 0xFF so they
 * never collide with a character). */
#define KEY_UP    0x101
#define KEY_DOWN  0x102
#define KEY_PGUP  0x103
#define KEY_PGDN  0x104
#define KEY_HOME  0x105
#define KEY_END   0x106
#define KEY_LEFT  0x107
#define KEY_RIGHT 0x108

#define EV_MOUSE  2   /* a = x, b = y (window-local), mouse-button down */
#define EV_CLOSE  3   /* the window's close button was pressed */
#define EV_MOUSE_R 4  /* a = x, b = y (window-local), right-button down */
#define EV_THEME  5   /* the system light/dark theme changed -- repaint to follow */
#define EV_MOUSE_UP   6  /* a = x, b = y (window-local); `button` says which one came up */
#define EV_MOUSE_MOVE 7  /* a = x, b = y (window-local); the pointer moved. Coalesced (see below) */
#define EV_WHEEL      8  /* a = x, b = y (window-local); `wheel` = notches, + = scroll DOWN */

/* Modifier keys held when the event was generated (struct logit_event.mods).
 * Sampled in the IRQ that produced the event, not when the app polls it -- a
 * shift released while the app was repainting must not un-shift the click that
 * is still sitting in the queue. */
#define EV_MOD_SHIFT 0x01
#define EV_MOD_CTRL  0x02
#define EV_MOD_ALT   0x04

/* Which button a press/release is about (struct logit_event.button); 0 on every
 * other event type. EV_MOUSE stays "a button went down" and EV_MOUSE_R stays
 * "the right button went down" so apps written before this field keep working;
 * `button` is how a new app tells left from middle without a third down-type.
 * (A pre-existing app that ignores `button` reads a middle-click as a left
 * click. That is the price of not minting EV_MOUSE_M, and it is the same trade
 * X11 made when it numbered buttons instead of typing them.) */
#define EV_BTN_NONE   0
#define EV_BTN_LEFT   1
#define EV_BTN_RIGHT  2
#define EV_BTN_MIDDLE 3

/* GROWN, not packed. Window-local coordinates do fit in 16 bits, so button +
 * modifiers could have ridden in the high halves of `a`/`b` without changing
 * sizeof -- and that is exactly the argument against it: every app in the tree
 * reads e.a/e.b as plain ints, so a packed right-click at x=100 would silently
 * read as 131172, and NOTHING in the build would notice. Appending fields
 * instead leaves every existing offset alone (asserted field by field in
 * c/apps/as/abi_layout.inc) and moves only sizeof -- which the same generated
 * _Static_assert catches, and which `make check-abi` catches by name. The ABI
 * machinery exists to make staleness a build failure; growing is the change it
 * can see, packing is the change it cannot. */
struct logit_event {
    int type;
    int a;
    int b;
    int mods;     /* EV_MOD_* bitmask -- key, button and wheel events */
    int button;   /* EV_BTN_* on EV_MOUSE / EV_MOUSE_R / EV_MOUSE_UP; 0 otherwise */
    int wheel;    /* EV_WHEEL: notches this event scrolled. + = down/away from
                   * the content start, matching the DOM's deltaY sign. PS/2
                   * has no horizontal wheel, so shift+wheel is the horizontal
                   * gesture -- read `mods` for it. */
};

/* Wall-clock time (mirrors the kernel's struct rtc_time field order). */
struct logit_time {
    int year, month, day;
    int hour, minute, second;
    int weekday;
};

/* Network info filled by SYS_NET_INFO. IPs are host order (a.b.c.d packed). */
struct logit_netinfo {
    unsigned ip, mask, gw;
    unsigned char mac[6];
};

/* M17 L1: payloads for the ring-3 render syscalls. */
struct logit_run  { int x, y, px, mono; unsigned color; const char *s; int len; };
struct logit_blit { int x, y, w, h; const unsigned char *rgba; int sw, sh; };

/* SYS_IMG_DECODE: the app provides `path` + an `rgba` buffer of `max` bytes; the
 * kernel decodes the image (PNG/GIF) and fills rgba + w/h (w*h*4 must be <= max). */
struct logit_imgreq { const char *path; unsigned char *rgba; int max; int w, h; };

#endif /* LOGIT_ABI_H */
