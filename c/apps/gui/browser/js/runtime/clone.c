/* SPDX-License-Identifier: MIT */
/* Native half of HTML structuredClone. The graph walker captures immutable
 * native brands rather than probing user constructors. Transfer intentionally
 * copies into preallocated receiving buffers: this engine exposes detachment,
 * but no ownership-swap API. It preserves observable transfer semantics at the
 * cost of a copy; it does not claim a zero-copy optimization.
 *
 * All graph getters, allocations, and transfer validation finish before the
 * first detach. In particular, reentrant author code can detach a listed buffer
 * while the outer graph is being read; commit must revalidate EVERY buffer,
 * not detach each one as it validates it and leave a partially-transferred list.
 * JS_WriteObject remains the separate bytecode/packet format, not this API. */
#include "js/runtime/clone.h"
#include "js/runtime/clone_graph.h"
#include <stdint.h>
#include <string.h>

struct clone_buffer {
    JSValue source;
    JSValue target;
    uint8_t *from;
    uint8_t *to;
    size_t length;
};

static JSValue clone_error(JSContext *ctx, JSValueConst constructor)
{
    JSValue args[2] = {
        JS_NewString(ctx, "The value cannot be cloned."),
        JS_NewString(ctx, "DataCloneError")
    };
    if (JS_IsException(args[0]) || JS_IsException(args[1])) {
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        return JS_EXCEPTION;
    }
    JSValue error = JS_CallConstructor(ctx, constructor, 2, (JSValueConst *)args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    return JS_IsException(error) ? error : JS_Throw(ctx, error);
}

static JSValue clone_brand(JSContext *ctx, JSValueConst self, int argc,
                           JSValueConst *argv)
{
    (void)self;
    return JS_NewUint32(ctx, argc ? JS_GetClassID(argv[0]) : 0);
}

static JSValue clone_string(JSContext *ctx, JSValueConst self, int argc,
                            JSValueConst *argv)
{
    (void)self;
    return JS_ToString(ctx, argc ? argv[0] : JS_UNDEFINED);
}

static int buffer_brand(JSContext *ctx, JSValueConst value, JSValueConst expected)
{
    uint32_t class_id;
    /* `expected` is an initialization-time token in a CFunctionData record;
     * never a public numeric enum or author-supplied property. */
    return JS_ToUint32(ctx, &class_id, expected) == 0 &&
           JS_GetClassID(value) == class_id;
}

static JSValue clone_copy_buffer(JSContext *ctx, JSValueConst self, int argc,
                                 JSValueConst *argv, int magic, JSValue *data)
{
    (void)self;
    (void)magic;
    if (argc < 1 || !buffer_brand(ctx, argv[0], data[1]))
        return clone_error(ctx, data[0]);
    if (JS_IsDetachedArrayBuffer(argv[0])) {
        /* HTML validates transfer-list type/duplicates before serialization,
         * but its detached check follows graph serialization. An already
         * detached transfer gets a private zero-byte placeholder so author
         * getters still run in the specified order; commit will refuse it.
         * Ordinary cloning of that same buffer must refuse immediately. */
        if (argc > 1 && JS_ToBool(ctx, argv[1]))
            return JS_NewArrayBufferCopy(ctx, NULL, 0);
        return clone_error(ctx, data[0]);
    }
    size_t size;
    uint8_t *bytes = JS_GetArrayBuffer(ctx, &size, argv[0]);
    /* An externally-backed empty buffer may have a NULL pointer. Its internal
     * detached slot was checked above; NULL alone is not a refusal signal. */
    return JS_NewArrayBufferCopy(ctx, bytes, size);
}

static JSValue clone_read_view(JSContext *ctx, JSValueConst self, int argc,
                               JSValueConst *argv, int magic, JSValue *data)
{
    (void)self;
    (void)magic;
    size_t offset, length, width;
    JSValue buffer = JS_GetArrayBufferView(ctx, argc ? argv[0] : JS_UNDEFINED,
                                          &offset, &length, &width);
    if (JS_IsException(buffer)) {
        /* The graph dispatcher already checked the native view brand. This
         * failure therefore means its backing buffer was detached, for which
         * HTML requires DataCloneError rather than the engine's TypeError. */
        JS_FreeValue(ctx, JS_GetException(ctx));
        return clone_error(ctx, data[0]);
    }
    JSValue view = JS_NewArray(ctx);
    if (JS_IsException(view)) {
        JS_FreeValue(ctx, buffer);
        return view;
    }
    JSValue values[4] = {
        buffer, JS_NewInt64(ctx, offset), JS_NewInt64(ctx, length),
        JS_NewInt64(ctx, width)
    };
    for (uint32_t i = 0; i < 4; i++) {
        if (JS_DefinePropertyValueUint32(ctx, view, i, values[i], JS_PROP_C_W_E) < 0) {
            for (uint32_t j = i + 1; j < 4; j++) JS_FreeValue(ctx, values[j]);
            JS_FreeValue(ctx, view);
            return JS_EXCEPTION;
        }
    }
    return view;
}

static void free_buffers(JSContext *ctx, struct clone_buffer *buffers, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        JS_FreeValue(ctx, buffers[i].source);
        JS_FreeValue(ctx, buffers[i].target);
    }
    js_free(ctx, buffers);
}

static JSValue clone_commit_buffers(JSContext *ctx, JSValueConst self, int argc,
                                    JSValueConst *argv, int magic, JSValue *data)
{
    (void)self;
    (void)magic;
    if (argc != 2) return clone_error(ctx, data[0]);
    /* Both arrays are private lists made by the graph walker with data own
     * elements. No author can replace a getter between validation and detach.
     * Array.prototype pollution cannot intercept any of these own reads. */
    JSValue size = JS_GetPropertyStr(ctx, argv[0], "length");
    uint32_t count = 0;
    int ok = JS_ToUint32(ctx, &count, size);
    JS_FreeValue(ctx, size);
    if (ok < 0) return JS_EXCEPTION;
    if (!count) return JS_UNDEFINED;
    if (sizeof(struct clone_buffer) > SIZE_MAX / (size_t)count)
        return JS_ThrowOutOfMemory(ctx);
    struct clone_buffer *buffers = js_malloc(ctx, (size_t)count * sizeof *buffers);
    if (!buffers) return JS_EXCEPTION;
    for (uint32_t i = 0; i < count; i++) {
        buffers[i].source = JS_UNDEFINED;
        buffers[i].target = JS_UNDEFINED;
    }
    for (uint32_t i = 0; i < count; i++) {
        struct clone_buffer *b = &buffers[i];
        b->source = JS_GetPropertyUint32(ctx, argv[0], i);
        b->target = JS_GetPropertyUint32(ctx, argv[1], i);
        if (JS_IsException(b->source) || JS_IsException(b->target)) goto fail;
        if (!buffer_brand(ctx, b->source, data[1]) ||
            !buffer_brand(ctx, b->target, data[1]) ||
            JS_IsDetachedArrayBuffer(b->source) || JS_IsDetachedArrayBuffer(b->target)) {
            clone_error(ctx, data[0]);
            goto fail;
        }
        size_t target_size;
        b->from = JS_GetArrayBuffer(ctx, &b->length, b->source);
        b->to = JS_GetArrayBuffer(ctx, &target_size, b->target);
        if (b->length != target_size) {
            clone_error(ctx, data[0]);
            goto fail;
        }
    }
    /* From this point there are no JS calls or allocations. Copy all current
     * payloads first, then detach all sources. A failed graph/validation above
     * has touched no source, including transfers absent from the data graph. */
    for (uint32_t i = 0; i < count; i++)
        if (buffers[i].length) memcpy(buffers[i].to, buffers[i].from, buffers[i].length);
    /* The unified HTML gate builds this control with only detachment disabled.
     * Its real ArrayBuffer/old-view assertions must turn red; a substitute
     * whole-API implementation would not isolate the irreversible boundary. */
#ifndef JS_CLONE_KEEP_BUFFER
    for (uint32_t i = 0; i < count; i++) JS_DetachArrayBuffer(ctx, buffers[i].source);
#endif
    free_buffers(ctx, buffers, count);
    return JS_UNDEFINED;
fail:
    free_buffers(ctx, buffers, count);
    return JS_EXCEPTION;
}

int js_clone_install_host(JSContext *ctx, JSValueConst hooks)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue exception = JS_GetPropertyStr(ctx, global, "DOMException");
    if (!JS_IsFunction(ctx, exception)) {
        JS_FreeValue(ctx, exception);
        JS_FreeValue(ctx, global);
        JS_ThrowInternalError(ctx, "structuredClone requires DOMException at bootstrap");
        return 0;
    }
    JSValue sample = JS_NewArrayBufferCopy(ctx, NULL, 0);
    if (JS_IsException(sample)) {
        JS_FreeValue(ctx, exception);
        JS_FreeValue(ctx, global);
        return 0;
    }
    JSValue data[2] = { exception, JS_NewUint32(ctx, JS_GetClassID(sample)) };
    JS_FreeValue(ctx, sample);
    JSValue args[6] = {
        JS_NewCFunction(ctx, clone_brand, "clone brand", 1),
        JS_NewCFunctionData(ctx, clone_copy_buffer, 1, 0, 2, (JSValueConst *)data),
        JS_NewCFunctionData(ctx, clone_read_view, 1, 0, 2, (JSValueConst *)data),
        JS_NewCFunctionData(ctx, clone_commit_buffers, 2, 0, 2, (JSValueConst *)data),
        JS_NewCFunction(ctx, clone_string, "clone ToString", 1),
        JS_DupValue(ctx, hooks)
    };
    JS_FreeValue(ctx, exception);
    int ready = 1;
    for (int i = 0; i < 6; i++) if (JS_IsException(args[i])) ready = 0;
    JSValue factory = ready ? JS_Eval(ctx, js_clone_graph, js_clone_graph_size,
                                     "<structured clone>", JS_EVAL_TYPE_GLOBAL) : JS_EXCEPTION;
    JSValue function = JS_IsException(factory) ? JS_EXCEPTION :
        JS_Call(ctx, factory, JS_UNDEFINED, 6, (JSValueConst *)args);
    JS_FreeValue(ctx, factory);
    for (int i = 0; i < 6; i++) JS_FreeValue(ctx, args[i]);
    int installed = 0;
    if (!JS_IsException(function)) {
        JS_MarkNativeFunction(ctx, function, 0);
        installed = JS_DefinePropertyValueStr(ctx, global, "structuredClone", function,
                                              JS_PROP_C_W_E) >= 0;
    }
    JS_FreeValue(ctx, global);
    return installed;
}

void js_clone_install(JSContext *ctx)
{
    (void)js_clone_install_host(ctx, JS_UNDEFINED);
}
