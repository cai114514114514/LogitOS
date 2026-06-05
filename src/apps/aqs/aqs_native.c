#include "aqs.h"
#include "aqua_abi.h"   /* SYS_* numbers (shared kernel ABI) */

/* A3 indirection builtins: raw memory (peek/poke), the address of an object's
 * backing store (addr), typed pointers (iNptr -> p[i]), and direct Aqua syscalls
 * (syscall). This is what lets AquaScript do systems programming, not just compute. */

static Value n_addr(int argc, Value *args)
{
    if (argc != 1) return aqs_native_fail("addr() takes 1 argument");
    Value v = args[0];
    if (IS_STR(v))  return INT_VAL((int64_t)(uintptr_t)AS_STR(v)->chars);
    if (IS_LIST(v)) return INT_VAL((int64_t)(uintptr_t)AS_LIST(v)->items);
    if (IS_PTR(v))  return INT_VAL((int64_t)AS_PTR(v)->addr);
    return aqs_native_fail("addr() needs a string, list, or pointer");
}

static Value peek_w(int argc, Value *args, int w)
{
    if (argc != 1 || !IS_INT(args[0])) return aqs_native_fail("peek expects an integer address");
    return INT_VAL((int64_t)aqs_ll_peek((uint64_t)AS_INT(args[0]), w));
}
static Value n_peek8 (int c, Value *a) { return peek_w(c, a, 1); }
static Value n_peek16(int c, Value *a) { return peek_w(c, a, 2); }
static Value n_peek32(int c, Value *a) { return peek_w(c, a, 4); }
static Value n_peek64(int c, Value *a) { return peek_w(c, a, 8); }

static Value poke_w(int argc, Value *args, int w)
{
    if (argc != 2 || !IS_INT(args[0]) || !IS_INT(args[1])) return aqs_native_fail("poke expects (addr, value) integers");
    aqs_ll_poke((uint64_t)AS_INT(args[0]), w, (uint64_t)AS_INT(args[1]));
    return NIL_VAL;
}
static Value n_poke8 (int c, Value *a) { return poke_w(c, a, 1); }
static Value n_poke16(int c, Value *a) { return poke_w(c, a, 2); }
static Value n_poke32(int c, Value *a) { return poke_w(c, a, 4); }
static Value n_poke64(int c, Value *a) { return poke_w(c, a, 8); }

static Value ptr_w(int argc, Value *args, int w)
{
    if (argc != 1 || !IS_INT(args[0])) return aqs_native_fail("iNptr expects an integer address");
    return OBJ_VAL(aqs_ptr_new((uint64_t)AS_INT(args[0]), w, 1));
}
static Value n_i8ptr (int c, Value *a) { return ptr_w(c, a, 1); }
static Value n_i16ptr(int c, Value *a) { return ptr_w(c, a, 2); }
static Value n_i32ptr(int c, Value *a) { return ptr_w(c, a, 4); }
static Value n_i64ptr(int c, Value *a) { return ptr_w(c, a, 8); }

static Value n_syscall(int argc, Value *args)
{
    long a[4] = { 0, 0, 0, 0 };
    if (argc < 1 || argc > 4) return aqs_native_fail("syscall() takes 1 to 4 arguments");
    for (int i = 0; i < argc; i++) {
        if (!IS_INT(args[i])) return aqs_native_fail("syscall() arguments must be integers");
        a[i] = (long)AS_INT(args[i]);
    }
    return INT_VAL((int64_t)aqs_ll_syscall(a[0], a[1], a[2], a[3]));
}

void aqs_install_indirection(void)
{
    aqs_define_native("addr", n_addr);
    aqs_define_native("peek8",  n_peek8);  aqs_define_native("poke8",  n_poke8);
    aqs_define_native("peek16", n_peek16); aqs_define_native("poke16", n_poke16);
    aqs_define_native("peek32", n_peek32); aqs_define_native("poke32", n_poke32);
    aqs_define_native("peek64", n_peek64); aqs_define_native("poke64", n_poke64);
    aqs_define_native("i8ptr",  n_i8ptr);  aqs_define_native("i16ptr", n_i16ptr);
    aqs_define_native("i32ptr", n_i32ptr); aqs_define_native("i64ptr", n_i64ptr);
    aqs_define_native("syscall", n_syscall);

    aqs_define_int("SYS_WRITE",  SYS_WRITE);
    aqs_define_int("SYS_READ",   SYS_READ);
    aqs_define_int("SYS_OPEN",   SYS_OPEN);
    aqs_define_int("SYS_CLOSE",  SYS_CLOSE);
    aqs_define_int("SYS_LSEEK",  SYS_LSEEK);
    aqs_define_int("SYS_EXIT",   SYS_EXIT);
    aqs_define_int("SYS_YIELD",  SYS_YIELD);
    aqs_define_int("SYS_GETPID", SYS_GETPID);
    aqs_define_int("SYS_FORK",   SYS_FORK);
    aqs_define_int("SYS_MKDIR",  SYS_MKDIR);
    aqs_define_int("SYS_GETCWD", SYS_GETCWD);
}
