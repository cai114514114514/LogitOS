#ifndef _ICONV_H
#define _ICONV_H

/* Character-set conversion (POSIX <iconv.h>).
 *
 * THIS IS A CLOSED SET, ON PURPOSE, AND IT IS NOT GNU LIBICONV. Real libiconv
 * ships ~150 charsets and a transliteration database with thousands of
 * entries; a from-scratch libc has no business pretending to be that. What
 * ported code actually needs from iconv(), overwhelmingly, is moving text
 * between the handful of encodings a modern system actually uses: UTF-8 (this
 * OS's native encoding end to end -- fonts, DOM, filesystem, the "C" locale's
 * multibyte encoding, see <wchar.h>), UTF-16/UCS-2 (Windows-authored text,
 * Java/JS interop data), UTF-32/UCS-4/WCHAR_T (this ABI's wchar_t), and
 * ISO-8859-1/ASCII (the encodings that predate Unicode and are still what a
 * lot of legacy plain-text and protocol data shows up in). Supported,
 * case-insensitively and ignoring '-'/'_' in the name (so "utf8", "UTF_8" and
 * "UTF-8" name the same charset -- iconv.c's normalize() is deliberately more
 * permissive here than glibc's own matcher, which accepts case and '-' but
 * NOT '_'; that is a considered superset, not a bug, and is why charset-name
 * parsing is not one of the things this header's test diffs against glibc):
 *
 *   UTF-8
 *   UTF-16, UTF-16LE, UTF-16BE
 *   UTF-32, UTF-32LE, UTF-32BE
 *   UCS-2   (fixed little-endian, BMP only, no surrogates -- see iconv.c)
 *   UCS-4   (fixed BIG-endian, no BOM -- glibc's own historical convention,
 *            verified directly against glibc's iconv() on the host; it is
 *            NOT the same endianness rule as UCS-2, which is glibc-authentic
 *            and looks like an inconsistency because it is one)
 *   ISO-8859-1, LATIN1  (aliases of the same charset)
 *   ASCII, US-ASCII     (aliases of the same charset)
 *   WCHAR_T             (this ABI's wchar_t: UTF-32, little-endian, no BOM --
 *                         see <wchar.h> -- so WCHAR_T is simply UTF-32LE under
 *                         another name; iconv.c aliases it directly rather
 *                         than carrying a redundant code path)
 *
 * Anything else fails iconv_open() with EINVAL. That is a real error a caller
 * can act on, not a silent no-op -- see iconv.c's file comment for why lying
 * about support is the one thing this library refuses to do.
 *
 * BOM POLICY (this is the part hand-written iconv implementations most often
 * get wrong, and it is pinned by tests/unit/libc_iconv_test.c against real
 * glibc behaviour, not guessed):
 *   - UTF-16 / UTF-32 with NO explicit endianness: decoding consumes a
 *     leading BOM if present and adopts its endianness for the rest of the
 *     stream; if no BOM is present the stream is assumed to be in the host's
 *     native order (little-endian -- this library only ever targets x86-64).
 *     Encoding to these generic forms always WRITES a little-endian BOM
 *     before the first character.
 *   - UTF-16LE/BE and UTF-32LE/BE do NEITHER: a leading FF FE / FE FF byte
 *     pair on either the input or output side is just the ordinary character
 *     U+FEFF (ZERO WIDTH NO-BREAK SPACE), not a marker. This is the single
 *     most common hand-written-iconv bug and is exactly why the LE/BE forms
 *     exist as distinct charsets from the generic ones.
 *   - UCS-2 and UCS-4 never use a BOM at all (see the endianness note above).
 *   - iconv(cd, NULL, NULL, outbuf, outbytesleft) resets the descriptor's
 *     conversion state -- which for this library means: forget any BOM
 *     already consumed/emitted on `cd`, so the NEXT real conversion on a
 *     generic UTF-16/UTF-32 descriptor auto-detects (decode side) or emits
 *     (encode side) a fresh BOM, exactly as if the descriptor were freshly
 *     opened. None of the charsets here carry a shift-out sequence to flush
 *     (that machinery is for stateful encodings like ISO-2022-JP, which this
 *     library does not support), so a reset call never itself writes bytes.
 *
 * //IGNORE and //TRANSLIT (appended to `tocode`, as glibc does; parsed
 * case-insensitively, either order, both allowed together):
 *   - //IGNORE is implemented HONESTLY, matching glibc's actual (if
 *     surprising) behaviour: a malformed input sequence or a valid code point
 *     with no representation in the target charset is silently dropped and
 *     conversion continues, BUT if the input is otherwise fully consumed the
 *     call still reports failure -- returns (size_t)-1 with errno == EILSEQ --
 *     to tell the caller that something was in fact discarded. This is not
 *     this library's invention; it is what real iconv() does, verified
 *     directly against glibc on the host, and callers that already cope with
 *     glibc's //IGNORE cope with this one identically. An incomplete sequence
 *     at the very end of the input is NEVER dropped by //IGNORE -- it always
 *     reports EINVAL, because it might complete if the caller supplies more
 *     bytes in a later call, and //IGNORE only discards input that cannot
 *     ever become valid.
 *   - //TRANSLIT is REJECTED with EINVAL at iconv_open() time. The
 *     alternative -- a lossy substitution table -- would need to track
 *     glibc's locale-dependent transliteration data to mean the same thing it
 *     means everywhere else, and a hand-picked handful of substitutions would
 *     be worse than nothing: a caller who checks for EINVAL learns transliteration
 *     isn't available and can choose its own fallback, whereas a partial table
 *     would silently transliterate some inputs and not others with no way to
 *     tell which. Refusing outright is the honest answer; see iconv.c.
 *
 * REVERSIBILITY. Every conversion this library performs among the charsets
 * above is fully information-preserving when it succeeds -- there is no
 * lossy-but-successful path (that would BE //TRANSLIT, which is rejected), so
 * iconv() never has a legitimate reason to return a positive count. Success
 * is always exactly 0; the only non-zero, non-(size_t)-1 return glibc itself
 * documents (a count of non-reversible substitutions) simply cannot occur
 * here and is not something a caller needs to check for.
 */

#include <stddef.h>

typedef void *iconv_t;

/* Returns a live descriptor for tocode<-fromcode, or (iconv_t)-1 with errno
 * set (EINVAL for an unrecognised or malformed charset name, ENOMEM if the
 * small bookkeeping structure can't be allocated). */
iconv_t iconv_open(const char *tocode, const char *fromcode);

/* Convert *inbytesleft bytes starting at *inbuf, writing up to *outbytesleft
 * bytes starting at *outbuf; advances all four in place to reflect exactly
 * what was consumed/produced, even on error, so the caller can always resume.
 *
 * Returns the number of non-reversible conversions performed (always 0 here,
 * see the file header), or (size_t)-1 with errno set to one of:
 *   EILSEQ  an invalid sequence was found in the input (or, with //IGNORE
 *           active, one or more invalid/unmappable sequences were silently
 *           dropped) -- *inbuf points at the offending byte, unless it was
 *           dropped by //IGNORE, in which case the whole input was consumed
 *   EINVAL  the input ends with an incomplete multibyte/wide sequence -- the
 *           input pointer is left at the start of that incomplete sequence,
 *           which the caller should represent (unmodified) at the front of
 *           the next call once more bytes are available
 *   E2BIG   there was not enough room at the output pointer for the next
 *           converted character -- the input side is left BEFORE that
 *           character, not after, so nothing is ever silently lost to a
 *           full buffer
 *
 * Passing a NULL inbuf, or a NULL *inbuf, resets conversion state (see the
 * BOM policy above) instead of converting; inbytesleft is ignored then. */
size_t iconv(iconv_t cd, char **inbuf, size_t *inbytesleft,
             char **outbuf, size_t *outbytesleft);

/* Frees a descriptor from iconv_open(). Returns 0, or -1/EBADF for a NULL
 * descriptor.
 *
 * NOT COVERED: a descriptor that has ALREADY been closed. An earlier
 * revision of this comment claimed EBADF for that case too, which was never
 * true and, found during adversarial re-verification, could not be made
 * true without a real cost: `cd` is a bare `free()`-able pointer with no
 * live-descriptor registry backing it, so by the time a caller passes an
 * already-closed `cd` back in, this function has no way to tell it apart
 * from a still-open one -- the memory has already been returned to the
 * allocator. Calling iconv_close() (or iconv()) again on an already-closed
 * descriptor is therefore undefined behaviour here, exactly as it is for
 * calling free() twice on the same pointer (which is, mechanically, what
 * this function's second call would do) -- and, verified on the host, as it
 * is for glibc's own iconv_close(NULL) and iconv_open(NULL, ...): a caller
 * bug, not a condition this function is required to survive gracefully. */
int iconv_close(iconv_t cd);

#endif /* _ICONV_H */
