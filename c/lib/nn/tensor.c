/* tensor.c -- shapes, allocation, and describing memory somebody else owns.
 *
 * The whole file is bookkeeping; the arithmetic is in matmul.c and ops.c. It
 * is separate because the ownership rule is the part that has to be right:
 * a model's weights arrive as one mapping of one file, and every tensor that
 * points into it must be describable without copying it. See nn.h. */
#include <stdlib.h>
#include <string.h>
#include "nn.h"

size_t nn_numel(const struct nn_tensor *t)
{
    if (!t || t->ndim <= 0) return 0;
    size_t n = 1;
    for (int i = 0; i < t->ndim; i++) {
        if (t->dim[i] <= 0) return 0;
        n *= (size_t)t->dim[i];
    }
    return n;
}

size_t nn_bytes(const struct nn_tensor *t)
{
    size_t n = nn_numel(t);
    if (!n) return 0;
    if (t->dtype == NN_Q8) {
        /* the payload AND the per-row scales, because "how much memory does
         * this cost" is the question a caller is asking and the scales are
         * part of the answer */
        size_t rows = (size_t)t->dim[0];
        return n + rows * sizeof(float);
    }
    return n * sizeof(float);
}

int nn_new(struct nn_tensor *out, int dtype, int ndim, const int *dim)
{
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (ndim <= 0 || ndim > NN_MAXDIM || !dim) return 0;
    size_t n = 1;
    for (int i = 0; i < ndim; i++) {
        if (dim[i] <= 0) return 0;
        /* Overflow is refused rather than wrapped: a shape whose product does
         * not fit is a shape this machine could not hold anyway, and a
         * wrapped size_t would allocate a small buffer for a huge tensor and
         * fail somewhere else entirely. */
        if ((size_t)dim[i] > (size_t)-1 / n) return 0;
        n *= (size_t)dim[i];
        out->dim[i] = dim[i];
    }
    out->ndim = ndim;
    out->dtype = dtype;

    if (dtype == NN_Q8) {
        size_t rows = (size_t)dim[0];
        out->q = (int8_t *)malloc(n);
        out->scale = (float *)malloc(rows * sizeof(float));
        if (!out->q || !out->scale) {
            free(out->q); free(out->scale);
            memset(out, 0, sizeof *out);
            return 0;
        }
        memset(out->q, 0, n);
        memset(out->scale, 0, rows * sizeof(float));
    } else {
        out->data = (float *)malloc(n * sizeof(float));
        if (!out->data) { memset(out, 0, sizeof *out); return 0; }
        memset(out->data, 0, n * sizeof(float));
    }
    out->own = 1;
    return 1;
}

void nn_free(struct nn_tensor *t)
{
    if (!t) return;
    if (t->own) { free(t->data); free(t->q); free(t->scale); }
    memset(t, 0, sizeof *t);
}

void nn_wrap_f32(struct nn_tensor *out, float *data, int ndim, const int *dim)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (ndim <= 0 || ndim > NN_MAXDIM || !dim) return;
    out->dtype = NN_F32;
    out->ndim = ndim;
    for (int i = 0; i < ndim; i++) out->dim[i] = dim[i];
    out->data = data;
    out->own = 0;
}

void nn_wrap_q8(struct nn_tensor *out, int8_t *q, float *scale,
                int ndim, const int *dim)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (ndim <= 0 || ndim > NN_MAXDIM || !dim) return;
    out->dtype = NN_Q8;
    out->ndim = ndim;
    for (int i = 0; i < ndim; i++) out->dim[i] = dim[i];
    out->q = q;
    out->scale = scale;
    out->own = 0;
}
