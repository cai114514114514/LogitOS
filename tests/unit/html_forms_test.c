/* Native controls first, then the actual shared JavaScript fixture in the
 * shipping DOM/runtime installers. Only network/image IO is stubbed: replacing
 * the DOM, event dispatcher or control state would make this test inspect its
 * own apparatus instead of the browser. Host success is not guest UI proof. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"
#include "dom/tree.h"
#include "dom/forms.h"
#include "dom/focus.h"
#include "dom/validation.h"
#include "js/dom/dom.h"
#include "js/runtime/page.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void kfree(void *p) { free(p); }
__attribute__((__weak__)) void img_register(void *data) { (void)data; }
__attribute__((__weak__)) void img_register_anim(void *a, void *b, void *c)
{ (void)a; (void)b; (void)c; }
int bfetch_resolve(const char *base, const char *ref, char *out, int max)
{ (void)base; if (!ref || !out || max <= 0) return 0; snprintf(out, (size_t)max, "%s", ref); return 1; }
int bfetch_sync(const char *ref, unsigned char **out, int *len)
{ (void)ref; (void)out; (void)len; return 0; }
void bfetch_prefetch(const char *ref) { (void)ref; }
void bfetch_prefetch_wait(void) { }
int res_fetch(const char *src, unsigned char **buf, int *len)
{ (void)src; (void)buf; (void)len; return -1; }
int text_measure(const char *text, int len, int px, int mono)
{ (void)text; (void)mono; return len * (px / 2); }

static int checks, failures;

static void check(int condition, const char *label)
{
    checks++;
    if (!condition) { printf("FAIL %s\n", label); failures++; }
}

static struct node *control(struct node *root, const char *tag, struct node *parent)
{
    struct node *n = dom_create_element(root->doc, tag, (int)strlen(tag));
    if (!n) { puts("HARNESS could not allocate a control"); exit(2); }
    if (parent) dom_append_child(parent, n);
    return n;
}

static int invalid_events, submit_events, click_events, navigations, reports;
static int cancel_invalid, cancel_click, navigation_event_flag;
static struct node *submitted_by, *invalid_first, *invalid_second;
static struct node *repair_target, *destroy_target;
static char report_message[160];

static int dispatch(struct node *target, const char *type, int bubbles, int cancelable)
{
    if (!strcmp(type, "invalid")) {
        invalid_events++;
        check(!bubbles && cancelable, "native invalid is non-bubbling and cancelable");
        if (!invalid_first) invalid_first = target;
        else if (!invalid_second) invalid_second = target;
        if (repair_target) {
            struct node *repair = repair_target; repair_target = 0;
            fc_set_value(repair, "fixed", 5);
        }
        if (destroy_target) {
            struct node *destroy = destroy_target; destroy_target = 0;
            dom_destroy_subtree(destroy);
        }
        return !cancel_invalid;
    }
    if (!strcmp(type, "submit")) { submit_events++; return 1; }
    if (!strcmp(type, "click")) { click_events++; return !cancel_click; }
    return 1;
}

static int navigate(struct node *form, struct node *submitter, int fire_event)
{
    (void)form;
    navigations++; submitted_by = submitter; navigation_event_flag = fire_event;
    return 1;
}

static void report(struct node *n, const char *message)
{
    (void)n; reports++;
    snprintf(report_message, sizeof report_message, "%s", message);
}

static void reset_events(void)
{
    invalid_events = submit_events = click_events = navigations = reports = 0;
    cancel_invalid = cancel_click = 0; navigation_event_flag = -1;
    submitted_by = invalid_first = invalid_second = repair_target = destroy_target = 0;
    report_message[0] = 0;
}

static void native_checks(void)
{
    const char *html = "<!doctype html><html><body id=b><form id=f>"
        "<input id=i required><button id=s type=submit>Send</button>"
        "</form></body></html>";
    struct node *root = dom_parse(html, (int)strlen(html));
    if (!root) { puts("HARNESS native parse failed"); exit(2); }
    struct node *body = dom_get_element_by_id(root->doc, "b");
    struct node *form = dom_get_element_by_id(root->doc, "f");
    struct node *input = dom_get_element_by_id(root->doc, "i");
    struct node *button = dom_get_element_by_id(root->doc, "s");
    if (!body || !form || !input || !button) { puts("HARNESS parsed controls missing"); exit(2); }
    fc_set_dispatch(dispatch); fc_set_submit(navigate); fc_set_validation_reporter(report);
    reset_events();
    check(fc_will_validate(input) && (fc_validity(input) & FC_VALUE_MISSING), "native required has a live error");
    check(!fc_submit(form, button, 1) && !navigations && !submit_events && invalid_events == 1,
          "native required prevents default submission");
    check(reports == 1 && report_message[0], "native rejected submission requests a visible message");
    cancel_invalid = 1;
    check(!fc_submit(form, button, 1) && !navigations && reports == 1,
          "native canceled invalid suppresses reporting but still blocks submission");
    reset_events();
    dom_set_attr(form, "novalidate", "");
    check(fc_submit(form, button, 1) && navigations == 1 && submit_events == 1 && !invalid_events,
          "native novalidate reaches the navigation consumer");
    check(navigation_event_flag == 0 && submitted_by == button, "native backend cannot dispatch a duplicate submit");
    dom_remove_attr(form, "novalidate");
    dom_set_attr(button, "formnovalidate", "");
    check(fc_submit(form, button, 1) && navigations == 2 && !invalid_events,
          "native submitter formnovalidate bypasses validation");
    dom_remove_attr(button, "formnovalidate");
    check(fc_submit(form, 0, 0) && navigations == 3 && submit_events == 2,
          "native submit method bypasses both validation and the submit event");

    fc_set_value(input, "filled", 6);
    reset_events();
    check(fc_implicit_submit(input) && navigations == 1 && click_events == 1 && submitted_by == button,
          "native Enter activates the default submit button");
    dom_set_attr(button, "disabled", "");
    check(!fc_implicit_submit(input) && navigations == 1 && click_events == 1,
          "native disabled default button prevents implicit submission");
    dom_remove_attr(button, "disabled"); cancel_click = 1;
    check(!fc_implicit_submit(input) && navigations == 1, "native canceled default-button click prevents submission");
    cancel_click = 0; dom_remove_child(form, button);
    struct node *second = control(root, "input", form);
    check(!fc_implicit_submit(input) && navigations == 1, "native multiple blocking fields prevent implicit submission without a button");
    dom_remove_child(form, second);
    check(fc_implicit_submit(input) && navigations == 2 && submitted_by == 0,
          "native single field can implicitly submit without a button");
    dom_remove_child(body, form);
    check(!fc_submit(form, 0, 0) && navigations == 2, "native disconnected form cannot navigate");
    dom_append_child(body, form); dom_append_child(form, button);

    dom_set_attr(input, "minlength", "4"); fc_set_value(input, "", 0);
    check(fc_edit_insert(input, "x", 1) && (fc_validity(input) & FC_TOO_SHORT),
          "native user editing activates minlength");
    fc_edit_insert(input, "abcd", 4); dom_set_attr(input, "maxlength", "2");
    check(fc_validity(input) & FC_TOO_LONG, "native lowering maxlength detects a prior user edit");
    fc_set_value(input, "abcdefg", 7);
    check(!(fc_validity(input) & (FC_TOO_LONG | FC_TOO_SHORT)), "native script assignment clears user-only length errors");
    dom_remove_attr(input, "minlength"); fc_set_value(input, "", 0);
    const char *emoji = "\xf0\x9f\x98\x80";
    check(fc_edit_insert(input, emoji, 4) && !fc_edit_insert(input, "a", 1) &&
          fc_value_length(fc_value(input, 0), 4) == 2, "native maxlength counts astral text in UTF-16 units");

    struct node *number = control(root, "input", form); dom_set_attr(number, "type", "number");
    fc_set_value(number, "nonsense", 8);
    check(!fc_value(number, 0)[0] && !fc_bad_input(number), "native number script assignment sanitizes invalid text");
    check(fc_edit_insert(number, "-", 1) && fc_bad_input(number) && !fc_value(number, 0)[0] &&
          !strcmp(fc_edit_value(number, 0), "-"), "native number editor preserves bad input while the API value is empty");
    fc_set_value(number, "0.3", 3); dom_set_attr(number, "min", "0.2"); dom_set_attr(number, "step", "0.1");
    check(!(fc_validity(number) & FC_STEP_MISMATCH), "native decimal steps tolerate binary rounding");
    fc_set_value(number, "0.35", 4);
    check(fc_validity(number) & FC_STEP_MISMATCH, "native decimal steps reject off-lattice values");
    dom_set_attr(number, "step", "any"); dom_set_attr(number, "max", "0.3");
    check((fc_validity(number) & FC_RANGE_OVERFLOW) && !(fc_validity(number) & FC_STEP_MISMATCH),
          "native max and step any remain independent");

    struct node *fieldset = control(root, "fieldset", form); dom_set_attr(fieldset, "disabled", "");
    struct node *legend = control(root, "legend", fieldset);
    struct node *allowed = control(root, "input", legend);
    struct node *barred = control(root, "input", fieldset);
    struct node *later_legend = control(root, "legend", fieldset);
    struct node *later = control(root, "input", later_legend);
    check(!fc_disabled(allowed) && fc_will_validate(allowed) && fc_disabled(barred) && fc_disabled(later),
          "native first legend exemption is local to its fieldset");
    struct node *outer = control(root, "fieldset", form); dom_set_attr(outer, "disabled", "");
    control(root, "legend", outer); dom_append_child(outer, fieldset);
    check(fc_disabled(allowed), "native inner legend cannot escape a disabled outer fieldset");

    /* The invalid set is fixed before listeners run. A repaired second field
     * still receives its initial invalid event; a destroyed identity must not
     * dispatch at the new node that reuses its arena slot. */
    struct node *snapshot = control(root, "form", body);
    struct node *a = control(root, "input", snapshot), *b = control(root, "input", snapshot);
    dom_set_attr(a, "required", ""); dom_set_attr(b, "required", "");
    reset_events(); repair_target = b;
    check(!fc_validate_form(snapshot, 0) && invalid_events == 2 && invalid_first == a && invalid_second == b,
          "native validation snapshots the invalid set before listeners repair a peer");
    fc_set_value(b, "", 0); fc_set_custom_validity(b, "old identity", 12);
    reset_events(); destroy_target = b;
    check(!fc_validate_form(snapshot, 0) && invalid_events == 1,
          "native validation does not dispatch to a destroyed peer");
    struct node *fresh = control(root, "input", snapshot);
    check(!(fc_validity(fresh) & FC_CUSTOM_ERROR), "native recycled node does not inherit a custom error");

    fc_set_dispatch(0); fc_set_submit(0); fc_set_validation_reporter(0);
    focus_reset(); fc_reset(); dom_free(root);
}

static JSValue js_check(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    const char *label = argc > 1 ? JS_ToCString(ctx, argv[1]) : 0;
    char name[240]; snprintf(name, sizeof name, "js %s", label ? label : "missing assertion label");
    check(argc > 0 && JS_ToBool(ctx, argv[0]) > 0, name);
    if (label) JS_FreeCString(ctx, label);
    return JS_UNDEFINED;
}

static char *fixture_read(const char *path, int *len)
{
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END)) { fclose(file); return 0; }
    long size = ftell(file);
    if (size < 0 || size > 1000000 || fseek(file, 0, SEEK_SET)) { fclose(file); return 0; }
    char *source = malloc((size_t)size + 1);
    if (!source || fread(source, 1, (size_t)size, file) != (size_t)size) {
        free(source); fclose(file); return 0;
    }
    fclose(file); source[size] = 0; *len = (int)size;
    return source;
}

static int eval(JSContext *ctx, const char *source, int len, const char *file)
{
    JSValue result = JS_Eval(ctx, source, (size_t)len, file, JS_EVAL_TYPE_GLOBAL);
    int ok = !JS_IsException(result);
    if (!ok) {
        JSValue error = JS_GetException(ctx);
        const char *text = JS_ToCString(ctx, error);
        printf("HARNESS %s: %s\n", file, text ? text : "exception");
        if (text) JS_FreeCString(ctx, text);
        JS_FreeValue(ctx, error);
    }
    JS_FreeValue(ctx, result); js_dom_run_jobs(ctx);
    return ok;
}

static int dispatch_js(struct node *target, const char *type, int bubbles, int cancelable)
{
    /* The host has no app/main.c, which installs this exact native-to-DOM
     * seam in the guest. Omitting it would test missing harness wiring instead
     * of invalid events; the real dispatcher still owns capture and bubbling. */
    struct js_event_init init = { 0 };
    init.bubbles = bubbles; init.cancelable = cancelable;
    return js_dom_dispatch(target, type, &init);
}

static void js_checks(void)
{
    const char *html = "<!doctype html><html><body></body></html>";
    struct node *root = dom_parse(html, (int)strlen(html));
    js_page_set_location("http://example.com/forms");
    if (!root || !js_page_open(root)) { puts("HARNESS JS runtime open failed"); exit(2); }
    if (!js_page_eval("void 0;", 7, "<warmup>", 0)) { puts("HARNESS JS warmup failed"); exit(2); }
    fc_set_dispatch(dispatch_js);
    JSContext *ctx = js_page_ctx();
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__formCheck", JS_NewCFunction(ctx, js_check, "check", 2));
    JS_FreeValue(ctx, global);
    const char *path = "tests/fixtures/html-priority/forms.js";
    int len = 0; char *source = fixture_read(path, &len);
    if (!source) { printf("HARNESS could not read %s\n", path); exit(2); }
    printf("[html-forms] fixture=%s bytes=%d\n", path, len);
    if (!eval(ctx, source, len, path)) { free(source); exit(2); }
    free(source);
    const char *run = "globalThis.__formsFinished=false;testHTMLForms(__formCheck).then(function(){__formsFinished=true;},"
        "function(error){__formCheck(false,'fixture threw '+String(error)+' '+(error.stack||''));__formsFinished=true;});";
    if (!eval(ctx, run, (int)strlen(run), "<run-forms>")) exit(2);
    global = JS_GetGlobalObject(ctx);
    JSValue done = JS_GetPropertyStr(ctx, global, "__formsFinished");
    check(JS_ToBool(ctx, done) > 0, "js async fixture completed in the real runtime");
    JS_FreeValue(ctx, done); JS_FreeValue(ctx, global);
    fc_set_dispatch(0); js_page_close(); focus_reset(); fc_reset(); dom_free(root);
}

int main(void)
{
    native_checks();
    js_checks();
    printf("HTML forms: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
