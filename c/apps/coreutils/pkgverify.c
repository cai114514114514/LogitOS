/* /bin/pkgverify -- "is this file from someone this machine trusts?"
 *
 * The whole user-visible surface of the trust model. It reads a .lpk, verifies
 * it against the signer keys compiled into the kernel image (see
 * c/crypto/trust/pkgsig.h for why they are compiled in and not read from a
 * file), and prints one line.
 *
 * It does NOT install anything. Installing is policy -- where does it go, what
 * if it is already there, who is allowed to ask -- and none of those questions
 * are safe to answer before this one is. When a package manager exists it calls
 * this code path first and does its own work second.
 *
 *   pkgverify <file.lpk>            verify against the built-in signers
 *   pkgverify --any <file.lpk>      accept any valid self-signature; prints the
 *                                   key. This is how you tell "the file is
 *                                   corrupt" apart from "the file is fine and I
 *                                   do not know who signed it", which are
 *                                   different problems with different fixes.
 *   pkgverify --extract <f> <dst>   verify, then write the payload to <dst>.
 *                                   The order is the point: nothing is written
 *                                   unless the signature checked out first.
 *   pkgverify --roots               list the trusted signers
 *
 * Exit status: 0 verified, 1 usage/IO, 3 refused. */
#include "clib.h"
#include "pkgsig.h"
#include <stddef.h>

/* -ffreestanding still lets the compiler emit calls to these for structure
 * assignment and array copies (ge_scalarmult's `*r = acc`, sha*_update's
 * buffering), and CLI programs link no libc. Every other coreutil gets away
 * without them because none of them copy a struct; this one links crypto. */
void *memcpy(void *d, const void *s, size_t n)
{ unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, size_t n)
{ unsigned char *a = d; while (n--) *a++ = (unsigned char)c; return d; }

/* One package at a time, in .bss: a CLI process has a 32 KiB stack and the
 * whole file has to be contiguous for the digest. 1 MiB bounds what a package
 * can be here, which is a real limit and is stated rather than discovered --
 * .aex files on this disk run to ~1 MiB (the browser), so this is exactly one
 * of those and no more. */
#define PKG_MAX (1024 * 1024)
static unsigned char buf[PKG_MAX];

static void puthex(const unsigned char *b, int n)
{
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) { outc(H[b[i] >> 4]); outc(H[b[i] & 15]); }
}

static const char *errname(int e)
{
    switch (e) {
    case LPK_E_SHORT:     return "truncated";
    case LPK_E_MAGIC:     return "not a package (bad magic)";
    case LPK_E_VERSION:   return "unknown format version";
    case LPK_E_FIELD:     return "malformed header field";
    case LPK_E_DIGEST:    return "payload does not match its digest";
    case LPK_E_SIG:       return "SIGNATURE INVALID";
    case LPK_E_UNTRUSTED: return "signature valid, signer NOT TRUSTED";
    default:              return "refused";
    }
}

static int readall(const char *path, int *outlen)
{
    int fd = sys_open(path, 0);
    if (fd < 0) return -1;
    int n = 0;
    for (;;) {
        int k = sys_read(fd, buf + n, PKG_MAX - n);
        if (k <= 0) break;
        n += k;
        if (n >= PKG_MAX) break;
    }
    sys_close(fd);
    *outlen = n;
    return 0;
}

int main(int argc, char **argv)
{
    int any = 0, i = 1;
    const char *extract = 0;

    if (argc > 1 && c_streq(argv[1], "--roots")) {
        outs("trusted package signers, compiled into this kernel:\n");
        for (int r = 0; r < pkg_root_count(); r++) {
            outs("  "); outs(pkg_root_name(r)); outs("  ");
            puthex(pkg_root_key(r), 32); outs("\n");
        }
        outn(pkg_root_count()); outs(" signer(s)\n");
        return 0;
    }
    while (i < argc && argv[i][0] == '-') {
        if (c_streq(argv[i], "--any")) { any = 1; i++; }
        else if (c_streq(argv[i], "--extract")) { i++; if (i + 1 >= argc) break; extract = argv[i+1]; i++; }
        else break;
    }
    if (i >= argc) {
        errs("usage: pkgverify [--any] [--extract <dst>] <file.lpk>\n"
             "       pkgverify --roots\n");
        return 1;
    }

    int n = 0;
    if (readall(argv[i], &n) != 0) { errs("pkgverify: cannot read "); errs(argv[i]); errs("\n"); return 1; }

    struct lpk pk;
    int r = lpk_verify(buf, (unsigned long)n, &pk, any);
    if (r != LPK_OK) {
        outs("PKG_REFUSED "); outn(r); outs(" "); outs(errname(r)); outs("\n");
        return 3;
    }

    outs("PKG_OK name="); outs(pk.name);
    outs(" bytes="); outn((long)pk.payload_len);
    outs(" signer=");
    if (pk.root_index >= 0) outs(pkg_root_name(pk.root_index));
    else { outs("untrusted:"); puthex(pk.signer, 32); }
    outs("\n");

    if (extract) {
        /* Only now. The whole reason this program exists is that the write
         * happens after the verdict and not before it. */
        int fd = sys_open(extract, 0x41 /* O_WRONLY|O_CREAT */);
        if (fd < 0) { errs("pkgverify: cannot create output\n"); return 1; }
        long off = 0;
        while (off < (long)pk.payload_len) {
            int k = sys_write(fd, pk.payload + off, (int)(pk.payload_len - off));
            if (k <= 0) break;
            off += k;
        }
        sys_close(fd);
        outs("PKG_EXTRACTED "); outn(off); outs("\n");
    }
    return 0;
}
