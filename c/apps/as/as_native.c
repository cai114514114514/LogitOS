#include "as.h"
#include "logit_abi.h"   /* SYS_* numbers (shared kernel ABI) */

/* The syscall numbers above are an identity, not a copy -- as_define_int()
 * below hands the kernel's own SYS_* to the script, so they cannot drift. This
 * makes the struct LAYOUTS the same kind of identity: every offset and size in
 * the generated fsroot/as/lib/abi.as is asserted here against the real struct,
 * by this very compile. A kernel that reorders a field breaks the build instead
 * of leaving a script reading the wrong bytes.
 *   regenerate: python3 tools/gen_abi.py --write   verify: make check-abi */
#include "abi_layout.inc"

/* A3 indirection builtins: raw memory (peek/poke), the address of an object's
 * backing store (addr), typed pointers (iNptr -> p[i]), and direct Logit syscalls
 * (syscall). This is what lets AetherScript do systems programming, not just compute. */

/* --- M28 D4/D5: CAP_RAW gates every native in this file, per call, HERE. ----
 *
 * peek/poke do not reach the kernel -- as_ll_peek/as_ll_poke (as_ll.c) are
 * plain `volatile` dereferences in this process's own address space. There is
 * no syscall to intercept, so a kernel-side gate (the D6 category-bit check at
 * syscall_dispatch) cannot see them at all: this is the ONE place the check
 * can live. syscall() is the other half -- it reaches open/pipe/execve/fork
 * directly, with no port and no capability object involved, so a check placed
 * only in as_port.c's natives would be bypassed by any script that just calls
 * syscall() itself. Both halves are gated by the SAME bit, AS_CAP_RAW,
 * because both are the same kind of hole: a way to touch memory or the kernel
 * that nothing else in the language mediates.
 *
 * The check has to sit HERE and not lower or higher:
 *   - not in as_ll.c: as_ll_peek/poke are real dereferences on the HOST build
 *     too (that's what makes peek/poke testable without QEMU at all). An
 *     ungated call reaching as_ll_peek from a denied script would SIGSEGV the
 *     host test binary instead of producing a clean, catchable failure line.
 *   - not once at as_install_indirection() time: that runs at VM startup,
 *     before any script code has executed and before as_caps_set() has
 *     necessarily even been called by the embedder in the order the embedder
 *     chooses to call things. A check there would either misfire on a grant
 *     that arrives after registration, or -- worse -- pass once and then
 *     permit every access for the rest of the run, because nothing re-checks
 *     it. The capability has to be live-checked against the CURRENT held set
 *     on every call, because the whole point of a capability is that it can
 *     be narrower for this call than it was for the process a moment ago
 *     (D1's attenuation chain has no other way to bite).
 *
 * A denial is a catchable as_native_fail() that NAMES the missing capability
 * -- never a refusal to register the native (no `as_define_native` call
 * skipped). Two reasons this matters: an "undefined variable" error is
 * indistinguishable from a typo in the script, giving no signal that a
 * capability was the issue at all; and a script that captured the native's
 * Value into a variable BEFORE any check could run (natives are ordinary
 * global values once defined) would keep calling it successfully forever if
 * the gate were "don't register" rather than "check on every invocation".
 *
 * Returns 1 (granted -- caller proceeds) or 0 (denied -- as_native_fail is
 * already armed with a message naming the capability and the caller must
 * return immediately, same as any other `as_native_fail` call site in this
 * file). A plain int, not a Value: as_native_fail always returns NIL_VAL
 * whether it succeeds or fails, so a Value return here could not tell the
 * caller which happened, and every call site needs to know. */
static int require_raw(const char *what)
{
    /* NEGATIVE CONTROL, and it belongs here as much as in as_port.c. Until this
     * guard existed, -DAS_CAP_NO_CHECK removed only the PORT-side checks, so
     * `make test-as-cap-negctl` still passed every CAP_RAW assertion and
     * therefore under-reported: a control that disables half the checks proves
     * the battery detects that half and says nothing about the rest. The whole
     * capability system has to be removable in one flag or the control is
     * measuring an arbitrary subset of itself. */
#ifdef AS_CAP_NO_CHECK
    (void)what;
    return 1;
#else
    if (as_caps_have(AS_CAP_RAW)) return 1;
    char msg[192];
    snprintf(msg, sizeof msg,
             "%s() needs capability 'raw' (CAP_RAW): it reaches outside the VM's own "
             "control and no syscall or port exists to gate it here (M28 spec D4/D5)",
             what);
    as_native_fail(msg);
    return 0;
#endif
}

static Value n_addr(int argc, Value *args)
{
    if (!require_raw("addr")) return NIL_VAL;
    if (argc != 1) return as_native_fail("addr() takes 1 argument");
    Value v = args[0];
    if (IS_STR(v))  return INT_VAL((int64_t)(uintptr_t)AS_STR(v)->chars);
    if (IS_LIST(v)) return INT_VAL((int64_t)(uintptr_t)AS_LIST(v)->items);
    if (IS_PTR(v))  return INT_VAL((int64_t)AS_PTR(v)->addr);
    /* A buffer's address is the whole point of it: this is the pointer a syscall
     * receives, and for a layout it is exactly the kernel's struct pointer. */
    if (IS_BUF(v))  return INT_VAL((int64_t)(uintptr_t)AS_BUF(v)->raw);
    return as_native_fail("addr() needs a string, list, buffer, or pointer");
}

static Value peek_w(int argc, Value *args, int w, const char *name)
{
    if (!require_raw(name)) return NIL_VAL;
    if (argc != 1 || !IS_INT(args[0])) return as_native_fail("peek expects an integer address");
    return INT_VAL((int64_t)as_ll_peek((uint64_t)AS_INT(args[0]), w));
}
static Value n_peek8 (int c, Value *a) { return peek_w(c, a, 1, "peek8"); }
static Value n_peek16(int c, Value *a) { return peek_w(c, a, 2, "peek16"); }
static Value n_peek32(int c, Value *a) { return peek_w(c, a, 4, "peek32"); }
static Value n_peek64(int c, Value *a) { return peek_w(c, a, 8, "peek64"); }

static Value poke_w(int argc, Value *args, int w, const char *name)
{
    if (!require_raw(name)) return NIL_VAL;
    if (argc != 2 || !IS_INT(args[0]) || !IS_INT(args[1])) return as_native_fail("poke expects (addr, value) integers");
    as_ll_poke((uint64_t)AS_INT(args[0]), w, (uint64_t)AS_INT(args[1]));
    return NIL_VAL;
}
static Value n_poke8 (int c, Value *a) { return poke_w(c, a, 1, "poke8"); }
static Value n_poke16(int c, Value *a) { return poke_w(c, a, 2, "poke16"); }
static Value n_poke32(int c, Value *a) { return poke_w(c, a, 4, "poke32"); }
static Value n_poke64(int c, Value *a) { return poke_w(c, a, 8, "poke64"); }

static Value ptr_w(int argc, Value *args, int w, const char *name)
{
    if (!require_raw(name)) return NIL_VAL;
    if (argc != 1 || !IS_INT(args[0])) return as_native_fail("iNptr expects an integer address");
    return OBJ_VAL(as_ptr_new((uint64_t)AS_INT(args[0]), w, 1));
}
static Value n_i8ptr (int c, Value *a) { return ptr_w(c, a, 1, "i8ptr"); }
static Value n_i16ptr(int c, Value *a) { return ptr_w(c, a, 2, "i16ptr"); }
static Value n_i32ptr(int c, Value *a) { return ptr_w(c, a, 4, "i32ptr"); }
static Value n_i64ptr(int c, Value *a) { return ptr_w(c, a, 8, "i64ptr"); }

/* M23.5 sys/gui support: raw heap buffers + memory<->string bridges, so pure
 * AetherScript modules (lib/sys.as, lib/gui.as) can marshal syscall arguments
 * (event structs, argv arrays, read buffers) without any C per syscall.
 *
 * M28: alloc()/dealloc() are NOT deleted (spec s8's out-of-scope list is
 * explicit about this) -- they become CAP_RAW-gated legacy beside region()/
 * buffer() (the GC-owned ObjBuf replacement, vm.c). Their footprint is one
 * script and a handful of host tests; retiring them is a migration, and
 * migrations do not belong in the batch that changes the opcode set. They sit
 * on bare malloc'd memory with no GC accounting at all, which is exactly the
 * kind of unmediated access CAP_RAW exists to gate. */
static Value n_alloc(int argc, Value *args)
{
    if (!require_raw("alloc")) return NIL_VAL;
    if (argc != 1 || !IS_INT(args[0]) || AS_INT(args[0]) <= 0)
        return as_native_fail("alloc() takes a positive byte count");
    void *m = malloc((size_t)AS_INT(args[0]));
    if (!m) return as_native_fail("alloc(): out of memory");
    memset(m, 0, (size_t)AS_INT(args[0]));
    return OBJ_VAL(as_ptr_new((uint64_t)(uintptr_t)m, 1, 0));
}
static Value n_dealloc(int argc, Value *args)
{
    if (!require_raw("dealloc")) return NIL_VAL;
    if (argc != 1 || !IS_PTR(args[0])) return as_native_fail("dealloc() takes a pointer from alloc()");
    free((void *)(uintptr_t)AS_PTR(args[0])->addr);
    AS_PTR(args[0])->addr = 0;             /* poison: a reuse faults loudly at 0 */
    return NIL_VAL;
}
static Value n_mem2str(int argc, Value *args)   /* (ptr|addr, len) -> str */
{
    /* Gated like peek: this reads whatever `len` bytes sit at a caller-supplied
     * address, exactly as peek does one word at a time (M28 spec D4's own note
     * that the original design document's "gate peek/poke/addr" list is
     * incomplete -- mem2str/mem2cstr read arbitrary memory too). */
    if (!require_raw("mem2str")) return NIL_VAL;
    /* len is int64 but ObjStr.len/as_str_copy take int: reject values that would
     * truncate (e.g. 4294967295 -> -1 -> memcpy of SIZE_MAX). */
    if (argc != 2 || !IS_INT(args[1]) || AS_INT(args[1]) < 0 || AS_INT(args[1]) > INT32_MAX)
        return as_native_fail("mem2str() takes (ptr_or_addr, len)");
    uint64_t a = IS_PTR(args[0]) ? AS_PTR(args[0])->addr
               : IS_BUF(args[0]) ? (uint64_t)(uintptr_t)AS_BUF(args[0])->raw
               : IS_INT(args[0]) ? (uint64_t)AS_INT(args[0]) : 0;
    if (!a) return as_native_fail("mem2str() takes (ptr_or_addr, len)");
    ObjStr *s = as_str_copy((const char *)(uintptr_t)a, (int)AS_INT(args[1]));
    return s ? OBJ_VAL(s) : NIL_VAL;
}
static Value n_mem2cstr(int argc, Value *args)  /* (ptr|addr) -> str up to the NUL */
{
    if (!require_raw("mem2cstr")) return NIL_VAL;  /* same reasoning as mem2str above */
    if (argc != 1) return as_native_fail("mem2cstr() takes a pointer or address");
    uint64_t a = IS_PTR(args[0]) ? AS_PTR(args[0])->addr
               : IS_BUF(args[0]) ? (uint64_t)(uintptr_t)AS_BUF(args[0])->raw
               : IS_INT(args[0]) ? (uint64_t)AS_INT(args[0]) : 0;
    if (!a) return as_native_fail("mem2cstr() takes a pointer or address");
    const char *p = (const char *)(uintptr_t)a;
    int n = 0;
    while (n < 4096 && p[n]) n++;          /* bounded: a missing NUL can't run away */
    ObjStr *s = as_str_copy(p, n);
    return s ? OBJ_VAL(s) : NIL_VAL;
}

static Value n_syscall(int argc, Value *args)
{
    /* M28 spec D5: syscall() reaches open/pipe/execve/fork -- everything the
     * port constructors reach -- with NO port and NO capability object in the
     * way, and it takes a bare integer (fsroot/as/examples/setcheck.as hand-
     * codes SYS_SETTING_GET as 103 for exactly this reason), so no table keyed
     * on the ~60 SYS_* names as_native.c happens to know about could ever cover
     * it. It is the universal bypass of every other language-level check in
     * this codebase and is classed with the other universal bypass, CAP_RAW,
     * rather than with the finer-grained FS/NET/PROC/GUI bits. */
    if (!require_raw("syscall")) return NIL_VAL;
    long a[4] = { 0, 0, 0, 0 };
    if (argc < 1 || argc > 4) return as_native_fail("syscall() takes 1 to 4 arguments");
    for (int i = 0; i < argc; i++) {
        if (!IS_INT(args[i])) return as_native_fail("syscall() arguments must be integers");
        a[i] = (long)AS_INT(args[i]);
    }
    return INT_VAL((int64_t)as_ll_syscall(a[0], a[1], a[2], a[3]));
}

/* caps() -- the process's own held set, as a Value.
 *
 * DELIBERATELY NOT require_raw()-GATED, and the reason is the whole argument for
 * why it is safe to expose at all: it reports what this process was already
 * granted, and every gated native consults that same held set directly on every
 * call. A script that can call caps() could already do everything the returned
 * capability describes, so the call confers nothing. What it enables is the
 * opposite -- narrowing, via the methods on the returned value, so that a script
 * can hand something else strictly less than it holds. Gating the only way to
 * REDUCE authority behind the most powerful capability would be exactly
 * backwards. See as_caps_value() in object.c. */
static Value n_caps(int argc, Value *args)
{
    (void)args;
    if (argc != 0) return as_native_fail("caps() takes no arguments");
    ObjCap *c = as_caps_value();
    return c ? OBJ_VAL(c) : NIL_VAL;           /* NULL => g_oom already set */
}

void as_install_indirection(void)
{
    as_define_native("caps", n_caps);
    as_define_native("addr", n_addr);
    as_define_native("peek8",  n_peek8);  as_define_native("poke8",  n_poke8);
    as_define_native("peek16", n_peek16); as_define_native("poke16", n_poke16);
    as_define_native("peek32", n_peek32); as_define_native("poke32", n_poke32);
    as_define_native("peek64", n_peek64); as_define_native("poke64", n_poke64);
    as_define_native("i8ptr",  n_i8ptr);  as_define_native("i16ptr", n_i16ptr);
    as_define_native("i32ptr", n_i32ptr); as_define_native("i64ptr", n_i64ptr);
    as_define_native("syscall", n_syscall);
    as_define_native("alloc",    n_alloc);
    as_define_native("dealloc",  n_dealloc);
    as_define_native("mem2str",  n_mem2str);
    as_define_native("mem2cstr", n_mem2cstr);

    /* M28 capability classes, injected the same way the SYS_* numbers are and
     * for the same reason: a script narrowing a capability has to name the bit
     * it is keeping, and `c.without(32)` is the setcheck.as anti-pattern -- a
     * bare integer with no symbolic name anywhere in the tree. */
    as_define_int("CAP_FS_READ",  AS_CAP_FS_READ);
    as_define_int("CAP_FS_WRITE", AS_CAP_FS_WRITE);
    as_define_int("CAP_NET",      AS_CAP_NET);
    as_define_int("CAP_PROC",     AS_CAP_PROC);
    as_define_int("CAP_GUI",      AS_CAP_GUI);
    as_define_int("CAP_RAW",      AS_CAP_RAW);

    as_define_int("SYS_WRITE",  SYS_WRITE);
    as_define_int("SYS_READ",   SYS_READ);
    as_define_int("SYS_OPEN",   SYS_OPEN);
    as_define_int("SYS_CLOSE",  SYS_CLOSE);
    as_define_int("SYS_LSEEK",  SYS_LSEEK);
    as_define_int("SYS_EXIT",   SYS_EXIT);
    as_define_int("SYS_YIELD",  SYS_YIELD);
    as_define_int("SYS_GETPID", SYS_GETPID);
    as_define_int("SYS_FORK",   SYS_FORK);
    as_define_int("SYS_MKDIR",  SYS_MKDIR);
    as_define_int("SYS_GETCWD", SYS_GETCWD);

    /* M23.5: the full system surface for lib/sys.as + lib/gui.as. */
    as_define_int("SYS_EXECVE",      SYS_EXECVE);
    as_define_int("SYS_WAITPID",     SYS_WAITPID);
    as_define_int("SYS_PIPE",        SYS_PIPE);
    as_define_int("SYS_DUP2",        SYS_DUP2);
    as_define_int("SYS_CHDIR",       SYS_CHDIR);
    as_define_int("SYS_READ_FILE",   SYS_READ_FILE);
    as_define_int("SYS_WRITE_FILE",  SYS_WRITE_FILE);
    as_define_int("SYS_DELETE_FILE", SYS_DELETE_FILE);
    as_define_int("SYS_RENAME",      SYS_RENAME);
    as_define_int("SYS_DIR_COUNT",   SYS_DIR_COUNT);
    as_define_int("SYS_DIR_NAME",    SYS_DIR_NAME);
    as_define_int("SYS_GET_TIME",    SYS_GET_TIME);
    as_define_int("SYS_MONOTONIC_MS", SYS_MONOTONIC_MS);
    as_define_int("SYS_NET_INFO",    SYS_NET_INFO);
    as_define_int("SYS_NET_PING",    SYS_NET_PING);
    as_define_int("SYS_NET_PING_RTT",SYS_NET_PING_RTT);
    as_define_int("SYS_NET_DNS",     SYS_NET_DNS);
    as_define_int("SYS_NET_DNS_RESULT", SYS_NET_DNS_RESULT);
    as_define_int("SYS_CPU_INDEX",   SYS_CPU_INDEX);
    as_define_int("SYS_UI_DARK",     SYS_UI_DARK);
    as_define_int("SYS_GUI_CREATE",  SYS_GUI_CREATE);
    as_define_int("SYS_GUI_CLEAR",   SYS_GUI_CLEAR);
    as_define_int("SYS_GUI_RECT",    SYS_GUI_RECT);
    as_define_int("SYS_GUI_TEXT",    SYS_GUI_TEXT);
    as_define_int("SYS_GUI_TEXT_MONO", SYS_GUI_TEXT_MONO);
    as_define_int("SYS_GUI_FLUSH",   SYS_GUI_FLUSH);
    as_define_int("SYS_GUI_ICON",    SYS_GUI_ICON);
    as_define_int("SYS_GUI_GLASS",   SYS_GUI_GLASS);
    as_define_int("SYS_POLL_EVENT",  SYS_POLL_EVENT);
    as_define_int("SYS_WAIT_EVENT",  SYS_WAIT_EVENT);
    /* Every remaining symbol a generated abi.as wrapper can name: the wrappers
     * are only resolved at call time, so a missing define here is an
     * "undefined variable" at first call rather than a build error. Keep this
     * list in step with include/abi/logit_calls.abi. */
    as_define_int("SYS_GET_ARG",     SYS_GET_ARG);
    as_define_int("SYS_SYSINFO",     SYS_SYSINFO);
    as_define_int("SYS_FILE_COUNT",  SYS_FILE_COUNT);
    as_define_int("SYS_FILE_NAME",   SYS_FILE_NAME);
    as_define_int("SYS_SPAWN",       SYS_SPAWN);
    as_define_int("SYS_DUP",         SYS_DUP);
    as_define_int("SYS_SETNB",       SYS_SETNB);
    as_define_int("SYS_FSYNC",       SYS_FSYNC);
    as_define_int("SYS_OPEN_PATH",   SYS_OPEN_PATH);
    as_define_int("SYS_IMG_DECODE",  SYS_IMG_DECODE);
    as_define_int("SYS_KHEAP_STRESS", SYS_KHEAP_STRESS);
    as_define_int("SYS_HTTP_GET",    SYS_HTTP_GET);
    as_define_int("SYS_HTTP_STATUS", SYS_HTTP_STATUS);
    as_define_int("SYS_HTTP_BODY",   SYS_HTTP_BODY);
    as_define_int("SYS_RES_FETCH",   SYS_RES_FETCH);
    as_define_int("SYS_TEXT_MEASURE", SYS_TEXT_MEASURE);
    as_define_int("SYS_GUI_TEXT_RUN", SYS_GUI_TEXT_RUN);
    as_define_int("SYS_GUI_BLIT",    SYS_GUI_BLIT);
    as_define_int("SYS_GUI_RRECT",   SYS_GUI_RRECT);
    as_define_int("SYS_GUI_CLIP",    SYS_GUI_CLIP);
    as_define_int("EV_NONE",  EV_NONE);
    as_define_int("EV_KEY",   EV_KEY);
    as_define_int("EV_MOUSE", EV_MOUSE);
    as_define_int("EV_CLOSE", EV_CLOSE);
    as_define_int("EV_MOUSE_R", EV_MOUSE_R);
    as_define_int("EV_THEME", EV_THEME);
    as_define_int("EV_MOUSE_UP", EV_MOUSE_UP);
    as_define_int("EV_MOUSE_MOVE", EV_MOUSE_MOVE);
    as_define_int("EV_WHEEL", EV_WHEEL);
    /* The two side fields of an event: which button, and which modifiers. A
     * script reads them off the Event layout in abi.as, so it needs the names
     * for the bits as much as it needs the names for the types. */
    as_define_int("EV_MOD_SHIFT", EV_MOD_SHIFT);
    as_define_int("EV_MOD_CTRL", EV_MOD_CTRL);
    as_define_int("EV_MOD_ALT", EV_MOD_ALT);
    as_define_int("EV_BTN_NONE", EV_BTN_NONE);
    as_define_int("EV_BTN_LEFT", EV_BTN_LEFT);
    as_define_int("EV_BTN_RIGHT", EV_BTN_RIGHT);
    as_define_int("EV_BTN_MIDDLE", EV_BTN_MIDDLE);

    /* THE EIGHTEEN THAT WERE MISSING, and why they were missed for so long.
     *
     * Every name above is here because somebody remembered. The list two
     * comments up already asked the next person to "keep this list in step
     * with include/abi/logit_calls.abi", and by the time anyone diffed the two
     * (2026-08-16, while adding SYS_STAT for lib/image.as) eighteen generated
     * wrappers named a symbol that had no binding at all: the whole socket
     * family, the whole thread family, the futex, getrandom, TLS base, both
     * window-state calls, and stat.
     *
     * NONE OF THAT WAS A BUILD ERROR. abi.as is data -- a wrapper body is only
     * resolved when it is CALLED -- so `sock_open(...)` from a script compiled,
     * shipped, and then failed at the call site with `undefined variable
     * 'SYS_SOCK_OPEN'`, which is precisely what a typo in the script looks
     * like. The author of the script has no reason to suspect the library.
     *
     * That is why the fix is not only these lines. tools/gen_abi.py --check now
     * diffs the SYS_* names abi.as CALLS against the names this function
     * DEFINES and fails when one is unbound, so the next `call` line added
     * without its define stops `make check-abi` instead of stopping a user.
     * The check found all eighteen on its first run. */
    as_define_int("SYS_STAT",           SYS_STAT);
    as_define_int("SYS_GUI_WIN_MIN",    SYS_GUI_WIN_MIN);
    as_define_int("SYS_GUI_WIN_STATE",  SYS_GUI_WIN_STATE);
    as_define_int("SYS_SOCK_OPEN",      SYS_SOCK_OPEN);
    as_define_int("SYS_SOCK_POLL",      SYS_SOCK_POLL);
    as_define_int("SYS_SOCK_SEND",      SYS_SOCK_SEND);
    as_define_int("SYS_SOCK_RECV",      SYS_SOCK_RECV);
    as_define_int("SYS_SOCK_ALPN",      SYS_SOCK_ALPN);
    as_define_int("SYS_SOCK_CLOSE",     SYS_SOCK_CLOSE);
    as_define_int("SYS_THREAD_CREATE",  SYS_THREAD_CREATE);
    as_define_int("SYS_THREAD_EXIT",    SYS_THREAD_EXIT);
    as_define_int("SYS_THREAD_JOIN",    SYS_THREAD_JOIN);
    as_define_int("SYS_THREAD_DETACH",  SYS_THREAD_DETACH);
    as_define_int("SYS_THREAD_SELF",    SYS_THREAD_SELF);
    as_define_int("SYS_THREAD_INFO",    SYS_THREAD_INFO);
    as_define_int("SYS_SET_TLS",        SYS_SET_TLS);
    as_define_int("SYS_FUTEX",          SYS_FUTEX);
    as_define_int("SYS_GETRANDOM",      SYS_GETRANDOM);

    /* File-type bits of logit_stat.mode (LST_IFMT & friends, include/abi/
     * logit_abi.h). Named for the same reason CAP_* are: `(st.mode & 61440) ==
     * 16384` is a directory test nobody can read, and the alternative to naming
     * them here is every script re-deriving POSIX's octal constants. */
    as_define_int("LST_IFMT",  LST_IFMT);
    as_define_int("LST_IFREG", LST_IFREG);
    as_define_int("LST_IFDIR", LST_IFDIR);
    as_define_int("LST_IFLNK", LST_IFLNK);
}
