#include "clib.h"
#include "logit_rich.h"
#include "logit_sniff.h"

/* show <file> [width]
 *
 * "Open this file and do the right thing with it", where the right thing is
 * decided by THE BYTES (c/apps/coreutils/logit_sniff.h) and never by the name.
 * The Finder's fallback for a file type nothing claims launches the Terminal
 * with `show <path>`, so this program is what happens when you double-click an
 * unknown file -- which is how a 2.2 MB font came to be printed onto a
 * character grid one glyph at a time.
 *
 * Four outcomes, one rule:
 *
 *   image  -> an LRT image frame; the terminal decodes and draws it
 *   video  -> an LRT video frame; the terminal decodes and PLAYS it
 *   text   -> copied to stdout, but still watched byte by byte (a "text" file
 *             can turn binary halfway through, and the prefix cannot know)
 *   other  -> REFUSED, with the format named and its first bytes hexdumped
 *
 * WHY REFUSE RATHER THAN HEXDUMP THE WHOLE THING, OR RENDER A SPECIMEN
 * -------------------------------------------------------------------
 * A full hexdump of a 2.2 MB font is 140 000 lines: it does not garble the
 * screen but it destroys the scrollback just as thoroughly, and nobody reads
 * it. Rendering a specimen of a font would be the delightful answer, and it is
 * not available honestly from here: the rasterizer in c/lib/text belongs to the
 * kernel, there is no syscall that loads a font from an arbitrary path, and
 * inventing one is a kernel change this line does not own. So the answer is the
 * one `file(1)` gives, plus the eight bytes that prove it: what it is, how big
 * it is, what would open it, and nothing on the grid that is not text.
 *
 * The compatibility claim is unchanged and still structural: redirected or
 * mid-pipeline, the shell withholds the rich channel, rt_isrich() is 0, and
 * every path above degrades to plain text on fd 1.
 */

static struct rt_enc E;

static void abspath(const char *in, char *out, int max)
{
    if (in[0] == '/') { c_strcpy(out, in, max); return; }
    char cwd[128];
    sys_getcwd(cwd, sizeof cwd);
    path_join(out, cwd, in, max);
}

static void put_num(long v)
{
    char t[24]; int i = 0;
    if (v <= 0) { rt_out("0"); return; }
    while (v > 0) { t[i++] = (char)('0' + v % 10); v /= 10; }
    char o[25]; int k = 0; while (i) o[k++] = t[--i]; o[k] = 0;
    rt_out(o);
}

int main(int argc, char **argv)
{
    rt_init(argc, argv);
    if (argc < 2) { errs("usage: show <file> [width]\n"); return 2; }

    char abs[192];
    abspath(argv[1], abs, sizeof abs);

    int fd = sys_open(abs, O_RDONLY);
    if (fd < 0) { errs("show: cannot open "); errs(argv[1]); errs("\n"); return 1; }
    long size = sys_lseek(fd, 0, SEEK_END);
    sys_lseek(fd, 0, SEEK_SET);

    /* Look first. One read, before a single byte is forwarded anywhere. */
    unsigned char head[SNIFF_PREFIX];
    int nh = sys_read(fd, head, (int)sizeof head);
    if (nh < 0) nh = 0;
    int kind = sniff_id(head, nh);
    int cls = sniff_class(kind);
    int want = argc > 2 ? c_atoi(argv[2]) : 0;

    if (cls == SNC_IMAGE) {
        sys_close(fd);
        if (rt_isrich()) {
            rt_reset(&E);
            rt_u8(&E, RT_IMG_PATH);
            rt_u16(&E, want);                     /* width in points, 0 = natural */
            rt_u16(&E, 0);
            rt_str(&E, abs);
            if (rt_send(RT_T_IMAGE, &E) == 0) return 0;
        }
        rt_out(abs); rt_out(": "); rt_out(sniff_name(kind)); rt_out(", ");
        put_num(size); rt_out(" bytes\n");
        return 0;
    }

    if (cls == SNC_VIDEO) {
        sys_close(fd);
        if (rt_isrich()) {
            rt_reset(&E);
            rt_u8(&E, RT_VID_PATH);
            rt_u16(&E, want);
            rt_u16(&E, 0);
            rt_u16(&E, RT_VID_LOOP);
            rt_str(&E, abs);
            if (rt_send(RT_T_VIDEO, &E) == 0) return 0;
        }
        rt_out(abs); rt_out(": "); rt_out(sniff_name(kind)); rt_out(", ");
        put_num(size); rt_out(" bytes\n");
        return 0;
    }

    if (cls == SNC_OPAQUE) {
        sys_close(fd);
        /* The refusal. It goes to STDOUT because it is the answer to the
         * question that was asked ("what is in this file?") -- the failure is
         * not in the command, it is in the assumption that everything is text.
         * stderr gets the one line that explains the refusal itself, so an
         * `errors only` filter shows why nothing appeared. */
        char hex[3 * 16 + 4];
        int nx = nh < 16 ? nh : 16;
        sniff_hex(head, nx, hex, (int)sizeof hex);
        rt_out(abs); rt_out(": "); rt_out(sniff_name(kind)); rt_out(", ");
        put_num(size); rt_out(" bytes\n");
        rt_out("  starts with: "); rt_out(hex); rt_out("\n");
        const char *op = sniff_opener(kind);
        if (op) { rt_out("  open it with: "); rt_out(op); rt_out(" "); rt_out(argv[1]); rt_out("\n"); }
        else rt_out("  not text -- refusing to print it\n");
        errs("show: "); errs(argv[1]); errs(" is ");
        errs(sniff_name(kind)); errs(", not text\n");
        return 0;
    }

    /* Text. The prefix said so; the rest of the file is still watched, because
     * a log with a NUL in the middle of it is a real thing and the terminal's
     * own guard should not be the first line of defence for a program that can
     * see the bytes before it writes them. */
    struct sniff_guard g;
    sniff_guard_reset(&g);

    {
        int keep = 0;
        while (keep < nh && !sniff_guard_byte(&g, head[keep])) keep++;
        rt_outn((const char *)head, keep);
    }
    if (!g.binary) {
        char b[512];
        int n;
        while (!g.binary && (n = sys_read(fd, b, sizeof b)) > 0) {
            int keep = 0;
            while (keep < n && !sniff_guard_byte(&g, (unsigned char)b[keep])) keep++;
            rt_outn(b, keep);
            if (g.binary) break;
        }
    }
    sys_close(fd);
    if (g.binary) {
        rt_out("\n[");
        rt_out(sniff_name(sniff_guard_kind(&g)));
        rt_out(" after ");
        put_num((long)g.bytes - 1);
        rt_out(" bytes of text -- rest not printed]\n");
        errs("show: "); errs(argv[1]); errs(" turns binary; stopped\n");
    }
    return 0;
}
