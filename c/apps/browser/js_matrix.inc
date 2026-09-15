/* DOMMatrix's value storage and computed transform share css_interp's 4x4
 * authority. Apple gallery's real consumer reads all sixteen fields, including
 * m41/m42/m43; a six-field shim or an empty computed string silently initializes
 * every tween with the wrong transform. No site/library detection is involved.
 *
 * This bounded surface implements construction, fields, flags and serialization.
 * Arithmetic/fromMatrix/point methods deliberately remain absent until their
 * full semantics have gates. It does not pretend a no-op is a matrix operation.
 * Optional host consumers without css_interp retain the absent constructors. */
#include "css_interp.h"
#include "css_transform_context.h"
int ci_transform_parse(const char *,int,double,double,struct ci_xform *) LOGIT_WEAK;
void ci_transform_matrix(const struct ci_xform *,double,double,double *) LOGIT_WEAK;
int ci_matrix_text(const double *,char *,int) LOGIT_WEAK;
int ci_transform_text(const struct ci_xform *,char *,int) LOGIT_WEAK;
LOGIT_WEAK_STUB(ci_transform_parse);
LOGIT_WEAK_STUB(ci_transform_matrix);
LOGIT_WEAK_STUB(ci_matrix_text);
LOGIT_WEAK_STUB(ci_transform_text);
static int matrix_math_available(void)
{
    return LOGIT_HAVE(ci_transform_parse) && LOGIT_HAVE(ci_transform_matrix) &&
           LOGIT_HAVE(ci_matrix_text) && LOGIT_HAVE(ci_transform_text);
}

/* The raw winning declaration also holds animation's current value. Resolve
 * percentages only when layout supplies the element's OWN reference box, never
 * an ink union or viewport guess. Hidden/boxless values retain their computed
 * function list; DOMMatrix then correctly refuses unresolved percentages. */
int js_cssom_computed_transform(struct node *n, char *out, int cap)
{
    if (!matrix_math_available() || !LOGIT_HAVE(ci_transform_parse_context)) return -1;
    flush_layout();
    const struct cstyle *s = n ? n->style : 0;
    if (!s || !s->xraw[XR_TRANSFORM]) { if(cap<5)return -1;memcpy(out,"none",5);return 4; }
    struct ci_xform xf;
    if (css_node_transform_parse(n,s->xraw[XR_TRANSFORM],
                                  s->xrawlen[XR_TRANSFORM],&xf)) return -1;
    if (!xf.n) { if(cap<5)return -1;memcpy(out,"none",5);return 4; }
    int x,y,w,h;
    if (s->display != DISP_NONE && s->display != DISP_CONTENTS &&
        LOGIT_HAVE(layout_node_box) && layout_node_box(n,&x,&y,&w,&h)) {
        double m[16];ci_transform_matrix(&xf,w,h,m);
        return ci_matrix_text(m,out,cap);
    }
    return ci_transform_text(&xf,out,cap);
}

static int matrix_3d_function(int kind)
{
    return kind==CI_TRANSLATEZ || kind==CI_TRANSLATE3D || kind==CI_SCALEZ ||
        kind==CI_SCALE3D || kind==CI_ROTATEX || kind==CI_ROTATEY ||
        kind==CI_ROTATEZ || kind==CI_ROTATE3D || kind==CI_MATRIX3D || kind==CI_PERSPECTIVE;
}
static JSValue matrix_parse(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    size_t len;const char *s=argc?JS_ToCStringLen(ctx,&len,argv[0]):0;
    if(!s)return JS_EXCEPTION;
    int valid=1, nonspace=0;
    /* DOMMatrix has no element/font reference: even 0em is invalid, whereas
     * CSS computed transform may resolve em using its element's actual font.
     * Scan identifiers, not substrings (perspective contains letters 'em' in
     * neither a unit nor a length); css_interp still owns grammar and numbers. */
    for(size_t i=0;i<len;) {
        unsigned char c=s[i];if(c!=' '&&c!='\t'&&c!='\r'&&c!='\n'&&c!='\f')nonspace=1;
        if(c=='%')valid=0;
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')) {
            char word[32];int j=0;
            while(i<len&&((s[i]>='a'&&s[i]<='z')||(s[i]>='A'&&s[i]<='Z'))) {
                if(j<31)word[j++]=(char)(s[i]|32);i++;
            }
            word[j]=0;
            if(!strcmp(word,"em")||!strcmp(word,"rem")||!strcmp(word,"ex")||!strcmp(word,"ch"))valid=0;
        } else i++;
    }
    struct ci_xform xf;
    if(len&&!nonspace)valid=0;
    if(valid && ci_transform_parse(s,(int)len,0,0,&xf))valid=0;
    JS_FreeCString(ctx,s);
    if(!valid)return JS_NULL; /* JS wrapper constructs the specified DOMException. */
    double m[16];ci_transform_matrix(&xf,0,0,m);int two=1;
    for(int i=0;i<xf.n;i++)if(matrix_3d_function(xf.f[i].kind))two=0;
    JSValue a=JS_NewArray(ctx);
    for(int i=0;i<16;i++)JS_SetPropertyUint32(ctx,a,i,JS_NewFloat64(ctx,m[i]));
    JS_SetPropertyUint32(ctx,a,16,JS_NewBool(ctx,two));return a;
}
static JSValue matrix_serialize(JSContext *ctx, JSValueConst t,int argc,JSValueConst *argv)
{
    (void)t;if(!argc)return JS_EXCEPTION;
    double m[16];
    for(int i=0;i<16;i++){JSValue v=JS_GetPropertyUint32(ctx,argv[0],i);int rc=JS_ToFloat64(ctx,&m[i],v);JS_FreeValue(ctx,v);if(rc)return JS_EXCEPTION;}
    char out[2048];int n;
    if(argc>1&&JS_ToBool(ctx,argv[1]))n=ci_matrix_text(m,out,sizeof out);
    else {struct ci_xform xf;memset(&xf,0,sizeof xf);xf.n=1;xf.f[0].kind=CI_MATRIX3D;xf.f[0].nargs=16;memcpy(xf.f[0].a,m,sizeof m);n=ci_transform_text(&xf,out,sizeof out);}
    return JS_NewStringLen(ctx,out,n);
}
static const char MATRIX_JS[] =
"(function(parse,serialize){\n"
" const slots=new WeakMap(),idx=[0,1,4,5,12,13],names=[];\n"
" const identity=()=>[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1];\n"
" function get(o){const s=slots.get(o);if(!s)throw new TypeError('Illegal invocation');return s}\n"
" function syntax(){if(typeof DOMException==='function')throw new DOMException('Invalid transform','SyntaxError');throw new SyntaxError('Invalid transform')}\n"
" class DOMMatrixReadOnly {\n"
"  constructor(init){let m,two=true;if(init===undefined)m=identity();\n"
"   else if(init!==null && (typeof init==='object'||typeof init==='function') && init[Symbol.iterator]!==undefined){\n"
"    const a=Array.from(init,x=>+x);if(a.length!==6&&a.length!==16)throw new TypeError('Expected 6 or 16 values');\n"
"    if(a.length===16){m=a;two=false}else{m=identity();idx.forEach((j,i)=>m[j]=a[i])}\n"
"   }else{const a=parse(String(init));if(a===null)syntax();m=a.slice(0,16);two=a[16]}\n"
"   slots.set(this,{m,two});}\n"
"  get is2D(){return get(this).two}\n"
"  get isIdentity(){return get(this).m.every((v,i)=>v===(i%5===0?1:0))}\n"
"  toString(){const s=get(this);if(s.m.some(v=>!Number.isFinite(v)))throw new (typeof DOMException==='function'?DOMException:Error)('Non-finite matrix','InvalidStateError');return serialize(s.m,s.two)}\n"
" }\n"
" class DOMMatrix extends DOMMatrixReadOnly {}\n"
" for(let c=1;c<=4;c++)for(let r=1;r<=4;r++)names.push('m'+c+r);\n"
" function field(name,i){Object.defineProperty(DOMMatrixReadOnly.prototype,name,{configurable:true,enumerable:true,get(){return get(this).m[i]}});\n"
"  Object.defineProperty(DOMMatrix.prototype,name,{configurable:true,enumerable:true,get(){return get(this).m[i]},set(v){const s=get(this);v=+v;s.m[i]=v;if(idx.indexOf(i)<0&&v!==(i%5===0?1:0))s.two=false}})}\n"
" names.forEach(field);'abcdef'.split('').forEach((n,i)=>field(n,idx[i]));\n"
" Object.defineProperty(DOMMatrixReadOnly.prototype,Symbol.toStringTag,{value:'DOMMatrixReadOnly',configurable:true});\n"
" Object.defineProperty(DOMMatrix.prototype,Symbol.toStringTag,{value:'DOMMatrix',configurable:true});\n"
" globalThis.DOMMatrixReadOnly=DOMMatrixReadOnly;globalThis.DOMMatrix=DOMMatrix;\n"
"})(globalThis.__matrixParse,globalThis.__matrixSerialize);\n"
"delete globalThis.__matrixParse;delete globalThis.__matrixSerialize;\n";
static void matrix_install(JSContext *ctx,JSValue g)
{
#ifndef DOM_MATRIX_NO_INSTALL
    if(!matrix_math_available())return;
    JS_SetPropertyStr(ctx,g,"__matrixParse",JS_NewCFunction(ctx,matrix_parse,"",1));
    JS_SetPropertyStr(ctx,g,"__matrixSerialize",JS_NewCFunction(ctx,matrix_serialize,"",2));
    run_shim(ctx,MATRIX_JS,"<DOMMatrix>");
#else
    (void)ctx;(void)g;
#endif
}
