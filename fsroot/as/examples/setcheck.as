# setcheck -- drive the settings store from a shell, so a durability harness can
# set a value, reboot the machine, and read it back.
#
#   as /usr/as/examples/setcheck.as set   ui.dark 1
#   as /usr/as/examples/setcheck.as get   ui.dark
#   as /usr/as/examples/setcheck.as check ui.dark 1
#   as /usr/as/examples/setcheck.as frame clock 40 60 320 240
#   as /usr/as/examples/setcheck.as selftest
#   as /usr/as/examples/setcheck.as reset
#
# There is no `defaults` coreutil and this does not need one: the store is four
# syscalls, and AetherScript's raw syscall() reaches them directly. That is the
# A3 indirection the language exists for -- see c/apps/as/as_native.c.
#
# `check` prints SETCHECK-OK or SETCHECK-BAD WITH BOTH VALUES, because a harness
# that greps only for the value it wrote cannot tell a store that remembered it
# from a store that handed back its own compiled-in default.

SYS_SETTING_GET  = 103
SYS_SETTING_SET  = 104
SYS_SETTING_CTL  = 106

SETCTL_COMMIT   = 1
SETCTL_RESET    = 2
SETCTL_DIAG     = 4
SETCTL_RELOAD   = 5
SETCTL_SELFTEST = 6
SETCTL_KVCOUNT  = 7

BUFSZ = 256

def get(key):
    b = alloc(BUFSZ)
    n = syscall(SYS_SETTING_GET, addr(key), addr(b), BUFSZ)
    s = ""
    if n >= 0:
        s = mem2cstr(addr(b))
    dealloc(b)
    return s

# commit = 1: go to disk NOW. A harness that is about to reboot the machine has
# no later moment to do it in.
def put(key, val):
    return syscall(SYS_SETTING_SET, addr(key), addr(val), 1)

a = args()
if len(a) < 2:
    print("usage: setcheck.as set|get|check|frame|selftest|reset|diag|reload ...")
else:
    cmd = a[1]
    if cmd == "selftest":
        f = syscall(SYS_SETTING_CTL, SETCTL_SELFTEST, 0, 0)
        if f == 0:
            print("SETCHECK-SELFTEST-OK")
        else:
            print("SETCHECK-SELFTEST-FAIL", f)
    elif cmd == "reset":
        syscall(SYS_SETTING_CTL, SETCTL_RESET, 0, 0)
        print("SETCHECK-RESET")
    elif cmd == "diag":
        d = syscall(SYS_SETTING_CTL, SETCTL_DIAG, 0, 0)
        k = syscall(SYS_SETTING_CTL, SETCTL_KVCOUNT, 0, 0)
        print("SETCHECK-DIAG", d, "keys", k)
    elif cmd == "reload":
        # Everything below is about what is ON THE DISK. Re-reading first is
        # what makes that true even if something set a key without committing.
        d = syscall(SYS_SETTING_CTL, SETCTL_RELOAD, 0, 0)
        print("SETCHECK-RELOAD", d)
    elif cmd == "get":
        if len(a) < 3:
            print("usage: setcheck.as get KEY")
        else:
            print("SETCHECK-VALUE", a[2], "=", get(a[2]))
    elif cmd == "set":
        if len(a) < 4:
            print("usage: setcheck.as set KEY VALUE")
        elif put(a[2], a[3]) < 0:
            print("SETCHECK-SET-FAIL", a[2])
        else:
            print("SETCHECK-SET", a[2], "=", a[3])
    elif cmd == "check":
        if len(a) < 4:
            print("usage: setcheck.as check KEY EXPECTED")
        else:
            got = get(a[2])
            if got == a[3]:
                print("SETCHECK-OK", a[2], "=", got)
            else:
                print("SETCHECK-BAD", a[2], "want", a[3], "got", got)
    elif cmd == "frame":
        # A window frame, in the format c/kernel/core/settings.h documents:
        #   x y w h open zoomed minimized rx ry rw rh
        # Written through the ordinary string path on purpose: it IS an ordinary
        # key, and a harness proving frames survive a reboot should exercise the
        # same code the window manager will.
        if len(a) < 7:
            print("usage: setcheck.as frame APP X Y W H")
        else:
            v = a[3] + " " + a[4] + " " + a[5] + " " + a[6] + " 1 0 0 " + a[3] + " " + a[4] + " " + a[5] + " " + a[6]
            if put("win." + a[2] + ".frame", v) < 0:
                print("SETCHECK-SET-FAIL frame")
            else:
                print("SETCHECK-SET win." + a[2] + ".frame =", v)
    elif cmd == "truncate":
        # Cut /etc/settings.conf to N bytes, in the guest, through the ordinary
        # file API -- which is exactly what a power cut or a text editor that
        # died mid-save would leave behind. The next boot has to come up.
        if len(a) < 3:
            print("usage: setcheck.as truncate N")
        else:
            src = file_read("/etc/settings.conf")
            if src == nil:
                print("SETCHECK-TRUNC-FAIL no file")
            else:
                n = 0
                i = 0
                while i < len(a[2]):
                    n = n * 10 + (ord(a[2][i]) - 48)
                    i = i + 1
                if n > len(src):
                    n = len(src)
                out = ""
                i = 0
                while i < n:
                    out = out + src[i]
                    i = i + 1
                if file_write("/etc/settings.conf", out) < 0:
                    print("SETCHECK-TRUNC-FAIL write")
                else:
                    print("SETCHECK-TRUNCATED", n, "of", len(src))
    elif cmd == "garbage":
        # The hand-edited file the brief names: a negative number, a colour out
        # of range, a window off-screen, a key that is not a key at all. Every
        # one has to be REFUSED BY NAME on the next boot, and the desktop has to
        # come up anyway.
        g = "# hand-edited, badly\n"
        g = g + "ui.dark = banana\n"
        g = g + "ui.accent = 0xFFFFFFFF\n"
        g = g + "desktop.restore_session = -1\n"
        g = g + "net.dhcp = 2\n"
        g = g + "net.ip = 999.1.1.1\n"
        g = g + "win.clock.frame = -9000 -9000 4 4 1 0 0 0 0 0 0\n"
        g = g + "= nokey\n"
        g = g + "no_equals_sign_at_all\n"
        g = g + "ui.wallpaper = /does/not/exist.png\n"
        if file_write("/etc/settings.conf", g) < 0:
            print("SETCHECK-GARBAGE-FAIL")
        else:
            print("SETCHECK-GARBAGE-WRITTEN", len(g))
    else:
        print("SETCHECK-BAD unknown command", cmd)
