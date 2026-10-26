/* aui_motion.inc -- the animation core: anim_tab, slot lookup, curve mapping, wake contract.
 * Unity-included by aui.c (one translation unit): every static it names is
 * file-scope in aui.c, and the variant builds (-DAUI_NO_AA, -DAUI_COST, ...)
 * reach it automatically. Moved verbatim; do not compile on its own. */

/* ---------------------------------------------------- 5c. motion ----------
 * The animation core. aui.h carries the design; this carries the three things
 * that are only decidable in code.
 *
 * ONE. IDENTITY IN AN IMMEDIATE-MODE TOOLKIT. There are no widget objects, so
 * "the button that was 40% hovered last frame" has to be recovered from
 * something. It is recovered from the SAME THING FOCUS ALREADY USES: the widget
 * id, which is call order (`++id_ctr` at the top of every widget). During a
 * widget's body, `id_ctr` IS that widget's id, so aui_anim() needs no argument
 * for it and no registration step. This is not a new bet -- aui.h has said
 * "focus order IS call order" since Tab worked -- but it is a bet, and the two
 * rules below are what make it safe rather than merely conventional.
 *
 * TWO. THE CONTINUITY RULE, which is what stops a stuck or flickering state.
 * A slot carries the FRAME NUMBER it was last queried at. If that is not the
 * immediately preceding frame, the slot is treated as fresh: it latches to its
 * target and does not animate. Two things fall out, both wanted. A widget
 * appearing for the first time appears FINISHED -- open a dialog with thirty
 * controls and you get one frame, not thirty hover fades. And an evicted or
 * abandoned slot can never resume mid-flight holding a value that belonged to
 * something else. It is a frame counter and deliberately not a clock: an app
 * that sleeps ten seconds and then repaints is still "consecutive", which is
 * right, because an immediate-mode frame is a pure function of state and the
 * passage of time did not alter it.
 *
 * When the caller's layout DOES change so that a widget inherits an id, that
 * slot is continuous and re-aims from the previous occupant's value. The result
 * is one bounded cross-fade of the wrong quantity, over at most `ms`. It cannot
 * stick (arrival is `elapsed >= dur`, not a threshold on the value) and it
 * cannot flicker (re-aim is from the CURRENT value, so the sequence of drawn
 * values is continuous by construction). aui_anim_reset() is the escape hatch,
 * and it is the same escape hatch, for the same reason, as aui_set_focus().
 *
 * THREE. THE WAKE CONTRACT, and the property to protect is that an idle
 * desktop costs exactly what it cost before this file grew a clock. Both
 * public queries return from a static and READ NO CLOCK when nothing is
 * animating -- monotonic_ms() is a syscall on this machine, and a toolkit that
 * called it once per loop turn would be reintroducing, in a smaller way,
 * exactly the 3.3-million-syscall mistake the top of aui.h is about. */

struct anim_slot {
    int      id;        /* owning widget id; 0 = never used                 */
    int      key;       /* AUI_AK_*: several animations on one widget       */
    int      from, to;  /* endpoints, 0..255 (aui_mix's domain)             */
    int      cur;       /* what the last frame actually drew                */
    int      dur;       /* ms                                               */
    int      curve;     /* AUI_EASE_*                                       */
    unsigned t0;        /* frame_ms when this leg started                   */
    unsigned gen;       /* frame number this slot was last queried at       */
    int      arrived;   /* latched: returns `to` and schedules nothing      */
};

static struct anim_slot anim_tab[AUI_ANIM_MAX];
static int anim_reduced;
static unsigned anim_gen = 1;      /* frames since process start; aui_begin++ */
static unsigned anim_frames_n;     /* frames drawn because a deadline fired   */
static unsigned anim_due_ms;       /* absolute deadline, ms                   */
static int      anim_have_due;     /* ...and whether there is one at all      */
static int      anim_loop_want;    /* an ENDLESS animation drew this frame    */
static int      anim_live;         /* slots still in flight after this frame  */
static int      anim_armed;        /* aui_anim_due() said yes, frame pending  */

/* Offset pointers are the existing scroll-container identity. Using widget
 * call order here would alias slots when a list culls a different set of rows.
 * Keep only borrowed identities: an absent container's pointer is never read.
 */
#define SCROLL_SLOTS 16
static struct scroll_slot {
    int *owner;
    struct aui_scroll_motion motion;
    struct aui_rect viewport;
    unsigned gen, order;
} scroll_slots[SCROLL_SLOTS];
static unsigned scroll_order;
static int *scroll_wheel_owner;
static int scroll_wheel_used;

#ifndef AUI_ANIM_OFF          /* unreferenced in the negative-control build */
static struct anim_slot *anim_slot_for(int id, int key, int *fresh)
{
    struct anim_slot *freeslot = 0, *lru = &anim_tab[0];
    /* THE WHOLE TABLE IS SCANNED BEFORE ANYTHING IS ALLOCATED, and the first
     * version of this function did not do that: it took the first free slot and
     * broke out of the loop. With a hole at index 2 and this widget's live slot
     * at index 5, that allocated a NEW slot at 2 and orphaned 5 -- so the widget
     * restarted from its target every frame and never moved, while a second slot
     * quietly ate a table entry. It would have looked exactly like "the
     * animation does not work", which is the least informative symptom there is. */
    for (int i = 0; i < AUI_ANIM_MAX; i++) {
        struct anim_slot *s = &anim_tab[i];
        if (s->id == id && s->key == key) {
            /* Found. Continuous only if it was queried in the frame directly
             * before this one -- see THE CONTINUITY RULE above. */
            *fresh = (s->gen + 1 != anim_gen);
            return s;
        }
        if (!s->id) { if (!freeslot) freeslot = s; continue; }
        /* Least-recently-touched. Unsigned compare is fine: gen is monotone
         * and the table is scanned within one frame, so no two live entries
         * can straddle a wrap. */
        if (s->gen < lru->gen) lru = s;
    }
    struct anim_slot *n = freeslot ? freeslot : lru;
    n->id = id; n->key = key; n->gen = 0; n->t0 = frame_ms;
    *fresh = 1;
    return n;
}
#endif

int aui_anim(int key, int target, int ms, int curve)
{
#ifdef AUI_ANIM_OFF
    /* THE NEGATIVE CONTROL, and it has to live here rather than in the harness.
     * Every widget still calls aui_anim() and still draws whatever it returns,
     * so the picture is identical at rest and every code path above and below
     * is unchanged -- the value simply arrives instantly. Nothing is live, so
     * anim_schedule() registers no deadline and the interaction produces ONE
     * composite instead of eight.
     *
     * WHAT IT IS PROTECTING AGAINST is the reading this gate would otherwise
     * give for free: a compositor that happens to repaint several times after
     * any click would satisfy the positive assertion whether or not a single
     * animation ran. If the OFF build also reads eight composites, the harness
     * is measuring the machine's idle repaints and the `anim` row is not a
     * measurement. That is a thing this tree has shipped before -- see aui.h's
     * note on the toggle whose animation the header documented for as long as
     * it did not exist. */
    (void)key; (void)ms; (void)curve;
    return iclamp(target, 0, 255);
#else
    int id = id_ctr;                 /* the widget currently being drawn */
    int fresh = 0;
    struct anim_slot *s;

    target = iclamp(target, 0, 255);
    if (ms < 1) ms = 1;
    /* id 0 means aui_anim() was called outside any widget, where there is no
     * stable identity to key on. Answer instantly rather than colliding with a
     * free slot, which is what id 0 and key 0 would otherwise match. */
    if (id <= 0) return target;
    s = anim_slot_for(id, key, &fresh);

    if (fresh || anim_reduced) {
        s->from = s->to = s->cur = target;
        s->arrived = 1;
    } else if (target != s->to) {
        /* RE-AIM FROM WHERE THE PIXELS ARE, not from where this leg started.
         * A pointer that leaves mid-fade reverses out of the value on screen
         * and never snaps -- which is the whole difference between an
         * interruptible animation and a broken one. */
        s->from = s->cur;
        s->to = target;
        s->arrived = 0;
        s->t0 = frame_ms;
    }
    s->dur = ms; s->curve = curve; s->gen = anim_gen;

    if (!s->arrived) {
        unsigned el = frame_ms - s->t0;
        if ((int)el >= s->dur) {
            /* TERMINATION IS A PROOF, NOT A THRESHOLD. The slot latches its
             * target here and stops registering deadlines, so the machine
             * draws exactly one more frame -- the one that puts the final
             * pixel down -- and then sleeps for real. */
            s->cur = s->to;
            s->arrived = 1;
        } else {
            int sdk_curve = curve == AUI_EASE_INOUT ? OL_EASE_INOUT
                          : curve == AUI_EASE_LINEAR ? OL_LINEAR : OL_EASE_OUT;
            /* THE 255/256 SEAM IS CLOSED HERE AND NOWHERE ELSE. The curves are
             * exact on 0..256 because 256 is a power of two; aui_mix is on
             * 0..255. Scaling the DELTA by e/256 lands on `to` exactly when e
             * hits 256 and never overshoots, so no widget ever sees a 256. */
            /* 2026-09-13: widget identity and scheduling remain here; the
             * actual interpolation now belongs to the shared SDK. */
            s->cur = ol_transition256(s->from,s->to,el,(uint64_t)s->dur,sdk_curve,0);
        }
    }
    if (!s->arrived) anim_live++;
    return s->cur;
#endif /* AUI_ANIM_OFF */
}

unsigned aui_anim_loop(void) { if(anim_reduced)return 0; anim_loop_want = 1; return frame_ms; }

void aui_anim_reset(void)
{
    for (int i = 0; i < AUI_ANIM_MAX; i++) anim_tab[i].id = 0;
    for (int i = 0; i < SCROLL_SLOTS; i++) scroll_slots[i].owner = 0;
    anim_have_due = 0; anim_live = 0; anim_armed = 0;
}

int      aui_anim_active(void) { return anim_live; }
unsigned aui_anim_frames(void) { return anim_frames_n; }

/* Called by aui_end() AFTER gui_flush(), and the ordering is the whole
 * anti-spin argument: the deadline is `now + tick`, where `now` is measured
 * once the frame has actually landed -- never `last + tick`. A frame that took
 * 27 ms against a 16 ms tick therefore yields a 43 ms period rather than a
 * queue of already-expired deadlines to catch up on. */
static void anim_schedule(void)
{
    int tick;
    if (!anim_live && !anim_loop_want) {
        /* Nothing moves. Return before touching the clock: this is the path an
         * idle desktop takes and it must cost zero syscalls. */
        anim_have_due = 0;
        return;
    }
    if (anim_live) {
        /* CADENCE IS DERIVED FROM THE WINDOW, NOT A CONSTANT. SYS_GUI_FLUSH
         * carries no rectangle, so the frame this deadline buys will cost the
         * whole canvas; asking for frames faster than the compositor can make
         * them just queues work behind the BKL. A wrong estimate makes an
         * animation coarse and can never make it spin, because the floor is
         * AUI_ANIM_TICK_MIN and the deadline is computed after the fact. */
        int sc = aui_scale();
        long long px = (long long)win_w * win_h * sc * sc / 10000;
        long long est = px * AUI_NS_PER_PX / 1000000;
        tick = est > 1000 ? 1000 : (int)est;
        if (tick < AUI_ANIM_TICK_MIN) tick = AUI_ANIM_TICK_MIN;
        /* Both kinds live at once: the SOONER deadline wins. */
        if (anim_loop_want && AUI_ANIM_TICK_LOOP < tick) tick = AUI_ANIM_TICK_LOOP;
    } else {
        tick = AUI_ANIM_TICK_LOOP;
    }
    anim_due_ms = (unsigned)monotonic_ms() + (unsigned)tick;
    anim_have_due = 1;
}

int aui_anim_due(void)
{
    if (!anim_have_due) { anim_armed = 0; return 0; }
    /* A minimized window keeps its state, but cannot show intermediate frames.
     * Leave the deadline pending; restore's focus event makes the endpoint
     * eligible again without a periodic hidden-window wake. */
    if (_sys(SYS_GUI_WIN_STATE, WINS_MINIMIZED, 0, 0) > 0) return 0;
    unsigned now = (unsigned)monotonic_ms();
    if ((int)(now - anim_due_ms) < 0) return 0;     /* signed delta: wrap-safe */
    anim_armed = 1;
    return 1;
}

int aui_anim_wait(void)
{
    /* THE ZERO IS THE LOAD-BEARING VALUE. wait_idle(0) is "sleep until an
     * event", which is what every app did before this file grew a clock, so the
     * safe answer is also the default answer and no app can reintroduce a spin
     * by forgetting a case. */
    if (!anim_have_due) return 0;
    if (_sys(SYS_GUI_WIN_STATE, WINS_MINIMIZED, 0, 0) > 0) return 0;
    unsigned now = (unsigned)monotonic_ms();
    int d = (int)(anim_due_ms - now);
    /* ...and never 0 on this path, because 0 means FOREVER. A deadline already
     * passed must round up to 1 ms, not down to a sleep that never returns. */
    return d > 0 ? d : 1;
}
