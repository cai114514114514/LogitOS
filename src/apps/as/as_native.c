#include "as.h"
#include "aether_abi.h"   /* SYS_* numbers (shared kernel ABI) */

/* A3 indirection builtins: raw memory (peek/poke), the address of an object's
 * backing store (addr), typed pointers (iNptr -> p[i]), and direct Aether syscalls
 * (syscall). This is what lets AetherScript do systems programming, not just compute. */

static Value n_addr(int argc, Value *args)
{
    if (argc != 1) return as_native_fail("addr() takes 1 argument");
    Value v = args[0];
    if (IS_STR(v))  return INT_VAL((int64_t)(uintptr_t)AS_STR(v)->chars);
    if (IS_LIST(v)) return INT_VAL((int64_t)(uintptr_t)AS_LIST(v)->items);
    if (IS_PTR(v))  return INT_VAL((int64_t)AS_PTR(v)->addr);
    return as_native_fail("addr() needs a string, list, or pointer");
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
}
