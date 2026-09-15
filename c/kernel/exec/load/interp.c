/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "elf.h"
#include "vfs.h"
#include "vfs_meta.h"
#include "pcache.h"

int elf_interpreter_open(const char *path, struct elf_reader *rd, struct elf_src *src)
{
    /* Execute the interpreter with the caller's credentials, just as the main
     * image. No grants, setuid transition or alternate path search happen
     * here. An interpreter is a bare ELF, not another AEX/container chain. */
    if (vfs_access(path, MAY_READ | MAY_EXEC) < 0) return ELF_E_INTERP;
    int size = vfs_size(path);
    if (size < 64) return ELF_E_INTERP;
    *rd = (struct elf_reader){ .path = path, .size = (uint64_t)size };
    *src = (struct elf_src){ .fh = pcache_file_open(path),
                            .file_pages = ((uint64_t)size + 4095) >> 12 };
    return ELF_OK;
}

void elf_interpreter_close(const struct elf_src *src)
{
    if (src->fh >= 0) pcache_file_put(src->fh);
}
