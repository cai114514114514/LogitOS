/* SPDX-License-Identifier: MIT */
#include "numeric.h"
#include <string.h>

int as_source_version(const char *s)
{
    /* `#aether: 3` is what a first Studio program looks like. Requiring a
     * space after `#` classified that alias as unversioned A2; after the VM
     * was deleted, Run printed "engine removed" instead of compiling the file. */
    if (*s != '#') {
        return AS_LANGUAGE_VM;
    }
    s++;
    s += strspn(s, " \t\r\f\v");
    const char *name = "aether:";
    size_t name_length = strlen(name);
    if (strncmp(s, name, name_length)) {
        return AS_LANGUAGE_VM;
    }
    s += name_length;
    s += strspn(s, " \t\r\f\v");
    int version;
    if (*s == '1') {
        version = AS_LANGUAGE_RETIRED;
    } else if (*s == '2') {
        version = AS_LANGUAGE_VM;
    } else if (*s == '3') {
        version = AS_LANGUAGE_NATIVE;
    } else {
        return 0;
    }
    s++;
    /* 3 aliases 3.0. A newer minor must be implemented before accepting it;
     * accepting any 3.x would falsely promise future language capabilities. */
    if (version == AS_LANGUAGE_NATIVE && *s == '.') {
        s++;
        if (*s != '0' + AS_LANGUAGE_MINOR) {
            return 0;
        }
        s++;
    }
    s += strspn(s, " \t\r\f\v");
    return !*s || *s == '\n' ? version : 0;
}

const char *as_version_error(int version)
{
    if (version == AS_LANGUAGE_RETIRED) {
        return "AetherScript 1 has been removed; migrate this source to 2, then to 3.x (line 1)";
    }
    if (version == AS_LANGUAGE_NATIVE) {
        return "AetherScript 3.x requires as check/build/run/test; it cannot run in the A2 VM "
               "(line 1)";
    }
    return "unsupported AetherScript language version; supported: 2 (migration), 3.0 (native) "
           "(line 1)";
}
