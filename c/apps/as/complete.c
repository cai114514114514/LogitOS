/* libcomplete -- semantic autocomplete engine for AetherScript. Self-contained:
 * its own tolerant tokenizer (the production lexer.c uses snprintf and aborts on
 * mid-edit input), a single-pass scope/symbol model, a last-assignment type
 * table, and fuzzy ranking. No libc -- compiles freestanding + native. */

#include <stdint.h>
#include "complete.h"

/* ---- dependency-free char helpers ---- */
static int c_isd(char c){ return c >= '0' && c <= '9'; }
static int c_isa(char c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'; }
static int c_isan(char c){ return c_isa(c)||c_isd(c); }
static int c_issp(char c){ return c==' '||c=='\t'; }
/* token a[0..n) equals NUL-terminated literal b (and b is exactly n long, OR b is
 * the literal and n is its length for a region-prefix test -- both uses hold). */
static int c_neq(const char *a, const char *b, int n){
    for (int i=0;i<n;i++){ if (a[i]!=b[i]) return 0; if (!b[i]) return 0; }
    return b[n]==0;
}
static int c_slen(const char *s){ int n=0; while (s[n]) n++; return n; }
/* compare two length-n byte slices (neither need be NUL-terminated) */
static int c_eqn(const char *a, const char *b, int n){ for (int i=0;i<n;i++) if (a[i]!=b[i]) return 0; return 1; }
static int has_dot(const char *s, int n){ for (int i=0;i<n;i++) if (s[i]=='.') return 1; return 0; }

/* ---- constant tables ---- */
static const char *const KEYWORDS[] = {
    "def","return","if","elif","else","class","super","try","except","raise",
    "while","for","in","and","or","not","lambda","import","from",
    "true","false","nil","break","continue","with", 0
};
/* Mirrors the as_define_native() calls in vm.c + as_native.c + as_port.c.
 * `make check-asops` (tools/gen_as_opcodes.py) asserts this list stays complete
 * -- it had silently missed the seven M21-P3 self-hosting natives since they
 * landed. */
static const char *const BUILTINS[] = {
    /* `region` is M28's name for `buffer` -- the SAME native registered twice,
     * not a second allocator (see D2 of the M28 spec: ObjBuf already carried a
     * length, so there is no O_REGION). Both spellings complete, because both
     * are what a reader will type. */
    "print","len","range","str","gc","gc_stats","buffer","region","layout","caps",
    "chr","ord","f64bits","parse_float","file_read","file_write","args",
    "addr","syscall","alloc","dealloc","mem2str","mem2cstr",
    "peek8","peek16","peek32","peek64",
    "poke8","poke16","poke32","poke64","i8ptr","i16ptr","i32ptr","i64ptr",
    "open","port","pipe","run","port_stats", 0
};
/* the M23.5 system surface (mirrors as_native.c) -- prefix-typed (SYS_/EV_), so
 * they only surface when the user starts typing one */
static const char *const SYSCONSTS[] = {
    "SYS_WRITE","SYS_READ","SYS_OPEN","SYS_CLOSE","SYS_LSEEK","SYS_EXIT","SYS_YIELD",
    "SYS_GETPID","SYS_FORK","SYS_EXECVE","SYS_WAITPID","SYS_PIPE","SYS_DUP2",
    "SYS_MKDIR","SYS_GETCWD","SYS_CHDIR","SYS_READ_FILE","SYS_WRITE_FILE",
    "SYS_DELETE_FILE","SYS_RENAME","SYS_DIR_COUNT","SYS_DIR_NAME","SYS_GET_TIME",
    "SYS_MONOTONIC_MS",
    "SYS_NET_INFO","SYS_NET_PING","SYS_NET_PING_RTT","SYS_NET_DNS","SYS_NET_DNS_RESULT",
    "SYS_CPU_INDEX","SYS_UI_DARK","SYS_GUI_CREATE","SYS_GUI_CLEAR","SYS_GUI_RECT",
    "SYS_GUI_TEXT","SYS_GUI_TEXT_MONO","SYS_GUI_FLUSH","SYS_GUI_ICON","SYS_GUI_GLASS",
    "SYS_GET_ARG","SYS_SYSINFO","SYS_FILE_COUNT","SYS_FILE_NAME","SYS_SPAWN","SYS_DUP",
    "SYS_SETNB","SYS_FSYNC","SYS_OPEN_PATH","SYS_IMG_DECODE","SYS_KHEAP_STRESS",
    "SYS_HTTP_GET","SYS_HTTP_STATUS","SYS_HTTP_BODY","SYS_RES_FETCH","SYS_TEXT_MEASURE",
    "SYS_GUI_TEXT_RUN","SYS_GUI_BLIT","SYS_GUI_RRECT","SYS_GUI_CLIP",
    "SYS_POLL_EVENT","SYS_WAIT_EVENT","EV_NONE","EV_KEY","EV_MOUSE","EV_CLOSE","EV_MOUSE_R","EV_THEME",
    "EV_MOUSE_UP","EV_MOUSE_MOVE","EV_WHEEL",
    "EV_MOD_SHIFT","EV_MOD_CTRL","EV_MOD_ALT",
    "EV_BTN_NONE","EV_BTN_LEFT","EV_BTN_RIGHT","EV_BTN_MIDDLE",
    /* The eighteen that as_native.c had never defined at all until 2026-08-16
     * (see the block comment at the end of as_install_indirection): the socket
     * family, the thread family, the futex, TLS base, getrandom, both
     * window-state calls, and stat. They are here now for the same reason they
     * are there now -- a generated wrapper in lib/abi.as calls each of them by
     * name, and a name the completer does not know is a name the reader is
     * told does not exist. */
    "SYS_STAT","SYS_GUI_WIN_MIN","SYS_GUI_WIN_STATE",
    "SYS_SOCK_OPEN","SYS_SOCK_POLL","SYS_SOCK_SEND","SYS_SOCK_RECV",
    "SYS_SOCK_ALPN","SYS_SOCK_CLOSE",
    "SYS_THREAD_CREATE","SYS_THREAD_EXIT","SYS_THREAD_JOIN","SYS_THREAD_DETACH",
    "SYS_THREAD_SELF","SYS_THREAD_INFO","SYS_SET_TLS","SYS_FUTEX","SYS_GETRANDOM",
    /* File-type bits of logit_stat.mode, for `(st.mode & LST_IFMT) ==
     * LST_IFDIR` -- the directory test lib/image.as makes before it tells a
     * user their picture is a folder. */
    "LST_IFMT","LST_IFREG","LST_IFDIR","LST_IFLNK",
    /* M28 capability classes (mirrors as_native.c's as_define_int block). */
    "CAP_FS_READ","CAP_FS_WRITE","CAP_NET","CAP_PROC","CAP_GUI","CAP_RAW", 0
};
static const char *const LIST_METHODS[] = { "append", 0 };
static const char *const DICT_METHODS[] = { "get","has","keys","values","remove", 0 };
static const char *const STR_METHODS[]  = { "join","split","strip","upper","lower","replace","find","sub", 0 };
/* M27 ports (mirrors as_port.c's port_method/proc_method). */
static const char *const PORT_METHODS[] = { "read","readall","line","lines","write","close","closed","fd","kind", 0 };
static const char *const PROC_METHODS[] = { "start","wait","out","pid","status","argv", 0 };
/* M28. Every one of these NARROWS -- there is deliberately no widen/union, so
 * the completion list is also the complete list of what a capability can do. */
static const char *const CAP_METHODS[] = { "scope","without","bits","path", 0 };

static int is_kw(const char *s, int n){
    for (int i=0; KEYWORDS[i]; i++) if (c_neq(s, KEYWORDS[i], n)) return 1;
    return 0;
}

/* ---- tolerant tokenizer ---- */
typedef struct { int kind; int start; int len; } Tok;

static int lex(const char *src, int len, Tok *toks, int max){
    int n = 0, i = 0;
    while (i < len && n < max) {
        char c = src[i];
        if (c == '\n') { toks[n].kind=TK_NL; toks[n].start=i; toks[n].len=1; n++; i++; continue; }
        if (c_issp(c) || c=='\r') { i++; continue; }
        if (c == '#') { while (i < len && src[i] != '\n') i++; continue; }
        if (c == '.') { toks[n].kind=TK_DOT; toks[n].start=i; toks[n].len=1; n++; i++; continue; }
        if (c=='"' || c=='\'') {                      /* tolerant: ends at quote/EOL/EOF */
            int s=i; char q=c; i++;
            while (i<len && src[i]!=q && src[i]!='\n'){ if (src[i]=='\\' && i+1<len) i++; i++; }
            if (i<len && src[i]==q) i++;
            toks[n].kind=TK_STR; toks[n].start=s; toks[n].len=i-s; n++; continue;
        }
        if (c_isd(c)) { int s=i; while (i<len && (c_isan(src[i])||src[i]=='.')) i++; toks[n].kind=TK_NUM; toks[n].start=s; toks[n].len=i-s; n++; continue; }
        if (c_isa(c)) { int s=i; while (i<len && c_isan(src[i])) i++; toks[n].kind = is_kw(src+s, i-s) ? TK_KW : TK_IDENT; toks[n].start=s; toks[n].len=i-s; n++; continue; }
        toks[n].kind=TK_OP; toks[n].start=i; toks[n].len=1; n++; i++;
    }
    return n;
}

/* ---- caret context ---- */
static int line_start_of(const char *src, int caret){
    int i = caret; while (i > 0 && src[i-1] != '\n') i--; return i;
}
static CmpCtx ctx_at(const char *src, int len, int caret){
    CmpCtx c; c.prefix[0]=0; c.receiver[0]=0; c.after_import=0; c.word_start=caret;
    c.in_string=0; c.recv_str=0;
    if (caret < 0) caret = 0; if (caret > len) caret = len;
    int ls = line_start_of(src, caret);
    /* String-state scan of the line up to the caret. States: CODE; STR (inside a
     * quoted literal, is_f marks an f-string); HOLE (inside an f-string {expr},
     * which is CODE-like); HSTR (a string literal nested in a hole). The caret in
     * STR/HSTR suppresses the popup; in a HOLE completion works as expression
     * context (the M23 f-string holes are full expressions). */
    {
        enum { ST_CODE, ST_STR, ST_HOLE, ST_HSTR } st = ST_CODE;
        char q=0, hq=0; int isf=0, hd=0;
        for (int i = ls; i < caret; i++) {
            char ch = src[i];
            if (st == ST_CODE) {
                if (ch=='#') break;                       /* comment: rest is dead anyway */
                if (ch=='"' || ch=='\'') {
                    isf = (i>ls && (src[i-1]=='f'||src[i-1]=='F') && (i-1==ls || !c_isan(src[i-2])));
                    q = ch; st = ST_STR;
                }
            } else if (st == ST_STR) {
                if (ch=='\\' && i+1<caret) { i++; continue; }
                if (ch==q) { st = ST_CODE; }
                else if (isf && ch=='{') {
                    if (i+1<caret && src[i+1]=='{') { i++; continue; }
                    st = ST_HOLE; hd = 1;
                }
            } else if (st == ST_HOLE) {
                if (ch=='"' || ch=='\'') { hq = ch; st = ST_HSTR; }
                else if (ch=='{'||ch=='('||ch=='[') hd++;
                else if (ch=='}'||ch==')'||ch==']') { hd--; if (hd<=0) st = ST_STR; }
            } else { /* ST_HSTR */
                if (ch=='\\' && i+1<caret) { i++; continue; }
                if (ch==hq) st = ST_HOLE;
            }
        }
        if (st == ST_STR || st == ST_HSTR) { c.in_string = 1; return c; }
    }
    int p = caret; while (p > ls && c_isan(src[p-1])) p--;
    c.word_start = p;
    int plen = caret - p; if (plen > 47) plen = 47;
    for (int i=0;i<plen;i++) c.prefix[i]=src[p+i]; c.prefix[plen]=0;
    int q = p; while (q > ls && c_issp(src[q-1])) q--;
    if (q > ls && src[q-1] == '.') {
        int r = q-1; while (r > ls && c_issp(src[r-1])) r--;
        if (r > ls && (src[r-1]=='"' || src[r-1]=='\'')) {
            c.recv_str = 1;                       /* "literal".<caret> -> string methods */
        } else {
            int re = r; while (r > ls && c_isan(src[r-1])) r--;
            int rl = re - r; if (rl > 47) rl = 47;
            for (int i=0;i<rl;i++) c.receiver[i]=src[r+i]; c.receiver[rl]=0;
        }
    }
    int s = ls; while (s < caret && c_issp(src[s])) s++;
    if ((s + 6 <= len && c_neq(src+s, "import", 6)) || (s + 4 <= len && c_neq(src+s, "from", 4))) c.after_import = 1;
    return c;
}

/* ---- candidate emit + ranking ---- */
static void put(Completion *out, int *n, int max, const char *label, int llen, int kind, int score){
    if (*n >= max) return;
    if (llen > 47) llen = 47;
    Completion *e = &out[*n];
    for (int i=0;i<llen;i++){ e->label[i]=label[i]; e->insert[i]=label[i]; }
    e->label[llen]=0; e->insert[llen]=0; e->kind=kind; e->score=score;
    (*n)++;
}
static int already(Completion *out, int n, const char *s, int slen){
    for (int i=0;i<n;i++) if ((int)c_slen(out[i].label)==slen && c_neq(s, out[i].label, slen)) return 1;
    return 0;
}
/* subsequence match: chars of pre appear in s in order. Bonus score, or -1. */
static int fuzzy(const char *s, const char *pre){
    if (!pre[0]) return 0;
    int si=0, pi=0, bonus=0, run=0, first=-1;
    while (s[si] && pre[pi]) {
        char a=s[si], b=pre[pi];
        char la=(a>='A'&&a<='Z')?a+32:a, lb=(b>='A'&&b<='Z')?b+32:b;
        if (la==lb) { if (first<0) first=si; run++; bonus += 1 + run + (si==0?5:0) + ((si>0 && s[si-1]=='_')?3:0); pi++; }
        else run=0;
        si++;
    }
    if (pre[pi]) return -1;
    bonus -= first;
    return bonus < 0 ? 0 : bonus;
}
static void sort_cmp(Completion *c, int n){
    for (int i=1;i<n;i++){ Completion k=c[i]; int j=i-1;
        while (j>=0 && ( c[j].score < k.score
              || (c[j].score==k.score && c_slen(c[j].label) > c_slen(k.label)) )) { c[j+1]=c[j]; j--; }
        c[j+1]=k; }
}

/* ---- scope model (IDENTIFIER context) ---- */
static int is_assign(const Tok *t, int nt, const char *src, int i){
    return i+1<nt && t[i+1].kind==TK_OP && src[t[i+1].start]=='='
        && !(i+2<nt && t[i+2].kind==TK_OP && src[t[i+2].start]=='=');
}
static int collect_scope(const Tok *t, int nt, const char *src, Completion *out, int max){
    int n = 0;
    for (int i = 0; i < nt; i++) {
        if (t[i].kind==TK_IDENT && is_assign(t,nt,src,i))
            if (!already(out,n,src+t[i].start,t[i].len)) put(out,&n,max,src+t[i].start,t[i].len,CMP_LOCAL,70);
        /* M23 multiple assignment: a, b, c = ... -> every chain ident is a local */
        if (t[i].kind==TK_IDENT && i+1<nt && t[i+1].kind==TK_OP && src[t[i+1].start]==',') {
            int j = i, ok = 0;
            while (j+2 < nt && t[j+1].kind==TK_OP && src[t[j+1].start]==',' && t[j+2].kind==TK_IDENT) j += 2;
            ok = is_assign(t, nt, src, j);
            if (ok) for (int k = i; k <= j; k += 2)
                if (!already(out,n,src+t[k].start,t[k].len)) put(out,&n,max,src+t[k].start,t[k].len,CMP_LOCAL,70);
        }
        if (t[i].kind==TK_KW && (c_neq(src+t[i].start,"def",t[i].len)||c_neq(src+t[i].start,"class",t[i].len)) && i+1<nt && t[i+1].kind==TK_IDENT) {
            int kw_def = c_neq(src+t[i].start,"def",t[i].len);
            if (!already(out,n,src+t[i+1].start,t[i+1].len)) put(out,&n,max,src+t[i+1].start,t[i+1].len, kw_def?CMP_FUNC:CMP_CLASS, 75);
            int j = i+2;
            if (j<nt && t[j].kind==TK_OP && src[t[j].start]=='(') {
                for (j++; j<nt && !(t[j].kind==TK_OP && src[t[j].start]==')'); j++)
                    if (t[j].kind==TK_IDENT && !already(out,n,src+t[j].start,t[j].len))
                        put(out,&n,max,src+t[j].start,t[j].len,CMP_PARAM,80);
            }
        }
        if (t[i].kind==TK_KW && c_neq(src+t[i].start,"for",t[i].len) && i+1<nt && t[i+1].kind==TK_IDENT)
            if (!already(out,n,src+t[i+1].start,t[i+1].len)) put(out,&n,max,src+t[i+1].start,t[i+1].len,CMP_LOCAL,80);
        if (t[i].kind==TK_KW && c_neq(src+t[i].start,"import",t[i].len) && i+1<nt && t[i+1].kind==TK_IDENT)
            if (!already(out,n,src+t[i+1].start,t[i+1].len)) put(out,&n,max,src+t[i+1].start,t[i+1].len,CMP_IMPORT,65);
    }
    return n;
}

/* ---- cross-file module exports (MODULE-on-receiver context) ---- */
static int is_imported(const Tok *t, int nt, const char *src, const char *name){
    int nl = c_slen(name);
    for (int i=0;i+1<nt;i++)
        if (t[i].kind==TK_KW && c_neq(src+t[i].start,"import",t[i].len)
            && t[i+1].kind==TK_IDENT && t[i+1].len==nl && c_neq(src+t[i+1].start,name,nl))
            return 1;
    return 0;
}
static int collect_exports(const char *msrc, int mlen, Completion *out, int max){
    static Tok et[4096];
    int nt = lex(msrc, mlen, et, 4096);
    int n = 0;
    for (int i=0;i<nt;i++){
        int at0 = (et[i].start==0) || (et[i].start>0 && msrc[et[i].start-1]=='\n');
        if (!at0) continue;
        if (et[i].kind==TK_KW && (c_neq(msrc+et[i].start,"def",et[i].len)||c_neq(msrc+et[i].start,"class",et[i].len)) && i+1<nt && et[i+1].kind==TK_IDENT){
            int isdef=c_neq(msrc+et[i].start,"def",et[i].len);
            put(out,&n,max,msrc+et[i+1].start,et[i+1].len,isdef?CMP_FUNC:CMP_CLASS,90);
        } else if (et[i].kind==TK_IDENT && msrc[et[i].start]!='_' && is_assign(et,nt,msrc,i)){
            put(out,&n,max,msrc+et[i].start,et[i].len,CMP_GLOBAL,85);
        }
    }
    return n;
}

/* ---- type table (MEMBER context) ---- */
static int is_class(const Tok *t, int nt, const char *src, const char *nm, int nl){
    /* nm is a slice of the source buffer (not NUL-terminated) -> length compare */
    for (int i=0;i+1<nt;i++)
        if (t[i].kind==TK_KW && c_neq(src+t[i].start,"class",t[i].len)
            && t[i+1].kind==TK_IDENT && t[i+1].len==nl && c_eqn(src+t[i+1].start,nm,nl)) return 1;
    return 0;
}
static int type_of(const Tok *t, int nt, const char *src, int caret, const char *var, char *cls, int cmax){
    int vl = c_slen(var), ty = TY_UNKNOWN;
    if (cls && cmax) cls[0]=0;
    for (int i=0;i+1<nt;i++){
        if (t[i].start >= caret) break;
        if (!(t[i].kind==TK_IDENT && t[i].len==vl && c_neq(src+t[i].start,var,vl))) continue;
        if (!is_assign(t,nt,src,i)) continue;
        if (i+2 >= nt) { ty = TY_UNKNOWN; continue; }
        const Tok *r = &t[i+2];
        char rc = src[r->start];
        /* RHS shape -> type. Checked in specificity order. */
        int mty = TY_UNKNOWN;                        /* X.method(...) result type */
        if ((r->kind==TK_STR || r->kind==TK_IDENT) && i+4<nt
            && t[i+3].kind==TK_DOT && t[i+4].kind==TK_IDENT) {
            const char *m = src + t[i+4].start; int ml = t[i+4].len;
            if (c_neq(m,"join",ml)||c_neq(m,"strip",ml)||c_neq(m,"upper",ml)
                ||c_neq(m,"lower",ml)||c_neq(m,"replace",ml)) mty = TY_STR;
            else if (c_neq(m,"split",ml)||c_neq(m,"keys",ml)||c_neq(m,"values",ml)) mty = TY_LIST;
            else if (c_neq(m,"find",ml)) mty = TY_INT;
        }
        if (mty != TY_UNKNOWN) ty = mty;
        else if (rc=='[') ty=TY_LIST;
        else if (rc=='{') ty=TY_DICT;
        else if (r->kind==TK_STR) ty=TY_STR;
        else if (r->kind==TK_IDENT && r->len==1 && (rc=='f'||rc=='F')
                 && i+3<nt && t[i+3].kind==TK_STR) ty=TY_STR;   /* x = f"..." */
        else if (r->kind==TK_IDENT && c_neq(src+r->start,"str",r->len)
                 && i+3<nt && t[i+3].kind==TK_OP && src[t[i+3].start]=='(') ty=TY_STR;
        else if (r->kind==TK_NUM) ty = has_dot(src+r->start, r->len) ? TY_FLOAT : TY_INT;
        /* M27: `p = open(...)` / `port(...)` is a port, `c = run(...)` a process.
         * Same shape as the `str(...)` rule above -- a builtin name followed by
         * '(' -- which is as much inference as this engine does anywhere. */
        else if (r->kind==TK_IDENT && i+3<nt && t[i+3].kind==TK_OP && src[t[i+3].start]=='('
                 && (c_neq(src+r->start,"open",r->len) || c_neq(src+r->start,"port",r->len))) ty=TY_PORT;
        else if (r->kind==TK_IDENT && i+3<nt && t[i+3].kind==TK_OP && src[t[i+3].start]=='('
                 && c_neq(src+r->start,"run",r->len)) ty=TY_PROC;
        /* M28: `c = caps()` is a capability. Its methods all NARROW, so the
         * completion list doubles as the complete list of what one can do. */
        else if (r->kind==TK_IDENT && i+3<nt && t[i+3].kind==TK_OP && src[t[i+3].start]=='('
                 && c_neq(src+r->start,"caps",r->len)) ty=TY_CAP;
        else if (r->kind==TK_IDENT && i+3<nt && t[i+3].kind==TK_OP && src[t[i+3].start]=='('
                 && is_class(t,nt,src,src+r->start,r->len)) {
            ty=TY_INSTANCE;
            int k=r->len>cmax-1?cmax-1:r->len;
            if (cls){ for (int j=0;j<k;j++) cls[j]=src[r->start+j]; cls[k]=0; }
        } else ty=TY_UNKNOWN;
    }
    return ty;
}
static int collect_class(const Tok *t, int nt, const char *src, const char *cls, int cl, Completion *out, int max){
    int n=0, i=0, found=0;
    for (; i+1<nt; i++)
        if (t[i].kind==TK_KW && c_neq(src+t[i].start,"class",t[i].len)
            && t[i+1].kind==TK_IDENT && t[i+1].len==cl && c_neq(src+t[i+1].start,cls,cl)) { found=1; i+=2; break; }
    if (!found) return 0;
    for (; i<nt; i++){
        int at0 = (t[i].start==0) || (t[i].start>0 && src[t[i].start-1]=='\n');
        if (at0 && t[i].kind!=TK_NL) break;                  /* column-0 token = left class body */
        if (t[i].kind==TK_KW && c_neq(src+t[i].start,"def",t[i].len) && i+1<nt && t[i+1].kind==TK_IDENT)
            if (!already(out,n,src+t[i+1].start,t[i+1].len)) put(out,&n,max,src+t[i+1].start,t[i+1].len,CMP_METHOD,90);
        if (t[i].kind==TK_IDENT && c_neq(src+t[i].start,"self",t[i].len) && i+2<nt && t[i+1].kind==TK_DOT && t[i+2].kind==TK_IDENT)
            if (!already(out,n,src+t[i+2].start,t[i+2].len)) put(out,&n,max,src+t[i+2].start,t[i+2].len,CMP_FIELD,85);
    }
    return n;
}

/* ---- providers ---- */
static cmp_list_modules_fn g_list = 0;
static cmp_read_module_fn  g_read = 0;
void as_complete_set_providers(cmp_list_modules_fn l, cmp_read_module_fn r){ g_list=l; g_read=r; }

/* ---- the dispatcher ---- */
int as_complete(const char *src, int len, int caret, Completion *out, int max){
    if (!src || max <= 0) return 0;
    if (len < 0) len = 0;
    CmpCtx cx = ctx_at(src, len, caret);
    static Tok toks[4096];
    int nt = lex(src, len, toks, 4096);
    static Completion all[512];
    int na = 0;

    if (cx.in_string) return 0;                /* no popup inside string text */
    if (cx.recv_str) {                          /* "literal". -> string methods */
        for (int i=0;STR_METHODS[i]&&na<512;i++) put(all,&na,512,STR_METHODS[i],c_slen(STR_METHODS[i]),CMP_METHOD,90);
        goto rank;
    }
    if (cx.after_import && cx.receiver[0]==0) {
        char names[64][48];
        int nm = g_list ? g_list(names, 64) : 0;
        for (int i=0;i<nm && na<512;i++) put(all,&na,512,names[i],c_slen(names[i]),CMP_MODULE,90);
        goto rank;
    }
    if (cx.receiver[0] && g_read && is_imported(toks, nt, src, cx.receiver)) {
        static char mbuf[65536];
        int ml = g_read(cx.receiver, mbuf, (int)sizeof mbuf);
        if (ml > 0) na = collect_exports(mbuf, ml, all, 512);
        goto rank;
    }
    if (cx.receiver[0]) {
        if (c_neq(cx.receiver, "self", c_slen(cx.receiver))) {
            /* self.<x> inside a class body: members of the ENCLOSING class --
             * the nearest `class NAME` whose body the caret is inside. */
            const char *cn = 0; int cl = 0;
            for (int i=0; i+1<nt && toks[i].start < caret; i++)
                if (toks[i].kind==TK_KW && c_neq(src+toks[i].start,"class",toks[i].len)
                    && toks[i+1].kind==TK_IDENT) { cn = src+toks[i+1].start; cl = toks[i+1].len; }
            if (cn) {
                char cls2[48]; int k = cl > 47 ? 47 : cl;
                for (int j=0;j<k;j++) cls2[j]=cn[j]; cls2[k]=0;
                na = collect_class(toks,nt,src,cls2,k,all,512);
            }
            goto rank;
        }
        char cls[48];
        int ty = type_of(toks, nt, src, caret, cx.receiver, cls, (int)sizeof cls);
        if (ty==TY_LIST)          for (int i=0;LIST_METHODS[i]&&na<512;i++) put(all,&na,512,LIST_METHODS[i],c_slen(LIST_METHODS[i]),CMP_METHOD,90);
        else if (ty==TY_DICT)     for (int i=0;DICT_METHODS[i]&&na<512;i++) put(all,&na,512,DICT_METHODS[i],c_slen(DICT_METHODS[i]),CMP_METHOD,90);
        else if (ty==TY_STR)      for (int i=0;STR_METHODS[i]&&na<512;i++) put(all,&na,512,STR_METHODS[i],c_slen(STR_METHODS[i]),CMP_METHOD,90);
        else if (ty==TY_PORT)     for (int i=0;PORT_METHODS[i]&&na<512;i++) put(all,&na,512,PORT_METHODS[i],c_slen(PORT_METHODS[i]),CMP_METHOD,90);
        else if (ty==TY_PROC)     for (int i=0;PROC_METHODS[i]&&na<512;i++) put(all,&na,512,PROC_METHODS[i],c_slen(PROC_METHODS[i]),CMP_METHOD,90);
        else if (ty==TY_CAP)      for (int i=0;CAP_METHODS[i]&&na<512;i++) put(all,&na,512,CAP_METHODS[i],c_slen(CAP_METHODS[i]),CMP_METHOD,90);
        else if (ty==TY_INSTANCE) na = collect_class(toks,nt,src,cls,c_slen(cls),all,512);
        goto rank;
    }

    na = collect_scope(toks, nt, src, all, 512);
    for (int i=0; BUILTINS[i] && na<512; i++) put(all,&na,512,BUILTINS[i],c_slen(BUILTINS[i]),CMP_BUILTIN,40);
    for (int i=0; SYSCONSTS[i] && na<512; i++) put(all,&na,512,SYSCONSTS[i],c_slen(SYSCONSTS[i]),CMP_BUILTIN,35);
    for (int i=0; KEYWORDS[i] && na<512; i++) put(all,&na,512,KEYWORDS[i],c_slen(KEYWORDS[i]),CMP_KEYWORD,30);

rank: ;
    int n = 0;
    for (int i=0;i<na;i++){
        int f = fuzzy(all[i].label, cx.prefix);
        if (f < 0) continue;
        all[i].score += f;
        if (n < max) out[n++] = all[i];
    }
    sort_cmp(out, n);
    return n;
}

/* ---- test hooks ---- */
#ifdef AS_COMPLETE_TEST
int as__lex_test(const char *src, int len, int *kinds, int max){
    static Tok t[4096]; int n = lex(src, len, t, max < 4096 ? max : 4096);
    for (int i=0;i<n;i++) kinds[i]=t[i].kind;
    return n;
}
CmpCtx as__ctx_test(const char *src, int len, int caret){ return ctx_at(src, len, caret); }
int as__typeof_test(const char *src, int len, int caret, const char *var, char *cls, int cmax){
    static Tok t[4096]; int nt = lex(src, len, t, 4096);
    return type_of(t, nt, src, caret, var, cls, cmax);
}
#endif
