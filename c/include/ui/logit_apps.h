/* logit_apps.h -- app identity: display names, icon art, launch arguments and
 * the window-title rule (design_language.md 3.6 display names, 3.7.4 app
 * icons, 3.7.5 the Logit mark, 5.1 titles; launch args per critique 23).
 *
 * The STEM is the .aex file name without ".aex" (files, settings, ...). It is
 * also the English identity the WM prints in `[wm] launched %s`, which test
 * drivers match, so it never changes; the Chinese display name below is what
 * the Dock tooltip, the menu bar, the switcher and Spotlight show.
 *
 * Freestanding, header-only, `static const` tables only, no libc. */
#ifndef LOGIT_UI_APPS_H
#define LOGIT_UI_APPS_H

#include <stdint.h>
#include <stddef.h>

/* Icon glyphs (3.7.4). The renderer (tools/mkicons, ui-art-assets) keys its
 * art on these; `glyph` below is the same name as a string. */
enum logit_app_glyph {
    LOGIT_GLYPH_FOLDER_TWOTONE,   /* back plane white @ 170, front white */
    LOGIT_GLYPH_GEAR_RING,        /* gear with a ring cut-out */
    LOGIT_GLYPH_GLOBE_COMPASS,    /* meridians plus a compass needle (accent) */
    LOGIT_GLYPH_DIAL,             /* 12 ticks, hands at 10:08, second hand (accent) */
    LOGIT_GLYPH_SPARKLINE,        /* sparkline (accent) with a gradient fill under it */
    LOGIT_GLYPH_PHOTOS,           /* two tilted photos (-8°, +6°), sky-gradient content */
    LOGIT_GLYPH_CODE,             /* `</>` */
    LOGIT_GLYPH_LOGIT_SPARKLE,    /* the Logit mark plus a 4-point sparkle */
    LOGIT_GLYPH_PROMPT,           /* `>_` with an accent cursor */
    LOGIT_GLYPH_RULED_PEN,        /* ruled paper with an accent pen */
    LOGIT_GLYPH_GRID4,            /* 2 x 2 rounded squares */
    LOGIT_GLYPH_TILES3,           /* 1 large and 2 small rounded tiles */
    LOGIT_GLYPH_BUBBLES,          /* two speech bubbles (not in 3.7.4: see 对话) */
    LOGIT_GLYPH_LOGIT_MARK,       /* the Logit mark alone (3.7.5) */
    LOGIT_GLYPH_SEARCH,           /* magnifier (not in 3.7.4: see 搜索) */
    LOGIT_GLYPH_COUNT
};

#define LOGIT_APP_F_BRAND_GRADIENT 1u   /* tile is the 135° Logit gradient (3.1.6) */
#define LOGIT_APP_F_LIGHT_TILE     2u   /* light tile: glyph in #1D1D23 */
#define LOGIT_APP_F_NO_DOCK        4u   /* never a Dock tile (panels, greeter, test apps) */
#define LOGIT_APP_F_TEST_ONLY      8u   /* not on the product or release disk */

struct logit_app {
    const char *stem;        /* .aex stem, the English identity */
    const char *zh;          /* display name (3.6), UTF-8 */
    const char *en;          /* English alias: Spotlight matches it ("set" -> 设置) */
    const char *alias;       /* further alias words, space separated, or NULL */
    uint32_t grad_top, grad_bottom;   /* 0xRRGGBB, 2-stop vertical gradient */
    uint8_t glyph;           /* enum logit_app_glyph */
    const char *glyph_name;  /* the same, as a string */
    uint32_t glyph_ink;      /* #FFFFFF, or #1D1D23 on light tiles */
    uint32_t glyph_accent;   /* the glyph's one coloured detail, 0 = none */
    uint8_t flags;           /* LOGIT_APP_F_* */
};

#define LOGIT_APP_INK_WHITE 0xFFFFFFu
#define LOGIT_APP_INK_DARK  0x1D1D23u

/* Rows for 对话 (ch), 登录 (greeter) and 搜索 (spotlight) are not in the
 * 3.7.4 table. 登录 uses the 3.7.5 mark tile (white mark on the Logit
 * gradient), which 3.7.5 names for the Greeter. 对话 and 搜索 take a tile in
 * the family's 2-stop style; they are never in the Dock. */
static const struct logit_app LOGIT_APPS[] = {
    { "files",     "文件管理",   "Finder",    "Files",
      0x62C6FF, 0x1E7CF2, LOGIT_GLYPH_FOLDER_TWOTONE, "folder-twotone", LOGIT_APP_INK_WHITE, 0, 0 },
    { "settings",  "设置",       "Settings",  "Preferences",
      0x9A9AA6, 0x4A4A55, LOGIT_GLYPH_GEAR_RING, "gear-ring", LOGIT_APP_INK_WHITE, 0, 0 },
    { "browser",   "浏览器",     "Browser",   "Web",
      0x55D3FF, 0x3360FF, LOGIT_GLYPH_GLOBE_COMPASS, "globe-compass", LOGIT_APP_INK_WHITE, 0xFF5F56, 0 },
    { "clock",     "时钟",       "Clock",     "Time",
      0xFFFFFF, 0xE6E6EC, LOGIT_GLYPH_DIAL, "dial", LOGIT_APP_INK_DARK, 0xFF9F0A, LOGIT_APP_F_LIGHT_TILE },
    { "monitor",   "活动监视器", "Monitor",   "Activity Monitor",
      0x2E2E36, 0x101015, LOGIT_GLYPH_SPARKLINE, "sparkline", LOGIT_APP_INK_WHITE, 0x34D17A, 0 },
    { "preview",   "预览",       "Preview",   "Photos Images",
      0xFFFFFF, 0xECEEF4, LOGIT_GLYPH_PHOTOS, "photos", LOGIT_APP_INK_DARK, 0, LOGIT_APP_F_LIGHT_TILE },
    { "studio",    "代码工作室", "Studio",    "Code",
      0xA98BFF, 0x6A2BD9, LOGIT_GLYPH_CODE, "code", LOGIT_APP_INK_WHITE, 0, 0 },
    { "assistant", "Logit 助手", "Assistant", "Tasks Logit",
      0x7C5CFF, 0x38A8FF, LOGIT_GLYPH_LOGIT_SPARKLE, "logit-sparkle", LOGIT_APP_INK_WHITE, 0,
      LOGIT_APP_F_BRAND_GRADIENT },
    { "terminal",  "终端",       "Terminal",  "Shell",
      0x2C2C34, 0x131318, LOGIT_GLYPH_PROMPT, "prompt", LOGIT_APP_INK_WHITE, 0x34D17A, 0 },
    { "textedit",  "文本编辑",   "TextEdit",  "Text Editor",
      0xFFFFFF, 0xF0F0F4, LOGIT_GLYPH_RULED_PEN, "ruled-pen", LOGIT_APP_INK_DARK, 0xFF9F0A,
      LOGIT_APP_F_LIGHT_TILE },
    { "gallery",   "组件库",     "Gallery",   NULL,
      0xFF9F0A, 0xFF375F, LOGIT_GLYPH_GRID4, "grid4", LOGIT_APP_INK_WHITE, 0, LOGIT_APP_F_TEST_ONLY },
    { "widgets",   "小组件",     "Widgets",   NULL,
      0x34C7A5, 0x2280F0, LOGIT_GLYPH_TILES3, "tiles3", LOGIT_APP_INK_WHITE, 0, LOGIT_APP_F_TEST_ONLY },
    { "ch",        "对话",       "Chat",      "Messages",
      0x7FE0A6, 0x1FAE5B, LOGIT_GLYPH_BUBBLES, "bubbles", LOGIT_APP_INK_WHITE, 0, 0 },
    { "greeter",   "登录",       "Login",     "Greeter",
      0x7C5CFF, 0x38A8FF, LOGIT_GLYPH_LOGIT_MARK, "logit-mark", LOGIT_APP_INK_WHITE, 0,
      LOGIT_APP_F_BRAND_GRADIENT | LOGIT_APP_F_NO_DOCK },
    { "spotlight", "搜索",       "Spotlight", "Search",
      0x8B8AF5, 0x4B49D0, LOGIT_GLYPH_SEARCH, "search", LOGIT_APP_INK_WHITE, 0, LOGIT_APP_F_NO_DOCK },
};
#define LOGIT_APP_COUNT ((int)(sizeof LOGIT_APPS / sizeof LOGIT_APPS[0]))

/* Finds an app by its stem. A variant suffix starting at the first '-' or
 * '_' is ignored (browser-nofetch, gallery_noaa, monitor_negctl), and so are
 * a leading directory and a trailing ".aex" (/sbin/greeter.aex). Unknown
 * stems return NULL (callers fall back to the window title). */
static inline const struct logit_app *logit_app_lookup(const char *stem)
{
    if (!stem) return NULL;
    const char *s = stem;
    for (const char *p = stem; *p; p++)
        if (*p == '/') s = p + 1;
    size_t n = 0;
    while (s[n] && s[n] != '-' && s[n] != '_' && s[n] != '.') n++;
    if (s[n] == '.') {            /* only ".aex" may follow the stem */
        const char *e = s + n;
        if (!(e[0] == '.' && e[1] == 'a' && e[2] == 'e' && e[3] == 'x' && e[4] == 0)) return NULL;
    }
    if (n == 0) return NULL;
    for (int i = 0; i < LOGIT_APP_COUNT; i++) {
        const char *a = LOGIT_APPS[i].stem;
        size_t k = 0;
        while (k < n && a[k] && a[k] == s[k]) k++;
        if (k == n && a[k] == 0) return &LOGIT_APPS[i];
    }
    return NULL;
}
/* The display name for a stem, or `fallback` when the stem is unknown. */
static inline const char *logit_app_display_name(const char *stem, const char *fallback)
{
    const struct logit_app *a = logit_app_lookup(stem);
    return a ? a->zh : fallback;
}

/* ------------------------------------------------- launch arguments
 *
 * The WM passes one argument string to a launched app (wm_launch(aex, arg)).
 * Two structured forms exist; anything else is today's convention, a file
 * path (Preview, TextEdit, Terminal) or empty:
 *   --pane=<key>          Settings opens on that pane;
 *   --reveal=<abs path>   文件管理 opens the parent folder with the item selected.
 */
#define LOGIT_ARG_PANE   "--pane="
#define LOGIT_ARG_REVEAL "--reveal="

enum logit_pane {
    LOGIT_PANE_APPEARANCE, LOGIT_PANE_DESKTOP, LOGIT_PANE_DOCK, LOGIT_PANE_INPUT,
    LOGIT_PANE_TIME, LOGIT_PANE_NETWORK, LOGIT_PANE_SYSTEM, LOGIT_PANE_ALL,
    LOGIT_PANE_COUNT
};
struct logit_pane_name { const char *key, *zh; };
static const struct logit_pane_name LOGIT_PANES[LOGIT_PANE_COUNT] = {
    [LOGIT_PANE_APPEARANCE] = { "appearance", "外观" },
    [LOGIT_PANE_DESKTOP]    = { "desktop",    "桌面与墙纸" },
    [LOGIT_PANE_DOCK]       = { "dock",       "程序坞" },
    [LOGIT_PANE_INPUT]      = { "input",      "鼠标与滚动" },
    [LOGIT_PANE_TIME]       = { "time",       "日期与时间" },
    [LOGIT_PANE_NETWORK]    = { "network",    "网络" },
    [LOGIT_PANE_SYSTEM]     = { "system",     "系统" },
    [LOGIT_PANE_ALL]        = { "all",        "所有设置" },
};

enum logit_arg_kind {
    LOGIT_ARG_NONE,      /* empty or NULL */
    LOGIT_ARG_KIND_PANE, /* .pane is valid */
    LOGIT_ARG_KIND_REVEAL, /* .path/.path_len name an absolute path */
    LOGIT_ARG_PATH,      /* a bare argument (a file to open): .path/.path_len */
    LOGIT_ARG_INVALID,   /* "--..." that is not a known form, an unknown pane,
                            or a --reveal path that is not absolute */
};
struct logit_app_arg {
    int kind;            /* enum logit_arg_kind */
    int pane;            /* enum logit_pane, -1 unless KIND_PANE */
    const char *path;    /* points into the parsed string, not terminated by us */
    int path_len;
};

static inline int logit__prefix(const char *s, const char *p)
{
    while (*p) if (*s++ != *p++) return 0;
    return 1;
}
static inline int logit__streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static inline int logit__strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static inline int logit_app_arg_parse(const char *arg, struct logit_app_arg *out)
{
    out->kind = LOGIT_ARG_NONE; out->pane = -1; out->path = NULL; out->path_len = 0;
    if (!arg || !*arg) return out->kind;
    if (logit__prefix(arg, LOGIT_ARG_PANE)) {
        const char *k = arg + sizeof LOGIT_ARG_PANE - 1;
        for (int i = 0; i < LOGIT_PANE_COUNT; i++)
            if (logit__streq(k, LOGIT_PANES[i].key)) { out->kind = LOGIT_ARG_KIND_PANE; out->pane = i; return out->kind; }
        return out->kind = LOGIT_ARG_INVALID;
    }
    if (logit__prefix(arg, LOGIT_ARG_REVEAL)) {
        const char *p = arg + sizeof LOGIT_ARG_REVEAL - 1;
        if (p[0] != '/') return out->kind = LOGIT_ARG_INVALID;
        out->path = p; out->path_len = logit__strlen(p);
        return out->kind = LOGIT_ARG_KIND_REVEAL;
    }
    if (arg[0] == '-' && arg[1] == '-') return out->kind = LOGIT_ARG_INVALID;
    out->path = arg; out->path_len = logit__strlen(arg);
    return out->kind = LOGIT_ARG_PATH;
}

/* Writes "--pane=<key>" / "--reveal=<path>" into buf (always terminated when
 * cap > 0). Returns the length, or -1 if it does not fit or is invalid. */
static inline int logit__put(char *buf, int cap, int at, const char *s)
{
    while (*s) {
        if (at + 1 >= cap) return -1;
        buf[at++] = *s++;
    }
    buf[at] = 0;
    return at;
}
static inline int logit_app_arg_pane(char *buf, int cap, int pane)
{
    if (cap <= 0) return -1;
    buf[0] = 0;
    if (pane < 0 || pane >= LOGIT_PANE_COUNT) return -1;
    int n = logit__put(buf, cap, 0, LOGIT_ARG_PANE);
    if (n >= 0) n = logit__put(buf, cap, n, LOGIT_PANES[pane].key);
    if (n < 0) buf[0] = 0;
    return n;
}
static inline int logit_app_arg_reveal(char *buf, int cap, const char *abs_path)
{
    if (cap <= 0) return -1;
    buf[0] = 0;
    if (!abs_path || abs_path[0] != '/') return -1;
    int n = logit__put(buf, cap, 0, LOGIT_ARG_REVEAL);
    if (n >= 0) n = logit__put(buf, cap, n, abs_path);
    if (n < 0) buf[0] = 0;
    return n;
}

/* ------------------------------------------------- window-title rule (5.1)
 *
 * A title is the document name or the Chinese app name, NEVER a path:
 * "未命名", not "/untitled.txt". While dirty it gains " — 已编辑" (drawn in
 * label.secondary). The title string is always set (Exposé, the switcher). */
#define LOGIT_TITLE_UNTITLED      "未命名"
#define LOGIT_TITLE_EDITED_SUFFIX " — 已编辑"
#define LOGIT_TITLE_PATH_SEP      '/'

/* 1 if t may be shown as a window title: non-empty and free of '/'. */
static inline int logit_title_ok(const char *t)
{
    if (!t || !*t) return 0;
    for (; *t; t++) if (*t == LOGIT_TITLE_PATH_SEP) return 0;
    return 1;
}
/* The title for a document path: its last component, or 未命名 for an
 * empty path or one ending in '/'. Returns the length, -1 if cap is short. */
static inline int logit_title_from_path(char *buf, int cap, const char *path)
{
    if (cap <= 0) return -1;
    buf[0] = 0;
    const char *b = path ? path : "";
    for (const char *p = b; *p; p++) if (*p == '/') b = p + 1;
    if (!*b) b = LOGIT_TITLE_UNTITLED;
    int n = logit__put(buf, cap, 0, b);
    if (n < 0) buf[0] = 0;
    return n;
}

#endif /* LOGIT_UI_APPS_H */
