#ifndef MENU_BLOB_H
#define MENU_BLOB_H

/* An app's menus, as SYS_GUI_MENU_SET carries them ("Fluid" P5).
 *
 * Until this existed the menu bar was three string constants in the kernel
 * ("LogitOS", "File", "View") and no app could put a command there. Now the
 * focused app's own menus follow the kernel's LogitOS menu, and choosing an
 * item sends that app EV_MENU with the item's id.
 *
 * THE FORMAT (bytes, little-endian, no padding):
 *
 *   'M' 1 nmenus 0                       header: magic, version, count, reserved
 *   per menu:  tlen title[tlen] nitems
 *   per item:  id_lo id_hi flags key llen label[llen]
 *
 *   flags  MB_SEP (a separator: no label, never fires), MB_DISABLED, MB_CHECKED
 *   key    the Cmd shortcut, drawn at the right ("S" -> Cmd+S), 0 for none.
 *          NOT only a hint: Cmd+<key> with no system meaning, pressed while
 *          this window has the keyboard, sends the same EV_MENU a click does
 *          (wm.c menu_key), so the app handles each command once, by id.
 *
 * Limits are part of the format, checked here, and a blob that breaks one is
 * REFUSED WHOLE with the reason -- never truncated to the part that fitted, a
 * menu missing its last items being a menu that lies about what the app can do.
 *
 * Pure and header-only: the kernel parses with it, make test-menu-blob drives it
 * on the host over a corpus of malformed blobs, and aui builds blobs with the
 * same constants. */

#include <stdint.h>

#define MB_MAGIC     'M'
#define MB_VERSION   1
#define MB_MAX_MENUS 6
#define MB_MAX_ITEMS 24
#define MB_TITLE_MAX 24
#define MB_LABEL_MAX 40
#define MB_BLOB_MAX  4096

#define MB_SEP       0x01
#define MB_DISABLED  0x02
#define MB_CHECKED   0x04

struct mb_item {
    uint16_t id;
    uint8_t  flags;
    char     key;
    char     label[MB_LABEL_MAX + 1];
};
struct mb_menu {
    char title[MB_TITLE_MAX + 1];
    int  n;
    struct mb_item it[MB_MAX_ITEMS];
};
struct mb_set {
    int n;
    struct mb_menu m[MB_MAX_MENUS];
};

/* Text in a blob is shown on the menu bar: printable ASCII or UTF-8 bytes,
 * never a control character (which the painter would render as a box and a
 * terminal log would interpret). */
static inline int mb_text_ok(const uint8_t *s, int n)
{
    for (int i = 0; i < n; i++) if (s[i] < 32 || s[i] == 127) return 0;
    return 1;
}

/* 0 and *out filled, or -1 and *why names the rule broken. */
static inline int mb_parse(const uint8_t *b, int len, struct mb_set *out, const char **why)
{
#define MB_NEED(k) do { if (p + (k) > len) { *why = "truncated"; return -1; } } while (0)
    int p = 0;
    *why = "";
    if (len < 4 || len > MB_BLOB_MAX) { *why = "size"; return -1; }
    if (b[0] != MB_MAGIC) { *why = "magic"; return -1; }
    if (b[1] != MB_VERSION) { *why = "version"; return -1; }
    int nm = b[2];
    if (b[3] != 0) { *why = "reserved byte"; return -1; }
    if (nm < 1 || nm > MB_MAX_MENUS) { *why = "menu count"; return -1; }
    p = 4;
    out->n = nm;
    for (int m = 0; m < nm; m++) {
        struct mb_menu *mm = &out->m[m];
        MB_NEED(1);
        int tl = b[p++];
#ifndef MB_PARSE_NEGCTL
        if (tl < 1 || tl > MB_TITLE_MAX) { *why = "title length"; return -1; }
#endif
        MB_NEED(tl);
        if (!mb_text_ok(b + p, tl)) { *why = "title text"; return -1; }
        for (int i = 0; i < tl && i < MB_TITLE_MAX; i++) mm->title[i] = (char)b[p + i];
        mm->title[tl < MB_TITLE_MAX ? tl : MB_TITLE_MAX] = 0;
        p += tl;
        MB_NEED(1);
        int ni = b[p++];
        if (ni < 1 || ni > MB_MAX_ITEMS) { *why = "item count"; return -1; }
        mm->n = ni;
        for (int i = 0; i < ni; i++) {
            struct mb_item *it = &mm->it[i];
            MB_NEED(5);
            it->id = (uint16_t)(b[p] | b[p + 1] << 8);
            it->flags = b[p + 2];
            it->key = (char)b[p + 3];
            int ll = b[p + 4];
            p += 5;
            if (it->flags & ~(MB_SEP | MB_DISABLED | MB_CHECKED)) { *why = "item flags"; return -1; }
            if (it->key && (it->key < 33 || it->key > 126)) { *why = "shortcut key"; return -1; }
            if (it->flags & MB_SEP) {
                if (ll) { *why = "separator with a label"; return -1; }
            } else if (ll < 1 || ll > MB_LABEL_MAX) { *why = "label length"; return -1; }
            MB_NEED(ll);
            if (!mb_text_ok(b + p, ll)) { *why = "label text"; return -1; }
            for (int k = 0; k < ll; k++) it->label[k] = (char)b[p + k];
            it->label[ll] = 0;
            p += ll;
        }
    }
    if (p != len) { *why = "trailing bytes"; return -1; }
    return 0;
#undef MB_NEED
}


/* THE BUILDER -- the only way an app should make a blob, so the writer and
 * the parser above cannot disagree about the format. Errors are sticky: a
 * builder that overflowed any limit returns -1 from mb_end, and an app that
 * publishes anyway gets the kernel's refusal rather than a quiet truncation. */
struct mb_builder { uint8_t buf[MB_BLOB_MAX]; int len, err, count_at; };
static inline void mb_put(struct mb_builder *b, int c)
{
    if (b->len >= MB_BLOB_MAX) { b->err = 1; return; }
    b->buf[b->len++] = (uint8_t)c;
}
static inline void mb_str(struct mb_builder *b, const char *s, int max)
{
    int n = 0; while (s && s[n]) n++;
    if (n > max) { b->err = 1; n = max; }
    mb_put(b, n);
    for (int i = 0; i < n; i++) mb_put(b, (uint8_t)s[i]);
}
static inline void mb_begin(struct mb_builder *b)
{
    b->len = b->err = 0; b->count_at = -1;
    mb_put(b, MB_MAGIC); mb_put(b, MB_VERSION); mb_put(b, 0); mb_put(b, 0);
}
static inline void mb_menu(struct mb_builder *b, const char *title)
{
    if (b->buf[2] >= MB_MAX_MENUS) b->err = 1;
    b->buf[2]++;
    mb_str(b, title, MB_TITLE_MAX);
    b->count_at = b->len;
    mb_put(b, 0);
}
static inline void mb_item(struct mb_builder *b, int id, int flags, char key, const char *label)
{
    if (b->count_at < 0 || b->count_at >= b->len) { b->err = 1; return; }
    if (b->buf[b->count_at] >= MB_MAX_ITEMS) b->err = 1;
    b->buf[b->count_at]++;
    mb_put(b, id & 255); mb_put(b, (id >> 8) & 255); mb_put(b, flags); mb_put(b, (uint8_t)key);
    mb_str(b, (flags & MB_SEP) ? "" : label, MB_LABEL_MAX);
}
static inline void mb_sep(struct mb_builder *b) { mb_item(b, 0, MB_SEP, 0, ""); }
static inline int mb_end(struct mb_builder *b) { return b->err ? -1 : b->len; }

#endif
