; A .aex emitted by a TOOL, not by a linker.
;
; docs/superpowers/specs/2026-08-05-aetherscript-2-language-design.md §P4/M30
; wants `as -c --native` to produce a real ring-3 binary, "legitimate precisely
; because we own the ABI, the ELF/aex loader, and the page tables". The format
; question that raises -- can something that is not ld.lld produce a loadable
; file? -- is answered by tools/mkaex.py --emit, which builds a minimal ELF64
; out of flat blobs and wraps it.
;
; This is the proof, and it is deliberately the crudest possible producer: nasm
; assembles it to a FLAT binary with no ELF, no sections and no symbol table --
; exactly what a code generator has at the end, a buffer of bytes and an entry
; offset. mkaex --emit turns that into something the kernel loads and the disk
; carries, and tests/boot/run-exec-test.sh runs it on the machine.
;
; It is also the loader's smallest possible input: one PT_LOAD and a
; PT_GNU_STACK. If the loader ever grows a requirement that only lld's output
; happens to satisfy, this is what stops loading.

bits 64
org 0x50000000                  ; the common CLI link base (see CLI_RULE)

_start:
    mov eax, 1                  ; SYS_WRITE
    mov edi, 1                  ; fd 1 -> the serial console
    lea rsi, [rel msg]
    mov edx, msglen
    int 0x80

    mov eax, 2                  ; SYS_EXIT
    xor edi, edi
    int 0x80
.hang:
    jmp .hang

msg:    db "ASNATIVE-OK: loaded from an .aex a compiler could have emitted", 10
msglen  equ $ - msg
