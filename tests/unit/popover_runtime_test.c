#define main select_state_original_main
#include "select_state_test.c"
#undef main
#include "focus.h"
#include "top_layer.h"
static void native_ck(int ok,const char *name){checks++;if(!ok){failures++;printf("FAIL %s\n",name);}}
int main(void) {
 const char *html="<!doctype html><button id=o popovertarget=p>OPEN</button><div id=p popover><button id=i>IN</button></div><div id=m popover=manual>MANUAL</div><div id=h popover=hint>HINT</div><button id=b>BACK</button><dialog id=d><button>MODAL</button></dialog>";
 struct node *root=dom_parse(html,strlen(html));if(!js_page_open(root))return 2;js_page_eval("void 0",6,"<warmup>",0);
 ck("showPopover changes native state","var p=document.getElementById('p'),o=document.getElementById('o'),i=document.getElementById('i'),m=document.getElementById('m'),d=document.getElementById('d');o.focus();p.showPopover();p.matches(':popover-open')");
 struct node *p=dom_get_element_by_id(root->doc,"p"),*o=dom_get_element_by_id(root->doc,"o"),*i=dom_get_element_by_id(root->doc,"i"),*b=dom_get_element_by_id(root->doc,"b");
 native_ck(top_layer_is_popover(p)&&top_layer_contains(p)&&!top_layer_is_modal(p)&&!top_layer_current(),"popover enters real nonmodal top layer");
 ck("popover does not trap native focus","i.focus();o.focus();document.activeElement===o");
 native_ck(top_layer_allows_input(b),"popover leaves background input enabled");
 top_layer_pointer_down(i);top_layer_pointer_up(i);ck("inside click does not dismiss","p.matches(':popover-open')");
 top_layer_pointer_down(i);top_layer_pointer_up(b);ck("drag from inside does not dismiss","p.matches(':popover-open')");
 top_layer_pointer_down(b);native_ck(top_layer_pointer_up(b),"outside pointer reports a settled change");ck("outside click light dismisses auto popover","!p.matches(':popover-open')");
 ck("popover target button uses native show","o.click();p.matches(':popover-open')");
 top_layer_pointer_down(o);top_layer_pointer_up(o);ck("invoker pointerup does not dismiss before toggle click","p.matches(':popover-open')");
 ck("popover target toggles closed exactly once","o.click();!p.matches(':popover-open')");
 ck("manual popover shows","m.showPopover();m.matches(':popover-open')");
 top_layer_pointer_down(b);top_layer_pointer_up(b);top_layer_escape();ck("manual ignores outside click and Escape","m.matches(':popover-open')");
 ck("auto can open beside manual","p.showPopover();p.matches(':popover-open')&&m.matches(':popover-open')");top_layer_escape();
 ck("Escape dismisses auto but preserves manual","!p.matches(':popover-open')&&m.matches(':popover-open')");
 ck("hide and toggle update native state","m.hidePopover();p.togglePopover(true)&&p.togglePopover(false)===false&&!p.matches(':popover-open')");
 ck("autofocus enters popover","i.setAttribute('autofocus','');o.focus();p.showPopover();document.activeElement===i");
 ck("hide restores focus only when focus remained inside","p.hidePopover();document.activeElement===o");
 ck("source preserves an ancestor auto popover","var child=document.createElement('div');child.popover='auto';document.body.appendChild(child);p.showPopover();child.showPopover({source:i});p.matches(':popover-open')&&child.matches(':popover-open')");
 top_layer_pointer_down(p);top_layer_pointer_up(p);
 ck("click ancestor closes only the descendant auto popover","p.matches(':popover-open')&&!child.matches(':popover-open')");top_layer_escape();
 ck("closing event cannot recursively hide forever","var closedEvents=0;function reh(e){if(e.newState==='closed'){closedEvents++;p.hidePopover()}}p.addEventListener('beforetoggle',reh);p.showPopover();p.hidePopover();p.removeEventListener('beforetoggle',reh);closedEvents===1&&!p.matches(':popover-open')");
 ck("cancelable beforetoggle prevents native admission","function veto(e){if(e.newState==='open')e.preventDefault()}p.addEventListener('beforetoggle',veto);p.showPopover();!p.matches(':popover-open')");
 ck("attribute removal closes native layer","p.removeEventListener('beforetoggle',veto);var closes=0;p.addEventListener('beforetoggle',function(e){if(e.newState==='closed')closes++});p.showPopover();p.removeAttribute('popover');!p.matches(':popover-open')&&closes===1");
 native_ck(!top_layer_contains(p),"removed popover attribute prunes native layer");
 ck("detachment closes native layer","p.setAttribute('popover','');p.showPopover();p.remove();!p.matches(':popover-open')");
 ck("unsupported hint refuses instead of claiming visible","var refused=false;try{document.getElementById('h').showPopover()}catch(e){refused=e.name==='NotSupportedError'}refused");
 ck("modal state remains separate","d.showModal();d.matches(':modal')&&!d.matches(':popover-open')");top_layer_escape();ck("modal Escape retains dialog close behavior","!d.open");
 top_layer_reset();focus_reset();js_page_close();dom_free(root);
 printf("popover runtime: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
