/* /bin/lm -- load a LOGITLM model and generate bytes on the device.
 *
 * The other check programs in this tree (vidcheck, audiocheck, h2check) exist
 * to make a claim checkable ("the decoder is bit-exact") by printing one
 * number a harness can diff. This one is different: there is no independent
 * oracle for "how fast is this machine's own arithmetic", because the number
 * IS the measurement -- tokens/s under TCG is the whole reason to run this
 * program rather than just linking the library into a test. So the shape here
 * is closer to a benchmark with a REPL feel than to a CRC checker: print what
 * loaded, stream what it produces as it is produced (so a person watching the
 * serial console sees generation happen rather than waiting for a block), and
 * end with the number.
 *
 * Same link shape as vidcheck.c: mini-libc + crt0_cli at 0x50000000, stdio
 * (printf/putchar for output), not the logit.h inline syscalls -- this is a
 * CLI .aex, not a windowed app. The MODEL FILE itself no longer goes through
 * stdio, see the loader section below.
 *
 * ---------------------------------------------------------------- loading --
 *
 * THIS PROGRAM USED TO DO ONE malloc() FOR THE WHOLE FILE AND fread() IT IN.
 * That is a copy the machine cannot always afford: a 355 MiB q4 file (the
 * full-vocabulary Qwen3-0.6B shape -- 151,936 tokens, see model.h) next to a
 * 512 MiB budget and a KV cache that is not small either. The copy was never
 * necessary in the first place -- model.h says so in its own opening
 * paragraph: "a loader does not read the file, it points at it" -- and
 * c/apps/libc/src/mman.c now reaches SYS_MMAP_FILE, so ring 3 can actually do
 * that.
 *
 * So the default here is MAP: open the file, mmap() it PROT_READ, and hand
 * lm_open the mapping. lm_open already BORROWS whatever pointer it is given
 * (model.h's struct lm_model comment) rather than copying it, so nothing
 * downstream of lm_open needed to change -- the format was built for exactly
 * this and the only thing missing was a caller willing to map instead of
 * read.
 *
 * THE OLD PATH IS KEPT, ON PURPOSE, AS THE CONTROL. --read forces it. The
 * whole claim of this file is "mapping lets a model run that reading cannot",
 * and a claim needs the thing it is being measured against sitting right next
 * to it in the same binary, not a since-deleted code path somebody has to
 * trust was faithfully remembered. It is also the automatic FALLBACK: if
 * mmap() itself fails (ENODEV -- not a LogitFS regular file, no VMA slot, see
 * <sys/mman.h>), this program does not just give up on a machine that could
 * still serve the model by copying it, it falls back and says why.
 *
 * WHAT MAPPING CHANGES ABOUT THE BUDGET LINE. Before, "model KiB + state KiB
 * = total KiB" was a resident-memory total, because both terms were malloc'd.
 * With a mapping, the model's bytes are NOT resident -- they are page-cache
 * frames that appear on first touch and can be DROPPED and re-faulted rather
 * than swapped, which is the whole point of a file-backed mapping over an
 * anonymous one (mman.c's header; the reclaim tier-1 machinery CLAUDE.md
 * describes). Printing "total" as model+state when the model is mapped would
 * be a lie the ceiling then refuses models it could actually run -- so this
 * file reports RESIDENT and MAPPED as two separate numbers, and the ceiling
 * below is checked against resident bytes only. See the load section for the
 * exact split and the refusal message for which term it names.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "model.h"
#include "infer.h"

/* ---------------------------------------------------------------- budget --
 *
 * "Does it fit" has to be answerable before lm_state_new touches the
 * allocator, on a machine with 512 MiB total and no swap unless a harness
 * attaches one (CLAUDE.md, Memory reclaim + swap). 256 MiB is not the whole
 * budget -- it is deliberately half of it, because /bin/lm is one process
 * among others on this desktop: the kernel image, its heap arena, the
 * framebuffer, and (CLAUDE.md again) a browser whose own arena alone wants 96
 * MiB. Model + state under 256 MiB leaves at least that much for everything
 * this process is not. A model that does not fit is refused here, loudly,
 * rather than left to OOM somewhere inside layer 2 of the forward pass.
 *
 * THE CEILING NOW BOUNDS RESIDENT BYTES, NOT FILE BYTES. A mapped model's
 * bytes are not this process's committed memory -- see the header comment --
 * so a 355 MiB mapped file and a 28 KiB state comfortably clears this ceiling
 * even though the FILE is bigger than it, and that is correct: the 256 MiB
 * policy exists to protect what this process pins, and a page-cache mapping
 * pins nothing that reclaim cannot take back. */
#define LM_MAX_TOTAL_BYTES (256u * 1024u * 1024u)

/* --budget MiB RAISES OR LOWERS THAT CEILING, AND IT EXISTS TO MEASURE IT.
 *
 * 256 MiB is a POLICY -- half of a 512 MiB machine, argued above -- and the
 * question this line has to answer is a different one: what is the largest
 * RESIDENT footprint this machine can physically hold? The two are not the
 * same number and nothing here knew either. With the ceiling compiled in,
 * every state above it produces the same message whether the allocator would
 * have succeeded or not, so the refusal is unfalsifiable: it reports the
 * constant back to you.
 *
 * It is a flag rather than a raised constant because BOTH readings are wanted.
 * The default is unchanged, so an ordinary run still refuses at 256 MiB with
 * the reason it always gave; `--budget 512` asks the machine instead of the
 * policy, and what comes back is either a run that proceeds or a specific
 * failure (a NULL malloc, a short read, a fault) with a size attached.
 *
 * 0 means "no ceiling at all" -- print the arithmetic and try. That is not a
 * dangerous default because it is not the default; it is how you find out
 * where the real wall is, which is a measurement that cannot be taken from
 * behind a check that fires first. */
static unsigned long long g_budget_bytes = LM_MAX_TOTAL_BYTES;

/* --read: force the malloc+fread control path (see the header comment). This
 * is also what a mmap() failure falls back to automatically, so the same
 * loader code serves both the deliberate control and the safety net -- one
 * copy of "read the whole file into a buffer" rather than two that could
 * drift apart. */
static int g_force_read = 0;

/* --posix-read: within the copy path, use open()+read() rather than
 * SYS_READ_FILE. It is the CONTROL for the claim that open() on this kernel
 * copies the whole file into the kernel heap -- see the copy path for the
 * line of file.c that does it. Same model, same machine, one flag apart, and
 * the difference is visible from OUTSIDE the process in the kernel's own
 * `[kheap] grow` lines, which is what makes it evidence rather than this
 * program's opinion of itself. */
static int g_posix_read = 0;

#define LM_SYS_READ_FILE 11        /* logit_abi.h:17 -- (name, buf, max) */

/* --seq N LOWERS THE KV CACHE'S CONTEXT LENGTH, and it exists because the
 * question "at what context length does this machine stop fitting the model?"
 * otherwise costs one 355 MiB file per answer.
 *
 * It is sound because seq_len is NOT part of the on-disk payload -- c/lib/nn/
 * model.c:162 says so in as many words, and lm_expected_size() never reads it.
 * It sizes exactly two things, both allocated after the file is already open:
 * the KV cache (seq_len * kv_dim * 2 * n_layers floats) and the attention
 * score row (n_heads * seq_len). So lowering the header's copy after lm_open
 * has validated the file changes the STATE and nothing about the WEIGHTS.
 *
 * LOWER ONLY, NEVER RAISE. Raising it would be arithmetic the file's RoPE
 * tables and training never saw, and worse, it would make lm_state_bytes
 * report a number for a configuration that is not the model's -- the
 * measurement would be of an invented shape. Refused out loud rather than
 * clamped silently, because a harness that asked for 1024 and got 512 would
 * record the wrong x-axis. */
static long g_seq_cap = 0;         /* 0 = use the header's seq_len */

/* --mm PRINTS THE KERNEL'S OWN MEMORY COUNTERS, not this program's idea of
 * its size. Everything else /bin/lm prints about memory is arithmetic over
 * the header; the only thing that knows how many physical frames this run
 * actually cost is the kernel, and c/kernel/mm/mmsys.c's SYS_MEMINFO
 * diagnostic door (a == 0, b == MMCTL_*) is how a ring-3 program asks. The
 * output goes to the kernel's kprintf -- i.e. the serial console -- not to
 * this program's stdout, so it can never land inside a byte-equality region
 * tests/boot/run-lm-test.sh holds; that is a property of the channel, not
 * something this file has to keep arranging.
 *
 * OFF BY DEFAULT because MMCTL_REPORT is not free (it walks the kheap arenas)
 * and because a run whose numbers are being compared against the host should
 * not be doing extra work the host cannot do at all. */
static int g_mm = 0;

#define LM_SYS_MEMINFO  94         /* logit_abi.h:531 */
#define LM_MMCTL_REPORT 1          /* c/kernel/mm/mmsys.c:22-25 */
#define LM_MMCTL_STATS  4

/* Defined with the mapping code below, where the int 0x80 guard is argued.
 * Forward-declared here rather than moved up so that the whole "how this
 * program talks to the kernel behind its libc" story stays in one place. */
static long lm_syscall(long n, long a, long b, long c);

static void mm_mark(const char *tag)
{
    if (!g_mm) return;
    /* The tag cannot be passed through -- mm_report() takes one from the
     * kernel side and MMCTL_REPORT hardcodes "on demand" -- so print our own
     * marker line first and let the kernel's lines follow it. Serial output
     * from both is line-buffered by the same UART, so the ordering holds. */
    printf("lm: --- mm mark: %s ---\n", tag);
    fflush(stdout);
    lm_syscall(LM_SYS_MEMINFO, 0, LM_MMCTL_STATS, 0);
    lm_syscall(LM_SYS_MEMINFO, 0, LM_MMCTL_REPORT, 0);
}

static void usage(const char *argv0)
{
    printf("usage: %s [-m /model.lm] [-p \"prompt\"] [-n 200] [-t 0.8] "
           "[-s seed] [--greedy] [--budget MiB] [--seq N] [--read] "
           "[--posix-read] [--mm]\n"
           "       [--ids 1,2,3] [--print-ids] [--dump-logits FILE]\n"
           "  --ids replaces -p with real token ids (the tokenizer stays on\n"
           "  the host); --print-ids prints generated ids instead of bytes;\n"
           "  --dump-logits writes the raw f32 logit row after the prompt.\n",
           argv0);
}

/* lm_open's refusal codes are named in model.h; a person on a serial console
 * with a load failure has nothing else to go on, so print which, not
 * "failed to load". */
static const char *lm_open_strerror(int rc)
{
    switch (rc) {
    case -1: return "bad magic (not a LOGITLM file)";
    case -2: return "wrong version";
    case -3: return "a reserved header field is set (file is newer than this build understands)";
    case -4: return "inconsistent header (a zero dimension, or dim not a multiple of n_heads)";
    case -5: return "file size does not match what the header declares (truncated or corrupt)";
    case -6: return "out of memory building the model's tensor descriptors";
    default: return "unknown error";
    }
}

/* ------------------------------------------- mapping a file on LogitOS --
 *
 * mmap(fd) THROUGH THIS LIBC CANNOT WORK, AND THAT IS NOT A BUG IN THIS FILE.
 * c/apps/libc/src/mman.c refuses every request that is not
 * MAP_ANONYMOUS|MAP_PRIVATE with fd == -1, out loud, with ENODEV -- its header
 * says why: "there is no file-backed mapping ANYWHERE in this kernel". That
 * sentence was true when it was written and is not true now:
 * c/kernel/mm/mmsys.c implements SYS_MMAP_FILE (162) over the page cache, and
 * c/kernel/exec/syscall.c forwards it. So the capability exists at the
 * syscall boundary and the C library in front of it has not been taught about
 * it yet.
 *
 * WHY /bin/lm REACHES PAST THE LIBC INSTEAD OF FIXING THE LIBC. Ownership,
 * only: c/apps/libc/src/mman.c belongs to another workflow in flight, and a
 * second edit to it would be resolved by whoever writes it last rather than
 * by whoever is right. The right long-term shape is obviously mman.c gaining
 * the file branch and this function disappearing -- when it does, delete this
 * and pass fd to mmap(). Until then the alternative is not "use the libc", it
 * is "do not map at all", i.e. copy 355 MiB into a 512 MiB machine, which is
 * the exact wall this program exists to get past.
 *
 * THE LIBC IS STILL TRIED FIRST, and the order matters. On the host build
 * mmap(fd) is the real POSIX one and works, so this function is never
 * reached there; on device it fails with ENODEV and this is the fallback. One
 * decision site, not a compile-time fork between two loaders that could
 * silently diverge in what they hand to lm_open.
 *
 * The int 0x80 guard is c/apps/as/as_ll.c's, verbatim in intent and for the
 * reason it states: on x86_64 Linux a real int 0x80 traps into the i386
 * compat table (-ENOSYS), so the asm must not be compiled into a hosted
 * build even though the host is also x86_64. */
struct lm_mmap_file_req {          /* mirrors struct logit_mmap_file_req,
                                    * include/abi/logit_abi.h:578. Copied
                                    * rather than #included because that
                                    * header is a kernel<->user ABI file this
                                    * program does not otherwise include, and
                                    * the field order is checked below by the
                                    * only test that matters -- the call
                                    * either returns a mapping or 0. */
    unsigned long long hint, len, off;
    int fd, prot;
};

#define LM_SYS_MMAP_FILE 162       /* logit_abi.h:596 */
#define LM_MMAP_PROT_READ 0x1      /* logit_abi.h:518 */

static long lm_syscall(long n, long a, long b, long c)
{
#if defined(__x86_64__) && !defined(__APPLE__) && !__STDC_HOSTED__
    long r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
    return r;
#else
    (void)n; (void)a; (void)b; (void)c;
    return 0;                      /* hosted: no LogitOS syscalls. 0 is
                                    * SYS_MMAP_FILE's own "failed" value, so a
                                    * host build falls through to the read
                                    * path exactly as an unmappable file does. */
#endif
}

/* Returns the base address, or NULL. Never partial: the kernel either
 * reserves the whole length or reserves nothing (vma_reserve_file). */
static void *lm_map_file(int fd, size_t len, const char **why)
{
    struct lm_mmap_file_req req;
    req.hint = 0; req.len = (unsigned long long)len; req.off = 0;
    req.fd = fd; req.prot = LM_MMAP_PROT_READ;
    long base = lm_syscall(LM_SYS_MMAP_FILE, (long)&req, 0, 0);
    if (base == -1) {              /* LOGIT_MMAP_FILE_E_WRITE -- cannot happen
                                    * with PROT_READ alone, but the code is
                                    * distinguishable on purpose (logit_abi.h)
                                    * so it is reported distinguishably. */
        *why = "SYS_MMAP_FILE refused a write mapping (this is a bug in lm.c: "
               "it asked for PROT_READ)";
        return NULL;
    }
    if (base == 0) {
        /* Every generic refusal: not an F_VFS fd, the pcache file table full,
         * no VMA slot, a length that overflows. The kernel has already said
         * which on the console in the cases where it can (pcache_file_open). */
        *why = "SYS_MMAP_FILE returned 0 -- not a mappable LogitFS file, or "
               "no VMA/page-cache slot (see the kernel's own line above)";
        return NULL;
    }
    *why = NULL;
    return (void *)(unsigned long)base;
}

/* Release whatever the loader produced. One function rather than the
 * munmap-or-free branch repeated at every early return: the two calls take
 * different arguments (munmap wants the length back, free does not) and a
 * copy-pasted branch is exactly how one return path ends up freeing a mapping
 * or munmapping a malloc'd pointer. `fd` may be -1 (never opened) or already
 * consumed; closing -1 is refused by close() harmlessly on this libc (EBADF,
 * ignored -- there is nothing left to do about it here). */
static void release_blob(unsigned char *blob, size_t len, int mapped, int fd)
{
    if (blob) {
        if (mapped) munmap(blob, len);
        else free(blob);
    }
    if (fd >= 0) close(fd);
}

/* Fold one logits row into the running health summary. See the declaration
 * of lo_min/lo_max in main() for why this is measured at all. `rows` doubles
 * as the "have we seen anything yet" flag, so min/max start from a real value
 * rather than from a sentinel that could survive into the printout. */
static void logit_scan(const float *v, int n, float *mn, float *mx,
                       long *nonfinite, long *rows)
{
    int have = (*rows != 0);   /* a previous row already seeded min/max. NOT
                                * "i != 0": if the very first element of the
                                * very first row is a NaN it is skipped, and
                                * seeding on index alone would then compare
                                * every later value against an uninitialised
                                * pair -- exactly the case this whole check
                                * exists to detect, silently mishandled. */
    for (int i = 0; i < n; i++) {
        float x = v[i];
        if (x != x || x > 3.4e38f || x < -3.4e38f) { (*nonfinite)++; continue; }
        if (!have) { *mn = *mx = x; have = 1; }
        else { if (x < *mn) *mn = x; if (x > *mx) *mx = x; }
    }
    if (have) (*rows)++;       /* a row of nothing but NaN seeds nothing, so it
                                * must not be counted as one that did */
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* The --ids buffer bound. 512 because it is the seq_len this model line is
 * built at, so a longer list could not be fed anyway; a fixed buffer rather
 * than a malloc because the list arrives on a command line, where the length
 * is bounded by the OS's argv limit long before it is bounded by anything
 * here. */
#define LM_IDS_MAX 512

/* THE ARENA ACCESSORS EXIST ONLY IN THE FREESTANDING BUILD, and the guard is
 * __STDC_HOSTED__ because that is the discriminator malloc.c itself uses to
 * pick its backend. build/lm_host is an ordinary hosted program linked against
 * glibc's malloc, which has no arena and no such symbols -- declaring them
 * unconditionally is an undefined reference at link time, which is how this
 * was found. Declared here rather than reached through a header because
 * malloc.c exports them without one: it is the allocator every other TU
 * depends on and deliberately acquires no include that might itself allocate
 * (its own comment at arena_sys). */
#if !__STDC_HOSTED__
extern size_t malloc_arena_size(void);
extern int    malloc_arena_failed;
#define LM_HAVE_ARENA 1
#endif

int main(int argc, char **argv)
{
    const char *model_path = "/model.lm";
    const char *prompt = "";
    int n_predict = 200;
    double temperature = 0.8;
    unsigned long long seed = 0;
    int have_seed = 0;
    int greedy = 0;

    /* --ids / --print-ids: THE TOKENIZER STAYS ON THE HOST.
     *
     * -p feeds the prompt one BYTE at a time, which is the right and only
     * convention for a byte-level synthetic model and is simply wrong for a
     * real one: Qwen3's vocabulary is 151,936 BPE tokens and "hello" is not
     * five of them. A real tokenizer on the device would be ~887 KiB of merge
     * table (tools/lmtok.py measured it) and, worse, a SECOND implementation
     * of the encoder to keep in step with the host's -- and a divergence
     * there is indistinguishable from a divergence in the arithmetic, which
     * is the one comparison this program exists to make trustworthy.
     *
     * So ids in, ids out. The host encodes with tools/lmtok.py (gated against
     * the real tokenizer at 38,990 tokens exact) and decodes the same way,
     * and HOST-vs-DEVICE becomes an exact integer comparison with no text,
     * no locale and no font in the middle of it. */
    const char *ids_arg = NULL;
    int print_ids = 0;
    const char *dump_logits = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            model_path = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            prompt = argv[++i];
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            n_predict = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            temperature = atof(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            seed = strtoull(argv[++i], NULL, 10);
            have_seed = 1;
        } else if (strcmp(argv[i], "--budget") == 0 && i + 1 < argc) {
            unsigned long long mib = strtoull(argv[++i], NULL, 10);
            /* The multiplication is done in 64 bits and the argument is
             * bounded first: `--budget 4294967296` on a 32-bit product would
             * wrap to a SMALL ceiling and refuse a model that fits, which is
             * the one failure mode of this flag that would look like a
             * finding. 65536 MiB is past any machine this will run on. */
            if (mib > 65536ull) {
                printf("lm: --budget %llu MiB is not a number this machine "
                       "could mean\n", mib);
                return 2;
            }
            g_budget_bytes = mib * 1024ull * 1024ull;
        } else if (strcmp(argv[i], "--ids") == 0 && i + 1 < argc) {
            ids_arg = argv[++i];
        } else if (strcmp(argv[i], "--print-ids") == 0) {
            print_ids = 1;
        } else if (strcmp(argv[i], "--dump-logits") == 0 && i + 1 < argc) {
            dump_logits = argv[++i];
        } else if (strcmp(argv[i], "--greedy") == 0) {
            greedy = 1;
        } else if (strcmp(argv[i], "--read") == 0) {
            g_force_read = 1;
        } else if (strcmp(argv[i], "--mm") == 0) {
            g_mm = 1;
        } else if (strcmp(argv[i], "--posix-read") == 0) {
            g_posix_read = 1;
            g_force_read = 1;    /* --posix-read is a variant OF the copy path;
                                  * asking for it while still trying to map
                                  * would silently measure nothing. */
        } else if (strcmp(argv[i], "--seq") == 0 && i + 1 < argc) {
            g_seq_cap = strtol(argv[++i], NULL, 10);
            if (g_seq_cap <= 0) {
                printf("lm: --seq %s is not a positive token count\n", argv[i]);
                return 2;
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            printf("lm: unrecognised argument '%s'\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }
    if (n_predict < 0) {
        printf("lm: -n must be >= 0\n");
        return 2;
    }

    /* --------------------------------------------------------- load --- */
    /* stat(), NOT open()+fstat(), AND THAT IS THE MOST EXPENSIVE LINE IN THIS
     * FILE. It used to be open-then-fstat, on the reasonable-sounding ground
     * that finding and sizing the file is apparatus rather than measurement
     * and so should be identical on every path. On this kernel it is not
     * apparatus:
     *
     *   c/kernel/exec/file.c:759, file_open_vfs() --
     *       f->backing = kmalloc((size_t)cap);   cap == the whole file
     *
     * open(2) on LogitOS COPIES THE ENTIRE FILE INTO THE KERNEL HEAP. There
     * is no offset read anywhere below the VFS -- vfs_read(path, buf, max) is
     * all-or-nothing from byte 0 -- so an fd is a whole-file buffer with a
     * cursor, by construction. For a 3 KiB model that is invisible. For a 355
     * MiB one it is 355 MiB of KERNEL heap spent before this program has
     * asked for anything, and then the read path spends 355 MiB more in ring
     * 3 for the copy it actually wanted: 710 MiB on a 512 MiB machine, to
     * load a 355 MiB file.
     *
     * SYS_STAT does not do this (c/kernel/exec/syscall.c:1209 -> meta_syscall
     * -> c/fs/vfs_meta.c: a metadata lookup, no backing buffer), so the size
     * is taken from the path and the fd is bought only by the path that
     * genuinely needs one -- mapping, whose SYS_MMAP_FILE takes an fd and not
     * a name (logit_abi.h:582 argues why: the access decision was already
     * made at open time and must not be made twice).
     *
     * MEASURED, NOT REASONED, and it is what sent this whole run the right
     * way: build/scr/k28.log is a 511 MiB machine loading a 269 MiB model to
     * completion with the kernel heap never growing past 20,480 KiB. A
     * kmalloc of 269 MiB would have printed a `[kheap] grow` line for itself
     * -- kheap.c:232 prints one every time, on purpose. It printed four, all
     * at boot. So the load that worked never called open(). */
    long len = 0;
    {
        struct stat fst;
        if (stat(model_path, &fst) != 0) {
            printf("lm: cannot stat %s (does it exist?)\n", model_path);
            return 1;
        }
        if (fst.st_size <= 0) {
            printf("lm: %s is empty or unreadable\n", model_path);
            return 1;
        }
        len = (long)fst.st_size;
    }
    int fd = -1;              /* opened ONLY on the mapping path, below */

    /* The BASELINE, taken before a single model byte exists in this address
     * space. Every later mark is only meaningful as a delta against it: the
     * kernel's free-frame count includes the kernel, the desktop and whatever
     * else booted, none of which is this run's cost. */
    mm_mark("baseline, before the model file is touched");

    unsigned char *blob = NULL;
    int mapped = 0;              /* 1: blob is SYS_MMAP_FILE, not resident until
                                  * touched. 0: blob is a malloc'd, fully
                                  * resident copy (the control / the fallback). */
    const char *why_not_mapped = NULL;   /* NULL when mapped; else the reason,
                                          * for the load-path line below. */
    const char *copy_route = "copy: (not taken)";  /* which of the two copy
                                          * routes actually ran; set at the
                                          * point of decision, not re-derived
                                          * from the flags afterwards -- the
                                          * host falls back silently and a
                                          * flag-derived label would call that
                                          * a deliberate control. */
    int map_via_syscall = 0;             /* mapped, but through SYS_MMAP_FILE
                                          * directly because this libc's
                                          * mmap() refuses an fd. Reported
                                          * separately: "it mapped" and "it
                                          * mapped despite the libc" are
                                          * different facts about the machine. */

    double t_load0 = now_s();

    if (!g_force_read) {
        /* The fd exists only for this branch, and on this kernel it is not
         * free -- see the stat() comment above. Charging it to the mapping
         * path rather than to every path is the whole point of buying it
         * here, and it is also honest about what mapping COSTS on this
         * machine today: an fd whose backing buffer is the entire file, i.e.
         * mapping cannot avoid the copy it exists to avoid until the VFS
         * grows a pread. That is a finding, not a workaround, and the load
         * line below reports it. */
        fd = open(model_path, O_RDONLY);
        if (fd < 0) {
            why_not_mapped = "open() failed, so there is no fd to map "
                             "(SYS_MMAP_FILE takes an fd, not a path)";
        } else {
        void *m = mmap(NULL, (size_t)len, PROT_READ, MAP_PRIVATE, fd, 0);
        if (m == MAP_FAILED) {
            /* The libc said no. On the host that is a real refusal and the
             * read path is the answer; on LogitOS it is ENODEV from a mman.c
             * that has not been taught about SYS_MMAP_FILE yet, and the
             * kernel underneath it CAN do this -- see lm_map_file above. Try
             * the syscall directly before giving up on mapping. Both reasons
             * are kept and printed together, because "the libc refused" and
             * "the kernel refused too" are different findings and only the
             * second one means the file cannot be mapped. */
            const char *libc_why = strerror(errno);
            const char *raw_why = NULL;
            void *d = lm_map_file(fd, (size_t)len, &raw_why);
            if (d) {
                blob = (unsigned char *)d;
                mapped = 1;
                map_via_syscall = 1;
            } else {
                static char both[256];
                snprintf(both, sizeof both, "libc mmap(fd): %s; direct %s",
                         libc_why, raw_why ? raw_why : "?");
                why_not_mapped = both;
            }
        } else {
            blob = (unsigned char *)m;
            mapped = 1;
        }
        }
        /* The fd is CLOSED when the mapping failed, because on this kernel it
         * is holding a whole-file kernel buffer and the read path below is
         * about to allocate the same bytes again in ring 3. Keeping it "just
         * in case" would double the peak for no benefit -- the read path does
         * not use it (see below). When the mapping SUCCEEDED the fd is kept:
         * the VMA holds a page-cache reference, not this descriptor, but
         * closing it here would be a change of lifetime this program has not
         * measured, and release_blob() closes it at the end either way. */
        if (!mapped && fd >= 0) { close(fd); fd = -1; }
    } else {
        why_not_mapped = "--read forces the copy path";
    }

    if (!mapped) {
        /* ONE malloc for the whole file -- and, on this kernel, ONE copy of
         * it, which the obvious open()+read() loop does not give you.
         *
         * THE COPY PATH HAS TWO IMPLEMENTATIONS AND THAT IS DELIBERATE, with
         * the second one existing to be the control for a claim about the
         * kernel rather than about this program:
         *
         *   SYS_READ_FILE (11)  vfs_read() straight into the ring-3 buffer
         *                       (c/kernel/exec/syscall.c:468). No fd, no
         *                       F_VFS backing buffer, no kernel copy. ONE
         *                       copy of the file exists when this returns.
         *   open() + read()     the POSIX shape. Correct, portable, and on
         *                       LogitOS it pays for the file TWICE -- once in
         *                       the kernel heap at open() (file.c:759) and
         *                       once here.
         *
         * The first is used on device, the second on the host (where it is
         * the only one that exists, and where open() does not copy anything).
         * --posix-read forces the second on device, which is how "open()
         * doubles the peak" gets measured instead of asserted: the same
         * model, the same machine, one flag apart.
         *
         * NOT looped: SYS_READ_FILE is all-or-nothing by construction
         * (logitfs.c refuses any max smaller than the file), so a partial
         * return is not a short read to retry, it is a failure -- and the
         * length check below treats it as one. */
        blob = (unsigned char *)malloc((size_t)len);
        if (!blob) {
            printf("lm: out of memory allocating the %ld-byte model buffer "
                   "(this is ring 3's malloc arena, not the machine -- see "
                   "ARENA_SIZE in c/apps/libc/src/malloc.c)\n", len);
            if (fd >= 0) close(fd);
            return 1;
        }
        long got = -1;
        int used_posix = g_posix_read;
        if (!g_posix_read) {
            got = lm_syscall(LM_SYS_READ_FILE, (long)model_path,
                             (long)blob, (long)len);
            if (got == 0 && len > 0) {
                /* A hosted build: lm_syscall is the stub and returns 0. Fall
                 * through to the POSIX path rather than reporting a
                 * zero-byte model. `used_posix` and not g_posix_read, so the
                 * report line below still distinguishes "the host has no
                 * other route" from "--posix-read was asked for on a machine
                 * that does". */
                got = -1;
                used_posix = 1;
            }
        }
        if (used_posix || got < 0) {
            used_posix = 1;
            if (fd < 0) fd = open(model_path, O_RDONLY);
            if (fd < 0) {
                printf("lm: cannot open %s\n", model_path);
                free(blob);
                return 1;
            }
            size_t n = 0;
            while (n < (size_t)len) {
                ssize_t r = read(fd, blob + n, (size_t)len - n);
                if (r < 0) {
                    printf("lm: read error on %s\n", model_path);
                    free(blob); close(fd);
                    return 1;
                }
                if (r == 0) break;   /* short file -- the check below catches it */
                n += (size_t)r;
            }
            got = (long)n;
        }
        if (got != len) {
            printf("lm: short read on %s (%ld of %ld bytes)\n",
                   model_path, got, len);
            free(blob);
            if (fd >= 0) close(fd);
            return 1;
        }
        /* Whichever route was taken, the descriptor has done its job and is
         * holding a whole-file kernel buffer if it exists at all. */
        if (fd >= 0) { close(fd); fd = -1; }
        copy_route = used_posix
            ? (g_posix_read ? "copy: open()+read()  <-- ALSO buys a whole-file "
                              "copy in the KERNEL heap (file.c:759)"
                            : "copy: open()+read() (host: the only route)")
            : "copy: SYS_READ_FILE (one copy, straight into ring 3)";
    }
    double t_load1 = now_s();
    double load_s = t_load1 - t_load0;

    struct lm_model m;
    int rc = lm_open(&m, blob, (size_t)len);
    if (rc != 0) {
        printf("lm: cannot load %s: %s (lm_open rc=%d)\n",
               model_path, lm_open_strerror(rc), rc);
        release_blob(blob, (size_t)len, mapped, fd);
        return 1;
    }

    /* --seq, applied here: after lm_open has validated the file against
     * lm_expected_size() (which never reads seq_len) and before anything
     * computes a state size from it. See the flag's comment for why this is
     * a legal edit to an already-open model and why it may only go down. */
    if (g_seq_cap) {
        if ((unsigned long)g_seq_cap > (unsigned long)m.h.seq_len) {
            printf("lm: --seq %ld is ABOVE this model's seq_len=%u; refusing "
                   "rather than clamping, because a run recorded at the "
                   "number you asked for would be a run at a different one\n",
                   g_seq_cap, (unsigned)m.h.seq_len);
            lm_close(&m);
            release_blob(blob, (size_t)len, mapped, fd);
            return 2;
        }
        printf("lm: seq_len       %u -> %ld (--seq; the KV cache and the "
               "attention row are the only things this resizes)\n",
               (unsigned)m.h.seq_len, g_seq_cap);
        m.h.seq_len = (unsigned)g_seq_cap;
    }

    char desc[160];
    lm_describe(&m, desc, (int)sizeof(desc));
    printf("%s\n", desc);

    printf("lm: load path     %s (%.3f s%s)\n",
           !mapped ? copy_route
                   : (map_via_syscall ? "mmap (SYS_MMAP_FILE, direct -- this "
                                        "libc's mmap() has no fd branch)"
                                      : "mmap (libc)"),
           load_s,
           mapped ? " -- address space only, no bytes read yet"
                  : " -- the whole file, copied");
    if (why_not_mapped)
        printf("lm: not mapped    %s\n", why_not_mapped);

    /* The budget check, BEFORE lm_state_new asks the allocator for anything.
     * lm_state_bytes is a pure function of the header (infer.h), so this is
     * knowable without touching memory beyond what lm_open already holds.
     *
     * RESIDENT AND MAPPED ARE COUNTED SEPARATELY, and the ceiling below is
     * resident-only -- see the header comment for why a total that included
     * mapped bytes would be a lie. `model_resident` is the model's own
     * contribution: the whole file when read, zero when mapped (its pages are
     * page-cache frames, not this process's committed memory, until and
     * unless they are touched -- and even touched ones are reclaimable, never
     * pinned the way a malloc'd block is). */
    size_t state_bytes = lm_state_bytes(&m);
    size_t model_resident = mapped ? 0 : (size_t)len;
    size_t resident_bytes = model_resident + state_bytes;
    size_t mapped_bytes = mapped ? (size_t)len : 0;

    /* THE "mapped" DETAIL LINE GOES BEFORE THE "KiB total" LINE, NOT AFTER,
     * and that ordering is not cosmetic. tests/boot/run-lm-test.sh's
     * compare.py takes the "KiB total" line's END as the start of the region
     * it holds to byte equality between host and device (the prompt +
     * generated bytes) -- so ANY line printed after "KiB total" and before
     * the generation loop would be swept into a region the harness assumes
     * is deterministic model output, and would fail it for printing a fact
     * (bytes mapped) that is true on both machines but was ordered wrong. */
    if (mapped)
        printf("lm: mapped        %lu KiB (page-cache, reclaimable, not counted "
               "toward the ceiling below)\n",
               (unsigned long)((mapped_bytes + 1023) / 1024));

    /* This line's trailing "KiB total" is read by tests/boot/run-lm-test.sh
     * as the marker for "generation output starts after this line" -- keep
     * the exact substring even though what the number COUNTS has changed
     * (resident bytes, not file bytes; see the header comment). Qualifiers go
     * BEFORE the number, never after, so the marker string is never split
     * across a printf that could get reordered. */
    printf("lm: model %lu KiB %s + state %lu KiB resident = %lu KiB total\n",
           (unsigned long)((((size_t)len) + 1023) / 1024),
           mapped ? "mapped (0 resident)" : "resident",
           (unsigned long)((state_bytes + 1023) / 1024),
           (unsigned long)((resident_bytes + 1023) / 1024));

    if (g_budget_bytes && (unsigned long long)resident_bytes > g_budget_bytes) {
        /* NAME WHICH TERM HIT IT, because "resident_bytes exceeds the
         * ceiling" is two different findings depending on whether the model
         * is mapped: with mapping, only the STATE (KV cache + activations)
         * can be at fault, and no amount of mmap saves a context length whose
         * cache alone does not fit; without mapping, the model itself is the
         * usual culprit and the fix (this program has one) is to stop
         * forcing --read. */
        printf("lm: refusing to run: %lu KiB resident exceeds the %lu KiB "
               "ceiling (512 MiB machine, no swap assumed -- see lm.c; "
               "--budget MiB moves it, --budget 0 removes it)\n",
               (unsigned long)(resident_bytes / 1024),
               (unsigned long)(g_budget_bytes / 1024));
        if (mapped)
            printf("lm: the model is MAPPED and contributes 0 of that -- the "
                   "%lu KiB inference state (KV cache + activations, a "
                   "function of seq_len) is the whole resident total by "
                   "itself\n", (unsigned long)(state_bytes / 1024));
        else
            printf("lm: the model was READ into memory (%lu KiB, %s) and is "
                   "the majority of that total; %s\n",
                   (unsigned long)(model_resident / 1024),
                   g_force_read ? "--read was passed" : "mmap() failed above",
                   g_force_read ? "drop --read to map it instead"
                                : "the machine cannot map this file either way");
        lm_close(&m);
        release_blob(blob, (size_t)len, mapped, fd);
        return 1;
    }

    mm_mark("before the inference state is allocated");

    /* WHAT THE HEAP ACTUALLY IS, printed before the allocation that may fail
     * on it. malloc.c's arena_reserve() HALVES its request until one succeeds
     * (128 -> 64 -> 32 -> ...), so a build compiled with a 128 MiB reservation
     * can be running on 16 -- and the failure message below, which names the
     * size ASKED FOR, then sends the reader hunting for a machine out of
     * memory when the machine has hundreds of megabytes free and it is the
     * RESERVATION that is small. Measured on the device at 1024 MiB: 455 MiB
     * free and a 29 MiB state refused, which is not explicable from any other
     * number this program prints. */
#ifdef LM_HAVE_ARENA
    printf("lm: ring-3 heap    %lu KiB reserved%s\n",
           (unsigned long)(malloc_arena_size() / 1024),
           malloc_arena_failed ? " (RESERVATION FAILED -- there is no heap)" : "");
#endif

    struct lm_state st;
    if (lm_state_new(&st, &m) != 0) {
        printf("lm: out of memory allocating the %lu KiB inference state\n",
               (unsigned long)(state_bytes / 1024));
        lm_close(&m);
        release_blob(blob, (size_t)len, mapped, fd);
        return 1;
    }

    /* -------------------------------------------------------- seed --- */
    /* --greedy is deterministic by construction (lm_sample_greedy takes no
     * rng) and is what the device test asserts on -- the seed below is dead
     * code on that path and is not read. For the stochastic path, an
     * unspecified seed is drawn from the monotonic clock (10 ms granularity,
     * see c/apps/libc/src/time.c) so an interactive run varies run to run;
     * -s makes a run reproducible for anyone who needs to bisect one. */
    if (!have_seed) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        seed = (unsigned long long)ts.tv_sec * 1000000000ull
             + (unsigned long long)ts.tv_nsec;
        if (seed == 0) seed = 1; /* the rng below is a zero-is-a-fixed-point LCG-ish xorshift */
    }
    unsigned long long rng = seed;

    /* Nucleus threshold for the stochastic path. Not a CLI flag: -t is the
     * one knob this program's spec names, and top-p has no established
     * default in this codebase to defer to, so 0.9 (the llama.cpp / GPT-2
     * convention) is used rather than invented per run. */
    const float topp = 0.9f;

    int vocab = (int)m.h.vocab;
    int seq_len = (int)m.h.seq_len;
    size_t prompt_len = strlen(prompt);
    long total_steps = (long)prompt_len + (long)n_predict;
    if (total_steps > seq_len) {
        printf("lm: prompt (%lu bytes) + n (%d) = %ld tokens exceeds this "
               "model's seq_len=%d (the KV cache's size); truncating output "
               "to fit\n", (unsigned long)prompt_len, n_predict, total_steps, seq_len);
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long steps_run = 0;

    /* first-token vs steady-state, timed separately. At this size the two are
     * not close: when the model is mapped, the very first lm_forward call is
     * also the moment every weight matrix it touches gets faulted in from
     * disk for the first time, and folding that into an average over
     * n_predict tokens would report neither the load cost nor the decode
     * rate -- and the blend would move with -n, so it would not even be a
     * stable wrong answer (the same argument tools/lmshape.c's --forward
     * makes; this is that argument's device-and-host-shared counterpart). */
    double t_first = -1.0, t_rest = 0.0;

    /* ------------------------------------------------- logit health -----
     *
     * WHY THIS IS NOT OPTIONAL AT THIS DEPTH. A 4-layer model that goes wrong
     * produces visibly wrong bytes; a 28-layer one produces bytes that look
     * exactly as random as the correct ones, because the weights ARE random
     * (tools/lmshape.c: "a claim about arithmetic, never about language"). So
     * the output stream cannot tell a working forward pass from one that
     * saturated into NaN at layer 19 and has been sampling argmax over a row
     * of NaNs ever since -- lm_sample_greedy would keep returning index 0 and
     * print a stream of NUL bytes, which is a perfectly plausible-looking
     * thing for an untrained model to do.
     *
     * The two numbers that CAN tell them apart are the extreme logit and the
     * count of non-finite ones, gathered over every step rather than the last
     * one, because a single bad position among 80 is the case a final-row
     * check would miss entirely.
     *
     * NO isnan()/isinf(): -ffreestanding, and <math.h> on this libc is musl's
     * subset. `v != v` is NaN by IEEE-754's own definition and needs no
     * header; infinity is caught by the magnitude bound, which is a bound on
     * what a f32 logit can be and not a fitted threshold -- see the check. */
    float lo_min = 0.0f, lo_max = 0.0f;
    long lo_nonfinite = 0, lo_rows = 0;

    /* --------------------------------------------------- prompt fill --- */
    /* Byte-level vocabulary (model.h / CLAUDE.md): token == byte, no
     * tokenizer file exists or is wanted. Fed one byte at a time through
     * lm_forward because infer.h is explicit that this layer has no batch
     * path (ONE TOKEN AT A TIME) -- nn_matmul_f32 is there for a caller that
     * wants a real prefill and this program is not that caller. `st.pos` is
     * read back after every call rather than tracked separately, because
     * infer.h's contract is stated in terms of it ("pos must be s->pos") and
     * relying on the field itself is the one reading that cannot drift from
     * whatever lm_forward actually does to it. */
    const float *logits = NULL;

    /* --ids REPLACES the byte feed above rather than adding to it. Parsed
     * here and not at argument time because `vocab` and `seq_len` -- the two
     * things an id has to be checked against -- are properties of the model,
     * which is not open yet when argv is walked.
     *
     * AN OUT-OF-RANGE ID IS REFUSED, NOT CLAMPED. lm_embed_row would read off
     * the end of the embedding table, and the failure that produces is a
     * plausible-looking vector rather than a fault -- the same shape as every
     * other silent failure this file argues about. */
    int  ids_buf[LM_IDS_MAX];
    long ids_n = 0;
    if (ids_arg) {
        const char *s = ids_arg;
        while (*s) {
            while (*s == ',' || *s == ' ') s++;
            if (!*s) break;
            if (*s < '0' || *s > '9') {
                printf("lm: --ids: '%s' is not a comma-separated list of "
                       "non-negative integers\n", ids_arg);
                lm_state_free(&st); lm_close(&m);
                release_blob(blob, (size_t)len, mapped, fd); return 1;
            }
            long v = 0;
            while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
            if (v >= (long)vocab) {
                printf("lm: --ids: token %ld is outside the model's vocabulary "
                       "of %d -- refused rather than clamped, because reading "
                       "past the embedding table produces a plausible vector "
                       "and no error\n", v, vocab);
                lm_state_free(&st); lm_close(&m);
                release_blob(blob, (size_t)len, mapped, fd); return 1;
            }
            if (ids_n >= LM_IDS_MAX) {
                printf("lm: --ids: more than %d tokens; this is a fixed buffer "
                       "because the list comes off a command line\n", LM_IDS_MAX);
                lm_state_free(&st); lm_close(&m);
                release_blob(blob, (size_t)len, mapped, fd); return 1;
            }
            ids_buf[ids_n++] = (int)v;
        }
        prompt_len = (size_t)ids_n;
    }

    for (size_t i = 0; i < prompt_len && st.pos < seq_len; i++) {
        int tok = ids_arg ? ids_buf[i] : (int)(unsigned char)prompt[i];
        double c0 = now_s();
        logits = lm_forward(&m, &st, tok, st.pos);
        double c1 = now_s();
        if (t_first < 0.0) t_first = c1 - c0; else t_rest += c1 - c0;
        if (!logits) {
            printf("lm: lm_forward refused during prompt fill at byte %lu "
                   "(pos/seq_len mismatch -- this is a bug in lm.c, not the model)\n",
                   (unsigned long)i);
            lm_state_free(&st);
            lm_close(&m);
            release_blob(blob, (size_t)len, mapped, fd);
            return 1;
        }
        logit_scan(logits, vocab, &lo_min, &lo_max, &lo_nonfinite, &lo_rows);
        steps_run++;
    }
    /* Nothing is echoed in --ids mode: the ids the host sent are the ids the
     * host already has, and this program cannot turn them back into text
     * without the tokenizer it deliberately does not carry. Printing the raw
     * argv string instead would put a comma-separated integer list in the
     * middle of the generated output, where a reader would take it for text
     * the model produced. */
    if (!ids_arg) fputs(prompt, stdout);

    /* --dump-logits: THE LOGIT ROW AFTER THE LAST PROMPT TOKEN, raw f32.
     *
     * This is the file the whole "is it really Qwen" question is settled on.
     * A generated SAMPLE cannot settle it -- a wrong rope base, a transposed
     * matrix and a wrong tokenizer all produce confident, grammatical, wrong
     * text -- so the comparison against PyTorch is made on the logits, where
     * the two sides either agree to the quantiser's error or they do not.
     *
     * Raw little-endian f32, `vocab` of them, no header: the reader is
     * numpy.fromfile on a machine that is x86-64 on both ends, and a header
     * would be a second thing to keep in step for no reader's benefit.
     *
     * A FAILED WRITE IS REPORTED AND CHANGES THE EXIT STATUS. An oracle
     * comparison that silently reads a stale file from the previous run is
     * the exact failure CLAUDE.md opens with -- the thing reporting the
     * result was not looking at what the reader assumed. */
    if (dump_logits) {
        if (!logits) {
            printf("lm: --dump-logits: nothing has run (an empty prompt has "
                   "no logit row), so there is nothing to dump\n");
            lm_state_free(&st); lm_close(&m);
            release_blob(blob, (size_t)len, mapped, fd); return 1;
        }
        FILE *lf = fopen(dump_logits, "wb");
        size_t wrote = lf ? fwrite(logits, sizeof(float), (size_t)vocab, lf) : 0;
        if (lf) fclose(lf);
        if (wrote != (size_t)vocab) {
            printf("lm: --dump-logits: wrote %lu of %d floats to %s\n",
                   (unsigned long)wrote, vocab, dump_logits);
            lm_state_free(&st); lm_close(&m);
            release_blob(blob, (size_t)len, mapped, fd); return 1;
        }
        printf("lm: dumped %d logits after prompt position %d to %s\n",
               vocab, st.pos - 1, dump_logits);
    }

    /* -------------------------------------------------------- generate -- */
    int next_tok = 0;
    if (logits) {
        next_tok = greedy ? lm_sample_greedy(logits, vocab)
                           : lm_sample_topp((float *)logits, vocab,
                                             (float)temperature, topp, &rng);
    } else {
        /* Empty prompt: nothing has run yet, so there is no logits row to
         * sample from. Seed generation with byte 0 at position 0 -- this is
         * the same "start of sequence" convention a byte-level model with no
         * reserved BOS token has to pick SOME token to open on, and it is
         * consistent with the KV cache holding nothing yet. */
        next_tok = 0;
    }

    int produced = 0;
    while (produced < n_predict && st.pos < seq_len) {
        double c0 = now_s();
        logits = lm_forward(&m, &st, next_tok, st.pos);
        double c1 = now_s();
        if (t_first < 0.0) t_first = c1 - c0; else t_rest += c1 - c0;
        if (!logits) {
            printf("\nlm: lm_forward refused during generation at step %d "
                   "(pos/seq_len mismatch)\n", produced);
            break;
        }
        logit_scan(logits, vocab, &lo_min, &lo_max, &lo_nonfinite, &lo_rows);
        steps_run++;
        if (print_ids) printf("%d ", next_tok);
        else           putchar(next_tok);
        produced++;
        next_tok = greedy ? lm_sample_greedy(logits, vocab)
                           : lm_sample_topp((float *)logits, vocab,
                                             (float)temperature, topp, &rng);
    }
    putchar('\n');

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed_s = (double)(t1.tv_sec - t0.tv_sec)
                      + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    /* The clock's granularity is 10 ms (c/apps/libc/src/time.c); a run under
     * that is not a throughput measurement, it is noise, so say so rather
     * than print a tokens/s built on a near-zero denominator. */
    /* THE BREAKDOWN LINES ("first token" / "steady state") PRINT AFTER THE
     * SUMMARY LINE, NOT BEFORE -- again because of where
     * tests/boot/run-lm-test.sh's compare.py ends the region it holds to byte
     * equality: at the START of the first "lm: N forward steps" line
     * following "KiB total". Printing the timing breakdown first would put
     * two more lines of WALL-CLOCK numbers -- which differ between the host
     * and a TCG-emulated device by construction, that is the whole point of
     * measuring them -- inside a region the harness expects to be identical
     * prompt-and-generated bytes. Emitted after, they are outside every
     * region the harness ever looks at, on every run in a multi-run session. */
    if (elapsed_s < 0.01) {
        printf("lm: %ld forward steps in %.3f s -- too short to time at this "
               "clock's 10 ms granularity; run a longer -n\n",
               steps_run, elapsed_s);
    } else {
        printf("lm: %ld forward steps (%lu prompt + %d generated) in %.3f s "
               "= %.2f tokens/s\n",
               steps_run, (unsigned long)prompt_len, produced, elapsed_s,
               (double)steps_run / elapsed_s);
        if (steps_run >= 1 && t_first >= 0.0)
            printf("lm: first token    %.3f s%s\n", t_first,
                   mapped ? "  (arithmetic + faulting the touched weights in)"
                          : "  (arithmetic only -- the model was already resident)");
        if (steps_run >= 2 && t_rest > 0.0)
            printf("lm: steady state   %ld tokens in %.3f s -> %.2f tok/s\n",
                   steps_run - 1, t_rest, (double)(steps_run - 1) / t_rest);
    }

    /* THE ARITHMETIC'S OWN VERDICT, printed on every run including the ones
     * that were too short to time -- a run that produced NaN is exactly the
     * run whose timing does not matter. Outside every region
     * run-lm-test.sh compares, for the reason given above the summary line.
     *
     * `nonfinite 0` is the whole claim. The range is context for a reader:
     * an f32 logit from this arithmetic is a sum of `dim` products of
     * O(1)-scaled values, so single digits to low tens is what a healthy
     * 28-layer pass produces, and 1e30 means saturation is under way even
     * though nothing is formally infinite yet. Neither bound is enforced
     * here, because "what a logit should be" for a model of random weights
     * is not something this program can derive -- only the non-finite count
     * is a fact rather than an expectation, so only it is stated as one. */
    printf("lm: logits         %ld rows x %d, range [%.4f, %.4f], "
           "non-finite %ld%s\n",
           lo_rows, vocab, (double)lo_min, (double)lo_max, lo_nonfinite,
           lo_nonfinite ? "  <-- the forward pass produced NaN or Inf; every "
                          "byte sampled after the first one is meaningless"
                        : "  (no NaN/Inf anywhere in the run)");

    mm_mark("after generation, before anything is freed");

    lm_state_free(&st);
    lm_close(&m);
    release_blob(blob, (size_t)len, mapped, fd);
    return 0;
}
