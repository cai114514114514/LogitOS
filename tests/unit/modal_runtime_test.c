#define main select_state_original_main
#include "select_state_test.c"
#undef main
#include "focus.h"
#include "top_layer.h"
int main(void) {
    const char *html="<!doctype html><button id=o>OPEN</button><dialog id=d><input id=i autofocus><button id=b>CLOSE</button></dialog><dialog id=e><button id=q>NESTED</button></dialog>";
    struct node *root=dom_parse(html,strlen(html));js_page_set_location("http://example.com/");if(!js_page_open(root))return 2;
    js_page_eval("void 0;",7,"<warmup>",0);
    ck("showModal reaches native state and native focus","var o=document.getElementById('o'),d=document.getElementById('d'),i=document.getElementById('i'),e=document.getElementById('e'),q=document.getElementById('q');o.focus();d.showModal();d.open && d.matches(':modal') && document.activeElement===i");
    checks++;if(top_layer_current()!=dom_get_element_by_id(root->doc,"d")){puts("FAIL script state reaches native top layer");failures++;}
    ck("native rejected focus has no synthetic fallback","o.focus();document.activeElement===i");
    ck("nested modal saves previous focus","e.showModal();document.activeElement===q && e.matches(':modal') && d.matches(':modal')");
    ck("nested close restores previous modal focus","e.close();document.activeElement===i && !e.matches(':modal') && d.matches(':modal')");
    ck("cancel listener installed","var cancels=0,closed=0;function veto(ev){cancels++;ev.preventDefault()}d.addEventListener('cancel',veto);d.addEventListener('close',function(){closed++});true");
    top_layer_escape();ck("Escape honors canceled cancel event","d.open && cancels===1 && closed===0 && document.activeElement===i");
    ck("remove cancel veto","d.removeEventListener('cancel',veto);true");top_layer_escape();
    ck("Escape closes and restores focus","!d.open && !d.matches(':modal') && closed===1 && document.activeElement===o");
    ck("ordinary show remains nonmodal","d.show();d.open && !d.matches(':modal')");
    ck("showModal rejects nonmodal open state","var invalid=false;try{d.showModal()}catch(err){invalid=err.name==='InvalidStateError'}d.close();invalid");
    ck("close returnValue and focus restoration","o.focus();d.showModal();d.close('accepted');d.returnValue==='accepted' && document.activeElement===o");
    ck("direct open removal releases modal state","d.showModal();d.removeAttribute('open');!d.matches(':modal') && document.activeElement===o");
    ck("detached modal releases native block","d.showModal();d.remove();!d.matches(':modal') && document.activeElement===o");
    top_layer_reset();focus_reset();js_page_close();dom_free(root);
    printf("modal runtime: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
