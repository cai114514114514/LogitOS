/* Reuse the actual CSSOM/layout apparatus, including text measurement and
 * embedder recascade, rather than inventing a node-box stub for percentages. */
#define main cssom_original_main
#include "cssom_test.c"
#undef main
static void matrix_check(const char *label,const char *expr)
{
    char *s=evalstr(expr);checks++;
    if(s&&!strcmp(s,"true"))printf("  ok   %s\n",label);
    else {printf("  FAIL %s\n",label);fails++;}free(s);
}
static void matrix_reflow(void)
{
    css_apply(g_root,g_sheet,g_sheetlen);
    css_extra_apply(g_root,g_sheet,g_sheetlen);
    layout_page(g_root,800);
}
int main(void)
{
    const char *html="<html><body><div id='target'></div><div id='hidden'></div></body></html>";
    g_root=dom_parse(html,strlen(html));css_init();css_viewport(800,600);
    strcpy(g_sheet,"html{font-size:20px}#target{width:200px;height:80px;transform:translate(25%,50%) scale(2,3)}#hidden{display:none;transform:translate(25%,2em)}");g_sheetlen=strlen(g_sheet);
    matrix_reflow();js_page_set_clock(clk);if(!js_page_open(g_root))return 2;ctx=js_page_ctx();js_cssom_set_reflow(matrix_reflow);
    matrix_check("matrix installer is present","typeof DOMMatrix==='function'&&typeof DOMMatrixReadOnly==='function'");
    matrix_check("identity forms","[undefined,'','none'].every(x=>{let m=new DOMMatrix(x);return m.is2D&&m.isIdentity&&m.m33===1&&m.m44===1})");
    matrix_check("six coefficients and aliases","(()=>{let m=new DOMMatrix([2,3,4,5,40,50]);return m.m11===2&&m.m12===3&&m.m21===4&&m.m22===5&&m.m41===40&&m.m42===50&&m.a===2&&m.b===3&&m.c===4&&m.d===5&&m.e===40&&m.f===50&&m.m33===1&&m.is2D&&!m.isIdentity})()");
    matrix_check("sixteen column major coefficients","(()=>{let a=Array.from({length:16},(_,i)=>i+1),m=new DOMMatrix(a);for(let c=1;c<5;c++)for(let r=1;r<5;r++)if(m['m'+c+r]!==a[(c-1)*4+r-1])return false;return !m.is2D})()");
    matrix_check("three dimensional identity keeps its dimensional flag","(()=>{let m=new DOMMatrix('translateZ(0px)');return !m.is2D&&m.isIdentity&&String(m).startsWith('matrix3d(')})()");
    matrix_check("three dimensional perspective coefficients","(()=>{let m=new DOMMatrix('perspective(100px) translateZ(20px)');return !m.is2D&&m.m43===20&&Math.abs(m.m34+.01)<1e-9&&Math.abs(m.m44-.8)<1e-9})()");
    matrix_check("rotation and absolute length math","(()=>{let m=new DOMMatrix('translateX(1in) rotate(90deg)');return Math.abs(m.m11)<1e-9&&Math.abs(m.m12-1)<1e-9&&Math.abs(m.m21+1)<1e-9&&m.m41===96})()");
    matrix_check("mutable aliases and sticky dimensional flag","(()=>{let m=new DOMMatrix();m.e=9;m.m43=2;m.m43=0;return m.m41===9&&!m.is2D&&!m.isIdentity})()");
    matrix_check("readonly fields and branded access","(()=>{let m=new DOMMatrixReadOnly();m.e=5;let caught=false;try{Object.getOwnPropertyDescriptor(DOMMatrixReadOnly.prototype,'m11').get.call({})}catch(e){caught=e instanceof TypeError}return m.e===0&&caught&&new DOMMatrix() instanceof DOMMatrixReadOnly})()");
    matrix_check("invalid transform syntax rejected","['bogus','matrix(1,2)','translateX(10%)','translateX(0em)','translateX(2rem)',' '].every(s=>{try{new DOMMatrix(s);return false}catch(e){return e.name==='SyntaxError'}})");
    matrix_check("invalid sequence length and bigint rejected","[[],[1,2], [1n,0,0,1,0,0]].every(s=>{try{new DOMMatrix(s);return false}catch(e){return e instanceof TypeError}})");
    matrix_check("iterable conversion and nonfinite values","(()=>{let m=new DOMMatrix(new Set([2,3,4,5,6,7])),n=new DOMMatrix([NaN,0,0,1,0,0]);return m.a===2&&m.f===7&&Number.isNaN(n.a)&&!n.isIdentity})()");
    matrix_check("computed transform consumer resolves actual box","(()=>{let m=new DOMMatrix(getComputedStyle(document.getElementById('target')).getPropertyValue('transform'));return m.m11===2&&m.m22===3&&m.m41===50&&m.m42===40})()");
    matrix_check("computed transform tracks style mutation and reflow","(()=>{let e=document.getElementById('target');e.style.width='320px';let m=new DOMMatrix(getComputedStyle(e).transform);return m.m41===80&&m.m42===40})()");
    matrix_check("hidden computed transform preserves percentages","(()=>{let s=getComputedStyle(document.getElementById('hidden')).transform;return s.indexOf('25%')>=0&&s.indexOf('40px')>=0})()");
    matrix_check("untransformed computed value is none","getComputedStyle(document.body).transform==='none'");
    js_page_close();dom_free(g_root);css_init();printf("dom-matrix: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
