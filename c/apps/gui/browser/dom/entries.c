/* HTML 4.10.22.4: collect the actual control state, in tree order. This is
 * deliberately not form.elements: that collection excludes image buttons and
 * author code can replace its getter. File selection still has no native
 * broker; its genuine empty state contributes the specified empty File. */
#include "dom/entries.h"
#include "dom/forms.h"
#include "../../../../lib/text/bidi.h"
#include "../../../../../include/weaksym.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

extern int bidi_class(uint32_t cp) LOGIT_WEAK;
LOGIT_WEAK_STUB(bidi_class);

static struct node *next_node(struct node *n, struct node *root)
{
    if (n->first_child) return n->first_child;
    while (n != root && !n->next) n = n->parent;
    return n == root ? NULL : n->next;
}

int fc_entry_add(struct form_entries *list, const char *name, size_t nl,
                     const char *value, size_t vl, int file)
{
    if (nl == SIZE_MAX || vl == SIZE_MAX) return -1;
    char *nm = malloc(nl + 1), *val = malloc(vl + 1);
    if (!nm || !val) { free(nm); free(val); return -1; }
    memcpy(nm, name, nl); nm[nl] = 0;
    memcpy(val, value, vl); val[vl] = 0;
    if (list->count == list->capacity) {
        size_t cap = list->capacity ? list->capacity * 2 : 16;
        if (cap < list->capacity || cap > SIZE_MAX / sizeof *list->items) {
            free(nm); free(val); return -1;
        }
        struct form_entry *items = realloc(list->items, cap * sizeof *items);
        if (!items) { free(nm); free(val); return -1; }
        list->items = items; list->capacity = cap;
    }
    list->items[list->count++] = (struct form_entry){nm, val, nl, vl, file};
    return 0;
}

void fc_entries_free(struct form_entries *list)
{
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].name); free(list->items[i].value);
    }
    free(list->items); memset(list, 0, sizeof *list);
}

static int encoded_byte(char *out, int offset, int max, unsigned char ch)
{
    static const char hex[]="0123456789ABCDEF";
    if ((ch>='A'&&ch<='Z') || (ch>='a'&&ch<='z') ||
        (ch>='0'&&ch<='9') || ch=='*' || ch=='-' || ch=='.' || ch=='_') {
        if (offset>=max) return -1;
        out[offset++]=(char)ch;
    } else if (ch==' ') {
        if (offset>=max) return -1;
        out[offset++]='+';
    } else {
        if (max-offset<3) return -1;
        out[offset++]='%';out[offset++]=hex[ch>>4];out[offset++]=hex[ch&15];
    }
    return offset;
}

static int encoded_string(char *out, int offset, int max, const char *s, size_t length)
{
    /* HTML normalizes every entry name/string value, not just textarea. The
     * older encoder dropped lone CR entirely and preserved CR in other fields. */
    for (size_t i=0;i<length;i++) {
        unsigned char ch=(unsigned char)s[i];
        if (ch=='\r'||ch=='\n') {
            if (ch=='\r'&&i+1<length&&s[i+1]=='\n') i++;
            offset=encoded_byte(out,offset,max,'\r');
            if (offset<0) return -1;
            ch='\n';
        }
        offset=encoded_byte(out,offset,max,ch);
        if (offset<0) return -1;
    }
    return offset;
}

int fc_entries_encode(const struct form_entries *list, char *out, int capacity)
{
    if (!out||capacity<=0) return -1;
    int offset=0,max=capacity-1;
    for (size_t i=0;i<list->count;i++) {
        const struct form_entry *entry=&list->items[i];
        if (i) {if (offset>=max) return -1;out[offset++]='&';}
        offset=encoded_string(out,offset,max,entry->name,entry->name_len);
        if (offset<0||offset>=max) return -1;
        out[offset++]='=';
        offset=encoded_string(out,offset,max,entry->value,entry->value_len);
        if (offset<0) return -1;
    }
    out[offset]=0;return offset;
}

static int ascii_space(unsigned char ch)
{ return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f'; }

static char *option_value(struct node *option, size_t *length)
{
    const char *value = dom_attr(option, "value");
    if (value) {
        *length = strlen(value);
        char *copy = malloc(*length + 1);
        if (copy) memcpy(copy, value, *length + 1);
        return copy;
    }
    /* option.value uses its text, not its label attribute. The previous
     * 512-byte fc_option_value path truncated uploads and used label as data. */
    size_t capacity = 1;
    for (struct node *n = option; n; n = next_node(n, option))
        if (n->type == N_TEXT) {
            if ((size_t)n->textlen > SIZE_MAX - capacity) return NULL;
            capacity += (size_t)n->textlen;
        }
    char *copy = malloc(capacity);
    if (!copy) return NULL;
    size_t count = 0; int pending_space = 0;
    for (struct node *n = option; n; n = next_node(n, option)) {
        if (n->type != N_TEXT) continue;
        for (int i = 0; i < n->textlen; i++) {
            unsigned char ch = (unsigned char)n->text[i];
            if (ascii_space(ch)) { pending_space = count != 0; continue; }
            if (pending_space) copy[count++] = ' ';
            pending_space = 0; copy[count++] = (char)ch;
        }
    }
    copy[count] = 0; *length = count; return copy;
}

static int option_disabled(struct node *option)
{
    return dom_attr(option, "disabled") != NULL ||
        (option->parent && fc__tag(option->parent->tag, "optgroup") &&
         dom_attr(option->parent, "disabled") != NULL);
}

static uint32_t decode_cp(const char **cursor, const char *end)
{
    const unsigned char *s = (const unsigned char *)*cursor;
    unsigned char ch = *s++; uint32_t cp = ch; int extra = 0;
    if (ch >= 0xc2 && ch < 0xe0) { cp = ch & 31; extra = 1; }
    else if (ch >= 0xe0 && ch < 0xf0) { cp = ch & 15; extra = 2; }
    else if (ch >= 0xf0 && ch < 0xf5) { cp = ch & 7; extra = 3; }
    else if (ch >= 0x80) cp = 0xfffd;
    if (extra) {
        if ((const char *)s + extra > end) { cp = 0xfffd; extra = 0; }
        for (int i = 0; i < extra; i++) {
            if ((s[i] & 0xc0) != 0x80) { cp = 0xfffd; extra = 0; break; }
            cp = (cp << 6) | (s[i] & 63);
        }
        s += extra;
    }
    *cursor = (const char *)s; return cp;
}

static int first_strong(const char *value, size_t length)
{
    if (!length) return -1;
    if (!LOGIT_HAVE(bidi_class)) return -2;
    const char *end=value+length;
    while (value<end) {
        int cls=bidi_class(decode_cp(&value,end));
        if (cls==BIDI_L) return 0;
        if (cls==BIDI_R||cls==BIDI_AL) return 1;
    }
    return -1;
}

static int auto_control(struct node *n)
{
    int kind=fc_kind(n);
    return kind==FC_TEXTAREA || (fc__tag(n->tag,"input") &&
        (kind==FC_HIDDEN||kind==FC_TEXT||kind==FC_SEARCH||kind==FC_TEL||
         kind==FC_URL||kind==FC_EMAIL||kind==FC_PASSWORD||kind==FC_SUBMIT||
         kind==FC_RESET||kind==FC_BUTTON));
}

static int auto_direction(struct node *root)
{
    if (auto_control(root)) {
        int length=0;const char *value=fc_value(root,&length);
        int result=first_strong(value,(size_t)length);
        return result==-1?0:result;
    }
    for (struct node *n=root->first_child;n;) {
        const char *dir=n->type==N_ELEM?dom_attr(n,"dir"):NULL;
        int excluded=n->type==N_ELEM && (fc__tag(n->tag,"bdi")||
            fc__tag(n->tag,"script")||fc__tag(n->tag,"style")||
            fc__tag(n->tag,"textarea")||fc__ieq(dir,"ltr")||
            fc__ieq(dir,"rtl")||fc__ieq(dir,"auto"));
        if (n->type==N_TEXT) {
            int result=first_strong(n->text,(size_t)n->textlen);
            if (result!=-1) return result;
        }
        if (!excluded) {n=next_node(n,root);continue;}
        /* An isolated descendant must exclude its WHOLE subtree. Looking
         * only at the text node's own attributes incorrectly includes it. */
        while (n!=root&&!n->next) n=n->parent;
        n=n==root?NULL:n->next;
    }
    return 0;
}

static int direction(struct node *control)
{
    for (struct node *n = control; n; n = n->parent) {
        if (dom_is_shadow_root(n)) return -2; /* host/assigned-node direction bridge pending */
        if (n->ns!=NS_HTML) continue;
        const char *dir = dom_attr(n, "dir");
        if (fc__ieq(dir, "rtl")) return 1;
        if (fc__ieq(dir, "ltr")) return 0;
        /* Use the engine's Unicode classes, not a guessed Hebrew/Arabic
         * range. A reduced host build without them must refuse this case. */
        if (fc__ieq(dir,"auto")||fc__tag(n->tag,"bdi")) return auto_direction(n);
        if (fc_kind(n)==FC_TEL) return 0;
    }
    return 0;
}

static int collect_control(struct form_entries *list, struct node *form,
                           struct node *submitter, struct node *n)
{
    int kind = fc_kind(n);
    if (n->ns != NS_HTML || !kind || fc_form_of(n) != form || fc_disabled(n)) return 0;
    for (struct node *p = n->parent; p; p = p->parent)
        if (fc__tag(p->tag, "datalist")) return 0;
    if (FC_IS_BUTTON(kind) && n != submitter) return 0;
    if (FC_IS_TOGGLE(kind) && !fc_checked(n)) return 0;
    const char *name = dom_attr(n, "name");
    if (kind == FC_IMAGEBTN) {
        /* New FormData(form, image) starts with the image's initial 0,0
         * coordinates. Native click-coordinate capture is still pending. */
        size_t nl = name ? strlen(name) : 0;
        if (nl > SIZE_MAX - 3) return -1;
        char *key = malloc(nl + 3);
        if (!key) return -1;
        if (nl) { memcpy(key, name, nl); key[nl++] = '.'; }
        key[nl] = 'x'; int rc = fc_entry_add(list, key, nl + 1, "0", 1, 0);
        if (!rc) { key[nl] = 'y'; rc = fc_entry_add(list, key, nl + 1, "0", 1, 0); }
        free(key); return rc;
    }
    if (!name || !*name) return 0;
    size_t nl = strlen(name);
    if (kind == FC_SELECT) {
        int count = fc_option_count(n), single = dom_attr(n, "multiple") == NULL;
        int selected = single ? fc_selected_index(n) : -1;
        for (int i = 0; i < count; i++) {
            struct node *option = fc_option_at(n, i);
            if (!option || option_disabled(option) ||
                (single ? i != selected : !fc_option_selected(option))) continue;
            size_t vl = 0; char *value = option_value(option, &vl);
            if (!value) return -1;
            int rc = fc_entry_add(list, name, nl, value, vl, 0);
            free(value); if (rc) return rc;
        }
        return 0;
    }
    int len = 0; const char *value;
    if (FC_IS_TOGGLE(kind) || FC_IS_BUTTON(kind)) {
        value = dom_attr(n, "value");
        if (!value) value = FC_IS_TOGGLE(kind) ? "on" : "";
        len = (int)strlen(value);
    } else if (kind == FC_HIDDEN && fc__ieq(name, "_charset_")) {
        value = "UTF-8"; len = 5;
    } else value = fc_value(n, &len);
    int rc = fc_entry_add(list, name, nl, value, (size_t)len, kind == FC_FILE);
    if (rc) return rc;
    const char *dirname = dom_attr(n, "dirname");
    if (dirname && *dirname && auto_control(n)) {
        int rtl = direction(n);
        if (rtl < 0) return rtl;
        return fc_entry_add(list, dirname, strlen(dirname), rtl ? "rtl" : "ltr", 3, 0);
    }
    return 0;
}

int fc_entries(struct node *form, struct node *submitter, struct form_entries *out)
{
    memset(out, 0, sizeof *out);
    if (!form || !fc__tag(form->tag, "form")) return -2;
    struct node *root = form;
    while (root->parent) root = root->parent;
    for (struct node *n = root; n; n = next_node(n, root)) {
        if (n->type != N_ELEM) continue;
        int rc = collect_control(out, form, submitter, n);
        if (rc) { fc_entries_free(out); return rc; }
    }
    return 0;
}
