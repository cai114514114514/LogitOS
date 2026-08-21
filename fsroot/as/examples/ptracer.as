# ptracer -- the on-device gate for SYS_PTRACE (c/kernel/exec/ptrace.h).
#
#   as /usr/as/examples/ptracer.as             the tracer
#   as /usr/as/examples/ptracer.as intrude     the same file, as the INTRUDER
#
# ONE FILE, TWO ROLES, and that is not economy. The intruder has to be a
# SEPARATE PROCESS -- the whole question it answers is whether a process that
# never attached can read a stopped process's registers -- and a second .as
# file would need the tracee's pid passed to it as a string and parsed back,
# which AetherScript has no integer parser for. Instead the intruder SCANS every
# pid, which is a better statement of the property anyway: not "it cannot read
# THIS one" but "a pid is not a capability, and guessing does not help".
#
# WHAT EACH CHECK IS FOR, because "ptrace returned 0" is not evidence:
#
#   cs, rip, rsp ranges   these are a RING-3 program's registers, in the text
#                         and stack of a /bin/as process -- not zeroes, not the
#                         caller's, not kernel addresses.
#   rip MOVED             taken twice with a resume in between. The tracer is
#                         sitting inside a syscall the whole time, so its own
#                         rip is CONSTANT; only a third party that is actually
#                         running has one that changes. This is the check that
#                         says whose registers these are.
#   peek == our own       the tracee is /bin/as and so is this program, at the
#                         same fixed link base (Makefile CLI_RULE: 0x50000000),
#                         so the instruction word at the tracee's rip must equal
#                         the word this process reads at that address itself.
#                         Proves the address translation walked a real page
#                         table and landed on real bytes.
#   peek of nowhere       0xdeadbee0 is mapped in no process here; it must come
#                         back PT_E_FAULT and not a value. A debugger that
#                         invents zeroes for unmapped memory is worse than one
#                         that refuses.
#   poke of the text      a read-only page must be refused. Not tested by
#                         corrupting anything: the refusal IS the test.
#   after DETACH          the link is gone, so the same call that worked a line
#                         earlier must now be refused.
#   ATTACH to self        refused.

from sys import spawn, wait, pid

SYS_PTRACE     = 187
SYS_NANOSLEEP  = 84

PTRACE_ATTACH   = 1
PTRACE_DETACH   = 2
PTRACE_GETREGS  = 3
PTRACE_PEEKDATA = 5
PTRACE_POKEDATA = 6

PT_OK      = 0
PT_E_PERM  = 0 - 3
PT_E_FAULT = 0 - 6

# Indices into the 27-register array. The SAME order a core dump's NT_PRSTATUS
# uses (c/kernel/exec/coredump.h's CORE_* enum, which is glibc's
# user_regs_struct order and is diffed against it host-side), so a register
# named here and a register named by /bin/readcore are the same register.
R_RIP = 16
R_CS  = 17
R_RSP = 19

NGREG = 27

# The address map every CLI program on this machine has: text at the fixed link
# base CLI_RULE gives them, stack placed by c/kernel/exec/exec.c just under it.
TEXT_LO  = 0x50000000
TEXT_HI  = 0x50100000
STACK_LO = 0x53f00000
STACK_HI = 0x54000000

BAD_ADDR = 0xdeadbee0

fails = 0

def check(name, ok):
    if ok:
        print("ok  :", name)
    else:
        print("FAIL:", name)
    return 0 if ok else 1

def pt(req, p, arg):
    return syscall(SYS_PTRACE, req, p, arg)

def getregs(p, buf):
    return pt(PTRACE_GETREGS, p, addr(buf))

def reg(buf, i):
    return peek64(addr(buf) + 8 * i)

# struct logit_ptrace_word { unsigned long long addr, data; }
def peekword(p, a, w):
    poke64(addr(w), a)
    poke64(addr(w) + 8, 0)
    return pt(PTRACE_PEEKDATA, p, addr(w))

def pokeword(p, a, v, w):
    poke64(addr(w), a)
    poke64(addr(w) + 8, v)
    return pt(PTRACE_POKEDATA, p, addr(w))

Ts = layout("logit_timespec", 16, [
    ["sec", 0, 8, "i"],
    ["nsec", 8, 8, "i"]
])

# Sub-second only, and the fields are set directly rather than divided out of a
# millisecond count: `/` on two AetherScript ints is not integer division, and a
# float landing in a timespec's tv_sec is a sleep of an unpredictable length --
# which in a harness reads as "ptrace was slow", not as "the argument was
# wrong". Nothing here needs to sleep for a whole second.
def nap_ms(ms):
    t = Ts()
    t.sec = 0
    t.nsec = ms * 1000000
    syscall(SYS_NANOSLEEP, addr(t), 0, 0)

# ---------------------------------------------------------------- INTRUDER --
# A process that attached to nothing, walking every pid this machine can have
# (NPROC is 32, c/kernel/exec/proc.h) and asking for its registers. It must be
# refused by all of them -- including the one that is stopped RIGHT NOW, which
# is the only pid where the answer could differ.
a = args()
if len(a) > 1 and a[1] == "intrude":
    g = buffer(NGREG * 8)
    readable = 0
    p = 1
    while p < 33:
        if getregs(p, g) == PT_OK:
            readable = readable + 1
            print("PTINTRUDE read pid", p, "rip", reg(g, R_RIP))
        p = p + 1
    print("PTINTRUDE readable", readable)
else:
    me = pid()
    print("PTRACER-UP pid", me)

    child = spawn("/bin/as", ["as", "/usr/as/examples/ptracee.as"])
    fails = fails + check("spawned a tracee", child > 0)
    # Let it get through execve and into its loop, so the registers read below
    # are the TRACEE's and not those of the forked copy of this program that
    # has not called execve yet. ATTACH would work either way; this is what
    # makes the text-range check mean something.
    nap_ms(700)

    fails = fails + check("ATTACH", pt(PTRACE_ATTACH, child, 0) == PT_OK)

    g1 = buffer(NGREG * 8)
    fails = fails + check("GETREGS", getregs(child, g1) == PT_OK)
    rip1 = reg(g1, R_RIP)
    rsp1 = reg(g1, R_RSP)
    cs1  = reg(g1, R_CS)
    print("     rip", rip1, "rsp", rsp1, "cs", cs1)
    fails = fails + check("cs is a ring-3 selector", (cs1 & 3) == 3)
    fails = fails + check("rip is inside /bin/as text", rip1 >= TEXT_LO and rip1 < TEXT_HI)
    fails = fails + check("rsp is inside its stack", rsp1 >= STACK_LO and rsp1 < STACK_HI)

    w = buffer(16)
    at = rip1 & ~7
    fails = fails + check("PEEKDATA at its rip", peekword(child, at, w) == PT_OK)
    theirs = peek64(addr(w) + 8)
    ours = peek64(at)
    print("     word at", at, "theirs", theirs, "ours", ours)
    fails = fails + check("the instruction word matches our own copy of /bin/as",
                          theirs == ours)

    fails = fails + check("PEEKDATA of an unmapped address is refused",
                          peekword(child, BAD_ADDR, w) == PT_E_FAULT)
    fails = fails + check("POKEDATA into read-only text is refused",
                          pokeword(child, at, 0, w) == PT_E_FAULT)

    # WHILE IT IS STILL STOPPED: a process that attached to nothing tries.
    code = wait(spawn("/bin/as", ["as", "/usr/as/examples/ptracer.as", "intrude"]))
    fails = fails + check("the intruder ran", code == 0)

    # ------------------------------------------------ IS IT REALLY EXECUTING?
    # Four stops with a resume between each, and at least two of the rips must
    # differ. There is no PTRACE_STOP in this ABI -- DETACH continues it and
    # ATTACH stops it, which is the whole vocabulary.
    #
    # WHY "AT LEAST TWO OF FOUR" AND NOT "rip2 != rip1". The tracee spins in the
    # VM's interpreter dispatch, which is a few dozen instructions, so two
    # independent timer interrupts land on the same one perhaps once in forty
    # tries. A `!=` between two samples is a gate that fails a correct kernel a
    # few percent of the time, and a gate that is occasionally wrong for no
    # reason is worse than one check fewer -- it teaches everyone to re-run.
    # Three resumes make an all-identical run about one in ten thousand.
    #
    # This is NOT the check that says whose registers these are: the POKE
    # round-trip at the end of the file is, and it is exact. This one says the
    # tracee is genuinely running between stops rather than wedged.
    g2 = buffer(NGREG * 8)
    seen = rip1
    moved = 0
    round = 0
    while round < 3:
        fails = fails + check("DETACH", pt(PTRACE_DETACH, child, 0) == PT_OK)
        nap_ms(300)
        fails = fails + check("re-ATTACH", pt(PTRACE_ATTACH, child, 0) == PT_OK)
        fails = fails + check("GETREGS again", getregs(child, g2) == PT_OK)
        r = reg(g2, R_RIP)
        print("     stop", round + 2, "rip", r)
        if r != seen:
            moved = 1
        round = round + 1
    fails = fails + check("rip differed across four stops -- it is executing", moved == 1)

    # ------------------------------------- WHOSE ADDRESS SPACE WAS THAT? EXACT
    # A word ABOVE the tracee's stack pointer -- in the top page of its stack,
    # which every process touches when execve builds its argv -- is written
    # through ptrace and read back. Then this process reads the SAME ADDRESS in
    # its own memory and it must be unchanged.
    #
    # That is the whole feature in one check. Both processes are /bin/as at the
    # same link base with a stack at the same virtual address, so if PEEK/POKE
    # were operating on the CALLER's address space -- the easiest way to get
    # this wrong, and one that passes every check above -- the sentinel would
    # appear right here. It does not, because c/kernel/exec/ptrace.c walks the
    # TRACEE's page table and reaches the frame through the identity map.
    #
    # It is last on purpose: it writes into the tracee's live VM frames, so the
    # tracee may not survive it. Nothing after this needs it alive.
    SENT = 0x5AFE1234DEADBEE0
    sa = (rsp1 + 256) & ~7
    mine_before = peek64(sa)
    fails = fails + check("POKEDATA into its stack", pokeword(child, sa, SENT, w) == PT_OK)
    fails = fails + check("PEEKDATA reads it back", peekword(child, sa, w) == PT_OK)
    print("     at", sa, "read back", peek64(addr(w) + 8), "ours now", peek64(sa))
    fails = fails + check("the value came back", peek64(addr(w) + 8) == SENT)
    fails = fails + check("OUR memory at that address is untouched -- it is ITS space",
                          peek64(sa) == mine_before and peek64(sa) != SENT)

    fails = fails + check("DETACH again", pt(PTRACE_DETACH, child, 0) == PT_OK)
    fails = fails + check("GETREGS after DETACH is refused",
                          getregs(child, g2) == PT_E_PERM)
    fails = fails + check("ATTACH to self is refused",
                          pt(PTRACE_ATTACH, me, 0) == PT_E_PERM)

    if fails == 0:
        print("PTRACE-OK")
    else:
        print("PTRACE-FAILED", fails)
