/* The SYS_SND_* implementations.
 *
 * They live here rather than as a dozen cases in c/kernel/exec/syscall.c so the
 * audio logic stays in the audio directory and the shared dispatcher takes one
 * forwarding line. syscall.c keeps what only it can do -- deciding that this is
 * a ring-3 caller at all -- and this file does the user-pointer validation and
 * the copy, because those are per-syscall facts (which argument is a buffer,
 * how long it is) that a generic dispatcher cannot know.
 *
 * THE COPY IS NOT AVOIDABLE HERE. snd_stream_write takes a kernel pointer, so
 * user PCM is bounced through a kernel buffer before it reaches the ring. With
 * shared memory that copy would go away and the ring would be mapped into the
 * process; there is no mmap yet, so it does not.
 */
#include "snd.h"
#include "usercopy.h"
#include "proc.h"
#include "kheap.h"
#include "kprintf.h"

/* One bounce buffer per call, sized so a caller writing a whole period at a
 * time never round-trips more than once. Bigger writes are simply served in
 * several passes -- SYS_SND_WRITE is a short-write interface, so returning less
 * than asked is already part of its contract and needs no special case. */
#define SND_BOUNCE 8192

static int g_reported;

/* The boot report. Called from snd_init() when a card was found (so it lands in
 * the boot log next to the device dump), and from the first audio syscall
 * otherwise.
 *
 * GAP, stated rather than hidden: on a machine with NO sound card this line is
 * emitted on first use instead of at boot, because the natural place to call it
 * unconditionally is kmain.c, which belongs to another line. One call to
 * snd_init() there would make it a boot line on both paths and nothing else
 * would change -- snd_init() is already idempotent-safe on the no-card path
 * (it allocates nothing and starts no thread). */
void snd_report_once(void)
{
    if (g_reported) return;
    g_reported = 1;
    snd_report();
}

static void *cur_owner(void)
{
    return (void *)proc_current();
}

long snd_syscall(long num, long a, long b, long c)
{
    void *owner = cur_owner();

    snd_report_once();

    switch (num) {
    case SYS_SND_INFO: {
        struct logit_sndinfo si;
        if (!user_range_ok((void *)a, sizeof si, 1)) return SND_E_FAULT;
        snd_info_fill(&si);
        user_copy_to((void *)a, &si, sizeof si);
        return snd_present() ? 1 : 0;
    }

    case SYS_SND_OPEN: {
        struct logit_sndfmt f;
        if (!user_range_ok((const void *)a, sizeof f, 0)) return SND_E_FAULT;
        user_copy_from(&f, (const void *)a, sizeof f);
        return snd_stream_open(owner, &f);
    }

    case SYS_SND_WRITE: {
        /* Static, not on the stack: 8 KiB does not belong on a 32 KiB kernel
         * stack (this kernel has already overflowed one into its own page
         * tables -- see the M11 note in CLAUDE.md). Sharing one buffer between
         * callers is safe only because syscalls run under the BKL; when audio
         * joins syscall_is_bkl_free() this must become per-stream. */
        static uint8_t bounce[SND_BOUNCE];
        int want = (int)c;
        if (want <= 0) return 0;
        if (want > SND_BOUNCE) want = SND_BOUNCE;
        if (!user_range_ok((const void *)b, (uint64_t)want, 0)) return SND_E_FAULT;
        user_copy_from(bounce, (const void *)b, (uint64_t)want);
        return snd_stream_write(owner, (int)a, bounce, want);
    }

    case SYS_SND_AVAIL:
        return snd_stream_avail(owner, (int)a);

    case SYS_SND_CLOSE:
        return snd_stream_close(owner, (int)a, (int)b);

    case SYS_SND_STATE: {
        struct logit_sndstate st;
        int rc;
        if (!user_range_ok((void *)b, sizeof st, 1)) return SND_E_FAULT;
        rc = snd_stream_state(owner, (int)a, &st);
        if (rc == 0) user_copy_to((void *)b, &st, sizeof st);
        return rc;
    }

    default:
        return SND_E_BADH;
    }
}
