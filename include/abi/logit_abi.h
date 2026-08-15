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
#define SYS_SPAWN       63 /* (path, argv[]) -> child pid, or <0 (fork+execve convenience).
                             * DECLARED, NEVER IMPLEMENTED: there has never been a dispatch
                             * case for this number in c/kernel/exec/syscall.c, so every call
                             * to it has always fallen through to wm_gui_syscall()'s default
                             * and returned -1. M28 needed a capability-checked spawn/exec
                             * and, on finding this number unclaimed, deliberately did NOT
                             * claim it -- see SYS_CAP_SPAWN near the end of this file and the
                             * comment above proc_cap_spawn() in c/kernel/exec/exec.c for why a
                             * fresh number was safer than repurposing this one. Left exactly
                             * as declared. */
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
/* a = new content WIDTH in points, b = new content HEIGHT. The window's canvas
 * has ALREADY been reallocated at this size when the event is delivered -- see
 * the SYS_GUI_WIN_* block below. Repaint everything; nothing you drew survived.
 *
 * NOT coalesced -- unlike EV_MOUSE_MOVE, which the event ring merges onto an
 * unread sample of its own type. It does not need to be: the window manager
 * reallocates a canvas at most once every RESIZE_APPLY_MS during a drag (see
 * wm.c), so a resize that lasts a second delivers about eight of these, not one
 * per pointer packet. The throttle is upstream of the queue on purpose, because
 * the expensive part is the reallocation, and a coalescing queue would have
 * done the reallocations anyway and then thrown the events away. */
#define EV_RESIZE     9

/* Modifier keys held when the event was generated (struct logit_event.mods).
 * Sampled in the IRQ that produced the event, not when the app polls it -- a
 * shift released while the app was repainting must not un-shift the click that
 * is still sitting in the queue. */
#define EV_MOD_SHIFT 0x01
#define EV_MOD_CTRL  0x02
#define EV_MOD_ALT   0x04
/* The COMMAND key -- PS/2 set 1 `E0 5B` / `E0 5C`, what a PC keyboard calls the
 * Windows key and what this desktop calls Cmd. It is the system modifier, and it
 * is deliberately the one modifier no app in this tree had already spent:
 * Ctrl+letter is folded into a control code by the keyboard driver (Ctrl+S
 * arrives as 0x13 and TextEdit reads it there), and /bin/sh in the Terminal owns
 * the whole of Ctrl by convention. Claiming Ctrl+W for the window manager would
 * have taken ^W away from the shell.
 *
 * THE CLAIM RULE, which an app author needs in one sentence: the window manager
 * intercepts a CLOSED, DOCUMENTED LIST of Cmd combinations before the focused
 * app sees them (Cmd+W/Q/M/Tab/`, see wm.c) and forwards EVERYTHING else --
 * every unmodified key, and every Cmd combination not on that list -- to the
 * app with EV_MOD_SUPER set. An app cannot take a claimed one back; a shortcut
 * any app can swallow is not a system shortcut, and the first text field to eat
 * Cmd+W leaves a window that cannot be closed from the keyboard. The list is
 * closed so that "which Cmd keys are mine" has an answer that can be read. */
#define EV_MOD_SUPER 0x08

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

/* ---- M28: the time subsystem ----------------------------------------------
 *
 * What userland had before this: SYS_GET_TIME (the CMOS wall clock, in whole
 * SECONDS, settable, so it cannot measure an interval) and SYS_MONOTONIC_MS
 * (milliseconds since boot, but derived from the 100 Hz tick, so it steps in
 * tens). Both stay, byte for byte -- SYS_MONOTONIC_MS still advances in 10 ms
 * steps and run-clock-test.sh still asserts it. These three are the honest
 * surface underneath them.
 *
 * The clock ids are POSIX's numbers on purpose, so mini-libc's clock_gettime()
 * passes its argument straight through instead of translating -- a translation
 * table is a second place for the two sides to disagree. */
#define SYS_CLOCK_GETTIME 83 /* (clock_id, struct logit_timespec*) -> 0, or -1 on a bad id */
#define SYS_NANOSLEEP     84 /* (const struct logit_timespec *req, struct logit_timespec *rem) -> 0 */
#define SYS_CLOCK_INFO    85 /* (struct logit_clockinfo*, set_source) -> 0; set_source<0 = query only */

#define LOGIT_CLOCK_REALTIME            0  /* Unix epoch ns; jumps if the wall clock is set */
#define LOGIT_CLOCK_MONOTONIC           1  /* since boot; NEVER backwards. Subtract this one. */
#define LOGIT_CLOCK_PROCESS_CPUTIME_ID  2  /* CPU consumed by this process (user+sys) */
#define LOGIT_CLOCK_THREAD_CPUTIME_ID   3
#define LOGIT_CLOCK_MONOTONIC_RAW       4  /* unclamped source reading -- diagnostics only */
#define LOGIT_CLOCK_BOOTTIME            7  /* == MONOTONIC (LogitOS never suspends) */

/* POSIX layout exactly: mini-libc's struct timespec is `long tv_sec; long
 * tv_nsec`, and both sides are LP64, so the kernel can fill a user timespec
 * without a conversion step. */
struct logit_timespec { long tv_sec; long tv_nsec; };

/* Filled by SYS_CLOCK_INFO. This is the machine-readable form of the line the
 * kernel prints at boot; when time is wrong on unfamiliar hardware, `source`
 * and `hz` are the entire diagnosis.
 *
 * `set_source` (the second syscall argument, not a field) switches the live
 * clocksource: 0 = TSC, 1 = PIT, <0 = do not switch. The monotonic clock is
 * continuous across the switch, which is what makes it safe to exercise the
 * fallback on a running system instead of trusting that it would work. */
struct logit_clockinfo {
    int      source;             /* 0 = tsc, 1 = pit */
    int      nsources;
    unsigned long long hz;       /* calibrated frequency of the live source */
    unsigned long long res_ns;   /* smallest step it can report */
    unsigned long long mono_ns;
    unsigned long long real_ns;
    unsigned long long reads;        /* monotonic reads since boot */
    unsigned long long backsteps;    /* times the cross-core clamp had to intervene */
    unsigned long long backstep_max_ns;
    unsigned long long timers_queued;
    unsigned long long timers_fired;
    unsigned int       cores_seen;   /* bitmap of cores that have read the clock */
    char               name[16];     /* "tsc" / "pit" */
};

/* ---- M29: audio ------------------------------------------------------------
 *
 * The shape of this ABI is set by its first real consumer, which is not a GUI
 * app but a DECODER: something that produces PCM in bursts, at whatever rate
 * the file happens to be in, and must never stall the machine while it waits
 * for the card to drain. So:
 *
 *   - You declare YOUR format at open time, not the card's. The kernel
 *     converts and resamples (see c/kernel/audio/pcm.c). A decoder that emits
 *     44100 Hz mono float never has to know the card is running 48000 Hz
 *     stereo s16, and never has to carry a resampler of its own.
 *   - SYS_SND_WRITE takes what fits and TELLS YOU HOW MUCH. It is a short-write
 *     interface, like a socket, because that is the only shape that lets a
 *     decoder stay in control of its own loop. By default a write with no room
 *     PARKS the calling thread on a wait queue until the card drains a period
 *     (it does not spin, and it does not hold the machine); open with
 *     SND_F_NONBLOCK and it returns 0 instead.
 *   - SYS_SND_AVAIL is the "how much room" query, so a decoder can decode
 *     exactly as much as it can place and no more.
 *
 * The canonical decoder loop:
 *
 *      int h = snd(SYS_SND_OPEN, &fmt);
 *      while (decode_frame(&pcm, &len)) {
 *          int off = 0;
 *          while (off < len) {
 *              int k = snd(SYS_SND_WRITE, h, pcm + off, len - off);
 *              if (k < 0) goto fail;          // device went away
 *              off += k;                      // k==0 only if SND_F_NONBLOCK
 *          }
 *      }
 *      snd(SYS_SND_CLOSE, h, 1);              // 1 = drain what is queued
 *
 * More than one stream may be open at once; the kernel mixes them. Closing one
 * does not disturb the others. */
#define SYS_SND_INFO   86 /* (struct logit_sndinfo*) -> 1 if a card is present, 0 if none */
#define SYS_SND_OPEN   87 /* (struct logit_sndfmt*) -> stream handle >= 0, or SND_E_* */
#define SYS_SND_WRITE  88 /* (h, buf, bytes) -> bytes ACCEPTED (may be short), or SND_E_* */
#define SYS_SND_AVAIL  89 /* (h) -> bytes of room in the stream's ring right now, or SND_E_* */
#define SYS_SND_CLOSE  90 /* (h, drain) -> 0; drain != 0 plays out what is queued first */
#define SYS_SND_STATE  91 /* (h, struct logit_sndstate*) -> 0, or SND_E_* */

/* Sample formats. S16 is the one every path supports and the one a decoder
 * should emit unless it has a reason not to; the rest are converted on write. */
#define SND_FMT_S16  0   /* signed 16-bit little-endian (native everywhere) */
#define SND_FMT_U8   1   /* unsigned 8-bit, 0x80 = silence */
#define SND_FMT_S32  2   /* signed 32-bit little-endian */
#define SND_FMT_F32  3   /* IEEE float, nominal -1.0 .. +1.0, clamped */

#define SND_F_NONBLOCK 1u  /* SYS_SND_WRITE returns 0 rather than parking */

/* Negative returns. Distinct values because "no card" and "bad format" call for
 * completely different behaviour in a player, and collapsing them to -1 is how
 * a decoder ends up reporting a missing speaker as a corrupt file. */
#define SND_E_NODEV   -1  /* no sound card was found at boot */
#define SND_E_FORMAT  -2  /* rate/channels/format cannot be served */
#define SND_E_NOMEM   -3  /* no free stream slot, or the ring would not fit */
#define SND_E_BADH    -4  /* not an open handle of this process */
#define SND_E_FAULT   -5  /* the buffer pointer is not mapped in this process */

/* Stream states (logit_sndstate.state). */
#define SND_S_RUNNING 0
#define SND_S_DRAINING 1
#define SND_S_CLOSED  2

struct logit_sndfmt {
    unsigned int   rate;       /* Hz, any value 4000..192000; resampled to the card's */
    unsigned short channels;   /* 1..8. 1 = mono (fanned out); >2 keeps front L/R only */
    unsigned short format;     /* SND_FMT_* */
    unsigned int   buffer_ms;  /* wanted ring size in ms; 0 = default. Clamped, see info */
    unsigned int   flags;      /* SND_F_* */
};

/* What the machine actually has. This is the machine-readable form of the line
 * the kernel prints at boot -- on unfamiliar hardware that line is the whole
 * diagnosis, so it names the driver, the codec the card reported, and the exact
 * buffering, not just "audio ok". */
struct logit_sndinfo {
    char           driver[16];      /* "hda", "ac97", or "" when there is no card */
    char           codec[32];       /* what the card reported about itself */
    unsigned int   rate;            /* the rate the DMA engine is actually running */
    unsigned short channels;
    unsigned short format;          /* SND_FMT_* the hardware consumes natively */
    unsigned int   period_bytes;    /* DMA period: the granularity of one refill */
    unsigned int   periods;         /* periods in the hardware ring */
    unsigned int   streams_max;     /* how many mixer streams can coexist */
    unsigned int   streams_open;
    unsigned int   underruns;       /* device-wide: periods that went out silent */
    unsigned int   irq_mode;        /* 0 none/polled, 1 intx, 2 msi, 3 msi-x */
};

struct logit_sndstate {
    unsigned long long frames_written;  /* frames this stream has handed the kernel */
    unsigned long long frames_played;   /* frames the card has consumed of them */
    unsigned int       avail_bytes;     /* room right now, in YOUR format's bytes */
    unsigned int       ring_bytes;      /* total ring, in YOUR format's bytes */
    unsigned int       underruns;       /* times this stream ran dry while running */
    unsigned int       state;           /* SND_S_* */
};

/* --------------------------------------------------------------------------
 * Memory: a process can ask for more of it.
 *
 * Until now the only memory a ring-3 program had was the fixed arena it linked
 * with -- which is why mini-libc carries a static 24 MiB heap and why its
 * allocator cannot grow: there was nothing to grow into. mmap is the piece
 * that changes that. It reserves ADDRESS SPACE; the frames appear on first
 * touch, so reserving 64 MiB and using 200 KiB of it costs 200 KiB.
 *
 * Deliberately not POSIX mmap: no file backing, no MAP_FIXED, no offset. It is
 * the anonymous-memory subset an allocator needs, and adding the rest later
 * costs a flag, not an ABI break.
 * -------------------------------------------------------------------------- */
#define MMAP_PROT_READ   0x1
#define MMAP_PROT_WRITE  0x2
#define MMAP_PROT_EXEC   0x4

/* (len, prot, hint) -> base address, or 0 on failure.
 * `hint` is a preferred base (0 = anywhere) and is honoured only if it is free
 * -- it never evicts an existing mapping. `len` is rounded up to a page. */
#define SYS_MMAP        92
/* (addr, len) -> 0, or -1 if the range is not a subset of what is mapped.
 * Frames that were touched are released; ones that never were cost nothing. */
#define SYS_MUNMAP      93
/* (struct logit_meminfo *) -> 0. The machine's memory in numbers, so a program
 * (or a test) can see sharing and leaks rather than infer them. */
#define SYS_MEMINFO     94

struct logit_meminfo {
    unsigned long long frame_bytes;      /* 4096 */
    unsigned long long frames_total;     /* usable RAM, in frames */
    unsigned long long frames_free;
    unsigned long long frames_used;      /* frames with at least one reference */
    unsigned long long frames_shared;    /* used frames referenced more than once */
    unsigned long long refs_total;       /* sum of every frame's reference count.
                                          * refs_total - frames_used is how many
                                          * mappings sharing saved. */
    unsigned long long frames_pinned;    /* reference counts that saturated: a
                                          * known, bounded, deliberate leak */
    unsigned long long cow_pages;        /* pages currently mapped copy-on-write */
    unsigned long long cow_faults;       /* write faults resolved BY COPYING */
    unsigned long long cow_reuse;        /* ...resolved without copying (sole owner) */
    unsigned long long anon_faults;      /* first-touch anonymous pages filled */
    unsigned long long mm_bugs;          /* allocator invariant violations; must be 0 */
    unsigned long long mmap_reserved;    /* bytes this process has reserved */
};

/* --------------------------------------------------------------------------
 * File-backed mmap: a SECOND mmap form, added once c/kernel/mm/pcache.h had a
 * real page cache to back it with. SYS_MMAP above stays exactly what it was --
 * anonymous, zero-filled, no fd -- and this is additive, not a replacement.
 *
 * A SEPARATE SYSCALL NUMBER rather than a flag on SYS_MMAP, because the
 * three-register convention this whole ABI uses (rax = number, rdi/rsi/rdx =
 * args -- see the file's banner comment) has no room left: fd, byte offset
 * and length together already outgrow three registers, before `hint` is even
 * counted. The struct-pointer shape below is what this ABI already reaches
 * for whenever a call outgrows three scalars (logit_imgreq, logit_dirreq,
 * logit_capreq below) -- this is that pattern again, not a new one.
 *
 * UNCLASSIFIED (CAP_NONE) in c/kernel/exec/syscall.c's syscall_cap_class().
 * That function's own rule is: CAP_FS gates a syscall that acquires fs access
 * BY NAME; a syscall that only operates on an fd ALREADY HELD (its own listed
 * examples are read/write/close/lseek/dup/fsync/fstat) is deliberately left
 * out, because whatever fd it is holding could only exist by having already
 * passed that check at SYS_OPEN. This call takes an fd, not a path, so it is
 * the same shape as fstat/fsync and stays out of that table for the same
 * reason -- gating it too would check one grant twice, not check a new one.
 *
 * READ-ONLY, ALWAYS. c/kernel/mm/pcache.h says why in full: there is no dirty
 * page, no writeback and no msync anywhere in this kernel. A caller that asks
 * for a writable file mapping is REFUSED OUT LOUD (LOGIT_MMAP_FILE_E_WRITE)
 * rather than silently handed a private copy whose writes go nowhere. */
struct logit_mmap_file_req {
    unsigned long long hint;   /* preferred base VA, 0 = anywhere (as SYS_MMAP) */
    unsigned long long len;    /* bytes, rounded up to a page */
    unsigned long long off;    /* byte offset into the file; must be page aligned */
    int fd;                    /* from SYS_OPEN; must name a regular (F_VFS) file */
    int prot;                  /* MMAP_PROT_* ; MMAP_PROT_WRITE is refused, see above */
};

/* (struct logit_mmap_file_req *) -> base address (always > 0 on success --
 * every mapping lands at or above MM_MMAP_BASE), or:
 *   0   a generic failure: bad/non-regular fd, a misaligned `off`, a zero or
 *       overflowing `len`, no VMA slot, or the file could not be opened by
 *       the page cache at all (not a LogitFS regular file -- see pcv_stat()
 *       in c/kernel/mm/pcache_vfs.c). The same "0 = failure" shape SYS_MMAP
 *       already uses; the caller falls back to read().
 *   LOGIT_MMAP_FILE_E_WRITE (-1)  MMAP_PROT_WRITE was set. Distinguishable
 *       from the generic failure on purpose: this is not "out of resources,
 *       try again", it is "this kernel cannot do what was asked, ever". */
#define SYS_MMAP_FILE 162
#define LOGIT_MMAP_FILE_E_WRITE (-1)

/* ---- the process table, and ending a process ------------------------------
 * (struct logit_procinfo *buf, int max) -> entries filled, or -1.
 *
 * SYS_SYSINFO already prints a "process list", but it is formatted TEXT and it
 * lists the window manager's GUI windows -- not the PCB table. A task manager
 * cannot act on either of those: you cannot kill a line of prose, and a shell
 * or a forked child has no window to be listed under. So this is the process
 * table itself, as a struct.
 *
 * WHAT IS DELIBERATELY NOT IN THIS STRUCT, and why it is not an oversight:
 * per-process CPU time, resident memory, disk bytes and network bytes. This
 * kernel accounts NONE of the four per process today --
 *   - the scheduler counts DISPATCHES per thread (sched.c `slices`), never time,
 *     and offers no thread-by-id lookup, so a proc cannot even reach its own;
 *   - pmm/rmap refcount every frame but nothing sums them per address space,
 *     and the only per-cr3 memory number that exists (vma_reserved_bytes) counts
 *     mmap reservations, which no GUI app makes -- it reads 0 for all of them;
 *   - there is no I/O or per-socket byte accounting anywhere.
 * Each gap is closable, and each closes in a file this change does not own. A
 * column filled with a plausible-looking number nobody measured is worse than
 * an absent column, so they are absent. See proc_list() in c/kernel/exec/proc.c.
 */
#define SYS_PROCS       95
/* (pid) -> 0 on acceptance, else a negative LOGIT_KILL_* code. ASYNCHRONOUS:
 * acceptance means the process is marked, not that it has already died. See the
 * long comment above proc_kill() in c/kernel/exec/proc.c for why killing a
 * thread from outside its own control flow is not safe here, and what the mark
 * costs a process that is not being killed (one load and one branch). */
#define SYS_KILL        96

/* ---- window management: resize, zoom, minimise ----------------------------
 *
 * Until these, the entire verb vocabulary a user had over a window was DRAG and
 * CLOSE. A window was born at the size the app asked for and died at it.
 *
 * The window manager owns the geometry; an app never sets its own frame. What an
 * app gets is (a) a floor it can raise -- SYS_GUI_WIN_MIN -- and (b) EV_RESIZE,
 * which is the WM telling it the canvas it has been drawing into is now a
 * different size. THAT EVENT IS NOT ADVISORY. The surface behind the window has
 * already been reallocated when it arrives; anything the app painted before it
 * is gone, and the compositor is showing a STRETCHED copy of the old canvas
 * until the app paints a new one. An app that ignores EV_RESIZE therefore does
 * not "keep its old layout" -- it shows a magnified one forever.
 *
 * Sizes here are POINTS and they are CONTENT sizes: the canvas below the
 * titlebar, exactly the coordinate space gui_create() named and every gui_rect()
 * draws into. The titlebar is not the app's and is not counted.
 */
#define SYS_GUI_WIN_MIN  101 /* ((w<<16)|h) points -> 0. The smallest content size
                              * this window may be resized to. Clamped up to the
                              * WM's own floor, so an app cannot make itself
                              * ungrabbable. Sticky: set it once after create. */
#define SYS_GUI_WIN_STATE 102 /* (what, arg) -> value, or -1. `what` is a WINS_*. */

/* SYS_GUI_WIN_STATE selectors. The queries exist because an app that missed an
 * EV_RESIZE (or wants its size before the first one) has no other way to ask,
 * and re-deriving it from gui_create()'s argument is exactly the assumption
 * resize invalidates. The commands exist so an app's own chrome can drive the
 * same transitions the titlebar and the keyboard do -- one implementation of
 * zoom, not two. */
#define WINS_W         0   /* -> content width in points */
#define WINS_H         1   /* -> content height in points */
#define WINS_ZOOMED    2   /* -> 1 if maximized, else 0 */
#define WINS_MINIMIZED 3   /* -> 1 if hidden to the dock, else 0 */
#define WINS_SET_ZOOM  4   /* arg: 0 restore, 1 maximize, -1 toggle -> new state */
#define WINS_SET_MIN   5   /* arg: 1 minimise, 0 restore -> new state */

/* logit_procinfo.state -- mirrors enum proc_state in c/kernel/exec/proc.h. */
#define LOGIT_PROC_RUNNING   1
#define LOGIT_PROC_ZOMBIE    2

/* logit_procinfo.flags */
#define LOGIT_PROC_GUI       0x01   /* owns a window (a GUI app, not a CLI proc) */
#define LOGIT_PROC_DYING     0x02   /* a kill was accepted; it has not exited yet */
#define LOGIT_PROC_SELF      0x04   /* this is the calling process */
/* SYS_KILL would refuse this one. Published by the kernel rather than
 * re-derived by the caller on purpose: the rule for "which process must not be
 * killed" then exists in exactly one place, so a UI can grey the control out
 * BEFORE the click and can never disagree with the answer it would get after
 * it. Two copies of a safety rule is one copy too many. */
#define LOGIT_PROC_PROTECTED 0x08

/* proc_kill() results. Distinct codes, so a caller can say WHY it refused
 * rather than just failing -- a task manager that says "no" without a reason is
 * indistinguishable from one that is broken. */
#define LOGIT_KILL_OK         0
#define LOGIT_KILL_ENOENT    (-1)   /* no process with that pid */
#define LOGIT_KILL_PROTECTED (-2)   /* pid <= 1: the console shell / the compositor */
#define LOGIT_KILL_ZOMBIE    (-3)   /* already exited, waiting to be reaped */

struct logit_procinfo {
    int  pid, ppid;
    int  state;        /* LOGIT_PROC_RUNNING / LOGIT_PROC_ZOMBIE */
    int  tid;          /* scheduler thread backing it, -1 if none */
    int  flags;        /* LOGIT_PROC_* */
    int  nfds;         /* open file descriptors */
    char name[32];
    char cwd[128];
};

/* ---- the clipboard, and notifications -------------------------------------
 *
 * These are the two services that make the machine one system rather than a
 * set of processes that happen to share a screen. Before them, NOTHING crossed
 * an application boundary: the Terminal's output could not reach TextEdit, a
 * page in the Browser could not reach Code Studio, and nothing anywhere could
 * tell the user that a thing had finished except by opening a window in their
 * face.
 *
 * Both live in the kernel, and for the same reason: a service that has to
 * outlive the process that used it cannot be owned by one.
 *
 * NOTE ON SHAPE: every one of these four calls passes plain register arguments
 * rather than a struct, deliberately. A struct in this header changes the
 * generated AetherScript bindings (fsroot/as/lib/abi.as + c/apps/as/
 * abi_layout.inc, both regenerated by tools/gen_abi.py) -- and neither a
 * clipboard nor a notification is worth that blast radius. SYS_SCREEN_INFO made
 * the same trade above and says so.
 * -------------------------------------------------------------------------- */

/* (flavour | flags<<16, buf, len) -> bytes stored, or a CLIP_E_* code.
 *
 * OWNERSHIP AND LIFETIME, stated because this is the whole design:
 *
 *   The bytes are COPIED into kernel memory at the moment of the call, and the
 *   kernel owns them from then on. The clipboard holds no pointer into the
 *   calling process, no reference to it, and no promise from it. When the
 *   copying application exits -- crashes, is killed, is closed -- the clipboard
 *   is untouched, because nothing about it was ever the application's.
 *
 *   That is the opposite of X11's selection model, where the "owner" keeps the
 *   data and hands it over on request, and is exactly why a copy from a program
 *   you have since quit pastes nothing there. The cost of copying eagerly is
 *   one bounded allocation per set (CLIP_MAX_BYTES, below); the cost of not
 *   copying is a clipboard that lies.
 *
 * THE CAP IS ENFORCED, NOT ASSUMED. A payload longer than CLIP_MAX_BYTES is
 * REFUSED with CLIP_E_TOOBIG -- it is never silently truncated, because a
 * silent truncation of text is how a clipboard produces a broken multi-byte
 * character, and this system renders CJK. */
#define SYS_CLIP_SET    97
/* (flavour, buf, max) -> bytes copied, or a CLIP_E_* code.
 *
 * Short buffers are legal and are the normal case. What is copied is a PREFIX,
 * and for CLIP_F_TEXT the prefix always ends on a UTF-8 character boundary:
 * asked for 10 bytes of a string whose 10th byte is the middle of a 4-byte
 * codepoint, this returns 7, not 10. It never returns a partial character.
 * Compare the result with CLIP_Q_LEN to find out whether you got all of it. */
#define SYS_CLIP_GET    98
/* (what, flavour, 0) -> a CLIP_Q_* answer, or a CLIP_E_* code.
 *
 * The point of this call is to ask WHAT IS THERE without copying the payload:
 * a paste menu that has to move 64 KiB to find out whether it should be greyed
 * out is a paste menu that stutters. */
#define SYS_CLIP_INFO   99
/* (title, body, level) -> notification id >= 1, or -1 on a bad pointer.
 *
 * Returns IMMEDIATELY and always. It does not block, it does not take focus,
 * it does not need to be acknowledged, and the calling app never learns whether
 * anybody read it -- which is the entire difference between a notification and
 * a dialog. */
#define SYS_NOTIFY     100

/* ---- clipboard flavours ---------------------------------------------------
 *
 * One piece of content, several representations of it: the same selection is
 * text to a text editor and markup to something that can lay markup out. This
 * is macOS's pasteboard-types model and X11's targets model, and both exist
 * because the alternative -- one blob and a guess -- loses formatting on every
 * paste in one direction and produces angle brackets in a text field in the
 * other.
 *
 * THE FLAVOUR MECHANISM IS BUILT NOW; only CLIP_F_TEXT has a producer today.
 * That is a deliberate choice and not an accident of scope: the flavour has to
 * be an ARGUMENT of set/get from the first version, because adding a parameter
 * to a syscall afterwards breaks every caller, while adding a producer for an
 * id that already exists breaks nobody. The mechanism costs a four-entry array
 * and a bitmask; the seam costs an ABI break. So the ids below are all real,
 * all settable and all gettable today -- the kernel simply has no component
 * that puts HTML on the clipboard yet, which is a statement about the apps and
 * not about this interface. */
#define CLIP_F_TEXT   0   /* UTF-8 plain text; validated (see CLIP_E_UTF8) */
#define CLIP_F_HTML   1   /* UTF-8 HTML fragment */
#define CLIP_F_URI    2   /* UTF-8 absolute URL */
#define CLIP_F_PATH   3   /* UTF-8 absolute filesystem path */
#define CLIP_NFLAVOUR 4

/* SYS_CLIP_SET flags, in the HIGH half of the first argument.
 *
 * Without CLIP_SET_ADD a set REPLACES the clipboard: every other flavour is
 * dropped, because they described the old content and keeping them is how a
 * paste-as-HTML yields the previous selection's markup. With it, the flavour
 * joins the current content -- which is how one copy publishes text and HTML
 * for the same selection in two calls. */
#define CLIP_SET_ADD  0x0001

/* SYS_CLIP_INFO queries. */
#define CLIP_Q_FLAVOURS 0  /* -> bitmask of (1 << flavour) present. 0 = empty */
#define CLIP_Q_LEN      1  /* (flavour) -> its byte length, or CLIP_E_EMPTY */
#define CLIP_Q_SERIAL   2  /* -> generation; bumps on EVERY successful set, so an
                            * app can notice a change without re-reading bytes */
#define CLIP_Q_OWNER    3  /* -> the pid that last set the content. INFORMATIONAL
                            * ONLY: that process may be long gone and the
                            * clipboard is still valid -- see the lifetime note
                            * above. Nothing reads the content through it. */
#define CLIP_Q_MAX      4  /* -> CLIP_MAX_BYTES, from the kernel that enforces it
                            * rather than from this header, so a program built
                            * against an older header still learns the truth */

/* The bound, and it is a real one: SYS_CLIP_SET refuses more. 64 KiB holds
 * roughly 20 000 CJK characters or 65 000 of ASCII, which is a large document's
 * worth of selection; the number exists so that "arbitrary length" is a claim
 * with an edge somebody tested rather than an invitation to exhaust the heap
 * from ring 3. */
#define CLIP_MAX_BYTES (64 * 1024)

#define CLIP_E_ARG    (-1)  /* bad flavour, negative length, unmapped buffer */
#define CLIP_E_TOOBIG (-2)  /* len > CLIP_MAX_BYTES. NOT truncated -- refused */
#define CLIP_E_NOMEM  (-3)  /* the kernel could not hold it */
#define CLIP_E_EMPTY  (-4)  /* nothing of that flavour is on the clipboard */
#define CLIP_E_UTF8   (-5)  /* a CLIP_F_TEXT payload that is not well-formed
                             * UTF-8. Refused at SET, so that every consumer may
                             * assume the text flavour decodes -- which is what
                             * makes boundary-safe truncation possible at all */

/* ---- notifications --------------------------------------------------------
 *
 * PRESENTATION, decided and written down here because an app author has to be
 * able to predict it:
 *
 *   WHERE   top-right, immediately under the menu bar, macOS-style.
 *   HOW LONG  NOTIFY_MS_DEFAULT, then it goes away on its own.
 *   SEVERAL   up to NOTIFY_VISIBLE are stacked downward, newest at the top;
 *             beyond that they QUEUE and appear as slots free. Nothing is
 *             dropped and nothing overwrites anything.
 *   DISMISS   a click anywhere on a card removes it, and the ones below it
 *             move up. The click is consumed -- it does not fall through to
 *             whatever is underneath.
 *   FOCUS     never taken. A notification is not a window, has no input queue
 *             and is not in the z-order; the keyboard keeps going exactly where
 *             it was going. tests/qmp/qmp_notify.py asserts that by typing
 *             through one.
 *   ANIMATION none. See the comment above notify_compose() in
 *             c/kernel/gui/notify.c for the measurement that decided it. */
#define NOTIFY_INFO   0
#define NOTIFY_WARN   1
#define NOTIFY_ERROR  2

#define NOTIFY_VISIBLE     3     /* cards on screen at once; the rest queue */
#define NOTIFY_MS_DEFAULT  4000  /* how long one stays up */
#define NOTIFY_TITLE_MAX   48    /* bytes, including the NUL; longer is cut */
#define NOTIFY_BODY_MAX    120

/* ---- settings: what this machine remembers about its user -----------------
 *
 * Before these, nothing on this machine survived a reboot. The disk has been
 * provably durable since 2026-08-08 and there was nothing on it that belonged
 * to the user; every boot was the machine's first.
 *
 * The store is KERNEL-SIDE, not a library each app links, and the reason is
 * not convenience. LogitFS rewrites a WHOLE FILE per write (logitfs_write ->
 * inode_write), so two apps each holding their own copy of the settings and
 * writing it back would not race on one key -- each would silently delete
 * every key the other had. There has to be exactly one writer. The other three
 * reasons are in c/kernel/core/settings.h.
 *
 * The file is /etc/settings.conf, line-oriented text, `cat`-able and
 * repairable in TextEdit. Values are RANGE-CHECKED ON READ, never on write: a
 * file edited by hand will contain nonsense eventually, and checking at set()
 * time proves nothing about a value that arrived through a text editor. A key
 * that is missing, unparseable or out of range reads as its built-in default,
 * so there is no content of that file that can stop the desktop coming up.
 *
 * Unknown keys are PRESERVED across a rewrite. Another line wanting to persist
 * something -- a notification history, a clipboard pin -- can use this store
 * today without a kernel change, and should, rather than building a second one.
 */
#define SYS_SETTING_GET  103 /* (key, buf, max) -> bytes written, or -1 if unset.
                              * The value is the STORED string, or the schema
                              * default when nothing is stored. */
#define SYS_SETTING_SET  104 /* (key, value, flags) -> 0, or -1. flags bit 0 = 1
                              * commits to disk now; 0 leaves it in RAM for a
                              * caller about to set several keys (commit once
                              * with SETCTL_COMMIT -- one file write, one
                              * transaction, one atomic replacement). */
#define SYS_SETTING_ENUM 105 /* (index, struct logit_setting*, 0) -> 0, or -1
                              * past the end. Walks the SCHEMA, so an app that
                              * renders this is automatically correct about a
                              * setting added to the kernel after it shipped. */
#define SYS_SETTING_CTL  106 /* (op, a, b) -> per-op; op is a SETCTL_*. */

#define SETCTL_GEN       0   /* -> a counter that bumps on every commit. This is
                              * the change notification, and it is a POLL on
                              * purpose: pushing an event would need the WM to
                              * broadcast into every app's queue, and reading one
                              * integer once a frame is cheaper than that for
                              * every app that does not care. Theme changes keep
                              * their existing dedicated EV_THEME. */
#define SETCTL_COMMIT    1   /* -> 0; write the table out now */
#define SETCTL_RESET     2   /* -> 0; delete the file, restore every default */
#define SETCTL_COUNT     3   /* -> number of schema entries */
#define SETCTL_DIAG      4   /* -> SET_D_* bits from the last load */
#define SETCTL_RELOAD    5   /* -> SET_D_* bits; re-read the file from disk */
#define SETCTL_SELFTEST  6   /* -> failures in the truncate-at-every-offset
                              * sweep, run through the real boot-path loader.
                              * Must be 0. See settings_selftest(). */
#define SETCTL_KVCOUNT   7   /* -> live keys in the store, schema or not */
#define SETCTL_KVAT      8   /* (SETCTL_KVAT, buf, index) -> "key=value" length.
                              * How an app lists keys it has never heard of,
                              * which is the point of preserving them. `buf`
                              * must be at least LOGIT_SET_KVLINE bytes. */
/* The buffer SETCTL_KVAT writes into: key + '=' + value + NUL. Allocate this,
 * not a rounded-up guess -- every caller guessing independently is why raising
 * the value limit once turned several safe buffers into overflows at the same
 * moment. Mirrors SET_KVLINE in c/kernel/core/settings.h. */
#define LOGIT_SET_KVLINE 210
/* Buffer size for one value, INCLUDING the NUL (SETLIM_VALLEN is the usable
 * count, one less). An app that reads a setting into a smaller buffer than this
 * truncates it on the way in and then writes the truncation back out on save,
 * which reintroduces from the app side exactly the silent corruption the store
 * stopped doing. Mirrors SET_VALLEN. */
#define LOGIT_SET_VALMAX 160
#define SETCTL_LIMITS    9   /* (SETCTL_LIMITS, which) -> a SETLIM_* answer.
                              * Ask the machine rather than reading a header. */

/* SETCTL_LIMITS selectors. See the LIMITS block in c/kernel/core/settings.h for
 * the policy: this store is for SETTINGS -- small, typed, human-repairable
 * values -- because the whole store is one file written in one filesystem
 * transaction, and that is the property the entire corruption design rests on.
 * BULK DATA BELONGS IN FILES. A browsing history, a notification log or a
 * clipboard ring should live under its own directory and keep only its ENABLE
 * SWITCH and its preferences here. The caps below are what keep that true. */
#define SETLIM_MAXKEYS   0   /* keys the whole machine may store */
#define SETLIM_VALLEN    1   /* usable bytes in one value (excludes the NUL) */
#define SETLIM_KEYLEN    2   /* usable bytes in one key */
#define SETLIM_FREE      3   /* keys still unused right now */

/* SYS_SETTING_SET results. Distinct codes, because "-1" for both "your value is
 * too long" and "the machine is full" is the kind of error that costs somebody
 * an afternoon. A value that does not fit is REFUSED, never truncated: a
 * silently shortened path is a corruption you discover somewhere else, later,
 * and blame on the wrong thing. */
#define LOGIT_SET_OK        0
#define LOGIT_SET_EBADKEY (-1)
#define LOGIT_SET_ETOOBIG (-2)
#define LOGIT_SET_EFULL   (-3)
#define LOGIT_SET_EIO     (-4)

/* logit_setting.type -- mirrors SET_T_* in c/kernel/core/settings.h */
#define LOGIT_SET_BOOL   0
#define LOGIT_SET_INT    1
#define LOGIT_SET_COLOR  2
#define LOGIT_SET_STR    3
#define LOGIT_SET_IP     4
#define LOGIT_SET_ENUM   5

/* logit_setting.group -- the Settings app's tabs */
#define LOGIT_SETG_APPEARANCE 0
#define LOGIT_SETG_DESKTOP    1
#define LOGIT_SETG_NETWORK    2
#define LOGIT_SETG_SYSTEM     3
#define LOGIT_SETG_N          4

/* SETCTL_DIAG bits (SET_D_* in settings.h). A machine that booted with
 * defaults because its settings file was damaged should be able to SAY SO, and
 * the Settings app shows this. */
#define LOGIT_SETD_NOFILE     0x01
#define LOGIT_SETD_BADLINE    0x02
#define LOGIT_SETD_TRUNCATED  0x04
#define LOGIT_SETD_CRCBAD     0x08
#define LOGIT_SETD_FULL       0x10
#define LOGIT_SETD_RANGE      0x20

struct logit_setting {
    char key[48];
    char label[40];
    char value[80];      /* what is in force now */
    char dflt[80];       /* what a fresh machine would use */
    int  type;           /* LOGIT_SET_*  */
    int  group;          /* LOGIT_SETG_* */
    int  lo, hi;         /* bounds for INT/COLOR; 0,0 otherwise */
};

/* ===========================================================================
 * M30 THREADS -- two ring-3 execution flows inside ONE address space.
 *
 * Until now "concurrent" here meant PROCESSES: fork() gave you a second flow and
 * a second address space, and SYS_SPAWN gave you a child that shared nothing.
 * That is why c/apps/libc/include/pthread.h was a stub whose whole body was
 * `return 0` -- there was nothing under it. CPython 3.7 removed
 * --without-threads, so every CPython since REQUIRES a working threading
 * implementation, and this block is that requirement met.
 *
 * The five pieces, and why each one is a syscall rather than a library trick:
 *
 *   THE THREAD ITSELF (SYS_THREAD_CREATE/_EXIT/_JOIN/_DETACH/_SELF).
 *      c/kernel/sched/sched.c has been able to build a ring-3 thread since M6
 *      (thread_create_user) and schedule() has set TSS rsp0 per thread since
 *      SMP landed. What did not exist was a door from ring 3, and the
 *      per-thread lifecycle a threading library needs: an exit that ends ONE
 *      flow instead of the process, a join that delivers the return value, and
 *      a detach that releases the stack with nobody waiting.
 *
 *   THREAD-LOCAL STORAGE (SYS_SET_TLS). `__thread` on x86-64 is `%fs`-relative
 *      addressing, and ring 3 cannot set its own %fs base on this machine --
 *      see the long note on SYS_SET_TLS below for why the MSR and not
 *      `wrfsbase`. CPython uses `__thread` extensively, so this is not optional.
 *
 *   WAITING (SYS_FUTEX). One kernel primitive under mutex, condition variable,
 *      semaphore, once, join and rwlock. See SYS_FUTEX.
 *
 * WHAT A THREAD IS, precisely: a scheduler thread whose ->data points at the
 * SAME struct proc as its siblings. So all of pid, cwd, and the fd table are
 * shared, which is what POSIX means by a thread, and every existing syscall
 * that resolves the caller through proc_current() keeps working unchanged.
 * =========================================================================== */

/* (struct logit_thread_spec *) -> new tid (>0), or a THR_E_*. */
#define SYS_THREAD_CREATE 107
/* (retval) -> never returns. Ends THIS thread; the process survives while any
 * sibling lives. The LAST thread out runs the ordinary proc_exit() teardown. */
#define SYS_THREAD_EXIT   108
/* (tid, unsigned long *retval) -> 0, or a THR_E_*. Parks until the thread ends.
 * Refuses a detached thread, a thread of another process, and self-join. */
#define SYS_THREAD_JOIN   109
/* (tid) -> 0, or a THR_E_*. After this nobody may join; the stack and the
 * descriptor are released when the thread ends. */
#define SYS_THREAD_DETACH 110
/* () -> the calling thread's tid. Stable for the life of the thread. */
#define SYS_THREAD_SELF   111

/* (fsbase) -> 0, or -1. Set the calling thread's %fs base -- the TLS pointer
 * `__thread` and every pthread_getspecific() resolve against.
 * (-1) -> QUERY: the base currently installed, or 0 if there is none.
 *
 * The query exists because there are now TWO things that can install a thread
 * pointer and they must not fight. The ELF loader (c/kernel/exec/elf.c) lays
 * out and installs one from PT_TLS for a program's MAIN thread -- it is the
 * only thing that can, since only the loader has the image -- and mini-libc
 * installs one for every thread it creates. A libc that could not ask would
 * have to assume, and assuming "none" means overwriting the loader's block on
 * the first pthread call, silently discarding any `__thread` value main() had
 * already written. Ring 3 cannot read the base itself (RDFSBASE needs
 * CR4.FSGSBASE, which is not set here), and reading %fs:0 to find out is not a
 * test: if the base is 0 that is a load from linear address 0.
 *
 * Same "negative argument queries" shape as SYS_UI_DARK and SYS_CLOCK_INFO. -1
 * is unambiguous: a thread pointer is a user address, so it can never be -1.
 *
 * WHY THE MSR (IA32_FS_BASE, 0xC0000100) AND NOT `wrfsbase`.
 * cpufeat.c does detect `fsgsbase` and the boot log does list it as present, so
 * the instruction exists on this machine. It is not used, for two reasons and
 * the second is the decisive one:
 *
 *   1. `wrfsbase` FAULTS (#UD) unless CR4.FSGSBASE (bit 16) is set, and it is
 *      not set here -- c/boot/long.asm sets OSFXSR/OSXMMEXCPT/SMEP and stops.
 *      Turning it on means editing the BSP's long-mode entry AND every AP's
 *      bring-up, in files this line does not own, to gain an instruction only
 *      userland would use.
 *
 *   2. Even with CR4.FSGSBASE on, the kernel would still have to reload the FS
 *      base at every context switch, because two threads of one process have
 *      DIFFERENT TLS blocks and the same CR3 -- so nothing about the address
 *      space switch distinguishes them. The write has to happen in sched.c
 *      either way. Given that, `wrfsbase` would save one wrmsr per switch on
 *      the switches where the base actually changes, and cost a CR4 change on
 *      every core. The kernel skips the write entirely when prev and next carry
 *      the same base (kernel threads carry 0), which is the case that is
 *      actually hot.
 *
 * So ring 3 asks and the kernel writes, exactly as Linux's arch_prctl(ARCH_SET_FS)
 * did before FSGSBASE, and switching to `wrfsbase` later is a two-line change
 * inside sched.c with no ABI consequence. */
#define SYS_SET_TLS       112

/* (uaddr, (timeout_ms<<32)|val, op) -> per-op; see FUTEX_* below.
 *
 * WHY A FUTEX AND NOT A KERNEL MUTEX SYSCALL.
 * The competing design is "SYS_MUTEX_LOCK / SYS_MUTEX_UNLOCK", and it is wrong
 * here for a reason specific to this kernel: THERE IS A BIG KERNEL LOCK. Every
 * syscall that is not in syscall_is_bkl_free() serialises the whole machine for
 * its duration. A lock design whose UNCONTENDED path enters the kernel would
 * therefore turn every `with lock:` in a Python program into a global
 * serialisation point -- the exact cost the futex was invented to remove, on a
 * machine where it is far more expensive than on Linux.
 *
 * With a futex the uncontended lock and unlock are one atomic compare-exchange
 * in ring 3 and no syscall at all. The kernel is entered only when a thread
 * genuinely has to WAIT, which is the only case where a context switch was
 * going to happen anyway.
 *
 * The second reason is arithmetic: mutex, condition variable, semaphore,
 * pthread_once, rwlock and join are SIX primitives, and all six are built in
 * c/apps/libc/src/pthread.c out of this one call. The alternative is six
 * syscall families, six kernel-side lifetime problems, and six chances to get
 * the lost-wakeup ordering wrong instead of one.
 *
 * The kernel side is c/kernel/sched/uthread.c and it does not invent a sleep:
 * it parks on c/kernel/core/wait.c's sched_block_self_unlock(), whose
 * lost-wakeup argument (sched.h) is the one this depends on. */
#define SYS_FUTEX         113

/* (buf, len, flags) -> bytes written, or -1.  buf == NULL -> 1 if the DRBG has
 * a hardware entropy source behind it (RDSEED/RDRAND), 0 if it fell back.
 *
 * ARBITRATED FOR THE CRYPTO LINE, not needed by threads. The kernel has had a
 * real SHA-256 Hash_DRBG in c/kernel/core/rng.c, seeded from RDSEED/RDRAND
 * where the CPU has it, since the TLS work -- and ring 3 could not reach it, so
 * c/apps/browser/js_platform.c says in capital letters that
 * crypto.getRandomValues is a lie. This is the door; the back end is
 * rng_syscall() in that file, which owns the semantics.
 *
 * Blocking is not a behaviour here: the DRBG is seeded before ring 3 exists, so
 * GRND_NONBLOCK is accepted and changes nothing, and this call never parks. */
#define SYS_GETRANDOM     114

/* (what, 0, 0) -> one THRINFO_* count, or -1. Diagnostics -- the leak check in
 * tests/unit needs to see descriptors returning to the free pool across
 * thousands of create/join cycles, and "it did not crash" is not that. */
#define SYS_THREAD_INFO   115

/* What ring 3 hands SYS_THREAD_CREATE. `unsigned long` throughout rather than a
 * pointer type so tools/gen_abi.py can lay it out (it refuses typedefs on
 * purpose) and so AetherScript can build one. */
struct logit_thread_spec {
    unsigned long entry;       /* ring-3 address to start at. Entered by IRETQ, so
                                * see stack_top below -- the alignment is the ABI. */
    unsigned long stack_top;   /* one past the highest byte of the thread's stack */
    unsigned long stack_base;  /* SYS_MMAP base the kernel should SYS_MUNMAP when the
                                * thread ends; 0 = the caller owns the stack and the
                                * kernel must not touch it */
    unsigned long stack_len;   /* bytes at stack_base (0 with stack_base 0) */
    unsigned long tls;         /* initial %fs base; 0 = leave it at 0 */
    unsigned long arg;         /* stored at the new thread's [rsp]; the ring-3
                                * trampoline reads it from there. Not passed in a
                                * register because the register state at the IRETQ
                                * belongs to c/boot/enter_user.asm, which this line
                                * does not own. */
};

/* THE STACK, and what it costs.
 *
 * A CLI program's stack is CLI_STACK_PAGES = 256 pages = 1 MiB (c/kernel/exec/
 * exec.c), of which TWO are mapped eagerly and the rest is a VMA reservation
 * faulted in on touch. A thread's stack is the same kind of object -- ordinary
 * SYS_MMAP anonymous memory, demand-paged by c/kernel/mm/fault.c -- so the
 * DEFAULT below is a reservation, not an allocation: a thread that uses 8 KiB
 * of it costs 8 KiB of physical memory plus two page-table pages, whatever the
 * number says.
 *
 * 8 MiB, and not 1 MiB, because that is what CPython assumes. Its evaluator
 * recurses per Python frame and its default THREAD_STACK_SIZE on Linux is
 * derived from the platform's 8 MiB; a Python thread that blows a 1 MiB stack
 * does not raise RecursionError, it takes a page fault the kernel turns into a
 * dead process. Since the pages are not committed until touched, matching Linux
 * costs address space -- of which a 64-bit process has plenty -- rather than RAM.
 *
 * MIN is 16 KiB because below that the guard page and the initial frame are most
 * of it. MAX is a sanity bound, not a policy. */
#define LOGIT_THREAD_STACK_DEFAULT (8ul * 1024 * 1024)
#define LOGIT_THREAD_STACK_MIN     (16ul * 1024)
#define LOGIT_THREAD_STACK_MAX     (256ul * 1024 * 1024)

/* How many threads one process may have at once, as far as the THREAD TABLE is
 * concerned (c/kernel/sched/uthread.c).
 *
 * IT IS NOT THE BINDING LIMIT, and the real one is lower and elsewhere: every
 * thread's stack is one mmap, and c/kernel/mm/vma.h caps an address space at
 * VMA_MAXAREA = 16 areas -- of which the program image's own stack and libc's
 * malloc arena already take some. So the practical ceiling is around THIRTEEN
 * concurrent threads per process, and a program that asks for more does not get
 * a thread-table error, it gets pthread_create returning EAGAIN partway through
 * a loop.
 *
 * Written here rather than left to be discovered because the number a caller
 * reads in a header is the number it will size its worker pool by. Raising it
 * means raising VMA_MAXAREA (a fixed table, deliberately not allocated, because
 * it is consulted from the page-fault path) or giving thread stacks a different
 * kind of reservation -- both in c/kernel/mm/, neither this line's to do. */
#define LOGIT_THREADS_MAX 64

/* SYS_THREAD_* results. Distinct codes for the same reason the settings block
 * gives: "-1" for both "no such thread" and "the table is full" costs somebody
 * an afternoon. */
#define THR_E_ARG      (-1)   /* a malformed spec, or a stack outside the bounds above */
#define THR_E_NOMEM    (-2)   /* kernel could not build the thread */
#define THR_E_FULL     (-3)   /* LOGIT_THREADS_MAX reached in this process */
#define THR_E_SRCH     (-4)   /* no such tid in this process */
#define THR_E_INVAL    (-5)   /* detached, already being joined, or self-join */
#define THR_E_DEADLK   (-6)   /* the join would deadlock */

/* SYS_FUTEX ops. */
#define FUTEX_WAIT  0   /* if *uaddr == val, park. -> 0 woken, FUTEX_E_AGAIN if the
                         * value had already changed, FUTEX_E_TIMEDOUT on timeout.
                         * timeout_ms 0 = wait forever. SPURIOUS WAKES HAPPEN: every
                         * caller is a `while (!cond)` loop, exactly as the kernel's
                         * own sleepers are (c/kernel/core/wait.h rule 2). */
#define FUTEX_WAKE  1   /* wake up to `val` waiters on uaddr -> the number woken.
                         * val = INT32_MAX is the broadcast. */

#define FUTEX_E_AGAIN    (-1)   /* *uaddr != val; the caller must re-test and retry */
#define FUTEX_E_TIMEDOUT (-2)
#define FUTEX_E_ARG      (-3)   /* unaligned address, bad op, or memory not the caller's */

/* SYS_GETRANDOM flags. Both are accepted and neither changes anything -- said
 * out loud rather than left for a caller to discover: the DRBG is seeded before
 * ring 3 exists, so there is no "not ready yet" state to block on or refuse. */
#define GRND_NONBLOCK 0x0001
#define GRND_RANDOM   0x0002
#define GRND_MAX      4096      /* bytes per call; more is a short read, not an error */

/* SYS_THREAD_INFO selectors. */
#define THRINFO_LIVE      0   /* threads of the calling process running right now */
#define THRINFO_SLOTS     1   /* descriptors in use machine-wide */
#define THRINFO_SLOTS_MAX 2   /* the table size, so a test can assert it went back down */
#define THRINFO_CREATED   3   /* threads created since boot (monotonic) */
#define THRINFO_REAPED    4   /* descriptors returned to the pool since boot */

/* ---- M31: file metadata ----------------------------------------------------
 *
 * WHAT RING 3 COULD ASK BEFORE THIS, IN FULL:
 *
 *   #define SYS_DIR_NAME 20  // (dir, i, buf<=64) -> size, -2 if dir, -1 if none
 *
 * That is the whole surface. A program that wants a file's size must find the
 * file's PARENT, enumerate it by index, and match the name -- there was no way
 * to ask about a path. `-2` overloads the size to mean "directory". Nothing
 * could read a mode, an owner, a link count, an inode number or a timestamp,
 * and a name longer than 63 bytes came back truncated with no way to tell.
 *
 * The data was already on the medium: LogitFS v4 inodes carry atime/mtime/ctime
 * (c/fs/logitfs_fmt.h) and the VFS has kept modes, owners, link counts and
 * symlink targets since the mount-table work (c/fs/vfs_meta.c). This block is
 * the pipe between the two, not a new feature.
 *
 * WHY A LENGTH ARGUMENT. Every call below that fills a struct takes the buffer's
 * size and the kernel fills min(that, what it knows), stamping `len` with what
 * it actually wrote. An OLD binary calling a NEW kernel gets its own smaller
 * struct filled; a NEW binary on an OLD kernel sees a smaller `len` and knows
 * the tail is untouched rather than reading whatever was on its stack. That is
 * cheap here and impossible to retrofit, which is the entire reason it is here.
 *
 * WHY `attr`. A stat that cannot say "this 0644 is the default, nobody ever set
 * it" is a stat that lies quietly. Every field that can be a fallback is
 * accompanied by a bit saying whether it was read from somewhere. See LSTA_*. */
#define SYS_STAT      120 /* (path, struct logit_stat*, len) -> 0, or <0. Follows symlinks. */
#define SYS_LSTAT     121 /* (path, struct logit_stat*, len) -> 0, or <0. Does NOT follow. */
#define SYS_FSTAT     122 /* (fd,   struct logit_stat*, len) -> 0, or <0 */
#define SYS_GETDENTS  123 /* (struct logit_dirreq*) -> entries written, 0 at end, <0 error */
#define SYS_CHMOD     124 /* (path, mode) -> 0, or <0. Owner or root only. */
#define SYS_UMASK     125 /* (mask) -> the PREVIOUS mask. mask < 0 = query only. */
#define SYS_SYMLINK   126 /* (target, linkpath) -> 0, or <0 */
#define SYS_READLINK  127 /* (path, buf, max) -> bytes written (no NUL), or <0 */
#define SYS_LINK      128 /* (oldpath, newpath) -> 0, or <0 */
#define SYS_CHOWN     129 /* (path, uid, gid) -> 0, or <0. Root only. */

/* File type, in the high bits of logit_stat.mode. POSIX's numbering, so
 * mini-libc's S_IFMT masks apply unchanged and there is no translation table to
 * drift. The low 12 bits are the permission bits, also POSIX-numbered. */
#define LST_IFMT   0170000
#define LST_IFLNK  0120000
#define LST_IFREG  0100000
#define LST_IFDIR  0040000
#define LST_IFCHR  0020000
#define LST_IFIFO  0010000

/* logit_stat.attr -- WHICH OF THE ANSWERS ABOVE ARE REAL.
 *
 * A caller that ignores this field gets today's behaviour and today's plausible
 * defaults. A caller that reads it can tell a recorded 0600 from the 0644 that
 * every unrecorded file reports, which is the difference between a permission
 * model and a decoration. */
#define LSTA_MODE_STORED   0x0001 /* mode/uid/gid are a RECORD, not the default */
#define LSTA_MODE_DURABLE  0x0002 /* ...and that record is on the medium, so it
                                   * survives a reboot. Without this bit the
                                   * record is the VFS's RAM store and will not. */
#define LSTA_TIMES         0x0004 /* atime/mtime/ctime came off the medium */
#define LSTA_INO           0x0008 /* `ino` is a real inode number, not 0 */
#define LSTA_NLINK_RAM     0x0010 /* nlink > 1 because of the VFS's RAM link
                                   * store; the extra names do not survive a boot */

struct logit_stat {
    unsigned int       len;      /* bytes the kernel wrote (<= the len argument) */
    unsigned int       version;  /* LOGIT_STAT_VERSION */
    unsigned int       mode;     /* LST_IF* | permission bits */
    unsigned int       attr;     /* LSTA_* -- read this before believing the rest */
    unsigned int       uid;
    unsigned int       gid;
    unsigned int       nlink;
    unsigned int       blksize;  /* the filesystem's block size */
    unsigned long long ino;
    unsigned long long dev;      /* mount index; two paths with the same (dev,ino)
                                  * are the same file */
    unsigned long long size;     /* bytes. For a directory: the entry count. */
    unsigned long long blocks;   /* 512-byte units actually allocated */
    long long          atime;    /* whole seconds, Unix epoch. 0 = unknown, which
                                  * a caller must not read as 1970. */
    long long          mtime;
    long long          ctime;
};
#define LOGIT_STAT_VERSION 1

/* SYS_GETDENTS: the directory read that SYS_DIR_NAME is not.
 *
 * Three things it fixes, in the order they bite: a name may be up to 255 bytes
 * (SYS_DIR_NAME caps at 63 and cannot report that it truncated), the type is
 * its own field instead of an overloaded return value, and one call returns
 * MANY entries so listing a directory is not N syscalls.
 *
 * THE CURSOR, said exactly. `cursor` is opaque: pass 0 to start, then pass back
 * whatever the kernel left in it. It is NOT an entry index and callers must not
 * do arithmetic on it. The guarantee is: for a directory that is not modified
 * between calls, resuming from the returned cursor visits every entry exactly
 * once. If the directory IS modified mid-walk, an entry may be skipped or
 * repeated -- the backends here enumerate by position, and a stronger promise
 * would need an on-disk iterator the filesystem does not have. Stated rather
 * than glossed, because "stable cursor" usually means the stronger thing.
 *
 * SYS_DIR_COUNT / SYS_DIR_NAME are unchanged and still work; several apps use
 * them and none had to move. */
struct logit_dirent {
    unsigned int       reclen;   /* bytes of THIS record: always sizeof(struct
                                  * logit_dirent) in version 1. Step by it, do
                                  * not assume it. */
    unsigned int       type;     /* LST_IFREG / LST_IFDIR / LST_IFLNK, 0 unknown */
    unsigned long long ino;      /* 0 when the backend has no inode numbers */
    unsigned long long size;     /* bytes; for a directory, 0 */
    char               name[256];/* NUL-terminated */
};

struct logit_dirreq {
    const char    *path;
    unsigned char *buf;      /* receives an array of struct logit_dirent */
    int            max;      /* bytes available at buf */
    int            cursor;   /* IN: 0 to start / a previous OUT. OUT: where to resume */
    int            count;    /* OUT: entries written */
    int            pad;
};

/* ===========================================================================
 * M31 -- SIGNALS. A second entry into user code, forced by the kernel.
 *
 * WHAT WAS HERE BEFORE, and why it was not signals. c/kernel/exec/proc.c had
 * `g_killmark[]`: one bit per process meaning "you have been killed and do not
 * know it yet". No number, no handler, no delivery -- SYS_KILL meant DESTROY.
 * So there was no Ctrl+C, a ring-3 fault killed the process without userland
 * ever learning why, a shell could not be told a child had died, and nothing
 * could ever be asked to exit cleanly.
 *
 * A signal is not a syscall. Delivery means: at a moment when the kernel is
 * about to return to ring 3, pick a pending unblocked signal; push a frame on
 * the USER stack holding the interrupted register state, the FPU/SSE state and
 * the old mask; point rip at the handler; and when the handler returns, restore
 * every bit of that exactly. The three numbers below (SIGACTION / SIGPROCMASK /
 * SIGRETURN) are the doors; the mechanism is c/kernel/exec/ksignal.c.
 *
 * THE FPU/SSE STATE IS PART OF THE FRAME, and this is the piece that is easy to
 * omit and impossible to notice. c/boot/isr.asm FXSAVEs on every kernel entry
 * precisely because the kernel is built with SSE; a handler is ordinary ring-3
 * C that may touch a double, so without the 512-byte FXSAVE area in the frame a
 * signal landing mid-computation silently changes the answer. Every simple
 * handler still works, `kill -TERM` still works, the shell still works. See
 * tests/unit/signal_test.c and the SIGNAL_NO_FPU negative control.
 *
 * THREADS, said plainly rather than half-built. Signals here are
 * PROCESS-DIRECTED ONLY: one pending set and one blocked mask per process, and
 * a pending signal is taken by whichever of the process's threads next reaches
 * a return-to-ring-3 boundary. POSIX permits exactly that for a process-
 * directed signal ("delivered to one thread that does not block it"). What is
 * NOT here: per-thread masks, thread-directed signals (pthread_kill still
 * returns ENOSYS), sigqueue/siginfo payloads, and alternate signal stacks
 * (sigaltstack). A synchronous fault signal is the one exception and is
 * necessarily thread-directed: it is delivered on the faulting thread's own
 * frame, which is the only frame that describes the fault.
 *
 * SESSIONS, likewise. There are no process groups and no sessions -- SYS_SETSID
 * and SYS_GETPGID do not exist and are not added here. The tty therefore holds
 * ONE foreground pid, set by the last process to read the console, and Ctrl+C
 * sends SIGINT to it and to its children. That is less than job control and it
 * is said out loud rather than dressed up as it.
 * =========================================================================== */

/* (signo, const struct logit_sigaction *act, struct logit_sigaction *old)
 * -> 0, or a SIG_E_*. Either pointer may be NULL. SIGKILL and SIGSTOP are
 * refused (SIG_E_ARG) because they must remain uncatchable. */
#define SYS_SIGACTION   130

/* (how, const unsigned long *set, unsigned long *old) -> 0, or SIG_E_ARG.
 * `how` is a SIG_BLOCK/SIG_UNBLOCK/SIG_SETMASK below. SIGKILL and SIGSTOP
 * cannot be blocked and are silently dropped from any set, exactly as on Linux
 * -- refusing the call instead would break every program that does sigfillset()
 * before a critical section. */
#define SYS_SIGPROCMASK 131

/* () -> does not return normally. Restores the frame the kernel pushed: all
 * GPRs, rip, rflags, rsp, the FPU/SSE state and the pre-handler mask. Only
 * meaningful from the restorer the kernel pushed as the handler's return
 * address; called anywhere else it fails and returns SIG_E_ARG. */
#define SYS_SIGRETURN   132

/* (unsigned long *out) -> 0, or SIG_E_ARG. Signals raised but blocked. */
#define SYS_SIGPENDING  133

/* (seconds) -> whole seconds left on the previous alarm (0 if none). 0 cancels.
 * SIGALRM is posted from the timer tick, so the resolution is the 100 Hz tick
 * and a one-second alarm is accurate to 10 ms, not to the second. */
#define SYS_ALARM       134

/* (const unsigned long *mask) -> SIG_E_INTR, always. Installs `mask`, waits
 * until a signal that is NOT in it is delivered, then restores the old mask.
 * The one call in this family whose ONLY successful outcome is an error, which
 * is what POSIX specifies. */
#define SYS_SIGSUSPEND  135

/* (what, 0, 0) -> one SIGQ_* counter, or -1. Diagnostics: a test that asserts
 * "the handler ran" is asserting about ring 3; these let it also assert that
 * the KERNEL delivered exactly once, which is what catches a double push. */
#define SYS_SIGQUERY    136

/* 137-139 reserved for this line. */

/* --- SYS_KILL 96, extended -------------------------------------------------
 * The old call is (pid, 0, 0) and means DESTROY THAT PROCESS -- no number, no
 * handler, straight to the deferred-teardown mark. c/apps/gui/monitor.c (the
 * task manager) is its only caller and it still works BIT FOR BIT, because the
 * signal meaning is behind a flag in the THIRD argument rather than a value in
 * the second:
 *
 *   (pid, sig, LOGIT_KILL_SIGNAL)   POSIX kill(2). sig 0 = existence probe.
 *   (pid, anything, 0)              the historical destroy. Unchanged.
 *
 * A flag and not "sig != 0 means signal" because POSIX gives sig == 0 a
 * meaning of its own, and a task manager passing 0 must not be reinterpreted as
 * a program probing for existence. */
#define LOGIT_KILL_SIGNAL 1

/* Signal numbers. Linux/x86-64, for the same reason <errno.h> is: a ported
 * program's own tables are written against them. c/apps/libc/include/signal.h
 * carries the identical list and the two are checked against each other by
 * tests/unit/signal_test.c. */
#define LOGIT_SIGHUP     1
#define LOGIT_SIGINT     2
#define LOGIT_SIGQUIT    3
#define LOGIT_SIGILL     4
#define LOGIT_SIGTRAP    5
#define LOGIT_SIGABRT    6
#define LOGIT_SIGBUS     7
#define LOGIT_SIGFPE     8
#define LOGIT_SIGKILL    9
#define LOGIT_SIGUSR1   10
#define LOGIT_SIGSEGV   11
#define LOGIT_SIGUSR2   12
#define LOGIT_SIGPIPE   13
#define LOGIT_SIGALRM   14
#define LOGIT_SIGTERM   15
#define LOGIT_SIGCHLD   17
#define LOGIT_SIGCONT   18
#define LOGIT_SIGSTOP   19
#define LOGIT_SIGTSTP   20
#define LOGIT_SIGTTIN   21
#define LOGIT_SIGTTOU   22
#define LOGIT_SIGURG    23
#define LOGIT_SIGWINCH  28
#define LOGIT_NSIG      32      /* signals are 1..31; the mask is one uint64 */

/* sigprocmask `how`. Same values as the libc header. */
#define LOGIT_SIG_BLOCK   0
#define LOGIT_SIG_UNBLOCK 1
#define LOGIT_SIG_SETMASK 2

/* sa_flags. Only the three that change kernel behaviour are honoured; the rest
 * are accepted and ignored, and which is which is stated here rather than left
 * to be discovered.
 *
 * HONOURED: SA_RESETHAND (reset to SIG_DFL before the handler runs -- this is
 * what signal() is specified with), SA_NODEFER (do not add the signal itself to
 * the mask for the duration), SA_RESTART (a syscall interrupted by this handler
 * is restarted rather than returning EINTR -- see the note on SIG_E_INTR).
 *
 * IGNORED: SA_SIGINFO (there is no siginfo_t here; a three-argument handler is
 * called with rsi = 0 and rdx = the struct logit_sigctx, so it can read the
 * machine state but not a siginfo), SA_NOCLDSTOP and SA_NOCLDWAIT (there is no
 * job control to report and zombies are always reapable). */
#define LOGIT_SA_NOCLDSTOP 0x00000001
#define LOGIT_SA_NOCLDWAIT 0x00000002
#define LOGIT_SA_SIGINFO   0x00000004
#define LOGIT_SA_RESTART   0x10000000
#define LOGIT_SA_NODEFER   0x40000000
#define LOGIT_SA_RESETHAND 0x80000000

/* SYS_SIG* results. */
#define SIG_E_ARG    (-1)   /* bad signal number, bad `how`, or an unusable pointer */
#define SIG_E_SRCH   (-2)   /* no such process */
#define SIG_E_PERM   (-3)   /* refused: the protected console process */
#define SIG_E_NOSYS  (-5)   /* the call exists but this case is not implemented */

/* THE INTERRUPTED-SYSCALL RETURN, and it is deliberately not -1.
 *
 * A blocking call that is cut short by a delivered signal must be
 * distinguishable from one that failed, or a shell cannot tell "Ctrl+C" from
 * "no such child" -- and -1 already means the latter everywhere in this ABI. So
 * interrupted returns its own value, and mini-libc turns it into errno EINTR.
 * The numeric value is -EINTR with EINTR = 4, so a reader who knows the errno
 * table can recognise it on sight. */
#define SIG_E_INTR   (-4)

/* SYS_SIGQUERY selectors. */
#define SIGQ_DELIVERED 0   /* frames pushed onto a user stack since boot */
#define SIGQ_RETURNED  1   /* SYS_SIGRETURNs that restored one; must equal the above
                            * once every handler has returned -- a leak of one is a
                            * frame that was pushed and never unwound */
#define SIGQ_POSTED    2   /* signals raised (delivered, ignored or defaulted) */
#define SIGQ_DEFAULTED 3   /* signals that took their default action */
#define SIGQ_DROPPED   4   /* refused for lack of user stack, or a bad frame */
#define SIGQ_PENDING   5   /* the calling process's pending set right now */
#define SIGQ_BLOCKED   6   /* the calling process's blocked mask right now */
#define SIGQ_FPUSAVED  7   /* frames whose FXSAVE area was actually written.
                            * The negative control (SIGNAL_NO_FPU) builds a kernel
                            * that delivers correctly and leaves this at 0. */

/* What ring 3 hands SYS_SIGACTION. `unsigned long` throughout so tools/gen_abi.py
 * can lay it out and AetherScript can build one -- the same rule struct
 * logit_thread_spec follows. */
struct logit_sigaction {
    unsigned long handler;   /* ring-3 address. 0 = SIG_DFL, 1 = SIG_IGN. */
    unsigned long mask;      /* additionally blocked while the handler runs */
    unsigned long flags;     /* LOGIT_SA_* */
    unsigned long restorer;  /* REQUIRED, and 0 is refused. See below. */
};

/* WHY A RESTORER IS REQUIRED, rather than the kernel supplying a trampoline.
 *
 * A handler is an ordinary C function: it ends in `ret`, so something must be
 * at [rsp] when it starts, and that something has to execute `int 0x80` with
 * rax = SYS_SIGRETURN. On Linux that code lives in the VDSO. There is no VDSO
 * here, and the two alternatives are both worse: writing the instructions into
 * the frame means executing off the user stack, which is exactly what the NX
 * bit on a data page exists to stop; and mapping a kernel-owned executable page
 * into every address space is page-table work in c/kernel/mm/ for four
 * instructions.
 *
 * So ring 3 supplies the four instructions, the same way Linux did with
 * SA_RESTORER before the VDSO existed. mini-libc fills this in for every
 * sigaction() and signal() call, so no ordinary program ever sees it; a raw
 * SYS_SIGACTION with restorer 0 is refused with SIG_E_ARG rather than delivered
 * to a handler that would return into nothing. */

/* The machine state the kernel pushes, and SYS_SIGRETURN restores.
 *
 * LAYOUT ON THE USER STACK at the moment the handler starts:
 *
 *      rsp+0                     the restorer address (the handler's `ret` target)
 *      rsp+8                     struct logit_sigctx      (16-byte aligned)
 *      rsp+8+sizeof(sigctx)      512 bytes of FXSAVE area (16-byte aligned)
 *
 * rsp % 16 == 8 at handler entry, which is what the SysV ABI guarantees after a
 * `call` -- a handler compiled with SSE will use `movaps` on its own locals and
 * getting this wrong is a #GP inside the handler.
 *
 * The struct's size is a multiple of 16 on purpose, so the FXSAVE area that
 * follows it lands aligned without padding arithmetic. rdi = the signal number,
 * rsi = 0 (no siginfo), rdx = &this struct.
 *
 * Field order is the order c/boot/isr.asm pushes them, so the copy in and out is
 * a straight loop rather than fifteen assignments that can be mistyped. */
struct logit_sigctx {
    unsigned long r15, r14, r13, r12, r11, r10, r9, r8;
    unsigned long rbp, rdi, rsi, rdx, rcx, rbx, rax;
    unsigned long rip, rflags, rsp;
    unsigned long oldmask;   /* the blocked mask to restore on sigreturn */
    unsigned long signo;
    unsigned long err;       /* the fault's error code, 0 for an async signal */
    unsigned long trapno;    /* the CPU vector, or 0 */
    unsigned long cr2;       /* the faulting address, or 0 */
    unsigned long fpstate;   /* user address of the 512-byte FXSAVE area, or 0
                              * in a build with the FPU state left out (which is
                              * the negative control, and nothing else) */
};


/* ===========================================================================
 * SERVER SOCKETS (140-149) -- answering a connection, not only making one.
 *
 * WHAT WAS MISSING, PLAINLY. Everything above this block that touches the
 * network is a CLIENT. SYS_SOCK_OPEN takes a host name and a port and connects
 * outward; SYS_HTTP_GET fetches; the TLS stack verifies a server's certificate
 * against 130 roots. There was no listen, no accept, and no passive open
 * anywhere in c/net/transport/tcp.c -- so a machine with DHCP, IPv6, four NIC
 * drivers and HTTP/2 could not serve one byte to anybody. Not a web server, not
 * a debug port, not a file share.
 *
 * THESE ARE BSD SOCKETS, ON PURPOSE. The names, the argument order and the
 * error convention are the ones every piece of network software already
 * assumes, because the point of this block is software that was NOT written for
 * LogitOS. A private "listen API" would have been less work and would have made
 * every future port a rewrite.
 *
 * AN ACCEPTED CONNECTION IS AN ORDINARY FILE DESCRIPTOR. SYS_ACCEPT returns an
 * fd that SYS_READ, SYS_WRITE, SYS_CLOSE, SYS_DUP2 and SYS_SETNB all work on,
 * and that a child inherits across SYS_FORK. That is the whole difference
 * between a socket userland can hand to something else and a socket that needs
 * its own I/O calls -- with the second kind, `cat </dev/tcp` and a CGI-style
 * fork+exec are both impossible.
 *
 * THE BLOCKING MODEL, AND THE HONEST PART.
 *
 *   SYS_ACCEPT and a read() on a connected socket BLOCK by default: the calling
 *   thread parks on a kernel wait queue, unlinked from the scheduler's run
 *   ring, and is woken by the segment that completes the handshake or delivers
 *   the data. It does not spin and it does not hold the big kernel lock.
 *
 *   SYS_SETNB on the fd makes both return LSK_E_AGAIN instead. That is the same
 *   O_NONBLOCK a pipe already honours in c/kernel/exec/file.c -- deliberately
 *   the same word and the same code, because a second convention for "would
 *   block" is exactly the kind of thing that costs somebody an afternoon.
 *
 *   THERE IS NO select/poll/epoll IN THIS ABI, and this block does not add half
 *   of one. So a server CANNOT wait on several connections at once. The model
 *   that works today is a THREAD PER CONNECTION: SYS_THREAD_CREATE landed
 *   before this did, an accept loop spawning a thread per accepted fd is the
 *   shape /bin/httpd uses, and each thread blocks on its own descriptor. The
 *   ceiling is real and is written down where it will be read: about thirteen
 *   threads per process (see LOGIT_THREADS_MAX above -- VMA_MAXAREA, not the
 *   thread table, is the binding limit), and eight connection slots reserved
 *   for the client path out of thirty-two. A server that needs hundreds of
 *   idle connections needs poll(), and poll() is not this block.
 *
 * THE OTHER CONSTRAINT, SAID BEFORE SOMEBODY DISCOVERS IT. Inbound segments are
 * delivered by the NIC interrupt (c/net/core/net.c routes RX through
 * SOFTIRQ_NET), so a SYN and a request arrive whether or not the window manager
 * is running. But TCP's TIMERS -- retransmission, delayed-ACK flush, TIME_WAIT
 * reaping, and the drain that pushes queued response bytes onto the wire when
 * the window opens -- run in tcp_poll(), which is called only from net_poll(),
 * which is called from the window manager's render loop. A response therefore
 * moves at the WM's cadence (~100 Hz), and on a machine whose WM is wedged a
 * server accepts connections and answers them slowly or not at all. That is a
 * property of the whole network stack, not of this block, and fixing it means
 * giving the network a timer of its own.
 * ======================================================================== */

#define SYS_SOCKET      140 /* (domain, type, protocol) -> fd, or LSK_E_*.
                             * domain = AF_INET, type = SOCK_STREAM|SOCK_DGRAM,
                             * protocol 0. No fd is bound to anything yet. */
#define SYS_BIND        141 /* (fd, const struct logit_sockaddr *, len) -> 0 */
#define SYS_LISTEN      142 /* (fd, backlog) -> 0. backlog is clamped to 8. */
#define SYS_ACCEPT      143 /* (fd, struct logit_sockaddr *peer, timeout_ms)
                             * -> a NEW connected fd, or LSK_E_*.
                             * `peer` may be NULL. timeout_ms is the deadline
                             * for the blocking form: 0 means "the default",
                             * which is to wait indefinitely by re-parking; a
                             * non-blocking fd (SYS_SETNB) ignores it and
                             * returns LSK_E_AGAIN at once. */
#define SYS_GETSOCKNAME 144 /* (fd, struct logit_sockaddr *out, 0) -> 0.
                             * The local address+port -- which is how a caller
                             * that bound port 0 learns what it actually got. */
#define SYS_SETSOCKOPT  145 /* (fd, (level<<16)|optname, value) -> 0.
                             * Value-in-a-register rather than the POSIX
                             * pointer+length: every option here is one int, and
                             * a pointer would be a copy-in for no gain. */
#define SYS_SHUTDOWN    146 /* (fd, how) -> 0. how = SHUT_WR sends the FIN and
                             * KEEPS RECEIVING -- an HTTP/1.0 server saying "the
                             * body ends here" without discarding the peer's
                             * reply. SHUT_RD/SHUT_RDWR are accepted. */
#define SYS_RECVFROM    147 /* (fd, struct logit_dgram *) -> bytes, or LSK_E_*.
                             * UDP only. Fills `addr` with the sender. */
#define SYS_SENDTO      148 /* (fd, const struct logit_dgram *) -> bytes sent */
#define SYS_SOCKSTAT    149 /* (what) -> one SOCKSTAT_* counter below, or -1.
                             * Observability, and the reason a test can assert
                             * that a limit FIRED rather than assert that the
                             * machine did not crash. */

/* Address families and socket types. Only what is implemented is defined: an
 * AF_INET6 constant this kernel would refuse is worse than no constant. */
#define LOGIT_AF_INET    2
#define LOGIT_SOCK_STREAM 1
#define LOGIT_SOCK_DGRAM  2

/* An address. PORT AND ADDR ARE HOST ORDER, like every other IP in this ABI
 * (see SYS_NET_PING and struct logit_netinfo) -- NOT network order as in
 * <netinet/in.h>. That is a deliberate break with BSD: this ABI has used host
 * order for an IPv4 address everywhere since M9, and one struct that disagreed
 * would be a byte-swap bug in whichever direction the caller guessed. */
struct logit_sockaddr {
    unsigned short family;      /* LOGIT_AF_INET */
    unsigned short port;        /* host order */
    unsigned int   addr;        /* host order: a<<24|b<<16|c<<8|d, 0 = any */
};

/* One datagram, for SYS_RECVFROM / SYS_SENDTO. A struct rather than four
 * registers because the kernel takes at most three arguments after the call
 * number and a datagram needs buffer, length and address. */
struct logit_dgram {
    /* `unsigned char *` rather than `void *`: tools/gen_abi.py parses this
     * header to generate AetherScript's view of it and refuses a type it cannot
     * lay out rather than guessing. A byte pointer is what the buffer is. */
    unsigned char *buf;
    int            len;         /* capacity on recv, byte count on send */
    int            flags;       /* reserved, must be 0 */
    /* The peer, spelled out rather than an embedded struct logit_sockaddr --
     * same three fields at the same offsets, but gen_abi.py lays out scalars
     * and byte arrays only and refuses a nested struct rather than guessing at
     * its padding. Flattening is the change that costs a caller nothing. */
    unsigned short family;      /* LOGIT_AF_INET */
    unsigned short port;        /* host order; the sender on recv, the
                                 * destination on send */
    unsigned int   addr;        /* host order */
};

/* setsockopt levels and options. */
#define LOGIT_SOL_SOCKET  1
#define LOGIT_SO_REUSEADDR 2   /* rebind a port a previous listener left behind.
                                * On this stack a listener is torn down
                                * synchronously and holds no TIME_WAIT of its
                                * own, so this is accepted and recorded for
                                * source compatibility and changes little; it
                                * is NOT silently ignored -- SYS_SOCKSTAT
                                * reports it, so a port that is genuinely in
                                * use still fails loudly. */
#define LOGIT_SO_RCVTIMEO 3    /* milliseconds; 0 = wait indefinitely */
#define LOGIT_TCP_NODELAY 4    /* level LOGIT_SOL_SOCKET here too: this ABI has
                                * one level, because IPPROTO_TCP as a second
                                * one would buy a namespace nothing needs. */

/* shutdown() directions, the POSIX numbers. */
#define LOGIT_SHUT_RD   0
#define LOGIT_SHUT_WR   1
#define LOGIT_SHUT_RDWR 2

/* SYS_SOCKSTAT selectors. Every one of these is a counter the TCP layer keeps
 * anyway (c/net/transport/tcp.c tcp_server_stats); publishing them is what
 * lets a test distinguish "the backlog limit refused this connection" from
 * "the connection vanished". */
#define SOCKSTAT_SYN_RECEIVED    0
#define SOCKSTAT_ACCEPTED        1
#define SOCKSTAT_REFUSED_BACKLOG 2   /* the listener's queue was full */
#define SOCKSTAT_REFUSED_SLOTS   3   /* the client reserve stopped a passive open */
#define SOCKSTAT_REFUSED_NOPORT  4   /* a SYN for a port nobody listens on */
#define SOCKSTAT_FREE_CONNS      5   /* connection slots still available */
#define SOCKSTAT_LISTENERS       6   /* listening ports right now */

/* SYS_SOCKET..SYS_SENDTO results. Distinct codes, for the reason the settings
 * and thread blocks both give: one "-1" covering "bad descriptor", "port taken"
 * and "nothing to accept yet" makes the third indistinguishable from the first
 * two, and the third is not even an error. */
#define LSK_E_ARG    (-1)   /* bad fd, unsupported family/type, malformed address */
#define LSK_E_AGAIN  (-2)   /* would block: nothing to accept, no datagram yet.
                             * Same value SYS_READ uses on a non-blocking pipe. */
#define LSK_E_INUSE  (-3)   /* that port is already bound */
#define LSK_E_FULL   (-4)   /* no listener slot, no fd, or no connection slot */
#define LSK_E_STATE  (-5)   /* listen on a connected socket, accept on an
                             * unbound one, sendto on a stream -- the call is
                             * fine, the socket is in the wrong state for it */
#define LSK_E_NET    (-6)   /* no NIC is up */

/* --- M32 identity: who is running this (150-159) ---------------------------
 *
 * WHAT WAS ALREADY HERE, AND WHY IT DID NOTHING
 * ---------------------------------------------|
 * The filesystem has enforced permissions for a while: c/fs/vfs.c checks
 * search permission on every directory of a path walk, and write permission on
 * the file or its parent, against a `struct vcred` from c/fs/vfs_cred.c. Since
 * f647bed the mode and the owner are on the MEDIUM (logitfs getattr/setattr),
 * so they survive a reboot. Every part of that was real except one: nothing
 * could set the credential to anything but uid 0, so every check compared a
 * file's owner against root and every check passed. The enforcement was real
 * and the identity was a constant.
 *
 * These ten numbers are the identity.
 *
 * NO SEPARATE EFFECTIVE UID, said plainly. geteuid() == getuid() and
 * getegid() == getgid(), always. That is not laziness: an effective uid exists
 * to be raised by a setuid BINARY, and there is no setuid bit in this
 * filesystem -- vmeta's mode is nine permission bits and nothing else, and
 * c/fs/vfs_cred.c states the rule the whole model rests on ("root may become
 * anybody; anybody else may not change uid at all", which is what makes
 * dropping privilege one-way). Adding euid without the mechanism that raises
 * it would be a field that is always equal to another field, and a caller that
 * believed it meant something would be believing a decoration. When a setuid
 * bit lands, euid lands with it and geteuid stops being an alias.
 *
 * SETUID IS ONE-WAY AND PERMANENT for the calling process. There is no
 * saved-set-uid to come back through. `setuid` from a non-root uid to any
 * other uid -- including the one you already have -- is ID_E_PERM.
 *
 * INHERITANCE is what a credential does across fork and execve: nothing. A
 * child is its parent's credential and an exec'd image keeps the caller's, so
 * /bin/login authenticates, drops to the user, and execve's /bin/sh, which is
 * already the user without ever having been root.
 *
 * THE SESSION (158/159) is the one idea here that is not straight POSIX, and
 * it exists because this machine boots a desktop with no login on it. The
 * window manager launches files.aex before anything has authenticated, and
 * those processes descend from a kernel thread, so they have no credential to
 * inherit. The session is the answer to "who is a process that was never told
 * belong to": before anyone logs in it is root (the boot state, which is what
 * makes mounting the filesystem and loading fonts work), and after /bin/login
 * succeeds it is the person at the keyboard. SYS_SETSESSION sets it AND the
 * caller's own credential in one call, because doing them in either order
 * separately leaves a window where login has already lost root and cannot
 * finish. Root only. */
#define SYS_GETUID     150 /* () -> uid. Never fails. */
#define SYS_GETEUID    151 /* () -> uid. Identical to SYS_GETUID; see above. */
#define SYS_GETGID     152 /* () -> gid. Never fails. */
#define SYS_GETEGID    153 /* () -> gid. Identical to SYS_GETGID. */
#define SYS_SETUID     154 /* (uid) -> 0, or ID_E_PERM. Root only, one-way. */
#define SYS_SETGID     155 /* (gid) -> 0, or ID_E_PERM. Root only. */
#define SYS_GETGROUPS  156 /* (n, uint32_t *list) -> count written, or the total
                            * count when n == 0 (list may then be NULL). */
#define SYS_SETGROUPS  157 /* (n, const uint32_t *list) -> 0. Root only.
                            * n == 0 clears the supplementary set. */
#define SYS_SETSESSION 158 /* (uid, gid) -> 0. Root only. Establishes the login
                            * session AND the caller's credential atomically.
                            * This is the call /bin/login makes -- and the
                            * greeter (c/apps/gui/greeter.c), which is the same
                            * event arriving through the desktop's door.
                            *
                            * IT HAS ONE SIDE EFFECT AND IT IS DOCUMENTED
                            * BECAUSE IT IS NOT GUESSABLE: the kernel uses this
                            * call -- the last instant at which the caller can
                            * still read root:root 0600 /etc/passwd -- to point
                            * the settings store at that user's
                            * $HOME/.config/settings.conf, layered over
                            * /etc/settings.conf as read-only defaults. See
                            * c/kernel/core/settings.h and the SYS_SETSESSION
                            * case in c/kernel/exec/syscall.c. A caller does
                            * nothing to opt in and cannot opt out; there is no
                            * ordering for it to get wrong. */
#define SYS_GETSESSION 159 /* (which) -> the session uid (which == 0) or gid
                            * (which == 1). Never fails; before a login both
                            * are 0, i.e. the machine belongs to root. */

/* At most this many supplementary groups per process. Small on purpose: the
 * whole set is copied on every fork-equivalent lookup, and a machine with one
 * account and no group database has no use for sixty-four. */
#define ID_NGROUPS_MAX 8

#define ID_E_PERM   (-1)   /* not root, or a non-root process trying to setuid */
#define ID_E_ARG    (-2)   /* bad pointer, or n > ID_NGROUPS_MAX */
#define ID_E_NOPROC (-3)   /* no calling process (a kernel thread) */

/* ============================================================================
 * M28 -- AetherScript capabilities. Implementation spec:
 * docs/superpowers/specs/2026-08-14-m28-capabilities.md (D-numbers below refer
 * to its sections). The design document this supersedes on this point is
 * 2026-08-05-aetherscript-2-language-design.md.
 *
 * THE MODEL, IN ONE SENTENCE (D1). A capability is not granted by WHICH BINARY
 * runs -- every AetherScript program is the same binary, /bin/as, so that
 * question has no answer that distinguishes two scripts. It is granted by
 * PROCESS LINEAGE: a child's capability set must be a subset of its parent's,
 * checked by the kernel and by nothing else. proc_spawn() (the kernel
 * launching init's shell, c/kernel/exec/exec.c) is the one root where a grant
 * exists "by construction" rather than by narrowing something held already.
 *
 * WHERE THE STATE LIVES. struct proc (c/kernel/exec/proc.h) carries `caps` (a
 * CAP_* bitmap) and `fs_prefix` (further scopes CAP_FS to one subtree). SYS_FORK
 * copies both unchanged; SYS_EXECVE touches neither (replacing the image is not
 * replacing the grant -- see the comment in proc_execve()); SYS_CAP_SPAWN below
 * is the only way a PROCESS ends up with a set narrower than its parent's. */

/* CAP_* -- the bits struct proc's `caps` field is built from. Three for M28
 * (spec D6): filesystem access BY PATH, network access, and the raw/unmediated
 * escape hatch. Hex literals, matching this file's existing bitmask style
 * (LOGIT_PROC_GUI et al. above), not shifted expressions.
 *
 * CAP_RAW gates `peek`/`poke` and the `syscall()` passthrough in AetherScript --
 * but NOT here. Spec D4: those never reach the kernel (as_ll_peek/poke are
 * plain volatile dereferences in the process's own address space; there is no
 * syscall to intercept), so CAP_RAW is enforced per-call in c/apps/as/
 * as_native.c, a file this line does not own. It is defined here anyway
 * because the BIT VALUE is shared ABI: SYS_CAP_SPAWN's request and
 * SYS_CAP_QUERY's answer both carry it, even though the kernel dispatch gate
 * (syscall_cap_class() in c/kernel/exec/syscall.c) never tests it -- there is
 * no kernel syscall class that means "raw memory access". */
#define CAP_FS   0x01   /* open/read/write/delete/mkdir/rename/stat/symlink/... by PATH */
#define CAP_NET  0x02   /* net info/dns/http/sockets, client and server */
#define CAP_RAW  0x04   /* peek/poke + syscall() -- enforced in as_native.c, not here */
/* CAP_PROC and CAP_GUI exist so the GRANT can express them; the kernel's
 * dispatch gate does not test them YET, and that split is deliberate.
 *
 * Without these bits the model has a hole that only appears when the two halves
 * are wired together: AetherScript's run()/pipe() require AS_CAP_PROC
 * (c/apps/as/as_port.c), so a faithful kernel->language translation with no
 * CAP_PROC to translate would deny process creation to EVERY script on the
 * machine -- killing fsroot/as/examples/ash.as, which is M27's entire payoff.
 * The alternative, inventing the permission during translation, is worse: it
 * makes the language claim an authorization the kernel never made, in the one
 * codepath whose whole job is to carry authority faithfully.
 *
 * So the bit is real ABI: a parent can withhold it through SYS_CAP_SPAWN,
 * proc_cap_subset() narrows it like any other, SYS_CAP_QUERY reports it, and
 * c/apps/as enforces it per call. What is NOT here is an entry in
 * syscall_cap_class() (c/kernel/exec/syscall.c) mapping SYS_FORK/SYS_EXECVE and
 * the SYS_GUI_* family to them -- adding kernel enforcement that cannot be
 * booted and tested is how a machine ships broken, and this tree currently
 * cannot build an ISO (c/kernel/mm/fault.c, another line's half-finished work).
 * That classification is the named follow-up, and it is additive: the bits it
 * would test already travel correctly. */
#define CAP_PROC 0x08   /* fork/execve/spawn -- carried and narrowed here, enforced in as_port.c */
#define CAP_GUI  0x10   /* SYS_GUI_* -- same: carried here, not yet in syscall_cap_class() */
#define CAP_ALL  (CAP_FS | CAP_NET | CAP_RAW | CAP_PROC | CAP_GUI)  /* what proc_spawn() grants the console shell */

/* The requested grant for a SYS_CAP_SPAWN child. This is the FULL request, not
 * a delta from the caller's own grant, and the kernel accepts it only if it is
 * <= what the caller currently holds (proc_cap_subset() in
 * c/kernel/exec/proc.c): `caps` a bitwise subset, and `prefix` either empty or
 * a component-wise extension of the caller's own current prefix. `prefix[0]==0`
 * requests NO path scoping, which is only a legal request when the caller
 * itself is unscoped -- a scoped caller can never hand out an unscoped child.
 *
 * A plain struct, not packed register arguments (contrast SYS_CLIP_SET /
 * SYS_NOTIFY's note above, which chose registers deliberately): this needs a
 * path string's worth of data (`prefix`) alongside a bitmap, which does not
 * fit in the two general-purpose registers int 0x80 leaves free after `path`
 * and `argv`. The layout is ordinary scalars + one fixed byte span, so
 * tools/gen_abi.py can still emit and _Static_assert-check the offsets --
 * see the header comment on that tool before hand-writing an AetherScript
 * struct literal against this shape. */
struct logit_capreq {
    unsigned long caps;
    char          prefix[64];
};

#define SYS_CAP_SPAWN 160 /* (path, argv[], const struct logit_capreq *req) ->
                           * child pid, or a negative LOGIT_CAP_E_* code.
                           *
                           * fork()+load+execve(path) in ONE kernel call, like
                           * the historical SYS_SPAWN's documented shape, except
                           * the child's struct proc does NOT inherit a copy of
                           * the caller's capability set -- it gets exactly
                           * `req`, and the whole call is refused (no address
                           * space allocated, no child created, nothing
                           * partially built) unless proc_cap_subset() accepts
                           * `req` against the CALLER's own current (caps,
                           * fs_prefix). This is D1's ceiling, enforced at the
                           * one place a process's grant can shrink.
                           *
                           * The child otherwise behaves like a SYS_FORK child
                           * that immediately SYS_EXECVE'd: it inherits the
                           * caller's fd table (dup'd) and cwd. It does NOT
                           * inherit envp (this call has no register left to
                           * carry one; the child's environment is empty, the
                           * same as SYS_SPAWN's original documented shape and
                           * the kernel's own proc_spawn()). See
                           * proc_cap_spawn() in c/kernel/exec/exec.c. */
#define SYS_CAP_QUERY 161 /* (buf, max, 0) -> the CALLING process's own current
                           * CAP_* bitmap (the return value itself, like
                           * SYS_GETPID). If buf != NULL and max > 0, also
                           * copies the caller's current fs_prefix
                           * (NUL-terminated, truncated to max-1 bytes) into
                           * buf. Never fails -- there is nothing to refuse in
                           * reading your own state, and no argument
                           * combination is invalid (a NULL/zero buf just skips
                           * the prefix copy).
                           *
                           * WHY THIS EXISTS, though the milestone spec's DO
                           * list names only ONE new syscall. Neither argv nor
                           * envp can carry a grant (both are caller-forgeable
                           * across execve -- spec section 1), so a process has
                           * no OTHER way to learn what it was actually handed.
                           * Without this call the model type-checks but has no
                           * bootstrap: AetherScript's as_run() would have
                           * nothing to build its first capability object from
                           * on the real kernel (spec D9 covers only the HOST
                           * test harness, which grants through a C entry point
                           * that has no kernel equivalent). A read-only query
                           * of a process's own state needed no ceiling check
                           * to design, so it is the minimal completion, not an
                           * extra privilege. */
#define LOGIT_CAP_E_ARG  (-1)   /* bad path/argv/pointer, or an unreadable request */
#define LOGIT_CAP_E_CEIL (-2)   /* `req` is not <= the caller's own (caps, fs_prefix) */
#define LOGIT_CAP_E_NOENT (-3)  /* path/image missing, unreadable, or not a valid .aex */
#define LOGIT_CAP_E_NOMEM (-4)  /* OOM building the child's address space or thread */

#endif /* LOGIT_ABI_H */
