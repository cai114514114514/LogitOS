#include "ols_util.h"
#include <stdlib.h>
#include <string.h>
/* Legacy OLS-IR text assembler. The LSL source frontend is a separate module. */
static int number(const char *s, unsigned *out, int reg)
{
    char *end;
    if (reg) {
        if (*s != 'r')
            return 0;
        s++;
    }
    if (*s < '0' || *s > '9')
        return 0;
    unsigned long n = strtoul(s, &end, 10);
    if (*end || n > 255)
        return 0;
    *out = (unsigned)n;
    return 1;
}
int ol_shader_compile(const char *text, size_t bytes, struct ol_shader_program *p,
                      struct ol_shader_error *e)
{
    if (!text || !p || bytes > 131072)
        return ols_error(e, 0, "source exceeds 128 KiB");
    memset(p, 0, sizeof *p);
    unsigned line = 0;
    size_t at = 0;
    int header = 0;
    static const char *names[] = {"const",   "input", "uniform",  "mov",     "add",    "sub",
                                  "mul",     "mad",   "dot3",     "dot4",    "min",    "max",
                                  "rcp",     "rsqrt", "sin",      "cos",     "select", "tex2d",
                                  "swizzle", "mat4",  "position", "varying", "color"};
    while (at < bytes) {
        line++;
        char buf[512];
        size_t n = 0;
        while (at < bytes && text[at] != '\n') {
            if (n >= sizeof buf - 1)
                return ols_error(e, line, "line exceeds 511 bytes");
            buf[n++] = text[at++];
        }
        if (at < bytes)
            at++;
        buf[n] = 0;
        char *comment = strchr(buf, '#');
        if (comment)
            *comment = 0;
        char *tok[9], *q = buf;
        unsigned nt = 0;
        while (*q) {
            while (*q == ' ' || *q == '\t' || *q == '\r')
                q++;
            if (!*q)
                break;
            if (nt == 9)
                return ols_error(e, line, "too many operands");
            tok[nt++] = q;
            while (*q && *q != ' ' && *q != '\t' && *q != '\r')
                q++;
            if (*q)
                *q++ = 0;
        }
        if (!nt)
            continue;
        if (!header) {
            if (nt != 3 || strcmp(tok[0], "ols") || strcmp(tok[1], "1") ||
                (strcmp(tok[2], "vertex") && strcmp(tok[2], "pixel")))
                return ols_error(e, line, "expected: ols 1 vertex|pixel");
            p->magic = OLS_MAGIC;
            p->version = 1;
            p->stage = !strcmp(tok[2], "vertex") ? OLS_VERTEX : OLS_PIXEL;
            header = 1;
            continue;
        }
        if (p->count == OLS_INSTRUCTIONS)
            return ols_error(e, line, "instruction budget exhausted");
        struct ols_instruction *c = &p->code[p->count++];
        unsigned op = 0;
        while (op <= OLS_COLOR && strcmp(tok[0], names[op]))
            op++;
        if (op > OLS_COLOR)
            return ols_error(e, line, "unknown instruction");
        c->op = op;
        int ok = 1;
        if (op == OLS_CONST) {
            ok = nt == 6 && number(tok[1], &c->dst, 1);
            if (ok)
                for (int k = 0; k < 4; k++) {
                    char *end;
                    c->literal[k] = strtof(tok[k + 2], &end);
                    if (*end || !isfinite(c->literal[k]))
                        ok = 0;
                }
        } else if (op == OLS_POSITION || op == OLS_COLOR)
            ok = nt == 2 && number(tok[1], &c->a, 1);
        else if (op == OLS_VARYING)
            ok = nt == 3 && number(tok[1], &c->dst, 0) && number(tok[2], &c->a, 1);
        else if (op == OLS_INPUT || op == OLS_UNIFORM)
            ok = nt == 3 && number(tok[1], &c->dst, 1) && number(tok[2], &c->a, 0);
        else if (op == OLS_MOV || op == OLS_RCP || op == OLS_RSQRT || op == OLS_SIN ||
                 op == OLS_COS)
            ok = nt == 3 && number(tok[1], &c->dst, 1) && number(tok[2], &c->a, 1);
        else if (op == OLS_MAD || op == OLS_SELECT)
            ok = nt == 5 && number(tok[1], &c->dst, 1) && number(tok[2], &c->a, 1) &&
                 number(tok[3], &c->b, 1) && number(tok[4], &c->c, 1);
        else
            ok = nt == 4 && number(tok[1], &c->dst, 1) && number(tok[2], &c->a, 1) &&
                 number(tok[3], &c->b, !(op == OLS_TEX2D || op == OLS_SWIZZLE || op == OLS_MAT4));
        if (!ok)
            return ols_error(e, line, "incorrect operands");
    }
    return ol_shader_validate(p, e);
}
