/* HTML constraint validation, separated from editing/painting because a
 * validity query is a read of live control state, not an edit and not a form
 * serialization. Spec: form-control-infrastructure.html#constraints.
 *
 * Scope boundary: temporal input states still use this browser's text fallback;
 * this file does not invent date parsing, a calendar, IDN email editing, or a
 * Unicode-sets regex implementation. URL and pattern use the real existing
 * engines through explicit callbacks. A realm without RegExp 'v' support
 * leaves pattern validation unsupported instead of changing its language. */
#include "dom/validation.h"
#include "dom/forms.h"
#include "dom/focus.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

static fc_url_validator_fn url_validator;
static fc_pattern_validator_fn pattern_validator;
static fc_validation_report_fn validation_reporter;

void fc_set_url_validator(fc_url_validator_fn fn) { url_validator = fn; }
void fc_set_pattern_validator(fc_pattern_validator_fn fn) { pattern_validator = fn; }
void fc_set_validation_reporter(fc_validation_report_fn fn) { validation_reporter = fn; }

int fc_ascii_space(char c)
{ return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static struct node *tree_root(struct node *n)
{
    while (n && n->parent && !dom_is_shadow_root(n)) n = n->parent;
    return n;
}

int fc_connected(struct node *n)
{
    if (!n || !n->doc) return 0;
    struct node *root = n;
    while (root->parent) root = root->parent;
    return root == dom_doc_root(n->doc);
}

/* Iterative tree order includes the root itself, so a detached form/radio
 * subtree is tested too. No fixed depth stack may silently discard controls. */
static struct node *tree_next(struct node *n, struct node *root)
{
    if (n->first_child) return n->first_child;
    while (n != root && !n->next) n = n->parent;
    return n == root ? 0 : n->next;
}

static int decimal_number(const char *value, int len, double *number)
{
    char *copy = malloc((size_t)len + 1);
    if (!copy) return 0;
    memcpy(copy, value, (size_t)len); copy[len] = 0;
    double parsed = strtod(copy, 0);
    free(copy);
    if (!isfinite(parsed)) return 0;
    if (number) *number = parsed;
    return 1;
}

int fc_parse_number(const char *value, int len, double *number)
{
    if (!value || !len) return 0;
    if (len < 0) len = (int)strlen(value);
    int i = value[0] == '-' ? 1 : 0, digits = 0;
    while (i < len && value[i] >= '0' && value[i] <= '9') { i++; digits++; }
    if (i < len && value[i] == '.') {
        int start = ++i;
        while (i < len && value[i] >= '0' && value[i] <= '9') i++;
        if (i == start) return 0;
        digits += i - start;
    }
    if (!digits) return 0;
    if (i < len && (value[i] == 'e' || value[i] == 'E')) {
        i++;
        if (i < len && (value[i] == '+' || value[i] == '-')) i++;
        int start = i;
        while (i < len && value[i] >= '0' && value[i] <= '9') i++;
        if (i == start) return 0;
    }
    if (i != len) return 0;
    /* strtod accepts whitespace, +, hex, Infinity and trailing garbage. Its
     * permissiveness must not silently become input[type=number]'s grammar. */
    return decimal_number(value, len, number);
}

int fc_value_length(const char *value, int len)
{
    int units = 0;
    for (int i = 0; value && i < len;) {
        unsigned char c = (unsigned char)value[i];
        if (c == '\r' && i + 1 < len && value[i + 1] == '\n') { i++; continue; }
        int width = c >= 0xf0 && c < 0xf8 ? 4 : c >= 0xe0 && c < 0xf0 ? 3 :
                    c >= 0xc0 && c < 0xe0 ? 2 : 1;
        if (i + width > len) width = 1;
        for (int j = 1; j < width; j++)
            if (((unsigned char)value[i + j] & 0xc0) != 0x80) { width = 1; break; }
        units += width == 4 ? 2 : 1;
        i += width;
    }
    return units;
}

int fc_length_applies(struct node *n)
{
    int kind = fc_kind(n);
    return (kind >= FC_TEXT && kind <= FC_TEL) || kind == FC_TEXTAREA;
}

int fc_length_limit(struct node *n, const char *attribute)
{
    const char *v = dom_attr(n, attribute);
    if (!v) return -1;
    while (fc_ascii_space(*v)) v++;
    if (*v == '+') v++;
    if (*v < '0' || *v > '9') return -1;
    int result = 0;
    while (*v >= '0' && *v <= '9') {
        if (result > (INT_MAX - (*v - '0')) / 10) return INT_MAX;
        result = result * 10 + (*v++ - '0');
    }
    return result;
}

int fc_will_validate(struct node *n)
{
    int kind = fc_kind(n);
    if (!kind || kind == FC_HIDDEN || kind == FC_RESET || kind == FC_BUTTON ||
        fc_disabled(n) || fc_readonly(n)) return 0;
    for (struct node *p = n->parent; p && !dom_is_shadow_root(p); p = p->parent)
        if (p->type == N_ELEM && p->ns == NS_HTML && fc__tag(p->tag, "datalist")) return 0;
    return 1;
}

static int radio_missing(struct node *radio)
{
    const char *name = dom_attr(radio, "name");
    struct node *form = fc_form_of(radio), *root = tree_root(radio);
    int required = dom_attr(radio, "required") != 0, checked = fc_checked(radio);
    if (!name || !name[0]) return required && !checked;
    for (struct node *n = root; n; n = tree_next(n, root)) {
        if (n == radio || fc_kind(n) != FC_RADIO || fc_form_of(n) != form) continue;
        const char *peer = dom_attr(n, "name");
        if (!peer || strcmp(peer, name)) continue;
        if (dom_attr(n, "required")) required = 1;
        if (fc_checked(n)) checked = 1;
    }
    return required && !checked;
}

static int select_missing(struct node *select)
{
    int count = fc_option_count(select), index = fc_selected_index(select);
    int multiple = dom_attr(select, "multiple") != 0;
    const char *size = dom_attr(select, "size");
    int placeholder = !multiple && (!size || atoi(size) <= 1);
    for (int i = 0; i < count; i++) {
        struct node *option = fc_option_at(select, i);
        int selected = multiple ? fc_option_selected(option) : i == index;
        if (!selected) continue;
        /* Only the FIRST option, directly under a single-line select, can be
         * its placeholder label option. A later empty-valued option is valid. */
        const struct dom_attr *attribute = dom_find_attr(option, "value");
        int empty = attribute ? attribute->vlen == 0 : 0;
        if (!attribute) {
            char first[2];
            fc_option_value(option, first, sizeof first);
            empty = first[0] == 0;
        }
        if (!(placeholder && i == 0 && option->parent == select && empty)) return 0;
    }
    return 1;
}

static int ascii_alnum(char c)
{ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

static int email_address(const char *value, int len)
{
    int at = 0;
    while (at < len && value[at] != '@') {
        char c = value[at];
        if (!ascii_alnum(c) && !strchr(".!#$%&'*+-/=?^_`{|}~", c)) return 0;
        if (!c) return 0;
        at++;
    }
    if (!at || at == len) return 0;
    int start = at + 1;
    for (int i = start; i <= len; i++) {
        if (i < len && value[i] != '.') {
            if (!ascii_alnum(value[i]) && value[i] != '-') return 0;
            continue;
        }
        int length = i - start;
        if (!length || length > 63 || !ascii_alnum(value[start]) ||
            !ascii_alnum(value[i - 1])) return 0;
        start = i + 1;
    }
    return 1;
}

static int email_value(struct node *n, const char *value, int len)
{
    if (!len) return 1;
    if (!dom_attr(n, "multiple")) return email_address(value, len);
    int start = 0;
    for (int i = 0; i <= len; i++) {
        if (i < len && value[i] != ',') continue;
        int a = start, b = i;
        while (a < b && fc_ascii_space(value[a])) a++;
        while (b > a && fc_ascii_space(value[b - 1])) b--;
        if (!email_address(value + a, b - a)) return 0;
        start = i + 1;
    }
    return 1;
}

static int attribute_number(struct node *n, const char *name, double *out)
{
    const char *value = dom_attr(n, name);
    if (!value) return 0;
    while (fc_ascii_space(*value)) value++;
    if (*value == '+') value++;
    int i = value[0] == '-' ? 1 : 0, digits = 0;
    while (value[i] >= '0' && value[i] <= '9') { i++; digits++; }
    int end = i;
    if (value[i] == '.') {
        int first = ++i;
        while (value[i] >= '0' && value[i] <= '9') { i++; digits++; }
        if (i > first) end = i;
    }
    if (!digits) return 0;
    if (value[i] == 'e' || value[i] == 'E') {
        i++;
        if (value[i] == '+' || value[i] == '-') i++;
        int first = i;
        while (value[i] >= '0' && value[i] <= '9') i++;
        if (i > first) end = i;
    }
    /* This is the permissive HTML attribute parser, distinct from the strict
     * value sanitization grammar: min=" +1junk" is parsed as 1; assigning
     * input.value=" +1junk" must still sanitize to the empty string. */
    return decimal_number(value, end, out);
}

static int matches_pattern(struct node *n, const char *value, int len)
{
    if (fc_kind(n) != FC_EMAIL || !dom_attr(n, "multiple"))
        return pattern_validator(n, value, len);
    int start = 0;
    for (int i = 0; i <= len; i++) {
        if (i < len && value[i] != ',') continue;
        int answer = pattern_validator(n, value + start, i - start);
        if (answer <= 0) return answer;
        start = i + 1;
    }
    return 1;
}

unsigned fc_validity(struct node *n)
{
    unsigned flags = 0;
    int custom_len = 0;
    fc_custom_validity(n, &custom_len);
    if (custom_len) flags |= FC_CUSTOM_ERROR;
    int kind = fc_kind(n), len = 0;
    if (!kind) return flags;
    const char *value = fc_value(n, &len);
    if (fc_bad_input(n)) flags |= FC_BAD_INPUT;
    if (kind == FC_RADIO) {
        if (radio_missing(n)) flags |= FC_VALUE_MISSING;
    } else if (dom_attr(n, "required")) {
        if ((kind == FC_CHECKBOX && !fc_checked(n)) || kind == FC_FILE ||
            (kind == FC_SELECT && select_missing(n)) ||
            (FC_IS_TEXTUAL(kind) && !len && !fc_disabled(n) && !fc_readonly(n)))
            flags |= FC_VALUE_MISSING;
    }
    if (kind == FC_EMAIL && !email_value(n, value, len)) flags |= FC_TYPE_MISMATCH;
    if (kind == FC_URL && len && url_validator && url_validator(value, len) == 0)
        flags |= FC_TYPE_MISMATCH;
    if (fc_length_applies(n)) {
        if (len && kind != FC_TEXTAREA && dom_attr(n, "pattern") && pattern_validator &&
            matches_pattern(n, value, len) == 0) flags |= FC_PATTERN_MISMATCH;
        if (fc_user_edited(n)) {
            int units = fc_value_length(value, len);
            int min = fc_length_limit(n, "minlength"), max = fc_length_limit(n, "maxlength");
            if (max >= 0 && units > max) flags |= FC_TOO_LONG;
            if (min >= 0 && len && units < min) flags |= FC_TOO_SHORT;
        }
    }
    if (kind == FC_NUMBER && len) {
        double number, min = 0, max = 0, step = 1, base = 0;
        if (fc_parse_number(value, len, &number)) {
            int has_min = attribute_number(n, "min", &min);
            if (has_min && number < min) flags |= FC_RANGE_UNDERFLOW;
            if (attribute_number(n, "max", &max) && number > max) flags |= FC_RANGE_OVERFLOW;
            const char *step_text = dom_attr(n, "step");
            if (!fc__ieq(step_text, "any")) {
                if (!attribute_number(n, "step", &step) || step <= 0) step = 1;
                if (has_min) base = min;
                else attribute_number(n, "value", &base);
                double remainder = fmod(fmod(number, step) - fmod(base, step), step);
                /* Decimal step=.1 has no exact binary representation. Comparing
                 * fmod to zero rejects ordinary .3; allow only accumulated
                 * double-rounding error, not a fixed fraction of every step. */
                double tolerance = 8 * 2.2204460492503131e-16 * fabs(number) +
                                   8 * 2.2204460492503131e-16 * fabs(base) +
                                   8 * 2.2204460492503131e-16 * fabs(step);
                double distance = fabs(remainder);
                if (fabs(step) - distance < distance) distance = fabs(step) - distance;
                if (distance > tolerance) flags |= FC_STEP_MISMATCH;
            }
        }
    }
#ifdef FORM_VALIDATION_IGNORE_ERRORS
    /* The semantic gate builds this control to prove that ignored constraints
     * turn the same submission consumers red, not merely a getter's shape. */
    return 0;
#else
    return flags;
#endif
}

const char *fc_validation_message(struct node *n, int *len)
{
    const char *message = "";
    unsigned flags = fc_validity(n);
    if (fc_will_validate(n) && flags) {
        if (flags & FC_CUSTOM_ERROR) return fc_custom_validity(n, len);
        if (flags & FC_BAD_INPUT) message = "Please enter a number.";
        else if (flags & FC_VALUE_MISSING) message = "Please fill out this field.";
        else if (flags & FC_TYPE_MISMATCH) message = fc_kind(n) == FC_EMAIL ?
            "Please enter an email address." : "Please enter an absolute URL.";
        else if (flags & FC_PATTERN_MISMATCH) message = "Please match the requested format.";
        else if (flags & FC_TOO_LONG) message = "Please shorten this text.";
        else if (flags & FC_TOO_SHORT) message = "Please lengthen this text.";
        else if (flags & FC_RANGE_UNDERFLOW) message = "The number is below the minimum.";
        else if (flags & FC_RANGE_OVERFLOW) message = "The number exceeds the maximum.";
        else if (flags & FC_STEP_MISMATCH) message = "Please enter a value allowed by the step.";
    }
    if (len) *len = (int)strlen(message);
    return message;
}

static void report_control(struct node *n)
{
    const char *message = fc_validation_message(n, 0);
    if (!message[0]) return;
    if (validation_reporter) validation_reporter(n, message);
    else if (fc_connected(n) && focus_is_focusable(n)) { focus_set(n); fc_mark_focus(n); }
}

int fc_check_validity(struct node *n, int report)
{
    if (!fc_will_validate(n) || !fc_validity(n)) return 1;
    uint32_t serial = n->serial;
    int unhandled = fc_dispatch(n, "invalid", 0, 1);
    if (report && unhandled && n->serial == serial) report_control(n);
    return 0;
}

struct invalid_control { struct node *node; uint32_t serial; };

int fc_validate_form(struct node *form, int report)
{
    struct node *root = tree_root(form);
    struct invalid_control *invalid = 0;
    int count = 0, capacity = 0;
    if (!root) return 1;
    for (struct node *n = root; n; n = tree_next(n, root)) {
        if (!fc_kind(n) || fc_form_of(n) != form || !fc_will_validate(n) || !fc_validity(n)) continue;
        if (count == capacity) {
            int next = capacity ? capacity * 2 : 8;
            void *grown = realloc(invalid, (size_t)next * sizeof *invalid);
            if (!grown) { free(invalid); return 0; }
            invalid = grown; capacity = next;
        }
        invalid[count++] = (struct invalid_control){ n, n->serial };
    }
    /* Snapshot BEFORE firing invalid. Otherwise the first handler can remove
     * or repair a later field and change both the event set and the result.
     * Serial checks protect against handlers recycling a node's arena slot. */
    struct invalid_control first_unhandled = { 0, 0 };
    for (int i = 0; i < count; i++) {
        struct node *n = invalid[i].node;
        if (n->serial != invalid[i].serial) continue;
        int unhandled = fc_dispatch(n, "invalid", 0, 1);
        if (unhandled && !first_unhandled.node && n->serial == invalid[i].serial)
            first_unhandled = invalid[i];
    }
    if (report && first_unhandled.node && first_unhandled.node->serial == first_unhandled.serial)
        report_control(first_unhandled.node);
    free(invalid);
    return count == 0;
}

int fc_implicit_submit(struct node *control)
{
    struct node *form = fc_form_of(control), *root = tree_root(form), *button = 0;
    if (!form || !root || fc_disabled(control)) return 0;
    int blocking = 0;
    for (struct node *n = root; n; n = tree_next(n, root)) {
        if (fc_form_of(n) != form) continue;
        int kind = fc_kind(n);
        if (!button && (kind == FC_SUBMIT || kind == FC_IMAGEBTN)) button = n;
        if (n->type == N_ELEM && fc__tag(n->tag, "input") &&
            kind >= FC_TEXT && kind <= FC_NUMBER) blocking++;
    }
    if (!button) return blocking > 1 ? 0 : fc_submit(form, 0, 1);
    if (fc_disabled(button)) return 0;
    uint32_t serial = button->serial, form_serial = form->serial;
    if (!fc_dispatch(button, "click", 1, 1)) return 0;
    if (button->serial != serial || form->serial != form_serial ||
        fc_disabled(button) || fc_form_of(button) != form) return 0;
    return fc_submit(form, button, 1);
}
