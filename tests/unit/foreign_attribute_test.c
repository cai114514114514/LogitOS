/* Real parser and JS bindings, including the length-carrying setter. A raw
 * SVG renderer can hide these DOM errors, so the oracle queries the DOM too. */
#define main dom_id_original_main
#include "dom_id_test.c"
#undef main
#include <libwapcaplet/libwapcaplet.h>
#define CCHK(v,label) do {checks++;if(!(v)){failures++;printf("FAIL %s\n",label);}} while(0)
static int same(const char *a,const char *b){return a&&b&&!strcmp(a,b);}
int main(void){
 const char *html="<!doctype html><body><svg id=s viewBox='0 0 24 12'><clipPath id=c clipPathUnits='objectBoundingBox'></clipPath></svg><math id=m definitionURL='math-ref'></math><div id=h WIDTH='24'></div></body>";
 struct node *root=dom_parse(html,strlen(html));if(!root)return 2;
 struct node *s=dom_get_element_by_id(root->doc,"s"),*c=dom_get_element_by_id(root->doc,"c"),*m=dom_get_element_by_id(root->doc,"m"),*h=dom_get_element_by_id(root->doc,"h");
 CCHK(s&&c&&m&&h,"parser creates foreign fixture nodes");
 CCHK(s->ns==NS_SVG&&m->ns==NS_MATHML,"parser assigns actual foreign namespaces");
 CCHK(same(dom_attr(s,"viewBox"),"0 0 24 12"),"native parsed SVG viewBox is readable");
 CCHK(!dom_attr(s,"viewbox")&&!dom_attr(s,"VIEWBOX"),"native SVG lookup preserves case");
 CCHK(same(dom_attr(c,"clipPathUnits"),"objectBoundingBox"),"native clipPathUnits keeps parser adjusted spelling");
 CCHK(same(dom_attr(m,"definitionURL"),"math-ref")&&!dom_attr(m,"definitionurl"),"native MathML lookup preserves adjusted case");
 int count=s->nattr;dom_set_attr(s,"viewBox","0 0 24 48");
 CCHK(s->nattr==count&&same(dom_attr(s,"viewBox"),"0 0 24 48"),"native SVG update replaces adjusted attribute without duplicate");
 dom_set_attr(s,"viewbox","separate");
 CCHK(s->nattr==count+1&&same(dom_attr(s,"viewbox"),"separate"),"foreign differently cased names remain distinct");
 CCHK(!dom_remove_attr(s,"VIEWBOX")&&same(dom_attr(s,"viewBox"),"0 0 24 48"),"foreign wrong-case removal changes nothing");
 CCHK(dom_remove_attr(s,"viewbox")&&!dom_attr(s,"viewbox")&&dom_attr(s,"viewBox"),"foreign removal affects only exact spelling");
 count=h->nattr;dom_set_attr(h,"WiDtH","48");
 CCHK(h->nattr==count&&same(dom_attr(h,"WIDTH"),"48")&&same(dom_attr(h,"width"),"48"),"HTML mixed-case read and update remain insensitive");
 lwc_string *upper=0,*exact=0;lwc_intern_string("WIDTH",5,&upper);lwc_intern_string("viewBox",7,&exact);
 CCHK(same(dom_attr_lw(h,upper),"48")&&dom_has_attr_lw(h,upper),"interned HTML queries share ASCII folding");
 CCHK(same(dom_attr_lw(s,exact),"0 0 24 48")&&dom_has_attr_lw(s,exact),"interned SVG queries preserve exact spelling");
 lwc_string_unref(upper);lwc_string_unref(exact);
 CCHK(dom_remove_attr(h,"WIDTH")&&!dom_attr(h,"width"),"HTML uppercase removal reaches canonical name");
 char name[101];memset(name,'a',100);name[70]='B';name[100]=0;dom_set_attr(s,name,"long");
 CCHK(same(dom_attr(s,name),"long"),"foreign cold lookup preserves long mixed-case names");
 name[70]='b';CCHK(!dom_attr(s,name)&&!dom_remove_attr(s,name),"foreign long wrong-case name stays absent");
 js_page_set_location("https://attributes.example/");if(!js_page_open(root))return 2;
 js_page_eval("void 0;",7,"<warmup>",0);
 ck("JS parsed SVG attributes use their exact names","var s=document.getElementById('s'),c=document.getElementById('c'),m=document.getElementById('m'),h=document.getElementById('h');s.getAttribute('viewBox')==='0 0 24 48'&&s.getAttribute('viewbox')===null");
 ck("JS SVG setAttribute updates existing viewBox","var n=s.getAttributeNames().length;s.setAttribute('viewBox','0 0 30 60');s.getAttributeNames().length===n&&s.getAttribute('viewBox')==='0 0 30 60'");
 ck("JS SVG hasAttribute is case sensitive","s.hasAttribute('viewBox')&&!s.hasAttribute('viewbox')&&!s.hasAttribute('VIEWBOX')");
 ck("JS getAttributeNode shares exact foreign lookup","s.getAttributeNode('viewBox').name==='viewBox'&&s.getAttributeNode('viewbox')===null");
 ck("JS wrong-case remove leaves original","s.removeAttribute('viewbox');s.hasAttribute('viewBox')");
 ck("JS clipPathUnits update does not append lowercase alias","var n=c.getAttributeNames().length;c.setAttribute('clipPathUnits','userSpaceOnUse');c.getAttributeNames().length===n&&c.getAttribute('clipPathUnits')==='userSpaceOnUse'&&!c.hasAttribute('clippathunits')");
 ck("JS MathML normal access stays case sensitive","m.setAttribute('definitionURL','next');m.getAttribute('definitionURL')==='next'&&!m.hasAttribute('definitionurl')");
 ck("JS differently cased foreign names coexist","s.setAttribute('Mixed','one');s.setAttribute('mixed','two');s.getAttribute('Mixed')==='one'&&s.getAttribute('mixed')==='two'");
 ck("JS byte-length values keep embedded NUL","s.setAttribute('Mixed','a\\u0000b');s.getAttribute('Mixed').length===3&&s.getAttribute('mixed')==='two'");
 ck("JS removal is exact for both mixed names","s.removeAttribute('Mixed');!s.hasAttribute('Mixed')&&s.getAttribute('mixed')==='two'");
 ck("JS HTML uppercase writes stay canonical","h.setAttribute('WIDTH','64');h.getAttribute('width')==='64'&&h.getAttribute('WiDtH')==='64'&&h.getAttributeNames().indexOf('WIDTH')<0");
 ck("JS HTML uppercase snapshots and removal agree","h.getAttributeNode('WIDTH').name==='width'&&(h.removeAttribute('WiDtH'),!h.hasAttribute('width'))");
 ck("JS foreign uppercase ID does not alter lowercase index","s.setAttribute('ID','upper-id');s.id==='s'&&document.getElementById('upper-id')===null&&s.getAttribute('ID')==='upper-id'");
 ck("JS raw NS spelling remains intact","s.setAttributeNS(null,'preserveAspectRatio','none');s.getAttribute('preserveAspectRatio')==='none'&&s.getAttributeNS(null,'preserveAspectRatio')==='none'");
 ck("JS HTML raw NS mixed-case removal remains exact","h.setAttributeNS(null,'MixedNS','raw');h.getAttributeNS(null,'MixedNS')==='raw'&&(h.removeAttributeNS(null,'MixedNS'),!h.hasAttributeNS(null,'MixedNS'))");
 js_page_close();dom_free(root);printf("foreign-attributes: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
