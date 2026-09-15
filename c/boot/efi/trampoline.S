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
 *   rcx      = kernel entry point, physical == linear, guaranteed < 1 GiB by
 *              the caller (loader.c refuses a higher one, never truncates it)
 *   rdx      = physical address of the Multiboot2 information block, which
 *              loader.c allocates below 1 GiB -- see INFO_MAX_ADDR there for
 *              why 1 GiB and not 4
 * Does not return. Callers must have already called ExitBootServices.
 *
 * WHY THERE IS NO INSTALL COPY. The previous ABI carried destination/source/
 * length so this routine could overwrite firmware-owned low memory after
 * ExitBootServices. In particular it accepted ACPI NVS. The kernel now links at
 * 32 MiB and loader.c requires AllocateAddress plus two memory-map ownership
 * checks before reaching here, so the image is already in its final address.
 * This routine performs only the CPU-mode descent and hand-off.
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
 * THE PCIDE AND LA57 TRAPS. UEFI promises long mode, paging, and an identity map; it does
 * not promise CR4.PCIDE=0. Intel requires PCIDE to be clear before software
 * clears CR0.PG. Doing that after the compatibility jump is already too late:
 * the next architectural step is the forbidden PG clear. We therefore clear
 * and read back PCIDE here, while still in 64-bit mode with paging enabled.
 * Conversely LA57 may only change while paging is off. The kernel builds a
 * four-level PML4, so after leaving IA-32e mode we clear and read back LA57
 * before the kernel enables paging again. Otherwise firmware that used
 * five-level paging would make that PML4 be interpreted as a PML5.
 *
 * BREADCRUMBS. Five characters go out of COM1 during the descent, one per
 * step, because this is the stretch with no console, no exception handler and
 * no way back: a triple fault here reboots the machine with nothing on screen.
 * "ICPLJ" on the wire means PCIDE and LA57 clears were confirmed,
 * compatibility mode
 * reached, paging off, long mode disabled, about to jump -- so a failure is
 * LOCATED rather than mysterious. The EFI_FORCE_PCIDE test build prefixes F
 * after it has forced and read back PCIDE=1, making "FICPLJ" executable proof
 * that the I gate handled the dangerous incoming state. Mutation-only guest
 * builds add a final 64-bit readback: "FU" means PCIDE was still one and the
 * test seam halted before the far jump. This explicit observation is needed
 * because QEMU TCG does not reliably model the architectural #GP for the later
 * CR0.PG transition; it is not itself a claim about that simulated exception.
 * loader.c prints the "[efi] descent " prefix immediately before calling.
 */

/* Selectors into the GDT built below. The values are the byte offsets of the
 * descriptors, which is what a selector IS (SDM Vol 3A sec 3.4.2). */
#define SEL_CODE32 0x08
#define SEL_DATA32 0x10

#define COM1 0x3F8
#define CR4_PCIDE (1 << 17)
#define CR4_PAE   (1 << 5)
#define CR4_PGE   (1 << 7)
#define CR4_LA57  (1 << 12)

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
    cld                             /* MB2 sec 3.3 also requires DF clear. */

    /* Park both arguments before changing modes. No stack access is needed
     * after entry; r8-r15 cease to be addressable in compatibility mode. */
    movq %rdx, %r11                 /* r11 = &mb2 info */
    movq %rcx, %r10                 /* r10 = kernel entry */

#ifdef EFI_FORCE_PCIDE
    /* Guest-only executable seam. Setting PCIDE when it is already clear
     * requires CR3[11:0]=0; OVMF/QEMU satisfies that architectural precondition.
     * Refuse the seam rather than perturb firmware's CR3 if it does not. */
    movq %cr4, %rax
    testq $CR4_PCIDE, %rax
    jnz .Lpcide_forced
    movq %cr3, %rax
    testq $0xFFF, %rax
    jnz .Lpcide_failed
    movq %cr4, %rax
    orq $CR4_PCIDE, %rax
    movq %rax, %cr4
.Lpcide_forced:
    movq %cr4, %rax
    testq $CR4_PCIDE, %rax
    jz .Lpcide_failed
    TXCHAR 'F'
#endif

#if !defined(EFI_PCIDE_NEGCTL_SKIP_CLEAR) && !defined(EFI_PCIDE_NEGCTL_LATE_CLEAR)
    /* Intel SDM Vol. 3A, paging-mode transitions: CR4.PCIDE=1 makes a later
     * clear of CR0.PG fault. Clear it while PG=1 and read the architectural
     * register back before any far jump can enter the no-return descent. */
    movq %cr4, %rax
    andq $~CR4_PCIDE, %rax
    movq %rax, %cr4
#ifndef EFI_PCIDE_NEGCTL_SKIP_READBACK
    movq %cr4, %rax
    testq $CR4_PCIDE, %rax
    jnz .Lpcide_failed
#endif
    TXCHAR 'I'
#endif

    /* ---- patch the two runtime-relocated pointers, while things are quiet ----
     * Both are LINEAR addresses and the firmware may have relocated this image
     * away from its preferred base, so they are computed here rather than baked
     * in by the linker. That costs four instructions and makes the whole
     * descent independent of the load address and of whether base relocations
     * were applied at all. Both writes land in this image's .data, which
     * loader.c has verified is disjoint from the kernel destination. */
    leaq  gdt32(%rip), %rax
    movq  %rax, gdt32_ptr + 2(%rip)
    lgdt  gdt32_ptr(%rip)

    leaq  compat32(%rip), %rax      /* < 4 GiB: loader.c refuses an image the */
    movl  %eax, farptr32(%rip)      /* firmware placed above it */

    /* Park the two hand-off values in registers that still exist in 32-bit
     * protected mode. r8-r15 do NOT (SDM Vol 3A sec 3.4.1.1: the extended
     * registers are accessible only in 64-bit mode), so they cannot simply be
     * left in r10/r11 across the far jump.
     *
     * They are copied only after the runtime-relocated GDT pointers are patched,
     * so later scratch-register use cannot corrupt the hand-off. */
    movl %r10d, %edi                /* edi = kernel entry */
    movl %r11d, %esi                /* esi = &mb2 info */

#ifdef EFI_PCIDE_TEST_ASSERT_CLEAR
    /* Executable mutation witness, deliberately separate from the production
     * gate above. QEMU TCG has been observed to permit CR0.PG to be cleared
     * with PCIDE still set, so waiting for a simulated #GP would be a false
     * test oracle. Read the architectural register one last time while still
     * in 64-bit mode with paging enabled; unsafe variants print U and halt.
     * The actual production guarantee remains the unconditional clear and
     * readback above, whose instruction order is checked from the real object. */
    movq %cr4, %rax
    testq $CR4_PCIDE, %rax
    jnz .Lpcide_test_unsafe
#endif

#ifdef EFI_LA57_NEGCTL_EARLY_CLEAR
    /* Assembly-only mutation: LA57 cannot change while CR0.PG is set. The
     * host order gate must reject this object; it is never executed. */
    movq %cr4, %rax
    andq $~CR4_LA57, %rax
    movq %rax, %cr4
#endif

    /* Enter compatibility mode: a far transfer to a code segment with L=0.
     *
     * FF /5, "JMP m16:32" -- far jump, absolute indirect, through MEMORY (SDM
     * Vol 2A, JMP; the 64-bit column says Valid). The textbook route is a far
     * RETURN off a manufactured stack frame instead, because the immediate far
     * jump opcode EA is invalid in 64-bit mode -- but LRETQ reads the firmware
     * stack after the point of no return. The indirect far jump reads a
     * six-byte cell in this image's own .data, so the descent needs no valid
     * stack at any point. */
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
#ifdef EFI_PCIDE_NEGCTL_LATE_CLEAR
    /* Deliberately wrong production-assembly mutation. Intel hardware requires
     * PCIDE to have been cleared before the MOV to CR0 immediately above; this
     * repair is therefore too late. The guest mutation witness stops before
     * the far jump rather than pretending QEMU TCG models that #GP. */
    movl %cr4, %eax
    andl $~CR4_PCIDE, %eax
    movl %eax, %cr4
#endif
    TXCHAR 'P'

    /* Step 4: clear EFER.LME (bit 8 of MSR 0xC0000080, SDM Vol 4 Table 2-2).
     * Long mode is now neither active nor enabled. */
    movl $0xC0000080, %ecx
    rdmsr
    andl $0xFFFFFEFF, %eax          /* ~(1 << 8) */
    wrmsr
    TXCHAR 'L'

#ifdef EFI_FORCE_LA57_AFTER_PG
    /* Guest-only executable seam. LA57 is changed only after both paging and
     * LME are off. A readback plus breadcrumb proves the control bit really
     * was one before the production hand-off cleanup ran. */
    movl %cr4, %eax
    orl  $CR4_LA57, %eax
    movl %eax, %cr4
    movl %cr4, %eax
    testl $CR4_LA57, %eax
    jz .Lla57_failed32
    TXCHAR '5'
#endif

    /* Not a step of the SDM sequence, but the difference between "protected
     * mode" and "protected mode that still smells of long mode": drop CR4.PAE
     * (bit 5), CR4.PGE (bit 7), and CR4.LA57 (bit 12). The kernel ORs PAE back on at
     * c/boot/boot.asm:105, so leaving it set would in fact be harmless -- this
     * is here so the state handed to `start` is the state GRUB hands it, and so
     * that "it works from GRUB but not from us" can never turn out to be a
     * difference in a control register nobody thought to compare. Clearing PAE
     * is legal only now that LME is off (SDM Vol 3A sec 4.1.2: software must
     * not clear CR4.PAE while IA-32e mode is active). LA57 can be changed only
     * while CR0.PG is clear; that is why this block cannot move upward. */
#if !defined(EFI_LA57_NEGCTL_SKIP_CLEAR) && !defined(EFI_LA57_NEGCTL_EARLY_CLEAR)
    movl %cr4, %eax
    andl $~(CR4_PAE | CR4_PGE | CR4_LA57), %eax
    movl %eax, %cr4
#ifndef EFI_LA57_NEGCTL_SKIP_READBACK
    movl %cr4, %eax
    testl $CR4_LA57, %eax
    jnz .Lla57_failed32
#endif
#endif

#ifdef EFI_LA57_TEST_ASSERT_CLEAR
    /* A fresh test-only readback makes the skip-clear mutation observable in
     * QEMU without treating a later boot failure as proof of its cause. */
    movl %cr4, %eax
    testl $CR4_LA57, %eax
    jnz .Lla57_test_unsafe32
#endif

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

#ifdef EFI_LA57_TEST_ASSERT_CLEAR
.Lla57_test_unsafe32:
    TXCHAR 'U'
    jmp .Lla57_dead32
#endif

.Lla57_failed32:
    TXCHAR 'X'
.Lla57_dead32:
    cli
    hlt
    jmp .Lla57_dead32

    .code64
#ifdef EFI_PCIDE_TEST_ASSERT_CLEAR
.Lpcide_test_unsafe:
    /* Test-only proof that the mutated instruction stream still carried
     * PCIDE=1 immediately before the far jump. Never compiled into BOOTX64.EFI. */
    TXCHAR 'U'
    jmp .Lpcide_dead
#endif

.Lpcide_failed:
    /* The readback cannot be trusted, so neither can the paging transition.
     * There are no boot services after this point and no safe return path. */
    TXCHAR 'X'
.Lpcide_dead:
    cli
    hlt
    jmp .Lpcide_dead

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
