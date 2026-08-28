/* CRC-32C (Castagnoli) known-answer test.
 *
 * VECTORS AND THEIR SOURCE, named individually because a CRC KAT is exactly
 * the kind of thing that is silently wrong in two directions at once (see
 * crc32c.h and crc32c.c for both traps):
 *
 *  1-4. RFC 3720 ("iSCSI") Appendix B.4, "CRC Examples" -- fetched verbatim
 *     from https://www.rfc-editor.org/rfc/rfc3720.txt on 2026-08-28 (page
 *     217-218 of the printed RFC): 32 zero bytes, 32 bytes of 0xff, 32 bytes
 *     incrementing 00..1f, 32 bytes decrementing 1f..00. The RFC prints each
 *     result "as transmitted" -- see crc32c.h's header for exactly what that
 *     means and why this file byte-swaps before comparing rather than after.
 *  5. The same appendix's iSCSI SCSI-Read(10) Command PDU example, 48 bytes,
 *     also "as transmitted". (The RFC's B.4 has exactly ONE PDU example, not
 *     two -- the incrementing/decrementing pair are the other two 32-byte
 *     cases, not PDUs. Said here so nobody goes looking for a sixth vector.)
 *  6. The RevEng "CRC Catalogue" check value for CRC-32/ISCSI: ASCII
 *     "123456789" -> 0xe3069283, in REGISTER form (no byte swap) -- this is
 *     the value crc32c.h documents as what crc32c() returns directly, and it
 *     is the value essentially every non-iSCSI CRC-32C implementation quotes
 *     (ext4, Btrfs, the SSE4.2 CRC32 instruction, Python's `crc32c` package).
 *
 * INDEPENDENTLY CONFIRMED, not just quoted: all six values were cross-checked
 * against Python's `crc32c` package (a genuine independent implementation --
 * PyPI, C-extension over Google's crc32c library, not this tree's code) on
 * the same host that wrote this file. `crc32c.crc32c(bytes(32))` returned
 * 0x8a9136aa; reversing its four bytes gives aa 36 91 8a, the RFC's printed
 * value. Same story for the other three 32-byte cases and the PDU. This is
 * belt-and-suspenders: the RFC text is the authority, the Python package is
 * a second, structurally unrelated implementation that happens to agree.
 *
 * NO OPENSSL DIFFERENTIAL: openssl has no crc32c command or EVP entry (it
 * ships CRC-32 only via `openssl dgst` for some builds, never Castagnoli),
 * so there is nothing to differential-test against here. Said plainly rather
 * than fabricated: this gate is KAT-only, backed by an external spec (RFC
 * 3720) and one independent external implementation (PyPI `crc32c`), not by
 * a differential binary the way test-mlkem-openssl or crypto_diff_test are.
 *
 * SECOND ORACLE, A DIFFERENT SHAPE THAN THE IMPLEMENTATION: crc32c.c is
 * table-driven (256-entry lookup, one iteration per BYTE). ref_crc32c() below
 * is bit-at-a-time (32 register... no: 8 iterations per byte, one per BIT,
 * no table) -- the same reflected construction written out longhand instead
 * of pre-folded into a table. It is not a copy of crc32c.c's table-build
 * loop crc32c.c happened to also use once (to generate the table); it is the
 * whole per-message algorithm run without ever materialising a table, so a
 * bug that survives table generation (e.g. a wrong per-byte fold sequence in
 * crc32c_update) is not guaranteed to survive here too. Every KAT is checked
 * against BOTH crc32c() and ref_crc32c(); they must agree with each other
 * AND with the external vector for a pass.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "crc32c.h"

static int pass, fail;
static void ck(int cond, const char *what)
{
    if (cond) { pass++; }
    else { fail++; printf("FAIL: %s\n", what); }
}

/* Bit-at-a-time reflected CRC-32C, no table -- the second, structurally
 * different oracle described above. */
static uint32_t ref_crc32c(const void *data, size_t n)
{
    const unsigned char *p = (const unsigned char *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (0x82F63B78u ^ (crc >> 1)) : (crc >> 1);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* Reverse the byte order of a 32-bit register value -- turns crc32c()'s
 * register form into RFC 3720's "as transmitted" printed form. See
 * crc32c.h's header comment before reusing this pattern anywhere real. */
static uint32_t byteswap32(uint32_t v)
{
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}

struct vec {
    const char *name;
    const unsigned char *data;
    size_t len;
    uint32_t as_transmitted; /* RFC 3720's printed bytes, as one big-endian word */
};

int main(void)
{
    static unsigned char zeros[32];
    static unsigned char ones[32];
    static unsigned char incr[32];
    static unsigned char decr[32];
    for (int i = 0; i < 32; i++) { ones[i] = 0xFF; incr[i] = (unsigned char)i; decr[i] = (unsigned char)(31 - i); }

    /* RFC 3720 Appendix B.4's SCSI Read(10) Command PDU, 48 bytes, exactly as
     * printed (four bytes per row, 12 rows). */
    static const unsigned char pdu[48] = {
        0x01,0xc0,0x00,0x00,  0x00,0x00,0x00,0x00,  0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,  0x14,0x00,0x00,0x00,  0x00,0x00,0x04,0x00,
        0x00,0x00,0x00,0x14,  0x00,0x00,0x00,0x18,  0x28,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,  0x02,0x00,0x00,0x00,  0x00,0x00,0x00,0x00,
    };

    static const struct vec vecs[] = {
        { "RFC3720 B.4: 32 zero bytes",        zeros, 32, 0xaa36918aUL },
        { "RFC3720 B.4: 32 bytes of 0xff",     ones,  32, 0x43aba862UL },
        { "RFC3720 B.4: 32 bytes incr 00..1f", incr,  32, 0x4e79dd46UL },
        { "RFC3720 B.4: 32 bytes decr 1f..00", decr,  32, 0x5cdb3f11UL },
        { "RFC3720 B.4: SCSI Read(10) PDU",    pdu,   48, 0x563a96d9UL },
    };

    for (size_t i = 0; i < sizeof(vecs) / sizeof(vecs[0]); i++) {
        uint32_t got = crc32c(vecs[i].data, vecs[i].len);
        uint32_t got_ref = ref_crc32c(vecs[i].data, vecs[i].len);
        char msg[128];

        snprintf(msg, sizeof msg, "%s (crc32c vs ref_crc32c agree)", vecs[i].name);
        ck(got == got_ref, msg);

        /* Compare in REGISTER form: byte-swap the RFC's printed "as
         * transmitted" word back to a register value, rather than
         * byte-swapping our result to match the RFC's print order. Either
         * direction proves the same equality, but doing it this way means
         * the swap is visibly applied to the EXTERNAL constant, not folded
         * silently into what our function returns -- crc32c.h is explicit
         * that crc32c() never swaps on its own. */
        snprintf(msg, sizeof msg, "%s (register value)", vecs[i].name);
        ck(got == byteswap32(vecs[i].as_transmitted), msg);

        /* And the other direction too, spelled out separately so a reader
         * can see the RFC's own printed bytes reproduced without having to
         * mentally invert byteswap32(). */
        snprintf(msg, sizeof msg, "%s (as-transmitted bytes match RFC text)", vecs[i].name);
        ck(byteswap32(got) == vecs[i].as_transmitted, msg);
    }

    /* Incremental API: crc32c_update/crc32c_final split across an arbitrary
     * boundary must equal the one-shot call over the whole buffer -- the
     * property a KAT list never happens to exercise on its own. */
    {
        uint32_t whole = crc32c(pdu, sizeof pdu);
        uint32_t c = CRC32C_INIT;
        c = crc32c_update(c, pdu, 17);
        c = crc32c_update(c, pdu + 17, sizeof(pdu) - 17);
        ck(crc32c_final(c) == whole, "incremental update matches one-shot (split at 17)");

        c = CRC32C_INIT;
        for (size_t i = 0; i < sizeof pdu; i++)
            c = crc32c_update(c, pdu + i, 1);
        ck(crc32c_final(c) == whole, "incremental update matches one-shot (byte at a time)");
    }

    /* CRC catalogue check value (RevEng, CRC-32/ISCSI): ASCII "123456789",
     * REGISTER form, no byte swap -- see the file header for why this one is
     * NOT swapped while the RFC 3720 vectors above are. Independently
     * confirmed against Python's `crc32c` package. */
    {
        static const unsigned char check[] = "123456789";
        uint32_t got = crc32c(check, 9);
        ck(got == 0xe3069283UL, "CRC catalogue check value (\"123456789\")");
        ck(ref_crc32c(check, 9) == 0xe3069283UL, "CRC catalogue check value, bit-at-a-time oracle");
    }

    /* Empty input: init XOR final, no bytes folded in. Every reflected CRC
     * with init==final==0xFFFFFFFF returns 0 for the empty string; worth its
     * own line because it is the one input where crc32c_update never runs
     * its loop body at all. */
    {
        uint32_t got = crc32c(NULL, 0);
        ck(got == 0x00000000UL, "empty input");
    }

    printf("crc32c: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
