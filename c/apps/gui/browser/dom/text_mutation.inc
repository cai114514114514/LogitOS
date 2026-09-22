/* UTF-8 storage / UTF-16 DOM offsets. Decode through code units only on a
 * replacing edit; the tokenizer's unobserved geometric append stays cheap.
 * Unmatched surrogates intentionally round-trip as WTF-8, as QuickJS does. */
static unsigned dom_decode(const char *s, unsigned len, unsigned *at)
{
    unsigned c = (unsigned char)s[(*at)++], n = 0, v = c;
    if (c >= 0xc2 && c < 0xe0) { n = 1; v = c & 31; }
    else if (c >= 0xe0 && c < 0xf0) { n = 2; v = c & 15; }
    else if (c >= 0xf0 && c < 0xf5) { n = 3; v = c & 7; }
    else return c < 128 ? c : 0xfffd;
    if (n > len - *at) return 0xfffd;
    for (unsigned k = 0; k < n; k++) {
        unsigned b = (unsigned char)s[*at + k];
        if ((b & 0xc0) != 0x80) return 0xfffd;
        v = (v << 6) | (b & 63);
    }
    *at += n;
    return v > 0x10ffff ? 0xfffd : v;
}
unsigned dom_utf16_length(const char *s, unsigned len)
{
    unsigned count = 0, at = 0;
    while (s && at < len) count += dom_decode(s, len, &at) > 0xffff ? 2 : 1;
    return count;
}
unsigned dom_text_length(const struct node *n)
{ return n ? dom_utf16_length(n->text, n->textlen > 0 ? (unsigned)n->textlen : 0) : 0; }
static unsigned dom_units(const char *s, unsigned len, uint16_t *out)
{
    unsigned at = 0, k = 0;
    while (at < len) {
        unsigned c = dom_decode(s, len, &at);
        if (c > 0xffff) { c -= 0x10000; out[k++] = 0xd800 + (c >> 10); out[k++] = 0xdc00 + (c & 1023); }
        else out[k++] = c;
    }
    return k;
}
static unsigned dom_encode_units(const uint16_t *u, unsigned n, char *out)
{
    unsigned k = 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned c = u[i];
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < n && u[i+1] >= 0xdc00 && u[i+1] <= 0xdfff)
            c = 0x10000 + ((c - 0xd800) << 10) + (u[++i] - 0xdc00);
        if (c < 128) out[k++] = c;
        else if (c < 0x800) { out[k++] = 0xc0 | (c >> 6); out[k++] = 0x80 | (c & 63); }
        else if (c < 0x10000) { out[k++] = 0xe0 | (c >> 12); out[k++] = 0x80 | ((c >> 6) & 63); out[k++] = 0x80 | (c & 63); }
        else { out[k++] = 0xf0 | (c >> 18); out[k++] = 0x80 | ((c >> 12) & 63); out[k++] = 0x80 | ((c >> 6) & 63); out[k++] = 0x80 | (c & 63); }
    }
    out[k] = 0; return k;
}
int dom_text_replace(struct node *n, unsigned offset, unsigned count, const char *s, int len)
{
    if (!n || !n->doc || (n->type != N_TEXT && n->type != N_COMMENT) || len < 0 || (!s && len)) return 0;
    unsigned old = dom_text_length(n), added = dom_utf16_length(s, (unsigned)len);
    if (offset > old) return 0;
    if (count > old - offset) count = old - offset;
    if (old > 0x1fffffff || added > 0x1fffffff - old) return 0;
    unsigned total = old - count + added;
    uint16_t *units = kmalloc(((unsigned long)old + added + 1) * sizeof *units);
    if (!units) return 0;
    dom_units(n->text, n->textlen, units);
    /* Move the suffix before decoding the insertion: it may alias old text. */
    if (added > count) for (unsigned k = old; k > offset + count; k--) units[k - count + added - 1] = units[k - 1];
    else for (unsigned k = offset + count; k < old; k++) units[k - count + added] = units[k];
    if (len) dom_units(s, (unsigned)len, units + offset);
    char *out = arena_alloc(n->doc, (size_t)total * 3 + 1);
    if (!out) { kfree(units); return 0; }
    unsigned bytes = dom_encode_units(units, total, out);
    kfree(units);
    struct dom_mutation m = { .kind = DOM_MUT_TEXT, .node = n,
        .offset = offset, .removed = count, .added = added,
        .old_value = n->text, .old_len = n->textlen, .new_value = out, .new_len = bytes };
    n->text = out; n->textlen = bytes; n->textcap = total * 3 + 1;
    dom_emit(n->doc, &m);
    return 1;
}
int dom_text_splice_bytes(struct node *n, unsigned a, unsigned b, const char *s, int len)
{
    if (!n || !n->doc || (n->type != N_TEXT && n->type != N_COMMENT) ||
        len < 0 || (!s && len) || b < a || b > (unsigned)n->textlen) return 0;
    size_t total = (size_t)n->textlen - (b - a) + (unsigned)len;
    if (total > 0x7ffffffe) return 0;
    char *out = arena_alloc(n->doc, total + 1);
    if (!out) return 0;
    if (a) memcpy(out, n->text, a);
    if (len) memcpy(out + a, s, (unsigned)len);
    if ((unsigned)n->textlen > b) memcpy(out + a + len, n->text + b, (unsigned)n->textlen - b);
    out[total] = 0;
    struct dom_mutation m = { .kind = DOM_MUT_TEXT, .node = n,
        .offset = dom_utf16_length(n->text, a),
        .removed = dom_utf16_length(n->text + a, b - a),
        .added = dom_utf16_length(s, (unsigned)len),
        .old_value = n->text, .old_len = n->textlen,
        .new_value = out, .new_len = (unsigned)total };
    n->text = out; n->textlen = total; n->textcap = total + 1;
    dom_emit(n->doc, &m); return 1;
}

struct node *dom_text_split(struct node *n, unsigned offset)
{
    if (!n || n->type != N_TEXT || offset > dom_text_length(n)) return 0;
    unsigned length = dom_text_length(n);
    /* Allocate both strings before exposing a new sibling: OOM must not split
     * the tree while leaving the original data unchanged. */
    uint16_t *u = kmalloc(((unsigned long)length + 1) * 2);
    char *a = arena_alloc(n->doc, (size_t)length * 3 + 1);
    char *b = arena_alloc(n->doc, (size_t)length * 3 + 1);
    if (!u || !a || !b) { if (u) kfree(u); return 0; }
    dom_units(n->text, n->textlen, u);
    unsigned al = dom_encode_units(u, offset, a), bl = dom_encode_units(u + offset, length - offset, b);
    kfree(u);
    struct node *tail = dom_create_text(n->doc, b, bl);
    if (!tail) return 0;
    struct node *parent = n->parent;
    if (parent) dom_insert_before(parent, tail, n->next);
    struct dom_mutation split = { .kind = DOM_MUT_SPLIT, .node = n, .parent = parent, .related = tail, .offset = offset };
    /* A detached split has no range transfer (DOM split-text step 7). Its old
     * boundaries clamp during replace-data just like an ordinary truncation. */
    if (parent) dom_emit(n->doc, &split);
    struct dom_mutation text = { .kind = DOM_MUT_TEXT, .node = n, .offset = offset,
        .removed = length - offset, .old_value = n->text, .old_len = n->textlen,
        .new_value = a, .new_len = al };
    n->text = a; n->textlen = al; n->textcap = length * 3 + 1;
    dom_emit(n->doc, &text);
    return tail;
}
