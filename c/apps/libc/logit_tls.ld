/* Thread-local storage bounds, published to the runtime by the linker.
 *
 * WHY THIS FILE EXISTS. `__thread` on x86-64 compiles to `%fs`-relative
 * addressing at a NEGATIVE displacement: the thread pointer is the TOP of the
 * TLS block, variables live below it, and the displacement the linker bakes in
 * is `offset_in_segment - align_up(memsz, align)`. So a runtime that wants to
 * give a new thread working `__thread` variables has to allocate a block of
 * exactly that size, put the thread pointer at its end, and copy the
 * initialised part (.tdata) in -- and it cannot do any of that without knowing
 * where the PT_TLS segment is and how big it is.
 *
 * There is no auxv here (no AT_PHDR) and mini-libc programs are static, so the
 * usual routes are closed. `__ehdr_start` does resolve with ld.lld, but it
 * points at 0x200000 -- the default image base, outside the private user region
 * this kernel maps programs into (0x40000000-0x7FFFFFFF) -- so walking our own
 * program headers would mean trusting a mapping that is not part of the
 * contract. The linker already knows the three numbers; this asks it for them.
 *
 * `INSERT BEFORE` is what makes it additive: ld.lld keeps its own default
 * layout and only these two output sections are described here, so nothing
 * about where .text or .data land changes.
 *
 * THE ALIGN(64) AT THE END OF .tbss IS LOAD-BEARING, not tidiness. It rounds
 * memsz up to a multiple of 64, which makes align_up(memsz, p_align) == memsz
 * for any p_align <= 64 -- and THAT is what lets the runtime compute the thread
 * pointer as simply `block + (__logit_tls_end - __logit_tls_start)` without
 * having to discover p_align, which no symbol can carry. A __thread object
 * needing more than 64-byte alignment would break the identity; nothing in this
 * tree has one, and c/apps/libc/src/pthread.c checks the resulting layout at
 * runtime with a sentinel rather than trusting this paragraph.
 *
 * Programs link this with `-T c/apps/libc/logit_tls.ld`. A program that does
 * NOT is not broken by it: the three symbols are declared weak in pthread.c and
 * resolve to 0, TLS size reads 0, `__thread` is unavailable, and the sentinel
 * check catches it and says so on stderr instead of writing into the TCB.
 */
SECTIONS
{
  .tdata : ALIGN(64) {
    __logit_tls_start = .;
    *(.tdata .tdata.*)
    __logit_tdata_end = .;
  }
  .tbss : {
    *(.tbss .tbss.*)
    . = ALIGN(64);
    __logit_tls_end = .;
  }
}
INSERT BEFORE .comment;
