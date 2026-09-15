/* c/boot/efi/loader.c -- LogitOS's own UEFI boot loader.
 *
 * ============================ WHAT THIS IS ============================
 * This loader emits only Logit's native v1 block and enters logit_native_start
 * in the long mode UEFI already supplied. GOP, the EFI memory map and the EFI
 * configuration-table RSDP become the three native tags consumed by fb.c,
 * pmm.c and acpi.c through bootinfo.c's single entry-time adapter.
 *
 * HISTORY (retired 2026-09-15): the old build forged a Multiboot2 block, tore
 * UEFI long mode down to 32-bit protected mode, then made the kernel climb back
 * to long mode. That path and its PCIDE/LA57 descent gates were deleted after
 * both loaders gained the native entry. GRUB survives only in a separately
 * linked test fixture as the fixed-SeaBIOS differential oracle.
 *
 * ================= THE ONE THING GRUB NEVER HAS TO DO =================
 * GRUB loads PT_LOAD segments at the ELF physical addresses after consulting
 * the BIOS memory map. UEFI still owns the machine while this loader runs, so
 * this path must claim the whole kernel span itself. The old 1 MiB link address
 * crossed OVMF's ACPI NVS and BootServicesData ranges; copying there after
 * ExitBootServices destroyed firmware state and made a QEMU convenience into a
 * real-machine hazard.
 *
 * The shared ELF now links at 32 MiB, inside the native first-1-GiB identity
 * runway. The loader accepts exactly one placement path: AllocatePages at
 * the ELF address must succeed, and both an immediate memory-map readback and
 * the final map used for ExitBootServices must describe every byte as
 * EfiLoaderData. Any refusal, hole, type mismatch, or descriptor overflow stops
 * before ExitBootServices. No code writes the kernel image after firmware exit.
 *
 * WHAT IS STILL GENUINELY THIN. There is no module/initrd tag,
 * so this loader has no way to hand the kernel anything except the kernel. The
 * image path is the hardcoded pair \logit.elf / \LOGIT.ELF: no config, no
 * second entry, no fallback kernel. Nothing verifies the image it is about to
 * run, on a machine that compiles a root key into the kernel for .lpk. And
 * placement is one path or death by design (see above) -- which is the right
 * trade against corrupting firmware state, and is still a machine where 32 MiB
 * being occupied is a machine that does not boot.
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
#include "load_policy.h"
#include "../../../include/abi/logit_boot.h"

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
#ifndef EFI_NATIVE_EBS_EARLY
static void sputd(UINT64 v) { char b[21]; sputs(fmt_dec(b, v)); }
#endif

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

/* The allocated PT_LOAD span. It is checked once immediately after
 * AllocatePages and again against each final map offered to ExitBootServices. */
static UINT64 kernel_lo, kernel_hi;

/* One jar, two firmware doors: this value is not repeated here. MB2 reaches
 * boot.asm's historical 1-GiB table; native builds construct the same runway
 * and the entry normalizer probes its last byte before trusting a tag. */
#define BOOT_IDENTITY_LIMIT ((UINT64)LOGIT_BOOT_IDENTITY_MAP_BYTES)

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
 * jump depends on is false -- the kernel is missing, the firmware refused its
 * fixed address, ExitBootServices failed. Continuing from any of those means
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

/* NOT in a header. c/kernel/exec/load/elf.h already owns the basename "elf.h", and
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
 *  the boot information block, built with a bounds-checked cursor      *
 * ------------------------------------------------------------------ */

#define BOOT_TAG_MMAP        LOGIT_BOOT_TAG_MEMORY_MAP
#define BOOT_TAG_FRAMEBUFFER LOGIT_BOOT_TAG_FRAMEBUFFER
#define BOOT_TAG_ACPI_OLD    LOGIT_BOOT_TAG_ACPI_OLD
#define BOOT_TAG_ACPI_NEW    LOGIT_BOOT_TAG_ACPI_NEW
#define BOOT_TAG_END         LOGIT_BOOT_TAG_END
_Static_assert(sizeof(struct logit_boot_framebuffer_tag) == LOGIT_BOOT_FRAMEBUFFER_TAG_SIZE,
               "native framebuffer wire size drifted");

struct infobuf {
    UINT8 *base;
    UINTN  cap;
    UINTN  off;
};

/* Reserve `n` bytes and 8-align the cursor after them (both protocols require
 * eight-byte tag alignment). Overflow is a refusal, not a truncation: pmm's
 * walk stops at a malformed tag, so a truncated list is a kernel that boots
 * and then reports no memory -- a far worse thing to debug than a loader that
 * stops and says so. */
static void *ib_take(struct infobuf *ib, UINTN n)
{
    UINTN start = ib->off;
    UINTN end   = (start + n + 7) & ~(UINTN)7;
    if (end > ib->cap) die("boot information block too small");
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
 * AllocateAddress is an ownership request, not just a convenient allocator.
 * Success must turn the whole target into EfiLoaderData in the firmware map.
 * We read that map back immediately, then repeat the same validation on the
 * exact map/key passed to ExitBootServices. The latter closes the window where
 * a later firmware allocation or a broken implementation changes ownership.
 * The pure policy is in load_policy.h so a host mutation can prove that
 * accepting ACPI NVS or any other reserved overlap makes the gate red. */

static void map_walk_begin(EFI_MEMORY_DESCRIPTOR **m, UINTN *map_size, UINTN *dsz)
{
    UINTN size = 0, key = 0;
    UINT32 ver = 0;
    *m = 0; *map_size = 0; *dsz = 0;
    if (BS->GetMemoryMap(&size, 0, &key, dsz, &ver) != EFI_BUFFER_TOO_SMALL) return;
    size += 8192;
    if (BS->AllocatePool(EfiLoaderData, size, (void **)m) != EFI_SUCCESS) return;
    if (BS->GetMemoryMap(&size, *m, &key, dsz, &ver) != EFI_SUCCESS || *dsz == 0) {
        BS->FreePool(*m); *m = 0; return;
    }
    *map_size = size;
}

/* Print every descriptor overlapping [lo,hi). "The firmware said no" is a
 * refusal; naming the exact reserved owner is a diagnosis. */
static void report_who_owns(UINT64 lo, UINT64 hi)
{
    EFI_MEMORY_DESCRIPTOR *m; UINTN map_size, dsz;
    map_walk_begin(&m, &map_size, &dsz);
    if (!m) return;
    say("[efi] who owns that range, per the firmware's own map:\n");
    for (UINTN off = 0; off + dsz <= map_size; off += dsz) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)m + off);
        UINT64 a = d->PhysicalStart, b = a + d->NumberOfPages * 4096;
        if (b <= lo || a >= hi) continue;
        say("[efi]   "); sayx(a); say(".."); sayx(b);
        say("  ");       say(efi_type_name(d->Type));
        say(d->Type == EfiConventionalMemory ? "  (free)\n" : "  (NOT free)\n");
    }
    BS->FreePool(m);
}

/* Immediate readback after AllocatePages. The final EBS map is checked again
 * separately; neither observation substitutes for the other. */
static int allocated_range_is_loader_data(UINT64 lo, UINT64 hi)
{
    EFI_MEMORY_DESCRIPTOR *m; UINTN map_size, dsz;
    map_walk_begin(&m, &map_size, &dsz);
    if (!m) {
        say("[efi] cannot read the memory map to verify the allocation\n");
        return 0;
    }
    int ok = efi_load_range_is_loader_data(m, map_size, dsz, lo, hi);
    BS->FreePool(m);
    return ok;
}

/* [a,b) and [c,d) intersect. */
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

    /* Firmware relocates a PE image wherever it likes. The native handoff
     * replaces CR3 with a table mapping exactly the first 1 GiB, so the code
     * performing that switch must itself be inside the runway. */
    UINT64 img_end = (UINT64)(UINTN)li->ImageBase + li->ImageSize;
    if (img_end > BOOT_IDENTITY_LIMIT)
        die("firmware placed the native loader outside its 1-GiB runway");
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
     * linked to run identity-mapped at 32 MiB so the two are equal today, but
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
    if (hi > BOOT_IDENTITY_LIMIT)
        die("the kernel image does not fit in boot.asm's first-1-GiB identity map");

    UINT64 page_lo = lo & ~(UINT64)0xFFF;
    UINT64 page_hi = (hi + 0xFFF) & ~(UINT64)0xFFF;
    UINTN  pages   = (UINTN)((page_hi - page_lo) >> 12);
    kernel_lo = page_lo;
    kernel_hi = page_hi;

    /* The loader must not be standing on the kernel. A preferred PE base is a
     * request, so check the actual LoadedImage placement before asking for the
     * fixed ELF destination. */
    if (overlaps((UINT64)(UINTN)li->ImageBase, img_end, page_lo, page_hi))
        die("this loader is loaded on top of the kernel's link address "
            "(change /base in c/boot/efi/build.sh)");

    /* AllocateAddress is the only safe placement path. The former fallback
     * staged the image and copied it over ACPI NVS/BootServices memory after
     * ExitBootServices. That made a firmware reservation disappear by writing
     * through it, which is precisely what a loader must never do. A fixed
     * non-relocatable kernel either owns its complete address range now or the
     * boot is refused with the conflicting map entries printed. */
    EFI_PHYSICAL_ADDRESS at = page_lo;
    st = BS->AllocatePages(AllocateAddress, EfiLoaderData, pages, &at);
    if (st != EFI_SUCCESS || at != page_lo) {
        say("[efi] fixed kernel address unavailable ("); sayd(pages);
        say(" pages at "); sayx(page_lo); say(")\n");
        report_who_owns(page_lo, page_hi);
        if (st != EFI_SUCCESS)
            die_st("firmware refused the fixed kernel address; no safe "
                   "post-ExitBootServices fallback exists", st);
        die("AllocateAddress returned a different physical address");
    }
    if (!allocated_range_is_loader_data(page_lo, page_hi)) {
        report_who_owns(page_lo, page_hi);
        die("firmware did not mark the complete kernel allocation LoaderData");
    }
    say("[efi] load reserved "); sayx(page_lo); say(".."); sayx(page_hi); say("\n");

    /* Zero the ENTIRE span before loading, rather than each segment's
     * (memsz - filesz) tail. Two things fall out of one clear: .bss is zero as
     * the kernel's C requires, and so is every alignment gap BETWEEN segments
     * -- which no per-segment loop covers, and which AllocatePages does not
     * promise to hand over clean. */
    mzero((void *)(UINTN)page_lo, page_hi - page_lo);

    for (UINT16 i = 0; i < eh.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD || phdrs[i].p_memsz == 0) continue;
        if (phdrs[i].p_filesz) {
            seek(file, phdrs[i].p_offset);
            read_exact(file,
                       (void *)(UINTN)phdrs[i].p_paddr,
                       phdrs[i].p_filesz, "short read of a PT_LOAD segment");
        }
    }
    file->Close(file);
    root->Close(root);

    /* Native loaders use ELF e_entry directly. There is deliberately no
     * scanned boot header or second entry-address field that could disagree. */
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

/* Emits the native framebuffer tag, or emits nothing and says why. Absence is
 * a supported outcome: with `-vga none`, fb_init() drives virtio-gpu itself.
 * A fabricated linear address would turn a usable fallback into corruption. */
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

    struct logit_boot_framebuffer_tag *t = ib_take(ib, sizeof(*t));
    t->tag.type = BOOT_TAG_FRAMEBUFFER;
    t->tag.size = sizeof(*t);           /* 32 of header+geometry, 6 of colour */
    t->addr   = gop->Mode->FrameBufferBase;
    /* PixelsPerScanLine is in PIXELS and may exceed the visible width (the
     * scanline is padded); fb.c's `pitch` is in BYTES -- it strides fb_mem with
     * it. Multiplying by 4 is the whole conversion, and getting it wrong is the
     * classic sheared-image bug. */
    t->pitch  = mi->PixelsPerScanLine * 4;
    t->width  = mi->HorizontalResolution;
    t->height = mi->VerticalResolution;
    t->bpp     = 32;                    /* fb.c:157 requires exactly this */
    t->framebuffer_type = 1;            /* RGB; fb.c requires exactly this */
    t->red_position   = rp; t->red_mask_size   = 8;
    t->green_position = gp; t->green_mask_size = 8;
    t->blue_position  = bp; t->blue_mask_size  = 8;

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
    UINT32 len = 20, type = BOOT_TAG_ACPI_OLD;
    if (rsdp2) {
        UINT32 l;
        mcopy(&l, (UINT8 *)rsdp + 20, 4);
        if (l < 20 || l > 4096)               /* implausible: keep the v1 shape */
            say("[efi] rsdp length is implausible -- forwarding 20 bytes only\n");
        else { len = l; type = BOOT_TAG_ACPI_NEW; }
    }

    struct logit_boot_tag *t = ib_take(ib, 8 + len);
    t->type = type;
    t->size = 8 + len;
    mcopy((UINT8 *)t + sizeof(*t), rsdp, len);

    say("[efi] rsdp ");
    say(type == BOOT_TAG_ACPI_NEW ? "acpi2 " : "acpi1 ");
    sayx((UINT64)(UINTN)rsdp);
    say(" len ");        sayd(len);
    say(" -> boot tag "); sayx(type);
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
#ifndef EFI_NATIVE_EBS_EARLY
static UINT32 boot_memory_type_of(UINT32 efi_type)
{
    switch (efi_type) {
    case EfiConventionalMemory:
    case EfiBootServicesCode:
    case EfiBootServicesData:
    case EfiLoaderCode:
    case EfiLoaderData:
        return LOGIT_BOOT_MEMORY_AVAILABLE;
    case EfiACPIReclaimMemory: return LOGIT_BOOT_MEMORY_ACPI_RECLAIM;
    case EfiACPIMemoryNVS:     return LOGIT_BOOT_MEMORY_NVS;
    case EfiUnusableMemory:    return LOGIT_BOOT_MEMORY_BADRAM;
    default:                   return LOGIT_BOOT_MEMORY_RESERVED;
    }
}
#endif

/* ------------------------------------------------------------------ *
 *  entry                                                              *
 * ------------------------------------------------------------------ */

/* Four pages immediately below the kernel allocation: a temporary stack,
 * PML4, PDPT and PD. This matches the BIOS native layout for a non-cosmetic
 * reason: pmm.c's existing reserve from zero through the kernel covers these
 * active tables. A pool allocation wherever firmware happened to choose could
 * be advertised as free RAM and silently recycled while CR3 still used it. */
static UINT64 native_aux_lo, native_aux_hi;
static UINT64 native_pml4, native_stack_top;

struct native_gdt_pointer {
    UINT16 limit;
    UINT64 base;
} __attribute__((packed));

static UINT64 native_gdt[3] __attribute__((aligned(8))) = {
    0,
    (1ULL << 43) | (1ULL << 44) | (1ULL << 47) | (1ULL << 53),
    (1ULL << 41) | (1ULL << 44) | (1ULL << 47),
};
static struct native_gdt_pointer native_gdt_ptr = {
    sizeof(native_gdt) - 1, 0
};

static void setup_native_runway(void)
{
    if (kernel_lo < 4 * LOGIT_BOOT_BASE_PAGE_BYTES)
        die("kernel leaves no reserved low pages for the native runway");
    native_aux_lo = kernel_lo - 4 * LOGIT_BOOT_BASE_PAGE_BYTES;
    native_aux_hi = kernel_lo;

    EFI_PHYSICAL_ADDRESS at = native_aux_lo;
    EFI_STATUS st = BS->AllocatePages(AllocateAddress, EfiLoaderData, 4, &at);
    if (st != EFI_SUCCESS || at != native_aux_lo)
        die_st("cannot reserve the native page tables below the kernel", st);
    if (!allocated_range_is_loader_data(native_aux_lo, native_aux_hi))
        die("firmware did not mark the native page tables LoaderData");

    UINT64 *pml4 = (UINT64 *)(UINTN)(native_aux_lo + 0x1000);
    UINT64 *pdpt = (UINT64 *)(UINTN)(native_aux_lo + 0x2000);
    UINT64 *pd   = (UINT64 *)(UINTN)(native_aux_lo + 0x3000);
    mzero((void *)(UINTN)native_aux_lo, 4 * LOGIT_BOOT_BASE_PAGE_BYTES);
    pml4[0] = (UINT64)(UINTN)pdpt | 3;
    pdpt[0] = (UINT64)(UINTN)pd | 3;
    for (UINTN i = 0;
         i < LOGIT_BOOT_IDENTITY_MAP_BYTES / LOGIT_BOOT_IDENTITY_PAGE_BYTES;
         i++)
        pd[i] = (UINT64)i * LOGIT_BOOT_IDENTITY_PAGE_BYTES | 0x83;

    native_stack_top = native_aux_lo + LOGIT_BOOT_BASE_PAGE_BYTES;
    native_pml4 = (UINT64)(UINTN)pml4;
    native_gdt_ptr.base = (UINT64)(UINTN)native_gdt;
    say("[efi] native identity runway 0x0..");
    sayx(LOGIT_BOOT_IDENTITY_MAP_BYTES);
    say(" tables "); sayx(native_aux_lo); say(".."); sayx(native_aux_hi);
    say("\n");
}

/* No descent is hidden here. UEFI already supplied long mode; the only
 * transition installs the documented GDT and the loader-owned 1-GiB CR3,
 * moves to the reserved low stack before replacing CR3, reloads CS through a
 * far return, then jumps with RAX/RDI. Every operand is parked in a register
 * before either change, and no C access follows. */
__attribute__((noreturn, noinline))
static void native_handoff(UINT64 entry, UINT64 info, UINT64 pml4, UINT64 stack)
{
    __asm__ volatile (
        "cli\n\t"
        "cld\n\t"
        "movq %3, %%rsp\n\t"
        "lgdt %5\n\t"
        "movq %2, %%cr3\n\t"
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "xor %%eax, %%eax\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movq %1, %%rdi\n\t"
        "movl %4, %%eax\n\t"
        "jmp *%0\n\t"
        :
        : "r"(entry), "r"(info), "r"(pml4), "r"(stack),
          "i"(LOGIT_BOOT_MAGIC), "m"(native_gdt_ptr)
        : "rax", "rdi", "memory");
    __builtin_unreachable();
}

/* THE 1 GiB CEILING ON THE INFORMATION BLOCK. The native contract promises
 * exactly this runway and pmm_init reads the block through the low identity
 * mapping. A block at 1 GiB would page-fault before there is an IDT to report
 * it, so allocation uses the highest reachable address and verifies it. */
#define INFO_MAX_ADDR (BOOT_IDENTITY_LIMIT - 1) /* uppermost byte below 1 GiB */

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

    say("\n[efi] LogitOS UEFI loader -- native v1 handoff, no descent\n");
    say("[efi] firmware revision "); sayx(ST->Hdr.Revision); say("\n");

    /* UEFI 2.10 sec 7.5: the firmware arms a 5-minute watchdog before calling a
     * boot application and lets it RESET THE MACHINE. Reading a 12 MiB kernel
     * off a slow USB stick is not a hang, but the watchdog cannot tell. */
    BS->SetWatchdogTimer(0, 0, 0, 0);

    UINT64 entry = load_kernel(image);

    setup_native_runway();

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
     * entry per EFI descriptor -- plus room for the fixed tags. */
    UINTN max_entries = map_cap / desc_size + 8;
    UINTN info_bytes  = LOGIT_BOOT_HEADER_SIZE
                      + 48                       /* framebuffer tag */
                      + 4096                     /* ACPI tag, generously */
                      + 16 + max_entries * 24    /* mmap tag */
                      + 16;                      /* end tag + alignment */
    info_bytes = (info_bytes + 0xFFF) & ~(UINTN)0xFFF;

    EFI_PHYSICAL_ADDRESS info_at;
#if defined(EFI_NATIVE_INFO_ABOVE_MAP)
    /* Executable control: with 2 GiB of guest RAM this is a real allocation at
     * the first byte the promised page tables do not map. The native entry
     * checks RDI before dereferencing it, so the expected result is a named
     * refusal rather than an unlocated page fault. */
    info_at = LOGIT_BOOT_IDENTITY_MAP_BYTES;
    st = BS->AllocatePages(AllocateAddress, EfiLoaderData,
                           info_bytes >> 12, &info_at);
#else
    info_at = INFO_MAX_ADDR;
    st = BS->AllocatePages(AllocateMaxAddress, EfiLoaderData,
                           info_bytes >> 12, &info_at);
#endif
    if (st) die_st("no reachable pages for the boot information block", st);
    if (!allocated_range_is_loader_data(info_at, info_at + info_bytes))
        die("firmware did not mark the boot information block LoaderData");

    /* AllocateMaxAddress should place this far above the already-reserved
     * kernel, but firmware allocations are verified rather than inferred. */
    if (overlaps(info_at, info_at + info_bytes, kernel_lo, kernel_hi))
        die("the boot information block landed inside the kernel image");

    struct infobuf ib = {
        (UINT8 *)(UINTN)info_at, info_bytes,
        LOGIT_BOOT_HEADER_SIZE
    };
    struct logit_boot_header *native_header =
        (struct logit_boot_header *)ib.base;
    mzero(native_header, LOGIT_BOOT_HEADER_SIZE);
    native_header->magic = LOGIT_BOOT_MAGIC;
#ifdef EFI_NATIVE_BAD_VERSION
    native_header->version = LOGIT_BOOT_VERSION + 1;
#else
    native_header->version = LOGIT_BOOT_VERSION;
#endif
    native_header->header_size = LOGIT_BOOT_HEADER_SIZE;
    native_header->identity_map_bytes = LOGIT_BOOT_IDENTITY_MAP_BYTES;

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
     * the second runs with no operation that can change the map between
     * GetMemoryMap and ExitBootServices. */
    serial_only = 1;

    for (int attempt = 0; ; attempt++) {
        ib.off = mmap_tag_off;

        map_size = map_cap;
        st = BS->GetMemoryMap(&map_size, map, &map_key, &desc_size, &desc_ver);
        if (st) die_st("GetMemoryMap failed with a sized buffer", st);

        /* This is the last ownership decision before the point of no return.
         * It runs on every EBS attempt and on the exact bytes associated with
         * map_key. Serial diagnostics below use raw port I/O and cannot mutate
         * the firmware map. */
        if (!efi_load_range_is_loader_data(map, map_size, desc_size,
                                           kernel_lo, kernel_hi)) {
            sputs("\n[efi] REFUSING TO BOOT: final memory map no longer owns "
                  "the complete kernel span as LoaderData\n[efi] halted.\n");
            halt();
        }
        if (!efi_load_range_is_loader_data(map, map_size, desc_size,
                                           native_aux_lo, native_aux_hi)) {
            sputs("\n[efi] REFUSING TO BOOT: final memory map no longer owns "
                  "the native page tables as LoaderData\n[efi] halted.\n");
            halt();
        }
        if (!efi_load_range_is_loader_data(map, map_size, desc_size,
                                           info_at, info_at + info_bytes)) {
            sputs("\n[efi] REFUSING TO BOOT: final memory map no longer owns "
                  "the boot information block as LoaderData\n[efi] halted.\n");
            halt();
        }
        if (attempt == 0)
            sputs("[efi] kernel map LoaderData confirmed\n");

#if defined(EFI_NATIVE_EBS_EARLY)
        /* Control-only wrong order: the header remains visibly incomplete
         * (total_size is still zero), and no byte is written after EBS. The
         * kernel must refuse it through the native entry normalizer. */
        if (attempt == 0)
            sputs("[efi] CONTROL ExitBootServices before native block complete\n");
#else
        UINTN n_desc = map_size / desc_size;
        struct logit_boot_mmap_tag *mm =
            ib_take(&ib, sizeof(*mm) + n_desc * sizeof(mm->entries[0]));
        mm->tag.type = BOOT_TAG_MMAP;
        mm->tag.size = (UINT32)(sizeof(*mm) + n_desc * sizeof(mm->entries[0]));
        mm->entry_size = sizeof(mm->entries[0]); /* pmm.c refuses smaller */
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
            mm->entries[i].type     = boot_memory_type_of(d->Type);
            mm->entries[i].reserved = 0;
            if (mm->entries[i].type == LOGIT_BOOT_MEMORY_AVAILABLE)
                avail += mm->entries[i].len;

            /* -DEFI_DUMP_MMAP prints the EFI descriptor and native entry it
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
                sputs(" boot=");      sputd(mm->entries[i].type);
                sputs(mm->entries[i].type == LOGIT_BOOT_MEMORY_AVAILABLE ? " AVAIL\n" : "\n");
            }
#endif
        }

        /* End tag, then the total size -- both inside the loop, because a retry
         * with a longer map moves both. */
        struct logit_boot_tag *end = ib_take(&ib, sizeof(*end));
        end->type = BOOT_TAG_END;
        end->size = sizeof(*end);
        native_header->total_size = (UINT32)ib.off;

        if (attempt == 0) {
            sputs("[efi] mmap ");   sputd(n_desc);
            sputs(" entries, ");    sputd(avail >> 20);
            sputs(" MiB available\n[efi] info ");
            sputx((UINT64)(UINTN)ib.base);
            sputs(" size ");        sputd(ib.off);
            sputs("\n");
        }
#endif

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

    sputs("[efi] native jump (UEFI long mode retained)\n");
    native_handoff(entry, (UINT64)(UINTN)ib.base, native_pml4,
                   native_stack_top);

    halt();   /* native_handoff does not return; this is for the compiler */
}
