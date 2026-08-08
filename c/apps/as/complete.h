#ifndef AS_COMPLETE_H
#define AS_COMPLETE_H

/* libcomplete -- semantic autocomplete for AetherScript (Code Studio).
 * A self-contained, dependency-free engine: its own tolerant tokenizer + a
 * single-pass scope/symbol model + last-assignment type table + fuzzy ranking.
 * Compiles freestanding (into the IDE) and natively (host unit tests). */

enum {                       /* candidate kind -> popup icon/color */
    CMP_KEYWORD, CMP_BUILTIN, CMP_LOCAL, CMP_PARAM, CMP_GLOBAL,
    CMP_IMPORT, CMP_MODULE, CMP_FUNC, CMP_CLASS, CMP_METHOD, CMP_FIELD
};

typedef struct {
    char label[48];          /* shown in the popup            */
    char insert[48];         /* inserted on accept            */
    int  kind;               /* CMP_*                         */
    int  score;              /* ranking; higher = better      */
} Completion;

/* Up to `max` ranked candidates for the caret (byte offset) in src[0..len).
 * Never aborts on malformed / mid-edit input. Returns the count. */
int as_complete(const char *src, int len, int caret, Completion *out, int max);

/* The app supplies how to enumerate module names and read a module's source, so
 * the engine stays OS-independent (host tests inject in-memory modules). Either
 * may be NULL (module/member-on-module completion then yields nothing). */
typedef int (*cmp_list_modules_fn)(char names[][48], int max);
typedef int (*cmp_read_module_fn)(const char *name, char *buf, int max); /* len or -1 */
void as_complete_set_providers(cmp_list_modules_fn list, cmp_read_module_fn read);

/* internal token kinds + caret context + value types (exposed so host tests can
 * assert against them; harmless in the app build) */
enum { TK_EOF, TK_IDENT, TK_KW, TK_NUM, TK_STR, TK_DOT, TK_OP, TK_NL };
typedef struct {
    char prefix[48]; char receiver[48]; int after_import; int word_start;
    int in_string;   /* caret inside a plain string / f-string TEXT -> suppress popup */
    int recv_str;    /* receiver is a string LITERAL ("ab".<caret>) -> string methods */
} CmpCtx;
enum { TY_UNKNOWN, TY_LIST, TY_DICT, TY_STR, TY_INT, TY_FLOAT, TY_INSTANCE,
       TY_PORT, TY_PROC };   /* M27 ports */

#ifdef AS_COMPLETE_TEST
/* test-only hooks into the internals */
int    as__lex_test(const char *src, int len, int *kinds, int max);
CmpCtx as__ctx_test(const char *src, int len, int caret);
int    as__typeof_test(const char *src, int len, int caret, const char *var, char *cls, int cmax);
#endif

#endif /* AS_COMPLETE_H */
