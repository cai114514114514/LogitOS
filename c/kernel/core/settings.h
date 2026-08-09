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

/* ---------------------------------------------------------------------------
 * LIMITS, and the policy they encode.  The browser-tabs line hit these first
 * and asked whether they are right, so the answer belongs here rather than in
 * a reply: THE SPLIT THEY CHOSE IS THE INTENDED ONE, and these numbers are the
 * mechanism that forces it.
 *
 * This store holds SETTINGS: values a human might type, that a human might
 * want to repair in TextEdit, and that are cheap enough to rewrite whole. It is
 * not a database and it must never become one, because the ENTIRE corruption
 * argument rests on the whole store being one file written in one LogitFS
 * transaction.  That property is bought with the size cap.  A 256-entry
 * browsing history in here would mean rewriting -- and re-CRCing, and
 * re-parsing at every boot -- tens of kilobytes every time somebody opens a
 * page, and it would put a megabyte of churn in the path that also carries
 * whether the desktop is dark.
 *
 * So: bulk data goes in FILES (the tabs line's /browser directory is right),
 * and the settings store holds the SWITCH that says whether to read them --
 * `browser.restore_session` is a perfect use of this store and the history is
 * a perfect thing to keep out of it.
 *
 * What WAS wrong, and is fixed: an over-long value used to be silently
 * TRUNCATED.  For a boolean that is harmless; for a consumer storing a path it
 * is data corruption discovered much later and somewhere else.  A value that
 * does not fit is now REFUSED with SET_E_TOOBIG, and the limits are queryable
 * at runtime (SETCTL_LIMITS) so a caller can find out from the machine instead
 * of from this comment.
 *
 * The numbers were raised once, from 80/64, so that an ordinary filesystem path
 * fits comfortably; they were NOT raised to fit a URL, because a 600-byte URL
 * is the case that should be a file.
 *
 * Cost: 96 x 208 = 19.5 KiB of static table, plus a 32 KiB serialisation
 * buffer.  Static rather than kmalloc'd on purpose -- see the note on `buf`.
 */
#define SET_KEYLEN   48
#define SET_VALLEN   160
#define SET_MAXKV    96
#define SET_PATH     "/etc/settings.conf"

/* ---------------------------------------------------------------------------
 * TWO STORES, AND WHY -- the regression this closes, stated before the API.
 * ---------------------------------------------------------------------------
 * /etc/settings.conf is root:root 0600.  settings_commit() writes it through
 * vfs_write(), which uses the CALLING PROCESS's credential (c/fs/vfs.c
 * cred_now).  So from the moment /bin/login started dropping the desktop to a
 * real uid, a user flipping dark mode in the menu bar was REFUSED and their
 * choice was gone at the next boot.  Nothing was wrong with the permission
 * check; the file was simply in the wrong place.  POSIX-correct, product-wrong.
 *
 * So there are two files and they have different jobs:
 *
 *   /etc/settings.conf              THE SYSTEM STORE.  Root's, and after a
 *                                   login it is read-only to everyone else.
 *                                   It supplies DEFAULTS -- which is also the
 *                                   only answer to a question that turns up
 *                                   immediately: the greeter needs a theme and
 *                                   a wallpaper BEFORE anybody has logged in,
 *                                   and at that moment there is no user store
 *                                   to read.
 *
 *   $HOME/.config/settings.conf     THE USER STORE.  Written by the logged-in
 *                                   user, over the system one, key by key.
 *
 * A key present in both: the user's value wins.  A key the user has never
 * touched is NOT copied into their file -- the user store holds only what that
 * user actually changed, so a later change to a system default reaches every
 * user who never overrode it.  That is why each table entry carries `sys`.
 *
 * ON A MACHINE WITH NO ACCOUNTS there is no user, no user path, and this file
 * behaves EXACTLY as it did before: one store, /etc/settings.conf, written by
 * root.  Every pre-existing settings test asserts that behaviour and none of
 * them were changed.
 *
 * WHERE THE HOME COMES FROM.  Not from a path handed in by userland: it is
 * looked up from /etc/passwd, through the SAME header-only parser /bin/login
 * uses (c/apps/coreutils/accounts.h), so there is one definition of the account
 * format and no caller -- root or not -- can point the settings store at a
 * directory of its choosing.  The lookup is driven from SYS_SETSESSION in
 * c/kernel/exec/syscall.c and it is deliberately TWO calls, because the one
 * instant that has to be split is the credential drop:
 *
 *   settings_prepare_user(uid)   runs while /bin/login (or the greeter) is
 *                                STILL ROOT, which is the only moment
 *                                /etc/passwd can be read.  It resolves the
 *                                home and remembers the path.  It switches
 *                                nothing.
 *   settings_adopt_user()        runs after the credential has dropped, so the
 *                                user's own file is read as the user -- which
 *                                is the correct credential for it, and proves
 *                                the file is really theirs.
 *   settings_discard_user()      the session change failed; forget the pending
 *                                path.  Never leave a half-switched store.
 * ------------------------------------------------------------------------ */
#define SET_USER_REL "/.config/settings.conf"
#define SET_PATHLEN  160

/* The buffer a SETCTL_KVAT caller must provide: "key" + '=' + "value" + NUL.
 * Named rather than open-coded, because every caller had independently rounded
 * a guess up and raising SET_VALLEN turned all of those guesses into overflows
 * at once. */
#define SET_KVLINE   (SET_KEYLEN + SET_VALLEN + 2)

/* settings_set_* / SYS_SETTING_SET results.  Distinct, so a caller can say WHY
 * it failed; "-1" for both "your value is too long" and "the machine is full"
 * is the kind of error that costs somebody an afternoon. */
#define SET_E_BADKEY  (-1)   /* empty, over-long, or contains '=' / '#' / space */
#define SET_E_TOOBIG  (-2)   /* the VALUE exceeds SET_VALLEN-1 bytes */
#define SET_E_FULL    (-3)   /* SET_MAXKV keys already stored */
#define SET_E_IO      (-4)   /* the commit could not be written */

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

/* The per-user store.  See the two-store block above for the argument and for
 * why the switch is split in two.  `settings_prepare_user` returns 0 when it
 * resolved a home for `uid`, or <0 -- and a <0 is NOT a reason to refuse the
 * login: a user with no row in /etc/passwd (or an unreadable store) simply
 * keeps the system settings read-only, which is a worse desktop and not a
 * broken one. */
int  settings_prepare_user(unsigned uid);
void settings_adopt_user(void);
void settings_discard_user(void);

/* The file a commit would write, absolute.  "/etc/settings.conf" before a
 * login.  Never NULL. */
const char *settings_store_path(void);

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
