/* Compare the real painter's emitted glyph positions with the prefix advance
 * consumed by browser.c's caret, selection and mouse paths. Unequal advances
 * and a Wi pair adjustment stop byte-counting or whole-prefix reshaping from
 * masquerading as the painter's spacing. This is a host geometry gate; the
 * caret-geometry.html guest specimen covers native font/input integration.
 * With this stub and letter-spacing:3px, the old first interior prefix was
 * x=11 while the actual painter emitted x=14; the shared advance returns 14.
 * The negative control must retain spaced paint and restore only raw metrics. */
#define main caret_advance_unused_main
#define text_measure caret_advance_unused_measure
#include "text_wiring_test.c"
#undef text_measure
#undef main

int text_measure(const char *s, int len, int px, int face)
{
    (void)px;
    if (len > 1024) return 0;
    int width = 0;
    for (int p = 0; p < len;) {
        unsigned char c = s[p];
        int size = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
        width += c >= 128 ? 16 : c == 'W' ? 11 : c == 'i' ? 4 : c == ' ' ? 5 : 8;
        if (face & LOGIT_FACE_BOLD) width++;
        if (p > 0 && s[p - 1] == 'W' && c == 'i') width -= 2;
        p += size;
    }
    return width;
}

static void specimen(const char *label, const char *value, const char *style)
{
    char html[1024], css[1024];
    snprintf(html, sizeof html, "<body><div id='probe'>%s</div></body>", value);
    snprintf(css, sizeof css, "body{margin:0;font-size:20px}#probe{width:700px;white-space:pre;%s}", style);
    struct node *root = dom_parse(html, (int)strlen(html));
    struct node *probe = dom_get_element_by_id(root->doc, "probe");
    CHECK(probe != 0, "advance fixture contains its real text node");
    if (!probe) { dom_free(root); return; }
    css_viewport(800, 400);
    css_apply(root, css, (int)strlen(css));
    css_extra_apply(root, css, (int)strlen(css));
    layout_page(root, 800);
    paint_nops = 0;
    browser_paint_scroll(0, 0, 800, 400, 0, 0);
    const struct item *items = layout_items();
    int matches = 0, interior = 0;
    printf("advance specimen %s\n", label);
    for (int i = 0; i < layout_count(); i++) {
        const struct item *e = &items[i];
        if (e->type != IT_TEXT || !e->node || e->node->parent != probe) continue;
        CHECK(browser_text_run_advance(e, 0) == 0, "empty prefix leaves the caret at run origin");
        for (int j = 0; j < paint_nops; j++) {
            const struct paintop *op = &paint_ops[j];
            uintptr_t start = (uintptr_t)e->text, ptr = (uintptr_t)op->text;
            if (op->kind != OP_TEXT || ptr < start || ptr >= start + e->len) continue;
            int offset = (int)(ptr - start);
            matches++;
            if (offset > 0) interior++;
            int caret = e->x + browser_text_run_advance(e, offset);
            if (caret != op->x) printf("advance mismatch specimen=%s byte=%d caret=%d painted=%d\n", label, offset, caret, op->x);
            CHECK(caret == op->x, "caret prefix equals actual painted segment position");
        }
        CHECK(browser_text_run_advance(e, e->len) == e->w, "caret End advance equals independently formatted run width");
        CHECK(browser_text_run_advance(e, e->len + 7) == browser_text_run_advance(e, e->len), "prefix beyond the item clamps to its final advance");
    }
    CHECK(matches > 0, "real painter emitted the measured specimen");
    if (style[0]) CHECK(interior > 0, "spacing specimen exercises interior painted boundaries");
    layout_free();
    dom_free(root);
}

/* A model response or minified inline can be one display run even when it is
 * much longer than the kernel text syscall's copy buffer.  Keep the malformed
 * apparatus out of this measurement: the three-byte character straddles the
 * first nominal 256-byte cut, so every emitted call also proves the shared cut
 * backed up to a UTF-8 boundary.  The host measurer below reproduces the real
 * SYS_TEXT_MEASURE refusal (>1024 -> 0); paint op lengths expose the otherwise
 * silent SYS_GUI_TEXT_RUN truncation before the request reaches the kernel. */
static void long_specimen(void)
{
    enum { PREFIX = 255, TAIL = 1100, VALUE_LEN = PREFIX + 3 + TAIL };
    char *value = malloc(VALUE_LEN + 1);
    char *html = malloc(VALUE_LEN + 96);
    CHECK(value != 0 && html != 0, "long-run fixture allocation succeeds");
    if (!value || !html) { free(value); free(html); return; }
    memset(value, 'a', PREFIX);
    value[PREFIX + 0] = (char)0xe4;
    value[PREFIX + 1] = (char)0xb8;
    value[PREFIX + 2] = (char)0xad;
    memset(value + PREFIX + 3, 'b', TAIL);
    value[VALUE_LEN] = 0;
    int hn = snprintf(html, VALUE_LEN + 96,
                      "<body><div id='long'>%s</div></body>", value);
    struct node *root = dom_parse(html, hn);
    struct node *probe = dom_get_element_by_id(root->doc, "long");
    CHECK(probe != 0 && probe->first_child != 0 &&
          probe->first_child->textlen == VALUE_LEN,
          "long-run fixture reaches the DOM without truncation");
    const char *sheet =
        "body{margin:0;font-size:20px}#long{width:18000px;white-space:pre}";
    css_viewport(20000, 400);
    css_apply(root, sheet, (int)strlen(sheet));
    css_extra_apply(root, "", 0);
    layout_page(root, 20000);

    const struct item *items = layout_items(), *run = 0;
    for (int i = 0; i < layout_count(); i++)
        if (items[i].type == IT_TEXT && items[i].node &&
            items[i].node->parent == probe) { run = &items[i]; break; }
    CHECK(run != 0 && run->len == VALUE_LEN,
          "one ordinary display run retains every source byte");
    if (run) {
        CHECK(run->w > 0,
              "long ordinary run keeps a nonzero layout measurement");
        CHECK(browser_text_run_advance(run, run->len) == run->w,
              "long contenteditable End shares the layout advance");
        CHECK(browser_text_run_advance(run, PREFIX) > 0 &&
              browser_text_run_advance(run, PREFIX + 3) >
              browser_text_run_advance(run, PREFIX),
              "long hit-test prefixes advance across a split UTF-8 scalar");

        paint_nops = 0;
        browser_paint_scroll(0, 0, 20000, 400, 0, 0);
        int calls = 0, bytes = 0, boundaries = 1, positions = 1;
        for (int i = 0; i < paint_nops; i++) {
            const struct paintop *op = &paint_ops[i];
            uintptr_t start = (uintptr_t)run->text, ptr = (uintptr_t)op->text;
            if (op->kind != OP_TEXT || ptr < start || ptr >= start + run->len) continue;
            int off = (int)(ptr - start);
            calls++; bytes += op->len;
            if (op->len > 256 || off + op->len > run->len ||
                (off > 0 && ((unsigned char)run->text[off] & 0xc0) == 0x80) ||
                (off + op->len < run->len &&
                 ((unsigned char)run->text[off + op->len] & 0xc0) == 0x80))
                boundaries = 0;
            if (op->x != run->x + browser_text_run_advance(run, off))
                positions = 0;
        }
        printf("long ordinary bytes=%d width=%d native_calls=%d native_bytes=%d\n",
               run->len, run->w, calls, bytes);
        CHECK(calls > 1 && bytes == VALUE_LEN,
              "long ordinary run reaches native drawing in bounded complete calls");
        CHECK(boundaries,
              "every native drawing call ends on a UTF-8 boundary");
        CHECK(positions,
              "paint chunks and caret/hit-test prefixes share every advance");
    }
    layout_free();
    dom_free(root);
    free(html);
    free(value);
}

int main(void)
{
    specimen("plain", "Wi plain", "");
    specimen("letters", "WiWiii", "letter-spacing:3px");
    specimen("words", "Wi ii WW", "word-spacing:7px");
    specimen("mixed", "Wi中文 i中", "letter-spacing:2px;word-spacing:5px");
    specimen("CJK", "中文测量", "letter-spacing:4px");
    specimen("negative", "WiWiii", "letter-spacing:-1px");
    specimen("bold", "Wi bold", "font-weight:bold;letter-spacing:3px");
    long_specimen();
    puts(fail ? "caret-advance: FAIL" : "caret-advance: PASS");
    return fail ? 1 : 0;
}
