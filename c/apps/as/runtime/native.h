/* SPDX-License-Identifier: MIT */
#ifndef AS_NATIVE_RUNTIME_H
#define AS_NATIVE_RUNTIME_H
#include "exception.h"
#include "type.h"
#include "buffer.h"
#include "capability.h"
#include "file.h"
#include <stddef.h>
#include <stdint.h>

/* Generated scanners describe native layout, including references nested in
 * value aggregates. Scalars are never boxed for the collector. */
typedef void (*AtScan)(void *);

typedef struct AtRoot {
    struct AtRoot *previous;
    void *slot;
    AtScan scan;
} AtRoot;

/* Stable list identity, separately managed backing storage. count bounds the
 * initialized elements; scanners must never inspect spare capacity. */
typedef struct {
    int64_t count;
    int64_t capacity;
    int64_t stride;
    AtScan element_scan;
    unsigned char *data;
} AtList;

typedef struct AtAny AtAny;
AtAny *at_any_new(int type, int64_t bytes, AtScan scan, const void *value,
                  const AtNativeType *description);
void *at_any_data(AtAny *box, int type);
const AtNativeType *at_any_type(AtAny *box);
const void *at_any_value(AtAny *box);
/* Equal values return 1, unequal values 0; -1 reports workspace allocation
 * failure. Type identity is exact, including integer width and signedness. */
int at_any_equal(AtAny *left, AtAny *right);

/* Hash/equality callbacks are generated for supported key types and cannot
 * allocate. Key/value scanners describe their independent native layouts. */
typedef uint64_t (*AtHash)(const void *);
typedef int (*AtEqual)(const void *, const void *);
typedef struct AtDict AtDict;
AtDict *at_dict_new(int64_t key_bytes, int64_t value_bytes, AtScan key_scan, AtScan value_scan,
                    AtHash hash, AtEqual equal);
int64_t at_dict_len(AtDict *dict);
void *at_dict_get(AtDict *dict, const void *key);
int at_dict_set(AtDict *dict, const void *key, const void *value);
int at_dict_remove(AtDict *dict, const void *key);
AtList *at_dict_keys(AtDict *dict);
AtList *at_dict_values(AtDict *dict);
/* Visitors must not allocate through the GC or mutate the table. */
typedef int (*AtDictVisit)(void *context, const void *key, const void *value);
int at_dict_visit(AtDict *dict, AtDictVisit visit, void *context);
uint64_t at_hash_u64(uint64_t value);
uint64_t at_hash_text(const char *text, int64_t length);

/* Heap API: only allocate/collect are safepoints. Register roots before either
 * and restore the entry frame on every exit, including failure. */
void *at_gc_frame(void);
void at_gc_root(AtRoot *root, void *slot, AtScan scan);
void at_gc_restore(void *frame);
void at_gc_mark(void *pointer);
void at_gc_collect(void);
int64_t at_gc_live_bytes(void);
int64_t at_gc_live_objects(void);
int64_t at_gc_reclaim(void);
void *at_gc_allocate(size_t bytes, AtScan scan);
void *at_object_new(int64_t bytes, AtScan scan);
void at_class_prepare(void *object, const void *methods, int64_t required);
void at_class_field_initialized(void *object, int32_t field);
int32_t at_class_ready(const void *object);

/* Environments retain cell identities. A cell uses at_object_new with the
 * concrete value scanner; changing its payload is visible through every alias. */
void *at_closure_new(int64_t count, void *const *cells);
void *at_closure_cell(void *environment, int64_t index);

AtList *at_list_new(int64_t stride, AtScan element_scan);
int64_t at_list_len(AtList *list);
void *at_list_at(AtList *list, int64_t index);
int at_list_append(AtList *list, const void *value);

/* The C entry frame owns argv for the process lifetime. args() returns fresh
 * managed copies, so changing a returned list cannot mutate future calls. */
void at_process_init(int argc, char **argv);
AtList *at_process_args(void);

/* Immutable lazy ranges keep identity without materializing their elements.
 * A negative length means the mathematical count cannot fit language i64. */
typedef struct AtRange AtRange;
AtRange *at_range_new(int64_t start, int64_t stop, int64_t step);
int64_t at_range_bound(AtRange *range, int bound);
int64_t at_range_len(AtRange *range);
int at_range_at(int64_t *out, AtRange *range, int64_t index);
int at_range_contains(AtRange *range, int64_t item);

int at_format_i64(AtNativeText *out, int64_t value);
int at_format_u64(AtNativeText *out, uint64_t value);
int at_format_f64(AtNativeText *out, double value);
void at_format_bool(AtNativeText *out, int value);
int at_text_chr(AtNativeText *out, int64_t value);
int at_parse_int(int64_t *out, const char *text, int64_t length);
int at_parse_float(double *out, const char *text, int64_t length);

/* Output pointers avoid platform-specific C aggregate return conventions.
 * Allocating text helpers return zero on failure; generated callers attach
 * the language source location and dispatch MemoryError. */
int at_text_concat(AtNativeText *out, const char *left, int64_t left_length, const char *right,
                   int64_t right_length);
int at_text_repeat(AtNativeText *out, const char *text, int64_t length, int64_t count);
void at_text_slice(AtNativeText *out, const char *text, int64_t length, int64_t start,
                   int64_t stop);
int at_text_join(AtNativeText *out, const char *separator, int64_t separator_length, AtList *parts);
void at_text_strip(AtNativeText *out, const char *text, int64_t length);
int at_text_case(AtNativeText *out, const char *text, int64_t length, int upper);
int at_text_split(AtList **out, const char *text, int64_t length, const char *separator,
                  int64_t separator_length);
int at_text_replace(AtNativeText *out, const char *text, int64_t length, const char *old,
                    int64_t old_length, const char *replacement, int64_t replacement_length);
int at_text_compare(const char *left, int64_t left_length, const char *right, int64_t right_length);
int64_t at_text_find(const char *text, int64_t length, const char *needle, int64_t needle_length);

/* Pending exceptions are runtime roots while a callee frame is unwound. Heap
 * code calls this hook instead of depending on the exception record's fields. */
extern int at_failed;
void at_exception_mark(void);
const char *at_exception_name(int64_t code);
void at_exception_make(AtNativeException *out, int code, const char *message, int64_t length);
void at_exception_throw(const AtNativeException *error, const char *path, int line, int column);
void at_exception_take(AtNativeException *out);
int at_exception_kind(void);
void at_raise(int kind, const char *path, int line, int column);
int at_finish(void);

void at_print_i64(int64_t value);
void at_print_u64(uint64_t value);
void at_print_f64(double value);
void at_print_str(const char *text, int64_t length);
void at_print_error(const AtNativeException *error);
void at_print_bool(int value);
void at_print_sep(int newline);
#endif
