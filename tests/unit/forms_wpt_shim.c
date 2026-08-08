/* The one symbol forms.c needs that the WPT runner does not provide.
 *
 * forms.c measures text -- for the caret's x, the horizontal scroll of a long
 * value, and the offset a click maps to. In the browser that resolves to
 * browser_rt.c's font syscall; in tests/unit/forms_test.c it resolves to that
 * test's own fake metrics. The WPT runner links neither, because it drives the
 * DOM and the Web APIs and never paints anything.
 *
 * A half-em advance rather than 0: a stub that returned zero would make every
 * caret position identical, and any WPT subtest that ever compares
 * selectionStart geometry would pass for the wrong reason.
 */
int text_measure(const char *s, int len, int px, int mono)
{
    (void)s;
    (void)mono;
    if (len < 0) len = 0;
    return len * (px / 2);
}
