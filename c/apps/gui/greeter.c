#include "aui.h"
#include "accounts.h"

/* greeter.aex -- the thing that stands between power-on and this desktop.
 *
 * =========================================================================
 * WHAT THE MACHINE LOOKED LIKE THIS MORNING
 * =========================================================================
 * /bin/login authenticated THE SERIAL CONSOLE. The desktop did not: wm_run()
 * launched files.aex before init ran at all, so 2.7 seconds after power-on the
 * previous user's home directory was on screen and nothing had asked anybody
 * anything. The console login was real and the desktop went around it.
 *
 * =========================================================================
 * A GREETER, NOT A LOCK SCREEN -- and the difference is not cosmetic
 * =========================================================================
 * Two shapes were available and both are defensible:
 *
 *   (a) THE DESKTOP STARTS LOCKED. Everything launches as it does today and an
 *       overlay covers it until a password arrives.
 *   (b) A GREETER OWNS THE SCREEN and the desktop is not started at all until
 *       somebody authenticates.
 *
 * (b), for one reason that outranks the rest: in (a) the user's file manager,
 * their open windows and their wallpaper are ALREADY RUNNING behind the
 * overlay, so the security of the machine is the security of the overlay's
 * z-order. Every bug that has ever put a window above another window becomes a
 * bug that shows a stranger somebody's files. In (b) there is nothing behind
 * the greeter to reveal, because nothing has been launched -- and the gate is
 * not "do not draw it", it is `wm_launch()` REFUSING to start anything but
 * this program while the machine is locked (c/kernel/gui/wm.c, g_locked). A
 * refusal in the launcher cannot be lost to a compositing mistake.
 *
 * The cost of (b) is honest and worth stating: this is a login, not a screen
 * unlock. There is nothing to come back TO, so there is no session to restore
 * and no "switch user". When those arrive they need (a)'s machinery as well as
 * this, and the lock flag this sets is where it would hang.
 *
 * =========================================================================
 * ONE DEFINITION OF "IS THIS PASSWORD CORRECT"
 * =========================================================================
 * This program does not implement authentication. It calls
 * acct_check_password() out of c/apps/coreutils/accounts.h -- the same inline
 * the console login calls, over the same /etc/passwd, through the same
 * PBKDF2. That is deliberate and it is what makes the negative control
 * possible: `make LOGIN_NEGCTL=1` compiles -DLOGIN_NEGCTL_ACCEPT_ANY into BOTH
 * programs at once, and then the greeter appears, the field masks, the timing
 * line prints, the desktop comes up -- and every password is correct. A suite
 * that cannot go red on that build is testing that a box was drawn.
 *
 * =========================================================================
 * WHAT IT REFUSES
 * =========================================================================
 *   - a wrong password: GREETER-DENIED, the field is cleared, nothing starts.
 *   - an unknown user: the same message after the same work (see the decoy
 *     below), so the prompt does not answer "does this account exist" faster
 *     than it answers "is this the password".
 *   - an empty password: refused without touching the store at all.
 *   - closing it: there is no close button. The window manager draws no chrome
 *     while locked and routes every keystroke and every click here; the dock
 *     and the menu bar are not drawn and not hit-tested.
 *
 * NO CREDENTIAL IS EVER GENERATED HERE. A machine with no /etc/passwd never
 * sees this program -- wm_run() does not launch it, exactly as before, because
 * a machine with nobody to authenticate has nothing to ask.
 */

/* accounts.h is shared with a program that links no libc; these come along for
 * the same reason they do in login.c (clang emits them for struct assignment
 * regardless, and pbkdf2.c calls them outright). aui/gfx do not provide them. */
void *memcpy(void *d, const void *s, unsigned long n)
{
    unsigned char *a = (unsigned char *)d; const unsigned char *b = (const unsigned char *)s;
    for (unsigned long i = 0; i < n; i++) a[i] = b[i];
    return d;
}
void *memset(void *d, int c, unsigned long n)
{
    unsigned char *a = (unsigned char *)d;
    for (unsigned long i = 0; i < n; i++) a[i] = (unsigned char)c;
    return d;
}

int pwhash_check(const char *record, const char *password);

#define PWMAX 128

static char  store[ACCT_MAX + 1];
static int   store_len;

/* ------------------------------------------------------------------ serial --
 * The greeter has fd 1 = the tty (wm_launch gives every app real stdio), and
 * these lines are how a harness with no display asserts on it. They are also
 * why the boot log can prove the ORDER: "GREETER-OK" strictly before
 * "[wm] launched files.aex" is the whole claim of this file. */
static int slen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static void say(const char *s) { sys_write(1, s, slen(s)); }
static void say_u(unsigned long v)
{
    char t[24]; int k = 0;
    if (!v) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    char o[24]; int n = 0;
    while (k) o[n++] = t[--k];
    sys_write(1, o, n);
}

/* ------------------------------------------------------------------- state --*/
enum { F_NAME = 0, F_PW = 1 };

static char name[ACCT_NAME];
static char pw[PWMAX];
static int  nname, npw;
static int  field = F_NAME;
static int  checking;               /* a verify is queued for the next frame */
static int  denied;                 /* show the refusal                      */
static unsigned long last_ms;       /* what the verify cost                  */

static void clear_pw(void) { for (int i = 0; i < PWMAX; i++) pw[i] = 0; npw = 0; }

/* --------------------------------------------------------------- the paint --*/
static void paint(int W, int H)
{
    aui_begin(AUI_BG);

    int cw = 380, ch = 250;
    if (cw > W - 40) cw = W - 40;
    int cx = (W - cw) / 2, cy = (H - ch) / 2 - 20;
    if (cy < 20) cy = 20;

    aui_card(cx, cy, cw, ch, AUI_ELEV_3);

    const char *t = "LogitOS";
    int tw = text_measure_px(t, slen(t), AUI_FS_HEADING, 0);
    gui_text_run(cx + (cw - tw) / 2, cy + 26, AUI_FS_HEADING, 0, AUI_TEXT, t, slen(t));

    const char *sub = checking ? "Checking..."
                    : denied   ? "Incorrect user name or password"
                               : "Sign in to continue";
    int sw = text_measure_px(sub, slen(sub), AUI_FS_LABEL, 0);
    gui_text_run(cx + (cw - sw) / 2, cy + 62, AUI_FS_LABEL, 0,
                 denied ? AUI_ERROR : AUI_MUTED, sub, slen(sub));

    int fx = cx + 28, fw = cw - 56, fh = AUI_H_LG;

    /* name */
    aui_label(fx, cy + 96, "User", AUI_MUTED);
    gui_rrect(fx, cy + 114, fw, fh, AUI_R_MD,
              field == F_NAME ? AUI_FOCUS : AUI_BORDER);
    gui_rrect(fx + 2, cy + 116, fw - 4, fh - 4, AUI_R_MD, AUI_SURFACE);
    if (nname) gui_text_run(fx + 12, cy + 122, AUI_FS_BODY, 0, AUI_TEXT, name, nname);

    /* password -- MASKED IN THE FIELD, and unlike the serial console there is
     * no echo to race: nothing ever draws the character. */
    aui_label(fx, cy + 160, "Password", AUI_MUTED);
    gui_rrect(fx, cy + 178, fw, fh, AUI_R_MD,
              field == F_PW ? AUI_FOCUS : AUI_BORDER);
    gui_rrect(fx + 2, cy + 180, fw - 4, fh - 4, AUI_R_MD, AUI_SURFACE);
    {
        char dots[PWMAX + 1];
        int n = npw > PWMAX ? PWMAX : npw;
        for (int i = 0; i < n; i++) dots[i] = '*';
        dots[n] = 0;
        if (n) gui_text_run(fx + 12, cy + 186, AUI_FS_BODY, 0, AUI_TEXT, dots, n);
    }

    const char *hint = "Tab switches fields  -  Enter signs in";
    int hw = text_measure_px(hint, slen(hint), AUI_FS_CAPTION, 0);
    gui_text_run(cx + (cw - hw) / 2, cy + ch - 26, AUI_FS_CAPTION, 0, AUI_MUTED,
                 hint, slen(hint));

    aui_end();
}

/* ------------------------------------------------------------- the attempt --*/
static void attempt(void)
{
    /* An empty password never reaches the store. There is no account it could
     * match -- enrolment refuses one -- so verifying it would only teach the
     * prompt's timing to a bystander. */
    if (!nname || !npw) { denied = 1; clear_pw(); return; }

    unsigned long long t0 = monotonic_ms();
    struct account a;
    int found = acct_find(store, store_len, name, &a);
    int ok;
    if (found) ok = acct_check_password(&a, pw);
    else {
        /* The same decoy /bin/login uses, for the same reason and with the
         * same argument: a FABRICATED record could carry a different iteration
         * count, or fail to parse and cost nothing, and either hands an
         * observer "that user does not exist" in the timing. The first real
         * row costs exactly what a real check costs, because it is one. */
        struct account decoy; int pos = 0;
        if (acct_next(store, store_len, &pos, &decoy)) acct_check_password(&decoy, pw);
        ok = 0;
    }
    last_ms = (unsigned long)(monotonic_ms() - t0);
    clear_pw();

    if (!ok) {
        denied = 1;
        say("GREETER-DENIED user="); say(name);
        say(" ms="); say_u(last_ms); say("\n");
        return;
    }

    /* THE MOMENT ROOT IS GIVEN UP. One call, and the kernel does the rest of
     * it: SYS_SETSESSION establishes the login session AND this process's own
     * credential (c/fs/vfs_cred.c), and c/kernel/exec/syscall.c uses that same
     * instant -- the last one at which the caller can read /etc/passwd -- to
     * point the settings store at this user's $HOME/.config/settings.conf.
     * The window manager notices the session and starts the desktop. */
    if (sys_setsession(a.uid, a.gid) < 0) {
        denied = 1;
        say("GREETER-DENIED could not establish the session\n");
        return;
    }
    say("GREETER-OK "); say(a.name);
    say(" uid="); say_u((unsigned long)sys_getuid());
    say(" gid="); say_u((unsigned long)sys_getgid());
    say(" ms="); say_u(last_ms); say("\n");
    app_exit(0);
}

void app_main(void)
{
    int W = screen_w(), H = screen_h();
    gui_create("Sign in", W, H);

    /* Read the store BEFORE anything else: this process is root (nothing has
     * authenticated, so it inherits the boot session -- see vfs_cred.h) and
     * that is the only reason a 0600 file is readable here at all. */
    int n = read_file("/etc/passwd", store, ACCT_MAX);
    if (n > 0) { store_len = n; store[n] = 0; }

    if (store_len <= 0) {
        /* wm_run() only launches this program when the store exists, so
         * arriving here means it became unreadable between that check and this
         * read. Never fall through to "let them in": say so and sit there. The
         * console login makes the same choice on the same state. */
        say("GREETER-NOSTORE /etc/passwd is unreadable -- refusing\n");
    }

    /* One account is the common case, so pre-fill it and put the cursor in the
     * password field. Two or more and the user says who they are. */
    {
        struct account a; int pos = 0;
        if (acct_next(store, store_len, &pos, &a)) {
            struct account b;
            if (!acct_next(store, store_len, &pos, &b)) {
                acct_cpyn(name, a.name, acct_len(a.name), ACCT_NAME);
                nname = acct_len(name);
                field = F_PW;
            }
        }
    }

    say("GREETER-READY accounts=");
    { struct account a; int pos = 0, c = 0; while (acct_next(store, store_len, &pos, &a)) c++; say_u((unsigned long)c); }
    say("\n");

    int repaint = 1;
    for (;;) {
        struct logit_event e;
        while (poll_event(&e)) {
            /* EV_CLOSE is not honoured. There is no close button drawn while
             * the machine is locked and the WM does not hit-test one, so this
             * can only arrive from a shortcut -- and a greeter a keystroke can
             * dismiss is not a greeter. */
            if (e.type == EV_CLOSE) continue;
            if (e.type == EV_THEME) { aui_set_dark(sys_ui_dark(-1)); repaint = 1; continue; }
            if (e.type != EV_KEY) continue;

            int c = e.a;
            denied = 0;
            if (c == '\t')      field = field == F_NAME ? F_PW : F_NAME;
            else if (c == '\n' || c == '\r') {
                if (field == F_NAME && !npw) field = F_PW;
                else checking = 1;              /* paint "Checking..." first */
            } else if (c == '\b' || c == 127) {
                if (field == F_NAME) { if (nname) name[--nname] = 0; }
                else                 { if (npw)   pw[--npw] = 0; }
            } else if (c >= 32 && c < 127) {
                if (field == F_NAME) { if (nname < ACCT_NAME - 1) { name[nname++] = (char)c; name[nname] = 0; } }
                else                 { if (npw   < PWMAX - 1)     { pw[npw++]     = (char)c; pw[npw]     = 0; } }
            }
            repaint = 1;
        }

        if (repaint) { repaint = 0; paint(W, H); }

        /* THE VERIFY RUNS AFTER A FRAME HAS BEEN PRESENTED, never inside the
         * keystroke that asked for it. PBKDF2 at 120000 iterations is about a
         * second under TCG, and a second of a frozen window with the old text
         * still on it is indistinguishable from a machine that has died. This
         * costs one extra frame and buys the difference. */
        if (checking) {
            checking = 0;
            attempt();
            repaint = 1;
        }
        sys_yield();
    }
}
