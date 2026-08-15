#include "clib.h"

/* /bin/poweroff -- ask the machine to actually turn itself off.
 *
 * Before this, the only way to stop LogitOS was to be yanked from outside
 * (kill the QEMU process, pull the plug on real hardware). Durability
 * survives that because the filesystem journals -- but a machine that can
 * only be yanked is a machine whose every shutdown is a crash with good
 * manners. This is the other kind: SYS_POWEROFF syncs the filesystem, tells
 * ACPI to remove power (PM1a_CNT via the FACP the kernel already parses,
 * see c/kernel/cpu/acpi.c), and does not come back.
 *
 * THE CONTROL FLOW HERE IS BACKWARDS FROM EVERY OTHER COREUTIL ON PURPOSE.
 * A working power-off does not return -- the CPU this program is running on
 * loses power along with everything else, so there is no instruction stream
 * left to print a success message from. That means main() has exactly one
 * job: make the call, and then handle the one case that is by construction
 * always the failure case -- the syscall came back at all.
 *
 * ID_E_PERM is not invented here. It is the exact value SYS_SETUID already
 * returns for "you are not root" (include/abi/logit_abi.h) -- copied rather
 * than given a new name, per this project's convention that a root-only
 * syscall refuses with ID_E_PERM. Anything else that comes back means the
 * call reached the kernel and the kernel chose not to go down (no FACP, no
 * PM1a, or the write did not take) -- worth saying plainly on stderr rather
 * than silently returning as if nothing had been asked. */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    long rc = _sys(SYS_POWEROFF, 0, 0, 0);

    /* Still executing: the machine is still up. */
    if (rc == ID_E_PERM) {
        errs("poweroff: refused -- only root may power off this machine\n");
    } else {
        errs("poweroff: the kernel came back instead of going down (rc=");
        outn_fd(2, rc);
        errs(") -- power control is not available on this machine\n");
    }
    return 1;
}
