/* Procedural monochrome vector icons (M26). Each icon is a vg_cmd path in a
 * 0..100 unit box (y down); vg_render_path rasterizes it to a coverage bitmap
 * that fb_blit_glyph paints in any color. Filled silhouettes; holes/cutouts use
 * reverse winding (nonzero rule). Rasterized once per (id,px) and cached. */
#include "icons.h"
#include "vg.h"
#include "fb.h"

void *kmalloc(unsigned long);
void  kfree(void *);
static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

#define MV(a,b)               { VG_MOVE,  {(short)(a)},                 {(short)(b)} }
#define LN(a,b)               { VG_LINE,  {(short)(a)},                 {(short)(b)} }
#define QD(cx,cy,a,b)         { VG_QUAD,  {(short)(cx),(short)(a)},     {(short)(cy),(short)(b)} }
#define CB(p,q,r,s,a,b)       { VG_CUBIC, {(short)(p),(short)(r),(short)(a)}, {(short)(q),(short)(s),(short)(b)} }
#define CL                    { VG_CLOSE, {0}, {0} }

/* --- the icons --- */
static const struct vg_cmd ic_folder[] = {       /* filled folder w/ tab */
    MV(9,34), LN(37,34), LN(45,26), LN(88,26), QD(92,26,92,32), LN(92,82),
    QD(92,86,88,86), LN(13,86), QD(9,86,9,82), CL,
};
static const struct vg_cmd ic_doc[] = {          /* page with folded corner */
    MV(26,12), LN(58,12), LN(78,32), LN(78,88), LN(26,88), CL,
};
static const struct vg_cmd ic_terminal[] = {     /* window frame + >_ */
    MV(10,22), LN(90,22), LN(90,78), LN(10,78), CL,                 /* outer */
    MV(16,72), LN(84,72), LN(84,28), LN(16,28), CL,                 /* inner (reverse -> hole) */
    MV(24,38), LN(40,50), LN(24,62), LN(29,62), LN(45,50), LN(29,38), CL,  /* > */
    MV(50,58), LN(70,58), LN(70,63), LN(50,63), CL,                 /* _ */
};
static const struct vg_cmd ic_grid[] = {         /* 2x2 squares */
    MV(14,14), LN(44,14), LN(44,44), LN(14,44), CL,
    MV(56,14), LN(86,14), LN(86,44), LN(56,44), CL,
    MV(14,56), LN(44,56), LN(44,86), LN(14,86), CL,
    MV(56,56), LN(86,56), LN(86,86), LN(56,86), CL,
};
static const struct vg_cmd ic_globe[] = {        /* ring + cross (wireframe globe) */
    MV(92,50), CB(92,73,73,92,50,92), CB(27,92,8,73,8,50), CB(8,27,27,8,50,8), CB(73,8,92,27,92,50), CL, /* outer CW */
    MV(50,15), CB(27,15,15,27,15,50), CB(15,73,27,85,50,85), CB(73,85,85,73,85,50), CB(85,27,73,15,50,15), CL, /* inner (rev) */
    MV(15,48), LN(85,48), LN(85,52), LN(15,52), CL,                 /* equator */
    MV(48,15), LN(52,15), LN(52,85), LN(48,85), CL,                 /* meridian */
};
static const struct vg_cmd ic_code[] = {         /* < > chevrons */
    MV(34,28), LN(40,33), LN(24,50), LN(40,67), LN(34,72), LN(13,50), CL,   /* < */
    MV(66,28), LN(60,33), LN(76,50), LN(60,67), LN(66,72), LN(87,50), CL,   /* > */
};
static const struct vg_cmd ic_chart[] = {        /* bar chart */
    MV(18,82), LN(34,82), LN(34,56), LN(18,56), CL,
    MV(42,82), LN(58,82), LN(58,34), LN(42,34), CL,
    MV(66,82), LN(82,82), LN(82,48), LN(66,48), CL,
    MV(12,84), LN(88,84), LN(88,89), LN(12,89), CL,                 /* baseline */
};
static const struct vg_cmd ic_clock[] = {        /* ring + 2 hands */
    MV(92,50), CB(92,73,73,92,50,92), CB(27,92,8,73,8,50), CB(8,27,27,8,50,8), CB(73,8,92,27,92,50), CL,
    MV(50,16), CB(27,16,16,27,16,50), CB(16,73,27,84,50,84), CB(73,84,84,73,84,50), CB(84,27,73,16,50,16), CL,
    MV(47,50), LN(47,28), LN(53,28), LN(53,50), CL,                 /* hour hand up */
    MV(50,47), LN(72,47), LN(72,53), LN(50,53), CL,                 /* minute hand right */
};
static const struct vg_cmd ic_image[] = {        /* frame + sun + mountain */
    MV(12,20), LN(88,20), LN(88,80), LN(12,80), CL,                 /* outer */
    MV(18,74), LN(82,74), LN(82,26), LN(18,26), CL,                 /* inner (hole) */
    MV(38,40), CB(38,46,33,50,28,50), CB(23,50,18,46,18,40), CB(18,34,23,30,28,30), CB(33,30,38,34,38,40), CL, /* sun */
    MV(18,74), LN(44,48), LN(60,64), LN(72,52), LN(82,74), CL,      /* mountain */
};

static const struct { const struct vg_cmd *c; int n; } TBL[ICON_COUNT] = {
    [ICON_FOLDER]   = { ic_folder,   sizeof ic_folder   / sizeof(struct vg_cmd) },
    [ICON_DOC]      = { ic_doc,      sizeof ic_doc      / sizeof(struct vg_cmd) },
    [ICON_TERMINAL] = { ic_terminal, sizeof ic_terminal / sizeof(struct vg_cmd) },
    [ICON_GRID]     = { ic_grid,     sizeof ic_grid     / sizeof(struct vg_cmd) },
    [ICON_GLOBE]    = { ic_globe,    sizeof ic_globe    / sizeof(struct vg_cmd) },
    [ICON_CODE]     = { ic_code,     sizeof ic_code     / sizeof(struct vg_cmd) },
    [ICON_CHART]    = { ic_chart,    sizeof ic_chart    / sizeof(struct vg_cmd) },
    [ICON_CLOCK]    = { ic_clock,    sizeof ic_clock    / sizeof(struct vg_cmd) },
    [ICON_IMAGE]    = { ic_image,    sizeof ic_image    / sizeof(struct vg_cmd) },
};

static struct { int px, w, h; uint8_t *cov; } cache[ICON_COUNT];

void icon_draw(int id, int x, int y, int px, uint32_t color)
{
    if (id < 0 || id >= ICON_COUNT || !TBL[id].c) return;
    if (cache[id].px != px || !cache[id].cov) {
        if (cache[id].cov) { kfree(cache[id].cov); cache[id].cov = 0; }
        uint8_t *cov = kmalloc((unsigned long)px * px);
        if (!cov) return;
        int w, h;
        if (vg_render_path(TBL[id].c, TBL[id].n, 100, px, cov, px * px, &w, &h) != 0) { kfree(cov); return; }
        cache[id].px = px; cache[id].w = w; cache[id].h = h; cache[id].cov = cov;
    }
    fb_blit_glyph(x, y, cache[id].cov, cache[id].w, cache[id].h, color);
}

int icon_for_app(const char *name, const char *ext)
{
    (void)ext;
    if (seq(name, "files.aex"))    return ICON_FOLDER;
    if (seq(name, "terminal.aex")) return ICON_TERMINAL;
    if (seq(name, "clock.aex"))    return ICON_CLOCK;
    if (seq(name, "textedit.aex")) return ICON_DOC;
    if (seq(name, "monitor.aex"))  return ICON_CHART;
    if (seq(name, "widgets.aex"))  return ICON_GRID;
    if (seq(name, "preview.aex"))  return ICON_IMAGE;
    if (seq(name, "studio.aex"))   return ICON_CODE;
    if (seq(name, "browser.aex"))  return ICON_GLOBE;
    return -1;
}
