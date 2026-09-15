/* Author extension declarations share LibCSS's matched selector specificity.
 * The old block parser chose the last textual value, then patched whole rules
 * in source order. The 22-check regression measured 11 wrong results, including
 * an ordinary inline declaration defeating a stylesheet !important. Split
 * importance BEFORE merging declarations, and preserve declaration order so a
 * shorthand can replace an earlier longhand. Raw spans retain their original
 * sheet/attribute owner; no temporary buffer may escape into computed style.
 * This is the author, unlayered extension cascade. Layers, user/UA extension
 * origins and physical-vs-logical competition with LibCSS remain separate work.
 */
static void xpatch_merge(struct xpatch *a, const struct xpatch *b)
{
    if(b->backface_set){a->backface_set=1;a->backface=b->backface;}
    if(b->preserve_set){a->preserve_set=1;a->preserve=b->preserve;}
    a->do_none |= b->do_none; a->do_masked |= b->do_masked;
    if (b->do_grid) {
        a->do_grid = 1; a->gcols = b->gcols;
        memcpy(a->gtracks, b->gtracks, sizeof a->gtracks);
    }
    if (b->gx_set) { a->gx_set = 1; a->gx = b->gx; }
    if (b->gy_set) { a->gy_set = 1; a->gy = b->gy; }
    if (b->anim) a->anim = b->anim;
    if (b->trans_raw) { a->trans_op = b->trans_op; a->trans_raw = b->trans_raw; a->trans_rawlen = b->trans_rawlen; }
    if (b->anim_raw) { a->anim_raw = b->anim_raw; a->anim_rawlen = b->anim_rawlen; }
    for (int i = 0; i < LGX__COUNT; i++) if (b->lg_set[i]) { a->lg_set[i] = 1; a->lg[i] = b->lg[i]; }
    for (int i = 0; i < 4; i++) if (b->inset_raw[i]) { a->inset_raw[i] = b->inset_raw[i]; a->inset_len[i] = b->inset_len[i]; }
    for (int i = 0; i < 4; i++) if (b->margin_raw[i]) { a->margin_raw[i] = b->margin_raw[i]; a->margin_len[i] = b->margin_len[i]; }
    for (int i = 0; i < 2; i++) if (b->gap_raw[i]) { a->gap_raw[i] = b->gap_raw[i]; a->gap_len[i] = b->gap_len[i]; }
    for (int i = 0; i < 4; i++) if (b->padding_raw[i]) { a->padding_raw[i] = b->padding_raw[i]; a->padding_len[i] = b->padding_len[i]; }
    for (int i = 0; i < GR__COUNT; i++) if (b->gr[i]) { a->gr_any = 1; a->gr[i] = b->gr[i]; a->gr_len[i] = b->gr_len[i]; }
    for (int i = 0; i < XR__COUNT; i++) if (b->xr[i]) { a->xr_any = 1; a->xr[i] = b->xr[i]; a->xr_len[i] = b->xr_len[i]; }
}

static int xc_trivia(const char *s, int i, int end)
{
    for (;;) {
        while (i < end && spc(s[i])) i++;
        if (i + 1 >= end || s[i] != '/' || s[i+1] != '*') return i;
        i += 2;
        while (i + 1 < end && !(s[i] == '*' && s[i+1] == '/')) i++;
        i = i + 1 < end ? i + 2 : end;
    }
}

static void parse_cascade_decls(const char *s, int len, struct xpatch *normal, struct xpatch *important)
{
    memset(normal, 0, sizeof *normal); memset(important, 0, sizeof *important);
    int i = 0;
    while (i < len) {
        i = xc_trivia(s, i, len);
        if (i == len) break;
        if (s[i] == ';') { i++; continue; }
        int start = i, bang = -1, depth = 0, colon = 0;
        char quote = 0;
        for (; i < len; i++) {
            char c = s[i];
            if (c == '\\' && i + 1 < len) { i++; continue; }
            if (quote) { if (c == quote) quote = 0; continue; }
            if (c == '\'' || c == '"') { quote = c; continue; }
            if (c == '/' && i + 1 < len && s[i+1] == '*') { i = xc_trivia(s, i, len) - 1; continue; }
            if (c == '(' || c == '[' || c == '{') { depth++; continue; }
            if (c == ')' || c == ']' || c == '}') { if (depth) depth--; continue; }
            if (!depth && c == ':') colon = 1;
            if (!depth && c == '!') bang = i;
            if (!depth && c == ';') break;
        }
        int end = i, imp = 0;
        if (bang >= 0) {
            int j = xc_trivia(s, bang + 1, end), k = 0;
            const char *word = "important";
            for (; k < 9 && j + k < end; k++) {
                char c = s[j+k]; if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                if (c != word[k]) break;
            }
            if (k == 9 && xc_trivia(s, j + k, end) == end) { imp = 1; end = bang; }
        }
        if (colon && end > start) {
            struct xpatch p;
            parse_decls(s + start, end - start, &p);
            xpatch_merge(imp ? important : normal, &p);
        }
        if (i < len) i++;
    }
}
