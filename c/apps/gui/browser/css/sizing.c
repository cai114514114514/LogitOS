/* Preferred ratios and the normal/start/stretch distinction, CSS Sizing 4
 * and Box Alignment 3. Included by extra.c like its existing grammar modules:
 * retaining that consumer keeps old host source lists on the real producer.
 *
 * LibCSS used FLEX_START/STRETCH as initial encodings and consequently could
 * not tell an author's explicit value from Grid's normal initial value. The
 * complete author declaration set is captured by the SAME matched-selector,
 * importance and declaration-order cascade, rather than using a last-string
 * override that would discard an earlier !important winner.
 *
 * Ratios accept finite nonnegative CSS numbers, auto, and CSS-wide defaults.
 * calc()/var() are not guessed here: var() must already have been expanded;
 * unsupported calculated ratios publish no patch. Zero is valid syntax but
 * a degenerate ratio is unusable, which layout must handle without division.
 */
#include "css/sizing.h"
#include "css/style.h"
#include <string.h>
#include <float.h>
void *kmalloc(unsigned long);
void kfree(void *);
const char *css_sizing_property(int index)
{ return index==0?"aspect-ratio":NULL; }

static int sizing_space(int c)
{ return c==' ' || c=='\t' || c=='\n' || c=='\r' || c=='\f'; }

static int sizing_word(const char *s, int n, const char *word)
{
    int i = 0;
    while (i < n && word[i]) {
        int c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != word[i]) return 0;
        i++;
    }
    return i == n && !word[i];
}

static void sizing_trim(const char **s, int *n)
{
    while (*n && sizing_space(**s)) { (*s)++; (*n)--; }
    while (*n && sizing_space((*s)[*n-1])) (*n)--;
}

/* No strtod on a borrowed, non-NUL-terminated sheet span. Work is bounded by
 * the input and exponent range; overflow refuses the whole declaration. */
static int sizing_number(const char *s, int n, int *at, double *out)
{
    int p = *at, negative = 0, digits = 0;
    double value = 0, fraction = 0.1;
    if (p < n && (s[p] == '+' || s[p] == '-')) negative = s[p++] == '-';
    while (p < n && s[p] >= '0' && s[p] <= '9') {
        int digit=s[p++]-'0';
        if(value>(DBL_MAX-digit)/10)return 0;
        value = value * 10 + digit; digits++;
    }
    if (p < n && s[p] == '.') {
        if (p+1 >= n || s[p+1] < '0' || s[p+1] > '9') return 0;
        p++;
        while (p < n && s[p] >= '0' && s[p] <= '9') {
            value += (s[p++] - '0') * fraction; fraction *= 0.1; digits++;
        }
    }
    if (!digits) return 0;
    if (p < n && (s[p] == 'e' || s[p] == 'E')) {
        int sign = 1, exponent = 0, count = 0;
        p++;
        if (p < n && (s[p] == '+' || s[p] == '-')) sign = s[p++] == '-' ? -1 : 1;
        while (p < n && s[p] >= '0' && s[p] <= '9') {
            exponent = exponent * 10 + s[p++] - '0'; count++;
            if (exponent > 308) return 0;
        }
        if (!count) return 0;
        while (exponent--) {
            if(sign>0 && value>DBL_MAX/10)return 0;
            value = sign < 0 ? value / 10 : value * 10;
        }
    }
    if (negative && value != 0) return 0;
    *at = p; *out = value;
    return 1;
}

static int sizing_ratio(const char *s, int n, struct css_sizing_patch *out)
{
#ifdef CSS_SIZING_NO_RATIO
    (void)s; (void)n; (void)out;
    return 0; /* producer control: keep alignment and the rest of extra intact */
#else
    int p = 0, auto_seen = 0, ratio_seen = 0;
    out->width = out->height = 0;
    for (;;) {
        while (p < n && sizing_space(s[p])) p++;
        if (p == n) break;
        if (n-p >= 4 && sizing_word(s+p, 4, "auto") &&
            (p+4 == n || sizing_space(s[p+4]))) {
            if (auto_seen) return 0;
            auto_seen = 1; p += 4;
        } else {
            if (ratio_seen || !sizing_number(s, n, &p, &out->width)) return 0;
            out->height = 1; ratio_seen = 1;
            while (p < n && sizing_space(s[p])) p++;
            if (p < n && s[p] == '/') {
                p++;
                while (p < n && sizing_space(s[p])) p++;
                if (!sizing_number(s, n, &p, &out->height)) return 0;
            }
            if (p < n && !sizing_space(s[p])) return 0;
        }
    }
    if (!auto_seen && !ratio_seen) return 0;
    out->automatic = (unsigned char)auto_seen;
    out->has_ratio = (unsigned char)ratio_seen;
    out->ratio_set = 1;
    return 1;
#endif
}

int css_sizing_declaration(const char *property, int plen,
                           const char *value, int vlen,
                           struct css_sizing_patch *out)
{
    if (!property || !value || !out || plen < 0 || vlen < 0) return 0;
    /* Comments are token trivia, including around '/' and keyword values.
     * Preserve token boundaries with a space, never join auto and 1 across it.
     * A ratio has no quoted/function grammar, so comment scanning here cannot
     * reinterpret a string. Calculations remain an explicit refusal. */
    for(int i=0;i+1<vlen;i++)if(value[i]=='/'&&value[i+1]=='*') {
        char *clean=kmalloc((unsigned long)vlen+1);if(!clean)return 0;
        int j=0;
        for(int k=0;k<vlen;k++) {
            if(k+1<vlen && value[k]=='/' && value[k+1]=='*') {
                k+=2;while(k+1<vlen && !(value[k]=='*'&&value[k+1]=='/'))k++;
                if(k+1==vlen){kfree(clean);return 0;}
                k++;clean[j++]=' ';
            } else clean[j++]=value[k];
        }
        clean[j]=0;int ok=css_sizing_declaration(property,plen,clean,j,out);
        kfree(clean);return ok;
    }
    memset(out, 0, sizeof *out);
    sizing_trim(&value, &vlen);
    int ratio = sizing_word(property, plen, css_sizing_property(0));
    int justify = sizing_word(property, plen, "justify-content");
    int items = sizing_word(property, plen, "align-items");
    int self = sizing_word(property, plen, "align-self");
    if (!ratio && !justify && !items && !self) return 0;
    int parent = sizing_word(value, vlen, "inherit");
    int initial = sizing_word(value, vlen, "initial") ||
                  sizing_word(value, vlen, "unset") ||
                  sizing_word(value, vlen, "revert");
    if (ratio) {
#ifdef CSS_SIZING_NO_RATIO
        return 0;
#endif
        if (parent || initial) {
            out->ratio_set = 1; out->automatic = parent ? 2 : 1;
            return 1;
        }
        return sizing_ratio(value, vlen, out);
    }
    int result = -1;
    if (parent) result = CSS_SIZING_PARENT;
    else if (initial) result = justify ? JC_NORMAL : self ? AL_AUTO : AL_NORMAL;
    else if (sizing_word(value, vlen, "normal")) result = justify ? JC_NORMAL : AL_NORMAL;
    else if (sizing_word(value, vlen, "stretch")) result = justify ? JC_STRETCH : AL_STRETCH;
    else if (sizing_word(value, vlen, "start") || sizing_word(value, vlen, "flex-start"))
        result = justify ? JC_START : AL_START;
    else if (sizing_word(value, vlen, "end") || sizing_word(value, vlen, "flex-end"))
        result = justify ? JC_END : AL_END;
    else if (sizing_word(value, vlen, "center")) result = justify ? JC_CENTER : AL_CENTER;
    else if (justify && sizing_word(value, vlen, "space-between")) result = JC_BETWEEN;
    else if (justify && sizing_word(value, vlen, "space-around")) result = JC_AROUND;
    else if (justify && sizing_word(value, vlen, "space-evenly")) result = JC_EVENLY;
    else if (!justify && sizing_word(value, vlen, "baseline")) result = AL_BASELINE;
    else if (self && sizing_word(value, vlen, "auto")) result = AL_AUTO;
    if (result < 0) return 0;
    if (justify) { out->justify_set = 1; out->justify = result; }
    if (items) { out->items_set = 1; out->items = result; }
    if (self) { out->self_set = 1; out->self = result; }
    return 1;
}

int css_sizing_supports(const char *property, int plen, const char *value, int vlen)
{
    struct css_sizing_patch patch;
    return css_sizing_declaration(property, plen, value, vlen, &patch);
}

void css_sizing_merge(struct css_sizing_patch *to, const struct css_sizing_patch *from)
{
    if (from->ratio_set) {
        to->ratio_set = 1; to->width = from->width; to->height = from->height;
        to->automatic = from->automatic;
        to->has_ratio = from->has_ratio;
    }
    if (from->justify_set) { to->justify_set = 1; to->justify = from->justify; }
    if (from->items_set) { to->items_set = 1; to->items = from->items; }
    if (from->self_set) { to->self_set = 1; to->self = from->self; }
}

void css_sizing_apply(struct cstyle *style, const struct css_sizing_patch *patch)
{
    if (patch->ratio_set) {
        style->aspect_w = patch->width; style->aspect_h = patch->height;
        style->aspect_auto = patch->automatic;
        style->aspect_has_ratio = patch->has_ratio;
    }
    if (patch->justify_set) style->justify = patch->justify;
    if (patch->items_set) style->align_items = patch->items;
    if (patch->self_set) style->align_self = patch->self;
}

void css_sizing_resolve(struct cstyle *style, const struct cstyle *parent)
{
    if (style->aspect_auto == 2) {
        style->aspect_w = parent ? parent->aspect_w : 0;
        style->aspect_h = parent ? parent->aspect_h : 0;
        style->aspect_auto = parent ? parent->aspect_auto : 1;
        style->aspect_has_ratio = parent ? parent->aspect_has_ratio : 0;
    }
    if (style->justify == CSS_SIZING_PARENT) style->justify = parent ? parent->justify : JC_NORMAL;
    if (style->align_items == CSS_SIZING_PARENT) style->align_items = parent ? parent->align_items : AL_NORMAL;
    if (style->align_self == CSS_SIZING_PARENT) style->align_self = parent ? parent->align_self : AL_AUTO;
}
