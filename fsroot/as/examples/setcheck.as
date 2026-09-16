# aether: 3.0
# setcheck -- settings-store commands used by reboot/durability harnesses.
# Run /usr/as/bin/setcheck.aex set|get|check|frame|selftest|reset|diag|reload ...
#
# This is the original tool migrated to native code. Each invocation reaches
# the real settings syscalls. Writes request immediate commit because a caller
# may reboot before the desktop's next periodic save. Failed checks now return
# nonzero status, so callers cannot mistake a printed BAD for success.

BUFSZ: i64 = 256
SETTINGS_FILE: str = "/etc/settings.conf"


def get(key: str) -> str:
    # A3 strings may be interior slices without a trailing NUL. Keep an owned,
    # explicitly terminated copy alive across the syscall.
    key_bytes = Bytes(key + "\0")
    data = buffer(BUFSZ)
    unsafe:
        count = syscall(SYS_SETTING_GET, addr(key_bytes), addr(data), len(data))
    if count == -1:
        # Preserve the old unset-key result. A missing key still cannot satisfy
        # a nonempty check expectation; both values appear in the failure.
        return ""
    if count < 0 or count >= len(data):
        raise IOError("Invalid settings read length: " + str(count))
    unsafe:
        return mem2str(data, count)


def put(key: str, value: str) -> i64:
    key_bytes = Bytes(key + "\0")
    value_bytes = Bytes(value + "\0")
    unsafe:
        return syscall(SYS_SETTING_SET, addr(key_bytes), addr(value_bytes), 1)


def _control(operation: i64) -> i64:
    unsafe:
        result = syscall(SYS_SETTING_CTL, operation, 0, 0)
    if result < 0:
        raise IOError("Settings control failed: " + str(result))
    return result


def _set(key: str, value: str) -> i64:
    if put(key, value) < 0:
        print("SETCHECK-SET-FAIL", key)
        return 1
    print("SETCHECK-SET", key, "=", value)
    return 0


def _frame(arguments: List[str]) -> i64:
    if len(arguments) < 7:
        print("usage: setcheck.as frame APP X Y W H")
        return 2
    # The window manager consumes this same ordinary setting: x/y/w/h,
    # open/zoom/minimized and the complete restore-frame rectangle.
    rectangle = arguments[3] + " " + arguments[4] + " " + arguments[5] + " " + arguments[6]
    value = rectangle + " 1 0 0 " + rectangle
    if put("win." + arguments[2] + ".frame", value) < 0:
        print("SETCHECK-SET-FAIL frame")
        return 1
    print("SETCHECK-SET win." + arguments[2] + ".frame =", value)
    return 0


def _truncate(arguments: List[str]) -> i64:
    if len(arguments) < 3:
        print("usage: setcheck.as truncate N")
        return 2
    # Byte copying preserves a partial UTF-8 sequence too. Parse before opening
    # for write: malformed input must not change this reboot-test fixture.
    count = parse_int(arguments[2])
    if count < 0:
        raise ValueError("Truncation length must be nonnegative")
    try:
        source = file_read(SETTINGS_FILE)
    except IOError as error:
        print("SETCHECK-TRUNC-FAIL no file")
        return 1
    if count > len(source):
        count = len(source)
    prefix = buffer(count)
    for index in range(count):
        prefix[index] = source[index]
    try:
        file_write(SETTINGS_FILE, Bytes(prefix))
    except IOError as error:
        print("SETCHECK-TRUNC-FAIL write")
        return 1
    print("SETCHECK-TRUNCATED", count, "of", len(source))
    return 0


def _garbage() -> i64:
    # Retain the original malformed-file fixture. The reboot gate checks each
    # rejected field by name; a valid substitute would defeat that gate.
    content = "# hand-edited, badly\n"
    content += "ui.dark = banana\n"
    content += "ui.accent = 0xFFFFFFFF\n"
    content += "desktop.restore_session = -1\n"
    content += "net.dhcp = 2\n"
    content += "net.ip = 999.1.1.1\n"
    content += "win.clock.frame = -9000 -9000 4 4 1 0 0 0 0 0 0\n"
    content += "= nokey\n"
    content += "no_equals_sign_at_all\n"
    content += "ui.wallpaper = /does/not/exist.png\n"
    try:
        file_write(SETTINGS_FILE, Bytes(content))
    except IOError as error:
        print("SETCHECK-GARBAGE-FAIL")
        return 1
    print("SETCHECK-GARBAGE-WRITTEN", len(content))
    return 0


def main() -> i64:
    arguments = args()
    if len(arguments) < 2:
        print("usage: setcheck.as set|get|check|frame|selftest|reset|diag|reload ...")
        return 2
    command = arguments[1]
    if command == "selftest":
        failures = _control(SETCTL_SELFTEST)
        if failures != 0:
            print("SETCHECK-SELFTEST-FAIL", failures)
            return 1
        print("SETCHECK-SELFTEST-OK")
    elif command == "reset":
        _control(SETCTL_RESET)
        print("SETCHECK-RESET")
    elif command == "diag":
        diagnostic = _control(SETCTL_DIAG)
        keys = _control(SETCTL_KVCOUNT)
        print("SETCHECK-DIAG", diagnostic, "keys", keys)
    elif command == "reload":
        print("SETCHECK-RELOAD", _control(SETCTL_RELOAD))
    elif command == "get":
        if len(arguments) < 3:
            print("usage: setcheck.as get KEY")
            return 2
        print("SETCHECK-VALUE", arguments[2], "=", get(arguments[2]))
    elif command == "set":
        if len(arguments) < 4:
            print("usage: setcheck.as set KEY VALUE")
            return 2
        return _set(arguments[2], arguments[3])
    elif command == "check":
        if len(arguments) < 4:
            print("usage: setcheck.as check KEY EXPECTED")
            return 2
        actual = get(arguments[2])
        if actual != arguments[3]:
            print("SETCHECK-BAD", arguments[2], "want", arguments[3], "got", actual)
            return 1
        print("SETCHECK-OK", arguments[2], "=", actual)
    elif command == "frame":
        return _frame(arguments)
    elif command == "truncate":
        return _truncate(arguments)
    elif command == "garbage":
        return _garbage()
    else:
        print("SETCHECK-BAD unknown command", command)
        return 2
    return 0
