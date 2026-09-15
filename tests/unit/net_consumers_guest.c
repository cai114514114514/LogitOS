/* SPDX-License-Identifier: MIT */
#include "clib.h"

/* sha256 of the HOST fixture's 300123-byte i%251 sequence (hashlib). Keep
 * outputs in a pipe: shell echoing an expected hash is not a measurement. */
static const char sha[] = "1844c05aa2a59cafd844d6a225f8f702e675cf2bed64220578250c5ff102f009";
static const char bad[] = "0000000000000000000000000000000000000000000000000000000000000000";
static char output[4096], url[800];
static int checks, failures;
static int contains(const char *s, const char *part)
{ for (; *s; s++) if (!c_strncmp(s, part, c_strlen(part))) return 1; return 0; }
static void check(int ok, const char *name)
{ checks++; outs(ok ? "NET_CASE_PASS " : "NET_CASE_FAIL "); outs(name); outc('\n'); if (!ok) failures++; }
static int command(char **args, int expected, const char *marker)
{
    int fds[2]; if (sys_pipe(fds)) return 0;
    int pid = sys_fork();
    if (pid == 0) {
        sys_close(fds[0]); sys_dup2(fds[1], 1); sys_dup2(fds[1], 2); sys_close(fds[1]);
        char *env[] = {0}; sys_execve(args[0], args, env); app_exit(97); return 0;
    }
    sys_close(fds[1]);
    if (pid < 0) { sys_close(fds[0]); return 0; }
    int total = 0, n; char block[256];
    while ((n = sys_read(fds[0], block, sizeof block)) > 0) {
        for (int i = 0; i < n && total < (int)sizeof output - 1; i++) output[total++] = block[i];
    }
    output[total] = 0; sys_close(fds[0]);
    int status = -1;
    if (sys_waitpid(pid, &status) != pid) return 0;
    int ok = n == 0 && status == expected && contains(output, marker);
    if (!ok) { outs("status="); outn(status); outc('\n'); outs(output); }
    return ok;
}
static char *endpoint(const char *base, const char *path)
{
    c_strcpy(url, base, sizeof url);
    c_strcpy(url + c_strlen(url), path, (int)sizeof url - c_strlen(url));
    return url;
}
static int bytes_match(const char *path, int expected)
{
    int fd = sys_open(path, 0), off = 0, n; unsigned char buf[997];
    if (fd < 0) return 0;
    int ok = 1;
    while ((n = sys_read(fd, buf, sizeof buf)) > 0) {
        for (int i = 0; i < n; i++) if (buf[i] != (unsigned)(((off+i)%(expected>300123?300123:251))%251)) ok = 0;
        off += n;
    }
    if (sys_close(fd) < 0 || n < 0) ok = 0;
    return ok && off == expected;
}
int main(int argc, char **argv)
{
    if (argc < 2) return 9;
    char *net = "/bin/net", *dest = "/net-download.bin";
    const char *paths[] = {"/data", "/chunked", "/redirect", "/query"};
    for (int i = 0; i < 4; i++) {
        char *args[] = {net, "get", endpoint(argv[1], paths[i]), "sha256", (char *)sha, 0};
        check(command(args, 0, "checksum verified") && contains(output, "http bytes 300123"), paths[i]);
    }
    char *save[] = {net, "save", endpoint(argv[1], "/chunked"), dest, "sha256", (char *)sha, 0};
    check(command(save, 0, "saved /net-download.bin"), "verified-save");
    check(bytes_match(dest, 300123), "saved-file-exact-bytes");
    char *verify[] = {net, "verify", "sha256", (char *)sha, dest, 0};
    check(command(verify, 0, "verified /net-download.bin"), "local-verify");
    char *sum[] = {net, "checksum", "sha256", dest, 0};
    check(command(sum, 0, sha), "local-checksum");
    const char *b3 = "d00278ae47eb27b34faecf67b4fe263f82d5412916c1ffd97c8cb7fb814b8444";
    char *b3save[] = {net, "save", endpoint(argv[1], "/b3"), "/net-b3.bin", "blake3", (char *)b3, 0};
    check(command(b3save, 0, "checksum verified"), "blake3-download");
    check(bytes_match("/net-b3.bin", 1025), "blake3-file-exact-bytes");
    char *b3sum[] = {net, "checksum", "blake3", "/net-b3.bin", 0};
    check(command(b3sum, 0, b3), "blake3-local-file");
    char *wrong[] = {net, "save", endpoint(argv[1], "/data"), dest, "sha256", (char *)bad, 0};
    check(command(wrong, 2, "checksum mismatch"), "wrong-digest-refused");
    check(bytes_match(dest, 300123), "wrong-digest-kept-destination");
    const char *failpaths[] = {"/truncated", "/missing", "/partial", "/compressed"};
    for (int i = 0; i < 4; i++) {
        char *args[] = {net, "save", endpoint(argv[1], failpaths[i]), dest, "sha256", (char *)sha, 0};
        check(command(args, 1, "net: ") && bytes_match(dest, 300123), failpaths[i]);
    }
    char *auto1[]={net,"download",endpoint(argv[1],"/attachment"),"sha256",(char *)sha,0};
    check(command(auto1,0,"saved /download/下载.bin")&&bytes_match("/download/下载.bin",300123),"automatic-download-exact");
    char *auto2[]={net,"download",endpoint(argv[1],"/attachment"),0};
    check(command(auto2,0,"saved /download/下载 (2).bin")&&bytes_match("/download/下载 (2).bin",300123)&&bytes_match("/download/下载.bin",300123),"automatic-collision-preserves-first");
    char *empty[]={net,"download",endpoint(argv[1],"/empty"),0};
    check(command(empty,0,"saved /download/empty")&&bytes_match("/download/empty",0),"automatic-empty-file");
    char *large[]={net,"save",endpoint(argv[1],"/large-save"),"/large-download.bin","sha256","1c3b1160db224f4b1cb9b074aa4f467f1cacc34837b46d328c96c195112a3cb9",0};
    check(command(large,0,"saved /large-download.bin")&&bytes_match("/large-download.bin",9437184),"nine-MiB-saved-exact");
    if (argc > 2) {
        char *tls[] = {net, "get", endpoint(argv[2], "/data"), "sha256", (char *)sha, 0};
        check(command(tls, 0, "checksum verified") && contains(output, "http bytes 300123"), "https-x448-download");
    }
    outs("NET_CONSUMERS_DONE checks="); outn(checks); outs(" failures="); outn(failures); outc('\n');
    return failures ? 1 : 0;
}
