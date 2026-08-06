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

static Value n_addr(int argc, Value *args)
{
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

static Value peek_w(int argc, Value *args, int w)
{
    if (argc != 1 || !IS_INT(args[0])) return as_native_fail("peek expects an integer address");
    return INT_VAL((int64_t)as_ll_peek((uint64_t)AS_INT(args[0]), w));
}
static Value n_peek8 (int c, Value *a) { return peek_w(c, a, 1); }
static Value n_peek16(int c, Value *a) { return peek_w(c, a, 2); }
static Value n_peek32(int c, Value *a) { return peek_w(c, a, 4); }
static Value n_peek64(int c, Value *a) { return peek_w(c, a, 8); }

static Value poke_w(int argc, Value *args, int w)
{
    if (argc != 2 || !IS_INT(args[0]) || !IS_INT(args[1])) return as_native_fail("poke expects (addr, value) integers");
    as_ll_poke((uint64_t)AS_INT(args[0]), w, (uint64_t)AS_INT(args[1]));
    return NIL_VAL;
}
static Value n_poke8 (int c, Value *a) { return poke_w(c, a, 1); }
static Value n_poke16(int c, Value *a) { return poke_w(c, a, 2); }
static Value n_poke32(int c, Value *a) { return poke_w(c, a, 4); }
static Value n_poke64(int c, Value *a) { return poke_w(c, a, 8); }

static Value ptr_w(int argc, Value *args, int w)
{
    if (argc != 1 || !IS_INT(args[0])) return as_native_fail("iNptr expects an integer address");
    return OBJ_VAL(as_ptr_new((uint64_t)AS_INT(args[0]), w, 1));
}
static Value n_i8ptr (int c, Value *a) { return ptr_w(c, a, 1); }
static Value n_i16ptr(int c, Value *a) { return ptr_w(c, a, 2); }
static Value n_i32ptr(int c, Value *a) { return ptr_w(c, a, 4); }
static Value n_i64ptr(int c, Value *a) { return ptr_w(c, a, 8); }

/* M23.5 sys/gui support: raw heap buffers + memory<->string bridges, so pure
 * AetherScript modules (lib/sys.as, lib/gui.as) can marshal syscall arguments
 * (event structs, argv arrays, read buffers) without any C per syscall. */
static Value n_alloc(int argc, Value *args)
{
    if (argc != 1 || !IS_INT(args[0]) || AS_INT(args[0]) <= 0)
        return as_native_fail("alloc() takes a positive byte count");
    void *m = malloc((size_t)AS_INT(args[0]));
    if (!m) return as_native_fail("alloc(): out of memory");
    memset(m, 0, (size_t)AS_INT(args[0]));
    return OBJ_VAL(as_ptr_new((uint64_t)(uintptr_t)m, 1, 0));
}
static Value n_dealloc(int argc, Value *args)
{
    if (argc != 1 || !IS_PTR(args[0])) return as_native_fail("dealloc() takes a pointer from alloc()");
    free((void *)(uintptr_t)AS_PTR(args[0])->addr);
    AS_PTR(args[0])->addr = 0;             /* poison: a reuse faults loudly at 0 */
    return NIL_VAL;
}
static Value n_mem2str(int argc, Value *args)   /* (ptr|addr, len) -> str */
{
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
    long a[4] = { 0, 0, 0, 0 };
    if (argc < 1 || argc > 4) return as_native_fail("syscall() takes 1 to 4 arguments");
    for (int i = 0; i < argc; i++) {
        if (!IS_INT(args[i])) return as_native_fail("syscall() arguments must be integers");
        a[i] = (long)AS_INT(args[i]);
    }
    return INT_VAL((int64_t)as_ll_syscall(a[0], a[1], a[2], a[3]));
}

void as_install_indirection(void)
{
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
    as_define_int("EV_NONE",  EV_NONE);
    as_define_int("EV_KEY",   EV_KEY);
    as_define_int("EV_MOUSE", EV_MOUSE);
    as_define_int("EV_CLOSE", EV_CLOSE);
    as_define_int("EV_MOUSE_R", EV_MOUSE_R);
    as_define_int("EV_THEME", EV_THEME);
}
