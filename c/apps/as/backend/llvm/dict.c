/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"

/* Keys and values keep their concrete native layout. Temporary key slots are
 * entry-block allocations, so lookup inside a loop uses bounded stack space. */
Val at_ir_dict_lookup(Gen *g, AtNode *site, Val dictionary, Val key)
{
    at_ir_emit(g, "  store %s %s, ptr %%dictkey%d\n", at_ir_type(g, key.type), key.text, site->id);
    Val pointer = at_ir_temp(g, 0);
    at_ir_emit(g, "  %s = call ptr @at_dict_get(ptr %s, ptr %%dictkey%d)\n", pointer.text,
               dictionary.text, site->id);
    return pointer;
}

void at_ir_dict_store(Gen *g, AtNode *site, Val dictionary, Val key, Val value)
{
    at_ir_emit(g, "  store %s %s, ptr %%dictkey%d\n", at_ir_type(g, key.type), key.text, site->id);
    at_ir_emit(g, "  store %s %s, ptr %%dictvalue%d\n", at_ir_type(g, value.type), value.text,
               site->id);
    Val status = at_ir_temp(g, AT_I32);
    at_ir_emit(g, "  %s = call i32 @at_dict_set(ptr %s, ptr %%dictkey%d, ptr %%dictvalue%d)\n",
               status.text, dictionary.text, site->id, site->id);
    at_ir_allocation(g, status, site, 0);
}

Val at_ir_dict_literal(Gen *g, AtNode *node, Val result)
{
    AtType *type = &g->p->types[node->type];
    at_ir_emit(g,
               "  %s = call ptr @at_dict_new(i64 ptrtoint (ptr getelementptr (%s, ptr null, "
               "i32 1) to i64), i64 ptrtoint (ptr getelementptr (%s, ptr null, i32 1) to i64), "
               "ptr @scan%d, ptr @scan%d, ptr @hash%d, ptr @equal%d)\n",
               result.text, at_ir_type(g, type->key), at_ir_type(g, type->element), type->key,
               type->element, type->key, type->key);
    at_ir_allocation(g, result, node, 1);
    at_ir_emit(g, "  store ptr %s, ptr %%construction%d\n", result.text, node->id);
    for (int i = 0; i < node->count; i++) {
        AtNode *pair = node->args[i];
        Val key = at_ir_expression(g, pair->a);
        Val value = at_ir_expression(g, pair->b);
        at_ir_dict_store(g, node, result, key, value);
    }
    return result;
}

Val at_ir_dict_call(Gen *g, AtNode *node, Val result)
{
    Val receiver = at_ir_expression(g, node->a->a);
    if (node->symbol == AT_CALL_DICT_KEYS || node->symbol == AT_CALL_DICT_VALUES) {
        at_ir_emit(g, "  %s = call ptr @%s(ptr %s)\n", result.text,
                   node->symbol == AT_CALL_DICT_KEYS ? "at_dict_keys" : "at_dict_values",
                   receiver.text);
        at_ir_allocation(g, result, node, 1);
        return result;
    }
    Val key = at_ir_expression(g, node->args[0]);
    if (node->symbol == AT_CALL_DICT_REMOVE) {
        Val removed = at_ir_temp(g, AT_I32);
        at_ir_emit(g, "  store %s %s, ptr %%dictkey%d\n", at_ir_type(g, key.type), key.text,
                   node->id);
        at_ir_emit(g, "  %s = call i32 @at_dict_remove(ptr %s, ptr %%dictkey%d)\n", removed.text,
                   receiver.text, node->id);
        at_ir_emit(g, "  %s = icmp ne i32 %s, 0\n", result.text, removed.text);
        return result;
    }
    /* get's default is an ordinary eager argument. Evaluate it before lookup:
     * it may mutate/resize the dictionary, invalidating an earlier bucket. */
    Val fallback = at_ir_value(node->type, "zeroinitializer");
    if (node->symbol == AT_CALL_DICT_GET && node->count == 2) {
        fallback = at_ir_expression(g, node->args[1]);
    }
    Val pointer = at_ir_dict_lookup(g, node, receiver, key);
    if (node->symbol == AT_CALL_DICT_HAS) {
        at_ir_emit(g, "  %s = icmp ne ptr %s, null\n", result.text, pointer.text);
        return result;
    }
    Val missing = at_ir_temp(g, AT_BOOL);
    int absent = at_ir_label(g), present = at_ir_label(g), done = at_ir_label(g);
    at_ir_emit(g, "  %s = icmp eq ptr %s, null\n", missing.text, pointer.text);
    at_ir_emit(g, "  br i1 %s, label %%b%d, label %%b%d\n", missing.text, absent, present);
    at_ir_mark(g, absent);
    at_ir_jump(g, done);
    at_ir_mark(g, present);
    int value_type = g->p->types[receiver.type].element;
    Val found = at_ir_temp(g, value_type);
    at_ir_emit(g, "  %s = load %s, ptr %s\n", found.text, at_ir_type(g, value_type), pointer.text);
    if (node->count == 1) {
        found = at_ir_optional_wrap(g, found, node->type);
    }
    at_ir_jump(g, done);
    at_ir_mark(g, done);
    at_ir_emit(g, "  %s = phi %s [ %s, %%b%d ], [ %s, %%b%d ]\n", result.text,
               at_ir_type(g, node->type), fallback.text, absent, found.text, present);
    return result;
}

void at_ir_dict_callbacks(Gen *g, int type)
{
    const char *layout = at_ir_type(g, type);
    at_ir_emit(g, "define internal i64 @hash%d(ptr %%slot) {\nentry:\n", type);
    at_ir_emit(g, "  %%value = load %s, ptr %%slot\n", layout);
    if (type == AT_STR) {
        at_ir_emit(g, "  %%text = extractvalue %s %%value, 0\n", layout);
        at_ir_emit(g, "  %%size = extractvalue %s %%value, 1\n", layout);
        at_ir_emit(g, "  %%hash = call i64 @at_hash_text(ptr %%text, i64 %%size)\n");
    } else {
        const char *operand = "%value";
        if (at_bits(g->p, type) < 64) {
            at_ir_emit(g, "  %%wide = %s %s %%value to i64\n",
                       at_signed(g->p, type) ? "sext" : "zext", layout);
            operand = "%wide";
        }
        at_ir_emit(g, "  %%hash = call i64 @at_hash_u64(i64 %s)\n", operand);
    }
    at_ir_emit(g, "  ret i64 %%hash\n}\n");
    at_ir_emit(g, "define internal i32 @equal%d(ptr %%lhs, ptr %%rhs) {\nentry:\n", type);
    at_ir_emit(g, "  %%left = load %s, ptr %%lhs\n  %%right = load %s, ptr %%rhs\n", layout,
               layout);
    if (type == AT_STR) {
        at_ir_emit(g, "  %%a = extractvalue %s %%left, 0\n  %%an = extractvalue %s %%left, 1\n",
                   layout, layout);
        at_ir_emit(g, "  %%b = extractvalue %s %%right, 0\n  %%bn = extractvalue %s %%right, 1\n",
                   layout, layout);
        at_ir_emit(
            g,
            "  %%comparison = call i32 @at_text_compare(ptr %%a, i64 %%an, ptr %%b, i64 %%bn)\n");
        at_ir_emit(g, "  %%same = icmp eq i32 %%comparison, 0\n");
    } else {
        at_ir_emit(g, "  %%same = icmp eq %s %%left, %%right\n", layout);
    }
    at_ir_emit(g, "  %%result = zext i1 %%same to i32\n  ret i32 %%result\n}\n");
}

void at_ir_dict_slots(Gen *g, AtNode *node)
{
    int type = 0;
    if (node->kind == AN_DICT) {
        type = node->value_type;
    } else if (node->kind == AN_INDEX) {
        type = node->a->type;
    } else if (node->kind == AN_BINARY && node->op == T_IN) {
        type = node->b->type;
    } else if (node->kind == AN_CALL && node->symbol <= AT_CALL_DICT_GET &&
               node->symbol >= AT_CALL_DICT_REMOVE) {
        type = node->a->a->type;
    }
    if (g->p->types[type].kind != AT_DICT) {
        return;
    }
    AtType *dictionary = &g->p->types[type];
    at_ir_emit(g, "  %%dictkey%d = alloca %s\n", node->id, at_ir_type(g, dictionary->key));
    at_ir_emit(g, "  %%dictvalue%d = alloca %s\n", node->id, at_ir_type(g, dictionary->element));
}
