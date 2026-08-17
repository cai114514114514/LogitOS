#include "clib.h"
#include "logit_rich.h"

/* clear -- empty the screen.
 *
 * This was the ONLY escape-emitting program in the tree (`\033[2J\033[H`, and a
 * grep over c/, fsroot/, tools/ and tests/ finds no second one), and in the GUI
 * Terminal it did NOTHING: put_char() refuses C0 on purpose -- a character grid
 * that interprets its own input is exactly the in-band control language LRT/1
 * exists to replace -- so both escapes were dropped and the scrollback stayed.
 *
 * The fix is not an escape parser. One caller does not buy a VT state machine,
 * and a second control language beside the side band would be the thing this
 * protocol was written to avoid. It is a frame: RT_T_CLEAR, no payload.
 *
 * The escapes stay for the case they were right for. The serial console really
 * is a VT and has no rich channel, and so does anything that redirects us; there
 * rt_isrich() is 0 and the bytes go to fd 1 exactly as before. Same program, two
 * true statements of the same intent, chosen by which listener is present.
 */
int main(int argc, char **argv)
{
    rt_init(argc, argv);
    /* Not "if rich, frame": if the frame reached the terminal. A channel that
     * died mid-write latches rich mode off inside rt_send, and falling through
     * is then the only way `clear` still does something. */
    if (rt_isrich() && rt_send(RT_T_CLEAR, 0) == 0) return 0;
    outs("\033[2J\033[H");
    return 0;
}
