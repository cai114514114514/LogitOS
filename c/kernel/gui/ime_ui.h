#ifndef LOGIT_IME_UI_H
#define LOGIT_IME_UI_H

/* The pinyin input method: the composition state machine and its candidate
 * bar. The engine (segmentation + candidate lookup) is c/lib/ime/pinyin.c and
 * knows nothing about windows; this file is everything between a scancode and
 * a Unicode codepoint on its way to an app.
 *
 * WHY IT IS IN THE WINDOW MANAGER AND NOT IN AN APP. The WM already owns the
 * dispatch -- c/kernel/gui/wm.c:wm_process_key() is the single point every
 * keystroke passes through on its way to the focused window's event queue --
 * so an IME that lives anywhere else is either a second dispatch path or a
 * per-app reimplementation. The rejected alternative was an ime.aex daemon
 * with a syscall pair: it needs a way to inject events into another process's
 * queue, which is a capability nothing in this tree has and which would be a
 * strictly larger change than this one, for a feature that is by definition
 * system-wide.
 *
 * WHAT AN APP SEES. Nothing new. A committed character arrives as an ordinary
 * EV_KEY with `a` = the Unicode codepoint and mods = 0, one event per
 * character -- exactly the shape an ASCII keystroke already has. No app in
 * this tree is edited for this feature and none has to be.
 *
 * ---------------------------------------------------------------------------
 * WM-HOOKS -- the whole of what this asks of c/kernel/gui/wm.c
 * ---------------------------------------------------------------------------
 * Four statements and one #include, in the style notify.h established:
 *
 *   1. #include "ime_ui.h"
 *
 *   2. in wm_init(), after the filesystem is mounted (the dictionary is read
 *      from it):
 *          ime_ui_init();
 *
 *   3. in wm_process_key(), immediately BEFORE the enqueue_input(EV_KEY) that
 *      hands the key to the app -- see ime_ui_key()'s contract for the
 *      three-way return and for the cost of the not-composing path:
 *          uint32_t cps[IME_UI_MAXCP];
 *          int n = ime_ui_key(wi, c, mods, cps, IME_UI_MAXCP);
 *          if (n >= 0) { ...deliver cps as EV_KEY...; return; }
 *
 *   4. in render_region(), AFTER the dock/menu and BEFORE notify_compose() --
 *      the bar sits above every window and below a notification, because a
 *      notification is the system speaking and the bar is the user typing:
 *          ime_ui_compose();
 *      No rect_hit() guard, for notify.c's reason: the fb clip is already the
 *      damage rectangle and every primitive used here is clip-exact.
 *
 * And one thing in the other direction, declared in wm.h: wm_ime_anchor(),
 * which is how this file learns where the focused window is without reaching
 * into wm.c's statics.
 * --------------------------------------------------------------------------- */

#include <stdint.h>

/* The most codepoints one commit can produce. Equal to the engine's
 * IME_CAND_MAXCP (c/lib/ime/pinyin.h), which is measured against the shipped
 * dictionary at 15 (the longest phrase) with headroom to 20 -- EXCEPT on the
 * raw-literal path (Enter), which commits up to IME_MAX_RAW = 64 typed ASCII
 * letters. 64 is therefore the real bound and the one used here; sizing this
 * to 20 would silently truncate a long mistyped word on the one path whose
 * whole job is to give the user back exactly what they typed. */
#define IME_UI_MAXCP 64

/* Windows this file keeps state for. Must be >= wm.c's MAXWIN, and wm.c
 * _Static_asserts exactly that beside its own definition -- a drift here would
 * otherwise be a silent "the IME does not work in the last window you opened",
 * because ime_ui_key() bounds-checks and returns "not consumed". */
#define IME_UI_MAXWIN 16

/* Load /ime/pinyin.dat and open it. 1 = the IME is available; 0 = it is not,
 * and ime_ui_key() will REFUSE the Ctrl+Space toggle out loud (a serial line
 * naming the file) rather than silently doing nothing. Safe to call twice. */
int  ime_ui_init(void);

/* WM-HOOK 3: one key from the focused window's stream, before the app sees it.
 *
 * Returns  -1  the key was NOT consumed: hand it to the app unchanged. This is
 *              the ONLY path an ASCII keystroke takes while the IME is off,
 *              and it is deliberately the first thing the function decides
 *              (see ime_ui.c's cost note and the measured instruction count).
 *          >=0 the key WAS consumed. The value is how many codepoints were
 *              committed and written to `out` -- 0 for a key that only changed
 *              the composition (a letter, a page turn, Escape), N for a commit.
 *
 * `wi` is the focused window's index (wins[] in wm.c); the composition is
 * per-window and switching focus parks the outgoing one and restores the
 * incoming one. `mods` is the EV_MOD_* mask. */
int  ime_ui_key(int wi, int c, int mods, uint32_t *out, int max);

/* WM-HOOK 4: draw the candidate bar into the current fb target. Draws nothing
 * and costs one predictable branch when no composition is open. */
void ime_ui_compose(void);

/* A window is going away: drop any composition it owned, so its slot cannot be
 * reused with a stale composition attached. Called from wm.c's window teardown.
 * Cheap and idempotent. */
void ime_ui_win_gone(int wi);

/* For the device gate and for `about:` style introspection: 1 while a
 * composition is open, and the IME's on/off state for a window. */
int  ime_ui_composing(void);
int  ime_ui_enabled(int wi);

#endif /* LOGIT_IME_UI_H */
