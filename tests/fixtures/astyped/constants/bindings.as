# aether: 3.0
# User bindings take precedence over implicit system names in their scope.
SYS_WRITE: i64 = 900

def SYS_READ() -> i64:
    return 901

def value() -> i64:
    return SYS_WRITE + SYS_READ()
