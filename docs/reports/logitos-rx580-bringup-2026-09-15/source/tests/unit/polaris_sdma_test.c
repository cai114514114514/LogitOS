#include "polaris_sdma.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { \
    ++failures; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); \
} } while (0)

/* These are literal protocol words from the AMD v3 layout, deliberately not
 * assembled from the implementation's opcode/count constants. A count-minus-
 * one build must fail the COPY_LITERAL_ORACLE check below. No mock GPU interprets
 * our own encoder here, and passing does not prove firmware/ring execution. */
static const uint32_t copy_oracle[] = {
    0x00000001u, 0x00000020u, 0x00000000u,
    0xffffffe0u, 0x00000000u, 0xabcdef40u, 0x00000012u,
};
static const uint32_t fill_oracle[] = {
    0x0000000bu, 0xabcdef80u, 0x00000012u, 0xa1b2c3d4u, 0x00000040u,
};
static const uint32_t fence_oracle[] = {
    0x00000005u, 0xabcdefc0u, 0x00000012u, 0x12345678u,
};

static uint32_t words[32], snapshot[32];
static struct polaris_sdma_stream stream;

static void reset(void)
{
    for (size_t i = 0; i < 32; ++i)
        words[i] = 0xbeef0000u + (uint32_t)i;
    stream = (struct polaris_sdma_stream){ words, 32, 0 };
}

#define REJECT_UNCHANGED(expression, error) do { \
    struct polaris_sdma_stream old = stream; \
    memcpy(snapshot, words, sizeof words); \
    CHECK((expression) == (error)); \
    CHECK(stream.words == old.words && stream.capacity_dw == old.capacity_dw && \
          stream.used_dw == old.used_dw && !memcmp(snapshot, words, sizeof words)); \
} while (0)

int main(void)
{
    const struct polaris_gpu_range src = { UINT64_C(0xffffffc0), 0x100 };
    const struct polaris_gpu_range dst = { UINT64_C(0x12abcdef00), 0x1000000 };
    reset();
    CHECK(polaris_sdma_emit_copy(&stream, &src, 0x20, &dst, 0x40, 32) == 0);
    CHECK(!memcmp(words, copy_oracle, sizeof copy_oracle)); /* COPY_LITERAL_ORACLE */
    CHECK(stream.used_dw == 7 && words[7] == 0xbeef0007u);
    CHECK(polaris_sdma_emit_fill(&stream, &dst, 0x80, 0xa1b2c3d4u, 64) == 0);
    CHECK(!memcmp(words + 7, fill_oracle, sizeof fill_oracle));
    CHECK(stream.used_dw == 12 && words[12] == 0xbeef000cu);
    CHECK(polaris_sdma_emit_fence(&stream, &dst, 0xc0, 0x12345678u) == 0);
    CHECK(!memcmp(words + 12, fence_oracle, sizeof fence_oracle));
    CHECK(stream.used_dw == 16 && words[16] == 0xbeef0010u);

    const uint32_t bad_counts[] = { 0, 1, 3, 7, 0x3fffe4, 0x3fffff, UINT32_MAX };
    for (size_t i = 0; i < sizeof bad_counts / sizeof bad_counts[0]; ++i) {
        REJECT_UNCHANGED(polaris_sdma_emit_copy(&stream, &src, 0, &dst, 0,
                                               bad_counts[i]), -1);
        REJECT_UNCHANGED(polaris_sdma_emit_fill(&stream, &dst, 0, 0,
                                               bad_counts[i]), -1);
    }

    const struct polaris_gpu_range invalid[] = {
        { 0x1000, 0 }, { 0x1001, 0x100 }, { 0x1000, 0x101 },
        { UINT64_C(0x10000000000), 4 },
        { UINT64_C(0xfffffffffc), 8 },
        { UINT64_MAX - 3, 8 }, { 0, UINT64_MAX - 3 },
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        REJECT_UNCHANGED(polaris_sdma_emit_copy(&stream, &invalid[i], 0,
                                               &dst, 0, 4), -1);
        REJECT_UNCHANGED(polaris_sdma_emit_copy(&stream, &src, 0,
                                               &invalid[i], 0, 4), -1);
        REJECT_UNCHANGED(polaris_sdma_emit_fill(&stream, &invalid[i], 0, 0, 4), -1);
        REJECT_UNCHANGED(polaris_sdma_emit_fence(&stream, &invalid[i], 0, 1), -1);
    }
    const uint64_t bad_offsets[] = { 1, 0xfd, 0x100, 0x104, UINT64_MAX - 3 };
    for (size_t i = 0; i < sizeof bad_offsets / sizeof bad_offsets[0]; ++i) {
        REJECT_UNCHANGED(polaris_sdma_emit_copy(&stream, &src, bad_offsets[i],
                                               &dst, 0, 4), -1);
        REJECT_UNCHANGED(polaris_sdma_emit_fill(&stream, &src, bad_offsets[i],
                                               0, 4), -1);
        REJECT_UNCHANGED(polaris_sdma_emit_fence(&stream, &src, bad_offsets[i],
                                                0), -1);
    }
    REJECT_UNCHANGED(polaris_sdma_emit_copy(&stream, &src, 0xfc, &dst, 0, 8), -1);
    REJECT_UNCHANGED(polaris_sdma_emit_copy(&stream, &dst, 0, &src, 0xfc, 8), -1);
    REJECT_UNCHANGED(polaris_sdma_emit_fill(&stream, &src, 0xfc, 0, 8), -1);
    REJECT_UNCHANGED(polaris_sdma_emit_copy(&stream, &src, 0, &src, 0, 32), -1);
    REJECT_UNCHANGED(polaris_sdma_emit_copy(&stream, &src, 0, &src, 4, 32), -1);
    REJECT_UNCHANGED(polaris_sdma_emit_copy(&stream, &src, 4, &src, 0, 32), -1);
    const struct polaris_gpu_range alias = { UINT64_C(0xffffffe0), 64 };
    REJECT_UNCHANGED(polaris_sdma_emit_copy(&stream, &src, 0x20, &alias, 0, 32), -1);
    REJECT_UNCHANGED(polaris_sdma_emit_copy(&stream, NULL, 0, &dst, 0, 4), -1);
    REJECT_UNCHANGED(polaris_sdma_emit_copy(&stream, &src, 0, NULL, 0, 4), -1);
    REJECT_UNCHANGED(polaris_sdma_emit_fill(&stream, NULL, 0, 0, 4), -1);
    REJECT_UNCHANGED(polaris_sdma_emit_fence(&stream, NULL, 0, 0), -1);
    REJECT_UNCHANGED(polaris_sdma_emit_copy(NULL, &src, 0, &dst, 0, 4), -1);

    /* Exercise every capacity below a complete packet, with an existing prefix:
     * insufficient space must not leave a command header in the live prefix. */
    for (size_t available = 0; available < 7; ++available) {
        reset(); stream.used_dw = 2; stream.capacity_dw = 2 + available;
        REJECT_UNCHANGED(polaris_sdma_emit_copy(&stream, &src, 0, &dst, 0, 4), -2);
        if (available < 5)
            REJECT_UNCHANGED(polaris_sdma_emit_fill(&stream, &dst, 0, 0, 4), -2);
        if (available < 4)
            REJECT_UNCHANGED(polaris_sdma_emit_fence(&stream, &dst, 0, 0), -2);
    }
    reset(); stream.capacity_dw = 7;
    CHECK(polaris_sdma_emit_copy(&stream, &src, 0, &dst, 0, 4) == 0);
    CHECK(stream.used_dw == 7 && words[7] == 0xbeef0007u);
    CHECK(words[1] == 4u);
    reset(); stream.capacity_dw = 5;
    CHECK(polaris_sdma_emit_fill(&stream, &dst, 0, 0, 4) == 0);
    CHECK(stream.used_dw == 5 && words[5] == 0xbeef0005u);
    CHECK(words[4] == 4u);
    reset(); stream.capacity_dw = 4;
    CHECK(polaris_sdma_emit_fence(&stream, &dst, 0, 0) == 0);
    CHECK(stream.used_dw == 4 && words[4] == 0xbeef0004u);

    reset(); stream.used_dw = 33;
    REJECT_UNCHANGED(polaris_sdma_emit_fence(&stream, &dst, 0, 0), -1);
    reset(); stream.capacity_dw = SIZE_MAX;
    REJECT_UNCHANGED(polaris_sdma_emit_fence(&stream, &dst, 0, 0), -1);
    reset(); stream.words = NULL;
    REJECT_UNCHANGED(polaris_sdma_emit_fence(&stream, &dst, 0, 0), -1);
    reset(); stream.words = (uint32_t *)((unsigned char *)words + 1);
    REJECT_UNCHANGED(polaris_sdma_emit_fence(&stream, &dst, 0, 0), -1);
    reset(); stream.words = (uint32_t *)(UINTPTR_MAX - 3); stream.capacity_dw = 2;
    REJECT_UNCHANGED(polaris_sdma_emit_fence(&stream, &dst, 0, 0), -1);

    /* End-exact, 4 GiB carry, adjacent self-copy, GPU address zero, top of the
     * 40-bit window, and the documented hardware maximum are all legal. */
    reset();
    CHECK(polaris_sdma_emit_copy(&stream, &src, 0xfc, &dst, 0, 4) == 0);
    CHECK(words[3] == 0xbcu && words[4] == 1u);
    CHECK(polaris_sdma_emit_copy(&stream, &src, 0, &src, 32, 32) == 0);
    const struct polaris_gpu_range zero = { 0, 0x1000000 };
    const struct polaris_gpu_range top = { UINT64_C(0xfffffffffc), 4 };
    reset();
    CHECK(polaris_sdma_emit_copy(&stream, &zero, 0, &dst, 0, 0x3fffe0) == 0);
    CHECK(words[1] == 0x3fffe0u);
    CHECK(polaris_sdma_emit_fill(&stream, &dst, 0, 0, 0x3fffe0) == 0);
    CHECK(words[11] == 0x3fffe0u);
    CHECK(polaris_sdma_emit_fence(&stream, &top, 0, UINT32_MAX) == 0);
    CHECK(words[13] == 0xfffffffcu && words[14] == 0xffu && words[15] == UINT32_MAX);
    reset();
    CHECK(polaris_sdma_emit_fill(&stream, &top, 0, 0x12345678, 4) == 0);
    CHECK(polaris_sdma_emit_fence(&stream, &zero, 0, 0) == 0);

    printf("POLARIS_SDMA: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
