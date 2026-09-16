/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"
#include <stdlib.h>

/* A place describes storage, not an already evaluated value. Reference owners
 * and indices are SSA snapshots rooted by their expression nodes. Inline
 * fields/arrays retain their parent place so List/Dict growth can move backing
 * storage between the initial read and the final write. These descriptors live
 * only in the compiler; generated programs allocate no place objects. */
typedef struct AssignmentPlace {
    AtNode *node;
    struct AssignmentPlace *parent;
    Val owner;
    Val index;
    Val initial_address;
} AssignmentPlace;

static Val locate_place(Gen *g, AssignmentPlace *place, int refresh)
{
    AtNode *node = place->node;
    if (node->kind == AN_NAME || node->kind == AN_GLOBAL) {
        return place->initial_address;
    }
    Val base = place->owner;
    if (place->parent) {
        base = refresh ? locate_place(g, place->parent, 1) : place->parent->initial_address;
    }
    int owner_type = node->a->type;
    AtType *type = &g->p->types[owner_type];
    Val address = at_ir_temp(g, 0);
    if (node->kind == AN_FIELD) {
        if (type->kind == AT_LAYOUT) {
            return at_ir_layout_address(g, node, base);
        }
        const char *layout =
            type->kind == AT_CLASS ? at_ir_class_layout(g, owner_type) : at_ir_type(g, owner_type);
        int field = node->field + (type->kind == AT_CLASS ? AT_CLASS_HEADER_COUNT : 0);
        at_ir_emit(g, "  %s = getelementptr %s, ptr %s, i32 0, i32 %d\n", address.text, layout,
                   base.text, field);
    } else if (type->kind == AT_POINTER) {
        address = at_ir_pointer_at(g, base, place->index, node);
    } else if (type->kind == AT_REGION) {
        address = at_ir_region_at(g, base, place->index, node, 1);
    } else if (type->kind == AT_LIST) {
        address = at_ir_list_at(g, base, place->index, node);
    } else if (type->kind == AT_DICT) {
        address = at_ir_dict_lookup(g, node, base, place->index);
        Val missing = at_ir_temp(g, AT_BOOL);
        at_ir_emit(g, "  %s = icmp eq ptr %s, null\n", missing.text, address.text);
        at_ir_guard(g, missing, node, AT_E_KEY);
    } else if (at_byte_storage_kind(type->kind)) {
        address = at_ir_buffer_at(g, base, place->index, node);
    } else {
        Val length = at_ir_temp(g, AT_I64);
        if (type->kind == AT_ARRAY) {
            at_ir_emit(g, "  %s = add i64 0, %d\n", length.text, type->count);
        } else {
            Val data = at_ir_temp(g, 0);
            at_ir_emit(g, "  %s = extractvalue %s %s, 0\n", data.text, at_ir_type(g, owner_type),
                       base.text);
            at_ir_emit(g, "  %s = extractvalue %s %s, 1\n", length.text, at_ir_type(g, owner_type),
                       base.text);
            base = data;
        }
        Val invalid = at_ir_temp(g, AT_BOOL);
        at_ir_emit(g, "  %s = icmp uge i64 %s, %s\n", invalid.text, place->index.text, length.text);
        at_ir_guard(g, invalid, node, AT_E_INDEX);
        if (type->kind == AT_ARRAY) {
            at_ir_emit(g, "  %s = getelementptr %s, ptr %s, i32 0, i64 %s\n", address.text,
                       at_ir_type(g, owner_type), base.text, place->index.text);
        } else {
            at_ir_emit(g, "  %s = getelementptr %s, ptr %s, i64 %s\n", address.text,
                       at_ir_type(g, node->type), base.text, place->index.text);
        }
    }
    if (node->conversion == AT_OPTION_UNWRAP) {
        Val payload = at_ir_temp(g, 0);
        at_ir_emit(g, "  %s = getelementptr %s, ptr %s, i32 0, i32 1\n", payload.text,
                   at_ir_type(g, node->value_type), address.text);
        address = payload;
    }
    return address;
}

static AssignmentPlace *capture_place(Gen *g, AtNode *node, AssignmentPlace *storage, int *used)
{
    AssignmentPlace *place = &storage[(*used)++];
    place->node = node;
    if (node->kind == AN_NAME || node->kind == AN_GLOBAL) {
        place->initial_address = at_ir_address(g, node);
        return place;
    }
    int owner_kind = g->p->types[node->a->type].kind;
    if ((node->kind == AN_FIELD && owner_kind != AT_CLASS && owner_kind != AT_LAYOUT) ||
        (node->kind == AN_INDEX && owner_kind == AT_ARRAY)) {
        place->parent = capture_place(g, node->a, storage, used);
    } else {
        place->owner = at_ir_expression(g, node->a);
    }
    if (node->kind == AN_INDEX) {
        place->index = at_ir_expression(g, node->b);
    }
    /* The index itself can call user code and resize an inline ancestor.
     * Resolve parents again using their captured owners/indices, never a stale
     * element pointer. Earlier guards still precede this index's side effects. */
    place->initial_address = locate_place(g, place, 1);
    return place;
}

static AssignmentPlace *allocate_places(Gen *g, AtNode *node)
{
    int capacity = 1;
    for (AtNode *part = node->a; part && part->a; part = part->a) {
        capacity++;
    }
    AssignmentPlace *storage = calloc((size_t)capacity, sizeof(*storage));
    if (!storage) {
        g->failed = 1;
        at_error(g->p, node->module, node->token, "AS3501",
                 "Cannot allocate compound assignment storage plan");
        return NULL;
    }
    return storage;
}

void at_ir_compound_assignment(Gen *g, AtNode *node)
{
    AssignmentPlace *storage = allocate_places(g, node);
    if (!storage) {
        return;
    }
    int used = 0;
    AssignmentPlace *place = capture_place(g, node->a, storage, &used);
    int buffer_byte =
        node->a->kind == AN_INDEX &&
        (node->a->a->type == AT_REGION || at_byte_storage_kind(g->p->types[node->a->a->type].kind));
    const char *alignment =
        ((node->a->kind == AN_FIELD && g->p->types[node->a->a->type].kind == AT_LAYOUT) ||
         (node->a->kind == AN_INDEX && g->p->types[node->a->a->type].kind == AT_POINTER))
            ? ", align 1"
            : "";
    Val previous;
    if (buffer_byte) {
        previous = at_ir_buffer_read(g, place->initial_address);
    } else {
        previous = at_ir_temp(g, node->type);
        at_ir_emit(g, "  %s = load %s, ptr %s%s\n", previous.text, at_ir_type(g, node->type),
                   place->initial_address.text, alignment);
    }
    if (at_ir_references(g, previous.type)) {
        /* str += can replace the last reference to the previous string during
         * RHS evaluation. Loading a value alone is not a native GC root. */
        at_ir_emit(g, "  store %s %s, ptr %%rootvalue%d\n", at_ir_type(g, previous.type),
                   previous.text, node->a->id);
    }
    Val right = at_ir_expression(g, node->b);
    int operation = node->op == T_PLUSEQ    ? T_PLUS
                    : node->op == T_MINUSEQ ? T_MINUS
                    : node->op == T_STAREQ  ? T_STAR
                    : node->op == T_SLASHEQ ? T_SLASH
                                            : T_PERCENT;
    Val result = at_ir_numeric(g, operation, previous, right, node, 0);
    if (at_ir_references(g, result.type)) {
        /* A subsequent dictionary insertion can allocate too. */
        at_ir_emit(g, "  store %s %s, ptr %%rootvalue%d\n", at_ir_type(g, result.type), result.text,
                   node->id);
    }
    if (node->a->kind == AN_INDEX && g->p->types[node->a->a->type].kind == AT_DICT) {
        /* Like an ordinary subscript store, this can recreate a key removed
         * by the RHS. The initial read must still reject an absent key. */
        at_ir_dict_store(g, node->a, place->owner, place->index, result);
    } else {
        Val address = locate_place(g, place, 1);
        if (buffer_byte) {
            at_ir_buffer_write(g, node, address, result);
        } else {
            at_ir_emit(g, "  store %s %s, ptr %s%s\n", at_ir_type(g, node->type), result.text,
                       address.text, alignment);
        }
    }
    at_ir_class_field_initialized(g, node->a);
    free(storage);
}

void at_ir_store_target(Gen *g, AtNode *node, Val value)
{
    if (node->a->kind == AN_FIELD && g->p->types[node->a->a->type].kind == AT_LAYOUT) {
        at_ir_layout_store(g, node->a, value);
        return;
    }
    if (node->a->kind == AN_NAME || node->a->kind == AN_GLOBAL) {
        at_ir_store_binding(g, node->a, value);
        return;
    }
    /* Plain assignment evaluates its RHS before reaching this function. Its
     * target indices can still resize an inline ancestor, so share the same
     * captured-place walk instead of retaining an interior element address. */
    AssignmentPlace *storage = allocate_places(g, node);
    if (!storage) {
        return;
    }
    int used = 0;
    AssignmentPlace *place = capture_place(g, node->a, storage, &used);
    const char *alignment =
        node->a->kind == AN_INDEX && g->p->types[node->a->a->type].kind == AT_POINTER ? ", align 1"
                                                                                      : "";
    if (node->a->kind == AN_INDEX && node->a->a->type == AT_REGION) {
        /* Region indexes are i64 values over byte storage, not i64 cells. */
        at_ir_buffer_write(g, node, place->initial_address, value);
    } else {
        at_ir_emit(g, "  store %s %s, ptr %s%s\n", at_ir_type(g, node->type), value.text,
                   place->initial_address.text, alignment);
    }
    at_ir_class_field_initialized(g, node->a);
    free(storage);
}
