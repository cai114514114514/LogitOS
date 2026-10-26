#ifndef LOGIT_QMP_PROBE_H
#define LOGIT_QMP_PROBE_H
/* Whether to paint the 6x6 QMP test probes (Settings, Activity Monitor, the
 * Preview picker): a block of an odd colour at window-local (4,4) that tells a
 * QMP driver the window's content origin and which tab is showing.
 *
 * They were painted unconditionally, so every person who opened Settings or
 * the Monitor saw a magenta square in the corner -- the first thing a desktop
 * tour (2026-10-26) listed under "looks like a toy". They are instruments, so
 * they now need the instrument's disk: /etc/qmp-probes exists on $(DISK), the
 * image the QMP gates boot, and not on $(RELEASE_DISK) (see the Makefile).
 * Asked once per process; an app that started before the file appeared keeps
 * its first answer, which no gate depends on.
 *
 * Gallery's probe is not behind this: Gallery itself is packed only onto the
 * gate disks that test it. Include after logit.h. */
static inline int qmp_probes_on(void)
{
    static int on = -1;
    if (on < 0) {
        int fd = sys_open("/etc/qmp-probes", O_RDONLY);
        on = fd >= 0;
        if (fd >= 0) sys_close(fd);
    }
    return on;
}
#endif
