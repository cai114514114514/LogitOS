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

#include "openlogit_motion.h"

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
    /* aui_spring / aui_anim_color live in the SAME slots (key spaces below), so
     * the 32-slot limit and LRU eviction cover them. */
    struct ol_spring256 sp;     /* aui_spring: position/velocity in 24.8          */
    unsigned last_ms;           /* aui_spring: frame_ms of the last step          */
    uint32_t c_from, c_to, c_cur;   /* aui_anim_color: leg endpoints, last drawn  */
};
#define AUI_KEY_SPRING 0x1000   /* slot key = caller key | this */
#define AUI_KEY_COLOR  0x2000

/* AUI_EASE_* below 13 ARE ol_curve values; the presets are beziers. */
_Static_assert(AUI_EASE_LINEAR == OL_LINEAR && AUI_EASE_OUT == OL_EASE_OUT &&
               AUI_EASE_INOUT == OL_EASE_INOUT && AUI_EASE_IN == OL_EASE_IN &&
               AUI_EASE_OUT_CUBIC == OL_EASE_OUT_CUBIC && AUI_EASE_IN_CUBIC == OL_EASE_IN_CUBIC &&
               AUI_EASE_INOUT_CUBIC == OL_EASE_INOUT_CUBIC && AUI_EASE_OUT_QUART == OL_EASE_OUT_QUART &&
               AUI_EASE_OUT_EXPO == OL_EASE_OUT_EXPO && AUI_EASE_OUT_BACK == OL_EASE_OUT_BACK &&
               AUI_EASE_INOUT_BACK == OL_EASE_INOUT_BACK, "AUI_EASE_* must mirror ol_curve");

#ifndef AUI_ANIM_OFF
/* Curve id -> ol_ease256_ex arguments. Anything unknown (and bezier/steps ids
 * 4 and 5, which carry parameters this enum cannot) falls back to ease-out. */
static int aui_ease_eval(int curve, int t256)
{
#ifdef AUI_MOTION_LEGACY3      /* negative control: the old three-curve map */
    int c = curve == AUI_EASE_INOUT ? OL_EASE_INOUT : curve == AUI_EASE_LINEAR ? OL_LINEAR : OL_EASE_OUT;
    return ol_ease256_ex((enum ol_curve)c, t256, 0, 0);
#else
    static const struct ol_bezier bz[4] = {
        { OL_FX(0.2),  OL_FX(0.0),  OL_FX(0.0), OL_FX(1.0)  },   /* STANDARD   */
        { OL_FX(0.05), OL_FX(0.7),  OL_FX(0.1), OL_FX(1.0)  },   /* EMPHASIZED */
        { OL_FX(0.0),  OL_FX(0.0),  OL_FX(0.2), OL_FX(1.0)  },   /* DECEL      */
        { OL_FX(0.34), OL_FX(1.56), OL_FX(0.64), OL_FX(1.0) },   /* BACK       */
    };
    if (curve >= AUI_EASE_STANDARD && curve <= AUI_EASE_BACK)
        return ol_ease256_ex(OL_CUBIC_BEZIER, t256, &bz[curve - AUI_EASE_STANDARD], 0);
    if (curve == OL_CUBIC_BEZIER || curve == OL_STEPS || curve < 0 || curve > AUI_EASE_INOUT_BACK)
        curve = AUI_EASE_OUT;
    return ol_ease256_ex((enum ol_curve)curve, t256, 0, 0);
#endif
}
#endif

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
            /* THE 255/256 SEAM IS CLOSED HERE AND NOWHERE ELSE. The curves are
             * exact on 0..256 because 256 is a power of two; aui_mix is on
             * 0..255. Scaling the DELTA by e/256 lands on `to` exactly when e
             * hits 256. The back family overshoots 256, so the result is
             * clamped into 0..255: no widget ever sees a 256 or a negative. */
            int t = (int)((double)el * 256 / (double)s->dur);
            int e = aui_ease_eval(curve, t);
            s->cur = iclamp(s->from + (int)((long long)(s->to - s->from) * e / 256), 0, 255);
        }
    }
    if (!s->arrived) anim_live++;
    return s->cur;
#endif /* AUI_ANIM_OFF */
}

#ifndef AUI_ANIM_OFF
/* ---- aui_spring ----------------------------------------------------------
 * Same slot table, key space `key | AUI_KEY_SPRING`. dt is the frame clock's
 * delta (capped, so a window that slept for a minute does not integrate a
 * minute). Critical-ish damping (zeta 0.5): a visible overshoot that settles
 * in a few hundred ms. The slot is `arrived` once ol_spring256_step says
 * settled, which is also what stops it requesting frames. */
#define AUI_SPRING_STIFF 900u
#define AUI_SPRING_DAMP  7680u     /* Q8: 30/s, zeta 0.5 at omega 30/s */
#define AUI_SPRING_DT_MAX_MS 250u

int aui_spring(int id, int target24)
{
    int wid = id_ctr, fresh = 0;
    struct anim_slot *s;
    if (wid <= 0) return target24;
    s = anim_slot_for(wid, id | AUI_KEY_SPRING, &fresh);
    if (fresh || anim_reduced) {
        ol_spring256_init(&s->sp, target24, AUI_SPRING_STIFF, AUI_SPRING_DAMP);
        s->arrived = 1; s->last_ms = frame_ms; s->gen = anim_gen;
        s->cur = target24;
        return target24;
    }
    s->gen = anim_gen;
    if (target24 != s->sp.target) {
        ol_spring256_retarget(&s->sp, target24);
        s->arrived = 0;
    }
    if (!s->arrived) {
        unsigned dt = frame_ms - s->last_ms;
        if (dt > AUI_SPRING_DT_MAX_MS) dt = AUI_SPRING_DT_MAX_MS;
        if (!ol_spring256_step(&s->sp, dt * 1000u)) s->arrived = 1;
    }
    s->last_ms = frame_ms;
    s->cur = s->sp.x;
    if (!s->arrived) anim_live++;
    return s->sp.x;
}

/* ---- aui_anim_color -------------------------------------------------------
 * Key space `key | AUI_KEY_COLOR`. Progress is linear over AUI_T_FAST, the
 * mix is ol_mix_argb_premul. s->from/to/cur are unused here; c_* carry it. */
uint32_t aui_anim_color(int id, uint32_t from, uint32_t to)
{
    int wid = id_ctr, fresh = 0;
    struct anim_slot *s;
    if (wid <= 0) return to;
    s = anim_slot_for(wid, id | AUI_KEY_COLOR, &fresh);
    if (anim_reduced) {
        s->c_from = s->c_to = s->c_cur = to; s->arrived = 1; s->gen = anim_gen;
        return to;
    }
    if (fresh) {
        s->c_from = from; s->c_to = to; s->c_cur = from;
        s->t0 = frame_ms; s->arrived = (from == to);
    } else if (to != s->c_to) {
        s->c_from = s->c_cur; s->c_to = to;      /* re-aim from what is on screen */
        s->t0 = frame_ms; s->arrived = 0;
    }
    s->gen = anim_gen;
    if (!s->arrived) {
        unsigned el = frame_ms - s->t0;
        if ((int)el >= AUI_T_FAST) { s->c_cur = s->c_to; s->arrived = 1; }
        else s->c_cur = ol_mix_argb_premul(s->c_from, s->c_to, (int)(el * 256u / AUI_T_FAST));
    }
    if (!s->arrived) anim_live++;
    return s->c_cur;
}
#else  /* instant, like aui_anim() in this build */
int aui_spring(int id, int target24) { (void)id; return target24; }
uint32_t aui_anim_color(int id, uint32_t from, uint32_t to) { (void)id; (void)from; return to; }
#endif

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
