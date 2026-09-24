/* The menu blob parser (c/kernel/gui/menu_blob.h) -- the kernel runs it on
 * bytes an app hands it, so it is tested as an attack surface:
 *
 *   ROUND TRIP   a valid blob parses back to exactly what was written
 *   PREFIXES     EVERY strict prefix of a valid blob is refused
 *   CORPUS       each rule of the format, broken one at a time, is refused
 *                with the reason named (checked by return code, never by an
 *                ASan abort -- rule 5)
 *   FUZZ         20,000 random blobs: only 0 or -1, and (under ASan in the
 *                gate) no out-of-bounds read
 *
 * The control (-DMB_PARSE_NEGCTL) accepts an over-long menu title -- the
 * parser silently truncating instead of refusing -- and must fail. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "menu_blob.h"

static int checks, fails;
static void check(int ok, const char *what)
{ checks++; if (!ok) { fails++; printf("FAIL %s\n", what); } }

static uint8_t buf[MB_BLOB_MAX + 64];
static int blen;
static void put(int c) { buf[blen++] = (uint8_t)c; }
static void puts_(const char *s) { put((int)strlen(s)); for (; *s; s++) put(*s); }
static void begin(int n) { blen = 0; put('M'); put(1); put(n); put(0); }
static void item(int id, int flags, int key, const char *label)
{ put(id & 255); put(id >> 8); put(flags); put(key); if (label) puts_(label); else put(0); }

static void valid(void)
{
    begin(2);
    puts_("File"); put(3);
    item(1, 0, 'N', "New"); item(0, MB_SEP, 0, 0); item(513, MB_DISABLED | MB_CHECKED, 0, "Revert");
    puts_("Edit"); put(1);
    item(7, 0, 'A', "Select All");
}

static int refused(const char *why_expected)
{
    struct mb_set s; const char *why;
    int r = mb_parse(buf, blen, &s, &why);
    return r == -1 && (!why_expected || !strcmp(why, why_expected));
}

int main(void)
{
    struct mb_set s; const char *why;
    valid();
    int r = mb_parse(buf, blen, &s, &why);
    check(r == 0, "ROUND TRIP: the valid blob parses");
    check(s.n == 2 && !strcmp(s.m[0].title, "File") && s.m[0].n == 3 && !strcmp(s.m[1].title, "Edit"),
          "ROUND TRIP: menus and titles");
    check(s.m[0].it[0].id == 1 && s.m[0].it[0].key == 'N' && !strcmp(s.m[0].it[0].label, "New") &&
          (s.m[0].it[1].flags & MB_SEP) && s.m[0].it[2].id == 513 &&
          s.m[0].it[2].flags == (MB_DISABLED | MB_CHECKED) && !strcmp(s.m[1].it[0].label, "Select All"),
          "ROUND TRIP: items, ids, flags, keys, labels");

    int full = blen, bad = 0;
    for (int k = 0; k < full; k++) { blen = k; if (!refused(0)) bad++; }
    check(bad == 0, "PREFIXES: every strict prefix of a valid blob is refused");

    valid(); buf[0] = 'X';            check(refused("magic"), "CORPUS: wrong magic");
    valid(); buf[1] = 2;              check(refused("version"), "CORPUS: unknown version");
    valid(); buf[3] = 1;              check(refused("reserved byte"), "CORPUS: reserved byte set");
    begin(0);                         check(refused("menu count"), "CORPUS: zero menus");
    begin(7);                         check(refused("menu count"), "CORPUS: seven menus");
    begin(1); puts_("ABCDEFGHIJKLMNOPQRSTUVWXY"); put(1); item(1, 0, 0, "x");
                                      check(refused("title length"), "CORPUS: a 25-byte title is refused, not truncated");
    begin(1); puts_("Ti\ttle"); put(1); item(1, 0, 0, "x");
                                      check(refused("title text"), "CORPUS: control character in a title");
    begin(1); puts_("File"); put(25); for (int i = 0; i < 25; i++) item(i, 0, 0, "x");
                                      check(refused("item count"), "CORPUS: 25 items");
    begin(1); puts_("File"); put(1); item(1, 0, 0, "0123456789012345678901234567890123456789X");
                                      check(refused("label length"), "CORPUS: a 41-byte label");
    begin(1); puts_("File"); put(1); item(1, 0, 0, "bad\x01");
                                      check(refused("label text"), "CORPUS: control character in a label");
    begin(1); puts_("File"); put(1); item(1, MB_SEP, 0, "sep");
                                      check(refused("separator with a label"), "CORPUS: a separator with a label");
    begin(1); puts_("File"); put(1); item(1, 0x80, 0, "x");
                                      check(refused("item flags"), "CORPUS: unknown flag bits");
    begin(1); puts_("File"); put(1); item(1, 0, 7, "x");
                                      check(refused("shortcut key"), "CORPUS: a control character as the shortcut");
    valid(); put(0);                  check(refused("trailing bytes"), "CORPUS: trailing bytes");

    /* THE BUILDER writes what the parser reads -- and refuses what it would. */
    struct mb_builder *b = malloc(sizeof *b);
    mb_begin(b);
    mb_menu(b, "File"); mb_item(b, 19, 0, 'S', "Save"); mb_sep(b); mb_item(b, 300, MB_CHECKED, 0, "Wrap");
    mb_menu(b, "Edit"); mb_item(b, 1, 0, 'A', "Select All");
    int bl = mb_end(b);
    r = bl > 0 ? mb_parse(b->buf, bl, &s, &why) : -1;
    check(r == 0 && s.n == 2 && s.m[0].n == 3 && s.m[0].it[0].id == 19 && s.m[0].it[0].key == 'S' &&
          (s.m[0].it[1].flags & MB_SEP) && s.m[0].it[2].id == 300 && !strcmp(s.m[1].it[0].label, "Select All"),
          "BUILDER: a built blob parses back to what was built");
    mb_begin(b); mb_menu(b, "File"); for (int i = 0; i < 25; i++) mb_item(b, i, 0, 0, "x");
    check(mb_end(b) == -1, "BUILDER: a 25th item is an error, not a truncation");
    mb_begin(b); mb_item(b, 1, 0, 0, "orphan");
    check(mb_end(b) == -1, "BUILDER: an item before any menu is an error");
    mb_begin(b); for (int i = 0; i < 7; i++) { mb_menu(b, "M"); mb_item(b, 1, 0, 0, "x"); }
    check(mb_end(b) == -1, "BUILDER: a seventh menu is an error");
    free(b);

    srand(4242);
    int odd = 0;
    for (int t = 0; t < 20000; t++) {
        int n = rand() % 300;
        for (int i = 0; i < n; i++) buf[i] = (uint8_t)rand();
        if (n > 3 && rand() % 2) { buf[0] = 'M'; buf[1] = 1; buf[2] = (uint8_t)(1 + rand() % 6); buf[3] = 0; }
        int rr = mb_parse(buf, n, &s, &why);
        if (rr != 0 && rr != -1) odd++;
    }
    check(odd == 0, "FUZZ: 20,000 random blobs return only 0 or -1");

    printf("menu-blob: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
