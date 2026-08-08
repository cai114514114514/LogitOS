#include "clib.h"

/* notify -- raise a system notification from the shell.
 *
 * The point of this program is that it is unremarkable. A notification is a
 * service, not a widget: any process may raise one, it needs no window, no
 * toolkit and no permission, and the caller learns nothing about what happened
 * to it afterwards. A twelve-line coreutil is the honest shape of that.
 *
 *   notify TITLE BODY [level]      level: 0 info (default), 1 warn, 2 error
 *   notify --burst N               N notifications at once, numbered -- the
 *                                  "several arrive together" case, so that the
 *                                  stacking and queueing behaviour can be
 *                                  driven from a test rather than described
 *
 * --burst exists because "what happens when several arrive at once" is a design
 * question the ABI answers in prose (up to NOTIFY_VISIBLE stack, the rest
 * queue, nothing is dropped and nothing is overwritten) and prose is not a
 * test. This is how tests/qmp/qmp_notify.py makes it happen on purpose. */

static void num(char *d, int v)
{
    char t[12]; int i = 0, k = 0;
    if (!v) t[i++] = '0';
    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) d[k++] = t[--i];
    d[k] = 0;
}

int main(int argc, char **argv)
{
    /* There is deliberately no "how many are showing" syscall. A notification
     * is fire-and-forget, and an ABI that lets an app poll the overlay's state
     * is an ABI some app will eventually WAIT on -- at which point it is a
     * dialog again. The kernel prints `[wm] notify ...` with the showing and
     * queued counts on every change instead, which is where a test should be
     * reading it from anyway. */

    if (argc >= 3 && c_streq(argv[1], "--burst")) {
        int n = c_atoi(argv[2]);
        if (n < 1) n = 1;
        if (n > 32) n = 32;
        for (int i = 1; i <= n; i++) {
            char title[32] = "Burst ", body[64] = "notification number ";
            char d[12];
            num(d, i);
            int k = c_strlen(title); for (int j = 0; d[j]; j++) title[k++] = d[j]; title[k] = 0;
            k = c_strlen(body);      for (int j = 0; d[j]; j++) body[k++] = d[j];  body[k] = 0;
            int id = notify(title, body, i % 3);
            outs("NOTIFY_ID "); outn(id); outc('\n');
        }
        return 0;
    }

    if (argc < 3) { errs("usage: notify TITLE BODY [level]   |   notify --burst N\n"); return 2; }
    int level = argc > 3 ? c_atoi(argv[3]) : 0;
    int id = notify(argv[1], argv[2], level);
    outs("NOTIFY_ID "); outn(id); outc('\n');
    return id > 0 ? 0 : 1;
}
