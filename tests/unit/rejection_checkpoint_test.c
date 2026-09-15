#define main iface_fixture_main
#include "dom_iface_test.c"
#undef main
int main(void)
{
    struct node *root=dom_parse(HTML,(int)strlen(HTML));
    js_page_set_location("https://example.com/");
    if(!root||!js_page_open(root))return 2;g_ctx=js_page_ctx();
    ckjs("var notices=[],handled=[];onunhandledrejection=function(e){notices.push(e);e.preventDefault()};onrejectionhandled=function(e){handled.push(e)};Promise.reject('caught').catch(function(){});notices.length===0", "caught rejection is not synchronously reported");
    js_page_pump();js_page_run_due();
    ckjs("notices.length===0", "catch before checkpoint suppresses report");
    ckjs("var late=Promise.reject('late');notices.length===0", "unhandled rejection waits for task boundary");
    js_page_pump();js_page_run_due();
    ckjs("notices.length===1&&notices[0].promise===late&&notices[0].reason==='late'&&notices[0].defaultPrevented", "unhandled event carries real promise and cancelable reason");
    ckjs("late.catch(function(){});handled.length===0", "late handling is asynchronous");
    js_page_pump();js_page_run_due();
    ckjs("handled.length===1&&handled[0].promise===late&&handled[0].type==='rejectionhandled'&&!handled[0].cancelable", "late handler receives rejectionhandled once");
    js_page_run_due();
    ckjs("notices.length===1&&handled.length===1", "notification does not repeat on idle poll");
    ckjs("(async function(){try{await Promise.reject('awaited')}catch(e){}})();true", "await fixture starts");
    js_page_pump();js_page_run_due();
    ckjs("notices.length===1", "await catches rejection before notification");
    ckjs("Promise.reject('closing');true", "pending close fixture");
    js_page_close();dom_free(root);
    root=dom_parse(HTML,(int)strlen(HTML));if(!js_page_open(root))return 2;g_ctx=js_page_ctx();
    ckjs("var clean=0;onunhandledrejection=function(e){clean++;e.preventDefault()};true", "new page opens");
    js_page_run_due();ckjs("clean===0", "closed page cannot report into new page");
    js_page_close();dom_free(root);
    printf("rejection-checkpoint: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
