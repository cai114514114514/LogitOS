#include "layout.h"
#include "css.h"

void *kmalloc(unsigned long);
void  kfree(void *);
void *memset(void *, int, unsigned long);
int   text_measure(const char *s, int len, int px, int mono);
int   res_fetch(const char *url, uint8_t **buf, int *len);   /* net/http.c */

#define MAXITEM 8192
static struct item *items;
static int nitem;
static int doc_h;
static int canvas;

static struct item *additem(int type)
{
    if (!items || nitem >= MAXITEM) return 0;
    struct item *it = &items[nitem++];
    memset(it, 0, sizeof *it);
    it->type = type;
    return it;
}

static int sp(int c){ return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'; }
static int tag_eq(const char *t, const char *lit){ int i=0; for(;lit[i];i++) if(t[i]!=lit[i]) return 0; return t[i]==0; }
static int atoi_(const char *s){ int n=0; while(*s>='0'&&*s<='9') n=n*10+(*s++-'0'); return n; }

/* ---- inline flow ---- */
/* current pen for inline content within a block */
struct iflow { int x0, x1, x, y, lineh, line_started; };

static void newline(struct iflow *f)
{
    if (f->line_started) f->y += f->lineh;
    f->x = f->x0; f->lineh = 0; f->line_started = 0;
}

/* place a text run (one element's text) into the flow, wrapping on words */
static void flow_text(struct iflow *f, const char *s, int len, struct cstyle *st, const char *href)
{
    int px = st->font_px, mono = st->mono;
    int lh = st->line_px > px ? st->line_px : px*5/4;
    int spacew = text_measure(" ", 1, px, mono);
    int i = 0;
    while (i < len) {
        while (i < len && sp(s[i])) i++;                 /* collapse whitespace */
        if (i >= len) break;
        int ws = i; while (i < len && !sp(s[i])) i++;     /* one word [ws,i) */
        int wlen = i - ws;
        int ww = text_measure(s + ws, wlen, px, mono);
        if (f->line_started && f->x + spacew + ww > f->x1 && ww <= (f->x1 - f->x0)) {
            newline(f);                                   /* wrap */
        }
        if (f->line_started) f->x += spacew;
        struct item *it = additem(IT_TEXT);
        if (!it) return;
        it->x = f->x; it->w = ww; it->text = s + ws; it->len = wlen;
        it->font_px = px; it->bold = st->bold; it->italic = st->italic; it->mono = mono;
        it->underline = st->underline; it->color = st->color; it->href = href;
        f->x += ww;
        f->line_started = 1;
        if (lh > f->lineh) f->lineh = lh;
        it->y = f->y;                                     /* top of line; baseline handled in paint */
        it->h = lh;
    }
}

static void flow_node(struct iflow *f, struct node *c, const char *href);

/* flow all inline descendants of `n` (text + inline elements + img) */
static void flow_children(struct iflow *f, struct node *n, const char *href)
{
    for (struct node *c = n->first_child; c; c = c->next)
        flow_node(f, c, href);
}

/* flow a single node into the inline context */
static void flow_node(struct iflow *f, struct node *c, const char *href)
{
    struct cstyle *st = c->style;
    if (st && st->display == DISP_NONE) return;
    if (c->parent && c->parent->style && ((struct cstyle *)c->parent->style)->display == DISP_NONE) return;

    if (c->type == N_TEXT) {
        struct cstyle *ps = c->parent && c->parent->style ? c->parent->style : st;
        if (ps) flow_text(f, c->text, c->textlen, ps, href);
        return;
    }
    if (c->type != N_ELEM) return;

    const char *h2 = href;
    if (tag_eq(c->tag, "a")) { const char *u = dom_attr(c, "href"); if (u) h2 = u; }
    if (tag_eq(c->tag, "br")) { newline(f); return; }

    if (tag_eq(c->tag, "img")) {
        /* Reserve the box from CSS/HTML width&height; the actual pixels are
         * fetched later by layout_load_images() so layout never blocks. */
        int iw = 0, ih = 0;
        if (st && st->has_w && !st->w_pct) iw = st->width;
        if (st && st->has_h && !st->h_pct) ih = st->height;
        if (!iw) { const char *wa = dom_attr(c, "width");  if (wa) iw = atoi_(wa); }
        if (!ih) { const char *ha = dom_attr(c, "height"); if (ha) ih = atoi_(ha); }
        if (iw <= 0) iw = ih > 0 ? ih : 24;
        if (ih <= 0) ih = iw;
        if (iw > f->x1 - f->x0) { int s2 = f->x1 - f->x0; ih = ih*s2/iw; iw = s2; }
        if (f->line_started && f->x + iw > f->x1) newline(f);
        struct item *it = additem(IT_IMAGE);
        if (it) { it->x = f->x; it->y = f->y; it->w = iw; it->h = ih;
                  it->img = 0; it->imgsrc = dom_attr(c, "src"); it->href = h2; }
        f->x += iw; f->line_started = 1; if (ih > f->lineh) f->lineh = ih;
        return;
    }

    flow_children(f, c, h2);                               /* descend inline element */
}

static int is_block(struct node *n)
{
    if (!n || n->type != N_ELEM) return 0;
    struct cstyle *st = n->style;
    return st && (st->display == DISP_BLOCK);
}
static int has_block_child(struct node *n)
{
    for (struct node *c = n->first_child; c; c = c->next) if (is_block(c)) return 1;
    return 0;
}

/* lay out the children of block `n` whose content box starts at (x,y) width w;
 * returns the bottom y. */
static int layout_block(struct node *n, int x, int y, int w)
{
    int cy = y;
    /* if this block has no block children, the whole content is one inline
     * context. */
    if (!has_block_child(n)) {
        struct iflow f = { x, x + w, x, cy, 0, 0 };
        flow_children(&f, n, 0);
        newline(&f);
        return f.y;
    }
    for (struct node *c = n->first_child; c; c = c->next) {
        struct cstyle *st = c->style;
        if (st && st->display == DISP_NONE) continue;
        if (is_block(c)) {
            int ml = st->ml<0?0:st->ml, mr = st->mr<0?0:st->mr;
            int bx = x + ml;
            int bw = st->has_w && !st->w_pct ? st->width
                   : st->has_w && st->w_pct ? w*st->width/100
                   : w - ml - mr;
            if (bw < 0) bw = 0;
            if (st->ml < 0 && st->mr < 0 && st->has_w) bx = x + (w - bw)/2;   /* margin:auto center */
            cy += st->mt > 0 ? st->mt : 0;
            int top = cy;
            int bgidx = -1;
            if (st->has_bg || st->border_w) {
                struct item *bg = additem(IT_RECT);
                if (bg) { bgidx = (int)(bg - items);
                    bg->x = bx; bg->y = top; bg->w = bw;
                    bg->bg = st->background; bg->has_bg = st->has_bg;
                    bg->border_w = st->border_w; bg->border_color = st->border_color;
                    bg->radius = st->radius; }
            }
            int inner = layout_block(c, bx + st->pl, top + st->pt, bw - st->pl - st->pr);
            int ch = (inner - top) + st->pb;
            if (st->has_h && !st->h_pct && st->height > ch) ch = st->height;
            if (ch < st->font_px) ch = st->font_px;          /* min line */
            if (bgidx >= 0) items[bgidx].h = ch;
            cy = top + ch + (st->mb > 0 ? st->mb : 0);
        } else {
            /* run of inline siblings: gather until next block */
            struct iflow f = { x, x + w, x, cy, 0, 0 };
            while (c && !is_block(c)) {
                struct cstyle *cs = c->style;
                if (!(cs && cs->display == DISP_NONE)) flow_node(&f, c, 0);
                struct node *nx = c->next;
                if (!nx || is_block(nx)) break;
                c = nx;
            }
            newline(&f);
            cy = f.y;
        }
    }
    return cy;
}

void layout_page(struct node *root, int canvas_w)
{
    layout_free();
    items = kmalloc(sizeof(struct item) * MAXITEM);
    nitem = 0; canvas = canvas_w;
    if (!items) { doc_h = 0; return; }
    /* find <body> (or use root) */
    struct node *body = 0;
    for (struct node *h = root->first_child; h && !body; h = h->next)
        for (struct node *b = h->first_child; b; b = b->next)
            if (b->type==N_ELEM && tag_eq(b->tag, "body")) { body = b; break; }
    struct node *start = body ? body : root;
    struct cstyle *bst = start->style;
    int mx = bst ? (bst->ml>0?bst->ml:0) : 8;
    doc_h = layout_block(start, mx, bst&&bst->mt>0?bst->mt:8, canvas_w - 2*mx);
}

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
        if (holder && img_decode(buf, blen, &tmp) == 0) { *holder = tmp; it->img = holder; loaded++; }
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
}
