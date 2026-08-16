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
#     as /usr/as/examples/chlaunch.as

from abi import open_path

rc = open_path("/bin/ch.aex")
if rc == 0:
    print("CHLAUNCH_OK")
else:
    # Loud, and with the number: a launch that fails silently looks exactly
    # like an app that started and drew nothing.
    print("CHLAUNCH_FAILED rc=" + str(rc))
