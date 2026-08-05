#include "dom.h"

void *kmalloc(unsigned long);
void  kfree(void *);
void *memcpy(void *, const void *, unsigned long);
void *memset(void *, int, unsigned long);

static int nmatch(const char *a, const char *b, int n) { for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }
static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int sp(int c) { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'; }
static int ieq(const char *a, const char *b) { while (*a && lc(*a)==lc(*b)) {a++;b++;} return lc(*a)==lc(*b); }

static int is_void(const char *t)
{
    static const char *v[] = {"area","base","br","col","embed","hr","img","input",
                              "link","meta","param","source","track","wbr",0};
    for (int i = 0; v[i]; i++) if (ieq(t, v[i])) return 1;
    return 0;
}
static int is_rawtext(const char *t) { return ieq(t,"script") || ieq(t,"style"); }

static struct node *newnode(int type)
{
    struct node *n = kmalloc(sizeof *n);
    if (n) { memset(n, 0, sizeof *n); n->type = type; }
    return n;
}
static void add_child(struct node *p, struct node *c)
{
    if (!p || !c) return;
    c->parent = p;
    if (p->last_child) p->last_child->next = c; else p->first_child = c;
    p->last_child = c;
}

/* Named HTML entities (common subset). Codepoint is UTF-8 encoded below. */
static int slen(const char *s){ int n=0; while(s[n]) n++; return n; }
static const struct { const char *name; int cp; } ENTITIES[] = {
    {"amp",38},{"lt",60},{"gt",62},{"quot",34},{"apos",39},{"nbsp",32},
    {"copy",169},{"reg",174},{"trade",8482},{"mdash",8212},{"ndash",8211},
    {"hellip",8230},{"times",215},{"divide",247},{"laquo",171},{"raquo",187},
    {"ldquo",8220},{"rdquo",8221},{"lsquo",8216},{"rsquo",8217},{"middot",183},
    {"bull",8226},{"deg",176},{"plusmn",177},{"sect",167},{"para",182},
    {"dagger",8224},{"euro",8364},{"pound",163},{"cent",162},{"yen",165},
    {"frac12",189},{"frac14",188},{"frac34",190},{"sup2",178},{"sup3",179},
    {"larr",8592},{"rarr",8594},{"uarr",8593},{"darr",8595},{"harr",8596},
    {"emsp",32},{"ensp",32},{"thinsp",32},{"iexcl",161},{"iquest",191},
    {"szlig",223},{"aacute",225},{"eacute",233},{"egrave",232},{"agrave",224},
    {"ccedil",231},{"ntilde",241},{"ouml",246},{"uuml",252},{"auml",228},
    {"ograve",242},{"oacute",243},{"uacute",250},{"iacute",237},
    {0,0}
};

/* Decode HTML entities in [s,e) into out (capacity cap); returns out length. */
static int decode_text(const char *s, const char *e, char *out, int cap)
{
    int o = 0;
    while (s < e && o < cap - 4) {
        if (*s != '&') { out[o++] = *s++; continue; }
        const char *p = s + 1, *semi = 0;
        for (const char *k = p; k < e && k < p + 10; k++) if (*k == ';') { semi = k; break; }
        if (!semi) { out[o++] = *s++; continue; }
        int v = -1;
        if (*p == '#') {
            int hex = (p[1]=='x'||p[1]=='X'); const char *d = p + (hex?2:1); v = 0;
            int nd = 0;
            for (; d < semi; d++) { char c=*d;
                int dig = -1;
                if (hex) { if(c>='0'&&c<='9')dig=c-'0'; else if(c>='a'&&c<='f')dig=c-'a'+10; else if(c>='A'&&c<='F')dig=c-'A'+10; }
                else if (c>='0'&&c<='9') dig=c-'0';
                if (dig < 0) { v = -1; break; }            /* e.g. &#xZZ; -> emit literally, not a NUL byte */
                v = v*(hex?16:10)+dig; nd++;
                if (v > 0x10FFFF) { v = -1; break; }       /* cap: larger values would emit invalid UTF-8 */
            }
            if (!nd) v = -1;
        } else {
            int l = semi - p;
            for (int e2 = 0; ENTITIES[e2].name; e2++)
                if (slen(ENTITIES[e2].name) == l && nmatch(p, ENTITIES[e2].name, l)) { v = ENTITIES[e2].cp; break; }
        }
        if (v < 0) { out[o++] = *s++; continue; }
        if (v < 0x80) out[o++]=(char)v;
        else if (v < 0x800) { out[o++]=(char)(0xC0|(v>>6)); out[o++]=(char)(0x80|(v&0x3F)); }
        else if (v < 0x10000) { out[o++]=(char)(0xE0|(v>>12)); out[o++]=(char)(0x80|((v>>6)&0x3F)); out[o++]=(char)(0x80|(v&0x3F)); }
        else { out[o++]=(char)(0xF0|(v>>18)); out[o++]=(char)(0x80|((v>>12)&0x3F)); out[o++]=(char)(0x80|((v>>6)&0x3F)); out[o++]=(char)(0x80|(v&0x3F)); }
        s = semi + 1;
    }
    out[o] = 0;
    return o;
}

static void emit_text(struct node *parent, const char *s, const char *e)
{
    if (e <= s) return;
    int cap = (int)(e - s) + 8;
    char *buf = kmalloc(cap);
    if (!buf) return;
    int len = decode_text(s, e, buf, cap);
    if (len == 0) { kfree(buf); return; }
    struct node *t = newnode(N_TEXT);
    if (!t) { kfree(buf); return; }
    t->text = buf; t->textlen = len;
    add_child(parent, t);
}

#define MAXDEPTH 64

struct node *dom_parse(const char *html, int len)
{
    struct node *root = newnode(N_ELEM);
    if (!root) return 0;
    memcpy(root->tag, "#document", 10);
    struct node *stack[MAXDEPTH]; int sd = 0; stack[sd++] = root;
    #define TOP stack[sd-1]
    const char *p = html, *end = html + len;

    while (p < end) {
        if (*p != '<') {                                  /* text run */
            const char *t = p;
            while (p < end && *p != '<') p++;
            emit_text(TOP, t, p);
            continue;
        }
        /* a tag */
        if (p + 1 < end && p[1] == '!') {                 /* comment / doctype */
            if (p + 3 < end && p[2]=='-' && p[3]=='-') {
                p += 4; while (p + 2 < end && !(p[0]=='-'&&p[1]=='-'&&p[2]=='>')) p++; p = (p+3<end)?p+3:end;
            } else { while (p < end && *p != '>') p++; if (p<end) p++; }
            continue;
        }
        if (p + 1 < end && p[1] == '/') {                 /* end tag */
            p += 2; const char *t = p;
            while (p < end && *p != '>' && !sp(*p)) p++;
            char name[16]; int nl = 0; for (const char *k=t; k<p && nl<15; k++) name[nl++]=lc(*k); name[nl]=0;
            while (p < end && *p != '>') p++; if (p<end) p++;
            /* pop to the matching open element (tolerant) */
            for (int i = sd-1; i >= 1; i--)
                if (ieq(stack[i]->tag, name)) { sd = i; break; }
            continue;
        }
        /* start tag */
        p++; const char *t = p;
        while (p < end && *p != '>' && *p != '/' && !sp(*p)) p++;
        char name[16]; int nl = 0; for (const char *k=t; k<p && nl<15; k++) name[nl++]=lc(*k); name[nl]=0;
        if (!nl) { while (p<end && *p!='>') p++; if(p<end)p++; continue; }
        /* HTML5 optional end tags: a new peer auto-closes the still-open one */
        if (ieq(name,"tr")) { while (sd>1 && (ieq(TOP->tag,"td")||ieq(TOP->tag,"th")||ieq(TOP->tag,"tr"))) sd--; }
        else if (ieq(name,"td")||ieq(name,"th")) { while (sd>1 && (ieq(TOP->tag,"td")||ieq(TOP->tag,"th"))) sd--; }
        else if (ieq(name,"li")) { while (sd>1 && ieq(TOP->tag,"li")) sd--; }
        else if (ieq(name,"dd")||ieq(name,"dt")) { while (sd>1 && (ieq(TOP->tag,"dd")||ieq(TOP->tag,"dt"))) sd--; }
        else if (ieq(name,"option")) { while (sd>1 && ieq(TOP->tag,"option")) sd--; }
        else if (ieq(name,"p")) { while (sd>1 && ieq(TOP->tag,"p")) sd--; }
        /* implied <tbody>: a <tr> placed directly inside <table> (HTML5) */
        if (ieq(name,"tr") && sd >= 1 && ieq(TOP->tag,"table") && sd < MAXDEPTH) {
            struct node *tb = newnode(N_ELEM);
            if (tb) { memcpy(tb->tag, "tbody", 6); add_child(TOP, tb); stack[sd++] = tb; }
        }
        struct node *el = newnode(N_ELEM);
        if (!el) { while (p<end && *p!='>') p++; if(p<end)p++; continue; }
        memcpy(el->tag, name, nl+1);
        /* attributes */
        struct attr tmp[32]; int na = 0;
        while (p < end && *p != '>' && *p != '/') {
            while (p < end && sp(*p)) p++;
            if (p>=end || *p=='>' || *p=='/') break;
            const char *as = p;
            while (p < end && *p!='=' && *p!='>' && *p!='/' && !sp(*p)) p++;
            if (na < 32) {
                int an=0; for (const char *k=as; k<p && an<31; k++) tmp[na].name[an++]=lc(*k); tmp[na].name[an]=0;
                tmp[na].val[0]=0;
                while (p<end && sp(*p)) p++;
                if (p<end && *p=='=') {
                    p++; while (p<end && sp(*p)) p++;
                    int vn=0;
                    if (p<end && (*p=='"'||*p=='\'')) { char q=*p++; while (p<end && *p!=q && vn<255) tmp[na].val[vn++]=*p++; while (p<end && *p!=q) p++; if(p<end)p++; }  /* skip a truncated value to its closing quote so a quoted '>' can't end the tag early */
                    else { while (p<end && !sp(*p) && *p!='>' && vn<255) tmp[na].val[vn++]=*p++; }  /* unquoted: until ws/'>' */
                    tmp[na].val[vn]=0;
                }
                na++;
            } else { while (p<end && !sp(*p) && *p!='>' && *p!='/') {     /* overflow attr: skip, honoring quoted values */
                         if (*p=='"'||*p=='\'') { char q=*p++; while (p<end && *p!=q) p++; if(p<end)p++; }
                         else p++; } }
        }
        int selfclose = (p<end && *p=='/');
        while (p < end && *p != '>') p++; if (p<end) p++;
        if (na) { el->attrs = kmalloc(na*sizeof(struct attr)); if (el->attrs) { memcpy(el->attrs, tmp, na*sizeof(struct attr)); el->nattr=na; } }
        if (sd >= MAXDEPTH) {
            if (el->attrs) { kfree(el->attrs); el->attrs = 0; }
            kfree(el);
            continue;
        }
        add_child(TOP, el);
        if (is_void(name) || selfclose) continue;
        if (is_rawtext(name)) {                           /* consume raw text to </name> */
            const char *rs = p;
            while (p < end) {
                if (*p=='<' && p+1<end && p[1]=='/') {
                    const char *q=p+2, *qt=q; while (q<end && *q!='>' && !sp(*q)) q++;
                    char cn[16]; int cl=0; for(const char*k=qt;k<q&&cl<15;k++)cn[cl++]=lc(*k); cn[cl]=0;
                    if (ieq(cn,name)) break;
                }
                p++;
            }
            if (p>rs) { char *buf=kmalloc((int)(p-rs)+1); if(buf){memcpy(buf,rs,p-rs);buf[p-rs]=0;struct node*tx=newnode(N_TEXT);if(tx){tx->text=buf;tx->textlen=(int)(p-rs);add_child(el,tx);}else kfree(buf);} }
            while (p<end && *p!='>') p++; if(p<end)p++;
            continue;
        }
        stack[sd++] = el;
    }
    #undef TOP
    return root;
}

const char *dom_attr(const struct node *n, const char *name)
{
    if (n->type != N_ELEM) return 0;
    for (int i = 0; i < n->nattr; i++) if (ieq(n->attrs[i].name, name)) return n->attrs[i].val;
    return 0;
}

void dom_free(struct node *n)
{
    if (!n) return;
    struct node *c = n->first_child;
    while (c) { struct node *nx = c->next; dom_free(c); c = nx; }
    if (n->attrs) kfree(n->attrs);
    if (n->text) kfree(n->text);
    if (n->style) kfree(n->style);
    kfree(n);
}
