# aether: 3.0
# chlaunch -- open the chat window from a shell.
#
# /bin/ch.aex is a GUI app, and a GUI app on this machine cannot be started by
# fork+execve: wm_launch() spawns it with a bare stack and no argv, and its
# crt0.asm calls app_main() rather than main(argc, argv). The three ways in are
# the Dock (which does not scan /bin), a file association, and SYS_OPEN_PATH --
# the same syscall the Finder and the Terminal use when you click a .aex, and
# the one wm.c routes to wm_launch() for any path ending in .aex.
#
# So this is the shell's door to it, and it is also what the boot harness
# (tests/boot/run-ch-test.sh) uses, because a harness that clicked a Dock icon
# would be asserting against an icon index that every future app moves.
#
#     /usr/as/bin/chlaunch.aex

from std.abi import gui_create, open_path


def main() -> i64:
    # SYS_OPEN_PATH belongs to the GUI syscall group. A standalone native CLI
    # must first own a window, just like the media-association launchers. Its
    # temporary window is removed when this process exits; Chat owns its own.
    if gui_create("opener", 200, 100) < 0:
        raise IOError("Could not create the chat launcher window")
    result = open_path("/bin/ch.aex")
    if result != 0:
        # A refused launch must also fail the process, otherwise a parent that
        # checks exit status sees success while waiting for a nonexistent UI.
        print("CHLAUNCH_FAILED rc=" + str(result))
        return 1
    print("CHLAUNCH_OK")
    return 0
