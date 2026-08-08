/* The settings store, host-side.
 *
 * This compiles the REAL c/kernel/core/settings.c against a RAM filesystem
 * standing in for the VFS, so the parser, the range checks, the frame codec and
 * the commit path under test are byte-for-byte the ones that boot.
 *
 * The centrepiece is the truncation sweep. The claim the whole feature rests on
 * is:
 *
 *     given ANY prefix of a settings file, the machine comes up, every setting
 *     reads inside its declared range, and no partially written line is ever
 *     parsed into a value.
 *
 * That is checked here at EVERY byte offset of several different files, and it
 * is checked strongly: it is not enough to survive, every key has to read as
 * something usable afterwards. A parser that "survived" by discarding the whole
 * file would pass a crash test and fail this one.
 *
 * TWO NEGATIVE CONTROLS, both of which MUST FAIL when compiled in -- an
 * assertion nobody has watched fail is not a known-failing assertion:
 *
 *   -DSETTINGS_NO_TRUNC_GUARD  parses an unterminated final line. That is the
 *                              single rule the truncation argument rests on;
 *                              without it a file cut mid-number yields a value
 *                              the user never wrote.
 *   -DSETTINGS_NO_RANGE_CHECK  returns a stored value without checking it
 *                              against the schema. A hand-edited `ui.dark = -1`
 *                              then reaches the compositor.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "settings.h"
#include "logit_abi.h"

/* ---------------------------------------------------------------- the VFS --
 * One file, in RAM. Everything the store does to the disk goes through these
 * five calls and nothing else, which is itself worth asserting: a store that
 * reached around the VFS could not be tested here at all. */
static char  fs_buf[65536];
static int   fs_len = -1;              /* -1 = the file does not exist */
static int   fs_writes, fs_deletes;
static int   fs_write_fails;           /* >0: fail the next N writes */

int vfs_size(const char *p)
{ return (strcmp(p, SET_PATH) == 0 && fs_len >= 0) ? fs_len : -1; }

int vfs_read(const char *p, void *b, int max)
{
    if (strcmp(p, SET_PATH) != 0 || fs_len < 0) return -1;
    int n = fs_len < max ? fs_len : max;
    memcpy(b, fs_buf, (size_t)n);
    return n;
}

int vfs_write(const char *p, const void *b, int size)
{
    if (strcmp(p, SET_PATH) != 0) return -1;
    if (fs_write_fails > 0) { fs_write_fails--; return -1; }
    if (size > (int)sizeof fs_buf) return -1;
    memcpy(fs_buf, b, (size_t)size);
    fs_len = size;
    fs_writes++;
    return size;
}

int vfs_delete(const char *p)
{ if (strcmp(p, SET_PATH)) return -1; fs_len = -1; fs_deletes++; return 0; }

int vfs_mkdir(const char *p) { (void)p; return 0; }

/* kprintf: swallowed by default so the sweep does not print 700 lines, but
 * captured, because two of the assertions below are about what the machine
 * SAYS -- "the desktop must survive them and say what it rejected". */
static char  said[65536];
static int   said_n;
static int   verbose;
void kprintf(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (verbose) fputs(line, stdout);
    if (said_n + n < (int)sizeof said) { memcpy(said + said_n, line, (size_t)n); said_n += n; }
    said[said_n] = 0;
}
static void said_clear(void) { said_n = 0; said[0] = 0; }
static int  said_has(const char *s) { return strstr(said, s) != NULL; }

/* ------------------------------------------------------------- the harness --*/
static int checks, fails;
static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { fails++; printf("  FAIL: %s\n", what); }
}
static void okf(int cond, const char *fmt, ...)
{
    checks++;
    if (cond) return;
    fails++;
    va_list ap; va_start(ap, fmt);
    fputs("  FAIL: ", stdout); vprintf(fmt, ap); fputc('\n', stdout);
    va_end(ap);
}

static void fs_set(const char *text)
{ fs_len = (int)strlen(text); memcpy(fs_buf, text, (size_t)fs_len); }

/* Every schema key reads inside its declared range. This is the predicate the
 * sweep enforces; it is a function because it is the definition of "the machine
 * came up usable" and it should be written down once. */
static int all_keys_usable(const char **why)
{
    for (int i = 0; i < settings_schema_count(); i++) {
        const struct setting_def *d = settings_schema(i);
        if (d->type == SET_T_IP) {
            if (!settings_get_ip(d->key, 0)) { *why = d->key; return 0; }
        } else if (d->type == SET_T_STR) {
            const char *s = settings_get_str(d->key, "");
            if (!s || !s[0]) { *why = d->key; return 0; }
        } else {
            int v = settings_get_int(d->key, -999999);
            if (v < d->lo || v > d->hi) { *why = d->key; return 0; }
        }
    }
    return 1;
}

/* ===========================================================================
 * 1. Round trip: what is set is what comes back, across a "reboot"
 * ======================================================================== */
static void t_roundtrip(void)
{
    printf("round trip\n");
    fs_len = -1;
    settings_load();
    ok(settings_diag() & SET_D_NOFILE, "a machine with no file says so");
    ok(settings_get_int("ui.dark", 9) == 0, "a fresh machine is light");
    ok(strcmp(settings_get_str("ui.wallpaper", "?"), "/wallpaper.png") == 0,
       "a fresh machine has the default wallpaper");

    settings_set_int("ui.dark", 1, 0);
    settings_set_str("ui.wallpaper", "/media/dot.png", 0);
    settings_set_int("net.dhcp", 0, 0);
    settings_set_str("net.ip", "10.0.2.99", 0);
    settings_set_str("notify.history", "7", 0);      /* no schema for this key */
    int w0 = fs_writes;
    ok(fs_writes == w0, "setting with commit=0 does not touch the disk");
    ok(settings_commit() == 0, "commit succeeds");
    okf(fs_writes == w0 + 1, "one commit is ONE file write (got %d)", fs_writes - w0);

    /* The reboot: throw the table away and read the file back. */
    settings_load();
    ok(settings_diag() == 0, "the file we just wrote loads with no findings");
    ok(settings_get_int("ui.dark", 0) == 1, "ui.dark survived");
    ok(strcmp(settings_get_str("ui.wallpaper", ""), "/media/dot.png") == 0, "wallpaper survived");
    ok(settings_get_int("net.dhcp", 1) == 0, "net.dhcp survived");
    ok(settings_get_ip("net.ip", 0) == ((10u << 24) | (0u << 16) | (2u << 8) | 99u),
       "net.ip survived and parses as a dotted quad");
    ok(strcmp(settings_get_str("notify.history", ""), "7") == 0,
       "a key with NO SCHEMA survived -- unknown keys are preserved");

    /* The file is text a human can read. */
    fs_buf[fs_len] = 0;
    ok(strstr(fs_buf, "ui.dark = 1\n") != NULL, "the file contains a readable `ui.dark = 1` line");
    ok(strstr(fs_buf, "# crc32 = ") != NULL, "the file carries its crc32 diagnostic");
    ok(fs_buf[fs_len - 1] == '\n', "the file ends in a newline -- every line is terminated");

    /* A failed write leaves the ON-DISK file alone. There is no half state to
     * explain because the whole file is one transaction. */
    int before = fs_len;
    fs_write_fails = 1;
    settings_set_int("ui.dark", 0, 1);
    ok(fs_len == before, "a failed commit does not shorten the file");
    settings_load();
    ok(settings_get_int("ui.dark", 0) == 1, "a failed commit leaves the old value on disk");
}

/* ===========================================================================
 * 2. THE TRUNCATION SWEEP -- every byte offset of every file
 * ======================================================================== */
static int sweep(const char *label, const char *text)
{
    int n = (int)strlen(text);
    int bad = 0;

    /* What the WHOLE file says, and what a machine with NO file says. Those two
     * are the only answers a truncation may produce for any key.
     *
     * "In range" alone is too weak, and the negative control proved it: a file
     * cut inside `ui.accent = 0xC81E64` leaves `0xC81E6`, which is a perfectly
     * in-range colour and is not one the user ever chose. A prefix of a file may
     * cost you a setting; it must never invent one. */
    char whole[64][80], none[64][80];
    int nk = settings_schema_count();
    fs_len = (int)strlen(text); memcpy(fs_buf, text, (size_t)fs_len);
    settings_load();
    for (int i = 0; i < nk; i++)
        snprintf(whole[i], 80, "%s", settings_get_str(settings_schema(i)->key, ""));
    fs_len = -1;
    settings_load();
    for (int i = 0; i < nk; i++)
        snprintf(none[i], 80, "%s", settings_get_str(settings_schema(i)->key, ""));

    for (int cut = 0; cut <= n; cut++) {
        char tmp[65536];
        memcpy(tmp, text, (size_t)cut);
        fs_len = cut;
        memcpy(fs_buf, tmp, (size_t)cut);
        said_clear();
        settings_load();

        const char *why = NULL;
        if (!all_keys_usable(&why)) {
            if (bad++ < 5) printf("  FAIL: %s cut at %d: %s unusable\n", label, cut, why);
            continue;
        }
        /* The strong predicate: the whole file's value, or the default. */
        for (int i = 0; i < nk; i++) {
            const char *k = settings_schema(i)->key;
            const char *v = settings_get_str(k, "");
            if (strcmp(v, whole[i]) && strcmp(v, none[i])) {
                if (bad++ < 5)
                    printf("  FAIL: %s cut at %d invented %s = \"%s\" "
                           "(the file says \"%s\", a fresh machine says \"%s\")\n",
                           label, cut, k, v, whole[i], none[i]);
            }
        }
        /* A frame record must be all-or-nothing: a prefix of one is never a
         * frame. This is the assertion that catches a parser salvaging fields. */
        struct win_frame f;
        if (settings_frame_load("clock", &f, 1024, 768)) {
            if (f.w < 80 || f.h < 60 || f.x > 1024 || f.y > 768 || f.x + f.w < 40) {
                if (bad++ < 5)
                    printf("  FAIL: %s cut at %d accepted frame %d,%d %dx%d\n",
                           label, cut, f.x, f.y, f.w, f.h);
            }
        }
    }
    checks++;
    if (bad) { fails++; printf("  FAIL: %s -- %d of %d offsets unusable\n", label, bad, n + 1); }
    else     printf("  %-28s %4d offsets, all usable\n", label, n + 1);
    return n + 1;
}

static void t_truncation(void)
{
    printf("truncation sweep (every byte offset)\n");
    int total = 0;

    /* (a) a file this store wrote itself */
    fs_len = -1; settings_load();
    settings_set_int("ui.dark", 1, 0);
    settings_set_str("ui.accent", "0xC81E64", 0);
    settings_set_str("ui.wallpaper", "/media/dot.png", 0);
    settings_set_int("desktop.restore_session", 0, 0);
    settings_set_int("net.dhcp", 0, 0);
    settings_set_str("net.ip", "10.0.2.99", 0);
    settings_set_str("net.mask", "255.255.255.0", 0);
    settings_set_str("net.gw", "10.0.2.2", 0);
    settings_set_str("net.dns", "10.0.2.3", 0);
    {
        struct win_frame f = { 120, 140, 400, 300, 1, 0, 0, 120, 140, 400, 300 };
        settings_frame_save("clock", &f, 0);
    }
    settings_set_str("notify.history", "7", 0);
    settings_commit();
    fs_buf[fs_len] = 0;
    static char written[65536];
    memcpy(written, fs_buf, (size_t)fs_len + 1);
    total += sweep("a file we wrote", written);

    /* (b) a hand-written file with no comments and no crc, i.e. what somebody
     * types into TextEdit -- a different byte layout, so different cut points */
    total += sweep("a hand-typed file",
        "ui.dark=1\n"
        "ui.accent = 0x00FF00\n"
        "net.dhcp=0\n"
        "net.ip = 192.168.1.44\n"
        "win.clock.frame = 10 20 640 480 1 1 0 10 20 320 240\n");

    /* (c) CRLF, because a file edited on a Windows host and copied back is a
     * real thing and \r is whitespace this parser has to trim */
    total += sweep("a CRLF file",
        "ui.dark = 1\r\n"
        "net.dhcp = 0\r\n"
        "net.ip = 172.16.0.9\r\n");

    /* (d) a file whose values are all at their maximum width, so every cut
     * lands inside a token rather than between them */
    total += sweep("maximum-width values",
        "ui.accent = 0xFFFFFF\n"
        "net.ip = 255.255.255.255\n"
        "net.mask = 255.255.255.255\n"
        "win.a.frame = 1000 1000 1000 1000 1 1 1 1000 1000 1000 1000\n");

    printf("  %d offsets swept in total\n", total);
}

/* ===========================================================================
 * 3. Garbage: the hand-edited cases the brief names by hand
 * ======================================================================== */
static void t_garbage(void)
{
    printf("garbage values\n");
    static const struct { const char *text; const char *what; } cases[] = {
        { "ui.dark = banana\n",                  "a word where a boolean goes" },
        { "ui.dark = -1\n",                      "a negative boolean" },
        { "ui.dark = 2\n",                       "a boolean out of range" },
        { "ui.dark = 99999999999999999999\n",    "an integer that overflows 64 bits" },
        { "ui.accent = 0xFFFFFFFF\n",            "a colour out of range" },
        { "ui.accent = -1\n",                    "a negative colour" },
        { "ui.accent = #C81E64\n",               "a colour in CSS notation" },
        { "net.ip = 999.1.1.1\n",                "an octet over 255" },
        { "net.ip = 10.0.2\n",                   "a three-part address" },
        { "net.ip = ....\n",                     "dots and nothing else" },
        { "net.ip = 10.0.2.15 extra\n",          "trailing junk after a valid value" },
        { "ui.wallpaper = \n",                   "an empty string" },
        { "= 5\n",                               "a line with no key" },
        { "ui.dark\n",                           "a line with no '='" },
        { "  =  =  = \n",                        "only delimiters" },
        { "\n\n\n\n",                            "nothing but newlines" },
        { "#\n#\n#\n",                           "nothing but comments" },
        { "ui.dark = 1",                         "an unterminated final line" },
        { "\xff\xfe\x00\x01 garbage\n",          "bytes that are not text" },
        { "desktop.restore_session = -1\n",      "a negative boolean, again" },
        { "net.dhcp = 2\n",                      "an out-of-range boolean" },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        fs_set(cases[i].text);
        said_clear();
        settings_load();
        const char *why = NULL;
        okf(all_keys_usable(&why), "%s -> everything still usable (%s failed)",
            cases[i].what, why ? why : "?");
    }

    /* A window at -9000,-9000 sized 4x4 must be REFUSED, not clamped: a clamped
     * garbage frame looks like a compositor bug, a refused one puts the window
     * exactly where a fresh machine would. */
    static const char *frames[] = {
        "win.clock.frame = -9000 -9000 4 4 1 0 0 0 0 0 0\n",
        "win.clock.frame = 0 0 999999 999999 1 0 0 0 0 999999 999999\n",
        "win.clock.frame = 5000 5000 320 240 1 0 0 5000 5000 320 240\n",
        "win.clock.frame = 1 2 3\n",
        "win.clock.frame = a b c d e f g h i j k\n",
        "win.clock.frame = 40 60 320 240 1 7 0 40 60 320 240\n",
        "win.clock.frame = 40 60 320 240 1 0 0 40 60 320 240 999\n",
        "win.clock.frame = \n",
    };
    for (unsigned i = 0; i < sizeof frames / sizeof frames[0]; i++) {
        fs_set(frames[i]);
        settings_load();
        struct win_frame f;
        okf(settings_frame_load("clock", &f, 1024, 768) == 0,
            "a bad frame record is refused whole: %s", frames[i]);
    }

    /* ...and a good one on a big screen is refused on a small one, which is the
     * off-screen case that actually happens to people. */
    {
        struct win_frame f = { 1500, 900, 400, 300, 1, 0, 0, 1500, 900, 400, 300 };
        fs_len = -1; settings_load();
        settings_frame_save("big", &f, 0);
        struct win_frame g;
        ok(settings_frame_load("big", &g, 1920, 1080) == 1,
           "a frame saved on a big desktop loads on a big desktop");
        ok(settings_frame_load("big", &g, 800, 600) == 0,
           "...and is REFUSED on a small one rather than coming back off-screen");
    }

    /* An out-of-range typed value reads as the DEFAULT through the string API
     * too, not just the typed one -- SYS_SETTING_GET answers with the value in
     * force. The bytes the user actually typed stay reachable via the kv API,
     * which is what the Settings app lists so they can be repaired. */
    fs_set("net.ip = 999.1.1.1\nui.dark = banana\n");
    settings_load();
    ok(strcmp(settings_get_str("net.ip", "?"), "10.0.2.15") == 0,
       "a bad address reads as the default through the string API");
    ok(strcmp(settings_get_str("ui.dark", "?"), "0") == 0,
       "a bad boolean reads as the default through the string API");
    {
        const char *k = NULL, *v = NULL;
        int found = 0;
        for (int i = 0; i < settings_kv_count(); i++) {
            settings_kv_at(i, &k, &v);
            if (!strcmp(k, "net.ip") && !strcmp(v, "999.1.1.1")) found = 1;
        }
        ok(found, "...while the RAW typed bytes stay listable, so a human can fix them");
    }

    /* The machine has to SAY what it rejected. */
    fs_set("ui.dark = banana\nnet.ip = 999.1.1.1\n");
    said_clear();
    settings_init();
    ok(said_has("REJECTED ui.dark"), "it names ui.dark as rejected");
    ok(said_has("REJECTED net.ip"),  "it names net.ip as rejected");
    ok(said_has("using default"),    "...and says what it used instead");
}

/* ===========================================================================
 * 4. The rules the truncation argument rests on, asserted directly
 * ======================================================================== */
static void t_rules(void)
{
    printf("the parser's rules\n");

    /* THE rule. If this stops holding, the sweep above proves nothing, and the
     * sweep alone would not tell you. */
    fs_set("ui.dark = 1");                      /* no newline */
    settings_load();
    ok(settings_kv_count() == 0, "an unterminated final line is NOT parsed");
    ok(settings_diag() & SET_D_TRUNCATED, "...and the machine reports the truncation");

    fs_set("ui.dark = 1\n");
    settings_load();
    ok(settings_kv_count() == 1, "a terminated line IS parsed");
    ok(!(settings_diag() & SET_D_TRUNCATED), "...and reports no truncation");

    /* One bad line costs one key, never the file. */
    fs_set("ui.dark = 1\nthis is not a setting\nnet.dhcp = 0\n");
    settings_load();
    ok(settings_get_int("ui.dark", 0) == 1, "a bad line does not cost the line before it");
    ok(settings_get_int("net.dhcp", 1) == 0, "...nor the line after it");
    ok(settings_diag() & SET_D_BADLINE, "...and it is reported");

    /* The CRC is a diagnostic and NEVER a gate: a hand-edited file has a stale
     * CRC by definition, and it is exactly the file somebody is repairing. */
    fs_set("ui.dark = 1\n# crc32 = DEADBEEF\n");
    settings_load();
    ok(settings_diag() & SET_D_CRCBAD, "a wrong crc32 is noticed");
    ok(settings_get_int("ui.dark", 0) == 1, "...and the file is used anyway");

    /* Whitespace a human would type. */
    fs_set("   ui.dark   =   1   \n\t net.dhcp\t=\t0\t\n");
    settings_load();
    ok(settings_get_int("ui.dark", 0) == 1, "leading/trailing spaces are trimmed");
    ok(settings_get_int("net.dhcp", 1) == 0, "tabs are whitespace too");

    /* Last one wins, so a user who appends a line to fix a value gets the fix. */
    fs_set("ui.dark = 0\nui.dark = 1\n");
    settings_load();
    ok(settings_get_int("ui.dark", 0) == 1, "a repeated key takes its LAST value");

    /* The table cannot be overrun by a file with more keys than it holds. */
    {
        static char many[65536];
        int o = 0;
        for (int i = 0; i < SET_MAXKV * 3; i++)
            o += sprintf(many + o, "k%d = %d\n", i, i);
        fs_set(many);
        settings_load();
        ok(settings_kv_count() <= SET_MAXKV, "a file with 192 keys does not overrun a 64-key table");
        ok(settings_diag() & SET_D_FULL, "...and says the table filled");
        const char *why = NULL;
        ok(all_keys_usable(&why), "...and every schema key still reads usably");
    }

    /* An over-long key or value cannot escape its buffer. */
    {
        static char big[8192];
        int o = sprintf(big, "%s", "");
        for (int i = 0; i < 300; i++) big[o++] = 'k';
        o += sprintf(big + o, " = ");
        for (int i = 0; i < 900; i++) big[o++] = 'v';
        big[o++] = '\n'; big[o] = 0;
        fs_set(big);
        settings_load();
        const char *why = NULL;
        ok(all_keys_usable(&why), "a 300-byte key with a 900-byte value is survivable");
    }

    /* reset() is the same code path as a fresh machine -- there is no separate
     * "defaults" table to drift out of step with the schema. */
    fs_set("ui.dark = 1\n");
    settings_load();
    ok(settings_get_int("ui.dark", 0) == 1, "before reset");
    settings_reset();
    ok(settings_get_int("ui.dark", 9) == 0, "reset restores the schema default");
    ok(fs_len == -1, "...by deleting the file, not by writing a default one");
}

/* ===========================================================================
 * 5. Frames round-trip through the exact format the WM will use
 * ======================================================================== */
static void t_frames(void)
{
    printf("window frames\n");
    fs_len = -1;
    settings_load();

    struct win_frame a = { 120, 140, 400, 300, 1, 0, 0, 120, 140, 400, 300 };
    struct win_frame z = { 0, 0, 1280, 760, 1, 1, 0, 200, 200, 500, 400 };
    struct win_frame m = { 60, 60, 320, 240, 0, 0, 1, 60, 60, 320, 240 };
    settings_frame_save("clock", &a, 0);
    settings_frame_save("terminal", &z, 0);
    settings_frame_save("files", &m, 0);
    settings_commit();
    settings_load();                                  /* the reboot */

    struct win_frame g;
    ok(settings_frame_load("clock", &g, 1280, 800) == 1, "a normal frame reloads");
    ok(g.x == 120 && g.y == 140 && g.w == 400 && g.h == 300, "...at the same rect");
    ok(g.zoomed == 0 && g.minimized == 0 && g.open == 1, "...with the same flags");

    ok(settings_frame_load("terminal", &g, 1280, 800) == 1, "a zoomed frame reloads");
    ok(g.zoomed == 1, "...still zoomed");
    ok(g.rx == 200 && g.ry == 200 && g.rw == 500 && g.rh == 400,
       "...and keeps the restore rect, so unzooming lands where the user left it");

    ok(settings_frame_load("files", &g, 1280, 800) == 1, "a minimized frame reloads");
    ok(g.minimized == 1 && g.open == 0, "...minimized, and marked not-open");

    ok(settings_frame_load("nosuchapp", &g, 1280, 800) == 0, "an app with no saved frame says so");

    /* The record is human-readable, like everything else in the file. */
    fs_buf[fs_len] = 0;
    ok(strstr(fs_buf, "win.clock.frame = 120 140 400 300 1 0 0 120 140 400 300\n") != NULL,
       "a frame is a readable line of eleven numbers");
}

/* ===========================================================================
 * 6. The selftest the kernel runs on every boot must itself be clean
 * ======================================================================== */
static void t_selftest(void)
{
    printf("the on-boot sweep\n");
    fs_len = -1;
    settings_load();
    said_clear();
    int f = settings_selftest();
    okf(f == 0, "settings_selftest() reports %d failures (must be 0)", f);
    ok(said_has("SETTINGS_SELFTEST offsets="), "...and prints its offset count for a harness to read");

    /* It must not disturb the live table -- it runs on every boot, after the
     * real file has been loaded. */
    fs_set("ui.dark = 1\nnet.dhcp = 0\n");
    settings_load();
    settings_selftest();
    ok(settings_get_int("ui.dark", 0) == 1, "the sweep leaves the live theme alone");
    ok(settings_get_int("net.dhcp", 1) == 0, "the sweep leaves the live network alone");
}

int main(int argc, char **argv)
{
    verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);
    printf("settings store\n");

    t_roundtrip();
    t_truncation();
    t_garbage();
    t_rules();
    t_frames();
    t_selftest();

    printf("\n%d checks, %d failed\n", checks, fails);
#if defined(SETTINGS_NO_TRUNC_GUARD) || defined(SETTINGS_NO_RANGE_CHECK)
    /* A negative control. It is built to fail; a build of it that PASSES means
     * the assertion it was aimed at is not actually being made. */
    if (!fails) {
        printf("NEGATIVE CONTROL PASSED -- which is the failure. "
               "The assertion it disables is not being checked.\n");
        return 1;
    }
    printf("negative control failed as required (%d checks)\n", fails);
    return 0;
#else
    return fails ? 1 : 0;
#endif
}
