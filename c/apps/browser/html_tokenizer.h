/* html_tokenizer.h -- a spec-conformant HTML5 tokenizer.
 *
 * This is the WHATWG tokenization stage and nothing else: bytes in, tokens
 * out.  It knows nothing about the DOM, about elements, about nesting -- the
 * tree construction stage sits above it and is the only thing that has an
 * opinion about what a <p> means.  Keeping that line sharp is what makes the
 * tokenizer testable in isolation against html5lib-tests' 6810 cases, which is
 * the only way to have a NUMBER for conformance instead of a feeling.
 *
 * The one place the two stages talk is content model: the tree builder must
 * tell us that we just opened <title> (RCDATA), <style> (RAWTEXT) or <script>
 * (script data), because the tokenizer cannot know that on its own.  That is
 * html_tok_set_state() plus html_tok_set_last_start_tag(), and it is the whole
 * of the coupling.
 *
 * ------------------------------------------------------------------------
 * THREE RULES THAT KEEP STREAMING A LATER ADDITION RATHER THAN A REWRITE
 * ------------------------------------------------------------------------
 * We are fed whole documents today (http_get buffers the body).  We will not
 * always be.  A tokenizer that assumed a single contiguous buffer has to be
 * rewritten to stream; one written to these three rules only has to be handed
 * more buffer:
 *
 *   1. Tag names, attribute names and values, comment data and doctype
 *      identifiers ALWAYS accumulate into the tokenizer's own growable
 *      buffers, never as (pointer, length) into the input.  A token that spans
 *      a network-buffer boundary then has self-consistent state: the part we
 *      already saw is in our buffer, not in memory that is about to be reused.
 *
 *   2. Only TOK_CHARS may point into the input, and only on the fast path (a
 *      run in the data state with no '&', '<', NUL or CR).  CONTRACT: the
 *      caller must consume or copy a token's payload BEFORE calling
 *      html_tok_next() again.  This is what makes plain text -- the bulk of
 *      every real page -- zero-copy.
 *
 *   3. On input exhaustion with eof == 0, the tokenizer rolls `pos` back to
 *      where this token started, restores the state it started in, drops the
 *      partial token, and returns 0 without advancing.  Re-running it after
 *      more bytes arrive reproduces the token exactly.  This costs a re-scan
 *      of one token's worth of input per buffer refill and buys a state
 *      machine with no "resume in the middle of a doctype public identifier"
 *      cases at all.
 *
 * Because of rule 2, EVERY pointer in a struct html_token (name, attribute
 * names and values, data, pubid, sysid) is valid only until the next
 * html_tok_next() call on the same tokenizer.
 */
#ifndef HTML_TOKENIZER_H
#define HTML_TOKENIZER_H

#include <stdint.h>
#include <stddef.h>

#include "html_tags.inc"        /* enum html_tag -- generated, enum part only */

/* ------------------------------------------------------------- tokens --- */

enum html_tok_type {
    TOK_DOCTYPE = 0,
    TOK_START,
    TOK_END,
    TOK_COMMENT,
    TOK_CHARS,
    TOK_EOF
};

/* Attributes are (name, value) slices.  Duplicates are already removed: the
 * spec drops the *later* of two attributes with the same name, and doing it
 * here means the tree builder never sees a tag it has to de-duplicate. */
struct html_attr {
    const char *n; uint32_t nl;
    const char *v; uint32_t vl;
};

struct html_token {
    uint8_t type;                    /* enum html_tok_type */

    const char *name; uint32_t namelen;   /* START/END: lowercased tag name
                                           * DOCTYPE:   lowercased doctype name */
    struct html_attr *attrs;
    uint16_t nattr;
    uint8_t self_closing;

    const char *data; uint32_t datalen;   /* CHARS payload / COMMENT data */

    const char *pubid, *sysid;            /* DOCTYPE identifiers, see has_* */
    uint32_t pubidlen, sysidlen;

    uint8_t force_quirks;                 /* DOCTYPE: quirks-mode trigger */
    uint8_t has_name, has_pubid, has_sysid;  /* "missing" != "empty string":
                                              * <!DOCTYPE html PUBLIC ""> has an
                                              * empty public id, <!DOCTYPE html>
                                              * has none, and quirks detection
                                              * distinguishes them. */

    uint16_t tag;                    /* enum html_tag, 0 = unknown/custom */

    uint32_t src_start, src_end;     /* byte offsets in the input buffer.  The
                                      * tree builder needs these to hand a raw
                                      * <svg> subtree to the SVG parser without
                                      * re-serialising the DOM. */
};

/* ------------------------------------------------------------- states --- */
/* The first six are the content models the tree builder may switch us into;
 * everything past HTML_STATE__PUBLIC is internal and must not be passed to
 * html_tok_set_state(). */
enum {
    HTML_STATE_DATA = 0,
    HTML_STATE_RCDATA,
    HTML_STATE_RAWTEXT,
    HTML_STATE_SCRIPT_DATA,
    HTML_STATE_PLAINTEXT,
    HTML_STATE_CDATA_SECTION,
    HTML_STATE__PUBLIC
};

/* --------------------------------------------------------- the machine --- */

struct html_charbuf { char *p; uint32_t len, cap; };

/* One attribute under construction.  Offsets, not pointers: the arena they
 * live in reallocs as the tag grows, and a pointer taken before the realloc is
 * a use-after-free that only bites on tags with many attributes -- exactly the
 * kind of bug hand-written tokenizers ship. */
struct html_pattr { uint32_t noff, nlen, voff, vlen; uint8_t dropped; };

struct html_tokenizer {
    int state, return_state;

    const char *buf;                 /* input, borrowed */
    size_t len, pos;
    int eof;

    /* rule 1: every token field except CHARS is accumulated here */
    struct html_charbuf name, comment, pubid, sysid, chars, temp, attrbuf;

    struct html_pattr *attrs;  uint32_t nattr, attrcap;
    struct html_attr  *oattrs; uint32_t oattrcap;   /* materialised for output */

    uint8_t cur_type;                /* token under construction, 0xFF = none */
    uint8_t self_closing, force_quirks, has_name, has_pubid, has_sysid;

    /* An end tag is "appropriate" only if it matches the last start tag
     * emitted; that is what makes "</b>" inside <script> plain text and
     * "</script>" a tag. */
    char last_start_tag[64];
    uint32_t last_start_len;

    int in_foreign;                  /* adjusted current node is MathML/SVG:
                                      * enables <![CDATA[ ]]> sections */

    struct html_token pending;       /* a token held back so a pending run of
                                      * characters can be emitted first */
    uint8_t has_pending, finished;

    uint32_t tok_src;                /* input offset the current token began at */
    uint32_t cpos;                   /* input offset of the character being
                                      * processed -- src_start for a run of
                                      * characters built the slow way */
    uint32_t chars_src;
};

/* src/len is borrowed and must outlive the tokenizer.  eof == 1 means "this is
 * the whole document"; eof == 0 means more bytes may follow, and rule 3
 * applies. */
void html_tok_init(struct html_tokenizer *t, const char *src, size_t len, int eof);
void html_tok_free(struct html_tokenizer *t);

/* 1 = token produced, 0 = need more input (only possible when eof == 0),
 * -1 = done (TOK_EOF has already been produced, once). */
int  html_tok_next(struct html_tokenizer *t, struct html_token *out);

/* Tree-builder hooks. */
void html_tok_set_state(struct html_tokenizer *t, int state);
void html_tok_set_last_start_tag(struct html_tokenizer *t, const char *name, uint32_t len);
void html_tok_set_foreign(struct html_tokenizer *t, int foreign);

/* Hand more input to a tokenizer that returned 0.  buf must still contain
 * everything from the old buffer (we rolled `pos` back into it). */
void html_tok_feed(struct html_tokenizer *t, const char *src, size_t len, int eof);

/* enum html_tag for a lowercased tag name, HTAG_UNKNOWN if not a known tag. */
uint16_t html_tag_id(const char *name, uint32_t len);

#endif /* HTML_TOKENIZER_H */
