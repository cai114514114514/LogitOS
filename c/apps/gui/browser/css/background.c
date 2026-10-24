/* Background grammar shared by cascade, resource discovery and painting.
 * Do not extract url() with a substring search: quoted ')' and data payloads
 * are ordinary URLs, while a URL in a second layer is not our supported image.
 * Old claim: multiple layers and alternate origin/clip boxes were absent.
 * Correction (2026-10-02): whole lists and shorthand components now travel
 * through the same cascade slots; each layer has its own geometry and clip.
 * Fixed/local attachment still requires a scroll/viewport coordinate model
 * and is refused, rather than silently painted as an ordinary scroll image.
 * This file is included by extra.c, like the existing grid/flex implementation
 * seam: every host consumer that links the CSS producer gets the same parser. */
#include "background.h"
#include "css/style.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../../../../include/weaksym.h"
extern int img_css_color(const char *,int,unsigned char *) LOGIT_WEAK;
#ifndef CSS_BACKGROUND_EMBEDDED
LOGIT_WEAK_STUB(img_css_color);
#endif

struct bg_scan { const char *s; int n, i; };
static int bg_whitespace(char c)
{return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f';}
static void bg_space(struct bg_scan *k)
{
    for (;;) {
        while (k->i < k->n && bg_whitespace(k->s[k->i])) k->i++;
        if (k->i+1 >= k->n || k->s[k->i] != '/' || k->s[k->i+1] != '*') return;
        k->i += 2;
        while (k->i+1 < k->n && !(k->s[k->i] == '*' && k->s[k->i+1] == '/')) k->i++;
        k->i = k->i+1 < k->n ? k->i+2 : k->n;
    }
}
static int bg_word(struct bg_scan *k, const char *word)
{
    int start=k->i, i=0;
    while (word[i] && start+i<k->n && (k->s[start+i]|32)==word[i]) i++;
    if (word[i]) return 0;
    char next=start+i<k->n?k->s[start+i]:0;
    if ((next>='a'&&next<='z') || (next>='A'&&next<='Z') ||
        (next>='0'&&next<='9') || next=='-' || next=='_' || next=='\\' ||
        (unsigned char)next>=128) return 0;
    k->i+=i; return 1;
}
static int bg_hex(char c)
{
    if(c>='0'&&c<='9')return c-'0';
    c|=32; return c>='a'&&c<='f'?c-'a'+10:-1;
}
static int bg_utf8(char *out,int capacity,int *used,unsigned cp)
{
    unsigned char bytes[4]; int count;
    if (!cp || cp>0x10ffff || (cp>=0xd800&&cp<=0xdfff)) cp=0xfffd;
    if(cp<128){bytes[0]=cp;count=1;}
    else if(cp<2048){bytes[0]=0xc0|(cp>>6);bytes[1]=0x80|(cp&63);count=2;}
    else if(cp<65536){bytes[0]=0xe0|(cp>>12);bytes[1]=0x80|((cp>>6)&63);bytes[2]=0x80|(cp&63);count=3;}
    else{bytes[0]=0xf0|(cp>>18);bytes[1]=0x80|((cp>>12)&63);bytes[2]=0x80|((cp>>6)&63);bytes[3]=0x80|(cp&63);count=4;}
    if(*used+count>=capacity)return 0;
    for(int i=0;i<count;i++)out[(*used)++]=bytes[i];return 1;
}
static int bg_url_token(struct bg_scan *scan,char *url,int capacity)
{
    struct bg_scan k=*scan;int used=0;bg_space(&k);
    if(!bg_word(&k,"url") || k.i==k.n || k.s[k.i++]!='(')return 0;
    bg_space(&k);char quote=0;
    if(k.i<k.n&&(k.s[k.i]=='\''||k.s[k.i]=='"'))quote=k.s[k.i++];
    int closed=0;
    while(k.i<k.n){
        unsigned char c=k.s[k.i++];
        if(quote?c==quote:c==')'){closed=1;break;}
        if(c=='\\'){
            if(k.i==k.n)return 0;
            unsigned cp=0;int digits=0;
            while(k.i<k.n&&digits<6&&bg_hex(k.s[k.i])>=0){cp=cp*16+bg_hex(k.s[k.i++]);digits++;}
            if(digits){
                if(k.i<k.n&&bg_whitespace(k.s[k.i])){char ws=k.s[k.i++];if(ws=='\r'&&k.i<k.n&&k.s[k.i]=='\n')k.i++;}
                if(!bg_utf8(url,capacity,&used,cp))return 0;
                continue;
            }
            c=k.s[k.i++];
            if(c=='\n'||c=='\r'||c=='\f'){
                if(!quote)return 0;if(c=='\r'&&k.i<k.n&&k.s[k.i]=='\n')k.i++;continue;
            }
        }else if(!quote&&bg_whitespace(c)){bg_space(&k);if(k.i>=k.n||k.s[k.i++]!=')')return 0;closed=1;break;}
        else if(c<' ' || (!quote&&(c=='('||c=='\''||c=='"')))return 0;
        if(used+1>=capacity)return 0;url[used++]=c;
    }
    if(!closed)return 0;
    if(quote){bg_space(&k);if(k.i==k.n||k.s[k.i++]!=')')return 0;}
    if(!used)return 0;url[used]=0;*scan=k;return 1;
}
int css_background_url(const char *value,int len,char *url,int capacity)
{
    if(!value||len<=0||!url||capacity<2)return 0;
    struct bg_scan k={value,len,0};
    if(!bg_url_token(&k,url,capacity))return 0;
    bg_space(&k);return k.i==k.n;
}
int css_background_url_token(const char *value,int len,int *consumed,char *url,int capacity)
{
    if(!value||len<=0||!consumed||!url||capacity<2)return 0;
    struct bg_scan k={value,len,0};
    if(!bg_url_token(&k,url,capacity))return 0;*consumed=k.i;return 1;
}
static int bg_number(struct bg_scan *k,double *out)
{
    int start=k->i,i=start,digits=0;
    if(i<k->n&&(k->s[i]=='+'||k->s[i]=='-'))i++;
    while(i<k->n&&k->s[i]>='0'&&k->s[i]<='9'){i++;digits++;}
    if(i+1<k->n&&k->s[i]=='.'&&k->s[i+1]>='0'&&k->s[i+1]<='9'){
        i++;while(i<k->n&&k->s[i]>='0'&&k->s[i]<='9'){i++;digits++;}
    }
    if(!digits)return 0;
    if(i<k->n&&(k->s[i]=='e'||k->s[i]=='E')){
        int e=i++,ed=0;if(i<k->n&&(k->s[i]=='+'||k->s[i]=='-'))i++;
        while(i<k->n&&k->s[i]>='0'&&k->s[i]<='9'){i++;ed++;}if(!ed)i=e;
    }
    if(i-start>=64)return 0;char text[64];memcpy(text,k->s+start,i-start);text[i-start]=0;
    double v=strtod(text,0);if(!isfinite(v)||fabs(v)>1e7)return 0;*out=v;k->i=i;return 1;
}
static int bg_length(struct bg_scan *k,double font,double root,struct bg_length *out,int automatic)
{
    bg_space(k);struct bg_length v={0};
    if(automatic&&bg_word(k,"auto")){v.automatic=1;*out=v;return 1;}
    double number;if(!bg_number(k,&number))return 0;
    if(k->i<k->n&&k->s[k->i]=='%'){k->i++;v.percent=number/100;}
    else if(bg_word(k,"px"))v.px=number;
    else if(bg_word(k,"rem"))v.px=number*root;
    else if(bg_word(k,"em"))v.px=number*font;
    else if(bg_word(k,"vw"))v.px=number*css_media_width()/100;
    else if(bg_word(k,"vh"))v.px=number*css_media_height()/100;
    else if(bg_word(k,"pt"))v.px=number*4/3;
    else if(bg_word(k,"pc"))v.px=number*16;
    else if(bg_word(k,"in"))v.px=number*96;
    else if(bg_word(k,"cm"))v.px=number*96/2.54;
    else if(bg_word(k,"mm"))v.px=number*96/25.4;
    else if(bg_word(k,"q"))v.px=number*96/101.6;
    else if(number!=0)return 0;
    *out=v;return 1;
}
static int bg_size(const char *value,int len,double font,double root,struct bg_geometry *g)
{
    struct bg_scan k={value,len,0};bg_space(&k);
    if(bg_word(&k,"cover"))g->mode=BG_COVER;
    else if(bg_word(&k,"contain"))g->mode=BG_CONTAIN;
    else{
        g->mode=BG_EXPLICIT;
        if(!bg_length(&k,font,root,&g->size[0],1))return 0;
        bg_space(&k);g->size[1]=(struct bg_length){0,0,1};
        if(k.i<k.n&&!bg_length(&k,font,root,&g->size[1],1))return 0;
        for(int i=0;i<2;i++)if(g->size[i].px<0||g->size[i].percent<0)return 0;
    }
    bg_space(&k);return k.i==k.n;
}
static int bg_position(const char *value,int len,double font,double root,struct bg_geometry *g)
{
    struct bg_scan k={value,len,0};
    int axis[4]={-1,-1,-1,-1},center[4]={0},count=0;
    struct bg_length parts[4];
    while(count<4){
        bg_space(&k);if(k.i==k.n)break;
        struct bg_length v={0};int a=-1;
        if(bg_word(&k,"left")){a=0;v.percent=0;}
        else if(bg_word(&k,"right")){a=0;v.percent=1;}
        else if(bg_word(&k,"top")){a=1;v.percent=0;}
        else if(bg_word(&k,"bottom")){a=1;v.percent=1;}
        else if(bg_word(&k,"center")){v.percent=.5;center[count]=1;}
        else if(!bg_length(&k,font,root,&v,0))return 0;
        axis[count]=a;parts[count++]=v;
    }
    bg_space(&k);if(!count||k.i!=k.n)return 0;
    g->position[0]=(struct bg_length){0,.5,0};g->position[1]=g->position[0];
    if(count==1){g->position[axis[0]==1?1:0]=parts[0];return 1;}
    if(count==2){
        if(axis[0]>=0&&axis[0]==axis[1])return 0;
        if(axis[0]==1){if(axis[1]<0&&!center[1])return 0;g->position[1]=parts[0];g->position[0]=parts[1];}
        else if(axis[1]==0){if(axis[0]<0&&!center[0])return 0;g->position[1]=parts[0];g->position[0]=parts[1];}
        else{g->position[0]=parts[0];g->position[1]=parts[1];}
        return 1;
    }
    /* In three/four-component positions, lengths are EDGE OFFSETS, not a
     * second ordinary x/y pair. A right/bottom offset is subtracted from
     * the remaining space, including its percentage term. */
    int assigned=0,explicit_axes=0;
    for(int i=0;i<count;i++)if(axis[i]>=0)explicit_axes|=1<<axis[i];
    for(int i=0;i<count;i++){
        int a=axis[i];
        if(center[i]){
#ifdef CSS_BACKGROUND_CENTER_ORDER
            a=(assigned&1)?1:0;
#else
            /* The original rule assigned a leading center to x. Correction:
             * Backgrounds 3's unordered pair lets `center right 10px` put
             * center on y. The explicit edge determines the other axis;
             * choosing by token order silently rejected that valid position.
             * Both explicit axes plus center would be a third axis group,
             * and must still refuse the whole value. */
            if(explicit_axes==1)a=1;
            else if(explicit_axes==2)a=0;
            else return 0;
#endif
        }
        if(a<0 || (assigned&(1<<a)))return 0;
        struct bg_length v=parts[i];
        if(!center[i]&&i+1<count&&axis[i+1]<0&&!center[i+1]){
            struct bg_length offset=parts[++i];
            if(v.percent==1){v.px=-offset.px;v.percent=1-offset.percent;}
            else v=offset;
        }
        g->position[a]=v;assigned|=1<<a;
    }
    return assigned==3;
}
static int bg_repeat(const char *value,int len,struct bg_geometry *g)
{
    struct bg_scan k={value,len,0};bg_space(&k);
    if(bg_word(&k,"repeat-x")){g->repeat[0]=1;g->repeat[1]=0;}
    else if(bg_word(&k,"repeat-y")){g->repeat[0]=0;g->repeat[1]=1;}
    else{
        int count=0;
        while(count<2){bg_space(&k);if(k.i==k.n)break;
            if(bg_word(&k,"no-repeat"))g->repeat[count++]=BG_NO_REPEAT;
            else if(bg_word(&k,"repeat"))g->repeat[count++]=BG_REPEAT;
            else if(bg_word(&k,"space"))g->repeat[count++]=BG_SPACE;
            else if(bg_word(&k,"round"))g->repeat[count++]=BG_ROUND;
            else return 0;
        }
        if(!count)return 0;if(count==1)g->repeat[1]=g->repeat[0];
    }
    bg_space(&k);return k.i==k.n;
}
static int bg_equal(const char *text,int len,const char *word)
{
    struct bg_scan k={text,len,0};bg_space(&k);if(!bg_word(&k,word))return 0;bg_space(&k);return k.i==k.n;
}
static const char *const bg_names[BG_SLOTS]={
    "background-image","background-size","background-position","background-repeat",
    "background-origin","background-clip","background-attachment","background-color"
};
static const char *const bg_initial[BG_SLOTS]={
    "none","auto","0% 0%","repeat","padding-box","border-box","scroll","transparent"
};
static struct bg_span bg_span(const char *value,int len)
{
    while(len&&bg_whitespace(*value)){value++;len--;}
    while(len&&bg_whitespace(value[len-1]))len--;
    return (struct bg_span){value,len};
}
/* Split only at top-level commas. A quoted data URL and a gradient's own
 * color commas cannot become background layers. Unbalanced input and excess
 * layers refuse the WHOLE declaration; no prefix survives either failure. */
static int bg_list(const char *value,int len,struct bg_span out[BG_MAX_LAYERS])
{
    int depth=0,start=0,count=0;char quote=0;
    for(int i=0;i<=len;i++){
        char c=i<len?value[i]:',';
        if(quote){if(c=='\\'&&i+1<len)i++;else if(c==quote)quote=0;continue;}
        if(c=='\\'){if(i+1>=len)return 0;i++;continue;}
        if(c=='\''||c=='"'){quote=c;continue;}
        if(c=='/'&&i+1<len&&value[i+1]=='*'){
            i+=2;while(i+1<len&&!(value[i]=='*'&&value[i+1]=='/'))i++;
            if(i+1>=len)return 0;i++;continue;
        }
        if(c=='('){if(++depth>128)return 0;}
        else if(c==')'){if(--depth<0)return 0;}
        else if(c==','&&!depth){
            if(count==BG_MAX_LAYERS)return 0;
            struct bg_span part=bg_span(value+start,i-start);
            if(!part.len)return 0;out[count++]=part;start=i+1;
        }
    }
    return depth||quote?0:count;
}
static void bg_default(struct bg_layer *layer)
{
    memset(layer,0,sizeof *layer);
    layer->image=(struct bg_span){bg_initial[BG_IMAGE],4};
    layer->geometry.size[0].automatic=layer->geometry.size[1].automatic=1;
    layer->geometry.repeat[0]=layer->geometry.repeat[1]=BG_REPEAT;
    layer->origin=BG_PADDING_BOX;layer->clip=BG_BORDER_BOX;
}
static int bg_box(struct bg_span v,int *out)
{
    if(bg_equal(v.value,v.len,"border-box"))*out=BG_BORDER_BOX;
    else if(bg_equal(v.value,v.len,"padding-box"))*out=BG_PADDING_BOX;
    else if(bg_equal(v.value,v.len,"content-box"))*out=BG_CONTENT_BOX;
    else return 0;
    return 1;
}
static int bg_color_valid(struct bg_span v)
{
    /* CSS-wide keywords are whole declarations, never a color component of
     * `background: url(...) inherit`. Native color validation accepts them
     * in isolation, so calling it alone would affirm that invalid shorthand. */
    static const char *const wide[]={"inherit","initial","unset","revert","revert-layer"};
    for(unsigned i=0;i<sizeof wide/sizeof *wide;i++)if(bg_equal(v.value,v.len,wide[i]))return 0;
    return css_native_supports_decl("color",5,v.value,v.len);
}
static int bg_attachment(struct bg_span value)
{
    if(bg_equal(value.value,value.len,"scroll"))return 0;
    if(bg_equal(value.value,value.len,"fixed"))return 1;
    if(bg_equal(value.value,value.len,"local"))return 2;
    return -1;
}
/* The native grammar knows fixed/local even though the painting road does
 * not. Its declaration parser must reject the SAME unsupported attachment,
 * including the shorthand's color; rejecting only the extension patch would
 * leave a red native background from an otherwise discarded declaration. */
int logit_css_background_attachment(const char *value,int len)
{
#ifdef CSS_BACKGROUND_NATIVE_ATTACHMENT
    (void)value;(void)len;return 1;
#else
    return bg_attachment((struct bg_span){value,len})<=0;
#endif
}
static int bg_image(struct bg_span v,struct bg_layer *layer)
{
    char url[4096];struct cgradient gradient;
    if(bg_equal(v.value,v.len,"none"))layer->kind=BG_NONE;
    else if(css_background_url(v.value,v.len,url,sizeof url))layer->kind=BG_URL;
    else if(css_gradient_parse(v.value,v.len,16,16,&gradient))layer->kind=BG_GRADIENT;
    else return 0;
    layer->image=v;return 1;
}
/* One shorthand token, including a whole function/string. Slash and comma
 * remain separate grammar delimiters; consuming them with a URL prefix is
 * exactly how a valid image could survive an invalid shorthand tail. */
static int bg_token(struct bg_scan *k,struct bg_span *out)
{
    bg_space(k);int start=k->i,depth=0;char quote=0;
    for(;k->i<k->n;k->i++){
        char c=k->s[k->i];
        if(quote){if(c=='\\'&&k->i+1<k->n)k->i++;else if(c==quote)quote=0;continue;}
        if(c=='\\'){if(k->i+1==k->n)return 0;k->i++;continue;}
        if(c=='\''||c=='"'){quote=c;continue;}
        if(!depth&&(bg_whitespace(c)||c=='/'||c==','))break;
        if(c=='('){if(++depth>128)return 0;}
        else if(c==')'){if(--depth<0)return 0;}
    }
    if(depth||quote||k->i==start)return 0;
    *out=(struct bg_span){k->s+start,k->i-start};return 1;
}
static int bg_shorthand(const char *value,int len,double font,double root,struct bg_layers *out)
{
    struct bg_span list[BG_MAX_LAYERS];int count=bg_list(value,len,list);
    if(!count)return 0;memset(out,0,sizeof *out);out->count=count;
    out->color=(struct bg_span){bg_initial[BG_COLOR],11};
    for(int i=0;i<count;i++){
        struct bg_layer *layer=&out->layer[i];bg_default(layer);
        struct bg_scan k={list[i].value,list[i].len,0};
        int image=0,position=0,size=0,repeat=0,boxes=0,color=0,attachment=0;
        while(k.i<k.n){
            struct bg_span token;bg_space(&k);if(k.i==k.n)break;
            if(!bg_token(&k,&token))return 0;
            struct bg_layer candidate=*layer;int box;
            if(bg_image(token,&candidate)){
                if(image++)return 0;layer->image=candidate.image;layer->kind=candidate.kind;continue;
            }
            if(bg_box(token,&box)){
                if(boxes==0)layer->origin=layer->clip=box;
                else if(boxes==1)layer->clip=box;
                else return 0;boxes++;continue;
            }
            int attached=bg_attachment(token);
            if(attached==0){if(attachment++)return 0;continue;}
            if(attached>0)return 0;
            struct bg_geometry geometry=layer->geometry;
            if(bg_repeat(token.value,token.len,&geometry)){
                if(repeat++)return 0;
                struct bg_scan next=k;struct bg_span second;
                if(!bg_equal(token.value,token.len,"repeat-x")&&!bg_equal(token.value,token.len,"repeat-y")&&
                   bg_token(&next,&second)&&bg_repeat(second.value,second.len,&candidate.geometry)&&
                   !bg_equal(second.value,second.len,"repeat-x")&&!bg_equal(second.value,second.len,"repeat-y")){
                    geometry.repeat[1]=candidate.geometry.repeat[0];k=next;
                }
                memcpy(layer->geometry.repeat,geometry.repeat,sizeof geometry.repeat);continue;
            }
            if(bg_position(token.value,token.len,font,root,&geometry)){
                if(position++)return 0;
                int start=(int)(token.value-k.s),end=start+token.len;
                struct bg_scan accepted=k,scan=k;
                for(int n=1;n<4;n++){
                    struct bg_span second;
                    if(!bg_token(&scan,&second))break;
                    struct bg_geometry trial=geometry;
                    int last=(int)(second.value-k.s)+second.len;
                    struct bg_geometry atom={0};
                    if(!bg_position(second.value,second.len,font,root,&atom))break;
                    /* `bottom 10px` is not a two-component position, but it
                     * is the prefix of `bottom 10px right 20px`. Scan the
                     * whole position group before choosing its valid extent. */
                    if(bg_position(k.s+start,last-start,font,root,&trial)){
                        geometry=trial;end=last;accepted=scan;
                    }
                }
                k=accepted;
                if(!bg_position(k.s+start,end-start,font,root,&geometry))return 0;
                memcpy(layer->geometry.position,geometry.position,sizeof geometry.position);
                bg_space(&k);
                if(k.i<k.n&&k.s[k.i]=='/'){
                    if(size++)return 0;k.i++;struct bg_span first;
                    if(!bg_token(&k,&first)||!bg_size(first.value,first.len,font,root,&geometry))return 0;
                    struct bg_scan next=k;struct bg_span second;
                    struct bg_geometry trial=geometry;
                    if(bg_token(&next,&second)&&bg_size(first.value,(int)(second.value-first.value)+second.len,font,root,&trial)){geometry=trial;k=next;}
                    layer->geometry.mode=geometry.mode;
                    memcpy(layer->geometry.size,geometry.size,sizeof geometry.size);
                }
                continue;
            }
            if(i==count-1&&!color&&bg_color_valid(token)){out->color=token;color=1;continue;}
            return 0;
        }
    }
    return 1;
}
static int bg_component(int slot,struct bg_span value,double font,double root,struct bg_layer *layer)
{
    switch(slot){
    case BG_IMAGE:return bg_image(value,layer);
    case BG_SIZE:return bg_size(value.value,value.len,font,root,&layer->geometry);
    case BG_POSITION:return bg_position(value.value,value.len,font,root,&layer->geometry);
    case BG_REPEAT_STYLE:return bg_repeat(value.value,value.len,&layer->geometry);
    case BG_ORIGIN:return bg_box(value,&layer->origin);
    case BG_CLIP:return bg_box(value,&layer->clip);
    case BG_ATTACHMENT:return bg_attachment(value)==0;
    case BG_COLOR:return bg_color_valid(value);
    }
    return 0;
}
int css_background_xr(int slot)
{
    static const int slots[BG_SLOTS]={XR_BG_IMAGE,XR_BG_SIZE,XR_BG_POSITION,XR_BG_REPEAT,
        XR_BG_ORIGIN,XR_BG_CLIP,XR_BG_ATTACHMENT,XR_BG_COLOR};
    return slot>=0&&slot<BG_SLOTS?slots[slot]:-1;
}
int css_background_slot(int xr)
{
    for(int i=0;i<BG_SLOTS;i++)if(xr==css_background_xr(i))return i;
    return -1;
}
int css_background_supports(const char *property,int plen,const char *value,int len)
{
    if(!property||!value||plen<=0||len<=0)return 0;
    int wide=bg_equal(value,len,"initial")||bg_equal(value,len,"unset")||bg_equal(value,len,"inherit");
    if(bg_equal(property,plen,"background")){struct bg_layers layers;return wide||bg_shorthand(value,len,16,16,&layers);}
    for(int slot=0;slot<BG_SLOTS;slot++)if(bg_equal(property,plen,bg_names[slot])){
        if(wide)return 1;
        struct bg_span list[BG_MAX_LAYERS];int count=bg_list(value,len,list);
        if(!count||(slot==BG_COLOR&&count!=1))return 0;
        for(int i=0;i<count;i++){struct bg_layer layer;bg_default(&layer);if(!bg_component(slot,list[i],16,16,&layer))return 0;}
        return 1;
    }
    return 0;
}
int css_background_declaration(const char *property,int plen,const char *value,int len,struct bg_declaration *out)
{
    if(!out||!property||!value||plen<=0||len<=0)return 0;memset(out,0,sizeof *out);
    int wide=bg_equal(value,len,"initial")||bg_equal(value,len,"unset");
    int inherited=bg_equal(value,len,"inherit");
#ifndef CSS_BACKGROUND_RAW_INHERIT
    /* Originally every accepted inherit spelling retained its source span.
     * Correction: bg_equal ignores CSS comments and ASCII case, while the
     * extension's parent-first handoff compares the complete keyword. Keeping
     * a comment after inherit therefore accepted syntax but lost the image and
     * computed font basis. Publish ONE static canonical token for both doors;
     * ordinary values still borrow the original complete declaration. */
    if(inherited){value="inherit";len=7;}
#endif
    for(int slot=0;slot<BG_SLOTS;slot++)if(bg_equal(property,plen,bg_names[slot])){
        if(!wide&&!inherited&&!css_background_supports(property,plen,value,len))return 0;
        out->value[slot]=wide?bg_initial[slot]:value;
        out->len[slot]=wide?(int)strlen(bg_initial[slot]):len;return 1;
    }
    if(!bg_equal(property,plen,"background"))return 0;
    struct bg_layers parsed;
    if(!wide&&!inherited&&!bg_shorthand(value,len,16,16,&parsed))return 0;
    for(int slot=0;slot<BG_SLOTS;slot++){
        out->value[slot]=wide?bg_initial[slot]:value;
        out->len[slot]=wide?(int)strlen(bg_initial[slot]):len;
    }
    if(!wide&&!inherited)out->shorthand_bits=(1u<<BG_SLOTS)-1;
    return 1;
}
int css_background_layers(const struct cstyle *st,struct bg_layers *out)
{
    if(!st||!out)return 0;memset(out,0,sizeof *out);out->count=1;bg_default(&out->layer[0]);
    int font=st->font_px>0?st->font_px:16,root=css_root_px();
    struct bg_layers parsed;
    const char *cached=0;int cached_len=0,cached_basis=0;
    for(int slot=0;slot<BG_SLOTS;slot++){
        int xr=css_background_xr(slot);
        const char *value=st->xraw[xr];int len=st->xrawlen[xr];
        if(!value)continue;
        int basis=slot==BG_SIZE?st->background_font[0]:slot==BG_POSITION?st->background_font[1]:font;
        if(basis<=0)basis=font;
        struct bg_span list[BG_MAX_LAYERS];int count;
        int shorthand=(st->background_shorthand>>slot)&1;
        if(shorthand){
            if(value!=cached||len!=cached_len||basis!=cached_basis){
                if(!bg_shorthand(value,len,basis,root,&parsed))return 0;
                cached=value;cached_len=len;cached_basis=basis;
            }
            count=parsed.count;
        }
        else {count=bg_list(value,len,list);if(!count)return 0;}
        if(slot==BG_IMAGE){
            out->count=count;
            for(int i=0;i<count;i++){
                bg_default(&out->layer[i]);
                if(shorthand){out->layer[i].image=parsed.layer[i].image;out->layer[i].kind=parsed.layer[i].kind;}
                else if(!bg_image(list[i],&out->layer[i]))return 0;
            }
            continue;
        }
        if(slot==BG_COLOR){
            if(shorthand)out->color=parsed.color;
            else {if(count!=1||!bg_color_valid(list[0]))return 0;out->color=list[0];}
            continue;
        }
        for(int i=0;i<out->count;i++){
            int j=i%count;
            if(!shorthand){if(!bg_component(slot,list[j],basis,root,&out->layer[i]))return 0;continue;}
            struct bg_layer *a=&out->layer[i],*b=&parsed.layer[j];
            if(slot==BG_SIZE){a->geometry.mode=b->geometry.mode;memcpy(a->geometry.size,b->geometry.size,sizeof a->geometry.size);}
            else if(slot==BG_POSITION)memcpy(a->geometry.position,b->geometry.position,sizeof a->geometry.position);
            else if(slot==BG_REPEAT_STYLE)memcpy(a->geometry.repeat,b->geometry.repeat,sizeof a->geometry.repeat);
            else if(slot==BG_ORIGIN)a->origin=b->origin;
            else if(slot==BG_CLIP)a->clip=b->clip;
            else if(slot==BG_ATTACHMENT)a->attachment=b->attachment;
        }
    }
#ifdef CSS_BACKGROUND_FIRST_LAYER
    /* Watched negative control: accepting an image list then rendering and
     * loading its first item used to look successful while losing the rest. */
    out->count=1;
#endif
    return 1;
}
int css_background_geometry(const struct cstyle *st,struct bg_geometry *geometry)
{
    struct bg_layers layers;if(!geometry||!css_background_layers(st,&layers))return 0;
    *geometry=layers.layer[0].geometry;return 1;
}
int css_background_color(const struct cstyle *st,unsigned *rgb,int *alpha)
{
    struct bg_layers layers;
    if(!st||!rgb||!alpha||!css_background_layers(st,&layers)||!layers.color.value)return 0;
    if(bg_equal(layers.color.value,layers.color.len,"currentcolor")){*rgb=st->color;*alpha=st->color_alpha;return 1;}
    if(!LOGIT_HAVE(img_css_color))return 0;
    unsigned char rgba[4];if(!img_css_color(layers.color.value,layers.color.len,rgba))return 0;
    *rgb=((unsigned)rgba[0]<<16)|((unsigned)rgba[1]<<8)|rgba[2];*alpha=rgba[3];return 1;
}
