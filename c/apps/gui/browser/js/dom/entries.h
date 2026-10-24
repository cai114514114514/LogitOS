#ifndef LOGIT_JS_FORM_ENTRIES_H
#define LOGIT_JS_FORM_ENTRIES_H
#include "quickjs.h"
#include "../../../../../../include/weaksym.h"
struct node;
#ifdef JS_ENTRIES_OPTIONAL
extern JSValue js_form_entries(JSContext *, JSValueConst, int, JSValueConst *) LOGIT_WEAK;
extern JSValue js_form_data_event(JSContext *, JSValueConst, int, JSValueConst *) LOGIT_WEAK;
LOGIT_WEAK_STUB(js_form_entries);
LOGIT_WEAK_STUB(js_form_data_event);
extern int js_form_encode(JSContext *, struct node *, struct node *, char *, int) LOGIT_WEAK;
LOGIT_WEAK_STUB(js_form_encode);
#else
JSValue js_form_entries(JSContext *, JSValueConst, int, JSValueConst *);
JSValue js_form_data_event(JSContext *, JSValueConst, int, JSValueConst *);
int js_form_encode(JSContext *, struct node *, struct node *, char *, int);
#endif
#endif
