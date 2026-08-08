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
/* a = new content WIDTH in points, b = new content HEIGHT. The window's canvas
 * has ALREADY been reallocated at this size when the event is delivered -- see
 * the SYS_GUI_WIN_* block below. Repaint everything; nothing you drew survived.
 *
 * Coalesced like motion, and for the same reason: a drag produces a stream of
 * sizes and only the newest one is true. An app that polls once per painted
 * frame therefore sees one resize per frame, not one per pointer sample. */
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
                              * which is the point of preserving them. */

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

#endif /* LOGIT_ABI_H */
