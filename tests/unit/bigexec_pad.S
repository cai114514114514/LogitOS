/* The blob, as .rodata so it becomes a real read-only PT_LOAD the loader has to
 * map and the filesystem has to actually hold -- see bigexec_pad.c for why
 * .bss would prove nothing.
 *
 * .balign 4096 is not cosmetic: elf.c's elf_file_runs() only hands whole pages
 * to the page cache, and a segment whose file offset is not page-congruent with
 * its virtual address is refused outright (PASS 0). Aligning here is what makes
 * this binary exercise the file-backed path rather than silently measuring only
 * the copy path.
 *
 * PADFILE comes from -D on the command line so one source builds every size;
 * clang runs the preprocessor over .S, which is why this is .S and not .s. */
    .section .rodata
    .globl pad_blob
    .balign 4096
pad_blob:
    .incbin PADFILE
    .globl pad_end
pad_end:
    .byte 0
