# aether: 3.0
# This fixture runs under the ordinary full grant on both host and guest.
initial = caps()

class Holder:
    grant: Cap

def grant_path() -> Optional[str]:
    # The returned text points into a capability whose other references die.
    return caps().scope("/usr/as/中文").path()

def identity[T: Equatable](left: T, right: T) -> bool:
    return left == right

def main() -> None:
    all_bits = CAP_FS_READ | CAP_FS_WRITE | CAP_NET | CAP_PROC | CAP_GUI | CAP_RAW
    root = caps()
    assert root.bits() == all_bits
    assert initial.bits() == all_bits
    assert root.path() == None
    assert root != initial
    assert identity(root, root)

    scoped = root.scope("/usr//as/./lib/../中文/")
    assert scoped.path() == "/usr/as/中文"
    assert scoped.bits() == all_bits
    weaker = scoped.without(CAP_FS_WRITE | CAP_RAW)
    assert weaker.bits() == (all_bits & ~(CAP_FS_WRITE | CAP_RAW))
    assert weaker.path() == scoped.path()
    assert weaker != scoped
    assert scoped.without(-1).bits() == 0
    assert caps().bits() == all_bits
    assert str(weaker) == "<cap fs_read|net|proc|gui @/usr/as/中文>"
    assert str(root.without(-1)) == "<cap none @/>"

    denied = 0
    for path in ["/", "/usr/as", "/usr/as/中文x", "/usr/as/中文/.."]:
        try:
            scoped.scope(path)
        except PermissionError as error:
            assert error.line > 0
            denied += 1
    assert denied == 4
    assert scoped.scope("/usr/as/中文/child/../child").path() == "/usr/as/中文/child"

    invalid = 0
    for path in ["", "relative", "/usr\0/as", "/" + "x" * 4096]:
        try:
            root.scope(path)
        except ValueError:
            invalid += 1
    assert invalid == 4

    held = Holder(weaker)
    boxed = Any(held.grant)
    captured = lambda: held.grant
    gc_collect()
    assert cast[Cap](boxed) == weaker
    assert captured() == weaker
    assert is_type[Cap](boxed)
    assert not is_type[Buffer](boxed)
    retained_path = grant_path()
    gc_collect()
    assert retained_path == "/usr/as/中文"
    if retained_path != None:
        assert len(retained_path) > 0
    print("native capabilities ok")
