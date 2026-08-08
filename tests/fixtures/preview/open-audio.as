# The association, exercised the way the Finder exercises it.
#
# A double-click in the Finder is SYS_OPEN_PATH and nothing else: the kernel
# sniffs the file (opens_in_preview in c/kernel/gui/wm.c), picks an app and
# hands it the path as its launch argument. Issuing that syscall from a
# script tests the same route a user takes, without any of the pointer
# arithmetic a double-click needs -- and it is two lines, because that is all
# the route is. tests/qmp/qmp_preview.py --assoc runs these.
#
# The window comes first and is not decoration: wm_gui_syscall refuses every
# GUI call except SYS_GUI_CREATE from a process that has no window, and
# SYS_OPEN_PATH is one of them. The Finder has a window; a CLI script has to
# ask for one. 200x100, and it is never drawn into.
title = "opener"
syscall(SYS_GUI_CREATE, addr(title), 200 * 65536 + 100, 0)
p = "/media/sample.mp3"
print("opening " + p)
syscall(SYS_OPEN_PATH, addr(p), 0, 0)
