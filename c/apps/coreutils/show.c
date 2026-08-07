#include "clib.h"
#include "logit_rich.h"

/* show <file> [width]
 *
 * The compatibility claim in one program. Under the rich terminal an image
 * becomes real pixels in the scrollback; anywhere else -- redirected to a file,
 * piped into another program, or run from the serial console -- it prints a
 * plain line and nothing else, because the shell did not hand this process the
 * rich channel and rt_isrich() is therefore 0.
 *
 * A non-image is simply catted, so `show` is useful even where nothing is rich.
 */

static struct rt_enc E;

static void abspath(const char *in, char *out, int max)
{
    if (in[0] == '/') { c_strcpy(out, in, max); return; }
    char cwd[128];
    sys_getcwd(cwd, sizeof cwd);
    path_join(out, cwd, in, max);
}

static int ends_with(const char *s, const char *suf)
{
    int ls = c_strlen(s), lf = c_strlen(suf);
    if (lf > ls) return 0;
    for (int i = 0; i < lf; i++) {
        char a = s[ls - lf + i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return 0;
    }
    return 1;
}

static int is_image(const char *p)
{
    return ends_with(p, ".png") || ends_with(p, ".jpg") || ends_with(p, ".jpeg") ||
           ends_with(p, ".gif") || ends_with(p, ".bmp");
}

static long fsize(const char *p)
{
    int fd = sys_open(p, O_RDONLY);
    if (fd < 0) return -1;
    long n = sys_lseek(fd, 0, SEEK_END);
    sys_close(fd);
    return n;
}

static void put_num(long v)
{ char t[24]; int i = 0; if (!v) { rt_out("0"); return; }
  while (v > 0) { t[i++] = (char)('0' + v % 10); v /= 10; }
  char o[25]; int k = 0; while (i) o[k++] = t[--i]; o[k] = 0; rt_out(o); }

int main(int argc, char **argv)
{
    rt_init(argc, argv);
    if (argc < 2) { errs("usage: show <file> [width]\n"); return 2; }

    char abs[192];
    abspath(argv[1], abs, sizeof abs);

    if (is_image(argv[1])) {
        long n = fsize(abs);
        if (n < 0) { errs("show: cannot open "); errs(argv[1]); errs("\n"); return 1; }
        if (rt_isrich()) {
            rt_reset(&E);
            rt_u8(&E, RT_IMG_PATH);
            rt_u16(&E, argc > 2 ? c_atoi(argv[2]) : 0);   /* width in points, 0 = natural */
            rt_u16(&E, 0);
            rt_str(&E, abs);
            if (rt_send(RT_T_IMAGE, &E) == 0) return 0;
            /* the channel died mid-run: fall through to the plain form */
        }
        rt_out(abs); rt_out(": image, "); put_num(n); rt_out(" bytes\n");
        return 0;
    }

    int fd = sys_open(abs, O_RDONLY);
    if (fd < 0) { errs("show: cannot open "); errs(argv[1]); errs("\n"); return 1; }
    char b[512];
    int n;
    while ((n = sys_read(fd, b, sizeof b)) > 0) rt_outn(b, n);
    sys_close(fd);
    return 0;
}
