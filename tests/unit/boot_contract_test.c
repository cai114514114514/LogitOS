/*
 * Host-side source gate for the boot contract shared by every loader.
 *
 * This test reads the assembly, linker script, and kernel consumers instead of
 * restating their answers in a second header.  The comparison scan deliberately
 * recognizes the shapes used by today's bounded Multiboot2 walks:
 *
 *     tag->type == CONSTANT       in pmm_init() and fb_init()
 *     type == CONSTANT            in rsdp_from_mb2()
 *
 * It also requires the two direct mb_info handoffs in kernel_main().  Replacing
 * a comparison with a switch, helper predicate, lookup table, renamed local, or
 * moving a consumer outside those named functions can make a text scan miss a
 * real consumer.  The per-file match floors make replacement of today's walks
 * fail closed; an edit introducing a new shape must extend this scanner and its
 * floor in the same patch.  In particular, do not weaken a floor to accommodate
 * a refactor: teach the gate the new source shape and first watch its control go
 * red.  This narrow parser is preferable to including kernel headers because a
 * host compile would then test a parallel declaration, not the sources a loader
 * must actually follow.
 */

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static unsigned failures;

static void failf(const char *fmt, ...)
{
    va_list ap;
    failures++;
    fputs("FAIL: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static char *read_source(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        failf("cannot open %s: %s", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        failf("cannot seek %s", path);
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        failf("cannot size %s", path);
        fclose(f);
        return NULL;
    }
    char *text = malloc((size_t)n + 1);
    if (!text) {
        failf("out of memory reading %s", path);
        fclose(f);
        return NULL;
    }
    size_t got = fread(text, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) {
        failf("short read from %s", path);
        free(text);
        return NULL;
    }
    text[n] = '\0';
    return text;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

struct symbol {
    char name[64];
    uint64_t value;
};

struct symbols {
    struct symbol item[64];
    size_t count;
};

static int symbol_get(const struct symbols *syms, const char *name, uint64_t *value)
{
    for (size_t i = 0; i < syms->count; i++) {
        if (strcmp(syms->item[i].name, name) == 0) {
            *value = syms->item[i].value;
            return 1;
        }
    }
    return 0;
}

static int symbol_put(struct symbols *syms, const char *name, uint64_t value)
{
    for (size_t i = 0; i < syms->count; i++) {
        if (strcmp(syms->item[i].name, name) == 0) {
            syms->item[i].value = value;
            return 1;
        }
    }
    if (syms->count == ARRAY_LEN(syms->item) || strlen(name) >= sizeof(syms->item[0].name))
        return 0;
    strcpy(syms->item[syms->count].name, name);
    syms->item[syms->count].value = value;
    syms->count++;
    return 1;
}

struct expr_parser {
    const char *p;
    const struct symbols *syms;
    int ok;
};

static void expr_space(struct expr_parser *ep)
{
    while (isspace((unsigned char)*ep->p)) ep->p++;
}

static uint64_t expr_sum(struct expr_parser *ep);

static uint64_t expr_primary(struct expr_parser *ep)
{
    expr_space(ep);
    if (*ep->p == '(') {
        ep->p++;
        uint64_t value = expr_sum(ep);
        expr_space(ep);
        if (*ep->p != ')') ep->ok = 0;
        else ep->p++;
        return value;
    }
    if (*ep->p == '+' || *ep->p == '-') {
        int negative = *ep->p++ == '-';
        uint64_t value = expr_primary(ep);
        return negative ? (uint64_t)(0 - value) : value;
    }
    if (isdigit((unsigned char)*ep->p)) {
        char *end;
        errno = 0;
        uint64_t value = strtoull(ep->p, &end, 0);
        if (errno || end == ep->p) ep->ok = 0;
        ep->p = end;
        while (*ep->p == 'u' || *ep->p == 'U' || *ep->p == 'l' || *ep->p == 'L') ep->p++;
        return value;
    }
    if (isalpha((unsigned char)*ep->p) || *ep->p == '_') {
        char name[64];
        size_t n = 0;
        while (isalnum((unsigned char)*ep->p) || *ep->p == '_') {
            if (n + 1 < sizeof(name)) name[n++] = *ep->p;
            ep->p++;
        }
        name[n] = '\0';
        uint64_t value;
        if (!symbol_get(ep->syms, name, &value)) ep->ok = 0;
        return ep->ok ? value : 0;
    }
    ep->ok = 0;
    return 0;
}

static uint64_t expr_sum(struct expr_parser *ep)
{
    uint64_t value = expr_primary(ep);
    for (;;) {
        expr_space(ep);
        char op = *ep->p;
        if (op != '+' && op != '-') break;
        ep->p++;
        uint64_t rhs = expr_primary(ep);
        value = op == '+' ? value + rhs : value - rhs;
    }
    return value;
}

static int eval_expr(const char *text, const struct symbols *syms, uint64_t *value)
{
    struct expr_parser ep = { text, syms, 1 };
    *value = expr_sum(&ep);
    expr_space(&ep);
    return ep.ok && *ep.p == '\0';
}

struct asm_atom {
    uint64_t offset;
    unsigned width;
    char expr[256];
};

static void strip_asm_comment(char *line)
{
    char *semi = strchr(line, ';');
    if (semi) *semi = '\0';
}

static int asm_equates(char *copy, struct symbols *syms, const char *path)
{
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        strip_asm_comment(line);
        char *p = trim(line);
        char name[64], op[16], expr[256];
        if (sscanf(p, "%63s %15s %255[^\n]", name, op, expr) == 3 && strcmp(op, "equ") == 0) {
            uint64_t value;
            char *rhs = trim(expr);
            if (!eval_expr(rhs, syms, &value) || !symbol_put(syms, name, value)) {
                failf("cannot parse equate in %s: %s", path, p);
                return 0;
            }
        }
    }
    return 1;
}

static int add_asm_values(struct asm_atom *atoms, size_t *count, uint64_t *offset,
                          unsigned width, char *values, const char *path)
{
    char *start = values;
    unsigned depth = 0;
    for (char *p = values;; p++) {
        if (*p == '(') depth++;
        else if (*p == ')' && depth) depth--;
        if ((*p == ',' && depth == 0) || *p == '\0') {
            char saved = *p;
            *p = '\0';
            char *expr = trim(start);
            if (!*expr || *count == 128 || strlen(expr) >= sizeof(atoms[0].expr)) {
                failf("unsupported data declaration in %s", path);
                return 0;
            }
            atoms[*count].offset = *offset;
            atoms[*count].width = width;
            strcpy(atoms[*count].expr, expr);
            (*count)++;
            *offset += width;
            if (saved == '\0') break;
            start = p + 1;
        }
    }
    return 1;
}

static int parse_asm_header(const char *text, const char *path, struct symbols *syms,
                            struct asm_atom *atoms, size_t *atom_count)
{
    char *copy = strdup(text);
    char *equ_copy = strdup(text);
    if (!copy || !equ_copy) {
        failf("out of memory parsing %s", path);
        free(copy);
        free(equ_copy);
        return 0;
    }
    if (!asm_equates(equ_copy, syms, path)) {
        free(copy);
        free(equ_copy);
        return 0;
    }
    free(equ_copy);

    int inside = 0;
    uint64_t offset = 0;
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        strip_asm_comment(line);
        char *p = trim(line);
        if (!*p) continue;

        char *colon = strchr(p, ':');
        if (colon) {
            *colon = '\0';
            char *label = trim(p);
            if (strcmp(label, "header_start") == 0) {
                inside = 1;
                offset = 0;
            }
            if (inside && !symbol_put(syms, label, offset)) {
                failf("too many or overlong labels in %s", path);
                free(copy);
                return 0;
            }
            p = trim(colon + 1);
            if (!*p) continue;
        }
        if (!inside) continue;

        char directive[16], rest[512];
        if (sscanf(p, "%15s %511[^\n]", directive, rest) != 2) continue;
        if (strcmp(directive, "align") == 0) {
            uint64_t alignment;
            if (!eval_expr(trim(rest), syms, &alignment) || !alignment || (alignment & (alignment - 1))) {
                failf("invalid header alignment in %s: %s", path, rest);
                free(copy);
                return 0;
            }
            offset = (offset + alignment - 1) & ~(alignment - 1);
            continue;
        }
        unsigned width = strcmp(directive, "db") == 0 ? 1 :
                         strcmp(directive, "dw") == 0 ? 2 :
                         strcmp(directive, "dd") == 0 ? 4 :
                         strcmp(directive, "dq") == 0 ? 8 : 0;
        if (width && !add_asm_values(atoms, atom_count, &offset, width, rest, path)) {
            free(copy);
            return 0;
        }
    }
    free(copy);
    uint64_t ignored;
    if (!symbol_get(syms, "header_start", &ignored) || !symbol_get(syms, "header_end", &ignored)) {
        failf("%s must declare header_start and header_end", path);
        return 0;
    }
    return 1;
}

static void check_multiboot_header(const char *path)
{
    char *text = read_source(path);
    if (!text) return;
    struct symbols syms = {0};
    struct asm_atom atoms[128];
    size_t atom_count = 0;
    if (!parse_asm_header(text, path, &syms, atoms, &atom_count)) {
        free(text);
        return;
    }

    uint64_t magic, arch, start, end;
    if (!symbol_get(&syms, "MB2_MAGIC", &magic) || magic != UINT64_C(0xe85250d6))
        failf("%s must declare MB2_MAGIC equ 0xe85250d6", path);
    if (!symbol_get(&syms, "MB2_ARCH", &arch) || arch != 0)
        failf("%s must declare MB2_ARCH equ 0", path);
    if (!symbol_get(&syms, "header_start", &start) || !symbol_get(&syms, "header_end", &end) || end < start) {
        failf("%s has an invalid Multiboot2 header extent", path);
        free(text);
        return;
    }
    uint64_t length = end - start;
    if (atom_count < 4) {
        failf("%s has only %zu header fields; need magic, architecture, length, checksum", path, atom_count);
        free(text);
        return;
    }

    uint64_t fixed[4] = {0};
    int fixed_ok = 1;
    for (size_t i = 0; i < 4; i++) {
        if (atoms[i].width != 4 || atoms[i].offset != i * 4 ||
            !eval_expr(atoms[i].expr, &syms, &fixed[i])) {
            failf("cannot evaluate fixed Multiboot2 header field %zu in %s", i, path);
            fixed_ok = 0;
        }
    }
    if (fixed_ok) {
        if ((uint32_t)fixed[0] != (uint32_t)magic)
            failf("Multiboot2 header magic field does not use MB2_MAGIC in %s", path);
        if ((uint32_t)fixed[1] != (uint32_t)arch)
            failf("Multiboot2 header architecture field does not use MB2_ARCH in %s", path);
        if ((uint32_t)fixed[2] != (uint32_t)length)
            failf("Multiboot2 header length is %u, but its source extent is %u in %s",
                  (unsigned)fixed[2], (unsigned)length, path);
        uint32_t sum = (uint32_t)fixed[0] + (uint32_t)fixed[1] +
                       (uint32_t)fixed[2] + (uint32_t)fixed[3];
        uint32_t complement = (uint32_t)(0u - (uint32_t)fixed[0] -
                                         (uint32_t)fixed[1] - (uint32_t)fixed[2]);
        if (sum != 0 || (uint32_t)fixed[3] != complement)
            failf("Multiboot2 checksum: magic + architecture + length + checksum = 0x%08x (expected 0x00000000)", sum);
    }

    unsigned fb_tags = 0;
    for (size_t i = 4; i + 1 < atom_count; i++) {
        uint64_t type, flags;
        if (atoms[i].width != 2 || (atoms[i].offset & 7) != 0 ||
            !eval_expr(atoms[i].expr, &syms, &type) || type != 5)
            continue;
        fb_tags++;
        if (atoms[i + 1].width != 2 || atoms[i + 1].offset != atoms[i].offset + 2 ||
            !eval_expr(atoms[i + 1].expr, &syms, &flags)) {
            failf("framebuffer request tag in %s has no readable flags word", path);
        } else if ((flags & 1) == 0) {
            failf("framebuffer request tag in %s is mandatory; flags bit 0 must be set", path);
        }
    }
    if (fb_tags < 1) failf("%s has no aligned Multiboot2 framebuffer request tag (type 5)", path);
    free(text);
}

static char *strip_c_comments_and_literals(const char *source)
{
    size_t n = strlen(source);
    char *out = malloc(n + 1);
    if (!out) return NULL;
    enum { NORMAL, LINE_COMMENT, BLOCK_COMMENT, STRING, CHARACTER } state = NORMAL;
    int escape = 0;
    for (size_t i = 0; i < n; i++) {
        char c = source[i], next = i + 1 < n ? source[i + 1] : '\0';
        out[i] = c == '\n' ? '\n' : ' ';
        if (state == NORMAL) {
            if (c == '/' && next == '/') { state = LINE_COMMENT; i++; out[i] = ' '; }
            else if (c == '/' && next == '*') { state = BLOCK_COMMENT; i++; out[i] = ' '; }
            else if (c == '"') { state = STRING; escape = 0; }
            else if (c == '\'') { state = CHARACTER; escape = 0; }
            else out[i] = c;
        } else if (state == LINE_COMMENT) {
            if (c == '\n') state = NORMAL;
        } else if (state == BLOCK_COMMENT) {
            if (c == '*' && next == '/') { i++; out[i] = ' '; state = NORMAL; }
        } else {
            if (!escape && ((state == STRING && c == '"') || (state == CHARACTER && c == '\'')))
                state = NORMAL;
            escape = !escape && c == '\\';
            if (c != '\\') escape = 0;
        }
    }
    out[n] = '\0';
    return out;
}

struct token {
    char text[128];
};

static size_t c_tokens(const char *text, struct token *tokens, size_t capacity)
{
    size_t count = 0;
    for (const char *p = text; *p;) {
        if (isspace((unsigned char)*p)) { p++; continue; }
        const char *start = p;
        size_t len = 1;
        if (isalpha((unsigned char)*p) || *p == '_') {
            p++;
            while (isalnum((unsigned char)*p) || *p == '_') p++;
            len = (size_t)(p - start);
        } else if (isdigit((unsigned char)*p)) {
            p++;
            while (isalnum((unsigned char)*p) || *p == 'x' || *p == 'X') p++;
            len = (size_t)(p - start);
        } else if ((p[0] == '-' && p[1] == '>') || (p[0] == '=' && p[1] == '=')) {
            p += 2;
            len = 2;
        } else {
            p++;
        }
        if (count < capacity) {
            if (len >= sizeof(tokens[count].text)) len = sizeof(tokens[count].text) - 1;
            memcpy(tokens[count].text, start, len);
            tokens[count].text[len] = '\0';
        }
        count++;
    }
    return count;
}

static char *function_body(const char *clean, const char *name, const char *path)
{
    char needle[160];
    snprintf(needle, sizeof(needle), "%s(", name);
    const char *at = strstr(clean, needle);
    if (!at) {
        failf("cannot find %s() in %s", name, path);
        return NULL;
    }
    const char *open = strchr(at, '{');
    if (!open) {
        failf("cannot find body of %s() in %s", name, path);
        return NULL;
    }
    unsigned depth = 0;
    const char *close = NULL;
    for (const char *p = open; *p; p++) {
        if (*p == '{') depth++;
        else if (*p == '}' && --depth == 0) { close = p; break; }
    }
    if (!close) {
        failf("unterminated body of %s() in %s", name, path);
        return NULL;
    }
    size_t n = (size_t)(close - open + 1);
    char *body = malloc(n + 1);
    if (!body) {
        failf("out of memory extracting %s() from %s", name, path);
        return NULL;
    }
    memcpy(body, open, n);
    body[n] = '\0';
    return body;
}

static void collect_c_defines(const char *clean, struct symbols *syms)
{
    char *copy = strdup(clean);
    if (!copy) return;
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char name[64], value_text[128];
        if (sscanf(line, " # define %63s %127s", name, value_text) == 2 ||
            sscanf(line, " #define %63s %127s", name, value_text) == 2) {
            uint64_t value;
            if (eval_expr(value_text, syms, &value)) symbol_put(syms, name, value);
        }
    }
    free(copy);
}

struct tag_set {
    uint32_t value[64];
    size_t count;
};

static void tag_add(struct tag_set *tags, uint32_t value)
{
    if (value == 0) return;
    for (size_t i = 0; i < tags->count; i++) if (tags->value[i] == value) return;
    if (tags->count == ARRAY_LEN(tags->value)) {
        failf("too many distinct Multiboot2 tag consumers");
        return;
    }
    tags->value[tags->count++] = value;
}

static int token_value(const char *token, const struct symbols *syms, uint32_t *value)
{
    uint64_t wide;
    if (!eval_expr(token, syms, &wide) || wide > UINT32_MAX) return 0;
    *value = (uint32_t)wide;
    return 1;
}

static unsigned scan_tag_comparisons(const char *body, const char *path,
                                     int member_form, unsigned minimum,
                                     struct symbols *syms, struct tag_set *tags)
{
    struct token tokens[8192];
    size_t count = c_tokens(body, tokens, ARRAY_LEN(tokens));
    if (count > ARRAY_LEN(tokens)) {
        failf("token capacity exceeded while scanning %s", path);
        return 0;
    }
    unsigned matches = 0;
    size_t span = member_form ? 4 : 2;
    for (size_t i = 0; i + span < count; i++) {
        int operand = member_form
            ? strcmp(tokens[i].text, "tag") == 0 && strcmp(tokens[i + 1].text, "->") == 0 &&
              strcmp(tokens[i + 2].text, "type") == 0 && strcmp(tokens[i + 3].text, "==") == 0
            : strcmp(tokens[i].text, "type") == 0 && strcmp(tokens[i + 1].text, "==") == 0;
        if (!operand) continue;
        size_t rhs = i + span;
        uint32_t value;
        matches++;
        if (!token_value(tokens[rhs].text, syms, &value))
            failf("unresolved Multiboot2 tag comparison '%s' in %s", tokens[rhs].text, path);
        else
            tag_add(tags, value);
    }
    if (matches < minimum)
        failf("%s yielded %u Multiboot2 tag comparisons; require at least %u", path, matches, minimum);

    for (size_t i = 0; i + 4 < count; i++) {
        if (strcmp(tokens[i].text, "switch") != 0 || strcmp(tokens[i + 1].text, "(") != 0) continue;
        int on_tag_type = member_form
            ? strcmp(tokens[i + 2].text, "tag") == 0 && strcmp(tokens[i + 3].text, "->") == 0 &&
              strcmp(tokens[i + 4].text, "type") == 0
            : strcmp(tokens[i + 2].text, "type") == 0;
        if (on_tag_type) failf("unsupported switch-based Multiboot2 tag scan in %s", path);
    }
    return matches;
}

static void scan_consumer_function(const char *path, const char *function, int member_form,
                                   unsigned minimum, struct tag_set *tags)
{
    char *source = read_source(path);
    if (!source) return;
    char *clean = strip_c_comments_and_literals(source);
    free(source);
    if (!clean) {
        failf("out of memory scanning %s", path);
        return;
    }
    struct symbols syms = {0};
    collect_c_defines(clean, &syms);
    char *body = function_body(clean, function, path);
    if (body) scan_tag_comparisons(body, path, member_form, minimum, &syms, tags);
    free(body);
    free(clean);
}

static void scan_extra_consumer(const char *path, struct tag_set *tags)
{
    char *source = read_source(path);
    if (!source) return;
    char *clean = strip_c_comments_and_literals(source);
    free(source);
    if (!clean) {
        failf("out of memory scanning %s", path);
        return;
    }
    struct symbols syms = {0};
    collect_c_defines(clean, &syms);
    scan_tag_comparisons(clean, path, 1, 1, &syms, tags);
    free(clean);
}

static unsigned count_call(struct token *tokens, size_t count, const char *callee, const char *arg)
{
    unsigned calls = 0;
    for (size_t i = 0; i + 3 < count; i++) {
        if (strcmp(tokens[i].text, callee) == 0 && strcmp(tokens[i + 1].text, "(") == 0 &&
            strcmp(tokens[i + 2].text, arg) == 0 && strcmp(tokens[i + 3].text, ")") == 0)
            calls++;
    }
    return calls;
}

static void check_kmain_handoffs(const char *path)
{
    char *source = read_source(path);
    if (!source) return;
    char *clean = strip_c_comments_and_literals(source);
    free(source);
    if (!clean) {
        failf("out of memory scanning %s", path);
        return;
    }
    char *body = function_body(clean, "kernel_main", path);
    free(clean);
    if (!body) return;
    struct token tokens[4096];
    size_t count = c_tokens(body, tokens, ARRAY_LEN(tokens));
    free(body);
    if (count > ARRAY_LEN(tokens)) {
        failf("token capacity exceeded while scanning %s", path);
        return;
    }
    unsigned pmm = count_call(tokens, count, "pmm_init", "mb_info");
    unsigned acpi = count_call(tokens, count, "acpi_set_mb2_info", "mb_info");
    if (pmm + acpi < 2)
        failf("%s yielded %u direct mb_info handoffs; require at least 2", path, pmm + acpi);
    if (pmm < 1) failf("%s does not hand mb_info directly to pmm_init", path);
    if (acpi < 1) failf("%s does not hand mb_info directly to acpi_set_mb2_info", path);
}

static void check_boot_magic(const char *path)
{
    char *source = read_source(path);
    if (!source) return;
    unsigned immediate_matches = 0;
    int inside_check = 0;
    char *save = NULL;
    for (char *line = strtok_r(source, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        strip_asm_comment(line);
        char *clean = trim(line);
        size_t clean_len = strlen(clean);
        if (clean_len && clean[clean_len - 1] == ':' && clean[0] != '.') {
            clean[clean_len - 1] = '\0';
            inside_check = strcmp(trim(clean), "check_multiboot") == 0;
            continue;
        }
        if (!inside_check) continue;
        char op[32], lhs[32], rhs[128];
        if (sscanf(clean, "%31s %31[^,], %127s", op, lhs, rhs) != 3 ||
            strcmp(op, "cmp") != 0 || strcmp(trim(lhs), "eax") != 0)
            continue;
        struct symbols none = {0};
        uint64_t value;
        if (!eval_expr(rhs, &none, &value)) continue;
        immediate_matches++;
        if (value != UINT64_C(0x36d76289))
            failf("%s compares eax against 0x%llx, not Multiboot2 entry magic 0x36d76289",
                  path, (unsigned long long)value);
    }
    if (immediate_matches < 1)
        failf("%s yielded %u immediate cmp eax matches; require at least 1", path, immediate_matches);
    free(source);
}

static int size_literal(const char *text, uint64_t *value)
{
    while (isspace((unsigned char)*text)) text++;
    char *end;
    errno = 0;
    uint64_t n = strtoull(text, &end, 0);
    if (errno || end == text) return 0;
    uint64_t scale = 1;
    if (*end == 'K' || *end == 'k') { scale = 1024; end++; }
    else if (*end == 'M' || *end == 'm') { scale = 1024 * 1024; end++; }
    else if (*end == 'G' || *end == 'g') { scale = UINT64_C(1024) * 1024 * 1024; end++; }
    while (isspace((unsigned char)*end)) end++;
    if (*end != ';') return 0;
    end++;
    while (isspace((unsigned char)*end)) end++;
    if (*end) return 0;
    *value = n * scale;
    return 1;
}

static void check_link_origin(const char *path)
{
    char *source = read_source(path);
    if (!source) return;
    char *clean = strip_c_comments_and_literals(source);
    free(source);
    if (!clean) {
        failf("out of memory scanning %s", path);
        return;
    }
    char *whole = strdup(clean);
    if (!whole) {
        failf("out of memory verifying _kernel_start placement in %s", path);
        free(clean);
        return;
    }
    unsigned numeric_origins = 0, kernel_starts = 0;
    uint64_t last_origin = 0;
    char *save = NULL;
    for (char *line = strtok_r(clean, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *p = trim(line);
        if (p[0] == '.' && p[1] != '\0') {
            char *equal = strchr(p, '=');
            if (equal) {
                uint64_t value;
                if (size_literal(equal + 1, &value)) {
                    numeric_origins++;
                    last_origin = value;
                    if (value != UINT64_C(32) * 1024 * 1024)
                        failf("%s sets a numeric location-counter origin to %llu bytes, not 32 MiB",
                              path, (unsigned long long)value);
                }
            }
        }
        char compact[256];
        size_t n = 0;
        for (const char *q = p; *q && n + 1 < sizeof(compact); q++)
            if (!isspace((unsigned char)*q)) compact[n++] = *q;
        compact[n] = '\0';
        if (strcmp(compact, "_kernel_start=.;") == 0) {
            kernel_starts++;
            if (last_origin != UINT64_C(32) * 1024 * 1024)
                failf("%s assigns _kernel_start without a preceding 32 MiB origin", path);
        }
    }
    if (numeric_origins < 1) failf("%s yielded %u numeric origin assignments; require at least 1", path, numeric_origins);
    if (kernel_starts < 1) failf("%s yielded %u _kernel_start assignments; require at least 1", path, kernel_starts);

    size_t compact_len = 0;
    for (const char *p = whole; *p; p++) if (!isspace((unsigned char)*p)) compact_len++;
    char *compact_source = malloc(compact_len + 1);
    if (!compact_source) {
        failf("out of memory verifying _kernel_start placement in %s", path);
    } else {
        size_t out = 0;
        for (const char *p = whole; *p; p++)
            if (!isspace((unsigned char)*p)) compact_source[out++] = *p;
        compact_source[out] = '\0';
        if (!strstr(compact_source, ".=32M;_kernel_start=.;"))
            failf("%s must assign _kernel_start directly after the 32M origin", path);
        free(compact_source);
    }
    free(whole);
    free(clean);
}

static int tag_present(const struct tag_set *tags, uint32_t value)
{
    for (size_t i = 0; i < tags->count; i++) if (tags->value[i] == value) return 1;
    return 0;
}

static void check_exact_tag_set(const struct tag_set *tags, const char *extra_path)
{
    static const uint32_t expected[] = {6, 8, 14, 15};
    for (size_t i = 0; i < ARRAY_LEN(expected); i++) {
        if (!tag_present(tags, expected[i]))
            failf("missing Multiboot2 consumer tag %u", expected[i]);
    }
    for (size_t i = 0; i < tags->count; i++) {
        int expected_tag = 0;
        for (size_t j = 0; j < ARRAY_LEN(expected); j++)
            if (tags->value[i] == expected[j]) expected_tag = 1;
        if (!expected_tag)
            failf("unexpected Multiboot2 consumer tag %u%s%s", tags->value[i],
                  extra_path ? " in " : "", extra_path ? extra_path : "");
    }
}

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s [--multiboot PATH] [--extra-consumer PATH]\n", argv0);
}

int main(int argc, char **argv)
{
    const char *multiboot = "c/boot/multiboot2.asm";
    const char *extra_consumer = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--multiboot") == 0 && i + 1 < argc) multiboot = argv[++i];
        else if (strcmp(argv[i], "--extra-consumer") == 0 && i + 1 < argc) extra_consumer = argv[++i];
        else { usage(argv[0]); return 2; }
    }

    check_multiboot_header(multiboot);
    check_boot_magic("c/boot/boot.asm");
    check_link_origin("linker.ld");

    struct tag_set tags = {0};
    scan_consumer_function("c/kernel/mm/pmm.c", "pmm_init", 1, 1, &tags);
    scan_consumer_function("c/kernel/gui/fb.c", "fb_init", 1, 2, &tags);
    scan_consumer_function("c/kernel/cpu/acpi.c", "rsdp_from_mb2", 0, 3, &tags);
    check_kmain_handoffs("c/kernel/core/kmain.c");
    if (extra_consumer) scan_extra_consumer(extra_consumer, &tags);
    check_exact_tag_set(&tags, extra_consumer);

    if (failures) {
        fprintf(stderr, "FAIL: boot contract (%u assertion%s)\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    puts("PASS: boot contract header, entry magic, 32 MiB origin, and consumer tags {6,8,14,15}");
    return 0;
}
