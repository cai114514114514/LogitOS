# aether: 3.0
# Both qualified and from-import calls share the same native module instance.
import mathx
from mathx import square


def main() -> None:
    assert mathx.PI == 3.141592653589793
    print("PI =", mathx.PI)
    print("quad(2) =", mathx.quad(2))
    print("from-import square(9) =", square(9))
