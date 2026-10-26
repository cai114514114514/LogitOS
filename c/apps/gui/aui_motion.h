/* aui_motion.h -- the animation vocabulary and wake contract.
 * Included by aui.h (apps include only aui.h); it relies on logit.h and
 * struct aui_rect from there. Declarations moved verbatim. */
#ifndef AUI_MOTION_H
#define AUI_MOTION_H

/* ==========================================================================
 * MOTION -- the toolkit's animation vocabulary.
 *
 * WHY THIS EXISTS AT ALL, AND WHY IT IS SMALL. Before it, the entire animated
 * surface of this toolkit was four things: aui_mix (a colour lerp), the
 * spinner, the indeterminate progress stripe, and -- claimed but not real --
 * the switch knob. Everything else changed state in one frame. The brief was
 * "far too few animations" AND minimal, and those pull the same way only if the
 * vocabulary is a SMALL CLOSED SET applied everywhere, rather than fifteen
 * bespoke curves. Three curves, three durations, one function.
 *
 * THE VOCABULARY IS NOT INVENTED HERE. The window manager has animated in ring
 * 0 for some time -- Expose, the dock fly-down, the window open pop -- on a
 * quadratic ease-out over 0..256 and an 18-tick (180 ms) duration. The toolkit
 * did not know. Inventing a second curve and a second duration in ring 3 would
 * have put one jar behind two doors on the two numbers a person can literally
 * see at the same time: a segmented pill sliding at one speed while the window
 * behind it minimises at another. So the curve is gfx_ease_out() in c/lib/gfx
 * (the one library BOTH rings link -- wm.c calls it now too) and AUI_T_BASE is
 * the kernel's EX_DUR_TICKS restated in milliseconds.
 *
 * THE BUDGET IS THE DESIGN. Measured on this machine, 2026-08-28, at the
 * shipped 1920x1200: a composite costs ~30 ns per composited pixel, and
 * SYS_GUI_FLUSH carries no rectangle -- so the floor on ANY animated frame is
 * the app's WHOLE CANVAS, not the widget. A 640x480 pt window at 150% is
 * 691,200 device px = ~21 ms per frame. That single number decides the whole
 * table below: 90 ms buys 4 delivered frames and 180 ms buys 8, and four steps
 * of a colour fade is banding the eye forgives while four steps of a 33 px
 * travel is a strobe it does not. Hence the rule, which is enforceable by
 * reading a diff: AUI_T_FAST may only appear next to a colour or an alpha.
 * ========================================================================== */

/* ---- curves ----
 * Three, and no more. A public ease-IN is deliberately absent: "starts slow" is
 * only ever right for something LEAVING, and everything that leaves here leaves
 * on alpha in 90 ms, where in, out and linear are indistinguishable. */
#define AUI_EASE_LINEAR  0   /* LOOPS ONLY. Easing a loop puts a pulse in it
                              * that reads as stutter, not as design.        */
#define AUI_EASE_OUT     1   /* the default; ~90% of everything              */
#define AUI_EASE_INOUT   2   /* rest at BOTH ends. Earned by exactly one
                              * category: an indicator that travels across
                              * open space and whose midpoint the eye tracks
                              * (the segmented pill, the tab underline).     */
/* The rest of the catalog mirrors enum ol_curve numerically (aui_motion.inc
 * asserts it), so a value here IS the ol_ease256_ex curve. Used sparingly:
 * the rule above about a small closed set still stands. */
#define AUI_EASE_IN           3
#define AUI_EASE_OUT_CUBIC    6
#define AUI_EASE_IN_CUBIC     7
#define AUI_EASE_INOUT_CUBIC  8
#define AUI_EASE_OUT_QUART    9
#define AUI_EASE_OUT_EXPO    10
#define AUI_EASE_OUT_BACK    11
#define AUI_EASE_INOUT_BACK  12
/* Bezier presets (CSS cubic-bezier, resolved by ol_ease256_ex). */
#define AUI_EASE_STANDARD    16  /* cubic-bezier(.2, 0, 0, 1)        */
#define AUI_EASE_EMPHASIZED  17  /* cubic-bezier(.05, .7, .1, 1)     */
#define AUI_EASE_DECEL       18  /* cubic-bezier(0, 0, .2, 1)        */
#define AUI_EASE_BACK        19  /* cubic-bezier(.34, 1.56, .64, 1)  */
/* aui_anim() answers on 0..255 (aui_mix's domain), so a curve that overshoots
 * (the back family) is clamped there: the overshoot is visible only through
 * aui_spring(), whose domain is 24.8 and unclamped. */

/* ---- durations, in milliseconds ----
 * One number and two ratios, so there is one thing to change. 180 is not taste:
 * it is the kernel's EX_DUR_TICKS/MINFLY_TICKS (18 ticks at the 100 Hz PIT),
 * already shipped, already known survivable under TCG, already what a window
 * does when it minimises. */
#define AUI_T_FAST   90      /* 0.5x -- colour and alpha, IN PLACE. NEVER geometry. */
#define AUI_T_BASE  180      /* 1.0x -- something MOVES or RESIZES in place         */
#define AUI_T_SLOW  270      /* 1.5x -- a whole NEW surface arrives over content    */

/* ---- cadence ---- these are wake intervals, not durations.
 * A transition runs eight frames and should get every one it can; a loop runs
 * forever and must be cheap. THE SPINNER'S PRICE, stated here so nobody puts
 * one in a full-screen window: at 20 Hz in a 640x480 pt window it is
 * 20 x 691,200 x 30 ns = 415 ms of BKL-held compositor per second of spinning,
 * i.e. 41% of the machine, continuously, for a busy indicator. */
#define AUI_ANIM_TICK_MIN   16   /* the fastest wake a transition may ask for */
#define AUI_ANIM_TICK_LOOP  50   /* the cadence for an ENDLESS animation      */
/* Measured cost of one composited device pixel, 2026-08-28, 1920x1200 4-core
 * TCG. aui_anim_wait() uses it to derive a cadence from the window's own size,
 * so a large window stops asking for frames the compositor cannot make while
 * holding the BKL. A wrong value here makes an animation coarse; it can never
 * make it spin, because the floor is AUI_ANIM_TICK_MIN and the deadline is
 * computed AFTER the frame lands. */
#define AUI_NS_PER_PX       30

/* ---- animation keys ----
 * `key` distinguishes several animations on ONE widget. Named rather than
 * numbered at the call site so a widget that grows a second animation cannot
 * silently collide with its first. */
#define AUI_AK_FACE   0      /* the control face: hover / press cross-fade */
#define AUI_AK_VALUE  1      /* the thing that moves: knob, pill, underline */
#define AUI_AK_FOCUS  2      /* the focus ring's alpha                      */
#define AUI_AK_EXTRA  3

/* ---- the one widget-side call ----
 *
 *     int t = aui_anim(AUI_AK_FACE, hovered ? 255 : 0, AUI_T_FAST, AUI_EASE_OUT);
 *     fill = aui_mix(base, hover_colour, t);
 *
 * Returns where the value IS this frame, on 0..255 -- aui_mix's domain, which
 * was the entire consistency requirement. (The curves are computed on 0..256;
 * this is the SINGLE place that seam is closed, so no widget ever sees a 256.)
 * Must be called between aui_begin() and aui_end(), from inside the widget that
 * owns the animation, AFTER that widget has taken its id.
 *
 * If `target` differs from what the slot was aiming at, the slot RE-AIMS FROM
 * ITS CURRENT VALUE rather than from its original start -- so a pointer that
 * leaves mid-fade reverses out of where the pixels actually are and never
 * snaps. That is the property that makes an interruptible animation look
 * deliberate instead of broken.
 *
 * IDENTITY, which is the hard problem in an immediate-mode toolkit and is
 * solved here the same way focus already solves it: the slot is keyed by
 * (widget id, key), and the widget id is CALL ORDER -- see the focus note above,
 * which has staked the same bet since the toolkit grew a Tab key. Two extra
 * rules make it safe rather than merely conventional:
 *
 *   - A slot whose id was not queried in the IMMEDIATELY PRECEDING frame is
 *     treated as fresh: it latches to `target` and does not animate. So a
 *     widget appearing for the first time appears finished (a dialog does not
 *     open with thirty hover fades running), and a stale slot can never resume
 *     mid-flight from a value that belonged to something else. Note this is a
 *     FRAME counter, not a clock: an app that sits idle for ten seconds and
 *     then repaints still counts as consecutive, which is exactly right,
 *     because an immediate-mode frame is a pure function of state that the
 *     passage of time did not alter.
 *   - If the caller's layout changes so that a widget takes an id that belonged
 *     to a different widget last frame, that slot re-aims from the previous
 *     occupant's value. The visible consequence is one bounded cross-fade of
 *     the wrong quantity, self-correcting within `ms`; it cannot stick, and it
 *     cannot flicker, because re-aiming is continuous by construction. An app
 *     that reorders deliberately calls aui_anim_reset() -- the same escape
 *     hatch, for the same reason, as aui_set_focus().
 *
 * The table is AUI_ANIM_MAX slots, evicted least-recently-touched. The app
 * declares nothing and immediate mode is untouched. */
#define AUI_ANIM_MAX 32
int  aui_anim(int key, int target, int ms, int curve);
/* A spring on 24.8 values. `id` is an AUI_AK_* style key, scoped to the widget
 * being drawn exactly like aui_anim()'s. Returns where the value IS this frame
 * (24.8); `target24` re-aims it keeping position AND velocity. It keeps
 * requesting frames until ol_spring256 reports settled (|x-target| and |v| both
 * under 1/256), then snaps to the target and asks for nothing. A fresh slot
 * latches to the target; reduced motion snaps on the first sample. Shares the
 * AUI_ANIM_MAX slots with aui_anim (separate key space). */
int aui_spring(int id, int target24);
/* A colour tween in premultiplied space (ol_mix_argb_premul): opaque red to
 * transparent passes through half-transparent RED, not dark red. Straight ARGB
 * in and out. The tween runs `from` -> `to` over AUI_T_FAST, linearly; it
 * restarts from the colour currently on screen when `to` changes, and a fresh
 * slot starts at `from`. Reduced motion returns `to` on the first sample. */
uint32_t aui_anim_color(int id, uint32_t from, uint32_t to);
/* Register an ENDLESS animation for this frame (the spinner, the indeterminate
 * bar). It keeps no value -- it only tells the wake contract below that a frame
 * is wanted every AUI_ANIM_TICK_LOOP ms. Returns aui_ms() so a caller can phase
 * off it in one expression. Before this existed, both loop animations computed
 * a phase from the clock and NOTHING WOKE THE APP, so in any app sleeping on
 * wait_idle(0) they were still pictures. */
unsigned aui_anim_loop(void);
/* Forget every slot. For an app that has just restructured its frame on
 * purpose (switched tabs, opened a different page) and would rather have the
 * new widgets appear settled than inherit the old ones' values. */
void aui_anim_reset(void);

/* ---- the app-side wake contract ----
 * THE APP LOOP CHANGES BY ONE LINE, AND THE LOAD-BEARING PART IS THE ZERO:
 *
 *     for (;;) {
 *         int drew = 0;
 *         while (poll_event(&e)) {
 *             if (e.type == EV_CLOSE) app_exit(0);
 *             aui_feed(&e);
 *             if (aui_want_repaint()) { frame(); drew = 1; }
 *             aui_feed_done();
 *         }
 *         if (!drew && aui_anim_due()) frame();
 *         wait_idle(aui_anim_wait());
 *     }
 *
 * When nothing is animating aui_anim_wait() returns 0, and wait_idle(0) is
 * "sleep until an event" -- byte for byte the behaviour that deleting
 * sys_yield() bought (see the top of this file: 3.3 MILLION syscalls in one
 * 8.8-second boot). The regression cannot come back through an app forgetting a
 * case, because the SAFE VALUE IS THE DEFAULT RETURN. Both calls answer from a
 * static and read no clock when nothing is animating, so an idle desktop makes
 * exactly zero extra syscalls -- which is the number the gate checks.
 *
 * NO BACKLOG, EVER. The next deadline is `now + tick` computed in aui_end(),
 * AFTER the frame has landed -- never `last + tick`. A frame that took 27 ms
 * against a 16 ms tick therefore yields a 43 ms period, not a queue of
 * already-expired deadlines. That one choice is the whole difference between
 * this contract and a spin.
 *
 * HOW AN ANIMATION ENDS, which is the half a threshold cannot do: at
 * elapsed >= ms a slot LATCHES its target and is marked arrived. It returns the
 * target and registers no further deadline, and an arrived slot is invisible to
 * both calls below. So the machine draws exactly one frame after arrival -- the
 * frame that puts the final pixel down -- and the next wait is a real sleep.
 * Termination is a proof, not a guess. */
int  aui_anim_due(void);    /* 1 if a deadline has passed: draw a frame     */
int  aui_anim_wait(void);   /* ms to hand wait_idle(); 0 when nothing moves */
/* Two introspection calls, and BOTH ARE CURRENTLY UNCALLED -- said out loud
 * because this tree keeps a list of things built with no real consumer and a
 * silent addition to it is worse than a stale claim. The gate in
 * tests/repaint.mk deliberately does not use them: it counts composites from
 * the COMPOSITOR's own counters, which is a measurement the toolkit cannot
 * fake. They are here for an app that wants to say "busy" honestly, and for an
 * on-device probe. If they still have no caller when the vocabulary has been
 * applied, delete them.
 *
 * aui_anim_active(): slots still in flight. Zero on an idle desktop.
 * aui_anim_frames(): frames drawn because a deadline came due rather than
 *   because of an event. In the loop above these are the same thing; an app
 *   that ignores aui_anim_due() will see this over-count by the frames it
 *   declined to draw. */
int  aui_anim_active(void);
unsigned aui_anim_frames(void);

#endif /* AUI_MOTION_H */
