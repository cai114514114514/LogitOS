#include "js/dom/entries.h"
#include "js/dom/dom.h"
#include "dom/entries.h"
#include "dom/forms.h"
#include <string.h>
int printf(const char *, ...);
extern JSValue js_webapi_form_entries(JSContext *, JSValueConst, JSValueConst) LOGIT_WEAK;
LOGIT_WEAK_STUB(js_webapi_form_entries);

static JSValue dom_error(JSContext *ctx, const char *name, const char *message)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, "DOMException");
    JSValue args[2] = {JS_NewString(ctx, message), JS_NewString(ctx, name)};
    JSValue error = JS_CallConstructor(ctx, ctor, 2, (JSValueConst *)args);
    JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, ctor); JS_FreeValue(ctx, global);
    return JS_IsException(error) ? error : JS_Throw(ctx, error);
}

JSValue js_form_entries(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    struct node *form = argc ? js_dom_node_from(argv[0]) : NULL;
    if (!form || form->ns != NS_HTML || !fc__tag(form->tag, "form"))
        return JS_ThrowTypeError(ctx, "FormData requires an HTMLFormElement");
    struct node *submitter = NULL;
    if (argc > 1 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])) {
        submitter = js_dom_node_from(argv[1]);
        int kind = fc_kind(submitter);
        if (kind != FC_SUBMIT && kind != FC_IMAGEBTN)
            return JS_ThrowTypeError(ctx, "FormData submitter is not a submit button");
        if (fc_form_of(submitter) != form)
            return dom_error(ctx, "NotFoundError", "submitter has a different form owner");
    }
    struct form_entries list;
    int rc = fc_entries(form, submitter, &list);
    if (rc == -1) return JS_ThrowOutOfMemory(ctx);
    if (rc) return dom_error(ctx, "NotSupportedError", "form directionality is unavailable");
    JSValue result = JS_NewArray(ctx);
    for (size_t i = 0; i < list.count && !JS_IsException(result); i++) {
        struct form_entry *entry = &list.items[i];
        JSValue pair = JS_NewArray(ctx);
        if (JS_IsException(pair) ||
            JS_SetPropertyUint32(ctx, pair, 0, JS_NewStringLen(ctx, entry->name, entry->name_len)) < 0 ||
            JS_SetPropertyUint32(ctx, pair, 1, JS_NewStringLen(ctx, entry->value, entry->value_len)) < 0 ||
            JS_SetPropertyUint32(ctx, pair, 2, JS_NewBool(ctx, entry->file)) < 0) {
            JS_FreeValue(ctx, pair);
            JS_FreeValue(ctx, result); result = JS_EXCEPTION;
            break;
        }
        if (JS_SetPropertyUint32(ctx, result, (uint32_t)i, pair) < 0) {
            /* The final insertion consumes pair on either outcome. */
            JS_FreeValue(ctx, result); result = JS_EXCEPTION; break;
        }
    }
    fc_entries_free(&list); return result;
}

JSValue js_form_data_event(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    struct node *form = argc ? js_dom_node_from(argv[0]) : NULL;
    if (!form || argc < 2) return JS_ThrowTypeError(ctx, "formdata target is not a form");
    return js_dom_deliver_event(ctx, form, argv[1]);
}

int js_form_encode(JSContext *ctx, struct node *form, struct node *submitter, char *out, int max)
{
    struct node *root=js_dom_root();
    if (!ctx||!root||root->doc!=form->doc||!LOGIT_HAVE(js_webapi_form_entries)) return -2;
    JSValue owner=js_dom_node_value(ctx,form), button=js_dom_node_value(ctx,submitter);
    JSValue pairs=js_webapi_form_entries(ctx,owner,button);
    JS_FreeValue(ctx,owner);JS_FreeValue(ctx,button);
    if (JS_IsUndefined(pairs)) return -2;
    struct form_entries list={0};int result=-1,pending=JS_IsException(pairs);
    if (!JS_IsException(pairs)) {
        JSValue length=JS_GetPropertyStr(ctx,pairs,"length");
        uint32_t count=0;int failed=JS_ToUint32(ctx,&count,length)<0;
        JS_FreeValue(ctx,length);pending=failed;
        for (uint32_t i=0;i<count&&!failed;i++) {
            JSValue pair=JS_GetPropertyUint32(ctx,pairs,i);
            JSValue name=JS_GetPropertyUint32(ctx,pair,0),value=JS_GetPropertyUint32(ctx,pair,1);
            size_t nl=0,vl=0;
            const char *nm=NULL,*val=NULL;
            if (JS_IsException(pair)||JS_IsException(name)||JS_IsException(value)) failed=pending=1;
            else {
                nm=JS_ToCStringLen(ctx,&nl,name);val=JS_ToCStringLen(ctx,&vl,value);
                if (!nm||!val) failed=pending=1;
                else if (fc_entry_add(&list,nm,nl,val,vl,0)<0) {
                    JS_ThrowOutOfMemory(ctx);failed=pending=1;
                }
            }
            if (nm) JS_FreeCString(ctx,nm);if (val) JS_FreeCString(ctx,val);
            JS_FreeValue(ctx,name);JS_FreeValue(ctx,value);JS_FreeValue(ctx,pair);
        }
        if (!failed) result=fc_entries_encode(&list,out,max);
    }
    if (pending) {
        JSValue error=JS_GetException(ctx);
        const char *message=JS_ToCString(ctx,error);
        printf("[forms] entry construction failed: %s\n",message?message:"?");
        if (message) JS_FreeCString(ctx,message);JS_FreeValue(ctx,error);
    }
    fc_entries_free(&list);JS_FreeValue(ctx,pairs);return result;
}
