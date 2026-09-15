#include "lsl_internal.h"

/* Reuse expression temporaries, but keep local registers live until main ends.
 * Instruction count and simultaneously live registers are separate budgets. */
int lsl_allocate_register(struct lsl_parser *parser)
{
    if (parser->failed)
        return -1;

    for (int index = 0; index < OLS_REGISTERS; index++) {
        uint64_t bit = UINT64_C(1) << index;
        if (!(parser->live_registers & bit)) {
            parser->live_registers |= bit;
            return index;
        }
    }
    lsl_fail(parser, "live register budget exhausted");
    return -1;
}

void lsl_release_value(struct lsl_parser *parser, struct lsl_value value)
{
    if (!lsl_is_numeric(value.type) && value.type != LSL_BOOL)
        return;
    if (value.register_index >= 0 && value.register_index < OLS_REGISTERS)
        parser->live_registers &= ~(UINT64_C(1) << value.register_index);
}

static int has_instruction_space(struct lsl_parser *parser, unsigned required)
{
    if (parser->failed)
        return 0;
    if (required > OLS_INSTRUCTIONS - parser->shader->ir.count)
        return lsl_fail(parser, "instruction budget exhausted");
    return 1;
}

struct lsl_value lsl_emit_instruction(struct lsl_parser *parser, int type,
                                    unsigned opcode, unsigned a,
                                    unsigned b, unsigned c)
{
    int needs_padding_mask = type == LSL_VEC2 || type == LSL_VEC3;
    if (!has_instruction_space(parser, needs_padding_mask ? 3 : 1))
        return (struct lsl_value){0};

    int register_index = lsl_allocate_register(parser);
    if (register_index < 0)
        return (struct lsl_value){0};

    struct ol_shader_program *program = &parser->shader->ir;
    program->code[program->count++] = (struct ols_instruction){
        .op = opcode,
        .dst = register_index,
        .a = a,
        .b = b,
        .c = c
    };

    if (needs_padding_mask) {
        /* All short-vector producers must agree that padding is zero. Merely
         * tagging MOV as vec2 leaves a source vec4's z/w alive: later arithmetic
         * can overflow those invisible values and make an otherwise valid
         * expression fail the VM's four-lane finite-result check. */
        int mask_register = lsl_allocate_register(parser);
        if (mask_register < 0)
            return (struct lsl_value){0};
        program->code[program->count++] = (struct ols_instruction){
            .op = OLS_CONST,
            .dst = mask_register,
            .literal = {1, 1, type == LSL_VEC3, 0}
        };
        program->code[program->count++] = (struct ols_instruction){
            .op = OLS_MUL,
            .dst = register_index,
            .a = register_index,
            .b = mask_register
        };
        lsl_release_value(parser, (struct lsl_value){
            .type = LSL_VEC4,
            .register_index = mask_register
        });
    }
    return (struct lsl_value){.type = type, .register_index = register_index};
}

struct lsl_value lsl_emit_vector(struct lsl_parser *parser,
                               float x, float y, float z, float w)
{
    struct lsl_value value = lsl_emit_instruction(parser, LSL_VEC4,
                                                 OLS_CONST, 0, 0, 0);
    if (!parser->failed) {
        struct ol_shader_program *program = &parser->shader->ir;
        float *literal = program->code[program->count - 1].literal;
        literal[0] = x;
        literal[1] = y;
        literal[2] = z;
        literal[3] = w;
    }
    return value;
}

struct lsl_value lsl_emit_scalar(struct lsl_parser *parser, float number)
{
    /* Scalars occupy every lane so scalar/vector arithmetic can use the same
     * component-wise VM instruction without broadcasting at each use. */
    struct lsl_value value = lsl_emit_vector(parser, number, number, number, number);
    if (!parser->failed)
        value.type = LSL_FLOAT;
    return value;
}

void lsl_emit_output(struct lsl_parser *parser, unsigned opcode,
                     unsigned slot, unsigned register_index)
{
    if (!has_instruction_space(parser, 1))
        return;

    struct ol_shader_program *program = &parser->shader->ir;
    program->code[program->count++] = (struct ols_instruction){
        .op = opcode,
        .dst = slot,
        .a = register_index
    };
}
