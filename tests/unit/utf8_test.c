#include <stdio.h>
#include <stdint.h>
#include "utf8.h"
static int fail;
static void chk(const char *s, uint32_t want, int wlen){
    uint32_t cp; const char *p = utf8_next(s, &cp);
    if (cp != want || (int)(p - s) != wlen){
        printf("FAIL \"%s\": cp=%x len=%ld want %x/%d\n", s, cp, (long)(p - s), want, wlen);
        fail = 1;
    }
}
int main(void){
    chk("A", 0x41, 1);
    chk("\xc3\xa9", 0xE9, 2);              /* é */
    chk("\xe4\xbd\xa0", 0x4F60, 3);        /* 你 */
    chk("\xf0\x9f\x98\x80", 0x1F600, 4);   /* 😀 */
    chk("\xff", 0xFFFD, 1);                /* bad lead byte */
    chk("\xe4\x28", 0xFFFD, 1);            /* bad continuation -> 1 byte */
    printf(fail ? "SOME FAILED\n" : "ALL PASS\n");
    return fail;
}
