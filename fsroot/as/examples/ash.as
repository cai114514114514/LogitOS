# ash -- the LogitOS shell, written in AetherScript.
#
# This is M27's payoff, not a demo. A shell is the hardest possible test of "OS
# objects as values": it is nothing BUT process creation, fd wiring, redirection
# and stream reading. c/apps/coreutils/sh.c does the same job in 971 lines of C
# that reaches the kernel through int 0x80 by hand. This does it in AetherScript,
# because the language now has the same vocabulary that C had:
#
#   port(0)                       an fd this process was GIVEN -- borrowed, so
#                                 closing it detaches instead of breaking the
#                                 console the parent is still using
#   for line in stdin:            a port is its own cursor; one line at a time,
#                                 blocking on the tty, O(one line) of memory
#   run(argv)                     a command as a VALUE, not yet running
#   a |> b                        wire a's stdout to b's stdin, before either runs
#   p <- path  /  p -> path       redirect the head's stdin / the tail's stdout
#   with f = open(path):          held for the block, closed on every way out of
#                                 it -- the bottom, a `return`, or a raise
#
# `exec_pipeline` is the line that matters: a whole pipeline with redirections is
# built by FOLDING `|>` over the parsed stages and handing the result to `.wait()`.
# There is no fork, no dup2, no waitpid and no file-descriptor arithmetic
# anywhere in this file. That is the difference between a language that reaches
# for the OS through `syscall()` and a language the OS speaks to.
#
# Deliberately NOT here: job control, environment variables, globbing, `&&`.
# sh.c has some of those; they are not what this milestone is proving.

PROMPT = "ash$ "
STDOUT = port(1)

# ---- tokenizer ----------------------------------------------------------
# Whitespace-separated words, with '...' and "..." keeping spaces, and the three
# operators |, < and > split off even when they touch a word. An operator comes
# back as its own one-character word.
def tokenize(s):
    out = []
    cur = ""
    have = false
    i = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c == " " or c == "\t":
            if have:
                out.append(cur)
                cur = ""
                have = false
            i += 1
        elif c == "|" or c == "<" or c == ">":
            if have:
                out.append(cur)
                cur = ""
                have = false
            out.append(c)
            i += 1
        elif c == "\"" or c == "'":
            q = c
            i += 1
            have = true
            while i < n and s[i] != q:
                cur = cur + s[i]
                i += 1
            i += 1                       # closing quote (or end of line)
        else:
            cur = cur + c
            have = true
            i += 1
    if have:
        out.append(cur)
    return out

# ---- parser -------------------------------------------------------------
# words -> [stages, infile, outfile], stages being a list of argv lists.
# A malformed line RAISES; the caller reports it and reads the next line, which
# is what a shell does with a syntax error.
def parse(words):
    stages = []
    argv = []
    infile = nil
    outfile = nil
    i = 0
    while i < len(words):
        w = words[i]
        if w == "|":
            if len(argv) == 0:
                raise "syntax error near '|'"
            stages.append(argv)
            argv = []
        elif w == "<" or w == ">":
            if i + 1 >= len(words):
                raise f"syntax error: '{w}' needs a file name"
            i += 1
            if w == "<":
                infile = words[i]
            else:
                outfile = words[i]
        else:
            argv.append(w)
        i += 1
    if len(argv) > 0:
        stages.append(argv)
    return [stages, infile, outfile]

# ---- execution ----------------------------------------------------------
# `run(argv)` names a command without starting it, `|>` folds the stages into one
# pipeline while every stage is STILL UNSTARTED (the only moment the fds between
# them can be wired), `<-`/`->` attach the ends, and `.wait()` runs the lot in a
# single fork/dup2/exec pass and answers the last stage's exit status -- which is
# the status a shell reports. sh.c spends about 120 lines on the same thing.
def exec_pipeline(stages, infile, outfile):
    cmd = run(stages[0])
    for i in range(1, len(stages)):
        cmd = cmd |> run(stages[i])
    if infile != nil:
        cmd = cmd <- infile
    if outfile != nil:
        cmd = cmd -> outfile
    return cmd.wait()

# ---- builtins -----------------------------------------------------------
# cd and pwd change and read state belonging to THIS process, so no child can do
# them. They are the one place left in this file going through the old
# `syscall()` keyhole: a working directory is not a port. Giving it a proper home
# is P2's business (capabilities), not M27's -- recorded here rather than hidden.
def do_cd(argv):
    target = argv[1] if len(argv) > 1 else "/"
    if syscall(SYS_CHDIR, addr(target)) < 0:
        print(f"ash: cd: {target}: no such directory")
    return 1

def do_pwd():
    buf = buffer(256)
    n = syscall(SYS_GETCWD, addr(buf), 256)
    print(mem2cstr(buf) if n >= 0 else "/")
    return 1

# One command line. Returns false when the shell should stop.
def run_line(text):
    text = text.strip()
    if len(text) == 0 or text[0] == "#":
        return true
    words = tokenize(text)
    if len(words) == 0:
        return true
    if words[0] == "exit":
        print("ash: bye")
        return false
    try:
        p = parse(words)
        stages = p[0]
        if len(stages) == 0:
            return true
        if stages[0][0] == "cd":
            do_cd(stages[0])
        elif stages[0][0] == "pwd" and len(stages) == 1:
            do_pwd()
        else:
            exec_pipeline(stages, p[1], p[2])
    except e:
        print(f"ash: {e}")
    return true

# `print` always adds a newline; a prompt must not. Writing to the stdout PORT is
# the shell using its own abstraction instead of asking the VM for a special case.
def prompt():
    STDOUT.write(PROMPT)

# ---- entry --------------------------------------------------------------
# With an argument, ash runs a script -- which is what makes it a candidate for
# init's language, and the one place in the shell where `with` earns its keep:
# the script file is closed at the end of the block whether the loop finishes,
# an `exit` returns out of it, or a command raises.
def run_script(path):
    f = open(path, "r")
    if f == nil:
        print(f"ash: cannot open {path}")
        return 1
    with script = f:
        for line in script:
            if not run_line(line):
                return 0        # `exit` inside the script: the file still closes
    return 0

# Interactive: a port IS the read loop. One blocking line at a time off the tty,
# ending when the console closes. No buffer, no length, no syscall in sight.
def interactive():
    stdin = port(0)
    prompt()
    for line in stdin:
        if not run_line(line):
            return 0
        prompt()
    print("ash: eof")
    return 0

def main():
    print("ash: LogitOS AetherScript shell")
    a = args()
    if len(a) > 1:
        return run_script(a[1])
    return interactive()

main()
