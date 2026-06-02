#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "ttf.h"
static int fail;
static void eq(const char *what, int got, int want){
    if (got != want){ printf("FAIL %s: got %d want %d\n", what, got, want); fail = 1; }
}
int main(int argc, char **argv){
    const char *path = argc > 1 ? argv[1] : "fsroot/fonts/ui.ttf";
    FILE *fp = fopen(path, "rb"); if(!fp){ printf("no %s\n", path); return 2; }
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint8_t *buf = malloc(n); if (fread(buf, 1, n, fp) != (size_t)n) return 2; fclose(fp);

    struct ttf_font f;
    eq("parse", ttf_parse(buf, (int)n, &f), 0);
    eq("upem", f.units_per_em, 1000);
    eq("ascent", f.ascent, 860);
    eq("descent", f.descent, -140);
    eq("gid('A')", ttf_glyph_id(&f, 0x41), 33);
    eq("gid('你')", ttf_glyph_id(&f, 0x4F60), 130);
    eq("adv('A')", ttf_advance(&f, ttf_glyph_id(&f, 0x41)), 740);
    eq("adv('你')", ttf_advance(&f, ttf_glyph_id(&f, 0x4F60)), 1000);
    eq("gid(missing)", ttf_glyph_id(&f, 0x1FBFF), 0);
    printf(fail ? "SOME FAILED\n" : "ALL PASS\n");
    return fail;
}
