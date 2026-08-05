# sys -- direct Logit system access from AetherScript (M23.5).
# Pure AetherScript over syscall()/alloc()/addr(): files, dirs, processes
# (fork+execve+waitpid), time, network. This is the surface a sandboxed
# scripting language doesn't get: the OS, first-class.
#   from sys import read_file, write_file, ls, run, time
# Strings passed to the kernel are NUL-terminated (ObjStr always is); buffers
# come from alloc() and are freed with dealloc().

# ---- files ----
def read_file(path):
    return read_file_max(path, 262144)

def read_file_max(path, max):
    buf = alloc(max)
    n = syscall(SYS_READ_FILE, addr(path), addr(buf), max)
    s = mem2str(buf, n) if n >= 0 else nil
    dealloc(buf)
    return s

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
    buf = alloc(64)
    for i in range(n):
        sz = syscall(SYS_DIR_NAME, addr(dir), i, addr(buf))
        if sz != -1:
            out.append(mem2cstr(buf))
    dealloc(buf)
    return out

def cwd():
    buf = alloc(128)
    n = syscall(SYS_GETCWD, addr(buf), 128)
    s = mem2str(buf, n) if n >= 0 else nil
    dealloc(buf)
    return s

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
    pv = alloc(8 * (n + 1))
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
    st = alloc(4)
    rc = syscall(SYS_WAITPID, p, addr(st), 0)
    code = peek32(addr(st))
    dealloc(st)
    return code if rc >= 0 else -1

def run(path, args):
    return wait(spawn(path, args))

def exit(code):
    syscall(SYS_EXIT, code)

def cpu():
    return syscall(SYS_CPU_INDEX)

# ---- time ----
# -> {"year","month","day","hour","minute","second","weekday"} (RTC wall clock)
def time():
    buf = alloc(28)
    syscall(SYS_GET_TIME, addr(buf))
    a = addr(buf)
    t = {}
    t["year"] = peek32(a)
    t["month"] = peek32(a + 4)
    t["day"] = peek32(a + 8)
    t["hour"] = peek32(a + 12)
    t["minute"] = peek32(a + 16)
    t["second"] = peek32(a + 20)
    t["weekday"] = peek32(a + 24)
    dealloc(buf)
    return t

def sleep(secs):
    t0 = time()
    start = (t0["hour"] * 60 + t0["minute"]) * 60 + t0["second"]
    while true:
        t = time()
        now = (t["hour"] * 60 + t["minute"]) * 60 + t["second"]
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
