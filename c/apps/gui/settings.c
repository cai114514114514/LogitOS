#include "aui.h"

/* ============================================================================
 * Settings -- the window where this machine's memory of its user is editable.
 *
 * Everything here is drawn with the aui toolkit and NOTHING is laid out at
 * hand-picked absolute coordinates. Positions come from aui_vstack/aui_row and
 * the cut helpers, sizes from the AUI_H_* metrics, colours from the theme
 * tokens, spacing from AUI_SP(). That is not tidiness for its own sake: hand
 * placed coordinates are what made this UI look like 1998, and they are also
 * what makes a window that cannot be resized -- every number would have to be
 * recomputed by hand. Read c/apps/gui/gallery.c for the vocabulary; this app
 * uses tabs, cards, toggles, sliders, dropdowns, text fields, a table and the
 * keyboard focus ring, all of which already existed.
 *
 * THE TWO-LAYER VIEW, and why both are here:
 *
 *   The four tabs are a CURATED view -- the right widget for each setting, a
 *   toggle for a boolean, a colour row for an accent, a dotted-quad field for
 *   an address, grouped the way a person thinks about them.
 *
 *   The "All settings" tab is a GENERATED view -- every key in the store,
 *   including keys this app has never heard of, read back through
 *   SYS_SETTING_CTL/SETCTL_KVAT. It exists because the store deliberately
 *   preserves unknown keys so another line can persist its own state without a
 *   kernel change; a store that keeps keys nothing can show you is a store
 *   nobody can debug. It is also the screen that says WHAT WAS REJECTED when
 *   the settings file was damaged.
 *
 * WRITES ARE BATCHED. Moving a slider does not write the disk. Every edit goes
 * into the kernel's in-RAM table (commit = 0) and "Apply" commits once: one
 * whole-file write, one LogitFS transaction, one atomic replacement. A commit
 * per keystroke in a text field would be one disk transaction per character.
 * ========================================================================== */

#define WINW 640
#define WINH 480

/* A probe rect in an unmistakable colour, at window-local (4,4), exactly as
 * Gallery does it: the QMP driver finds the window's content origin from this
 * instead of hard-coding a compositor layout it cannot see. One colour per tab
 * so the driver can tell which page it is looking at without OCR. */
#define PROBE_X 4
#define PROBE_Y 4
#define PROBE_W 6
#define PROBE_H 6

enum { T_APPEAR, T_DESKTOP, T_NETWORK, T_ALL, NTAB };
static const char *const tabs[NTAB] = { "Appearance", "Desktop", "Network", "All settings" };
static const unsigned probe_rgb[NTAB] = { 0xFF0080, 0x00FF80, 0xFFC800, 0x00A0FF };
static int tab;

/* ---- live values, loaded from the store at start ---- */
static int  v_dark;
static int  v_accent_h, v_accent_s, v_accent_l;   /* the accent, as HSL */
static char v_wallpaper[LOGIT_SET_VALMAX];
static int  v_restore;
static int  v_dhcp;
static char v_ip[LOGIT_SET_VALMAX], v_mask[LOGIT_SET_VALMAX],
            v_gw[LOGIT_SET_VALMAX], v_dns[LOGIT_SET_VALMAX];

static int  dirty;          /* an edit is pending a commit */
static int  saved_flash;    /* ms timestamp: show "Saved" briefly after Apply */
static int  last_gen;       /* SETCTL_GEN, to notice another process's commit */

/* The footer's message, and how loud it is.
 *
 * Feedback lives IN THE WINDOW rather than going through the notification
 * service, and that is a dependency decision, not a design preference: the
 * notification ABI belongs to another line and has not landed. Building
 * against a header that only exists in somebody's working tree is how a commit
 * stops building from a clean clone of itself -- which is exactly how this was
 * caught here. The footer was always going to say "Unsaved changes"; saying
 * "Saved" and "Could not write" in the same place costs nothing and depends on
 * nothing. */
enum { M_NONE, M_SAVED, M_ERROR, M_RESET };
static int  msg;

/* ---- the generated tab's state ---- */
static int  all_sel, all_scroll;
#define MAXKV 64
static char kv_key[MAXKV][48];
static char kv_val[MAXKV][LOGIT_SET_VALMAX];
static const char *kv_src[MAXKV];
static const char *kv_cell[MAXKV * 3];
static int  nkv;

/* ---- wallpaper choices. Only paths that are actually on the disk are
 * offered; a dropdown listing a file that does not exist would "work" and then
 * silently do nothing, which is the failure mode this whole app exists to
 * avoid. The last entry is the no-wallpaper case (the gradient). ---- */
static const char *const wp_paths[] = { "/wallpaper.png", "/media/dot.png", "(gradient)" };
#define NWP ((int)(sizeof wp_paths / sizeof wp_paths[0]))
static int wp_sel;

/* ------------------------------------------------------------- utilities --*/
static int s_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static int s_eq(const char *a, const char *b)
{ for (int i = 0;; i++) { if (a[i] != b[i]) return 0; if (!a[i]) return 1; } }
static void s_cpy(char *d, const char *s, int cap)
{ int i = 0; if (cap <= 0) return; while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; } d[i] = 0; }

static void itoa_(int v, char *b)
{
    char t[16];
    int n = 0, p = 0;
    unsigned u = v < 0 ? (unsigned)(-v) : (unsigned)v;
    if (!u) t[n++] = '0';
    while (u) { t[n++] = (char)('0' + u % 10); u /= 10; }
    if (v < 0) b[p++] = '-';
    while (n) b[p++] = t[--n];
    b[p] = 0;
}

static void hex6(unsigned v, char *b)
{
    static const char *H = "0123456789ABCDEF";
    b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 6; i++) b[2 + i] = H[(v >> (20 - 4 * i)) & 0xF];
    b[8] = 0;
}

/* An RGB back to HSL, so the three sliders start where the stored accent is
 * rather than at an arbitrary point that would silently rewrite the user's
 * colour the first time any slider moved. */
static void rgb_to_hsl(unsigned c, int *h, int *s, int *l)
{
    int r = (int)((c >> 16) & 255), g = (int)((c >> 8) & 255), b = (int)(c & 255);
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int L = (mx + mn) * 100 / 510;
    int S = 0, H = 0;
    if (mx != mn) {
        int d = mx - mn;
        S = (mx + mn) <= 255 ? d * 100 / (mx + mn) : d * 100 / (510 - mx - mn);
        if (mx == r)      H = (g - b) * 60 / d + (g < b ? 360 : 0);
        else if (mx == g) H = (b - r) * 60 / d + 120;
        else              H = (r - g) * 60 / d + 240;
    }
    *h = H < 0 ? H + 360 : H; *s = S; *l = L;
}

static unsigned accent_now(void) { return aui_hsl(v_accent_h, v_accent_s, v_accent_l); }

/* ============================================================================
 * Loading and saving
 * ========================================================================= */
static void load_all(void)
{
    v_dark    = setting_int("ui.dark", 0) ? 1 : 0;
    v_restore = setting_int("desktop.restore_session", 1) ? 1 : 0;
    v_dhcp    = setting_int("net.dhcp", 1) ? 1 : 0;
    rgb_to_hsl((unsigned)setting_int("ui.accent", 0x5E96FF),
               &v_accent_h, &v_accent_s, &v_accent_l);

    if (setting_get("ui.wallpaper", v_wallpaper, (int)sizeof v_wallpaper) <= 0)
        s_cpy(v_wallpaper, "/wallpaper.png", (int)sizeof v_wallpaper);
    wp_sel = NWP - 1;
    for (int i = 0; i < NWP; i++) if (s_eq(wp_paths[i], v_wallpaper)) wp_sel = i;

    if (setting_get("net.ip",   v_ip,   (int)sizeof v_ip)   <= 0) s_cpy(v_ip,   "10.0.2.15", LOGIT_SET_VALMAX);
    if (setting_get("net.mask", v_mask, (int)sizeof v_mask) <= 0) s_cpy(v_mask, "255.255.255.0", LOGIT_SET_VALMAX);
    if (setting_get("net.gw",   v_gw,   (int)sizeof v_gw)   <= 0) s_cpy(v_gw,   "10.0.2.2", LOGIT_SET_VALMAX);
    if (setting_get("net.dns",  v_dns,  (int)sizeof v_dns)  <= 0) s_cpy(v_dns,  "10.0.2.3", LOGIT_SET_VALMAX);

    last_gen = setting_gen();
    dirty = 0;
}

/* Split "key=value" as SETCTL_KVAT returns it. */
static void kv_split(const char *line, char *k, char *v)
{
    int e = 0;
    while (line[e] && line[e] != '=') e++;
    int n = e < 47 ? e : 47;
    for (int i = 0; i < n; i++) k[i] = line[i];
    k[n] = 0;
    s_cpy(v, line[e] ? line + e + 1 : "", LOGIT_SET_VALMAX);
}

/* The generated view. THREE columns, and the third is the one that earns the
 * tab: a value is "stored" (this machine's, on disk), "default" (nothing stored,
 * the compiled-in answer) or "rejected" (something IS stored and the schema
 * refused it, so the machine is running on the default instead).
 *
 * Every schema key is listed even when nothing is stored, because a fresh
 * machine has an empty store and a table with no rows in it tells a user
 * nothing -- least of all the user who came here to find out what a setting is
 * called. And the RAW stored bytes are what a rejected row shows, not the
 * effective value: the whole point of showing it is so it can be repaired. */
static void load_kv(void)
{
    nkv = 0;
    int ns = setting_count();
    for (int i = 0; i < ns && nkv < MAXKV; i++) {
        struct logit_setting s;
        if (setting_enum(i, &s) < 0) break;
        s_cpy(kv_key[nkv], s.key, 48);

        /* Is anything actually stored for this key, and what? */
        const char *src = "default";
        char raw[LOGIT_SET_VALMAX];
        raw[0] = 0;
        int stored = setting_kv_count();
        for (int j = 0; j < stored; j++) {
            char line[LOGIT_SET_KVLINE], k[48], v[LOGIT_SET_VALMAX];
            if (setting_kv_at(j, line) < 0) break;
            kv_split(line, k, v);
            if (s_eq(k, s.key)) { s_cpy(raw, v, LOGIT_SET_VALMAX); break; }
        }
        if (raw[0]) {
            s_cpy(kv_val[nkv], raw, LOGIT_SET_VALMAX);
            src = s_eq(raw, s.value) ? "stored" : "rejected";
        } else {
            s_cpy(kv_val[nkv], s.value, LOGIT_SET_VALMAX);
        }
        kv_src[nkv] = src;
        nkv++;
    }

    /* Then every stored key the schema does not name -- another line's state,
     * kept across rewrites precisely so it can live here. */
    int stored = setting_kv_count();
    for (int j = 0; j < stored && nkv < MAXKV; j++) {
        char line[LOGIT_SET_KVLINE], k[48], v[LOGIT_SET_VALMAX];
        if (setting_kv_at(j, line) < 0) break;
        kv_split(line, k, v);
        int known = 0;
        for (int i = 0; i < ns; i++) {
            struct logit_setting s;
            if (setting_enum(i, &s) < 0) break;
            if (s_eq(k, s.key)) { known = 1; break; }
        }
        if (known) continue;
        s_cpy(kv_key[nkv], k, 48);
        s_cpy(kv_val[nkv], v, LOGIT_SET_VALMAX);
        kv_src[nkv] = "app";
        nkv++;
    }

    for (int i = 0; i < nkv; i++) {
        kv_cell[i * 3 + 0] = kv_key[i];
        kv_cell[i * 3 + 1] = kv_val[i];
        kv_cell[i * 3 + 2] = kv_src[i];
    }
}

/* Push every edited value into the kernel's table WITHOUT committing, then
 * commit once. If the commit fails the store is unchanged on disk -- the whole
 * file is one transaction -- so there is no half-applied state to explain. */
static void apply_all(void)
{
    char b[24];
    itoa_(v_dark, b);           setting_set("ui.dark", b, 0);
    itoa_(v_restore, b);        setting_set("desktop.restore_session", b, 0);
    itoa_(v_dhcp, b);           setting_set("net.dhcp", b, 0);
    hex6(accent_now(), b);      setting_set("ui.accent", b, 0);
    setting_set("ui.wallpaper", wp_sel == NWP - 1 ? "(none)" : wp_paths[wp_sel], 0);
    setting_set("net.ip",   v_ip,   0);
    setting_set("net.mask", v_mask, 0);
    setting_set("net.gw",   v_gw,   0);
    setting_set("net.dns",  v_dns,  0);

    saved_flash = (int)aui_ms();
    if (setting_commit() == 0) {
        dirty = 0;
        last_gen = setting_gen();
        load_kv();
        msg = M_SAVED;
    } else {
        /* `dirty` stays set. The disk still holds the previous file, whole --
         * one write is one transaction -- so the honest thing is to say the
         * change is still unsaved rather than to clear the flag and pretend. */
        msg = M_ERROR;
    }
}

/* ============================================================================
 * Pages
 * ========================================================================= */
static void row_label(struct aui_rect *r, const char *title, const char *sub)
{
    struct aui_rect line = aui_cut_top(r, sub ? AUI_SP(11) : AUI_H_CTL);
    aui_label(line.x, line.y + (sub ? 0 : 6), title, AUI_TEXT);
    if (sub) aui_text_sz(line.x, line.y + AUI_SP(5), sub, AUI_MUTED, AUI_FS_CAPTION);
}

/* The Appearance card's height, and it is a computed number rather than a
 * chosen one: 16 inset + 36 toggle row + 8 + 1 rule + 12 + 44 accent header +
 * 3 x 28 sliders + 20 hex row + 16 inset = 236. The first version of this
 * function guessed 120 and the preview card landed on top of two of the
 * sliders, which is the failure mode of picking a card size by eye. */
#define APPEAR_CARD_H  AUI_SP(59)      /* 236 */

static void page_appearance(struct aui_rect body)
{
    aui_card(body.x, body.y, body.w, APPEAR_CARD_H, AUI_ELEV_1);
    struct aui_rect c = aui_inset(aui_r(body.x, body.y, body.w, APPEAR_CARD_H), AUI_PAD, AUI_PAD);

    /* Dark mode. Applied LIVE through SYS_UI_DARK rather than waiting for
     * Apply: the whole desktop repaints, so the user sees the setting doing
     * what it says while the switch is still under their finger. It is still
     * written to the store by Apply like everything else -- and the WM writes
     * it too, because flipping the menu-bar switch has to persist whether or
     * not this window is open. */
    struct aui_rect r = aui_cut_top(&c, AUI_H_LG);
    aui_label(r.x, r.y + 8, "Dark appearance", AUI_TEXT);
    if (aui_toggle(r.x + r.w - 48, r.y + 4, &v_dark, 1)) {
        sys_ui_dark(v_dark);
        aui_set_dark(v_dark);
        dirty = 1;
    }
    aui_cut_top(&c, AUI_SP(2));
    aui_separator(c.x, c.y, c.w);
    aui_cut_top(&c, AUI_SP(3));

    /* Accent, as three sliders over HSL rather than a hex field: a hex field
     * makes you already know the answer, and three sliders let you find it.
     *
     * The swatch sits in the header row's own right-hand end (a cut_right off
     * that row), not at a coordinate measured from the card. That is the whole
     * reason the cut helpers exist -- the previous version anchored it to
     * body.y and the card's inset, and it drifted the moment the rows above it
     * changed height. */
    struct aui_rect hr = aui_cut_top(&c, AUI_SP(12));
    struct aui_rect sw = aui_cut_right(&hr, AUI_SP(13));
    aui_label(hr.x, hr.y, "Accent colour", AUI_TEXT);
    aui_text_sz(hr.x, hr.y + AUI_SP(5),
                "Used for selection, focus rings and the primary button",
                AUI_MUTED, AUI_FS_CAPTION);
    unsigned acc = accent_now();
    aui_round(sw.x, sw.y, AUI_SP(12), AUI_SP(12), AUI_R_MD, acc);

    struct aui_rect sr = aui_cut_top(&c, AUI_H_CTL);
    aui_text_sz(sr.x, sr.y + 7, "Hue", AUI_MUTED, AUI_FS_CAPTION);
    if (aui_slider(sr.x + AUI_SP(9), sr.y + 4, sr.w - AUI_SP(11), &v_accent_h, 0, 359)) dirty = 1;
    sr = aui_cut_top(&c, AUI_H_CTL);
    aui_text_sz(sr.x, sr.y + 7, "Sat", AUI_MUTED, AUI_FS_CAPTION);
    if (aui_slider(sr.x + AUI_SP(9), sr.y + 4, sr.w - AUI_SP(11), &v_accent_s, 0, 100)) dirty = 1;
    sr = aui_cut_top(&c, AUI_H_CTL);
    aui_text_sz(sr.x, sr.y + 7, "Lum", AUI_MUTED, AUI_FS_CAPTION);
    if (aui_slider(sr.x + AUI_SP(9), sr.y + 4, sr.w - AUI_SP(11), &v_accent_l, 20, 80)) dirty = 1;

    /* The hex the file will hold, so what is on screen and what
     * `cat /etc/settings.conf` shows are visibly the same thing. */
    acc = accent_now();
    struct aui_rect hxr = aui_cut_top(&c, AUI_SP(5));
    char hx[12];
    hex6(acc, hx);
    aui_text_in(hxr, hx, AUI_MUTED, AUI_FS_CAPTION, AUI_ALIGN_RIGHT);

    /* Live preview of the accent on a real control, because a swatch does not
     * tell you whether the label on top of it will be readable. */
    aui_set_accent(acc);
    struct aui_rect pv = aui_r(body.x, body.y + APPEAR_CARD_H + AUI_SP(3), body.w, AUI_SP(16));
    aui_card(pv.x, pv.y, pv.w, pv.h, AUI_ELEV_1);
    aui_text_sz(pv.x + AUI_PAD, pv.y + AUI_SP(2), "Preview", AUI_MUTED, AUI_FS_CAPTION);
    aui_button_ex(pv.x + AUI_PAD, pv.y + AUI_SP(6), 96, AUI_H_CTL, "Primary", AUI_V_PRIMARY, 1);
    aui_button_ex(pv.x + AUI_PAD + 104, pv.y + AUI_SP(6), 96, AUI_H_CTL, "Secondary", AUI_V_SECONDARY, 1);
    aui_badge(pv.x + AUI_PAD + 216, pv.y + AUI_SP(7), "Accent", acc);
}

static void page_desktop(struct aui_rect body)
{
    aui_card(body.x, body.y, body.w, AUI_SP(26), AUI_ELEV_1);
    struct aui_rect c = aui_inset(aui_r(body.x, body.y, body.w, AUI_SP(26)), AUI_PAD, AUI_PAD);

    row_label(&c, "Wallpaper", "Read at boot by the compositor; a missing file falls back to the gradient");
    struct aui_rect r = aui_cut_top(&c, AUI_H_CTL);
    if (aui_select(r.x, r.y, 240, wp_paths, NWP, &wp_sel)) dirty = 1;
    aui_spacer(AUI_SP(2));

    aui_separator(c.x, c.y + AUI_SP(2), c.w);
    c.y += AUI_SP(5); c.h -= AUI_SP(5);

    r = aui_cut_top(&c, AUI_H_LG);
    aui_label(r.x, r.y + 8, "Reopen windows on login", AUI_TEXT);
    if (aui_toggle(r.x + r.w - 48, r.y + 4, &v_restore, 1)) dirty = 1;
    aui_text_sz(c.x, c.y, "Saved window frames are kept either way; this only says whether to use them",
                AUI_MUTED, AUI_FS_CAPTION);

    /* Honesty panel. The store side of window geometry is done and tested; the
     * two call sites are the window-management line's. Saying so in the UI is
     * better than a toggle that looks like it works. */
    struct aui_rect n = aui_r(body.x, body.y + AUI_SP(28), body.w, AUI_SP(14));
    aui_card(n.x, n.y, n.w, n.h, AUI_ELEV_0);
    aui_text_sz(n.x + AUI_PAD, n.y + AUI_SP(3), "Window frames", AUI_TEXT, AUI_FS_LABEL);
    char line[96];
    int nf = 0;
    for (int i = 0; i < nkv; i++)
        if (kv_key[i][0] == 'w' && kv_key[i][1] == 'i' && kv_key[i][2] == 'n' && kv_key[i][3] == '.') nf++;
    char num[12];
    itoa_(nf, num);
    s_cpy(line, num, (int)sizeof line);
    int L = s_len(line);
    s_cpy(line + L, " stored (see the All settings tab)", (int)sizeof line - L);
    aui_text_sz(n.x + AUI_PAD, n.y + AUI_SP(8), line, AUI_MUTED, AUI_FS_CAPTION);
}

static void page_network(struct aui_rect body)
{
    aui_card(body.x, body.y, body.w, AUI_SP(38), AUI_ELEV_1);
    struct aui_rect c = aui_inset(aui_r(body.x, body.y, body.w, AUI_SP(38)), AUI_PAD, AUI_PAD);

    struct aui_rect r = aui_cut_top(&c, AUI_H_LG);
    aui_label(r.x, r.y + 8, "Configure automatically (DHCP)", AUI_TEXT);
    if (aui_toggle(r.x + r.w - 48, r.y + 4, &v_dhcp, 1)) dirty = 1;
    aui_text_sz(c.x, c.y, "Takes effect at the next boot -- the address is chosen in net_init()",
                AUI_MUTED, AUI_FS_CAPTION);
    c.y += AUI_SP(5); c.h -= AUI_SP(5);
    aui_separator(c.x, c.y, c.w);
    c.y += AUI_SP(3); c.h -= AUI_SP(3);

    /* The four address fields are DISABLED while DHCP is on rather than hidden.
     * Hiding them would make the window jump by four rows on a toggle; greyed
     * out, they also show what the machine would use if you turned DHCP off. */
    int en = !v_dhcp;
    static const char *const names[4] = { "IP address", "Subnet mask", "Router", "DNS server" };
    char *const bufs[4] = { v_ip, v_mask, v_gw, v_dns };
    for (int i = 0; i < 4; i++) {
        r = aui_cut_top(&c, AUI_SP(9));
        aui_text_sz(r.x, r.y + 7, names[i], en ? AUI_TEXT : AUI_DISABLED_TX, AUI_FS_LABEL);
        if (aui_textfield_ex(r.x + AUI_SP(26), r.y + 2, 180, bufs[i], LOGIT_SET_VALMAX, "0.0.0.0", en))
            dirty = 1;
    }
    aui_text_sz(c.x, c.y + AUI_SP(1),
                "A value that is not a dotted quad is rejected at boot and the default used",
                AUI_MUTED, AUI_FS_CAPTION);
}

static void page_all(struct aui_rect body)
{
    /* What the machine will say about a damaged file, in the window rather than
     * only on the serial line. */
    int d = setting_diag();
    struct aui_rect hdr = aui_cut_top(&body, AUI_SP(11));
    if (d == 0 || d == LOGIT_SETD_NOFILE) {
        aui_badge(hdr.x, hdr.y, d ? "No settings file yet -- showing defaults" : "Loaded cleanly",
                  d ? AUI_MUTED : AUI_SUCCESS);
    } else {
        const char *msg = (d & LOGIT_SETD_TRUNCATED) ? "File was truncated -- incomplete final line dropped"
                        : (d & LOGIT_SETD_BADLINE)   ? "Some lines did not parse and were ignored"
                        : (d & LOGIT_SETD_RANGE)     ? "Some values were out of range -- defaults used"
                        : (d & LOGIT_SETD_CRCBAD)    ? "Checksum mismatch (hand-edited, or a torn write)"
                        : "Settings table full";
        aui_badge(hdr.x, hdr.y, msg, AUI_WARNING);
    }

    /* Widths sum to 560, which is the table's inner width (608) less the two
     * border pixels and the 12-point scrollbar gutter, less a little slack --
     * a column set that overflows is ellipsised rather than clipped, but that
     * is this app's arithmetic being wrong, not the widget's. */
    static const char *const cols[3] = { "Key", "Value", "Source" };
    static const int colw[3] = { 230, 250, 80 };
    aui_table(body.x, body.y, body.w, body.h - AUI_SP(12), cols, colw, 3,
              kv_cell, nkv, &all_sel, &all_scroll);

    /* Two short lines rather than one long one: at AUI_FS_CAPTION the single
     * line ran past the window edge and was ellipsised mid-word, which loses
     * exactly the half that explains "rejected". */
    aui_text_sz(body.x, body.y + body.h - AUI_SP(10),
                "stored = on disk   default = built in   rejected = on disk, refused",
                AUI_MUTED, AUI_FS_CAPTION);
    aui_text_sz(body.x, body.y + body.h - AUI_SP(6),
                "/etc/settings.conf -- plain text; `cat` it, or open it in TextEdit",
                AUI_MUTED, AUI_FS_CAPTION);
}

/* ============================================================================
 * The frame
 * ========================================================================= */
static void frame(void)
{
    aui_begin(AUI_BG);

    struct aui_rect all = aui_r(0, 0, WINW, WINH);
    struct aui_rect top = aui_cut_top(&all, AUI_SP(13));
    aui_heading(AUI_PAD, top.y + AUI_SP(2), "Settings", AUI_TEXT);

    struct aui_rect strip = aui_cut_top(&all, AUI_SP(11));
    aui_tabs(AUI_PAD, strip.y, WINW - 2 * AUI_PAD, tabs, NTAB, &tab);

    /* The page probe -- see the note at the top. Painted AFTER aui_tabs, which
     * is not a detail: aui is immediate-mode, so `tab` is updated BY the call
     * above. Drawing the probe before it advertised the previous frame's page
     * while the body drew the new one, and the QMP driver believed the probe --
     * which is exactly what a probe is for, so the probe has to be right. */
    aui_fill(PROBE_X, PROBE_Y, PROBE_W, PROBE_H, probe_rgb[tab]);

    struct aui_rect foot = aui_cut_bottom(&all, AUI_SP(13));
    struct aui_rect body = aui_inset(all, AUI_PAD, AUI_SP(2));

    switch (tab) {
    case T_APPEAR:  page_appearance(body); break;
    case T_DESKTOP: page_desktop(body);    break;
    case T_NETWORK: page_network(body);    break;
    default:        page_all(body);        break;
    }

    /* Footer: state on the left, actions on the right. */
    aui_hairline(0, foot.y, WINW);
    {
        int fresh = saved_flash && (int)aui_ms() - saved_flash < 2500;
        const char *state = "";
        unsigned tint = AUI_MUTED;
        if (fresh && msg == M_ERROR) { state = "Could not write /etc/settings.conf"; tint = AUI_ERROR; }
        else if (dirty)              { state = "Unsaved changes";                    tint = AUI_WARNING; }
        else if (fresh && msg == M_RESET) { state = "Reset to defaults";             tint = AUI_MUTED; }
        else if (fresh && msg == M_SAVED) { state = "Saved to /etc/settings.conf";   tint = AUI_SUCCESS; }
        if (state[0])
            aui_text_sz(AUI_PAD, foot.y + AUI_SP(4), state, tint, AUI_FS_CAPTION);
    }

    int bx = WINW - AUI_PAD - 90;
    if (aui_button_ex(bx, foot.y + AUI_SP(2), 90, AUI_H_CTL, "Apply",
                      AUI_V_PRIMARY, dirty)) apply_all();
    bx -= 98;
    if (aui_button_ex(bx, foot.y + AUI_SP(2), 90, AUI_H_CTL, "Revert",
                      AUI_V_SECONDARY, dirty)) { setting_reload(); load_all(); load_kv(); }
    bx -= 110;
    if (aui_button_ex(bx, foot.y + AUI_SP(2), 102, AUI_H_CTL, "Reset all",
                      AUI_V_DANGER, 1)) {
        /* Deleting the file IS the reset: every key then reads as its built-in
         * default, which is exactly what a fresh machine does. There is no
         * separate "defaults" code path to drift out of step with the schema. */
        setting_reset();
        load_all();
        load_kv();
        sys_ui_dark(v_dark);
        aui_set_dark(v_dark);
        aui_set_accent(accent_now());
        msg = M_RESET;
        saved_flash = (int)aui_ms();
    }

    aui_end();
}

void app_main(void)
{
    gui_create("Settings", WINW, WINH);
    aui_set_size(WINW, WINH);
    aui_set_dark(sys_ui_dark(-1) > 0);
    load_all();
    load_kv();
    aui_set_accent(accent_now());
    frame();

    struct logit_event e;
    unsigned last_poll = 0;
    for (;;) {
        int drew = 0;
        while (poll_event(&e)) {
            if (e.type == EV_CLOSE) app_exit(0);
            aui_feed(&e);
            if (aui_want_repaint()) { frame(); drew = 1; }
            aui_feed_done();
        }
        /* The change notification, polled. Another process committing a
         * setting -- the menu-bar dark-mode switch is the one that actually
         * happens -- bumps the generation, and this window follows instead of
         * showing a value that is no longer true. Once every 250 ms is two
         * syscalls a second; an event broadcast would cost every app in the
         * system a queue slot to tell most of them something they do not want. */
        unsigned now = aui_ms();
        if (now - last_poll >= 250) {
            last_poll = now;
            int g = setting_gen();
            if (g != last_gen && !dirty) { load_all(); load_kv(); frame(); drew = 1; }
            else if (saved_flash && (int)now - saved_flash < 2800) { frame(); drew = 1; }
        }
        if (!drew) wait_idle(100);   /* was sys_yield(): a spin. input-driven */
    }
}
