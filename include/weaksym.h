/* include/weaksym.h -- one portable spelling for "this dependency may not be
 * linked", because the one this tree used is an ELF-only idiom.
 *
 * THE IDIOM. About ninety declarations under c/ say
 *
 *     void arp_poll(void) __attribute__((weak));
 *     ...
 *     if (arp_poll) arp_poll();
 *
 * so a link that omits the provider resolves the symbol to NULL and the caller
 * takes its other branch. c/kernel/core/settings.c:9-22 states the argument in
 * full: "adding a dependency here cannot break a consumer that has not been
 * told about it". Fifty-one host source lists name c/apps/browser/css_engine.c
 * and do not name css_interp.c (counted at css_engine.c:9, with the Makefile
 * continuations joined), so the idiom is load-bearing, not decorative.
 *
 * WHAT WAS WRONG. An UNDEFINED weak reference is an ELF property. On Mach-O it
 * is a hard link error, and the documented dev host in CLAUDE.md is macOS on
 * Apple Silicon. Measured on this machine, ld-1266.8:
 *
 *     extern int f(int) __attribute__((weak));      -> ld: symbol(s) not found
 *     extern int f(int) __attribute__((weak_import)); -> ld: symbol(s) not found
 *
 * weak_import is NOT the fix -- it is for a symbol a *dylib* may not export,
 * and the object file it produces is byte-identical in `nm -m` to the plain
 * weak one ("(undefined) weak external") which ld still refuses. Also measured
 * and rejected: -ld_classic, -flat_namespace, -dead_strip, a deployment-target
 * floor, and `.linker_option "-U"` from the source (ld prints "unknown linker
 * option from object file ignored" and then fails anyway). Only the LINK-LINE
 * flags -U/-undefined dynamic_lookup work, and the ~100 recipes that would
 * need them are owned by other lines. So the protection the comments promise
 * did not exist on the host, and roughly a dozen host gates died at link on
 * symbols nobody had touched.
 *
 * WHAT THIS IS. On ELF, nothing changes -- LOGIT_WEAK is the same attribute,
 * LOGIT_WEAK_STUB emits no code, and LOGIT_HAVE(f) is (f). The kernel's object
 * files are byte-identical across this change; that was checked with md5, not
 * assumed.
 *
 * On Mach-O the reference is made DEFINED rather than undefined: each site
 * emits a weak definition of a single trapping instruction into its own
 * section, __TEXT,__lgtweak. A real provider's strong definition overrides it
 * exactly as it overrides an ELF weak definition, so "is anybody providing
 * this?" becomes "is this address inside the stub section?", answered through
 * ld's own section$start$/section$end$ bounds. Empty section (every symbol
 * provided) gives lo == hi, an empty range, and everything reports present --
 * checked, because that is the common case and it must not need the section to
 * exist.
 *
 * HOW TO USE IT. Two lines at the declaration, and LOGIT_HAVE at every guard:
 *
 *     void arp_poll(void) LOGIT_WEAK;
 *     LOGIT_WEAK_STUB(arp_poll);
 *     ...
 *     if (LOGIT_HAVE(arp_poll)) arp_poll();
 *
 * A guard that is left as a bare `if (arp_poll)` still COMPILES and is simply
 * always true on Mach-O, so it calls the stub and takes SIGTRAP on the spot.
 * That is deliberate: the failure is loud and points at the missing guard,
 * rather than a stub quietly returning 0 where the caller wanted a default.
 * It costs nothing on ELF, where the bare guard was already correct.
 *
 * SUPPRESSING ONE STUB, and why the escape hatch has to exist. Four host gates
 * are WHITE-BOX -- tests/unit/raw_test.c #includes route.c, ip.c, reasm.c,
 * icmp.c and raw.c into ONE translation unit -- so ip.c's stub for icmp_input
 * and icmp.c's real definition of it land in the same assembly file, and the
 * assembler says "symbol '_icmp_input' is already defined". A weak definition
 * only yields to a strong one ACROSS objects; inside one it is a redefinition,
 * and that is true of __attribute__((weak)) on a C definition too. Such a TU
 * writes, before its #includes,
 *
 *     #define LOGIT_WEAK_LOCAL_icmp_input 1
 *
 * and that one stub is not emitted -- correctly, because in that link the
 * symbol is not undefined. It is per SYMBOL and not per file on purpose: the
 * same TU still needs the stubs for tcp_input and udp_input, which nothing in
 * it defines. The macro must expand to 1 (or be undefined); anything else
 * reads as undefined.
 *
 * The header is included by RELATIVE PATH ("../../include/weaksym.h"), not by
 * basename. The kernel's INCDIRS would resolve the basename, but every host
 * gate builds with its own narrow -I list (BTEST_INC is five directories,
 * test-vfs-mount is two), and those recipes live in the tests fragments, which
 * this change does not own. c/kernel/core/settings.c:63 already reaches the
 * tree the same way.
 */
#ifndef LOGIT_WEAKSYM_H
#define LOGIT_WEAKSYM_H

/* "Is LOGIT_WEAK_LOCAL_<name> defined to 1?", answered in the preprocessor
 * because #if cannot be written inside a macro. This is Linux's IS_ENABLED
 * trick: a macro defined to 1 pastes into a placeholder that ends in a comma
 * and so shifts the argument list by one; anything else pastes into a token
 * that is not a macro and does not. The third argument to LOGIT_WEAK_2ND_ is a
 * filler so that __VA_ARGS__ is never empty, which C99 does not allow. */
#define LOGIT_WEAK_PH_1              0,
#define LOGIT_WEAK_2ND_(a, val, ...) val
#define LOGIT_WEAK_ISDEF(x)          LOGIT_WEAK_ISDEF_(x)
#define LOGIT_WEAK_ISDEF_(v)         LOGIT_WEAK_ISDEF__(LOGIT_WEAK_PH_##v)
#define LOGIT_WEAK_ISDEF__(a)        LOGIT_WEAK_2ND_(a 1, 0, 0)

#define LOGIT_WEAK_STUB(name)                                                \
    LOGIT_WEAK_STUB_SEL_(LOGIT_WEAK_ISDEF(LOGIT_WEAK_LOCAL_##name), name)
#define LOGIT_WEAK_STUB_SEL_(c, name)  LOGIT_WEAK_STUB_SEL__(c, name)
#define LOGIT_WEAK_STUB_SEL__(c, name) LOGIT_WEAK_STUB_##c(name)

/* Suppressed, and on ELF always: declares a struct tag and nothing else, so
 * the call site's trailing `;` is the whole statement and no storage, symbol
 * or diagnostic is produced. */
#define LOGIT_WEAK_STUB_1(name)  struct logit_weak_unused_##name

#if defined(__MACH__)

/* ld synthesises these two for any section named in this form. They are
 * declared as arrays so the comparison below is on object pointers. */
extern const char logit_weak_lo[] __asm__("section$start$__TEXT$__lgtweak");
extern const char logit_weak_hi[] __asm__("section$end$__TEXT$__lgtweak");

static inline int logit_weak_is_stub(const void *p)
{
    return (const char *)p >= logit_weak_lo && (const char *)p < logit_weak_hi;
}

/* weak_import rather than weak on the declaration only so that clang does not
 * warn about a weak declaration of a symbol this TU also defines below; the
 * link behaviour comes entirely from the stub.
 *
 * SPELLED WITH THE UNDERSCORES, in both branches. mini-libc's
 * c/apps/libc/include/features.h #defines the bare name `weak`, and it is
 * force-included into every browser TU (-include features.h), so
 * __attribute__((weak)) expands to __attribute__((__attribute__((__weak__))))
 * there and stops compiling. js_dom.c:3436 records that costing a session. */
#  define LOGIT_WEAK        __attribute__((__weak_import__))

#  if defined(__aarch64__) || defined(__arm64__)
#    define LOGIT_WEAK_TRAP_ "  brk #0\n"
#    define LOGIT_WEAK_ALIGN_ ".p2align 2\n"
#  elif defined(__x86_64__)
#    define LOGIT_WEAK_TRAP_ "  ud2\n"
#    define LOGIT_WEAK_ALIGN_ ".p2align 4\n"
#  else
#    error "weaksym.h: add a trap instruction for this Mach-O architecture"
#  endif

/* The stub. `.weak_definition` is Mach-O's weak-definition marker, so a strong
 * definition from any other object in the link wins and this body is dropped.
 * `used` is not needed -- module-level asm is never dead-stripped by the
 * compiler -- but the section must be restored to __text afterwards or the
 * next function emitted in this TU lands in __lgtweak and reads as absent. */
#  define LOGIT_WEAK_STUB_0(name)                                            \
      __asm__(".section __TEXT,__lgtweak,regular,pure_instructions\n"        \
              LOGIT_WEAK_ALIGN_                                              \
              ".globl _" #name "\n"                                          \
              ".weak_definition _" #name "\n"                                \
              "_" #name ":\n"                                                \
              LOGIT_WEAK_TRAP_                                               \
              ".text")

#  define LOGIT_HAVE(f)     (!logit_weak_is_stub((const void *)(f)))

#else  /* ELF: the tree's original behaviour, unchanged */

#  define LOGIT_WEAK             __attribute__((__weak__))
#  define LOGIT_WEAK_STUB_0(name) LOGIT_WEAK_STUB_1(name)
#  define LOGIT_HAVE(f)          (f)

#endif

#endif /* LOGIT_WEAKSYM_H */
