/* Same font/scale as gui_text_run_w, with no drawing. Old kernels return 0. */
static inline int text_run_metrics_px(const char *s, int len, int px, int face,
                                      struct logit_text_metrics *out)
{
    struct logit_text_metrics_query q = {
        sizeof q, LOGIT_TEXT_METRICS_VERSION, s, len, px, face & 3, 0, (unsigned char *)out
    };
    return (int)_sys(SYS_TEXT_MEASURE, (long)&q, LOGIT_TEXT_METRICS_QUERY, 0);
}
