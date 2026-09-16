/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"

static void release_list(Gen *generator, AtNode *node);

void at_ir_release_temporaries(Gen *generator, AtNode *node)
{
    if (!node || node->function != generator->f - generator->p->functions) {
        return;
    }

    /* Expression slots used to remain populated until function return. Six
     * distinct image.decode assignments then retained six 5 MiB buffers even
     * after overwriting the only source owner, exhausting the guest arena.
     *
     * Clear only the completed statement's subtree. Clearing all function
     * temporaries here would also drop an enclosing for-loop's live iterable
     * or a caller's still-needed argument. Local/captured/global owner slots
     * are independent and remain registered. These stores never collect. */
    if (at_ir_references(generator, node->type)) {
        at_ir_emit(generator, "  store %s zeroinitializer, ptr %%rootvalue%d\n",
                   at_ir_type(generator, node->type), node->id);
    }

    int original_kind = generator->p->types[node->value_type].kind;
    if ((node->kind == AN_ARRAY && original_kind == AT_LIST) || node->kind == AN_COMPREHENSION ||
        node->kind == AN_DICT || (node->kind == AN_CALL && node->symbol == AT_CALL_CLASS)) {
        at_ir_emit(generator, "  store ptr null, ptr %%construction%d\n", node->id);
    }
    if ((node->kind == AN_FOR || node->kind == AN_COMPREHENSION) && node->b &&
        generator->p->types[node->b->type].kind == AT_DICT) {
        at_ir_emit(generator, "  store ptr null, ptr %%snapshot%d\n", node->id);
    }
    if (node->kind == AN_EXCEPT) {
        at_ir_emit(generator, "  store %s zeroinitializer, ptr %%caught%d\n",
                   at_ir_type(generator, generator->p->exception_type), node->id);
    }

    release_list(generator, node->a);
    release_list(generator, node->b);
    release_list(generator, node->c);
    for (int index = 0; index < node->count; index++) {
        at_ir_release_temporaries(generator, node->args[index]);
    }
}

static void release_list(Gen *generator, AtNode *node)
{
    for (; node; node = node->next) {
        at_ir_release_temporaries(generator, node);
    }
}
