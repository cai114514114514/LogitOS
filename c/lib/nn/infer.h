/* infer.h -- one token in, one row of logits out.
 *
 * THE MEMORY BUDGET IS THE INTERFACE. On a 512 MiB machine the question that
 * decides whether a model runs is not "how fast" but "how much", and it has to
 * be answerable BEFORE anything is allocated -- so `lm_state_bytes` exists and
 * every buffer an inference pass uses lives in the state. Nothing below
 * allocates during a forward pass. That is also what makes the pass safe to
 * run from a GUI app's event loop: no malloc, no failure path, no surprise.
 *
 * THE KV CACHE IS THE BUDGET. Activations are a few kilobytes; the cache is
 *     n_layers * seq_len * n_kv_heads * head_dim * 2 * 4 bytes
 * and at seq_len 256, 4 layers, dim 128 that is 1 MiB -- small, and growing
 * linearly in the context length, which is the number a caller has to reason
 * about when it decides how long a conversation may get.
 *
 * ONE TOKEN AT A TIME, and no batch. A batch exists to amortise the weight
 * read across several sequences, and this machine runs one. Adding an M
 * dimension here would make every shape two-dimensional to serve a case that
 * does not occur -- nn_matmul_f32 is there for a prefill that wants it, and
 * this layer deliberately does not.
 */
#ifndef LOGIT_LM_INFER_H
#define LOGIT_LM_INFER_H

#include <stddef.h>
#include "model.h"

struct lm_state {
    const struct lm_model *m;
    int pos;                    /* how many tokens are in the cache */
    /* every one of these points into `arena`; the state owns exactly one
     * allocation, so a caller that wants to place it somewhere specific can */
    float *x;                   /* [dim]      the residual stream */
    /* [max(dim, q_dim)] -- TWO producers of different widths write it, and
     * they are equal only when head_dim is derived. nn_rmsnorm fills `dim`;
     * the attention block fills n_heads*head_dim. At Qwen3-0.6B's shape that
     * is 2048 against a dim of 1024, and the buffer used to be `dim`. */
    float *xb;
    float *xb2;                 /* [dim]      scratch */
    float *hb;                  /* [hidden]   scratch */
    float *hb2;                 /* [hidden]   scratch */
    float *q;                   /* [q_dim] = n_heads * head_dim, NOT dim */
    float *k;                   /* [kv_dim]   this step's key  */
    float *v;                   /* [kv_dim]   this step's value */
    float *att;                 /* [n_heads * seq_len] */
    float *logits;              /* [vocab] */
    float *kcache;              /* [n_layers, seq_len, kv_dim] */
    float *vcache;              /* [n_layers, seq_len, kv_dim] */
    float *deq;                 /* [max_row_len] -- see infer.c on why */
    void  *arena;               /* the single allocation */
    size_t arena_len;
};

/* THE REFUSALS, NAMED. They were bare integers at the two return sites, which
 * was fine while "did it work" was the only question -- and stopped being fine
 * the moment a header could be refused for a reason a caller can act on. A
 * shape whose q_dim does not fit an int is a bad FILE; a geometry the loader
 * and the sizer disagree about is a BUG in one of them; an allocation failure
 * is neither, and on a 512 MiB machine it is the expected answer for a model
 * that is simply too big. Flattening the three onto one code makes the log
 * say "lm_state_new failed" and nothing else.
 *
 * Numbered as they already were, so nothing that reads the old values moves;
 * E_GEOM and E_SIZE are new codes for cases that used to be E_HEADER. */
#define LM_STATE_OK        0
#define LM_STATE_E_ARG    (-1)   /* a NULL argument */
#define LM_STATE_E_HEADER (-2)   /* a field is zero, or a rule the header
                                  * itself must satisfy does not hold */
#define LM_STATE_E_ALLOC  (-3)   /* malloc said no */
#define LM_STATE_E_CARVE  (-4)   /* the two layout passes disagreed; cannot
                                  * happen, checked anyway (see infer.c) */
#define LM_STATE_E_GEOM   (-5)   /* the head geometry this file derives from
                                  * the header is NOT the one lm_open put in
                                  * the model -- so the arena would be sized
                                  * for a shape the forward pass does not use */
#define LM_STATE_E_SIZE   (-6)   /* the shape is self-consistent and does not
                                  * fit the machine's arithmetic */

/* Exactly how many bytes lm_state_new will ask for. A caller can print this,
 * refuse, or place it itself; it is a pure function of the header. */
size_t lm_state_bytes(const struct lm_model *m);

/* Why the above returned 0, or LM_STATE_OK when it did not. A size_t has one
 * way to say no and there are now four reasons to; this is how a caller tells
 * them apart WITHOUT allocating, which is the whole point of asking the size
 * first. Same codes lm_state_new returns, computed by the same call. */
int lm_state_why(const struct lm_model *m);

int  lm_state_new(struct lm_state *s, const struct lm_model *m);
void lm_state_free(struct lm_state *s);
/* Forget the conversation, keep the allocation. */
void lm_state_reset(struct lm_state *s);

/* Run one token at position `pos` and return the logits, which live in the
 * state and are valid until the next call. `pos` must be s->pos (the next
 * position); passing anything else is a caller bug and returns NULL rather
 * than quietly producing attention over a cache that does not contain what it
 * claims -- which reads as a slightly worse model rather than as an error. */
const float *lm_forward(const struct lm_model *m, struct lm_state *s,
                        int token, int pos);

/* ------------------------------------------------------------- sampling --
 *
 * Greedy is separate from stochastic because they answer different questions:
 * greedy is DETERMINISTIC and is therefore the only one a test can assert on,
 * and every gate in this line uses it. top-p is what a person wants to read. */
int lm_sample_greedy(const float *logits, int n);

/* Temperature + nucleus (top-p). `logits` is modified in place. `rng` is the
 * caller's state so a run is reproducible from a seed -- an inference engine
 * whose output cannot be reproduced from its inputs cannot be bisected. */
int lm_sample_topp(float *logits, int n, float temp, float topp,
                   unsigned long long *rng);

#endif /* LOGIT_LM_INFER_H */
