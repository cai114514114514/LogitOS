# aether: 3.0
# The seven format fixtures share the same Finder association path. This
# helper keeps GUI ownership and error handling identical for every format;
# the actual media path remains explicit in each fixture's main function.
from std.abi import gui_create, open_path


def open_media(path: str) -> i64:
    # SYS_OPEN_PATH belongs to the GUI interface. A shell child must first
    # own a window, just as Finder does, before asking the WM to open a file.
    if gui_create("opener", 200, 100) < 0:
        raise IOError("Could not create the association launcher window")
    print("opening " + path)
    result = open_path(path)
    if result < 0:
        raise IOError("Could not open " + path + ": " + str(result))
    return 0
