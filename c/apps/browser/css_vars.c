/* CSS custom properties (var()) preprocessor.
 *
 * The vendored LibCSS (third_party/css) predates CSS variables, so sites whose
 * colors are all `var(--x)` (github, bilibili) come back unstyled. We resolve
 * var() in the raw stylesheet text before LibCSS sees it:
 *   1. collect every `--name: value` declaration (flat, last-wins -- this
 *      ignores per-selector cascade, a pragmatic simplification);
 *   2. resolve indirection among them (var(--a) whose value is var(--b));
 *   3. substitute `var(--name[, fallback])` throughout the sheet.
 *
 * Pure text in/out (depends only on strlen) so it is unit-tested on the host
 * (tools/t/var_test.c) independently of LibCSS. */

unsigned long strlen(const char *);

#define MAXVARS 1024
#define VNAME   48
#define VVAL    160
static char vnames[MAXVARS][VNAME];
static char vvals[MAXVARS][VVAL];
static int  nvars_;

static int vid(int c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'; }

static int var_find(const char *name, int nlen){
    for (int i = nvars_ - 1; i >= 0; i--) {          /* last-wins: search newest first */
        int j = 0; for (; j < nlen && vnames[i][j]; j++) if (vnames[i][j] != name[j]) break;
        if (j == nlen && vnames[i][j] == 0) return i;
    }
    return -1;
}
static void var_set(const char *name, int nlen, const char *val, int vlen){
    if (nlen <= 0 || nlen >= VNAME) return;
    if (vlen >= VVAL) vlen = VVAL - 1; if (vlen < 0) vlen = 0;
    int idx = var_find(name, nlen);
    if (idx < 0) { if (nvars_ >= MAXVARS) return; idx = nvars_++;
                   for (int k = 0; k < nlen; k++) vnames[idx][k] = name[k]; vnames[idx][nlen] = 0; }
    for (int k = 0; k < vlen; k++) vvals[idx][k] = val[k]; vvals[idx][vlen] = 0;
}

/* substitute var() in s[0..n) into out (single pass); returns out length.
 * `depth` bounds fallback recursion: nested var(--x, var(--x, ...)) needs only
 * ~8 input bytes per level, so an unbounded recursion would blow the ring-3
 * stack on a large stylesheet. Past the limit the fallback is left unexpanded. */
#define VAR_MAX_DEPTH 32
static int var_subst(const char *s, int n, char *out, int omax, int depth){
    int o = 0;
    for (int i = 0; i < n && o < omax - 1;) {
        if (i + 4 <= n && s[i]=='v'&&s[i+1]=='a'&&s[i+2]=='r'&&s[i+3]=='(') {
            int p = i + 4; while (p < n && s[p]==' ') p++;
            if (p + 1 < n && s[p]=='-' && s[p+1]=='-') {
                int ns = p + 2, ne = ns; while (ne < n && vid(s[ne])) ne++;
                int depth_ = 1, r = ne, fbs = -1;
                while (r < n && depth_ > 0) { char c = s[r];
                    if (c=='(') depth_++; else if (c==')') { depth_--; if (!depth_) break; }
                    else if (c==',' && depth_==1 && fbs < 0) fbs = r + 1; r++; }
                int close = r;                            /* s[close]==')' or n */
                int idx = var_find(s + ns, ne - ns);
                if (idx >= 0) { for (int k = 0; vvals[idx][k] && o < omax-1; k++) out[o++] = vvals[idx][k]; }
                else if (fbs >= 0 && depth < VAR_MAX_DEPTH) { int fe = close; while (fbs < fe && s[fbs]==' ') fbs++;
                                     o += var_subst(s + fbs, fe - fbs, out + o, omax - o, depth + 1); }  /* fallback may itself be var() */
                i = (close < n) ? close + 1 : n;
                continue;
            }
        }
        out[o++] = s[i++];
    }
    out[o] = 0; return o;
}
static int has_var(const char *s){ for (; s[0]; s++) if (s[0]=='v'&&s[1]=='a'&&s[2]=='r'&&s[3]=='(') return 1; return 0; }

int css_expand_vars(const char *in, int inlen, char *out, int outmax){
    nvars_ = 0; char prev = 0;
    for (int i = 0; i + 2 < inlen; i++) {              /* pass 1: collect --name:value */
        char c = in[i];
        if (c=='-' && in[i+1]=='-' && (prev=='{' || prev==';')) {
            int ns = i + 2, ne = ns; while (ne < inlen && vid(in[ne])) ne++;
            int p = ne; while (p < inlen && (in[p]==' '||in[p]=='\t')) p++;
            if (p < inlen && in[p]==':') { p++; while (p < inlen && (in[p]==' '||in[p]=='\t')) p++;
                int vs = p; while (p < inlen && in[p]!=';' && in[p]!='}') p++;
                int ve = p; while (ve > vs && (in[ve-1]==' '||in[ve-1]=='\t'||in[ve-1]=='\n'||in[ve-1]=='\r')) ve--;
                var_set(in + ns, ne - ns, in + vs, ve - vs);
            }
        }
        if (c!=' '&&c!='\t'&&c!='\n'&&c!='\r') prev = c;
    }
    for (int it = 0; it < 8; it++) {                   /* pass 2: resolve var()-in-value chains */
        int changed = 0; char tmp[VVAL];
        for (int i = 0; i < nvars_; i++) {
            if (!has_var(vvals[i])) continue;
            int tn = var_subst(vvals[i], (int)strlen(vvals[i]), tmp, VVAL, 0);
            int same = 1; for (int k = 0; k <= tn; k++) if (tmp[k] != vvals[i][k]) { same = 0; break; }
            if (!same) { for (int k = 0; k <= tn; k++) vvals[i][k] = tmp[k]; changed = 1; }
        }
        if (!changed) break;
    }
    return var_subst(in, inlen, out, outmax, 0);       /* pass 3: substitute throughout */
}
