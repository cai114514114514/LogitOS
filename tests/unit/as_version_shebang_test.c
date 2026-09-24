/* /bin/as finds its '# aether: 3.0' header on line 2 when line 1 is a `#!`
 * line (c/apps/as/common/version.c). Without that, an executable AetherScript
 * script -- `#!/bin/as` then the header -- is classified as the removed
 * AetherScript 2 and refused. The control (-DAS_VERSION_NEGCTL_NO_SHEBANG)
 * removes the skip and must fail. */
#include <stdio.h>
#include "numeric.h"

static int checks, fails;
static void t(const char *name, const char *src, int want)
{
    int got = as_source_version(src);
    checks++;
    if (got != want) { fails++; printf("FAIL %s: got %d want %d\n", name, got, want); }
}

int main(void)
{
    t("header on line 1", "# aether: 3.0\nprint(1)\n", AS_LANGUAGE_NATIVE);
    t("#! then header", "#!/bin/as\n# aether: 3.0\n", AS_LANGUAGE_NATIVE);
    t("#! with arg then header", "#!/bin/as -q\n# aether: 3\n", AS_LANGUAGE_NATIVE);
    t("#! CRLF then header", "#!/bin/as\r\n# aether: 3.0\r\n", AS_LANGUAGE_NATIVE);
    t("#! and no header", "#!/bin/as\nprint(1)\n", AS_LANGUAGE_VM);
    t("header must be line 2, not 3", "#!/bin/as\n\n# aether: 3.0\n", AS_LANGUAGE_VM);
    printf("as-version-shebang: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
