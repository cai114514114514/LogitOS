#include "smp_boot_model.h"

#ifdef LOGIT_SMP_BOOT_HOST
void smp_boot_host_publish_pause(volatile int *state);
#define PUBLISH_PAUSE(s) smp_boot_host_publish_pause((s))
#else
#define PUBLISH_PAUSE(s) do { (void)(s); } while (0)
#endif

int smp_ap_publish_online(volatile int *state)
{
    if (!state) return -1;
    int expected = SMP_AP_CLAIMED;
    if (!__atomic_compare_exchange_n(state, &expected, SMP_AP_PUBLISHING, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return -1;
    PUBLISH_PAUSE(state);
    expected = SMP_AP_PUBLISHING;
    return __atomic_compare_exchange_n(state, &expected, SMP_AP_ONLINE, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
               ? 0 : -1;
}

int smp_bsp_commit_online(volatile int *state, volatile int *online_count,
                          int slot)
{
    if (!state || !online_count || slot <= 0) return -1;
    int published = __atomic_load_n(state, __ATOMIC_ACQUIRE);
#if defined(LOGIT_X2APIC_NEGCTL_COMMIT_PUBLISHING) || \
    defined(LOGIT_RAPTOR_SMP_NEGCTL_COMMIT_PUBLISHING)
    if (published != SMP_AP_ONLINE && published != SMP_AP_PUBLISHING) return -1;
#else
    if (published != SMP_AP_ONLINE) return -1;
#endif
    int expected = slot;
    return __atomic_compare_exchange_n(online_count, &expected, slot + 1, 0,
                                       __ATOMIC_RELEASE, __ATOMIC_RELAXED)
               ? 0 : -1;
}

enum smp_ap_boot_state smp_bsp_cancel_unpublished(volatile int *state)
{
    if (!state) return SMP_AP_REJECTED;
    int current = __atomic_load_n(state, __ATOMIC_ACQUIRE);
    for (;;) {
        if (current != SMP_AP_CLAIMED && current != SMP_AP_PUBLISHING)
            return (enum smp_ap_boot_state)current;
        if (__atomic_compare_exchange_n(state, &current, SMP_AP_REJECTED, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return SMP_AP_REJECTED;
    }
}

int smp_ap_stack_may_free(enum smp_ap_boot_state state, int startup_sent)
{
#ifdef LOGIT_X2APIC_NEGCTL_FREE_TIMEOUT
    (void)state;
    (void)startup_sent;
    return 1;
#else
    if (startup_sent) return 0;
    return state == SMP_AP_EMPTY || state == SMP_AP_REJECTED;
#endif
}
