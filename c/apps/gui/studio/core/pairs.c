/* SPDX-License-Identifier: MIT */
#include "internal.h"

static unsigned closing(unsigned cp)
{
    switch(cp){case '(':return ')';case '[':return ']';case '{':return '}';
    case '\'':case '"':return cp;
    case 0x2018:return 0x2019;case 0x201c:return 0x201d;
    case 0xff08:return 0xff09;case 0x3010:return 0x3011;default:return 0;}
}
static unsigned decode(const char *s,int n)
{
    unsigned c=(unsigned char)s[0];if(c<128)return c;
    unsigned cp=c&(n==2?31:n==3?15:7);
    for(int i=1;i<n;i++)cp=(cp<<6)|((unsigned char)s[i]&63);return cp;
}
/* Tolerant lexical context, including AS's multiline quoted literals. Pairing
 * must not turn a quote escaped with a backslash into a string terminator, or
 * insert programming punctuation into comments and ordinary string text.
 * Interpolation holes currently inherit string context (conservative). */
static int context(const StDocument *d,int at,int *escaped)
{
    int quote=0,comment=0,escape=0;
    for(int i=0;i<at;i++){char c=d->text[i];
        if(comment){if(c=='\n')comment=0;continue;}
        if(quote){if(escape)escape=0;else if(c=='\\')escape=1;else if(c==quote)quote=0;}
        else if(c=='#')comment=1;else if(c=='\''||c=='"')quote=c;
    }*escaped=escape;return comment?-1:quote;
}
int st_engine_type(StEngine *e,unsigned cp)
{
    StDocument *d=st_current(e);if(!d)return -1;
    char left[4],right[4];int ln=st_encode(cp,left);if(!ln)return -1;
    unsigned close=closing(cp);int rn=close?st_encode(close,right):0;
    int lo=d->caret<d->anchor?d->caret:d->anchor,hi=d->caret>d->anchor?d->caret:d->anchor;
    if(close&&lo<hi){
        int n=hi-lo;if(n>ST_DOCUMENT_LIMIT-ln-rn)return -1;
        char *wrapped=malloc((size_t)(n+ln+rn));if(!wrapped)return -1;
        memcpy(wrapped,left,(size_t)ln);memcpy(wrapped+ln,d->text+lo,(size_t)n);memcpy(wrapped+ln+n,right,(size_t)rn);
        int caret=d->caret,anchor=d->anchor,r=st_apply(d,lo,hi,wrapped,n+ln+rn,1);free(wrapped);
        if(!r){d->caret=caret+ln;d->anchor=anchor+ln;st_changed(e);}return r;
    }
    int escaped=0,ctx=context(d,lo,&escaped);
    int terminator=cp==')'||cp==']'||cp=='}'||cp==0x2019||cp==0x201d||cp==0xff09||cp==0x3011;
    if(lo==hi&&hi+ln<=d->length&&!memcmp(d->text+hi,left,(size_t)ln)&&
       ((terminator&&ctx==0)||((cp=='\''||cp=='"')&&ctx==(int)cp&&!escaped))){
        st_engine_select(e,hi+ln,hi+ln);return 0;
    }
    /* Inside a word/string/comment, inserting a single quote is intentional.
     * An opener before an identifier also remains single: wrapping requires
     * an explicit selection, so typing beside existing code cannot consume it. */
    int boundary=hi==d->length||strchr(" \t\r\n)]},:;",d->text[hi])!=NULL;
    int previous_word=lo>0&&((d->text[lo-1]>='a'&&d->text[lo-1]<='z')||
                      (d->text[lo-1]>='A'&&d->text[lo-1]<='Z')||d->text[lo-1]=='_');
    if(close&&ctx==0&&boundary&&!((cp=='\''||cp=='"')&&previous_word)){
        char pair[8];memcpy(pair,left,(size_t)ln);memcpy(pair+ln,right,(size_t)rn);
        int r=st_engine_insert(e,pair,ln+rn);if(!r)d->caret=d->anchor=lo+ln;return r;
    }
    return st_engine_insert(e,left,ln);
}
int st_engine_backspace(StEngine *e)
{
    StDocument *d=st_current(e);if(!d)return -1;
    if(d->caret==d->anchor&&d->caret>0&&d->caret<d->length){
        int lo=st_prev(d->text,d->caret),hi=st_next(d->text,d->caret,d->length);
        unsigned left=decode(d->text+lo,d->caret-lo),right=decode(d->text+d->caret,hi-d->caret);
        int escaped=0,ctx=context(d,lo,&escaped);
        if(ctx==0&&closing(left)==right){
            int r=st_apply(d,lo,hi,"",0,1);if(!r)st_changed(e);return r;
        }
    }return st_engine_delete(e,0);
}
