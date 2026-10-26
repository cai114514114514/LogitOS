#ifndef LOGIT_TEXT_LINEBREAK_H
#define LOGIT_TEXT_LINEBREAK_H

/* UAX #14 line breaking (Unicode 16.0.0), freestanding and ring-3 only: this
 * file is filtered out of the kernel's C_SRC and reaches apps through
 * $(AUI_LIBS) (libtl.a).  The class lookup and pair-rule engine are the SAME
 * code the browser's layout/text.c uses (c/lib/text/linebreak_core.inc); the
 * browser layers its CSS tailoring on top of it, this wrapper layers the small
 * subset below.
 *
 * No libc, no malloc.  Scratch is static, so the call is NOT reentrant, and a
 * single call handles at most TL_BREAK_MAX code points (a longer string is
 * refused, never silently mis-broken). */

enum tl_brk { TL_BRK_NONE = 0, TL_BRK_ALLOWED = 1, TL_BRK_MANDATORY = 2 };

/* flags: one TL_LB_* value, optionally ORed with one TL_WB_* value. */
#define TL_LB_NORMAL     0x00   /* default; identical to AUTO in CSS */
#define TL_LB_STRICT     0x01   /* no strict-only tailoring is implemented, so
                                 * this is today the same as NORMAL (as in the
                                 * browser) */
#define TL_LB_LOOSE      0x02   /* CJ (small kana, prolonged sound mark) is ID */
#define TL_LB_MASK       0x03
#define TL_WB_BREAK_ALL  0x10   /* break between any two letters, except where
                                 * a non-tailorable rule (LB2..LB12) forbids */
#define TL_WB_KEEP_ALL   0x20   /* no break inside a run of CJK; only at
                                 * spaces/hyphens/ZW etc.  BREAK_ALL wins if
                                 * both are given */

#define TL_BREAK_MAX     2048   /* code points per call */

/* Break opportunities over UTF-8 text.  `out_brk` must hold len+1 bytes;
 * out_brk[i] is the opportunity BEFORE byte i (an enum tl_brk value).  Bytes
 * that are not the first byte of a code point are TL_BRK_NONE, out_brk[0] is
 * TL_BRK_NONE (LB2) and out_brk[len] is TL_BRK_MANDATORY (LB3).  Ill-formed
 * bytes decode as one U+FFFD each.  Returns len+1, or -1 if the text has more
 * than TL_BREAK_MAX code points (out_brk is then all NONE except the end). */
int tl_break_utf8(const char *s, int len, unsigned char *out_brk, int flags);

#endif /* LOGIT_TEXT_LINEBREAK_H */
