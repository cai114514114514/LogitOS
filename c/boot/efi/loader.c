/* c/boot/efi/loader.c -- LogitOS's own UEFI boot loader.
 *
 * ============================ WHAT THIS IS ============================
 * THIS LOADER IMPERSONATES GRUB. That sentence is the whole design.
 *
 * The kernel's boot contract is Multiboot2 and it is not renegotiated by this
 * milestone: c/boot/boot.asm:38 compares eax against 0x36D76289, and every
 * consumer in the kernel walks the tag list that ebx points at. So rather than
 * teach the kernel a second way to be booted -- two entry paths, two sets of
 * assumptions, and a permanent obligation to test both -- this loader gathers
 * from UEFI exactly what GRUB gathers from BIOS, forges the same information
 * block, gives the CPU back the way the firmware handed it over (32-bit
 * protected mode, paging off), and jumps to the same address GRUB jumps to.
 *
 *   ONE KERNEL, TWO LOADERS. The kernel does not change in this milestone.
 *
 * The three translations, each with its consumer named -- because the consumer
 * is the authority for the layout, not the spec (see mb2.h):
 *
 *   GOP mode                -> mb2 framebuffer tag  (8)  -> c/kernel/gui/fb.c
 *   EFI memory map          -> mb2 mmap tag         (6)  -> c/kernel/mm/pmm.c
 *   EFI config-table RSDP   -> mb2 ACPI tag        (15)  -> nobody yet
 *
 * ================= THE ONE THING GRUB NEVER HAS TO DO =================
 * GRUB on BIOS loads this kernel straight to its link address, because BIOS
 * hands over a machine where 1 MiB upwards is simply free. UEFI does not: the
 * firmware owns the machine until ExitBootServices and has its own data in low
 * memory. On the firmware this gate runs on, the kernel's 11.5 MiB image does
 * not fit in the 7 MiB that is free down there.
 *
 * So placement has two paths, and the audit above destination_is_takeable()
 * carries the full argument for both:
 *
 *   DIRECT  AllocatePages(AllocateAddress) succeeds -> the image goes straight
 *           to 1 MiB and nothing happens after ExitBootServices but the jump.
 *           This is what the milestone was planned around and what real
 *           firmware, which keeps its reservations high, generally allows.
 *
 *   STAGED  it fails -> ask the question that actually matters, which is not
 *           "may I have this now" but "is this MINE ONCE BOOT SERVICES ARE
 *           GONE". Most of what blocks the address is BootServicesData, which
 *           by definition stops existing at EBS. If every byte qualifies, the
 *           finished image is built in a staging buffer and installed by
 *           trampoline.S after EBS -- in assembly, with no stack, because the
 *           destination overlaps the UEFI stack itself.
 *
 * A refusal remains a refusal: memory that never becomes the OS's (runtime
 * services, the ACPI tables, MMIO) is not taken, and this kernel is not
 * relocatable, so there is nowhere else to put it.
 *
 * ======================= THE KNOWN GAP, ON PURPOSE ====================
 * c/kernel/cpu/acpi.c finds the RSDP by scanning the BIOS EBDA/ROM area, which
 * under UEFI holds nothing. So the first UEFI boot comes up with NO ACPI: one
 * CPU, no FADT. That is accepted for this milestone and the gate asserts a boot
 * WITHOUT SMP. The tag is emitted here anyway -- carrying it now means the
 * ~20-line acpi.c patch (c/boot/efi/acpi-mb2-tag.patch, deferred because
 * another line owns that file right now) lands against a loader that already
 * provides what it reads.
 *
 * ============================ HOUSE RULES =============================
 * REFUSALS ARE LOUD. Every failure path prints WHY on both ConOut and serial
 * and then stalls forever. There is no path in this file that jumps to an
 * address it did not verify: a loader that guesses produces a machine that dies
 * somewhere else entirely, hours of debugging away from the actual mistake.
 *
 * SERIAL IS THE REAL CHANNEL. ConOut is a firmware protocol; ExitBootServices
 * frees the code behind it, so every message from that point on goes to COM1 by
 * raw port I/O only. There is a second, earlier deadline on ConOut as well --
 * see the map-key comment in efi_main -- so the two output paths are kept
 * separate throughout: `sput*` is serial alone, `say*` is both.
 */

#include "efi.h"
#include "mb2.h"

/* L"..." must be UCS-2 for every string handed to a firmware protocol. It is,
 * because the build targets x86_64-unknown-windows where wchar_t is 16-bit --
 * but that is a property of the triple in build.sh, so it gets checked here
 * rather than assumed. sizeof(L"a") is two characters plus the terminator. */
_Static_assert(sizeof(L"a") == 4, "CHAR16 literals must be 16-bit (check the target triple)");

/* ------------------------------------------------------------------ *
 *  freestanding basics                                               *
 * ------------------------------------------------------------------ */

/* The C library does not exist here, and -ffreestanding does NOT stop LLVM's
 * loop-idiom pass from recognising a byte loop and rewriting it into a call to
 * memset()/memcpy() -- which would then be an undefined symbol at link time.
 * The volatile destination pointer is what forbids that rewrite. This is the
 * same device c/lib/gfx's gfx_zero() uses and for the same reason (CLAUDE.md,
 * "Open Logit": "whose volatile pointer is what forbids -O2 rewriting it into
 * a call to memset").
 *
 * The 8-bytes-at-a-time middle is not premature: this is what clears the
 * kernel's ~12 MiB image span, and under TCG a byte-at-a-time loop over that
 * is seconds of black screen that looks exactly like a hang. */
static void mzero(void *dst, UINT64 n)
{
    volatile UINT8 *d = (volatile UINT8 *)dst;
    while (n && ((UINTN)d & 7)) { *d++ = 0; n--; }
    {
        volatile UINT64 *q = (volatile UINT64 *)d;
        while (n >= 8) { *q++ = 0; n -= 8; }
        d = (volatile UINT8 *)q;
    }
    while (n--) *d++ = 0;
}

static void mcopy(void *dst, const void *src, UINT64 n)
{
    volatile UINT8 *d = (volatile UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;
    while (n--) *d++ = *s++;
}

static int meq(const void *a, const void *b, UINT64 n)
{
    const UINT8 *x = (const UINT8 *)a, *y = (const UINT8 *)b;
    while (n--) if (*x++ != *y++) return 0;
    return 1;
}

static UINT64 slen(const char *s) { UINT64 n = 0; while (s[n]) n++; return n; }

/* ------------------------------------------------------------------ *
 *  COM1, by raw port I/O                                             *
 * ------------------------------------------------------------------ */

/* Byte-for-byte the same line settings as the kernel's own driver
 * (c/drivers/char/serial.c serial_init: divisor 3 => 38400 baud, 8N1). QEMU
 * does not care; a real machine does, and a mismatch would show up as the
 * loader's banner arriving clean and the kernel's first line arriving as
 * garbage -- a symptom that looks like a kernel fault and is not one. */
#define COM1 0x3F8

static inline void outb(UINT16 port, UINT8 val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline UINT8 inb(UINT16 port)
{
    UINT8 v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void serial_init(void)
{
    outb(COM1 + 1, 0x00);   /* interrupts off */
    outb(COM1 + 3, 0x80);   /* DLAB */
    outb(COM1 + 0, 0x03);   /* divisor low  = 3 -> 38400 baud */
    outb(COM1 + 1, 0x00);   /* divisor high */
    outb(COM1 + 3, 0x03);   /* 8 bits, no parity, 1 stop */
    outb(COM1 + 2, 0xC7);   /* FIFO on + cleared, 14-byte trigger */
    outb(COM1 + 4, 0x0B);   /* DTR/RTS/OUT2 */
}

static void sputc(char c)
{
    /* Bounded, exactly as serial.c is: a machine with no UART must not turn a
     * diagnostic into a hang, because the diagnostic is what we are here for. */
    for (long spins = 0; spins < 1000000; spins++)
        if (inb(COM1 + 5) & 0x20) { outb(COM1, (UINT8)c); return; }
}

static void sputs(const char *s)
{
    while (*s) { if (*s == '\n') sputc('\r'); sputc(*s++); }
}

/* ------------------------------------------------------------------ *
 *  number formatting, shared by both output paths                     *
 * ------------------------------------------------------------------ */

/* Formatted backwards into the caller's buffer and returned as a pointer into
 * it, so the same digits can go to serial alone or to serial and ConOut
 * without a second implementation and without a static buffer. */
static const char *fmt_hex(char *buf19, UINT64 v)
{
    int i = 18;
    buf19[i] = 0;
    do { buf19[--i] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
    buf19[--i] = 'x'; buf19[--i] = '0';
    return &buf19[i];
}

static const char *fmt_dec(char *buf21, UINT64 v)
{
    int i = 20;
    buf21[i] = 0;
    do { buf21[--i] = (char)('0' + (v % 10)); v /= 10; } while (v);
    return &buf21[i];
}

static void sputx(UINT64 v) { char b[19]; sputs(fmt_hex(b, v)); }
static void sputd(UINT64 v) { char b[21]; sputs(fmt_dec(b, v)); }

/* ------------------------------------------------------------------ *
 *  globals + the dual output path                                     *
 * ------------------------------------------------------------------ */

static EFI_SYSTEM_TABLE  *ST;
static EFI_BOOT_SERVICES *BS;

/* Set as soon as the memory map has been taken -- which is EARLIER than
 * ExitBootServices, and deliberately so. See the map-key comment in efi_main:
 * a ConOut print can allocate, and an allocation between GetMemoryMap and
 * ExitBootServices invalidates the key. One flag closes both windows. */
static int serial_only;

/* Where the kernel image must end up, and (if the firmware would not let us
 * write there before ExitBootServices) where its finished copy is waiting.
 * install_len == 0 means the image is already in place and the trampoline has
 * nothing to install. Everything allocated after the kernel is loaded is
 * checked against [kernel_lo, kernel_hi) before it is used, because the install
 * overwrites that span without looking. */
static UINT64 kernel_lo, kernel_hi;
static UINT64 install_dst, install_src, install_len;

/* ConOut wants UCS-2 and every string in this file is ASCII, so widen on the
 * way out rather than carrying two copies of every message. 256 CHAR16 is
 * 512 bytes of static, not stack -- worth noting only because the MSVC ABI
 * calls __chkstk for frames over 4096 bytes and this binary has no __chkstk. */
static void cputs(const char *s)
{
    static CHAR16 w[256];
    UINTN i = 0;
    if (serial_only || !ST || !ST->ConOut) return;
    while (*s && i < 254) {
        if (*s == '\n') w[i++] = '\r';
        w[i++] = (CHAR16)(UINT8)*s++;
    }
    w[i] = 0;
    ST->ConOut->OutputString(ST->ConOut, w);
}

/* ------------------------------------------------------------------ *
 *  say(): a LINE at a time, to both channels                          *
 * ------------------------------------------------------------------ *
 * WHY THIS IS BUFFERED, which it did not need to be until it was run.
 * On OVMF the firmware's ConOut is ALSO wired to the serial port, so a message
 * emitted as "[efi] load " + hex + ".." + hex + "\n" -- five say() calls --
 * came out of the port as ten interleaved fragments:
 *
 *     [efi] load [efi] load 0x1000000x100000....
 *
 * which is not a log, it is a puzzle. Assembling the whole line first and
 * emitting it once per channel fixes it: on OVMF each line simply appears
 * twice in a row, and on a machine whose video console is not the serial port
 * it appears once on each, which is what "print on both" was asking for.
 *
 * The buffer is flushed on '\n' and when it fills, so a refusal can never be
 * lost to buffering -- die() ends every message with a newline. */
static char line_buf[240];
static UINTN line_len;

static void line_flush(void)
{
    if (!line_len) return;
    line_buf[line_len] = 0;
    line_len = 0;
    sputs(line_buf);
    cputs(line_buf);
}

static void say(const char *s)
{
    while (*s) {
        line_buf[line_len++] = *s;
        if (*s == '\n' || line_len >= sizeof line_buf - 2) line_flush();
        s++;
    }
}

static void sayx(UINT64 v) { char b[19]; say(fmt_hex(b, v)); }
static void sayd(UINT64 v) { char b[21]; say(fmt_dec(b, v)); }

__attribute__((noreturn))
static void halt(void)
{
    for (;;) __asm__ volatile ("cli; hlt");
}

/* A refusal.
 *
 * STALLING IS THE POINT. Every caller has discovered that an assumption the
 * jump depends on is false -- the kernel is missing, the firmware would not
 * give us 1 MiB, ExitBootServices failed. Continuing from any of those means
 * jumping into memory whose contents nobody knows, which produces a triple
 * fault or, worse, a silently wrong machine with no evidence of the cause. A
 * stalled machine with a printed reason is a solved bug. */
__attribute__((noreturn))
static void die(const char *why)
{
    say("\n[efi] REFUSING TO BOOT: "); say(why); say("\n[efi] halted.\n");
    halt();
}

__attribute__((noreturn))
static void die_st(const char *why, EFI_STATUS st)
{
    say("\n[efi] REFUSING TO BOOT: "); say(why);
    say(" (EFI_STATUS "); sayx(st); say(")\n[efi] halted.\n");
    halt();
}

/* ------------------------------------------------------------------ *
 *  ELF64, only the part a loader reads                                *
 * ------------------------------------------------------------------ */

/* NOT in a header. c/kernel/exec/elf.h already owns the basename "elf.h", and
 * the Makefile's INCDIRS is one flat -I list over `find c include -type d`
 * (CLAUDE.md, "Source layout") -- a second elf.h in c/boot/efi would sort
 * before the kernel's and silently replace it for every kernel TU that
 * includes it, failing in a file nobody edited. That trap has already cost
 * this tree twice. Thirty lines of struct stay local instead. */
struct elf64_ehdr {
    UINT8  e_ident[16];
    UINT16 e_type, e_machine;
    UINT32 e_version;
    UINT64 e_entry, e_phoff, e_shoff;
    UINT32 e_flags;
    UINT16 e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct elf64_phdr {
    UINT32 p_type, p_flags;
    UINT64 p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};
/* `readelf -h build/kernel.elf` prints exactly these two numbers ("Size of
 * this header: 64", "Size of program headers: 56"). e_phentsize is checked
 * against the second at runtime too -- a struct that is the wrong size here
 * would stride the program-header table wrongly and load garbage. */
_Static_assert(sizeof(struct elf64_ehdr) == 64, "Elf64_Ehdr is 64 bytes");
_Static_assert(sizeof(struct elf64_phdr) == 56, "Elf64_Phdr is 56 bytes");

#define PT_LOAD    1
#define ET_EXEC    2
#define EM_X86_64  62
#define MAX_PHNUM  64          /* the kernel has 5; 64 is slack, not a guess */

static struct elf64_phdr phdrs[MAX_PHNUM];

/* ------------------------------------------------------------------ *
 *  the Multiboot2 information block, built with a bounds-checked cursor
 * ------------------------------------------------------------------ */

struct infobuf {
    UINT8 *base;
    UINTN  cap;
    UINTN  off;
};

/* Reserve `n` bytes and 8-align the cursor after them (Multiboot2 sec 3.6:
 * tags are 8-byte aligned). Overflow is a refusal, not a truncation: pmm's
 * walk stops at a malformed tag, so a truncated list is a kernel that boots
 * and then reports no memory -- a far worse thing to debug than a loader that
 * stops and says so. */
static void *ib_take(struct infobuf *ib, UINTN n)
{
    UINTN start = ib->off;
    UINTN end   = (start + n + 7) & ~(UINTN)7;
    if (end > ib->cap) die("multiboot2 information block too small");
    ib->off = end;
    mzero(ib->base + start, end - start);      /* including the alignment pad */
    return ib->base + start;
}

/* ------------------------------------------------------------------ *
 *  step 2: find and load the kernel                                   *
 * ------------------------------------------------------------------ */

/* EFI_FILE_PROTOCOL.Read may return fewer bytes than asked for, and some
 * firmware dislikes a single multi-megabyte request, so read in slices and
 * loop. A read that returns zero bytes with no error is a truncated file, and
 * that is a refusal: the alternative is a kernel whose tail is whatever the
 * freshly allocated page happened to hold. */
static void read_exact(EFI_FILE_PROTOCOL *f, void *buf, UINT64 want, const char *what)
{
    UINT8 *p = (UINT8 *)buf;
    while (want) {
        UINTN n = (want > 0x100000) ? 0x100000 : (UINTN)want;   /* 1 MiB slices */
        EFI_STATUS st = f->Read(f, &n, p);
        if (st) die_st(what, st);
        if (n == 0) die(what);
        p += n; want -= n;
    }
}

static void seek(EFI_FILE_PROTOCOL *f, UINT64 pos)
{
    EFI_STATUS st = f->SetPosition(f, pos);
    if (st) die_st("seek in logit.elf failed", st);
}

static const char *efi_type_name(UINT32 t)
{
    switch (t) {
    case EfiReservedMemoryType:      return "Reserved";
    case EfiLoaderCode:              return "LoaderCode";
    case EfiLoaderData:              return "LoaderData";
    case EfiBootServicesCode:        return "BootServicesCode";
    case EfiBootServicesData:        return "BootServicesData";
    case EfiRuntimeServicesCode:     return "RuntimeServicesCode";
    case EfiRuntimeServicesData:     return "RuntimeServicesData";
    case EfiConventionalMemory:      return "Conventional";
    case EfiUnusableMemory:          return "Unusable";
    case EfiACPIReclaimMemory:       return "ACPIReclaim";
    case EfiACPIMemoryNVS:           return "ACPINVS";
    case EfiMemoryMappedIO:          return "MMIO";
    case EfiMemoryMappedIOPortSpace: return "MMIOPort";
    case EfiPalCode:                 return "PalCode";
    case EfiPersistentMemory:        return "Persistent";
    default:                         return "?";
    }
}

/* ================= AUDITING THE DESTINATION, AND WHY ==================
 *
 * The plan of record for this milestone was: AllocatePages(AllocateAddress) at
 * the kernel's link address, and refuse if the firmware says no. That is the
 * right FIRST move and it is still the first move below -- but on the machine
 * this gate runs on it cannot succeed, and the firmware's own memory map says
 * why. The kernel's image spans 0x100000..0xC05000 (11.5 MiB, nearly all of it
 * .bss) and OVMF leaves only 0x109000..0x800000 free down there:
 *
 *     0x100000..0x800000   Conventional        free
 *     0x800000..0x900000   ACPI NVS            firmware
 *     0x900000..0x1780000  BootServicesData    firmware, until EBS
 *
 * Those addresses are FIXED -- measured identical at -m 512M and -m 2048M, so
 * they are not a function of how much RAM the machine has and no amount of it
 * moves them.
 *
 * So there is a second question after "will the firmware give me this address",
 * and it is the one that actually matters: WILL THIS MEMORY BE MINE AFTER
 * ExitBootServices? For most of that span the answer is yes and the firmware
 * simply cannot say so in advance, because AllocateAddress is a question about
 * NOW and BootServicesData stops existing at EBS. That is what this audit
 * answers, per descriptor, with three outcomes:
 *
 *   MINE AFTER EBS -- Conventional, BootServicesCode/Data, LoaderCode/Data.
 *     Exactly the set mb2_type_of() forwards to the kernel as available, which
 *     is not a coincidence: it is the same claim, made to the same authority.
 *
 *   ACPI NVS -- TAKEN, AND SAID OUT LOUD. This is the one judgement call in
 *     the file, so here is the whole argument. EfiACPIMemoryNVS is memory the
 *     firmware asks the OS to preserve across the S1-S3 sleep states. LogitOS
 *     implements no sleep state and cannot resume from one, so the capability
 *     being destroyed is a capability that does not exist. It is NOT the ACPI
 *     tables: on this machine those live at 0x1fb6d000 in EfiACPIReclaimMemory,
 *     near the top of RAM and nowhere near the kernel, and ACPIReclaim is
 *     REFUSED below precisely so the forwarded RSDP keeps pointing at something
 *     real for the deferred acpi.c patch. Independent confirmation that the low
 *     block is only the S3 reservation: booting the same firmware with
 *     `-global ICH9-LPC.disable_s3=1` turns exactly those descriptors into
 *     BootServicesData and nothing else changes.
 *
 *   ANYTHING ELSE -- REFUSED. RuntimeServicesCode/Data are live firmware we
 *     never called SetVirtualAddressMap on; ACPIReclaim holds the tables;
 *     Reserved/MMIO/Unusable/Persistent are not ours to take at all. A hole in
 *     the map is refused too: memory the firmware does not describe is not
 *     memory we may assume is RAM.
 *
 * On firmware that puts its reservations high -- which is most real firmware --
 * none of this fires and the direct path is taken. */
static UINT32 mb2_type_of(UINT32 efi_type);      /* defined with the mmap tag */

static void map_walk_begin(EFI_MEMORY_DESCRIPTOR **m, UINTN *count, UINTN *dsz)
{
    UINTN size = 0, key = 0;
    UINT32 ver = 0;
    *m = 0; *count = 0; *dsz = 0;
    if (BS->GetMemoryMap(&size, 0, &key, dsz, &ver) != EFI_BUFFER_TOO_SMALL) return;
    size += 8192;
    if (BS->AllocatePool(EfiLoaderData, size, (void **)m) != EFI_SUCCESS) return;
    if (BS->GetMemoryMap(&size, *m, &key, dsz, &ver) != EFI_SUCCESS || *dsz == 0) {
        BS->FreePool(*m); *m = 0; return;
    }
    *count = size / *dsz;
}

/* Print every descriptor overlapping [lo,hi). Used by both the audit and the
 * refusal path: "the firmware said no" is a refusal, "the firmware said no
 * because 0x800000..0x900000 is ACPI NVS" is a diagnosis. */
static void report_who_owns(UINT64 lo, UINT64 hi)
{
    EFI_MEMORY_DESCRIPTOR *m; UINTN n, dsz;
    map_walk_begin(&m, &n, &dsz);
    if (!m) return;
    say("[efi] who owns that range, per the firmware's own map:\n");
    for (UINTN i = 0; i < n; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)m + i * dsz);
        UINT64 a = d->PhysicalStart, b = a + d->NumberOfPages * 4096;
        if (b <= lo || a >= hi) continue;
        say("[efi]   "); sayx(a); say(".."); sayx(b);
        say("  ");       say(efi_type_name(d->Type));
        say(d->Type == EfiConventionalMemory ? "  (free)\n" : "  (NOT free)\n");
    }
    BS->FreePool(m);
}

/* Nonzero if every byte of [lo,hi) is ours once boot services are gone. */
static int destination_is_takeable(UINT64 lo, UINT64 hi)
{
    EFI_MEMORY_DESCRIPTOR *m; UINTN n, dsz;
    UINT64 covered = 0, nvs = 0;
    int ok = 1;

    map_walk_begin(&m, &n, &dsz);
    if (!m) { say("[efi] cannot read the memory map to audit the load address\n"); return 0; }

    for (UINTN i = 0; i < n; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)m + i * dsz);
        UINT64 a = d->PhysicalStart, b = a + d->NumberOfPages * 4096;
        if (b <= lo || a >= hi) continue;
        if (a < lo) a = lo;
        if (b > hi) b = hi;
        covered += b - a;

        if (mb2_type_of(d->Type) == MB2_MEM_AVAILABLE) continue;   /* ours at EBS */
        if (d->Type == EfiACPIMemoryNVS) { nvs += b - a; continue; }

        say("[efi] the kernel's load address overlaps "); say(efi_type_name(d->Type));
        say(" at "); sayx(a); say(".."); sayx(b);
        say(" -- that memory never becomes the OS's\n");
        ok = 0;
    }
    BS->FreePool(m);

    if (covered != hi - lo) {
        say("[efi] the firmware's map does not describe all of ");
        sayx(lo); say(".."); sayx(hi);
        say(" -- undescribed memory is not assumed to be RAM\n");
        ok = 0;
    }
    if (ok && nvs) {
        /* Said every boot, not hidden in a comment: this is a real casualty. */
        say("[efi] NOTE: taking "); sayd(nvs >> 10);
        say(" KiB of ACPI NVS inside the kernel image. LogitOS implements no\n"
            "[efi]       sleep state, so the S3 resume data being destroyed is\n"
            "[efi]       for a capability that does not exist. The ACPI TABLES\n"
            "[efi]       are in ACPIReclaim elsewhere and are refused, not taken.\n");
    }
    return ok;
}

/* [a,b) and [c,d) intersect. Used for every "is this buffer in the way" check
 * below -- and all of them exist because the post-ExitBootServices install
 * overwrites the destination unconditionally, so anything still live that
 * happens to sit there is destroyed WHILE IT IS BEING USED. */
static int overlaps(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{
    return a < d && c < b;
}

/* Returns the kernel entry point. */
static UINT64 load_kernel(EFI_HANDLE image)
{
    EFI_GUID li_guid  = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID fi_guid  = EFI_FILE_INFO_ID;
    EFI_LOADED_IMAGE_PROTOCOL *li = 0;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
    EFI_FILE_PROTOCOL *root = 0, *file = 0;
    EFI_STATUS st;

    /* The road from "I am running" to "here is the volume I came from":
     * LoadedImage.DeviceHandle is that volume's handle and SimpleFileSystem
     * hangs off it (UEFI 2.10 sec 9.1). Going through the image we were
     * actually loaded from -- rather than hunting every filesystem in the
     * machine for a logit.elf -- is what makes booting from a USB stick pick
     * up THAT stick's kernel and not the one on the internal disk. */
    st = BS->HandleProtocol(image, &li_guid, (void **)&li);
    if (st) die_st("no LoadedImageProtocol on our own image handle", st);

    /* ============ THE ASSUMPTION THE WHOLE DESCENT RESTS ON =============
     * trampoline.S clears CR0.PG while executing out of THIS image, which is
     * legal only if the instruction doing it is identity-mapped (SDM Vol 3A
     * sec 9.8.5.4). UEFI identity-maps everything in its memory map (UEFI 2.10
     * sec 2.3.4), so that holds -- provided the image is below 4 GiB, because
     * the moment paging goes off the address space is 32 bits wide and the
     * GDTR base loaded from a 64-bit pointer is truncated.
     *
     * Firmware relocates a PE image wherever it likes. In practice that is low
     * memory and this check has never fired; it is here because the failure it
     * prevents is a machine that resets with the firmware logo still on screen
     * and nothing on the wire. Verified, not assumed. */
    UINT64 img_end = (UINT64)(UINTN)li->ImageBase + li->ImageSize;
    if (img_end > 0xFFFFFFFFULL || (UINT64)(UINTN)&mzero > 0xFFFFFFFFULL) {
        say("[efi] loader image at "); sayx((UINT64)(UINTN)li->ImageBase);
        say(" size "); sayx(li->ImageSize); say("\n");
        die("firmware placed this loader above 4 GiB; the 32-bit descent "
            "cannot run from there");
    }
    say("[efi] image "); sayx((UINT64)(UINTN)li->ImageBase);
    say(".."); sayx(img_end); say("\n");

    st = BS->HandleProtocol(li->DeviceHandle, &sfs_guid, (void **)&fs);
    if (st) die_st("boot volume has no SimpleFileSystem (is the ESP FAT?)", st);

    st = fs->OpenVolume(fs, &root);
    if (st) die_st("OpenVolume on the boot volume failed", st);

    /* The ESP layout this expects is the one tools/mkesp.py builds:
     *   /EFI/BOOT/BOOTX64.EFI   this program, at the removable-media default
     *                           path (UEFI 2.10 sec 3.5.1.1) so the firmware
     *                           boots it with no NVRAM entry to install
     *   /logit.elf              the kernel, unmodified -- byte for byte the
     *                           same file the GRUB ISO carries */
    /* Both spellings are tried. FAT is case-insensitive and EFI_FILE_PROTOCOL
     * .Open inherits that, so one lookup SHOULD be enough -- but tools/mkesp.py
     * writes the kernel under the 8.3 name LOGIT.ELF, and a firmware whose FAT
     * driver compares case-sensitively would turn that detail into a black
     * screen. One extra call is cheaper than the bug report. */
    st = root->Open(root, &file, L"\\logit.elf", EFI_FILE_MODE_READ, 0);
    if (st) st = root->Open(root, &file, L"\\LOGIT.ELF", EFI_FILE_MODE_READ, 0);
    if (st) die_st("cannot open \\logit.elf on the boot volume", st);

    /* Size it, only to report it: the segments are read at their own offsets
     * and lengths, so the file size is a breadcrumb and not a load parameter.
     * The buffer is UINT64[] rather than UINT8[] because EFI_FILE_INFO's first
     * member is a UINT64 and a byte array has no alignment guarantee. */
    {
        UINT64 raw[(sizeof(EFI_FILE_INFO) + 64 + 7) / 8];
        UINTN n = sizeof raw;
        st = file->GetInfo(file, &fi_guid, &n, raw);
        if (!st) {
            say("[efi] kernel logit.elf ");
            sayd(((EFI_FILE_INFO *)raw)->FileSize);
            say(" bytes\n");
        }
    }

    struct elf64_ehdr eh;
    seek(file, 0);
    read_exact(file, &eh, sizeof eh, "short read of the ELF header");

    if (!(eh.e_ident[0] == 0x7F && eh.e_ident[1] == 'E' &&
          eh.e_ident[2] == 'L'  && eh.e_ident[3] == 'F'))
        die("logit.elf is not an ELF file");
    if (eh.e_ident[4] != 2)        die("logit.elf is not ELF64 (EI_CLASS)");
    if (eh.e_ident[5] != 1)        die("logit.elf is not little-endian (EI_DATA)");
    if (eh.e_type != ET_EXEC)      die("logit.elf is not ET_EXEC");
    if (eh.e_machine != EM_X86_64) die("logit.elf is not x86-64");
    if (eh.e_phentsize != sizeof(struct elf64_phdr))
        die("logit.elf e_phentsize is not 56");
    if (eh.e_phnum == 0 || eh.e_phnum > MAX_PHNUM)
        die("logit.elf has an implausible program-header count");

    seek(file, eh.e_phoff);
    read_exact(file, phdrs, (UINT64)eh.e_phnum * sizeof(struct elf64_phdr),
               "short read of the program headers");

    /* Span of the image in PHYSICAL memory. p_paddr, not p_vaddr: the kernel is
     * linked to run identity-mapped at 1 MiB so the two are equal today, but
     * the physical address is the field that answers "where does this go", and
     * reading the other one would be right only by luck. */
    UINT64 lo = ~(UINT64)0, hi = 0;
    for (UINT16 i = 0; i < eh.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD || phdrs[i].p_memsz == 0) continue;
        if (phdrs[i].p_filesz > phdrs[i].p_memsz)
            die("a PT_LOAD segment has filesz > memsz");
        UINT64 a = phdrs[i].p_paddr, b = phdrs[i].p_paddr + phdrs[i].p_memsz;
        if (b < a) die("a PT_LOAD segment wraps the address space");
        if (a < lo) lo = a;
        if (b > hi) hi = b;
    }
    if (lo == ~(UINT64)0) die("logit.elf has no PT_LOAD segments");
    if (hi > 0xFFFFFFFFULL) die("the kernel image does not fit below 4 GiB");

    UINT64 page_lo = lo & ~(UINT64)0xFFF;
    UINT64 page_hi = (hi + 0xFFF) & ~(UINT64)0xFFF;
    UINTN  pages   = (UINTN)((page_hi - page_lo) >> 12);
    kernel_lo = page_lo;
    kernel_hi = page_hi;

    /* ==================== THE TRAP THIS STEP EXISTS FOR ====================
     * The kernel links at 1 MiB and UEFI may own that memory -- it is the
     * firmware's machine until ExitBootServices, and nothing entitles us to
     * any particular address. So claim the WHOLE span first, in one call, with
     * AllocateAddress (UEFI 2.10 sec 7.2, "allocates pages at a specified
     * address"), and treat a refusal as final.
     *
     * WHY REFUSING IS RIGHT and not "load it somewhere else": a kernel written
     * over the firmware's own data structures does not fail here, it fails
     * later and elsewhere -- inside a firmware callback, or after the jump,
     * with nothing pointing back at this line. OVMF leaves low RAM free so this
     * succeeds on the machine the gate runs on; real firmware may not, and the
     * honest behaviour until the kernel can be relocated is to say so.
     *
     * KNOWN FUTURE WORK, named rather than hidden: the fix is a relocatable
     * kernel (a Multiboot2 relocatable header tag, sec 3.1.10, plus
     * position-independent early boot code), at which point this becomes
     * AllocateAnyPages. That is a kernel change, and this milestone changes no
     * kernel. */
    /* The loader must not be standing on the kernel. Nothing later checks this
     * for us: in the staged path below, the install overwrites the destination
     * unconditionally, so a loader image inside the span would be erased while
     * executing. (This is not hypothetical -- the first build of this file used
     * /base:0x100000 and OVMF honoured it exactly.) */
    if (overlaps((UINT64)(UINTN)li->ImageBase, img_end, page_lo, page_hi))
        die("this loader is loaded on top of the kernel's link address "
            "(change /base in c/boot/efi/build.sh)");

    EFI_PHYSICAL_ADDRESS at = page_lo;
    UINT64 dest = page_lo;      /* where the segments get written RIGHT NOW */

    st = BS->AllocatePages(AllocateAddress, EfiLoaderData, pages, &at);
    if (st == EFI_SUCCESS) {
        /* PATH 1, the simple one and the one real firmware usually allows: the
         * kernel goes straight to its link address and nothing happens after
         * ExitBootServices except the jump.
         *
         * NOT EXERCISED BY THE GATE, and worth knowing: this OVMF build's low
         * reservations are at fixed physical addresses (measured identical at
         * -m 512M and -m 2048M, and on both -machine q35 and -machine pc), so
         * the 11.5 MiB image can never fit the 7 MiB hole and this branch is
         * structurally unreachable here. It is the strictly SIMPLER path --
         * install_len stays 0 and trampoline.S skips the copy on a single
         * testq/jz -- so what goes untested is a subset of what is tested, not
         * a parallel implementation. */
        say("[efi] load direct "); sayx(page_lo); say(".."); sayx(page_hi); say("\n");
    } else {
        /* PATH 2. The firmware owns part of the span NOW. That is not the same
         * as owning it after ExitBootServices -- see the audit's header comment
         * -- so ask the real question before giving up. */
        say("[efi] link address unavailable now ("); sayd(pages);
        say(" pages at "); sayx(page_lo); say("): asking whether it becomes ours\n");
        report_who_owns(page_lo, page_hi);

        if (!destination_is_takeable(page_lo, page_hi))
            die_st("the kernel's link address holds memory that never becomes "
                   "the OS's -- this kernel is not relocatable, so there is "
                   "nowhere else to put it", st);

        /* Build the finished image in a staging buffer and install it after
         * ExitBootServices, when the firmware's claim on the destination has
         * lapsed. Staged as ONE flat span rather than a list of segments so the
         * post-EBS work is a single copy with no structure to walk -- see
         * mb2_handoff, which does it with no stack and no memory reference
         * outside the two buffers. */
        EFI_PHYSICAL_ADDRESS stage = 0xFFFFFFFFULL;    /* keep it below 4 GiB */
        st = BS->AllocatePages(AllocateMaxAddress, EfiLoaderData, pages, &stage);
        if (st) die_st("no staging buffer for the kernel image", st);
        if (overlaps(stage, stage + (page_hi - page_lo), page_lo, page_hi))
            die("the staging buffer landed inside the kernel's own destination");

        dest        = stage;
        install_dst = page_lo;
        install_src = stage;
        install_len = page_hi - page_lo;

        say("[efi] load staged "); sayx(stage);
        say(" -> ");               sayx(page_lo);
        say(" ("); sayd(install_len >> 20); say(" MiB, installed after EBS)\n");
    }

    /* Zero the ENTIRE span before loading, rather than each segment's
     * (memsz - filesz) tail. Two things fall out of one clear: .bss is zero as
     * the kernel's C requires, and so is every alignment gap BETWEEN segments
     * -- which no per-segment loop covers, and which AllocatePages does not
     * promise to hand over clean. In the staged path this also means the buffer
     * copied later IS the finished image, byte for byte. */
    mzero((void *)(UINTN)dest, page_hi - page_lo);

    for (UINT16 i = 0; i < eh.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD || phdrs[i].p_memsz == 0) continue;
        if (phdrs[i].p_filesz) {
            seek(file, phdrs[i].p_offset);
            read_exact(file,
                       (void *)(UINTN)(dest + (phdrs[i].p_paddr - page_lo)),
                       phdrs[i].p_filesz, "short read of a PT_LOAD segment");
        }
    }
    file->Close(file);
    root->Close(root);

    /* The entry point is the ELF's e_entry, because that is what GRUB uses:
     * c/boot/multiboot2.asm carries only a framebuffer request tag and the end
     * tag -- no address tag and no entry-address tag -- and Multiboot2 sec
     * 3.1.5 makes e_entry the entry point in exactly that case. */
    if (eh.e_entry < lo || eh.e_entry >= hi)
        die("the ELF entry point is outside the loaded image");

    say("[efi] entry "); sayx(eh.e_entry); say("\n");
    return eh.e_entry;
}

/* ------------------------------------------------------------------ *
 *  step 3: the framebuffer                                            *
 * ------------------------------------------------------------------ */

/* Bit position and width of a mask, for PixelBitMask modes. */
static void mask_field(UINT32 mask, UINT8 *pos, UINT8 *size)
{
    UINT8 p = 0, n = 0;
    if (!mask) { *pos = 0; *size = 0; return; }
    while (!(mask & 1)) { mask >>= 1; p++; }
    while (mask & 1)    { mask >>= 1; n++; }
    *pos = p; *size = n;
}

/* Emits the mb2 framebuffer tag, or emits nothing and says why. Emitting
 * nothing is a SUPPORTED outcome, not a failure: c/boot/multiboot2.asm already
 * marks its framebuffer request optional because `-vga none` means GRUB has
 * none either, and fb_init() then falls back to driving virtio-gpu itself
 * (fb.c:131). Refusing to boot over a missing framebuffer would be stricter
 * than the loader we are impersonating. */
static void build_fb_tag(struct infobuf *ib)
{
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    EFI_STATUS st = BS->LocateProtocol(&gop_guid, 0, (void **)&gop);
    if (st || !gop || !gop->Mode || !gop->Mode->Info) {
        say("[efi] gop none (kernel will drive its own display)\n");
        return;
    }

    /* v1 takes the mode the firmware already set and does not call SetMode.
     * Mode-setting is a separate risk with its own failure modes (a mode the
     * panel cannot show is a black screen on a running machine), and OVMF's
     * default is a perfectly good linear framebuffer. The kernel picks its UI
     * scale from whatever size it is handed (fb.c pick_scale), so there is
     * nothing to gain here and a boot to lose. */
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = gop->Mode->Info;

    UINT8 rp = 0, rs = 0, gp = 0, gs = 0, bp = 0, bs = 0;
    switch (mi->PixelFormat) {
    /* UEFI 2.10 sec 12.9: these two names describe the byte order IN MEMORY.
     * On a little-endian machine the first byte is the least significant, so
     * "RedGreenBlueReserved" puts red at bit 0 and "BlueGreenRedReserved" puts
     * blue there. The second is what QEMU/OVMF and essentially all real
     * hardware report, and it is also what the kernel's own virtio-gpu path
     * already uses (fb.c:134 -- rpos 16, gpos 8, bpos 0). */
    case PixelRedGreenBlueReserved8BitPerColor:
        rp = 0;  gp = 8;  bp = 16; rs = gs = bs = 8; break;
    case PixelBlueGreenRedReserved8BitPerColor:
        bp = 0;  gp = 8;  rp = 16; rs = gs = bs = 8; break;
    case PixelBitMask:
        mask_field(mi->PixelInformation.RedMask,   &rp, &rs);
        mask_field(mi->PixelInformation.GreenMask, &gp, &gs);
        mask_field(mi->PixelInformation.BlueMask,  &bp, &bs);
        break;
    default:
        /* PixelBltOnly: there is no linear framebuffer at all, only Blt(), and
         * Blt() dies with boot services. A tag here would hand fb.c a pointer
         * to page zero. */
        say("[efi] gop is PixelBltOnly, no linear framebuffer -- no fb tag\n");
        return;
    }
    if (rs != 8 || gs != 8 || bs != 8) {
        say("[efi] gop is not 8 bits per channel -- no fb tag (fb.c wants 32bpp)\n");
        return;
    }

    struct mb2_tag_framebuffer *t = ib_take(ib, 38);
    t->type   = MB2_TAG_FRAMEBUFFER;
    t->size   = 38;                     /* 32 of header+geometry, 6 of colour */
    t->addr   = gop->Mode->FrameBufferBase;
    /* PixelsPerScanLine is in PIXELS and may exceed the visible width (the
     * scanline is padded); fb.c's `pitch` is in BYTES -- it strides fb_mem with
     * it. Multiplying by 4 is the whole conversion, and getting it wrong is the
     * classic sheared-image bug. */
    t->pitch  = mi->PixelsPerScanLine * 4;
    t->width  = mi->HorizontalResolution;
    t->height = mi->VerticalResolution;
    t->bpp     = 32;                    /* fb.c:157 requires exactly this */
    t->fb_type = MB2_FB_TYPE_RGB;       /* fb.c:157 requires exactly this */
    t->red_pos   = rp; t->red_size   = 8;
    t->green_pos = gp; t->green_size = 8;
    t->blue_pos  = bp; t->blue_size  = 8;

    say("[efi] gop ");
    sayd(t->width); say("x"); sayd(t->height);
    say(rp == 16 ? " bgr" : (rp == 0 ? " rgb" : " mask"));
    say(" pitch "); sayd(t->pitch);
    say(" fb ");    sayx(t->addr);
    say("\n");
}

/* ------------------------------------------------------------------ *
 *  step 5: the RSDP                                                   *
 * ------------------------------------------------------------------ */

static void build_acpi_tag(struct infobuf *ib)
{
    EFI_GUID g2 = EFI_ACPI_20_TABLE_GUID;
    EFI_GUID g1 = EFI_ACPI_10_TABLE_GUID;
    void *rsdp2 = 0, *rsdp1 = 0;

    /* Under UEFI the RSDP is not findable by scanning: there is no EBDA and the
     * ROM area holds nothing. The configuration table IS the mechanism (UEFI
     * 2.10 sec 4.6), which is why acpi.c's BIOS scan comes up empty and why the
     * deferred patch exists. */
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *e = &ST->ConfigurationTable[i];
        if (meq(&e->VendorGuid, &g2, sizeof(EFI_GUID))) rsdp2 = e->VendorTable;
        if (meq(&e->VendorGuid, &g1, sizeof(EFI_GUID))) rsdp1 = e->VendorTable;
    }

    /* ACPI 2.0+ first: its RSDP carries the XSDT with 64-bit table pointers,
     * which is what anything made this century populates. */
    void *rsdp = rsdp2 ? rsdp2 : rsdp1;
    if (!rsdp) { say("[efi] rsdp none in the configuration table\n"); return; }

    if (!meq(rsdp, "RSD PTR ", 8)) {          /* ACPI 6.5 sec 5.2.5.3 */
        say("[efi] rsdp signature is wrong -- refusing to forward it\n");
        return;
    }

    /* A v1 RSDP is exactly 20 bytes and has no length field; a v2+ one carries
     * its own length at offset 20 (ACPI 6.5 table 5.3). COPY, do not point: the
     * firmware's copy sits in EfiACPIReclaimMemory, which is the memory type
     * whose name says a kernel is entitled to reuse it. */
    UINT32 len = 20, type = MB2_TAG_ACPI_OLD;
    if (rsdp2) {
        UINT32 l;
        mcopy(&l, (UINT8 *)rsdp + 20, 4);
        if (l < 20 || l > 4096)               /* implausible: keep the v1 shape */
            say("[efi] rsdp length is implausible -- forwarding 20 bytes only\n");
        else { len = l; type = MB2_TAG_ACPI_NEW; }
    }

    struct mb2_tag_acpi *t = ib_take(ib, 8 + len);
    t->type = type;
    t->size = 8 + len;
    mcopy(t->rsdp, rsdp, len);

    say("[efi] rsdp ");
    say(type == MB2_TAG_ACPI_NEW ? "acpi2 " : "acpi1 ");
    sayx((UINT64)(UINTN)rsdp);
    say(" len ");        sayd(len);
    say(" -> mb2 tag "); sayd(type);
    say("\n");
}

/* ------------------------------------------------------------------ *
 *  step 4: the memory map                                             *
 * ------------------------------------------------------------------ */

/* THE MAPPING IS DELIBERATELY CONSERVATIVE, and the asymmetry is the reason:
 * a page wrongly marked available is handed to the allocator and corrupts the
 * machine somewhere far away from here; a page wrongly marked reserved merely
 * goes unused. When in doubt, reserved.
 *
 * The three kinds that ARE available once boot services are gone:
 *   EfiConventionalMemory     -- free RAM, uncontroversial.
 *   EfiBootServicesCode/Data  -- the firmware's boot-time code and heap. UEFI
 *     2.10 sec 7.4 hands these to the OS at ExitBootServices, and refusing them
 *     would give up a large fraction of RAM on real firmware.
 *   EfiLoaderCode/Data        -- this loader's image and stack, the KERNEL
 *     IMAGE, and the information block itself. Marking them available is
 *     correct, and it is safe for one structural reason: pmm_init() re-reserves
 *     the kernel (reserve(0, _kernel_end + metadata), pmm.c:285) and the
 *     information block (reserve(mb_info_addr, total_size), pmm.c:286) before
 *     it hands out a single frame. Nothing else of the loader's is alive after
 *     the jump.
 *
 * Everything else is reserved -- including runtime services, which are STILL
 * LIVE: this loader never calls SetVirtualAddressMap, so the firmware's runtime
 * code and data must stay exactly where they are. */
static UINT32 mb2_type_of(UINT32 efi_type)
{
    switch (efi_type) {
    case EfiConventionalMemory:
    case EfiBootServicesCode:
    case EfiBootServicesData:
    case EfiLoaderCode:
    case EfiLoaderData:
        return MB2_MEM_AVAILABLE;
    case EfiACPIReclaimMemory: return MB2_MEM_ACPI_RECLAIM;
    case EfiACPIMemoryNVS:     return MB2_MEM_NVS;
    case EfiUnusableMemory:    return MB2_MEM_BADRAM;
    default:                   return MB2_MEM_RESERVED;
    }
}

/* ------------------------------------------------------------------ *
 *  entry                                                              *
 * ------------------------------------------------------------------ */

/* c/boot/efi/trampoline.S. Not returning is part of its contract.
 *
 * It takes the install as parameters rather than doing it here because the
 * copy can destroy this C environment: the UEFI stack lives in
 * BootServicesData, which on this firmware is exactly the memory the kernel's
 * .bss lands on. mb2_handoff reads all five arguments into registers first and
 * then touches no stack at all, so a stack that ceases to exist half way
 * through the copy costs nothing -- there is nothing left to return to. */
extern void mb2_handoff(UINT64 dst, UINT64 src, UINT64 len,
                        UINT64 entry, UINT64 info);

/* THE 1 GiB CEILING ON THE INFORMATION BLOCK, and it is 1 GiB, not 4.
 *
 * The obvious constraint is that ebx is 32 bits wide, so the block must be
 * below 4 GiB. The REAL constraint is tighter and comes from the kernel:
 * boot.asm's setup_page_tables (c/boot/boot.asm:83-101) identity-maps exactly
 * the first 1 GiB with 2 MiB pages, and pmm_init reads the block through
 * mm_p2v(), which on the target is the identity (c/kernel/mm/mmhost.h:43). A
 * block at 2 GiB would satisfy Multiboot2 perfectly and page-fault the kernel
 * on its first read of total_size -- before there is an IDT to report it. */
#define INFO_MAX_ADDR 0x3FFFFFFFULL     /* uppermost byte, i.e. below 1 GiB */

/* Slack on the map buffer. GetMemoryMap's own documentation (UEFI 2.10 sec 7.2)
 * warns that the map can GROW between the sizing call and the real one --
 * because the sizing call's bookkeeping, and any allocation after it, can split
 * a descriptor in two. Asking for exactly the size just reported is the single
 * most common way a loader ends up in a GetMemoryMap/BUFFER_TOO_SMALL loop. */
#define MMAP_SLACK 8192

__attribute__((noreturn))
void efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab)
{
    EFI_STATUS st;

    ST = systab;
    serial_init();

    /* Validate both tables before calling a single function pointer out of
     * them. efi.h asserts the OFFSETS at compile time, which only proves this
     * file agrees with itself; the signatures are the runtime half -- they
     * prove the transcription describes the object the firmware handed us. Two
     * comparisons, and they turn "BootServices is at the wrong offset" from a
     * call through garbage into a printed refusal. */
    if (!ST || ST->Hdr.Signature != EFI_SYSTEM_TABLE_SIGNATURE) {
        sputs("\n[efi] REFUSING TO BOOT: bad EFI system table signature\n");
        halt();
    }
    BS = ST->BootServices;
    if (!BS || BS->Hdr.Signature != EFI_BOOT_SERVICES_SIGNATURE) {
        sputs("\n[efi] REFUSING TO BOOT: bad EFI boot services signature\n");
        halt();
    }

    say("\n[efi] LogitOS UEFI loader -- multiboot2 handoff, no GRUB\n");
    say("[efi] firmware revision "); sayx(ST->Hdr.Revision); say("\n");

    /* UEFI 2.10 sec 7.5: the firmware arms a 5-minute watchdog before calling a
     * boot application and lets it RESET THE MACHINE. Reading a 12 MiB kernel
     * off a slow USB stick is not a hang, but the watchdog cannot tell. */
    BS->SetWatchdogTimer(0, 0, 0, 0);

    UINT64 entry = load_kernel(image);

    /* ---- everything that ALLOCATES happens here, before the map is taken ---- */

    /* Size the EFI memory map. The sizing call is expected to fail with
     * EFI_BUFFER_TOO_SMALL: that failure is how it reports the size. */
    UINTN map_size = 0, map_key = 0, desc_size = 0;
    UINT32 desc_ver = 0;
    st = BS->GetMemoryMap(&map_size, 0, &map_key, &desc_size, &desc_ver);
    if (st != EFI_BUFFER_TOO_SMALL)
        die_st("GetMemoryMap sizing call did not report a size", st);
    if (desc_size < sizeof(EFI_MEMORY_DESCRIPTOR))
        die("firmware reports a memory descriptor smaller than the spec's");

    UINTN map_cap = map_size + MMAP_SLACK;
    EFI_MEMORY_DESCRIPTOR *map = 0;
    st = BS->AllocatePool(EfiLoaderData, map_cap, (void **)&map);
    if (st) die_st("AllocatePool for the EFI memory map failed", st);

    /* The information block, sized from the map just measured -- one 24-byte
     * mb2 entry per EFI descriptor -- plus room for the fixed tags. */
    UINTN max_entries = map_cap / desc_size + 8;
    UINTN info_bytes  = 8                        /* total_size + reserved */
                      + 64                       /* bootloader-name tag */
                      + 48                       /* framebuffer tag */
                      + 4096                     /* ACPI tag, generously */
                      + 16 + max_entries * 24    /* mmap tag */
                      + 16;                      /* end tag + alignment */
    info_bytes = (info_bytes + 0xFFF) & ~(UINTN)0xFFF;

    EFI_PHYSICAL_ADDRESS info_at = INFO_MAX_ADDR;
    st = BS->AllocatePages(AllocateMaxAddress, EfiLoaderData,
                           info_bytes >> 12, &info_at);
    if (st) die_st("no free pages below 1 GiB for the multiboot2 info block", st);

    /* The staged install overwrites [kernel_lo, kernel_hi) wholesale and runs
     * AFTER this block is finished, so an info block inside that span would be
     * erased between being built and being read. AllocateMaxAddress hands back
     * the highest free run under the limit, which is nowhere near the kernel --
     * but "is nowhere near" is an observation about one firmware, and this is a
     * check. */
    if (overlaps(info_at, info_at + info_bytes, kernel_lo, kernel_hi))
        die("the multiboot2 info block landed inside the kernel image");

    struct infobuf ib = { (UINT8 *)(UINTN)info_at, info_bytes, 8 };
    mzero(ib.base, 8);        /* total_size is written last; reserved stays 0 */

    /* Tag 2, bootloader name (Multiboot2 sec 3.6.3). The kernel ignores it, so
     * it is here for two other reasons: it is what GRUB does, and it is an
     * ODD-LENGTH tag standing in front of the two tags the kernel does read --
     * so if the 8-byte alignment arithmetic in ib_take() were wrong, the very
     * first boot would fail loudly, instead of on some future machine whose
     * RSDP happened to have a different length. */
    {
        const char *name = "LogitOS-EFI";
        UINT32 n = (UINT32)slen(name) + 1;
        struct mb2_tag *t = ib_take(&ib, 8 + n);
        t->type = MB2_TAG_LOADER;
        t->size = 8 + n;
        mcopy((UINT8 *)t + 8, name, n);
    }

    build_fb_tag(&ib);
    build_acpi_tag(&ib);

    /* The mmap tag goes last because it is the only variable-size tag that may
     * have to be rebuilt if ExitBootServices rejects the key. Remembering the
     * cursor here makes that retry a rewind rather than a rebuild. */
    UINTN mmap_tag_off = ib.off;

    /* =================== THE PART THAT KILLS LOADERS ====================
     * GetMemoryMap hands back a MAP KEY, and ExitBootServices accepts the key
     * only for the map as it is at that instant (UEFI 2.10 sec 7.4). ANYTHING
     * that changes the map in between invalidates it -- an AllocatePool, an
     * AllocatePages, and in practice a ConOut print, because the console driver
     * allocates. The symptom is EFI_INVALID_PARAMETER out of ExitBootServices,
     * and a loader that treats that as fatal fails on firmware that is behaving
     * exactly as specified.
     *
     * So: everything that allocates is already done, above. `serial_only` goes
     * up here and closes ConOut for the rest of the boot -- the conversion
     * below writes into a buffer that already exists, which is pure computation
     * and changes nothing. The spec's own remedy for a stale key is to take the
     * map again and retry, which is this loop; two attempts is enough, because
     * the second runs with literally nothing between GetMemoryMap and
     * ExitBootServices. */
    serial_only = 1;

    for (int attempt = 0; ; attempt++) {
        ib.off = mmap_tag_off;

        map_size = map_cap;
        st = BS->GetMemoryMap(&map_size, map, &map_key, &desc_size, &desc_ver);
        if (st) die_st("GetMemoryMap failed with a sized buffer", st);

        UINTN n_desc = map_size / desc_size;
        struct mb2_tag_mmap *mm = ib_take(&ib, 16 + n_desc * 24);
        mm->type = MB2_TAG_MMAP;
        mm->size = (UINT32)(16 + n_desc * 24);
        mm->entry_size = 24;        /* pmm.c:236 refuses anything smaller */
        mm->entry_version = 0;

        UINT64 avail = 0;
        for (UINTN i = 0; i < n_desc; i++) {
            /* Stride by desc_size, NEVER by sizeof(EFI_MEMORY_DESCRIPTOR): the
             * spec explicitly permits the firmware to return a LARGER
             * descriptor than the one it defines, precisely so it can be
             * extended, and a loader that strides by its own struct size walks
             * off into misalignment on exactly that firmware. */
            EFI_MEMORY_DESCRIPTOR *d =
                (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + i * desc_size);
            mm->entries[i].addr     = d->PhysicalStart;
            mm->entries[i].len      = d->NumberOfPages * 4096;
            mm->entries[i].type     = mb2_type_of(d->Type);
            mm->entries[i].reserved = 0;
            if (mm->entries[i].type == MB2_MEM_AVAILABLE)
                avail += mm->entries[i].len;

            /* -DEFI_DUMP_MMAP prints the EFI descriptor and the mb2 entry it
             * became, side by side, so the conversion can be AUDITED rather
             * than trusted -- "no reserved region became available" is a claim
             * about 128 descriptors that no summary line can carry. Off by
             * default because it is ~130 lines of serial at 115200 in the one
             * window where the map key must stay fresh; safe to enable because
             * sputs() is direct 0x3F8 port I/O with no allocation (serial_only
             * is already up, and attempt 0 prints here regardless). */
#ifdef EFI_DUMP_MMAP
            if (attempt == 0) {
                sputs("[efi] mm ");   sputx(mm->entries[i].addr);
                sputs(" +");          sputx(mm->entries[i].len);
                sputs(" efi=");       sputs(efi_type_name(d->Type));
                sputs(" mb2=");       sputd(mm->entries[i].type);
                sputs(mm->entries[i].type == MB2_MEM_AVAILABLE ? " AVAIL\n" : "\n");
            }
#endif
        }

        /* End tag, then the total size -- both inside the loop, because a retry
         * with a longer map moves both. */
        struct mb2_tag *end = ib_take(&ib, 8);
        end->type = MB2_TAG_END;
        end->size = 8;
        *(UINT32 *)ib.base = (UINT32)ib.off;

        if (attempt == 0) {
            sputs("[efi] mmap ");   sputd(n_desc);
            sputs(" entries, ");    sputd(avail >> 20);
            sputs(" MiB available\n[efi] info ");
            sputx((UINT64)(UINTN)ib.base);
            sputs(" size ");        sputd(ib.off);
            sputs("\n");
        }

        st = BS->ExitBootServices(image, map_key);
        if (st == EFI_SUCCESS) break;

        if (attempt >= 1) {
            sputs("\n[efi] REFUSING TO BOOT: ExitBootServices rejected the map "
                  "key twice\n[efi] halted.\n");
            halt();
        }
        sputs("[efi] ebs stale map key, retrying once (this is normal)\n");
    }

    /* From here the firmware does not exist. No ConOut, no boot services, and
     * no interrupt we are allowed to take -- the firmware's IDT is still loaded
     * in the CPU and the handlers behind it may already be gone. */
    __asm__ volatile ("cli");

    sputs("[efi] ebs ok\n");
    sputs("[efi] jump entry ");
    sputx(entry);
    sputs(" info ");
    sputx((UINT64)(UINTN)ib.base);
    sputs("\n");

    /* The prefix for the trampoline's own breadcrumbs -- it emits one character
     * per step of the descent and cannot afford a string. A log that stops
     * after "[efi] descent CP" says the machine died between clearing CR0.PG
     * and clearing EFER.LME, which is a located bug; a log that stops after
     * "[efi] jump" says only that something went wrong somewhere. */
    if (install_len) {
        sputs("[efi] install ");
        sputx(install_src);
        sputs(" -> ");
        sputx(install_dst);
        sputs("\n");
    }

    sputs("[efi] descent ");

    mb2_handoff(install_dst, install_src, install_len,
                entry, (UINT64)(UINTN)ib.base);

    halt();   /* mb2_handoff does not return; this is so the compiler knows */
}
