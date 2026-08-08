#include "aui.h"

/* ============================================================================
 * Activity Monitor -- a task manager, not a text dump.
 *
 * THE BRIEF, and how the two halves are divided:
 *   - macOS contributes the SHAPE: peer views per resource, one process table
 *     re-sorted by whichever view you are in, a quiet toolbar.
 *   - Windows contributes the POINT: you can end a process from wherever you
 *     are looking. The kill control is in the footer of EVERY tab, not parked
 *     in one place you have to navigate to first, because the whole reason to
 *     open a task manager is to act.
 *   - macOS's abstract memory display is explicitly NOT copied. There is no
 *     "memory pressure" graph, no "compressed", no app/wired/cached taxonomy.
 *     The Memory tab shows bytes, and shows the same quantity twice -- rounded
 *     for reading and exact for checking -- so a number here can be checked
 *     against a number somewhere else.
 *
 * WHY THERE IS NO CPU TAB, AND NO PER-PROCESS MEMORY COLUMN.
 * This is the part worth reading. A task manager's columns are measurements,
 * and this kernel does not currently take those measurements:
 *
 *   CPU per process   the scheduler counts DISPATCHES per thread, not time,
 *                     and `struct thread` is opaque with no lookup by id.
 *   Memory per process nothing sums frames per address space; the one per-cr3
 *                     figure that exists counts mmap reservations, and no GUI
 *                     app calls mmap, so it reads 0 for all of them.
 *   Disk / network     not accounted per process at all.
 *
 * Each gap closes in a file this change does not own (sched.c, c/kernel/mm/,
 * sock.c). So rather than fill four tabs with three fabrications, the tabs are
 * the ones the machine can actually answer, and the Memory tab says in the UI
 * itself which number is missing and why. A column that shows a plausible
 * figure nobody measured is worse than an absent column: it is the one thing a
 * user can catch the program lying about.
 *
 * Every number on screen comes from SYS_PROCS, SYS_MEMINFO or SYS_SYSINFO.
 * None is interpolated, smoothed or estimated.
 * ==========================================================================*/

#define WINW 660
#define WINH 470

#define MAXPROC 32          /* NPROC in c/kernel/exec/proc.h */
#define NCOL    6

/* ---- syscall wrappers ----------------------------------------------------
 * Defined here rather than added to c/apps/logit.h: that header is shared by
 * every app and this change owns only the Activity Monitor. */
static inline int sys_procs(struct logit_procinfo *b, int max)
{ return (int)_sys(SYS_PROCS, (long)b, max, 0); }
static inline int sys_kill(int pid)
{ return (int)_sys(SYS_KILL, pid, 0, 0); }
/* sys_meminfo() already exists in logit.h -- used as-is. */

/* ---- small formatting helpers (GUI apps have no libc) ------------------- */

static int ustr(char *d, unsigned long long v)      /* -> length written */
{
    char t[24]; int i = 0, n = 0;
    if (!v) { d[0] = '0'; d[1] = 0; return 1; }
    while (v) { t[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (i) d[n++] = t[--i];
    d[n] = 0;
    return n;
}

static int scat(char *d, int at, const char *s)
{ while (*s) d[at++] = *s++; d[at] = 0; return at; }

/* 1 234 567 -> "1,234,567". The exact byte count is meant to be READ and
 * compared against another tool's number, and an ungrouped 9-digit run cannot
 * be. */
static void ugroup(char *d, unsigned long long v)
{
    char raw[24]; int n = ustr(raw, v), o = 0;
    for (int i = 0; i < n; i++) {
        if (i && (n - i) % 3 == 0) d[o++] = ',';
        d[o++] = raw[i];
    }
    d[o] = 0;
}

/* Bytes -> "229.4 MiB", one decimal, integer arithmetic only. Binary units
 * spelled MiB rather than MB because that is what the number actually is: the
 * kernel counts 4096-byte frames. */
static void ubytes(char *d, unsigned long long bytes)
{
    static const char *unit[] = { "B", "KiB", "MiB", "GiB" };
    int u = 0;
    unsigned long long whole = bytes, frac = 0;
    while (whole >= 1024 && u < 3) {
        frac  = ((whole % 1024) * 10) / 1024;
        whole /= 1024;
        u++;
    }
    int n = ustr(d, whole);
    if (u) { d[n++] = '.'; d[n++] = (char)('0' + (int)frac); d[n] = 0; }
    n = scat(d, n, " ");
    scat(d, n, unit[u]);
}

/* Case-insensitive compare, for sorting by name. */
static int sless(const char *a, const char *b)
{
    for (;;) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca < cb;
        if (!ca) return 0;
        a++; b++;
    }
}

/* ---- state -------------------------------------------------------------- */

enum { T_PROC = 0, T_MEM, T_SYS, NTAB };
static const char *const tabs[NTAB] = { "Processes", "Memory", "System" };

enum { S_PID = 0, S_NAME, S_FILES, NSORT };
static const char *const sorts[NSORT] = { "PID", "Name", "Files" };

static struct logit_procinfo pr[MAXPROC];
static int npr;
static int order[MAXPROC];              /* sorted view -> pr[] index */

static int tab, sortkey, sortdesc, selpid = -1, tsel, tscroll;
static int confirm_open, self_pid;
static int last_result;                 /* LOGIT_KILL_* of the last attempt */
static char last_msg[96];
static unsigned long long last_result_ms;

static struct logit_meminfo mi;
static char sysbuf[1024];

/* Repaint cost, measured rather than assumed -- the brief asks what a live
 * table costs, and aui_begin() clears the whole window because SYS_GUI_FLUSH
 * carries no dirty rectangle, so the answer is a real question. */
static unsigned long long paint_us, paint_us_max;
static unsigned paints;
static void report_cost(void);          /* defined below, next to its counters */
static void report_xcheck(void);        /* ...and the cross-checks beside it */
static unsigned long long rtc_secs(void);
static unsigned long long t0_mono, t0_rtc;   /* baselines for the uptime check */

/* Cell text storage. aui_table takes an array of pointers that must stay valid
 * for the call, so the strings live in a static block rebuilt each refresh. */
static char  cellbuf[MAXPROC][NCOL][28];
static const char *cells[MAXPROC * NCOL];
static const char *cols[NCOL]  = { "PID", "Process", "Kind", "State", "Parent", "Files" };
static const int   colw[NCOL]  = { 54, 190, 74, 84, 68, 60 };

/* ---- data --------------------------------------------------------------- */

static void resort(void)
{
    for (int i = 0; i < npr; i++) order[i] = i;
    /* Insertion sort: npr <= 32, and a stable simple sort keeps equal keys in
     * pid order, which stops rows swapping places between refreshes. */
    for (int i = 1; i < npr; i++) {
        int v = order[i], j = i - 1;
        while (j >= 0) {
            const struct logit_procinfo *a = &pr[order[j]], *b = &pr[v];
            int gt;
            if (sortkey == S_NAME)       gt = sless(b->name, a->name);
            else if (sortkey == S_FILES) gt = b->nfds < a->nfds;
            else                         gt = b->pid < a->pid;
            if (sortdesc) {
                if (sortkey == S_NAME)       gt = sless(a->name, b->name);
                else if (sortkey == S_FILES) gt = b->nfds > a->nfds;
                else                         gt = b->pid > a->pid;
            }
            if (!gt) break;
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = v;
    }
}

static void build_cells(void)
{
    for (int r = 0; r < npr; r++) {
        const struct logit_procinfo *p = &pr[order[r]];
        char *c;
        c = cellbuf[r][0]; ustr(c, (unsigned long long)p->pid);
        c = cellbuf[r][1];
        { int n = scat(c, 0, p->name[0] ? p->name : "(unnamed)");
          if (p->flags & LOGIT_PROC_SELF) scat(c, n, " *"); }
        c = cellbuf[r][2]; scat(c, 0, (p->flags & LOGIT_PROC_GUI) ? "App" : "CLI");
        c = cellbuf[r][3];
        if (p->flags & LOGIT_PROC_DYING)          scat(c, 0, "Quitting");
        else if (p->state == LOGIT_PROC_ZOMBIE)   scat(c, 0, "Zombie");
        else                                      scat(c, 0, "Running");
        c = cellbuf[r][4]; ustr(c, (unsigned long long)p->ppid);
        c = cellbuf[r][5]; ustr(c, (unsigned long long)p->nfds);
        for (int k = 0; k < NCOL; k++) cells[r * NCOL + k] = cellbuf[r][k];
    }
}

static void refresh(void)
{
    int n = sys_procs(pr, MAXPROC);
    npr = n < 0 ? 0 : n;
    sys_meminfo(&mi);
    sysinfo(sysbuf, sizeof sysbuf);
    resort();
    build_cells();
    /* Selection follows the PID, not the row: sorting and process exit both
     * renumber rows, and a kill button that acts on "row 3" is a kill button
     * that eventually kills the wrong thing. */
    tsel = -1;
    for (int r = 0; r < npr; r++)
        if (pr[order[r]].pid == selpid) { tsel = r; break; }
    if (tsel < 0) selpid = -1;
}

static const struct logit_procinfo *selected(void)
{
    for (int i = 0; i < npr; i++) if (pr[i].pid == selpid) return &pr[i];
    return 0;
}

/* Would the kernel accept a kill for the current selection? One predicate, used
 * by the button, the tooltip and the Delete key, so the three cannot disagree
 * about what is killable.
 *
 * NEGATIVE CONTROL (-DMONITOR_NEGCTL): ignore the kernel's protection flag, so
 * the button lights up for the console shell too. Built that way,
 * tests/qmp/qmp_monitor.py's refusal assertion MUST fail -- which is how "the
 * greyed-out button is driven by the kernel" is demonstrated instead of
 * asserted. See the Makefile's test-monitor-negctl. */
static int killable(void)
{
    const struct logit_procinfo *s = selected();
#ifdef MONITOR_NEGCTL
    return s != 0;
#else
    return s && !(s->flags & LOGIT_PROC_PROTECTED)
             && s->state != LOGIT_PROC_ZOMBIE
             && !(s->flags & LOGIT_PROC_DYING);
#endif
}

/* ---- the kill path ------------------------------------------------------ */

static void do_kill(int pid)
{
#ifdef MONITOR_NEGCTL
    /* ...and aim the kill at pid 0, which does not exist. The kernel refuses,
     * nothing dies, and the three assertions that watch a process leave the
     * table must fail. */
    pid = 0;
#endif
    int rc = sys_kill(pid);
    last_result = rc;
    last_result_ms = monotonic_ms();
    int n;
    if (rc == LOGIT_KILL_OK) {
        n = scat(last_msg, 0, "Force quit sent to pid ");
        n += ustr(last_msg + n, (unsigned long long)pid);
        scat(last_msg, n, " -- it exits at its next system call");
    } else if (rc == LOGIT_KILL_PROTECTED) {
        n = scat(last_msg, 0, "Refused: pid ");
        n += ustr(last_msg + n, (unsigned long long)pid);
        scat(last_msg, n, " is protected (the console shell / the compositor)");
    } else if (rc == LOGIT_KILL_ZOMBIE) {
        scat(last_msg, 0, "Already exited; waiting to be reaped");
    } else {
        n = scat(last_msg, 0, "No process with pid ");
        ustr(last_msg + n, (unsigned long long)pid);
    }
    refresh();
}

/* ---- views -------------------------------------------------------------- */

static void view_processes(int x, int y, int w)
{
    aui_text_sz(x, y, "Sort by", AUI_MUTED, AUI_FS_LABEL);
    int seg_w = 210;
    int before = sortkey;
    aui_segmented(x + 56, y - 6, seg_w, AUI_H_SM, sorts, NSORT, &sortkey);
    int d = sortdesc;
    aui_checkbox(x + 56 + seg_w + AUI_SP(4), y - 4, "Descending", &sortdesc);
    if (before != sortkey || d != sortdesc) { resort(); build_cells();
        tsel = -1;
        for (int r = 0; r < npr; r++) if (pr[order[r]].pid == selpid) { tsel = r; break; } }

    int act = aui_table(x, y + 26, w, 236, cols, colw, NCOL, cells, npr, &tsel, &tscroll);
    (void)act;
    if (tsel >= 0 && tsel < npr) selpid = pr[order[tsel]].pid;
}

static void kv(int x, int *y, const char *k, const char *v, unsigned vc)
{
    aui_text_sz(x, *y, k, AUI_MUTED, AUI_FS_LABEL);
    aui_text_sz(x + 168, *y, v, vc, AUI_FS_LABEL);
    *y += 22;
}

static void view_memory(int x, int y, int w)
{
    unsigned long long fb = mi.frame_bytes ? mi.frame_bytes : 4096;
    unsigned long long total = mi.frames_total * fb;
    unsigned long long used  = mi.frames_used  * fb;
    char a[40], b[40], line[96];

    aui_card(x, y, w, 118, AUI_ELEV_1);
    int cy = y + 14;
    aui_text_sz(x + 16, cy, "Physical memory", AUI_TEXT, AUI_FS_TITLE);
    cy += 30;

    /* The headline pair: the same quantity rounded and exact. */
    ubytes(a, used); ugroup(b, used);
    int n = scat(line, 0, a); n = scat(line, n, "  (");
    n = scat(line, n, b); scat(line, n, " bytes) in use");
    aui_text_sz(x + 16, cy, line, AUI_TEXT, AUI_FS_BODY);
    cy += 24;
    ubytes(a, total); ugroup(b, total);
    n = scat(line, 0, "of "); n = scat(line, n, a); n = scat(line, n, "  (");
    n = scat(line, n, b); scat(line, n, " bytes) installed");
    aui_text_sz(x + 16, cy, line, AUI_MUTED, AUI_FS_LABEL);
    cy += 22;
    {
        int pct = mi.frames_total ? (int)((mi.frames_used * 100) / mi.frames_total) : 0;
        aui_progress(x + 16, cy, w - 32, pct);
    }

    y += 132;
    aui_card(x, y, w, 150, AUI_ELEV_1);
    int ly = y + 14, lx = x + 16;
    aui_text_sz(lx, ly, "Frames", AUI_TEXT, AUI_FS_TITLE); ly += 28;

    /* The qualifier goes in the KEY, not the value: a value column that
     * sometimes carries a sentence runs into the next column, which is exactly
     * what it did. */
    ugroup(a, mi.frames_total);  kv(lx, &ly, "Total", a, AUI_TEXT);
    ugroup(a, mi.frames_free);   kv(lx, &ly, "Free", a, AUI_TEXT);
    ugroup(a, mi.frames_shared); kv(lx, &ly, "Shared (>1 ref)", a, AUI_TEXT);
    ugroup(a, mi.frames_pinned); kv(lx, &ly, "Pinned (never freed)", a, AUI_TEXT);

    int rx = x + w / 2 + 20; ly = y + 42;
    ugroup(a, mi.cow_pages);  kv(rx, &ly, "Copy-on-write", a, AUI_TEXT);
    ugroup(a, mi.cow_faults); kv(rx, &ly, "CoW copies", a, AUI_TEXT);
    ugroup(a, mi.cow_reuse);  kv(rx, &ly, "CoW reuses", a, AUI_TEXT);
    ugroup(a, mi.mm_bugs);
    kv(rx, &ly, "Allocator bugs", a, mi.mm_bugs ? AUI_ERROR : AUI_SUCCESS);

    /* The absent column, named in the UI rather than quietly omitted. */
    y += 162;
    aui_text_sz(x, y, "Per-process memory is not shown because the kernel does not"
                      " account it: nothing sums", AUI_MUTED, AUI_FS_CAPTION);
    aui_text_sz(x, y + 15, "frames per address space yet. These totals are"
                           " machine-wide and exact.", AUI_MUTED, AUI_FS_CAPTION);
}

static void view_system(int x, int y, int w)
{
    aui_card(x, y, w, 250, AUI_ELEV_1);
    int ly = y + 14, lx = x + 16;
    aui_text_sz(lx, ly, "Kernel report", AUI_TEXT, AUI_FS_TITLE);
    ly += 30;
    /* SYS_SYSINFO's own text, one line per row. It is the kernel's report and
     * it is rendered as the kernel wrote it -- reformatting it here would mean
     * this app deciding what the numbers mean. */
    char line[96];
    int ll = 0;
    for (int i = 0;; i++) {
        char c = sysbuf[i];
        if (c == '\n' || c == 0) {
            line[ll] = 0;
            /* Stop at SYS_SYSINFO's own process list. It reports the window
             * manager's GUI windows, which is a DIFFERENT set from the PCB
             * table on the Processes tab -- showing both, disagreeing, in one
             * app is how a monitor loses the user's trust. The Processes tab is
             * the authoritative one; it is the table you can act on. */
            if (line[0] == 'P' && line[1] == 'I' && line[2] == 'D') break;
            if (ll && ly < y + 232) {
                aui_text_sz(lx, ly, line, AUI_MUTED, AUI_FS_LABEL);
                ly += 19;
            }
            ll = 0;
            if (c == 0) break;
            continue;
        }
        if (ll < 95) line[ll++] = c;
    }
    /* The cross-check, stated on screen: the Memory tab's headline is computed
     * from pmm_used_frames(), and the "Memory" line above is computed by the
     * kernel as total-minus-free. Two independently maintained counters; if
     * they ever disagree by more than rounding, one of them is wrong. */
    ly += 6;
    aui_text_sz(lx, ly, "The Memory tab derives its total from a different kernel"
                        " counter than the line above.", AUI_MUTED, AUI_FS_CAPTION);
    aui_text_sz(lx, ly + 15, "They are meant to agree to within MiB rounding.",
                AUI_MUTED, AUI_FS_CAPTION);
    (void)w;
}

/* ---- footer: the kill control, present on every tab --------------------- */

static void footer(int x, int y, int w)
{
    const struct logit_procinfo *s = selected();
    aui_separator(x, y - 10, w);

    /* 256, not 160: name(31) + pid(10) + cwd(127) plus the joining words is
     * 189 bytes worst case, and scat() does not bound-check -- it is a fixed
     * concatenation helper, so the buffer has to be sized for the longest
     * string that can reach it, which is a full-length cwd. */
    char line[256];
    if (s) {
        int n = scat(line, 0, "Selected: ");
        n = scat(line, n, s->name[0] ? s->name : "(unnamed)");
        n = scat(line, n, "  pid ");
        n += ustr(line + n, (unsigned long long)s->pid);
        if (s->cwd[0]) { n = scat(line, n, "  in "); scat(line, n, s->cwd); }
    } else {
        scat(line, 0, "No process selected -- choose one in the Processes tab");
    }
    aui_text_ellipsis(x, y + 6, w - 150, line, s ? AUI_TEXT : AUI_MUTED, AUI_FS_LABEL);

    /* Enabled only for a process the kernel would actually accept, so a refusal
     * is visible BEFORE the click as well as after it. LOGIT_PROC_PROTECTED is
     * the kernel's own answer, not a rule re-derived here -- see the flag's
     * definition in logit_abi.h. */
    int can = killable();
    if (aui_button_ex(x + w - 132, y, 132, AUI_H_CTL, "Force Quit",
                      AUI_V_DANGER, can))
        confirm_open = 1;
    if (s && !can)
        aui_tooltip((s->flags & LOGIT_PROC_PROTECTED)
                        ? "Protected: the console shell (init)"
                        : (s->flags & LOGIT_PROC_DYING) ? "Already quitting"
                                                        : "Already exited");
}

static void confirm_dialog(void)
{
    const struct logit_procinfo *s = selected();
    if (!s) { confirm_open = 0; return; }
    /* Escape maps to the LAST button and Enter to the first, so Cancel is last:
     * of the two, Escape-cancels is the one that must not be got wrong. */
    static const char *const btn[2] = { "Force Quit", "Cancel" };
    if (aui_dialog_begin("Force Quit Process", 380, 150)) {
        char line[128];
        int n = scat(line, 0, s->name[0] ? s->name : "(unnamed)");
        n = scat(line, n, "  (pid ");
        n += ustr(line + n, (unsigned long long)s->pid);
        scat(line, n, ")");
        aui_text_sz(20, 16, line, AUI_TEXT, AUI_FS_BODY);
        aui_text_sz(20, 42, "The process is ended without being asked to save.",
                    AUI_MUTED, AUI_FS_LABEL);
        aui_text_sz(20, 60, "Unsaved work in it is lost.", AUI_MUTED, AUI_FS_LABEL);
        int p = aui_dialog_buttons(btn, 2);
        aui_dialog_end();
        if (p == 0) { do_kill(s->pid); confirm_open = 0; }
        else if (p == 1) confirm_open = 0;
    }
}

/* ---- frame -------------------------------------------------------------- */

static void frame(void)
{
    unsigned long long t0 = monotonic_ns();
    aui_begin(AUI_BG);

    /* Test probe, the same device c/apps/gui/gallery.c uses: a 6x6 rect at
     * window-local (4,4) in a colour unique to the current tab. It gives the
     * QMP driver the window's content origin without hard-coding anything
     * about the compositor, and tells it which view is showing. Two facts one
     * cheap rect can carry, and it sits in the margin left of the heading. */
    {
        static const unsigned probe[NTAB] = { 0xFF0080u, 0x00FF80u, 0xFFC800u };
        aui_fill(4, 4, 6, 6, probe[tab]);
    }

    aui_heading(AUI_PAD, 10, "Activity Monitor", AUI_TEXT);
    {
        char sub[64];
        int n = ustr(sub, (unsigned long long)npr);
        scat(sub, n, npr == 1 ? " process" : " processes");
        aui_text_sz(AUI_PAD + aui_text_w("Activity Monitor", AUI_FS_TITLE) + 12, 16,
                    sub, AUI_MUTED, AUI_FS_LABEL);
    }
    aui_tabs(AUI_PAD, 44, WINW - 2 * AUI_PAD, tabs, NTAB, &tab);

    int x = AUI_PAD, y = 96, w = WINW - 2 * AUI_PAD;
    if      (tab == T_PROC) view_processes(x, y, w);
    else if (tab == T_MEM)  view_memory(x, y, w);
    else                    view_system(x, y, w);

    /* The result of the last attempt, including a refusal, shown for 4 s. */
    if (last_msg[0] && monotonic_ms() - last_result_ms < 4000)
        aui_text_ellipsis(AUI_PAD, WINH - 74, WINW - 2 * AUI_PAD, last_msg,
                          last_result == LOGIT_KILL_OK ? AUI_SUCCESS : AUI_WARNING,
                          AUI_FS_CAPTION);

    footer(AUI_PAD, WINH - 50, WINW - 2 * AUI_PAD);
    if (confirm_open) confirm_dialog();
    aui_end();

    unsigned long long us = (monotonic_ns() - t0) / 1000;
    paint_us += us; paints++;
    if (us > paint_us_max) paint_us_max = us;
}

void app_main(void)
{
    gui_create("Activity Monitor", WINW, WINH);
    aui_set_size(WINW, WINH);
    self_pid = sys_getpid();
    t0_mono = monotonic_ms();
    t0_rtc = rtc_secs();
    refresh();
    frame();

    unsigned long long last = monotonic_ms();
    unsigned secs = 0;
    for (;;) {
        int drew = 0;
        struct logit_event e;
        while (poll_event(&e)) {
            if (e.type == EV_CLOSE) app_exit(0);

            /* THE MODAL TAKES THE KEYBOARD FIRST, and it has to be done here
             * rather than left to the toolkit. aui's scrim swallows CLICKS
             * aimed behind a dialog, but keys are dispatched to widgets in draw
             * order -- and the process table is drawn before the dialog, so
             * aui_table() consumed Enter (as "activate the selected row") and
             * marked it used before aui_dialog_buttons() ever saw it. The
             * confirmation could then only be answered with the mouse.
             * Answering the dialog here, before the event reaches aui at all,
             * keeps Enter/Escape working without forking the toolkit. */
            if (confirm_open && e.type == EV_KEY) {
                if (e.a == '\n') {
                    const struct logit_procinfo *s = selected();
                    if (s) do_kill(s->pid);
                    confirm_open = 0;
                    frame(); drew = 1;
                    continue;
                }
                if (e.a == 27) { confirm_open = 0; frame(); drew = 1; continue; }
            }

            /* Delete / Backspace is the Windows habit; it opens the same
             * confirmation the button does rather than killing outright. */
            if (e.type == EV_KEY && (e.a == 127 || e.a == 8) && killable())
                confirm_open = 1;
            aui_feed(&e);
            if (aui_want_repaint() || confirm_open) { frame(); drew = 1; }
            aui_feed_done();
        }
        unsigned long long now = monotonic_ms();
        if (now - last >= 1000) {
            last = now;
            refresh();
            frame();
            drew = 1;
            if (++secs % 5 == 0) { report_cost(); report_xcheck(); }
        }
        (void)drew;
        sys_yield();
    }
}

/* What a live table actually costs, on the serial console.
 *
 * aui_begin() clears the whole window and SYS_GUI_FLUSH carries no dirty
 * rectangle, so a 1 Hz refresh repaints 660x470 points every second whether
 * anything changed or not. Whether that is affordable is a measurement, not an
 * opinion, so the app times its own frame and prints the average and the worst
 * case rather than leaving it to be guessed at. */
/* ---- cross-checks: every number against a second source -------------------
 *
 * A task manager that lies is the one program a user catches immediately, so
 * the two quantities this app puts on screen are checked against independently
 * maintained sources and the result is printed where a test can assert on it.
 *
 *   MEMORY.  pmm keeps three counts by different means -- frames_total from the
 *            firmware map, frames_free from the free list, frames_used by
 *            counting frames whose refcount is >= 1. used + free == total is
 *            therefore an agreement between two structures, not an identity;
 *            if a frame is ever neither free nor referenced, this is what says
 *            so. (This is the same shape of check rmap_audit() makes.)
 *
 *   UPTIME.  monotonic_ms() is derived from the 100 Hz PIT. The RTC is a
 *            separate CMOS device on a separate crystal. Over an interval both
 *            must advance by the same number of seconds; a drift that grows is
 *            a tick being lost. Compared over the whole run, not per sample, so
 *            the RTC's 1-second granularity stays noise rather than signal.
 */
static unsigned long long rtc_secs(void)
{
    struct logit_time t;
    get_time(&t);
    /* Seconds within the day is enough: the checks compare DELTAS over minutes.
     * The day rollover is handled by simply not trusting a negative delta. */
    return (unsigned long long)t.hour * 3600u + (unsigned long long)t.minute * 60u
           + (unsigned long long)t.second;
}

static void report_xcheck(void)
{
    char m[256];
    unsigned long long sum = mi.frames_used + mi.frames_free;
    long long dm = (long long)sum - (long long)mi.frames_total;

    int n = scat(m, 0, "[monitor] xcheck mem used+free=");
    n += ustr(m + n, sum);
    n = scat(m, n, " total=");
    n += ustr(m + n, mi.frames_total);
    n = scat(m, n, " delta=");
    if (dm < 0) { m[n++] = '-'; m[n] = 0; dm = -dm; }
    n += ustr(m + n, (unsigned long long)dm);

    unsigned long long mono = (monotonic_ms() - t0_mono) / 1000;
    unsigned long long rtc = rtc_secs();
    long long dr = (long long)(rtc - t0_rtc) - (long long)mono;
    n = scat(m, n, "  uptime mono=");
    n += ustr(m + n, mono);
    n = scat(m, n, "s rtc=");
    n += ustr(m + n, (unsigned long long)(rtc - t0_rtc));
    n = scat(m, n, "s drift=");
    if (dr < 0) { m[n++] = '-'; m[n] = 0; dr = -dr; }
    n += ustr(m + n, (unsigned long long)dr);
    n = scat(m, n, "\n");
    sys_write(1, m, n);
}

static void report_cost(void)
{
    char m[160];
    int n = scat(m, 0, "[monitor] paints=");
    n += ustr(m + n, paints);
    n = scat(m, n, " avg_us=");
    n += ustr(m + n, paints ? paint_us / paints : 0);
    n = scat(m, n, " max_us=");
    n += ustr(m + n, paint_us_max);
    n = scat(m, n, " rows=");
    n += ustr(m + n, (unsigned long long)npr);
    n = scat(m, n, "\n");
    sys_write(1, m, n);
}
