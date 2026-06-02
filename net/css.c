#include "css.h"

void *kmalloc(unsigned long);
void *memset(void *, int, unsigned long);
void *memcpy(void *, const void *, unsigned long);

static int lc(int c){ return (c>='A'&&c<='Z')?c+32:c; }
static int sp(int c){ return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'; }
static int ieq(const char *a, const char *b){ while(*a&&lc(*a)==lc(*b)){a++;b++;} return lc(*a)==lc(*b); }
static int neq(const char *a, const char *b, int n){ for(int i=0;i<n;i++) if(lc(a[i])!=lc(b[i])) return 0; return 1; }
static int slen(const char *s){ int n=0; while(s[n])n++; return n; }
static int has_sub(const char *h, const char *n){ for(;*h;h++){ const char*a=h,*b=n; while(*a&&*b&&lc(*a)==lc(*b)){a++;b++;} if(!*b)return 1; } return 0; }

/* ---- value parsers ---- */
struct named { const char *n; uint32_t c; };
static const struct named NAMED[] = {
    {"black",0x000000},{"white",0xffffff},{"red",0xff0000},{"green",0x008000},
    {"blue",0x0000ff},{"gray",0x808080},{"grey",0x808080},{"silver",0xc0c0c0},
    {"maroon",0x800000},{"yellow",0xffff00},{"olive",0x808000},{"lime",0x00ff00},
    {"aqua",0x00ffff},{"cyan",0x00ffff},{"teal",0x008080},{"navy",0x000080},
    {"fuchsia",0xff00ff},{"magenta",0xff00ff},{"purple",0x800080},{"orange",0xffa500},
    {"pink",0xffc0cb},{"brown",0xa52a2a},{"gold",0xffd700},{"darkgray",0xa9a9a9},
    {"lightgray",0xd3d3d3},{"lightgrey",0xd3d3d3},{"transparent",0xff000000},{0,0}
};
static int hexv(int c){ if(c>='0'&&c<='9')return c-'0'; c=lc(c); if(c>='a'&&c<='f')return c-'a'+10; return 0; }

/* parse a color token; returns 0xRRGGBB, or 0xFF000000 if unrecognized/transparent */
static uint32_t parse_color(const char *s)
{
    while(sp(*s))s++;
    if(*s=='#'){ s++; int n=0; while(s[n]&&!sp(s[n])&&s[n]!=';'&&s[n]!=')')n++;
        if(n>=6) return (hexv(s[0])<<20)|(hexv(s[1])<<16)|(hexv(s[2])<<12)|(hexv(s[3])<<8)|(hexv(s[4])<<4)|hexv(s[5]);
        if(n>=3) return (hexv(s[0])<<20)|(hexv(s[0])<<16)|(hexv(s[1])<<12)|(hexv(s[1])<<8)|(hexv(s[2])<<4)|hexv(s[2]);
        return 0xFF000000; }
    if(neq(s,"rgb",3)){ while(*s&&*s!='(')s++; if(*s)s++; int v[3]={0,0,0},k=0;
        while(*s&&*s!=')'&&k<3){ while(sp(*s))s++; int x=0,any=0; while(*s>='0'&&*s<='9'){x=x*10+(*s++-'0');any=1;} if(any)v[k++]=x; while(*s==','||sp(*s))s++; if(*s>=')')break; }
        return ((v[0]&0xff)<<16)|((v[1]&0xff)<<8)|(v[2]&0xff); }
    for(int i=0;NAMED[i].n;i++) if(ieq(s,NAMED[i].n)) return NAMED[i].c;
    return 0xFF000000;                                  /* unknown -> sentinel */
}

/* parse a length; returns px given the current font_px; *pct set if it was % */
static int parse_len(const char *s, int font_px, int *pct)
{
    while(sp(*s))s++;
    if(pct)*pct=0;
    int neg=0; if(*s=='-'){neg=1;s++;}
    int n=0,any=0; while(*s>='0'&&*s<='9'){n=n*10+(*s++-'0');any=1;}
    int frac=0,fd=1; if(*s=='.'){s++; while(*s>='0'&&*s<='9'){frac=frac*10+(*s++-'0');fd*=10;}}
    if(!any && !frac) return 0;
    if(*s=='%'){ if(pct)*pct=1; return neg?-n:n; }
    if(neq(s,"em",2)){ int px=(n*font_px)+(frac*font_px/fd); return neg?-px:px; }
    /* px or unitless */
    int px=n; return neg?-px:px;
}

/* ---- selectors + rules ---- */
struct ssel { char tag[16]; char cls[3][32]; int ncls; char id[32]; };
struct decl { char prop[20]; char val[80]; };
struct rule { struct ssel sels[4]; int nsel; struct decl decls[24]; int ndecl; int spec; int order; int origin; };

#define MAXRULE 512
static struct rule rules[MAXRULE];
static int nrule, ua_rules;
static int order_ctr;

static int parse_ssel(const char *s, const char *e, struct ssel *out)
{
    memset(out,0,sizeof *out);
    char buf[64]; int bi=0;
    int mode=0;                                          /* 0 tag,1 class,2 id */
    #define FLUSH() do{ buf[bi]=0; if(bi){ if(mode==1&&out->ncls<3){memcpy(out->cls[out->ncls++],buf,bi+1);} else if(mode==2){memcpy(out->id,buf,bi+1);} else {int j=0;for(;buf[j]&&j<15;j++)out->tag[j]=lc(buf[j]);out->tag[j]=0;} } bi=0; }while(0)
    for(const char *p=s;p<e;p++){
        if(*p=='.'){ FLUSH(); mode=1; }
        else if(*p=='#'){ FLUSH(); mode=2; }
        else if(*p=='*'){ /* universal: leave tag empty */ }
        else if(bi<63) buf[bi++]=*p;
    }
    FLUSH();
    #undef FLUSH
    return 0;
}

/* parse a CSS string into rules[] (origin: 0=UA, 1=page). */
static void parse_css(const char *css, int len, int origin)
{
    const char *p=css, *end=css+len;
    while(p<end && nrule<MAXRULE){
        while(p<end && sp(*p))p++;
        if(p>=end)break;
        if(*p=='<'){ while(p<end && *p!='>')p++; if(p<end)p++; continue; }   /* skip stray markup */
        if(p+3<end && p[0]=='/'&&p[1]=='*'){ p+=2; while(p+1<end && !(p[0]=='*'&&p[1]=='/'))p++; p+=2; continue; }
        if(*p=='@'){ /* skip at-rule up to ; or matching } */
            int depth=0; while(p<end){ if(*p=='{')depth++; else if(*p=='}'){if(--depth<=0){p++;break;}} else if(*p==';'&&depth==0){p++;break;} p++; } continue; }
        const char *selstart=p;
        while(p<end && *p!='{')p++;
        const char *selend=p;
        if(p>=end)break;
        p++;                                             /* past { */
        const char *declstart=p;
        while(p<end && *p!='}')p++;
        const char *declend=p;
        if(p<end)p++;                                    /* past } */
        /* split selector list by comma -> one rule each */
        const char *ss=selstart;
        while(ss<selend){
            const char *comma=ss; while(comma<selend && *comma!=',')comma++;
            if(nrule>=MAXRULE)break;
            struct rule *r=&rules[nrule];
            memset(r,0,sizeof *r);
            r->origin=origin; r->order=order_ctr++;
            /* split into simple selectors by whitespace (descendant combinator) */
            const char *t=ss;
            while(t<comma && r->nsel<4){
                while(t<comma && sp(*t))t++;
                const char *w=t; while(w<comma && !sp(*w))w++;
                if(w>t){ parse_ssel(t,w,&r->sels[r->nsel]);
                    struct ssel *S=&r->sels[r->nsel];
                    r->spec += (S->id[0]?100:0) + S->ncls*10 + (S->tag[0]?1:0);
                    r->nsel++; }
                t=w;
            }
            if(r->nsel==0){ ss=(comma<selend)?comma+1:selend; continue; }
            /* declarations */
            const char *d=declstart;
            while(d<declend && r->ndecl<24){
                while(d<declend && (sp(*d)||*d==';'))d++;
                const char *ps=d; while(d<declend && *d!=':'&&*d!=';'&&*d!='}')d++;
                if(d>=declend || *d!=':'){ while(d<declend&&*d!=';')d++; continue; }
                const char *pe=d; d++;                    /* past : */
                const char *vs=d; while(d<declend && *d!=';'&&*d!='}')d++;
                const char *ve=d;
                while(pe>ps && sp(pe[-1]))pe--;            /* trim */
                while(vs<ve && sp(*vs))vs++;
                while(ve>vs && sp(ve[-1]))ve--;
                struct decl *dd=&r->decls[r->ndecl];
                int pi=0; for(const char *k=ps;k<pe&&pi<19;k++)dd->prop[pi++]=lc(*k); dd->prop[pi]=0;
                int vi=0; for(const char *k=vs;k<ve&&vi<79;k++)dd->val[vi++]=*k; dd->val[vi]=0;
                if(pi)r->ndecl++;
            }
            nrule++;
            ss=(comma<selend)?comma+1:selend;
        }
    }
}

/* ---- UA stylesheet ---- */
static const char UA_CSS[] =
    "body{display:block;margin:8px}"
    "div,p,h1,h2,h3,h4,h5,h6,ul,ol,li,pre,header,footer,section,article,nav,main,blockquote,figure,figcaption,table,form{display:block}"
    "h1{font-size:32px;font-weight:bold;margin:14px 0}"
    "h2{font-size:24px;font-weight:bold;margin:12px 0}"
    "h3{font-size:19px;font-weight:bold;margin:10px 0}"
    "h4{font-size:16px;font-weight:bold;margin:10px 0}"
    "h5{font-size:13px;font-weight:bold;margin:8px 0}"
    "h6{font-size:11px;font-weight:bold;margin:8px 0}"
    "p{margin:8px 0}"
    "a{color:#1a0dab;text-decoration:underline}"
    "b{font-weight:bold}strong{font-weight:bold}"
    "i{font-style:italic}em{font-style:italic}"
    "ul{display:block;margin:8px 0;padding-left:28px}ol{display:block;margin:8px 0;padding-left:28px}"
    "li{display:list-item}"
    "pre{font-family:monospace;margin:8px 0}code{font-family:monospace}";

void css_init(void)
{
    nrule = 0; order_ctr = 0;
    parse_css(UA_CSS, (int)sizeof(UA_CSS)-1, 0);
    ua_rules = nrule;
}

/* ---- matching + apply ---- */
static int ssel_match(const struct ssel *s, const struct node *n)
{
    if(n->type!=N_ELEM) return 0;
    if(s->tag[0] && !ieq(s->tag, n->tag)) return 0;
    if(s->id[0]){ const char *id=dom_attr(n,"id"); if(!id||!ieq(id,s->id)) return 0; }
    for(int i=0;i<s->ncls;i++){
        const char *cl=dom_attr(n,"class"); if(!cl) return 0;
        int found=0; const char *p=cl;
        while(*p){ while(*p&&sp(*p))p++; const char *w=p; while(*p&&!sp(*p))p++;
            int wl=(int)(p-w); int cll=slen(s->cls[i]);
            if(wl==cll && neq(w,s->cls[i],wl)){found=1;break;} }
        if(!found) return 0;
    }
    return 1;
}
/* full selector (descendant chain) matches node? */
static int rule_match(const struct rule *r, const struct node *n)
{
    if(!ssel_match(&r->sels[r->nsel-1], n)) return 0;
    int si = r->nsel-2;
    const struct node *anc = n->parent;
    while(si>=0){
        int ok=0;
        while(anc){ if(ssel_match(&r->sels[si], anc)){ ok=1; anc=anc->parent; break; } anc=anc->parent; }
        if(!ok) return 0;
        si--;
    }
    return 1;
}

static void apply_decl(struct cstyle *cs, const char *prop, const char *val, int parent_font)
{
    if(ieq(prop,"display")){
        if(ieq(val,"none"))cs->display=DISP_NONE; else if(ieq(val,"block"))cs->display=DISP_BLOCK;
        else if(ieq(val,"inline-block"))cs->display=DISP_INLINE_BLOCK; else if(ieq(val,"inline"))cs->display=DISP_INLINE;
        else if(ieq(val,"list-item")){cs->display=DISP_BLOCK;cs->list_item=1;}
    }
    else if(ieq(prop,"color")){ uint32_t c=parse_color(val); if(!(c&0xFF000000))cs->color=c; }
    else if(ieq(prop,"background")||ieq(prop,"background-color")){ uint32_t c=parse_color(val); if(!(c&0xFF000000)){cs->background=c;cs->has_bg=1;} }
    else if(ieq(prop,"font-size")){ int pct; int v=parse_len(val,parent_font,&pct); cs->font_px = pct?(parent_font*v/100):(v>0?v:cs->font_px); if(cs->font_px<6)cs->font_px=6; }
    else if(ieq(prop,"font-weight")){ if(ieq(val,"bold")||ieq(val,"bolder"))cs->bold=1; else if(ieq(val,"normal"))cs->bold=0; else { int n=0;for(const char*p=val;*p>='0'&&*p<='9';p++)n=n*10+(*p-'0'); if(n)cs->bold=(n>=600); } }
    else if(ieq(prop,"font-style")){ cs->italic = ieq(val,"italic")||ieq(val,"oblique"); }
    else if(ieq(prop,"font-family")){ cs->mono = has_sub(val,"mono"); }
    else if(ieq(prop,"text-align")){ cs->text_align = ieq(val,"center")?ALIGN_CENTER:ieq(val,"right")?ALIGN_RIGHT:ALIGN_LEFT; }
    else if(ieq(prop,"line-height")){ int pct; int v=parse_len(val,cs->font_px,&pct); cs->line_px = pct?(cs->font_px*v/100):v; }
    else if(ieq(prop,"text-decoration")){ cs->underline = has_sub(val,"underline"); }
    else if(ieq(prop,"border-radius")){ int pct; cs->radius=parse_len(val,cs->font_px,&pct); }
    else if(ieq(prop,"width")){ int pct; int v=parse_len(val,cs->font_px,&pct); cs->has_w=1; cs->width=v; cs->w_pct=pct; }
    else if(ieq(prop,"height")){ int pct; int v=parse_len(val,cs->font_px,&pct); cs->has_h=1; cs->height=v; cs->h_pct=pct; }
    else if(ieq(prop,"border")){ /* "1px solid #ccc" */ int pct; cs->border_w=parse_len(val,cs->font_px,&pct); const char *c=val; while(*c){ if(*c=='#'||neq(c,"rgb",3)){cs->border_color=parse_color(c);break;} c++; } if(!cs->border_color)cs->border_color=0x808080; }
    else if(ieq(prop,"margin")||ieq(prop,"padding")){
        int v[4],nv=0; const char *q=val;
        while(*q&&nv<4){ while(sp(*q))q++; if(!*q)break; const char *w=q; while(*q&&!sp(*q))q++;
            char t[24];int ti=0;for(const char*k=w;k<q&&ti<23;k++)t[ti++]=*k;t[ti]=0;
            int pct; v[nv++]= (ieq(t,"auto")? -1 : parse_len(t,cs->font_px,&pct)); }
        int top=v[0],rt=v[0],bt=v[0],lf=v[0];
        if(nv==2){rt=lf=v[1];} else if(nv==3){rt=lf=v[1];bt=v[2];} else if(nv>=4){rt=v[1];bt=v[2];lf=v[3];}
        if(ieq(prop,"margin")){cs->mt=top;cs->mr=rt;cs->mb=bt;cs->ml=lf;} else {cs->pt=top<0?0:top;cs->pr=rt<0?0:rt;cs->pb=bt<0?0:bt;cs->pl=lf<0?0:lf;}
    }
    else if(ieq(prop,"margin-top"))   { int pct; cs->mt=ieq(val,"auto")?-1:parse_len(val,cs->font_px,&pct); }
    else if(ieq(prop,"margin-bottom")){ int pct; cs->mb=ieq(val,"auto")?-1:parse_len(val,cs->font_px,&pct); }
    else if(ieq(prop,"margin-left"))  { int pct; cs->ml=ieq(val,"auto")?-1:parse_len(val,cs->font_px,&pct); }
    else if(ieq(prop,"margin-right")) { int pct; cs->mr=ieq(val,"auto")?-1:parse_len(val,cs->font_px,&pct); }
    else if(ieq(prop,"padding-left")) { int pct; cs->pl=parse_len(val,cs->font_px,&pct); }
    else if(ieq(prop,"padding-top"))  { int pct; cs->pt=parse_len(val,cs->font_px,&pct); }
}


static void compute(struct node *n, struct cstyle *parent, const char *page_css, int page_len)
{
    struct cstyle *cs = kmalloc(sizeof *cs);
    memset(cs, 0, sizeof *cs);
    /* inherit */
    if(parent){ cs->color=parent->color; cs->font_px=parent->font_px; cs->bold=parent->bold;
                cs->italic=parent->italic; cs->mono=parent->mono; cs->text_align=parent->text_align; }
    else { cs->color=0x000000; cs->font_px=16; }
    cs->display = DISP_INLINE;
    cs->ml=cs->mr=cs->mt=cs->mb=0;
    if(n->type==N_TEXT) cs->display=DISP_INLINE;

    if(n->type==N_ELEM){
        /* gather + apply matching rules sorted by (origin, spec, order) */
        for(int pass=0; pass<2; pass++){                /* 0=UA, 1=page */
            int lo = pass? ua_rules : 0, hi = pass? nrule : ua_rules;
            /* simple specificity-ordered apply: iterate spec 0..120 */
            for(int s=0;s<=130;s++)
                for(int i=lo;i<hi;i++)
                    if(rules[i].spec==s && rule_match(&rules[i], n))
                        for(int k=0;k<rules[i].ndecl;k++)
                            apply_decl(cs, rules[i].decls[k].prop, rules[i].decls[k].val, parent?parent->font_px:16);
        }
        /* inline style="" wins */
        const char *st=dom_attr(n,"style");
        if(st){ char tmp[512]; int t=0; for(const char*p=st;*p&&t<510;p++)tmp[t++]=*p; tmp[t]=0;
            char *p=tmp; while(*p){ char *colon=p; while(*colon&&*colon!=':'&&*colon!=';')colon++;
                if(*colon!=':'){ while(*p&&*p!=';')p++; if(*p)p++; continue; }
                char prop[20]; int pi=0; for(char*k=p;k<colon&&pi<19;k++)prop[pi++]=lc(*k); prop[pi]=0;
                char *vs=colon+1; while(*vs&&sp(*vs))vs++; char *ve=vs; while(*ve&&*ve!=';')ve++;
                char val[80]; int vi=0; for(char*k=vs;k<ve&&vi<79;k++)val[vi++]=*k; val[vi]=0;
                apply_decl(cs, prop, val, parent?parent->font_px:16);
                p=(*ve)?ve+1:ve; } }
    }
    if(cs->line_px==0) cs->line_px = cs->font_px*5/4;
    n->style = cs;
    (void)page_css; (void)page_len;
    for(struct node *c=n->first_child;c;c=c->next) compute(c, cs, page_css, page_len);
}

void css_apply(struct node *root, const char *page_css, int page_len)
{
    css_init();
    if(page_css && page_len>0) parse_css(page_css, page_len, 1);
    compute(root, 0, page_css, page_len);
}
