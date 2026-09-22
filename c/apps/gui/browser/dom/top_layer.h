#ifndef LOGIT_TOP_LAYER_H
#define LOGIT_TOP_LAYER_H
struct node;
/* Modal state has ONE authority. js_semantics' showModal/close update this
 * stack; layout, paint, trusted input and focus read it. focus.c includes the
 * implementation so the existing BROWSER_PIPE and native-focus host links
 * cannot silently omit a new translation unit. */
int top_layer_push(struct node *dialog);
int top_layer_remove(struct node *dialog);
int top_layer_is_modal(const struct node *dialog);
int top_layer_contains(const struct node *node);
int top_layer_is_popover(const struct node *node);
int top_layer_is_hidden_popover(const struct node *node);
int top_layer_push_popover(struct node *node, struct node *source);
int top_layer_count(void);
struct node *top_layer_at(int index);
struct node *top_layer_current(void);
/* current is the highest MODAL; at/count enumerate every layer in paint order. */
/* Nearest top-layer ancestor; 0 means ordinary page content. */
struct node *top_layer_owner(const struct node *node);
int top_layer_allows_input(const struct node *node);
void top_layer_reset(void); /* navigation BEFORE freeing the old DOM/context */
void top_layer_set_close_request(void (*fn)(struct node *));
void top_layer_set_popover_hide(void (*fn)(struct node *));
void top_layer_pointer_down(struct node *target);
int top_layer_pointer_up(struct node *target); /* 1 means auto popovers closed */
/* 1 consumes Escape (including closedby=none / canceled cancel event), 0 means
 * no modal. Call AFTER a trusted keydown that did not preventDefault. */
int top_layer_escape(void);
#endif
