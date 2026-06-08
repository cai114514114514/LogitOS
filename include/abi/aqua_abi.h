#ifndef AQUA_ABI_H
#define AQUA_ABI_H

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
#define SYS_NET_INFO   21   /* (struct aqua_netinfo*) -> 1 if a NIC is up, else 0 */
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
#define SYS_GUI_TEXT_RUN 38 /* (struct aqua_run*) draw a length-delimited text run (px/mono/color) */
#define SYS_RES_FETCH    39 /* (src, buf, max) -> fetch a sub-resource's raw bytes; length, or <0 */
#define SYS_GUI_BLIT     40 /* (struct aqua_blit*) blit an RGBA bitmap into the window surface */
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
#define SYS_IMG_DECODE  67 /* (struct aqua_imgreq*) decode an image file -> RGBA in app buffer */
#define SYS_CPU_INDEX   68 /* () -> the index (0..N-1) of the core running the caller (SMP proof) */
#define SYS_KHEAP_STRESS 69 /* (iters, size, seed) -> corruption count; BKL-FREE concurrent kmalloc/kfree stress (M25 P1 gate) */

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

#define EV_MOUSE  2   /* a = x, b = y (window-local), mouse-button down */
#define EV_CLOSE  3   /* the window's close button was pressed */
#define EV_MOUSE_R 4  /* a = x, b = y (window-local), right-button down */

struct aqua_event {
    int type;
    int a;
    int b;
};

/* Wall-clock time (mirrors the kernel's struct rtc_time field order). */
struct aqua_time {
    int year, month, day;
    int hour, minute, second;
    int weekday;
};

/* Network info filled by SYS_NET_INFO. IPs are host order (a.b.c.d packed). */
struct aqua_netinfo {
    unsigned ip, mask, gw;
    unsigned char mac[6];
};

/* M17 L1: payloads for the ring-3 render syscalls. */
struct aqua_run  { int x, y, px, mono; unsigned color; const char *s; int len; };
struct aqua_blit { int x, y, w, h; const unsigned char *rgba; int sw, sh; };

/* SYS_IMG_DECODE: the app provides `path` + an `rgba` buffer of `max` bytes; the
 * kernel decodes the image (PNG/GIF) and fills rgba + w/h (w*h*4 must be <= max). */
struct aqua_imgreq { const char *path; unsigned char *rgba; int max; int w, h; };

#endif /* AQUA_ABI_H */
