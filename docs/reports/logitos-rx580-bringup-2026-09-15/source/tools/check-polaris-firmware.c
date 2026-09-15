/* Host inspection only: use the same parser compiled into the kernel against
 * supplied firmware files. This tool never installs or executes microcode.
 * Filenames and valid headers are not authenticity or ASIC matching proofs. */
#include <stdio.h>
#include <stdlib.h>
#include "polaris_firmware.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s firmware.bin [firmware1.bin]\n", argv[0]);
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) { perror(argv[i]); return 2; }
        if (fseek(f, 0, SEEK_END)) { fclose(f); return 2; }
        long length = ftell(f);
        /* Limit is local inspection policy, not part of AMD's file format. */
        if (length <= 0 || length > 4 * 1024 * 1024 || fseek(f, 0, SEEK_SET)) {
            fclose(f); fprintf(stderr, "invalid inspection file size\n"); return 2;
        }
        void *bytes = malloc((size_t)length);
        if (!bytes) { fclose(f); return 2; }
        size_t got = fread(bytes, 1, (size_t)length, f);
        int io_error = ferror(f);
        fclose(f);
        if (got != (size_t)length || io_error) { free(bytes); return 2; }
        struct polaris_sdma_firmware fw;
        int rc = polaris_sdma_firmware_parse(bytes, (size_t)length, &fw);
        if (rc) {
            fprintf(stderr, "POLARIS_FW_FILE rejected file=%s parser=%d\n", argv[i], rc);
            free(bytes); return 1;
        }
        printf("POLARIS_FW_FILE structure=valid file=%s header=%u.%u ip=%u.%u "
               "ucode_version=%u feature=%u words=%u jump=%u/%u "
               "digest_raw=%u integrity=unchecked loaded=0\n", argv[i],
               fw.header_major, fw.header_minor, fw.ip_major, fw.ip_minor,
               fw.ucode_version, fw.feature_version, fw.ucode_dwords,
               fw.jump_offset_dwords, fw.jump_size_dwords, fw.digest_size);
        free(bytes);
    }
    return 0;
}
