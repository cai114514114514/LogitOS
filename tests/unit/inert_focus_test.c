/* The existing select gate supplies host syscall/link stand-ins; all DOM,
 * focus, JS bindings and event dispatch below are the product implementations. */
#define main select_existing_main
#include "select_state_test.c"
#undef main
#include "focus.h"

static void native_check(int ok, const char *name)
{ checks++; if (!ok) { failures++; printf("FAIL %s\n", name); } }
static int focus_dispatch(struct node *n, const char *type, int bubbles, int cancelable)
{
    struct js_event_init init = {0}; init.bubbles = bubbles; init.cancelable = cancelable;
    return js_dom_dispatch(n, type, &init);
}
int main(void)
{
    const char *html = "<!doctype html><body><input id=outside>"
        "<section id=panel inert><div id=inside><input id=blocked tabindex=1>"
        "<button id=negative tabindex=-1>hidden from focus</button></div></section>"
        "<input id=after></body>";
    struct node *root = dom_parse(html, (int)strlen(html));
    if (!root) return 2;
    js_page_set_location("https://inert.example/");
    if (!js_page_open(root)) { dom_free(root); return 2; }
    fc_set_dispatch(focus_dispatch); focus_reset();
    struct node *outside = dom_get_element_by_id(root->doc, "outside");
    struct node *blocked = dom_get_element_by_id(root->doc, "blocked");
    struct node *after = dom_get_element_by_id(root->doc, "after");
    native_check(!focus_is_focusable(blocked), "inert ancestor removes native focusability");
    native_check(focus_tabbable_count(root) == 2, "Tab excludes inert positive tabindex subtree");
    native_check(focus_next(root, outside, 0) == after, "forward Tab skips inert descendants");
    native_check(focus_next(root, after, 1) == outside, "reverse Tab skips inert descendants");
    ck("program focus cannot enter inert subtree",
       "var outside=document.getElementById('outside'), blocked=document.getElementById('blocked'),"
       "panel=document.getElementById('panel'), inside=document.getElementById('inside');"
       "var events=[];document.addEventListener('focus',function(e){events.push(e.target.id);},true);"
       "outside.focus();blocked.focus();document.activeElement===outside && events.join(',')==='outside'");
    native_check(focus_current() == outside, "rejected inert focus preserves previous holder");
    focus_set_quiet(blocked);
    native_check(focus_current() == outside, "quiet restoration cannot enter inert subtree");
    ck("negative tabindex cannot bypass inert",
       "document.getElementById('negative').focus();document.activeElement===outside");
    ck("attribute removal restores program focus",
       "panel.removeAttribute('inert');blocked.focus();document.activeElement===blocked");
    ck("inert mutation invalidates activeElement on read",
       "panel.setAttribute('inert','');document.activeElement===document.body");
    native_check(focus_current() == NULL, "native holder clears when ancestor becomes inert");
    ck("removing inert does not resurrect old focus",
       "panel.removeAttribute('inert');document.activeElement===document.body");
    ck("boolean inert false string still blocks",
       "panel.setAttribute('inert','false');blocked.focus();document.activeElement===document.body");
    ck("nested child removal cannot override inert ancestor",
       "inside.setAttribute('inert','');inside.removeAttribute('inert');blocked.focus();document.activeElement===document.body");
    ck("dynamic insertion inherits inert immediately",
       "var fresh=document.createElement('input');fresh.id='fresh';inside.appendChild(fresh);fresh.focus();document.activeElement===document.body");
    ck("moving out restores focusability without restyle",
       "document.body.appendChild(fresh);fresh.focus();document.activeElement===fresh");
    ck("moving focused node into inert clears active read",
       "inside.appendChild(fresh);document.activeElement===document.body");
    ck("self inert invalidates focus and removing allows explicit focus",
       "panel.removeAttribute('inert');blocked.focus();blocked.setAttribute('inert','');"
       "var cleared=document.activeElement===document.body;blocked.removeAttribute('inert');blocked.focus();cleared&&document.activeElement===blocked");
    ck("explicit blur still works", "blocked.blur();document.activeElement===document.body");
    native_check(focus_advance(root, 0) && focus_current() == blocked,
                 "positive tabindex rejoins Tab order after inert removal");
#ifndef INERT_BEFORE
    native_check(!focus_is_inert(NULL), "null target is not an inert subtree");
    struct node *text = dom_get_element_by_id(root->doc, "negative")->first_child;
    ck("prepare inherited text target", "panel.setAttribute('inert','');true");
    native_check(focus_is_inert(text), "text hit target inherits inert from element ancestors");
#endif
    focus_reset(); fc_set_dispatch(NULL); js_page_close(); dom_free(root);
    printf("inert focus: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
