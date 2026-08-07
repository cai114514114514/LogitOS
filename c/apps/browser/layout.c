#include "layout.h"
#include "css.h"

void *kmalloc(unsigned long);
void  kfree(void *);
void *memset(void *, int, unsigned long);
int   text_measure(const char *s, int len, int px, int mono);
int   res_fetch(const char *url, uint8_t **buf, int *len);   /* net/http.c */

#define MAXITEM 16384
static struct item *items;
static int nitem;
static int doc_h;
static int canvas;
static uint32_t page_bg; static int page_has_bg;   /* html/body bg -> viewport fill */

/* Stacking level for everything emitted right now: the z-index of the nearest
 * enclosing positioned box (or flex item) that set one. layout_page sorts the
 * finished list by it. */
static int g_z;

/* Clip rectangle in force for everything emitted right now: the padding box of
 * the nearest ancestor whose overflow is not `visible`, intersected with any
 * outer clip. Document coordinates. Stamped onto each item exactly like g_z --
 * the painter has only the flat list, so the box that owns the clip cannot be
 * found again at paint time. */
static int g_clip_on, g_clipx, g_clipy, g_clipw, g_cliph;

/* ---- float exclusions ----
 *
 * A float is taken out of normal flow and placed against one content edge of
 * its block formatting context; every LINE BOX whose vertical band overlaps it
 * is narrowed by it, while block boxes keep their full width and simply paint
 * underneath (that asymmetry is real CSS, not a shortcut).
 *
 * The exclusions live in one flat array in DOCUMENT coordinates rather than in
 * a per-block structure, because a float declared in one block goes on
 * narrowing the lines of its *later siblings* until something clears it -- the
 * list is scoped by the block formatting context, not by the box that made it.
 * bfc_enter/bfc_leave are the scope: they truncate the array back, which is
 * also what makes a BFC root "contain" its floats.
 *
 * MAXFLOAT is a hard cap; past it a float still lays out and paints, it just
 * stops excluding. A page with 64 live floats in one BFC has already lost. */
#define MAXFLOAT 64
struct fbox { int x0, x1, y0, y1; unsigned char side; };
static struct fbox g_float[MAXFLOAT];
static int g_nfloat;
static int g_fbase;      /* first float index the current BFC can see */
static int g_in_float;   /* nonzero while emitting a floated box's own items */

static struct item *additem(int type, struct node *n)
{
    if (!items || nitem >= MAXITEM) return 0;
    struct item *it = &items[nitem++];
    memset(it, 0, sizeof *it);
    it->type = type;
    it->node = n;                       /* provenance: painted box -> DOM node */
    it->z = g_z;
    it->opacity = 255;
    it->has_clip = (unsigned char)g_clip_on;
    it->clip_x = g_clipx; it->clip_y = g_clipy;
    it->clip_w = g_clipw; it->clip_h = g_cliph;
    it->is_float = (unsigned char)g_in_float;
    return it;
}

/* The horizontal segment left for a line box occupying [y, y+h) inside the
 * content edges [lo0,hi0). A left float pushes the start right, a right float
 * pulls the end left; a band squeezed to nothing comes back empty (hi == lo)
 * and the caller drops to the next float bottom. */
static void float_band(int y, int h, int lo0, int hi0, int *lo, int *hi)
{
    *lo = lo0; *hi = hi0;
    if (h < 1) h = 1;
    for (int i = g_fbase; i < g_nfloat; i++) {
        const struct fbox *f = &g_float[i];
        if (f->y1 <= y || f->y0 >= y + h) continue;      /* no vertical overlap */
        if (f->side == FLT_LEFT) { if (f->x1 > *lo) *lo = f->x1; }
        else                     { if (f->x0 < *hi) *hi = f->x0; }
    }
    if (*hi < *lo) *hi = *lo;
}

/* The next y below `y` at which the set of overlapping floats changes, i.e. the
 * lowest float bottom strictly greater than y among floats that straddle y.
 * Returns y itself when nothing overlaps, which is the caller's loop guard. */
static int float_next_bottom(int y)
{
    int best = 0, any = 0;
    for (int i = g_fbase; i < g_nfloat; i++) {
        const struct fbox *f = &g_float[i];
        if (f->y1 <= y || f->y0 > y) continue;
        if (!any || f->y1 < best) { best = f->y1; any = 1; }
    }
    return any ? best : y;
}

/* `clear`: the first y at or below `y` that is past every float on the named
 * side(s). CLR_BOTH is the union, which is why the sides are tested with a
 * mask rather than an equality. */
static int float_clear_y(int which, int y)
{
    for (int i = g_fbase; i < g_nfloat; i++) {
        const struct fbox *f = &g_float[i];
        int want = (f->side == FLT_LEFT) ? CLR_LEFT : CLR_RIGHT;
        if (which != CLR_BOTH && which != want) continue;
        if (f->y1 > y) y = f->y1;
    }
    return y;
}

/* Lowest bottom edge among the floats added since `from`. A block formatting
 * context grows to contain its own floats -- this is the number that makes
 * `overflow:hidden` work as the classic clearfix. */
static int float_max_bottom(int from)
{
    int b = 0;
    for (int i = from; i < g_nfloat; i++) if (g_float[i].y1 > b) b = g_float[i].y1;
    return b;
}

static void float_add(int x0, int x1, int y0, int y1, int side)
{
    if (g_nfloat >= MAXFLOAT || x1 <= x0 || y1 <= y0) return;
    struct fbox *f = &g_float[g_nfloat++];
    f->x0 = x0; f->x1 = x1; f->y0 = y0; f->y1 = y1; f->side = (unsigned char)side;
}

/* Translate a display-list range. Used wherever a box's final position is only
 * known after its contents have been laid out: flex cross-axis alignment,
 * justify-content, position:relative. */
static void shift_items(int lo, int hi, int dx, int dy)
{
    if (!dx && !dy) return;
    if (hi > nitem) hi = nitem;
    for (int i = lo; i < hi; i++) { items[i].x += dx; items[i].y += dy; }
}

static int any_border(const struct cstyle *st)
{ return st->border_w[0] || st->border_w[1] || st->border_w[2] || st->border_w[3]; }

/* ---- box model ----
 * Every width/height below is a BORDER-BOX size: the rectangle the background
 * and borders are painted into, and the one CSS `box-sizing:border-box` names
 * directly. Content boxes are derived from it by subtracting borders+padding.
 * Before this the two were conflated -- `width` was treated as the padding box
 * and border widths took no space at all -- so a border-box site laid out
 * padding-sized and a content-box site lost its padding. */
static int hextra(const struct cstyle *st)
{ return st ? st->pl + st->pr + st->border_w[3] + st->border_w[1] : 0; }
static int vextra(const struct cstyle *st)
{ return st ? st->pt + st->pb + st->border_w[0] + st->border_w[2] : 0; }
/* Content-box origin offsets from the border-box origin. */
static int cx_off(const struct cstyle *st) { return st ? st->border_w[3] + st->pl : 0; }
static int cy_off(const struct cstyle *st) { return st ? st->border_w[0] + st->pt : 0; }

/* An authored width/height turned into a border-box size. */
static int to_border_w(const struct cstyle *st, int v)
{ return (st && st->box_sizing == BOX_BORDER) ? v : v + hextra(st); }
static int to_border_h(const struct cstyle *st, int v)
{ return (st && st->box_sizing == BOX_BORDER) ? v : v + vextra(st); }

/* A css length that is either px, or a percentage of `avail` plus a px addend
 * (the calc(100% - 20px) shape css_engine folds into pct+off). */
static int resolve_len(int v, int pct, int off, int avail)
{ return pct ? avail * v / 100 + off : v; }

/* Clamp a border-box width by min-width/max-width. Both are authored under the
 * element's own box-sizing, so they go through the same conversion. */
static int clamp_w(const struct cstyle *st, int w, int avail)
{
    if (st) {
        if (st->has_max_w) {
            int m = to_border_w(st, resolve_len(st->max_w, st->max_w_pct, 0, avail));
            if (w > m) w = m;
        }
        if (st->has_min_w) {
            int m = to_border_w(st, resolve_len(st->min_w, st->min_w_pct, 0, avail));
            if (w < m) w = m;
        }
    }
    return w < 0 ? 0 : w;
}

/* The specified border-box height, or -1 for auto. `avail` is the containing
 * block's content height, or -1 when that is itself auto -- in which case a
 * percentage height is undefined and we fall back to auto, as before. */
static int spec_h(const struct cstyle *st, int avail)
{
    if (!st || !st->has_h) return -1;
    if (st->h_pct && avail < 0) return -1;
    int h = to_border_h(st, resolve_len(st->height, st->h_pct, st->h_off, avail));
    /* max-height clamps the SPECIFIED height only. Clamping a content-derived
     * height would need the painter to clip, which it cannot; an overflowing
     * box growing past its max-height is far less wrong than one that overlaps
     * whatever follows it. */
    if (st->has_max_h && !(st->max_h_pct && avail < 0)) {
        int m = to_border_h(st, resolve_len(st->max_h, st->max_h_pct, 0, avail));
        if (h > m) h = m;
    }
    return h;
}

/* Final border-box height of a block whose content produced `ch`. */
static int block_height(const struct cstyle *st, int ch, int avail)
{
    int h = spec_h(st, avail);
    if (h > ch) ch = h;
    if (st && st->has_min_h && !(st->min_h_pct && avail < 0)) {
        int m = to_border_h(st, resolve_len(st->min_h, st->min_h_pct, 0, avail));
        if (ch < m) ch = m;
    }
    return ch;
}

/* Used border-box width of an in-flow block child inside a containing block of
 * content width `avail`. `auto` fills the line minus its own margins. */
static int block_width(const struct cstyle *st, int avail)
{
    if (!st) return avail;
    int ml = st->ml < 0 ? 0 : st->ml, mr = st->mr < 0 ? 0 : st->mr;
    int w = st->has_w ? to_border_w(st, resolve_len(st->width, st->w_pct, st->w_off, avail))
                      : avail - ml - mr;
    return clamp_w(st, w, avail);
}

/* Out-of-layout nodes: display:none, or position:absolute/fixed (we don't do
 * positioned overlays -- they'd smear hidden menus over the normal flow). */
static int skipped(struct node *n)
{ struct cstyle *st = n->style; return st && (st->display == DISP_NONE || st->pos_abs); }

/* Fill an IT_RECT (bg + per-edge borders + radius) at x/y/w; h set by caller. */
static void fill_rect_item(struct item *bg, const struct cstyle *st, int x, int y, int w)
{
    bg->x = x; bg->y = y; bg->w = w;
    bg->bg = st->background; bg->has_bg = st->has_bg; bg->bg_alpha = st->bg_alpha;
    for (int i = 0; i < 4; i++) {
        bg->border_w[i] = st->border_w[i]; bg->border_color[i] = st->border_color[i];
        bg->border_style[i] = st->border_style[i];
    }
    bg->radius = st->radius; bg->radius_pct = st->radius_pct;
    bg->hidden = st->hidden;
    bg->opacity = st->opacity;
}

static int sp(int c){ return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'; }
static int tag_eq(const char *t, const char *lit){ int i=0; for(;lit[i];i++) if(t[i]!=lit[i]) return 0; return t[i]==0; }
static int atoi_(const char *s){ int n=0; while(*s>='0'&&*s<='9'){ if(n>100000) break; n=n*10+(*s++-'0'); } return n; }

/* Extract the viewBox width/height from a raw svg source span (the spelling
 * must match what svg.c's parser accepts: exact case "viewBox"). Returns 1
 * and sets *ow/*oh on success. Needed because svg.c's decoder falls back to
 * the raw viewBox dims instead of the aspect-preserving size when only one
 * of width/height is given, so the decoded raster can't be used for the
 * missing-axis aspect derivation. */
static int raw_viewbox_wh(const char *s, int n, int *ow, int *oh)
{
    static const char vb[] = "viewBox";
    int i = 0;
    while (i + 7 <= n) {
        int j = 0;
        while (j < 7 && s[i + j] == vb[j]) j++;
        if (j == 7) break;
        i++;
    }
    if (i + 7 > n) return 0;
    i += 7;
    while (i < n && s[i] != '"' && s[i] != '\'') i++;
    if (i >= n) return 0;
    i++;
    double v[4]; int nv = 0;
    while (i < n && s[i] != '"' && s[i] != '\'' && nv < 4) {
        char c = s[i];
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.') {
            int neg = 0, any = 0;
            if (c == '-') { neg = 1; i++; } else if (c == '+') i++;
            double val = 0;
            while (i < n && s[i] >= '0' && s[i] <= '9') { val = val * 10 + (s[i] - '0'); i++; any = 1; }
            if (i < n && s[i] == '.') {
                i++; double f = 0.1;
                while (i < n && s[i] >= '0' && s[i] <= '9') { val += (s[i] - '0') * f; f *= 0.1; i++; any = 1; }
            }
            if (any) v[nv++] = neg ? -val : val;
        } else i++;
    }
    if (nv == 4 && v[2] > 0 && v[3] > 0) { *ow = (int)(v[2] + 0.5); *oh = (int)(v[3] + 0.5); return 1; }
    return 0;
}

/* Measurement-only width of an inline <svg> (no decode): CSS > attr > 16px. */
static int svg_attr_w(struct node *n, const struct cstyle *st)
{
    if (st && st->has_w && !st->w_pct) return st->width;
    const char *wa = dom_attr(n, "width");
    int iw = wa ? atoi_(wa) : 0;
    return iw > 0 ? iw : 16;
}

/* ---- inline flow ---- */
/* current pen for inline content within a block. `align` is the block's
 * text-align; `line_start` is the display-list index of the first item on the
 * current line, so newline() can shift the whole line for center/right.
 *
 * x0/x1 are the CURRENT LINE's edges, which is not the same thing as the
 * block's: bx0/bx1 are the block's content edges and x0/x1 are what is left of
 * them after the floats overlapping this line's band are subtracted. Every
 * existing user of x0/x1 (wrapping, tab stops, text-align, the replaced-element
 * fitting) therefore became float-aware for free; only the two places that mean
 * "the block, not this line" had to switch to bx0/bx1.
 *
 * `probe` is the line height used to query the float band before the line's
 * real height is known. Line height is content-driven and the band is
 * height-driven, so something has to break the circularity; real engines lay
 * the line out twice, we probe with the block's own line height. It is exact
 * unless a line mixes font sizes right at a float's top or bottom edge. */
struct iflow { int x0, x1, x, y, lineh, line_started, align, line_start;
               int bx0, bx1, probe; };

/* Re-derive the current line's edges from the float list, dropping past any
 * float that leaves no room at all. Called whenever the pen moves to a new y. */
static void flow_relayout_line(struct iflow *f)
{
    for (int guard = 0; guard < MAXFLOAT + 1; guard++) {
        float_band(f->y, f->probe, f->bx0, f->bx1, &f->x0, &f->x1);
        if (f->x1 > f->x0) break;
        int nb = float_next_bottom(f->y);
        if (nb <= f->y) break;                        /* nothing left to clear */
        f->y = nb;
    }
    f->x = f->x0;
}

static void iflow_init(struct iflow *f, int x, int w, int y, int align, int probe)
{
    f->bx0 = x; f->bx1 = x + w;
    f->y = y; f->lineh = 0; f->line_started = 0; f->align = align;
    f->line_start = nitem;
    f->probe = probe > 0 ? probe : 20;
    flow_relayout_line(f);
}

/* The block's own line height, used as the float-band probe. */
static int style_lineh(const struct cstyle *st)
{
    if (!st) return 20;
    int px = st->font_px > 0 ? st->font_px : 16;
    return st->line_px > px ? st->line_px : px * 5 / 4;
}

/* Close the current line. `last` marks a line that ends the block (or is cut
 * short by <br> or a block-level sibling): CSS does not justify those, and
 * stretching a two-word final line to the full measure is the classic
 * give-away of a broken justify implementation. */
static void newline2(struct iflow *f, int last)
{
    if (f->line_started) {
        int n = nitem - f->line_start;
        if (f->align == ALIGN_JUSTIFY && !last && n > 1) {
            /* Spread the slack between the words: item k of n moves right by
             * k/(n-1) of it. Only text items take part -- an inline image on
             * the line rides along with the word it follows. */
            int extra = (f->x1 - f->x0) - (f->x - f->x0);
            if (extra > 0)
                for (int i = 1; i < n; i++) {
                    /* A float that landed mid-line sits in this range but is
                     * out of flow: it must not be spread with the words (its
                     * position is what the line was measured against). It still
                     * counts in `n`, so the spacing either side of it is a
                     * little uneven -- a float inside a justified line is rare
                     * enough not to warrant a second index pass. */
                    if (items[f->line_start + i].is_float) continue;
                    items[f->line_start + i].x += (int)((long)extra * i / (n - 1));
                }
        } else if (f->align == ALIGN_CENTER || f->align == ALIGN_RIGHT) {
            int used = f->x - f->x0, avail = f->x1 - f->x0;
            int off = (f->align == ALIGN_CENTER) ? (avail - used) / 2 : (avail - used);
            if (off > 0)
                for (int i = f->line_start; i < nitem; i++)
                    if (!items[i].is_float) items[i].x += off;
        }
        f->y += f->lineh;
    }
    f->lineh = 0; f->line_started = 0;
    f->line_start = nitem;
    flow_relayout_line(f);              /* the new line sees a different band */
}

/* A soft break inside a paragraph: the line just closed is justifiable. */
static void newline(struct iflow *f) { newline2(f, 0); }

/* emit one text item at the current pen position and advance the pen */
static void emit_word(struct iflow *f, struct node *src, const char *s, int len, int w,
                      struct cstyle *st, const char *href, int lh, int px, int mono)
{
    struct item *it = additem(IT_TEXT, src);
    if (!it) return;
    it->x = f->x; it->w = w; it->text = s; it->len = len;
    it->font_px = px; it->bold = st->bold; it->italic = st->italic; it->mono = mono;
    it->underline = st->underline; it->strike = st->strike; it->overline = st->overline;
    it->color = st->color; it->href = href;
    it->hidden = st->hidden; it->opacity = st->opacity;
    f->x += w;
    f->line_started = 1;
    if (lh > f->lineh) f->lineh = lh;
    it->y = f->y;                                     /* top of line; baseline handled in paint */
    it->h = lh;
}

/* Force a line break even on an empty line -- a blank line inside <pre> still
 * takes a line box, and newline2 only advances y for a started line. */
static void hard_break(struct iflow *f, int lh)
{
    if (!f->line_started) { f->lineh = lh; f->line_started = 1; }
    newline2(f, 1);
}

/* A word that does not fit the current line but WOULD fit the block's full
 * measure is not a break-anywhere case -- a float is in the way. Drop the
 * (still empty) line past float bottoms until it fits or the floats run out.
 * Only ever called on an unstarted line, so there is nothing to move. */
static void flow_clear_for(struct iflow *f, int need)
{
    if (f->line_started || need <= f->x1 - f->x0) return;
    if (need > f->bx1 - f->bx0) return;               /* genuinely wider than the block */
    for (int guard = 0; guard < MAXFLOAT + 1; guard++) {
        int nb = float_next_bottom(f->y);
        if (nb <= f->y) return;
        f->y = nb;
        flow_relayout_line(f);
        if (need <= f->x1 - f->x0) return;
    }
}

/* place a text run (one element's text) into the flow.
 *
 * white-space decides three independent things and this is the only place any
 * of them matter:
 *   collapse   runs of spaces/tabs/newlines fold into one inter-word space
 *   keep_nl    a literal '\n' forces a line break
 *   can_wrap   a full line may break at a space
 * normal = collapse+wrap, pre = neither, nowrap = collapse only,
 * pre-wrap = wrap+newlines, pre-line = collapse+wrap+newlines.
 *
 * Tab stops are honoured wherever spaces are preserved: a tab advances the pen
 * to the next multiple of 8 space widths from the line's left edge, which is
 * what makes indented code in a <pre> line up. Measuring '\t' with the font
 * instead would draw a missing-glyph box. */
static void flow_text(struct iflow *f, struct node *src, const char *s, int len,
                      struct cstyle *st, const char *href)
{
    int px = st->font_px, mono = st->mono;
    int lh = st->line_px > px ? st->line_px : px*5/4;
    int spacew = text_measure(" ", 1, px, mono);
    int ws_mode = st->white_space;
    int collapse = (ws_mode == WS_NORMAL || ws_mode == WS_NOWRAP || ws_mode == WS_PRE_LINE);
    int keep_nl  = (ws_mode != WS_NORMAL && ws_mode != WS_NOWRAP);
    int can_wrap = (ws_mode != WS_PRE && ws_mode != WS_NOWRAP);
    int tabw = spacew * 8; if (tabw <= 0) tabw = 1;
    int i = 0;

    if (!collapse) {
        /* Preserved spaces: emit each run of literal characters verbatim, so
         * the indentation and the internal spacing survive into the item. */
        while (i < len) {
            if (s[i] == '\n') { hard_break(f, lh); i++; continue; }
            if (s[i] == '\r') { i++; continue; }
            if (s[i] == '\t') {
                int col = f->x - f->x0;
                f->x = f->x0 + (col / tabw + 1) * tabw;
                f->line_started = 1;
                if (lh > f->lineh) f->lineh = lh;
                i++; continue;
            }
            int seg = i;
            while (i < len && s[i] != '\n' && s[i] != '\r' && s[i] != '\t') i++;
            int slen = i - seg;
            if (!can_wrap) {                       /* pre: one item, may overflow */
                int w = text_measure(s + seg, slen, px, mono);
                emit_word(f, src, s + seg, slen, w, st, href, lh, px, mono);
                continue;
            }
            /* pre-wrap: break at spaces but keep them. Each token is
             * [run of spaces][run of non-spaces], emitted as one item so the
             * leading spaces are drawn. A token that will not fit starts a new
             * line, and its leading spaces are dropped there -- CSS "hangs"
             * them off the end of the previous line instead, which paints the
             * same. */
            int p = seg;
            while (p < i) {
                int t0 = p;
                while (p < i && s[p] == ' ') p++;
                int nsp = p - t0;
                while (p < i && s[p] != ' ') p++;
                int tlen = p - t0, tw = text_measure(s + t0, tlen, px, mono);
                if (f->line_started && f->x + tw > f->x1 &&
                    (tw <= f->x1 - f->x0 || tw <= f->bx1 - f->bx0)) {
                    newline(f);
                    t0 += nsp; tlen -= nsp;
                    if (tlen <= 0) continue;
                    tw = text_measure(s + t0, tlen, px, mono);
                }
                flow_clear_for(f, tw);          /* narrowed by a float, not by the measure */
                emit_word(f, src, s + t0, tlen, tw, st, href, lh, px, mono);
            }
        }
        return;
    }

    while (i < len) {
        if (keep_nl) {                                   /* pre-line */
            while (i < len && sp(s[i]) && s[i] != '\n') i++;
            if (i < len && s[i] == '\n') { hard_break(f, lh); i++; continue; }
        } else {
            while (i < len && sp(s[i])) i++;             /* collapse whitespace */
        }
        if (i >= len) break;
        int ws = i;
        while (i < len && !sp(s[i])) i++;                /* one word [ws,i) */
        int wlen = i - ws;
        if (!wlen) continue;
        int ww = text_measure(s + ws, wlen, px, mono);
        /* The second clause used to be `ww <= x1 - x0` alone: a word too wide
         * for a whole line is broken from where the pen stands rather than
         * pointlessly wrapped first. With floats the line can be narrower than
         * the block, so a word that the BLOCK could hold is still worth
         * wrapping -- and then dropping past the float. */
        if (can_wrap && f->line_started && f->x + spacew + ww > f->x1 &&
            (ww <= f->x1 - f->x0 || ww <= f->bx1 - f->bx0)) {
            newline(f);                                   /* wrap */
        }
        if (can_wrap) flow_clear_for(f, ww);
        if (!can_wrap || ww <= f->x1 - f->x0) {
            if (f->line_started) f->x += spacew;
            emit_word(f, src, s + ws, wlen, ww, st, href, lh, px, mono);
            continue;
        }
        /* A word wider than the whole line (CJK titles have no spaces to wrap
         * on): break it anywhere. This is also what makes flex items honor
         * their allocated width -- min-width:auto is compressible for text,
         * so an over-long word must shrink-wrap instead of overflowing. */
        int off = 0;
        while (off < wlen) {
            int avail = f->x1 - (f->line_started ? f->x + spacew : f->x);
            if (avail <= 0) {
                if (f->line_started) { newline(f); continue; }
                avail = f->x1 - f->x0;              /* degenerate 0-width box: force progress */
            }
            /* largest prefix (UTF-8 char boundaries) that fits avail; >=1 char */
            int bl = 0, bw = 0;
            for (int p = 0; p < wlen - off; ) {
                int adv = 1;
                while (p + adv < wlen - off && (s[ws + off + p + adv] & 0xC0) == 0x80) adv++;
                int mw = text_measure(s + ws + off, p + adv, px, mono);
                if (mw > avail && bl > 0) break;
                p += adv; bl = p; bw = mw;
                if (bw >= avail) break;
            }
            if (f->line_started) f->x += spacew;
            emit_word(f, src, s + ws + off, bl, bw, st, href, lh, px, mono);
            off += bl;
            if (off < wlen) newline(f);
        }
    }
}

static void flow_node(struct iflow *f, struct node *c, const char *href);

/* Nonzero while laying out an absolutely-positioned overlay's subtree: flow_node
 * must not drop children merely because their ancestor is position:absolute
 * (bilibili's cover <img> lives inside a pos_abs <picture> overlay). */
static int g_in_overlay;

/* flow all inline descendants of `n` (text + inline elements + img) */
static void flow_children(struct iflow *f, struct node *n, const char *href)
{
    for (struct node *c = n->first_child; c; c = c->next)
        flow_node(f, c, href);
}

/* flow a single node into the inline context */
static int layout_block(struct node *n, int x, int y, int w);   /* fwd */
static int is_block(struct node *n);                            /* fwd */
static void place_float(struct node *c, struct cstyle *st, int bx0, int bx1, int y); /* fwd */
static void flow_node(struct iflow *f, struct node *c, const char *href)
{
    struct cstyle *st = c->style;
    if (skipped(c)) return;
    if (c->parent && skipped(c->parent)) {
        struct cstyle *ps = c->parent->style;
        /* inside an overlay the ancestor's pos_abs is expected, not a reason to
         * drop the child -- display:none still prunes. */
        if (!(g_in_overlay && ps && ps->pos_abs && ps->display != DISP_NONE)) return;
    }

    if (c->type == N_TEXT) {
        struct cstyle *ps = c->parent && c->parent->style ? c->parent->style : st;
        if (ps) flow_text(f, c, c->text, c->textlen, ps, href);
        return;
    }
    if (c->type != N_ELEM) return;

    const char *h2 = href;
    if (tag_eq(c->tag, "a")) { const char *u = dom_attr(c, "href"); if (u) h2 = u; }

    /* Inline <svg>: a replaced element like <img>. Decode straight from the
     * verbatim source span the DOM parser recorded (keeps the viewBox case and
     * path data longer than the 255-char attr cap intact), emit IT_IMAGE.
     * Box size priority: CSS > width/height attrs > viewBox > 16px default.
     * Placed before the is_block check: CSS display:block/inline-block on an
     * svg (GitHub's .octicon is inline-block) must not route it to the empty
     * block-box path. */
    if (tag_eq(c->tag, "svg")) {
        int iw = 0, ih = 0;
        if (st && st->has_w && !st->w_pct) iw = st->width;
        if (st && st->has_h && !st->h_pct) ih = st->height;
        if (!iw) { const char *wa = dom_attr(c, "width");  if (wa) iw = atoi_(wa); }
        if (!ih) { const char *ha = dom_attr(c, "height"); if (ha) ih = atoi_(ha); }
        struct image tmp, *holder = 0;
        if (c->raw && img_decode((const uint8_t *)c->raw, c->rawlen, &tmp) == 0) {
            holder = kmalloc(sizeof *holder);
            if (holder) *holder = tmp;
            else img_free(&tmp);                      /* decoded but nowhere to keep it */
        }
        if (!holder) return;                          /* undecodable: skip silently */
        int vbw = 0, vbh = 0;
        if (raw_viewbox_wh(c->raw, c->rawlen, &vbw, &vbh)) {
            if (!iw && !ih) { iw = vbw; ih = vbh; }
            else if (!iw) iw = ih * vbw / vbh;
            else if (!ih) ih = iw * vbh / vbw;
        }
        if (iw <= 0) iw = ih > 0 ? ih : 16;
        if (ih <= 0) ih = iw;
        if (iw > f->x1 - f->x0) { int s2 = f->x1 - f->x0; ih = ih*s2/iw; iw = s2; }
        if (f->line_started && f->x + iw > f->x1) newline(f);
        struct item *it = additem(IT_IMAGE, c);
        if (it) { it->x = f->x; it->y = f->y; it->w = iw; it->h = ih;
                  it->img = holder; it->imgsrc = 0; it->href = h2;
                  it->hidden = st ? st->hidden : 0; }
        else { img_free(holder); kfree(holder); }     /* display list full */
        f->x += iw; f->line_started = 1; if (ih > f->lineh) f->lineh = ih;
        return;
    }

    /* A block box inside an inline context breaks the inline flow (CSS splits
     * the surrounding inline into anonymous block boxes). Custom elements
     * (<react-partial>, <turbo-frame>, ...) default to display:inline, so
     * without this their whole block subtree would be flattened into one
     * smeared inline run. */
    /* A float inside an inline context does NOT break the line: it is taken out
     * of flow at the current line's top and the words keep coming, now against
     * a narrower band. Placed before the is_block test because float blockifies
     * -- <span style="float:left"> arrives here as a block. */
    if (st && st->flt != FLT_NONE && !st->pos_abs && c->type == N_ELEM &&
        !tag_eq(c->tag, "svg")) {
        /* Words already on this line were measured against the old band, so a
         * mid-line float cannot narrow it retroactively: it is placed at the
         * NEXT line's top instead. CSS would keep it on this line when it still
         * fits beside the placed words; starting one line lower is the
         * conservative version of that rule and can never overlap text. */
        place_float(c, st, f->bx0, f->bx1, f->line_started ? f->y + f->lineh : f->y);
        if (!f->line_started) { flow_relayout_line(f); f->line_start = nitem; }
        return;
    }

    if (is_block(c)) {
        newline2(f, 1);                  /* the line it interrupts is a last line */
        /* A block box is measured against the BLOCK, not against the line: CSS
         * does not narrow block boxes by floats, only their line boxes. */
        int avail = f->bx1 - f->bx0;
        int bx = f->bx0 + (st && st->ml > 0 ? st->ml : 0);
        int bw = block_width(st, avail);
        int bgidx = -1;
        if (st && (st->has_bg || any_border(st))) {
            struct item *bg = additem(IT_RECT, c);
            if (bg) { bgidx = (int)(bg - items); fill_rect_item(bg, st, bx, f->y, bw); }
        }
        int inner = layout_block(c, bx + cx_off(st), f->y + cy_off(st), bw - hextra(st));
        int ch = (inner - f->y) + (st ? st->pb + st->border_w[2] : 0);
        ch = block_height(st, ch, -1);
        if (st && ch < st->font_px) ch = st->font_px;
        if (bgidx >= 0) items[bgidx].h = ch;
        f->y += ch;
        f->lineh = 0; f->line_started = 0; f->line_start = nitem;
        flow_relayout_line(f);
        return;
    }

    if (tag_eq(c->tag, "br")) { newline2(f, 1); return; }

    if (tag_eq(c->tag, "img")) {
        /* Reserve the box from CSS/HTML width&height; the actual pixels are
         * fetched later by layout_load_images() so layout never blocks. */
        int iw = 0, ih = 0;
        if (st && st->has_w && !st->w_pct) iw = st->width;
        if (st && st->has_h && !st->h_pct) ih = st->height;
        if (!iw) { const char *wa = dom_attr(c, "width");  if (wa) iw = atoi_(wa); }
        if (!ih) { const char *ha = dom_attr(c, "height"); if (ha) ih = atoi_(ha); }
        if (iw <= 0 && c->parent && c->parent->style &&
            ((struct cstyle *)c->parent->style)->pos_abs)
            iw = f->x1 - f->x0;              /* unsized <img> in an absolute overlay:
                                              * fill it (bilibili's cover <picture>) */
        int h_auto = 0;
        if (iw > 0 && ih <= 0) h_auto = 1;   /* height follows the decoded aspect */
        if (iw <= 0) iw = ih > 0 ? ih : 24;
        if (ih <= 0) ih = iw;
        if (iw > f->x1 - f->x0) { int s2 = f->x1 - f->x0; ih = ih*s2/iw; iw = s2; }
        if (f->line_started && f->x + iw > f->x1) newline(f);
        struct item *it = additem(IT_IMAGE, c);
        if (it) { it->x = f->x; it->y = f->y; it->w = iw; it->h = ih;
                  it->img = 0; it->imgsrc = dom_attr(c, "src"); it->href = h2;
                  it->h_auto = h_auto;
                  if (!it->imgsrc) it->imgsrc = dom_attr(c, "data-src");   /* lazy-load */
                  it->hidden = st ? st->hidden : 0;
                  it->opacity = st ? st->opacity : 255; }
        f->x += iw; f->line_started = 1; if (ih > f->lineh) f->lineh = ih;
        return;
    }

    flow_children(f, c, h2);                               /* descend inline element */
}

static int is_block(struct node *n)
{
    if (!n || n->type != N_ELEM) return 0;
    struct cstyle *st = n->style;
    /* inline-block joins the block path: it becomes a full-width box unless it
     * has an explicit CSS width (shrink-to-fit is out of scope, but this stops
     * button/chip rows smearing into the surrounding text run).
     *
     * `float` BLOCKIFIES: CSS computes display:inline on a floated box to
     * display:block, which is why `<span style="float:left">` is a box and not
     * a word. Our LibCSS reports the specified display, so the fixup is here. */
    return st && (st->display == DISP_BLOCK || st->display == DISP_FLEX ||
                  st->display == DISP_GRID || st->display == DISP_INLINE_BLOCK ||
                  (st->flt != FLT_NONE && st->display != DISP_NONE));
}

/* Does this box establish a new block formatting context? Floats inside a BFC
 * are invisible outside it, and the BFC root grows to contain them -- which is
 * exactly why `overflow:hidden` on a container is the clearfix everybody uses.
 */
static int is_bfc_root(struct node *n, const struct cstyle *st)
{
    if (!st) return 0;
    if (st->flt != FLT_NONE || st->pos_abs) return 1;
    if (st->overflow_x != OVF_VISIBLE || st->overflow_y != OVF_VISIBLE) return 1;
    if (st->display == DISP_INLINE_BLOCK || st->display == DISP_FLEX ||
        st->display == DISP_GRID) return 1;
    if (n && n->type == N_ELEM && tag_eq(n->tag, "table")) return 1;
    return 0;
}

/* A node that really takes the block path. <svg> is excluded: even with
 * display:block/inline-block it is a replaced element handled by flow_node's
 * image branch (routing it here would yield an empty block box). */
static int blockish(struct node *n)
{
    return is_block(n) && !(n->type == N_ELEM && tag_eq(n->tag, "svg"));
}

static int layout_flex(struct node *n, int x, int y, int w);   /* fwd: flex row */
static int layout_grid(struct node *n, int x, int y, int w);   /* fwd: minimal grid */
static int layout_table(struct node *t, int x, int y, int w);  /* fwd: minimal table */

/* Render list item `idx` (1-based) in the marker alphabet `kind` into `buf`,
 * returning the byte count. Bullets are UTF-8 glyphs; every numeric alphabet
 * gets the trailing '.' a UA sheet's ::marker content would supply. */
static int marker_text(int kind, int idx, char *buf, int max)
{
    /* U+2022 BULLET, U+25E6 WHITE BULLET, U+25AA BLACK SMALL SQUARE */
    static const char *const bullets[] = { "\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xAA" };
    if (kind >= LST_DISC && kind <= LST_SQUARE) {
        const char *b = bullets[kind - LST_DISC];
        int n = 0;
        while (b[n] && n < max) { buf[n] = b[n]; n++; }
        return n;
    }
    if (idx < 1) idx = 1;
    char tmp[16]; int p = 0;                    /* built least-significant first */
    if (kind == LST_LOWER_ALPHA || kind == LST_UPPER_ALPHA) {
        /* bijective base 26: 1->a .. 26->z, 27->aa */
        int base = (kind == LST_LOWER_ALPHA) ? 'a' : 'A', v = idx;
        while (v > 0 && p < 12) { tmp[p++] = (char)(base + (v - 1) % 26); v = (v - 1) / 26; }
    } else if (kind == LST_LOWER_ROMAN || kind == LST_UPPER_ROMAN) {
        static const int val[13] = { 1000,900,500,400,100,90,50,40,10,9,5,4,1 };
        static const char *const sym[13] = { "m","cm","d","cd","c","xc","l","xl",
                                             "x","ix","v","iv","i" };
        char r[16]; int rn = 0, v = idx > 3999 ? 3999 : idx;
        for (int i = 0; i < 13 && rn < 12; i++)
            while (v >= val[i] && rn < 12) {
                for (const char *s = sym[i]; *s && rn < 12; s++)
                    r[rn++] = (kind == LST_UPPER_ROMAN) ? (char)(*s - 32) : *s;
                v -= val[i];
            }
        while (rn > 0 && p < 12) tmp[p++] = r[--rn];   /* reversed; un-reversed below */
    } else {
        int v = idx;
        do { tmp[p++] = (char)('0' + v % 10); v /= 10; } while (v && p < 10);
        if (kind == LST_DECIMAL_ZERO && p < 2) tmp[p++] = '0';
    }
    int n = 0;
    while (p > 0 && n < max - 1) buf[n++] = tmp[--p];
    if (n < max) buf[n++] = '.';
    return n;
}

/* Emit the marker for a <li> block child. `bx` is the li's content-box left
 * edge, `top` its first line's y. The alphabet comes from the inherited
 * list-style-type, so `ol{list-style-type:lower-roman}` really numbers i, ii,
 * iii and `list-style:none` emits nothing. */
static void emit_list_marker(struct node *li, struct cstyle *st, int bx, int top, int minx)
{
    int kind = st->list_style;
    if (kind == LST_NONE) return;
    int idx = 1;
    if (kind >= LST_DECIMAL) {           /* only the counting alphabets need one */
        struct node *par = li->parent;
        idx = 0;
        if (par && par->type == N_ELEM) {
            for (struct node *s = par->first_child; s && s != li; s = s->next)
                if (s->type == N_ELEM && tag_eq(s->tag, "li") && !skipped(s)) idx++;
            /* <ol start=N> shifts the whole run; the attribute is the only way
             * a page can say "this list continues an earlier one". */
            if (tag_eq(par->tag, "ol")) {
                const char *sa = dom_attr(par, "start");
                if (sa) { int sv = atoi_(sa); if (sv > 0) idx += sv - 1; }
            }
        }
        idx++;
    }
    struct item *mk = additem(IT_TEXT, li);
    if (!mk) return;
    int n = marker_text(kind, idx, mk->marker, (int)sizeof mk->marker);
    mk->text = mk->marker; mk->len = n;
    mk->hidden = st->hidden; mk->opacity = st->opacity;
    mk->font_px = st->font_px; mk->bold = st->bold; mk->mono = st->mono;
    mk->color = st->color; mk->h = st->font_px * 5 / 4; mk->y = top;
    int mw = text_measure(mk->text, mk->len, st->font_px, st->mono);
    (void)minx;                                /* deep nests may push the marker to x=0 */
    mk->x = bx - mw - 6; if (mk->x < 0) mk->x = 0;
    mk->w = mw;
}

/* A floated child does NOT break the surrounding inline context (that is the
 * whole point of a float), so it must not push its block onto the
 * child-by-child path -- `<p><img style="float:left">lots of text</p>` has to
 * stay one inline formatting context for the text to wrap beside the image. */
static int floated(struct node *n)
{ struct cstyle *st = n->style; return st && st->flt != FLT_NONE && !st->pos_abs; }

static int has_block_child(struct node *n)
{
    for (struct node *c = n->first_child; c; c = c->next)
        if (blockish(c) && !floated(c)) return 1;
    return 0;
}

static int content_width(struct node *n, int px, int mono, int depth);      /* fwd */
static int min_content_width(struct node *n, int px, int mono, int depth);  /* fwd */

/* Border-box width of a floated box. A float is never auto-width in the block
 * sense: with no specified width it SHRINKS TO FIT, which CSS defines as
 * min(max(min-content, available), max-content). */
static int float_box_width(struct node *c, struct cstyle *st, int avail)
{
    if (st->has_w)
        return clamp_w(st, to_border_w(st, resolve_len(st->width, st->w_pct, st->w_off, avail)), avail);
    int px = st->font_px, mono = st->mono;
    int w = content_width(c, px, mono, 0);
    if (w > avail) w = avail;
    int minc = min_content_width(c, px, mono, 0);
    if (w < minc) w = minc;
    if (w > avail) w = avail;
    if (w < 0) w = 0;
    return clamp_w(st, w, avail);
}

/* Place one floated box out of flow against the left or right content edge of
 * the current block formatting context, and register its MARGIN box as an
 * exclusion. `y` is the earliest the float may start: the block's pen, or the
 * top of the line box it appeared in. Nothing is returned because the float
 * consumes no space in the flow -- only the exclusion list changes. */
static void place_float(struct node *c, struct cstyle *st, int bx0, int bx1, int y)
{
    int ml = st->ml > 0 ? st->ml : 0, mr = st->mr > 0 ? st->mr : 0;
    int mt = st->mt > 0 ? st->mt : 0, mb = st->mb > 0 ? st->mb : 0;
    int side = st->flt;
    int avail = bx1 - bx0 - ml - mr; if (avail < 0) avail = 0;
    int isimg = (c->type == N_ELEM && tag_eq(c->tag, "img"));
    int fw, fh = 0, h_auto = 0;
    if (isimg) {
        /* Same box reservation as the in-flow <img> paths: the pixels arrive
         * later from layout_load_images, so the exclusion has to be built from
         * the declared size. */
        int iw = 0, ih = 0;
        if (st->has_w && !st->w_pct) iw = st->width;
        if (st->has_h && !st->h_pct) ih = st->height;
        if (!iw) { const char *wa = dom_attr(c, "width");  if (wa) iw = atoi_(wa); }
        if (!ih) { const char *ha = dom_attr(c, "height"); if (ha) ih = atoi_(ha); }
        if (iw > 0 && ih <= 0) h_auto = 1;
        if (iw <= 0) iw = ih > 0 ? ih : 24;
        if (ih <= 0) { ih = iw; h_auto = 1; }
        if (avail > 0 && iw > avail) { ih = ih * avail / iw; iw = avail; }
        fw = iw; fh = ih;
    } else {
        fw = float_box_width(c, st, avail);
    }
    if (st->clr != CLR_NONE) y = float_clear_y(st->clr, y);

    /* Highest band at or below y that leaves room for the whole margin box.
     * The band is probed at the float's TOP (height 1) and not over its full
     * height, because the height is only known after it is laid out -- probing
     * the top is what lets two same-side floats sit side by side and then wrap
     * onto the next band when the third no longer fits. */
    int need = ml + fw + mr, lo = bx0, hi = bx1;
    for (int guard = 0; guard < MAXFLOAT + 1; guard++) {
        float_band(y, 1, bx0, bx1, &lo, &hi);
        if (hi - lo >= need) break;
        int nb = float_next_bottom(y);
        if (nb <= y) break;                    /* nothing left to drop past */
        y = nb;
    }
    int fx = (side == FLT_LEFT) ? lo + ml : hi - mr - fw;
    if (fx < bx0) fx = bx0;
    int top = y + mt;

    int zsave = g_z, flsave = g_in_float;
    g_in_float = 1;
    if (st->has_z && st->position != POS_STATIC) g_z = st->z_index;
    int ch;
    if (isimg) {
        struct item *it = additem(IT_IMAGE, c);
        if (it) { it->x = fx; it->y = top; it->w = fw; it->h = fh;
                  it->img = 0; it->imgsrc = dom_attr(c, "src"); it->h_auto = h_auto;
                  if (!it->imgsrc) it->imgsrc = dom_attr(c, "data-src");
                  it->hidden = st->hidden; it->opacity = st->opacity; }
        ch = fh;
    } else {
        int bgidx = -1;
        if (st->has_bg || any_border(st)) {
            struct item *bg = additem(IT_RECT, c);
            if (bg) { bgidx = (int)(bg - items); fill_rect_item(bg, st, fx, top, fw); }
        }
        if (st->list_item) emit_list_marker(c, st, fx + cx_off(st), top, bx0);
        int inw = fw - hextra(st); if (inw < 0) inw = 0;
        /* layout_block sees flt != none and opens a BFC, so the float's own
         * contents neither see nor leak the outer exclusions. */
        int inner = tag_eq(c->tag, "table")
            ? layout_table(c, fx + cx_off(st), top + cy_off(st), inw)
            : layout_block(c, fx + cx_off(st), top + cy_off(st), inw);
        ch = (inner - top) + st->pb + st->border_w[2];
        ch = block_height(st, ch, -1);
        if (ch < st->font_px) ch = st->font_px;
        if (bgidx >= 0) items[bgidx].h = ch;
    }
    g_z = zsave;
    g_in_float = flsave;
    float_add(fx - ml, fx + fw + mr, y, top + ch + mb, side);
}

/* Narrow the active clip to this box's padding box, which is where CSS clips
 * overflow. The box's own background and border were emitted by the CALLER
 * before this runs, so a box never clips away its own border.
 *
 * Only a definite height bounds the clip vertically: with an auto height the
 * content is what set the height, so there is nothing below it to cut. That
 * makes `overflow:hidden` exact for the sized case and a horizontal-only clip
 * for the auto case -- which is the shape (`nowrap` + `hidden`) that actually
 * occurs in stylesheets. */
static void clip_push(const struct cstyle *st, int x, int y, int w)
{
    int px0 = x - st->pl, px1 = x + w + st->pr;
    int py0 = y - st->pt, py1 = 0x3FFFFFFF;
    int sh = spec_h(st, -1);            /* -1 == auto, >= 0 == definite */
    if (sh >= 0) {
        int inner = sh - st->border_w[0] - st->border_w[2];
        py1 = py0 + (inner > 0 ? inner : 0);
    }
    if (g_clip_on) {
        if (g_clipx > px0) px0 = g_clipx;
        if (g_clipx + g_clipw < px1) px1 = g_clipx + g_clipw;
        if (g_clipy > py0) py0 = g_clipy;
        if (g_clipy + g_cliph < py1) py1 = g_clipy + g_cliph;
    }
    if (px1 < px0) px1 = px0;
    if (py1 < py0) py1 = py0;
    g_clip_on = 1;
    g_clipx = px0; g_clipy = py0; g_clipw = px1 - px0; g_cliph = py1 - py0;
}

static int layout_flow(struct node *n, int x, int y, int w);   /* fwd */

/* Lay out the children of block `n` whose content box starts at (x,y) with
 * content width w; returns the bottom y.
 *
 * This wrapper owns the two things that are properties of the BOX rather than
 * of its content: the block formatting context (float scoping + "a BFC root
 * grows to contain its floats") and the overflow clip stamped onto every item
 * the subtree emits. */
static int layout_block(struct node *n, int x, int y, int w)
{
    struct cstyle *nst = n->style;
    int nsave = g_nfloat, bsave = g_fbase, bfc = is_bfc_root(n, nst);
    int con = g_clip_on, cx0 = g_clipx, cy0 = g_clipy, cw0 = g_clipw, ch0 = g_cliph;
    if (bfc) g_fbase = g_nfloat;
    if (nst && (nst->overflow_x != OVF_VISIBLE || nst->overflow_y != OVF_VISIBLE))
        clip_push(nst, x, y, w);

    int cy;
    if (nst && nst->display == DISP_FLEX)                       cy = layout_flex(n, x, y, w);
    else if (nst && nst->display == DISP_GRID && nst->grid_cols > 0) cy = layout_grid(n, x, y, w);
    else                                                        cy = layout_flow(n, x, y, w);

    if (bfc) {
        int b = float_max_bottom(nsave);
        if (b > cy) cy = b;
        g_nfloat = nsave; g_fbase = bsave;
    }
    g_clip_on = con; g_clipx = cx0; g_clipy = cy0; g_clipw = cw0; g_cliph = ch0;
    return cy;
}

static int layout_flow(struct node *n, int x, int y, int w)
{
    struct cstyle *nst = n->style;
    int al = nst ? nst->text_align : ALIGN_LEFT;
    int cy = y;
    /* if this block has no block children, the whole content is one inline
     * context. */
    if (!has_block_child(n)) {
        const char *href = (n->type == N_ELEM && tag_eq(n->tag, "a")) ? dom_attr(n, "href") : 0;
        struct iflow f;
        iflow_init(&f, x, w, cy, al, style_lineh(nst));
        flow_children(&f, n, href);
        newline2(&f, 1);
        return f.y;
    }
    for (struct node *c = n->first_child; c; c = c->next) {
        struct cstyle *st = c->style;
        if (st && st->pos_abs && blockish(c)) {
            /* Absolutely-positioned overlay: anchor at this block's padding-box
             * origin (+ top/left offsets) and lay out out-of-flow (cy unchanged).
             * Full-bleed covers (bilibili's .bili-video-card__cover: top:0;left:0;
             * w/h:100%) land exactly on their card; dropdown menus come here too
             * but stay hidden through their visibility/opacity styles. */
            int ppl = nst ? nst->pl : 0, ppt = nst ? nst->pt : 0, ppr = nst ? nst->pr : 0;
            int ml = st->ml<0?0:st->ml;
            int pw = w + ppl + ppr;                      /* containing block = padding box */
            int ow = st->has_w ? to_border_w(st, resolve_len(st->width, st->w_pct, st->w_off, pw))
                               : pw - (st->has_left ? st->left : 0) - ml;
            ow = clamp_w(st, ow, pw);
            /* right/bottom anchor the opposite edge when the near one is auto:
             * `position:absolute;right:0` is how every close button and badge
             * in the corner of a card is written. */
            int ox = st->has_left || !st->has_right
                   ? x - ppl + (st->has_left ? st->left : 0) + ml
                   : x - ppl + pw - st->right - ow;
            int oy = y - ppt + (st->has_top ? st->top : 0);
            int zsave = g_z;
            if (st->has_z) g_z = st->z_index;
            if (st->has_bg || any_border(st)) {
                struct item *bg = additem(IT_RECT, c);
                if (bg) { fill_rect_item(bg, st, ox, oy, ow);
                          int sh = spec_h(st, -1); bg->h = sh > 0 ? sh : 0; }
            }
            int ovl_save = g_in_overlay;
            g_in_overlay = 1;
            layout_block(c, ox + cx_off(st), oy + cy_off(st), ow - hextra(st));
            g_in_overlay = ovl_save;
            g_z = zsave;
            continue;
        }
        if (skipped(c)) continue;
        if (blockish(c) && floated(c)) {
            /* Out of flow: cy does not move. The float may only start at the
             * current pen, never above content already placed. */
            place_float(c, st, x, x + w, cy);
            continue;
        }
        if (blockish(c)) {
            /* `clear` drops the box below the floats on the named side(s). It
             * applies before the top margin is added, which is why it is here
             * and not folded into the cy += mt below. */
            if (st->clr != CLR_NONE) cy = float_clear_y(st->clr, cy);
            if (tag_eq(c->tag, "img")) {
                /* Block-level <img> is a replaced element, not an empty block box
                 * (bilibili's blanket img{display:block} rule would otherwise eat
                 * every cover). Unsized: fill the line; height follows the
                 * decoded aspect via h_auto. */
                int ml = st->ml<0?0:st->ml, mr = st->mr<0?0:st->mr;
                int iw = st->has_w ? resolve_len(st->width, st->w_pct, st->w_off, w) : 0;
                int ih = st->has_h && !st->h_pct ? st->height : 0;
                if (!iw) { const char *wa = dom_attr(c, "width");  if (wa) iw = atoi_(wa); }
                if (!ih) { const char *ha = dom_attr(c, "height"); if (ha) ih = atoi_(ha); }
                int h_auto = 0;
                if (iw <= 0) { iw = w - ml - mr; h_auto = 1; }
                else if (ih <= 0) h_auto = 1;
                if (iw < 0) iw = 0;
                /* max-width really does apply to replaced elements, and
                 * `img{max-width:100%}` is in essentially every page's reset;
                 * without it a wide photo used to blow past its column. */
                { int cw = clamp_w(st, iw, w);
                  if (cw != iw) { if (iw > 0 && ih > 0 && !h_auto) ih = ih * cw / iw; iw = cw; } }
                if (ih <= 0) ih = iw;
                cy += st->mt > 0 ? st->mt : 0;
                struct item *it = additem(IT_IMAGE, c);
                if (it) { it->x = x + ml; it->y = cy; it->w = iw; it->h = ih;
                          it->img = 0; it->imgsrc = dom_attr(c, "src"); it->h_auto = h_auto;
                          if (!it->imgsrc) it->imgsrc = dom_attr(c, "data-src");
                          it->hidden = st->hidden; it->opacity = st->opacity; }
                cy += ih + (st->mb > 0 ? st->mb : 0);
                continue;
            }
            int ml = st->ml<0?0:st->ml;
            int bx = x + ml;
            int bw = block_width(st, w);
            if (st->ml < 0 && st->mr < 0) bx = x + (w - bw)/2;   /* margin:auto center */
            cy += st->mt > 0 ? st->mt : 0;
            int top = cy;
            int mark = nitem;                       /* for position:relative below */
            int zsave = g_z;
            if (st->has_z && st->position != POS_STATIC) g_z = st->z_index;
            if (st->list_item) emit_list_marker(c, st, bx + cx_off(st), top, x);
            int bgidx = -1;
            if (st->has_bg || any_border(st)) {
                struct item *bg = additem(IT_RECT, c);
                if (bg) { bgidx = (int)(bg - items); fill_rect_item(bg, st, bx, top, bw); }
            }
            int inw = bw - hextra(st); if (inw < 0) inw = 0;
            int inner = tag_eq(c->tag, "table")
                ? layout_table(c, bx + cx_off(st), top + cy_off(st), inw)
                : layout_block(c, bx + cx_off(st), top + cy_off(st), inw);
            int ch = (inner - top) + st->pb + st->border_w[2];
            ch = block_height(st, ch, -1);
            if (ch < st->font_px) ch = st->font_px;          /* min line */
            if (bgidx >= 0) items[bgidx].h = ch;
            /* position:relative (and sticky, which is relative until scrolled
             * to) offsets the painted box without changing the space it
             * reserved -- so shift what it emitted and leave cy alone. */
            if (st->position == POS_RELATIVE || st->position == POS_STICKY) {
                int dx = st->has_left ? st->left : (st->has_right ? -st->right : 0);
                int dy = st->has_top ? st->top : (st->has_bottom ? -st->bottom : 0);
                shift_items(mark, nitem, dx, dy);
            }
            g_z = zsave;
            cy = top + ch + (st->mb > 0 ? st->mb : 0);
        } else {
            /* Run of inline siblings: gather until the next block. A FLOATED
             * sibling does not end the run -- flow_node takes it out of flow
             * and the rest of the run wraps beside it. */
            struct iflow f;
            iflow_init(&f, x, w, cy, al, style_lineh(nst));
            while (c && (!blockish(c) || floated(c))) {
                if (!skipped(c)) flow_node(&f, c, 0);
                struct node *nx = c->next;
                if (!nx || (blockish(nx) && !floated(nx))) break;
                c = nx;
            }
            newline2(&f, 1);
            cy = f.y;
        }
    }
    return cy;
}

/* Word-wise width of one text node as a single unwrapped line. */
static int measure_words(const char *s, int len, int px, int mono)
{
    int w = 0, i = 0;
    while (i < len) {
        while (i < len && sp(s[i])) i++;
        int ws = i; while (i < len && !sp(s[i])) i++;
        if (i > ws) w += text_measure(s + ws, i - ws, px, mono) + px / 4;
    }
    return w;
}

/* Text width under an inline element (button label in a span, etc.). */
static int flex_text_width(struct node *n, int px, int mono)
{
    int w = 0;
    for (struct node *c = n->first_child; c; c = c->next) {
        if (c->type == N_TEXT) w += measure_words(c->text, c->textlen, px, mono);
        else if (c->type == N_ELEM) {
            struct cstyle *st = c->style;
            if (skipped(c)) continue;
            if (tag_eq(c->tag, "svg")) { w += svg_attr_w(c, st); continue; }
            w += flex_text_width(c, px, mono);
        }
    }
    return w;
}

/* Max-content width of a subtree: text measured unwrapped, flex rows summed,
 * block stacks take the widest child. Used to size auto flex items by content
 * (real flexbox sizes flex:auto items this way instead of splitting space). */
static int content_width(struct node *n, int px, int mono, int depth)
{
    if (depth > 32) return 0;
    struct cstyle *st = n->style;
    if (n->type == N_TEXT) return measure_words(n->text, n->textlen, px, mono);
    if (n->type != N_ELEM) return 0;
    if (skipped(n)) return 0;
    if (tag_eq(n->tag, "img")) {
        int iw = 0;
        if (st && st->has_w && !st->w_pct) iw = st->width;
        if (!iw) { const char *wa = dom_attr(n, "width"); if (wa) iw = atoi_(wa); }
        return iw > 0 ? iw : 24;
    }
    if (tag_eq(n->tag, "svg")) return svg_attr_w(n, st);
    /* Everything below is a BORDER-BOX max-content width, so borders and
     * padding count once, here, and box-sizing decides whether an explicit
     * width already includes them. */
    int extra = hextra(st);
    if (st && st->has_w && !st->w_pct) return to_border_w(st, st->width);
    int cpx = st ? st->font_px : px, cmono = st ? st->mono : mono;
    /* Only a ROW flex container sums its children; a column stacks them, so it
     * is as wide as its widest child like any block. */
    int row = st && st->display == DISP_FLEX &&
              (st->flex_dir == FDIR_ROW || st->flex_dir == FDIR_ROW_REV);
    int acc = 0;
    for (struct node *c = n->first_child; c; c = c->next) {
        int cw = content_width(c, cpx, cmono, depth + 1);
        /* horizontal margins are part of the child's footprint (a shrink-to-fit
         * parent must leave room for them or the child's text wraps). */
        if (c->type == N_ELEM && c->style && !skipped(c)) {
            struct cstyle *cs = c->style;
            if (cs->ml > 0) cw += cs->ml;
            if (cs->mr > 0) cw += cs->mr;
        }
        if (row) acc += cw; else if (cw > acc) acc = cw;
    }
    return acc + extra;
}

/* Widest unbreakable token in one text run. A token is whitespace-delimited;
 * one containing a multi-byte UTF-8 sequence counts only as its widest single
 * CHARACTER, because CJK has a line-break opportunity between any two
 * ideographs and flow_text's break-anywhere path already takes it. An ASCII
 * word has no such opportunity and stays indivisible, which is exactly the
 * distinction real line breakers draw. */
static int min_word_width(const char *s, int len, int px, int mono)
{
    int best = 0, i = 0;
    while (i < len) {
        while (i < len && sp(s[i])) i++;
        int ws = i, wide = 0;
        while (i < len && !sp(s[i])) { if ((unsigned char)s[i] & 0x80) wide = 1; i++; }
        if (i <= ws) continue;
        if (!wide) {
            int w = text_measure(s + ws, i - ws, px, mono);
            if (w > best) best = w;
        } else {
            for (int p = ws; p < i; ) {
                int adv = 1;
                while (p + adv < i && (s[p + adv] & 0xC0) == 0x80) adv++;
                int w = text_measure(s + p, adv, px, mono);
                if (w > best) best = w;
                p += adv;
            }
        }
    }
    return best;
}

/* Min-content width of a subtree: the narrowest the box can get without
 * breaking something the line breaker cannot break. This is CSS's automatic
 * minimum size -- what `min-width:auto` (the initial value FOR A FLEX ITEM,
 * unlike everywhere else) resolves to, and the reason a flex row squeezes its
 * spacing before it squeezes a label into a vertical letter-stack. */
static int min_content_width(struct node *n, int px, int mono, int depth)
{
    if (depth > 32) return 0;
    struct cstyle *st = n->style;
    if (n->type == N_TEXT) return min_word_width(n->text, n->textlen, px, mono);
    if (n->type != N_ELEM) return 0;
    if (skipped(n)) return 0;
    /* Replaced content has no internal break opportunity at all. */
    if (tag_eq(n->tag, "img") || tag_eq(n->tag, "svg"))
        return content_width(n, px, mono, depth);
    int cpx = st ? st->font_px : px, cmono = st ? st->mono : mono;
    int rowdir = st && st->display == DISP_FLEX &&
                 (st->flex_dir == FDIR_ROW || st->flex_dir == FDIR_ROW_REV);
    int acc = 0;
    for (struct node *c = n->first_child; c; c = c->next) {
        int cw = min_content_width(c, cpx, cmono, depth + 1);
        if (rowdir) acc += cw; else if (cw > acc) acc = cw;
    }
    return acc + hextra(st);
}

/* Measure one anonymous inline run starting at `first` as a single unwrapped
 * line; *end gets the first node past the run (a block sibling or NULL). */
static int flex_run(struct node *first, struct node **end, int px, int mono)
{
    int w = 0;
    struct node *c = first;
    for (; c; c = c->next) {
        if (c->type == N_ELEM) {
            struct cstyle *st = c->style;
            if (skipped(c)) continue;
            if (blockish(c)) break;
            if (tag_eq(c->tag, "svg")) { w += svg_attr_w(c, st); continue; }
            w += flex_text_width(c, px, mono);   /* inline element (button/svg icon etc.) */
            continue;
        }
        if (c->type == N_TEXT) w += measure_words(c->text, c->textlen, px, mono);
    }
    *end = c;
    return w;
}

/* ---- flexbox ----
 *
 * The real algorithm, bounded where the display list cannot express the
 * result. What IS implemented: flex-direction (all four), flex-wrap (including
 * wrap-reverse), order, flex-basis, flex-grow, flex-shrink, justify-content,
 * align-items and align-self, row/column gaps, and min/max clamping of the
 * resolved main size.
 *
 * The two structural liberties, both stated where they are taken below:
 *   - main sizes in a ROW are resolved from a max-content measurement
 *     (content_width) rather than from a trial layout, and flexible lengths
 *     are resolved in ONE pass rather than iterating after min/max clamping;
 *   - a COLUMN's items are stacked at their natural laid-out heights, because
 *     an auto-height column container has no free space to distribute -- grow
 *     and justify-content only engage when the container height is definite.
 *
 * Cross-axis alignment is done by translating each item's finished slice of
 * the display list, which is why every item records the [lo,hi) range it
 * emitted. That is also how position:relative works, and it is much cheaper
 * than laying an item out twice.
 *
 * Bare text / inline siblings participate as anonymous flex items, exactly as
 * CSS says (a <button>Platform<svg/></button> must not lose "Platform"). */

struct fitem {
    struct node *n;        /* element item, or the first node of an inline run */
    struct node *end;      /* anonymous run: one past its last node; else NULL */
    struct cstyle *st;     /* n->style for an element item, else NULL */
    int order;
    int base;              /* hypothetical main size (border box, no margins) */
    int ms, me;            /* main-axis start/end margins */
    int cms, cme;          /* cross-axis start/end margins */
    int used;              /* resolved main size */
    int minsz;             /* automatic minimum (min-width:auto), main axis */
    int grow, shrink;      /* css_fixed (1.0 == 1024) */
    int lo, hi;            /* display-list range once placed */
    int bgidx;             /* IT_RECT index, or -1 */
    int cross;             /* measured cross size (border box) */
};

/* The container's effective align value for one item. */
static int flex_align_of(const struct cstyle *nst, const struct fitem *fi)
{
    int a = fi->st ? fi->st->align_self : AL_AUTO;
    if (a == AL_AUTO) a = nst ? nst->align_items : AL_STRETCH;
    /* We have no baseline metrics in the display list; first-baseline
     * alignment of same-font items is indistinguishable from flex-start. */
    if (a == AL_BASELINE) a = AL_START;
    return a;
}

/* Gather the container's flex items in document order, then stable-sort by
 * `order`. `cap` is the child count, which is an upper bound on the item count
 * (an anonymous run always swallows at least one child). */
static int flex_collect(struct node *n, struct fitem *fi, int cap, int fpx, int fmono)
{
    int cnt = 0;
    struct node *c = n->first_child;
    while (c && cnt < cap) {
        if (c->type != N_ELEM || !blockish(c)) {
            struct node *end;
            int rw = flex_run(c, &end, fpx, fmono);
            if (end == c) { c = c->next; continue; }   /* nothing consumable */
            if (rw > 0) {
                struct fitem *f = &fi[cnt++];
                memset(f, 0, sizeof *f);
                f->n = c; f->end = end; f->base = rw;
                f->shrink = 1024; f->bgidx = -1;
            }
            c = end;
            continue;
        }
        if (skipped(c)) { c = c->next; continue; }
        struct fitem *f = &fi[cnt++];
        memset(f, 0, sizeof *f);
        f->n = c; f->st = c->style; f->bgidx = -1;
        f->order = f->st ? f->st->order : 0;
        f->grow = f->st ? f->st->flex_grow : 0;
        f->shrink = f->st ? f->st->flex_shrink : 1024;
        c = c->next;
    }
    /* Insertion sort: stable, and `order` is almost always all-zero so this is
     * a single comparison pass in practice. */
    for (int i = 1; i < cnt; i++) {
        struct fitem key = fi[i];
        int j = i - 1;
        while (j >= 0 && fi[j].order > key.order) { fi[j + 1] = fi[j]; j--; }
        fi[j + 1] = key;
    }
    return cnt;
}

/* Lay one flex item out at (px,py) with border-box main/cross sizes, recording
 * the display-list range and background index. Returns its border-box height. */
static int flex_place(struct fitem *f, int px, int py, int iw, int forced_h, int fpx, int fmono)
{
    f->lo = nitem;
    if (!f->st) {                                    /* anonymous inline run */
        struct iflow fl;
        iflow_init(&fl, px, iw > 0 ? iw : 1, py, ALIGN_LEFT, fpx * 5 / 4);
        for (struct node *r = f->n; r && r != f->end; r = r->next) flow_node(&fl, r, 0);
        newline2(&fl, 1);
        f->hi = nitem;
        (void)fpx; (void)fmono;
        return fl.y - py;
    }
    struct cstyle *st = f->st;
    int zsave = g_z;
    /* Every flex item is a block formatting context of its own: a float inside
     * one must not narrow the lines of the item beside it. layout_block only
     * opens a BFC when the item's own style says so, so scope it here. */
    int nsave = g_nfloat, bsave = g_fbase;
    g_fbase = g_nfloat;
    /* z-index applies to a flex ITEM even when it is not positioned -- that is
     * the one place CSS lets an unpositioned box make a stacking context. */
    if (st->has_z) g_z = st->z_index;
    if (st->has_bg || any_border(st)) {
        struct item *bg = additem(IT_RECT, f->n);
        if (bg) { f->bgidx = (int)(bg - items); fill_rect_item(bg, st, px, py, iw); }
    }
    int inw = iw - hextra(st); if (inw < 0) inw = 0;
    int inner = layout_block(f->n, px + cx_off(st), py + cy_off(st), inw);
    { int b = float_max_bottom(nsave); if (b > inner) inner = b; }
    g_nfloat = nsave; g_fbase = bsave;
    int ch = (inner - py) + st->pb + st->border_w[2];
    ch = block_height(st, ch, -1);
    if (forced_h > ch) ch = forced_h;
    if (ch < st->font_px) ch = st->font_px;
    if (f->bgidx >= 0) items[f->bgidx].h = ch;
    if (st->position == POS_RELATIVE || st->position == POS_STICKY)
        shift_items(f->lo, nitem,
                    st->has_left ? st->left : (st->has_right ? -st->right : 0),
                    st->has_top ? st->top : (st->has_bottom ? -st->bottom : 0));
    g_z = zsave;
    f->hi = nitem;
    return ch;
}

/* Distribute `freesp` main-axis pixels over one line, then clamp. Positive free
 * space goes to flex-grow, negative to flex-shrink scaled by the base size --
 * which is why an over-full row of default (shrink:1) items compresses in
 * proportion to how big each one wanted to be. */
static void flex_resolve(struct fitem *fi, int lo, int hi, int freesp, int mainw)
{
    long gsum = 0, ssum = 0;
    for (int i = lo; i < hi; i++) {
        gsum += fi[i].grow;
        ssum += (long)fi[i].shrink * (fi[i].base > 0 ? fi[i].base : 0);
    }
    long total = (freesp > 0) ? gsum : ssum;
    long acc = 0, given = 0;
    for (int i = lo; i < hi; i++) {
        int u = fi[i].base;
        if (total > 0 && freesp) {
            acc += (freesp > 0) ? (long)fi[i].grow
                                : (long)fi[i].shrink * (fi[i].base > 0 ? fi[i].base : 0);
            /* Cumulative rather than per-item, so the deltas sum to EXACTLY
             * freesp instead of each losing up to a pixel to truncation. A row
             * of eight grow items used to land eight pixels short of its
             * container's right edge. */
            long want = (long)freesp * acc / total;
            u += (int)(want - given);
            given = want;
        }
        if (u < 0) u = 0;
        if (u < fi[i].minsz) u = fi[i].minsz;
        /* One clamping pass, not the spec's freeze-and-redistribute loop: an
         * item that hits a bound here keeps space the others would otherwise
         * have shared. It shows only when several items on one line clamp at
         * once, and the line then over- or under-flows by the difference --
         * which is also what a real browser does when the minimums do not fit. */
        fi[i].used = clamp_w(fi[i].st, u, mainw);
    }
}

/* Leading offset and per-gap extra for justify-content over `slack` px. */
static void flex_justify(int mode, int slack, int count, int *lead, int *between)
{
    *lead = 0; *between = 0;
    if (slack <= 0 || count <= 0) return;
    switch (mode) {
    case JC_END:     *lead = slack; break;
    case JC_CENTER:  *lead = slack / 2; break;
    case JC_BETWEEN: if (count > 1) *between = slack / (count - 1); else *lead = 0; break;
    case JC_AROUND:  *between = slack / count; *lead = *between / 2; break;
    case JC_EVENLY:  *between = slack / (count + 1); *lead = *between; break;
    default:         break;                            /* flex-start */
    }
}

static int layout_flex(struct node *n, int x, int y, int w)
{
    struct cstyle *nst = n->style;
    int fpx = nst ? nst->font_px : 16, fmono = nst ? nst->mono : 0;
    if (w < 0) w = 0;
    int dir = nst ? nst->flex_dir : FDIR_ROW;
    int row = (dir == FDIR_ROW || dir == FDIR_ROW_REV);
    int rev = (dir == FDIR_ROW_REV || dir == FDIR_COL_REV);
    int wrap = nst ? nst->flex_wrap : FWRAP_NOWRAP;
    int gap_main = nst ? (row ? nst->grid_gap_x : nst->grid_gap_y) : 0;
    int gap_cross = nst ? (row ? nst->grid_gap_y : nst->grid_gap_x) : 0;
    if (gap_main < 0) gap_main = 0;
    if (gap_cross < 0) gap_cross = 0;

    /* One heap allocation for the items and the per-line bookkeeping, sized to
     * the actual child count. Heap and not stack because flex nests -- a column
     * of rows of columns is the standard card grid -- and sized rather than
     * capped because a wrapping tag cloud really can have hundreds of items,
     * and silently dropping the tail is a much worse failure than the malloc. */
    int nkids = 0;
    for (struct node *k = n->first_child; k; k = k->next) nkids++;
    if (!nkids) return y;
    struct fitem *fi = kmalloc(sizeof(struct fitem) * (unsigned long)nkids +
                               sizeof(int) * (unsigned long)nkids * 4);
    if (!fi) return y;
    int *lstart = (int *)(fi + nkids);
    int *lend = lstart + nkids, *ytop = lend + nkids, *yhgt = ytop + nkids;
    int cnt = flex_collect(n, fi, nkids, fpx, fmono);
    if (!cnt) { kfree(fi); return y; }

    if (!row) {
        /* ---- column ----
         * An auto-height column container has no free main space, so items
         * simply stack at the height their own layout produces -- which is
         * what a block stack does, and the right answer for the card/sidebar
         * shape that `flex-direction:column` is nearly always used for.
         * flex-wrap is treated as nowrap here: a multi-column wrap needs a
         * definite height to break against, which we do not have. */
        int container_h = spec_h(nst, -1);
        int cy = y, first = 1;
        for (int k = 0; k < cnt; k++) {
            struct fitem *f = &fi[rev ? cnt - 1 - k : k];
            struct cstyle *st = f->st;
            f->cms = st && st->ml > 0 ? st->ml : 0;
            f->cme = st && st->mr > 0 ? st->mr : 0;
            f->ms  = st && st->mt > 0 ? st->mt : 0;
            f->me  = st && st->mb > 0 ? st->mb : 0;
            int align = flex_align_of(nst, f);
            int avail = w - f->cms - f->cme; if (avail < 0) avail = 0;
            int iw;
            if (st && st->has_w) iw = clamp_w(st, block_width(st, w), w);
            else if (align == AL_STRETCH) iw = avail;
            else {                                   /* shrink to fit the content */
                iw = content_width(f->n, fpx, fmono, 0);
                if (iw > avail) iw = avail;
                iw = clamp_w(st, iw, w);
            }
            if (iw < 0) iw = 0;
            int ax = x + f->cms;
            if (align == AL_END)         ax = x + w - f->cme - iw;
            else if (align == AL_CENTER) ax = x + (w - iw) / 2;
            if (!first) cy += gap_main;
            first = 0;
            cy += f->ms;
            int forced = spec_h(st, container_h);
            int ch = flex_place(f, ax, cy, iw, forced > 0 ? forced : 0, fpx, fmono);
            f->used = ch;
            cy += ch + f->me;
        }
        /* Only a definite container height leaves anything to distribute. */
        if (container_h > 0 && container_h > cy - y) {
            int slack = container_h - (cy - y);
            long gsum = 0;
            for (int i = 0; i < cnt; i++) gsum += fi[i].grow;
            if (gsum > 0) {
                /* Growing an already-laid-out item stretches its box, not its
                 * content: the background/border grows and the following items
                 * move down, but the text stays at the top of the box. */
                int run = 0;
                for (int k = 0; k < cnt; k++) {
                    struct fitem *f = &fi[rev ? cnt - 1 - k : k];
                    shift_items(f->lo, f->hi, 0, run);
                    int d = (int)((long)slack * f->grow / gsum);
                    if (f->bgidx >= 0) items[f->bgidx].h += d;
                    run += d;
                }
                cy += run;
            } else {
                int lead, between;
                flex_justify(nst ? nst->justify : JC_START, slack, cnt, &lead, &between);
                int run = lead;
                for (int k = 0; k < cnt; k++) {
                    struct fitem *f = &fi[rev ? cnt - 1 - k : k];
                    shift_items(f->lo, f->hi, 0, run);
                    run += between;
                }
                cy = y + container_h;
            }
        }
        kfree(fi);
        return cy;
    }

    /* ---- row: measure hypothetical main sizes ---- */
    for (int i = 0; i < cnt; i++) {
        struct fitem *f = &fi[i];
        struct cstyle *st = f->st;
        if (!st) {
            /* Anonymous run: base is its unwrapped width, and its minimum is
             * the widest token in it, so a bare label between two sized items
             * is not shredded either. */
            int mn = 0;
            for (struct node *r = f->n; r && r != f->end; r = r->next) {
                int v = min_content_width(r, fpx, fmono, 0);
                if (v > mn) mn = v;
            }
            f->minsz = mn > w ? w : mn;
            continue;
        }
        f->ms  = st->ml > 0 ? st->ml : 0;
        f->me  = st->mr > 0 ? st->mr : 0;
        f->cms = st->mt > 0 ? st->mt : 0;
        f->cme = st->mb > 0 ? st->mb : 0;
        int spec = st->has_w ? to_border_w(st, resolve_len(st->width, st->w_pct, st->w_off, w)) : -1;
        int b;
        if (st->has_fb)       b = to_border_w(st, resolve_len(st->flex_basis, st->fb_pct, st->fb_off, w));
        else if (spec >= 0)   b = spec;
        else                  b = content_width(f->n, fpx, fmono, 0);
        f->base = clamp_w(st, b, w);
        /* min-width:auto. An explicit min-width replaces it outright (clamp_w
         * applies that), and overflow != visible switches it off -- which is
         * why `min-width:0` and `overflow:hidden` are the two standard escapes
         * from a flex item that refuses to shrink. */
        if (!st->has_min_w && st->overflow_x == OVF_VISIBLE) {
            int mn = min_content_width(f->n, fpx, fmono, 0);
            if (spec >= 0 && mn > spec) mn = spec;    /* capped by the size suggestion */
            if (mn > w) mn = w;                       /* never wider than the container */
            f->minsz = mn;
        }
    }

    /* ---- break into lines ---- */
    int nline = 0;
    if (wrap == FWRAP_NOWRAP) {
        lstart[0] = 0; lend[0] = cnt; nline = 1;
    } else {
        int i = 0;
        while (i < cnt && nline < nkids) {      /* every line takes >=1 item */
            int used = 0, j = i;
            while (j < cnt) {
                int outer = fi[j].ms + fi[j].base + fi[j].me + (j > i ? gap_main : 0);
                if (j > i && used + outer > w) break;
                used += outer; j++;
            }
            if (j == i) j = i + 1;                    /* always make progress */
            lstart[nline] = i; lend[nline] = j; nline++;
            i = j;
        }
    }

    /* ---- resolve, place, align ---- */
    int cy = y;
    for (int L = 0; L < nline; L++) {
        int lo = lstart[L], hi = lend[L], ncell = hi - lo;
        int sum = 0;
        for (int i = lo; i < hi; i++) sum += fi[i].ms + fi[i].base + fi[i].me;
        sum += gap_main * (ncell - 1);
        flex_resolve(fi, lo, hi, w - sum, w);

        int taken = gap_main * (ncell - 1);
        for (int i = lo; i < hi; i++) taken += fi[i].ms + fi[i].used + fi[i].me;
        int lead, between;
        flex_justify(nst ? nst->justify : JC_START, w - taken, ncell, &lead, &between);

        int cx = x + lead, linetop = cy, linecross = 0;
        for (int k = 0; k < ncell; k++) {
            struct fitem *f = &fi[rev ? hi - 1 - k : lo + k];
            cx += f->ms;
            int top = cy + f->cms;
            int ch = flex_place(f, cx, top, f->used, 0, fpx, fmono);
            f->cross = ch;
            if (f->cms + ch + f->cme > linecross) linecross = f->cms + ch + f->cme;
            cx += f->used + f->me + gap_main + between;
        }
        /* Cross-axis alignment, applied by translating each item's finished
         * range now that the line's cross size is known. */
        for (int i = lo; i < hi; i++) {
            struct fitem *f = &fi[i];
            int align = flex_align_of(nst, f);
            int space = linecross - (f->cms + f->cross + f->cme);
            if (align == AL_STRETCH) {
                /* Stretch grows the box, not the content -- same liberty as
                 * the column grow path above. An item with a definite height
                 * is not stretched. */
                if (f->bgidx >= 0 && space > 0 && !(f->st && f->st->has_h))
                    items[f->bgidx].h = f->cross + space;
            } else if (space > 0) {
                int off = (align == AL_END) ? space : (align == AL_CENTER) ? space / 2 : 0;
                shift_items(f->lo, f->hi, 0, off);
            }
        }
        ytop[L] = linetop; yhgt[L] = linecross;
        cy = linetop + linecross + (L + 1 < nline ? gap_cross : 0);
    }

    /* wrap-reverse stacks the lines from the far cross edge back. Mirroring the
     * finished lines is exact and costs one pass. */
    if (wrap == FWRAP_WRAP_REV && nline > 1) {
        int total = cy - y;
        for (int L = 0; L < nline; L++) {
            int newtop = y + total - (ytop[L] - y) - yhgt[L];
            for (int i = lstart[L]; i < lend[L]; i++)
                shift_items(fi[i].lo, fi[i].hi, 0, newtop - ytop[L]);
        }
    }

    kfree(fi);
    return cy;
}

/* ---- minimal grid layout ----
 * Only what css_extra parses: N equal/fr/px columns (repeat(N,1fr) etc.) with
 * px gaps. Children are placed in document order, left to right, wrapping
 * every N; each row is as tall as its tallest item; items lay out with their
 * column width as containing block. No areas/spans/auto-placement. */
static int layout_grid(struct node *n, int x, int y, int w)
{
    struct cstyle *nst = n->style;
    int nc = nst->grid_cols;
    if (nc > GRID_MAXCOL) nc = GRID_MAXCOL;
    if (nc < 1) return y;
    int gx = nst->grid_gap_x > 0 ? nst->grid_gap_x : 0;
    int gy = nst->grid_gap_y > 0 ? nst->grid_gap_y : 0;
    int fixed = 0, weights = 0;
    for (int i = 0; i < nc; i++) {
        int t = nst->grid_tracks[i];
        if (t > 0) fixed += t; else weights += -t;
    }
    int leftover = w - gx * (nc - 1) - fixed;
    if (leftover < 0) leftover = 0;
    int colw[GRID_MAXCOL], acc = 0;
    for (int i = 0; i < nc; i++) {
        int t = nst->grid_tracks[i];
        colw[i] = t > 0 ? t : (weights > 0 ? leftover * (-t) / weights : leftover / nc);
        acc += colw[i];
    }
    /* let a trailing fr track absorb the integer-rounding remainder */
    if (nc > 0 && nst->grid_tracks[nc-1] < 0 && leftover > acc)
        colw[nc-1] += leftover - acc;

    int cy = y, rowbot = y, col = 0, items_in_row = 0;
    for (struct node *c = n->first_child; c; c = c->next) {
        if (c->type != N_ELEM || skipped(c)) continue;
        if (col == 0 && items_in_row) { cy = rowbot + gy; rowbot = cy; }
        struct cstyle *st = c->style;
        int ml = st && st->ml > 0 ? st->ml : 0, mr = st && st->mr > 0 ? st->mr : 0;
        int cellx = x;
        for (int i = 0; i < col; i++) cellx += colw[i] + gx;
        int cw = colw[col] - ml - mr; if (cw < 0) cw = 0;
        cw = clamp_w(st, cw, colw[col]);
        int top = cy + (st && st->mt > 0 ? st->mt : 0);
        int bgidx = -1;
        if (st && (st->has_bg || any_border(st))) {
            struct item *bg = additem(IT_RECT, c);
            if (bg) { bgidx = (int)(bg - items); fill_rect_item(bg, st, cellx + ml, top, cw); }
        }
        int inw = cw - hextra(st); if (inw < 0) inw = 0;
        int nsave = g_nfloat, bsave = g_fbase;
        g_fbase = g_nfloat;                     /* each grid item is its own BFC */
        int inner = layout_block(c, cellx + ml + cx_off(st), top + cy_off(st), inw);
        { int b = float_max_bottom(nsave); if (b > inner) inner = b; }
        g_nfloat = nsave; g_fbase = bsave;
        int ch = (inner - top) + (st ? st->pb + st->border_w[2] : 0);
        ch = block_height(st, ch, -1);
        if (st && ch < st->font_px) ch = st->font_px;
        if (bgidx >= 0) items[bgidx].h = ch;
        if (top + ch > rowbot) rowbot = top + ch;
        items_in_row = 1;
        if (++col == nc) col = 0;
    }
    (void)items_in_row;
    return rowbot;
}

/* ---- minimal table layout ----
 * Rows are collected from the <table>'s children (through thead/tbody/tfoot
 * wrappers), column widths are proportional to each column's widest word, and
 * each row is as tall as its tallest cell. colspan/rowspan are treated as 1. */
#define TBL_MAXROWS 64
#define TBL_MAXCOLS 16

static int tbl_row_visible(struct node *r)
{
    struct cstyle *rs = r->style;
    return r->type == N_ELEM && tag_eq(r->tag, "tr") && !(rs && rs->display == DISP_NONE);
}

/* The widest single word anywhere under `n` (skip display:none subtrees). */
static int tbl_widest_word(struct node *n, int px, int mono)
{
    int best = 0;
    for (struct node *c = n->first_child; c; c = c->next) {
        if (skipped(c)) continue;
        if (c->type == N_TEXT) {
            const char *s = c->text; int len = c->textlen, i = 0;
            while (i < len) {
                while (i < len && sp(s[i])) i++;
                int ws = i; while (i < len && !sp(s[i])) i++;
                if (i > ws) {
                    int ww = text_measure(s + ws, i - ws, px, mono);
                    if (ww > best) best = ww;
                }
            }
        } else if (c->type == N_ELEM) {
            int w = tbl_widest_word(c, px, mono);
            if (w > best) best = w;
        }
    }
    return best;
}

static int tbl_cell_count(struct node *r)
{
    int n = 0;
    for (struct node *c = r->first_child; c; c = c->next) {
        if (c->type != N_ELEM || (!tag_eq(c->tag, "td") && !tag_eq(c->tag, "th"))) continue;
        if (!skipped(c)) n++;
    }
    return n;
}

static int layout_table(struct node *t, int x, int y, int w)
{
    struct node *rows[TBL_MAXROWS]; int nr = 0;
    for (struct node *c = t->first_child; c && nr < TBL_MAXROWS; c = c->next) {
        if (c->type != N_ELEM) continue;
        if (skipped(c)) continue;
        if (tbl_row_visible(c)) { rows[nr++] = c; continue; }
        if (tag_eq(c->tag, "tbody") || tag_eq(c->tag, "thead") || tag_eq(c->tag, "tfoot"))
            for (struct node *r = c->first_child; r && nr < TBL_MAXROWS; r = r->next)
                if (tbl_row_visible(r)) rows[nr++] = r;
    }
    if (!nr) return y;

    int nc = 0;
    for (int i = 0; i < nr; i++) { int k = tbl_cell_count(rows[i]); if (k > nc) nc = k; }
    if (!nc || nc > TBL_MAXCOLS) nc = nc > TBL_MAXCOLS ? TBL_MAXCOLS : nc;
    if (!nc) return y;

    int desired[TBL_MAXCOLS];
    for (int i = 0; i < nc; i++) desired[i] = 8;
    for (int i = 0; i < nr; i++) {
        int ci = 0;
        for (struct node *c = rows[i]->first_child; c && ci < nc; c = c->next) {
            if (c->type != N_ELEM || (!tag_eq(c->tag, "td") && !tag_eq(c->tag, "th"))) continue;
            struct cstyle *cs = c->style;
            if (skipped(c)) continue;
            int px = cs ? cs->font_px : 16, mono = cs ? cs->mono : 0;
            int dw = tbl_widest_word(c, px, mono) + (cs ? cs->pl + cs->pr : 0) + 12;
            if (dw > desired[ci]) desired[ci] = dw;
            ci++;
        }
    }
    int total = 0;
    for (int i = 0; i < nc; i++) total += desired[i];
    if (total <= 0) total = 1;
    int cw[TBL_MAXCOLS], acc = 0;
    for (int i = 0; i < nc; i++) { cw[i] = w * desired[i] / total; if (cw[i] < 24) cw[i] = 24; acc += cw[i]; }
    cw[nc-1] += w - acc; if (cw[nc-1] < 24) cw[nc-1] = 24;   /* absorb rounding */

    int cy = y;
    for (int i = 0; i < nr; i++) {
        int rx = x, maxb = cy, ci = 0;
        for (struct node *c = rows[i]->first_child; c && ci < nc; c = c->next) {
            if (c->type != N_ELEM || (!tag_eq(c->tag, "td") && !tag_eq(c->tag, "th"))) continue;
            struct cstyle *st = c->style;
            if (skipped(c)) continue;
            int ml = st && st->ml > 0 ? st->ml : 0;
            int cx = rx + ml, top = cy + (st && st->mt > 0 ? st->mt : 0);
            int bgidx = -1;
            if (st && (st->has_bg || any_border(st))) {
                struct item *bg = additem(IT_RECT, c);
                if (bg) { bgidx = (int)(bg - items); fill_rect_item(bg, st, rx, cy, cw[ci]); }
            }
            int inw = cw[ci] - ml - hextra(st); if (inw < 0) inw = 0;
            int nsave = g_nfloat, bsave = g_fbase;
            g_fbase = g_nfloat;                 /* each cell is its own BFC */
            int inner = layout_block(c, cx + cx_off(st), top + cy_off(st), inw);
            { int b = float_max_bottom(nsave); if (b > inner) inner = b; }
            g_nfloat = nsave; g_fbase = bsave;
            int ch = (inner - top) + (st ? st->pb + st->border_w[2] : 0);
            if (st && ch < st->font_px) ch = st->font_px;
            ch = block_height(st, ch, -1);
            if (bgidx >= 0) items[bgidx].h = ch;
            if (cy + ch > maxb) maxb = cy + ch;
            rx += cw[ci];
            ci++;
        }
        cy = maxb;
    }
    return cy;
}

/* Reorder the finished display list by stacking level.
 *
 * The list is painted forward and hit-tested backward, so its ORDER is the
 * paint order -- there is no separate stacking-context tree. A stable sort by
 * the z-index inherited from the nearest positioned ancestor (additem stamps
 * it) buys the visible half of z-index for one pass: a z-index:10 dropdown
 * paints over the content that follows it in the document, and the hit test
 * agrees. What it does NOT buy is real stacking contexts -- a positioned
 * descendant of a z-index:1 parent is not trapped inside its parent's level,
 * because ONE INTEGER per item cannot express containment.
 *
 * The fix, when it earns its keep, is a composite key rather than a box tree:
 * stamp each item with the PATH of z levels from the root (the nearest N
 * stacking contexts, outermost first, each level tie-broken by the document
 * order of the context that created it) and sort lexicographically. Two items
 * then compare at the level where their contexts diverge, which is exactly
 * CSS's painting order, and the display list stays flat -- neither the painter
 * nor the hit test changes. The cost is N ints per item (N = 4 covers every
 * real page) plus threading the path through additem the way g_z is threaded
 * now. It was left out here rather than approximated, because a half-built
 * stacking context is worse than an honest flat one: it reorders some pages
 * correctly and others arbitrarily.
 *
 * Skipped entirely when nothing set a z-index, which is the overwhelmingly
 * common case and keeps this off the hot path. */
static void zsort(void)
{
    int need = 0;
    for (int i = 0; i < nitem; i++) if (items[i].z) { need = 1; break; }
    if (!need || nitem < 2) return;
    int *idx = kmalloc(sizeof(int) * (unsigned long)nitem * 2);
    if (!idx) return;
    int *tmp = idx + nitem;
    for (int i = 0; i < nitem; i++) idx[i] = i;
    /* Bottom-up merge sort. `<` (not `<=`) on the right-hand run is what makes
     * it stable, so equal levels keep document order. */
    for (int width = 1; width < nitem; width *= 2) {
        for (int i = 0; i < nitem; i += 2 * width) {
            int m = i + width > nitem ? nitem : i + width;
            int r = i + 2 * width > nitem ? nitem : i + 2 * width;
            int a = i, b = m, o = i;
            while (a < m && b < r)
                tmp[o++] = (items[idx[b]].z < items[idx[a]].z) ? idx[b++] : idx[a++];
            while (a < m) tmp[o++] = idx[a++];
            while (b < r) tmp[o++] = idx[b++];
        }
        for (int i = 0; i < nitem; i++) idx[i] = tmp[i];
    }
    struct item *dst = kmalloc(sizeof(struct item) * (unsigned long)nitem);
    if (dst) {
        for (int i = 0; i < nitem; i++) dst[i] = items[idx[i]];
        for (int i = 0; i < nitem; i++) items[i] = dst[i];
        kfree(dst);
    }
    kfree(idx);
}

void layout_page(struct node *root, int canvas_w)
{
    layout_free();
    items = kmalloc(sizeof(struct item) * MAXITEM);
    nitem = 0; canvas = canvas_w; g_z = 0;
    g_nfloat = 0; g_fbase = 0; g_in_float = 0;
    g_clip_on = 0; g_clipx = g_clipy = g_clipw = g_cliph = 0;
    if (!items) { doc_h = 0; return; }
    /* <body> and <html> straight from the document. The tree builder always
     * produces both for a parsed page; the fallbacks cover a tree assembled
     * through the DOM API (the layout unit tests do exactly that). */
    struct node *body = root->doc ? dom_doc_body(root->doc) : 0;
    struct node *start = body ? body : root;
    struct cstyle *bst = start->style;
    int mx = bst ? (bst->ml>0?bst->ml:0) : 8;

    /* canvas background: html (else body) background propagates to the viewport */
    page_has_bg = 0;
    struct node *htmlel = root->doc ? dom_doc_element(root->doc) : 0;
    struct cstyle *hst = htmlel ? htmlel->style : 0;
    if (hst && hst->has_bg)      { page_has_bg = 1; page_bg = hst->background; }
    else if (bst && bst->has_bg) { page_has_bg = 1; page_bg = bst->background; }

    doc_h = layout_block(start, mx, bst&&bst->mt>0?bst->mt:8, canvas_w - 2*mx);
    /* The initial containing block contains its floats too: a page whose last
     * content is a tall float must still scroll far enough to see it. */
    { int b = float_max_bottom(0); if (b > doc_h) doc_h = b; }
    zsort();
}

/* Page (canvas) background, propagated from <html>/<body>. 1 if set. */
int layout_page_bg(uint32_t *out) { if (page_has_bg && out) *out = page_bg; return page_has_bg; }

/* Fetch + decode up to `max` of the reserved <img> boxes (bounded, blocking).
 * Called after layout_page so layout itself never touches the network. */
int layout_load_images(int max)
{
    int loaded = 0;
    for (int i = 0; i < nitem && loaded < max; i++) {
        struct item *it = &items[i];
        if (it->type != IT_IMAGE || it->img || !it->imgsrc) continue;
        uint8_t *buf; int blen;
        if (res_fetch(it->imgsrc, &buf, &blen) != 0) continue;
        struct image *holder = kmalloc(sizeof *holder);
        struct image tmp;
        if (holder && img_decode(buf, blen, &tmp) == 0) {
            *holder = tmp; it->img = holder; loaded++;
            /* height was only a guess: snap the box to the real aspect ratio */
            if (it->h_auto && tmp.w > 0 && tmp.h > 0)
                it->h = it->w * tmp.h / tmp.w;
        }
        else if (holder) kfree(holder);
        kfree(buf);
    }
    return loaded;
}

int layout_height(void) { return doc_h; }
int layout_count(void) { return nitem; }
const struct item *layout_items(void) { return items; }
void layout_free(void) {
    if (items) {
        for (int i = 0; i < nitem; i++) {
            if (items[i].img) {
                img_free(items[i].img);
                kfree(items[i].img);
            }
        }
        kfree(items);
        items = 0;
    }
    nitem = 0; doc_h = 0;
    page_has_bg = 0;    /* don't keep filling the viewport with the previous page's background */
}
