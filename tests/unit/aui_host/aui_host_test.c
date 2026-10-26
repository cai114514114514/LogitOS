/* Driver: renders every scene at every scale in both modes into OUTDIR.
 *
 *   aui_host_test OUTDIR            all 32 cases
 *   aui_host_test OUTDIR scene@pct  only the cases for that scene (any mode)
 *
 * Files: OUTDIR/<scene>@<pct>.ppm (INFO=0, the legacy kernel) and
 * OUTDIR/<scene>@<pct>.v2.ppm (INFO = OR of the registered v2_*.c bits), plus a
 * matching .stats text file with the deterministic per-case budgets.
 *
 * aui.c keeps its state in file-scope statics (cached scale, animation slots,
 * focus), so each case runs in a forked child: a scale or a widget id from one
 * case cannot leak into the next. Exit is non-zero when any case fails, and a
 * case fails on any syscall number nobody handled. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "aui_host.h"

static int run_case(const char *dir, int scene, int pct, int v2)
{
    int w, hh;
    aui_host_scene_size(scene, &w, &hh);
    struct aui_host *h = aui_host_new(w, hh, pct);
    if (!h) { fprintf(stderr, "FAIL alloc\n"); return 1; }
    unsigned info = 0;
    if (v2) info = aui_host_enable_v2(h);
    aui_host_stats_reset(h);
    aui_host_scene_draw(scene, h);

    char path[512];
    snprintf(path, sizeof path, "%s/%s@%d%s.ppm", dir, aui_host_scene_name(scene), pct, v2 ? ".v2" : "");
    if (aui_host_write_ppm(h, path)) { fprintf(stderr, "FAIL write %s\n", path); return 1; }

    snprintf(path, sizeof path, "%s/%s@%d%s.stats", dir, aui_host_scene_name(scene), pct, v2 ? ".v2" : "");
    FILE *f = fopen(path, "w");
    if (f) {
        unsigned long long total = 0;
        for (int i = 0; i < AUI_HOST_NSYS; i++) total += h->calls[i];
        fprintf(f, "scene %s pct %d mode %s info 0x%x\n", aui_host_scene_name(scene), pct, v2 ? "v2" : "legacy", info);
        fprintf(f, "syscalls %llu\npx_written %llu\nflushes %d\n", total, h->px_written, h->ndmg);
        for (int i = 0; i < AUI_HOST_NSYS; i++)
            if (h->calls[i]) fprintf(f, "  sys %d: %llu\n", i, h->calls[i]);
        fclose(f);
    }
    int bad = aui_host_unhandled_total(h);
    if (bad) {
        for (int i = 0; i < AUI_HOST_NSYS; i++)
            if (h->unhandled[i])
                fprintf(stderr, "FAIL unhandled syscall %d x%llu in %s@%d%s\n", i,
                        h->unhandled[i], aui_host_scene_name(scene), pct, v2 ? ".v2" : "");
        return 3;
    }
    aui_host_free(h);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s OUTDIR [scene@pct]\n", argv[0]); return 2; }
    const char *dir = argv[1], *only = argc > 2 ? argv[2] : 0;
    int failed = 0, ran = 0;
    for (int s = 0; s < AUI_HOST_NSCENES; s++)
        for (int p = 0; p < AUI_HOST_NPCT; p++)
            for (int v2 = 0; v2 < 2; v2++) {
                if (only) {
                    char tag[64];
                    snprintf(tag, sizeof tag, "%s@%d", aui_host_scene_name(s), aui_host_pcts[p]);
                    if (strcmp(tag, only)) continue;
                }
                fflush(stdout); fflush(stderr);
                pid_t pid = fork();
                if (pid == 0) _exit(run_case(dir, s, aui_host_pcts[p], v2));
                int st = 0;
                waitpid(pid, &st, 0);
                ran++;
                if (!WIFEXITED(st) || WEXITSTATUS(st)) {
                    printf("FAIL case %s@%d%s (status 0x%x)\n", aui_host_scene_name(s),
                           aui_host_pcts[p], v2 ? ".v2" : "", st);
                    failed++;
                }
            }
    printf("%s rendered %d case(s), %d failed\n", failed ? "FAIL" : "ok", ran, failed);
    return failed || !ran ? 1 : 0;
}
