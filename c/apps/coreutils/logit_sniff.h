#ifndef LOGIT_SNIFF_H
#define LOGIT_SNIFF_H

/* ===========================================================================
 * Look before you print.
 *
 * WHY THIS EXISTS
 * ---------------
 * `show /fonts/mono.ttf` used to write 2.2 MB of sfnt tables onto a character
 * grid. That is not a cosmetic bug. A terminal that will print anything you
 * hand it has no notion of what a file IS, and every real system solves that
 * the same way -- by looking at the bytes before committing them to a display.
 * `file(1)`, `less`, `git diff` and every browser do exactly this; none of them
 * trust the name.
 *
 * WHY NOT THE EXTENSION
 * ---------------------
 * The extension is a claim made by whoever named the file, and this OS opens
 * files it did not create (downloads, disk images built by a script, a stream
 * written by another program). The old `show` decided "is this an image?" with
 * strcmp on ".png" -- so a PNG saved as notes.txt was printed as garbage, and a
 * text file named photo.png was sent to the image decoder. Both are the same
 * mistake in opposite directions. The bytes are the only thing that is not a
 * guess, so the bytes decide.
 *
 * TWO USERS, ONE TABLE
 * --------------------
 * 1. THE PRODUCER (`show`) sniffs a file it is about to open and picks a
 *    handler: image -> an LRT image frame, video -> a video frame, text -> cat,
 *    anything else -> refuse, and say what it is.
 * 2. THE TERMINAL sniffs the BYTE STREAM on fd 1 / fd 2 (sniff_guard). That is
 *    the backstop, and it is the one that has to exist: `cat`, a shell script,
 *    a program written next year -- none of them consult this header, and the
 *    terminal is the last place that can refuse to paint a font onto the grid.
 *    Defence at the producer alone would only cover the programs we remember to
 *    change.
 *
 * WHAT THE GUARD COSTS, HONESTLY
 * ------------------------------
 * It can be wrong in one direction: output that is genuinely text but stuffed
 * with control bytes gets collapsed into a summary. It is never wrong in the
 * other direction in the way that matters -- a NUL byte, which no text stream
 * in this OS ever emits, is decisive on its own. UTF-8 is safe by construction:
 * every byte of a multi-byte sequence is >= 0x80 and the guard only counts C0
 * controls and NULs, so a page of Chinese is text and stays text. That property
 * is tested (tests/unit/sniff_test.c), not assumed.
 *
 * This header is PURE: no syscalls, no libc, no allocation. The host unit test
 * compiles it exactly as the OS does.
 * =========================================================================== */

/* ------------------------------------------------------------- kinds ----- */

#define SN_TEXT     0        /* printable, as far as the prefix can tell   */
#define SN_UNKNOWN  1        /* binary of no recognized kind               */

#define SN_PNG     10
#define SN_JPEG    11
#define SN_GIF     12
#define SN_BMP     13
#define SN_SVG     14
#define SN_WEBP    15

#define SN_H264    20
#define SN_H265    21
#define SN_MP4     22        /* ISO-BMFF container (the demuxer line owns it) */
#define SN_MKV     23

#define SN_TTF     30
#define SN_OTF     31
#define SN_TTC     32
#define SN_WOFF    33

#define SN_WAV     40
#define SN_FLAC    41
#define SN_MP3     42
#define SN_OGG     43

#define SN_ELF     50
#define SN_AEX     51
#define SN_ZIP     52
#define SN_GZIP    53
#define SN_PDF     54

/* classes -- what a consumer can DO with it */
#define SNC_TEXT   0
#define SNC_IMAGE  1
#define SNC_VIDEO  2
#define SNC_OPAQUE 3         /* refuse: printing it is destructive          */

/* How many bytes sniff_id() wants. Everything recognized here declares itself
 * in far less; 512 is one disk-friendly read and leaves room for the text test
 * to be about a representative sample rather than eight bytes. */
#define SNIFF_PREFIX 512

/* --------------------------------------------------------- primitives ---- */

static inline int sn_eq(const unsigned char *b, int n, int off, const char *m, int mn)
{
    if (off + mn > n) return 0;
    for (int i = 0; i < mn; i++) if (b[off + i] != (unsigned char)m[i]) return 0;
    return 1;
}

/* A C0 byte that no text stream in this OS produces. \t \n \r \b are the four a
 * terminal genuinely receives (put_char in terminal.c handles exactly those);
 * everything else below 0x20, plus DEL, is evidence of binary. ESC is counted
 * as evidence ON PURPOSE: LRT/1 has no in-band control language, so an escape
 * byte on fd 1 is either a mistake or an attempt at one. */
static inline int sn_ctl(unsigned char c)
{
    if (c == 9 || c == 10 || c == 13 || c == 8) return 0;
    return c < 32 || c == 127;
}

/* ------------------------------------------------------- Annex-B video --- */

/* H.264 and H.265 elementary streams both begin with a start code, so the start
 * code alone says "video" and the NAL header that follows says which codec.
 *
 *   H.264: one byte  -- 0 | nal_ref_idc(2) | type(5)
 *   H.265: two bytes -- 0 | type(6) | layer_id(6) | tid_plus1(3), tid_plus1 != 0
 *
 * The two are told apart by which reading is STRUCTURALLY VALID, never by a
 * table of "streams usually start with". Our fixtures: 0x67 is H.264 SPS (type
 * 7) and reads as H.265 type 51, which is in the reserved range -> H.264. 0x40
 * is H.265 VPS (type 32) and reads as H.264 type 0, which is unspecified ->
 * H.265. When both readings are valid the H.264 stream-start types (SPS/AUD/
 * SEI) win, because an H.265 stream that opens with a non-parameter-set NAL is
 * not a stream a decoder could start on anyway. */
static inline int sn_annexb(const unsigned char *b, int n)
{
    int sc;
    if (n >= 5 && b[0] == 0 && b[1] == 0 && b[2] == 1) sc = 3;
    else if (n >= 6 && b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1) sc = 4;
    else return 0;

    unsigned char c = b[sc], d = b[sc + 1];
    int t4 = c & 0x1F;
    int t5 = (c >> 1) & 0x3F;
    int layer = ((c & 1) << 5) | (d >> 3);
    int tid = d & 7;

    int ok264 = !(c & 0x80) && t4 >= 1 && t4 <= 23;
    int ok265 = !(c & 0x80) && layer == 0 && tid != 0 && t5 <= 40;

    if (ok265 && !(ok264 && (t4 == 7 || t4 == 9 || t4 == 6))) return SN_H265;
    if (ok264) return SN_H264;
    if (ok265) return SN_H265;
    return 0;
}

/* ------------------------------------------------------------ the table -- */

/* Identify `b` (the first `n` bytes of a file, or of a stream). Returns SN_*.
 * SN_TEXT and SN_UNKNOWN are the two fallbacks and are decided by sn_textish(). */
static inline int sniff_id(const unsigned char *b, int n)
{
    if (n <= 0) return SN_TEXT;                       /* empty prints as nothing */

    /* --- images (the same set c/lib/image/img.c registers detectors for) --- */
    if (sn_eq(b, n, 0, "\x89PNG\r\n\x1a\n", 8)) return SN_PNG;
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return SN_JPEG;
    if (sn_eq(b, n, 0, "GIF87a", 6) || sn_eq(b, n, 0, "GIF89a", 6)) return SN_GIF;
    if (sn_eq(b, n, 0, "BM", 2) && n >= 6) return SN_BMP;
    if (sn_eq(b, n, 0, "RIFF", 4) && sn_eq(b, n, 8, "WEBP", 4)) return SN_WEBP;
    if (sn_eq(b, n, 0, "RIFF", 4) && sn_eq(b, n, 8, "WAVE", 4)) return SN_WAV;

    /* --- fonts: the file that started this ------------------------------- */
    /* 0x00010000 is the sfnt version of a TrueType outline font -- and it is
     * also four bytes with two NULs in them, which is exactly why printing it
     * is the wrong thing to do. */
    if (n >= 4 && b[0] == 0 && b[1] == 1 && b[2] == 0 && b[3] == 0) return SN_TTF;
    if (sn_eq(b, n, 0, "true", 4)) return SN_TTF;      /* older Apple sfnt      */
    if (sn_eq(b, n, 0, "OTTO", 4)) return SN_OTF;      /* CFF outlines          */
    if (sn_eq(b, n, 0, "ttcf", 4)) return SN_TTC;      /* collection            */
    if (sn_eq(b, n, 0, "wOFF", 4) || sn_eq(b, n, 0, "wOF2", 4)) return SN_WOFF;

    /* --- media ----------------------------------------------------------- */
    { int v = sn_annexb(b, n); if (v) return v; }
    if (sn_eq(b, n, 4, "ftyp", 4)) return SN_MP4;
    if (sn_eq(b, n, 0, "\x1a\x45\xdf\xa3", 4)) return SN_MKV;
    if (sn_eq(b, n, 0, "fLaC", 4)) return SN_FLAC;
    if (sn_eq(b, n, 0, "OggS", 4)) return SN_OGG;
    if (sn_eq(b, n, 0, "ID3", 3)) return SN_MP3;
    if (n >= 2 && b[0] == 0xFF && (b[1] & 0xE0) == 0xE0) return SN_MP3;   /* bare frame sync */

    /* --- executables and archives ---------------------------------------- */
    if (sn_eq(b, n, 0, "\x7f" "ELF", 4)) return SN_ELF;
    if (sn_eq(b, n, 0, "AEX1", 4) || sn_eq(b, n, 0, "LAEX", 4)) return SN_AEX;
    if (sn_eq(b, n, 0, "PK\x03\x04", 4)) return SN_ZIP;
    if (n >= 2 && b[0] == 0x1F && b[1] == 0x8B) return SN_GZIP;
    if (sn_eq(b, n, 0, "%PDF-", 5)) return SN_PDF;

    /* --- text-shaped markup that only a human can tell from prose --------- */
    {
        int i = 0;
        while (i < n && (b[i] == ' ' || b[i] == '\t' || b[i] == '\n' || b[i] == '\r')) i++;
        if (sn_eq(b, n, i, "<svg", 4) || sn_eq(b, n, i, "<?xml", 5)) {
            /* An SVG is text AND an image. It is reported as an image because
             * that is the useful answer; it stays printable, so nothing is lost
             * if a consumer treats it as text instead. */
            for (int k = i; k + 4 <= n; k++) if (sn_eq(b, n, k, "<svg", 4)) return SN_SVG;
        }
    }

    /* --- neither: is it printable? --------------------------------------- */
    {
        int ctl = 0;
        for (int i = 0; i < n; i++) {
            if (b[i] == 0) return SN_UNKNOWN;          /* one NUL is decisive   */
            if (sn_ctl(b[i])) ctl++;
        }
        /* 2% control bytes. A text file has none; a binary that got this far
         * without a NUL in its first block is still riddled with them. */
        if (ctl * 50 > n) return SN_UNKNOWN;
        return SN_TEXT;
    }
}

static inline const char *sniff_name(int k)
{
    switch (k) {
    case SN_TEXT:   return "text";
    case SN_PNG:    return "PNG image";
    case SN_JPEG:   return "JPEG image";
    case SN_GIF:    return "GIF image";
    case SN_BMP:    return "BMP image";
    case SN_SVG:    return "SVG image";
    case SN_WEBP:   return "WebP image";
    case SN_H264:   return "H.264 video";
    case SN_H265:   return "H.265 video";
    case SN_MP4:    return "MP4 container";
    case SN_MKV:    return "Matroska container";
    case SN_TTF:    return "TrueType font";
    case SN_OTF:    return "OpenType font";
    case SN_TTC:    return "TrueType font collection";
    case SN_WOFF:   return "WOFF web font";
    case SN_WAV:    return "WAV audio";
    case SN_FLAC:   return "FLAC audio";
    case SN_MP3:    return "MP3 audio";
    case SN_OGG:    return "Ogg audio";
    case SN_ELF:    return "ELF executable";
    case SN_AEX:    return "LogitOS executable";
    case SN_ZIP:    return "ZIP archive";
    case SN_GZIP:   return "gzip data";
    case SN_PDF:    return "PDF document";
    default:        return "binary data";
    }
}

static inline int sniff_class(int k)
{
    switch (k) {
    case SN_TEXT:  return SNC_TEXT;
    case SN_PNG: case SN_JPEG: case SN_GIF: case SN_BMP: case SN_SVG: case SN_WEBP:
        return SNC_IMAGE;
    case SN_H264: case SN_H265:
        return SNC_VIDEO;
    default:
        return SNC_OPAQUE;
    }
}

/* Which app should open it, for the "and here is what to do instead" half of a
 * refusal. A refusal that only says no is a worse experience than the bug. */
static inline const char *sniff_opener(int k)
{
    switch (sniff_class(k)) {
    case SNC_IMAGE: return "show";
    case SNC_VIDEO: return "show";
    default:        return 0;
    }
}

/* ------------------------------------------------------------- hexdump --- */

/* "00 01 00 00 00 10 01 00" -- the first `n` bytes, space separated. Returns
 * the number of characters written (never more than max-1, always NUL-set). */
static inline int sniff_hex(const unsigned char *b, int n, char *out, int max)
{
    static const char H[] = "0123456789abcdef";
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (k + 4 > max) break;
        if (i) out[k++] = ' ';
        out[k++] = H[(b[i] >> 4) & 0xF];
        out[k++] = H[b[i] & 0xF];
    }
    if (max > 0) out[k < max ? k : max - 1] = 0;
    return k;
}

/* --------------------------------------------------- the stream guard ---- */

/* The terminal's backstop. Bytes are fed as they arrive on fd 1 / fd 2; the
 * guard latches `binary` the first time the stream proves it is not text, and
 * the terminal then stops painting glyphs and paints one summary line instead.
 *
 * The rule is deliberately LIVE rather than "decide from the first block": a
 * program that prints a page of text and then dumps a font must still be caught
 * at the font. So the evidence counter runs over a sliding 256-byte window --
 * a NUL anywhere, or four control bytes inside one window, and the stream is
 * binary from there on. Latching is one-way: once a stream has been judged
 * binary for a command, the rest of that command's output is summarized, since
 * a stream that interleaves the two is not something a grid can show anyway.
 *
 * `head` keeps the first 16 bytes so the summary can name the format (the same
 * sniff_id() the producer uses) and hexdump the start. */
#define SN_WINDOW  256
#define SN_CTL_MAX 4
#define SN_HEAD    16

struct sniff_guard {
    unsigned long bytes;              /* total bytes fed                     */
    unsigned long suppressed;         /* bytes fed after the latch           */
    int win_bytes, win_ctl;
    int binary;                       /* latched                             */
    unsigned char head[SN_HEAD];
    int nhead;
};

static inline void sniff_guard_reset(struct sniff_guard *g)
{
    g->bytes = 0; g->suppressed = 0;
    g->win_bytes = 0; g->win_ctl = 0;
    g->binary = 0; g->nhead = 0;
}

/* What the stream looks like, asked LAZILY rather than at the moment of the
 * latch. A font trips the guard on its very first byte -- the 0x00 of the sfnt
 * version -- when one byte is not enough to name the format; by the time the
 * summary is drawn the head buffer is full and the answer is "TrueType font"
 * rather than "binary data". When the binary began mid-stream the head holds
 * the text that preceded it and the honest answer is the generic one. */
static inline int sniff_guard_kind(const struct sniff_guard *g)
{
    int k = sniff_id(g->head, g->nhead);
    return k == SN_TEXT ? SN_UNKNOWN : k;
}

/* Feed one byte. Returns 1 on the transition into binary (once per stream), so
 * the caller can emit its announcement exactly once. */
static inline int sniff_guard_byte(struct sniff_guard *g, unsigned char c)
{
    if (g->nhead < SN_HEAD) g->head[g->nhead++] = c;
    g->bytes++;
    if (g->binary) { g->suppressed++; return 0; }

#ifdef SNIFF_NEGATIVE_CONTROL
    /* The negative control (make test-sniff-negctl): the guard sees every byte
     * and never latches -- i.e. the old behaviour, where anything printable-ish
     * reached the grid. The suite must FAIL when built this way, or it is not
     * testing the guard. */
    (void)c;
    return 0;
#else
    if (++g->win_bytes >= SN_WINDOW) { g->win_bytes = 0; g->win_ctl = 0; }
    int hit = 0;
    if (c == 0) hit = 1;
    else if (sn_ctl(c) && ++g->win_ctl >= SN_CTL_MAX) hit = 1;
    if (!hit) return 0;

    g->binary = 1;
    g->suppressed = 0;
    return 1;
#endif
}

static inline int sniff_guard_feed(struct sniff_guard *g, const unsigned char *p, int n)
{
    int trip = 0;
    for (int i = 0; i < n; i++) if (sniff_guard_byte(g, p[i])) trip = 1;
    return trip;
}

#endif /* LOGIT_SNIFF_H */
