/* Native subscriptions have document lifetime, but the subscriber owns its
 * storage. CLOSE unlinks them without freeing them so a later JS finalizer is
 * safe regardless of whether runtime or arena teardown happened first. */
void dom_unsubscribe(struct dom_subscription *s)
{
    if (!s || !s->doc) return;
    struct dom_subscription **p = &s->doc->subscriptions;
    while (*p && *p != s) p = &(*p)->next;
    if (*p) *p = s->next;
    s->doc = 0; s->next = 0;
}
void dom_subscribe(struct dom_doc *d, struct dom_subscription *s,
                    void (*fn)(void *, const struct dom_mutation *), void *opaque)
{
    dom_unsubscribe(s);
    if (!d) return;
    s->doc = d; s->notify = fn; s->opaque = opaque;
    s->next = d->subscriptions; d->subscriptions = s;
}
static void dom_emit(struct dom_doc *d, const struct dom_mutation *m)
{
    if (!d) return;
    for (struct dom_subscription *s = d->subscriptions; s; s = s->next)
        if (s->notify) s->notify(s->opaque, m);
}
static unsigned dom_child_index(struct node *n)
{
    unsigned i = 0;
    for (struct node *p = n ? n->prev : 0; p; p = p->prev) i++;
    return i;
}
static struct node *dom_scope_parent(struct node *n)
{ return !n || dom_is_shadow_root(n) ? 0 : n->parent; }
static struct node *dom_scope_root(struct node *n)
{ while (dom_scope_parent(n)) n = dom_scope_parent(n); return n; }
static int dom_descendant(struct node *n, struct node *ancestor)
{
    for (; n; n = dom_scope_parent(n)) if (n == ancestor) return 1;
    return 0;
}
static void dom_emit_child(enum dom_mutation_kind kind, struct node *p, struct node *n)
{
    /* Parser appends have no consumers. Do not turn that O(1) path into a
     * sibling walk per token merely to compute an index nobody will read. */
    if (!p->doc->subscriptions) return;
    struct dom_mutation m = { .kind = kind, .node = n, .parent = p,
        .offset = dom_child_index(n), .removed = kind == DOM_MUT_REMOVE,
        .added = kind == DOM_MUT_INSERT };
    dom_emit(p->doc, &m);
}
void dom_notify_attribute(struct node *n, const char *name, const char *old_value,
                           unsigned old_len, const char *new_value, unsigned new_len)
{
    if (!n || !n->doc) return;
    struct dom_mutation m = { .kind = DOM_MUT_ATTRIBUTE, .node = n, .name = name,
        .old_value = old_value, .new_value = new_value,
        .old_len = old_len, .new_len = new_len };
    dom_emit(n->doc, &m);
}
int dom_point_compare(struct node *a, unsigned ao, struct node *b, unsigned bo)
{
    if (!a || !b || dom_scope_root(a) != dom_scope_root(b)) return 2;
    if (a == b) return ao < bo ? -1 : ao > bo ? 1 : 0;
    if (dom_descendant(b, a)) {
        while (dom_scope_parent(b) != a) b = dom_scope_parent(b);
        return ao <= dom_child_index(b) ? -1 : 1;
    }
    if (dom_descendant(a, b)) return -dom_point_compare(b, bo, a, ao);
    unsigned ad = 0, bd = 0;
    for (struct node *n = a; n; n = dom_scope_parent(n)) ad++;
    for (struct node *n = b; n; n = dom_scope_parent(n)) bd++;
    while (ad > bd) { a = dom_scope_parent(a); ad--; }
    while (bd > ad) { b = dom_scope_parent(b); bd--; }
    while (dom_scope_parent(a) != dom_scope_parent(b)) {
        a = dom_scope_parent(a); b = dom_scope_parent(b);
    }
    return dom_child_index(a) < dom_child_index(b) ? -1 : 1;
}
static void dom_range_point_mutate(struct node **n, unsigned *off,
                                    const struct dom_mutation *m)
{
    if (!*n) return;
    switch (m->kind) {
    case DOM_MUT_CLOSE: *n = 0; *off = 0; break;
    case DOM_MUT_DESTROY:
        if (*n == m->node) { *n = 0; *off = 0; }
        break;
    case DOM_MUT_REMOVE:
        if (dom_descendant(*n, m->node)) { *n = m->parent; *off = m->offset; }
        else if (*n == m->parent && *off > m->offset) (*off)--;
        break;
    case DOM_MUT_INSERT:
        if (*n == m->parent && *off > m->offset) (*off)++;
        break;
    case DOM_MUT_TEXT:
        if (*n == m->node && *off > m->offset) {
            if (*off <= m->offset + m->removed) *off = m->offset;
            else *off = *off - m->removed + m->added;
        }
        break;
    case DOM_MUT_SPLIT:
        if (*n == m->node && *off > m->offset) {
            *n = m->related; *off -= m->offset;
        } else if (*n == m->parent && *off == dom_child_index(m->node) + 1) {
            (*off)++;
        }
        break;
    default: break;
    }
}
static void dom_range_mutate(void *opaque, const struct dom_mutation *m)
{
    struct dom_live_range *r = opaque;
#ifndef DOM_RANGE_NO_ADJUST
    dom_range_point_mutate(&r->start, &r->start_offset, m);
    dom_range_point_mutate(&r->end, &r->end_offset, m);
#else
    /* Negative control keeps lifecycle safety; it removes only the boundary
     * updates, recreating the old snapshot Range without a crash-only oracle. */
    if (m->kind == DOM_MUT_CLOSE || m->kind == DOM_MUT_DESTROY) {
        dom_range_point_mutate(&r->start, &r->start_offset, m);
        dom_range_point_mutate(&r->end, &r->end_offset, m);
    }
#endif
}
void dom_range_init(struct dom_live_range *r, struct dom_doc *d)
{
    memset(r, 0, sizeof *r);
    r->start = r->end = d ? d->root : 0;
    dom_subscribe(d, &r->sub, dom_range_mutate, r);
}
void dom_range_dispose(struct dom_live_range *r)
{ if (r) { dom_unsubscribe(&r->sub); r->start = r->end = 0; } }
int dom_range_set(struct dom_live_range *r, int end, struct node *n, unsigned offset)
{
    if (!r || !n || !n->serial || n->type == N_DOCTYPE) return 0;
    if (!r->sub.doc || n->doc != r->sub.doc) return -1;
    unsigned len = 0;
    if (n->type == N_TEXT || n->type == N_COMMENT) len = dom_text_length(n);
    else for (struct node *c = n->first_child; c; c = c->next) len++;
    if (offset > len) return 0;
    /* Setting a boundary in another root collapses the Range into that root,
     * per DOM. It never creates a range spanning a ShadowRoot or detached tree. */
    int order = end ? dom_point_compare(r->start, r->start_offset, n, offset)
                    : dom_point_compare(n, offset, r->end, r->end_offset);
    if (order == 2 || order > 0) {
        r->start = r->end = n; r->start_offset = r->end_offset = offset;
    } else if (end) { r->end = n; r->end_offset = offset; }
    else { r->start = n; r->start_offset = offset; }
    return 1;
}
