/* A live ValidityState keeps the real control wrapper in a private WeakMap.
 * It does not stamp attributes, snapshot booleans, or look only in the active
 * document. Detached controls and adopted wrappers therefore use the same
 * serial-checked native state as painting, editing and successful controls. */
#include "js/dom/validation.h"
#include "js/dom/dom.h"
#include "dom/forms.h"
#include "dom/validation.h"
#include "js/net/url.h"

#include <string.h>

static JSContext *validation_ctx;
static JSValue pattern_function;
static JSValue submit_event_function;

static int control_interface(struct node *n)
{
    if (!n || n->type != N_ELEM || n->ns != NS_HTML) return 0;
    return fc_kind(n) || fc__tag(n->tag, "fieldset") || fc__tag(n->tag, "output") ||
           fc__tag(n->tag, "object");
}

static struct node *validation_receiver(JSContext *ctx, int argc, JSValueConst *argv)
{
    struct node *n = argc ? js_dom_node_from(argv[0]) : 0;
    if (!control_interface(n)) JS_ThrowTypeError(ctx, "Illegal invocation");
    return control_interface(n) ? n : 0;
}

static JSValue validation_brand(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    struct node *n = argc ? js_dom_node_from(argv[0]) : 0;
    const char *tag = argc > 1 ? JS_ToCString(ctx, argv[1]) : 0;
    int matches = tag && n && n->type == N_ELEM && n->ns == NS_HTML && !strcmp(n->tag, tag);
    if (tag) JS_FreeCString(ctx, tag);
    if (!matches) return JS_ThrowTypeError(ctx, "Illegal invocation");
    return JS_UNDEFINED;
}

static JSValue input_type(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    struct node *n = argc ? js_dom_node_from(argv[0]) : 0;
    if (!n || n->ns != NS_HTML || n->type != N_ELEM || !fc__tag(n->tag, "input"))
        return JS_ThrowTypeError(ctx, "Illegal invocation");
    const char *type = "text";
    switch (fc_kind(n)) {
    case FC_PASSWORD: type = "password"; break;
    case FC_SEARCH: type = "search"; break;
    case FC_EMAIL: type = "email"; break;
    case FC_URL: type = "url"; break;
    case FC_TEL: type = "tel"; break;
    case FC_NUMBER: type = "number"; break;
    case FC_CHECKBOX: type = "checkbox"; break;
    case FC_RADIO: type = "radio"; break;
    case FC_SUBMIT: type = "submit"; break;
    case FC_RESET: type = "reset"; break;
    case FC_BUTTON: type = "button"; break;
    case FC_IMAGEBTN: type = "image"; break;
    case FC_HIDDEN: type = "hidden"; break;
    case FC_FILE: type = "file"; break;
    case FC_RANGE: type = "range"; break;
    case FC_COLOR: type = "color"; break;
    default: break;
    }
    return JS_NewString(ctx, type);
}

static JSValue validation_flags(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    struct node *n = validation_receiver(ctx, argc, argv);
    return n ? JS_NewInt32(ctx, (int)fc_validity(n)) : JS_EXCEPTION;
}

static JSValue validation_will(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    struct node *n = validation_receiver(ctx, argc, argv);
    return n ? JS_NewBool(ctx, fc_will_validate(n)) : JS_EXCEPTION;
}

static JSValue validation_message(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    struct node *n = validation_receiver(ctx, argc, argv);
    if (!n) return JS_EXCEPTION;
    int len = 0;
    const char *message = fc_validation_message(n, &len);
    return JS_NewStringLen(ctx, message, (size_t)len);
}

static JSValue validation_custom(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    struct node *n = validation_receiver(ctx, argc, argv);
    if (!n) return JS_EXCEPTION;
    if (argc < 2) return JS_ThrowTypeError(ctx, "setCustomValidity requires a message");
    uint32_t serial = n->serial;
    size_t len = 0;
    const char *message = JS_ToCStringLen(ctx, &len, argv[1]);
    if (!message) return JS_EXCEPTION;
    int live = n->serial == serial;
    int ok = live && fc_set_custom_validity(n, message, (int)len);
    JS_FreeCString(ctx, message);
    if (!live) return JS_ThrowTypeError(ctx, "The control was destroyed during message conversion");
    if (!ok) return JS_ThrowOutOfMemory(ctx);
    js_dom_control_changed(n);
    return JS_UNDEFINED;
}

static JSValue validation_check(JSContext *ctx, JSValueConst self, int argc,
                                JSValueConst *argv, int report)
{
    (void)self;
    struct node *n = argc ? js_dom_node_from(argv[0]) : 0;
    if (!n || n->type != N_ELEM) return JS_ThrowTypeError(ctx, "Illegal invocation");
    if (fc__tag(n->tag, "form")) return JS_NewBool(ctx, fc_validate_form(n, report));
    if (!control_interface(n)) return JS_ThrowTypeError(ctx, "Illegal invocation");
    return JS_NewBool(ctx, fc_check_validity(n, report));
}

static JSValue control_owner(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    struct node *n = argc ? js_dom_node_from(argv[0]) : 0;
    return js_dom_node_value(ctx, control_interface(n) ? fc_form_of(n) : 0);
}

static struct node *ordinary_root(struct node *n)
{
    while (n->parent && !dom_is_shadow_root(n)) n = n->parent;
    return n;
}

static struct node *ordinary_next(struct node *n, struct node *root)
{
    if (n->first_child) return n->first_child;
    while (n != root && !n->next) n = n->parent;
    return n == root ? 0 : n->next;
}

static JSValue form_controls(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    struct node *form = argc ? js_dom_node_from(argv[0]) : 0;
    if (!form || form->type != N_ELEM || !fc__tag(form->tag, "form"))
        return JS_ThrowTypeError(ctx, "Illegal invocation");
    JSValue result = JS_NewArray(ctx);
    if (JS_IsException(result)) return result;
    struct node *root = ordinary_root(form);
    uint32_t index = 0;
    for (struct node *n = root; n; n = ordinary_next(n, root)) {
        if (n == form || !control_interface(n) || fc_kind(n) == FC_IMAGEBTN || fc_form_of(n) != form) continue;
        if (JS_SetPropertyUint32(ctx, result, index++, js_dom_node_value(ctx, n)) < 0) {
            JS_FreeValue(ctx, result); return JS_EXCEPTION;
        }
    }
    return result;
}

static int absolute_url(const char *value, int len)
{
    urlrec *url = url_parse_w(value, len, 0);
    if (!url) return 0;
    url_free_w(url);
    return 1;
}

static int pattern_match(struct node *n, const char *value, int len)
{
    if (!validation_ctx || !JS_IsFunction(validation_ctx, pattern_function)) return -1;
    JSContext *ctx = validation_ctx;
    int pattern_len = 0;
    const char *pattern = js_dom_attr_len(n, "pattern", &pattern_len);
    if (!pattern) return -1;
    JSValue args[2] = { JS_NewStringLen(ctx, pattern, (size_t)pattern_len),
                        JS_NewStringLen(ctx, value, (size_t)len) };
    JSValue result = JS_Call(ctx, pattern_function, JS_UNDEFINED, 2, args);
    int32_t answer = -1;
    if (JS_IsException(result)) { JSValue error = JS_GetException(ctx); JS_FreeValue(ctx, error); }
    else JS_ToInt32(ctx, &answer, result);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]);
    return answer;
}

static int dispatch_submission(struct node *form, struct node *submitter)
{
    if (!validation_ctx || !JS_IsFunction(validation_ctx, submit_event_function)) return 1;
    JSContext *ctx = validation_ctx;
    JSValue argument = js_dom_node_value(ctx, submitter);
    JSValue event = JS_Call(ctx, submit_event_function, JS_UNDEFINED, 1, &argument);
    JS_FreeValue(ctx, argument);
    if (JS_IsException(event)) {
        JSValue error = JS_GetException(ctx); JS_FreeValue(ctx, error);
        JS_FreeValue(ctx, event);
        return 0;
    }
    /* Native delivery accepts an actual Event and marks this UA event trusted.
     * Using dispatchEvent() here lost that provenance; stamping isTrusted on
     * a user object would manufacture it. Neither path grants activation. */
    JSValue result = js_dom_deliver_event(ctx, form, event);
    int proceed = !JS_IsException(result) && JS_ToBool(ctx, result) > 0;
    if (JS_IsException(result)) { JSValue error = JS_GetException(ctx); JS_FreeValue(ctx, error); }
    JS_FreeValue(ctx, result); JS_FreeValue(ctx, event);
    return proceed;
}

static const JSCFunctionListEntry natives[] = {
    JS_CFUNC_DEF("__fc_validationBrand", 2, validation_brand),
    JS_CFUNC_DEF("__fc_inputType", 1, input_type),
    JS_CFUNC_DEF("__fc_validity", 1, validation_flags),
    JS_CFUNC_DEF("__fc_willValidate", 1, validation_will),
    JS_CFUNC_DEF("__fc_validationMessage", 1, validation_message),
    JS_CFUNC_DEF("__fc_customValidity", 2, validation_custom),
    JS_CFUNC_MAGIC_DEF("__fc_checkValidity", 1, validation_check, 0),
    JS_CFUNC_MAGIC_DEF("__fc_reportValidity", 1, validation_check, 1),
    JS_CFUNC_DEF("__fc_owner", 1, control_owner),
    JS_CFUNC_DEF("__fc_elements", 1, form_controls)
};

static const struct { const char *name; unsigned bit; } validity_bits[] = {
    { "valueMissing", FC_VALUE_MISSING }, { "typeMismatch", FC_TYPE_MISMATCH },
    { "patternMismatch", FC_PATTERN_MISMATCH }, { "tooLong", FC_TOO_LONG },
    { "tooShort", FC_TOO_SHORT }, { "rangeUnderflow", FC_RANGE_UNDERFLOW },
    { "rangeOverflow", FC_RANGE_OVERFLOW }, { "stepMismatch", FC_STEP_MISMATCH },
    { "badInput", FC_BAD_INPUT }, { "customError", FC_CUSTOM_ERROR }
};

static const char shim[] =
"(function(G){\n"
"'use strict';\n"
"var doc=G.document, states=new WeakMap(), owners=new WeakMap();\n"
"var flags=G.__fc_validity, will=G.__fc_willValidate, message=G.__fc_validationMessage;\n"
"var custom=G.__fc_customValidity, check=G.__fc_checkValidity, report=G.__fc_reportValidity;\n"
"var brand=G.__fc_validationBrand, formOwner=G.__fc_owner, elements=G.__fc_elements;\n"
"var bits=G.__fc_validityBits, keys=Object.keys(bits);\n"
"function ValidityState(){ throw new TypeError('Illegal constructor'); }\n"
"var vp=ValidityState.prototype;\n"
"function owner(v){ if(!owners.has(v)) throw new TypeError('Illegal invocation'); return owners.get(v); }\n"
"keys.forEach(function(name){ var bit=bits[name]; Object.defineProperty(vp,name,{\n"
"  configurable:true,enumerable:true,get:function(){return !!(flags(owner(this)) & bit);} }); });\n"
"Object.defineProperty(vp,'valid',{configurable:true,enumerable:true,get:function(){return flags(owner(this))===0;} });\n"
"Object.defineProperty(vp,Symbol.toStringTag,{value:'ValidityState',configurable:true});\n"
"G.ValidityState=ValidityState;\n"
"function getter(p,name,fn){Object.defineProperty(p,name,{configurable:true,enumerable:true,get:fn});}\n"
/* Reflection used to advertise every temporal keyword even though fc_kind
 * explicitly uses text for those states. Preserve the setter/attribute, but
 * report the implemented effective type so feature detection takes fallback. */
"var ip=Object.getPrototypeOf(doc.createElement('input')), typeDescriptor=Object.getOwnPropertyDescriptor(ip,'type');\n"
"var inputType=G.__fc_inputType;\n"
"Object.defineProperty(ip,'type',{configurable:true,enumerable:true,\n"
"  get:function(){return inputType(this);},set:typeDescriptor&&typeDescriptor.set});\n"
"['input','select','textarea','button','fieldset','output','object'].forEach(function(tag){\n"
"  var p=Object.getPrototypeOf(doc.createElement(tag));\n"
"  if(!p || p===G.HTMLElement.prototype) return;\n"
"  function require(el){brand(el,tag);return el;}\n"
"  getter(p,'willValidate',function(){return will(require(this));});\n"
"  getter(p,'validationMessage',function(){return message(require(this));});\n"
"  getter(p,'validity',function(){\n"
"    require(this); var state=states.get(this);\n"
"    if(!state){state=Object.create(vp);owners.set(state,this);states.set(this,state);} return state;\n"
"  });\n"
"  Object.defineProperty(p,'setCustomValidity',{configurable:true,writable:true,enumerable:true,\n"
"    value:function(text){require(this);if(arguments.length<1)throw new TypeError('setCustomValidity requires a message');custom(this,text);} });\n"
"  Object.defineProperty(p,'checkValidity',{configurable:true,writable:true,enumerable:true,value:function(){return check(require(this));}});\n"
"  Object.defineProperty(p,'reportValidity',{configurable:true,writable:true,enumerable:true,value:function(){return report(require(this));}});\n"
"  getter(p,'form',function(){return formOwner(require(this));});\n"
"});\n"
"var fp=Object.getPrototypeOf(doc.createElement('form'));\n"
"Object.defineProperty(fp,'checkValidity',{configurable:true,writable:true,enumerable:true,value:function(){brand(this,'form');return check(this);}});\n"
"Object.defineProperty(fp,'reportValidity',{configurable:true,writable:true,enumerable:true,value:function(){brand(this,'form');return report(this);}});\n"
"getter(fp,'elements',function(){brand(this,'form');return elements(this);});\n"
"var Regex=G.RegExp, regexExec=Function.call.bind(Regex.prototype.exec), supportsV=false;\n"
/* Correction: accepting the flag is insufficient. libregexp.h documents a
 * compatibility v flag that still uses u class parsing. The shared forms
 * fixture failed 1/103 checks when that alias was accepted as HTML pattern
 * support. Require the v-only set/string grammar and its invalid punctuation
 * boundary; otherwise even a simple-looking pattern can be interpreted in
 * the wrong language. The regex engine's missing grammar is not filled here. */
"try{\n"
"  var intersection=new Regex('[[a-z]&&[^aeiou]]','v'), subtraction=new Regex('[[a-z]--[aeiou]]','v');\n"
"  var strings=new Regex('[\\\\q{ab|cd}]','v');\n"
"  supportsV=regexExec(intersection,'b')!==null && regexExec(intersection,'a')===null &&\n"
"    regexExec(subtraction,'b')!==null && regexExec(subtraction,'a')===null &&\n"
"    regexExec(strings,'ab')!==null && regexExec(strings,'bc')===null;\n"
"  if(supportsV){try{new Regex('[()]','v');supportsV=false;}catch(e){supportsV=e.name==='SyntaxError';}}\n"
"}catch(e){supportsV=false;}\n"
"var Submit=G.SubmitEvent;\n"
"return {supportsV:supportsV,pattern:function(pattern,value){\n"
"  if(!supportsV)return -1;var re;try{re=new Regex('^(?:'+pattern+')$','v');}catch(e){return -1;}\n"
"  return regexExec(re,value)!==null?1:0;\n"
"},submitEvent:function(submitter){return new Submit('submit',{bubbles:true,cancelable:true,submitter:submitter});}};\n"
"})(globalThis)\n";

void js_validation_cleanup(void)
{
    fc_set_submit_event(0);
    fc_set_pattern_validator(0);
    if (validation_ctx) {
        JS_FreeValue(validation_ctx, pattern_function);
        JS_FreeValue(validation_ctx, submit_event_function);
    }
    pattern_function = JS_UNDEFINED; submit_event_function = JS_UNDEFINED;
    validation_ctx = 0;
}

void js_validation_install(JSContext *ctx)
{
    js_validation_cleanup();
    if (!ctx) return;
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyFunctionList(ctx, global, natives, sizeof natives / sizeof natives[0]);
    JSValue bits = JS_NewObject(ctx);
    for (unsigned i = 0; i < sizeof validity_bits / sizeof validity_bits[0]; i++)
        JS_SetPropertyStr(ctx, bits, validity_bits[i].name, JS_NewInt32(ctx, (int)validity_bits[i].bit));
    JS_SetPropertyStr(ctx, global, "__fc_validityBits", bits);
    JS_FreeValue(ctx, global);
    JSValue bindings = JS_Eval(ctx, shim, sizeof shim - 1, "<form-validation>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(bindings)) {
        JSValue error = JS_GetException(ctx);
        const char *text = JS_ToCString(ctx, error);
        if (text) { int printf(const char *, ...); printf("[forms] validation shim failed: %s\n", text); JS_FreeCString(ctx, text); }
        JS_FreeValue(ctx, error); JS_FreeValue(ctx, bindings);
        return;
    }
    validation_ctx = ctx;
    pattern_function = JS_GetPropertyStr(ctx, bindings, "pattern");
    submit_event_function = JS_GetPropertyStr(ctx, bindings, "submitEvent");
    JSValue supports = JS_GetPropertyStr(ctx, bindings, "supportsV");
    if (!JS_ToBool(ctx, supports)) {
        int printf(const char *, ...);
        printf("[forms] pattern validation unavailable: RegExp Unicode sets (v) is unsupported\n");
    }
    JS_FreeValue(ctx, supports); JS_FreeValue(ctx, bindings);
    fc_set_url_validator(absolute_url);
    fc_set_pattern_validator(pattern_match);
    fc_set_submit_event(dispatch_submission);
}
