/* <sys/utsname.h>. See the header for what each field means and why the
 * release/version strings are what they are. */
#include <sys/utsname.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

int uname(struct utsname *buf)
{
    if (!buf) { errno = EFAULT; return -1; }
    memset(buf, 0, sizeof *buf);
    strlcpy(buf->sysname, "LogitOS", sizeof buf->sysname);
    /* gethostname() never fails on this system (it always has an answer);
     * if that ever stops being true, an empty nodename is still honest. */
    if (gethostname(buf->nodename, sizeof buf->nodename) != 0)
        buf->nodename[0] = 0;
    strlcpy(buf->release, "0.29", sizeof buf->release);          /* M29 (audio) */
    strlcpy(buf->version, "LogitOS M29 x86_64", sizeof buf->version);
    strlcpy(buf->machine, "x86_64", sizeof buf->machine);
    return 0;
}
