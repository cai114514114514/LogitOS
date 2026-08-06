# sys -- direct Logit system access from AetherScript (M23.5).
# Pure AetherScript over syscall()/buffer()/addr(): files, dirs, processes
# (fork+execve+waitpid), time, network. This is the surface a sandboxed
# scripting language doesn't get: the OS, first-class.
#   from sys import read_file, write_file, ls, run, time
# Strings passed to the kernel are NUL-terminated (ObjStr always is); buffers
# come from buffer(), which the collector owns -- there is nothing to free, and
# nothing to leak when a call in between raises. Kernel structs come from `abi`,
# whose field offsets are generated from include/abi/logit_abi.h and checked
# against it by the compiler that builds /bin/as.

from abi import Time

# ---- files ----
def read_file(path):
    return read_file_max(path, 262144)

def read_file_max(path, max):
    buf = buffer(max)
    n = syscall(SYS_READ_FILE, addr(path), addr(buf), max)
    return mem2str(buf, n) if n >= 0 else nil

def write_file(path, data):
    return syscall(SYS_WRITE_FILE, addr(path), addr(data), len(data))

def remove(path):
    return syscall(SYS_DELETE_FILE, addr(path))

def rename(old, new):
    return syscall(SYS_RENAME, addr(old), addr(new))

def mkdir(path):
    return syscall(SYS_MKDIR, addr(path))

# ---- directories ----
def ls(dir):
    n = syscall(SYS_DIR_COUNT, addr(dir))
    if n < 0:
        return nil
    out = []
    buf = buffer(64)
    for i in range(n):
        sz = syscall(SYS_DIR_NAME, addr(dir), i, addr(buf))
        if sz != -1:
            out.append(mem2cstr(buf))
    return out

def cwd():
    buf = buffer(128)
    n = syscall(SYS_GETCWD, addr(buf), 128)
    return mem2str(buf, n) if n >= 0 else nil

def chdir(path):
    return syscall(SYS_CHDIR, addr(path))

# ---- processes ----
def pid():
    return syscall(SYS_GETPID)

# Build a NULL-terminated char*[] from a list of strings, in script memory.
# The list MUST stay live while the kernel reads the array (it does: execve
# copies before returning) -- the addr()s point into the strings' own bytes.
def _argv(args):
    n = len(args)
    pv = buffer(8 * (n + 1))
    for i in range(n):
        poke64(addr(pv) + 8 * i, addr(args[i]))
    poke64(addr(pv) + 8 * n, 0)
    return pv

# spawn(path, args) -> child pid (args = full argv incl. argv[0]).
# fork + execve in pure script: the child VM replaces itself with the program.
def spawn(path, args):
    p = syscall(SYS_FORK)
    if p == 0:
        pv = _argv(args)
        syscall(SYS_EXECVE, addr(path), addr(pv), 0)
        syscall(SYS_EXIT, 127)          # execve failed
    return p

def wait(p):
    st = buffer(4)
    rc = syscall(SYS_WAITPID, p, addr(st), 0)
    return peek32(addr(st)) if rc >= 0 else -1

def run(path, args):
    return wait(spawn(path, args))

def exit(code):
    syscall(SYS_EXIT, code)

def cpu():
    return syscall(SYS_CPU_INDEX)

# ---- time ----
# -> a Time layout: .year .month .day .hour .minute .second .weekday
# (RTC wall clock). The kernel writes struct logit_time straight into it.
def time():
    t = Time()
    syscall(SYS_GET_TIME, addr(t))
    return t

def sleep(secs):
    t0 = time()
    start = (t0.hour * 60 + t0.minute) * 60 + t0.second
    while true:
        t = time()
        now = (t.hour * 60 + t.minute) * 60 + t.second
        if now < start:
            now = now + 86400
        if now - start >= secs:
            return nil
        syscall(SYS_YIELD)

# ---- network ----
def dns(name):
    if syscall(SYS_NET_DNS, addr(name)) < 0:
        return 0
    for i in range(500):
        ip = syscall(SYS_NET_DNS_RESULT)
        if ip != 0:
            return 0 if ip == 0xFFFFFFFF else ip
        syscall(SYS_YIELD)
    return 0

def ip_str(ip):
    return f"{(ip >> 24) & 255}.{(ip >> 16) & 255}.{(ip >> 8) & 255}.{ip & 255}"

def ping(ip):
    if syscall(SYS_NET_PING, ip) < 0:
        return -1
    for i in range(500):
        rtt = syscall(SYS_NET_PING_RTT)
        if rtt >= 0:
            return rtt
        syscall(SYS_YIELD)
    return -1
