/* c/boot/efi/trampoline.S -- long mode down to 32-bit protected mode, and the
 * jump into the kernel exactly as GRUB would make it.
 *
 * WHY THIS EXISTS AT ALL. UEFI hands an application the CPU in 64-bit long
 * mode with paging on. The kernel's entry (c/boot/boot.asm:19 `start`) is
 * 32-bit protected-mode code: it checks eax for the Multiboot2 magic, builds
 * its OWN identity page tables, sets PAE/LME/PG and far-jumps to long mode by
 * itself. That is the contract this milestone refuses to change -- one kernel,
 * two loaders -- so the loader has to give back what the firmware gave it and
 * arrive at `start` in the machine state the Multiboot2 spec (sec 3.2/3.3)
 * describes: protected mode, paging off, interrupts off, eax = 0x36D76289,
 * ebx = the address of the information block.
 *
 * WHY HAND-WRITTEN ASSEMBLY AND NOT INLINE ASM IN loader.c. Between the far
 * jump and the kernel there is no valid C environment: the stack segment is
 * about to change meaning, r8-r15 stop existing, and the compiler would be
 * free to spill anything anywhere. The descent has to be one uninterrupted
 * instruction stream whose every memory reference is accounted for, and the
 * only way to have that is to write it.
 *
 * ENTRY (Microsoft x64, UEFI 2.10 sec 2.3.4 -- the loader is compiled for it):
 *   rcx      = install destination (the kernel's link address), or ignored
 *   rdx      = install source (the staged image), or ignored
 *   r8       = install length in bytes, or 0 for "already in place"
 *   r9       = kernel entry point, physical == linear, guaranteed < 4 GiB by
 *              the caller (loader.c refuses a higher one, never truncates it)
 *   [rsp+40] = physical address of the Multiboot2 information block, which
 *              loader.c allocates below 1 GiB -- see INFO_MAX_ADDR there for
 *              why 1 GiB and not 4
 * Does not return. Callers must have already called ExitBootServices.
 *
 * WHY THE INSTALL IS HERE AND NOT IN C. When the firmware will not hand over
 * the kernel's link address before ExitBootServices, loader.c stages the
 * finished image elsewhere and this routine copies it down. That copy CANNOT
 * be C, because on this firmware the destination (the kernel's ~10 MiB .bss)
 * overlaps BootServicesData -- which is where the UEFI application's own STACK
 * lives. A C memcpy would be running on memory it was in the middle of
 * destroying: return addresses, spilled locals, the lot. The sequence below
 * reads all five arguments into registers BEFORE the first store and then
 * never touches the stack again, so losing the stack costs nothing. There is
 * nothing to return to.
 *
 * THE ONE ENVIRONMENTAL ASSUMPTION, stated because everything rests on it:
 * the instructions that clear CR0.PG must live at a linear address equal to
 * their physical address (SDM Vol 3A sec 9.8.5.4: "the MOV CR0 instruction ...
 * and the subsequent instructions must be located in an identity-mapped
 * page"). UEFI identity-maps all the memory it describes (UEFI 2.10 sec 2.3.4:
 * "Paging mode is enabled and any memory space defined by the UEFI memory map
 * is identity mapped virtual to physical"), so this holds for the loader image
 * wherever the firmware placed it -- provided that place is below 4 GiB, which
 * loader.c CHECKS (against LoadedImage.ImageBase/ImageSize) and refuses loudly
 * rather than assuming.
 *
 * BREADCRUMBS. Four characters go out of COM1 during the descent, one per
 * step, because this is the stretch with no console, no exception handler and
 * no way back: a triple fault here reboots the machine with nothing on screen.
 * "CPLJ" on the wire means compatibility mode reached, paging off, long mode
 * disabled, about to jump -- so a failure is LOCATED rather than mysterious.
 * loader.c prints the "[efi] descent " prefix immediately before calling.
 */

/* Selectors into the GDT built below. The values are the byte offsets of the
 * descriptors, which is what a selector IS (SDM Vol 3A sec 3.4.2). */
#define SEL_CODE32 0x08
#define SEL_DATA32 0x10

#define COM1 0x3F8

/* THE NEGATIVE CONTROL, and it lives here because here is the only place the
 * value is produced.
 *
 * Everything else in this milestone is an assertion that the machine boots,
 * and a boot test that passes proves the chain END TO END without proving that
 * any particular link is load-bearing. In particular it does NOT prove that
 * boot.asm's check_multiboot (c/boot/boot.asm:38-44) still runs on this path:
 * if the loader jumped with eax holding garbage and the kernel booted anyway,
 * every positive assertion in tests/boot/run-uefi-test.sh would still pass.
 *
 * So the harness builds a SECOND loader with -DEFI_BAD_MAGIC, identical in
 * every other respect, and requires it to reach "[efi] jump" -- the loader did
 * nothing wrong, it did exactly what it was built to do -- and then requires
 * LOGIT_BOOT_OK never to appear. One bit off the real magic is the smallest
 * possible lie, which is what makes it a test of the comparison rather than of
 * the kernel's tolerance for nonsense. */
#ifdef EFI_BAD_MAGIC
#define MB2_MAGIC 0x36D7628A        /* deliberately wrong in the low nibble */
#else
#define MB2_MAGIC 0x36D76289        /* Multiboot2 sec 3.3; boot.asm:38 */
#endif

/* One character to COM1, 32-bit code, bounded. Clobbers eax, ecx, edx -- all
 * dead at every point this is used; the two values being carried live in edi
 * and esi throughout. The wait is bounded for the same reason serial_putc() in
 * c/drivers/char/serial.c is: a machine with no UART must not have its boot
 * turned into a hang BY THE DIAGNOSTIC. */
.macro TXCHAR ch
    movl  $100000, %ecx
    movw  $(COM1 + 5), %dx
1:  inb   %dx, %al
    testb $0x20, %al                /* LSR bit 5: transmitter holding empty */
    jnz   2f
    loop  1b
    jmp   3f
2:  movw  $COM1, %dx
    movb  $\ch, %al
    outb  %al, %dx
3:
.endm

    .text
    .globl mb2_handoff
    .code64
mb2_handoff:
    cli                             /* MB2 sec 3.3: EFLAGS.IF must be 0 on entry
                                     * to the kernel. Also: our IDT is about to
                                     * stop describing anything real. */

    /* ---- read EVERY argument out of the stack before the first store ----
     * The fifth argument lives at [rsp+40]: return address (8) plus the
     * 32-byte register-parameter shadow space the Microsoft x64 ABI reserves
     * even for arguments passed in registers. After this instruction the stack
     * is never read or written again, which is the whole point (see header). */
    movq 40(%rsp), %r11             /* r11 = &mb2 info */
    movq %r9,  %r10                 /* r10 = kernel entry */

    /* ---- patch the two runtime-relocated pointers, while things are quiet ----
     * Both are LINEAR addresses and the firmware may have relocated this image
     * away from its preferred base, so they are computed here rather than baked
     * in by the linker. That costs four instructions and makes the whole
     * descent independent of the load address and of whether base relocations
     * were applied at all. Both writes land in this image's .data, which
     * loader.c has verified is disjoint from the install destination. */
    leaq  gdt32(%rip), %rax
    movq  %rax, gdt32_ptr + 2(%rip)
    lgdt  gdt32_ptr(%rip)

    leaq  compat32(%rip), %rax      /* < 4 GiB: loader.c refuses an image the */
    movl  %eax, farptr32(%rip)      /* firmware placed above it */

    /* ---- install the staged image, if there is one ---- */
    testq %r8, %r8
    jz    .Lno_install
    movq  %rcx, %rdi                /* dst */
    movq  %rdx, %rsi                /* src */
    movq  %r8,  %rcx                /* len */
    cld                             /* MOVS must count upward. DF is undefined
                                     * on entry to a function under the ABI, so
                                     * it is set rather than assumed -- a
                                     * backwards 11 MiB copy would run off the
                                     * bottom of memory. */
    rep movsb                       /* Still 64-bit with the firmware's identity
                                     * map live, which is exactly why the copy
                                     * is HERE and not after the descent: below
                                     * it only 4 GiB is addressable and paging
                                     * has just been turned off. */
.Lno_install:

    /* Park the two hand-off values in registers that still exist in 32-bit
     * protected mode. r8-r15 do NOT (SDM Vol 3A sec 3.4.1.1: the extended
     * registers are accessible only in 64-bit mode), so they cannot simply be
     * left in r10/r11 across the far jump.
     *
     * AFTER THE COPY, NOT BEFORE, and this cost a boot: REP MOVSB advances RDI
     * and RSI by the count. Setting edi here first left the descent jumping to
     * dst+len -- the far end of the kernel's .bss -- which executed zeroes and
     * died with the full "descent CPLJ" breadcrumb printed and not one byte of
     * kernel output after it. r10 and r11 are untouched by the copy, which is
     * why they hold these values across it. */
    movl %r10d, %edi                /* edi = kernel entry */
    movl %r11d, %esi                /* esi = &mb2 info */

    /* Enter compatibility mode: a far transfer to a code segment with L=0.
     *
     * FF /5, "JMP m16:32" -- far jump, absolute indirect, through MEMORY (SDM
     * Vol 2A, JMP; the 64-bit column says Valid). The textbook route is a far
     * RETURN off a manufactured stack frame instead, because the immediate far
     * jump opcode EA is invalid in 64-bit mode -- but LRETQ READS THE STACK,
     * and the copy that may just have run is allowed to have destroyed it. The
     * indirect far jump reads a six-byte cell in this image's own .data, so the
     * descent needs no valid stack at any point, which is the invariant that
     * makes the whole staged path safe. */
    ljmpl *farptr32(%rip)

    .code32
compat32:
    /* Reload the data segments IMMEDIATELY. The selectors currently cached in
     * ds/es/ss came from the firmware's GDT, which we replaced; in
     * compatibility mode segmentation is live again (unlike 64-bit mode, where
     * base and limit are ignored), so the first memory access through a stale
     * selector would be checked against whatever descriptor now sits at that
     * index. Nothing between the far jump and here touches memory. */
    movl $SEL_DATA32, %eax
    movl %eax, %ds
    movl %eax, %es
    movl %eax, %fs
    movl %eax, %gs
    movl %eax, %ss
    TXCHAR 'C'

    /* --- SDM Vol 3A sec 9.8.5.4, "Switching Out of IA-32e Mode Operation",
     * step by step and in its order. Any other order faults or hangs. --- */

    /* Step 2: clear CR0.PG. This is what actually leaves IA-32e mode; the
     * processor clears EFER.LMA itself as a side effect. Legal only because
     * CS.L is now 0 (step 1, the far jump above) and only because this code is
     * identity-mapped (see the header comment).
     *
     * Steps 3 and 5 of that sequence -- load CR3 with legacy tables, turn
     * paging back on -- are deliberately NOT taken. The kernel builds its own
     * page tables from scratch (boot.asm setup_page_tables) and Multiboot2
     * sec 3.3 specifies paging OFF at the entry point, so re-enabling it here
     * would be work done only to be undone. */
    movl %cr0, %eax
    andl $0x7FFFFFFF, %eax          /* ~(1 << 31) */
    movl %eax, %cr0
    TXCHAR 'P'

    /* Step 4: clear EFER.LME (bit 8 of MSR 0xC0000080, SDM Vol 4 Table 2-2).
     * Long mode is now neither active nor enabled. */
    movl $0xC0000080, %ecx
    rdmsr
    andl $0xFFFFFEFF, %eax          /* ~(1 << 8) */
    wrmsr
    TXCHAR 'L'

    /* Not a step of the SDM sequence, but the difference between "protected
     * mode" and "protected mode that still smells of long mode": drop CR4.PAE
     * (bit 5) and CR4.PGE (bit 7). The kernel ORs PAE back on at
     * c/boot/boot.asm:105, so leaving it set would in fact be harmless -- this
     * is here so the state handed to `start` is the state GRUB hands it, and so
     * that "it works from GRUB but not from us" can never turn out to be a
     * difference in a control register nobody thought to compare. Clearing PAE
     * is legal only now that LME is off (SDM Vol 3A sec 4.1.2: software must
     * not clear CR4.PAE while IA-32e mode is active). */
    movl %cr4, %eax
    andl $0xFFFFFF5F, %eax          /* ~((1 << 5) | (1 << 7)) */
    movl %eax, %cr4

    TXCHAR 'J'                      /* last breadcrumb: eax is about to become
                                     * the magic and must not be clobbered */
    TXCHAR '\r'
    TXCHAR '\n'

    /* The Multiboot2 hand-off itself (spec sec 3.3). No stack is set up: the
     * spec leaves esp undefined and the kernel's first instruction is
     * `mov esp, stack_top` (c/boot/boot.asm:19). */
    movl %esi, %ebx                 /* ebx = &mb2 information block */
    movl $MB2_MAGIC, %eax           /* eax = bootloader magic; boot.asm:38 */
    jmp  *%edi                      /* -> c/boot/boot.asm `start` */

    /* Unreachable. If the kernel entry were ever to return, halting is the only
     * honest thing left: there is no firmware to go back to. (A NAMED label,
     * not a numeric local: `jmp 1b` here would bind to the most recent `1:`,
     * which after the TXCHAR expansions above is the middle of a UART poll
     * loop, not this hlt.) */
.Ldead:
    hlt
    jmp .Ldead

/* The GDT lives in .data, not .text. UEFI firmware may apply W^X page
 * protections to a loaded PE image's sections (UEFI 2.10 sec 2.5.1 and the
 * EDK II image protection policy), and the base of the pseudo-descriptor is
 * written at runtime -- a write into .text would fault on exactly the firmware
 * that is careful, which is the firmware worth working on.
 *
 * Two flat descriptors, both base 0 / limit 4 GiB (G=1, limit 0xFFFFF),
 * D/B=1 for 32-bit operand and address size, L=0 -- SDM Vol 3A sec 3.4.5:
 *   0x9A = present, DPL 0, code, executable + readable
 *   0x92 = present, DPL 0, data, writable
 *   0xCF = G=1, D/B=1, L=0, AVL=0, limit bits 19:16 = 0xF
 */
    .data
    .balign 8
gdt32:
    .quad 0x0000000000000000        /* 0x00: the required null descriptor */
    .word 0xFFFF, 0x0000            /* 0x08: 32-bit code, flat */
    .byte 0x00, 0x9A, 0xCF, 0x00
    .word 0xFFFF, 0x0000            /* 0x10: 32-bit data, flat */
    .byte 0x00, 0x92, 0xCF, 0x00
gdt32_end:

    .balign 8
gdt32_ptr:
    .word gdt32_end - gdt32 - 1     /* limit = size - 1 (SDM Vol 2A, LGDT) */
    .quad 0                         /* base, filled in above */

/* The m16:32 operand of the indirect far jump: a 32-bit offset followed by the
 * 16-bit selector, in that order (SDM Vol 2A, JMP -- the memory operand is
 * offset-then-selector, the opposite of the order LRET pops them in). The
 * offset is patched at runtime; the selector is known at assembly time. */
    .balign 8
farptr32:
    .long 0                         /* offset of compat32, filled in above */
    .word SEL_CODE32                /* selector */
