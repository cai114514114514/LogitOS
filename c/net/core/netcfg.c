#include <stdint.h>
#include "net.h"
#include "kprintf.h"
#include "cpufeat.h"
#include "../../../include/weaksym.h"   /* every weak hook below; an undefined weak ref is ELF-only */

/* ---------------------------------------------------------------------------
 * HOW THE MACHINE GETS ITS ADDRESS AT BOOT, and what it has until a lease
 * arrives.
 *
 * This used to be the tail of net_init(): DHCP for ~3 s, and on failure the
 * QEMU SLIRP static configuration (10.0.2.15) -- always, on every machine. On
 * the X79-VG4 that put 10.0.2.15 on a real LAN (2026-11-01): the router's link
 * came up after the 3 s were spent, DHCP was never tried again, and the address
 * was one no router on earth would route. It lives in its own file so the
 * policy can be tested on the host without net.c's x86 receive path.
 *
 * The policy now:
 *   1. A static configuration chosen by the user (settings net.dhcp = 0) wins
 *      and is never replaced.
 *   2. Otherwise DHCP. A lease is applied by dhcp.c and that is the end of it.
 *   3. No lease at boot. The DHCP client does not stop (dhcp.c keeps trying in
 *      the background and restarts on every link-up). What the machine is in
 *      the meantime depends on where it runs:
 *        - under a hypervisor the SLIRP configuration is applied, exactly as
 *          before, because every QEMU gate was written against it and QEMU
 *          user networking really is 10.0.2.0/24;
 *        - on real hardware the address stays 0.0.0.0. There is no honest
 *          guess for an unknown LAN, and a wrong address is worse than none:
 *          it makes the machine answer ARP for, and send from, a network it
 *          is not on.
 * ------------------------------------------------------------------------- */

/* dhcp.c is linked into the kernel but kept optional like the other layers. */
int dhcp_run(int timeout_ticks) LOGIT_WEAK;
LOGIT_WEAK_STUB(dhcp_run);

/* The settings store, weak for exactly the reason dhcp_run is: this file is
 * linked into host tests that have no filesystem to read a settings file from.
 * On the machine these resolve; host-side they are NULL and the configuration
 * is the DHCP-then-fallback path this file always had. */
int      settings_get_int(const char *key, int def) LOGIT_WEAK;
unsigned settings_get_ip(const char *key, unsigned def) LOGIT_WEAK;
LOGIT_WEAK_STUB(settings_get_int);
LOGIT_WEAK_STUB(settings_get_ip);

/* The primary interface's link (c/drivers/net/netdev.c); host tests supply
 * their own, and a build without it simply cannot say. */
int netdev_link_up(void) LOGIT_WEAK;
LOGIT_WEAK_STUB(netdev_link_up);

/* CPUID feature bits (c/kernel/cpu/cpufeat.c). */
int cpu_has(enum cpu_feat f) LOGIT_WEAK;
LOGIT_WEAK_STUB(cpu_has);

/* The boot-time exchange: ~3 s at the 100 Hz PIT, after the link wait. */
#define NET_DHCP_BOOT_TICKS 300

/* Whether the kernel runs under a hypervisor: CPUID leaf 1, ECX bit 31, which
 * Intel and AMD reserve for exactly this and which QEMU (TCG and KVM), VMware,
 * Hyper-V and VirtualBox set. Real hardware never does. Without the CPUID layer
 * the answer is "no" -- bare metal is the case a wrong "yes" breaks. */
static int net_under_hypervisor(void)
{
#ifdef NETCFG_NEGCTL_SLIRP_ON_BARE_METAL
    /* NEGATIVE CONTROL: the old behaviour, the SLIRP address whatever the
     * machine is. The bare-metal host gate MUST fail against this build. */
    return 1;
#else
    if (!LOGIT_HAVE(cpu_has))
        return 0;
    return cpu_has(CPU_HYPERVISOR);
#endif
}

/* Static config matching QEMU SLIRP (user networking) defaults, used under a
 * hypervisor when DHCP does not answer (and identical to what SLIRP hands out
 * when it does). */
static void net_cfg_fallback(void)
{
    net_cfg.ip   = IPV4(10, 0, 2, 15);
    net_cfg.mask = IPV4(255, 255, 255, 0);
    net_cfg.gw   = IPV4(10, 0, 2, 2);
    net_cfg.dns  = IPV4(10, 0, 2, 3);
    kprintf("[net] dhcp failed; static ip 10.0.2.15/24 gw 10.0.2.2 dns 10.0.2.3\n");
}

/* A user who has turned automatic configuration off. The addresses come from
 * the store and are already validated -- settings_get_ip() returns the schema
 * default for anything that is not a dotted quad, so `net.ip = 999.1.1.1` in a
 * hand-edited file configures 10.0.2.15 and does not configure garbage.
 *
 * Returns 1 if a static configuration was applied, 0 to fall through to DHCP. */
static int net_cfg_from_settings(void)
{
    if (!LOGIT_HAVE(settings_get_int) || !LOGIT_HAVE(settings_get_ip)) return 0;
    if (settings_get_int("net.dhcp", 1)) return 0;      /* automatic: DHCP below */

    net_cfg.ip   = settings_get_ip("net.ip",   IPV4(10, 0, 2, 15));
    net_cfg.mask = settings_get_ip("net.mask", IPV4(255, 255, 255, 0));
    net_cfg.gw   = settings_get_ip("net.gw",   IPV4(10, 0, 2, 2));
    net_cfg.dns  = settings_get_ip("net.dns",  IPV4(10, 0, 2, 3));
    kprintf("[net] static from settings: ip %u.%u.%u.%u gw %u.%u.%u.%u dns %u.%u.%u.%u\n",
            (net_cfg.ip >> 24) & 255, (net_cfg.ip >> 16) & 255,
            (net_cfg.ip >> 8) & 255, net_cfg.ip & 255,
            (net_cfg.gw >> 24) & 255, (net_cfg.gw >> 16) & 255,
            (net_cfg.gw >> 8) & 255, net_cfg.gw & 255,
            (net_cfg.dns >> 24) & 255, (net_cfg.dns >> 16) & 255,
            (net_cfg.dns >> 8) & 255, net_cfg.dns & 255);
    return 1;
}

/* What the machine is until a lease arrives: the policy in the block above,
 * and the one line that tells a person looking at a serial capture or a photo
 * that the address is missing on purpose and that the client is still going. */
static void net_cfg_without_lease(void)
{
    if (net_under_hypervisor()) {
        net_cfg_fallback();
        return;
    }
    int link_up = LOGIT_HAVE(netdev_link_up) ? netdev_link_up() : 0;
    kprintf("[net] no lease yet; background DHCP keeps trying (link %s)\n",
            link_up ? "up" : "down");
}

/* Called by net_init() once the NIC is bound and the receive path is armed.
 * The stack is unconfigured while negotiating: DHCPDISCOVER leaves from
 * 0.0.0.0, and only broadcast UDP can come back in until we own an address. */
void net_cfg_acquire(void)
{
    if (net_cfg_from_settings())
        return;
    if (LOGIT_HAVE(dhcp_run) && dhcp_run(NET_DHCP_BOOT_TICKS) == 0)
        return;
    net_cfg_without_lease();
}
