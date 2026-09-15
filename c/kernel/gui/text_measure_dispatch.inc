/* Included inside SYS_TEXT_MEASURE. The host gate includes this same branch;
 * it supplies ordinary usercopy and a pixel target, not a second dispatcher.
 * Negative len was already rejected before pointer access by the width ABI. */

#ifndef LOGIT_TEXT_METRICS_UNSUPPORTED
        if (b == LOGIT_TEXT_METRICS_QUERY && c == 0) {
            struct logit_text_metrics_query q;
            if (user_copy_from(&q, (const void *)a, sizeof q) < 0) return -1;
            if (q.size != sizeof q || q.version != LOGIT_TEXT_METRICS_VERSION)
                return 0;
            if (q.flags || q.len < 0 || q.len > LOGIT_TEXT_METRICS_MAX_BYTES ||
                q.px < 1 || q.px > 512 || (q.face & ~3)) return -1;
            char tmp[USER_TEXT_MAX];
            _Static_assert(LOGIT_TEXT_METRICS_MAX_BYTES < USER_TEXT_MAX,
                           "metrics and draw text limits must agree");
            if (q.len && user_copy_from(tmp, q.s, (unsigned)q.len) < 0) return -1;
            struct logit_text_metrics result;
            /* Snapshot scale once so the returned coordinates and metadata
             * describe one measurement even if the display scale changes. */
            int scale_percent = fb_scale();
            int rc = text_measure_run_metrics(tmp, q.len,
                        q.px * scale_percent / 100, q.face, &result);
            if (rc != 1) return -1;
            result.scale_percent = scale_percent;
            if (user_copy_to(q.out, &result, sizeof result) < 0) return -1;
            return 1;
        }
#endif

        const char *s = (const char *)a; int len = (int)b;
        /* (px << 2) | face, where face is LOGIT_FACE_MONO|LOGIT_FACE_BOLD.
         * It was (px << 1) | mono; bit 0 is unchanged and bit 1 is new, which
         * is why c/apps/logit.h's text_measure_px kept its signature and only
         * widened its mask. Both halves of that encoding live in exactly two
         * places -- there and here. */
        int px = (int)((c >> 2) & 0x3FFFFFFF), face = (int)(c & 3);
        if (len < 0 || len > USER_TEXT_MAX) return 0;
        if (px < 1 || px > 512) return 0;    /* unbounded px overflows the rasterizer's w*h math */
        char tmp[USER_TEXT_MAX];
        if (len > 0 && user_copy_from(tmp, s, (unsigned)len) < 0) return -1;
        /* Measure at the size it will actually be DRAWN at, then answer in
         * points. Measuring at the unscaled px instead would be self-consistent
         * arithmetic and still wrong: hinting-free advances do not scale exactly
         * linearly, so a caller that word-wraps on the 1x width would overflow
         * its own box once the 1.5x glyphs landed. */
        return PT(text_measure(tmp, len, S(px), face));
