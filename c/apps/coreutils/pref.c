#include "clib.h"

/* pref -- the defaults(1) of this machine.
 *
 * Every kernel-schema key (nine of them: ui.dark, net.ip, ...) already had a
 * store, a GUI and a host test. What had NOTHING was an application: no
 * ported program, and no LogitOS app either, had anywhere to remember a
 * window size across a launch. `pref` is the coreutil front end onto the
 * SAME kernel store (c/kernel/core/settings.c) that closes that hole for
 * anything under the "app." key prefix -- see the SYS_SETTING_ENUM comment
 * in include/abi/logit_abi.h, which is the contract this file is written
 * against: keys in the kernel's fixed SCHEMA are machine state (a non-root
 * write is refused), keys under "app." are preferences (any process may
 * write its own, no schema entry needed).
 *
 *   pref read   <key>            value, or "unset" (exit 1)
 *   pref write  <key> <value...> SET + commit; refusals print WHY
 *   pref delete <key>            see do_delete() -- honestly refused today
 *   pref list   [prefix]         every live key ("key=value"), one per line;
 *                                 no prefix lists the machine SCHEMA
 *
 * `list`'s prefix walk goes through SETCTL_KVCOUNT/KVAT rather than
 * SYS_SETTING_ENUM's third argument, even though settings.c's ENUM case now
 * ALSO walks the prefix correctly (schema and apptab[] as one indexed
 * sequence -- see its comment in c/kernel/core/settings.c). Both are correct
 * today; KVAT is used because it is the better fit, not because ENUM does not
 * work:
 *   (1) KVAT was ALREADY documented for exactly this ("how an app lists keys
 *       it has never heard of") and settings_kv_count()/kv_at() already walk
 *       ntab+napp as one sequence, so filtering by prefix client-side needed
 *       no kernel change to lean on.
 *   (2) ENUM's payload is struct logit_setting -- label/type/group/bounds,
 *       built for a Settings GUI rendering a control. `pref list` prints
 *       "key=value" text; KVAT already returns exactly that and nothing
 *       else it would have to discard.
 * `pref list` with NO prefix uses ENUM (prefix 0, the schema walk) instead,
 * because that IS the more useful default here: every key this machine's
 * schema knows about, the same view `setting_enum` has always given a
 * renderer, with no dependence on how many app-domain keys happen to exist.
 *
 * EVERY LINE THIS PROGRAM PRINTS GOES OUT IN ONE sys_write(). The serial
 * console (this OS's boot/test channel -- see CLAUDE.md) is a single shared
 * wire: the kernel's own diagnostics (the WM's frame-perf log, in
 * particular) can and do land on it between two syscalls of the SAME
 * process, at any point the scheduler chooses to preempt. clib.h's
 * outs()/outc() are each a SEPARATE sys_write -- fine for a program with no
 * scripted reader, wrong for one whose output a test harness greps a line at
 * a time, because "PREF-WRITE-OK ui.dark" and "=1\n" written as two syscalls
 * can come back on either side of an unrelated kernel log line. This was not
 * a theoretical worry: it is exactly what run-pref-test.sh's first real run
 * hit. Below, every logical line is assembled in a local buffer first and
 * handed to sys_write() once. */

#define VALBUF 512   /* Generous local staging buffer for joining argv into one
                       * value. The KERNEL enforces the real per-value limit
                       * (SETLIM_VALLEN) and REFUSES a value that does not fit
                       * rather than truncating it (see LOGIT_SET_ETOOBIG in
                       * logit_abi.h) -- this buffer only has to be big enough
                       * that pref's OWN argv-joining step is never the thing
                       * that silently clips a value before the kernel ever
                       * sees it. If joining would overflow it anyway, that is
                       * refused too, loudly, below. */
#define LINEBUF 384  /* One assembled output line -- see the file comment on
                       * why this is one sys_write() rather than several. */

struct oline { char b[LINEBUF]; int n; };

static void ol_s(struct oline *o, const char *s)
{ while (s && *s && o->n < LINEBUF - 1) o->b[o->n++] = *s++; }
static void ol_c(struct oline *o, char c)
{ if (o->n < LINEBUF - 1) o->b[o->n++] = c; }
static void ol_flush(struct oline *o, int fd)
{ if (o->n > 0) sys_write(fd, o->b, o->n); o->n = 0; }

static void fmt_dec(char *b, int cap, long v)
{
    char t[24]; int n = 0, p = 0;
    unsigned long u = v < 0 ? (unsigned long)(-v) : (unsigned long)v;
    if (!u) t[n++] = '0';
    while (u && n < (int)sizeof t) { t[n++] = (char)('0' + u % 10); u /= 10; }
    if (v < 0 && p < cap - 1) b[p++] = '-';
    while (n && p < cap - 1) b[p++] = t[--n];
    if (p < cap) b[p] = 0; else if (cap > 0) b[cap - 1] = 0;
}

static void ol_dec(struct oline *o, long v) { char b[24]; fmt_dec(b, sizeof b, v); ol_s(o, b); }

/* Is `key` one this kernel's SCHEMA knows -- i.e. machine state, per the
 * "the key namespace is the permission model" contract. Walked fresh each
 * call rather than cached: a whole machine's schema is a handful of entries
 * (nine, today), so this costs a handful of syscalls, and a cache could go
 * stale if a future schema key is added between two calls in a script. */
static int is_schema_key(const char *key)
{
    struct logit_setting s;
    for (int i = 0; setting_enum(i, &s) == 0; i++)
        if (c_streq(s.key, key)) return 1;
    return 0;
}

static int do_read(const char *key)
{
    char buf[LOGIT_SET_VALMAX];
    int n = setting_get(key, buf, (int)sizeof buf);
    struct oline o; o.n = 0;
    if (n < 0) { ol_s(&o, "unset\n"); ol_flush(&o, 1); return 1; }
    ol_s(&o, buf); ol_c(&o, '\n');
    ol_flush(&o, 1);
    return 0;
}

/* SYS_SETTING_SET's -1 is ambiguous BY CONSTRUCTION: LOGIT_SET_EBADKEY and
 * ID_E_PERM (the code a non-root machine-key write is refused with -- see the
 * SYS_SETTING_ENUM comment in logit_abi.h) are the same numeric value. The
 * LOGIT_SET_EBADKEY case below disambiguates the three real causes that can
 * produce it, using only information already available client-side. */
static int do_write(const char *key, const char *val)
{
    int rc = setting_set(key, val, 1 /* commit now: one call, one key, one
                                       * whole-file write -- see setting_set's
                                       * doc comment in logit.h for why a
                                       * batch of several keys would want
                                       * commit=0 + a trailing setting_commit()
                                       * instead; a single `pref write` is not
                                       * that case. */);
    struct oline o; o.n = 0;
    if (rc == 0) {
        ol_s(&o, "PREF-WRITE-OK "); ol_s(&o, key); ol_c(&o, '='); ol_s(&o, val); ol_c(&o, '\n');
        ol_flush(&o, 1);
        return 0;
    }

    int is_app = c_strncmp(key, "app.", 4) == 0;

    ol_s(&o, "pref: write "); ol_s(&o, key); ol_s(&o, ": refused: ");
    switch (rc) {
    case LOGIT_SET_ETOOBIG:
        /* SETLIM_VALLEN is one shared bound (struct kv's v[] is the same
         * shape in tab[] and apptab[]) -- accurate for either key kind. */
        ol_s(&o, "value too long (max "); ol_dec(&o, setting_limit(SETLIM_VALLEN)); ol_s(&o, " bytes)\n");
        break;
    case LOGIT_SET_EFULL:
        /* SETLIM_MAXKEYS/FREE report tab[]'s (schema table's) capacity only
         * -- settings.c keeps app.* keys in a SEPARATE table (apptab[],
         * capacity SET_APP_MAXKV) with no counterpart exposed through
         * SETCTL_LIMITS. Printing tab[]'s numbers for an apptab[] overflow
         * would be a precise-looking LIE, so an app-key EFULL gets an honest
         * "no number for this" instead of a wrong one. */
        if (is_app) {
            ol_s(&o, "the per-user preference table is full (its capacity is not exposed to this program)\n");
        } else {
            int max = setting_limit(SETLIM_MAXKEYS), free_ = setting_limit(SETLIM_FREE);
            ol_s(&o, "store full ("); ol_dec(&o, max - free_); ol_c(&o, '/'); ol_dec(&o, max); ol_s(&o, " keys used)\n");
        }
        break;
    case LOGIT_SET_EIO:
        ol_s(&o, "write failed (I/O error)\n");
        break;
    case LOGIT_SET_EBADKEY:
        /* THREE distinct kernel-level causes collapse to this one code, and
         * all three are disambiguated here with no extra syscall beyond the
         * schema walk do_write() already had to make:
         *   - a real schema key: the syscall's permission gate returned
         *     ID_E_PERM, numerically the SAME value as LOGIT_SET_EBADKEY (see
         *     the SYS_SETTING_ENUM comment in logit_abi.h) -- refused for who
         *     is asking, not for what was typed.
         *   - "app."-prefixed but still refused: is_app_key() accepted the
         *     namespace, so put_ex()'s key_ok() must be what said no --
         *     genuinely bad syntax (illegal characters, empty, or too long).
         *   - neither: settings_syscall() now refuses outright any key that
         *     is not a schema key and not under "app." (nobody's namespace to
         *     write into), a DIFFERENT reason than bad syntax even though the
         *     key text itself may be perfectly well-formed. */
        if (is_schema_key(key))      ol_s(&o, "machine key, root only\n");
        else if (!is_app)            ol_s(&o, "not a machine key and not under \"app.\" -- no namespace claims it\n");
        else                         ol_s(&o, "invalid key (bad characters, empty, or too long)\n");
        break;
    default:
        ol_s(&o, "rc="); ol_dec(&o, rc); ol_c(&o, '\n');
        break;
    }
    ol_flush(&o, 2);
    return 1;
}

/* THIS KERNEL HAS NO KEY-REMOVAL PRIMITIVE, and this function says so rather
 * than faking one. SYS_SETTING_SET can overwrite a value, and -- as an
 * emergent property of how settings_get_str() treats an empty stored string
 * (raw()[0] false, falls through past the schema default to the caller's own
 * NULL) -- writing "" already makes a LATER SYS_SETTING_GET report the key as
 * unset. That is not deletion: the slot is still occupied, SETCTL_KVAT (and
 * so `pref list`) would still print it as "key=", which is exactly the lie a
 * `delete` that claims to have worked would be telling. Refuse honestly
 * instead. If settings.c ever grows a real removal op, this is the one
 * function that changes. */
static int do_delete(const char *key)
{
    (void)key;
    errs("pref: delete: unsupported -- this store has no key-removal primitive\n");
    return 1;
}

static void list_schema(void)
{
    struct logit_setting s;
    for (int i = 0; setting_enum(i, &s) == 0; i++) {
        struct oline o; o.n = 0;
        ol_s(&o, s.key); ol_c(&o, '='); ol_s(&o, s.value); ol_c(&o, '\n');
        ol_flush(&o, 1);
    }
}

static void list_prefix(const char *prefix)
{
    char buf[LOGIT_SET_KVLINE];
    int plen = c_strlen(prefix);
    int n = setting_kv_count();
    for (int i = 0; i < n; i++) {
        int len = setting_kv_at(i, buf);
        if (len < 0) continue;
        if (c_strncmp(buf, prefix, plen) == 0) {
            struct oline o; o.n = 0;
            ol_s(&o, buf); ol_c(&o, '\n');
            ol_flush(&o, 1);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        errs("usage: pref read <key> | write <key> <value...> | delete <key> | list [prefix]\n");
        return 1;
    }

    if (c_streq(argv[1], "read")) {
        if (argc < 3) { errs("usage: pref read <key>\n"); return 1; }
        return do_read(argv[2]);
    }

    if (c_streq(argv[1], "write")) {
        if (argc < 4) { errs("usage: pref write <key> <value...>\n"); return 1; }
        /* Join argv[3..] with single spaces -- one shell word is the common
         * case (a window dimension, a bool, a path) but a preference is not
         * defined to be word-shaped, and re-quoting is the caller's job, not
         * a byte this program is allowed to have opinions about dropping. */
        char val[VALBUF];
        int p = 0, overflow = 0;
        for (int i = 3; i < argc && !overflow; i++) {
            const char *w = argv[i];
            for (int j = 0; w[j]; j++) {
                if (p >= VALBUF - 1) { overflow = 1; break; }
                val[p++] = w[j];
            }
            if (!overflow && i + 1 < argc) {
                if (p >= VALBUF - 1) { overflow = 1; }
                else val[p++] = ' ';
            }
        }
        if (overflow) {
            errs("pref: write: value too long to construct (over ");
            char b[16]; fmt_dec(b, sizeof b, VALBUF - 1); errs(b); errs(" bytes)\n");
            return 1;
        }
        val[p] = 0;
        return do_write(argv[2], val);
    }

    if (c_streq(argv[1], "delete")) {
        if (argc < 3) { errs("usage: pref delete <key>\n"); return 1; }
        return do_delete(argv[2]);
    }

    if (c_streq(argv[1], "list")) {
        if (argc >= 3) list_prefix(argv[2]);
        else           list_schema();
        return 0;
    }

    errs("pref: unknown subcommand '"); errs(argv[1]); errs("'\n");
    return 1;
}
