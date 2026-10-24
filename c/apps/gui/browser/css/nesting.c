/* CSS Nesting 1, sections 3.4/4/5. This is token-aware lowering into ordinary
 * rules, not Sass concatenation: & is :is(parent-list), whose specificity is
 * the maximum WHOLE complex parent arm. LibCSS's complex :is matcher is the
 * authority for both matching and specificity; no character-count estimate.
 *
 * Bare declarations are emitted in consecutive source-order runs using the
 * ORIGINAL parent selector, including its pseudo-elements and each arm's own
 * specificity. Wrapping these runs in :is(parent) would silently change both.
 *
 * Top-level & is valid CSS (scoping root) but deliberately unsupported here:
 * LibCSS has no :scope producer, so pretending it were '*' would change scope.
 * Nested @scope/@container, namespace-qualified nested selectors and explicit
 * child rules below a pseudo-element parent are deliberately refused. Their
 * semantics cannot be preserved by this lowering. Conditional bare runs still
 * work below pseudo-elements. Resource refusal drops the whole normalization,
 * never returns a partly lowered sheet that the three producers disagree on.
 */
#include "css/nesting.h"
#include <string.h>

void *kmalloc(unsigned long);
void kfree(void *);

enum { NEST_DEPTH = 32, NEST_LIMIT = 32 * 1024 * 1024 };
struct nest_output { char *data; int len, cap, failed; };
struct nest_span { const char *data; int len; };
struct nest_state {
    const struct css_nesting_hooks *hooks;
    struct css_nesting_report report;
    struct nest_output out;
};
static int nest_space(int c)
{ return c==' ' || c=='\t' || c=='\n' || c=='\r' || c=='\f'; }
static int nest_hex(int c)
{ return (c>='0'&&c<='9') || (c>='a'&&c<='f') || (c>='A'&&c<='F'); }

/* CSS escapes can include a whitespace terminator. Treating '\\26 ' as a
 * literal ampersand would invent a nesting selector in an escaped identifier. */
static int nest_escape(const char *s, int n, int i)
{
    i++;
    if (i<n && nest_hex(s[i])) {
        int k=0; while(i<n && nest_hex(s[i]) && k++<6)i++;
        if(i<n && nest_space(s[i])) { if(s[i]=='\r'&&i+1<n&&s[i+1]=='\n')i++; i++; }
    } else if(i<n)i++;
    return i;
}
static int nest_noise(const char *s, int n, int i)
{
    if(s[i]=='\\')return nest_escape(s,n,i);
    if(i+1<n && s[i]=='/' && s[i+1]=='*') {
        i+=2; while(i+1<n && !(s[i]=='*'&&s[i+1]=='/'))i++;
        return i+1<n?i+2:n;
    }
    if(s[i]=='\'' || s[i]=='"') {
        int quote=s[i++];
        while(i<n && s[i]!=quote) { if(s[i]=='\\')i=nest_escape(s,n,i); else i++; }
        return i<n?i+1:n;
    }
    return i;
}
static int nest_trivia(const char *s, int n, int i)
{
    for(;;) {
        while(i<n && nest_space(s[i]))i++;
        if(i+1<n && s[i]=='/' && s[i+1]=='*')i=nest_noise(s,n,i);
        else return i;
    }
}
static struct nest_span nest_trim(struct nest_span s)
{
    int i=nest_trivia(s.data,s.len,0); s.data+=i; s.len-=i;
    while(s.len && nest_space(s.data[s.len-1]))s.len--;
    return s;
}
static int nest_word(struct nest_span s, const char *name)
{
    int i=0; while(i<s.len && name[i]) {
        int c=(unsigned char)s.data[i]; if(c>='A'&&c<='Z')c+=32;
        if(c!=name[i])return 0; i++;
    }
    return i==s.len && !name[i];
}
static void nest_put(struct nest_output *o, const char *s, int n)
{
    if(o->failed || n<0 || n>NEST_LIMIT-o->len) { o->failed=1; return; }
    if(o->len+n+1>o->cap) {
        int cap=o->cap?o->cap:1024;
        while(cap<o->len+n+1 && cap<NEST_LIMIT)cap=cap>NEST_LIMIT/2?NEST_LIMIT:cap*2;
        if(cap<o->len+n+1) { o->failed=1; return; }
        char *next=kmalloc((unsigned long)cap);
        if(!next) { o->failed=1; return; }
        if(o->len)memcpy(next,o->data,(unsigned)o->len);
        kfree(o->data);o->data=next;o->cap=cap;
    }
    if(n)memcpy(o->data+o->len,s,(unsigned)n);
    o->len+=n;o->data[o->len]=0;
}
static void nest_literal(struct nest_output *o,const char *s)
{ nest_put(o,s,(int)strlen(s)); }

/* First delimiter outside strings/comments/escaped identifiers and ()/[];
 * brace values in custom declarations are consumed by nest_block instead. */
static int nest_delimiter(const char *s,int n,int i)
{
    int parens=0,brackets=0;
    while(i<n) {
        int next=nest_noise(s,n,i);
        if(next!=i) { i=next;continue; }
        int c=s[i];
        if(c=='(')parens++; else if(c==')' && parens)parens--;
        else if(c=='[')brackets++; else if(c==']' && brackets)brackets--;
        else if(!parens&&!brackets&&(c=='{'||c==';'||c=='}'))return i;
        i++;
    }
    return n;
}
static int nest_block(const char *s,int n,int open)
{
    int depth=1,i=open+1,parens=0,brackets=0;
    while(i<n) {
        int next=nest_noise(s,n,i); if(next!=i) {i=next;continue;}
        if(s[i]=='(')parens++;else if(s[i]==')'&&parens)parens--;
        else if(s[i]=='[')brackets++;else if(s[i]==']'&&brackets)brackets--;
        else if(!parens&&!brackets) {
            if(s[i]=='{')depth++;else if(s[i]=='}' && !--depth)return i;
        }
        i++;
    }
    return n; /* CSS Syntax closes an open block at EOF. */
}
static int nest_custom(const char *s,int n)
{
    int i=nest_trivia(s,n,0);
    if(i+2>n || s[i]!='-' || s[i+1]!='-')return 0;
    while(i<n && s[i]!=':')i++;
    return i<n;
}
static void nest_run(struct nest_state *st,struct nest_span parent,const char *s,int n)
{
    if(!nest_trim((struct nest_span){s,n}).len)return;
    nest_put(&st->out,parent.data,parent.len);nest_literal(&st->out,"{");
    nest_put(&st->out,s,n);nest_literal(&st->out,"}\n");
}
static int nest_has_amp(struct nest_span s)
{
    for(int i=0;i<s.len;) {
        int next=nest_noise(s.data,s.len,i);
        if(next!=i)i=next; else if(s.data[i++]=='&')return 1;
    }
    return 0;
}
static int nest_namespace(struct nest_span s)
{
    /* Attribute |= is not a namespace separator. This bounded lowering does
     * not move @namespace declarations into its temporary validation sheet. */
    int brackets=0;
    for(int i=0;i<s.len;) {
        int next=nest_noise(s.data,s.len,i);if(next!=i){i=next;continue;}
        if(s.data[i]=='[')brackets++;else if(s.data[i]==']')brackets--;
        else if(s.data[i]=='|'&&!brackets)return 1;
        i++;
    }
    return 0;
}
static void nest_parent(struct nest_output *out,struct nest_span parent)
{ nest_literal(out,":is(");nest_put(out,parent.data,parent.len);nest_literal(out,")"); }
static char *nest_selector(struct nest_state *st,struct nest_span parent,
                           struct nest_span child,int *length)
{
    if(!st->hooks || !st->hooks->selector || nest_namespace(parent))return NULL;
    int valid=st->hooks->selector(parent.data,parent.len);
    if(valid<0){st->out.failed=1;return NULL;}
    if(valid!=1)return NULL;
    struct nest_output out={0};int start=0,parens=0,brackets=0,invalid=0;
    for(int i=0;i<=child.len;i++) {
        if(i<child.len) {
            int next=nest_noise(child.data,child.len,i);
            if(next!=i){i=next-1;continue;}
            if(child.data[i]=='(')parens++; else if(child.data[i]==')')parens--;
            else if(child.data[i]=='[')brackets++; else if(child.data[i]==']')brackets--;
        }
        if(i==child.len || (!parens&&!brackets&&child.data[i]==',')) {
            struct nest_span arm=nest_trim((struct nest_span){child.data+start,i-start});
            if(!arm.len || nest_namespace(arm)) {invalid=1;break;}
            if(start)nest_literal(&out,",");
            int relative=arm.data[0]=='>'||arm.data[0]=='+'||arm.data[0]=='~';
            if(relative || !nest_has_amp(arm)) {nest_parent(&out,parent);nest_literal(&out," ");}
            for(int j=0;j<arm.len;) {
                int next=nest_noise(arm.data,arm.len,j);
                if(next!=j) {nest_put(&out,arm.data+j,next-j);j=next;}
                else if(arm.data[j]=='&') {
                    /* &suffix is not CSS nesting. A following type token must
                     * fail instead of turning into a plausible Sass class. */
                    int k=j+1;
                    if(k<arm.len && ((arm.data[k]>='a'&&arm.data[k]<='z') ||
                       (arm.data[k]>='A'&&arm.data[k]<='Z') || arm.data[k]=='_' ||
                       arm.data[k]=='-' || arm.data[k]=='\\')) {invalid=1;break;}
                    nest_parent(&out,parent);j++;
                } else nest_put(&out,arm.data+j++,1);
            }
            start=i+1;
            if(invalid)break;
        }
    }
    valid=out.failed?-1:invalid?0:out.len?st->hooks->selector(out.data,out.len):0;
    if(valid<0)st->out.failed=1;
    if(valid<=0) {
        kfree(out.data);return NULL;
    }
    *length=out.len;return out.data;
}
static void nest_rules(struct nest_state *,const char *,int,struct nest_span,int);
static void nest_group(struct nest_state *st,const char *s,int start,int brace,int end,
                       struct nest_span parent,int depth)
{
    int p=start+1;while(p<brace && ((s[p]>='a'&&s[p]<='z') ||
       (s[p]>='A'&&s[p]<='Z') || s[p]=='-'))p++;
    struct nest_span name={s+start+1,p-start-1};
    int media=nest_word(name,"media"),supports=nest_word(name,"supports");
    int layer=nest_word(name,"layer");
    if(media||supports||layer) {
        if(supports) {
            int ok=st->hooks&&st->hooks->supports?st->hooks->supports(s+p,brace-p):0;
            if(ok<0){st->out.failed=1;return;}
            nest_literal(&st->out,ok?"@media all{":"@media not all{");
        } else {nest_put(&st->out,s+start,brace-start+1);}
        nest_rules(st,s+brace+1,end-brace-1,parent,depth+1);
        nest_literal(&st->out,"}\n");
    } else if(parent.len)st->report.refused++;
    else {nest_put(&st->out,s+start,end-start);nest_literal(&st->out,"}");}
}
static void nest_rules(struct nest_state *st,const char *s,int n,
                       struct nest_span parent,int depth)
{
    if(depth>NEST_DEPTH) {st->out.failed=1;return;}
    int i=0,run=0;
    while(i<n && !st->out.failed) {
        int start=nest_trivia(s,n,i);
        if(start==n)break;
        int delim=nest_delimiter(s,n,start);
        if(parent.len && s[start]!='@' && nest_custom(s+start,delim-start)) {
            /* Custom properties may contain balanced {}. They remain values,
             * not nested style rules; do not let their text create selectors. */
            while(delim<n && s[delim]=='{') {
                int end=nest_block(s,n,delim);delim=nest_delimiter(s,n,end<n?end+1:end);
            }
        }
        if(delim==n || s[delim]!='{') {
            if(parent.len && s[start]!='@') {i=delim<n?delim+1:delim;continue;}
            if(parent.len) {
                nest_run(st,parent,s+run,start-run);
                st->report.refused++; /* e.g. nested @import is not hoisted */
            } else nest_put(&st->out,s+start,delim-start+(delim<n));
            i=delim<n?delim+1:delim;run=i;continue;
        }
        int end=nest_block(s,n,delim);
        if(parent.len)nest_run(st,parent,s+run,start-run);
        if(s[start]=='@')nest_group(st,s,start,delim,end,parent,depth);
        else {
            struct nest_span selector=nest_trim((struct nest_span){s+start,delim-start});
            char *owned=NULL;
            if(parent.len) {
                int length=0;owned=nest_selector(st,parent,selector,&length);
                if(owned) {selector.data=owned;selector.len=length;st->report.nested++;}
                else {st->report.refused++;selector.len=0;}
            } else if(nest_has_amp(selector)) {st->report.refused++;selector.len=0;}
            if(selector.len)nest_rules(st,s+delim+1,end-delim-1,selector,depth+1);
            kfree(owned);
        }
        i=end<n?end+1:end;run=i;
    }
    if(parent.len)nest_run(st,parent,s+run,n-run);
}
int css_nesting_normalize(const char *s,int n,char **out,int *length,
    const struct css_nesting_hooks *hooks,struct css_nesting_report *report)
{
    if(!s || n<0 || !out || !length)return -1;
    *out=NULL;*length=n;
#ifdef CSS_NESTING_NO_NORMALIZE
    (void)hooks;if(report)memset(report,0,sizeof *report);return 0;
#else
    struct nest_state st={0};st.hooks=hooks;
    nest_rules(&st,s,n,(struct nest_span){NULL,0},0);
    if(report)*report=st.report;
    if(st.out.failed) {kfree(st.out.data);return -1;}
    if(st.out.len==n && (!n || !memcmp(st.out.data,s,(unsigned)n))) {kfree(st.out.data);return 0;}
    *out=st.out.data;*length=st.out.len;return 1;
#endif
}
