/* Host-only staging consumer. The last argument is an explicit hypothetical
 * GPU placement, not a reservation. Output is not a flashable image and this
 * program performs no GPU access, firmware installation or mailbox writes. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "amd/polaris/smu/stage.h"

static void *read_file(const char *name, size_t *size)
{
    FILE *f = fopen(name, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0 || n > 4 * 1024 * 1024 || fseek(f, 0, SEEK_SET)) {
        fclose(f); return NULL;
    }
    void *b = malloc((size_t)n);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)n, f);
    int error = ferror(f);
    fclose(f);
    if (got != (size_t)n || error) { free(b); return NULL; }
    *size = (size_t)n;
    return b;
}
int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s sdma0.bin sdma1.bin output.bin proposed_gpu_base\n", argv[0]);
        return 2;
    }
    char *end;
    errno = 0;
    unsigned long long base = strtoull(argv[4], &end, 0);
    if (errno || end == argv[4] || *end || argv[4][0] == '-') return 2;
    size_t sizes[2];
    void *a = read_file(argv[1], &sizes[0]), *b = read_file(argv[2], &sizes[1]);
    if (!a || !b) { free(a); free(b); fprintf(stderr, "firmware read failed\n"); return 2; }
    size_t capacity = 4096 + ((sizes[0] + 4095) & ~(size_t)4095) +
                             ((sizes[1] + 4095) & ~(size_t)4095);
    uint8_t *image = malloc(capacity);
    struct polaris_smu_stage_info info;
    int rc = image ? polaris_smu_stage_sdma(image, capacity, base,
                       a, sizes[0], b, sizes[1], &info) : -1;
    free(a); free(b);
    if (rc) { free(image); fprintf(stderr, "POLARIS_STAGE rejected inputs\n"); return 1; }
    /* Opening the destination after validation preserves it on a bad input. */
    FILE *f = fopen(argv[3], "wb");
    if (!f) { free(image); return 2; }
    size_t written = fwrite(image, 1, info.bytes_used, f);
    int closed = fclose(f);
    free(image);
    if (written != info.bytes_used || closed) return 2;
    printf("POLARIS_STAGE bytes=%zu proposed_gpu_base=0x%llx "
           "offsets=%zu/%zu payloads=%u/%u present=0x%x missing=0x%x "
           "gpu_mapped=0 uploaded=0 loaded=0\n", info.bytes_used, base,
           info.image_offset[0], info.image_offset[1], info.image_bytes[0],
           info.image_bytes[1], info.inventory.present_mask, info.inventory.missing_mask);
    return 0;
}
