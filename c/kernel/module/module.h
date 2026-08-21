#ifndef LOGIT_MODULE_H
#define LOGIT_MODULE_H
/* ===========================================================================
 * Loadable kernel modules.
 *
 * WHY
 *   Everything in this kernel is statically linked, so it can only ever drive
 *   hardware it was compiled with. The registration half of the answer already
 *   existed: DRIVER_DECLARE (c/drivers/core/driver.h) puts a `struct driver *`
 *   in the `logit_drivers` linker section and dev_probe_all() walks it, so a
 *   driver file needs no call in kmain and no line in a central list. What was
 *   missing is everything on the other side of the link step -- getting the
 *   driver's CODE into the running kernel and its symbols resolved.
 *
 * THE FORMAT IS A PLAIN ET_REL ELF64 OBJECT. Not a wrapper around one.
 *   A .ko here is literally the output of `clang -c driver.c`. The object
 *   already carries the section table, the symbol table, the relocations AND
 *   the driver's own `logit_drivers` section -- everything the loader needs.
 *   The rejected alternative was a container like include/aex.h (a header
 *   wrapping an ELF, which is what a ring-3 program uses): it would add a
 *   second place to record the entry points and section list, and two records
 *   of the same fact are a place for them to disagree. The cost of not having
 *   one is that there is no version stamp on the file; nothing here checks
 *   that a module was built against this kernel's headers, and a stale one
 *   whose struct driver layout has changed will relocate cleanly and then
 *   misbehave. That is a real hole and it is named rather than papered over.
 *
 * WHAT elf.c COULD NOT BE REUSED FOR, since the question comes up
 *   c/kernel/exec/elf.c parses PROGRAM headers (PT_LOAD) of ET_EXEC/ET_DYN
 *   images. An ET_REL object has NO program headers at all -- e_phnum is 0 --
 *   and everything a module loader needs lives in the SECTION headers, which
 *   elf.c never looks at and has no types for (there is no Elf64_Shdr,
 *   Elf64_Sym or Elf64_Rela anywhere under c/, measured with grep). So this is
 *   not a copy of elf.c's parsing that drifted from it; it is a disjoint half
 *   of the format. The one genuine overlap is the e_ident sanity check, about
 *   ten lines, duplicated here rather than asking elf.c's owner to export it
 *   for that alone.
 *
 * WHERE THE CODE LIVES, and the measurement that says it is safe
 *   One kmalloc'd block, which is a run of contiguous PMM frames, IDENTITY
 *   MAPPED. On this machine that is physical memory below 512 MiB (Makefile
 *   QEMU_RAM = -m 512M), and the kernel image starts at 1 MiB. Measured on the
 *   three simplest real drivers in this tree, `clang -c` with the kernel's own
 *   flags emits exactly five relocation types and no others:
 *
 *       R_X86_64_PLT32  calls              (S + A - P, must fit int32)
 *       R_X86_64_PC32   rip-relative data  (S + A - P, must fit int32)
 *       R_X86_64_32     string addresses   (S + A,     must fit uint32)
 *       R_X86_64_32S    absolute data      (S + A,     must fit int32)
 *       R_X86_64_64     pointers in .data  (S + A,     64-bit, always fits)
 *
 *   The ABSOLUTE ones are the constraint, not the PC-relative ones, and that
 *   is the opposite of what one expects going in. -fno-pic -mcmodel=small
 *   makes every string literal reference an R_X86_64_32, so the module and
 *   everything it points at must live below 4 GiB (below 2 GiB for 32S). With
 *   512 MiB of RAM every address on the machine is below 0x20000000 and the
 *   widest possible displacement is about 511 MiB, so all five fit with three
 *   orders of magnitude to spare. It is checked at load time per relocation
 *   anyway (see mod_reloc_apply) rather than argued from this comment, because
 *   -m 4G is one Makefile character away and the failure would otherwise be a
 *   silently truncated pointer rather than a refusal.
 *
 * W^X: THERE IS NONE HERE, and the reason is not this file's to fix.
 *   The module's block is executable because the kernel's identity map is
 *   executable: boot.asm maps the first 1 GiB with 2 MiB pages carrying
 *   present|writable and no NX bit, so every byte of kernel heap is already
 *   RWX before this loader exists. Making module text read-only would mean
 *   splitting a huge page in the identity map, which is a change to
 *   c/kernel/mm and to the boot path, not an addition here. Recorded so that
 *   nobody reads the absence as an oversight of this feature.
 *
 * SECURITY, once and plainly
 *   Loading a module is arbitrary ring-0 code execution. That is not a
 *   weakness of this implementation, it is what the feature IS. Two things
 *   stand in front of it and neither is cryptographic:
 *     - the caller must have uid 0 (vfs_cred_current, the same check
 *       SYS_POWEROFF uses). This is meaningful on this machine and not merely
 *       decorative: /bin/login sets the session credential and vfs_meta now
 *       persists modes and owners across a reboot (see CLAUDE.md Storage), so
 *       "root" is a real state a process can fail to be in.
 *     - the image is read by the KERNEL through the VFS from a path, never
 *       handed in as a user buffer. The rejected alternative is Linux's
 *       init_module(void *, size_t), which is a better ABI in every other way
 *       (it decouples the loader from the filesystem) and is wrong here: with
 *       a buffer, the bytes that become ring-0 code never had to exist on
 *       disk, so no file mode ever guards them. With a path, a root-owned
 *       mode-0700 /lib/modules is the boundary, and it is one this machine
 *       can actually keep across a reboot.
 *   There is NO signature check and no hash. Saying so is the point; a
 *   "verified" flag that verified nothing would be worse than the absence.
 * =========================================================================== */

#include <stdint.h>
#include <stddef.h>

/* Errors. Negative, distinct, and each returned from exactly one situation --
 * a loader that returns one code for "not an ELF file" and "an ELF file this
 * loader cannot relocate" makes the first hour of every port a guess. */
#define MOD_E_PERM      -1   /* caller is not uid 0 */
#define MOD_E_NOFILE    -2   /* path does not resolve / cannot be read */
#define MOD_E_TOOBIG    -3   /* image exceeds MOD_MAX_IMAGE */
#define MOD_E_NOMEM     -4   /* kmalloc failed */
#define MOD_E_FORMAT    -5   /* not a little-endian 64-bit x86-64 ET_REL ELF */
#define MOD_E_RELOC     -6   /* an unsupported relocation type */
#define MOD_E_RANGE     -7   /* a relocation did not fit its field */
#define MOD_E_UNDEF     -8   /* an undefined symbol is not exported */
#define MOD_E_FULL      -9   /* the module table is full */
#define MOD_E_NOUNLOAD  -10  /* unload is refused -- see mod_unload() */
#define MOD_E_INVAL     -11  /* bad argument from userland */
#define MOD_E_DUP       -12  /* a module of that name is already loaded */

#define MOD_MAX          8   /* loaded modules; the table is static, see below */
#define MOD_NAME_LEN    32
/* One contiguous kmalloc. The kheap arena is 4 MiB (ARENA_FRAMES 1024) and a
 * request larger than an arena forces a grow that can only be served by
 * pmm_alloc_contig, a linear first-fit with no fallback -- so a cap well under
 * the arena size is the difference between "the module is too big" and "the
 * kernel heap fragmented and something unrelated failed later". The three
 * drivers measured are 3-9 KiB of object file. */
#define MOD_MAX_IMAGE   (1u << 20)

struct kmodule {
    int      id;                    /* 1-based; 0 means the slot is free */
    char     name[MOD_NAME_LEN];    /* basename of the path, extension dropped */
    void    *base;                  /* the one kmalloc'd block */
    uint32_t size;                  /* bytes of that block */
    uint32_t text_off, text_size;   /* so a backtrace can attribute an address */
    int      ndrivers;              /* drivers registered out of logit_drivers */
    int      nbound;                /* devices bound at load time (see refcount) */
};

/* ---------------------------------------------------------- symbol table --
 * ksyms.c. See the long argument there for why this is an EXPLICIT list. */
void *ksym_lookup(const char *name);
int   ksym_count(void);
const char *ksym_name_at(int i);

/* --------------------------------------------------------------- loading -- */
/* Read `path` through the VFS, relocate it, register its drivers and run one
 * probe pass. Returns the module id (>= 1) or a negative MOD_E_*. */
int mod_load(const char *path);

/* REFUSED, always, and the refusal is the honest answer -- see modload.c. */
int mod_unload(int id);

int  mod_count(void);
const struct kmodule *mod_at(int i);
void mod_dump(void);

/* Syscall entry: SYS_MODULE_LOAD / _UNLOAD / _LIST / _SYM. */
long mod_syscall(long n, long a0, long a1, long a2);

/* ---------------------------------------------- the relocatable ELF core --
 * Split out into modelf.c so it can be driven by a host test with no kernel
 * underneath it (make test-modreloc). Everything here is pure: it touches the
 * caller's two buffers and nothing else -- no kmalloc, no kprintf, no VFS.
 *
 * `img`/`imglen` is the whole .o file. `dst`/`dstlen` is where the SHF_ALLOC
 * sections are laid out. Call mod_elf_size() first to learn how big `dst` must
 * be; both calls parse the same headers and must agree, which the host test
 * asserts rather than assumes.
 *
 * `resolve` is called for every undefined symbol and returns its address or
 * NULL. It is a callback so the host test can supply a table of its own and
 * exercise MOD_E_UNDEF without a kernel symbol table in the process. */
typedef void *(*mod_resolve_fn)(const char *name, void *ctx);

/* Bytes needed for the SHF_ALLOC sections, including alignment padding, or a
 * negative MOD_E_*. */
long mod_elf_size(const void *img, uint32_t imglen);

struct mod_layout {
    void    *dst;
    uint32_t dstlen;
    uint32_t text_off, text_size;   /* first SHF_EXECINSTR section */
    void    *drv_start, *drv_stop;  /* the module's own logit_drivers section */
};

/* Lay out + relocate. `out` is filled on success. Negative MOD_E_* on failure,
 * and on MOD_E_UNDEF `*undef` (if non-NULL) names the symbol that was missing
 * -- a loader that says only "undefined symbol" makes the reader diff two
 * `nm` outputs by hand, which is most of the feature's first day. */
int mod_elf_load(const void *img, uint32_t imglen, void *dst, uint32_t dstlen,
                 mod_resolve_fn resolve, void *ctx,
                 struct mod_layout *out, const char **undef);

/* Apply ONE relocation. Exposed because it is the piece with an arithmetic
 * answer per type and a range answer per type, i.e. the piece worth a
 * table-driven test.
 *   `where`  the address being patched (already inside the destination block)
 *   `type`   R_X86_64_*
 *   `S`      the resolved symbol value
 *   `A`      the addend
 * Returns 0, MOD_E_RELOC (type not supported) or MOD_E_RANGE (did not fit). */
int mod_reloc_apply(void *where, uint32_t type, uint64_t S, int64_t A);

/* x86-64 relocation types this loader accepts. Named here rather than only in
 * the .c so the host test asserts against the same constants the loader uses
 * -- a test carrying its own copy of the numbers only proves the copies agree
 * with each other. */
#define R_X86_64_NONE   0
#define R_X86_64_64     1
#define R_X86_64_PC32   2
#define R_X86_64_PLT32  4
#define R_X86_64_32    10
#define R_X86_64_32S   11

#endif /* LOGIT_MODULE_H */
