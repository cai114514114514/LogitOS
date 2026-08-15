#include "clib.h"

/* /bin/reboot -- ask the machine to reset itself and boot again.
 *
 * Same shape as poweroff.c and for the same reason: a working reboot does
 * not return to this instruction stream either -- the reset line that
 * SYS_REBOOT pulls (see the kernel side, c/kernel/cpu/acpi.c) restarts the
 * CPU at the BIOS/firmware entry point, not here. So, as with poweroff,
 * everything past the syscall is the failure case, and ID_E_PERM is the same
 * root-only refusal SYS_SETUID and SYS_POWEROFF both use -- copied, not
 * invented. */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    long rc = _sys(SYS_REBOOT, 0, 0, 0);

    /* Still executing: the machine did not reset. */
    if (rc == ID_E_PERM) {
        errs("reboot: refused -- only root may reboot this machine\n");
    } else {
        errs("reboot: the kernel came back instead of resetting (rc=");
        outn_fd(2, rc);
        errs(") -- reboot control is not available on this machine\n");
    }
    return 1;
}
