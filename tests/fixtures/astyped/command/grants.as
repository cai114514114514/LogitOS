# aether: 3.0
def main() -> None:
    authority = caps().bits()
    allowed = (authority & CAP_PROC) != 0
    denied = False
    try:
        command = run("echo", "granted")
        assert allowed and command.out() == "granted\n"
    except PermissionError:
        denied = True
    assert denied == (not allowed)
    if allowed:
        # The restricted process grant carries no file authority. A redirect
        # must not let process creation bypass the normal file acquisition.
        denied = False
        try:
            run("echo", "must not be written") -> "/state/denied-command-output"
        except PermissionError:
            denied = True
        assert denied
    print("native command grants ok")
