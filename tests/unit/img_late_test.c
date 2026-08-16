/* tests/unit/img_late_test.c -- images that arrive LATE.
 *
 * WHAT WAS WRONG, measured before this file existed. c/apps/browser/layout.c's
 * layout_page() opens with layout_free(), and layout_free() frees every
 * decoded bitmap the display list owns. The embedder called
 * layout_load_images() exactly ONCE per navigation, right after the first
 * layout. Put those two together:
 *
 *   - a page that re-laid-out for ANY reason -- one script mutation, which is
 *     every real page within a second of load -- lost every picture it had,
 *     permanently and silently;
 *   - an <img> that did not exist at first layout (script inserted it, or
 *     moved its URL out of data-src into src) was never fetched at all;
 *   - and the 17th image on the page was never fetched either, because the one
 *     pass was capped at 16.
 *
 * bilibili.com drew every video card as an empty grey box for exactly the
 * second reason, and it is the most common visible defect in the corpus.
 *
 * WHAT THIS FILE MEASURES. layout.c's half of the fix: a decoded-image cache
 * keyed by the src attribute, so a bitmap survives a re-layout and is never
 * fetched twice; a load pass whose budget bounds NEW network work only, so it
 * can be run every frame; a negative entry, so a broken image is not
 * re-requested forever; and two bounds that refuse OUT LOUD instead of eating
 * memory. The counters below are the point -- an assertion that an image is
 * "present" cannot tell a cache hit from a silent refetch, and a refetch every
 * frame is the way this fix fails in production.
 *
 * WHAT IT DOES NOT MEASURE, stated rather than implied: browser.c's half --
 * settle_frame() calling the pass once a frame, and load_late_images() queueing
 * the batch before decoding it. Those need the network and the event loop, and
 * they are proven on the device with `make scoreboard-1 SITE=bilibili`.
 *
 * NEGATIVE CONTROLS (tests/imglate.mk), each of which this suite MUST fail:
 *   IMG_NEGCTL_NOCACHE   nothing is remembered -- the behaviour being replaced
 *   IMG_NEGCTL_NOBOUND   the byte bound never refuses
 *   IMG_NEGCTL_NONEG     a broken URL is re-requested by every pass
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "layout.h"
#include "css.h"
#include "dom.h"

int layout_img_cached(const char *url);
int layout_images_pending(void);
void layout_images_reset(void);
void layout_img_stats(int *ents, int *kbytes, int *refused);

void *kmalloc(unsigned long n){ return malloc(n); }
void  kfree(void *p){ free(p); }
/* Box geometry only; glyph advances are not what this file is about. */
int text_measure(const char *s, int len, int px, int mono){ (void)s;(void)mono; return len*(px/2); }

static int checks, fails;
#define CHECK(c,m) do{ checks++; if(!(c)){ printf("  FAIL: %s\n",(m)); fails++; } }while(0)
#define EQ(got,want,m) do{ checks++; if((got)!=(want)){ \
        printf("  FAIL: %s -- got %d, want %d\n",(m),(int)(got),(int)(want)); fails++; } }while(0)

/* ---------------------------------------------------------------------------
 * The fake network and the fake codec.
 *
 * A URL is its own description: "u<W>x<H>" serves a body that decodes to a
 * W x H image; "bad" serves a body that will not decode; "gone" does not fetch
 * at all. Every crossing is COUNTED per URL, which is what makes "did not
 * refetch" an assertion instead of an opinion.
 */
#define MAXU 64
static char  u_name[MAXU][64];
static int   u_fetch[MAXU], u_decode[MAXU];
static int   u_n;

static int uidx(const char *u){
    for(int i=0;i<u_n;i++) if(!strcmp(u_name[i],u)) return i;
    if(u_n>=MAXU) { printf("  FAIL: test URL table full\n"); fails++; return 0; }
    snprintf(u_name[u_n],sizeof u_name[0],"%s",u);
    u_fetch[u_n]=0; u_decode[u_n]=0;
    return u_n++;
}
static int fetches(const char *u){ return u_fetch[uidx(u)]; }
static int decodes(const char *u){ return u_decode[uidx(u)]; }
static void counters_reset(void){ for(int i=0;i<u_n;i++){ u_fetch[i]=0; u_decode[i]=0; } }

/* Live decoded buffers, so a double free or a leak is a FAILURE and not a
 * crash that might not happen. */
#define MAXLIVE 512
static uint8_t *live[MAXLIVE];
static int live_n;
static void live_add(uint8_t *p){
    if(live_n<MAXLIVE) live[live_n++]=p;
    else { printf("  FAIL: live table full\n"); fails++; }
}
static int live_del(uint8_t *p){
    for(int i=0;i<live_n;i++) if(live[i]==p){ live[i]=live[--live_n]; return 1; }
    return 0;
}
static int img_frees, double_frees;

int res_fetch(const char *u, uint8_t **b, int *l){
    int i = uidx(u);
    u_fetch[i]++;
    if(!strncmp(u,"gone",4)) return -1;          /* will not fetch */
    int n = (int)strlen(u)+1;
    uint8_t *p = malloc(n);
    memcpy(p,u,n);
    *b = p; *l = n;
    return 0;
}
int img_decode(const uint8_t *p, int n, struct image *o){
    (void)n;
    const char *u = (const char *)p;
    u_decode[uidx(u)]++;
    if(!strncmp(u,"bad",3)) return -1;           /* fetched, will not decode */
    int w=0,h=0;
    if(sscanf(u,"u%dx%d",&w,&h)!=2 || w<=0 || h<=0) return -1;
    o->w=w; o->h=h;
    o->rgba = malloc((size_t)w*h*4);
    live_add(o->rgba);
    return 0;
}
void img_free(struct image *o){
    if(!o || !o->rgba) return;
    img_frees++;
    if(!live_del(o->rgba)) { double_frees++; printf("  FAIL: img_free of a bitmap that is not live\n"); fails++; return; }
    free(o->rgba);
    o->rgba = 0;
}

/* ---- the page ---------------------------------------------------------- */
static struct node *g_root;
static char g_css[8192];

static int tag_is(const char *t, const char *lit){ int i=0; for(;lit[i];i++) if(t[i]!=lit[i]) return 0; return t[i]==0; }
static int collect_style(struct node *n, char *out, int o, int max){
    if(!n) return o;
    if(n->type==N_ELEM && tag_is(n->tag,"style"))
        for(struct node *c=n->first_child;c;c=c->next)
            if(c->type==N_TEXT && c->text)
                for(int i=0;i<c->textlen && o<max-1;i++) out[o++]=c->text[i];
    for(struct node *c=n->first_child;c;c=c->next) o=collect_style(c,out,o,max);
    return o;
}
static void page(const char *html, int cw){
    g_root = dom_parse(html,(int)strlen(html));
    int cl = collect_style(g_root,g_css,0,(int)sizeof g_css);
    css_apply(g_root,g_css,cl);
    layout_page(g_root,cw);
}
/* Lay the SAME document out again -- what restyle()/ce_settle()/a resize do,
 * and what used to destroy every picture on the page. */
static void relayout(int cw){
    css_apply(g_root,g_css,(int)strlen(g_css));
    layout_page(g_root,cw);
}
static struct node *find_id(struct node *n, const char *id){
    if(n->type==N_ELEM){ const char *a=dom_attr(n,"id"); if(a && !strcmp(a,id)) return n; }
    for(struct node *c=n->first_child;c;c=c->next){ struct node *r=find_id(c,id); if(r) return r; }
    return 0;
}

/* How many IT_IMAGE items currently hold a decoded bitmap. */
static int painted(void){
    const struct item *it = layout_items();
    int n=0;
    for(int i=0;i<layout_count();i++) if(it[i].type==IT_IMAGE && it[i].img) n++;
    return n;
}
/* The bitmap attached to the <img> with this id, or 0. */
static struct image *img_of(const char *id){
    struct node *e = find_id(g_root,id);
    const struct item *it = layout_items();
    for(int i=0;i<layout_count();i++)
        if(it[i].type==IT_IMAGE && it[i].node==e) return it[i].img;
    return 0;
}
static int cache_ents(void){ int e=0; layout_img_stats(&e,0,0); return e; }
static int cache_kb(void){ int k=0; layout_img_stats(0,&k,0); return k; }
static int cache_refused(void){ int r=0; layout_img_stats(0,0,&r); return r; }

static void teardown(void){
    layout_free();
    layout_images_reset();
    if(g_root){ dom_free(g_root); g_root=0; }
    counters_reset();
}

/* ======================================================================== */
int main(void)
{
    printf("img_late_test: images discovered after the first layout\n");

    /* --- 1. the baseline: two images load, once each ---------------------- */
    printf("\n1. first load\n");
    page("<html><body>"
         "<img id=a src='u4x4' width='40'>"
         "<img id=b src='u8x2' width='40'>"
         "</body></html>", 400);
    EQ(layout_load_images(16), 2, "first pass loads both images");
    EQ(painted(), 2, "both items hold a bitmap");
    EQ(fetches("u4x4"), 1, "u4x4 fetched once");
    EQ(fetches("u8x2"), 1, "u8x2 fetched once");
    EQ(cache_ents(), 2, "two cache entries");
    EQ(layout_images_pending(), 0, "nothing owed after the pass");
    /* h_auto: width 40, source 8x2 -> height 10 */
    { const struct item *it=layout_items(); int h=-1;
      struct node *e=find_id(g_root,"b");
      for(int i=0;i<layout_count();i++) if(it[i].type==IT_IMAGE && it[i].node==e) h=it[i].h;
      EQ(h, 10, "b's height snapped to the decoded 8x2 aspect"); }

    /* --- 2. THE DEFECT: a re-layout must not lose them -------------------- */
    printf("\n2. re-layout keeps the pictures, and costs no network\n");
    { struct image *before = img_of("a");
      int frees0 = img_frees;
      relayout(400);
      EQ(painted(), 2, "both images survived layout_page()");
      EQ(img_frees - frees0, 0, "a re-layout freed no bitmap");
      CHECK(img_of("a") == before, "the SAME bitmap is re-attached, not a copy");
      EQ(fetches("u4x4"), 1, "still one fetch for u4x4");
      EQ(decodes("u4x4"), 1, "still one decode for u4x4");
      /* Running the pass again must also cost nothing. */
      layout_load_images(16);
      EQ(fetches("u4x4"), 1, "a second pass over a cached page fetches nothing");
      EQ(cache_ents(), 2, "and adds no entry"); }

    /* --- 3. an <img> INSERTED after load ---------------------------------- */
    printf("\n3. an image the script inserted\n");
    { struct node *body = find_id(g_root,"a")->parent;
      struct node *n = dom_create_element(g_root->doc,"img",3);
      dom_set_attr(n,"id","c");
      dom_set_attr(n,"src","u2x2");
      dom_set_attr(n,"width","40");
      dom_append_child(body,n);
      relayout(400);
      EQ(painted(), 2, "before the pass the new one is empty, the old two are not");
      EQ(layout_images_pending(), 1, "layout says exactly one image is owed");
      EQ(layout_load_images(16), 1, "the pass decodes exactly ONE new image");
      EQ(painted(), 3, "all three painted");
      EQ(fetches("u2x2"), 1, "the new one fetched once");
      EQ(fetches("u4x4"), 1, "and the old ones not refetched");
      EQ(layout_images_pending(), 0, "nothing owed"); }

    /* --- 4. the lazy-load swap: data-src -> src --------------------------- */
    printf("\n4. data-src moved into src by script\n");
    teardown();
    page("<html><body><img id=l data-src='u6x3' width='60'></body></html>", 400);
    EQ(layout_load_images(16), 1, "data-src is read as a fallback at first layout");
    EQ(fetches("u6x3"), 1, "and fetched");
    { struct node *e = find_id(g_root,"l");
      dom_set_attr(e,"src","u9x3");        /* the real image arrives */
      relayout(400);
      EQ(layout_images_pending(), 1, "the swapped-in src is new work");
      layout_load_images(16);
      EQ(fetches("u9x3"), 1, "the real image is fetched");
      CHECK(img_of("l") != 0, "and painted");
      { struct image *im = img_of("l");
        EQ(im ? im->w : 0, 9, "the item holds the SWAPPED image, not the placeholder"); } }

    /* --- 5. the per-call budget bounds NEW work only ---------------------- */
    printf("\n5. the budget, and the drain\n");
    teardown();
    page("<html><body>"
         "<img id=seed src='u4x4' width='10'><img src='u5x5' width='10'>"
         "<img src='u6x6' width='10'><img src='u7x7' width='10'>"
         "<img src='u8x8' width='10'>"
         "</body></html>", 400);
    EQ(layout_images_pending(), 5, "five images owed");
    layout_load_images(2);
    EQ(painted(), 2, "a budget of 2 loads 2");
    EQ(layout_images_pending(), 3, "three still owed");
    layout_load_images(2);
    EQ(painted(), 4, "the next pass loads 2 more");
    EQ(layout_images_pending(), 1, "one owed");
    layout_load_images(2);
    EQ(painted(), 5, "and the last one lands");
    EQ(layout_images_pending(), 0, "the drain terminates");
    EQ(cache_ents(), 5, "five entries");
    /* THE BUDGET MUST NOT BE SPENT ON CACHE HITS. This is the assertion that
     * separates the fix from a plausible wrong one: charge re-attachments to
     * the budget and a page holding five cached pictures never reaches the
     * sixth, however many frames it is given. */
    { struct node *body = find_id(g_root,"seed")->parent;
      struct node *n = dom_create_element(g_root->doc,"img",3);
      dom_set_attr(n,"id","late"); dom_set_attr(n,"src","u1x1"); dom_set_attr(n,"width","10");
      dom_append_child(body,n);
      relayout(400);
      counters_reset();
      EQ(painted(), 5, "the five cached ones are back before the pass runs");
      layout_load_images(1);
      EQ(painted(), 6, "a budget of 1 still reaches the ONE new image");
      EQ(fetches("u1x1"), 1, "it was fetched");
      EQ(fetches("u4x4"), 0, "and no cached URL was refetched to get there"); }

    /* --- 6. a broken image is asked for ONCE ------------------------------ */
    printf("\n6. negative entries\n");
    teardown();
    page("<html><body>"
         "<img id=g src='gone' width='10'>"        /* will not fetch */
         "<img id=d src='bad' width='10'>"         /* fetches, will not decode */
         "<img id=k src='u3x3' width='10'>"
         "</body></html>", 400);
    layout_load_images(16);
    EQ(painted(), 1, "only the good one paints");
    EQ(fetches("gone"), 1, "the unfetchable one was tried once");
    EQ(fetches("bad"), 1, "the undecodable one was fetched once");
    EQ(decodes("bad"), 1, "and decoded once");
    EQ(layout_images_pending(), 0, "neither is still owed -- both are answered");
    for(int pass=0; pass<3; pass++){ relayout(400); layout_load_images(16); }
    EQ(fetches("gone"), 1, "three more passes: still ONE request for the unfetchable URL");
    EQ(fetches("bad"), 1, "still ONE request for the undecodable URL");
    EQ(decodes("bad"), 1, "and still ONE decode attempt");
    EQ(fetches("u3x3"), 1, "and the good one was never refetched either");

    /* --- 7. the byte bound refuses, and says so --------------------------- */
    printf("\n7. the memory bound (built with IMGCACHE_BYTES=%ld)\n", (long)IMGCACHE_BYTES);
    teardown();
    /* 512x512x4 = 1 MiB each. The first fits the 1 MiB test bound exactly; the
     * second cannot, and must be refused rather than silently held. */
    page("<html><body>"
         "<img id=p src='u512x512' width='20'>"
         "<img id=q src='u511x511' width='20'>"
         "</body></html>", 400);
    layout_load_images(16);
    EQ(cache_refused(), 1, "exactly one decode refused by the byte bound");
    EQ(cache_kb(), 1024, "the cache holds 1 MiB and not a byte more");
    EQ(painted(), 2, "the refused image still paints on the frame that decoded it");
    /* ...and is gone after a re-layout, which is what the refusal means. */
    relayout(400);
    layout_load_images(16);
    EQ(painted(), 1, "after a re-layout only the cached one comes back");
    EQ(fetches("u511x511"), 1, "and the refused URL is NOT fetched again");

    /* --- 8. no leaks, no double frees ------------------------------------- */
    printf("\n8. ownership\n");
    teardown();
    EQ(double_frees, 0, "no bitmap was freed twice");
    EQ(live_n, 0, "every decoded bitmap was freed exactly once");

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
