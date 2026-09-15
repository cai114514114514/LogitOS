/* Real JS form bindings -> native byte editor -> JS readback. Guest discovery
 * was 600 CJK UTF-16 units reporting selectionEnd=1800. No copied JS selection
 * implementation is used here; the raw-byte build must fail the same checks. */
#define main selection_existing_main
#include "select_state_test.c"
#undef main
#include "focus.h"

static void native_selection(struct node *n, int start, int end, const char *label)
{
    int a = -1, b = -1; fc_selection(n, &a, &b); checks++;
    if (a != start || b != end) { printf("FAIL %s: bytes=%d..%d expected=%d..%d\n", label, a, b, start, end); failures++; }
}

int main(void)
{
    const char *html = "<!doctype html><input id=f><textarea id=t></textarea><input id=p type=password><div id=ce1 contenteditable=true>WiWi 中文 End</div><div id=ce2 contenteditable=true>Wi word 中 End</div>";
    struct node *root = dom_parse(html, (int)strlen(html));
    js_page_set_location("http://fixture.test/selection");
    if (!js_page_open(root)) return 2;
    struct node *field = dom_get_element_by_id(root->doc, "f");
    /* The guest's two short spacing specimens use Selection, a separate bridge
     * from input.selectionStart. Drive native bytes here before JS readback so
     * a value-only input test cannot hide a CE conversion regression. */
    struct node *ce1=dom_get_element_by_id(root->doc,"ce1"),*ce2=dom_get_element_by_id(root->doc,"ce2");
    fc_ce_caret_in(ce1,1);
    ck("CE letter specimen native End counts UTF16", "getSelection().focusNode===document.getElementById('ce1').firstChild&&getSelection().focusOffset===11");
    fc_ce_move(-1,0,0);fc_ce_insert("Z",1);
    ck("CE letter specimen native Left and Z keep UTF16", "getSelection().focusOffset===11&&document.getElementById('ce1').textContent==='WiWi 中文 EnZd'");
    fc_ce_caret_in(ce2,1);
    ck("CE word specimen native End counts UTF16", "getSelection().focusOffset===13");
    fc_ce_move(-1,0,0);fc_ce_insert("Z",1);
    ck("CE word specimen native Left and Z keep UTF16", "getSelection().focusOffset===13&&document.getElementById('ce2').textContent==='Wi word 中 EnZd'");
    fc_ce_clear();
    ck("CJK getter counts UTF16 units", "var f=document.getElementById('f');f.value='中A文';f.selectionStart===3&&f.selectionEnd===3");
    fc_edit_move(field, -1, 0, 0);
    ck("native Left returns UTF16 offset", "f.selectionStart===2&&f.selectionEnd===2");
    fc_edit_insert(field, "z", 1);
    ck("native insertion returns UTF16 offset", "f.value==='中Az文'&&f.selectionStart===3&&f.selectionEnd===3");
    ck("UTF16 range selects the intended CJK bytes", "f.value='中A文';f.setSelectionRange(1,2);f.selectionStart===1&&f.selectionEnd===2");
    native_selection(field, 3, 4, "UTF16 setter reaches correct native byte boundaries");
    js_dom_clear_dirty();
    ck("repeated programmatic selection updates native endpoints", "f.setSelectionRange(0,2);f.setSelectionRange(1,2);f.selectionStart===1");
    checks++; if (js_dom_inval_level() < INVAL_PAINT) { puts("FAIL programmatic selection requests caret repaint"); failures++; }
    fc_edit_insert(field, "Z", 1);
    ck("native replace preserves UTF8 around JS selection", "f.value==='中Z文'&&f.selectionStart===2");
    ck("long CJK getter matches value length", "f.value='中文'.repeat(300);f.value.length===600&&f.selectionEnd===600");
    fc_edit_move(field, -1, 0, 0); fc_edit_insert(field, "z", 1);
    ck("guest CJK sequence reports 600 after Left and z", "f.value.length===601&&f.selectionStart===600&&f.selectionEnd===600");
    ck("textarea and password share UTF16 boundary", "var t=document.getElementById('t'),p=document.getElementById('p');t.value='中\\n文';p.value='密碼';t.selectionEnd===3&&p.selectionEnd===2");
    ck("astral pair counts two UTF16 units", "f.value='A😀中B';f.value.length===5&&f.selectionEnd===5");
    ck("range covers whole surrogate pair", "f.setSelectionRange(1,3);f.selectionStart===1&&f.selectionEnd===3");
    native_selection(field, 1, 5, "astral endpoints map around complete UTF8 scalar");
    fc_edit_backspace(field);
    ck("native deletion of astral selection preserves neighbours", "f.value==='A中B'&&f.selectionStart===1");
    ck("half-surrogate endpoint safely clamps to leading scalar", "f.value='A😀B';f.setSelectionRange(2,2);f.selectionStart===1&&f.selectionEnd===1&&f.value==='A😀B'");
    native_selection(field, 1, 1, "half-surrogate never leaves caret inside UTF8 bytes");
    fc_edit_insert(field, "Z", 1);
    ck("insertion after half-surrogate clamp keeps astral intact", "f.value==='AZ😀B'");
    ck("unsigned long negative wraps then clamps to End", "f.value='中AB';f.setSelectionRange(-1,-1);f.selectionStart===3&&f.selectionEnd===3");
    ck("uint32 wrap and fractional truncation precede clamp", "f.setSelectionRange(4294967297,2.9);f.selectionStart===1&&f.selectionEnd===2");
    ck("reversed range collapses at supplied end", "f.setSelectionRange(3,1);f.selectionStart===1&&f.selectionEnd===1");
    ck("selectionStart raises end when necessary", "f.setSelectionRange(0,1);f.selectionStart=2;f.selectionStart===2&&f.selectionEnd===2");
    ck("selectionEnd lowers start when necessary", "f.selectionEnd=1;f.selectionStart===1&&f.selectionEnd===1");
    ck("backward direction reaches native selection", "f.value='中AB';f.setSelectionRange(0,2,'backward');f.selectionDirection==='backward'&&f.selectionStart===0&&f.selectionEnd===2");
    focus_set(field);
    struct fpaint paint;
    fc_paint_state(field, 16, 0, 160, &paint);
    checks++; if (paint.caret_x != 0 || paint.sel_x1 <= paint.sel_x0) { puts("FAIL backward caret paints at active start with ordered highlight"); failures++; }
    fc_edit_move(field, 1, 0, 1);
    ck("Shift Right shrinks backward selection from active end", "f.selectionStart===1&&f.selectionEnd===2&&f.selectionDirection==='backward'");
    fc_edit_move(field, 1, 0, 1);
    ck("second Shift Right collapses at original anchor", "f.selectionStart===2&&f.selectionEnd===2");
    ck("direction setter retains endpoints and normalizes invalid value", "f.setSelectionRange(0,2,'backward');f.selectionDirection='forward';var a=f.selectionStart===0&&f.selectionEnd===2&&f.selectionDirection==='forward';f.selectionDirection='bogus';a&&f.selectionDirection==='none'");
    ck("detached field keeps UTF16 clamp and direction", "var d=document.createElement('input');d.value='中😀B';d.setSelectionRange(1,3,'backward');var ok=d.selectionStart===1&&d.selectionEnd===3&&d.selectionDirection==='backward';d.setSelectionRange(99,1);ok&&d.selectionStart===1&&d.selectionEnd===1");
    js_page_close(); fc_reset(); focus_reset(); dom_free(root);
    printf("form-selection-utf16: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
