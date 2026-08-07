#include "clib.h"
#include "logit_rich.h"

/* prog <steps> [label]
 *
 * Exercises the progress widget: under the rich terminal one drawn bar that
 * UPDATES IN PLACE (same RT_T_PROGRESS id), elsewhere one percentage line per
 * step. It is a demonstrator, not a tool -- named honestly, and it is what the
 * on-device pixel test drives to prove the widget is drawn rather than described.
 */

static struct rt_enc E;

static void put_num(unsigned v)
{ char t[16]; int i = 0; if (!v) { rt_out("0"); return; }
  while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
  char o[17]; int k = 0; while (i) o[k++] = t[--i]; o[k] = 0; rt_out(o); }

int main(int argc, char **argv)
{
    rt_init(argc, argv);
    int steps = argc > 1 ? c_atoi(argv[1]) : 10;
    if (steps < 1) steps = 1;
    if (steps > 1000) steps = 1000;
    const char *label = argc > 2 ? argv[2] : "working";

    for (int i = 0; i <= steps; i++) {
        int pm = i * 1000 / steps;
        if (rt_isrich()) {
            rt_reset(&E);
            rt_u32(&E, 1);                        /* one widget, updated in place */
            rt_u16(&E, pm);
            rt_u8(&E, i == steps);
            rt_str(&E, label);
            rt_send(RT_T_PROGRESS, &E);
        } else {
            rt_out(label);
            rt_out(" ");
            put_num((unsigned)(pm / 10));
            rt_out("%\n");
        }
        if (i < steps) sys_sleep_ms(60);
    }
    return 0;
}
