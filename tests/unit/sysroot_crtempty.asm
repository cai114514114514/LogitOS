; sysroot_crtempty.asm -- the object installed as /usr/lib/crti.o AND crtn.o.
;
; tcc links every executable as  crt1.o crti.o <user files> -lc libtcc1.a crtn.o
; (third_party/tcc/libtcc.c:974-980, tccelf.c:1206) and fails the link if any
; of the three crt files is missing from /usr/lib. On glibc, crti.o/crtn.o are
; the prologue and epilogue of _init/_fini, split across two objects so that
; .init sections contributed by the libraries in between land inside them.
; This machine has no _init/_fini: crt0_cli.asm (installed as crt1.o) reads
; argc/argv/envp, calls main and exits, and nothing runs constructors.
;
; So both are EMPTY. Assembled by nasm rather than written as a hand-made ELF
; by tools/mksysroot.py so that the tree's own assembler vouches for the
; object's shape; an empty .text is enough for tcc's loader, which requires
; only a well-formed relocatable with any number of sections, including none
; it needs (tccelf.c tcc_load_object_file).
section .text
