#ifndef BROWSER_CSS_NESTING_H
#define BROWSER_CSS_NESTING_H

/* Normalization owns its output; the caller must kfree it after its consumer
 * has parsed/copied the bytes. Never retain this buffer as the CSSOM source. */
struct css_nesting_hooks {
    /* 1 valid selector list, 2 contains a pseudo-element, 0 invalid; negative
     * is a resource refusal and must abort the WHOLE normalization. */
    int (*selector)(const char *, int);
    int (*supports)(const char *, int);
};
struct css_nesting_report { int nested, refused; };
int css_nesting_normalize(const char *, int, char **, int *,
    const struct css_nesting_hooks *, struct css_nesting_report *);
/* The engine supplies LibCSS validation and its own shared @supports hooks.
 * A CSS-only host without the engine may leave this weak bridge absent. */
int css_normalize_stylesheet(const char *, int, char **, int *);
#endif
