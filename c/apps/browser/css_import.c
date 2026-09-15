/* CSS @import used to reach LibCSS without its dependency: its pending-import
 * API was never serviced. Sphinx classic.css imports basic.css, so Navigation
 * headings stayed visible and inline navigation lists became a tall block stack
 * despite every <link> fetch succeeding. Expand PER SHEET before concatenation:
 * imports appended after their parent reverse the cascade, and resolving them
 * against the document instead of the response URL requests the wrong directory.
 *
 * This is lexical assembly, not a second cascade. Media tails are retained as
 * @media blocks for the existing evaluator, including after a window resize.
 * CSS layer()/supports() import qualifiers are deliberately retained unexpanded:
 * this loader cannot flatten layer precedence or conditional support honestly.
 * They are counted as unsupported, never fetched/applied unconditionally.
 *
 * Depth 8, 64 requests and 4 MiB imported bytes bound ordinary dependency graphs
 * and cycles. A cycle is path-local: importing the same file in two independent
 * branches MUST still apply it twice in those two cascade positions. */
#include <stdlib.h>
#include <string.h>
#include "css_import.h"

int css_text_reserve(char **out,int *cap,int needed,int limit)
{
    if(needed<0||needed>limit)return 0;
    if(*cap>=needed)return 1;
    int next=*cap?*cap:4096;
    while(next<needed){if(next>limit/2){next=limit;break;}next*=2;}
    if(next>limit)next=limit;
    char *p=realloc(*out,(size_t)next);if(!p)return 0;
    *out=p;*cap=next;return 1;
}
struct ci_out { char *p; int n, cap, bad, limit; };
static void ci_put(struct ci_out *o, const char *s, int n)
{
    if (o->bad) return;
    if(n<0||n>0x7ffffffe - o->n){o->bad=1;return;}
    if(n>o->cap-1-o->n && (!o->limit||!css_text_reserve(&o->p,&o->cap,o->n+n+1,o->limit))){o->bad=1;return;}
    memcpy(o->p + o->n, s, (size_t)n); o->n += n; o->p[o->n] = 0;
}
static int ci_ws(int c) { return c==' '||c=='\t'||c=='\r'||c=='\n'||c=='\f'; }
static int ci_ident(int c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'; }
static int ci_word(const char *s, int n, int at, const char *word)
{
    int i=0;
    while (word[i]) {
        if (at+i>=n) return 0;
        int c=(unsigned char)s[at+i]; if(c>='A'&&c<='Z') c+=32;
        if(c!=word[i++]) return 0;
    }
    return at+i==n || !ci_ident((unsigned char)s[at+i]);
}
static int ci_comment(const char *s,int n,int p)
{
    p+=2; while(p+1<n && !(s[p]=='*'&&s[p+1]=='/')) p++;
    return p+1<n ? p+2 : n;
}
static int ci_trivia(const char *s,int n,int p)
{
    for (;;) {
        while(p<n && ci_ws(s[p])) p++;
        if(p+1<n&&s[p]=='/'&&s[p+1]=='*') p=ci_comment(s,n,p); else return p;
    }
}
static int ci_quote_end(const char *s,int n,int p)
{
    int q=s[p++];
    while(p<n) { if(s[p]=='\\'&&p+1<n) {p+=2;continue;} if(s[p++]==q)return p; }
    return n;
}
/* A bounded URL token. CSS escapes are deliberately not decoded here; retain
 * that rule unchanged rather than requesting a subtly different URL. */
static int ci_url(const char *s,int n,int *pos,char *url,int cap)
{
    int p=ci_trivia(s,n,*pos), end, a;
    int fn=ci_word(s,n,p,"url");
    if(fn) {p=ci_trivia(s,n,p+3);if(p>=n||s[p++]!='(')return -1;p=ci_trivia(s,n,p);}
    if(p>=n)return -1;
    if(s[p]=='\''||s[p]=='"') {
        int q=s[p++];a=p;while(p<n&&s[p]!=q&&s[p]!='\\'&&s[p]!='\n'&&s[p]!='\r')p++;
        if(p>=n||s[p]!=q)return -1;end=p++;
    } else {
        if(!fn)return -1;
        a=p;while(p<n&&!ci_ws(s[p])&&s[p]!=')'&&s[p]!='\\'&&s[p]!='\''&&s[p]!='"'&&s[p]!='(')p++;
        end=p;
    }
    if(fn){p=ci_trivia(s,n,p);if(p>=n||s[p++]!=')')return -1;}
    if(end-a>=cap||end==a)return -1;
    memcpy(url,s+a,(size_t)(end-a));url[end-a]=0;*pos=p;return 0;
}
/* Find a top-level semicolon, respecting strings/comments/function arguments.
 * A block brace means this is not an import prelude. */
static int ci_rule_end(const char *s,int n,int p)
{
    int par=0;
    while(p<n) {
        if(s[p]=='\''||s[p]=='"'){p=ci_quote_end(s,n,p);continue;}
        if(p+1<n&&s[p]=='/'&&s[p+1]=='*'){p=ci_comment(s,n,p);continue;}
        if(s[p]=='(')par++;else if(s[p]==')'){if(!par)return -1;par--;}
        else if(!par&&s[p]==';')return p;
        else if(s[p]=='{'||s[p]=='}')return -1;
        p++;
    }
    return -1;
}
static int ci_cycle(const char *url,const char **stack,int depth)
{ for(int i=0;i<=depth;i++)if(stack[i]&&!strcmp(url,stack[i]))return 1;return 0; }
static void ci_expand(const char *s,int n,const char *base,struct ci_out *o,
                      struct css_import_budget *b,const struct css_import_io *io,
                      const char **stack,int depth)
{
    int p=0, imports=1;
    if(n>=3&&(unsigned char)s[0]==0xef&&(unsigned char)s[1]==0xbb&&(unsigned char)s[2]==0xbf)p=3;
    stack[depth]=base;
    while(p<n&&!o->bad) {
        int t=ci_trivia(s,n,p);ci_put(o,s+p,t-p);p=t;if(p>=n)break;
        if(imports&&s[p]=='@'&&ci_word(s,n,p+1,"import")) {
            int end=ci_rule_end(s,n,p+7), q=p+7;
            char ref[CSS_IMPORT_URL_MAX],abs[CSS_IMPORT_URL_MAX],final[CSS_IMPORT_URL_MAX];
            if(end<0||ci_url(s,end,&q,ref,sizeof ref)<0) {
                b->unsupported++;imports=0;continue;
            }
            int media=ci_trivia(s,end,q);
            if(ci_word(s,end,media,"layer")||ci_word(s,end,media,"supports")) {
                b->unsupported++;ci_put(o,s+p,end+1-p);p=end+1;continue;
            }
#ifdef CSS_IMPORT_NEGCTL_DROP
            /* The previous loader: imported rules never reached the cascade. */
            p=end+1;continue;
#endif
            if(!io->resolve||io->resolve(io->ctx,base,ref,abs,sizeof abs)<0){b->failed++;p=end+1;continue;}
            if(ci_cycle(abs,stack,depth)){b->cycles++;p=end+1;continue;}
            if(depth>=CSS_IMPORT_DEPTH||b->requests>=CSS_IMPORT_REQUESTS||b->bytes>=CSS_IMPORT_BYTES){b->limited++;p=end+1;continue;}
            unsigned char *data=NULL;int len=0;final[0]=0;b->requests++;
            if(!io->fetch||io->fetch(io->ctx,abs,&data,&len,final,sizeof final)<0||!data||len<0){free(data);b->failed++;p=end+1;continue;}
            if(len>CSS_IMPORT_BYTES-b->bytes){free(data);b->limited++;p=end+1;continue;}
            b->bytes+=len;
            if(!final[0]){memcpy(final,abs,strlen(abs)+1);}
            if(ci_cycle(final,stack,depth)){free(data);b->cycles++;p=end+1;continue;}
            b->loaded++;
            if(media<end){ci_put(o,"@media ",7);ci_put(o,s+media,end-media);ci_put(o," {\n",3);}
            ci_expand((const char *)data,len,final,o,b,io,stack,depth+1);
            if(media<end)ci_put(o,"\n}\n",3);else ci_put(o,"\n",1);
            free(data);p=end+1;continue;
        }
        /* @charset and an empty @layer statement may precede imports. Other
         * rules end the import prelude; an @import in a declaration/string or
         * after an ordinary rule must never initiate another fetch. */
        if(imports&&s[p]=='@'&&(ci_word(s,n,p+1,"charset")||ci_word(s,n,p+1,"layer"))) {
            int end=ci_rule_end(s,n,p+1);
            if(end>=0){ci_put(o,s+p,end+1-p);p=end+1;continue;}
        }
        imports=0;
        if(s[p]=='\''||s[p]=='"'){int end=ci_quote_end(s,n,p);ci_put(o,s+p,end-p);p=end;continue;}
        if((p==0||!ci_ident((unsigned char)s[p-1]))&&ci_word(s,n,p,"url")) {
            int q=p;char ref[CSS_IMPORT_URL_MAX],abs[CSS_IMPORT_URL_MAX];
            if(ci_url(s,n,&q,ref,sizeof ref)==0) {
                /* Local fragment references and data URIs do not need a base.
                 * Rebase other url() tokens before losing the sheet boundary. */
                if(ref[0]!='#'&&io->resolve&&io->resolve(io->ctx,base,ref,abs,sizeof abs)==0&&
                   !strchr(abs,'"')&&!strchr(abs,'\\')&&!strchr(abs,'\n')&&!strchr(abs,'\r')) {
                    ci_put(o,"url(\"",5);ci_put(o,abs,(int)strlen(abs));ci_put(o,"\")",2);
                } else ci_put(o,s+p,q-p);
                p=q;continue;
            }
        }
        ci_put(o,s+p,1);p++;
    }
}
int css_import_expand(const char *src,int len,const char *base,char *out,int used,int cap,
                      struct css_import_budget *budget,const struct css_import_io *io)
{
    if(!src||len<0||!base||!out||used<0||used>=cap||!budget||!io)return -1;
    int input_bytes = budget->bytes;
    struct ci_out o={out,used,cap,0};const char *stack[CSS_IMPORT_DEPTH+1]={0};
    ci_expand(src,len,base,&o,budget,io,stack,0);
    if(o.bad){out[used]=0;budget->limited++;return -1;}
    budget->rewrite_delta += o.n - used - len - (budget->bytes - input_bytes);
    out[o.n]=0;return o.n;
}

int css_import_expand_alloc(const char *src,int len,const char *base,char **out,int used,int *cap,int limit,
                            struct css_import_budget *budget,const struct css_import_io *io)
{
    if(!src||len<0||!base||!out||!cap||used<0||!budget||!io||
       !css_text_reserve(out,cap,used+1,limit))return -1;
    int input_bytes=budget->bytes;
    struct ci_out o={*out,used,*cap,0,limit};const char *stack[CSS_IMPORT_DEPTH+1]={0};
    ci_expand(src,len,base,&o,budget,io,stack,0);
    *out=o.p;*cap=o.cap;
    if(o.bad){(*out)[used]=0;budget->limited++;return -1;}
    budget->rewrite_delta+=o.n-used-len-(budget->bytes-input_bytes);
    (*out)[o.n]=0;return o.n;
}
