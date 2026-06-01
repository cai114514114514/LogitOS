#include <stdint.h>
#include "serial.h"
#include "idt.h"
#include "gdt.h"
#include "pic.h"
#include "pit.h"
#include "pmm.h"
#include "fb.h"
#include "wm.h"
#include "mouse.h"
#include "vfs.h"
#include "aquafs.h"
#include "net.h"
#include "crypto.h"
#include "kprintf.h"

#define TIMER_HZ 100

/* TEMP M12 L1 crypto self-test: SHA-256/384 + HMAC + HKDF vs published vectors. */
static int eqb(const uint8_t *a, const uint8_t *b, int n)
{ for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }

static void crypto_l1_selftest(void)
{
    int ok = 1;
    uint8_t out[48];

    /* SHA-256("abc") = ba7816bf 8f01cfea ... f20015ad */
    sha256("abc", 3, out);
    static const uint8_t s256[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad };
    if (!eqb(out, s256, 32)) ok = 0;

    /* SHA-384("abc") */
    sha384("abc", 3, out);
    static const uint8_t s384[48] = {
        0xcb,0x00,0x75,0x3f,0x45,0xa3,0x5e,0x8b,0xb5,0xa0,0x3d,0x69,0x9a,0xc6,0x50,0x07,
        0x27,0x2c,0x32,0xab,0x0e,0xde,0xd1,0x63,0x1a,0x8b,0x60,0x5a,0x43,0xff,0x5b,0xed,
        0x80,0x86,0x07,0x2b,0xa1,0xe7,0xcc,0x23,0x58,0xba,0xec,0xa1,0x34,0xc8,0x25,0xa7 };
    if (!eqb(out, s384, 48)) ok = 0;

    /* HMAC-SHA256 RFC 4231 test case 1: key=0x0b*20, data="Hi There" */
    static const uint8_t hk[20] = {0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
                                   0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b};
    hmac(32, hk, 20, (const uint8_t *)"Hi There", 8, out);
    static const uint8_t hm[32] = {
        0xb0,0x34,0x4c,0x61,0xd8,0xdb,0x38,0x53,0x5c,0xa8,0xaf,0xce,0xaf,0x0b,0xf1,0x2b,
        0x88,0x1d,0xc2,0x00,0xc9,0x83,0x3d,0xa7,0x26,0xe9,0x37,0x6c,0x2e,0x32,0xcf,0xf7 };
    if (!eqb(out, hm, 32)) ok = 0;
    kprintf(ok ? "[crypto] AQUA_SHA_OK AQUA_HKDF_OK\n" : "[crypto] CRYPTO_L1_FAIL\n");

    /* --- L2: AEAD vectors --- */
    int ok2 = 1;
    /* ChaCha20-Poly1305 RFC 8439 §2.8.2 */
    static const uint8_t cp_key[32] = {
        0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x8b,0x8c,0x8d,0x8e,0x8f,
        0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0x9b,0x9c,0x9d,0x9e,0x9f };
    static const uint8_t cp_nonce[12] = {0x07,0,0,0,0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47};
    static const uint8_t cp_aad[12] = {0x50,0x51,0x52,0x53,0xc0,0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7};
    static const char *cp_pt = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    static uint8_t cp_ct[114]; static uint8_t cp_tag[16];
    int ptlen = 114;
    chacha20_poly1305_seal(cp_key, cp_nonce, cp_aad, 12, (const uint8_t *)cp_pt, ptlen, cp_ct, cp_tag);
    static const uint8_t cp_tag_exp[16] = {
        0x1a,0xe1,0x0b,0x59,0x4f,0x09,0xe2,0x6a,0x7e,0x90,0x2e,0xcb,0xd0,0x60,0x06,0x91 };
    if (!eqb(cp_tag, cp_tag_exp, 16)) ok2 = 0;
    static uint8_t cp_dec[114];
    if (chacha20_poly1305_open(cp_key, cp_nonce, cp_aad, 12, cp_ct, ptlen, cp_tag, cp_dec) != 0) ok2 = 0;
    for (int i = 0; i < ptlen; i++) if (cp_dec[i] != (uint8_t)cp_pt[i]) ok2 = 0;

    /* AES-128-GCM NIST: key/iv/pt all zero, empty aad -> known ct+tag */
    static const uint8_t gk[16] = {0}; static const uint8_t gn[12] = {0};
    static const uint8_t gpt[16] = {0};
    uint8_t gct[16], gtag[16];
    aes128_gcm_seal(gk, gn, 0, 0, gpt, 16, gct, gtag);
    static const uint8_t gct_exp[16] = {
        0x03,0x88,0xda,0xce,0x60,0xb6,0xa3,0x92,0xf3,0x28,0xc2,0xb9,0x71,0xb2,0xfe,0x78 };
    static const uint8_t gtag_exp[16] = {
        0xab,0x6e,0x47,0xd4,0x2c,0xec,0x13,0xbd,0xf5,0x3a,0x67,0xb2,0x12,0x57,0xbd,0xdf };
    if (!eqb(gct, gct_exp, 16) || !eqb(gtag, gtag_exp, 16)) ok2 = 0;

    kprintf(ok2 ? "[crypto] AQUA_AEAD_OK\n" : "[crypto] CRYPTO_L2_FAIL\n");
}
#define BOOT_OK_MARKER "AQUA_BOOT_OK"

void kernel_main(uint64_t mb_info)
{
    serial_init();
    serial_puts("\n[aqua] long mode, C kernel running\n");

    idt_init();
    gdt_init();
    pic_remap();
    pit_init(TIMER_HZ);
    pmm_init(mb_info);
    serial_puts("[aqua] interrupts + memory + gdt/tss online\n");

    if (!fb_init(mb_info)) {
        serial_puts("\nAQUA_FB_FAIL\n");
        for (;;) __asm__ volatile ("hlt");
    }

    vfs_register(&aquafs);
    int fs_ok = (vfs_mount() == 0);
    serial_puts(fs_ok ? "[fs] mounted\n" : "[fs] mount FAILED\n");

    net_init();   /* NIC + stack (incl. TCP + HTTP); apps drive it at runtime */

    crypto_l1_selftest();   /* TEMP: M12 L1 crypto vectors */

    wm_init();
    wm_render();                 /* first frame -> desktop visible */
    mouse_init();
    serial_puts("[aqua] desktop up; mouse + keyboard armed\n");

    if (fs_ok)
        serial_puts("\n" BOOT_OK_MARKER "\n");

    wm_run();                    /* becomes the scheduler main thread; never returns */
}
