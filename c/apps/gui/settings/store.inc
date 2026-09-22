/* SPDX-License-Identifier: GPL-3.0-or-later */
static void kv_split(const char *line, char *k, char *v)
{
    int e = 0;
    while (line[e] && line[e] != '=') e++;
    int n = e < 47 ? e : 47;
    for (int i = 0; i < n; i++) k[i] = line[i];
    k[n] = 0;
    s_cpy(v, line[e] ? line + e + 1 : "", LOGIT_SET_VALMAX);
}

/* The generated view. THREE columns, and the third is the one that earns the
 * tab: a value is "stored" (this machine's, on disk), "default" (nothing stored,
 * the compiled-in answer) or "rejected" (something IS stored and the schema
 * refused it, so the machine is running on the default instead).
 *
 * Every schema key is listed even when nothing is stored, because a fresh
 * machine has an empty store and a table with no rows in it tells a user
 * nothing -- least of all the user who came here to find out what a setting is
 * called. And the RAW stored bytes are what a rejected row shows, not the
 * effective value: the whole point of showing it is so it can be repaired. */
static void load_kv(void)
{
    nkv = 0;
    int ns = setting_count();
    for (int i = 0; i < ns && nkv < MAXKV; i++) {
        struct logit_setting s;
        if (setting_enum(i, &s) < 0) break;
        s_cpy(kv_key[nkv], s.key, 48);

        /* Is anything actually stored for this key, and what? */
        const char *src = "default";
        char raw[LOGIT_SET_VALMAX];
        raw[0] = 0;
        int stored = setting_kv_count();
        for (int j = 0; j < stored; j++) {
            char line[LOGIT_SET_KVLINE], k[48], v[LOGIT_SET_VALMAX];
            if (setting_kv_at(j, line) < 0) break;
            kv_split(line, k, v);
            if (s_eq(k, s.key)) { s_cpy(raw, v, LOGIT_SET_VALMAX); break; }
        }
        if (raw[0]) {
            s_cpy(kv_val[nkv], raw, LOGIT_SET_VALMAX);
            src = s_eq(raw, s.value) ? "stored" : "rejected";
        } else {
            s_cpy(kv_val[nkv], s.value, LOGIT_SET_VALMAX);
        }
        kv_src[nkv] = src;
        nkv++;
    }

    /* Then every stored key the schema does not name -- another line's state,
     * kept across rewrites precisely so it can live here. */
    int stored = setting_kv_count();
    for (int j = 0; j < stored && nkv < MAXKV; j++) {
        char line[LOGIT_SET_KVLINE], k[48], v[LOGIT_SET_VALMAX];
        if (setting_kv_at(j, line) < 0) break;
        kv_split(line, k, v);
        int known = 0;
        for (int i = 0; i < ns; i++) {
            struct logit_setting s;
            if (setting_enum(i, &s) < 0) break;
            if (s_eq(k, s.key)) { known = 1; break; }
        }
        if (known) continue;
        s_cpy(kv_key[nkv], k, 48);
        s_cpy(kv_val[nkv], v, LOGIT_SET_VALMAX);
        kv_src[nkv] = "app";
        nkv++;
    }

    for (int i = 0; i < nkv; i++) {
        kv_cell[i * 3 + 0] = kv_key[i];
        kv_cell[i * 3 + 1] = kv_val[i];
        kv_cell[i * 3 + 2] = kv_src[i];
    }
}
