/* HTML "parse a sandboxing directive" and the CSP sandbox directive, against
 * the flag table the HTML Standard gives (Sandboxing, "parse a sandboxing
 * directive"). Every expectation below is written from that table, never
 * from what frame/sandbox.h happens to return: a flag is SET (restricted)
 * unless a keyword in the value clears it.
 *
 * -DSANDBOX_NEGCTL_CASE_SENSITIVE compares keywords byte-for-byte. The spec
 * says the tokens are ASCII case-insensitive, and exactly the checks that
 * spell a keyword in another ASCII case must fail -- the Unicode-folding and
 * everything-else checks must still pass. */
#include "frame/sandbox.h"
#include <stdio.h>
#include <string.h>

static int checks, failed;
static void ck(unsigned got, unsigned want, const char *name)
{
    checks++;
    if (got != want) { failed++; printf("FAIL: %s (got 0x%05x want 0x%05x)\n", name, got, want); }
}

/* Every restriction, then the ones a keyword clears. Written out per flag,
 * from the spec's list, so the oracle does not share the parser's tables. */
#define ALL (SANDBOX_NAVIGATION | SANDBOX_AUXILIARY_NAVIGATION | SANDBOX_TOP_NAVIGATION | \
             SANDBOX_TOP_NAVIGATION_USER | SANDBOX_PLUGINS | SANDBOX_ORIGIN | SANDBOX_FORMS | \
             SANDBOX_POINTER_LOCK | SANDBOX_SCRIPTS | SANDBOX_AUTOMATIC_FEATURES | \
             SANDBOX_DOCUMENT_DOMAIN | SANDBOX_PROPAGATES_TO_AUXILIARY | SANDBOX_MODALS | \
             SANDBOX_ORIENTATION_LOCK | SANDBOX_PRESENTATION | SANDBOX_DOWNLOADS | \
             SANDBOX_CUSTOM_PROTOCOLS | SANDBOX_STORAGE_ACCESS)
#define SCRIPTS (SANDBOX_SCRIPTS | SANDBOX_AUTOMATIC_FEATURES)

int main(void)
{
    /* The empty set of tokens restricts everything, and the always-set
     * flags (navigation, plugins, document.domain) have no keyword at all. */
    ck(ALL, SANDBOX_ALL, "the flag table covers every bit exactly once");
    ck(sandbox_parse(""), ALL, "empty attribute: every restriction");
    ck(sandbox_parse(NULL), ALL, "missing value parses as empty");
    ck(sandbox_parse(" \t\n\f\r "), ALL, "only ASCII whitespace: every restriction");
    ck(sandbox_parse("allow-scripts"), ALL & ~SCRIPTS, "allow-scripts clears scripts AND automatic features");
    ck(sandbox_parse("allow-same-origin"), ALL & ~SANDBOX_ORIGIN, "allow-same-origin clears only the origin flag");
    ck(sandbox_parse("allow-forms"), ALL & ~SANDBOX_FORMS, "allow-forms");
    ck(sandbox_parse("allow-modals"), ALL & ~SANDBOX_MODALS, "allow-modals");
    ck(sandbox_parse("allow-pointer-lock"), ALL & ~SANDBOX_POINTER_LOCK, "allow-pointer-lock");
    ck(sandbox_parse("allow-orientation-lock"), ALL & ~SANDBOX_ORIENTATION_LOCK, "allow-orientation-lock");
    ck(sandbox_parse("allow-presentation"), ALL & ~SANDBOX_PRESENTATION, "allow-presentation");
    ck(sandbox_parse("allow-downloads"), ALL & ~SANDBOX_DOWNLOADS, "allow-downloads");
    ck(sandbox_parse("allow-popups-to-escape-sandbox"), ALL & ~SANDBOX_PROPAGATES_TO_AUXILIARY,
       "allow-popups-to-escape-sandbox");
    ck(sandbox_parse("allow-storage-access-by-user-activation"), ALL & ~SANDBOX_STORAGE_ACCESS,
       "allow-storage-access-by-user-activation");
    /* The navigation keywords overlap, exactly as the table says. */
    ck(sandbox_parse("allow-popups"), ALL & ~(SANDBOX_AUXILIARY_NAVIGATION | SANDBOX_CUSTOM_PROTOCOLS),
       "allow-popups clears auxiliary navigation and custom protocols");
    ck(sandbox_parse("allow-top-navigation"),
       ALL & ~(SANDBOX_TOP_NAVIGATION | SANDBOX_TOP_NAVIGATION_USER | SANDBOX_CUSTOM_PROTOCOLS),
       "allow-top-navigation clears both top-navigation flags");
    ck(sandbox_parse("allow-top-navigation-by-user-activation"),
       ALL & ~(SANDBOX_TOP_NAVIGATION_USER | SANDBOX_CUSTOM_PROTOCOLS),
       "by-user-activation keeps the no-activation top-navigation flag");
    ck(sandbox_parse("allow-top-navigation-to-custom-protocols"), ALL & ~SANDBOX_CUSTOM_PROTOCOLS,
       "custom protocols keyword alone");
    ck(sandbox_parse("allow-top-navigation allow-top-navigation-by-user-activation"),
       sandbox_parse("allow-top-navigation"), "the redundant pair equals allow-top-navigation");

    /* The measured specimen: Cloudflare Turnstile's widget iframe. */
    ck(sandbox_parse("allow-same-origin allow-scripts allow-popups"),
       ALL & ~(SANDBOX_ORIGIN | SCRIPTS | SANDBOX_AUXILIARY_NAVIGATION | SANDBOX_CUSTOM_PROTOCOLS),
       "Turnstile widget value");
    ck(sandbox_parse("allow-same-origin allow-scripts allow-popups allow-forms"),
       ALL & ~(SANDBOX_ORIGIN | SCRIPTS | SANDBOX_AUXILIARY_NAVIGATION | SANDBOX_CUSTOM_PROTOCOLS | SANDBOX_FORMS),
       "Turnstile variant with allow-forms");

    /* Unknown tokens have no effect; duplicates change nothing. */
    ck(sandbox_parse("allow-everything"), ALL, "an unknown keyword grants nothing");
    ck(sandbox_parse("allow-everything allow-scripts bogus"), ALL & ~SCRIPTS, "unknown tokens are ignored beside known ones");
    ck(sandbox_parse("allow-scripts allow-scripts allow-scripts"), ALL & ~SCRIPTS, "duplicates are one token");
    ck(sandbox_parse("allow-script"), ALL, "a prefix of a keyword is not the keyword");
    ck(sandbox_parse("allow-scriptss"), ALL, "a keyword with a suffix is not the keyword");
    ck(sandbox_parse("allow-scripts,allow-same-origin"), ALL, "comma is not a separator: one unknown token");
    ck(sandbox_parse("allow-scripts\vallow-same-origin"), ALL, "vertical tab is not ASCII whitespace");
    ck(sandbox_parse("\tallow-scripts\n\nallow-same-origin\f \r"), ALL & ~(SCRIPTS | SANDBOX_ORIGIN),
       "every ASCII whitespace separates");
    ck(sandbox_parse_n("allow-scripts allow-same-origin", 13), ALL & ~SCRIPTS, "an explicit length bounds the value");

    /* ASCII case-insensitive, and ONLY ASCII. */
    ck(sandbox_parse("ALLOW-SCRIPTS"), ALL & ~SCRIPTS, "upper-case keyword");
    ck(sandbox_parse("Allow-Same-Origin allow-SCRIPTS"), ALL & ~(SCRIPTS | SANDBOX_ORIGIN), "mixed-case keywords");
    ck(sandbox_parse("allow-\xc5\xbf" "cripts"), ALL, "U+017F LATIN SMALL LONG S does not fold to s");
    ck(sandbox_parse("allow-scr\xc4\xb0pts"), ALL, "U+0130 CAPITAL I WITH DOT does not fold to i");
    ck(sandbox_parse("allow-\xe2\x80\x8bscripts"), ALL, "a zero-width space is not whitespace");

    /* CSP3 sandbox directive: enforced header policies only (the caller's job),
     * first occurrence per policy, union across policies. */
    ck(sandbox_from_csp(NULL), 0, "no policy: no sandbox");
    ck(sandbox_from_csp("default-src 'self'; script-src 'none'"), 0, "no sandbox directive: no flags");
    ck(sandbox_from_csp("sandbox"), ALL, "bare sandbox directive: every restriction");
    ck(sandbox_from_csp("default-src 'self'; sandbox allow-scripts"), ALL & ~SCRIPTS, "sandbox after another directive");
    ck(sandbox_from_csp("SANDBOX allow-scripts"), ALL & ~SCRIPTS, "directive name is ASCII case-insensitive");
    ck(sandbox_from_csp("sandboxed allow-scripts"), 0, "a longer directive name is not sandbox");
    ck(sandbox_from_csp("sandbox allow-scripts; sandbox"), ALL & ~SCRIPTS, "duplicate directive: the first one wins");
    ck(sandbox_from_csp("sandbox allow-scripts\nsandbox allow-same-origin"), ALL,
       "two policies: the union of restrictions (the intersection of allowances)");
    ck(sandbox_from_csp("sandbox allow-scripts allow-same-origin, script-src 'self'"), ALL & ~(SCRIPTS | SANDBOX_ORIGIN),
       "comma-separated serialized policies");
    ck(sandbox_from_csp("script-src 'self'\n  sandbox allow-forms ; img-src *"), ALL & ~SANDBOX_FORMS,
       "whitespace around a directive");

    char text[160];
    sandbox_describe(sandbox_parse("allow-scripts allow-same-origin"), text, sizeof text);
    checks++;
    if (strcmp(text, "allow-scripts allow-same-origin")) { failed++; printf("FAIL: describe names the allowances (%s)\n", text); }
    sandbox_describe(0, text, 4);
    checks++;
    if (strcmp(text, "uns")) { failed++; printf("FAIL: describe truncates within capacity (%s)\n", text); }

    printf("frame-sandbox: %d checks, %d failures\n", checks, failed);
    return failed ? 1 : 0;
}
