#ifndef LOGIT_SETTINGS_H
#define LOGIT_SETTINGS_H

/* ============================================================================
 * The settings store -- what this machine remembers about its user.
 *
 * Until this file existed, every boot was the machine's first: the theme, the
 * wallpaper, the network configuration and every window position were compiled
 * in, and nothing a user did survived a power cycle. The disk has been provably
 * durable since 2026-08-08 (five real boots, three files byte-for-byte, 1744
 * host crash points) and there was nothing on it that belonged to the user.
 * This closes that gap.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS IS IN THE KERNEL AND NOT A LIBRARY
 * ---------------------------------------------------------------------------
 * The obvious alternative -- a library each app links, reading and writing the
 * same file -- fails on four counts, and the fourth is fatal:
 *
 *   1. The window manager needs the theme and the wallpaper path BEFORE any
 *      ring-3 process exists.  wm_init() runs from kernel_main; there is no
 *      app to ask.
 *   2. net_cfg is kernel state, chosen in net_init() before userland starts.
 *   3. A change notification has to reach every app's event queue, and only
 *      the kernel can push into another process's queue.
 *   4. LogitFS rewrites a WHOLE FILE per write (see logitfs_write ->
 *      inode_write).  Two apps holding two copies of the settings and each
 *      writing the whole file back would silently destroy each other's keys --
 *      not a race on one key, a total loss of everything the other app had
 *      loaded.  There has to be exactly one writer, and it has to be the one
 *      thing both apps are already talking to.
 *
 * ---------------------------------------------------------------------------
 * THE FORMAT: /etc/settings.conf, one "key = value" per line
 * ---------------------------------------------------------------------------
 *     # LogitOS settings.
 *     ui.dark = 1
 *     ui.accent = 0x5E96FF
 *     ui.wallpaper = /wallpaper.png
 *     net.dhcp = 1
 *     # crc32 = 4a1c93e2
 *
 * Text, because the first time this file corrupts somebody is going to `cat`
 * it and then fix it in TextEdit, and both of those have to work.  Binary
 * would be faster and nothing here is remotely hot -- the file is read once at
 * boot and written when a human moves a slider.
 *
 * One file, not a directory of them.  A directory multiplies the number of
 * things that can be half-written by the number of apps and buys no atomicity:
 * on this filesystem one file is one transaction, and a settings change that
 * spans two files could commit half of itself.
 *
 * ---------------------------------------------------------------------------
 * CORRUPTION, WHICH IS THE ACTUAL DESIGN PROBLEM
 * ---------------------------------------------------------------------------
 * There are two independent sources of a bad file, and they need different
 * answers because only one of them is the filesystem's fault.
 *
 * (A) THE POWER GOES OUT MID-WRITE.  Handled by the filesystem, and the reason
 *     this code does no temp-file dance:
 *
 *       - logitfs_write() on an existing file calls inode_write(), which calls
 *         inode_trunc() and then balloc()s FRESH blocks for the entire file.
 *         The truncate's frees are DEFERRED to commit (bfree()), so the
 *         allocator cannot hand back a block the same operation just released.
 *         The new data therefore never lands on top of the old.
 *       - The new blocks are written and barriered (B1) before the commit
 *         record, and the inode that points at them installs only after it.
 *       - So the crash-consistency invariant stated above log_commit() in
 *         c/fs/logitfs.c applies verbatim: "every file is byte-for-byte its
 *         content before Tk+1 or after Tk+1, never a mixture".
 *
 *     A whole-file vfs_write() is already atomic.  I checked this rather than
 *     assuming it, and I also checked the idiom I would otherwise have used:
 *
 *       *** vfs_rename() CANNOT REPLACE AN EXISTING FILE ON THIS SYSTEM. ***
 *       logitfs_rename() opens with `if (resolve(new_path) != NOINO) return
 *       -1;` -- an explicit no-clobber.  The classic "write temp, rename over
 *       the original" is therefore not available; it would have to become
 *       delete-then-rename, which is TWO transactions with a window in between
 *       where the machine has no settings file at all.  That is strictly worse
 *       than the single atomic write this code does, so temp+rename is not
 *       merely unnecessary here, it is the wrong answer.  (The rename itself
 *       IS atomic -- one tx_begin/log_commit -- it just will not clobber.)
 *
 * (B) A HUMAN EDITS THE FILE.  No amount of journalling helps with this, and
 *     it is the case that actually happens.  The format is built so that a
 *     damaged file degrades instead of failing:
 *
 *       - LINES ARE INDEPENDENT.  One bad line costs one key, never the file.
 *       - A FINAL LINE WITH NO TERMINATING NEWLINE IS DISCARDED.  The writer
 *         always terminates every line, so an incomplete last line can only be
 *         a truncation or a tear.  This is what makes "truncate at every byte
 *         offset" safe BY CONSTRUCTION rather than by testing: cutting the
 *         file anywhere leaves a prefix of whole lines plus at most one
 *         partial line, and the partial one is never parsed.
 *       - VALUES ARE RANGE-CHECKED ON READ, NOT ON WRITE.  A file edited by
 *         hand will contain nonsense eventually; checking at set() time proves
 *         nothing about a value that arrived through TextEdit.  Every get_*
 *         validates and falls back to the compiled-in default.
 *       - THE CRC IS A DIAGNOSTIC, NOT A GATE.  The trailing `# crc32 = ...`
 *         line makes a torn write visible in the boot log.  It must NEVER
 *         refuse the file, because a hand-edited file has a stale CRC and is
 *         exactly the file the user is trying to repair.
 *
 * The result: there is no input to this parser that prevents the desktop from
 * coming up.  The worst case is a machine that boots with defaults and says on
 * the serial line which lines it rejected and why.
 *
 * ---------------------------------------------------------------------------
 * SCHEMA vs STORE
 * ---------------------------------------------------------------------------
 * The STORE is a flat key/value table and it holds ANY key, including ones
 * this kernel has never heard of -- an unknown key is preserved across a
 * rewrite rather than dropped.  That is deliberate: another line wanting to
 * persist something (a notification history, a clipboard pin) gets to use this
 * store immediately without a kernel change, and never has to build a second
 * one.
 *
 * The SCHEMA is a separate declaration of the keys this kernel understands --
 * type, range, default, human label, group.  It supplies defaults and range
 * checks, and it is what the Settings app enumerates, so a key added here
 * appears in the UI with no app change.
 * ========================================================================== */

#include <stdint.h>

/* ---- schema value types ---- */
enum {
    SET_T_BOOL = 0,   /* 0 or 1                                   */
    SET_T_INT  = 1,   /* decimal, clamped to [lo,hi]              */
    SET_T_COLOR= 2,   /* 0xRRGGBB                                 */
    SET_T_STR  = 3,   /* free text, length-checked                */
    SET_T_IP   = 4,   /* dotted quad                              */
    SET_T_ENUM = 5,   /* one of `choices`, '|' separated          */
};

/* ---- schema groups (the Settings app's tabs) ---- */
enum { SET_G_APPEARANCE = 0, SET_G_DESKTOP = 1, SET_G_NETWORK = 2,
       SET_G_SYSTEM = 3, SET_G_NGROUP = 4 };

struct setting_def {
    const char *key;
    const char *label;
    unsigned char type;
    unsigned char group;
    long lo, hi;               /* INT/COLOR bounds (inclusive)      */
    const char *dflt;          /* the default, as it would be written */
    const char *choices;       /* ENUM: "a|b|c"                     */
};

/* ---- limits.  Sized, not guessed: 64 keys x 128 bytes = 8 KiB static. ---- */
#define SET_KEYLEN   48
#define SET_VALLEN   80
#define SET_MAXKV    64
#define SET_PATH     "/etc/settings.conf"

/* ---- load findings (settings_diag(), and the boot log) ---- */
#define SET_D_NOFILE     0x01   /* no settings file: a fresh machine        */
#define SET_D_BADLINE    0x02   /* >=1 line did not parse                   */
#define SET_D_TRUNCATED  0x04   /* file did not end in a newline            */
#define SET_D_CRCBAD     0x08   /* trailing crc32 line disagreed            */
#define SET_D_FULL       0x10   /* more keys in the file than the table holds */
#define SET_D_OUTOFRANGE 0x20   /* >=1 value rejected by its schema range   */

/* ------------------------------------------------------------------------ */
void settings_init(void);          /* load from disk; call after vfs_mount() */
int  settings_load(void);          /* re-read the file; returns SET_D_* bits */
int  settings_commit(void);        /* write the table out; 0, or <0          */
int  settings_reset(void);         /* delete the file, restore defaults      */

/* Reads.  Every one range-checks and falls back to `def` (or to the schema
 * default when there is one), so a caller can never receive nonsense. */
int         settings_get_int(const char *key, int def);
unsigned    settings_get_color(const char *key, unsigned def);
const char *settings_get_str(const char *key, const char *def);
unsigned    settings_get_ip(const char *key, unsigned def);

/* Writes.  `commit` != 0 persists immediately; 0 leaves it in RAM for a caller
 * about to set several keys (the Settings app commits once at the end). */
int settings_set_str(const char *key, const char *val, int commit);
int settings_set_int(const char *key, long val, int commit);

/* Introspection -- the Settings app and SYS_SETTING_ENUM run off these. */
int  settings_schema_count(void);
const struct setting_def *settings_schema(int i);
const struct setting_def *settings_schema_find(const char *key);
int  settings_kv_count(void);                      /* live keys in the table */
int  settings_kv_at(int i, const char **k, const char **v);
unsigned settings_gen(void);        /* bumps on every committed change       */
int  settings_diag(void);           /* SET_D_* from the last load            */

/* ---------------------------------------------------------------------------
 * WINDOW GEOMETRY -- the shape, taken from what the window-management line
 * actually landed rather than guessed at.
 *
 * They shipped SYS_GUI_WIN_MIN / SYS_GUI_WIN_STATE (logit_abi.h 101-102) this
 * round, and that ABI settles every question this side had:
 *
 *   - a frame is measured in POINTS and it is the CONTENT size -- the canvas
 *     below the titlebar, the same coordinate space gui_create() names.  The
 *     titlebar is the WM's and is not counted, so a saved frame stays correct
 *     if the titlebar's height ever changes.
 *   - ZOOMED (maximized) and MINIMIZED are INDEPENDENT booleans in their model
 *     (WINS_ZOOMED, WINS_MINIMIZED), not one state word -- a window can be
 *     minimised while zoomed.  So this record carries two flags, not an enum.
 *     An earlier draft of this file had a single `state` word; it would have
 *     been unable to express a state their WM can be in.
 *   - WINS_SET_ZOOM 0 "restores", which means there is a restore rect the WM
 *     already keeps.  It is saved too, so unzooming after a reboot lands where
 *     the user left it instead of at a default.
 *
 * The stored form is one line per app, space separated, readable and
 * hand-repairable like everything else in the file:
 *
 *     win.clock.frame = 40 60 320 240 1 0 0 40 60 320 240
 *                       x  y  w   h  op z  m  rx ry rw  rh
 *
 * A frame that does not put a usable amount of the window on the CURRENT
 * screen is REFUSED on load.  A window saved on a 1920x1080 desktop must not
 * come back unreachable on an 800x600 one -- an off-screen window is the
 * single most common way a persisted position bricks a desktop, and it is the
 * garbage case the brief names.
 *
 * THE TWO CALL SITES ARE THEIRS, NOT MINE.  This side is complete and tested
 * (round-tripped across a real reboot by tests/boot/run-settings-test.sh); the
 * WM adds one settings_frame_load() where a window is created and one
 * settings_frame_save() where a drag/resize/zoom finishes.  `open` is the bit
 * that answers "which apps were open", and desktop.restore_session is the flag
 * that says whether to act on it. */
struct win_frame {
    int x, y, w, h;       /* content frame, points (gui_create's space) */
    int open;             /* was this app open when the session ended */
    int zoomed;           /* WINS_ZOOMED  */
    int minimized;        /* WINS_MINIMIZED */
    int rx, ry, rw, rh;   /* restore rect (== x/y/w/h when not zoomed) */
};

/* 1 if a usable frame was loaded, 0 if there is none or the stored one was
 * rejected (in which case the caller should do exactly what a fresh machine
 * does, and NOT try to salvage the record). */
int  settings_frame_load(const char *app, struct win_frame *f,
                         int screen_w, int screen_h);
int  settings_frame_save(const char *app, const struct win_frame *f, int commit);

/* The kernel-side syscall entry (SYS_SETTING_*), called from syscall.c. */
long settings_syscall(long num, long a, long b, long c);

/* The truncate-at-every-byte-offset sweep, run ON THE MACHINE through the real
 * boot-path loader.  Returns the number of offsets at which loading failed to
 * produce a usable configuration -- which must be zero.  See SETCTL_SELFTEST. */
int settings_selftest(void);

#endif /* LOGIT_SETTINGS_H */
