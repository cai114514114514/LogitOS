#ifndef LOGIT_FORM_VALIDATION_H
#define LOGIT_FORM_VALIDATION_H

#include "dom/tree.h"

/* Bits are shared with the live JS ValidityState getters, never duplicated as
 * independent booleans in a wrapper. A disabled control can still have an
 * error; candidacy controls checking/reporting, not the existence of state. */
enum {
    FC_VALUE_MISSING = 1u << 0,
    FC_TYPE_MISMATCH = 1u << 1,
    FC_PATTERN_MISMATCH = 1u << 2,
    FC_TOO_LONG = 1u << 3,
    FC_TOO_SHORT = 1u << 4,
    FC_RANGE_UNDERFLOW = 1u << 5,
    FC_RANGE_OVERFLOW = 1u << 6,
    FC_STEP_MISMATCH = 1u << 7,
    FC_BAD_INPUT = 1u << 8,
    FC_CUSTOM_ERROR = 1u << 9
};

int fc_ascii_space(char c);
int fc_connected(struct node *n);
int fc_parse_number(const char *value, int len, double *number);
int fc_value_length(const char *value, int len);
int fc_length_applies(struct node *n);
int fc_length_limit(struct node *n, const char *attribute);
int fc_will_validate(struct node *n);
unsigned fc_validity(struct node *n);
const char *fc_validation_message(struct node *n, int *len);
int fc_check_validity(struct node *n, int report);
int fc_validate_form(struct node *form, int report);

/* The core uses the already implemented WHATWG URL parser and the realm's
 * actual Unicode-sets RegExp compiler through these seams. -1 means the
 * compiler/capability is unavailable, not a successful match. In particular,
 * an old RegExp 'u' mode must never stand in for HTML's 'v' mode. */
typedef int (*fc_url_validator_fn)(const char *value, int len);
typedef int (*fc_pattern_validator_fn)(struct node *n, const char *value, int len);
void fc_set_url_validator(fc_url_validator_fn fn);
void fc_set_pattern_validator(fc_pattern_validator_fn fn);

/* The native chrome can display the message and scroll/focus the control.
 * Without it reportValidity provides a focus cue only; there is no fabricated
 * validation balloon. Canceling invalid suppresses this reporting door. */
typedef void (*fc_validation_report_fn)(struct node *n, const char *message);
void fc_set_validation_reporter(fc_validation_report_fn fn);

#endif
